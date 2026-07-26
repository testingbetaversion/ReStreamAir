#include "restream.h"
#include "rs_common.h"
#include "rs_auth.h"
#include "rs_json.h"
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

// Handles the playback routes that need no fetch: the /source redirect and a
// direct-source stream's /play redirect. Returns true if handled. The
// fetch/proxy/worker paths (HLS passthrough, DASH live) are not in the C server
// yet and fall through to an honest 501.
static bool handle_playback(restream_server_t *server, struct mg_connection *c,
                            struct mg_http_message *hm) {
    bool is_source = mg_match(hm->uri, mg_str("/source/*"), NULL);
    bool is_play = mg_match(hm->uri, mg_str("/play/#"), NULL);
    if (!is_source && !is_play) return false;

    // Playback auth: an API key is required only once at least one exists.
    char *key = playback_key(hm);
    bool allowed = rs_panel_playback_allowed(&server->state, key);
    free(key);
    if (!allowed) { reply_error(c, 401, "A valid playback key is required."); return true; }

    if (is_source) {
        char *id = capture(hm, "/source/*");
        const rs_json *stream = rs_panel_find_stream(&server->state, id);
        free(id);
        if (!stream) { reply_error(c, 404, "Stream not found."); return true; }
        const char *target = stream_source_target(stream);
        if (!target[0]) { reply_error(c, 400, "Stream has no source URL."); return true; }
        mg_printf(c, "HTTP/1.1 302 Found\r\nLocation: %s\r\nCache-Control: no-store\r\n"
                     "Content-Length: 0\r\n\r\n", target);
        return true;
    }

    // /play/<segment>/index.m3u8 or index.mpd — only the direct-source redirect
    // works without the fetch/worker layer.
    if (mg_match(hm->uri, mg_str("/play/*/index.m3u8"), NULL) ||
        mg_match(hm->uri, mg_str("/play/*/index.mpd"), NULL)) {
        char *segment = capture(hm, "/play/*/#");
        const rs_json *stream = rs_panel_find_stream(&server->state, segment);
        free(segment);
        if (!stream) { reply_error(c, 404, "Stream not found."); return true; }
        if (rs_json_obj_bool(stream, "directSource", false)) {
            const char *target = stream_source_target(stream);
            if (!target[0]) { reply_error(c, 400, "Stream has no source URL."); return true; }
            mg_printf(c, "HTTP/1.1 302 Found\r\nLocation: %s\r\nCache-Control: no-store\r\n"
                         "Content-Length: 0\r\n\r\n", target);
            return true;
        }
    }

    reply_error(c, 501,
                "Playback of this stream isn't in the C server yet — it needs the fetch/worker "
                "layer. Use the Swift 'restreamair' binary, or enable Direct source on the stream.");
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

    // Playback routes (redirect-only for now).
    if (server && (mg_match(hm->uri, mg_str("/source/#"), NULL) ||
                   mg_match(hm->uri, mg_str("/play/#"), NULL))) {
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
                  "restreamair-server: this is the C core. It answers /ping, the auth and "
                  "read-only state API, and (with --root pointing at public/) the panel's "
                  "static files. Editing and playback aren't in the C server yet.\n");
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
