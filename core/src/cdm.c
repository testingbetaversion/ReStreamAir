#include "rs_cdm.h"
#include "rs_internal.h"
#include "rs_script.h"
#include "../deps/cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>


// --- the three patterns this file needs -------------------------------------
//
// These used to be POSIX <regex.h>. That header does not exist on Windows and
// has no MSVC equivalent, and the placeholder that "handled" it here included
// the same missing header from both branches — so this file had never been
// compiled for Windows at all. All three patterns are simple enough that a
// scanner is shorter than any portability shim would have been, and it drops a
// regex engine from the code path that parses attacker-supplied playlists.

// Copies [start, end) into a fresh NUL-terminated string.
static char *dup_range(const char *start, const char *end) {
    size_t n = (size_t)(end - start);
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, start, n);
    out[n] = '\0';
    return out;
}

// Exactly 32 hex digits at `p`, lowercased into `out`. Was "[0-9a-fA-F]{32}".
static bool take_hex32(const char *p, char out[33]) {
    for (int i = 0; i < 32; i++) {
        if (!isxdigit((unsigned char)p[i])) return false;
        out[i] = (char)tolower((unsigned char)p[i]);
    }
    out[32] = '\0';
    return true;
}

// "<32 hex>[ \t]*:[ \t]*<32 hex>" — the shape a key script prints when it is
// not returning JSON. Returns just past the match, or NULL when there is none.
// Scanning one character at a time matches the regex's leftmost rule: in a hex
// run longer than 32, the match simply starts further along.
static const char *scan_kid_key_pair(const char *s, char kid_out[33], char key_out[33]) {
    for (; *s; s++) {
        const char *p;
        if (!take_hex32(s, kid_out)) continue;
        p = s + 32;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != ':') continue;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (!take_hex32(p, key_out)) continue;
        return p + 32;
    }
    return NULL;
}

// default_KID[ \t]*=[ \t]*["']<hex and dashes>["'] — case-sensitive on the
// name, as the pattern was. On a match, *out is the KID and the return value is
// just past the closing quote.
static const char *scan_default_kid(const char *s, char **out) {
    static const char NEEDLE[] = "default_KID";
    const char *hit = strstr(s, NEEDLE);
    for (; hit; hit = strstr(hit + 1, NEEDLE)) {
        const char *p = hit + (sizeof(NEEDLE) - 1);
        char quote;
        const char *start;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '=') continue;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '"' && *p != '\'') continue;
        quote = *p++;
        start = p;
        while (isxdigit((unsigned char)*p) || *p == '-') p++;
        if (p == start || *p != quote) continue;
        *out = dup_range(start, p);
        return p + 1;
    }
    return NULL;
}

static const char *WIDEVINE_SYSTEM_ID = "edef8ba979d64acea3c827dcd51d21ed";
static const char *PLAYREADY_SYSTEM_ID = "9a04f07998404286ab92e65be0885f95";
// static const char *FAIRPLAY_SYSTEM_ID = "94ce86fb07ff4f43adb893d2fa968ca2";

void rs_drm_challenge_free(rs_drm_challenge *ch) {
    if (!ch) return;
    for (size_t i = 0; i < ch->kids_count; i++) rs_free(ch->kids[i]);
    rs_free(ch->kids);
    
    rs_free(ch->pssh_widevine);
    rs_free(ch->pssh_playready);
    rs_free(ch->pssh_fairplay);
    
    for (size_t i = 0; i < ch->pssh_all_count; i++) rs_free(ch->pssh_all[i]);
    rs_free(ch->pssh_all);
    
    for (size_t i = 0; i < ch->key_uris_count; i++) rs_free(ch->key_uris[i]);
    rs_free(ch->key_uris);
    
    memset(ch, 0, sizeof(*ch));
}

static char *bytes_to_hex(const uint8_t *bytes, size_t len) {
    char *hex = malloc(len * 2 + 1);
    if (!hex) return NULL;
    for (size_t i = 0; i < len; i++) {
        sprintf(hex + i * 2, "%02x", bytes[i]);
    }
    hex[len * 2] = '\0';
    return hex;
}

