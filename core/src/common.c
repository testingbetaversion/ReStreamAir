#include "rs_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rs_free(void *p) {
    free(p);
}

void rs_free_strv(char **v, size_t n) {
    if (!v) return;
    for (size_t i = 0; i < n; i++) free(v[i]);
    free(v);
}

char *rs_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *copy = (char *)malloc(n);
    if (copy) memcpy(copy, s, n);
    return copy;
}

static const char base64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *rs_base64_encode(const uint8_t *data, size_t len) {
    size_t out_len = ((len + 2) / 3) * 4;
    char *out = (char *)malloc(out_len + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned b0 = data[i];
        unsigned b1 = i + 1 < len ? data[i + 1] : 0;
        unsigned b2 = i + 2 < len ? data[i + 2] : 0;
        unsigned triple = (b0 << 16) | (b1 << 8) | b2;
        out[o++] = base64_alphabet[(triple >> 18) & 0x3f];
        out[o++] = base64_alphabet[(triple >> 12) & 0x3f];
        out[o++] = i + 1 < len ? base64_alphabet[(triple >> 6) & 0x3f] : '=';
        out[o++] = i + 2 < len ? base64_alphabet[triple & 0x3f] : '=';
    }
    out[o] = '\0';
    return out;
}

static int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int rs_base64_decode(const char *text, uint8_t *out, size_t out_cap, size_t *out_len) {
    if (!text) return -1;
    size_t len = strlen(text);
    if (len % 4 != 0) return -1;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 4) {
        int v0 = base64_value(text[i]);
        int v1 = base64_value(text[i + 1]);
        if (v0 < 0 || v1 < 0) return -1;
        bool pad2 = text[i + 2] == '=';
        bool pad3 = text[i + 3] == '=';
        int v2 = pad2 ? 0 : base64_value(text[i + 2]);
        int v3 = pad3 ? 0 : base64_value(text[i + 3]);
        if ((!pad2 && v2 < 0) || (!pad3 && v3 < 0)) return -1;
        // Padding is only valid in the final quad.
        if ((pad2 || pad3) && i + 4 != len) return -1;
        if (pad2 && !pad3) return -1;

        unsigned triple = ((unsigned)v0 << 18) | ((unsigned)v1 << 12) |
                          ((unsigned)v2 << 6) | (unsigned)v3;
        if (o >= out_cap) return -1;
        out[o++] = (uint8_t)((triple >> 16) & 0xff);
        if (!pad2) {
            if (o >= out_cap) return -1;
            out[o++] = (uint8_t)((triple >> 8) & 0xff);
        }
        if (!pad3) {
            if (o >= out_cap) return -1;
            out[o++] = (uint8_t)(triple & 0xff);
        }
    }
    if (out_len) *out_len = o;
    return 0;
}

static bool buf_grow(rs_buf *b, size_t extra) {
    if (b->err) return false;
    size_t needed = b->len + extra + 1;  // room for the terminator
    if (needed <= b->cap) return true;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < needed) {
        if (cap > (size_t)-1 / 2) { b->err = true; return false; }
        cap *= 2;
    }
    char *data = (char *)realloc(b->data, cap);
    if (!data) { b->err = true; return false; }
    b->data = data;
    b->cap = cap;
    return true;
}

void rs_buf_append(rs_buf *b, const char *bytes, size_t len) {
    if (len == 0 || !buf_grow(b, len)) return;
    memcpy(b->data + b->len, bytes, len);
    b->len += len;
}

void rs_buf_append_str(rs_buf *b, const char *s) {
    if (s) rs_buf_append(b, s, strlen(s));
}

void rs_buf_append_char(rs_buf *b, char c) {
    if (!buf_grow(b, 1)) return;
    b->data[b->len++] = c;
}

void rs_buf_appendf(rs_buf *b, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list probe;
    va_copy(probe, args);
    int needed = vsnprintf(NULL, 0, fmt, probe);
    va_end(probe);
    if (needed < 0) { b->err = true; va_end(args); return; }
    if (buf_grow(b, (size_t)needed)) {
        vsnprintf(b->data + b->len, (size_t)needed + 1, fmt, args);
        b->len += (size_t)needed;
    }
    va_end(args);
}

char *rs_buf_take(rs_buf *b) {
    if (b->err) { rs_buf_dispose(b); return NULL; }
    if (!b->data && !buf_grow(b, 0)) { rs_buf_dispose(b); return NULL; }
    b->data[b->len] = '\0';
    char *out = b->data;
    b->data = NULL;
    b->len = b->cap = 0;
    return out;
}

void rs_buf_dispose(rs_buf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
    b->err = false;
}

void rs_strv_push_owned(rs_strv *v, char *s) {
    if (!s) { v->err = true; return; }
    if (v->err) { free(s); return; }
    if (v->len + 2 > v->cap) {  // +1 element, +1 for a NULL terminator slot
        size_t cap = v->cap ? v->cap * 2 : 16;
        char **items = (char **)realloc(v->items, cap * sizeof(char *));
        if (!items) { v->err = true; free(s); return; }
        v->items = items;
        v->cap = cap;
    }
    v->items[v->len++] = s;
}

void rs_strv_push(rs_strv *v, const char *s) {
    rs_strv_push_owned(v, rs_strdup(s ? s : ""));
}

void rs_strv_pushf(rs_strv *v, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list probe;
    va_copy(probe, args);
    int needed = vsnprintf(NULL, 0, fmt, probe);
    va_end(probe);
    if (needed < 0) { v->err = true; va_end(args); return; }
    char *s = (char *)malloc((size_t)needed + 1);
    if (s) vsnprintf(s, (size_t)needed + 1, fmt, args);
    va_end(args);
    rs_strv_push_owned(v, s);
}

void rs_strv_dispose(rs_strv *v) {
    rs_free_strv(v->items, v->len);
    v->items = NULL;
    v->len = v->cap = 0;
    v->err = false;
}

bool rs_is_space(char c) {
    return c == ' ' || c == '\t';
}

bool rs_is_space_nl(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

size_t rs_trim(const char *s, size_t len, bool nl, const char **out) {
    size_t start = 0;
    while (start < len && (nl ? rs_is_space_nl(s[start]) : rs_is_space(s[start]))) start++;
    while (len > start && (nl ? rs_is_space_nl(s[len - 1]) : rs_is_space(s[len - 1]))) len--;
    *out = s + start;
    return len - start;
}

char *rs_trim_dup(const char *s, size_t len, bool nl) {
    const char *start = NULL;
    size_t n = rs_trim(s, len, nl, &start);
    char *copy = (char *)malloc(n + 1);
    if (!copy) return NULL;
    memcpy(copy, start, n);
    copy[n] = '\0';
    return copy;
}
