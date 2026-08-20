#include "rs_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rs_crypto.h"

#define RS_PBKDF2_ITERATIONS 100000
#define RS_PBKDF2_KEY_LEN 32
#define RS_SALT_LEN 16
#define RS_SESSION_DEFAULT_SECONDS (24 * 60 * 60)
#define RS_SESSION_REMEMBER_SECONDS (30 * 24 * 60 * 60)
#define RS_COOKIE_NAME "restreamair_session"

// Timestamps in state.json are seconds since the 2001 reference epoch,
// and state.json is shared between the two servers, so persisted timestamps use
// that reference too — the same convention adminUsers.createdAt already
// follows. Storing Unix seconds here would look ~31 years stale to any reader of the
// binary, which would quietly drop every session the C server wrote.
#define RS_APPLE_EPOCH_OFFSET 978307200.0

// Sign-in throttle shape. The first RS_THROTTLE_FREE_ATTEMPTS failures are
// free, so a typo costs nothing; after that each failure doubles the wait, up
// to RS_THROTTLE_MAX_DELAY. A quiet RS_THROTTLE_WINDOW forgets the record
// entirely, so a locked-out operator is never locked out permanently.
#define RS_THROTTLE_FREE_ATTEMPTS 5
#define RS_THROTTLE_BASE_DELAY 2
#define RS_THROTTLE_MAX_DELAY (15 * 60)
#define RS_THROTTLE_WINDOW (60 * 60)

typedef struct {
    char *token_hash;  // SHA-256 of the token, hex — never the token itself
    char *username;
    time_t expires_at;
} rs_session;

typedef struct {
    char *identity;    // "username|client-ip"
    int failures;
    time_t last_failure;
    time_t retry_after;
} rs_throttle;

struct rs_auth {
    rs_session *sessions;
    size_t len;
    size_t cap;
    rs_throttle *throttles;
    size_t throttle_len;
    size_t throttle_cap;
};

rs_auth *rs_auth_create(void) {
    return (rs_auth *)calloc(1, sizeof(rs_auth));
}

void rs_auth_destroy(rs_auth *auth) {
    if (!auth) return;
    for (size_t i = 0; i < auth->len; i++) {
        free(auth->sessions[i].token_hash);
        free(auth->sessions[i].username);
    }
    free(auth->sessions);
    for (size_t i = 0; i < auth->throttle_len; i++) free(auth->throttles[i].identity);
    free(auth->throttles);
    free(auth);
}

// --- password hashing ------------------------------------------------------

int rs_auth_hash_password(const char *password, char **hash, char **salt) {
    *hash = NULL;
    *salt = NULL;
    uint8_t salt_bytes[RS_SALT_LEN];
    if (rs_random_bytes(salt_bytes, sizeof(salt_bytes)) != 0) return -1;

    uint8_t derived[RS_PBKDF2_KEY_LEN];
    if (rs_pbkdf2_sha256((const uint8_t *)password, strlen(password),
                         salt_bytes, sizeof(salt_bytes),
                         RS_PBKDF2_ITERATIONS, derived, sizeof(derived)) != 0) {
        return -1;
    }

    char *hash_b64 = rs_base64_encode(derived, sizeof(derived));
    char *salt_b64 = rs_base64_encode(salt_bytes, sizeof(salt_bytes));
    if (!hash_b64 || !salt_b64) {
        rs_free(hash_b64);
        rs_free(salt_b64);
        return -1;
    }
    *hash = hash_b64;
    *salt = salt_b64;
    return 0;
}