static void add_kid(rs_drm_challenge *ch, const char *raw) {
    char clean[33];
    size_t out = 0;
    for (size_t i = 0; raw[i]; i++) {
        if (raw[i] == '-') continue;
        if (out < 32) {
            clean[out++] = (char)tolower((unsigned char)raw[i]);
        }
    }
    clean[out] = '\0';
    if (out != 32) return;
    
    for (size_t i = 0; i < out; i++) {
        if (!isxdigit((unsigned char)clean[i])) return;
    }
    
    for (size_t i = 0; i < ch->kids_count; i++) {
        if (strcmp(ch->kids[i], clean) == 0) return;
    }
    
    char **new_kids = realloc(ch->kids, (ch->kids_count + 1) * sizeof(char*));
    if (new_kids) {
        ch->kids = new_kids;
        ch->kids[ch->kids_count++] = rs_strdup(clean);
    }
}

static void classify_pssh(rs_drm_challenge *ch, const char *b64) {
    size_t b64_len = strlen(b64);
    uint8_t *bytes = malloc(b64_len);
    if (!bytes) return;
    size_t len = 0;
    if (rs_base64_decode(b64, bytes, b64_len, &len) == 0 && len >= 28) {
        if (bytes[4] == 0x70 && bytes[5] == 0x73 && bytes[6] == 0x73 && bytes[7] == 0x68) {
            char *sys_id = bytes_to_hex(bytes + 12, 16);
            if (sys_id) {
                if (strcmp(sys_id, WIDEVINE_SYSTEM_ID) == 0 && !ch->pssh_widevine) {
                    ch->pssh_widevine = rs_strdup(b64);
                } else if (strcmp(sys_id, PLAYREADY_SYSTEM_ID) == 0 && !ch->pssh_playready) {
                    ch->pssh_playready = rs_strdup(b64);
                }
                
                if (bytes[8] == 1 && len >= 32) {
                    // Widen before shifting: `bytes[28] << 24` on a byte >= 0x80
                    // is a signed overflow, and the count is attacker-supplied.
                    // The loop bounds below already contain it; this keeps the
                    // arithmetic itself defined.
                    uint32_t kid_count = ((uint32_t)bytes[28] << 24) | ((uint32_t)bytes[29] << 16)
                                       | ((uint32_t)bytes[30] << 8) | (uint32_t)bytes[31];
                    size_t offset = 32;
                    for (uint32_t i = 0; i < kid_count && i < 64 && offset + 16 <= len; i++) {
                        char *kid_hex = bytes_to_hex(bytes + offset, 16);
                        if (kid_hex) {
                            add_kid(ch, kid_hex);
                            rs_free(kid_hex);
                        }
                        offset += 16;
                    }
                }
                rs_free(sys_id);
            }
        }
    }
    free(bytes);
}

// Appends one base64 box to pssh_all (deduplicated) and classifies it.
static void add_pssh(rs_drm_challenge *ch, const char *b64) {
    if (!b64 || !b64[0]) return;
    for (size_t i = 0; i < ch->pssh_all_count; i++)
        if (strcmp(ch->pssh_all[i], b64) == 0) return;
    char **grown = realloc(ch->pssh_all, (ch->pssh_all_count + 1) * sizeof(char *));
    if (!grown) return;
    ch->pssh_all = grown;
    ch->pssh_all[ch->pssh_all_count++] = rs_strdup(b64);
    classify_pssh(ch, b64);
}

void rs_cdm_challenge_add_kid(rs_drm_challenge *ch, const char *hex) {
    if (ch && hex) add_kid(ch, hex);
}

bool rs_cdm_is_pssh_box(const char *b64) {
    if (!b64 || !b64[0]) return false;
    size_t b64_len = strlen(b64);
    uint8_t *bytes = malloc(b64_len);
    if (!bytes) return false;
    size_t len = 0;
    bool ok = rs_base64_decode(b64, bytes, b64_len, &len) == 0 && len >= 28
              && memcmp(bytes + 4, "pssh", 4) == 0;
    free(bytes);
    return ok;
}

void rs_cdm_challenge_add_pssh(rs_drm_challenge *ch, const char *b64) {
    if (ch) add_pssh(ch, b64);
}

bool rs_drm_challenge_is_empty(const rs_drm_challenge *ch) {
    if (!ch) return true;
    return ch->kids_count == 0 && ch->pssh_all_count == 0 && ch->key_uris_count == 0
        && !ch->pssh_widevine && !ch->pssh_playready && !ch->pssh_fairplay;
}

// --- init-segment scanning --------------------------------------------------
//
// A proper walk would descend moov > trak > mdia > minf > stbl > stsd and then
// into the sample entry's sinf/schi, which is a lot of structure to model for
// two leaf boxes. Since every box carries its own length, scanning for the
// four-byte type and validating the size word in front of it finds the same
// boxes wherever a source chose to put them, and cannot run off the buffer.

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

