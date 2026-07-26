#ifndef RS_AES_H
#define RS_AES_H

// Pure-C AES — the port of AES.swift. Provides the block cipher plus the two
// modes the decryptors use: streaming CTR (CENC) and CBC decrypt (HLS AES-128).
// Byte-for-byte identical to the Swift implementation, and so to CommonCrypto.

#include "rs_common.h"

#define RS_AES_BLOCK_LEN 16

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t round_keys[240];  // 4 * (14 + 1) words, the AES-256 maximum
    int rounds;
} rs_aes;

// key_len must be 16, 24 or 32 (AES-128/192/256). Returns 0, or -1 otherwise.
int rs_aes_init(rs_aes *ctx, const uint8_t *key, size_t key_len);

void rs_aes_encrypt_block(const rs_aes *ctx, const uint8_t in[RS_AES_BLOCK_LEN],
                          uint8_t out[RS_AES_BLOCK_LEN]);
void rs_aes_decrypt_block(const rs_aes *ctx, const uint8_t in[RS_AES_BLOCK_LEN],
                          uint8_t out[RS_AES_BLOCK_LEN]);

// Streaming AES-CTR with a big-endian full-128-bit counter, matching
// CommonCrypto's kCCModeCTR + kCCModeOptionCTR_BE. Keystream position carries
// across calls, which is what CENC's per-subsample decryption relies on.
typedef struct {
    rs_aes aes;
    uint8_t counter[RS_AES_BLOCK_LEN];
    uint8_t keystream[RS_AES_BLOCK_LEN];
    int keystream_pos;
} rs_aes_ctr;

// The IV is zero-padded (or truncated) to 16 bytes, as AESCTR does.
int rs_aes_ctr_init(rs_aes_ctr *ctx, const uint8_t *key, size_t key_len,
                    const uint8_t *iv, size_t iv_len);

// XORs len bytes of buf in place with the keystream.
void rs_aes_ctr_process(rs_aes_ctr *ctx, uint8_t *buf, size_t len);

// AES-CBC decrypt with PKCS7 padding removal, matching CommonCrypto's
// CCCrypt(kCCDecrypt, kCCOptionPKCS7Padding).
//
// `out` must have room for ct_len bytes. Returns 0 and writes the plaintext
// length to *out_len, or -1 if the key is not 16/24/32 bytes, the ciphertext is
// empty or not a multiple of 16, or the IV is not 16 bytes.
//
// Note the deliberately lenient padding check inherited from AES.swift: when
// the final byte is not in 1...16 the FULL plaintext is returned rather than an
// error. Existing streams depend on that, so it is not a bug to fix here.
int rs_aes_cbc_decrypt(const uint8_t *key, size_t key_len, const uint8_t *iv, size_t iv_len,
                       const uint8_t *ct, size_t ct_len, uint8_t *out, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif  // RS_AES_H