// Length-aware constant-time compare, so verification doesn't leak the hash
// through timing.
static bool constant_time_equal(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < la; i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

bool rs_auth_verify_password(const char *password, const char *hash_b64, const char *salt_b64) {
    if (!password || !hash_b64 || !salt_b64) return false;
    uint8_t salt_bytes[64];
    size_t salt_len = 0;
    if (rs_base64_decode(salt_b64, salt_bytes, sizeof(salt_bytes), &salt_len) != 0) return false;

    uint8_t derived[RS_PBKDF2_KEY_LEN];
    if (rs_pbkdf2_sha256((const uint8_t *)password, strlen(password),
                         salt_bytes, salt_len,
                         RS_PBKDF2_ITERATIONS, derived, sizeof(derived)) != 0) {
        return false;
    }
    char *computed = rs_base64_encode(derived, sizeof(derived));
    if (!computed) return false;
    bool match = constant_time_equal(computed, hash_b64);
    rs_free(computed);
    return match;
}

// --- sessions --------------------------------------------------------------

// A 32-byte random token as hex — unguessable, and (unlike the persisted
// hashes) purely internal, so its shape needn't match any older token format.
static char *make_token(void) {
    uint8_t bytes[32];
    if (rs_random_bytes(bytes, sizeof(bytes)) != 0) return NULL;
    char *token = (char *)malloc(sizeof(bytes) * 2 + 1);
    if (!token) return NULL;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(bytes); i++) {
        token[i * 2] = hex[bytes[i] >> 4];
        token[i * 2 + 1] = hex[bytes[i] & 0xf];
    }
    token[sizeof(bytes) * 2] = '\0';
    return token;
}

// SHA-256 of a token, lowercase hex. This is what the session store holds and
// what state.json persists; the raw token exists only in the cookie.
static char *hash_token(const char *token) {
    uint8_t digest[RS_SHA256_DIGEST_LEN];
    rs_sha256((const uint8_t *)token, strlen(token), digest);
    char *out = (char *)malloc(sizeof(digest) * 2 + 1);
    if (!out) return NULL;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0xf];
    }
    out[sizeof(digest) * 2] = '\0';
    return out;
}

static void drop_session(rs_auth *auth, size_t index) {
    free(auth->sessions[index].token_hash);
    free(auth->sessions[index].username);
    memmove(&auth->sessions[index], &auth->sessions[index + 1],
            (auth->len - index - 1) * sizeof(rs_session));
    auth->len--;
}

// Adds a session record for an already-hashed token. Takes ownership of neither
// argument. Returns false on allocation failure.
static bool push_session(rs_auth *auth, char *token_hash, const char *username, time_t expires_at) {
    char *username_copy = rs_strdup(username);
    if (!username_copy) return false;
    if (auth->len + 1 > auth->cap) {
        size_t cap = auth->cap ? auth->cap * 2 : 8;
        rs_session *grown = (rs_session *)realloc(auth->sessions, cap * sizeof(rs_session));
        if (!grown) { free(username_copy); return false; }
        auth->sessions = grown;
        auth->cap = cap;
    }
    auth->sessions[auth->len].token_hash = token_hash;
    auth->sessions[auth->len].username = username_copy;
    auth->sessions[auth->len].expires_at = expires_at;
    auth->len++;
    return true;
}

char *rs_auth_create_session(rs_auth *auth, const char *username, bool remember) {
    if (!auth || !username) return NULL;
    char *token = make_token();
    if (!token) return NULL;
    char *token_hash = hash_token(token);
    if (!token_hash) { free(token); return NULL; }

    long lifetime = remember ? RS_SESSION_REMEMBER_SECONDS : RS_SESSION_DEFAULT_SECONDS;
    if (!push_session(auth, token_hash, username, time(NULL) + lifetime)) {
        free(token_hash);
        free(token);
        return NULL;
    }
    return token;  // the caller owns the only copy of the raw token
}

char *rs_auth_username_for_token(rs_auth *auth, const char *token) {
    if (!auth || !token) return NULL;
    char *presented = hash_token(token);
    if (!presented) return NULL;
    time_t now = time(NULL);
    char *found = NULL;
    for (size_t i = 0; i < auth->len;) {
        if (auth->sessions[i].expires_at <= now) {
            drop_session(auth, i);  // reap expired sessions as we pass them
            continue;
        }
        if (!found && constant_time_equal(auth->sessions[i].token_hash, presented)) {
            found = rs_strdup(auth->sessions[i].username);
        }
        i++;
    }
    free(presented);
    return found;
}

void rs_auth_end_session(rs_auth *auth, const char *token) {
    if (!auth || !token) return;
    char *presented = hash_token(token);
    if (!presented) return;
    for (size_t i = 0; i < auth->len; i++) {
        if (strcmp(auth->sessions[i].token_hash, presented) == 0) {
            drop_session(auth, i);
            break;
        }
    }
    free(presented);
}