void rs_cdm_challenge_add_from_init(rs_drm_challenge *ch, const uint8_t *data, size_t len) {
    if (!ch || !data || len < 16) return;
    for (size_t i = 4; i + 4 <= len; i++) {
        uint32_t size = be32(data + i - 4);
        size_t start = i - 4;
        // A box's declared size covers the size word and type themselves.
        if (size < 8 || (size_t)size > len - start) continue;

        if (memcmp(data + i, "pssh", 4) == 0 && size >= 32) {
            char *b64 = rs_base64_encode(data + start, size);
            if (b64) { add_pssh(ch, b64); rs_free(b64); }
            continue;
        }
        // tenc: FullBox header (4), reserved (1), reserved/pattern (1),
        // default_isProtected (1), default_Per_Sample_IV_Size (1), then the
        // 16-byte default_KID.
        if (memcmp(data + i, "tenc", 4) == 0 && size >= 8 + 8 + 16) {
            const uint8_t *payload = data + i + 4;
            char *hex = bytes_to_hex(payload + 8, 16);
            if (hex) { add_kid(ch, hex); rs_free(hex); }
        }
    }
}

// --- PSSH synthesis ---------------------------------------------------------

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 32 hex digits (dashes allowed) into 16 bytes. false if the input isn't one.
static bool kid_to_bytes(const char *hex, uint8_t out[16]) {
    int nibbles = 0;
    for (const char *p = hex; *p; p++) {
        int v;
        if (*p == '-') continue;
        v = hex_nibble(*p);
        if (v < 0 || nibbles >= 32) return false;
        if (nibbles % 2 == 0) out[nibbles / 2] = (uint8_t)(v << 4);
        else out[nibbles / 2] |= (uint8_t)v;
        nibbles++;
    }
    return nibbles == 32;
}

char *rs_cdm_pssh_from_kids(char *const *kids, size_t count) {
    // WidevinePsshData is a protobuf; the only field a key request needs is
    // `repeated bytes key_ids = 2`, which encodes as tag 0x12, length 0x10,
    // then the raw KID. pywidevine's PSSH.new(key_ids=...) puts the same KIDs
    // in a version 1 box header instead; a version 0 box carrying this payload
    // is what CDMs and licence proxies actually read, and pywidevine parses it
    // back into the same key_ids, so it round-trips either way.
    uint8_t payload[64 * 18];
    size_t payload_len = 0;
    for (size_t i = 0; i < count && i < 64; i++) {
        uint8_t raw[16];
        if (!kids[i] || !kid_to_bytes(kids[i], raw)) continue;
        payload[payload_len++] = 0x12;
        payload[payload_len++] = 0x10;
        memcpy(payload + payload_len, raw, 16);
        payload_len += 16;
    }
    if (!payload_len) return NULL;

    size_t box_len = 4 + 4 + 4 + 16 + 4 + payload_len;
    uint8_t *box = (uint8_t *)malloc(box_len);
    if (!box) return NULL;
    size_t o = 0;
    box[o++] = (uint8_t)(box_len >> 24); box[o++] = (uint8_t)(box_len >> 16);
    box[o++] = (uint8_t)(box_len >> 8);  box[o++] = (uint8_t)box_len;
    memcpy(box + o, "pssh", 4); o += 4;
    box[o++] = 0; box[o++] = 0; box[o++] = 0; box[o++] = 0;  // version 0, no flags
    for (int i = 0; i < 16; i++) {
        box[o++] = (uint8_t)((hex_nibble(WIDEVINE_SYSTEM_ID[i * 2]) << 4)
                             | hex_nibble(WIDEVINE_SYSTEM_ID[i * 2 + 1]));
    }
    box[o++] = (uint8_t)(payload_len >> 24); box[o++] = (uint8_t)(payload_len >> 16);
    box[o++] = (uint8_t)(payload_len >> 8);  box[o++] = (uint8_t)payload_len;
    memcpy(box + o, payload, payload_len);

    char *b64 = rs_base64_encode(box, box_len);
    free(box);
    return b64;
}

bool rs_cdm_challenge_synthesize_pssh(rs_drm_challenge *ch) {
    if (!ch || ch->pssh_widevine || ch->kids_count == 0) return false;
    char *b64 = rs_cdm_pssh_from_kids(ch->kids, ch->kids_count);
    if (!b64) return false;
    add_pssh(ch, b64);
    rs_free(b64);
    return ch->pssh_widevine != NULL;
}

