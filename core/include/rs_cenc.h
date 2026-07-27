#ifndef RS_CENC_H
#define RS_CENC_H

#include "rs_common.h"

#ifdef __cplusplus
extern "C" {
#endif

// MP4 box structure. The struct carries a tag (rs_mp4_box) so the recursive
// `children` member can name its own type — an anonymous `typedef struct {…}`
// leaves `struct rs_mp4_box` incomplete, which the parser can't subscript.
typedef struct rs_mp4_box {
    char type[5];      // FourCC + NUL
    size_t start;      // byte offset of box start
    int header_size;   // 8 or 16 (extended size)
    size_t payload_start;
    size_t end;        // byte offset past last byte
    // Children stored as flat array
    struct rs_mp4_box *children;
    size_t children_count;
} rs_mp4_box;

// Parse CENC key string "kid1:key1\nkid2:key2" into arrays
typedef struct {
    char **kids;       // hex KID strings (lowercase)
    uint8_t **keys;    // raw key bytes (16 bytes each)
    size_t count;
} rs_cenc_keys;

rs_cenc_keys rs_cenc_parse_keys(const char *text);
void rs_cenc_keys_free(rs_cenc_keys *keys);

// Detect protection: returns array of hex KIDs found in init segment
char** rs_cenc_detect_protection(const uint8_t *data, size_t len, size_t *count);

// Patch init segment: converts encv->avc1/enca->mp4a, removes sinf, retypes
// pssh->free. Also reports, via the optional out-params, the track's default
// KID (lowercase hex, caller frees with rs_free) and per-sample IV size from
// the tenc box — the two things the caller needs to pick the key and parse
// senc: pass those to rs_cenc_decrypt_segment. Either out-param may be NULL.
uint8_t* rs_cenc_patch_init(const uint8_t *data, size_t len, size_t *out_len,
                            char **out_kid_hex, int *out_iv_size);

// Decrypt a CENC 'cenc' (AES-128-CTR, full-sample or per-subsample) media
// fragment in place. `key` is the 16-byte clear key for the track's KID;
// `iv_size` is the tenc per-sample IV size from rs_cenc_patch_init (8 or 16;
// <=0 falls back to 8). Neutralizes senc/saiz/saio/seig and pssh once samples
// are clear. Returns malloc'd bytes (caller frees), or a copy of the input when
// there is nothing to decrypt.
uint8_t* rs_cenc_decrypt_segment(const uint8_t *data, size_t len, size_t *out_len,
                                 const uint8_t *key, int iv_size);

#ifdef __cplusplus
}
#endif

#endif  // RS_CENC_H
