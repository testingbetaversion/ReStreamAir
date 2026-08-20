#include "rs_m3u8.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "rs_internal.h"
#include "rs_url.h"

// Line handling is pinned by the frozen goldens, because the self-test
// diffs the two implementations byte for byte: split on "\n" keeping empty
// pieces, classify using the trimmed line but emit the raw one, then join with
// "\n" and append a single trailing newline.

typedef struct {
    const char *ptr;
    size_t len;
} slice;

typedef struct {
    slice *items;
    size_t count;
} lines;

static bool split_lines(const char *text, lines *out) {
    size_t count = 1;
    for (const char *p = text; *p; p++) {
        if (*p == '\n') count++;
    }
    out->items = (slice *)calloc(count, sizeof(slice));
    if (!out->items) return false;
    out->count = count;

    size_t index = 0;
    const char *start = text;
    for (const char *p = text;; p++) {
        if (*p == '\n' || *p == '\0') {
            out->items[index].ptr = start;
            out->items[index].len = (size_t)(p - start);
            index++;
            if (*p == '\0') break;
            start = p + 1;
        }
    }
    return true;
}

static void lines_dispose(lines *l) {
    free(l->items);
    l->items = NULL;
    l->count = 0;
}

static bool slice_has_prefix(slice s, const char *prefix) {
    size_t n = strlen(prefix);
    return s.len >= n && memcmp(s.ptr, prefix, n) == 0;
}

static bool slice_contains(slice s, const char *needle) {
    size_t n = strlen(needle);
    if (n > s.len) return false;
    for (size_t i = 0; i + n <= s.len; i++) {
        if (memcmp(s.ptr + i, needle, n) == 0) return true;
    }
    return false;
}

static slice slice_trimmed(slice s) {
    slice out;
    out.len = rs_trim(s.ptr, s.len, true, &out.ptr);
    return out;
}

static char *slice_dup(slice s) {
    char *copy = (char *)malloc(s.len + 1);
    if (!copy) return NULL;
    memcpy(copy, s.ptr, s.len);
    copy[s.len] = '\0';
    return copy;
}

// The accepted integer form is an optional sign followed by digits and nothing
// else; anything looser (trailing text, empty) yields nil.
static bool parse_int_strict(const char *s, size_t len, int64_t *out) {
    if (len == 0 || len > 20) return false;
    char buf[24];
    memcpy(buf, s, len);
    buf[len] = '\0';
    size_t i = (buf[0] == '+' || buf[0] == '-') ? 1 : 0;
    if (i >= len) return false;
    for (size_t j = i; j < len; j++) {
        if (buf[j] < '0' || buf[j] > '9') return false;
    }
    errno = 0;
    char *end = NULL;
    long long value = strtoll(buf, &end, 10);
    if (errno == ERANGE || !end || *end != '\0') return false;
    *out = (int64_t)value;
    return true;
}

int64_t rs_m3u8_media_sequence(const char *text) {
    if (!text) return 0;
    static const char tag[] = "#EXT-X-MEDIA-SEQUENCE:";
    const char *p = text;
    while ((p = strstr(p, tag)) != NULL) {
        const char *digits = p + sizeof(tag) - 1;
        size_t n = 0;
        while (digits[n] >= '0' && digits[n] <= '9') n++;
        int64_t value = 0;
        if (n > 0 && parse_int_strict(digits, n, &value)) return value;
        // A tag with no digits does not match the pattern; keep looking, as the
        // a regex search would.
        if (n == 0) {
            p = digits;
            continue;
        }
        return 0;  // digits present but out of range: Int(...) is nil, so 0
    }
    return 0;
}

bool rs_m3u8_is_master(const char *text) {
    return text && strstr(text, "#EXT-X-STREAM-INF") != NULL;
}

