#include "restream.h"
#include "rs_common.h"
#include "rs_auth.h"
#include "rs_json.h"
#include "rs_m3u8.h"
#include "rs_panel.h"
#include "rs_state.h"
#include "rs_sysstats.h"
#include "../deps/mongoose.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

struct restream_server {
    struct mg_mgr mgr;
    struct mg_connection *c;
    bool is_running;
    char *web_root;         // static file directory, or NULL
    rs_state state;         // state.json as a preserved DOM
    rs_auth *auth;          // admin accounts + sessions
    rs_sysstats *sysstats;  // live host stats for the monitoring view
};

// A live Server-Sent Events subscriber is marked in mongoose's per-connection
// scratch space, so the broadcast timer can find them.
#define RS_SSE_MARKER 'S'

// The probe/fetch handlers are registered by the C++ app; the core only calls
// through them, so libcurl/libxml2 never reach the Swift build. Declared here so
// the router (below) can reference them before their setters are defined.
static restream_probe_fn g_probe_handler = NULL;
static restream_fetch_fn g_fetch_handler = NULL;

// --- small response helpers ------------------------------------------------

static const char *JSON_HEADERS = "Content-Type: application/json\r\n";

// A JSON error body the panel's fetch layer reads as `payload.error`.
static void reply_error(struct mg_connection *c, int status, const char *message) {
    mg_http_reply(c, status, JSON_HEADERS, "{\"error\":%m}", MG_ESC(message));
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
    char headers[512];
    snprintf(headers, sizeof(headers), "%sCache-Control: no-store\r\n%s",
             JSON_HEADERS, extra_headers ? extra_headers : "");
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
    free(user);
    reply_json(c, 200, out, NULL);
}

// A unix-seconds timestamp expressed the way Swift's JSONEncoder writes a Date:
// seconds since the 2001 reference epoch. Keeps createdAt byte-compatible.
static double apple_epoch_now(void) {
    return (double)time(NULL) - 978307200.0;
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

    char *token = rs_auth_create_session(s->auth, username, remember);
    char *cookie = token ? rs_auth_set_cookie(token, remember) : NULL;
    free(username); free(password); rs_free(token);
    if (!cookie) { reply_error(c, 500, "Could not start a session."); return; }
    char extra[512];
    snprintf(extra, sizeof(extra), "Set-Cookie: %s\r\n", cookie);
    rs_free(cookie);
    reply_json(c, 200, rs_json_new_obj(), extra);  // {"ok":true} shape isn't required; UI only checks response.ok
}

static void handle_auth_login(restream_server_t *s, struct mg_connection *c,
                              struct mg_http_message *hm) {
    rs_json *body = rs_json_parse(hm->body.buf, hm->body.len);
    char *username = body ? body_str(body, "username") : NULL;
    char *password = body ? body_str(body, "password") : NULL;
    bool remember = body && strcmp(rs_json_as_str(rs_json_obj_get(body, "remember"), ""), "true") == 0;
    rs_json_free(body);

    rs_json *admin = (username && username[0]) ? admin_by_username(&s->state, username) : NULL;
    bool ok = admin && password &&
              rs_auth_verify_password(password,
                                      rs_json_as_str(rs_json_obj_get(admin, "passwordHash"), ""),
                                      rs_json_as_str(rs_json_obj_get(admin, "salt"), ""));
    if (!ok) {
        free(username); free(password);
        reply_error(c, 401, "Invalid username or password.");
        return;
    }
    char *token = rs_auth_create_session(s->auth, username, remember);
    char *cookie = token ? rs_auth_set_cookie(token, remember) : NULL;
    free(username); free(password); rs_free(token);
    if (!cookie) { reply_error(c, 500, "Could not start a session."); return; }
    char extra[512];
    snprintf(extra, sizeof(extra), "Set-Cookie: %s\r\n", cookie);
    rs_free(cookie);
    reply_json(c, 200, rs_json_new_obj(), extra);
}

