#include "net.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>

#include <curl/curl.h>

#ifndef _WIN32
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
extern char **environ;
#endif

#include "rs_common.h"

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} http_buf;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    http_buf *buf = (http_buf *)userdata;
    size_t incoming = size * nmemb;
    // Cap at 128 MB. A playlist is tiny; a real segment is a few MB; a
    // byte-range request returns only its slice. A whole-file response larger
    // than this is a wrong URL, so refuse rather than exhaust memory.
    if (buf->len + incoming > 128 * 1024 * 1024) return 0;
    if (buf->len + incoming + 1 > buf->cap) {
        size_t cap = buf->cap ? buf->cap * 2 : 16384;
        while (cap < buf->len + incoming + 1) cap *= 2;
        char *grown = (char *)realloc(buf->data, cap);
        if (!grown) return 0;
        buf->data = grown;
        buf->cap = cap;
    }
    memcpy(buf->data + buf->len, ptr, incoming);
    buf->len += incoming;
    buf->data[buf->len] = '\0';
    return incoming;
}

// Captures the Content-Range response header (libcurl has no getinfo for it).
static size_t header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
    char **content_range = (char **)userdata;
    size_t len = size * nitems;
    const char *prefix = "content-range:";
    if (len > strlen(prefix) && strncasecmp(buffer, prefix, strlen(prefix)) == 0) {
        const char *value = buffer + strlen(prefix);
        size_t vlen = len - strlen(prefix);
        while (vlen > 0 && (*value == ' ' || *value == '\t')) { value++; vlen--; }
        while (vlen > 0 && (value[vlen - 1] == '\r' || value[vlen - 1] == '\n' ||
                            value[vlen - 1] == ' ')) vlen--;
        free(*content_range);
        *content_range = (char *)malloc(vlen + 1);
        if (*content_range) { memcpy(*content_range, value, vlen); (*content_range)[vlen] = '\0'; }
    }
    return len;
}

// A non-2xx response often carries the origin's own error payload — e.g. a
// small JSON body like {"status":"FRUITION_EXCEED","message":"Limit of
// concurrent streams reached."} — which used to be fetched, buffered, and
// then thrown away, leaving only the bare "Upstream returned HTTP 403." in
// the logs. Appends a sanitized, capped snippet of it when the body looks
// like text (skipped for a binary body — that's not something to dump into a
// log line).
static void append_body_snippet(char *errbuf, size_t errbuf_len, const char *body, size_t len) {
    if (!body || len == 0 || !errbuf || errbuf_len == 0) return;
    size_t cap = len < 300 ? len : 300;
    size_t printable = 0;
    for (size_t i = 0; i < cap; i++) {
        unsigned char c = (unsigned char)body[i];
        if (c == '\t' || c == '\n' || c == '\r' || (c >= 0x20 && c < 0x7f)) printable++;
    }
    if (printable < cap * 9 / 10) return;
    size_t used = strnlen(errbuf, errbuf_len);
    static const char sep[] = " - ";
    if (used + sizeof(sep) >= errbuf_len) return;
    memcpy(errbuf + used, sep, sizeof(sep) - 1);
    used += sizeof(sep) - 1;
    size_t avail = errbuf_len - used - 1;
    size_t n = cap < avail ? cap : avail;
    for (size_t i = 0; i < n; i++) {
        char c = body[i];
        errbuf[used + i] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    }
    errbuf[used + n] = '\0';
}

