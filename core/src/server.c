#include "restream.h"
#include "rs_common.h"
#include "rs_auth.h"
#include "rs_cdm.h"
#include "rs_cenc.h"
#include "rs_hls_decrypt.h"
#include "rs_ffargs.h"
#include "rs_json.h"
#include "rs_live.h"
#include "rs_logo.h"
#include "rs_m3u8.h"
#include "rs_metrics.h"
#include "rs_mpegts.h"
#include "rs_netmatch.h"
#include "rs_nm3u8dlre.h"
#include "rs_panel.h"
#include "rs_script.h"
#include "rs_state.h"
#include "rs_internal.h"
#include "rs_sysstats.h"
#include "../deps/mongoose.h"
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
// live.c already spawns pthreads unconditionally (director/rep/prefetch workers)
// and restream_core links Threads::Threads on every platform including Windows,
// so this is available everywhere the live engine already relies on it being.
#include <pthread.h>
#ifndef _WIN32
#include <sys/stat.h>   // mkdir for the script session dir
#endif

// In-memory log ring buffer. Each entry is a panel/stream event the Logs tab
// renders: the synthetic stream id "__panel__" holds server/auth/start-stop and
// access lines, "script:<providerId>" holds script output, and real stream ids
// hold per-stream fetch/download activity.
//
// Sized for the live engine, which reports every poll, every segment, every
// discontinuity and every prune for every representation of every running
// stream. At roughly one line per segment per rendition that is a few thousand
// lines an hour per stream, so the ring has to be deep enough to still hold
// something useful from before whatever the user is currently investigating.
#define RS_LOG_CAP 20000
typedef struct {
    double ts_ms;     // milliseconds since the epoch (what the UI's Date() wants)
    char *level;      // "info" | "error"
    char *sid;        // stream id / "__panel__" / "script:<id>"
    char *event;
    char *message;    // optional detail
    char *url;        // optional
    long status;      // optional HTTP status (0 = none)
    long long bytes;  // optional byte count (-1 = none)
} rs_log_entry;

// An in-flight HLS-passthrough fetch (playlist or segment/key/init), dispatched
// to a worker thread so it never blocks the mongoose poll loop. Full definition
// and the machinery around it sit just above serve_hls_playlist, the only
// callers. Forward-declared here so restream_server can hold the pending list.
typedef struct rs_pending_job rs_pending_job;

// Per-sid "cleared before" cutoffs for the log ring — see log_clear_cutoff,
// defined with log_clear/log_view further down. Forward-declared here so
// restream_server can hold the list.
typedef struct rs_log_clear_entry rs_log_clear_entry;

// Direct-link viewers and the per-stream MPEG-TS muxes they share. Full
// definitions sit with the direct-link routes further down; forward-declared
// here so restream_server can hold the lists.
typedef struct rs_direct_client rs_direct_client;
typedef struct rs_ts_client rs_ts_client;
typedef struct rs_ts_session rs_ts_session;

struct restream_server {
    struct mg_mgr mgr;
    struct mg_connection *c;
    bool is_running;
    bool wakeup_ok;          // mg_wakeup_init succeeded — async fetches are safe to dispatch
    char *web_root;         // static file directory, or NULL
    rs_state state;         // state.json as a preserved DOM
    rs_auth *auth;          // admin accounts + sessions
    rs_sysstats *sysstats;  // live host stats for the monitoring view
    rs_metrics *metrics;    // per-stream client/bytes network monitor
    rs_pending_job *pending_head; // in-flight async jobs (playback fetches, probe, logo lookup, script actions)
    pthread_mutex_t pending_mu;     // guards pending_head and each entry's `cancelled` flag
    pthread_cond_t pending_cv;      // signalled whenever a job unlinks itself; wakes restream_server_destroy's drain
    pthread_mutex_t logo_mu;        // serialises RS_PENDING_LOGO workers' access to logo_cache (not itself thread-safe)
    uint16_t listen_port;   // the port actually bound, which is what /api/state must report
    // A restart requested from Settings. Restarting replaces this process, so it
    // cannot happen inside the request handler — the reply would never reach the
    // browser. The one-second timer performs it instead, by which point mongoose
    // has flushed the response and the operator has seen it succeed.
    bool restart_pending;
    rs_live *live;          // background DASH->HLS engines, one per running stream
    rs_direct_client *direct_head;  // open fMP4 byte-tail viewers
    rs_ts_client *ts_head;          // open muxed-MPEG-TS viewers
    rs_ts_session *ts_sessions;     // one mux per stream with .ts viewers on it
    rs_logo_cache *logo_cache;
    rs_log_entry log_ring[RS_LOG_CAP];
    size_t log_head;        // next write slot
    size_t log_count;       // entries in use (<= RS_LOG_CAP)
    rs_log_clear_entry *log_clears; // per-sid "cleared before" cutoffs; guarded by log_mu
#ifndef _WIN32
    pthread_mutex_t log_mu; // the live engine's worker threads write here too
#endif
};

// A live Server-Sent Events subscriber is marked in mongoose's per-connection
// scratch space, so the broadcast timer can find them.
#define RS_SSE_MARKER 'S'

// The probe/fetch handlers are registered by the C++ app; the core only calls
// through them, so libcurl/libxml2 never reach the Swift build. Declared here so
// everything below (including the logo fetcher and the live engine) can
// reference them before their setters are defined.
static restream_probe_fn g_probe_handler = NULL;
static restream_fetch_fn g_fetch_handler = NULL;
static restream_dash_fn g_dash_handler = NULL;
static restream_service_status_fn g_service_status = NULL;
static restream_service_action_fn g_service_action = NULL;

// The live-engine control plane is defined with the playback handlers, further
// down, but the /api/streams/*/start|stop|update|delete routes above them have
// to be able to start and stop engines.
static void live_sync_stream(restream_server_t *s, const char *stream_id);
static void live_stop_stream(restream_server_t *s, const char *stream_id);
static void live_sync_all(restream_server_t *s);
static const char *stream_source_target(const rs_json *stream);

// Forward declaration: query helpers are defined with the routing code below,
// but the playback handlers above them need it.
static char *query_var(struct mg_http_message *hm, const char *name);

// Forward declarations: the async HLS-passthrough fetch machinery is defined
// just above serve_hls_playlist (its only dispatcher), but ev_handler,
// restream_server_create and restream_server_destroy all need to reach it.
static void pending_job_cancel(restream_server_t *s, unsigned long conn_id);
static void pending_job_finish(restream_server_t *s, struct mg_connection *c, struct mg_str *data);
static void pending_job_wait_idle(restream_server_t *s);

// Forward declaration: apply_cenc is defined between serve_hls_playlist and
// serve_restream_item, but pending_job_finish_item (defined just above
// serve_hls_playlist) needs it too.
static uint8_t *apply_cenc(const char *decryption_keys, bool is_map, uint8_t *body, size_t body_len, size_t *out_len);

// Forward declarations: dispatch_logo_lookup/dispatch_probe/dispatch_script_action
// are defined alongside pending_job_dispatch (just above serve_hls_playlist),
// but handle_api — defined well before that — is their only caller. Each always
// replies to `c` itself, either immediately with an error or (once dispatched)
// asynchronously once the worker finishes.
static void dispatch_logo_lookup(restream_server_t *server, struct mg_connection *c, const char *name);
static void dispatch_probe(restream_server_t *server, struct mg_connection *c,
                           char *url, char *proxy, char *headers);
static void dispatch_script_action(restream_server_t *server, struct mg_connection *c,
                                   const char *sid, const char *script_path, char **args, int argc);

static char *logo_fetch_wrapper(const char *url, void *ctx) {
    (void)ctx;
    if (!g_fetch_handler) return NULL;
    char *body = NULL;
    size_t body_len = 0;
    long status = 0;
    char err[128] = {0};
    int rc = g_fetch_handler(url, NULL, NULL, NULL, NULL, NULL, &body, &body_len, &status,
                             NULL, NULL, NULL, err, sizeof(err), 30000, NULL, NULL);
    if (rc == 0 && status == 200 && body) return body;
    free(body);
    return NULL;
}

// --- log store -------------------------------------------------------------

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

// Lifecycle events that must reach the log verbatim, however often they repeat.
// Start/stop in particular: a stop-then-start inside the de-duplication window
// used to leave no trace at all, so the Logs tab could not answer the one
// question it is opened for — "did my click do anything?".
static bool log_always(const char *event) {
    static const char *keep[] = {
        "serverStart", "streamStart", "streamStop", "streamCreate", "streamUpdate",
        "streamDelete", "liveStart", "liveStop", "repStart", "repStop",
        "playlistReady", "renditions", "initReady", "discontinuity",
        "login", "logout", "loginFailed", "scriptStart", "scriptEnd", "playbackDenied",
    };
    if (!event) return false;
    for (size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); i++)
        if (strcmp(event, keep[i]) == 0) return true;
    return false;
}

static void log_lock(restream_server_t *s) {
#ifndef _WIN32
    pthread_mutex_lock(&s->log_mu);
#else
    (void)s;
#endif
}
static void log_unlock(restream_server_t *s) {
#ifndef _WIN32
    pthread_mutex_unlock(&s->log_mu);
#else
    (void)s;
#endif
}

// Records one log entry (ring buffer overwrites the oldest). url/message may be
// NULL; status 0 and bytes -1 mean "absent". Never blocks on I/O, and is safe to
// call from the live engine's worker threads.
static void log_record(restream_server_t *s, const char *sid, const char *level,
                       const char *event, const char *url, long status, long long bytes,
                       const char *message) {
    log_lock(s);
    // Collapse only an exact repeat inside one second, which exists purely to
    // stop a pathological retry loop from filling the ring in a few seconds.
    // Everything else is recorded: the point of this log is to show what the
    // server actually did, and an earlier thirty-second window was hiding the
    // per-poll and per-segment activity that makes a stalled stream diagnosable.
    if (s->log_count > 0 && !log_always(event)) {
        double cutoff = now_ms() - 1000.0;
        for (size_t i = 0; i < s->log_count && i < 8; i++) {
            size_t idx = (s->log_head + RS_LOG_CAP - 1 - i) % RS_LOG_CAP;
            rs_log_entry *p = &s->log_ring[idx];
            if (p->ts_ms < cutoff) break;
            if (strcmp(p->sid, sid ? sid : "__panel__") != 0) continue;
            if (strcmp(p->event, event ? event : "") != 0) continue;
            const char *pm = p->message ? p->message : "";
            const char *nm = message ? message : "";
            const char *pu = p->url ? p->url : "";
            const char *nu = url ? url : "";
            if (strcmp(pm, nm) == 0 && strcmp(pu, nu) == 0) { log_unlock(s); return; }
        }
    }
    rs_log_entry *e = &s->log_ring[s->log_head];
    if (s->log_count == RS_LOG_CAP) {
        // Overwriting the oldest live entry — free its strings first.
        free(e->level); free(e->sid); free(e->event); free(e->message); free(e->url);
    }
    e->ts_ms = now_ms();
    e->level = rs_strdup(level ? level : "info");
    e->sid = rs_strdup(sid ? sid : "__panel__");
    e->event = rs_strdup(event ? event : "");
    e->message = message ? rs_strdup(message) : NULL;
    e->url = url ? rs_strdup(url) : NULL;
    e->status = status;
    e->bytes = bytes;
    s->log_head = (s->log_head + 1) % RS_LOG_CAP;
    if (s->log_count < RS_LOG_CAP) s->log_count++;
    log_unlock(s);
}

// printf-style log_record, for the many call sites that have to assemble a
// message from counts and timings.
static void log_recordf(restream_server_t *s, const char *sid, const char *level,
                        const char *event, const char *url, long status, long long bytes,
                        const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    log_record(s, sid, level, event, url, status, bytes, msg);
}

// The live engine's log sink. Called from its worker threads, which is why
// log_record takes a lock.
static void live_log_sink(void *ctx, const char *stream_id, const char *level,
                          const char *event, const char *url, long status,
                          long long bytes, const char *message) {
    log_record((restream_server_t *)ctx, stream_id, level, event, url, status, bytes, message);
}

// Per-sid "cleared before" cutoff: log_clear(sid) can't compact the ring (its
// slots are shared by every sid, addressed only by position), so a per-stream
// clear instead records a timestamp and log_view hides anything from that sid
// older than it. A full clear (sid NULL/"") still empties the ring outright,
// which also makes every per-sid cutoff moot — cleared there too.
struct rs_log_clear_entry {
    char *sid;
    double cutoff_ms;
    struct rs_log_clear_entry *next;
};

// Caller holds log_mu.
static double log_clear_cutoff(restream_server_t *s, const char *sid) {
    for (rs_log_clear_entry *e = s->log_clears; e; e = e->next)
        if (strcmp(e->sid, sid) == 0) return e->cutoff_ms;
    return 0;
}

// Builds the {entries:[...],availableDates:[]} response, newest→oldest (the
// frontend renders top-to-bottom and expects the newest entry first — see
// loadLogs()/groupLogEntries() in app.js), keeping at most `limit` of the
// newest that match `sid` (NULL/"" = all).
static rs_json *log_view(restream_server_t *s, const char *sid, int limit) {
    rs_json *entries = rs_json_new_arr();
    if (limit <= 0) limit = 150;
    if (limit > RS_LOG_CAP) limit = RS_LOG_CAP;
    int n = 0;
    log_lock(s);
    // Walk newest→oldest; the ring already stores them that way relative to
    // log_head, so pushing in this order needs no separate reverse pass.
    for (size_t i = 0; i < s->log_count && n < limit; i++) {
        size_t idx = (s->log_head + RS_LOG_CAP - 1 - i) % RS_LOG_CAP;
        rs_log_entry *e = &s->log_ring[idx];
        if (sid && sid[0] && strcmp(sid, e->sid) != 0) continue;
        double cutoff = log_clear_cutoff(s, e->sid);
        if (cutoff > 0 && e->ts_ms < cutoff) continue;
        rs_json *o = rs_json_new_obj();
        rs_json_obj_set(o, "timestamp", rs_json_new_num(e->ts_ms));
        rs_json_obj_set_str(o, "level", e->level);
        rs_json_obj_set_str(o, "streamId", e->sid);
        rs_json_obj_set_str(o, "event", e->event);
        if (e->url) rs_json_obj_set_str(o, "url", e->url);
        if (e->message) rs_json_obj_set_str(o, "message", e->message);
        if (e->status) rs_json_obj_set_int(o, "status", e->status);
        if (e->bytes >= 0) rs_json_obj_set_int(o, "bytes", e->bytes);
        rs_json_arr_push(entries, o);
        n++;
    }
    log_unlock(s);
    rs_json *out = rs_json_new_obj();
    rs_json_obj_set(out, "entries", entries);
    rs_json_obj_set(out, "availableDates", rs_json_new_arr());
    return out;
}

// sid NULL/"" empties the ring outright. Otherwise (the panel's per-stream
// "Clear" button) the ring itself is untouched — see rs_log_clear_entry above
// — so log_view hides everything from that sid up to now, while new activity
// for it still shows up.
static void log_clear(restream_server_t *s, const char *sid) {
    log_lock(s);
    if (!sid || !sid[0]) {
        for (size_t i = 0; i < s->log_count; i++) {
            size_t idx = (s->log_head + RS_LOG_CAP - 1 - i) % RS_LOG_CAP;
            rs_log_entry *e = &s->log_ring[idx];
            free(e->level); free(e->sid); free(e->event); free(e->message); free(e->url);
            memset(e, 0, sizeof(*e));
        }
        s->log_count = 0; s->log_head = 0;
        while (s->log_clears) {
            rs_log_clear_entry *next = s->log_clears->next;
            free(s->log_clears->sid);
            free(s->log_clears);
            s->log_clears = next;
        }
    } else {
        rs_log_clear_entry *e = s->log_clears;
        for (; e; e = e->next) if (strcmp(e->sid, sid) == 0) break;
        if (!e) {
            e = (rs_log_clear_entry *)calloc(1, sizeof(*e));
            e->sid = rs_strdup(sid);
            e->next = s->log_clears;
            s->log_clears = e;
        }
        e->cutoff_ms = now_ms();
    }
    log_unlock(s);
}

// --- small response helpers ------------------------------------------------

static const char *JSON_HEADERS = "Content-Type: application/json\r\n";

// Security headers for the request currently being handled. Whether HSTS
// belongs on a response depends on how *that* request reached us, and threading
// that through the ~40 reply sites by hand would be forgotten at exactly the one
// that mattered. mongoose runs every connection on a single thread, so a
// request-scoped static is safe: set_request_security() fills it at the top of
// each request and clears it for replies that outlive one (async job results).
static char g_request_security[192] = "";

// A JSON error body the panel's fetch layer reads as `payload.error`.
static void reply_error(struct mg_connection *c, int status, const char *message) {
    char headers[320];
    snprintf(headers, sizeof(headers), "%s%s", JSON_HEADERS, g_request_security);
    mg_http_reply(c, status, headers, "{\"error\":%m}", MG_ESC(message));
}

// The same, with extra headers — a 429 has to carry Retry-After to be useful.
static void reply_error_headers(struct mg_connection *c, int status, const char *message,
                                const char *extra_headers) {
    char headers[512];
    snprintf(headers, sizeof(headers), "%s%s%s", JSON_HEADERS, g_request_security,
             extra_headers ? extra_headers : "");
    mg_http_reply(c, status, headers, "{\"error\":%m}", MG_ESC(message));
}

// Serializes and sends a JSON DOM, then frees it. `extra_headers` may be NULL.
static void reply_json(struct mg_connection *c, int status, rs_json *value,
                       const char *extra_headers) {
    char *body = rs_json_serialize(value, false);
    rs_json_free(value);
    if (!body) {
        reply_error(c, 500, "Out of memory building the response.");
        return;
    }
    char headers[640];
    snprintf(headers, sizeof(headers), "%sCache-Control: no-store\r\n%s%s",
             JSON_HEADERS, g_request_security, extra_headers ? extra_headers : "");
    mg_http_reply(c, status, headers, "%s", body);
    rs_free(body);
}

static bool method_is(struct mg_http_message *hm, const char *method) {
    return mg_strcmp(hm->method, mg_str(method)) == 0;
}

static char *header_dup(struct mg_http_message *hm, const char *name) {
    struct mg_str *value = mg_http_get_header(hm, name);
    if (!value) return NULL;
    char *out = (char *)malloc(value->len + 1);
    if (!out) return NULL;
    memcpy(out, value->buf, value->len);
    out[value->len] = '\0';
    return out;
}

// --- request context -------------------------------------------------------
//
// Behind a reverse proxy every request arrives from the proxy's address over
// plain HTTP, so taken literally the peer address is useless (one "client" for
// the whole internet) and the connection looks insecure even when the user is
// on HTTPS. The settings key `trustedProxies` names the hops we believe:
// "loopback", "private", "any", or a comma-separated list of addresses and CIDR
// blocks. Empty — the default — means trust nothing and use the peer address,
// which is the right answer for a directly exposed server.

static const char *trusted_proxies(restream_server_t *s) {
    const rs_json *settings = rs_json_obj_get(s->state.root, "settings");
    return rs_json_as_str(rs_json_obj_get(settings, "trustedProxies"), "");
}

// The peer address of the connection itself, ignoring any forwarding header.
static void peer_ip(struct mg_connection *c, char *out, size_t out_cap) {
    mg_snprintf(out, out_cap, "%M", mg_print_ip, &c->rem);
    // mg_print_ip renders a v4-mapped peer as ::ffff:1.2.3.4 on a dual-stack
    // listener; normalise so a "192.168.0.0/16" rule matches it as written.
    if (strncmp(out, "::ffff:", 7) == 0 && strchr(out, '.') != NULL) {
        memmove(out, out + 7, strlen(out + 7) + 1);
    }
}

// The address to attribute this request to: the peer, or what a trusted proxy
// says is behind it. Caller frees.
static char *client_ip(restream_server_t *s, struct mg_connection *c,
                       struct mg_http_message *hm) {
    char peer[64];
    peer_ip(c, peer, sizeof(peer));
    char *xff = header_dup(hm, "X-Forwarded-For");
    char *resolved = rs_client_ip(peer, xff, trusted_proxies(s));
    free(xff);
    return resolved ? resolved : rs_strdup(peer);
}

// True if the user reached us over HTTPS — directly, or through a trusted proxy
// that terminated TLS and said so.
static bool request_is_secure(restream_server_t *s, struct mg_connection *c,
                              struct mg_http_message *hm) {
    char peer[64];
    peer_ip(c, peer, sizeof(peer));
    char *proto = header_dup(hm, "X-Forwarded-Proto");
    bool secure = rs_request_is_secure(peer, proto, trusted_proxies(s), c->is_tls);
    free(proto);
    return secure;
}

// The security headers every panel response carries. HSTS is only meaningful —
// and only safe — on a connection the user actually reached over HTTPS; sending
// it from a plain-HTTP LAN install would pin browsers to a scheme that install
// does not serve.
static void security_headers(restream_server_t *s, struct mg_connection *c,
                             struct mg_http_message *hm, char *out, size_t out_cap) {
    const char *base = "X-Content-Type-Options: nosniff\r\n"
                       "Referrer-Policy: same-origin\r\n";
    if (request_is_secure(s, c, hm)) {
        snprintf(out, out_cap, "%sStrict-Transport-Security: max-age=31536000\r\n", base);
    } else {
        snprintf(out, out_cap, "%s", base);
    }
}

// --- auth helpers ----------------------------------------------------------

// The username of the caller's live session, or NULL. Caller frees.
static char *current_user(restream_server_t *s, struct mg_http_message *hm) {
    char *cookie = header_dup(hm, "Cookie");
    if (!cookie) return NULL;
    char *token = rs_auth_cookie_token(cookie);
    free(cookie);
    if (!token) return NULL;
    char *user = rs_auth_username_for_token(s->auth, token);
    free(token);
    return user;
}

