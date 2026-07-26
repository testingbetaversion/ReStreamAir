#ifndef RS_PANEL_H
#define RS_PANEL_H

// The panel's control-plane logic: the /api/state view transform and the
// provider/stream/user/key mutations, all operating on the state DOM. Ports the
// viewState/streamView/providerView shapes and the create/update/delete handlers
// from PanelServer.swift. Kept out of server.c (which stays HTTP glue) and out
// of the socket layer, so it can be unit-tested directly.

#include "rs_auth.h"
#include "rs_common.h"
#include "rs_json.h"
#include "rs_state.h"

#ifdef __cplusplus
extern "C" {
#endif

// Builds the full /api/state response — providers (each stream expanded with
// its computed play URLs and status), apiKeys, settings, and the placeholder
// system/bandwidth/version objects. `host` is the request's Host header, used
// to build absolute URLs. The result is freed with rs_json_free.
rs_json *rs_panel_view(const rs_state *st, const char *host);

// The /api/users and /api/keys list responses (their own shapes, not viewState).
rs_json *rs_panel_users_view(const rs_state *st);
rs_json *rs_panel_keys_view(const rs_state *st);

// Mutations. Each returns 0 on success, or a negative HTTP-ish status (e.g.
// -400, -404) with a human message in *err (a static string — do not free).
// They mutate the DOM but do not save; the caller saves and re-renders.
int rs_panel_create_provider(rs_state *st, const rs_json *body, const char **err);
int rs_panel_update_provider(rs_state *st, const char *id, const rs_json *body, const char **err);
int rs_panel_delete_provider(rs_state *st, const char *id, const char **err);

int rs_panel_create_stream(rs_state *st, const char *provider_id, const rs_json *body, const char **err);
int rs_panel_update_stream(rs_state *st, const char *stream_id, const rs_json *body, const char **err);
int rs_panel_delete_stream(rs_state *st, const char *stream_id, const char **err);

// User creation hashes the password, so it needs the auth module.
int rs_panel_create_user(rs_state *st, const rs_json *body, const char **err);
int rs_panel_delete_user(rs_state *st, const char *id, const char **err);

int rs_panel_create_key(rs_state *st, const rs_json *body, const char **err);
int rs_panel_delete_key(rs_state *st, const char *id, const char **err);

// A name slug, exposed for tests. Alphanumeric runs joined by single dashes,
// lowercased — matches PanelServer.slugify.
char *rs_panel_slugify(const char *name);

// Finds a stream by its id, or by a name slug when that slug is unique — the
// resolution a /play/<segment>/… or /source/<id> request needs. Returns the
// stream node (owned by the state), or NULL.
const rs_json *rs_panel_find_stream(const rs_state *st, const char *id_or_slug);

// Whether a request bearing `provided_key` (from ?key= or a Bearer header) may
// play. Playback is open when no API keys exist; otherwise the key must match
// one on file. `provided_key` may be NULL.
bool rs_panel_playback_allowed(const rs_state *st, const char *provided_key);

#ifdef __cplusplus
}
#endif

#endif  // RS_PANEL_H