// --- Connection reuse ------------------------------------------------------
//
// The cost that decides whether a live stream keeps up is the per-request round
// trip, not throughput: on a 491 Mbps host, TCP connect to izzigo.tv measured
// 0.34-0.94s with TLS adding ~0.15s, so a fresh connection per segment paid
// ~0.5-1.1s before a byte of media moved. A representation publishing one
// 1.935s segment every 1.935s cannot absorb that. It presents as "not enough
// bandwidth" when it is entirely latency.
//
// This used to be solved with a CURLSH sharing CURL_LOCK_DATA_CONNECT across
// every thread. libcurl's own documentation rules that out — CURLSHOPT_SHARE(3)
// on CURL_LOCK_DATA_CONNECT: "It is not supported to share connections between
// multiple concurrent threads." — and the live engine calls this from a pool of
// download threads, which is exactly the unsupported case.
//
// So connections are now reused the way every library that does this properly
// reuses them (N_m3u8DL-RE's single pooled HttpClient, streamlink's
// requests.Session): one long-lived easy handle per thread, reset between
// requests. curl_easy_reset(3) clears the options but explicitly keeps "live
// connections, the Session ID cache, the DNS cache" — so a thread that fetches
// segment after segment from one origin pays the handshake once, and no
// connection is ever touched by two threads. The engine's download threads are
// persistent for the life of a stream, which is what makes this pay.
//
// DNS and TLS session state are still shared process-wide: both are documented
// as safe to share, and a resumed TLS session skips a round trip on the
// connections that genuinely do have to be opened.
#ifndef _WIN32
#include <pthread.h>

static CURLSH *g_conn_share = NULL;
static pthread_mutex_t g_share_locks[CURL_LOCK_DATA_LAST];

static void share_lock_cb(CURL *handle, curl_lock_data data, curl_lock_access access, void *user) {
    (void)handle; (void)access; (void)user;
    if ((int)data >= 0 && data < CURL_LOCK_DATA_LAST) pthread_mutex_lock(&g_share_locks[data]);
}

static void share_unlock_cb(CURL *handle, curl_lock_data data, void *user) {
    (void)handle; (void)user;
    if ((int)data >= 0 && data < CURL_LOCK_DATA_LAST) pthread_mutex_unlock(&g_share_locks[data]);
}

static void share_init_once(void) {
    for (int i = 0; i < CURL_LOCK_DATA_LAST; i++) pthread_mutex_init(&g_share_locks[i], NULL);
    CURLSH *sh = curl_share_init();
    if (!sh) return;  // degrade to no sharing rather than failing the fetch
    curl_share_setopt(sh, CURLSHOPT_LOCKFUNC, share_lock_cb);
    curl_share_setopt(sh, CURLSHOPT_UNLOCKFUNC, share_unlock_cb);
    // Deliberately NOT CURL_LOCK_DATA_CONNECT — see above.
    curl_share_setopt(sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    g_conn_share = sh;
}

static CURLSH *shared_dns_cache(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, share_init_once);
    return g_conn_share;
}

// The per-thread handle. Freed by the pthread_key destructor when the thread
// exits, so a stream that stops does not leak its download threads' sockets.
static pthread_key_t g_handle_key;
static bool g_handle_key_ok = false;

static void handle_destructor(void *h) {
    if (h) curl_easy_cleanup((CURL *)h);
}

static void handle_key_init_once(void) {
    g_handle_key_ok = pthread_key_create(&g_handle_key, handle_destructor) == 0;
}

// A handle owned by, and only ever used by, the calling thread. Reset on every
// call so no option survives from the previous request.
static CURL *thread_handle(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, handle_key_init_once);
    if (!g_handle_key_ok) return curl_easy_init();  // caller cleans up (see own_handle)
    CURL *h = (CURL *)pthread_getspecific(g_handle_key);
    if (h) { curl_easy_reset(h); return h; }
    h = curl_easy_init();
    if (h) pthread_setspecific(g_handle_key, h);
    return h;
}

static bool thread_handle_is_owned(void) { return g_handle_key_ok; }

#else
// Windows in this build has no pthreads (see live.c), and the fetch path there
// is single-threaded, so a plain per-request handle is both correct and no
// slower than it was.
static CURLSH *shared_dns_cache(void) { return NULL; }
static CURL *thread_handle(void) { return curl_easy_init(); }
static bool thread_handle_is_owned(void) { return false; }
#endif

