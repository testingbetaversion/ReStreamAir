#ifndef RS_AUTH_H
#define RS_AUTH_H

// Admin password hashing and session tracking. Password hashes are persisted in
// state.json, and the PBKDF2 parameters and base64 framing are fixed by what is
// already written there: an account created by an older build has to keep
// logging in.
//
// Sessions persist too, through rs_auth_export_sessions/rs_auth_import_sessions
// and the "sessions" array in state.json. What is stored is the SHA-256 of the
// token, never the token itself: state.json is a file an operator copies
// around, and a bearer token in it would be a spare key to the panel. The
// server holds the same hash in memory and hashes each presented cookie to
// compare, so a restart — or switching which server is running — no longer
// signs everyone out, and "remember me for 30 days" means what it says.
//
// Failed sign-ins are tracked per identity (username + client IP) and answered
// with a growing delay, so a 100k-iteration PBKDF2 hash cannot be ground down
// by an unlimited number of guesses. That state is deliberately in-memory only:
// persisting it would let anyone who can reach the login form lock an account
// out across restarts.
//
// The server runs everything on mongoose's single poll thread, so the session
// store needs no locking.

#include "rs_common.h"
#include "rs_json.h"

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

// Ends every session belonging to `username` — what a password change or a
// deleted account must do, so an old cookie cannot outlive the credential it
// was issued against. Returns how many were dropped.
size_t rs_auth_end_sessions_for_user(rs_auth *auth, const char *username);

// Serialises the live (unexpired) sessions as a JSON array of
// {"tokenHash":hex,"username":…,"expiresAt":unix-seconds} objects, for storing
// in state.json. Caller owns the result (rs_json_free).
rs_json *rs_auth_export_sessions(rs_auth *auth);

// Restores sessions produced by rs_auth_export_sessions, skipping expired and
// malformed entries. Existing in-memory sessions are kept.
void rs_auth_import_sessions(rs_auth *auth, const rs_json *sessions);

// --- login throttling ------------------------------------------------------

// Seconds the caller must wait before another sign-in attempt for `identity`
// will be considered, or 0 if an attempt is allowed right now.
int rs_auth_throttle_delay(rs_auth *auth, const char *identity);

// Records a failed attempt and returns the delay now imposed (0 while still
// inside the free allowance).
int rs_auth_throttle_record_failure(rs_auth *auth, const char *identity);

// Clears the failure record after a successful sign-in.
void rs_auth_throttle_reset(rs_auth *auth, const char *identity);

// Extracts the restreamair_session value from a Cookie header (freed with
// rs_free), or NULL if absent.
char *rs_auth_cookie_token(const char *cookie_header);

// Builds the Set-Cookie header value for a login (freed with rs_free). Byte-for-
// byte identical to AuthStore.setCookieHeader. `secure` adds the Secure
// attribute, which must be set whenever the user reached us over HTTPS and must
// not be over plain HTTP — a Secure cookie on an http:// origin is simply
// dropped by the browser, which would present as "login does nothing".
char *rs_auth_set_cookie(const char *token, bool remember, bool secure);

// The Set-Cookie value that clears the session (freed with rs_free).
char *rs_auth_clear_cookie(bool secure);

// Parses "Basic base64(user:pass)" into freshly allocated username/password
// (both rs_free). Returns 0 on success, -1 otherwise.
int rs_auth_parse_basic(const char *authorization_header, char **username, char **password);

#ifdef __cplusplus
}
#endif

#endif  // RS_AUTH_H
