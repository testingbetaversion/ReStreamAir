#include "net.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>

#include <curl/curl.h>

#include "rs_common.h"
#include "rs_proc.h"
#include "rs_json.h"
#include "rs_url.h"
#include "rs_thread.h"
#include "restream.h"
#include <stdarg.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define POLICY_MKDIR(p) _mkdir(p)
static __declspec(thread) const rs_source_policy *active_policy;
static __declspec(thread) int manifest_priority;
static __declspec(thread) const void *t_proxy_pool;   // pool of the last proxy this thread used
static __declspec(thread) size_t t_proxy_index;
#else
#define POLICY_MKDIR(p) mkdir(p, 0700)
static _Thread_local const rs_source_policy *active_policy;
static _Thread_local int manifest_priority;
static _Thread_local const void *t_proxy_pool;
static _Thread_local size_t t_proxy_index;
#endif

int rs_fetch_set_manifest_priority(int on) {
    int previous = manifest_priority;
    manifest_priority = on;
    return previous;
}

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>   // mkstemp/close, for the downloader's temp files
#endif

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

// --- verbose (debug) logging ---------------------------------------------------
//
// With verbose logging on, every attempt is logged in full: request headers,
// proxy, response status and headers, and libcurl's timing breakdown. Secrets
// in headers and proxy URLs are masked; everything else is shown as sent.

static double net_now(void);

static void dbg_appendf(http_buf *b, const char *fmt, ...) {
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    size_t len = (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1;
    if (b->len + len + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1024;
        while (cap < b->len + len + 1) cap *= 2;
        char *grown = (char *)realloc(b->data, cap);
        if (!grown) return;
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, tmp, len);
    b->len += len;
    b->data[b->len] = '\0';
}

static bool dbg_secret_header(const char *name, size_t len) {
    static const char *const secret[] = {
        "authorization", "proxy-authorization", "cookie", "set-cookie",
        "x-api-key", "x-auth-token", "x-access-token",
    };
    for (size_t i = 0; i < sizeof(secret) / sizeof(secret[0]); i++)
        if (strlen(secret[i]) == len && strncasecmp(name, secret[i], len) == 0) return true;
    return false;
}

// Appends "  Name: value" lines from CRLF/LF-separated header text, masking
// the values of credential-bearing headers.
static void dbg_append_headers(http_buf *b, const char *headers) {
    const char *p = headers;
    while (p && *p) {
        const char *eol = p + strcspn(p, "\r\n");
        const char *line = p;
        while (line < eol && (*line == ' ' || *line == '\t')) line++;
        if (line < eol) {
            const char *colon = memchr(line, ':', (size_t)(eol - line));
            if (colon && dbg_secret_header(line, (size_t)(colon - line))) {
                const char *v = colon + 1;
                while (v < eol && *v == ' ') v++;
                dbg_appendf(b, "  %.*s: <masked, %d chars>\n", (int)(colon - line), line, (int)(eol - v));
            } else {
                dbg_appendf(b, "  %.*s\n", (int)(eol - line), line);
            }
        }
        p = eol;
        while (*p == '\r' || *p == '\n') p++;
    }
}

// "http://user:pass@host:port" -> "http://<masked>@host:port".
static void dbg_append_proxy(http_buf *b, const char *proxy) {
    const char *at = strrchr(proxy, '@');
    const char *scheme = strstr(proxy, "://");
    if (at) dbg_appendf(b, "proxy: %.*s<masked>%s\n",
                        scheme && scheme < at ? (int)(scheme + 3 - proxy) : 0, proxy, at);
    else dbg_appendf(b, "proxy: %s\n", proxy);
}

typedef struct {
    char **content_range;  // NULL when the caller did not ask for it
    http_buf *headers;     // NULL unless verbose logging is on
} header_sink;

// Captures the Content-Range response header (libcurl has no getinfo for it),
// and every response header line when verbose logging wants them.
static size_t header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
    header_sink *sink = (header_sink *)userdata;
    size_t len = size * nitems;
    if (sink->headers && sink->headers->len < 16384) dbg_appendf(sink->headers, "%.*s", (int)len, buffer);
    char **content_range = sink->content_range;
    const char *prefix = "content-range:";
    if (content_range && len > strlen(prefix) && strncasecmp(buffer, prefix, strlen(prefix)) == 0) {
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
#include "rs_thread.h"

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

// --- CA trust store, for the static build -----------------------------------
//
// A statically linked binary carries its own TLS library, and that library was
// compiled on Alpine: its built-in CA path is Alpine's, which does not exist on
// the Debian or RHEL box the download lands on. The host's trust store is fine,
// it is just somewhere else on every distro — so find it once and hand libcurl
// the answer. Without this the single-file build serves the panel perfectly and
// fails every upstream HTTPS fetch with "unable to get local issuer
// certificate", which is a failure that looks like a broken source.
//
// Dynamically linked builds use the system libcurl and already agree with the
// system about where certificates live, so this compiles to nothing there.
#ifdef RS_STATIC_BUILD
static const char *g_ca_bundle;

static void ca_bundle_probe(void) {
    // curl reads these itself, but only at curl_easy_init(); setting CAINFO
    // below would silently override them, so they come first here.
    const char *env = getenv("CURL_CA_BUNDLE");
    if (!env || !env[0]) env = getenv("SSL_CERT_FILE");
    if (env && env[0]) { g_ca_bundle = env; return; }

    static const char *const candidates[] = {
        "/etc/ssl/certs/ca-certificates.crt",                  // Debian, Ubuntu, Alpine, Arch
        "/etc/pki/tls/certs/ca-bundle.crt",                    // RHEL, CentOS, Fedora
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",   // newer RHEL family
        "/etc/ssl/ca-bundle.pem",                              // openSUSE
        "/etc/ssl/cert.pem",                                   // Alpine, FreeBSD, macOS
    };
    for (size_t i = 0; i < sizeof candidates / sizeof *candidates; i++) {
        if (access(candidates[i], R_OK) == 0) { g_ca_bundle = candidates[i]; return; }
    }
    // Nothing found: leave the compiled-in default alone rather than pointing
    // libcurl at a path that does not exist.
}

static void apply_ca_bundle(CURL *curl) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, ca_bundle_probe);
    if (!g_ca_bundle) return;
    curl_easy_setopt(curl, CURLOPT_CAINFO, g_ca_bundle);
    // An https:// proxy is verified separately and does not inherit CAINFO;
    // without this it falls back to Alpine's compiled-in path and every fetch
    // through the proxy fails with "Problem with the SSL CA cert".
    curl_easy_setopt(curl, CURLOPT_PROXY_CAINFO, g_ca_bundle);
}
#else
static void apply_ca_bundle(CURL *curl) { (void)curl; }
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
static pthread_mutex_t g_h2_mu = PTHREAD_MUTEX_INITIALIZER;
#define H2_LOCK()   pthread_mutex_lock(&g_h2_mu)
#define H2_UNLOCK() pthread_mutex_unlock(&g_h2_mu)

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

