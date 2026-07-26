#include "rs_crypto.h"

#include <string.h>

// SHA-256 (FIPS 180-4)

static const uint32_t k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotr(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_compress(uint32_t h[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        const uint8_t *p = block + i * 4;
        w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + s1 + ch + k[i] + w[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void rs_sha256_init(rs_sha256_ctx *ctx) {
    static const uint32_t iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    memcpy(ctx->h, iv, sizeof(iv));
    ctx->block_len = 0;
    ctx->total_len = 0;
}

void rs_sha256_update(rs_sha256_ctx *ctx, const uint8_t *data, size_t len) {
    if (len == 0) return;
    ctx->total_len += len;

    if (ctx->block_len > 0) {
        size_t want = RS_SHA256_BLOCK_LEN - ctx->block_len;
        size_t take = len < want ? len : want;
        memcpy(ctx->block + ctx->block_len, data, take);
        ctx->block_len += take;
        data += take;
        len -= take;
        if (ctx->block_len < RS_SHA256_BLOCK_LEN) return;
        sha256_compress(ctx->h, ctx->block);
        ctx->block_len = 0;
    }
    while (len >= RS_SHA256_BLOCK_LEN) {
        sha256_compress(ctx->h, data);
        data += RS_SHA256_BLOCK_LEN;
        len -= RS_SHA256_BLOCK_LEN;
    }
    if (len > 0) {
        memcpy(ctx->block, data, len);
        ctx->block_len = len;
    }
}

void rs_sha256_final(rs_sha256_ctx *ctx, uint8_t out[RS_SHA256_DIGEST_LEN]) {
    uint64_t bit_len = ctx->total_len * 8;
    ctx->block[ctx->block_len++] = 0x80;
    if (ctx->block_len > 56) {
        memset(ctx->block + ctx->block_len, 0, RS_SHA256_BLOCK_LEN - ctx->block_len);
        sha256_compress(ctx->h, ctx->block);
        ctx->block_len = 0;
    }
    memset(ctx->block + ctx->block_len, 0, 56 - ctx->block_len);
    for (int i = 0; i < 8; i++) {
        ctx->block[56 + i] = (uint8_t)((bit_len >> (56 - 8 * i)) & 0xff);
    }
    sha256_compress(ctx->h, ctx->block);

    for (int i = 0; i < 8; i++) {
        out[i * 4 + 0] = (uint8_t)((ctx->h[i] >> 24) & 0xff);
        out[i * 4 + 1] = (uint8_t)((ctx->h[i] >> 16) & 0xff);
        out[i * 4 + 2] = (uint8_t)((ctx->h[i] >> 8) & 0xff);
        out[i * 4 + 3] = (uint8_t)(ctx->h[i] & 0xff);
    }
}

void rs_sha256(const uint8_t *data, size_t len, uint8_t out[RS_SHA256_DIGEST_LEN]) {
    rs_sha256_ctx ctx;
    rs_sha256_init(&ctx);
    rs_sha256_update(&ctx, data, len);
    rs_sha256_final(&ctx, out);
}

// HMAC-SHA256 (RFC 2104)

// The two half-finished hash states an HMAC key produces. PBKDF2 reuses them
// across all its iterations instead of re-padding and re-hashing the key
// hundreds of thousands of times; the derived bytes are unchanged.
typedef struct {
    rs_sha256_ctx inner;
    rs_sha256_ctx outer;
} hmac_key;

static void hmac_key_init(hmac_key *hk, const uint8_t *key, size_t key_len) {
    uint8_t padded[RS_SHA256_BLOCK_LEN];
    memset(padded, 0, sizeof(padded));
    if (key_len > RS_SHA256_BLOCK_LEN) {
        rs_sha256(key, key_len, padded);
    } else if (key_len > 0) {
        memcpy(padded, key, key_len);
    }

    uint8_t pad[RS_SHA256_BLOCK_LEN];
    for (size_t i = 0; i < RS_SHA256_BLOCK_LEN; i++) pad[i] = padded[i] ^ 0x36;
    rs_sha256_init(&hk->inner);
    rs_sha256_update(&hk->inner, pad, sizeof(pad));

    for (size_t i = 0; i < RS_SHA256_BLOCK_LEN; i++) pad[i] = padded[i] ^ 0x5c;
    rs_sha256_init(&hk->outer);
    rs_sha256_update(&hk->outer, pad, sizeof(pad));
}

static void hmac_key_compute(const hmac_key *hk, const uint8_t *msg, size_t msg_len,
                             uint8_t out[RS_SHA256_DIGEST_LEN]) {
    rs_sha256_ctx ctx = hk->inner;
    rs_sha256_update(&ctx, msg, msg_len);
    uint8_t inner_digest[RS_SHA256_DIGEST_LEN];
    rs_sha256_final(&ctx, inner_digest);

    ctx = hk->outer;
    rs_sha256_update(&ctx, inner_digest, sizeof(inner_digest));
    rs_sha256_final(&ctx, out);
}

void rs_hmac_sha256(const uint8_t *key, size_t key_len,
                    const uint8_t *msg, size_t msg_len,
                    uint8_t out[RS_SHA256_DIGEST_LEN]) {
    hmac_key hk;
    hmac_key_init(&hk, key, key_len);
    hmac_key_compute(&hk, msg, msg_len, out);
}

// PBKDF2-HMAC-SHA256 (RFC 8018)

int rs_pbkdf2_sha256(const uint8_t *password, size_t password_len,
                     const uint8_t *salt, size_t salt_len,
                     uint32_t iterations, uint8_t *out, size_t out_len) {
    if (iterations == 0 || out_len == 0) return -1;

    hmac_key hk;
    hmac_key_init(&hk, password, password_len);

    uint8_t u[RS_SHA256_DIGEST_LEN];
    uint8_t t[RS_SHA256_DIGEST_LEN];
    size_t written = 0;
    for (uint32_t block_index = 1; written < out_len; block_index++) {
        // U1 = HMAC(password, salt || INT32_BE(block_index))
        rs_sha256_ctx ctx = hk.inner;
        rs_sha256_update(&ctx, salt, salt_len);
        uint8_t counter[4] = {
            (uint8_t)((block_index >> 24) & 0xff), (uint8_t)((block_index >> 16) & 0xff),
            (uint8_t)((block_index >> 8) & 0xff), (uint8_t)(block_index & 0xff),
        };
        rs_sha256_update(&ctx, counter, sizeof(counter));
        uint8_t inner_digest[RS_SHA256_DIGEST_LEN];
        rs_sha256_final(&ctx, inner_digest);
        ctx = hk.outer;
        rs_sha256_update(&ctx, inner_digest, sizeof(inner_digest));
        rs_sha256_final(&ctx, u);

        memcpy(t, u, sizeof(t));
        for (uint32_t i = 1; i < iterations; i++) {
            hmac_key_compute(&hk, u, sizeof(u), u);
            for (size_t j = 0; j < sizeof(t); j++) t[j] ^= u[j];
        }

        size_t take = out_len - written;
        if (take > sizeof(t)) take = sizeof(t);
        memcpy(out + written, t, take);
        written += take;
    }
    return 0;
}
