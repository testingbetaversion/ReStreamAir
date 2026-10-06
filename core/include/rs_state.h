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
// object — a fresh state, the same as a first run.
// Returns 0 on success, -1 on a present-but-malformed file or allocation
// failure.
int rs_state_load(rs_state *st, const char *path);

// Serializes the whole tree and writes it atomically (temp file + rename).
// Returns 0 on success, -1 on failure.
int rs_state_save(const rs_state *st);

void rs_state_dispose(rs_state *st);

// Copies the saved file into `dir` (created owner-only if missing) as
// state-YYYYMMDD-HHMMSS.json, owner-only like state.json itself, then removes
// all but the newest `keep` backups (keep <= 0 keeps everything). Only files
// with that exact name pattern are ever removed. The new name goes to
// `name_out`. Returns 0 on success, -1 on failure.
int rs_state_backup(const rs_state *st, const char *dir, int keep, char *name_out, size_t name_cap);

// [{name, time (unix seconds, UTC), bytes}], newest first. Never NULL unless
// out of memory; an absent directory is an empty array.
rs_json *rs_state_list_backups(const char *dir);

// When the newest backup in `dir` was taken (unix seconds), or 0 for none.
long long rs_state_last_backup_time(const char *dir);

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