// --- HTTP/2, and the origins that cannot do it ------------------------------
//
// HTTP/2 multiplexes every request for one origin onto a single connection, so
// a burst of segment fetches costs one handshake and one round trip rather than
// one each. That is the single biggest win available on a high-latency path,
// and it is why N_m3u8DL-RE sets DefaultRequestVersion = HTTP/2.
//
// It was previously disabled for everything because one origin (izzigo.tv) runs
// a broken HTTP/2 stack that drops mid-stream with "Stream error in the HTTP/2
// framing layer" — reproduced with plain curl, so it is the origin, not us.
// Turning it off globally to route around one origin gave up the win everywhere
// else, so instead: try HTTP/2, and when a host answers with an HTTP/2-specific
// framing error, remember that host and use 1.1 for it from then on. The
// request that hit the error is retried immediately, so the downgrade costs one
// request once per host rather than a failure.
#define RS_H2_DENY_MAX 32

static char *g_h2_deny[RS_H2_DENY_MAX];
static size_t g_h2_deny_n = 0;
#ifndef _WIN32
static pthread_mutex_t g_h2_mu = PTHREAD_MUTEX_INITIALIZER;
#define H2_LOCK()   pthread_mutex_lock(&g_h2_mu)
#define H2_UNLOCK() pthread_mutex_unlock(&g_h2_mu)
#else
#define H2_LOCK()   ((void)0)
#define H2_UNLOCK() ((void)0)
#endif

// The host part of an absolute URL, lowercased, into `out`.
static void url_host(const char *url, char *out, size_t outlen) {
    out[0] = '\0';
    const char *p = strstr(url, "://");
    if (!p) return;
    p += 3;
    const char *at = NULL;
    size_t i = 0;
    for (const char *q = p; *q && *q != '/' && *q != '?'; q++) if (*q == '@') at = q;
    if (at) p = at + 1;
    for (; p[i] && p[i] != '/' && p[i] != '?' && p[i] != ':' && i + 1 < outlen; i++)
        out[i] = (char)tolower((unsigned char)p[i]);
    out[i] = '\0';
}

static bool h2_denied(const char *host) {
    if (!host[0]) return false;
    bool found = false;
    H2_LOCK();
    for (size_t i = 0; i < g_h2_deny_n; i++)
        if (strcmp(g_h2_deny[i], host) == 0) { found = true; break; }
    H2_UNLOCK();
    return found;
}

static void h2_deny(const char *host) {
    if (!host[0]) return;
    H2_LOCK();
    bool found = false;
    for (size_t i = 0; i < g_h2_deny_n; i++)
        if (strcmp(g_h2_deny[i], host) == 0) { found = true; break; }
    // Full table: stop adding rather than evicting. A 33rd broken-HTTP/2 origin
    // is not a real deployment, and evicting would let the two of them flap.
    if (!found && g_h2_deny_n < RS_H2_DENY_MAX) g_h2_deny[g_h2_deny_n++] = rs_strdup(host);
    H2_UNLOCK();
}

// Errors that mean "this origin's HTTP/2 is broken", as opposed to any of the
// ordinary network failures that say nothing about the protocol version.
static bool is_http2_failure(CURLcode rc) {
    if (rc == CURLE_HTTP2) return true;
#ifdef CURLE_HTTP2_STREAM
    if (rc == CURLE_HTTP2_STREAM) return true;
#endif
    return false;
}