typedef struct {
    int (*should_cancel)(void *, size_t);
    void *ctx;
} fetch_cancel;

static int transfer_progress(void *opaque, curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t ultotal, curl_off_t ulnow) {
    (void)dltotal; (void)ultotal; (void)ulnow;
    fetch_cancel *cancel = (fetch_cancel *)opaque;
    size_t downloaded = dlnow > 0 ? (size_t)dlnow : 0;
    return cancel && cancel->should_cancel && cancel->should_cancel(cancel->ctx, downloaded) ? 1 : 0;
}

// Request-local tokenization; strtok's shared cursor corrupts concurrent
// provider headers and downloader arguments.
static char *net_token(char **cursor, const char *delimiters) {
    if (!*cursor) return NULL;
    char *start = *cursor + strspn(*cursor, delimiters);
    if (!*start) { *cursor = NULL; return NULL; }
    char *end = start + strcspn(start, delimiters);
    *cursor = *end ? end + 1 : NULL;
    if (*end) *end = 0;
    return start;
}

// One transfer on the calling thread's handle. `force_http11` is set by the
// caller when retrying after an HTTP/2 framing error.
static void log_attempt(CURL *curl, const char *url, const char *proxy, const char *headers,
                        const char *range, bool force_http11, CURLcode rc,
                        const http_buf *response_headers, size_t body_len) {
    http_buf m = {NULL, 0, 0};
    long code = 0, version = 0, redirects = 0, connects = 0;
    curl_off_t dns = 0, connect = 0, tls = 0, ttfb = 0, total = 0;
    char *ip = NULL, *eff = NULL;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_getinfo(curl, CURLINFO_HTTP_VERSION, &version);
    curl_easy_getinfo(curl, CURLINFO_REDIRECT_COUNT, &redirects);
    curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &connects);
    curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME_T, &dns);
    curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME_T, &connect);
    curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME_T, &tls);
    curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME_T, &ttfb);
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME_T, &total);
    curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &ip);
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff);
    const char *ver = version == CURL_HTTP_VERSION_1_0 ? "HTTP/1.0"
                    : version == CURL_HTTP_VERSION_1_1 ? "HTTP/1.1"
                    : version == CURL_HTTP_VERSION_2_0 ? "HTTP/2" : "HTTP";
    dbg_appendf(&m, "GET %s\n", url);
    if (range && range[0]) dbg_appendf(&m, "range: %s\n", range);
    if (proxy && proxy[0]) dbg_append_proxy(&m, proxy);
    if (force_http11) dbg_appendf(&m, "forced HTTP/1.1 (host refused HTTP/2 earlier)\n");
    dbg_appendf(&m, "request headers:\n  User-Agent: ReStreamAir/1.0 (unless overridden below)\n");
    if (headers && headers[0]) dbg_append_headers(&m, headers);
    if (rc != CURLE_OK)
        dbg_appendf(&m, "result: %s (curl error %d)\n", curl_easy_strerror(rc), (int)rc);
    else
        dbg_appendf(&m, "result: %s %ld, %lu body bytes\n", ver, code, (unsigned long)body_len);
    if (eff && strcmp(eff, url) != 0) dbg_appendf(&m, "final URL after %ld redirect(s): %s\n", redirects, eff);
    dbg_appendf(&m, "remote: %s, %s connection\n", ip && ip[0] ? ip : "?", connects ? "new" : "reused");
    dbg_appendf(&m, "timing (ms): dns %.1f, connect %.1f, tls %.1f, first byte %.1f, total %.1f\n",
                (double)dns / 1000.0, (double)connect / 1000.0, (double)tls / 1000.0,
                (double)ttfb / 1000.0, (double)total / 1000.0);
    if (response_headers && response_headers->len) {
        dbg_appendf(&m, "response headers:\n");
        dbg_append_headers(&m, response_headers->data);
    }
    if (m.len && m.data[m.len - 1] == '\n') m.data[--m.len] = '\0';
    restream_debug_log("httpFetch", url, code, rc == CURLE_OK ? (long long)body_len : -1,
                       m.data ? m.data : url);
    free(m.data);
}

