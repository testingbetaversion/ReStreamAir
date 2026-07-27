#ifndef RS_HLS_DECRYPT_H
#define RS_HLS_DECRYPT_H

#include "rs_common.h"

#ifdef __cplusplus
extern "C" {
#endif

// HLS AES-128-CBC segment decryption
// Returns 0 on success, -1 on error.
int rs_hls_decrypt(const uint8_t *data, size_t len, const char *key_hex, const char *iv_hex, int sequence, uint8_t **out, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif  // RS_HLS_DECRYPT_H
