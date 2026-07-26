#ifndef RS_CRYPTO_H
#define RS_CRYPTO_H

// SHA-256 / HMAC-SHA256 / PBKDF2-HMAC-SHA256 with no platform crypto
// dependency — the C port of PureCrypto in Crypto.swift. Output is
// byte-identical to that implementation (and so to CommonCrypto), which is what
// lets an admin password hashed by one build verify against another.

#include "rs_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RS_SHA256_DIGEST_LEN 32
#define RS_SHA256_BLOCK_LEN 64

typedef struct {
    uint32_t h[8];
    uint8_t block[RS_SHA256_BLOCK_LEN];
    size_t block_len;
    uint64_t total_len;
} rs_sha256_ctx;

// Streaming interface. The Swift original is one-shot; this is incremental so
// later phases can hash a whole media segment without buffering it.
void rs_sha256_init(rs_sha256_ctx *ctx);
void rs_sha256_update(rs_sha256_ctx *ctx, const uint8_t *data, size_t len);
void rs_sha256_final(rs_sha256_ctx *ctx, uint8_t out[RS_SHA256_DIGEST_LEN]);

void rs_sha256(const uint8_t *data, size_t len, uint8_t out[RS_SHA256_DIGEST_LEN]);

void rs_hmac_sha256(const uint8_t *key, size_t key_len,
                    const uint8_t *msg, size_t msg_len,
                    uint8_t out[RS_SHA256_DIGEST_LEN]);

// Derives out_len bytes. Returns 0, or -1 if iterations is 0 or out_len is 0.
int rs_pbkdf2_sha256(const uint8_t *password, size_t password_len,
                     const uint8_t *salt, size_t salt_len,
                     uint32_t iterations, uint8_t *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif  // RS_CRYPTO_H