static int fetch_once(CURL *curl, const char *url, const char *proxy, const char *headers,
                      const char *range, bool force_http11, int force_ipv6, long timeout_ms,
                      int (*should_cancel)(void *, size_t), void *cancel_ctx,
                      char **out, size_t *out_len, long *status, char **content_type,
                      char **content_range, char **effective_url,
                      CURLcode *out_rc, char *errbuf, size_t errbuf_len) {
    http_buf buf = {NULL, 0, 0};
    struct curl_slist *header_list = NULL;
    if (headers && headers[0]) {
        char *copy = rs_strdup(headers);
        char *cursor = copy;
        for (char *line = net_token(&cursor, "\r\n"); line; line = net_token(&cursor, "\r\n")) {
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
    // Everything this server fetches is a manifest, a segment or a key over
    // HTTP(S). libcurl's default set is far wider than that — file://, ftp://,
    // dict://, gopher:// and the rest are all compiled in — and a URL reaching
    // here is not always one we chose: playback routes carry a caller-supplied
    // target, and a redirect can change the scheme even when the first URL was
    // ours. Pinning both the request and the redirect chain keeps a wrong or
    // hostile target a failed HTTP fetch rather than a local file read.
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE,
                     force_ipv6 ? (long)CURL_IPRESOLVE_V6 : (long)CURL_IPRESOLVE_WHATEVER);
    if (timeout_ms <= 0) timeout_ms = 30000;
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms < 10000 ? timeout_ms : 10000);
    fetch_cancel cancel = {should_cancel, cancel_ctx};
    if (should_cancel) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transfer_progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancel);
    }
    // NO low-speed abort here, and that is deliberate. Cutting transfers that
    // move less than 1 KB/s for five seconds looked like the right way to kill
    // a request the CDN was never going to answer — but libcurl's low-speed
    // clock also runs while waiting for the first byte, and on the proxied
    // paths this server actually uses, time-to-first-byte is routinely several
    // seconds and manifest fetches have been observed taking seventeen. It
    // killed requests that were about to succeed: every segment paid a wasted
    // attempt before the retry got through, and the init segment — the one
    // fetch that must not fail, since it carries the timescale and the
    // decryption key — timed out outright. Dead requests are bounded by the
    // live engine's head-of-line rule instead, which measures the thing that
    // actually matters: whether this segment is holding up ones already
    // downloaded.
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
    curl_easy_setopt(curl, CURLOPT_COOKIELIST, "ALL");
    if (active_policy && active_policy->use_cookies) {
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, active_policy->cookie_file);
        curl_easy_setopt(curl, CURLOPT_COOKIEJAR, active_policy->cookie_file);
        curl_easy_setopt(curl, CURLOPT_COOKIELIST, "RELOAD");
    }
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ReStreamAir/1.0");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    if (proxy && proxy[0]) {
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy);
        // Proxy plans limit simultaneous connections per IP (Proxy-Seller:
        // 10). A thread's handle otherwise keeps up to 5 idle tunnels — one per
        // CDN host and proxy it touched — so a handful of streams held every
        // allowed connection and the provider script got 429 PROXY_MAX_CONNS.
        // Keep only the connection in use.
        curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, 1L);
    }
    if (header_list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    if (range && range[0]) {
        // libcurl's CURLOPT_RANGE wants just the byte spec, without "bytes=".
        const char *spec = strncasecmp(range, "bytes=", 6) == 0 ? range + 6 : range;
        curl_easy_setopt(curl, CURLOPT_RANGE, spec);
    }
    char *captured_range = NULL;
    bool debug = restream_debug_enabled();
    http_buf response_headers = {NULL, 0, 0};
    header_sink sink = {content_range ? &captured_range : NULL, debug ? &response_headers : NULL};
    if (content_range || debug) {
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &sink);
    } else {
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, NULL);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, NULL);
    }

    CURLcode rc = curl_easy_perform(curl);
    if (debug) {
        log_attempt(curl, url, proxy, headers, range, force_http11, rc, &response_headers, buf.len);
        free(response_headers.data);
    }
    if (active_policy && active_policy->use_cookies) {
        curl_easy_setopt(curl, CURLOPT_COOKIELIST, "FLUSH");
        curl_easy_setopt(curl, CURLOPT_COOKIEJAR, NULL);
#ifndef _WIN32
        chmod(active_policy->cookie_file, 0600);
#endif
    }
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
        if (rc == CURLE_ABORTED_BY_CALLBACK)
            snprintf(errbuf, errbuf_len, "Fetch cancelled because the segment is no longer current.");
        else
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
                         int force_ipv6,
                         char **out, size_t *out_len, long *status, char **content_type,
                         char **content_range, char **effective_url, char *errbuf, size_t errbuf_len,
                         long timeout_ms, int (*should_cancel)(void *, size_t), void *cancel_ctx) {
    if (content_type) *content_type = NULL;
    if (content_range) *content_range = NULL;
    if (effective_url) *effective_url = NULL;
    if (status) *status = 0;

    bool session_client = active_policy && active_policy->use_cookies;
    CURL *curl = session_client ? curl_easy_init() : thread_handle();
    if (!curl) { snprintf(errbuf, errbuf_len, "Could not initialise HTTP client."); return -1; }
    apply_ca_bundle(curl);   // after the handle is acquired: curl_easy_reset() clears it
    bool borrowed = !session_client && thread_handle_is_owned();  // false: this handle is ours to free

    char host[256];
    url_host(url, host, sizeof(host));
    bool force11 = h2_denied(host);

    CURLcode rc = CURLE_OK;
    int result = fetch_once(curl, url, proxy, headers, range, force11, force_ipv6,
                            timeout_ms, should_cancel, cancel_ctx,
                            out, out_len, status, content_type, content_range, effective_url,
                            &rc, errbuf, errbuf_len);

    // This origin's HTTP/2 is broken. Record it so every later request to the
    // same host goes straight to 1.1, and retry this one now.
    if (result != 0 && !force11 && is_http2_failure(rc)) {
        h2_deny(host);
        if (borrowed) curl_easy_reset(curl);
        result = fetch_once(curl, url, proxy, headers, range, true, force_ipv6,
                            timeout_ms, should_cancel, cancel_ctx,
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

typedef struct { char **v; int n; int cap; } argv_b;
static void ab_push(argv_b *a, const char *s) {
    if (a->n + 2 >= a->cap) { a->cap = a->cap ? a->cap * 2 : 32; a->v = realloc(a->v, (size_t)a->cap * sizeof(char *)); }
    a->v[a->n++] = s ? rs_strdup(s) : NULL;
}
static void ab_free(argv_b *a) { for (int i = 0; i < a->n; i++) free(a->v[i]); free(a->v); }

static int read_whole_file(const char *path, char **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    if (!buf) { fclose(f); return -1; }
    for (;;) {
        if (len + 1 >= cap) {
            char *g = realloc(buf, cap * 2);
            if (!g) { free(buf); fclose(f); return -1; }
            buf = g; cap *= 2;
        }
        size_t n = fread(buf + len, 1, cap - len - 1, f);
        len += n;
        if (n == 0) break;
    }
    int bad = ferror(f);
    fclose(f);
    if (bad) { free(buf); return -1; }
    buf[len] = '\0';
    *out = buf; *out_len = len;
    return 0;
}

// --- temp files -------------------------------------------------------------
// The external tools write the body and the response headers to files rather
// than to pipes, so this needs a real unique path on both platforms.

static const char *temp_dir(char *buf, size_t cap) {
#ifdef _WIN32
    DWORD n = GetTempPathA((DWORD)cap, buf);
    if (n == 0 || n >= cap) { snprintf(buf, cap, "."); return buf; }
    if (n && (buf[n - 1] == '\\' || buf[n - 1] == '/')) buf[n - 1] = '\0';   // no trailing separator
    return buf;
#else
    const char *t = getenv("TMPDIR");
    snprintf(buf, cap, "%s", (t && t[0]) ? t : "/tmp");
    return buf;
#endif
}

// Creates a unique empty file under `dir` and writes its full path into `path`.
static int make_temp_file(const char *dir, const char *prefix, char *path, size_t cap) {
#ifdef _WIN32
    char full[MAX_PATH];
    if (!GetTempFileNameA(dir, prefix, 0, full)) return -1;   // creates it, too
    if (strlen(full) + 1 > cap) { remove(full); return -1; }
    snprintf(path, cap, "%s", full);
    return 0;
#else
    int fd;
    snprintf(path, cap, "%s/%sXXXXXX", dir, prefix);
    fd = mkstemp(path);
    if (fd < 0) return -1;
    close(fd);
    return 0;
#endif
}

// Push each "Name: value" header line as an argv entry, styled per tool:
// curl wants "-H" then the value (two args); wget/aria2c want one "--header=…".
static void push_headers(argv_b *a, const char *headers, bool curl_style) {
    if (!headers || !headers[0]) return;
    char *copy = rs_strdup(headers);
    char *cursor = copy;
    for (char *line = net_token(&cursor, "\r\n"); line; line = net_token(&cursor, "\r\n")) {
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
    char *cursor = copy;
    for (char *tok = net_token(&cursor, " \t\r\n"); tok; tok = net_token(&cursor, " \t\r\n"))
        if (*tok) ab_push(a, tok);
    free(copy);
}

// Parse an HTTP header dump (curl -D output, or wget --server-response stderr).
// Keeps the last status line (so redirects resolve to the final response) and
// the Content-Range / Content-Type of that response.
static void parse_meta(const char *meta, long *status, char **content_type, char **content_range) {
    char *copy = rs_strdup(meta ? meta : "");
    char *cursor = copy;
    for (char *line = net_token(&cursor, "\r\n"); line; line = net_token(&cursor, "\r\n")) {
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
static int spawn_wait(const char *const argv[], const char *err_path) {
    rs_run_result res;
    // 180s: a whole segment over a slow link is legitimately slow, but a tool
    // that wedges must not pin this thread for the life of the process.
    int rc = rs_proc_run(argv, NULL, active_policy ? (double)active_policy->timeout_seconds : 180.0, false, false, err_path, &res, NULL, 0);
    int code;
    if (rc != 0) {
        // ENOENT is the one failure the caller can recover from: it means the
        // tool simply is not installed, so libcurl takes the request instead.
        code = (res.spawn_error == ENOENT) ? -2 : -1;
    } else if (res.timed_out || res.term_signal != 0) {
        code = -1;
    } else {
        code = res.exit_code;
    }
    rs_run_result_dispose(&res);
    return code;
}

static int fetch_external(const char *tool, const char *dl_params,
                          const char *url, const char *proxy, const char *headers, const char *range,
                          int force_ipv6,
                          char **out, size_t *out_len, long *status, char **content_type,
                          char **content_range, char **effective_url, char *errbuf, size_t errbuf_len) {
    if (content_type) *content_type = NULL;
    if (content_range) *content_range = NULL;
    if (effective_url) *effective_url = NULL;  // external tools don't report it
    if (status) *status = 0;

    char tmpdir_buf[1024];
    const char *tmpdir = temp_dir(tmpdir_buf, sizeof(tmpdir_buf));
    char body_tmpl[1024], meta_tmpl[1024];
    if (make_temp_file(tmpdir, "rsb", body_tmpl, sizeof(body_tmpl)) != 0) {
        snprintf(errbuf, errbuf_len, "Could not create temp file.");
        return -1;
    }
    if (make_temp_file(tmpdir, "rsm", meta_tmpl, sizeof(meta_tmpl)) != 0) {
        remove(body_tmpl);
        snprintf(errbuf, errbuf_len, "Could not create temp file.");
        return -1;
    }

    const char *range_spec = range && range[0]
        ? (strncasecmp(range, "bytes=", 6) == 0 ? range + 6 : range) : NULL;

    char timeout_arg[32], timeout_flag[48];
    snprintf(timeout_arg, sizeof(timeout_arg), "%d", active_policy ? active_policy->timeout_seconds : 60);
    snprintf(timeout_flag, sizeof(timeout_flag), "--timeout=%s", timeout_arg);
    argv_b a = {0};
    bool is_wget = strcasecmp(tool, "wget") == 0;
    bool is_aria = strcasecmp(tool, "aria2c") == 0 || strcasecmp(tool, "aria2") == 0 || strcasecmp(tool, "aria") == 0;
    const char *err_redirect = NULL;

    if (is_wget) {
        ab_push(&a, "wget"); ab_push(&a, "--server-response"); ab_push(&a, "-O"); ab_push(&a, body_tmpl);
        if (force_ipv6) ab_push(&a, "-6");
        ab_push(&a, timeout_flag); ab_push(&a, "--tries=1"); ab_push(&a, "-U"); ab_push(&a, "ReStreamAir/1.0");
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
        char *base = strrchr(basebuf, '/');
#ifdef _WIN32
        char *back = strrchr(basebuf, '\\');
        if (back && (!base || back > base)) base = back;
#endif
        base = base ? base + 1 : basebuf;
        ab_push(&a, "aria2c"); ab_push(&a, "--quiet=true"); ab_push(&a, "--allow-overwrite=true");
        ab_push(&a, "--max-tries=1"); ab_push(&a, "--auto-file-renaming=false"); ab_push(&a, "-x1"); ab_push(&a, "-s1");
        ab_push(&a, "--connect-timeout=10"); ab_push(&a, timeout_flag); ab_push(&a, "-U"); ab_push(&a, "ReStreamAir/1.0");
        ab_push(&a, "-d"); ab_push(&a, tmpdir); ab_push(&a, "-o"); ab_push(&a, base);
        if (proxy && proxy[0]) { char p[1200]; snprintf(p, sizeof(p), "--all-proxy=%s", proxy); ab_push(&a, p); }
        push_headers(&a, headers, false);
        if (range_spec) { char r[128]; snprintf(r, sizeof(r), "--header=Range: bytes=%s", range_spec); ab_push(&a, r); }
        push_extra_params(&a, dl_params);
        ab_push(&a, url);
    } else {  // curl (default)
        ab_push(&a, "curl"); ab_push(&a, "-sS"); ab_push(&a, "-L"); ab_push(&a, "--max-time"); ab_push(&a, timeout_arg);
        if (force_ipv6) ab_push(&a, "-6");
        ab_push(&a, "--connect-timeout"); ab_push(&a, "10"); ab_push(&a, "-A"); ab_push(&a, "ReStreamAir/1.0");
        ab_push(&a, "-o"); ab_push(&a, body_tmpl); ab_push(&a, "-D"); ab_push(&a, meta_tmpl);
        if (proxy && proxy[0]) { ab_push(&a, "-x"); ab_push(&a, proxy); }
        push_headers(&a, headers, true);
        if (range_spec) { ab_push(&a, "--range"); ab_push(&a, range_spec); }
        push_extra_params(&a, dl_params);
        ab_push(&a, url);
    }
    ab_push(&a, NULL);

    int code = spawn_wait((const char *const *)a.v, err_redirect);
    ab_free(&a);

    if (code == -2) {
        snprintf(errbuf, errbuf_len, "Downloader '%s' is not installed.", tool);
        remove(body_tmpl); remove(meta_tmpl);
        return -2;  // signal "missing tool" so the caller can fall back
    }
    if (code != 0) {
        snprintf(errbuf, errbuf_len, "%s exited with code %d.", tool, code);
        remove(body_tmpl); remove(meta_tmpl);
        return -1;
    }

    char *meta = NULL; size_t meta_len = 0;
    if (read_whole_file(meta_tmpl, &meta, &meta_len) == 0) {
        parse_meta(meta, status, content_type, content_range);
        free(meta);
    }
    remove(meta_tmpl);

    char *body = NULL; size_t body_len = 0;
    if (read_whole_file(body_tmpl, &body, &body_len) != 0) {
        remove(body_tmpl);
        snprintf(errbuf, errbuf_len, "Could not read %s output.", tool);
        if (content_type) { free(*content_type); *content_type = NULL; }
        if (content_range) { free(*content_range); *content_range = NULL; }
        return -1;
    }
    remove(body_tmpl);

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

// socks5:// and socks4:// make the CLIENT resolve the target and hand the
// proxy an address. On a dual-stack host that address is often IPv6, which a
// v4-only SOCKS proxy refuses ("cannot complete SOCKS5 connection ... (4)",
// host unreachable) — and it also sends the proxy this server's geo-DNS
// answer instead of its own. Letting the proxy resolve (socks5h/socks4a) is
// what a remote proxy is for, so the plain forms are upgraded. Writes into
// `buf` and returns it when rewritten, else returns `proxy` unchanged.
static const char *proxy_remote_dns(const char *proxy, char *buf, size_t cap) {
    if (!proxy) return proxy;
    const char *rest = NULL, *scheme = NULL;
    if (strncasecmp(proxy, "socks5://", 9) == 0) { rest = proxy + 9; scheme = "socks5h://"; }
    else if (strncasecmp(proxy, "socks4://", 9) == 0) { rest = proxy + 9; scheme = "socks4a://"; }
    if (!rest || (size_t)snprintf(buf, cap, "%s%s", scheme, rest) >= cap) return proxy;
    return buf;
}

// One fetch through one proxy. The public wrapper below expands a newline-
// separated proxy list and calls this until one succeeds.
static int fetch_through_one_proxy(const char *url, const char *proxy,
                                   const char *headers, const char *range,
                                   const char *downloader, const char *dl_params, int force_ipv6,
                                   char **out, size_t *out_len, long *status,
                                   char **content_type, char **content_range,
                                   char **effective_url, char *errbuf, size_t errbuf_len,
                                   long timeout_ms, int (*should_cancel)(void *, size_t), void *cancel_ctx) {
    // One scheme rule for every downloader. The libcurl path pins this with
    // CURLOPT_PROTOCOLS, but an external curl/aria2c is a separate program with
    // its own defaults — and it would read a file:// or ftp:// target happily.
    // Everything this function is ever asked for is HTTP(S), so refuse the rest
    // here rather than depending on which tool the provider happens to select.
    if (!url || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
        snprintf(errbuf, errbuf_len, "Refusing to fetch a non-HTTP(S) URL.");
        return -1;
    }
    char proxy_buf[1100];
    proxy = proxy_remote_dns(proxy, proxy_buf, sizeof(proxy_buf));
    // NULL / "" / "internal" / "libcurl" → in-process libcurl. Otherwise run the
    // chosen external tool, falling back to libcurl if it isn't installed so a
    // missing binary never dead-ends a stream.
    bool internal = !downloader || !downloader[0] ||
                    strcasecmp(downloader, "internal") == 0 || strcasecmp(downloader, "libcurl") == 0 ||
                    strcasecmp(downloader, "native") == 0;
    // aria2 can enable IPv6 but has no IPv6-only equivalent to curl/wget -6.
    // Use libcurl for this combination so "force" never silently falls back
    // to an IPv4 origin address.
    if (force_ipv6 && (strcasecmp(downloader ? downloader : "", "aria2c") == 0 ||
                       strcasecmp(downloader ? downloader : "", "aria2") == 0 ||
                       strcasecmp(downloader ? downloader : "", "aria") == 0))
        internal = true;
    if (active_policy && active_policy->use_cookies) internal = true;
    if (!internal) {
        double started = restream_debug_enabled() ? net_now() : 0;
        int rc = fetch_external(downloader, dl_params, url, proxy, headers, range, force_ipv6,
                                out, out_len, status, content_type, content_range, effective_url, errbuf, errbuf_len);
        if (started > 0) {
            http_buf m = {NULL, 0, 0};
            dbg_appendf(&m, "GET %s via %s%s%s\n", url, downloader,
                        dl_params && dl_params[0] ? " " : "", dl_params ? dl_params : "");
            if (range && range[0]) dbg_appendf(&m, "range: %s\n", range);
            if (proxy && proxy[0]) dbg_append_proxy(&m, proxy);
            if (headers && headers[0]) { dbg_appendf(&m, "request headers:\n"); dbg_append_headers(&m, headers); }
            if (rc == -2) dbg_appendf(&m, "result: %s is not installed, falling back to libcurl", downloader);
            else if (rc != 0) dbg_appendf(&m, "result: failed: %s", errbuf);
            else dbg_appendf(&m, "result: HTTP %ld, %lu bytes", status ? *status : 0L, (unsigned long)*out_len);
            dbg_appendf(&m, "\ntotal %.1f ms", (net_now() - started) * 1000.0);
            restream_debug_log("httpFetch", url, status ? *status : 0, rc == 0 ? (long long)*out_len : -1, m.data);
            free(m.data);
        }
        if (rc != -2) return rc;  // -2 = tool missing → fall through to libcurl
    }
    return fetch_libcurl(url, proxy, headers, range, force_ipv6, out, out_len, status, content_type,
                         content_range, effective_url, errbuf, errbuf_len,
                         timeout_ms, should_cancel, cancel_ctx);
}

#define RS_PROXY_FALLBACK_MAX 16
#define RS_PROXY_POOL_MAX 64

typedef struct {
    bool known;
    bool healthy;
    unsigned failures;
    unsigned in_flight;
    time_t retry_at;
} proxy_health;

typedef struct {
    char *list;
    size_t next_rotate;
    proxy_health proxy[RS_PROXY_FALLBACK_MAX];
} proxy_pool;

static proxy_pool g_proxy_pools[RS_PROXY_POOL_MAX];
static size_t g_proxy_pool_count = 0;
static pthread_mutex_t g_proxy_pool_mu = PTHREAD_MUTEX_INITIALIZER;

// Split one-proxy-per-line in place, trimming surrounding spaces. A legacy
// single URL naturally produces one entry, so no state migration is needed.
static size_t split_proxy_list(char *copy, char **items, size_t cap) {
    size_t count = 0;
    char *cursor = copy;
    while (cursor && *cursor && count < cap) {
        while (*cursor == '\r' || *cursor == '\n') cursor++;
        if (!*cursor) break;
        char *line = cursor;
        while (*cursor && *cursor != '\r' && *cursor != '\n') cursor++;
        if (*cursor) *cursor++ = '\0';
        while (*line == ' ' || *line == '\t') line++;
        char *end = line + strlen(line);
        while (end > line && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        if (*line) items[count++] = line;
    }
    return count;
}

// Finds (or creates) health state for an exact configured list. A changed list
// gets a fresh pool because its indices no longer identify the same proxies.
static proxy_pool *proxy_pool_locked(const char *list, size_t count) {
    for (size_t i = 0; i < g_proxy_pool_count; i++)
        if (strcmp(g_proxy_pools[i].list, list) == 0) return &g_proxy_pools[i];
    if (g_proxy_pool_count >= RS_PROXY_POOL_MAX) return NULL;
    char *copy = rs_strdup(list);
    if (!copy) return NULL;
    proxy_pool *pool = &g_proxy_pools[g_proxy_pool_count++];
    memset(pool, 0, sizeof(*pool));
    pool->list = copy;
    (void)count;
    return pool;
}

// Unknown proxies go first so the first few real requests check every entry
// without fetching one object more than once. Failed entries are quarantined
// with exponential backoff, then receive an ordinary request as a recovery
// probe. If the whole pool is quarantined, the oldest entry is allowed through
// so it can recover without external intervention.
static size_t proxy_acquire(const char *list, size_t count, bool rotate,
                            unsigned tried_mask) {
    time_t now = time(NULL);
    size_t chosen = count;
    pthread_mutex_lock(&g_proxy_pool_mu);
    proxy_pool *pool = proxy_pool_locked(list, count);
    if (!pool) {
        pthread_mutex_unlock(&g_proxy_pool_mu);
        for (size_t i = 0; i < count; i++) if (!(tried_mask & (1u << i))) return i;
        return count;
    }

    // Sticky per thread: with rotation a thread used to alternate proxies on
    // every request, holding a tunnel through each one. Threads are spread
    // round-robin when they first pick, then each keeps its proxy while it is
    // healthy — the load still splits across the pool, one connection each.
    if (rotate && t_proxy_pool == (const void *)pool && t_proxy_index < count &&
        !(tried_mask & (1u << t_proxy_index)) && pool->proxy[t_proxy_index].healthy)
        chosen = t_proxy_index;
    // Spread initial health checks across concurrent download workers.
    for (size_t i = 0; chosen == count && i < count; i++) {
        if (!(tried_mask & (1u << i)) && !pool->proxy[i].known && pool->proxy[i].in_flight == 0) {
            chosen = i;
            break;
        }
    }
    if (chosen == count) {
        for (size_t i = 0; i < count; i++) {
            proxy_health *h = &pool->proxy[i];
            if (!(tried_mask & (1u << i)) && h->known && !h->healthy &&
                h->retry_at <= now && h->in_flight == 0) {
                chosen = i;
                break;
            }
        }
    }
    if (chosen == count) {
        size_t start = rotate && count ? pool->next_rotate % count : 0;
        for (size_t step = 0; step < count; step++) {
            size_t i = rotate ? (start + step) % count : step;
            if (!(tried_mask & (1u << i)) && pool->proxy[i].healthy) {
                chosen = i;
                break;
            }
        }
        // Advance at hand-out, not on completion: the download pool asks for
        // many proxies before any request finishes, and advancing only on
        // success sent every one of those concurrent requests to the same
        // entry.
        if (rotate && chosen < count) pool->next_rotate = (chosen + 1) % count;
    }
    // Healthy entries may all be busy. Sharing one is still better than using
    // a proxy that is inside its failure cooldown.
    if (chosen == count) {
        for (size_t i = 0; i < count; i++)
            if (!(tried_mask & (1u << i)) && pool->proxy[i].healthy) { chosen = i; break; }
    }
    if (chosen == count) {
        time_t earliest = 0;
        for (size_t i = 0; i < count; i++) {
            if (tried_mask & (1u << i)) continue;
            if (chosen == count || pool->proxy[i].retry_at < earliest) {
                chosen = i;
                earliest = pool->proxy[i].retry_at;
            }
        }
    }
    if (chosen < count) pool->proxy[chosen].in_flight++;
    pthread_mutex_unlock(&g_proxy_pool_mu);
    return chosen;
}

typedef enum { PROXY_OK, PROXY_FAILED, PROXY_NO_VERDICT } proxy_outcome;

// Returns the cooldown in seconds when this release benched the proxy, -1 when
// it brought a benched proxy back, 0 otherwise — so the caller logs only state
// changes, not every request.
static int proxy_release(const char *list, size_t count, size_t index,
                         proxy_outcome outcome) {
    int change = 0;
    pthread_mutex_lock(&g_proxy_pool_mu);
    proxy_pool *pool = proxy_pool_locked(list, count);
    if (pool && index < count) {
        proxy_health *h = &pool->proxy[index];
        if (h->in_flight) h->in_flight--;
        if (outcome == PROXY_NO_VERDICT) {
            // Cancelled requests say nothing about the proxy.
        } else if (outcome == PROXY_OK) {
            t_proxy_pool = (const void *)pool;
            t_proxy_index = index;
            if (h->known && !h->healthy) change = -1;
            h->known = true;
            h->healthy = true;
            h->failures = 0;
            h->retry_at = 0;
        } else {
            // Requests already in flight when the proxy was benched fail too;
            // they extend nothing and say nothing new, so only log the first.
            bool already_benched = h->known && !h->healthy && h->retry_at > time(NULL);
            h->known = true;
            h->healthy = false;
            if (h->failures < 6) h->failures++;
            unsigned delay = 15u << (h->failures ? h->failures - 1 : 0);
            if (delay > 300u) delay = 300u;
            h->retry_at = time(NULL) + (time_t)delay;
            if (!already_benched) change = (int)delay;
        }
    }
    pthread_mutex_unlock(&g_proxy_pool_mu);
    return change;
}

static int fetch_with_proxies(const char *url, const char *proxy, const char *headers, const char *range,
                 const char *downloader, const char *dl_params, int force_ipv6, int rotate_proxies,
                 char **out, size_t *out_len, long *status, char **content_type,
                 char **content_range, char **effective_url, char *errbuf, size_t errbuf_len,
                 long timeout_ms, int (*should_cancel)(void *, size_t), void *cancel_ctx) {
    if (!proxy || !strpbrk(proxy, "\r\n")) {
        // A single proxy pasted with a trailing space reached libcurl as part
        // of the host name ("Could not resolve proxy name"); the list path
        // below trims each entry, so trim this one the same way.
        char one[1100];
        if (proxy && (*proxy == ' ' || *proxy == '\t' ||
                      (*proxy && (proxy[strlen(proxy) - 1] == ' ' || proxy[strlen(proxy) - 1] == '\t')))) {
            const char *start = proxy;
            while (*start == ' ' || *start == '\t') start++;
            size_t len = strlen(start);
            while (len && (start[len - 1] == ' ' || start[len - 1] == '\t')) len--;
            if (len < sizeof(one)) { memcpy(one, start, len); one[len] = '\0'; proxy = one; }
        }
        return fetch_through_one_proxy(url, proxy && proxy[0] ? proxy : NULL, headers, range, downloader,
                                       dl_params, force_ipv6,
                                       out, out_len, status, content_type, content_range,
                                       effective_url, errbuf, errbuf_len,
                                       timeout_ms, should_cancel, cancel_ctx);
    }

    char *copy = rs_strdup(proxy);
    if (!copy) {
        snprintf(errbuf, errbuf_len, "Out of memory while reading proxy fallbacks.");
        return -1;
    }
    char *items[RS_PROXY_FALLBACK_MAX];
    size_t count = split_proxy_list(copy, items, RS_PROXY_FALLBACK_MAX);
    if (count == 0) {
        free(copy);
        return fetch_through_one_proxy(url, NULL, headers, range, downloader, dl_params, force_ipv6,
                                       out, out_len, status, content_type, content_range,
                                       effective_url, errbuf, errbuf_len,
                                       timeout_ms, should_cancel, cancel_ctx);
    }

    int rc = -1;
    size_t attempted = 0;
    unsigned tried_mask = 0;
    while (attempted < count) {
        if (attempted > 0 && should_cancel && should_cancel(cancel_ctx, 0)) break;
        size_t index = proxy_acquire(proxy, count, rotate_proxies != 0, tried_mask);
        if (index >= count) break;
        tried_mask |= 1u << index;
        attempted++;
        long attempt_status = 0;
        long *status_out = status ? status : &attempt_status;
        *status_out = 0;  // never judge this proxy by the previous attempt's code
        rc = fetch_through_one_proxy(url, items[index], headers, range, downloader, dl_params, force_ipv6,
                                     out, out_len, status_out, content_type, content_range,
                                     effective_url, errbuf, errbuf_len,
                                     timeout_ms, should_cancel, cancel_ctx);
        // Health is provider-specific: a proxy that answers but cannot fetch
        // this provider's URL (geo 403, 407, transport error, 5xx) is not
        // useful to this pool and goes behind the working entries for a
        // cooldown. Two outcomes are not the proxy's fault and used to
        // quarantine healthy entries under load: a request the caller
        // cancelled (live-edge drop, stream stop), and an origin answer such
        // as 404/410/416 that every other proxy would repeat.
        long got = *status_out;
        bool cancelled = rc != 0 && should_cancel && should_cancel(cancel_ctx, 0);
        bool origin_final = rc != 0 && got >= 400 && got < 500 &&
                            got != 403 && got != 407 && got != 408 && got != 429;
        proxy_outcome outcome = rc == 0 || origin_final ? PROXY_OK
                              : cancelled ? PROXY_NO_VERDICT : PROXY_FAILED;
        int change = proxy_release(proxy, count, index, outcome);
        if (change) {
            // Name the proxy by host:port only; the credentials stay out of logs.
            const char *at = strrchr(items[index], '@');
            const char *host = at ? at + 1 : items[index];
            const char *scheme_end = at ? NULL : strstr(host, "://");
            if (scheme_end) host = scheme_end + 3;
            char msg[512];
            if (change > 0)
                snprintf(msg, sizeof(msg), "proxy %s failed (%s) — skipped for %ds, using the other proxies",
                         host, errbuf && errbuf[0] ? errbuf : "no response", change);
            else
                snprintf(msg, sizeof(msg), "proxy %s is answering again — back in rotation", host);
            restream_log_event(change > 0 ? "error" : "info", "proxyHealth", msg);
        }
        if (rc == 0 || cancelled || origin_final) break;
    }
    if (rc != 0 && count > 1 && attempted > 0 && errbuf && errbuf_len) {
        size_t used = strnlen(errbuf, errbuf_len);
        const char *suffix = " (all available proxies failed)";
        if (used + strlen(suffix) + 1 < errbuf_len) strcat(errbuf, suffix);
    }
    free(copy);
    return rc;
}


// A provider budget includes all of its concurrent upstream requests. Entries
// are reference counted while waiting/running, so deleting a provider cannot
// leave a worker holding a freed limiter. Cookie-jar requests serialize to
// avoid concurrent writes to the script's session file.
typedef struct provider_budget {
    char id[128];
    int active, users;
    struct provider_budget *next;
} provider_budget;
static provider_budget *budgets;
static pthread_mutex_t budget_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t budget_cv = PTHREAD_COND_INITIALIZER;

static double net_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static void budget_drop_locked(provider_budget *b) {
    if (--b->users != 0) return;
    provider_budget **link = &budgets;
    while (*link && *link != b) link = &(*link)->next;
    if (*link) *link = b->next;
    free(b);
}
static provider_budget *budget_enter(const rs_source_policy *policy,
                                     int (*cancel)(void *, size_t), void *ctx) {
    if (!policy || !policy->provider_id[0]) return NULL;
    int limit = policy->use_cookies ? 1 : policy->max_downloads;
    if (limit < 1) limit = 1;
    // Playlist reads get headroom above the download budget. Waiters are not
    // queued in order, so with more segment workers than slots (16 streams x
    // 6 connections against a budget of 50 on production) a manifest poll lost
    // the race for 20-90 s, its segments aged out of the window and the stream
    // stalled into a restart. A manifest is a few KB; letting a handful of them
    // past a full budget costs nothing. The cookie jar still serializes.
    if (manifest_priority && !policy->use_cookies) limit += limit / 4 > 2 ? limit / 4 : 2;
    double deadline = net_now() + policy->timeout_seconds;
    pthread_mutex_lock(&budget_mu);
    provider_budget *b = budgets;
    while (b && strcmp(b->id, policy->provider_id) != 0) b = b->next;
    if (!b) {
        b = (provider_budget *)calloc(1, sizeof(*b));
        if (!b) { pthread_mutex_unlock(&budget_mu); return NULL; }
        snprintf(b->id, sizeof(b->id), "%s", policy->provider_id);
        b->next = budgets; budgets = b;
    }
    b->users++;
    while (b->active >= limit) {
        pthread_mutex_unlock(&budget_mu);
        bool expired = net_now() >= deadline || (cancel && cancel(ctx, 0));
        pthread_mutex_lock(&budget_mu);
        if (expired) { budget_drop_locked(b); pthread_mutex_unlock(&budget_mu); return NULL; }
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 100000000;
        if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&budget_cv, &budget_mu, &until);
    }
    b->active++;
    pthread_mutex_unlock(&budget_mu);
    return b;
}
static void budget_leave(provider_budget *b) {
    if (!b) return;
    pthread_mutex_lock(&budget_mu);
    b->active--;
    budget_drop_locked(b);
    pthread_cond_broadcast(&budget_cv);
    pthread_mutex_unlock(&budget_mu);
}

static char *json_redirect_target(const char *data, size_t len, const char *base) {
    if (!data || len > 1024 * 1024) return NULL;
    rs_json *doc = rs_json_parse(data, len);
    if (!doc) return NULL;
    const rs_json *objects[2] = {doc, rs_json_obj_get(doc, "data")};
    static const char *keys[] = {"ManifestUrl", "manifestUrl", "url", "redirect", "location", "redirectUrl"};
    char *result = NULL;
    for (size_t i = 0; i < 2 && !result; i++) {
        for (size_t j = 0; j < sizeof(keys) / sizeof(keys[0]); j++) {
            const char *u = rs_json_as_str(rs_json_obj_get(objects[i], keys[j]), "");
            if (!u[0]) continue;
            result = rs_url_resolve(base, u);
            if (result && strncmp(result, "https://", 8) && strncmp(result, "http://", 7)) {
                free(result); result = NULL;
            }
            if (result) break;
        }
    }
    rs_json_free(doc);
    return result;
}

int rs_fetch_url(const char *url, const char *proxy, const char *headers, const char *range,
                 const char *downloader, const char *dl_params, int force_ipv6, int rotate_proxies,
                 char **out, size_t *out_len, long *status, char **content_type,
                 char **content_range, char **effective_url, char *errbuf, size_t errbuf_len,
                 long timeout_ms, int (*should_cancel)(void *, size_t), void *cancel_ctx,
                 const rs_source_policy *policy) {
    // A zero-initialized policy belongs to a standalone probe, not a provider.
    if (policy && !policy->provider_id[0]) policy = NULL;
    *out = NULL; *out_len = 0;
    long local_status = 0;
    long *result_status = status ? status : &local_status;
    *result_status = 0;
    if (content_type) *content_type = NULL;
    if (content_range) *content_range = NULL;
    if (effective_url) *effective_url = NULL;
    provider_budget *budget = budget_enter(policy, should_cancel, cancel_ctx);
    if (policy && !budget) { snprintf(errbuf, errbuf_len, "Provider request budget wait cancelled or timed out."); return -1; }
    const rs_source_policy *previous = active_policy;
    active_policy = policy;
    if (policy) timeout_ms = (long)policy->timeout_seconds * 1000;
    if (policy && policy->use_cookies) {
        char dir[256];
        POLICY_MKDIR("runtime"); POLICY_MKDIR("runtime/sessions");
        snprintf(dir, sizeof(dir), "runtime/sessions/%s", policy->provider_id);
        POLICY_MKDIR(dir);
    }
    int tries = policy ? policy->attempts : 1;
    if (tries < 1) tries = 1;
    int rc = -1;
    char *current = rs_strdup(url);
    for (int redirects = 0; current && redirects <= 5; redirects++) {
        for (int attempt = 0; attempt < tries; attempt++) {
            if (should_cancel && should_cancel(cancel_ctx, 0)) break;
            rc = fetch_with_proxies(current, proxy, headers, range, downloader, dl_params,
                                    force_ipv6, rotate_proxies, out, out_len, result_status,
                                    content_type, content_range, effective_url, errbuf, errbuf_len,
                                    timeout_ms, should_cancel, cancel_ctx);
            if (rc == 0) break;
            if (*result_status >= 400 && *result_status < 500 && *result_status != 408 && *result_status != 429) break;
        }
        if (rc != 0 || !policy || !policy->detect_json_redirect) break;
        const char *base = effective_url && *effective_url ? *effective_url : current;
        char *next = json_redirect_target(*out, *out_len, base);
        if (!next) break;
        free(*out); *out = NULL; *out_len = 0;
        if (content_type) { free(*content_type); *content_type = NULL; }
        if (content_range) { free(*content_range); *content_range = NULL; }
        if (effective_url) { free(*effective_url); *effective_url = NULL; }
        if (redirects == 5 || strcmp(next, current) == 0) {
            free(next); rc = -1;
            snprintf(errbuf, errbuf_len, "JSON redirect loop or limit reached."); break;
        }
        free(current); current = next;
    }
    free(current);
    active_policy = previous;
    budget_leave(budget);
    return rc;
}

int rs_post_json(const char *url, const char *json, long *status,
                 char *errbuf, size_t errbuf_len) {
    if (status) *status = 0;
    if (!url || !url[0] || !json) {
        snprintf(errbuf, errbuf_len, "Webhook URL and JSON body are required.");
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(errbuf, errbuf_len, "Could not initialise webhook HTTP client.");
        return -1;
    }
    apply_ca_bundle(curl);
    struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
    http_buf response = {NULL, 0, 0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(json));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ReStreamAir/1.0");

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (status) *status = code;
    if (rc != CURLE_OK) {
        snprintf(errbuf, errbuf_len, "Webhook request failed: %s", curl_easy_strerror(rc));
    } else if (code < 200 || code >= 300) {
        snprintf(errbuf, errbuf_len, "Webhook returned HTTP %ld.", code);
        rc = CURLE_HTTP_RETURNED_ERROR;
    }
    free(response.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK ? 0 : -1;
}