// Rewrites every URI="..." attribute in a line, as the uriAttributeRegex pass
// does. A URI that cannot be resolved is left exactly as written.
static char *rewrite_uri_attribute(slice line, const char *base, rs_m3u8_line_kind kind,
                                   rs_m3u8_transform_fn transform, void *userdata) {
    rs_buf out = RS_BUF_INIT;
    size_t i = 0;
    while (i < line.len) {
        // The pattern is URI="([^"]+)" — at least one character inside quotes.
        if (line.len - i >= 5 && memcmp(line.ptr + i, "URI=\"", 5) == 0) {
            size_t value_start = i + 5;
            size_t value_end = value_start;
            while (value_end < line.len && line.ptr[value_end] != '"') value_end++;
            if (value_end < line.len && value_end > value_start) {
                slice value = {line.ptr + value_start, value_end - value_start};
                char *uri = slice_dup(value);
                char *absolute = uri ? rs_url_resolve(base, uri) : NULL;
                free(uri);
                if (absolute) {
                    char *replacement = transform(userdata, absolute, kind, -1);
                    rs_buf_append_str(&out, "URI=\"");
                    rs_buf_append_str(&out, replacement ? replacement : absolute);
                    rs_buf_append_char(&out, '"');
                    free(replacement);
                    free(absolute);
                    i = value_end + 1;
                    continue;
                }
                // Unresolvable: copy the whole attribute through untouched.
                rs_buf_append(&out, line.ptr + i, value_end + 1 - i);
                i = value_end + 1;
                continue;
            }
        }
        rs_buf_append_char(&out, line.ptr[i]);
        i++;
    }
    return rs_buf_take(&out);
}

char *rs_m3u8_rewrite(const char *text, const char *base_url, bool drop_key,
                      rs_m3u8_transform_fn transform, void *userdata) {
    if (!text || !base_url || !transform) return NULL;

    lines l;
    if (!split_lines(text, &l)) return NULL;

    int64_t sequence = drop_key ? rs_m3u8_media_sequence(text) : 0;
    rs_buf out = RS_BUF_INIT;

    for (size_t i = 0; i < l.count; i++) {
        if (i > 0) rs_buf_append_char(&out, '\n');
        slice raw = l.items[i];
        slice line = slice_trimmed(raw);

        if (line.len == 0) {
            rs_buf_append(&out, raw.ptr, raw.len);
            continue;
        }
        if (slice_has_prefix(line, "#EXT-X-KEY")) {
            if (drop_key) continue;  // the line becomes empty, not absent
            char *rewritten = rewrite_uri_attribute(raw, base_url, RS_M3U8_LINE_KEY, transform, userdata);
            if (!rewritten) { out.err = true; break; }
            rs_buf_append_str(&out, rewritten);
            free(rewritten);
            continue;
        }
        if (slice_has_prefix(line, "#EXT-X-MAP")) {
            char *rewritten = rewrite_uri_attribute(raw, base_url, RS_M3U8_LINE_MAP, transform, userdata);
            if (!rewritten) { out.err = true; break; }
            rs_buf_append_str(&out, rewritten);
            free(rewritten);
            continue;
        }
        if (line.ptr[0] == '#') {
            rs_buf_append(&out, raw.ptr, raw.len);
            continue;
        }

        char *uri = slice_dup(line);
        char *absolute = uri ? rs_url_resolve(base_url, uri) : NULL;
        free(uri);
        if (!absolute) {
            rs_buf_append(&out, raw.ptr, raw.len);
            continue;
        }
        int64_t seq = drop_key ? sequence : -1;
        if (drop_key) sequence++;
        char *replacement = transform(userdata, absolute, RS_M3U8_LINE_SEGMENT, seq);
        rs_buf_append_str(&out, replacement ? replacement : absolute);
        free(replacement);
        free(absolute);
    }
    rs_buf_append_char(&out, '\n');

    lines_dispose(&l);
    return rs_buf_take(&out);
}

// Adapter so the master transform, which only takes a URI, can reuse
// rewrite_uri_attribute.
typedef struct {
    rs_m3u8_master_transform_fn transform;
    void *userdata;
} master_adapter;

static char *master_adapter_call(void *userdata, const char *absolute_uri,
                                 rs_m3u8_line_kind kind, int64_t seq) {
    (void)kind;
    (void)seq;
    master_adapter *adapter = (master_adapter *)userdata;
    return adapter->transform(adapter->userdata, absolute_uri);
}