static rs_json *admin_by_username(rs_state *st, const char *username) {
    const rs_json *users = rs_json_obj_get(st->root, "adminUsers");
    for (size_t i = 0; i < rs_json_arr_len(users); i++) {
        const rs_json *u = rs_json_arr_at(users, i);
        if (strcmp(rs_json_as_str(rs_json_obj_get(u, "username"), ""), username) == 0) {
            return (rs_json *)u;
        }
    }
    return NULL;
}

// Pulls a string field out of a JSON request body.
static char *body_str(rs_json *body, const char *key) {
    return rs_strdup(rs_json_as_str(rs_json_obj_get(body, key), ""));
}

// --- route handlers --------------------------------------------------------

static void handle_auth_status(restream_server_t *s, struct mg_connection *c,
                               struct mg_http_message *hm) {
    const rs_json *users = rs_json_obj_get(s->state.root, "adminUsers");
    char *user = current_user(s, hm);
    rs_json *out = rs_json_new_obj();
    rs_json_obj_set(out, "needsSetup", rs_json_new_bool(rs_json_arr_len(users) == 0));
    rs_json_obj_set(out, "authenticated", rs_json_new_bool(user != NULL));
    rs_json_obj_set(out, "username", user ? rs_json_new_str(user) : rs_json_new_null());
    const rs_json *account = user ? admin_by_username(&s->state, user) : NULL;
    rs_json_obj_set_str(out, "role", account ? rs_panel_user_role(account) : "admin");
    free(user);
    reply_json(c, 200, out, NULL);
}

// A unix-seconds timestamp expressed the way Swift's JSONEncoder writes a Date:
// seconds since the 2001 reference epoch. Keeps createdAt byte-compatible.
static double apple_epoch_now(void) {
    return (double)time(NULL) - 978307200.0;
}

// Mirrors the live session store into state.json. Called after anything that
// creates or drops a session, so a restart resumes them instead of signing
// everyone out. Only token *hashes* are written — see rs_auth.h.
static void persist_sessions(restream_server_t *s) {
    rs_json_obj_set(s->state.root, "sessions", rs_auth_export_sessions(s->auth));
    rs_state_save(&s->state);
}

// Builds the Set-Cookie (plus security) header block for a successful sign-in.
// Returns false if the session could not be created.
static bool session_headers(restream_server_t *s, struct mg_connection *c,
                            struct mg_http_message *hm, const char *username,
                            bool remember, char *out, size_t out_cap) {
    char *token = rs_auth_create_session(s->auth, username, remember);
    if (!token) return false;
    char *cookie = rs_auth_set_cookie(token, remember, request_is_secure(s, c, hm));
    rs_free(token);
    if (!cookie) return false;
    persist_sessions(s);
    snprintf(out, out_cap, "Set-Cookie: %s\r\n", cookie);
    rs_free(cookie);
    return true;
}

static void handle_auth_setup(restream_server_t *s, struct mg_connection *c,
                              struct mg_http_message *hm) {
    const rs_json *users = rs_json_obj_get(s->state.root, "adminUsers");
    if (rs_json_arr_len(users) > 0) {
        reply_error(c, 400, "An admin account already exists.");
        return;
    }
    rs_json *body = rs_json_parse(hm->body.buf, hm->body.len);
    char *username = body ? body_str(body, "username") : NULL;
    char *password = body ? body_str(body, "password") : NULL;
    bool remember = body && strcmp(rs_json_as_str(rs_json_obj_get(body, "remember"), ""), "true") == 0;
    rs_json_free(body);

    // Trim the username the same way the Swift handler does.
    if (username) {
        char *start = username;
        while (*start == ' ' || *start == '\t') start++;
        size_t len = strlen(start);
        while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t')) len--;
        memmove(username, start, len);
        username[len] = '\0';
    }

    if (!username || !username[0] || !password || strlen(password) < 8) {
        free(username);
        free(password);
        reply_error(c, 400, "Username and an 8+ character password are required.");
        return;
    }

    char *hash = NULL, *salt = NULL;
    if (rs_auth_hash_password(password, &hash, &salt) != 0) {
        free(username); free(password);
        reply_error(c, 500, "Could not hash the password.");
        return;
    }

    // Append the admin, mirroring AdminUser's fields, and persist.
    char id[64];
    uint8_t idbytes[8];
    rs_random_bytes(idbytes, sizeof(idbytes));
    snprintf(id, sizeof(id), "user_%02x%02x%02x%02x%02x%02x%02x%02x",
             idbytes[0], idbytes[1], idbytes[2], idbytes[3],
             idbytes[4], idbytes[5], idbytes[6], idbytes[7]);
    rs_json *admin = rs_json_new_obj();
    rs_json_obj_set(admin, "id", rs_json_new_str(id));
    rs_json_obj_set(admin, "username", rs_json_new_str(username));
    rs_json_obj_set(admin, "passwordHash", rs_json_new_str(hash));
    rs_json_obj_set(admin, "salt", rs_json_new_str(salt));
    rs_json_obj_set(admin, "createdAt", rs_json_new_num(apple_epoch_now()));
    rs_json_arr_push(rs_state_admin_users(&s->state), admin);
    rs_free(hash); rs_free(salt);

    if (rs_state_save(&s->state) != 0) {
        free(username); free(password);
        reply_error(c, 500, "Could not save state.");
        return;
    }

    char extra[768];
    bool ok = session_headers(s, c, hm, username, remember, extra, sizeof(extra));
    free(username); free(password);
    if (!ok) { reply_error(c, 500, "Could not start a session."); return; }
    reply_json(c, 200, rs_json_new_obj(), extra);  // {"ok":true} shape isn't required; UI only checks response.ok
}

static void handle_auth_login(restream_server_t *s, struct mg_connection *c,
                              struct mg_http_message *hm) {
    rs_json *body = rs_json_parse(hm->body.buf, hm->body.len);
    char *username = body ? body_str(body, "username") : NULL;
    char *password = body ? body_str(body, "password") : NULL;
    bool remember = body && strcmp(rs_json_as_str(rs_json_obj_get(body, "remember"), ""), "true") == 0;
    rs_json_free(body);

    char *ip = client_ip(s, c, hm);

    // The throttle is keyed on username *and* address, so one attacker cannot
    // lock a real operator out by guessing at their username from elsewhere,
    // and one host cannot spread its guessing across many usernames for free.
    char identity[320];
    snprintf(identity, sizeof(identity), "%s|%s", username ? username : "", ip ? ip : "");
    int wait = rs_auth_throttle_delay(s->auth, identity);
    if (wait > 0) {
        char msg[320];
        snprintf(msg, sizeof(msg), "throttled sign-in for '%s' from %s (%ds remaining)",
                 username ? username : "", ip ? ip : "", wait);
        log_record(s, "__panel__", "error", "loginThrottled", NULL, 0, -1, msg);
        char headers[128];
        snprintf(headers, sizeof(headers), "Retry-After: %d\r\n", wait);
        free(username); free(password); rs_free(ip);
        char body[160];
        snprintf(body, sizeof(body), "Too many failed sign-ins. Try again in %d seconds.", wait);
        reply_error_headers(c, 429, body, headers);
        return;
    }

    rs_json *admin = (username && username[0]) ? admin_by_username(&s->state, username) : NULL;
    bool ok = admin && password &&
              rs_auth_verify_password(password,
                                      rs_json_as_str(rs_json_obj_get(admin, "passwordHash"), ""),
                                      rs_json_as_str(rs_json_obj_get(admin, "salt"), ""));
    if (!ok) {
        int delay = rs_auth_throttle_record_failure(s->auth, identity);
        char msg[320];
        snprintf(msg, sizeof(msg), "failed sign-in for '%s' from %s%s",
                 username ? username : "", ip ? ip : "",
                 delay > 0 ? " (throttled)" : "");
        log_record(s, "__panel__", "error", "loginFailed", NULL, 0, -1, msg);
        free(username); free(password); rs_free(ip);
        reply_error(c, 401, "Invalid username or password.");
        return;
    }
    rs_auth_throttle_reset(s->auth, identity);
    char loginmsg[320];
    snprintf(loginmsg, sizeof(loginmsg), "%s signed in from %s", username, ip ? ip : "");
    log_record(s, "__panel__", "info", "login", NULL, 0, -1, loginmsg);
    rs_free(ip);

    char extra[768];
    bool started = session_headers(s, c, hm, username, remember, extra, sizeof(extra));
    free(username); free(password);
    if (!started) { reply_error(c, 500, "Could not start a session."); return; }
    reply_json(c, 200, rs_json_new_obj(), extra);
}

static void handle_auth_logout(restream_server_t *s, struct mg_connection *c,
                               struct mg_http_message *hm) {
    char *ip = client_ip(s, c, hm);
    log_record(s, "__panel__", "info", "logout", NULL, 0, -1, ip ? ip : "");
    rs_free(ip);
    char *cookie = header_dup(hm, "Cookie");
    if (cookie) {
        char *token = rs_auth_cookie_token(cookie);
        if (token) { rs_auth_end_session(s->auth, token); free(token); }
        free(cookie);
    }
    persist_sessions(s);
    char *clear = rs_auth_clear_cookie(request_is_secure(s, c, hm));
    char extra[512];
    snprintf(extra, sizeof(extra), "Set-Cookie: %s\r\n", clear ? clear : "");
    rs_free(clear);
    reply_json(c, 200, rs_json_new_obj(), extra);
}

// The Host header, for building absolute URLs in the state view. Caller frees.
static char *request_host(struct mg_http_message *hm) {
    char *host = header_dup(hm, "Host");
    return host ? host : rs_strdup("127.0.0.1");
}

// Overlays live network-monitor figures (active clients, in/out byte rate,
// all-time bytes) onto each stream in a freshly-built /api/state view. Kept out
// of rs_panel_view (core, no metrics) — the server owns the monitor.
static void inject_stream_metrics(restream_server_t *s, rs_json *view) {
    rs_json *providers = (rs_json *)rs_json_obj_get(view, "providers");
    for (size_t i = 0; i < rs_json_arr_len(providers); i++) {
        rs_json *streams = (rs_json *)rs_json_obj_get(rs_json_arr_at(providers, i), "streams");
        for (size_t j = 0; j < rs_json_arr_len(streams); j++) {
            rs_json *st = (rs_json *)rs_json_arr_at(streams, j);
            const char *id = rs_json_obj_str(st, "id", "");
            if (!id[0]) continue;
            long long bps = (long long)rs_metrics_bytes_per_sec(s->metrics, id);
            long long total = rs_metrics_total_bytes(s->metrics, id);
            rs_json_obj_set_int(st, "activeClients", rs_metrics_active_clients(s->metrics, id));
            rs_json *out = rs_json_new_obj();
            rs_json_obj_set_int(out, "bytesPerSecond", bps);
            rs_json_obj_set_int(out, "allTimeBytes", total);
            rs_json_obj_set(st, "bandwidth", out);
            rs_json *in = rs_json_new_obj();  // proxy: input rate ≈ output rate
            rs_json_obj_set_int(in, "bytesPerSecond", bps);
            rs_json_obj_set_int(in, "allTimeBytes", total);
            rs_json_obj_set(st, "inputBandwidth", in);
        }
    }
}

static void handle_state(restream_server_t *s, struct mg_connection *c, struct mg_http_message *hm) {
    char *host = request_host(hm);
    rs_json *view = rs_panel_view(&s->state, host);
    inject_stream_metrics(s, view);
    reply_json(c, 200, view, NULL);
    free(host);
}

// A matched capture as a fresh NUL-terminated string (mongoose hands them back
// as pointer+length into the request buffer, which is not NUL-terminated).
static char *capture_dup(struct mg_str s) {
    char *out = (char *)malloc(s.len + 1);
    if (!out) return NULL;
    memcpy(out, s.buf, s.len);
    out[s.len] = '\0';
    return out;
}

// Captures the first `*`/`#` segment of a matched URI pattern as a fresh string.
static char *capture(struct mg_http_message *hm, const char *pattern) {
    struct mg_str caps[4];
    if (!mg_match(hm->uri, mg_str(pattern), caps)) return NULL;
    char *out = (char *)malloc(caps[0].len + 1);
    if (!out) return NULL;
    memcpy(out, caps[0].buf, caps[0].len);
    out[caps[0].len] = '\0';
    return out;
}

// Runs a panel mutation, saves, and replies with the fresh state view — the
// shape every provider/stream mutation returns. `rc` is a rs_panel_* return
// value (0 or a negative HTTP status), `err` its message.
static void reply_after_mutation(restream_server_t *s, struct mg_connection *c,
                                 struct mg_http_message *hm, int rc, const char *err) {
    if (rc != 0) { reply_error(c, -rc, err ? err : "Request failed."); return; }
    if (rs_state_save(&s->state) != 0) { reply_error(c, 500, "Could not save state."); return; }
    handle_state(s, c, hm);
}

// Parses the request body as a JSON object. Returns an empty object for an empty
// body so callers can read optional fields uniformly.
static rs_json *parse_body(struct mg_http_message *hm) {
    if (hm->body.len == 0) return rs_json_new_obj();
    rs_json *body = rs_json_parse(hm->body.buf, hm->body.len);
    return body ? body : rs_json_new_obj();
}

static rs_json *settings_view(restream_server_t *s) {
    const rs_json *settings = rs_json_obj_get(s->state.root, "settings");
    rs_json *out = rs_json_new_obj();
    // The port actually bound, not the stored preference. Reporting the stored
    // value meant the Settings page showed 8787 (the default nobody had ever
    // changed) while the operator was connected on the real port — the field
    // claimed to describe the running server and described nothing at all.
    // `storedPort` keeps the preference visible for the "takes effect after
    // restart" case, where the two legitimately differ.
    long long stored = (long long)rs_json_as_num(rs_json_obj_get(settings, "port"), 0);
    rs_json_obj_set(out, "port", rs_json_new_int((long long)s->listen_port));
    rs_json_obj_set(out, "storedPort", rs_json_new_int(stored > 0 ? stored : (long long)s->listen_port));
    rs_json_obj_set(out, "bindAddress",
                    rs_json_new_str(rs_json_as_str(rs_json_obj_get(settings, "bindAddress"), "")));
    rs_json_obj_set(out, "trustedProxies",
                    rs_json_new_str(rs_json_as_str(rs_json_obj_get(settings, "trustedProxies"), "")));
    return out;
}

static void handle_settings(restream_server_t *s, struct mg_connection *c) {
    reply_json(c, 200, settings_view(s), NULL);
}

static void handle_settings_update(restream_server_t *s, struct mg_connection *c,
                                   struct mg_http_message *hm) {
    rs_json *body = parse_body(hm);
    rs_json *settings = rs_state_settings(&s->state);

    double port = rs_json_as_num(rs_json_obj_get(body, "port"), 0);
    if (port > 0 && port <= 65535) rs_json_obj_set_int(settings, "port", (long long)port);

    const rs_json *bind = rs_json_obj_get(body, "bindAddress");
    if (bind && rs_json_type_of(bind) == RS_JSON_STR) {
        const char *trimmed = NULL;
        size_t len = rs_trim(rs_json_as_str(bind, ""), strlen(rs_json_as_str(bind, "")), true, &trimmed);
        char *copy = (char *)malloc(len + 1);
        if (copy) {
            memcpy(copy, trimmed, len);
            copy[len] = '\0';
            rs_json_obj_set_str(settings, "bindAddress", copy);
            free(copy);
        }
    }

    // The trusted-proxy list is validated on the way in: a typo here does not
    // fail loudly at request time, it just silently stops matching, and the
    // operator is left believing their proxy is trusted when it is not.
    const rs_json *trusted = rs_json_obj_get(body, "trustedProxies");
    if (trusted && rs_json_type_of(trusted) == RS_JSON_STR) {
        const char *value = rs_json_as_str(trusted, "");
        char bad[80];
        if (!rs_ip_list_valid(value, bad, sizeof(bad))) {
            rs_json_free(body);
            char msg[192];
            snprintf(msg, sizeof(msg),
                     "'%s' is not an address, CIDR block, 'loopback', 'private' or 'any'.", bad);
            reply_error(c, 400, msg);
            return;
        }
        rs_json_obj_set_str(settings, "trustedProxies", value);
    }

    rs_json_free(body);
    if (rs_state_save(&s->state) != 0) { reply_error(c, 500, "Could not save state."); return; }
    rs_json *out = settings_view(s);
    rs_json_obj_set_str(out, "note", "Takes effect after restart.");
    reply_json(c, 200, out, NULL);
}

// Builds the metrics payload the SSE stream and the Server view consume. The
// system stats are real; bandwidth, per-stream metrics and connections are
// empty until the playback plane (slice 4) produces traffic.
static rs_json *build_metrics(restream_server_t *s) {
    rs_json *out = rs_json_new_obj();
    rs_json *streams = rs_json_new_obj();
    double global_bps = 0; long long global_total = 0; int global_clients = 0;

    // One entry per stream, plus running totals for the global monitor. The C
    // server is a proxy, so bytes fetched ≈ bytes served — report the served
    // rate as both the input (↓) and output (↑) figure.
    const rs_json *providers = rs_json_obj_get(s->state.root, "providers");
    for (size_t i = 0; i < rs_json_arr_len(providers); i++) {
        const rs_json *streams_arr = rs_json_obj_get(rs_json_arr_at(providers, i), "streams");
        for (size_t j = 0; j < rs_json_arr_len(streams_arr); j++) {
            const char *id = rs_json_obj_str(rs_json_arr_at(streams_arr, j), "id", "");
            if (!id[0]) continue;
            double bps = rs_metrics_bytes_per_sec(s->metrics, id);
            long long total = rs_metrics_total_bytes(s->metrics, id);
            int clients = rs_metrics_active_clients(s->metrics, id);
            rs_json *sm = rs_json_new_obj();
            rs_json_obj_set_int(sm, "bytesPerSecond", (long long)bps);
            rs_json_obj_set_int(sm, "allTimeBytes", total);
            rs_json_obj_set_int(sm, "activeClients", clients);
            rs_json_obj_set(streams, id, sm);
            global_bps += bps; global_total += total; global_clients += clients;
        }
    }

    rs_json *global = rs_json_new_obj();
    rs_json_obj_set_int(global, "bytesPerSecond", (long long)global_bps);
    rs_json_obj_set_int(global, "allTimeBytes", global_total);
    rs_json_obj_set_int(global, "activeClients", global_clients);
    rs_json_obj_set(out, "global", global);
    rs_json *ginput = rs_json_new_obj();
    rs_json_obj_set_int(ginput, "bytesPerSecond", (long long)global_bps);
    rs_json_obj_set_int(ginput, "allTimeBytes", global_total);
    rs_json_obj_set(out, "globalInput", ginput);
    rs_json_obj_set(out, "system", rs_sysstats_snapshot(s->sysstats));
    rs_json_obj_set(out, "streams", streams);
    rs_json_obj_set(out, "connections", rs_json_new_arr());
    return out;
}

static void handle_events(restream_server_t *s, struct mg_connection *c) {
    // SSE: send the headers, mark the connection, and push one frame right away
    // so the panel populates without waiting for the first tick.
    mg_printf(c, "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/event-stream\r\n"
                 "Cache-Control: no-cache\r\n"
                 "Connection: keep-alive\r\n\r\n");
    c->data[0] = RS_SSE_MARKER;
    rs_json *metrics = build_metrics(s);
    char *body = rs_json_serialize(metrics, false);
    rs_json_free(metrics);
    if (body) {
        mg_printf(c, "data: %s\n\n", body);
        rs_free(body);
    }
}

// The 1s timer: push a fresh metrics frame to every SSE subscriber.
static void broadcast_metrics(void *arg) {
    restream_server_t *s = (restream_server_t *)arg;
    // A restart asked for from Settings. Deferred to here so the HTTP reply has
    // been flushed — systemd stops this process the moment it accepts.
    if (s->restart_pending) {
        s->restart_pending = false;
        char err[256] = {0};
        if (!g_service_action || g_service_action("restart", 0, NULL, err, sizeof(err)) != 0)
            log_record(s, "__panel__", "error", "serviceRestart", NULL, 0, -1,
                       err[0] ? err : "the restart could not be handed to systemd");
    }
    rs_metrics_prune(s->metrics);  // expire stale clients/rate windows every tick
    // Join and free any engine that finished winding down since the last tick.
    // Doing it here rather than in the stop handler is what makes Stop return
    // instantly instead of waiting for an in-flight segment download.
    rs_live_reap(s->live);
    // Snapshot once, reuse for every subscriber.
    bool any = false;
    for (struct mg_connection *c = s->mgr.conns; c != NULL; c = c->next) {
        if (c->data[0] == RS_SSE_MARKER) { any = true; break; }
    }
    if (!any) return;  // don't sample the host with nobody watching
    rs_json *metrics = build_metrics(s);
    char *body = rs_json_serialize(metrics, false);
    rs_json_free(metrics);
    if (!body) return;
    for (struct mg_connection *c = s->mgr.conns; c != NULL; c = c->next) {
        if (c->data[0] == RS_SSE_MARKER) mg_printf(c, "data: %s\n\n", body);
    }
    rs_free(body);
}

// The Logs view. Everything the server does lands in the ring buffer — panel
// access, auth, stream start/stop, and every poll, download, decrypt,
// discontinuity and prune the live engine performs.
static void handle_logs(restream_server_t *s, struct mg_connection *c, struct mg_http_message *hm) {
    char sid[160] = {0}, lim[32] = {0};
    mg_http_get_var(&hm->query, "streamId", sid, sizeof(sid));
    mg_http_get_var(&hm->query, "limit", lim, sizeof(lim));
    reply_json(c, 200, log_view(s, sid, lim[0] ? atoi(lim) : 150), NULL);
}

// Dispatches /api/*. Returns true if it handled the request.
// Finds a provider node by id in the live state (mutable).
static rs_json *find_provider_by_id(rs_state *st, const char *id) {
    rs_json *providers = (rs_json *)rs_json_obj_get(st->root, "providers");
    for (size_t i = 0; i < rs_json_arr_len(providers); i++) {
        rs_json *p = (rs_json *)rs_json_arr_at(providers, i);
        if (strcmp(rs_json_obj_str(p, "id", ""), id) == 0) return p;
    }
    return NULL;
}

