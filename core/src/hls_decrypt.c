#include "../include/rs_hls_decrypt.h"
#include "../include/rs_aes.h"
#include "rs_internal.h"
#include <string.h>
#include <stdlib.h>

static int hex_char_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex(const char *hex, uint8_t *out, size_t expected_len) {
    size_t hex_len = strlen(hex);
    if (hex_len != expected_len * 2) return -1;
    for (size_t i = 0; i < expected_len; i++) {
        int hi = hex_char_val(hex[2 * i]);
        int lo = hex_char_val(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static bool is_empty_or_spaces(const char *s) {
    if (!s) return true;
    while (*s) {
        if (!rs_is_space(*s)) return false;
        s++;
    }
    return true;
}

int rs_hls_decrypt(const uint8_t *data, size_t len, const char *key_hex, const char *iv_hex, int sequence, uint8_t **out, size_t *out_len) {
    if (!data || !key_hex || !out || !out_len) return -1;

    uint8_t key[16];
    if (parse_hex(key_hex, key, 16) < 0) return -1;

    uint8_t iv[16];
    if (is_empty_or_spaces(iv_hex)) {
        memset(iv, 0, 16);
        uint64_t seq = (sequence >= 0) ? (uint64_t)sequence : 0;
        for (int i = 15; i >= 8; i--) {
            iv[i] = seq & 0xFF;
            seq >>= 8;
        }
    } else {
        if (parse_hex(iv_hex, iv, 16) < 0) return -1;
    }

    uint8_t *buf = malloc(len);
    if (!buf) return -1;

    size_t decoded_len = 0;
    if (rs_aes_cbc_decrypt(key, 16, iv, 16, data, len, buf, &decoded_len) < 0) {
        free(buf);
        return -1;
    }

    if (decoded_len >= len) {
        // Valid PKCS7 padding removes 1 to 16 bytes. Unshortened implies bad padding.
        free(buf);
        return -1;
    }

    *out = buf;
    *out_len = decoded_len;
    return 0;
}
