#include "rs_logo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "rs_internal.h"
#include "../deps/cJSON.h"

struct rs_logo_cache {
    char *cache_file_path;
    cJSON *cache_json; // Root object representing the cache dictionary
    int batch_depth;   // >0 while a bulk caller is holding the file write back
    bool dirty;        // a batched result is waiting to be written
};

rs_logo_cache* rs_logo_cache_create(const char *cache_file_path) {
    rs_logo_cache *lc = calloc(1, sizeof(rs_logo_cache));
    if (cache_file_path) {
        lc->cache_file_path = rs_strdup(cache_file_path);
        FILE *f = fopen(cache_file_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (len > 0) {
                char *data = malloc(len + 1);
                if (data) {
                    size_t read_len = fread(data, 1, len, f);
                    data[read_len] = '\0';
                    lc->cache_json = cJSON_Parse(data);
                    free(data);
                }
            }
            fclose(f);
        }
    }
    if (!lc->cache_json) {
        lc->cache_json = cJSON_CreateObject();
    }
    return lc;
}

static void persist_cache(rs_logo_cache *lc);

void rs_logo_cache_destroy(rs_logo_cache *lc) {
    if (!lc) return;
    // A batch that never reached its end — shutdown mid-import, say — still has
    // real lookups in it, and throwing them away means paying for them again.
    if (lc->dirty) persist_cache(lc);
    if (lc->cache_json) {
        cJSON_Delete(lc->cache_json);
    }
    rs_free(lc->cache_file_path);
    free(lc);
}

static void persist_cache(rs_logo_cache *lc) {
    if (!lc->cache_file_path || !lc->cache_json) return;
    lc->dirty = false;
    char *json_str = cJSON_PrintUnformatted(lc->cache_json);
    if (json_str) {
        FILE *f = fopen(lc->cache_file_path, "wb");
        if (f) {
            fwrite(json_str, 1, strlen(json_str), f);
            fclose(f);
        }
        cJSON_free(json_str);
    }
}

// Called after every change to the in-memory cache. Writing the whole file
// here is fine for a one-off lookup and quadratic for a bulk one, so a caller
// that is about to resolve many names opens a batch and takes a single write.
static void cache_changed(rs_logo_cache *lc) {
    if (lc->batch_depth > 0) { lc->dirty = true; return; }
    persist_cache(lc);
}

void rs_logo_cache_begin_batch(rs_logo_cache *lc) {
    if (lc) lc->batch_depth++;
}

void rs_logo_cache_end_batch(rs_logo_cache *lc) {
    if (!lc || lc->batch_depth == 0) return;
    if (--lc->batch_depth > 0) return;
    if (lc->dirty) persist_cache(lc);
}

static char* normalize_name(const char *name) {
    rs_buf b = RS_BUF_INIT;
    while (*name) {
        unsigned char c = (unsigned char)*name++;
        if (isalnum(c)) {
            rs_buf_append_char(&b, (char)tolower(c));
        }
    }
    char *res = rs_buf_take(&b);
    return res ? res : rs_strdup("");
}

static char* url_encode(const char *s) {
    rs_buf b = RS_BUF_INIT;
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            rs_buf_append_char(&b, c);
        } else {
            rs_buf_appendf(&b, "%%%02X", c);
        }
    }
    return rs_buf_take(&b);
}

// Splits a caller-supplied name into the trimmed form the query is built from
// and the normalised form the cache is keyed by, so rs_logo_lookup and
// rs_logo_cache_has can never disagree about whether a name is cached. Returns
// false (having written nothing) for a name with no usable characters.
static bool split_name(const char *name, char **out_trimmed, char **out_key) {
    const char *trimmed = name;
    size_t len = strlen(name);
    while (len > 0 && rs_is_space(trimmed[0])) {
        trimmed++;
        len--;
    }
    while (len > 0 && rs_is_space(trimmed[len - 1])) {
        len--;
    }
    if (len == 0) return false;

    char *trimmed_str = rs_trim_dup(trimmed, len, false);
    if (!trimmed_str) return false;

    char *key = normalize_name(trimmed_str);
    if (!key || key[0] == '\0') {
        rs_free(trimmed_str);
        rs_free(key);
        return false;
    }

    if (out_trimmed) *out_trimmed = trimmed_str; else rs_free(trimmed_str);
    *out_key = key;
    return true;
}