// Runs a provider's script for one action (login/pair/channels/keys/custom…)
// on a worker thread, passing the provider config + active account. Always
// replies to `c` itself — either immediately with an error, or (once
// dispatched) asynchronously once the run finishes: see the "async jobs"
// section above and pending_job_finish_script, which records stdout/stderr
// under "script:<providerId>" so the panel's script output + Logs tab show it.
// This is the C port of PanelServer's script-action runner (ScriptRunner).
static void run_provider_script(restream_server_t *s, struct mg_connection *c,
                                const rs_json *provider, const char *action) {
    const char *script = rs_json_obj_str(provider, "scriptPath", "");
    if (!script[0]) { reply_error(c, 400, "No script path set on this provider."); return; }
    const char *pid = rs_json_obj_str(provider, "id", "");
    char sid[192];
    snprintf(sid, sizeof(sid), "script:%s", pid);

    char sessiondir[512];
    snprintf(sessiondir, sizeof(sessiondir), "runtime/sessions/%s", pid);
#ifndef _WIN32
    mkdir("runtime", 0755); mkdir("runtime/sessions", 0755); mkdir(sessiondir, 0755);
#endif

    // Heap-allocated (rather than the stack array this used to be) because
    // dispatch_script_action hands it to a worker thread that outlives this
    // call — the stack array would be gone by the time the worker ran.
    char **args = (char **)malloc(24 * sizeof(char *));
    if (!args) { reply_error(c, 500, "Out of memory."); return; }
    int n = 0;
    args[n++] = rs_script_arg("action", action, false);
    args[n++] = rs_script_arg("sessiondir", sessiondir, false);
    const char *bind = rs_json_obj_str(provider, "scriptBind", "");
    const char *doh = rs_json_obj_str(provider, "scriptDoh", "");
    const char *worker = rs_json_obj_str(provider, "scriptWorker", "");
    if (bind[0]) args[n++] = rs_script_arg("bind", bind, false);
    if (doh[0]) args[n++] = rs_script_arg("doh", doh, false);
    if (worker[0]) args[n++] = rs_script_arg("worker", worker, false);
    const rs_json *accounts = rs_json_obj_get(provider, "scriptAccounts");
    const char *active = rs_json_obj_str(provider, "activeScriptAccountId", "");
    for (size_t i = 0; i < rs_json_arr_len(accounts) && n < 22; i++) {
        const rs_json *a = rs_json_arr_at(accounts, i);
        if (strcmp(rs_json_obj_str(a, "id", ""), active) != 0) continue;
        const char *u = rs_json_obj_str(a, "username", "");
        const char *p = rs_json_obj_str(a, "password", "");
        if (u[0]) args[n++] = rs_script_arg("username", u, false);
        if (p[0]) args[n++] = rs_script_arg("password", p, true);
        break;
    }

    log_record(s, sid, "info", "scriptStart", NULL, 0, -1, action);
    dispatch_script_action(s, c, sid, script, args, n);  // always replies
}

// --- M3U export, provider export/import, EPG --------------------------------

// Quotes cannot appear inside an M3U attribute value without breaking the parse
// for every player. Provider and stream names are free text nobody expected to
// need escaping, so strip rather than try to escape — same rule as
// PanelServer.m3uAttrSafe.
static void m3u_attr_append(rs_buf *out, const char *text) {
    for (const char *p = text ? text : ""; *p; p++) {
        if (*p == '"') continue;
        rs_buf_append_char(out, (*p == '\n' || *p == '\r') ? ' ' : *p);
    }
}

// `only_provider` NULL exports every provider; otherwise just that one.
static void serve_m3u_playlist(restream_server_t *s, struct mg_connection *c,
                               struct mg_http_message *hm, const char *only_provider) {
    char *host = request_host(hm);
    const rs_json *providers = rs_json_obj_get(s->state.root, "providers");
    bool found = only_provider == NULL;
    const char *filename = "restreamair-all";

    rs_buf body = RS_BUF_INIT;
    rs_buf_append_str(&body, "#EXTM3U\n");
    for (size_t i = 0; i < rs_json_arr_len(providers); i++) {
        const rs_json *provider = rs_json_arr_at(providers, i);
        if (only_provider && strcmp(rs_json_obj_str(provider, "id", ""), only_provider) != 0) continue;
        found = true;
        const char *provider_name = rs_json_obj_str(provider, "name", "");
        const char *provider_logo = rs_json_obj_str(provider, "logo", "");
        if (only_provider) filename = provider_name;

        const rs_json *streams = rs_json_obj_get(provider, "streams");
        for (size_t j = 0; j < rs_json_arr_len(streams); j++) {
            const rs_json *stream = rs_json_arr_at(streams, j);
            const char *id = rs_json_obj_str(stream, "id", "");
            const char *name = rs_json_obj_str(stream, "name", "");
            const char *logo = rs_json_obj_str(stream, "logo", "");
            if (!logo[0]) logo = provider_logo;
            // tvg-id is what binds a guide entry to this channel; without it the
            // EPG a script provider fetches has nothing to attach to.
            const char *tvg_id = rs_json_obj_str(stream, "tvgId", "");
            if (!tvg_id[0]) tvg_id = id;

            rs_buf_append_str(&body, "#EXTINF:-1 tvg-id=\"");
            m3u_attr_append(&body, tvg_id);
            rs_buf_append_str(&body, "\" tvg-name=\"");
            m3u_attr_append(&body, name);
            rs_buf_append_str(&body, "\"");
            if (logo[0]) {
                rs_buf_append_str(&body, " tvg-logo=\"");
                m3u_attr_append(&body, logo);
                rs_buf_append_str(&body, "\"");
            }
            rs_buf_append_str(&body, " group-title=\"");
            m3u_attr_append(&body, provider_name);
            rs_buf_append_str(&body, "\",");
            m3u_attr_append(&body, name);
            rs_buf_append_char(&body, '\n');
            rs_buf_append_str(&body, "http://");
            rs_buf_append_str(&body, host);
            rs_buf_append_str(&body, "/play/");
            rs_buf_append_str(&body, id);
            rs_buf_append_str(&body, "/index.m3u8\n");
        }
    }
    free(host);

    char *text = rs_buf_take(&body);
    if (!found) { rs_free(text); reply_error(c, 404, "Provider not found."); return; }
    if (!text) { reply_error(c, 500, "Out of memory."); return; }

    char *safe = rs_panel_slugify(filename);
    char headers[256];
    snprintf(headers, sizeof(headers),
             "Content-Type: audio/x-mpegurl\r\nCache-Control: no-store\r\n"
             "Content-Disposition: attachment; filename=\"%s.m3u8\"\r\n",
             safe && safe[0] ? safe : "playlist");
    mg_http_reply(c, 200, headers, "%s", text);
    rs_free(safe);
    rs_free(text);
}

// Reads a whole file into memory. Returns NULL if it cannot be read.
static uint8_t *read_file(const char *path, size_t *out_len) {
    *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) { fclose(f); return NULL; }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[read] = 0;
    *out_len = read;
    return buf;
}

static void serve_provider_export(restream_server_t *s, struct mg_connection *c,
                                  const char *provider_id) {
    rs_json *doc = rs_panel_export_provider(&s->state, provider_id);
    if (!doc) { reply_error(c, 404, "Provider not found."); return; }

    // Embed the script itself, so an import on another install is self-contained.
    const rs_json *providers = rs_json_obj_get(s->state.root, "providers");
    const char *script_path = "";
    for (size_t i = 0; i < rs_json_arr_len(providers); i++) {
        const rs_json *p = rs_json_arr_at(providers, i);
        if (strcmp(rs_json_obj_str(p, "id", ""), provider_id) == 0) {
            script_path = rs_json_obj_str(p, "scriptPath", "");
            break;
        }
    }
    if (script_path[0]) {
        size_t len = 0;
        uint8_t *contents = read_file(script_path, &len);
        if (contents) {
            char *encoded = rs_base64_encode(contents, len);
            free(contents);
            if (encoded) {
                const char *slash = strrchr(script_path, '/');
#ifdef _WIN32
                const char *backslash = strrchr(script_path, '\\');
                if (backslash && (!slash || backslash > slash)) slash = backslash;
#endif
                rs_json *file = rs_json_new_obj();
                rs_json_obj_set_str(file, "filename", slash ? slash + 1 : script_path);
                rs_json_obj_set_str(file, "contentBase64", encoded);
                rs_json_obj_set(doc, "scriptFile", file);
                rs_free(encoded);
            }
        }
    }

    // The export carries script-account passwords in the clear (the script has
    // to be handed the real thing), so it is as sensitive as state.json — the
    // API gate already keeps it admin-only.
    reply_json(c, 200, doc, "Content-Disposition: attachment; filename=\"provider.restreamair-provider.json\"\r\n");
}

static void handle_provider_import(restream_server_t *s, struct mg_connection *c,
                                   struct mg_http_message *hm) {
    rs_json *doc = parse_body(hm);
    char *written_path = NULL;

    // Write the embedded script under scripts/, never overwriting one already
    // there — an unrelated script of the same name must survive.
    const rs_json *file = rs_json_obj_get(doc, "scriptFile");
    const char *filename = rs_json_obj_str(file, "filename", "");
    const char *encoded = rs_json_obj_str(file, "contentBase64", "");
    if (filename[0] && encoded[0] && !strchr(filename, '/') && !strchr(filename, '\\')) {
        size_t cap = strlen(encoded);
        uint8_t *decoded = (uint8_t *)malloc(cap + 1);
        size_t decoded_len = 0;
        if (decoded && rs_base64_decode(encoded, decoded, cap, &decoded_len) == 0) {
#ifndef _WIN32
            mkdir("scripts", 0755);
#endif
            char candidate[512];
            snprintf(candidate, sizeof(candidate), "scripts/%s", filename);
            for (int attempt = 1; attempt < 100; attempt++) {
                FILE *existing = fopen(candidate, "rb");
                if (!existing) break;
                fclose(existing);
                snprintf(candidate, sizeof(candidate), "scripts/%d-%s", attempt, filename);
            }
            FILE *out = fopen(candidate, "wb");
            if (out) {
                fwrite(decoded, 1, decoded_len, out);
                fclose(out);
#ifndef _WIN32
                chmod(candidate, 0755);  // it has to be executable to be run
#endif
                written_path = rs_strdup(candidate);
            }
        }
        free(decoded);
    }

    const char *err = NULL;
    char *new_id = NULL;
    int rc = rs_panel_import_provider(&s->state, doc, written_path ? written_path : "", &new_id, &err);
    if (rc == 0) {
        log_recordf(s, "__panel__", "info", "providerImport", NULL, 0, -1,
                    "imported \"%s\" as %s",
                    rs_json_obj_str(rs_json_obj_get(doc, "provider"), "name", ""),
                    new_id ? new_id : "?");
    }
    rs_free(new_id);
    rs_free(written_path);
    rs_json_free(doc);
    reply_after_mutation(s, c, hm, rc, err);
}

static void serve_provider_epg(struct mg_connection *c, const char *provider_id) {
    // Stored by the script's `epg` action under the provider's session dir.
    char path[512];
    snprintf(path, sizeof(path), "runtime/sessions/%s/epg.xml", provider_id);
    size_t len = 0;
    uint8_t *data = read_file(path, &len);
    if (!data) {
        reply_error(c, 404, "No EPG stored for this provider yet — run the epg action first.");
        return;
    }
    // The script may return XMLTV or JSON; sniff it the same way the Swift
    // handler does rather than forcing one content type on both.
    const char *type = (len > 0 && data[0] == '<') ? "application/xml; charset=utf-8"
                                                   : "application/json; charset=utf-8";
    mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
                 "Cache-Control: no-store\r\n\r\n", type, (unsigned long)len);
    mg_send(c, data, len);
    free(data);
}

static bool handle_api(restream_server_t *s, struct mg_connection *c, struct mg_http_message *hm) {
    if (mg_match(hm->uri, mg_str("/api/auth/status"), NULL) && method_is(hm, "GET")) {
        handle_auth_status(s, c, hm); return true;
    }
    if (mg_match(hm->uri, mg_str("/api/auth/setup"), NULL) && method_is(hm, "POST")) {
        handle_auth_setup(s, c, hm); return true;
    }
    if (mg_match(hm->uri, mg_str("/api/auth/login"), NULL) && method_is(hm, "POST")) {
        handle_auth_login(s, c, hm); return true;
    }
    if (mg_match(hm->uri, mg_str("/api/auth/logout"), NULL) && method_is(hm, "POST")) {
        handle_auth_logout(s, c, hm); return true;
    }

    // Everything past here needs a signed-in account, same as the Swift panel.
    char *user = current_user(s, hm);
    if (!user) { reply_error(c, 401, "Not signed in."); return true; }

    // A viewer may read the panel but not change it. Enforcing on the method
    // rather than on a list of routes means a route added later is read-only
    // for viewers by default, which is the safe direction to fail in.
    const rs_json *account = admin_by_username(&s->state, user);
    bool is_viewer = account && strcmp(rs_panel_user_role(account), "viewer") == 0;
    free(user);
    if (is_viewer && !method_is(hm, "GET")) {
        reply_error(c, 403, "This account is read-only.");
        return true;
    }

    if (mg_match(hm->uri, mg_str("/api/state"), NULL) && method_is(hm, "GET")) {
        handle_state(s, c, hm); return true;
    }
    if (mg_match(hm->uri, mg_str("/api/settings"), NULL) && method_is(hm, "GET")) {
        handle_settings(s, c); return true;
    }
    if (mg_match(hm->uri, mg_str("/api/settings"), NULL) && method_is(hm, "POST")) {
        handle_settings_update(s, c, hm); return true;
    }
    if (mg_match(hm->uri, mg_str("/api/events"), NULL) && method_is(hm, "GET")) {
        handle_events(s, c); return true;
    }
    if (mg_match(hm->uri, mg_str("/api/logs"), NULL) && method_is(hm, "GET")) {
        handle_logs(s, c, hm); return true;
    }
    if (mg_match(hm->uri, mg_str("/api/logs"), NULL) && method_is(hm, "DELETE")) {
        char sid[160] = {0};
        mg_http_get_var(&hm->query, "streamId", sid, sizeof(sid));
        log_clear(s, sid);
        reply_json(c, 200, log_view(s, sid, 150), NULL);
        return true;
    }
    // --- service management ---
    // GET reports what systemd knows; POST restart/install act on it. Both are
    // behind the same admin auth as the rest of /api, and the action names are a
    // fixed pair rather than anything the caller composes.
    if (mg_match(hm->uri, mg_str("/api/service"), NULL) && method_is(hm, "GET")) {
        char *json = g_service_status ? g_service_status() : NULL;
        if (!json) {
            mg_http_reply(c, 200, JSON_HEADERS, "{\"systemdAvailable\":false}");
        } else {
            mg_http_reply(c, 200, JSON_HEADERS, "%s", json);
            rs_free(json);
        }
        return true;
    }
    if (mg_match(hm->uri, mg_str("/api/service/restart"), NULL) && method_is(hm, "POST")) {
        if (!g_service_action) {
            reply_error(c, 501, "Service management isn't available in this build.");
            return true;
        }
        char err[256] = {0};
        // Ask first, act on the timer: a successful restart replaces this
        // process, so the reply has to be on its way out before it happens.
        if (g_service_action("restart-check", 0, NULL, err, sizeof(err)) != 0) {
            reply_error(c, 400, err[0] ? err : "cannot restart the service");
            return true;
        }
        s->restart_pending = true;
        log_record(s, "__panel__", "info", "serviceRestart", NULL, 0, -1,
                   "restart requested from Settings — handing over to systemd");
        mg_http_reply(c, 200, JSON_HEADERS, "{\"restarting\":true}");
        return true;
    }
    if (mg_match(hm->uri, mg_str("/api/service/install"), NULL) && method_is(hm, "POST")) {
        if (!g_service_action) {
            reply_error(c, 501, "Service management isn't available in this build.");
            return true;
        }
        rs_json *body = parse_body(hm);
        double port = rs_json_as_num(rs_json_obj_get(body, "port"), 0);
        char *bind = body_str(body, "bindAddress");
        rs_json_free(body);
        if (port <= 0 || port > 65535) port = s->listen_port;
        char err[256] = {0};
        int rc = g_service_action("install", (unsigned)port, bind, err, sizeof(err));
        free(bind);
        if (rc != 0) {
            reply_error(c, 400, err[0] ? err : "could not install the service");
            return true;
        }
        log_recordf(s, "__panel__", "info", "serviceInstall", NULL, 0, -1,
                    "systemd unit installed and enabled (port %u)", (unsigned)port);
        mg_http_reply(c, 200, JSON_HEADERS, "{\"installed\":true}");
        return true;
    }

    if (mg_match(hm->uri, mg_str("/api/probe"), NULL) && method_is(hm, "POST")) {
        if (!g_probe_handler) {
            reply_error(c, 501, "Source auto-detect isn't in the C server build. "
                                "Add a representation manually, or use the Swift binary.");
            return true;
        }
        rs_json *body = parse_body(hm);
        char *url = body_str(body, "url");
        char *proxy = body_str(body, "proxy");
        char *headers = body_str(body, "headers");
        rs_json_free(body);
        dispatch_probe(s, c, url, proxy, headers);  // takes ownership, always replies
        return true;
    }

    // --- provider script actions ---
    // POST /api/providers/<id>/script/<action> — run a script action; the panel
    // polls /api/logs?streamId=script:<id> for the output.
    if (method_is(hm, "POST")) {
        struct mg_str caps[2];
        if (mg_match(hm->uri, mg_str("/api/providers/*/script/*"), caps)) {
            char pid[128] = {0}, action[64] = {0};
            snprintf(pid, sizeof(pid), "%.*s", (int)caps[0].len, caps[0].buf);
            snprintf(action, sizeof(action), "%.*s", (int)caps[1].len, caps[1].buf);
            rs_json *provider = find_provider_by_id(&s->state, pid);
            if (!provider) { reply_error(c, 404, "Provider not found."); return true; }

            if (strcmp(action, "clear-session") == 0) {
                // Best-effort: the script owns the files; just log it and let the
                // next run start fresh (the script recreates its session dir).
                char sid[192]; snprintf(sid, sizeof(sid), "script:%s", pid);
                log_record(s, sid, "info", "clearSession", NULL, 0, -1, "session cleared");
                reply_json(c, 200, log_view(s, sid, 150), NULL);
                return true;
            }

            run_provider_script(s, c, provider, action);  // always replies, sync or async
            return true;
        }
    }

    // --- provider CRUD ---
    if (mg_match(hm->uri, mg_str("/api/providers"), NULL) && method_is(hm, "POST")) {
        rs_json *body = parse_body(hm);
        const char *err = NULL;
        int rc = rs_panel_create_provider(&s->state, body, &err);
        rs_json_free(body);
        reply_after_mutation(s, c, hm, rc, err);
        return true;
    }
    if (mg_match(hm->uri, mg_str("/api/providers/*"), NULL) && method_is(hm, "PUT")) {
        char *id = capture(hm, "/api/providers/*");
        rs_json *body = parse_body(hm);
        const char *err = NULL;
        int rc = rs_panel_update_provider(&s->state, id, body, &err);
        rs_json_free(body);
        free(id);
        reply_after_mutation(s, c, hm, rc, err);
        return true;
    }
    if (mg_match(hm->uri, mg_str("/api/providers/*"), NULL) && method_is(hm, "DELETE")) {
        char *id = capture(hm, "/api/providers/*");
        const char *err = NULL;
        int rc = rs_panel_delete_provider(&s->state, id, &err);
        free(id);
        reply_after_mutation(s, c, hm, rc, err);
        return true;
    }

    // --- M3U export, provider export/import, EPG ---
    if (mg_match(hm->uri, mg_str("/api/playlist.m3u8"), NULL) && method_is(hm, "GET")) {
        serve_m3u_playlist(s, c, hm, NULL);
        return true;
    }
    if (mg_match(hm->uri, mg_str("/api/providers/*/playlist.m3u8"), NULL) && method_is(hm, "GET")) {
        char *provider_id = capture(hm, "/api/providers/*/playlist.m3u8");
        serve_m3u_playlist(s, c, hm, provider_id);
        free(provider_id);
        return true;
    }
    if (mg_match(hm->uri, mg_str("/api/providers/*/export"), NULL) && method_is(hm, "GET")) {
        char *provider_id = capture(hm, "/api/providers/*/export");
        serve_provider_export(s, c, provider_id);
        free(provider_id);
        return true;
    }
    if (mg_match(hm->uri, mg_str("/api/providers/import"), NULL) && method_is(hm, "POST")) {
        handle_provider_import(s, c, hm);
        return true;
    }
    if (mg_match(hm->uri, mg_str("/api/providers/*/epg"), NULL) && method_is(hm, "GET")) {
        char *provider_id = capture(hm, "/api/providers/*/epg");
        serve_provider_epg(c, provider_id);
        free(provider_id);
        return true;
    }

    // --- stream CRUD ---
    if (mg_match(hm->uri, mg_str("/api/providers/*/streams"), NULL) && method_is(hm, "POST")) {
        char *provider_id = capture(hm, "/api/providers/*/streams");
        rs_json *body = parse_body(hm);
        const char *err = NULL;
        int rc = rs_panel_create_stream(&s->state, provider_id, body, &err);
        if (rc == 0)
            log_recordf(s, "__panel__", "info", "streamCreate", NULL, 0, -1,
                        "\"%s\" (%s) added to provider %s",
                        rs_json_obj_str(body, "name", ""), rs_json_obj_str(body, "kind", "mpd"),
                        provider_id ? provider_id : "?");
        rs_json_free(body);
        free(provider_id);
        reply_after_mutation(s, c, hm, rc, err);
        return true;
    }
    // Start/stop. Both are answered immediately: starting only flips the stored
    // status and kicks off the background engine, and stopping only flags it.
    // Neither waits on the network, which is what made these buttons feel dead
    // before — the request used to queue behind whatever manifest or segment
    // fetch the single-threaded event loop happened to be blocked on.
    if (mg_match(hm->uri, mg_str("/api/streams/*/start"), NULL) && method_is(hm, "POST")) {
        char *id = capture(hm, "/api/streams/*/start");
        char *ip = client_ip(s, c, hm);
        const char *err = NULL;
        int rc = rs_panel_set_stream_running(&s->state, id, true, &err);
        if (rc == 0) {
            const rs_json *stream = rs_panel_find_stream(&s->state, id);
            const char *name = stream ? rs_json_obj_str(stream, "name", "") : "";
            const char *kind = stream ? rs_json_obj_str(stream, "kind", "mpd") : "mpd";
            const char *mode = stream ? rs_json_obj_str(stream, "inputMode", "internal") : "internal";
            const char *src = stream ? stream_source_target(stream) : "";
            log_recordf(s, id, "info", "streamStart", src, 0, -1,
                        "START requested by %s — \"%s\" (%s, %s mode)", ip, name, kind, mode);
            log_recordf(s, "__panel__", "info", "streamStart", src, 0, -1,
                        "%s (\"%s\") started by %s", id, name, ip);
            live_sync_stream(s, id);
        } else {
            log_recordf(s, "__panel__", "error", "streamStart", NULL, 0, -1,
                        "START of %s from %s failed: %s", id ? id : "?", ip,
                        err ? err : "unknown error");
        }
        rs_free(ip);
        free(id);
        reply_after_mutation(s, c, hm, rc, err);
        return true;
    }
    if (mg_match(hm->uri, mg_str("/api/streams/*/stop"), NULL) && method_is(hm, "POST")) {
        char *id = capture(hm, "/api/streams/*/stop");
        char *ip = client_ip(s, c, hm);
        const char *err = NULL;
        // Flag the engine before the status flip so the log reads in the order
        // things actually happened.
        live_stop_stream(s, id);
        int rc = rs_panel_set_stream_running(&s->state, id, false, &err);
        if (rc == 0) {
            const rs_json *stream = rs_panel_find_stream(&s->state, id);
            const char *name = stream ? rs_json_obj_str(stream, "name", "") : "";
            log_recordf(s, id, "info", "streamStop", NULL, 0, -1,
                        "STOP requested by %s — \"%s\"", ip, name);
            log_recordf(s, "__panel__", "info", "streamStop", NULL, 0, -1,
                        "%s (\"%s\") stopped by %s", id, name, ip);
        } else {
            log_recordf(s, "__panel__", "error", "streamStop", NULL, 0, -1,
                        "STOP of %s from %s failed: %s", id ? id : "?", ip,
                        err ? err : "unknown error");
        }
        rs_free(ip);
        free(id);
        reply_after_mutation(s, c, hm, rc, err);
        return true;
    }
    if (mg_match(hm->uri, mg_str("/api/streams/*"), NULL) && method_is(hm, "PUT")) {
        char *id = capture(hm, "/api/streams/*");
        rs_json *body = parse_body(hm);
        const char *err = NULL;
        int rc = rs_panel_update_stream(&s->state, id, body, &err);
        rs_json_free(body);
        if (rc == 0 && id) {
            log_record(s, id, "info", "streamUpdate", NULL, 0, -1, "settings saved");
            // A running stream adopts ordinary settings on the next poll. A
            // connection-pool size change performs the controlled restart that
            // is required to replace its pthread pool; a stream switched off
            // simply winds down.
            const rs_json *stream = rs_panel_find_stream(&s->state, id);
            if (stream && strcmp(rs_json_obj_str(stream, "status", "stopped"), "running") == 0)
                live_sync_stream(s, id);
            else
                live_stop_stream(s, id);
        }
        free(id);
        reply_after_mutation(s, c, hm, rc, err);
        return true;
    }
    if (mg_match(hm->uri, mg_str("/api/streams/*"), NULL) && method_is(hm, "DELETE")) {
        char *id = capture(hm, "/api/streams/*");
        const char *err = NULL;
        // Stop the engine before the stream disappears from state: its config
        // is read from there.
        live_stop_stream(s, id);
        int rc = rs_panel_delete_stream(&s->state, id, &err);
        if (rc == 0 && id)
            log_recordf(s, "__panel__", "info", "streamDelete", NULL, 0, -1, "%s deleted", id);
        free(id);
        reply_after_mutation(s, c, hm, rc, err);
        return true;
    }

    // --- users (own {"users":[...]} response shape) ---
    if (mg_match(hm->uri, mg_str("/api/users"), NULL) && method_is(hm, "GET")) {
        reply_json(c, 200, rs_panel_users_view(&s->state), NULL);
        return true;
    }
    if (mg_match(hm->uri, mg_str("/api/users"), NULL) && method_is(hm, "POST")) {
        rs_json *body = parse_body(hm);
        const char *err = NULL;
        int rc = rs_panel_create_user(&s->state, body, &err);
        rs_json_free(body);
        if (rc != 0) { reply_error(c, -rc, err); return true; }
        if (rs_state_save(&s->state) != 0) { reply_error(c, 500, "Could not save state."); return true; }
        reply_json(c, 200, rs_panel_users_view(&s->state), NULL);
        return true;
    }
    if (mg_match(hm->uri, mg_str("/api/users/*"), NULL) && method_is(hm, "DELETE")) {
        char *id = capture(hm, "/api/users/*");
        const char *err = NULL;
        int rc = rs_panel_delete_user(&s->state, id, &err);
        free(id);
        if (rc != 0) { reply_error(c, -rc, err); return true; }
        if (rs_state_save(&s->state) != 0) { reply_error(c, 500, "Could not save state."); return true; }
        reply_json(c, 200, rs_panel_users_view(&s->state), NULL);
        return true;
    }

    // --- api keys (own {"keys":[...]} response shape) ---
    if (mg_match(hm->uri, mg_str("/api/keys"), NULL) && method_is(hm, "GET")) {
        reply_json(c, 200, rs_panel_keys_view(&s->state), NULL);
        return true;
    }
    if (mg_match(hm->uri, mg_str("/api/keys"), NULL) && method_is(hm, "POST")) {
        rs_json *body = parse_body(hm);
        const char *err = NULL;
        int rc = rs_panel_create_key(&s->state, body, &err);
        rs_json_free(body);
        if (rc != 0) { reply_error(c, -rc, err); return true; }
        if (rs_state_save(&s->state) != 0) { reply_error(c, 500, "Could not save state."); return true; }
        reply_json(c, 200, rs_panel_keys_view(&s->state), NULL);
        return true;
    }
    if (mg_match(hm->uri, mg_str("/api/keys/*"), NULL) && method_is(hm, "DELETE")) {
        char *id = capture(hm, "/api/keys/*");
        const char *err = NULL;
        int rc = rs_panel_delete_key(&s->state, id, &err);
        free(id);
        if (rc != 0) { reply_error(c, -rc, err); return true; }
        if (rs_state_save(&s->state) != 0) { reply_error(c, 500, "Could not save state."); return true; }
        reply_json(c, 200, rs_panel_keys_view(&s->state), NULL);
        return true;
    }

    if (mg_match(hm->uri, mg_str("/api/logo-lookup"), NULL) && method_is(hm, "GET")) {
        char *name = query_var(hm, "name");
        if (!name || !name[0]) {
            free(name);
            reply_error(c, 400, "Missing ?name= parameter.");
            return true;
        }
        dispatch_logo_lookup(s, c, name);  // always replies, sync or async
        free(name);
        return true;
    }
    if (mg_match(hm->uri, mg_str("/api/ffmpeg-status"), NULL) && method_is(hm, "GET")) {
        char *path = rs_ffmpeg_resolve();
        rs_json *o = rs_json_new_obj();
        rs_json_obj_set_str(o, "status", path ? "available" : "missing");
        if (path) rs_json_obj_set_str(o, "path", path);
        // Presence is not capability. A build without libxml2 has no DASH
        // demuxer, so an .mpd source cannot be opened at all — a failure that
        // reads as a broken stream unless the panel names it.
        if (path) {
            rs_ffmpeg_caps caps;
            if (rs_ffmpeg_probe_caps(path, &caps) == 0) {
                rs_json_obj_set_bool(o, "hasDash", caps.has_dash);
                rs_json_obj_set_bool(o, "hasHls", caps.has_hls);
                rs_json_obj_set_bool(o, "hasLibxml2", caps.has_libxml2);
                rs_json_obj_set_bool(o, "hasSrt", caps.has_srt);
                rs_json_obj_set_bool(o, "hasHttps", caps.has_https);
                if (caps.version) rs_json_obj_set_str(o, "version", caps.version);
                rs_ffmpeg_caps_dispose(&caps);
            }
        }
        free(path);
        reply_json(c, 200, o, NULL);
        return true;
    }
    if (mg_match(hm->uri, mg_str("/api/nm3u8dlre-status"), NULL) && method_is(hm, "GET")) {
        char *path = rs_nm3u8dlre_resolve();
        rs_json *o = rs_json_new_obj();
        rs_json_obj_set_str(o, "status", path ? "available" : "missing");
        if (path) rs_json_obj_set_str(o, "path", path);
        free(path);
        reply_json(c, 200, o, NULL);
        return true;
    }
    if (mg_match(hm->uri, mg_str("/api/ffmpeg-install"), NULL) && (method_is(hm, "GET") || method_is(hm, "POST"))) {
        char *cmd_plan = rs_ffmpeg_install_plan();
        if (cmd_plan) {
            rs_json *o = rs_json_new_obj();
            rs_json_obj_set_str(o, "command", cmd_plan);
            reply_json(c, 200, o, NULL);
            free(cmd_plan);
        } else {
            reply_error(c, 501, "Automatic install not supported on this platform.");
        }
        return true;
    }
    if (mg_match(hm->uri, mg_str("/api/nm3u8dlre-install"), NULL) && (method_is(hm, "GET") || method_is(hm, "POST"))) {
        char *cmd_plan = rs_nm3u8dlre_install_plan();
        if (cmd_plan) {
            rs_json *o = rs_json_new_obj();
            rs_json_obj_set_str(o, "command", cmd_plan);
            reply_json(c, 200, o, NULL);
            free(cmd_plan);
        } else {
            reply_error(c, 501, "Automatic install not supported on this platform.");
        }
        return true;
    }

    return false;
}

