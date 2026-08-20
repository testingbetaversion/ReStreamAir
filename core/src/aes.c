#include "rs_aes.h"

#include <string.h>

#include "aes_tables.h"

// State is column-major throughout: index = col * 4 + row, as FIPS-197 lays it out.

int rs_aes_init(rs_aes *ctx, const uint8_t *key, size_t key_len) {
    switch (key_len) {
        case 16: ctx->rounds = 10; break;
        case 24: ctx->rounds = 12; break;
        case 32: ctx->rounds = 14; break;
        default: return -1;
    }

    const size_t key_words = key_len / 4;
    const size_t total_words = 4 * ((size_t)ctx->rounds + 1);
    uint8_t *w = ctx->round_keys;
    memset(w, 0, sizeof(ctx->round_keys));
    memcpy(w, key, key_len);

    size_t rcon_index = 0;
    for (size_t i = key_words; i < total_words; i++) {
        uint8_t temp[4] = {w[(i - 1) * 4], w[(i - 1) * 4 + 1], w[(i - 1) * 4 + 2], w[(i - 1) * 4 + 3]};
        if (i % key_words == 0) {
            uint8_t rot = temp[0];  // RotWord
            temp[0] = temp[1]; temp[1] = temp[2]; temp[2] = temp[3]; temp[3] = rot;
            for (int j = 0; j < 4; j++) temp[j] = rs_sbox[temp[j]];  // SubWord
            temp[0] ^= rs_rcon[rcon_index++];
        } else if (key_words > 6 && i % key_words == 4) {
            for (int j = 0; j < 4; j++) temp[j] = rs_sbox[temp[j]];  // AES-256 extra SubWord
        }
        for (size_t j = 0; j < 4; j++) w[i * 4 + j] = w[(i - key_words) * 4 + j] ^ temp[j];
    }
    return 0;
}

static void add_round_key(const rs_aes *ctx, uint8_t s[16], int round) {
    for (int i = 0; i < 16; i++) s[i] ^= ctx->round_keys[round * 16 + i];
}

static void shift_rows(uint8_t s[16]) {
    uint8_t original[16];
    memcpy(original, s, 16);
    for (int row = 1; row < 4; row++) {
        for (int col = 0; col < 4; col++) s[col * 4 + row] = original[((col + row) % 4) * 4 + row];
    }
}

static void inv_shift_rows(uint8_t s[16]) {
    uint8_t original[16];
    memcpy(original, s, 16);
    for (int row = 1; row < 4; row++) {
        for (int col = 0; col < 4; col++) s[col * 4 + row] = original[((col + 4 - row) % 4) * 4 + row];
    }
}

static void mix_columns(uint8_t s[16]) {
    for (int c = 0; c < 4; c++) {
        uint8_t a0 = s[c * 4], a1 = s[c * 4 + 1], a2 = s[c * 4 + 2], a3 = s[c * 4 + 3];
        s[c * 4]     = (uint8_t)(rs_mul2[a0] ^ rs_mul3[a1] ^ a2 ^ a3);
        s[c * 4 + 1] = (uint8_t)(a0 ^ rs_mul2[a1] ^ rs_mul3[a2] ^ a3);
        s[c * 4 + 2] = (uint8_t)(a0 ^ a1 ^ rs_mul2[a2] ^ rs_mul3[a3]);
        s[c * 4 + 3] = (uint8_t)(rs_mul3[a0] ^ a1 ^ a2 ^ rs_mul2[a3]);
    }
}

static void inv_mix_columns(uint8_t s[16]) {
    for (int c = 0; c < 4; c++) {
        uint8_t a0 = s[c * 4], a1 = s[c * 4 + 1], a2 = s[c * 4 + 2], a3 = s[c * 4 + 3];
        s[c * 4]     = (uint8_t)(rs_mul14[a0] ^ rs_mul11[a1] ^ rs_mul13[a2] ^ rs_mul9[a3]);
        s[c * 4 + 1] = (uint8_t)(rs_mul9[a0] ^ rs_mul14[a1] ^ rs_mul11[a2] ^ rs_mul13[a3]);
        s[c * 4 + 2] = (uint8_t)(rs_mul13[a0] ^ rs_mul9[a1] ^ rs_mul14[a2] ^ rs_mul11[a3]);
        s[c * 4 + 3] = (uint8_t)(rs_mul11[a0] ^ rs_mul13[a1] ^ rs_mul9[a2] ^ rs_mul14[a3]);
    }
}

