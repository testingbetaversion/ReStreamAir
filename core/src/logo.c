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

void rs_logo_cache_destroy(rs_logo_cache *lc) {
    if (!lc) return;
    if (lc->cache_json) {
        cJSON_Delete(lc->cache_json);
    }
    rs_free(lc->cache_file_path);
    free(lc);
}

static void persist_cache(rs_logo_cache *lc) {
    if (!lc->cache_file_path || !lc->cache_json) return;
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

char* rs_logo_lookup(rs_logo_cache *lc, const char *name, restream_fetch_fn fetch, void *fetch_ctx) {
    if (!lc || !name || !fetch) return NULL;
    
    // Trim whitespaces
    const char *trimmed = name;
    size_t len = strlen(name);
    while (len > 0 && rs_is_space(trimmed[0])) {
        trimmed++;
        len--;
    }
    while (len > 0 && rs_is_space(trimmed[len - 1])) {
        len--;
    }
    if (len == 0) return NULL;
    
    char *trimmed_str = rs_trim_dup(trimmed, len, false);
    if (!trimmed_str) return NULL;

    char *key = normalize_name(trimmed_str);
    if (!key || key[0] == '\0') {
        rs_free(trimmed_str);
        rs_free(key);
        return NULL;
    }

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

    if (logo_url) {
        cJSON_ReplaceItemInObjectCaseSensitive(lc->cache_json, key, cJSON_CreateString(logo_url));
        if (!cJSON_GetObjectItemCaseSensitive(lc->cache_json, key)) {
            cJSON_AddItemToObject(lc->cache_json, key, cJSON_CreateString(logo_url));
        }
    } else {
        cJSON_ReplaceItemInObjectCaseSensitive(lc->cache_json, key, cJSON_CreateString(""));
        if (!cJSON_GetObjectItemCaseSensitive(lc->cache_json, key)) {
            cJSON_AddItemToObject(lc->cache_json, key, cJSON_CreateString(""));
        }
    }
    
    persist_cache(lc);
    rs_free(key);
    return logo_url;
}
