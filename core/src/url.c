#include "rs_url.h"

#include <stdlib.h>
#include <string.h>

#include "rs_internal.h"

typedef struct {
    const char *scheme;
    size_t scheme_len;
    bool has_scheme;

    const char *authority;
    size_t authority_len;
    bool has_authority;

    const char *path;
    size_t path_len;

    const char *query;
    size_t query_len;
    bool has_query;

    const char *fragment;
    size_t fragment_len;
    bool has_fragment;
} uri;

static bool is_scheme_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_scheme_char(char c) {
    return is_scheme_start(c) || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
}

static bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Bytes Foundation percent-encodes rather than passing through. Control
// characters, space and DEL, the handful of ASCII symbols that are not legal in
// a URL, and every non-ASCII byte (which encodes UTF-8 a byte at a time).
// Structural characters — : / ? # @ — are never in this set, so encoding a
// whole reference cannot change how it parses.
static bool must_encode(unsigned char c) {
    if (c <= 0x20 || c >= 0x7f) return true;
    switch (c) {
        case '"': case '<': case '>': case '[': case '\\':
        case ']': case '^': case '`': case '{': case '|': case '}':
            return true;
        default:
            return false;
    }
}

// Percent-encodes a URL string the way Foundation does when it builds a URL:
// uppercase hex, and a '%' is only escaped when it does not already introduce a
// valid escape sequence, so an already-encoded URI survives untouched.
static char *percent_normalize(const char *s, size_t len) {
    static const char hex[] = "0123456789ABCDEF";
    rs_buf out = RS_BUF_INIT;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '%') {
            if (i + 2 < len && is_hex_digit(s[i + 1]) && is_hex_digit(s[i + 2])) {
                rs_buf_append(&out, s + i, 3);
                i += 2;
            } else {
                rs_buf_append_str(&out, "%25");
            }
            continue;
        }
        if (must_encode(c)) {
            char escape[3] = {'%', hex[c >> 4], hex[c & 0x0f]};
            rs_buf_append(&out, escape, 3);
        } else {
            rs_buf_append_char(&out, (char)c);
        }
    }
    return rs_buf_take(&out);
}

static void parse_uri(const char *s, size_t len, uri *out) {
    memset(out, 0, sizeof(*out));
    size_t i = 0;

    if (len > 0 && is_scheme_start(s[0])) {
        size_t j = 1;
        while (j < len && is_scheme_char(s[j])) j++;
        if (j < len && s[j] == ':') {
            out->scheme = s;
            out->scheme_len = j;
            out->has_scheme = true;
            i = j + 1;
        }
    }

    if (i + 1 < len && s[i] == '/' && s[i + 1] == '/') {
        i += 2;
        size_t start = i;
        while (i < len && s[i] != '/' && s[i] != '?' && s[i] != '#') i++;
        out->authority = s + start;
        out->authority_len = i - start;
        out->has_authority = true;
    }

    size_t path_start = i;
    while (i < len && s[i] != '?' && s[i] != '#') i++;
    out->path = s + path_start;
    out->path_len = i - path_start;

    if (i < len && s[i] == '?') {
        i++;
        size_t start = i;
        while (i < len && s[i] != '#') i++;
        out->query = s + start;
        out->query_len = i - start;
        out->has_query = true;
    }

    if (i < len && s[i] == '#') {
        i++;
        out->fragment = s + i;
        out->fragment_len = len - i;
        out->has_fragment = true;
    }
}

// RFC 3986 section 5.2.4. Writes into `out`, which must have room for len bytes.
static size_t remove_dot_segments(const char *path, size_t len, char *out) {
    size_t out_len = 0;
    size_t i = 0;
    while (i < len) {
        size_t remaining = len - i;
        const char *p = path + i;

        if (remaining >= 3 && memcmp(p, "../", 3) == 0) {          // 2A
            i += 3;
        } else if (remaining >= 2 && memcmp(p, "./", 2) == 0) {
            i += 2;
        } else if (remaining >= 3 && memcmp(p, "/./", 3) == 0) {   // 2B
            i += 2;  // leaves the "/" for the next pass to consume
        } else if (remaining == 2 && memcmp(p, "/.", 2) == 0) {
            out[out_len++] = '/';
            i += 2;
        } else if (remaining >= 4 && memcmp(p, "/../", 4) == 0) {  // 2C
            i += 3;
            while (out_len > 0 && out[out_len - 1] != '/') out_len--;
            if (out_len > 0) out_len--;
        } else if (remaining == 3 && memcmp(p, "/..", 3) == 0) {
            while (out_len > 0 && out[out_len - 1] != '/') out_len--;
            if (out_len > 0) out_len--;
            out[out_len++] = '/';
            i += 3;
        } else if (remaining == 1 && p[0] == '.') {                // 2D
            i += 1;
        } else if (remaining == 2 && memcmp(p, "..", 2) == 0) {
            i += 2;
        } else {                                                   // 2E
            size_t start = i;
            if (path[i] == '/') i++;
            while (i < len && path[i] != '/') i++;
            memcpy(out + out_len, path + start, i - start);
            out_len += i - start;
        }
    }
    return out_len;
}

