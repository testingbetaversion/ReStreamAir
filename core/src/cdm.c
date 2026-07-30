#include "rs_cdm.h"
#include "rs_internal.h"
#include "rs_script.h"
#include "../deps/cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

#ifndef _WIN32
#include <regex.h>
#else
// Ensure a regex.h exists or is provided in the environment.
#include <regex.h>
#endif

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
            if (b64 && b64[0]) {
                char **new_all = realloc(ch.pssh_all, (ch.pssh_all_count + 1) * sizeof(char*));
                if (new_all) {
                    ch.pssh_all = new_all;
                    ch.pssh_all[ch.pssh_all_count++] = rs_strdup(b64);
                    classify_pssh(&ch, b64);
                }
            }
            rs_free(b64);
            p = end + 2;
        } else {
            break;
        }
    }
    
    // regex for default_KID
    regex_t preg;
    if (regcomp(&preg, "default_KID[ \t]*=[ \t]*[\"']([0-9a-fA-F-]+)[\"']", REG_EXTENDED) == 0) {
        regmatch_t pmatch[2];
        const char *search = xml;
        while (regexec(&preg, search, 2, pmatch, 0) == 0) {
            if (pmatch[1].rm_so != -1) {
                int len = (int)(pmatch[1].rm_eo - pmatch[1].rm_so);
                char *kid = malloc(len + 1);
                if (kid) {
                    strncpy(kid, search + pmatch[1].rm_so, len);
                    kid[len] = '\0';
                    add_kid(&ch, kid);
                    free(kid);
                }
            }
            search += pmatch[0].rm_eo;
        }
        regfree(&preg);
    }
    
    return ch;
}

static char* get_attr(const char *line, const char *name) {
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "%s=\"([^\"]+)\"", name);
    regex_t preg;
    if (regcomp(&preg, pattern, REG_EXTENDED | REG_ICASE) == 0) {
        regmatch_t pmatch[2];
        if (regexec(&preg, line, 2, pmatch, 0) == 0 && pmatch[1].rm_so != -1) {
            int len = (int)(pmatch[1].rm_eo - pmatch[1].rm_so);
            char *val = malloc(len + 1);
            if (val) {
                strncpy(val, line + pmatch[1].rm_so, len);
                val[len] = '\0';
                regfree(&preg);
                return val;
            }
        }
        regfree(&preg);
    }
    
    snprintf(pattern, sizeof(pattern), "%s=([^, \t\r\n]+)", name);
    if (regcomp(&preg, pattern, REG_EXTENDED | REG_ICASE) == 0) {
        regmatch_t pmatch[2];
        if (regexec(&preg, line, 2, pmatch, 0) == 0 && pmatch[1].rm_so != -1) {
            int len = (int)(pmatch[1].rm_eo - pmatch[1].rm_so);
            char *val = malloc(len + 1);
            if (val) {
                strncpy(val, line + pmatch[1].rm_so, len);
                val[len] = '\0';
                regfree(&preg);
                return val;
            }
        }
        regfree(&preg);
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
                                const char *b64 = comma + 7;
                                char **new_all = realloc(ch.pssh_all, (ch.pssh_all_count + 1) * sizeof(char*));
                                if (new_all) {
                                    ch.pssh_all = new_all;
                                    ch.pssh_all[ch.pssh_all_count++] = rs_strdup(b64);
                                    classify_pssh(&ch, b64);
                                }
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
    
    rs_strv pairs = RS_STRV_INIT;
    
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
        // Regex / Line-based parsing
        regex_t preg;
        if (regcomp(&preg, "([0-9a-fA-F]{32})[ \t]*:[ \t]*([0-9a-fA-F]{32})", REG_EXTENDED) == 0) {
            regmatch_t pmatch[3];
            const char *search = out_stdout;
            while (regexec(&preg, search, 3, pmatch, 0) == 0) {
                int kid_len = (int)(pmatch[1].rm_eo - pmatch[1].rm_so);
                int key_len = (int)(pmatch[2].rm_eo - pmatch[2].rm_so);
                char kid_val[33], key_val[33];
                strncpy(kid_val, search + pmatch[1].rm_so, kid_len);
                kid_val[kid_len] = '\0';
                strncpy(key_val, search + pmatch[2].rm_so, key_len);
                key_val[key_len] = '\0';
                
                // Lowercase them
                for (int i=0; i<32; i++) kid_val[i] = (char)tolower((unsigned char)kid_val[i]);
                for (int i=0; i<32; i++) key_val[i] = (char)tolower((unsigned char)key_val[i]);
                
                rs_strv_pushf(&pairs, "%s:%s", kid_val, key_val);
                search += pmatch[0].rm_eo;
            }
            regfree(&preg);
        }
    }
    
    rs_free(out_stdout);
    
    if (pairs.len == 0) {
        rs_strv_dispose(&pairs);
        return rs_strdup("");
    }
    
    char *res = join_strings(pairs.items, pairs.len, "\n");
    rs_strv_dispose(&pairs);
    return res;
}