static void handle_auth_logout(restream_server_t *s, struct mg_connection *c,
                               struct mg_http_message *hm) {
    char *cookie = header_dup(hm, "Cookie");
    if (cookie) {
        char *token = rs_auth_cookie_token(cookie);
        if (token) { rs_auth_end_session(s->auth, token); free(token); }
        free(cookie);
    }
    char *clear = rs_auth_clear_cookie();
    char extra[256];
    snprintf(extra, sizeof(extra), "Set-Cookie: %s\r\n", clear ? clear : "");
    rs_free(clear);
    reply_json(c, 200, rs_json_new_obj(), extra);
}

// The Host header, for building absolute URLs in the state view. Caller frees.
static char *request_host(struct mg_http_message *hm) {
    char *host = header_dup(hm, "Host");
    return host ? host : rs_strdup("127.0.0.1");
}

static void handle_state(restream_server_t *s, struct mg_connection *c, struct mg_http_message *hm) {
    char *host = request_host(hm);
    reply_json(c, 200, rs_panel_view(&s->state, host), NULL);
    free(host);
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

static void handle_settings(restream_server_t *s, struct mg_connection *c) {
    const rs_json *settings = rs_json_obj_get(s->state.root, "settings");
    rs_json *out = rs_json_new_obj();
    rs_json_obj_set(out, "port",
                    rs_json_new_int((long long)rs_json_as_num(rs_json_obj_get(settings, "port"), 8787)));
    rs_json_obj_set(out, "bindAddress",
                    rs_json_new_str(rs_json_as_str(rs_json_obj_get(settings, "bindAddress"), "")));
    reply_json(c, 200, out, NULL);
}

// Builds the metrics payload the SSE stream and the Server view consume. The
// system stats are real; bandwidth, per-stream metrics and connections are
// empty until the playback plane (slice 4) produces traffic.
static rs_json *build_metrics(restream_server_t *s) {
    rs_json *out = rs_json_new_obj();
    rs_json *zero = rs_json_new_obj();
    rs_json_obj_set_int(zero, "bytesPerSecond", 0);
    rs_json_obj_set_int(zero, "allTimeBytes", 0);
    rs_json_obj_set(out, "global", zero);
    rs_json *zero2 = rs_json_new_obj();
    rs_json_obj_set_int(zero2, "bytesPerSecond", 0);
    rs_json_obj_set_int(zero2, "allTimeBytes", 0);
    rs_json_obj_set(out, "globalInput", zero2);
    rs_json_obj_set(out, "system", rs_sysstats_snapshot(s->sysstats));
    rs_json_obj_set(out, "streams", rs_json_new_obj());
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

// The Logs view. No workers run in the C server yet, so there's nothing to
// report — but the endpoint returns the shape the panel expects.
static void handle_logs(struct mg_connection *c) {
    rs_json *out = rs_json_new_obj();
    rs_json_obj_set(out, "entries", rs_json_new_arr());
    rs_json_obj_set(out, "availableDates", rs_json_new_arr());
    reply_json(c, 200, out, NULL);
}

// Dispatches /api/*. Returns true if it handled the request.
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

    // Everything past here needs a signed-in admin, same as the Swift panel.
    char *user = current_user(s, hm);
    if (!user) { reply_error(c, 401, "Not signed in."); return true; }
    free(user);

    if (mg_match(hm->uri, mg_str("/api/state"), NULL) && method_is(hm, "GET")) {
        handle_state(s, c, hm); return true;
    }
    if (mg_match(hm->uri, mg_str("/api/settings"), NULL) && method_is(hm, "GET")) {
        handle_settings(s, c); return true;
    }
    if (mg_match(hm->uri, mg_str("/api/events"), NULL) && method_is(hm, "GET")) {
        handle_events(s, c); return true;
    }
    if (mg_match(hm->uri, mg_str("/api/logs"), NULL) && method_is(hm, "GET")) {
        handle_logs(c); return true;
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
        char errbuf[256] = {0};
        char *result = g_probe_handler(url, proxy, headers, errbuf, sizeof(errbuf));
        free(url); free(proxy); free(headers);
        if (!result) {
            reply_error(c, 400, errbuf[0] ? errbuf : "Could not probe the source.");
            return true;
        }
        char headers_out[128];
        snprintf(headers_out, sizeof(headers_out), "%sCache-Control: no-store\r\n", JSON_HEADERS);
        mg_http_reply(c, 200, headers_out, "%s", result);
        rs_free(result);
        return true;
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

    // --- stream CRUD ---
    if (mg_match(hm->uri, mg_str("/api/providers/*/streams"), NULL) && method_is(hm, "POST")) {
        char *provider_id = capture(hm, "/api/providers/*/streams");
        rs_json *body = parse_body(hm);
        const char *err = NULL;
        int rc = rs_panel_create_stream(&s->state, provider_id, body, &err);
        rs_json_free(body);
        free(provider_id);
        reply_after_mutation(s, c, hm, rc, err);
        return true;
    }
    if (mg_match(hm->uri, mg_str("/api/streams/*"), NULL) && method_is(hm, "PUT")) {
        char *id = capture(hm, "/api/streams/*");
        rs_json *body = parse_body(hm);
        const char *err = NULL;
        int rc = rs_panel_update_stream(&s->state, id, body, &err);
        rs_json_free(body);
        free(id);
        reply_after_mutation(s, c, hm, rc, err);
        return true;
    }
    if (mg_match(hm->uri, mg_str("/api/streams/*"), NULL) && method_is(hm, "DELETE")) {
        char *id = capture(hm, "/api/streams/*");
        const char *err = NULL;
        int rc = rs_panel_delete_stream(&s->state, id, &err);
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

// The rewrite transforms need the stream id to build proxy/variant paths.
static char *media_transform(void *ud, const char *abs_uri, rs_m3u8_line_kind kind, int64_t seq) {
    (void)seq;
    const char *stream_id = (const char *)ud;
    const char *kindstr = kind == RS_M3U8_LINE_KEY ? "key" : (kind == RS_M3U8_LINE_MAP ? "map" : "segment");
    char *enc = query_encode(abs_uri);
    if (!enc) return NULL;
    size_t need = strlen(stream_id) + strlen(enc) + 64;
    char *out = (char *)malloc(need);
    if (out) snprintf(out, need, "/proxy/%s/item?u=%s&kind=%s", stream_id, enc, kindstr);
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
}

// Serves an HLS passthrough playlist: fetch the remote playlist (the configured
// URL, or a ?variant= link from an already-rewritten master), rewrite every URI
// to loop back through this server, and return it.
static void serve_hls_playlist(restream_server_t *server, struct mg_connection *c,
                               struct mg_http_message *hm, const rs_json *stream) {
    const char *stream_id = rs_json_obj_str(stream, "id", "");
    const rs_json *provider = provider_of(&server->state, stream);

    char *variant = query_var(hm, "variant");
    const char *manifest_url = variant ? variant : stream_source_target(stream);
    if (!manifest_url[0]) { free(variant); reply_error(c, 400, "Stream has no source URL."); return; }

    char *proxy = effective_proxy(provider, stream, rs_json_obj_bool(stream, "proxyManifest", true));
    char *headers = effective_headers(provider, stream, "manifestHeaders");
    char *body = NULL;
    size_t body_len = 0;
    char err[256] = {0};
    int rc = g_fetch_handler(manifest_url, proxy, headers, NULL, &body, &body_len,
                             NULL, NULL, NULL, err, sizeof(err));
    free(proxy);
    free(headers);
    if (rc != 0) { free(variant); reply_error(c, 502, err[0] ? err : "Could not fetch the playlist."); return; }

    char *rewritten;
    if (rs_m3u8_is_master(body)) {
        rewritten = rs_m3u8_rewrite_master(body, manifest_url, master_transform, (void *)stream_id);
    } else {
        // The player fetches (and, if encrypted, decrypts with) the key itself,
        // so keys are proxied through rather than dropped — server-side HLS
        // decryption is a later increment.
        rewritten = rs_m3u8_rewrite(body, manifest_url, false, media_transform, (void *)stream_id);
    }
    free(body);
    free(variant);
    if (!rewritten) { reply_error(c, 500, "Out of memory rewriting the playlist."); return; }
    mg_http_reply(c, 200, "Content-Type: application/vnd.apple.mpegurl; charset=utf-8\r\n"
                          "Cache-Control: no-store\r\n", "%s", rewritten);
    rs_free(rewritten);
}

// Proxies one segment/key/init: fetch ?u= through the stream's media settings
// and stream the bytes back.
static void serve_proxy_item(restream_server_t *server, struct mg_connection *c,
                             struct mg_http_message *hm, const char *stream_id) {
    const rs_json *stream = rs_panel_find_stream(&server->state, stream_id);
    if (!stream) { reply_error(c, 404, "Stream not found."); return; }
    char *url = query_var(hm, "u");
    if (!url) { reply_error(c, 400, "Missing ?u= target."); return; }
    const rs_json *provider = provider_of(&server->state, stream);

    // Forward the player's Range so byte-range segments and seeking fetch only
    // the requested slice (and never a whole huge file).
    char *range = header_dup(hm, "Range");
    char *proxy = effective_proxy(provider, stream, rs_json_obj_bool(stream, "proxyMedia", true));
    char *headers = effective_headers(provider, stream, "mediaHeaders");
    char *body = NULL, *ct = NULL, *cr = NULL;
    size_t body_len = 0;
    long status = 0;
    char err[256] = {0};
    int rc = g_fetch_handler(url, proxy, headers, range, &body, &body_len, &status, &ct, &cr,
                             err, sizeof(err));
    free(proxy);
    free(headers);
    free(range);
    if (rc != 0) { free(url); free(ct); free(cr); reply_error(c, 502, err[0] ? err : "Segment fetch failed."); return; }

    const char *type = (ct && ct[0]) ? ct : content_type_from_ext(url);
    // Relay the upstream's 206 + Content-Range so the player's byte-range
    // request is answered as a partial response, not a 200 of the wrong length.
    if (status == 206 && cr && cr[0]) {
        mg_printf(c, "HTTP/1.1 206 Partial Content\r\nContent-Type: %s\r\nContent-Range: %s\r\n"
                     "Content-Length: %lu\r\nAccept-Ranges: bytes\r\nCache-Control: no-store\r\n"
                     "Access-Control-Allow-Origin: *\r\n\r\n",
                  type, cr, (unsigned long)body_len);
    } else {
        mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
                     "Accept-Ranges: bytes\r\nCache-Control: no-store\r\n"
                     "Access-Control-Allow-Origin: *\r\n\r\n",
                  type, (unsigned long)body_len);
    }
    mg_send(c, body, body_len);
    free(url);
    free(ct);
    free(cr);
    free(body);
}

// Playback routes. Direct-source streams redirect; HLS passthrough is proxied
// live. DASH (mpd) and ffmpeg modes still need the worker layer and 501.
static bool handle_playback(restream_server_t *server, struct mg_connection *c,
                            struct mg_http_message *hm) {
    bool is_source = mg_match(hm->uri, mg_str("/source/*"), NULL);
    bool is_play = mg_match(hm->uri, mg_str("/play/#"), NULL);
    bool is_proxy = mg_match(hm->uri, mg_str("/proxy/#"), NULL);
    if (!is_source && !is_play && !is_proxy) return false;

    // Playback auth: an API key is required only once at least one exists.
    char *key = playback_key(hm);
    bool allowed = rs_panel_playback_allowed(&server->state, key);
    free(key);
    if (!allowed) { reply_error(c, 401, "A valid playback key is required."); return true; }

    if (is_proxy) {
        if (!g_fetch_handler) { reply_error(c, 501, "Proxy playback isn't in this build."); return true; }
        char *id = capture(hm, "/proxy/*/#");
        if (id) serve_proxy_item(server, c, hm, id);
        else reply_error(c, 400, "Bad proxy path.");
        free(id);
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
    }

    reply_error(c, 501,
                "This stream needs the DASH/worker layer, which isn't in the C server yet. "
                "Enable Direct source, or use an HLS (.m3u8) source for live proxying.");
    return true;
}

// Mongoose event handler
static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev != MG_EV_HTTP_MSG) return;
    struct mg_http_message *hm = (struct mg_http_message *)ev_data;
    restream_server_t *server = (restream_server_t *)c->fn_data;

    // Liveness probe.
    if (mg_match(hm->uri, mg_str("/ping"), NULL)) {
        mg_http_reply(c, 200, JSON_HEADERS, "{\"status\":\"ok\"}");
        return;
    }

    // Playback routes: /source and direct-source /play redirect; HLS
    // passthrough is proxied live; /proxy streams the segments.
    if (server && (mg_match(hm->uri, mg_str("/source/#"), NULL) ||
                   mg_match(hm->uri, mg_str("/play/#"), NULL) ||
                   mg_match(hm->uri, mg_str("/proxy/#"), NULL))) {
        if (handle_playback(server, c, hm)) return;
    }

    if (mg_match(hm->uri, mg_str("/api/#"), NULL)) {
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
        mg_http_serve_dir(c, hm, &opts);
        return;
    }

    // No web root: explain, rather than a bare 404.
    mg_http_reply(c, 404, "Content-Type: text/plain\r\n",
                  "restreamair-server: this is the C core. It answers /ping, the panel API "
                  "(auth, management, monitoring, direct-source playback) and, with --root "
                  "pointing at public/, the panel's static files. Proxied/DASH streaming "
                  "isn't in the C server yet.\n");
}

restream_server_t* restream_server_create(void) {
    restream_server_t* server = (restream_server_t*)calloc(1, sizeof(restream_server_t));
    if (!server) return NULL;
    mg_mgr_init(&server->mgr);
    server->c = NULL;
    server->is_running = false;
    server->web_root = NULL;
    server->auth = rs_auth_create();
    server->sysstats = rs_sysstats_create();
    // state.json lives in the working directory, matching the Swift binary.
    if (rs_state_load(&server->state, "state.json") != 0 || !server->auth || !server->sysstats) {
        // A malformed state.json is fatal — better to refuse than to risk
        // overwriting it with a fresh one and losing the user's data.
        rs_auth_destroy(server->auth);
        rs_sysstats_destroy(server->sysstats);
        rs_state_dispose(&server->state);
        mg_mgr_free(&server->mgr);
        free(server);
        return NULL;
    }
    return server;
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

bool restream_server_start(restream_server_t* server, uint16_t port, const char* bind_address) {
    if (!server) return false;
    
    char listen_url[256];
    if (bind_address && strlen(bind_address) > 0) {
        snprintf(listen_url, sizeof(listen_url), "http://%s:%d", bind_address, port);
    } else {
        snprintf(listen_url, sizeof(listen_url), "http://0.0.0.0:%d", port);
    }

    // Pass the server through as fn_data so ev_handler can read the web root.
    server->c = mg_http_listen(&server->mgr, listen_url, ev_handler, server);
    if (server->c == NULL) {
        return false;
    }
    // Push a metrics frame to SSE subscribers once a second.
    mg_timer_add(&server->mgr, 1000, MG_TIMER_REPEAT, broadcast_metrics, server);
    server->is_running = true;
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
        mg_mgr_free(&server->mgr);
        rs_auth_destroy(server->auth);
        rs_sysstats_destroy(server->sysstats);
        rs_state_dispose(&server->state);
        free(server->web_root);
        free(server);
    }
}