// The origin URL a stream redirects to (its primary source; the CDN-rotation
// the resident ffmpeg does lives in the worker layer, not here).
static const char *stream_source_target(const rs_json *stream) {
    return rs_json_obj_str(stream, "url", "");
}

// Extracts the playback key from ?key= or an Authorization: Bearer header.
static char *playback_key(struct mg_http_message *hm) {
    char buf[256];
    if (mg_http_get_var(&hm->query, "key", buf, sizeof(buf)) > 0) return rs_strdup(buf);
    char *auth = header_dup(hm, "Authorization");
    if (auth && strncmp(auth, "Bearer ", 7) == 0) {
        char *token = rs_strdup(auth + 7);
        free(auth);
        return token;
    }
    free(auth);
    return NULL;
}

// Reads a query variable into a fresh string sized to the query (a proxied
// segment URL can be long). NULL when absent.
static char *query_var(struct mg_http_message *hm, const char *name) {
    if (hm->query.len == 0) return NULL;
    char *buf = (char *)malloc(hm->query.len + 1);
    if (!buf) return NULL;
    int n = mg_http_get_var(&hm->query, name, buf, hm->query.len + 1);
    if (n <= 0) { free(buf); return NULL; }
    return buf;
}

// Percent-encodes for embedding a URL inside a query value, so its own
// '?'/'&'/'=' can't corrupt the /proxy or ?variant= query. Everything outside
// the RFC 3986 unreserved set is escaped.
static char *query_encode(const char *s) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = strlen(s);
    char *out = (char *)malloc(n * 3 + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0xf];
        }
    }
    out[o] = '\0';
    return out;
}

// Content-Type for a proxied item, preferring the upstream value and falling
// back to the URL's extension.
static const char *content_type_from_ext(const char *url) {
    const char *q = strchr(url, '?');
    size_t path_len = q ? (size_t)(q - url) : strlen(url);
    const char *dot = NULL;
    for (size_t i = 0; i < path_len; i++) {
        if (url[i] == '/') dot = NULL;
        else if (url[i] == '.') dot = url + i;
    }
    if (!dot) return "application/octet-stream";
    const char *ext = dot + 1;
    size_t elen = path_len - (size_t)(ext - url);
    struct { const char *ext; const char *type; } map[] = {
        {"ts", "video/mp2t"}, {"m4s", "video/mp4"}, {"mp4", "video/mp4"}, {"m4v", "video/mp4"},
        {"m4a", "audio/mp4"}, {"aac", "audio/aac"}, {"mp3", "audio/mpeg"}, {"vtt", "text/vtt"},
        {"webvtt", "text/vtt"}, {"m3u8", "application/vnd.apple.mpegurl"},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strlen(map[i].ext) == elen && strncmp(ext, map[i].ext, elen) == 0) return map[i].type;
    }
    return "application/octet-stream";
}

// Finds the provider object that owns `stream`.
static const rs_json *provider_of(rs_state *st, const rs_json *stream) {
    const rs_json *providers = rs_json_obj_get(st->root, "providers");
    for (size_t i = 0; i < rs_json_arr_len(providers); i++) {
        const rs_json *p = rs_json_arr_at(providers, i);
        const rs_json *streams = rs_json_obj_get(p, "streams");
        for (size_t j = 0; j < rs_json_arr_len(streams); j++) {
            if (rs_json_arr_at(streams, j) == stream) return p;
        }
    }
    return NULL;
}

// Effective proxy for a fetch: the stream's override, else the provider's, or
// empty when the per-category toggle is off. Caller frees.
static char *effective_proxy(const rs_json *provider, const rs_json *stream, bool category_on) {
    if (!category_on) return rs_strdup("");
    const char *sp = rs_json_obj_str(stream, "proxy", "");
    if (sp[0]) return rs_strdup(sp);
    return rs_strdup(rs_json_obj_str(provider, "proxy", ""));
}

// Provider generic headers plus the stream's category headers, newline-joined.
static char *effective_headers(const rs_json *provider, const rs_json *stream, const char *stream_field) {
    const char *generic = rs_json_obj_str(provider, "headers", "");
    const char *specific = rs_json_obj_str(stream, stream_field, "");
    size_t need = strlen(generic) + strlen(specific) + 2;
    char *out = (char *)malloc(need);
    if (!out) return NULL;
    out[0] = '\0';
    if (generic[0]) { strcat(out, generic); }
    if (specific[0]) { if (out[0]) strcat(out, "\n"); strcat(out, specific); }
    return out;
}

// The video rendition the panel has selected, to pin the engine to.
//
// The panel writes the checkbox selection to `representations` (an array that
// holds the chosen video AND audio ids); `representation` is the older single
// field. panel.c already documents the precedence — "representations if any,
// else the single representation field" — and the whole panel view is built on
// it, but the live-engine config read only the singular field. So every stream
// started with rep "auto" no matter what was ticked, and auto-select takes the
// *highest* bitrate video in the ladder. Selecting 720p silently got you 1080p
// at 4.5 Mbps, which is also why bandwidth demand never matched the config.
//
// The array is not ordered video-first (GLOBO BH stores ["stream_07",
// "stream_04"] — audio, then video), so the entry has to be identified by its
// type from representationMeta rather than by position. Returning "" leaves the
// engine on auto, which is the right fallback when nothing is typed: the engine
// picks the audio rendition itself either way (see the director's `pinned`).
static const char *selected_video_rep(const rs_json *stream) {
    const rs_json *reps = rs_json_obj_get(stream, "representations");
    size_t n = rs_json_arr_len(reps);
    const rs_json *meta = rs_json_obj_get(stream, "representationMeta");
    for (size_t i = 0; i < n; i++) {
        const char *id = rs_json_as_str(rs_json_arr_at(reps, i), "");
        if (!id[0]) continue;
        const rs_json *m = meta ? rs_json_obj_get(meta, id) : NULL;
        const char *type = m ? rs_json_obj_str(m, "type", "") : "";
        if (strcmp(type, "video") == 0) return id;
    }
    // No typing available (an older stream, or a probe that never ran): a
    // single entry is unambiguous enough to honour, more than one is not.
    if (n == 1) {
        const char *only = rs_json_as_str(rs_json_arr_at(reps, 0), "");
        if (only[0] && !meta) return only;
    }
    return rs_json_obj_str(stream, "representation", "");
}

// The provider's chosen download tool ("native", i.e. in-process libcurl, by
// default) and its extra params. Applied to this provider's manifest/segment
// fetches; see rs_fetch_url.
static const char *effective_downloader(const rs_json *provider, const rs_json *stream) {
    const char *sd = rs_json_obj_str(stream, "downloader", "");
    if (sd[0]) return sd;
    const char *d = rs_json_obj_str(provider, "downloader", "");
    return d[0] ? d : "native";
}
static const char *effective_downloader_params(const rs_json *provider, const rs_json *stream) {
    const char *sd = rs_json_obj_str(stream, "downloader", "");
    if (sd[0]) return rs_json_obj_str(stream, "downloaderParams", "");
    return rs_json_obj_str(provider, "downloaderParams", "");
}

// The rewrite transforms need the stream id to build restream/variant paths.
// Picks a filename with an extension ffmpeg's HLS demuxer will accept
// (keys/maps aside, it validates the segment URL's extension). Keeps the source
// URL's own extension when it's a short alphanumeric one, else a sane default.
static void proxy_filename(const char *abs_uri, rs_m3u8_line_kind kind, char *out, size_t outlen) {
    if (kind == RS_M3U8_LINE_KEY) { snprintf(out, outlen, "key.key"); return; }
    if (kind == RS_M3U8_LINE_MAP) { snprintf(out, outlen, "init.mp4"); return; }
    const char *q = strchr(abs_uri, '?');
    size_t plen = q ? (size_t)(q - abs_uri) : strlen(abs_uri);
    const char *dot = NULL;
    for (size_t i = 0; i < plen; i++) {
        if (abs_uri[i] == '/') dot = NULL;
        else if (abs_uri[i] == '.') dot = abs_uri + i;
    }
    if (dot) {
        size_t elen = plen - (size_t)(dot + 1 - abs_uri);
        bool ok = elen >= 1 && elen <= 5;
        for (size_t i = 0; ok && i < elen; i++)
            if (!isalnum((unsigned char)dot[1 + i])) ok = false;
        if (ok) { snprintf(out, outlen, "seg.%.*s", (int)elen, dot + 1); return; }
    }
    snprintf(out, outlen, "seg.ts");
}

static char *media_transform(void *ud, const char *abs_uri, rs_m3u8_line_kind kind, int64_t seq) {
    (void)seq;
    const char *stream_id = (const char *)ud;
    const char *kindstr = kind == RS_M3U8_LINE_KEY ? "key" : (kind == RS_M3U8_LINE_MAP ? "map" : "segment");
    char fname[16];
    proxy_filename(abs_uri, kind, fname, sizeof(fname));
    char *enc = query_encode(abs_uri);
    if (!enc) return NULL;
    size_t need = strlen(stream_id) + strlen(enc) + strlen(fname) + 64;
    char *out = (char *)malloc(need);
    if (out) snprintf(out, need, "/restream/%s/%s?u=%s&kind=%s", stream_id, fname, enc, kindstr);
    free(enc);
    return out;
}

static char *master_transform(void *ud, const char *abs_uri) {
    const char *stream_id = (const char *)ud;
    char *enc = query_encode(abs_uri);
    if (!enc) return NULL;
    size_t need = strlen(stream_id) + strlen(enc) + 48;
    char *out = (char *)malloc(need);
    if (out) snprintf(out, need, "/play/%s/index.m3u8?variant=%s", stream_id, enc);
    free(enc);
    return out;
}

static void send_redirect(struct mg_connection *c, const char *location) {
    mg_printf(c, "HTTP/1.1 302 Found\r\nLocation: %s\r\nCache-Control: no-store\r\n"
                 "Content-Length: 0\r\n\r\n", location);
    // mg_http_reply clears this itself; a raw mg_printf response has to do it
    // by hand or mongoose leaves the connection marked mid-response forever —
    // see the identical fix on the two mg_send() sites below for what that
    // does to a client that reuses the connection for its next request.
    c->is_resp = 0;
}

// --- async jobs --------------------------------------------------------------
//
// Five routes used to call blocking work — libcurl fetches, a libxml2 probe, or
// a spawned script with up to a 30s timeout — inline, in this thread: the
// single mongoose poll loop that every connection, every stream and the panel
// API all depend on. Any one of them stalling froze the entire server for as
// long as it took: HLS-passthrough playlist/segment fetches, `/api/probe`
// (source auto-detect), `/api/logo-lookup`, and provider script actions.
// serve_dash_master's comment further below documents and fixes the identical
// failure for DASH by never fetching inline there; this generalises the same
// fix to the rest.
//
// The blocking call — and, once it returns, any CPU-only post-processing
// (playlist rewriting, ClearKey CENC, HLS AES-128 decrypt, script log lines) —
// runs on a detached worker thread. struct mg_connection is not thread-safe,
// so the worker never touches `c`; mg_wakeup() is mongoose's documented
// thread-safe way back into the poll loop, which is the only thing allowed to
// build and send the reply.
typedef enum {
    RS_PENDING_PLAYLIST,
    RS_PENDING_ITEM,
    RS_PENDING_LOGO,
    RS_PENDING_PROBE,
    RS_PENDING_SCRIPT,
} rs_pending_kind;

struct rs_pending_job {
    struct rs_pending_job *next;  // server->pending_head intrusive list
    restream_server_t *server;
    unsigned long conn_id;
    rs_pending_kind kind;
    bool cancelled;  // connection closed while the job was in flight; guarded by server->pending_mu

