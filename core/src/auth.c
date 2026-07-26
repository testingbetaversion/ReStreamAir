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

typedef struct {
    char *token;
    char *username;
    time_t expires_at;
} rs_session;

struct rs_auth {
    rs_session *sessions;
    size_t len;
    size_t cap;
};

rs_auth *rs_auth_create(void) {
    return (rs_auth *)calloc(1, sizeof(rs_auth));
}

void rs_auth_destroy(rs_auth *auth) {
    if (!auth) return;
    for (size_t i = 0; i < auth->len; i++) {
        free(auth->sessions[i].token);
        free(auth->sessions[i].username);
    }
    free(auth->sessions);
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
// hashes) purely internal, so its shape needn't match the Swift token.
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

static void drop_session(rs_auth *auth, size_t index) {
    free(auth->sessions[index].token);
    free(auth->sessions[index].username);
    memmove(&auth->sessions[index], &auth->sessions[index + 1],
            (auth->len - index - 1) * sizeof(rs_session));
    auth->len--;
}

char *rs_auth_create_session(rs_auth *auth, const char *username, bool remember) {
    if (!auth || !username) return NULL;
    char *token = make_token();
    if (!token) return NULL;
    char *username_copy = rs_strdup(username);
    if (!username_copy) { free(token); return NULL; }

    if (auth->len + 1 > auth->cap) {
        size_t cap = auth->cap ? auth->cap * 2 : 8;
        rs_session *grown = (rs_session *)realloc(auth->sessions, cap * sizeof(rs_session));
        if (!grown) { free(token); free(username_copy); return NULL; }
        auth->sessions = grown;
        auth->cap = cap;
    }
    long lifetime = remember ? RS_SESSION_REMEMBER_SECONDS : RS_SESSION_DEFAULT_SECONDS;
    auth->sessions[auth->len].token = token;
    auth->sessions[auth->len].username = username_copy;
    auth->sessions[auth->len].expires_at = time(NULL) + lifetime;
    auth->len++;
    // Hand back a copy so the caller owns its own string.
    return rs_strdup(token);
}

char *rs_auth_username_for_token(rs_auth *auth, const char *token) {
    if (!auth || !token) return NULL;
    time_t now = time(NULL);
    for (size_t i = 0; i < auth->len;) {
        if (auth->sessions[i].expires_at <= now) {
            drop_session(auth, i);  // reap expired sessions as we pass them
            continue;
        }
        if (strcmp(auth->sessions[i].token, token) == 0) {
            return rs_strdup(auth->sessions[i].username);
        }
        i++;
    }
    return NULL;
}

void rs_auth_end_session(rs_auth *auth, const char *token) {
    if (!auth || !token) return;
    for (size_t i = 0; i < auth->len; i++) {
        if (strcmp(auth->sessions[i].token, token) == 0) {
            drop_session(auth, i);
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

char *rs_auth_set_cookie(const char *token, bool remember) {
    // Matches AuthStore.setCookieHeader exactly.
    const char *base_fmt = RS_COOKIE_NAME "=%s; HttpOnly; Path=/; SameSite=Lax";
    size_t need = strlen(token) + strlen(base_fmt) + 32;
    char *out = (char *)malloc(need);
    if (!out) return NULL;
    int n = snprintf(out, need, base_fmt, token);
    if (remember && n > 0 && (size_t)n < need) {
        snprintf(out + n, need - (size_t)n, "; Max-Age=%d", RS_SESSION_REMEMBER_SECONDS);
    }
    return out;
}

char *rs_auth_clear_cookie(void) {
    return rs_strdup(RS_COOKIE_NAME "=; HttpOnly; Path=/; Max-Age=0; SameSite=Lax");
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