// One transfer on the calling thread's handle. `force_http11` is set by the
// caller when retrying after an HTTP/2 framing error.
static int fetch_once(CURL *curl, const char *url, const char *proxy, const char *headers,
                      const char *range, bool force_http11,
                      char **out, size_t *out_len, long *status, char **content_type,
                      char **content_range, char **effective_url,
                      CURLcode *out_rc, char *errbuf, size_t errbuf_len) {
    http_buf buf = {NULL, 0, 0};
    struct curl_slist *header_list = NULL;
    if (headers && headers[0]) {
        char *copy = rs_strdup(headers);
        for (char *line = strtok(copy, "\r\n"); line; line = strtok(NULL, "\r\n")) {
            while (*line == ' ' || *line == '\t') line++;
            if (*line) header_list = curl_slist_append(header_list, line);
        }
        free(copy);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    // Abort a transfer that has connected but is not actually moving. The
    // overall 30s timeout is the wrong instrument for this: a CDN handed an
    // expired signed URL does not refuse it, it simply never answers, so the
    // request sat for the full thirty seconds — per attempt — while the live
    // engine's queue backed up behind it. Anything below 1 KB/s for five
    // seconds is not a slow segment, it is a dead request. A genuinely slow
    // but progressing transfer keeps its full 30s.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 5L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
                     force_http11 ? (long)CURL_HTTP_VERSION_1_1 : (long)CURL_HTTP_VERSION_2TLS);
    // Keep-alive is the whole point of the per-thread handle; make the probes
    // frequent enough that an idle connection through a tunnel is not silently
    // dropped between segments.
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 30L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 15L);
    CURLSH *dns = shared_dns_cache();
    if (dns) curl_easy_setopt(curl, CURLOPT_SHARE, dns);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ReStreamAir/1.0");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    if (proxy && proxy[0]) curl_easy_setopt(curl, CURLOPT_PROXY, proxy);
    if (header_list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    if (range && range[0]) {
        // libcurl's CURLOPT_RANGE wants just the byte spec, without "bytes=".
        const char *spec = strncasecmp(range, "bytes=", 6) == 0 ? range + 6 : range;
        curl_easy_setopt(curl, CURLOPT_RANGE, spec);
    }
    char *captured_range = NULL;
    if (content_range) {
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &captured_range);
    }

    CURLcode rc = curl_easy_perform(curl);
    *out_rc = rc;
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (status) *status = code;
    char *ct = NULL;
    if (content_type && curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct) == CURLE_OK && ct) {
        *content_type = rs_strdup(ct);
    }
    char *eff = NULL;
    if (effective_url && curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff) == CURLE_OK && eff) {
        *effective_url = rs_strdup(eff);
    }
    if (content_range) *content_range = captured_range;
    if (header_list) curl_slist_free_all(header_list);

    if (rc != CURLE_OK) {
        snprintf(errbuf, errbuf_len, "Fetch failed: %s", curl_easy_strerror(rc));
        free(buf.data);
        if (content_type) { free(*content_type); *content_type = NULL; }
        if (content_range) { free(*content_range); *content_range = NULL; }
        if (effective_url) { free(*effective_url); *effective_url = NULL; }
        return -1;
    }
    if (code < 200 || code >= 400) {
        snprintf(errbuf, errbuf_len, "Upstream returned HTTP %ld.", code);
        append_body_snippet(errbuf, errbuf_len, buf.data, buf.len);
        free(buf.data);
        if (content_type) { free(*content_type); *content_type = NULL; }
        if (content_range) { free(*content_range); *content_range = NULL; }
        if (effective_url) { free(*effective_url); *effective_url = NULL; }
        return -1;
    }
    if (!buf.data) {
        buf.data = (char *)malloc(1);
        if (!buf.data) { snprintf(errbuf, errbuf_len, "Out of memory."); return -1; }
        buf.data[0] = '\0';
    }
    *out = buf.data;
    *out_len = buf.len;
    return 0;
}

static int fetch_libcurl(const char *url, const char *proxy, const char *headers, const char *range,
                         char **out, size_t *out_len, long *status, char **content_type,
                         char **content_range, char **effective_url, char *errbuf, size_t errbuf_len) {
    if (content_type) *content_type = NULL;
    if (content_range) *content_range = NULL;
    if (effective_url) *effective_url = NULL;
    if (status) *status = 0;

    CURL *curl = thread_handle();
    if (!curl) { snprintf(errbuf, errbuf_len, "Could not initialise HTTP client."); return -1; }
    bool borrowed = thread_handle_is_owned();  // false: this handle is ours to free

    char host[256];
    url_host(url, host, sizeof(host));
    bool force11 = h2_denied(host);

    CURLcode rc = CURLE_OK;
    int result = fetch_once(curl, url, proxy, headers, range, force11,
                            out, out_len, status, content_type, content_range, effective_url,
                            &rc, errbuf, errbuf_len);

    // This origin's HTTP/2 is broken. Record it so every later request to the
    // same host goes straight to 1.1, and retry this one now.
    if (result != 0 && !force11 && is_http2_failure(rc)) {
        h2_deny(host);
        if (borrowed) curl_easy_reset(curl);
        result = fetch_once(curl, url, proxy, headers, range, true,
                            out, out_len, status, content_type, content_range, effective_url,
                            &rc, errbuf, errbuf_len);
    }

    if (!borrowed) curl_easy_cleanup(curl);
    return result;
}