size_t rs_auth_end_sessions_for_user(rs_auth *auth, const char *username) {
    if (!auth || !username) return 0;
    size_t dropped = 0;
    for (size_t i = 0; i < auth->len;) {
        if (strcmp(auth->sessions[i].username, username) == 0) {
            drop_session(auth, i);
            dropped++;
            continue;
        }
        i++;
    }
    return dropped;
}

rs_json *rs_auth_export_sessions(rs_auth *auth) {
    rs_json *arr = rs_json_new_arr();
    if (!auth || !arr) return arr;
    time_t now = time(NULL);
    for (size_t i = 0; i < auth->len; i++) {
        if (auth->sessions[i].expires_at <= now) continue;
        rs_json *entry = rs_json_new_obj();
        rs_json_obj_set_str(entry, "tokenHash", auth->sessions[i].token_hash);
        rs_json_obj_set_str(entry, "username", auth->sessions[i].username);
        rs_json_obj_set(entry, "expiresAt",
                        rs_json_new_num((double)auth->sessions[i].expires_at - RS_APPLE_EPOCH_OFFSET));
        rs_json_arr_push(arr, entry);
    }
    return arr;
}

void rs_auth_import_sessions(rs_auth *auth, const rs_json *sessions) {
    if (!auth || !sessions) return;
    time_t now = time(NULL);
    for (size_t i = 0; i < rs_json_arr_len(sessions); i++) {
        const rs_json *entry = rs_json_arr_at(sessions, i);
        const char *token_hash = rs_json_obj_str(entry, "tokenHash", "");
        const char *username = rs_json_obj_str(entry, "username", "");
        time_t expires_at = (time_t)(rs_json_obj_num(entry, "expiresAt", 0) + RS_APPLE_EPOCH_OFFSET);
        if (!token_hash[0] || !username[0] || expires_at <= now) continue;
        char *copy = rs_strdup(token_hash);
        if (!copy) return;
        if (!push_session(auth, copy, username, expires_at)) { free(copy); return; }
    }
}

// --- login throttling ------------------------------------------------------

static rs_throttle *throttle_find(rs_auth *auth, const char *identity, bool create) {
    time_t now = time(NULL);
    for (size_t i = 0; i < auth->throttle_len;) {
        // Drop records nothing has touched for a full window, so a long-running
        // server does not accumulate one entry per guessed username forever.
        if (now - auth->throttles[i].last_failure > RS_THROTTLE_WINDOW) {
            free(auth->throttles[i].identity);
            memmove(&auth->throttles[i], &auth->throttles[i + 1],
                    (auth->throttle_len - i - 1) * sizeof(rs_throttle));
            auth->throttle_len--;
            continue;
        }
        if (strcmp(auth->throttles[i].identity, identity) == 0) return &auth->throttles[i];
        i++;
    }
    if (!create) return NULL;
    if (auth->throttle_len + 1 > auth->throttle_cap) {
        size_t cap = auth->throttle_cap ? auth->throttle_cap * 2 : 8;
        rs_throttle *grown = (rs_throttle *)realloc(auth->throttles, cap * sizeof(rs_throttle));
        if (!grown) return NULL;
        auth->throttles = grown;
        auth->throttle_cap = cap;
    }
    char *copy = rs_strdup(identity);
    if (!copy) return NULL;
    rs_throttle *slot = &auth->throttles[auth->throttle_len++];
    slot->identity = copy;
    slot->failures = 0;
    slot->last_failure = now;
    slot->retry_after = 0;
    return slot;
}

int rs_auth_throttle_delay(rs_auth *auth, const char *identity) {
    if (!auth || !identity) return 0;
    rs_throttle *slot = throttle_find(auth, identity, false);
    if (!slot) return 0;
    time_t now = time(NULL);
    return slot->retry_after > now ? (int)(slot->retry_after - now) : 0;
}

int rs_auth_throttle_record_failure(rs_auth *auth, const char *identity) {
    if (!auth || !identity) return 0;
    rs_throttle *slot = throttle_find(auth, identity, true);
    if (!slot) return 0;
    time_t now = time(NULL);
    slot->failures++;
    slot->last_failure = now;
    if (slot->failures <= RS_THROTTLE_FREE_ATTEMPTS) {
        slot->retry_after = 0;
        return 0;
    }
    // 2s, 4s, 8s … capped. Shifting by more than the width of the type is
    // undefined, so the exponent is clamped well before that.
    int steps = slot->failures - RS_THROTTLE_FREE_ATTEMPTS - 1;
    if (steps > 20) steps = 20;
    long delay = (long)RS_THROTTLE_BASE_DELAY << steps;
    if (delay > RS_THROTTLE_MAX_DELAY) delay = RS_THROTTLE_MAX_DELAY;
    slot->retry_after = now + delay;
    return (int)delay;
}

