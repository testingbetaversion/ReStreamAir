#include "net.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <curl/curl.h>

#include "rs_common.h"

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} http_buf;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    http_buf *buf = (http_buf *)userdata;
    size_t incoming = size * nmemb;
    // Cap at 128 MB. A playlist is tiny; a real segment is a few MB; a
    // byte-range request returns only its slice. A whole-file response larger
    // than this is a wrong URL, so refuse rather than exhaust memory.
    if (buf->len + incoming > 128 * 1024 * 1024) return 0;
    if (buf->len + incoming + 1 > buf->cap) {
        size_t cap = buf->cap ? buf->cap * 2 : 16384;
        while (cap < buf->len + incoming + 1) cap *= 2;
        char *grown = (char *)realloc(buf->data, cap);
        if (!grown) return 0;
        buf->data = grown;
        buf->cap = cap;
    }
    memcpy(buf->data + buf->len, ptr, incoming);
    buf->len += incoming;
    buf->data[buf->len] = '\0';
    return incoming;
}

// Captures the Content-Range response header (libcurl has no getinfo for it).
static size_t header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
    char **content_range = (char **)userdata;
    size_t len = size * nitems;
    const char *prefix = "content-range:";
    if (len > strlen(prefix) && strncasecmp(buffer, prefix, strlen(prefix)) == 0) {
        const char *value = buffer + strlen(prefix);
        size_t vlen = len - strlen(prefix);
        while (vlen > 0 && (*value == ' ' || *value == '\t')) { value++; vlen--; }
        while (vlen > 0 && (value[vlen - 1] == '\r' || value[vlen - 1] == '\n' ||
                            value[vlen - 1] == ' ')) vlen--;
        free(*content_range);
        *content_range = (char *)malloc(vlen + 1);
        if (*content_range) { memcpy(*content_range, value, vlen); (*content_range)[vlen] = '\0'; }
    }
    return len;
}

int rs_fetch_url(const char *url, const char *proxy, const char *headers, const char *range,
                 char **out, size_t *out_len, long *status, char **content_type,
                 char **content_range, char *errbuf, size_t errbuf_len) {
    if (content_type) *content_type = NULL;
    if (content_range) *content_range = NULL;
    if (status) *status = 0;

    CURL *curl = curl_easy_init();
    if (!curl) { snprintf(errbuf, errbuf_len, "Could not initialise HTTP client."); return -1; }

    http_buf buf = {NULL, 0, 0};
    struct curl_slist *header_list = NULL;
    if (headers && headers[0]) {
        char *copy = rs_strdup(headers);
        for (char *line = strtok(copy, "\r\n"); line; line = strtok(NULL, "\r\n")) {
            while (*line == ' ' || *line == '\t') line++;
            if (*line) header_list = curl_slist_append(header_list, line);
        }
        free(copy);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ReStreamAir/1.0");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    if (proxy && proxy[0]) curl_easy_setopt(curl, CURLOPT_PROXY, proxy);
    if (header_list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    if (range && range[0]) {
        // libcurl's CURLOPT_RANGE wants just the byte spec, without "bytes=".
        const char *spec = strncasecmp(range, "bytes=", 6) == 0 ? range + 6 : range;
        curl_easy_setopt(curl, CURLOPT_RANGE, spec);
    }
    char *captured_range = NULL;
    if (content_range) {
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &captured_range);
    }

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (status) *status = code;
    char *ct = NULL;
    if (content_type && curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct) == CURLE_OK && ct) {
        *content_type = rs_strdup(ct);
    }
    if (content_range) *content_range = captured_range;
    if (header_list) curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        snprintf(errbuf, errbuf_len, "Fetch failed: %s", curl_easy_strerror(rc));
        free(buf.data);
        if (content_type) { free(*content_type); *content_type = NULL; }
        if (content_range) { free(*content_range); *content_range = NULL; }
        return -1;
    }
    if (code < 200 || code >= 400) {
        snprintf(errbuf, errbuf_len, "Upstream returned HTTP %ld.", code);
        free(buf.data);
        if (content_type) { free(*content_type); *content_type = NULL; }
        if (content_range) { free(*content_range); *content_range = NULL; }
        return -1;
    }
    if (!buf.data) {
        buf.data = (char *)malloc(1);
        if (!buf.data) { snprintf(errbuf, errbuf_len, "Out of memory."); return -1; }
        buf.data[0] = '\0';
    }
    *out = buf.data;
    *out_len = buf.len;
    return 0;
}