    // Request, copied up front: hm/stream (and the POST body) are only valid
    // for this one mongoose event, and a stream/provider JSON node can be
    // edited or deleted by another request while this job is in flight.
    char *stream_id;               // PLAYLIST/ITEM: stream id. SCRIPT: "script:<providerId>", also the log key.
    char *url;                     // PLAYLIST/ITEM: fetch URL. PROBE: source URL. LOGO: channel name to look up.
    char *proxy;                   // PLAYLIST/ITEM/PROBE
    char *headers;                 // PLAYLIST/ITEM/PROBE
    char *range;                   // ITEM only
    char *downloader;               // PLAYLIST/ITEM only
    char *downloader_params;       // PLAYLIST/ITEM only
    bool decrypt, is_map, is_key;  // ITEM only
    char *decryption_keys;         // ITEM only, "KID:KEY[,KID:KEY...]"
    char *hls_key, *hls_iv;        // ITEM only
    int seq;                       // ITEM only, for HLS AES-128 IV derivation
    char *user_agent;              // ITEM only, for the metrics/log line
    char *playback_key_str;        // ITEM only, ditto
    char *client_ip;               // ITEM only — resolved at dispatch, since the
                                   // request (and any X-Forwarded-For) is gone
                                   // by the time the worker reports back
    char *script_path;             // SCRIPT only
    char **script_args;            // SCRIPT only, each rs_script_arg-owned
    int script_argc;               // SCRIPT only

    // Result, filled by pending_job_worker.
    int rc;
    long status;
    char *body;              // ITEM/PLAYLIST: fetched bytes. PROBE: result JSON. LOGO: looked-up URL, or NULL.
    size_t body_len;
    char *content_type;
    char *content_range;
    char *script_stdout, *script_stderr;  // SCRIPT only
    char err[256];
};

static void pending_job_free(rs_pending_job *pf) {
    if (!pf) return;
    free(pf->stream_id); free(pf->url); free(pf->proxy); free(pf->headers);
    free(pf->range); free(pf->downloader); free(pf->downloader_params);
    free(pf->decryption_keys); free(pf->hls_key); free(pf->hls_iv);
    free(pf->user_agent); free(pf->playback_key_str); free(pf->client_ip);
    free(pf->script_path);
    for (int i = 0; i < pf->script_argc; i++) free(pf->script_args[i]);
    free(pf->script_args);
    free(pf->body); free(pf->content_type); free(pf->content_range);
    free(pf->script_stdout); free(pf->script_stderr);
    free(pf);
}

static void *pending_job_worker(void *arg) {
    rs_pending_job *pf = (rs_pending_job *)arg;
    restream_server_t *server = pf->server;

    switch (pf->kind) {
    case RS_PENDING_PLAYLIST:
    case RS_PENDING_ITEM:
        pf->rc = g_fetch_handler(pf->url, pf->proxy, pf->headers, pf->range,
                                 pf->downloader, pf->downloader_params,
                                 &pf->body, &pf->body_len, &pf->status,
                                 &pf->content_type, &pf->content_range, NULL,
                                 pf->err, sizeof(pf->err), 30000, NULL, NULL);
        break;
    case RS_PENDING_LOGO:
        // The cache itself isn't thread-safe (a plain cJSON tree, no locking —
        // see logo.c), and lazily creates server->logo_cache on first use, so
        // the whole lookup — cache hit or fetch-then-cache-write miss — runs
        // under logo_mu. That only serialises concurrent logo lookups against
        // each other; the poll thread is still free the whole time.
        pthread_mutex_lock(&server->logo_mu);
        if (!server->logo_cache) server->logo_cache = rs_logo_cache_create("logos.json");
        pf->body = rs_logo_lookup(server->logo_cache, pf->url, logo_fetch_wrapper, NULL);
        pthread_mutex_unlock(&server->logo_mu);
        pf->rc = pf->body ? 0 : -1;
        break;
    case RS_PENDING_PROBE:
        pf->body = g_probe_handler(pf->url, pf->proxy, pf->headers, pf->err, sizeof(pf->err));
        pf->rc = pf->body ? 0 : -1;
        break;
    case RS_PENDING_SCRIPT:
        pf->rc = rs_script_run_sync(pf->script_path, (const char **)pf->script_args, pf->script_argc,
                                    30.0, &pf->script_stdout, &pf->script_stderr);
        break;
    }

    pthread_mutex_lock(&server->pending_mu);
    // Unlink before reading `cancelled`, under the same lock the close handler
    // uses to set it — once unlinked nothing else can find pf, so everything
    // after unlock is safe to touch without the lock.
    rs_pending_job **link = &server->pending_head;
    while (*link && *link != pf) link = &(*link)->next;
    if (*link == pf) *link = pf->next;
    bool cancelled = pf->cancelled;
    pthread_cond_broadcast(&server->pending_cv);  // wakes restream_server_destroy's drain
    pthread_mutex_unlock(&server->pending_mu);

    // Either the player gave up and nobody is listening any more, or mg_wakeup
    // could not queue the message (pipe not initialised) — either way there is
    // no reply to build, so free the job here instead of leaking it.
    if (cancelled || !mg_wakeup(&server->mgr, pf->conn_id, &pf, sizeof(pf))) {
        pending_job_free(pf);
    }
    return NULL;
}

// Hands `pf` off to a worker thread. Always consumes pf: on success the worker
// frees it once the reply is built; on failure the caller must free it. Fills
// pf->server/conn_id, so callers only need everything else set.
static bool pending_job_dispatch(restream_server_t *server, struct mg_connection *c, rs_pending_job *pf) {
    // Without a working wakeup pipe a worker could still fetch, but could never
    // hand the result back — the connection would hang forever instead of
    // erroring. Fail the dispatch up front so callers reply with a clean 500.
    if (!server->wakeup_ok) return false;
    pf->server = server;
    pf->conn_id = c->id;
    pthread_mutex_lock(&server->pending_mu);
    pf->next = server->pending_head;
    server->pending_head = pf;
    pthread_mutex_unlock(&server->pending_mu);

    pthread_t tid;
    if (pthread_create(&tid, NULL, pending_job_worker, pf) != 0) {
        pthread_mutex_lock(&server->pending_mu);
        rs_pending_job **link = &server->pending_head;
        while (*link && *link != pf) link = &(*link)->next;
        if (*link == pf) *link = pf->next;
        pthread_mutex_unlock(&server->pending_mu);
        return false;
    }
    pthread_detach(tid);
    return true;
}

// Looks up a channel logo — see the "async jobs" section above and
// pending_job_finish_logo. Always replies to `c`.
static void dispatch_logo_lookup(restream_server_t *server, struct mg_connection *c, const char *name) {
    rs_pending_job *pf = (rs_pending_job *)calloc(1, sizeof(*pf));
    if (!pf) { reply_error(c, 500, "Out of memory."); return; }
    pf->kind = RS_PENDING_LOGO;
    pf->url = rs_strdup(name);  // reused as the lookup key — see the struct's field comments
    if (!pending_job_dispatch(server, c, pf)) {
        pending_job_free(pf);
        reply_error(c, 500, "Could not start the logo lookup.");
    }
}

// Source auto-detect — see the "async jobs" section above and
// pending_job_finish_probe. Takes ownership of url/proxy/headers. Always
// replies to `c`.
static void dispatch_probe(restream_server_t *server, struct mg_connection *c,
                           char *url, char *proxy, char *headers) {
    rs_pending_job *pf = (rs_pending_job *)calloc(1, sizeof(*pf));
    if (!pf) {
        free(url); free(proxy); free(headers);
        reply_error(c, 500, "Out of memory.");
        return;
    }
    pf->kind = RS_PENDING_PROBE;
    pf->url = url;
    pf->proxy = proxy;
    pf->headers = headers;  // pf owns all three now
    if (!pending_job_dispatch(server, c, pf)) {
        pending_job_free(pf);
        reply_error(c, 500, "Could not start the probe.");
    }
}

// Runs a provider script action — see the "async jobs" section above and
// pending_job_finish_script. Takes ownership of args (each element and the
// array itself). Always replies to `c`.
static void dispatch_script_action(restream_server_t *server, struct mg_connection *c,
                                   const char *sid, const char *script_path,
                                   char **args, int argc) {
    rs_pending_job *pf = (rs_pending_job *)calloc(1, sizeof(*pf));
    if (!pf) {
        for (int i = 0; i < argc; i++) free(args[i]);
        free(args);
        reply_error(c, 500, "Out of memory.");
        return;
    }
    pf->kind = RS_PENDING_SCRIPT;
    pf->stream_id = rs_strdup(sid);
    pf->script_path = rs_strdup(script_path);
    pf->script_args = args;  // pf owns it now
    pf->script_argc = argc;
    if (!pending_job_dispatch(server, c, pf)) {
        pending_job_free(pf);
        reply_error(c, 500, "Could not start the script.");
    }
}

static void pending_job_cancel(restream_server_t *s, unsigned long conn_id) {
    pthread_mutex_lock(&s->pending_mu);
    for (rs_pending_job *pf = s->pending_head; pf; pf = pf->next)
        if (pf->conn_id == conn_id) pf->cancelled = true;
    pthread_mutex_unlock(&s->pending_mu);
}

// Blocks until every dispatched job has finished and freed itself. Bounded by
// libcurl's own connect/transfer timeouts (net.c) or the script runner's 30s
// timeout (script.c), so this cannot hang forever even against an origin or
// script that never returns. Must run before mg_mgr_free and before
// pending_mu/pending_cv are destroyed — a worker mid-flight touches all three
// when it finishes.
static void pending_job_wait_idle(restream_server_t *s) {
    pthread_mutex_lock(&s->pending_mu);
    while (s->pending_head != NULL) pthread_cond_wait(&s->pending_cv, &s->pending_mu);
    pthread_mutex_unlock(&s->pending_mu);
}

static void pending_job_finish_playlist(struct mg_connection *c, rs_pending_job *pf) {
    if (pf->rc != 0) {
        reply_error(c, 502, pf->err[0] ? pf->err : "Could not fetch the playlist.");
        return;
    }
    char *rewritten;
    if (rs_m3u8_is_master(pf->body)) {
        rewritten = rs_m3u8_rewrite_master(pf->body, pf->url, master_transform, (void *)pf->stream_id);
    } else {
        // The player fetches (and, if encrypted, decrypts with) the key itself,
        // so keys are proxied through rather than dropped — server-side HLS
        // decryption is a later increment.
        rewritten = rs_m3u8_rewrite(pf->body, pf->url, false, media_transform, (void *)pf->stream_id);
    }
    if (!rewritten) { reply_error(c, 500, "Out of memory rewriting the playlist."); return; }
    mg_http_reply(c, 200, "Content-Type: application/vnd.apple.mpegurl; charset=utf-8\r\n"
                          "Cache-Control: no-store\r\n", "%s", rewritten);
    rs_free(rewritten);
}

static void pending_job_finish_item(restream_server_t *server, struct mg_connection *c, rs_pending_job *pf) {
    if (pf->rc != 0) {
        log_record(server, pf->stream_id, "error", pf->is_map ? "downloadInit" : "downloadSegment",
                   pf->url, 0, -1, pf->err[0] ? pf->err : "fetch failed");
        reply_error(c, 502, pf->err[0] ? pf->err : "Segment fetch failed.");
        return;
    }

    uint8_t *body = (uint8_t *)pf->body;
    size_t body_len = pf->body_len;
    long status = pf->status;

    if (pf->decrypt) {
        size_t new_len = 0;
        body = apply_cenc(pf->decryption_keys, pf->is_map, body, body_len, &new_len);
        body_len = new_len;
        status = 0; free(pf->content_range); pf->content_range = NULL;  // decrypted body is a fresh whole object
    }

    if (pf->hls_key && pf->hls_key[0] != '\0' && !pf->is_key) {
        uint8_t *dec_out = NULL;
        size_t dec_len = 0;
        if (rs_hls_decrypt(body, body_len, pf->hls_key, pf->hls_iv ? pf->hls_iv : "", pf->seq, &dec_out, &dec_len) == 0 && dec_out) {
            free(body);
            body = dec_out;
            body_len = dec_len;
            status = 0; free(pf->content_range); pf->content_range = NULL;
        } else {
            log_record(server, pf->stream_id, "error", "decryptSegment", pf->url, 0, -1, "HLS decryption failed");
        }
    }

    // Network monitor: attribute the served bytes to this stream + client, and
    // log the fetch so the stream's Logs tab shows download activity.
    const char *peer = pf->client_ip ? pf->client_ip : "";
    const char *identity = (pf->playback_key_str && pf->playback_key_str[0]) ? pf->playback_key_str : peer;
    rs_metrics_record(server->metrics, pf->stream_id, identity, peer,
                      pf->user_agent ? pf->user_agent : "", (int)body_len);
    log_recordf(server, pf->stream_id, "info", pf->is_map ? "serveInit" : "serveSegment",
                pf->url, status, (long long)body_len, "fetched for %s", peer);

    const char *type = (pf->content_type && pf->content_type[0]) ? pf->content_type : content_type_from_ext(pf->url);
    // Relay the upstream's 206 + Content-Range so the player's byte-range
    // request is answered as a partial response, not a 200 of the wrong length.
    if (status == 206 && pf->content_range && pf->content_range[0]) {
        mg_printf(c, "HTTP/1.1 206 Partial Content\r\nContent-Type: %s\r\nContent-Range: %s\r\n"
                     "Content-Length: %lu\r\nAccept-Ranges: bytes\r\nCache-Control: no-store\r\n"
                     "Access-Control-Allow-Origin: *\r\n\r\n",
                  type, pf->content_range, (unsigned long)body_len);
    } else {
        mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
                     "Accept-Ranges: bytes\r\nCache-Control: no-store\r\n"
                     "Access-Control-Allow-Origin: *\r\n\r\n",
                  type, (unsigned long)body_len);
    }
    mg_send(c, body, body_len);
    // mg_http_reply resets this when it's done; mg_send on its own does not,
    // so mongoose still considered this connection "generating a response"
    // once the handler returned. A client that reuses the connection — every
    // real HLS player does, for every segment after the first — then sent its
    // next request into a connection mongoose would never read from again:
    // no error, no response, just a hang that looked identical to a network
    // stall no matter how the player or the origin fetch was tuned.
    c->is_resp = 0;
    // CENC/HLS decrypt free pf->body and hand back a distinct allocation, so
    // `body` may not be pf->body any more — free whichever one we ended up
    // with here, and null pf->body so pending_job_free doesn't double-free it.
    free(body);
    pf->body = NULL;
}

static void pending_job_finish_logo(struct mg_connection *c, rs_pending_job *pf) {
    if (!pf->body) { reply_error(c, 404, "Logo not found."); return; }
    rs_json *o = rs_json_new_obj();
    rs_json_obj_set_str(o, "url", pf->body);
    reply_json(c, 200, o, NULL);
}

static void pending_job_finish_probe(struct mg_connection *c, rs_pending_job *pf) {
    if (!pf->body) { reply_error(c, 400, pf->err[0] ? pf->err : "Could not probe the source."); return; }
    char headers_out[128];
    snprintf(headers_out, sizeof(headers_out), "%sCache-Control: no-store\r\n", JSON_HEADERS);
    mg_http_reply(c, 200, headers_out, "%s", pf->body);
}

// Mirrors run_provider_script's post-run handling exactly — split stdout/
// stderr into log lines, log the exit, persist state (a script action can
// change provider fields, e.g. session cookies), then answer with the log
// tail so the panel's script output shows immediately.
static void pending_job_finish_script(restream_server_t *server, struct mg_connection *c, rs_pending_job *pf) {
    if (pf->script_stdout && pf->script_stdout[0]) {
        char *copy = rs_strdup(pf->script_stdout);
        for (char *line = strtok(copy, "\r\n"); line; line = strtok(NULL, "\r\n"))
            if (line[0]) log_record(server, pf->stream_id, "info", "scriptOutput", NULL, 0, -1, line);
        free(copy);
    }
    if (pf->script_stderr && pf->script_stderr[0]) {
        char *copy = rs_strdup(pf->script_stderr);
        for (char *line = strtok(copy, "\r\n"); line; line = strtok(NULL, "\r\n"))
            if (line[0]) log_record(server, pf->stream_id, "error", "scriptError", NULL, 0, -1, line);
        free(copy);
    }
    log_record(server, pf->stream_id, pf->rc == 0 ? "info" : "error", "scriptEnd", NULL, pf->rc, -1,
               pf->rc == 0 ? "ok" : "script exited non-zero");
    // A script action can change provider state (session cookies etc.) even on
    // a nonzero exit, so persist regardless. A nonzero exit is reported through
    // the log, not as an HTTP error — matches the original synchronous handler,
    // and the panel surfaces it via the script output/Logs tab either way.
    rs_state_save(&server->state);
    reply_json(c, 200, log_view(server, pf->stream_id, 150), NULL);
}

static void pending_job_finish(restream_server_t *server, struct mg_connection *c, struct mg_str *data) {
    if (!data || data->len != sizeof(rs_pending_job *)) return;
    rs_pending_job *pf;
    memcpy(&pf, data->buf, sizeof(pf));
    switch (pf->kind) {
    case RS_PENDING_PLAYLIST: pending_job_finish_playlist(c, pf); break;
    case RS_PENDING_ITEM: pending_job_finish_item(server, c, pf); break;
    case RS_PENDING_LOGO: pending_job_finish_logo(c, pf); break;
    case RS_PENDING_PROBE: pending_job_finish_probe(c, pf); break;
    case RS_PENDING_SCRIPT: pending_job_finish_script(server, c, pf); break;
    }
    pending_job_free(pf);
}

// Serves an HLS passthrough playlist: fetch the remote playlist (the configured
// URL, or a ?variant= link from an already-rewritten master), rewrite every URI
// to loop back through this server, and return it. The fetch runs on a worker
// thread — see the "async HLS-passthrough fetch" section above.
static void serve_hls_playlist(restream_server_t *server, struct mg_connection *c,
                               struct mg_http_message *hm, const rs_json *stream) {
    const char *stream_id = rs_json_obj_str(stream, "id", "");
    const rs_json *provider = provider_of(&server->state, stream);

    char *variant = query_var(hm, "variant");
    const char *manifest_url = variant ? variant : stream_source_target(stream);
    if (!manifest_url[0]) { free(variant); reply_error(c, 400, "Stream has no source URL."); return; }

    rs_pending_job *pf = (rs_pending_job *)calloc(1, sizeof(*pf));
    if (!pf) { free(variant); reply_error(c, 500, "Out of memory."); return; }
    pf->kind = RS_PENDING_PLAYLIST;
    pf->stream_id = rs_strdup(stream_id);
    pf->url = rs_strdup(manifest_url);
    pf->proxy = effective_proxy(provider, stream, rs_json_obj_bool(stream, "proxyManifest", true));
    pf->headers = effective_headers(provider, stream, "manifestHeaders");
    pf->downloader = rs_strdup(effective_downloader(provider, stream));
    pf->downloader_params = rs_strdup(effective_downloader_params(provider, stream));
    free(variant);

    if (!pending_job_dispatch(server, c, pf)) {
        pending_job_free(pf);
        reply_error(c, 500, "Could not start the playlist fetch.");
    }
}

// Applies ClearKey CENC to a fetched DASH item in place: patches the init
// segment (kind=map) or AES-CTR-decrypts a media segment using the stream's
// configured KID:KEY. No-op (returns the input) when no key is set. Takes the
// raw "KID:KEY" string rather than the stream object itself so it can run on
// pending_job_worker's copy after the stream JSON may have changed underneath.
static uint8_t *apply_cenc(const char *decryption_keys, bool is_map, uint8_t *body, size_t body_len, size_t *out_len) {
    *out_len = body_len;
    rs_cenc_keys keys = rs_cenc_parse_keys(decryption_keys ? decryption_keys : "");
    uint8_t *out = NULL;
    if (is_map) {
        // The init needs patching to a clear codec even before any key exists.
        out = rs_cenc_patch_init(body, body_len, out_len, NULL, NULL);
    } else if (keys.count > 0) {
        out = rs_cenc_decrypt_segment(body, body_len, out_len, keys.keys[0], 8);
    }
    rs_cenc_keys_free(&keys);
    if (out) { free(body); return out; }
    *out_len = body_len;
    return body;
}