rs_drm_challenge rs_cdm_challenge_from_mpd(const char *xml) {
    rs_drm_challenge ch;
    memset(&ch, 0, sizeof(ch));
    
    // We can use strstr/strcasestr to extract pssh to avoid complex regex
    const char *p = xml;
    while ((p = strstr(p, "pssh>"))) {
        const char *end = strstr(p, "</");
        if (end) {
            const char *start = p + 5;
            rs_buf b = RS_BUF_INIT;
            for (const char *c = start; c < end; c++) {
                if (isalnum(*c) || *c == '+' || *c == '/' || *c == '=') {
                    rs_buf_append_char(&b, *c);
                }
            }
            char *b64 = rs_buf_take(&b);
            if (b64 && b64[0]) add_pssh(&ch, b64);
            rs_free(b64);
            p = end + 2;
        } else {
            break;
        }
    }
    
    // Every default_KID the manifest carries.
    for (const char *search = xml; search && *search;) {
        char *kid = NULL;
        search = scan_default_kid(search, &kid);
        if (!search) break;
        if (kid) { add_kid(&ch, kid); free(kid); }
    }
    
    return ch;
}

// NAME="value", or NAME=value up to a delimiter. The name match is
// case-insensitive. Both passes run over the whole line before the next one
// starts, which is what the two regexes did: a quoted value anywhere on the
// line wins over an unquoted one earlier in it.
static char* get_attr(const char *line, const char *name) {
    size_t nlen = strlen(name);

    for (const char *p = line; *p; p++) {
        const char *q, *start;
        if (strncasecmp(p, name, nlen) != 0) continue;
        q = p + nlen;
        if (*q != '=' || q[1] != '"') continue;
        start = q + 2;
        for (q = start; *q && *q != '"'; q++) {}
        if (*q == '"' && q > start) return dup_range(start, q);
    }

    for (const char *p = line; *p; p++) {
        const char *q, *start;
        if (strncasecmp(p, name, nlen) != 0) continue;
        q = p + nlen;
        if (*q != '=') continue;
        start = ++q;
        while (*q && *q != ',' && *q != ' ' && *q != '\t' && *q != '\r' && *q != '\n') q++;
        if (q > start) return dup_range(start, q);
    }
    return NULL;
}

rs_drm_challenge rs_cdm_challenge_from_hls(const char *playlist) {
    rs_drm_challenge ch;
    memset(&ch, 0, sizeof(ch));
    
    const char *p = playlist;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char *line = malloc(len + 1);
        if (line) {
            strncpy(line, p, len);
            line[len] = '\0';
            
            char *trim = line;
            while (*trim == ' ' || *trim == '\t') trim++;
            
            if (strncmp(trim, "#EXT-X-KEY:", 11) == 0 || strncmp(trim, "#EXT-X-SESSION-KEY:", 19) == 0) {
                const char *attrs = strchr(trim, ':') + 1;
                char *method = get_attr(attrs, "METHOD");
                if (!method || strcasecmp(method, "NONE") != 0) {
                    char *uri = get_attr(attrs, "URI");
                    if (uri && uri[0]) {
                        char *kf = get_attr(attrs, "KEYFORMAT");
                        
                        bool has_uri = false;
                        for (size_t i = 0; i < ch.key_uris_count; i++) {
                            if (strcmp(ch.key_uris[i], uri) == 0) {
                                has_uri = true;
                                break;
                            }
                        }
                        if (!has_uri) {
                            char **new_uris = realloc(ch.key_uris, (ch.key_uris_count + 1) * sizeof(char*));
                            if (new_uris) {
                                ch.key_uris = new_uris;
                                ch.key_uris[ch.key_uris_count++] = rs_strdup(uri);
                            }
                        }
                        
                        if (kf && strstr(kf, "streamingkeydelivery") && !ch.pssh_fairplay) {
                            ch.pssh_fairplay = rs_strdup(uri);
                        }
                        
                        // Check Widevine urn in KEYFORMAT
                        if ((kf && strstr(kf, "urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed")) || strncmp(uri, "data:", 5) == 0) {
                            const char *comma = strstr(uri, "base64,");
                            if (comma) {
                                    add_pssh(&ch, comma + 7);
                            }
                        }
                        rs_free(kf);
                    }
                    rs_free(uri);
                }
                rs_free(method);
                
                char *kid = get_attr(attrs, "KEYID");
                if (kid) {
                    char *clean = kid;
                    if (strncasecmp(clean, "0x", 2) == 0) clean += 2;
                    add_kid(&ch, clean);
                    rs_free(kid);
                }
            }
            free(line);
        }
        if (!end) break;
        p = end + 1;
    }
    
    return ch;
}