bool rs_logo_cache_has(rs_logo_cache *lc, const char *name) {
    if (!lc || !name) return false;
    char *key = NULL;
    if (!split_name(name, NULL, &key)) return false;
    bool present = cJSON_GetObjectItemCaseSensitive(lc->cache_json, key) != NULL;
    rs_free(key);
    return present;
}

char* rs_logo_lookup(rs_logo_cache *lc, const char *name, rs_logo_fetch_fn fetch, void *fetch_ctx) {
    if (!lc || !name || !fetch) return NULL;

    char *trimmed_str = NULL, *key = NULL;
    if (!split_name(name, &trimmed_str, &key)) return NULL;

    cJSON *cached = cJSON_GetObjectItemCaseSensitive(lc->cache_json, key);
    if (cached && cJSON_IsString(cached)) {
        rs_free(trimmed_str);
        char *res = NULL;
        if (cached->valuestring && cached->valuestring[0] != '\0') {
            res = rs_strdup(cached->valuestring);
        }
        rs_free(key);
        return res;
    }

    char *encoded = url_encode(trimmed_str);
    rs_free(trimmed_str);
    
    if (!encoded) {
        rs_free(key);
        return NULL;
    }

    rs_buf url_buf = RS_BUF_INIT;
    rs_buf_append_str(&url_buf, "https://autocomplete.clearbit.com/v1/companies/suggest?query=");
    rs_buf_append_str(&url_buf, encoded);
    rs_free(encoded);
    
    char *url = rs_buf_take(&url_buf);
    if (!url) {
        rs_free(key);
        return NULL;
    }

    char *response = fetch(url, fetch_ctx);
    rs_free(url);

    char *logo_url = NULL;
    if (response) {
        cJSON *arr = cJSON_Parse(response);
        if (arr && cJSON_IsArray(arr)) {
            cJSON *first = cJSON_GetArrayItem(arr, 0);
            if (first && cJSON_IsObject(first)) {
                cJSON *domain = cJSON_GetObjectItemCaseSensitive(first, "domain");
                if (domain && cJSON_IsString(domain) && domain->valuestring && domain->valuestring[0] != '\0') {
                    rs_buf favicon_buf = RS_BUF_INIT;
                    rs_buf_append_str(&favicon_buf, "https://www.google.com/s2/favicons?sz=128&domain=");
                    rs_buf_append_str(&favicon_buf, domain->valuestring);
                    logo_url = rs_buf_take(&favicon_buf);
                }
            }
        }
        if (arr) {
            cJSON_Delete(arr);
        }
        rs_free(response);
    }

    // Record the result — a miss is cached as "" so the same name does not go
    // back to the network on every import.
    //
    // Replace only when the key is already there. cJSON_ReplaceItemInObject*
    // takes no ownership when the key is absent: it strdups the name onto the
    // node it was handed, discovers there is nothing to replace, and returns
    // false, leaking both. That is the common path here — a lookup only reaches
    // this line when the cache did NOT have the name — so the old
    // replace-then-add pair leaked one node per newly resolved logo, which a
    // bulk channel import does thousands of at a time.
    cJSON *value = cJSON_CreateString(logo_url ? logo_url : "");
    if (value) {
        if (cJSON_GetObjectItemCaseSensitive(lc->cache_json, key)) {
            if (!cJSON_ReplaceItemInObjectCaseSensitive(lc->cache_json, key, value))
                cJSON_Delete(value);
        } else if (!cJSON_AddItemToObject(lc->cache_json, key, value)) {
            cJSON_Delete(value);
        }
    }

    cache_changed(lc);
    rs_free(key);
    return logo_url;
}