void rs_aes_encrypt_block(const rs_aes *ctx, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    memcpy(s, in, 16);
    add_round_key(ctx, s, 0);
    for (int round = 1; round < ctx->rounds; round++) {
        for (int i = 0; i < 16; i++) s[i] = rs_sbox[s[i]];
        shift_rows(s);
        mix_columns(s);
        add_round_key(ctx, s, round);
    }
    for (int i = 0; i < 16; i++) s[i] = rs_sbox[s[i]];
    shift_rows(s);
    add_round_key(ctx, s, ctx->rounds);
    memcpy(out, s, 16);
}

void rs_aes_decrypt_block(const rs_aes *ctx, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    memcpy(s, in, 16);
    add_round_key(ctx, s, ctx->rounds);
    for (int round = ctx->rounds - 1; round >= 1; round--) {
        inv_shift_rows(s);
        for (int i = 0; i < 16; i++) s[i] = rs_inv_sbox[s[i]];
        add_round_key(ctx, s, round);
        inv_mix_columns(s);
    }
    inv_shift_rows(s);
    for (int i = 0; i < 16; i++) s[i] = rs_inv_sbox[s[i]];
    add_round_key(ctx, s, 0);
    memcpy(out, s, 16);
}

int rs_aes_ctr_init(rs_aes_ctr *ctx, const uint8_t *key, size_t key_len,
                    const uint8_t *iv, size_t iv_len) {
    if (rs_aes_init(&ctx->aes, key, key_len) != 0) return -1;
    memset(ctx->counter, 0, sizeof(ctx->counter));
    if (iv && iv_len > 0) {
        size_t take = iv_len < RS_AES_BLOCK_LEN ? iv_len : (size_t)RS_AES_BLOCK_LEN;
        memcpy(ctx->counter, iv, take);
    }
    memset(ctx->keystream, 0, sizeof(ctx->keystream));
    ctx->keystream_pos = RS_AES_BLOCK_LEN;  // exhausted: generate a fresh block first
    return 0;
}

static void increment_counter(rs_aes_ctr *ctx) {
    for (int i = RS_AES_BLOCK_LEN - 1; i >= 0; i--) {
        if (++ctx->counter[i] != 0) break;
    }
}

void rs_aes_ctr_process(rs_aes_ctr *ctx, uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (ctx->keystream_pos == RS_AES_BLOCK_LEN) {
            rs_aes_encrypt_block(&ctx->aes, ctx->counter, ctx->keystream);
            ctx->keystream_pos = 0;
            increment_counter(ctx);
        }
        buf[i] ^= ctx->keystream[ctx->keystream_pos++];
    }
}

int rs_aes_cbc_decrypt(const uint8_t *key, size_t key_len, const uint8_t *iv, size_t iv_len,
                       const uint8_t *ct, size_t ct_len, uint8_t *out, size_t *out_len) {
    rs_aes aes;
    if (rs_aes_init(&aes, key, key_len) != 0) return -1;
    if (ct_len == 0 || ct_len % RS_AES_BLOCK_LEN != 0 || iv_len != RS_AES_BLOCK_LEN) return -1;

    uint8_t previous[RS_AES_BLOCK_LEN];
    memcpy(previous, iv, RS_AES_BLOCK_LEN);
    for (size_t offset = 0; offset < ct_len; offset += RS_AES_BLOCK_LEN) {
        uint8_t plain[RS_AES_BLOCK_LEN];
        rs_aes_decrypt_block(&aes, ct + offset, plain);
        for (int i = 0; i < RS_AES_BLOCK_LEN; i++) plain[i] ^= previous[i];
        memcpy(previous, ct + offset, RS_AES_BLOCK_LEN);
        memcpy(out + offset, plain, RS_AES_BLOCK_LEN);
    }

    // Strip PKCS7 padding — but see the header: an invalid pad byte yields the
    // full plaintext rather than an error (see the note in rs_aes.h).
    uint8_t pad = out[ct_len - 1];
    *out_len = (pad >= 1 && pad <= 16 && ct_len >= pad) ? ct_len - pad : ct_len;
    return 0;
}