void rs_auth_throttle_reset(rs_auth *auth, const char *identity) {
    if (!auth || !identity) return;
    for (size_t i = 0; i < auth->throttle_len; i++) {
        if (strcmp(auth->throttles[i].identity, identity) == 0) {
            free(auth->throttles[i].identity);
            memmove(&auth->throttles[i], &auth->throttles[i + 1],
                    (auth->throttle_len - i - 1) * sizeof(rs_throttle));
            auth->throttle_len--;
            return;
        }
    }
}

// --- cookies ---------------------------------------------------------------

char *rs_auth_cookie_token(const char *cookie_header) {
    if (!cookie_header) return NULL;
    const char *name = RS_COOKIE_NAME;
    size_t name_len = strlen(name);
    const char *p = cookie_header;
    while (*p) {
        while (*p == ' ' || *p == ';') p++;  // skip separators and leading space
        const char *start = p;
        while (*p && *p != ';') p++;         // one "key=value" span
        const char *segment_end = p;
        const char *eq = memchr(start, '=', (size_t)(segment_end - start));
        if (eq) {
            size_t key_len = (size_t)(eq - start);
            if (key_len == name_len && memcmp(start, name, name_len) == 0) {
                size_t value_len = (size_t)(segment_end - (eq + 1));
                char *value = (char *)malloc(value_len + 1);
                if (!value) return NULL;
                memcpy(value, eq + 1, value_len);
                value[value_len] = '\0';
                return value;
            }
        }
    }
    return NULL;
}

char *rs_auth_set_cookie(const char *token, bool remember, bool secure) {
    // Matches AuthStore.setCookieHeader exactly.
    const char *base_fmt = RS_COOKIE_NAME "=%s; HttpOnly; Path=/; SameSite=Lax";
    size_t need = strlen(token) + strlen(base_fmt) + 48;
    char *out = (char *)malloc(need);
    if (!out) return NULL;
    int n = snprintf(out, need, base_fmt, token);
    if (n < 0 || (size_t)n >= need) { free(out); return NULL; }
    if (remember) {
        int written = snprintf(out + n, need - (size_t)n, "; Max-Age=%d", RS_SESSION_REMEMBER_SECONDS);
        if (written > 0) n += written;
    }
    if (secure) snprintf(out + n, need - (size_t)n, "; Secure");
    return out;
}

char *rs_auth_clear_cookie(bool secure) {
    return rs_strdup(secure
                     ? RS_COOKIE_NAME "=; HttpOnly; Path=/; Max-Age=0; SameSite=Lax; Secure"
                     : RS_COOKIE_NAME "=; HttpOnly; Path=/; Max-Age=0; SameSite=Lax");
}

// --- basic auth ------------------------------------------------------------

int rs_auth_parse_basic(const char *header, char **username, char **password) {
    *username = NULL;
    *password = NULL;
    if (!header || strncmp(header, "Basic ", 6) != 0) return -1;
    const char *encoded = header + 6;
    size_t decoded_cap = strlen(encoded);  // decode is always shorter than input
    uint8_t *decoded = (uint8_t *)malloc(decoded_cap + 1);
    if (!decoded) return -1;
    size_t decoded_len = 0;
    if (rs_base64_decode(encoded, decoded, decoded_cap, &decoded_len) != 0) {
        free(decoded);
        return -1;
    }
    decoded[decoded_len] = '\0';
    char *colon = (char *)memchr(decoded, ':', decoded_len);
    if (!colon) { free(decoded); return -1; }
    *colon = '\0';
    char *user = rs_strdup((char *)decoded);
    char *pass = rs_strdup(colon + 1);
    free(decoded);
    if (!user || !pass) { rs_free(user); rs_free(pass); return -1; }
    *username = user;
    *password = pass;
    return 0;
}