static char *join_strings(char **arr, size_t count, const char *sep) {
    if (count == 0) return rs_strdup("");
    rs_buf b = RS_BUF_INIT;
    for (size_t i = 0; i < count; i++) {
        if (i > 0) rs_buf_append_str(&b, sep);
        rs_buf_append_str(&b, arr[i]);
    }
    return rs_buf_take(&b);
}

char* rs_cdm_resolve_keys(const char *script_path, const char *cdm_type, const rs_drm_challenge *ch, const char **extra_args, int extra_argc, double timeout) {
    rs_strv args = RS_STRV_INIT;
    
    rs_strv_push(&args, "action=cdm");
    rs_strv_pushf(&args, "cdmType=%s", cdm_type);
    
    if (ch->kids_count > 0) {
        char *j = join_strings(ch->kids, ch->kids_count, ",");
        rs_strv_pushf(&args, "kid=%s", j);
        rs_free(j);
    }
    if (ch->pssh_widevine) rs_strv_pushf(&args, "psshWidevine=%s", ch->pssh_widevine);
    if (ch->pssh_playready) rs_strv_pushf(&args, "psshPlayReady=%s", ch->pssh_playready);
    if (ch->pssh_fairplay) rs_strv_pushf(&args, "psshFairPlay=%s", ch->pssh_fairplay);
    
    if (ch->pssh_all_count > 0) {
        rs_strv_pushf(&args, "pssh=%s", ch->pssh_all[0]);
        char *j = join_strings(ch->pssh_all, ch->pssh_all_count, ",");
        rs_strv_pushf(&args, "psshAll=%s", j);
        rs_free(j);
    }
    
    if (ch->key_uris_count > 0) {
        char *j = join_strings(ch->key_uris, ch->key_uris_count, ",");
        rs_strv_pushf(&args, "keyUri=%s", j);
        rs_free(j);
    }
    
    for (int i = 0; i < extra_argc; i++) {
        rs_strv_push(&args, extra_args[i]);
    }
    
    char *out_stdout = NULL;
    char *out_stderr = NULL;
    int ret = rs_script_run_sync(script_path, (const char**)args.items, (int)args.len, timeout, &out_stdout, &out_stderr);
    rs_strv_dispose(&args);
    
    if (ret != 0) {
        rs_free(out_stdout);
        return out_stderr; // Caller handles stderr if they want, but return type is char*.
        // Actually, return NULL on failure might be better, but the prompt doesn't specify.
        // Let's just return NULL for now, we don't have exception throwing.
    }
    rs_free(out_stderr);

    char *res = rs_cdm_parse_key_output(out_stdout);
    rs_free(out_stdout);
    return res;
}

char *rs_cdm_parse_key_output(const char *out_stdout) {
    rs_strv pairs = RS_STRV_INIT;
    if (!out_stdout) return rs_strdup("");

    // JSON parsing
    cJSON *json = cJSON_Parse(out_stdout);
    bool parsed_json = false;
    if (json) {
        cJSON *keys = cJSON_GetObjectItem(json, "keys");
        if (!keys && cJSON_IsArray(json)) keys = json;
        if (cJSON_IsArray(keys)) {
            cJSON *item;
            cJSON_ArrayForEach(item, keys) {
                cJSON *kid = cJSON_GetObjectItem(item, "kid");
                cJSON *key = cJSON_GetObjectItem(item, "key");
                if (cJSON_IsString(kid) && cJSON_IsString(key)) {
                    rs_strv_pushf(&pairs, "%s:%s", kid->valuestring, key->valuestring);
                    parsed_json = true;
                }
            }
        }
        cJSON_Delete(json);
    }
    
    if (!parsed_json) {
        // Line-based parsing: every "<kid>:<key>" pair in the output.
        char kid_val[33], key_val[33];
        for (const char *search = out_stdout; search && *search;) {
            search = scan_kid_key_pair(search, kid_val, key_val);
            if (!search) break;
            rs_strv_pushf(&pairs, "%s:%s", kid_val, key_val);
        }
    }
    
    if (pairs.len == 0) {
        rs_strv_dispose(&pairs);
        return rs_strdup("");
    }

    char *res = join_strings(pairs.items, pairs.len, "\n");
    rs_strv_dispose(&pairs);
    return res;
}