// --- External downloader (curl / wget / aria2c) -----------------------------
//
// A provider can choose to have its manifest and segment downloads run through
// an external tool instead of the in-process libcurl fetch, with a free-form
// extra-params string. curl gives us the response status + Content-Range /
// Content-Type back (via -D), so byte-range relay stays correct; wget's
// --server-response gives the same from stderr; aria2c is best-effort (headers
// aren't recoverable) so it reports no status and suits whole-segment
// downloads. If the chosen tool is missing, the caller falls back to libcurl.

#ifndef _WIN32

typedef struct { char **v; int n; int cap; } argv_b;
static void ab_push(argv_b *a, const char *s) {
    if (a->n + 2 >= a->cap) { a->cap = a->cap ? a->cap * 2 : 32; a->v = realloc(a->v, (size_t)a->cap * sizeof(char *)); }
    a->v[a->n++] = s ? rs_strdup(s) : NULL;
}
static void ab_free(argv_b *a) { for (int i = 0; i < a->n; i++) free(a->v[i]); free(a->v); }

static int read_whole_file(const char *path, char **out, size_t *out_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    if (!buf) { close(fd); return -1; }
    ssize_t n;
    while ((n = read(fd, buf + len, cap - len - 1)) > 0) {
        len += (size_t)n;
        if (len + 1 >= cap) { cap *= 2; char *g = realloc(buf, cap); if (!g) { free(buf); close(fd); return -1; } buf = g; }
    }
    close(fd);
    if (n < 0) { free(buf); return -1; }
    buf[len] = '\0';
    *out = buf; *out_len = len;
    return 0;
}