char *rs_m3u8_rewrite_master(const char *text, const char *base_url,
                             rs_m3u8_master_transform_fn transform, void *userdata) {
    if (!text || !base_url || !transform) return NULL;

    lines l;
    if (!split_lines(text, &l)) return NULL;

    master_adapter adapter = {transform, userdata};
    rs_buf out = RS_BUF_INIT;
    bool first = true;
    size_t i = 0;
    while (i < l.count) {
        slice trimmed = slice_trimmed(l.items[i]);

        if (slice_has_prefix(trimmed, "#EXT-X-STREAM-INF")) {
            if (!first) rs_buf_append_char(&out, '\n');
            first = false;
            rs_buf_append(&out, l.items[i].ptr, l.items[i].len);
            if (i + 1 < l.count) {
                slice uri_line = slice_trimmed(l.items[i + 1]);
                char *uri = uri_line.len > 0 ? slice_dup(uri_line) : NULL;
                char *absolute = uri ? rs_url_resolve(base_url, uri) : NULL;
                free(uri);
                rs_buf_append_char(&out, '\n');
                if (absolute) {
                    char *replacement = transform(userdata, absolute);
                    rs_buf_append_str(&out, replacement ? replacement : absolute);
                    free(replacement);
                    free(absolute);
                } else {
                    rs_buf_append(&out, l.items[i + 1].ptr, l.items[i + 1].len);
                }
                i += 2;
                continue;
            }
            i += 1;
            continue;
        }

        if (!first) rs_buf_append_char(&out, '\n');
        first = false;
        if (slice_has_prefix(trimmed, "#EXT-X-MEDIA") && slice_contains(trimmed, "URI=")) {
            char *rewritten = rewrite_uri_attribute(l.items[i], base_url, RS_M3U8_LINE_SEGMENT,
                                                    master_adapter_call, &adapter);
            if (!rewritten) { out.err = true; break; }
            rs_buf_append_str(&out, rewritten);
            free(rewritten);
        } else {
            rs_buf_append(&out, l.items[i].ptr, l.items[i].len);
        }
        i += 1;
    }
    rs_buf_append_char(&out, '\n');

    lines_dispose(&l);
    return rs_buf_take(&out);
}

// Attribute list: a quote-aware comma split of everything after the first
// colon, then key=value with surrounding quotes stripped from the value. A
// repeated key keeps its last value, as assigning into a dictionary does.

typedef struct {
    char **keys;
    char **values;
    size_t count;
} attrs;

static void attrs_dispose(attrs *a) {
    rs_free_strv(a->keys, a->count);
    rs_free_strv(a->values, a->count);
    a->keys = a->values = NULL;
    a->count = 0;
}

static const char *attrs_get(const attrs *a, const char *key) {
    const char *found = NULL;
    for (size_t i = 0; i < a->count; i++) {
        if (strcmp(a->keys[i], key) == 0) found = a->values[i];
    }
    return found;
}

static void attrs_add_piece(attrs *a, const char *piece, size_t len) {
    const char *eq = (const char *)memchr(piece, '=', len);
    if (!eq) return;

    char *key = rs_trim_dup(piece, (size_t)(eq - piece), false);
    const char *value_start = eq + 1;
    size_t value_len = len - (size_t)(value_start - piece);
    const char *trimmed = NULL;
    size_t trimmed_len = rs_trim(value_start, value_len, false, &trimmed);
    if (trimmed_len >= 2 && trimmed[0] == '"' && trimmed[trimmed_len - 1] == '"') {
        trimmed++;
        trimmed_len -= 2;
    }
    char *value = (char *)malloc(trimmed_len + 1);
    if (!key || !value) {
        free(key);
        free(value);
        return;
    }
    memcpy(value, trimmed, trimmed_len);
    value[trimmed_len] = '\0';

    char **keys = (char **)realloc(a->keys, (a->count + 1) * sizeof(char *));
    if (keys) a->keys = keys;
    char **values = (char **)realloc(a->values, (a->count + 1) * sizeof(char *));
    if (values) a->values = values;
    if (!keys || !values) {
        free(key);
        free(value);
        return;
    }
    a->keys[a->count] = key;
    a->values[a->count] = value;
    a->count++;
}

static void parse_attributes(slice line, attrs *out) {
    memset(out, 0, sizeof(*out));
    const void *colon = memchr(line.ptr, ':', line.len);
    if (!colon) return;

    const char *text = (const char *)colon + 1;
    size_t len = line.len - (size_t)(text - line.ptr);
    bool in_quotes = false;
    size_t piece_start = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '"') in_quotes = !in_quotes;
        if (text[i] == ',' && !in_quotes) {
            attrs_add_piece(out, text + piece_start, i - piece_start);
            piece_start = i + 1;
        }
    }
    if (piece_start < len) attrs_add_piece(out, text + piece_start, len - piece_start);
}

static int64_t attr_int(const attrs *a, const char *key) {
    const char *value = attrs_get(a, key);
    int64_t parsed = 0;
    if (value && parse_int_strict(value, strlen(value), &parsed)) return parsed;
    return -1;
}

