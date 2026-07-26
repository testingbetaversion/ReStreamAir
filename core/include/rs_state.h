#ifndef RS_STATE_H
#define RS_STATE_H

// state.json as a preserved DOM. The whole file is held as an rs_json tree and
// written back in full, so fields the C code doesn't model yet are never lost —
// the invariant the entire control-plane port depends on.

#include "rs_common.h"
#include "rs_json.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    rs_json *root;  // the whole parsed file, always an object
    char *path;
} rs_state;

// Loads the file at `path`. A missing file is not an error — it yields an empty
// object, matching the Swift binary creating a fresh state on first run.
// Returns 0 on success, -1 on a present-but-malformed file or allocation
// failure.
int rs_state_load(rs_state *st, const char *path);

// Serializes the whole tree and writes it atomically (temp file + rename).
// Returns 0 on success, -1 on failure.
int rs_state_save(const rs_state *st);

void rs_state_dispose(rs_state *st);

// Returns the `adminUsers` array, creating it if absent so callers can push.
rs_json *rs_state_admin_users(rs_state *st);

// Returns the `settings` object, creating it if absent.
rs_json *rs_state_settings(rs_state *st);

// Read-only accessors for the top-level arrays; NULL if absent.
const rs_json *rs_state_providers(const rs_state *st);
const rs_json *rs_state_api_keys(const rs_state *st);

#ifdef __cplusplus
}
#endif

#endif  // RS_STATE_H