// Restreams one segment/key/init.
//
// For a DASH stream (live=1) this only ever hands back what the live engine
// already downloaded and decrypted — a memcpy, never a network request. The
// inline fetch below is for the HLS passthrough path, which has no engine
// behind it.
static void serve_restream_item(restream_server_t *server, struct mg_connection *c,
                                struct mg_http_message *hm, const char *stream_id,
                                const char *fname) {
    const rs_json *stream = rs_panel_find_stream(&server->state, stream_id);
    if (!stream) { reply_error(c, 404, "Stream not found."); return; }
    const char *st_val = rs_json_obj_str(stream, "status", "stopped");
    if (strcmp(st_val, "running") != 0 && !rs_json_obj_bool(stream, "directSource", false)) {
        reply_error(c, 404, "Stream is stopped.");
        return;
    }

    // Live-engine URL: "<repIndex>_<sequence>.m4s" or "<repIndex>_init.mp4".
    // The bytes were downloaded and decrypted by the background worker before
    // this segment was ever advertised, so answering costs a memcpy and the
    // request never touches the network. The URL deliberately carries no
    // upstream address — see the playlist renderer in live.c.
    // Parsed by hand rather than with sscanf: sscanf returns the number of
    // *assignments* it made, not whether the whole format matched, so
    // sscanf("0_4.m4s", "%d_init", &i) returns 1 — %d assigned, then the
    // literal "init" failed — and every segment was mistaken for the init
    // segment. Players got a valid fMP4 header with no samples on every
    // request and reported "Found no media in fragment".
    int rep_index = -1;
    long long want_seq = -1;
    bool live_init = false, live_seg = false;
    if (fname && fname[0] >= '0' && fname[0] <= '9') {
        char *after_idx = NULL;
        long idx = strtol(fname, &after_idx, 10);
        // rs_live_take_indexed bounds the index against the stream's actual
        // representation count; this only rejects obvious nonsense.
        if (after_idx != fname && *after_idx == '_' && idx >= 0 && idx < 64) {
            const char *rest = after_idx + 1;
            if (strncmp(rest, "init", 4) == 0) {
                rep_index = (int)idx;
                live_init = true;
            } else {
                char *after_seq = NULL;
                long long seq = strtoll(rest, &after_seq, 10);
                if (after_seq != rest) {
                    rep_index = (int)idx;
                    want_seq = seq;
                    live_seg = true;
                }
            }
        }
    }
    if (live_init || live_seg) {
        size_t clen = 0;
        uint8_t *cached = rs_live_take_indexed(server->live, stream_id, rep_index,
                                               want_seq, live_init, &clen);
        char *cip = client_ip(server, c, hm);
        if (cached) {
            char *cua = header_dup(hm, "User-Agent");
            char *ckey = playback_key(hm);
            rs_metrics_record(server->metrics, stream_id, (ckey && ckey[0]) ? ckey : cip,
                              cip, cua ? cua : "", (int)clen);
            log_recordf(server, stream_id, "info", live_init ? "serveInit" : "serveSegment",
                        NULL, 200, (long long)clen, "rep %d %s to %s",
                        rep_index, fname, cip);
            free(cua); free(ckey);
            // A subtitle rendition's segments are WebVTT documents, not fMP4 —
            // the live engine converted them on the way in. Serving them as
            // video/mp4 makes a browser refuse to parse the track.
            const char *fext = strrchr(fname, '.');
            const char *ctype = (fext && strcmp(fext, ".vtt") == 0)
                                    ? "text/vtt; charset=utf-8" : "video/mp4";
            mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
                         "Accept-Ranges: bytes\r\nCache-Control: no-store\r\n"
                         "Access-Control-Allow-Origin: *\r\n\r\n", ctype, (unsigned long)clen);
            mg_send(c, cached, clen);
            // Without this, mongoose leaves the connection marked as still
            // generating a response (mg_http_reply would have cleared it) and
            // never reads the next request off it — invisible on a one-shot
            // curl, which opens a fresh connection every time, but every real
            // HLS client keeps the connection alive across segment requests
            // and would silently hang on the second one, forever, with zero
            // bytes and no error on either side.
            c->is_resp = 0;
            rs_free(cached);
            rs_free(cip);
            return;
        }
        // The player asked for something that has aged out of the queue, so it
        // has fallen behind the advertised window. 404 immediately and let it
        // reseek to the live edge — that is the recovery it is built for, and
        // there is nothing to fetch inline because the origin URL is long gone.
        log_recordf(server, stream_id, "error",
                    live_init ? "cacheMissInit" : "cacheMissSegment", NULL, 404, -1,
                    "rep %d %s outside the live window — 404 so %s reseeks "
                    "(raise keepSegments if this repeats)", rep_index, fname, cip);
        mg_http_reply(c, 404, "Content-Type: text/plain\r\nCache-Control: no-store\r\n"
                              "Access-Control-Allow-Origin: *\r\n",
                      "Segment is outside the live window.\n");
        rs_free(cip);
        return;
    }

    // Everything below is the HLS passthrough path, which has no engine behind
    // it and still resolves ?u= per request. The fetch (and any CENC/HLS
    // decrypt) runs on a worker thread — see the "async HLS-passthrough fetch"
    // section above serve_hls_playlist — instead of blocking this, the single
    // mongoose poll loop every other connection also depends on.
    char *url = query_var(hm, "u");
    if (!url) { reply_error(c, 400, "Missing ?u= target."); return; }
    const rs_json *provider = provider_of(&server->state, stream);

    char *dec = query_var(hm, "dec");
    char *kindq = query_var(hm, "kind");
    bool decrypt = dec && dec[0] == '1';
    bool is_map = kindq && strcmp(kindq, "map") == 0;
    bool is_key = kindq && strcmp(kindq, "key") == 0;
    free(dec); free(kindq);

    rs_pending_job *pf = (rs_pending_job *)calloc(1, sizeof(*pf));
    if (!pf) { free(url); reply_error(c, 500, "Out of memory."); return; }
    pf->kind = RS_PENDING_ITEM;
    pf->stream_id = rs_strdup(stream_id);
    pf->url = url;  // pf owns it now
    pf->decrypt = decrypt;
    pf->is_map = is_map;
    pf->is_key = is_key;
    // Forward the player's Range so byte-range segments and seeking fetch only
    // the requested slice — but never for a to-be-decrypted item: CENC needs the
    // whole segment, so a ranged fetch would corrupt it.
    pf->range = decrypt ? NULL : header_dup(hm, "Range");
    pf->proxy = effective_proxy(provider, stream, rs_json_obj_bool(stream, "proxyMedia", true));
    pf->headers = effective_headers(provider, stream, "mediaHeaders");
    pf->downloader = rs_strdup(effective_downloader(provider, stream));
    pf->downloader_params = rs_strdup(effective_downloader_params(provider, stream));
    pf->decryption_keys = rs_strdup(rs_json_obj_str(stream, "decryptionKeys", ""));
    pf->hls_key = rs_strdup(rs_json_obj_str(stream, "hlsKey", ""));
    pf->hls_iv = rs_strdup(rs_json_obj_str(stream, "hlsIV", ""));
    char *seq_str = query_var(hm, "seq");
    pf->seq = seq_str ? atoi(seq_str) : 0;
    free(seq_str);
    pf->user_agent = header_dup(hm, "User-Agent");
    pf->client_ip = client_ip(server, c, hm);
    pf->playback_key_str = playback_key(hm);

    if (!pending_job_dispatch(server, c, pf)) {
        pending_job_free(pf);
        reply_error(c, 502, "Segment fetch failed.");
    }
}

// --- the live DASH engine's control plane -----------------------------------
//
// Everything DASH is produced by a background engine (rs_live) rather than by
// the request handler. The handlers below only start/stop engines and hand out
// what the engine has already built, so no playback route ever waits on the
// origin — which is what kept the panel's Start/Stop buttons unresponsive, the
// whole server being single-threaded.

// Brings the live engine for `stream_id` in line with the stored config:
// starts it if the stream is a running internal-mode MPD, and otherwise leaves
// it alone. Safe (and cheap) to call repeatedly — ordinary settings are picked
// up on the next poll. A changed connection-pool size performs a controlled
// engine restart because an existing pthread pool cannot resize in place.
static void live_sync_stream(restream_server_t *s, const char *stream_id) {
    if (!s || !s->live || !stream_id || !stream_id[0]) return;
    const rs_json *stream = rs_panel_find_stream(&s->state, stream_id);
    if (!stream) return;
    if (rs_json_obj_bool(stream, "directSource", false)) return;
    if (strcmp(rs_json_obj_str(stream, "kind", "mpd"), "mpd") != 0) return;
    if (strcmp(rs_json_obj_str(stream, "inputMode", "internal"), "internal") != 0) return;
    if (strcmp(rs_json_obj_str(stream, "status", "stopped"), "running") != 0) return;

    const char *src = stream_source_target(stream);
    if (!src[0]) {
        log_record(s, stream_id, "error", "liveStart", NULL, 0, -1,
                   "stream has no source URL — nothing to poll");
        return;
    }

    const rs_json *provider = provider_of(&s->state, stream);
    char *mproxy = effective_proxy(provider, stream, rs_json_obj_bool(stream, "proxyManifest", true));
    char *dproxy = effective_proxy(provider, stream, rs_json_obj_bool(stream, "proxyMedia", true));
    char *mheaders = effective_headers(provider, stream, "manifestHeaders");
    char *dheaders = effective_headers(provider, stream, "mediaHeaders");

    // CDN mirrors, in configured order. The panel has always collected these and
    // promised failover; the engine now actually rotates through them.
    const rs_json *mirrors = rs_json_obj_get(stream, "cdnUrls");
    size_t mirror_count = rs_json_arr_len(mirrors);
    const char **mirror_urls = NULL;
    size_t mirror_used = 0;
    if (mirror_count) {
        mirror_urls = (const char **)calloc(mirror_count, sizeof(char *));
        for (size_t i = 0; mirror_urls && i < mirror_count; i++) {
            const char *u = rs_json_as_str(rs_json_arr_at(mirrors, i), "");
            if (u && u[0]) mirror_urls[mirror_used++] = u;
        }
    }

    rs_live_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mpd_url = src;
    cfg.cdn_urls = mirror_urls;
    cfg.cdn_url_count = mirror_used;
    cfg.representation = selected_video_rep(stream);
    cfg.manifest_proxy = mproxy;
    cfg.media_proxy = dproxy;
    cfg.manifest_headers = mheaders;
    cfg.media_headers = dheaders;
    cfg.downloader = effective_downloader(provider, stream);
    cfg.dl_params = effective_downloader_params(provider, stream);
    cfg.decryption_keys = rs_json_obj_str(stream, "decryptionKeys", "");
    // inheritUrlParams wins: some CDNs sign a token only onto the manifest's
    // redirect target, so a segmentUrlParams typed ahead of time from the
    // configured source URL would always be empty for them (see
    // rs_dash_describe). Provider-level only — no per-stream override, same as
    // the Swift panel's effectiveSegmentUrlParams.
    cfg.inherit_url_params = provider ? rs_json_obj_bool(provider, "inheritUrlParams", false) : false;
    cfg.segment_url_params = (!cfg.inherit_url_params && provider) ? rs_json_obj_str(provider, "segmentUrlParams", "") : "";
    // Per-stream, off by default: some CDNs cap concurrent sessions per signed
    // token low enough that the director's own periodic re-poll (a 3rd fetch
    // alongside the video and audio workers) trips it by itself. See rs_live.h.
    cfg.reduced_manifest_polling = rs_json_obj_bool(stream, "reducedManifestPolling", true);
    cfg.playlist_segments = (int)rs_json_obj_int(stream, "playlistSegments", 6);
    cfg.keep_segments = (int)rs_json_obj_int(stream, "keepSegments", 10);
    cfg.download_ahead = (int)rs_json_obj_int(stream, "downloadAhead", 20);
    cfg.parallel_downloads = (int)rs_json_obj_int(stream, "parallelDownloads", 4);
    cfg.prioritize_oldest = rs_json_obj_bool(stream, "prioritizeOldest", false) ? 1 : 0;
    cfg.playback_delay_seconds = (int)rs_json_obj_int(stream, "playbackDelaySeconds", 0);
    cfg.audio_delay_ms = (int)rs_json_obj_int(stream, "audioDelayMs", 0);
    cfg.poll_interval = rs_json_obj_num(stream, "pollInterval", 0);

    bool already = rs_live_is_running(s->live, stream_id);
    int rc = rs_live_start(s->live, stream_id, &cfg);
    if (rc != 0) {
        log_record(s, stream_id, "error", "liveStart", src, 0, -1,
                   "could not start the live DASH engine");
    } else if (!already) {
        log_recordf(s, stream_id, "info", "liveStart", src, 0, -1,
                    "engine starting: window %d, ahead %d, keep %d, delay %ds, "
                    "audio offset %dms, keys %s, rep %s, %lu CDN mirror%s",
                    cfg.playlist_segments, cfg.download_ahead, cfg.keep_segments,
                    cfg.playback_delay_seconds, cfg.audio_delay_ms,
                    cfg.decryption_keys[0] ? "set" : "none",
                    cfg.representation[0] ? cfg.representation : "auto",
                    (unsigned long)cfg.cdn_url_count,
                    cfg.cdn_url_count == 1 ? "" : "s");
    } else {
        log_record(s, stream_id, "info", "liveConfig", src, 0, -1,
                   "settings applied — polling options update on the next poll; "
                   "a changed connection count restarts the worker pool");
    }

    free(mproxy); free(dproxy); free(mheaders); free(dheaders);
    free((void *)mirror_urls);  // the strings belong to the state DOM
}

static void live_stop_stream(restream_server_t *s, const char *stream_id) {
    if (!s || !s->live || !stream_id) return;
    if (!rs_live_is_running(s->live, stream_id)) return;
    char *status = rs_live_status_line(s->live, stream_id);
    log_record(s, stream_id, "info", "liveStop", NULL, 0, -1,
               status ? status : "engine stopping");
    rs_free(status);
    rs_live_stop(s->live, stream_id);
}

// Starts engines for every stream that state.json says is already running, so a
// server restart resumes the streams instead of waiting for someone to click
// Start again.
static void live_sync_all(restream_server_t *s) {
    if (!s || !s->live) return;
    const rs_json *providers = rs_json_obj_get(s->state.root, "providers");
    int started = 0;
    for (size_t i = 0; i < rs_json_arr_len(providers); i++) {
        const rs_json *streams = rs_json_obj_get(rs_json_arr_at(providers, i), "streams");
        for (size_t j = 0; j < rs_json_arr_len(streams); j++) {
            const rs_json *st = rs_json_arr_at(streams, j);
            const char *id = rs_json_obj_str(st, "id", "");
            if (!id[0]) continue;
            if (strcmp(rs_json_obj_str(st, "status", "stopped"), "running") != 0) continue;
            live_sync_stream(s, id);
            if (rs_live_is_running(s->live, id)) started++;
        }
    }
    if (started > 0)
        log_recordf(s, "__panel__", "info", "serverStart", NULL, 0, -1,
                    "resuming %d stream%s that were running before the restart",
                    started, started == 1 ? "" : "s");
}

// Serves the DASH master playlist. The engine renders it in the background and
// only publishes it once every rendition it points at can actually answer, so
// a player never gets a master whose variant playlists 404.
static void serve_dash_master(restream_server_t *server, struct mg_connection *c,
                              struct mg_http_message *hm, const rs_json *stream) {
    (void)hm;
    const char *stream_id = rs_json_obj_str(stream, "id", "");
    if (!server->live) {
        reply_error(c, 501, "DASH playback needs the threaded live engine, which this build "
                            "does not have. Use the Swift binary, or an HLS (.m3u8) source.");
        return;
    }

    // Normally a no-op: the engine was started when the stream was started.
    // It matters when a stream was started before this code was deployed, or
    // when the engine was reaped after an earlier stop.
    live_sync_stream(server, stream_id);

    char *master = rs_live_master_playlist(server->live, stream_id);
    if (!master) {
        // Answer immediately; never wait for the engine here.
        //
        // This used to block the event loop for up to six seconds on a cold
        // start. mongoose is single-threaded, so that is not a wait on *this*
        // stream — it is the entire server stopped, every other stream's
        // playlist reloads and segment reads included. A stream whose origin
        // never answers (geo-blocked, dead URL) is never ready, so every
        // request for it stalled everything for the full timeout, and healthy
        // streams showed up in players as playlist and segment timeouts.
        //
        // 503 + Retry-After is what a warming stream should say anyway: every
        // player retries a manifest, so a cold start costs one retry instead
        // of a server-wide freeze.
        log_record(server, stream_id, "info", "masterCold", NULL, 0, -1,
                   "engine still warming up — 503 so the player retries");
    }
    if (!master) {
        char *status = rs_live_status_line(server->live, stream_id);
        log_record(server, stream_id, "error", "master", NULL, 503, -1,
                   status ? status : "engine not ready yet");
        rs_free(status);
        mg_http_reply(c, 503, "Content-Type: text/plain\r\nRetry-After: 2\r\n"
                              "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n",
                      "The stream is still starting up. Retry shortly.\n");
        return;
    }

    log_record(server, stream_id, "info", "master", NULL, 200,
               (long long)strlen(master), "master playlist served");
    mg_http_reply(c, 200, "Content-Type: application/vnd.apple.mpegurl; charset=utf-8\r\n"
                          "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n",
                  "%s", master);
    rs_free(master);
}

// Serves one representation's media playlist. This is the hot path — a live
// player reloads it every target-duration seconds — so it does nothing but copy
// the string the representation's worker already rendered.
static void serve_dash_media(restream_server_t *server, struct mg_connection *c,
                             struct mg_http_message *hm, const rs_json *stream, const char *rep) {
    (void)hm;
    const char *stream_id = rs_json_obj_str(stream, "id", "");
    if (!server->live) {
        reply_error(c, 501, "DASH playback needs the threaded live engine, which this build "
                            "does not have. Use the Swift binary, or an HLS (.m3u8) source.");
        return;
    }

    char *playlist = rs_live_media_playlist(server->live, stream_id, rep);
    if (!playlist) {
        char *status = rs_live_status_line(server->live, stream_id);
        log_recordf(server, stream_id, "error", "playlist", NULL, 503, -1,
                    "%s: no window yet (%s)", rep, status ? status : "engine not running");
        rs_free(status);
        mg_http_reply(c, 503, "Content-Type: text/plain\r\nRetry-After: 1\r\n"
                              "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n",
                      "The stream is still starting up. Retry shortly.\n");
        return;
    }

    log_recordf(server, stream_id, "info", "playlist", NULL, 200, (long long)strlen(playlist),
                "%s: media playlist served", rep);
    mg_http_reply(c, 200, "Content-Type: application/vnd.apple.mpegurl; charset=utf-8\r\n"
                          "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n",
                  "%s", playlist);
    rs_free(playlist);
}

// --- direct links -----------------------------------------------------------
//
// Two shapes of "no playlist, one connection, bytes keep coming":
//
//   /direct/<id>[/<rep>]     one rendition as a fragmented-MP4 byte tail
//   /direct/<id>[/<rep>].ts  video and audio muxed together as MPEG-TS
//
// Both exist because HLS costs a round trip per segment per rendition plus a
// playlist reload to learn the segment even exists. A direct link pays that
// once, at connect. The fMP4 tail is the cheaper of the two (the bytes are
// already in the engine's queue, so serving is a memcpy) but carries one
// rendition, so a player gets either picture or sound. The .ts variant runs
// both renditions through rs_mpegts and hands over a single conventional
// stream, which is what a player pointed at a raw URL actually expects.
//
// Neither is served from a worker thread: the pump runs on the event loop's own
// timer, and every byte it moves is already in memory.

#define RS_DIRECT_MARKER 'D'   // fMP4 byte-tail viewer
#define RS_TSMUX_MARKER  'T'   // muxed MPEG-TS viewer

// How often the pump moves new segments to viewers. Segments arrive every few
// seconds, so this only bounds the added latency, not the throughput.
#define RS_DIRECT_PUMP_MS 250

// Per-viewer socket buffer ceiling. A viewer whose link is slower than the
// stream's bitrate would otherwise grow mongoose's send buffer until the
// process runs out of memory. At the cap the pump stops feeding it: an fMP4
// tail is dropped (its stream can't survive a hole), while a TS viewer is
// re-seated at the live edge on the next tick, which is exactly the resync a
// TS decoder is built for.
#define RS_DIRECT_SEND_CAP (8u * 1024u * 1024u)

// Segments handed to one viewer (or one mux) per tick. A viewer that just
// connected catches up over a few ticks instead of memcpying the whole window
// into the send buffer at once.
#define RS_DIRECT_BURST 8

// How far back from the live edge a new viewer starts. The engine holds
// keepSegments (typically 10-60) of history, and starting at the oldest of them
// would hand someone who just connected a minute of stale media to play through
// before reaching live — the exact latency a direct link exists to avoid. Two
// segments back is enough to open on a keyframe and to have something to send
// immediately, without the backlog.
#define RS_DIRECT_PREROLL 2

// The sequence a viewer should start after, to begin RS_DIRECT_PREROLL segments
// behind the live edge. -1 ("everything held") when the queue is still empty.
static long long direct_start_seq(const rs_live_rep_desc *rep) {
    if (!rep->held) return -1;
    long long start = rep->newest_seq - RS_DIRECT_PREROLL;
    if (start < rep->oldest_seq) start = rep->oldest_seq;
    return start - 1;  // take_after is exclusive
}

// One fMP4 byte-tail viewer.
typedef struct rs_direct_client {
    struct rs_direct_client *next;
    unsigned long conn_id;
    char *stream_id;
    int rep_index;
    bool sent_init;
    long long last_seq;
    char *identity, *ip, *ua;
} rs_direct_client;

// The muxed output for one stream, shared by every .ts viewer of it. Muxing
// once and fanning out is what keeps a second viewer free; it also means every
// viewer sees the same byte stream, so a join point is a join point for all.
#define RS_TS_RING_CAP   (8u * 1024u * 1024u)
#define RS_TS_MAX_JOINS  128

// Pumps a rendition may produce nothing for before the mux stops waiting on it.
// Comfortably longer than a segment duration plus a slow poll, so an ordinary
// hiccup never trips it.
#define RS_TS_STALL_TICKS 60   // 15 s at RS_DIRECT_PUMP_MS

// How long a mux outlives its last viewer. Tearing it down the instant someone
// disconnects means the next viewer — a player reconnecting after a hiccup, or
// simply the next person to open the link — pays for a fresh bind, preroll and
// track alignment before a single byte arrives, which can take longer than a
// client's own timeout. Lingering keeps a warm ring for the reconnect, at the
// cost of muxing a few more seconds of a stream nobody is watching.
#define RS_TS_LINGER_TICKS 40  // 10 s at RS_DIRECT_PUMP_MS

typedef struct rs_ts_session {
    struct rs_ts_session *next;
    char *stream_id;
    rs_ts_mux *mux;
    bool bound;             // every rendition's init has been handed to the mux
    size_t ntracks;
    struct {
        int rep_index;
        int track;          // rs_mpegts handle
        long long last_seq;
        int idle_ticks;     // pumps in a row that produced nothing
        bool ended;         // given up on; no longer gates the other track
    } tracks[RS_TS_MAX_TRACKS];

    // Recent output, as a byte window with absolute positions so a viewer's
    // cursor stays meaningful after the oldest bytes are dropped.
    uint8_t *ring;
    size_t ring_len;
    uint64_t ring_base;     // absolute position of ring[0]
    uint64_t joins[RS_TS_MAX_JOINS];
    size_t njoins;

    int viewers;
    int idle_pumps;   // consecutive pumps with no viewer; see RS_TS_LINGER_TICKS
} rs_ts_session;

// One muxed-TS viewer. `pos` is an absolute position in its session's stream.
typedef struct rs_ts_client {
    struct rs_ts_client *next;
    unsigned long conn_id;
    char *stream_id;
    uint64_t pos;
    bool seated;            // has been placed at a join point
    char *identity, *ip, *ua;
} rs_ts_client;

static struct mg_connection *conn_by_id(restream_server_t *s, unsigned long id) {
    for (struct mg_connection *c = s->mgr.conns; c != NULL; c = c->next)
        if (c->id == id) return c;
    return NULL;
}

