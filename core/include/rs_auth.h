#ifndef RS_AUTH_H
#define RS_AUTH_H

// Admin password hashing and session tracking — the C port of AuthStore.swift.
// Password hashes are persisted in state.json and must interoperate with the
// Swift binary (same PBKDF2 parameters, same base64 encoding), so an account
// created by either server logs in on the other. Sessions themselves are
// in-memory and per-process: switching which server is running means signing in
// again, exactly as a restart already does.
//
// The server runs everything on mongoose's single poll thread, so the session
// store needs no locking.

#include "rs_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rs_auth rs_auth;

rs_auth *rs_auth_create(void);
void rs_auth_destroy(rs_auth *auth);

// Hashes `password` with PBKDF2-HMAC-SHA256 (100k iterations, 32-byte key, a
// fresh 16-byte random salt), matching AuthStore.hashPassword. Writes freshly
// allocated base64 strings to *hash and *salt (both freed with rs_free).
// Returns 0 on success, -1 on an entropy or allocation failure.
int rs_auth_hash_password(const char *password, char **hash, char **salt);

// Verifies `password` against a stored base64 hash and salt. Constant-time
// comparison. Returns true on a match.
bool rs_auth_verify_password(const char *password, const char *hash_b64, const char *salt_b64);

// Creates a session for `username` and returns its token (freed with rs_free).
// `remember` selects the 30-day lifetime; otherwise 24 hours.
char *rs_auth_create_session(rs_auth *auth, const char *username, bool remember);

// Returns the username for a live session token (freed with rs_free), or NULL
// if the token is unknown or expired.
char *rs_auth_username_for_token(rs_auth *auth, const char *token);

void rs_auth_end_session(rs_auth *auth, const char *token);

// Extracts the restreamair_session value from a Cookie header (freed with
// rs_free), or NULL if absent.
char *rs_auth_cookie_token(const char *cookie_header);

// Builds the Set-Cookie header value for a login (freed with rs_free). Byte-for-
// byte identical to AuthStore.setCookieHeader.
char *rs_auth_set_cookie(const char *token, bool remember);

// The Set-Cookie value that clears the session (freed with rs_free).
char *rs_auth_clear_cookie(void);

// Parses "Basic base64(user:pass)" into freshly allocated username/password
// (both rs_free). Returns 0 on success, -1 otherwise.
int rs_auth_parse_basic(const char *authorization_header, char **username, char **password);

#ifdef __cplusplus
}
#endif

#endif  // RS_AUTH_H