// Push each "Name: value" header line as an argv entry, styled per tool:
// curl wants "-H" then the value (two args); wget/aria2c want one "--header=…".
static void push_headers(argv_b *a, const char *headers, bool curl_style) {
    if (!headers || !headers[0]) return;
    char *copy = rs_strdup(headers);
    for (char *line = strtok(copy, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        while (*line == ' ' || *line == '\t') line++;
        if (!*line) continue;
        if (curl_style) { ab_push(a, "-H"); ab_push(a, line); }
        else { char *h = malloc(strlen(line) + 10); sprintf(h, "--header=%s", line); ab_push(a, h); free(h); }
    }
    free(copy);
}

// Split free-form extra params on whitespace into argv tokens.
static void push_extra_params(argv_b *a, const char *params) {
    if (!params || !params[0]) return;
    char *copy = rs_strdup(params);
    for (char *tok = strtok(copy, " \t\r\n"); tok; tok = strtok(NULL, " \t\r\n"))
        if (*tok) ab_push(a, tok);
    free(copy);
}

// Parse an HTTP header dump (curl -D output, or wget --server-response stderr).
// Keeps the last status line (so redirects resolve to the final response) and
// the Content-Range / Content-Type of that response.
static void parse_meta(const char *meta, long *status, char **content_type, char **content_range) {
    char *copy = rs_strdup(meta ? meta : "");
    for (char *line = strtok(copy, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        while (*line == ' ' || *line == '\t') line++;
        if (strncasecmp(line, "HTTP/", 5) == 0) {
            const char *sp = strchr(line, ' ');
            if (sp && status) *status = strtol(sp + 1, NULL, 10);
        } else if (strncasecmp(line, "content-range:", 14) == 0 && content_range) {
            const char *v = line + 14; while (*v == ' ' || *v == '\t') v++;
            free(*content_range); *content_range = rs_strdup(v);
        } else if (strncasecmp(line, "content-type:", 13) == 0 && content_type) {
            const char *v = line + 13; while (*v == ' ' || *v == '\t') v++;
            free(*content_type); *content_type = rs_strdup(v);
        }
    }
    free(copy);
}

// Spawn argv (searching PATH), redirecting stdout to /dev/null and stderr to
// `err_path` (for wget). Returns the exit code, -2 if the binary is missing,
// -1 on spawn failure.
static int spawn_wait(char *const argv[], const char *err_path) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    if (err_path) posix_spawn_file_actions_addopen(&fa, 2, err_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    else posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return rc == ENOENT ? -2 : -1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int fetch_external(const char *tool, const char *dl_params,
                          const char *url, const char *proxy, const char *headers, const char *range,
                          char **out, size_t *out_len, long *status, char **content_type,
                          char **content_range, char **effective_url, char *errbuf, size_t errbuf_len) {
    if (content_type) *content_type = NULL;
    if (content_range) *content_range = NULL;
    if (effective_url) *effective_url = NULL;  // external tools don't report it
    if (status) *status = 0;

    const char *tmpdir = getenv("TMPDIR"); if (!tmpdir || !tmpdir[0]) tmpdir = "/tmp";
    char body_tmpl[1024], meta_tmpl[1024];
    snprintf(body_tmpl, sizeof(body_tmpl), "%s/rsdl_body_XXXXXX", tmpdir);
    snprintf(meta_tmpl, sizeof(meta_tmpl), "%s/rsdl_meta_XXXXXX", tmpdir);
    int bfd = mkstemp(body_tmpl); if (bfd < 0) { snprintf(errbuf, errbuf_len, "Could not create temp file."); return -1; }
    close(bfd);
    int mfd = mkstemp(meta_tmpl); if (mfd < 0) { unlink(body_tmpl); snprintf(errbuf, errbuf_len, "Could not create temp file."); return -1; }
    close(mfd);

    const char *range_spec = range && range[0]
        ? (strncasecmp(range, "bytes=", 6) == 0 ? range + 6 : range) : NULL;

    argv_b a = {0};
    bool is_wget = strcasecmp(tool, "wget") == 0;
    bool is_aria = strcasecmp(tool, "aria2c") == 0 || strcasecmp(tool, "aria2") == 0 || strcasecmp(tool, "aria") == 0;
    const char *err_redirect = NULL;

    if (is_wget) {
        ab_push(&a, "wget"); ab_push(&a, "--server-response"); ab_push(&a, "-O"); ab_push(&a, body_tmpl);
        ab_push(&a, "--timeout=60"); ab_push(&a, "--tries=2"); ab_push(&a, "-U"); ab_push(&a, "ReStreamAir/1.0");
        if (proxy && proxy[0]) {
            char e[1200];
            ab_push(&a, "-e"); ab_push(&a, "use_proxy=yes");
            snprintf(e, sizeof(e), "http_proxy=%s", proxy); ab_push(&a, "-e"); ab_push(&a, e);
            snprintf(e, sizeof(e), "https_proxy=%s", proxy); ab_push(&a, "-e"); ab_push(&a, e);
        }
        push_headers(&a, headers, false);
        if (range_spec) { char r[128]; snprintf(r, sizeof(r), "--header=Range: bytes=%s", range_spec); ab_push(&a, r); }
        push_extra_params(&a, dl_params);
        ab_push(&a, url);
        err_redirect = meta_tmpl;  // wget prints response headers to stderr
    } else if (is_aria) {
        char basebuf[1024]; snprintf(basebuf, sizeof(basebuf), "%s", body_tmpl);
        char *base = strrchr(basebuf, '/'); base = base ? base + 1 : basebuf;
        ab_push(&a, "aria2c"); ab_push(&a, "--quiet=true"); ab_push(&a, "--allow-overwrite=true");
        ab_push(&a, "--auto-file-renaming=false"); ab_push(&a, "-x1"); ab_push(&a, "-s1");
        ab_push(&a, "--connect-timeout=10"); ab_push(&a, "--timeout=60"); ab_push(&a, "-U"); ab_push(&a, "ReStreamAir/1.0");
        ab_push(&a, "-d"); ab_push(&a, tmpdir); ab_push(&a, "-o"); ab_push(&a, base);
        if (proxy && proxy[0]) { char p[1200]; snprintf(p, sizeof(p), "--all-proxy=%s", proxy); ab_push(&a, p); }
        push_headers(&a, headers, false);
        if (range_spec) { char r[128]; snprintf(r, sizeof(r), "--header=Range: bytes=%s", range_spec); ab_push(&a, r); }
        push_extra_params(&a, dl_params);
        ab_push(&a, url);
    } else {  // curl (default)
        ab_push(&a, "curl"); ab_push(&a, "-sS"); ab_push(&a, "-L"); ab_push(&a, "--max-time"); ab_push(&a, "60");
        ab_push(&a, "--connect-timeout"); ab_push(&a, "10"); ab_push(&a, "-A"); ab_push(&a, "ReStreamAir/1.0");
        ab_push(&a, "-o"); ab_push(&a, body_tmpl); ab_push(&a, "-D"); ab_push(&a, meta_tmpl);
        if (proxy && proxy[0]) { ab_push(&a, "-x"); ab_push(&a, proxy); }
        push_headers(&a, headers, true);
        if (range_spec) { ab_push(&a, "--range"); ab_push(&a, range_spec); }
        push_extra_params(&a, dl_params);
        ab_push(&a, url);
    }
    ab_push(&a, NULL);

    int code = spawn_wait(a.v, err_redirect);
    ab_free(&a);

    if (code == -2) {
        snprintf(errbuf, errbuf_len, "Downloader '%s' is not installed.", tool);
        unlink(body_tmpl); unlink(meta_tmpl);
        return -2;  // signal "missing tool" so the caller can fall back
    }
    if (code != 0) {
        snprintf(errbuf, errbuf_len, "%s exited with code %d.", tool, code);
        unlink(body_tmpl); unlink(meta_tmpl);
        return -1;
    }

    char *meta = NULL; size_t meta_len = 0;
    if (read_whole_file(meta_tmpl, &meta, &meta_len) == 0) {
        parse_meta(meta, status, content_type, content_range);
        free(meta);
    }
    unlink(meta_tmpl);

    char *body = NULL; size_t body_len = 0;
    if (read_whole_file(body_tmpl, &body, &body_len) != 0) {
        unlink(body_tmpl);
        snprintf(errbuf, errbuf_len, "Could not read %s output.", tool);
        if (content_type) { free(*content_type); *content_type = NULL; }
        if (content_range) { free(*content_range); *content_range = NULL; }
        return -1;
    }
    unlink(body_tmpl);

    long st = status ? *status : 0;
    if (st != 0 && (st < 200 || st >= 400)) {
        snprintf(errbuf, errbuf_len, "Upstream returned HTTP %ld.", st);
        append_body_snippet(errbuf, errbuf_len, body, body_len);
        free(body);
        if (content_type) { free(*content_type); *content_type = NULL; }
        if (content_range) { free(*content_range); *content_range = NULL; }
        return -1;
    }
    if (body_len == 0) {
        snprintf(errbuf, errbuf_len, "%s produced no data.", tool);
        free(body);
        if (content_type) { free(*content_type); *content_type = NULL; }
        if (content_range) { free(*content_range); *content_range = NULL; }
        return -1;
    }
    *out = body; *out_len = body_len;
    return 0;
}

#endif  // !_WIN32

int rs_fetch_url(const char *url, const char *proxy, const char *headers, const char *range,
                 const char *downloader, const char *dl_params,
                 char **out, size_t *out_len, long *status, char **content_type,
                 char **content_range, char **effective_url, char *errbuf, size_t errbuf_len) {
    // NULL / "" / "internal" / "libcurl" → in-process libcurl. Otherwise run the
    // chosen external tool, falling back to libcurl if it isn't installed so a
    // missing binary never dead-ends a stream.
    bool internal = !downloader || !downloader[0] ||
                    strcasecmp(downloader, "internal") == 0 || strcasecmp(downloader, "libcurl") == 0 ||
                    strcasecmp(downloader, "native") == 0;
#ifndef _WIN32
    if (!internal) {
        int rc = fetch_external(downloader, dl_params, url, proxy, headers, range,
                                out, out_len, status, content_type, content_range, effective_url, errbuf, errbuf_len);
        if (rc != -2) return rc;  // -2 = tool missing → fall through to libcurl
    }
#else
    (void)dl_params;
#endif
    return fetch_libcurl(url, proxy, headers, range, out, out_len, status, content_type, content_range, effective_url, errbuf, errbuf_len);
}