static void direct_client_free(rs_direct_client *d) {
    if (!d) return;
    rs_free(d->stream_id);
    rs_free(d->identity);
    rs_free(d->ip);
    rs_free(d->ua);
    free(d);
}

static void ts_client_free(rs_ts_client *t) {
    if (!t) return;
    rs_free(t->stream_id);
    rs_free(t->identity);
    rs_free(t->ip);
    rs_free(t->ua);
    free(t);
}

static void ts_session_free(rs_ts_session *s) {
    if (!s) return;
    rs_ts_mux_destroy(s->mux);
    rs_free(s->stream_id);
    free(s->ring);
    free(s);
}

// Drops every viewer registration belonging to a closed connection. Called from
// MG_EV_CLOSE, so a viewer that just walks away is reaped immediately rather
// than on the next failed send.
static void direct_forget_conn(restream_server_t *s, unsigned long conn_id) {
    for (rs_direct_client **pp = &s->direct_head; *pp;) {
        if ((*pp)->conn_id == conn_id) {
            rs_direct_client *dead = *pp;
            *pp = dead->next;
            direct_client_free(dead);
        } else {
            pp = &(*pp)->next;
        }
    }
    for (rs_ts_client **pp = &s->ts_head; *pp;) {
        if ((*pp)->conn_id == conn_id) {
            rs_ts_client *dead = *pp;
            *pp = dead->next;
            for (rs_ts_session *sess = s->ts_sessions; sess; sess = sess->next)
                if (strcmp(sess->stream_id, dead->stream_id) == 0 && sess->viewers > 0)
                    sess->viewers--;
            ts_client_free(dead);
        } else {
            pp = &(*pp)->next;
        }
    }
}

static rs_ts_session *ts_session_find(restream_server_t *s, const char *stream_id) {
    for (rs_ts_session *sess = s->ts_sessions; sess; sess = sess->next)
        if (strcmp(sess->stream_id, stream_id) == 0) return sess;
    return NULL;
}

static rs_ts_session *ts_session_open(restream_server_t *s, const char *stream_id) {
    rs_ts_session *sess = ts_session_find(s, stream_id);
    if (sess) return sess;
    sess = (rs_ts_session *)calloc(1, sizeof(*sess));
    if (!sess) return NULL;
    sess->mux = rs_ts_mux_create();
    sess->ring = (uint8_t *)malloc(RS_TS_RING_CAP);
    sess->stream_id = rs_strdup(stream_id);
    if (!sess->mux || !sess->ring || !sess->stream_id) {
        ts_session_free(sess);
        return NULL;
    }
    sess->next = s->ts_sessions;
    s->ts_sessions = sess;
    return sess;
}

static void ts_session_close(restream_server_t *s, const char *stream_id) {
    for (rs_ts_session **pp = &s->ts_sessions; *pp;) {
        if (strcmp((*pp)->stream_id, stream_id) == 0) {
            rs_ts_session *dead = *pp;
            *pp = dead->next;
            ts_session_free(dead);
        } else {
            pp = &(*pp)->next;
        }
    }
}

// Binds the mux to the stream's renditions once every one of them has an init
// segment. Partial binding is deliberately not allowed: a mux that started with
// video only would have already emitted a PMT without the audio PID, and no
// player re-reads a PMT it has already accepted.
static void ts_session_bind(restream_server_t *server, rs_ts_session *sess) {
    if (sess->bound) return;
    rs_live_rep_desc *all = NULL;
    size_t total = rs_live_reps(server->live, sess->stream_id, &all);
    if (!total) return;

    // Subtitle renditions are moved out of the way before anything else looks
    // at them. This muxer emits video and audio PES only, so a text track could
    // never be carried — and a sidecar WebVTT rendition has no initialization
    // segment at all, which would leave the completeness check below
    // permanently false and stop the .ts link ever binding for a stream that
    // merely has subtitles.
    //
    // Swapped rather than overwritten so every allocation stays in the array
    // exactly once and the whole of it is still released below.
    rs_live_rep_desc *reps = all;
    size_t n = 0;
    for (size_t i = 0; i < total; i++) {
        if (strcmp(all[i].kind, "text") == 0) continue;
        if (n != i) {
            rs_live_rep_desc tmp = reps[n];
            reps[n] = reps[i];
            reps[i] = tmp;
        }
        n++;
    }
    if (!n) { rs_live_reps_free(all, total); return; }
    if (n > RS_TS_MAX_TRACKS) n = RS_TS_MAX_TRACKS;

    // Every init must be in hand before the first one is registered, so a
    // half-bound mux is never left behind on the early return.
    uint8_t *inits[RS_TS_MAX_TRACKS] = {0};
    size_t lens[RS_TS_MAX_TRACKS] = {0};
    bool complete = true;
    for (size_t i = 0; i < n; i++) {
        inits[i] = rs_live_take_indexed(server->live, sess->stream_id, reps[i].index,
                                        0, true, &lens[i]);
        if (!inits[i] || !lens[i]) complete = false;
    }
    if (complete) {
        bool have_video = false, have_audio = false;
        for (size_t i = 0; i < n; i++) {
            // One video and one audio. A stream configured with several video
            // renditions is offering a player a bitrate choice, which is an HLS
            // idea — a single TS stream has no way to express it, and a player
            // handed two video PIDs picks one arbitrarily. The first is the one
            // the master playlist leads with.
            bool is_audio = strcmp(reps[i].kind, "audio") == 0;
            if (is_audio ? have_audio : have_video) continue;

            int track = rs_ts_mux_add_track(sess->mux, reps[i].kind, inits[i], lens[i]);
            if (track < 0) {
                log_recordf(server, sess->stream_id, "error", "tsMuxTrack", NULL, 0, -1,
                            "rendition %s (%s) has a codec the TS muxer can't carry — "
                            "the .ts direct link will leave it out",
                            reps[i].rep_id, reps[i].kind);
                continue;
            }
            if (is_audio) have_audio = true; else have_video = true;
            sess->tracks[sess->ntracks].rep_index = reps[i].index;
            sess->tracks[sess->ntracks].track = track;
            sess->tracks[sess->ntracks].last_seq = direct_start_seq(&reps[i]);
            sess->ntracks++;
        }
        sess->bound = sess->ntracks > 0;
        if (sess->bound)
            log_recordf(server, sess->stream_id, "info", "tsMuxReady", NULL, 0, -1,
                        "muxing %lu renditions into MPEG-TS", (unsigned long)sess->ntracks);
    }
    for (size_t i = 0; i < n; i++) rs_free(inits[i]);
    // `total`, not `n`: `n` skips the text renditions and is clamped to the
    // muxer's track limit, and freeing by it would leak everything past it.
    rs_live_reps_free(all, total);
}

// Appends muxed output to the ring, dropping the oldest bytes when it is full.
static void ts_ring_append(rs_ts_session *sess, const uint8_t *data, size_t len,
                           const size_t *joins, size_t njoins) {
    uint64_t append_pos = sess->ring_base + sess->ring_len;
    for (size_t i = 0; i < njoins; i++) {
        // Newest wins: a full array must drop its oldest entry rather than
        // refuse the new one, or the "newest join point" a viewer is seated at
        // would freeze at whatever was recorded first and strand every later
        // viewer that far behind the live edge.
        if (sess->njoins == RS_TS_MAX_JOINS) {
            memmove(sess->joins, sess->joins + 1, (RS_TS_MAX_JOINS - 1) * sizeof(*sess->joins));
            sess->njoins--;
        }
        sess->joins[sess->njoins++] = append_pos + joins[i];
    }

    if (len >= RS_TS_RING_CAP) {
        // A single take larger than the whole ring: keep only its tail.
        memcpy(sess->ring, data + (len - RS_TS_RING_CAP), RS_TS_RING_CAP);
        sess->ring_base = append_pos + (len - RS_TS_RING_CAP);
        sess->ring_len = RS_TS_RING_CAP;
    } else {
        if (sess->ring_len + len > RS_TS_RING_CAP) {
            // Drop a quarter of the ring at minimum, so this memmove happens
            // once in a while rather than on every append.
            size_t drop = (sess->ring_len + len) - RS_TS_RING_CAP;
            size_t slack = RS_TS_RING_CAP / 4;
            if (drop < slack) drop = slack;
            if (drop > sess->ring_len) drop = sess->ring_len;
            memmove(sess->ring, sess->ring + drop, sess->ring_len - drop);
            sess->ring_len -= drop;
            sess->ring_base += drop;
        }
        memcpy(sess->ring + sess->ring_len, data, len);
        sess->ring_len += len;
    }

    // Forget join points that have scrolled out of the window.
    size_t keep = 0;
    for (size_t i = 0; i < sess->njoins; i++)
        if (sess->joins[i] >= sess->ring_base) sess->joins[keep++] = sess->joins[i];
    sess->njoins = keep;
}

// The newest position a viewer can start decoding from, or the live edge when
// no keyframe has been seen yet.
static uint64_t ts_newest_join(const rs_ts_session *sess) {
    if (sess->njoins) return sess->joins[sess->njoins - 1];
    return sess->ring_base + sess->ring_len;
}

// A track cannot be added back to an MPEG-TS mux after it has been ended: its
// PID and codec belong in the PMT emitted at mux creation. Rebuild the shared
// mux when a stalled rendition starts producing again, and re-seat every
// viewer on the first keyframe from the rebuilt stream.
static bool ts_session_rebuild(restream_server_t *server, rs_ts_session *sess,
                               int recovered_rep) {
    rs_ts_mux *replacement = rs_ts_mux_create();
    if (!replacement) return false;

    rs_ts_mux_destroy(sess->mux);
    sess->mux = replacement;
    sess->bound = false;
    sess->ntracks = 0;
    sess->ring_base += sess->ring_len;
    sess->ring_len = 0;
    sess->njoins = 0;

    for (rs_ts_client *client = server->ts_head; client; client = client->next)
        if (strcmp(client->stream_id, sess->stream_id) == 0) client->seated = false;

    log_recordf(server, sess->stream_id, "info", "tsMuxRecovered", NULL, 0, -1,
                "rendition %d resumed — rebuilding the MPEG-TS mux with all tracks",
                recovered_rep);
    return true;
}

// Pulls whatever the live engine has produced since last tick into the mux, and
// pushes the muxed result into the ring.
static void ts_session_pump(restream_server_t *server, rs_ts_session *sess) {
    ts_session_bind(server, sess);
    if (!sess->bound) return;

    // rs_ts_mux_end_track permanently removes a stalled track from the current
    // mux. Probe ended tracks before pushing anything; if one has new media,
    // rebuild so the recovered audio/video is not silently discarded forever.
    for (size_t i = 0; i < sess->ntracks; i++) {
        if (!sess->tracks[i].ended) continue;
        size_t len = 0;
        long long seq = 0;
        uint8_t *seg = rs_live_take_after(server->live, sess->stream_id,
                                          sess->tracks[i].rep_index,
                                          sess->tracks[i].last_seq, &seq, NULL, &len);
        bool resumed = seg != NULL;
        rs_free(seg);
        if (!resumed) continue;
        int recovered_rep = sess->tracks[i].rep_index;
        if (!ts_session_rebuild(server, sess, recovered_rep)) return;
        ts_session_bind(server, sess);
        if (!sess->bound) return;
        break;
    }

    for (size_t i = 0; i < sess->ntracks; i++) {
        int taken = 0;
        for (int n = 0; n < RS_DIRECT_BURST; n++) {
            size_t len = 0;
            long long seq = 0;
            uint8_t *seg = rs_live_take_after(server->live, sess->stream_id,
                                              sess->tracks[i].rep_index,
                                              sess->tracks[i].last_seq, &seq, NULL, &len);
            if (!seg) break;
            rs_ts_mux_push(sess->mux, sess->tracks[i].track, seg, len);
            sess->tracks[i].last_seq = seq;
            rs_free(seg);
            taken++;
        }
        // Interleaving holds a sample back until every other rendition has
        // delivered something at least as late, so one rendition whose worker
        // has died would otherwise stop the whole stream rather than just its
        // own half of it. Past the stall window, stop waiting on it.
        if (taken) {
            sess->tracks[i].idle_ticks = 0;
        } else if (!sess->tracks[i].ended &&
                   ++sess->tracks[i].idle_ticks > RS_TS_STALL_TICKS) {
            sess->tracks[i].ended = true;
            rs_ts_mux_end_track(sess->mux, sess->tracks[i].track);
            log_recordf(server, sess->stream_id, "error", "tsMuxStalled", NULL, 0, -1,
                        "rendition %d produced nothing for %ds — muxing the rest without it",
                        sess->tracks[i].rep_index, RS_TS_STALL_TICKS * RS_DIRECT_PUMP_MS / 1000);
        }
    }

    size_t out_len = 0;
    uint8_t *out = rs_ts_mux_take(sess->mux, false, &out_len);
    if (out && out_len) {
        const size_t *joins = NULL;
        size_t njoins = rs_ts_mux_join_points(sess->mux, &joins);
        ts_ring_append(sess, out, out_len, joins, njoins);
    }
    rs_free(out);
}

// Moves bytes to one fMP4 tail viewer. Returns false if the viewer should be
// dropped.
static bool direct_client_pump(restream_server_t *server, rs_direct_client *d,
                               struct mg_connection *c) {
    if (c->send.len >= RS_DIRECT_SEND_CAP) {
        log_recordf(server, d->stream_id, "error", "directSlow", NULL, 0,
                    (long long)c->send.len,
                    "%s can't keep up with the stream — dropping the direct link", d->ip);
        return false;
    }
    size_t sent = 0;
    if (!d->sent_init) {
        size_t len = 0;
        uint8_t *init = rs_live_take_indexed(server->live, d->stream_id, d->rep_index,
                                             0, true, &len);
        if (!init) return true;  // engine hasn't produced one yet; try next tick
        mg_send(c, init, len);
        sent += len;
        rs_free(init);
        d->sent_init = true;
    }
    for (int n = 0; n < RS_DIRECT_BURST; n++) {
        size_t len = 0;
        long long seq = 0;
        uint8_t *seg = rs_live_take_after(server->live, d->stream_id, d->rep_index,
                                          d->last_seq, &seq, NULL, &len);
        if (!seg) break;
        mg_send(c, seg, len);
        sent += len;
        d->last_seq = seq;
        rs_free(seg);
    }
    if (sent)
        rs_metrics_record(server->metrics, d->stream_id, d->identity, d->ip, d->ua, (int)sent);
    return true;
}

// Moves bytes to one muxed-TS viewer.
static void ts_client_pump(restream_server_t *server, rs_ts_client *t,
                           rs_ts_session *sess, struct mg_connection *c) {
    // A viewer that has fallen out of the window (or has just arrived) is placed
    // at the newest keyframe: a TS decoder needs one to start, and the bytes it
    // missed are gone anyway.
    if (!t->seated || t->pos < sess->ring_base) {
        uint64_t seat = ts_newest_join(sess);
        if (t->seated)
            log_recordf(server, t->stream_id, "info", "tsResync", NULL, 0, -1,
                        "%s fell behind the buffer — resuming at the live edge", t->ip);
        t->pos = seat;
        t->seated = true;
    }
    if (c->send.len >= RS_DIRECT_SEND_CAP) return;  // let it drain; resync if it can't

    uint64_t head = sess->ring_base + sess->ring_len;
    if (t->pos >= head) return;
    size_t offset = (size_t)(t->pos - sess->ring_base);
    size_t avail = sess->ring_len - offset;
    mg_send(c, sess->ring + offset, avail);
    t->pos += avail;
    rs_metrics_record(server->metrics, t->stream_id, t->identity, t->ip, t->ua, (int)avail);
}

// The direct-link timer. Runs the muxes, then feeds every viewer.
static void pump_direct_links(void *arg) {
    restream_server_t *s = (restream_server_t *)arg;
    if (!s->direct_head && !s->ts_head && !s->ts_sessions) return;

    // A mux whose last viewer left, or whose stream stopped, is torn down —
    // otherwise it would keep draining the engine's queue forever.
    for (rs_ts_session *sess = s->ts_sessions; sess;) {
        rs_ts_session *next = sess->next;
        if (!rs_live_is_running(s->live, sess->stream_id)) {
            ts_session_close(s, sess->stream_id);
        } else if (sess->viewers > 0) {
            sess->idle_pumps = 0;
        } else if (++sess->idle_pumps > RS_TS_LINGER_TICKS) {
            ts_session_close(s, sess->stream_id);
        }
        sess = next;
    }
    for (rs_ts_session *sess = s->ts_sessions; sess; sess = sess->next)
        ts_session_pump(s, sess);

    for (rs_direct_client **pp = &s->direct_head; *pp;) {
        rs_direct_client *d = *pp;
        struct mg_connection *c = conn_by_id(s, d->conn_id);
        if (!c || !direct_client_pump(s, d, c)) {
            *pp = d->next;
            if (c) c->is_closing = 1;
            direct_client_free(d);
            continue;
        }
        pp = &d->next;
    }
    for (rs_ts_client **pp = &s->ts_head; *pp;) {
        rs_ts_client *t = *pp;
        struct mg_connection *c = conn_by_id(s, t->conn_id);
        rs_ts_session *sess = ts_session_find(s, t->stream_id);
        if (!c || !sess) {
            *pp = t->next;
            if (c) c->is_closing = 1;
            if (sess && sess->viewers > 0) sess->viewers--;
            ts_client_free(t);
            continue;
        }
        ts_client_pump(s, t, sess, c);
        pp = &t->next;
    }
}

// Resolves the rendition a /direct or /download path names. An empty name means
// "the stream's first rendition", which is what a single-rendition stream's URL
// carries. Returns -1 when the name matches nothing; `*out_start` receives the
// sequence to tail from (see direct_start_seq) and may be NULL.
static int direct_rep_index(restream_server_t *server, const char *stream_id,
                            const char *rep, long long *out_start) {
    rs_live_rep_desc *reps = NULL;
    size_t n = rs_live_reps(server->live, stream_id, &reps);
    int index = -1;
    size_t found = 0;
    if (n) {
        if (!rep || !rep[0]) {
            index = reps[0].index;
        } else {
            for (size_t i = 0; i < n; i++)
                if (strcmp(reps[i].rep_id, rep) == 0) { index = reps[i].index; found = i; break; }
            // The panel labels a single-rendition stream's link with the
            // representation id from the config, which the engine may not have
            // adopted verbatim; fall back rather than 404 on a link it built.
            if (index < 0 && n == 1) index = reps[0].index;
        }
    }
    if (out_start) *out_start = (index >= 0) ? direct_start_seq(&reps[found]) : -1;
    rs_live_reps_free(reps, n);
    return index;
}

// GET /direct/<id>[/<rep>] — the fMP4 byte tail.
static void serve_direct(restream_server_t *server, struct mg_connection *c,
                         struct mg_http_message *hm, const char *stream_id,
                         const char *rep) {
    long long start_seq = -1;
    int rep_index = direct_rep_index(server, stream_id, rep, &start_seq);
    if (rep_index < 0) {
        reply_error(c, 404, "Stream isn't producing that rendition yet — start it first.");
        return;
    }
    rs_direct_client *d = (rs_direct_client *)calloc(1, sizeof(*d));
    if (!d) { reply_error(c, 500, "Out of memory."); return; }
    char *ip = client_ip(server, c, hm);
    char *ua = header_dup(hm, "User-Agent");
    char *key = playback_key(hm);
    d->conn_id = c->id;
    d->stream_id = rs_strdup(stream_id);
    d->rep_index = rep_index;
    d->last_seq = start_seq;
    d->ip = ip ? ip : rs_strdup("");
    d->ua = ua ? ua : rs_strdup("");
    d->identity = (key && key[0]) ? key : rs_strdup(d->ip);
    if (key && !key[0]) free(key);

    // Framed by the connection closing, not by a length — the same contract the
    // Swift build served and the one every player that takes a raw URL expects.
    mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\n"
                 "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n"
                 "Connection: close\r\n\r\n");
    c->data[0] = RS_DIRECT_MARKER;
    d->next = server->direct_head;
    server->direct_head = d;
    log_recordf(server, stream_id, "info", "directOpen", NULL, 200, -1,
                "fMP4 direct link opened by %s (rendition %d)", d->ip, rep_index);
    direct_client_pump(server, d, c);
}

// GET /direct/<id>[/<rep>].ts — video and audio muxed into one MPEG-TS stream.
static void serve_direct_ts(restream_server_t *server, struct mg_connection *c,
                            struct mg_http_message *hm, const char *stream_id) {
    rs_ts_session *sess = ts_session_open(server, stream_id);
    if (!sess) { reply_error(c, 500, "Out of memory."); return; }
    rs_ts_client *t = (rs_ts_client *)calloc(1, sizeof(*t));
    if (!t) { reply_error(c, 500, "Out of memory."); return; }
    char *ip = client_ip(server, c, hm);
    char *ua = header_dup(hm, "User-Agent");
    char *key = playback_key(hm);
    t->conn_id = c->id;
    t->stream_id = rs_strdup(stream_id);
    t->ip = ip ? ip : rs_strdup("");
    t->ua = ua ? ua : rs_strdup("");
    t->identity = (key && key[0]) ? key : rs_strdup(t->ip);
    if (key && !key[0]) free(key);

    mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\n"
                 "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n"
                 "Connection: close\r\n\r\n");
    c->data[0] = RS_TSMUX_MARKER;
    t->next = server->ts_head;
    server->ts_head = t;
    sess->viewers++;
    log_recordf(server, stream_id, "info", "tsOpen", NULL, 200, -1,
                "MPEG-TS direct link opened by %s (%d viewer%s on this mux)",
                t->ip, sess->viewers, sess->viewers == 1 ? "" : "s");
    // First bytes go out on the next tick, once the mux has bound its tracks.
}