// RFC 3986 section 5.3.
static char *compose(const uri *t, const char *path, size_t path_len) {
    rs_buf buf = RS_BUF_INIT;
    if (t->has_scheme) {
        rs_buf_append(&buf, t->scheme, t->scheme_len);
        rs_buf_append_char(&buf, ':');
    }
    if (t->has_authority) {
        rs_buf_append_str(&buf, "//");
        rs_buf_append(&buf, t->authority, t->authority_len);
    }
    rs_buf_append(&buf, path, path_len);
    if (t->has_query) {
        rs_buf_append_char(&buf, '?');
        rs_buf_append(&buf, t->query, t->query_len);
    }
    if (t->has_fragment) {
        rs_buf_append_char(&buf, '#');
        rs_buf_append(&buf, t->fragment, t->fragment_len);
    }
    return rs_buf_take(&buf);
}

bool rs_url_is_absolute(const char *s) {
    if (!s || !s[0] || !is_scheme_start(s[0])) return false;
    size_t i = 1;
    while (s[i] && is_scheme_char(s[i])) i++;
    return s[i] == ':';
}

char *rs_url_resolve(const char *base, const char *ref) {
    // An empty reference is where Foundation returns nil rather than echoing
    // the base, and callers rely on that to pass a line through untouched.
    if (!base || !ref || ref[0] == '\0') return NULL;

    char *normalized_ref = percent_normalize(ref, strlen(ref));
    if (!normalized_ref) return NULL;
    // An absolute reference is already the answer. Its dot segments stay as
    // written: Foundation only removes those while resolving a relative one.
    if (rs_url_is_absolute(normalized_ref)) return normalized_ref;

    if (!rs_url_is_absolute(base)) {
        free(normalized_ref);
        return NULL;
    }
    char *normalized_base = percent_normalize(base, strlen(base));
    if (!normalized_base) {
        free(normalized_ref);
        return NULL;
    }

    uri b, r;
    parse_uri(normalized_base, strlen(normalized_base), &b);
    parse_uri(normalized_ref, strlen(normalized_ref), &r);

    uri t;
    memset(&t, 0, sizeof(t));
    t.scheme = b.scheme;
    t.scheme_len = b.scheme_len;
    t.has_scheme = true;

    // The merged path, before dot-segment removal. Its longest form is the base
    // path plus the reference path.
    size_t merged_cap = b.path_len + r.path_len + 2;
    char *merged = (char *)malloc(merged_cap);
    char *resolved = (char *)malloc(merged_cap);
    if (!merged || !resolved) {
        free(merged);
        free(resolved);
        free(normalized_base);
        free(normalized_ref);
        return NULL;
    }
    size_t merged_len = 0;

    if (r.has_authority) {
        t.authority = r.authority;
        t.authority_len = r.authority_len;
        t.has_authority = true;
        memcpy(merged, r.path, r.path_len);
        merged_len = r.path_len;
        t.query = r.query;
        t.query_len = r.query_len;
        t.has_query = r.has_query;
    } else {
        t.authority = b.authority;
        t.authority_len = b.authority_len;
        t.has_authority = b.has_authority;

        if (r.path_len == 0) {
            memcpy(merged, b.path, b.path_len);
            merged_len = b.path_len;
            if (r.has_query) {
                t.query = r.query;
                t.query_len = r.query_len;
                t.has_query = true;
            } else {
                t.query = b.query;
                t.query_len = b.query_len;
                t.has_query = b.has_query;
            }
        } else {
            if (r.path[0] == '/') {
                memcpy(merged, r.path, r.path_len);
                merged_len = r.path_len;
            } else {
                // Merge (section 5.2.3): the base path up to and including its
                // last slash, or "/" when the base has an authority and no path.
                if (b.has_authority && b.path_len == 0) {
                    merged[merged_len++] = '/';
                } else {
                    size_t cut = 0;
                    for (size_t i = 0; i < b.path_len; i++) {
                        if (b.path[i] == '/') cut = i + 1;
                    }
                    memcpy(merged, b.path, cut);
                    merged_len = cut;
                }
                memcpy(merged + merged_len, r.path, r.path_len);
                merged_len += r.path_len;
            }
            t.query = r.query;
            t.query_len = r.query_len;
            t.has_query = r.has_query;
        }
    }

    t.fragment = r.fragment;
    t.fragment_len = r.fragment_len;
    t.has_fragment = r.has_fragment;

    size_t resolved_len = remove_dot_segments(merged, merged_len, resolved);
    char *out = compose(&t, resolved, resolved_len);
    free(merged);
    free(resolved);
    free(normalized_base);
    free(normalized_ref);
    return out;
}
