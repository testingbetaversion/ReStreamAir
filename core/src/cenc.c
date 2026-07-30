#include "../include/rs_cenc.h"
#include "../include/rs_aes.h"
#include "rs_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

// Each byte is widened to uint32_t *before* shifting. Without the cast the
// operand is promoted to int, so any box whose size begins with a byte >= 0x80
// evaluates `b[off] << 24` as a signed overflow — undefined behaviour, and this
// runs on the size field of every box of every segment the engine decrypts.
// UBSan flags it as "left shift of 255 by 24 places cannot be represented in
// type 'int'"; an optimising build is entitled to do whatever it likes with the
// result, including feeding a nonsense length into the offsets computed below.
static uint16_t read_u16(const uint8_t *b, size_t off) {
    return (uint16_t)(((uint16_t)b[off] << 8) | (uint16_t)b[off+1]);
}
static uint32_t read_u32(const uint8_t *b, size_t off) {
    return ((uint32_t)b[off] << 24) | ((uint32_t)b[off+1] << 16)
         | ((uint32_t)b[off+2] << 8) | (uint32_t)b[off+3];
}
static uint64_t read_u64(const uint8_t *b, size_t off) {
    uint64_t v = 0;
    for (int i=0; i<8; i++) v = (v << 8) | b[off+i];
    return v;
}
static void write_u32(uint8_t *b, size_t off, uint32_t v) {
    b[off] = (v >> 24) & 0xFF; b[off+1] = (v >> 16) & 0xFF;
    b[off+2] = (v >> 8) & 0xFF; b[off+3] = v & 0xFF;
}
static void write_u64(uint8_t *b, size_t off, uint64_t v) {
    for (int i=0; i<8; i++) b[off+i] = (v >> (8 * (7 - i))) & 0xFF;
}
static void read_fourcc(const uint8_t *b, size_t off, char *out) {
    out[0] = b[off]; out[1] = b[off+1]; out[2] = b[off+2]; out[3] = b[off+3]; out[4] = '\0';
}
static void hex_encode(const uint8_t *bytes, size_t len, char *out) {
    for (size_t i = 0; i < len; i++) sprintf(out + i*2, "%02x", bytes[i]);
}
static int hex_char_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int hex_decode(const char *hex, uint8_t *out) {
    size_t len = strlen(hex);
    if (len % 2 != 0) return -1;
    for (size_t i = 0; i < len; i += 2) {
        int hi = hex_char_val(hex[i]); int lo = hex_char_val(hex[i+1]);
        if (hi < 0 || lo < 0) return -1;
        out[i/2] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}
static bool is_container_box(const char *type) {
    const char *c[] = {"moov", "trak", "mdia", "minf", "stbl", "moof", "traf", "mvex", "edts", "sinf", "schi", "udta", "meta"};
    for (size_t i = 0; i < sizeof(c)/sizeof(c[0]); i++) if (strcmp(type, c[i]) == 0) return true;
    return false;
}
static void free_boxes(rs_mp4_box *boxes, size_t count) {
    if (!boxes) return;
    for (size_t i = 0; i < count; i++) free_boxes(boxes[i].children, boxes[i].children_count);
    rs_free(boxes);
}
static rs_mp4_box* parse_boxes(const uint8_t *bytes, size_t start, size_t end, size_t *out_count) {
    *out_count = 0;
    size_t cap = 4;
    rs_mp4_box *boxes = malloc(cap * sizeof(rs_mp4_box));
    if (!boxes) return NULL;
    size_t offset = start;
    while (offset + 8 <= end) {
        uint32_t size32 = read_u32(bytes, offset);
        char type[5]; read_fourcc(bytes, offset + 4, type);
        int header_size = 8;
        size_t box_size = size32;
        if (size32 == 1) {
            if (offset + 16 > end) break;
            box_size = read_u64(bytes, offset + 8);
            header_size = 16;
        } else if (size32 == 0) {
            box_size = end - offset;
        }
        if (box_size < (size_t)header_size || box_size == 0 || offset + box_size > end) break;
        rs_mp4_box box; memset(&box, 0, sizeof(box));
        strcpy(box.type, type);
        box.start = offset; box.header_size = header_size;
        box.payload_start = offset + header_size; box.end = offset + box_size;
        if (is_container_box(type)) box.children = parse_boxes(bytes, box.payload_start, box.end, &box.children_count);
        if (*out_count >= cap) {
            cap *= 2;
            rs_mp4_box *nb = realloc(boxes, cap * sizeof(rs_mp4_box));
            if (!nb) { free_boxes(boxes, *out_count); free_boxes(box.children, box.children_count); return NULL; }
            boxes = nb;
        }
        boxes[(*out_count)++] = box;
        offset = box.end;
    }
    if (*out_count == 0) { rs_free(boxes); return NULL; }
    return boxes;
}
static rs_mp4_box* find_box(rs_mp4_box *boxes, size_t count, const char *type) {
    for (size_t i = 0; i < count; i++) if (strcmp(boxes[i].type, type) == 0) return &boxes[i];
    return NULL;
}

rs_cenc_keys rs_cenc_parse_keys(const char *text) {
    rs_cenc_keys res = {NULL, NULL, 0};
    if (!text) return res;
    char *copy = rs_strdup(text);
    if (!copy) return res;
    size_t cap = 4;
    res.kids = malloc(cap * sizeof(char*)); res.keys = malloc(cap * sizeof(uint8_t*));
    char *entry = copy;
    while (entry && *entry) {
        char *end = strpbrk(entry, "|\n\r");
        if (end) *end = '\0';
        char *colon = strchr(entry, ':');
        if (colon) {
            *colon = '\0';
            const char *kid_trim = NULL, *key_trim = NULL;
            size_t kid_len = rs_trim(entry, strlen(entry), true, &kid_trim);
            size_t key_len = rs_trim(colon+1, strlen(colon+1), true, &key_trim);
            if (kid_len > 0 && key_len > 0) {
                char *kid_clean = rs_trim_dup(kid_trim, kid_len, true);
                for (size_t i = 0; kid_clean[i]; i++) kid_clean[i] = (char)tolower((unsigned char)kid_clean[i]);
                char *key_clean = rs_trim_dup(key_trim, key_len, true);
                uint8_t kb[16];
                if (strlen(key_clean) == 32 && hex_decode(key_clean, kb) == 0) {
                    if (res.count >= cap) {
                        cap *= 2;
                        res.kids = realloc(res.kids, cap * sizeof(char*));
                        res.keys = realloc(res.keys, cap * sizeof(uint8_t*));
                    }
                    res.kids[res.count] = kid_clean;
                    res.keys[res.count] = malloc(16);
                    memcpy(res.keys[res.count], kb, 16);
                    res.count++;
                } else { rs_free(kid_clean); }
                rs_free(key_clean);
            }
        }
        entry = end ? end + 1 : NULL;
    }
    rs_free(copy);
    return res;
}
void rs_cenc_keys_free(rs_cenc_keys *keys) {
    if (!keys) return;
    for (size_t i = 0; i < keys->count; i++) { rs_free(keys->kids[i]); rs_free(keys->keys[i]); }
    rs_free(keys->kids); rs_free(keys->keys);
    keys->kids = NULL; keys->keys = NULL; keys->count = 0;
}

char** rs_cenc_detect_protection(const uint8_t *data, size_t len, size_t *count) {
    *count = 0;
    rs_strv kids = RS_STRV_INIT;
    size_t top_count = 0;
    rs_mp4_box *top = parse_boxes(data, 0, len, &top_count);
    if (!top) return NULL;
    rs_mp4_box *moov = find_box(top, top_count, "moov");
    if (moov) {
        for (size_t i = 0; i < moov->children_count; i++) {
            if (strcmp(moov->children[i].type, "trak") != 0) continue;
            rs_mp4_box *trak = &moov->children[i];
            rs_mp4_box *mdia = find_box(trak->children, trak->children_count, "mdia"); if (!mdia) continue;
            rs_mp4_box *minf = find_box(mdia->children, mdia->children_count, "minf"); if (!minf) continue;
            rs_mp4_box *stbl = find_box(minf->children, minf->children_count, "stbl"); if (!stbl) continue;
            rs_mp4_box *stsd = find_box(stbl->children, stbl->children_count, "stsd");
            if (!stsd || stsd->payload_start + 8 > stsd->end) continue;
            size_t enc_cnt = 0;
            rs_mp4_box *encs = parse_boxes(data, stsd->payload_start + 8, stsd->end, &enc_cnt);
            for (size_t j = 0; j < enc_cnt; j++) {
                rs_mp4_box *e = &encs[j];
                if (strcmp(e->type, "encv") != 0 && strcmp(e->type, "enca") != 0) continue;
                size_t fs = (strcmp(e->type, "encv") == 0) ? 86 : 36;
                size_t ns = e->start + fs;
                if (ns >= e->end) continue;
                size_t nc = 0; rs_mp4_box *nested = parse_boxes(data, ns, e->end, &nc);
                rs_mp4_box *sinf = find_box(nested, nc, "sinf");
                if (sinf) {
                    rs_mp4_box *schi = find_box(sinf->children, sinf->children_count, "schi");
                    if (schi) {
                        rs_mp4_box *tenc = find_box(schi->children, schi->children_count, "tenc");
                        if (tenc && tenc->payload_start + 24 <= tenc->end) {
                            char hex[33]; hex_encode(data + tenc->payload_start + 8, 16, hex);
                            bool fnd = false;
                            for (size_t k = 0; k < kids.len; k++) if (strcmp(kids.items[k], hex) == 0) fnd = true;
                            if (!fnd) rs_strv_push(&kids, hex);
                        }
                    }
                }
                free_boxes(nested, nc);
            }
            free_boxes(encs, enc_cnt);
        }
    }
    free_boxes(top, top_count);
    *count = kids.len;
    return kids.items;
}

static void retype_to_free(uint8_t *bytes, const rs_mp4_box *box) { memcpy(bytes + box->start + 4, "free", 4); }
static void neutralize_pssh(uint8_t *bytes, const rs_mp4_box *boxes, size_t count) {
    for (size_t i = 0; i < count; i++) if (strcmp(boxes[i].type, "pssh") == 0) retype_to_free(bytes, &boxes[i]);
}

uint8_t* rs_cenc_patch_init(const uint8_t *data, size_t len, size_t *out_len,
                            char **out_kid_hex, int *out_iv_size) {
    if (out_kid_hex) *out_kid_hex = NULL;
    if (out_iv_size) *out_iv_size = 0;
    uint8_t *bytes = malloc(len);
    if (!bytes) return NULL;
    memcpy(bytes, data, len);
    size_t cur_len = len;
    // Captured once from the first protected track's tenc, before the sinf that
    // contains it is excised: the default KID (to pick the key) and per-sample
    // IV size (to parse senc). Mirrors CENCDecryptor's defaultKIDHex/IVSize.
    char *cap_kid = NULL; int cap_iv = 0;
    while (true) {
        size_t tc = 0; rs_mp4_box *top = parse_boxes(bytes, 0, cur_len, &tc);
        if (!top) break;
        bool found = false;
        // Ancestor box spans whose size fields must shrink after the sinf is
        // spliced out. A named type (not an anonymous `struct {…}`) so the
        // compound-literal assignments below are the same type as the array.
        typedef struct { size_t s, e, hs; } box_span;
        box_span anc[10]; size_t ac = 0;
        size_t ss=0, se=0, eto=0; uint8_t nt[4];
        rs_mp4_box *moov = find_box(top, tc, "moov");
        if (moov) {
            for (size_t i = 0; !found && i < moov->children_count; i++) {
                if (strcmp(moov->children[i].type, "trak") != 0) continue;
                rs_mp4_box *trak = &moov->children[i];
                rs_mp4_box *mdia = find_box(trak->children, trak->children_count, "mdia"); if (!mdia) continue;
                rs_mp4_box *minf = find_box(mdia->children, mdia->children_count, "minf"); if (!minf) continue;
                rs_mp4_box *stbl = find_box(minf->children, minf->children_count, "stbl"); if (!stbl) continue;
                rs_mp4_box *stsd = find_box(stbl->children, stbl->children_count, "stsd");
                if (!stsd || stsd->payload_start + 8 > stsd->end) continue;
                size_t ec = 0; rs_mp4_box *entries = parse_boxes(bytes, stsd->payload_start + 8, stsd->end, &ec);
                for (size_t j = 0; !found && j < ec; j++) {
                    rs_mp4_box *e = &entries[j];
                    if (strcmp(e->type, "encv") != 0 && strcmp(e->type, "enca") != 0) continue;
                    size_t fs = (strcmp(e->type, "encv") == 0) ? 86 : 36;
                    size_t ns = e->start + fs; if (ns >= e->end) continue;
                    size_t nc = 0; rs_mp4_box *nested = parse_boxes(bytes, ns, e->end, &nc);
                    rs_mp4_box *sinf = find_box(nested, nc, "sinf");
                    if (sinf) {
                        rs_mp4_box *frma = find_box(sinf->children, sinf->children_count, "frma");
                        if (frma && frma->payload_start + 4 <= frma->end) {
                            memcpy(nt, bytes + frma->payload_start, 4);
                            // tenc: [ver/flags 4][_ 1][default_isProtected 1]
                            // [default_Per_Sample_IV_Size 1][default_KID 16].
                            if (!cap_kid) {
                                rs_mp4_box *schi = find_box(sinf->children, sinf->children_count, "schi");
                                rs_mp4_box *tenc = schi ? find_box(schi->children, schi->children_count, "tenc") : NULL;
                                if (tenc && tenc->payload_start + 24 <= tenc->end) {
                                    cap_iv = bytes[tenc->payload_start + 7];
                                    char hex[33]; hex_encode(bytes + tenc->payload_start + 8, 16, hex);
                                    cap_kid = rs_strdup(hex);
                                }
                            }
                            ss = sinf->start; se = sinf->end; eto = e->start + 4;
                            anc[0] = (box_span){moov->start, moov->end, moov->header_size};
                            anc[1] = (box_span){trak->start, trak->end, trak->header_size};
                            anc[2] = (box_span){mdia->start, mdia->end, mdia->header_size};
                            anc[3] = (box_span){minf->start, minf->end, minf->header_size};
                            anc[4] = (box_span){stbl->start, stbl->end, stbl->header_size};
                            anc[5] = (box_span){stsd->start, stsd->end, stsd->header_size};
                            anc[6] = (box_span){e->start, e->end, e->header_size};
                            ac = 7; found = true;
                        }
                    }
                    free_boxes(nested, nc);
                }
                free_boxes(entries, ec);
            }
        }
        free_boxes(top, tc);
        if (!found) break;
        size_t rem = se - ss;
        for (size_t i = 0; i < ac; i++) {
            size_t nw = (anc[i].e - anc[i].s) - rem;
            if (anc[i].hs == 16) write_u64(bytes, anc[i].s + 8, nw);
            else write_u32(bytes, anc[i].s, (uint32_t)nw);
        }
        memcpy(bytes + eto, nt, 4);
        memmove(bytes + ss, bytes + se, cur_len - se);
        cur_len -= rem;
    }
    size_t tc = 0; rs_mp4_box *top = parse_boxes(bytes, 0, cur_len, &tc);
    if (top) {
        rs_mp4_box *moov = find_box(top, tc, "moov");
        if (moov) neutralize_pssh(bytes, moov->children, moov->children_count);
        free_boxes(top, tc);
    }
    *out_len = cur_len;
    if (out_kid_hex) *out_kid_hex = cap_kid; else rs_free(cap_kid);
    if (out_iv_size) *out_iv_size = cap_iv;
    return bytes;
}

static void neutralize_cenc_boxes(uint8_t *bytes, const rs_mp4_box *traf) {
    for (size_t i = 0; i < traf->children_count; i++) {
        const rs_mp4_box *c = &traf->children[i];
        if (strcmp(c->type, "senc") == 0 || strcmp(c->type, "saiz") == 0 || strcmp(c->type, "saio") == 0) {
            retype_to_free(bytes, c);
        } else if (strcmp(c->type, "sbgp") == 0 || strcmp(c->type, "sgpd") == 0) {
            if (c->payload_start + 8 <= c->end) {
                char fcc[5]; read_fourcc(bytes, c->payload_start + 4, fcc);
                if (strcmp(fcc, "seig") == 0) retype_to_free(bytes, c);
            }
        }
    }
}

// --- Media-segment decryption (port of CENCDecryptor.decryptMediaSegment) ---

typedef struct { uint32_t clear; uint32_t enc; } cenc_sub;
typedef struct { uint8_t iv[16]; cenc_sub *subs; size_t nsub; } cenc_sample;
typedef struct { size_t start; size_t size; } cenc_range;

static void free_samples(cenc_sample *s, size_t n) {
    if (!s) return;
    for (size_t i = 0; i < n; i++) rs_free(s[i].subs);
    rs_free(s);
}

// Per-sample IVs (+ optional subsample clear/encrypted split) from a senc box.
// `iv_size` is the tenc default (senc carries no IV size of its own).
static cenc_sample* parse_senc(const uint8_t *b, const rs_mp4_box *senc, int iv_size, size_t *out_n) {
    *out_n = 0;
    size_t off = senc->payload_start;
    if (off + 8 > senc->end) return NULL;
    uint32_t flags = read_u32(b, off) & 0x00FFFFFF; off += 4;
    size_t count = read_u32(b, off); off += 4;
    bool has_sub = (flags & 0x2) != 0;
    int ivs = iv_size > 0 ? iv_size : 8; if (ivs > 16) ivs = 16;
    if (count == 0) return NULL;
    cenc_sample *res = calloc(count, sizeof(cenc_sample));
    if (!res) return NULL;
    for (size_t i = 0; i < count; i++) {
        if (off + (size_t)ivs > senc->end) { free_samples(res, i); return NULL; }
        memcpy(res[i].iv, b + off, (size_t)ivs); off += (size_t)ivs;
        if (has_sub) {
            if (off + 2 > senc->end) { free_samples(res, i); return NULL; }
            size_t nsub = read_u16(b, off); off += 2;
            if (nsub) {
                res[i].subs = malloc(nsub * sizeof(cenc_sub));
                if (!res[i].subs) { free_samples(res, i); return NULL; }
            }
            for (size_t j = 0; j < nsub; j++) {
                if (off + 6 > senc->end) { free_samples(res, i + 1); return NULL; }
                res[i].subs[j].clear = read_u16(b, off);
                res[i].subs[j].enc = read_u32(b, off + 2);
                off += 6;
            }
            res[i].nsub = nsub;
        }
    }
    *out_n = count;
    return res;
}

// Fallback aux-info source when there is no senc: sizes from saiz, and the byte
// offset (moof-relative) of the IV block from saio.
static cenc_sample* parse_saiz_saio(const uint8_t *b, const rs_mp4_box *saiz, const rs_mp4_box *saio,
                                    size_t moof_start, int iv_size, size_t total, size_t *out_n) {
    *out_n = 0;
    size_t p = saiz->payload_start;
    if (p + 4 > saiz->end) return NULL;
    uint32_t sf = read_u32(b, p) & 0x00FFFFFF; p += 4;
    if (sf & 0x1) p += 8;
    if (p + 5 > saiz->end) return NULL;
    uint8_t default_size = b[p]; p += 1;
    size_t count = read_u32(b, p); p += 4;
    if (count == 0) return NULL;
    uint8_t *sizes = malloc(count);
    if (!sizes) return NULL;
    if (default_size == 0) {
        if (p + count > saiz->end) { rs_free(sizes); return NULL; }
        memcpy(sizes, b + p, count); p += count;
    } else {
        memset(sizes, default_size, count);
    }

    size_t q = saio->payload_start;
    if (q + 4 > saio->end) { rs_free(sizes); return NULL; }
    uint8_t saio_ver = b[saio->payload_start];
    uint32_t of = read_u32(b, q) & 0x00FFFFFF; q += 4;
    if (of & 0x1) q += 8;
    if (q + 4 > saio->end) { rs_free(sizes); return NULL; }
    size_t entry_count = read_u32(b, q); q += 4;
    if (entry_count < 1) { rs_free(sizes); return NULL; }
    size_t first_off;
    if (saio_ver == 0) { if (q + 4 > saio->end) { rs_free(sizes); return NULL; } first_off = read_u32(b, q); }
    else { if (q + 8 > saio->end) { rs_free(sizes); return NULL; } first_off = read_u64(b, q); }

    int ivs = iv_size > 0 ? iv_size : 8; if (ivs > 16) ivs = 16;
    size_t offset = moof_start + first_off;
    cenc_sample *res = calloc(count, sizeof(cenc_sample));
    if (!res) { rs_free(sizes); return NULL; }
    for (size_t i = 0; i < count; i++) {
        size_t size = sizes[i];
        if (offset + (size_t)ivs > total) { free_samples(res, i); rs_free(sizes); return NULL; }
        memcpy(res[i].iv, b + offset, (size_t)ivs);
        size_t cursor = offset + (size_t)ivs;
        int remaining = (int)size - ivs;
        if (remaining >= 2) {
            if (cursor + 2 > total) { free_samples(res, i + 1); rs_free(sizes); return NULL; }
            size_t nsub = read_u16(b, cursor); cursor += 2;
            if (nsub) {
                res[i].subs = malloc(nsub * sizeof(cenc_sub));
                if (!res[i].subs) { free_samples(res, i + 1); rs_free(sizes); return NULL; }
            }
            for (size_t j = 0; j < nsub; j++) {
                if (cursor + 6 > total) { free_samples(res, i + 1); rs_free(sizes); return NULL; }
                res[i].subs[j].clear = read_u16(b, cursor);
                res[i].subs[j].enc = read_u32(b, cursor + 2);
                cursor += 6;
            }
            res[i].nsub = nsub;
        }
        offset += size;
    }
    rs_free(sizes);
    *out_n = count;
    return res;
}

// Sample byte ranges from trun (+ tfhd default_sample_size). Assumes
// default-base-is-moof (mandated by CMAF), so offsets are moof-relative.
static cenc_range* sample_byte_ranges(const uint8_t *b, const rs_mp4_box *trun, const rs_mp4_box *tfhd,
                                      size_t moof_start, size_t *out_n) {
    *out_n = 0;
    uint32_t default_sample_size = 0;
    if (tfhd) {
        size_t p = tfhd->payload_start;
        if (p + 8 > tfhd->end) return NULL;
        uint32_t flags = read_u32(b, p) & 0x00FFFFFF;
        p += 8; // full-box header (4) + track_ID (4)
        if (flags & 0x1) p += 8;
        if (flags & 0x2) p += 4;
        if (flags & 0x8) p += 4;
        if (flags & 0x10) {
            if (p + 4 > tfhd->end) return NULL;
            default_sample_size = read_u32(b, p); p += 4;
        }
    }
    size_t q = trun->payload_start;
    if (q + 8 > trun->end) return NULL;
    uint32_t tf = read_u32(b, q) & 0x00FFFFFF; q += 4;
    size_t count = read_u32(b, q); q += 4;
    int32_t data_offset = 0;
    if (tf & 0x1) { if (q + 4 > trun->end) return NULL; data_offset = (int32_t)read_u32(b, q); q += 4; }
    if (tf & 0x4) q += 4;
    bool has_dur = tf & 0x100, has_size = tf & 0x200, has_flags = tf & 0x400, has_cts = tf & 0x800;
    if (count == 0) return NULL;
    cenc_range *ranges = malloc(count * sizeof(cenc_range));
    if (!ranges) return NULL;
    long long cursor = (long long)moof_start + data_offset;
    for (size_t i = 0; i < count; i++) {
        if (has_dur) q += 4;
        uint32_t size = default_sample_size;
        if (has_size) { if (q + 4 > trun->end) { rs_free(ranges); return NULL; } size = read_u32(b, q); q += 4; }
        if (has_flags) q += 4;
        if (has_cts) q += 4;
        if (cursor < 0) { rs_free(ranges); return NULL; }
        ranges[i].start = (size_t)cursor;
        ranges[i].size = size;
        cursor += size;
    }
    *out_n = count;
    return ranges;
}

// AES-CTR one sample. An empty subsample list means the whole sample is
// encrypted (the norm for audio); the keystream carries across subsamples.
static void decrypt_sample(uint8_t *b, size_t total, size_t start, size_t size,
                           const uint8_t *iv, const cenc_sub *subs, size_t nsub, const uint8_t *key) {
    if (start + size > total) return;
    rs_aes_ctr ctr;
    if (rs_aes_ctr_init(&ctr, key, 16, iv, 16) != 0) return;
    size_t cursor = start;
    if (nsub == 0) {
        rs_aes_ctr_process(&ctr, b + cursor, size);
        return;
    }
    for (size_t j = 0; j < nsub; j++) {
        cursor += subs[j].clear;
        if (subs[j].enc > 0) {
            if (cursor + subs[j].enc > total) return;
            rs_aes_ctr_process(&ctr, b + cursor, subs[j].enc);
            cursor += subs[j].enc;
        }
    }
}

uint8_t* rs_cenc_decrypt_segment(const uint8_t *data, size_t len, size_t *out_len,
                                 const uint8_t *key, int iv_size) {
    uint8_t *bytes = malloc(len ? len : 1);
    if (!bytes) return NULL;
    memcpy(bytes, data, len);
    *out_len = len;

    size_t tc = 0; rs_mp4_box *top = parse_boxes(bytes, 0, len, &tc);
    if (!top) return bytes;
    rs_mp4_box *moof = find_box(top, tc, "moof");
    if (moof && key) {
        for (size_t i = 0; i < moof->children_count; i++) {
            rs_mp4_box *traf = &moof->children[i];
            if (strcmp(traf->type, "traf") != 0) continue;

            size_t ninfo = 0; cenc_sample *info = NULL;
            rs_mp4_box *senc = find_box(traf->children, traf->children_count, "senc");
            if (senc) {
                info = parse_senc(bytes, senc, iv_size, &ninfo);
            } else {
                rs_mp4_box *saiz = find_box(traf->children, traf->children_count, "saiz");
                rs_mp4_box *saio = find_box(traf->children, traf->children_count, "saio");
                if (saiz && saio) info = parse_saiz_saio(bytes, saiz, saio, moof->start, iv_size, len, &ninfo);
            }
            if (!info || ninfo == 0) { free_samples(info, ninfo); continue; }

            rs_mp4_box *trun = find_box(traf->children, traf->children_count, "trun");
            rs_mp4_box *tfhd = find_box(traf->children, traf->children_count, "tfhd");
            size_t nrange = 0;
            cenc_range *ranges = trun ? sample_byte_ranges(bytes, trun, tfhd, moof->start, &nrange) : NULL;
            // A sample-count mismatch means we can't safely align IVs to samples;
            // leave this traf encrypted (and its senc/saiz intact) rather than
            // strip the signalling off still-ciphertext samples.
            if (!ranges || nrange != ninfo) { rs_free(ranges); free_samples(info, ninfo); continue; }

            for (size_t s = 0; s < nrange; s++) {
                decrypt_sample(bytes, len, ranges[s].start, ranges[s].size,
                               info[s].iv, info[s].subs, info[s].nsub, key);
            }
            rs_free(ranges);
            free_samples(info, ninfo);
            neutralize_cenc_boxes(bytes, traf);
        }
        neutralize_pssh(bytes, moof->children, moof->children_count);
    }
    free_boxes(top, tc);
    return bytes;
}