// GET /download/<id>[/<rep>][.mp4] — everything currently held for a rendition,
// as one fragmented MP4. The init segment followed by its fragments is already
// a valid, playable file (each fragment is a self-contained moof+mdat), so this
// is a concatenation, not a remux.
static void serve_download(restream_server_t *server, struct mg_connection *c,
                           struct mg_http_message *hm, const char *stream_id,
                           const char *rep) {
    // A download wants the whole buffered window, not the live edge, so it
    // deliberately does not take direct_rep_index's start hint.
    int rep_index = direct_rep_index(server, stream_id, rep, NULL);
    if (rep_index < 0) {
        reply_error(c, 404, "Stream isn't producing that rendition yet — start it first.");
        return;
    }
    size_t cap = 0, len = 0;
    uint8_t *body = NULL;
    size_t init_len = 0;
    uint8_t *init = rs_live_take_indexed(server->live, stream_id, rep_index, 0, true, &init_len);
    if (!init) {
        reply_error(c, 404, "Stream isn't running yet — start it and let a few segments arrive.");
        return;
    }
    cap = init_len + 1;
    body = (uint8_t *)malloc(cap);
    if (body) { memcpy(body, init, init_len); len = init_len; }
    rs_free(init);
    if (!body) { reply_error(c, 500, "Out of memory."); return; }

    long long seq = -1;
    for (;;) {
        size_t slen = 0;
        long long got = 0;
        uint8_t *seg = rs_live_take_after(server->live, stream_id, rep_index, seq, &got, NULL, &slen);
        if (!seg) break;
        if (len + slen > cap) {
            size_t want = cap * 2;
            while (want < len + slen) want *= 2;
            uint8_t *nb = (uint8_t *)realloc(body, want);
            if (!nb) { rs_free(seg); break; }
            body = nb;
            cap = want;
        }
        memcpy(body + len, seg, slen);
        len += slen;
        seq = got;
        rs_free(seg);
    }

    const rs_json *stream = rs_panel_find_stream(&server->state, stream_id);
    const char *name = stream ? rs_json_obj_str(stream, "name", "stream") : "stream";
    char filename[160];
    size_t fi = 0;
    for (size_t i = 0; name[i] && fi + 1 < sizeof(filename); i++) {
        unsigned char ch = (unsigned char)name[i];
        filename[fi++] = (isalnum(ch) || ch == '.' || ch == '_' || ch == '-') ? (char)ch : '_';
    }
    filename[fi] = '\0';
    if (!fi) snprintf(filename, sizeof(filename), "stream");

    char *ip = client_ip(server, c, hm);
    log_recordf(server, stream_id, "info", "download", NULL, 200, (long long)len,
                "rendition %d downloaded by %s", rep_index, ip);
    rs_free(ip);
    mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nContent-Length: %lu\r\n"
                 "Content-Disposition: attachment; filename=\"%s.mp4\"\r\n"
                 "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n\r\n",
              (unsigned long)len, filename);
    mg_send(c, body, len);
    c->is_resp = 0;
    free(body);
}

// Playback routes. Direct-source streams redirect; HLS passthrough and DASH
// (on-demand MPD->HLS translation) are proxied live. ffmpeg modes still 501.
static bool handle_playback(restream_server_t *server, struct mg_connection *c,
                            struct mg_http_message *hm) {
    bool is_source = mg_match(hm->uri, mg_str("/source/*"), NULL);
    bool is_play = mg_match(hm->uri, mg_str("/play/#"), NULL);
    bool is_restream = mg_match(hm->uri, mg_str("/restream/#"), NULL);
    bool is_direct = mg_match(hm->uri, mg_str("/direct/#"), NULL);
    bool is_download = mg_match(hm->uri, mg_str("/download/#"), NULL);
    if (!is_source && !is_play && !is_restream && !is_direct && !is_download) return false;

    // Playback auth: an API key is required only once at least one exists.
    char *key = playback_key(hm);
    bool allowed = rs_panel_playback_allowed(&server->state, key);
    if (!allowed) {
        char *ip = client_ip(server, c, hm);
        log_recordf(server, "__panel__", "error", "playbackDenied", NULL, 401, -1,
                    "%.*s from %s rejected: %s", (int)hm->uri.len, hm->uri.buf, ip,
                    (key && key[0]) ? "invalid playback key" : "no playback key");
        rs_free(ip);
        free(key);
        reply_error(c, 401, "A valid playback key is required.");
        return true;
    }
    free(key);

    if (is_restream) {
        if (!g_fetch_handler) { reply_error(c, 501, "Restreaming isn't in this build."); return true; }
        struct mg_str rcaps[2];
        char *id = NULL, *fname = NULL;
        if (mg_match(hm->uri, mg_str("/restream/*/*"), rcaps)) {
            id = capture_dup(rcaps[0]);
            fname = capture_dup(rcaps[1]);
        }
        if (id && fname) serve_restream_item(server, c, hm, id, fname);
        else reply_error(c, 400, "Bad restream path.");
        free(id); free(fname);
        return true;
    }

    // /direct/<id>[/<rep>] and /download/<id>[/<rep>], with an optional
    // extension: ".ts" selects the muxed MPEG-TS stream, anything else (".mp4",
    // or nothing) the raw fragmented-MP4 of a single rendition.
    if (is_direct || is_download) {
        char *tail = capture(hm, is_direct ? "/direct/#" : "/download/#");
        if (!tail || !tail[0]) {
            free(tail);
            reply_error(c, 400, "Bad direct-link path.");
            return true;
        }
        char *slash = strchr(tail, '/');
        char *rep = NULL;
        if (slash) { *slash = '\0'; rep = slash + 1; }

        // The extension rides on whichever component is last.
        char *last = rep ? rep : tail;
        bool want_ts = false;
        char *dot = strrchr(last, '.');
        if (dot) {
            want_ts = strcmp(dot, ".ts") == 0;
            *dot = '\0';
        }

        const rs_json *stream = rs_panel_find_stream(&server->state, tail);
        if (!stream) { free(tail); reply_error(c, 404, "Stream not found."); return true; }
        const char *sid = rs_json_obj_str(stream, "id", tail);
        if (strcmp(rs_json_obj_str(stream, "status", "stopped"), "running") != 0) {
            free(tail);
            reply_error(c, 404, "Stream is stopped — start it first.");
            return true;
        }
        if (is_download) serve_download(server, c, hm, sid, rep);
        else if (want_ts) serve_direct_ts(server, c, hm, sid);
        else serve_direct(server, c, hm, sid, rep);
        free(tail);
        return true;
    }

    if (is_source) {
        char *id = capture(hm, "/source/*");
        const rs_json *stream = rs_panel_find_stream(&server->state, id);
        free(id);
        if (!stream) { reply_error(c, 404, "Stream not found."); return true; }
        const char *target = stream_source_target(stream);
        if (!target[0]) { reply_error(c, 400, "Stream has no source URL."); return true; }
        send_redirect(c, target);
        return true;
    }

    // /play/<segment>/index.m3u8 or index.mpd
    bool is_m3u8 = mg_match(hm->uri, mg_str("/play/*/index.m3u8"), NULL);
    bool is_mpd = mg_match(hm->uri, mg_str("/play/*/index.mpd"), NULL);
    if (is_m3u8 || is_mpd) {
        char *segment = capture(hm, "/play/*/#");
        const rs_json *stream = segment ? rs_panel_find_stream(&server->state, segment) : NULL;
        free(segment);
        if (!stream) { reply_error(c, 404, "Stream not found."); return true; }
        const char *st_val = rs_json_obj_str(stream, "status", "stopped");
        if (strcmp(st_val, "running") != 0 && !rs_json_obj_bool(stream, "directSource", false)) {
            reply_error(c, 404, "Stream is stopped.");
            return true;
        }

        if (rs_json_obj_bool(stream, "directSource", false)) {
            const char *target = stream_source_target(stream);
            if (!target[0]) { reply_error(c, 400, "Stream has no source URL."); return true; }
            send_redirect(c, target);
            return true;
        }

        const char *kind = rs_json_obj_str(stream, "kind", "mpd");
        const char *input_mode = rs_json_obj_str(stream, "inputMode", "internal");
        // HLS passthrough is the one live path in C so far: a kind=m3u8 stream on
        // the internal remuxer, fetched and rewritten per request.
        if (is_m3u8 && strcmp(kind, "m3u8") == 0 && strcmp(input_mode, "internal") == 0) {
            if (!g_fetch_handler) { reply_error(c, 501, "Proxy playback isn't in this build."); return true; }
            serve_hls_playlist(server, c, hm, stream);
            return true;
        }
        // DASH: MPD->HLS, served entirely out of what the background live
        // engine has already downloaded and decrypted. The master lists a video
        // and (when present) an audio rendition; ?rep= selects a rendition's
        // media playlist. Segments and init flow through /proxy with live=1.
        if (is_m3u8 && strcmp(kind, "mpd") == 0 && strcmp(input_mode, "internal") == 0) {
            if (!g_dash_handler || !g_fetch_handler) { reply_error(c, 501, "DASH playback isn't in this build."); return true; }
            char *rep = query_var(hm, "rep");
            if (rep && rep[0]) serve_dash_media(server, c, hm, stream, rep);
            else serve_dash_master(server, c, hm, stream);
            free(rep);
            return true;
        }
    }

    reply_error(c, 501,
                "This stream needs the DASH/worker layer, which isn't in the C server yet. "
                "Enable Direct source, or use an HLS (.m3u8) source for live proxying.");
    return true;
}

// Mongoose event handler
// The front-end router's view addresses (VIEW_ROUTES in public/app.js). Kept in
// step with PanelServer.panelViewPaths on the Swift side.
static bool is_panel_view_path(struct mg_str uri) {
    static const char *const views[] = {
        "/providers", "/streams", "/server", "/logs", "/keys", "/settings", "/help",
    };
    for (size_t i = 0; i < sizeof(views) / sizeof(views[0]); i++)
        if (mg_strcmp(uri, mg_str(views[i])) == 0) return true;
    return false;
}

static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    restream_server_t *server = (restream_server_t *)c->fn_data;
    // A connection with an in-flight async job (see pending_job_dispatch)
    // closing before the worker finishes — the player gave up, or the panel
    // request was aborted. Mark it so the worker frees its own result instead
    // of replying to a connection that no longer exists.
    if (ev == MG_EV_CLOSE) {
        if (server) {
            pending_job_cancel(server, c->id);
            direct_forget_conn(server, c->id);
        }
        return;
    }
    // A worker thread's fetch finished; ev_data is the struct mg_str mg_wakeup()
    // delivers, carrying the rs_pending_job* it was called with. This is the
    // only thread that may build a reply from it — pending_job_finish builds
    // and sends it, then frees the job.
    if (ev == MG_EV_WAKEUP) {
        // The request that started this job is long gone, so there is nothing
        // to describe: reply without a stale request's headers.
        g_request_security[0] = '\0';
        if (server) pending_job_finish(server, c, (struct mg_str *)ev_data);
        return;
    }
    if (ev != MG_EV_HTTP_MSG) return;
    struct mg_http_message *hm = (struct mg_http_message *)ev_data;

    if (server) security_headers(server, c, hm, g_request_security, sizeof(g_request_security));
    else g_request_security[0] = '\0';

    // Liveness probe. Carries the binary's build time so it's trivial to tell,
    // from an unauthenticated curl, whether a rebuilt server was actually
    // restarted (a stale process reports the old stamp).
    if (mg_match(hm->uri, mg_str("/ping"), NULL)) {
        mg_http_reply(c, 200, JSON_HEADERS, "{\"status\":\"ok\",\"build\":\"%s %s\"}", __DATE__, __TIME__);
        return;
    }

    // Playback routes: /source and direct-source /play redirect; HLS
    // passthrough is proxied live; /proxy streams the segments.
    if (server && (mg_match(hm->uri, mg_str("/source/#"), NULL) ||
                   mg_match(hm->uri, mg_str("/play/#"), NULL) ||
                   mg_match(hm->uri, mg_str("/restream/#"), NULL) ||
                   mg_match(hm->uri, mg_str("/direct/#"), NULL) ||
                   mg_match(hm->uri, mg_str("/download/#"), NULL))) {
        if (handle_playback(server, c, hm)) return;
    }

    if (mg_match(hm->uri, mg_str("/api/#"), NULL)) {
        // Panel access log, under "__panel__". The panel polls /api/state,
        // /api/logs and /api/events every second or two; those are recorded as
        // a separate low-detail event so they can be told apart from — and do
        // not bury — real API calls, while still showing that the panel is
        // connected at all. Everything else is logged in full.
        if (server) {
            bool poll = mg_match(hm->uri, mg_str("/api/state"), NULL) ||
                        mg_match(hm->uri, mg_str("/api/logs"), NULL) ||
                        mg_match(hm->uri, mg_str("/api/events"), NULL);
            char line[512];
            char *ip = client_ip(server, c, hm);
            snprintf(line, sizeof(line), "%.*s %.*s  from %s",
                     (int)hm->method.len, hm->method.buf, (int)hm->uri.len, hm->uri.buf, ip);
            log_record(server, "__panel__", "info", poll ? "poll" : "access", NULL, 0, -1, line);
            rs_free(ip);
        }
        if (server && handle_api(server, c, hm)) return;
        // A recognised prefix but not a route we serve yet: honest 501, so a
        // browser gets a clean error rather than an HTML page as JSON.
        reply_error(c, 501,
                    "This endpoint isn't implemented in the C server yet. "
                    "Run the Swift 'restreamair' binary for full functionality.");
        return;
    }

    // Serve the panel's static assets when a web root is configured.
    if (server && server->web_root) {
        struct mg_http_serve_opts opts = {0};
        opts.root_dir = server->web_root;
        opts.extra_headers = g_request_security;
        // The panel is a single page that gives every view its own address, so
        // each of those addresses has to return index.html and let the
        // front-end router pick the view up from the path. A fixed list, not a
        // catch-all fallback: an unknown path must still 404 rather than
        // silently answer with the panel's HTML, which would make a mistyped
        // asset or API path look like a broken JSON response instead of a 404.
        if (is_panel_view_path(hm->uri)) {
            char index_path[1024];
            snprintf(index_path, sizeof(index_path), "%s/index.html", server->web_root);
            mg_http_serve_file(c, hm, index_path, &opts);
            return;
        }
        mg_http_serve_dir(c, hm, &opts);
        return;
    }

    // No web root: explain, rather than a bare 404.
    mg_http_reply(c, 404, "Content-Type: text/plain\r\n",
                  "restreamair-server: this is the C core. It answers /ping, the panel API "
                  "(auth, management, monitoring), live DASH and HLS playback, and — with "
                  "--root pointing at public/ — the panel's static files. ffmpeg input "
                  "modes and script providers aren't in the C server yet.\n");
}

restream_server_t* restream_server_create(void) {
    restream_server_t* server = (restream_server_t*)calloc(1, sizeof(restream_server_t));
    if (!server) return NULL;
    mg_mgr_init(&server->mgr);
    // Lets pending_job_worker() (spawned by the HLS-passthrough playback
    // routes) hand a finished fetch back to this thread instead of ever
    // touching a struct mg_connection itself. If this fails (should not
    // happen — it's just a local socketpair) pending_job_dispatch refuses to
    // dispatch and those routes reply with a clean 500 instead of hanging.
    server->wakeup_ok = mg_wakeup_init(&server->mgr);
    server->c = NULL;
    server->is_running = false;
    server->web_root = NULL;
    server->auth = rs_auth_create();
    server->sysstats = rs_sysstats_create();
    server->metrics = rs_metrics_create();
    server->pending_head = NULL;
    pthread_mutex_init(&server->pending_mu, NULL);
    pthread_cond_init(&server->pending_cv, NULL);
    pthread_mutex_init(&server->logo_mu, NULL);
#ifndef _WIN32
    pthread_mutex_init(&server->log_mu, NULL);
#endif
    // The engine needs both hooks; without them (the Swift build registers
    // neither) it stays NULL and the DASH routes report that honestly instead
    // of half-working. The C++ app registers them before calling this.
    server->live = rs_live_create(g_fetch_handler, g_dash_handler, live_log_sink, server);
    // state.json lives in the working directory, matching the Swift binary.
    if (rs_state_load(&server->state, "state.json") != 0 || !server->auth || !server->sysstats || !server->metrics) {
        // A malformed state.json is fatal — better to refuse than to risk
        // overwriting it with a fresh one and losing the user's data.
        rs_auth_destroy(server->auth);
        rs_sysstats_destroy(server->sysstats);
        rs_metrics_destroy(server->metrics);
        rs_live_destroy(server->live);
        rs_state_dispose(&server->state);
        mg_mgr_free(&server->mgr);
        pthread_mutex_destroy(&server->pending_mu);
        pthread_cond_destroy(&server->pending_cv);
        pthread_mutex_destroy(&server->logo_mu);
#ifndef _WIN32
        pthread_mutex_destroy(&server->log_mu);
#endif
        free(server);
        return NULL;
    }
    // Resume the sessions the previous run persisted, so a restart (or a switch
    // between the C and Swift servers, which share state.json) leaves signed-in
    // browsers signed in.
    rs_auth_import_sessions(server->auth, rs_json_obj_get(server->state.root, "sessions"));
    log_recordf(server, "__panel__", "info", "serverStart", NULL, 0, -1,
                "ReStreamAir C server started (build %s %s, live DASH engine %s)",
                __DATE__, __TIME__, server->live ? "available" : "unavailable");
    return server;
}

void restream_server_set_service_handler(restream_service_status_fn status,
                                         restream_service_action_fn action) {
    g_service_status = status;
    g_service_action = action;
}

void restream_server_set_web_root(restream_server_t* server, const char* path) {
    if (!server) return;
    free(server->web_root);
    server->web_root = (path && path[0]) ? rs_strdup(path) : NULL;
}

void restream_server_set_verbose(bool verbose) {
    mg_log_set(verbose ? MG_LL_DEBUG : MG_LL_ERROR);
}

void restream_server_set_probe_handler(restream_probe_fn handler) {
    g_probe_handler = handler;
}

void restream_server_set_fetch_handler(restream_fetch_fn handler) {
    g_fetch_handler = handler;
}

void restream_server_set_dash_handler(restream_dash_fn handler) {
    g_dash_handler = handler;
}

uint16_t restream_server_stored_port(const restream_server_t* server) {
    if (!server) return 0;
    const rs_json *settings = rs_json_obj_get(server->state.root, "settings");
    double v = rs_json_as_num(rs_json_obj_get(settings, "port"), 0);
    if (v <= 0 || v > 65535) return 0;
    return (uint16_t)v;
}

const char* restream_server_stored_bind(const restream_server_t* server) {
    if (!server) return NULL;
    const rs_json *settings = rs_json_obj_get(server->state.root, "settings");
    const char *v = rs_json_as_str(rs_json_obj_get(settings, "bindAddress"), "");
    return (v && v[0]) ? v : NULL;
}

bool restream_server_start(restream_server_t* server, uint16_t port, const char* bind_address) {
    if (!server) return false;
    
    char listen_url[256];
    if (bind_address && strlen(bind_address) > 0) {
        snprintf(listen_url, sizeof(listen_url), "http://%s:%d", bind_address, port);
    } else {
        snprintf(listen_url, sizeof(listen_url), "http://0.0.0.0:%d", port);
    }

    server->listen_port = port;

    // Pass the server through as fn_data so ev_handler can read the web root.
    server->c = mg_http_listen(&server->mgr, listen_url, ev_handler, server);
    if (server->c == NULL) {
        return false;
    }
    // Push a metrics frame to SSE subscribers once a second, and reap any live
    // engines that have finished winding down.
    mg_timer_add(&server->mgr, 1000, MG_TIMER_REPEAT, broadcast_metrics, server);
    // Direct links run on their own faster timer: a second of added latency on
    // every segment is the very cost they exist to avoid.
    mg_timer_add(&server->mgr, RS_DIRECT_PUMP_MS, MG_TIMER_REPEAT, pump_direct_links, server);
    server->is_running = true;
    // Streams that state.json says were running come back up on their own, so a
    // restart does not silently leave every stream dark until someone notices.
    live_sync_all(server);
    return true;
}

void restream_server_poll(restream_server_t* server, int timeout_ms) {
    if (server && server->is_running) {
        mg_mgr_poll(&server->mgr, timeout_ms);
    }
}

void restream_server_stop(restream_server_t* server) {
    if (server && server->is_running) {
        server->is_running = false;
        // In Mongoose, closing the connections or destroying the manager handles stopping
    }
}

void restream_server_destroy(restream_server_t* server) {
    if (server) {
        // Detached pending-job worker threads (see pending_job_dispatch) call
        // mg_wakeup() on this server's mgr and read/write the pending list on
        // their own time; both become use-after-free the moment
        // mg_mgr_free/pthread_mutex_destroy below run. Block until none are
        // left in flight — bounded by each job's own timeout, so this cannot
        // hang forever.
        pending_job_wait_idle(server);
        mg_mgr_free(&server->mgr);
        // Direct-link viewers and their muxes hold no threads, but they do hold
        // buffers; their connections are gone with the manager.
        while (server->direct_head) {
            rs_direct_client *d = server->direct_head;
            server->direct_head = d->next;
            direct_client_free(d);
        }
        while (server->ts_head) {
            rs_ts_client *t = server->ts_head;
            server->ts_head = t->next;
            ts_client_free(t);
        }
        while (server->ts_sessions) {
            rs_ts_session *s = server->ts_sessions;
            server->ts_sessions = s->next;
            ts_session_free(s);
        }
        // Stops and joins every worker first: they hold a pointer to this
        // server as their log sink, so nothing else may be torn down while one
        // is still running.
        rs_live_destroy(server->live);
        server->live = NULL;
        rs_auth_destroy(server->auth);
        rs_sysstats_destroy(server->sysstats);
        rs_metrics_destroy(server->metrics);
        if (server->logo_cache) rs_logo_cache_destroy(server->logo_cache);
        log_clear(server, "");  // frees ring-buffer strings
        pthread_mutex_destroy(&server->pending_mu);
        pthread_cond_destroy(&server->pending_cv);
        pthread_mutex_destroy(&server->logo_mu);
#ifndef _WIN32
        pthread_mutex_destroy(&server->log_mu);
#endif
        rs_state_dispose(&server->state);
        free(server->web_root);
        free(server);
    }
}