// RESOLUTION is "WxH"; this takes the first component for width
// and the last for height, so a value with no "x" yields the same number twice.
static void attr_resolution(const attrs *a, int64_t *width, int64_t *height) {
    *width = -1;
    *height = -1;
    const char *value = attrs_get(a, "RESOLUTION");
    if (!value) return;
    size_t len = strlen(value);
    const char *x = (const char *)memchr(value, 'x', len);
    size_t first_len = x ? (size_t)(x - value) : len;
    const char *last = value;
    size_t last_len = len;
    for (const char *p = value + len; p > value; p--) {
        if (p[-1] == 'x') {
            last = p;
            last_len = len - (size_t)(p - value);
            break;
        }
    }
    int64_t parsed = 0;
    if (parse_int_strict(value, first_len, &parsed)) *width = parsed;
    if (parse_int_strict(last, last_len, &parsed)) *height = parsed;
}

int rs_m3u8_probe_master(const char *text, const char *base_url, rs_m3u8_probe *out) {
    if (!text || !base_url || !out) return -1;
    memset(out, 0, sizeof(*out));

    lines l;
    if (!split_lines(text, &l)) return -1;

    size_t i = 0;
    while (i < l.count) {
        slice line = slice_trimmed(l.items[i]);

        if (slice_has_prefix(line, "#EXT-X-STREAM-INF")) {
            attrs a;
            parse_attributes(line, &a);
            slice uri_line = (i + 1 < l.count) ? slice_trimmed(l.items[i + 1]) : (slice){"", 0};
            char *uri = slice_dup(uri_line);
            char *absolute = uri ? rs_url_resolve(base_url, uri) : NULL;
            free(uri);
            if (absolute) {
                rs_m3u8_variant *grown =
                    (rs_m3u8_variant *)realloc(out->variants, (out->variant_count + 1) * sizeof(*grown));
                if (!grown) {
                    free(absolute);
                    attrs_dispose(&a);
                    lines_dispose(&l);
                    rs_m3u8_probe_dispose(out);
                    return -1;
                }
                out->variants = grown;
                rs_m3u8_variant *v = &out->variants[out->variant_count++];
                v->uri = absolute;
                v->bandwidth = attr_int(&a, "BANDWIDTH");
                attr_resolution(&a, &v->width, &v->height);
                const char *codecs = attrs_get(&a, "CODECS");
                v->codecs = codecs ? rs_strdup(codecs) : NULL;
            }
            attrs_dispose(&a);
            i += 2;
            continue;
        }

        if (slice_has_prefix(line, "#EXT-X-MEDIA") && slice_contains(line, "TYPE=AUDIO")) {
            attrs a;
            parse_attributes(line, &a);
            rs_m3u8_audio_track *grown =
                (rs_m3u8_audio_track *)realloc(out->audio_tracks, (out->audio_count + 1) * sizeof(*grown));
            if (!grown) {
                attrs_dispose(&a);
                lines_dispose(&l);
                rs_m3u8_probe_dispose(out);
                return -1;
            }
            out->audio_tracks = grown;
            rs_m3u8_audio_track *track = &out->audio_tracks[out->audio_count++];
            const char *group = attrs_get(&a, "GROUP-ID");
            const char *name = attrs_get(&a, "NAME");
            const char *language = attrs_get(&a, "LANGUAGE");
            const char *uri = attrs_get(&a, "URI");
            track->group_id = rs_strdup(group ? group : "audio");
            track->name = rs_strdup(name ? name : "Audio");
            track->language = language ? rs_strdup(language) : NULL;
            track->uri = uri ? rs_url_resolve(base_url, uri) : NULL;
            attrs_dispose(&a);
        }
        i += 1;
    }

    lines_dispose(&l);
    return 0;
}

void rs_m3u8_probe_dispose(rs_m3u8_probe *probe) {
    if (!probe) return;
    for (size_t i = 0; i < probe->variant_count; i++) {
        free(probe->variants[i].uri);
        free(probe->variants[i].codecs);
    }
    free(probe->variants);
    for (size_t i = 0; i < probe->audio_count; i++) {
        free(probe->audio_tracks[i].group_id);
        free(probe->audio_tracks[i].name);
        free(probe->audio_tracks[i].language);
        free(probe->audio_tracks[i].uri);
    }
    free(probe->audio_tracks);
    memset(probe, 0, sizeof(*probe));
}
