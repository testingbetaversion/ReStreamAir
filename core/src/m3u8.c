#include "rs_m3u8.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
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

char *rs_m3u8_rewrite_master(const char *text, const char *base_url, bool drop_session_key,
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

        if (drop_session_key && slice_has_prefix(trimmed, "#EXT-X-SESSION-KEY")) {
            i++;
            continue;
        }

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

// --- variant limiting --------------------------------------------------------
//
// The provider's "Default video" rule list, applied to an HLS master: the same
// first-match-wins preference list the DASH engine reads (height<=N,
// bandwidth<=N, codec=X, best, worst), but as a filter on what the player may
// choose rather than a single pick. Rules naming a DASH representation id or a
// language say nothing about an HLS variant and are skipped.

typedef struct {
    size_t inf;   // line index of the #EXT-X-STREAM-INF or #EXT-X-I-FRAME-STREAM-INF tag
    size_t uri;   // line index of the URI line (== inf for an I-frame tag)
    int64_t height, bandwidth;
    char *codecs;
    bool iframe;
} hls_variant_line;

static bool variant_matches(const hls_variant_line *v, const char *rule, bool *usable) {
    *usable = true;
    if (!strncmp(rule, "height<=", 8)) return v->height > 0 && v->height <= strtoll(rule + 8, NULL, 10);
    if (!strncmp(rule, "height>=", 8)) return v->height > 0 && v->height >= strtoll(rule + 8, NULL, 10);
    if (!strncmp(rule, "height=", 7)) return v->height > 0 && v->height == strtoll(rule + 7, NULL, 10);
    if (!strncmp(rule, "bandwidth<=", 11)) return v->bandwidth > 0 && v->bandwidth <= strtoll(rule + 11, NULL, 10);
    if (!strncmp(rule, "codec=", 6)) return v->codecs && strstr(v->codecs, rule + 6);
    if (!strcmp(rule, "best") || !strcmp(rule, "worst")) return true;
    *usable = false;
    return false;
}

char *rs_m3u8_filter_master_video(const char *text, const char *filter, bool single) {
    if (!filter) filter = "";
    if (!text || (!filter[0] && !single) || !rs_m3u8_is_master(text)) return NULL;
    lines l;
    if (!split_lines(text, &l)) return NULL;

    hls_variant_line *vars = (hls_variant_line *)calloc(l.count, sizeof(*vars));
    bool *drop = (bool *)calloc(l.count, sizeof(bool));
    char *rules = rs_strdup(filter);
    char *out_text = NULL;
    size_t nvars = 0, nplayable = 0;
    if (!vars || !drop || !rules) goto done;

    for (size_t i = 0; i < l.count; i++) {
        slice t = slice_trimmed(l.items[i]);
        bool iframe = slice_has_prefix(t, "#EXT-X-I-FRAME-STREAM-INF:");
        if (!iframe && !slice_has_prefix(t, "#EXT-X-STREAM-INF:")) continue;
        size_t uri = i;
        if (!iframe) {
            // The URI is the next non-blank, non-tag line.
            for (uri = i + 1; uri < l.count; uri++) {
                slice u = slice_trimmed(l.items[uri]);
                if (u.len && u.ptr[0] != '#') break;
            }
            if (uri >= l.count) continue;
        }
        attrs a;
        parse_attributes(t, &a);
        int64_t width;
        hls_variant_line *v = &vars[nvars++];
        v->inf = i;
        v->uri = uri;
        v->iframe = iframe;
        attr_resolution(&a, &width, &v->height);
        v->bandwidth = attr_int(&a, "BANDWIDTH");
        const char *codecs = attrs_get(&a, "CODECS");
        v->codecs = codecs ? rs_strdup(codecs) : NULL;
        attrs_dispose(&a);
        if (!iframe) nplayable++;
    }
    if (!nplayable) goto done;

    // First rule that keeps at least one playable variant wins.
    for (char *rule = rules, *next; rule; rule = next) {
        next = strchr(rule, ',');
        if (next) *next++ = '\0';
        while (*rule == ' ' || *rule == '\t') rule++;
        char *end = rule + strlen(rule);
        while (end > rule && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        size_t kept = 0;
        bool usable = false;
        for (size_t k = 0; k < nvars; k++)
            if (!vars[k].iframe && variant_matches(&vars[k], rule, &usable)) kept++;
        if (!usable || !kept) continue;

        bool best = !strcmp(rule, "best"), worst = !strcmp(rule, "worst");
        int64_t pick = -1;
        if (best || worst) {
            for (size_t k = 0; k < nvars; k++) {
                if (vars[k].iframe) continue;
                if (pick < 0 || (best ? vars[k].bandwidth > pick : vars[k].bandwidth < pick)) pick = vars[k].bandwidth;
            }
        }
        for (size_t k = 0; k < nvars; k++) {
            bool keep;
            if (best || worst) keep = !vars[k].iframe && vars[k].bandwidth == pick;
            else keep = variant_matches(&vars[k], rule, &usable);
            if (!keep) {
                drop[vars[k].inf] = true;
                drop[vars[k].uri] = true;
            }
        }
        break;
    }

    // Single quality: of the playable variants still kept, only the
    // highest-bandwidth one stays (first listed wins a tie). Trick-play
    // I-frame variants go too — a one-quality restream has no use for them.
    if (single) {
        size_t chosen = nvars;
        for (size_t k = 0; k < nvars; k++) {
            if (vars[k].iframe || drop[vars[k].inf]) continue;
            if (chosen == nvars || vars[k].bandwidth > vars[chosen].bandwidth) chosen = k;
        }
        for (size_t k = 0; k < nvars; k++) {
            if (k == chosen) continue;
            drop[vars[k].inf] = true;
            drop[vars[k].uri] = true;
        }
    }

    {
        rs_buf out = RS_BUF_INIT;
        bool first = true;
        for (size_t i = 0; i < l.count; i++) {
            if (drop[i]) continue;
            if (!first) rs_buf_append_char(&out, '\n');
            rs_buf_append(&out, l.items[i].ptr, l.items[i].len);
            first = false;
        }
        out_text = rs_buf_take(&out);
    }

done:
    for (size_t k = 0; k < nvars; k++) free(vars[k].codecs);
    free(vars);
    free(drop);
    free(rules);
    lines_dispose(&l);
    return out_text;
}

// --- HLS sources for the internal engine -------------------------------------
//
// The live engine was built for DASH: it reads a manifest description, then
// prefetches, decrypts and republishes each rendition. These two functions let
// it take an HLS source the same way — one picks the renditions from a master,
// the other reads a media playlist into the segment window the engine wants.

static char *dup_or_null(const char *s) { return s ? rs_strdup(s) : NULL; }

// True when a CODECS entry names an audio codec.
static bool codec_is_audio(const char *c, size_t n) {
    static const char *const audio[] = {"mp4a", "ac-3", "ec-3", "opus", "flac", "mp3", "dtsc"};
    for (size_t i = 0; i < sizeof(audio) / sizeof(audio[0]); i++) {
        size_t k = strlen(audio[i]);
        if (n >= k && !strncmp(c, audio[i], k)) return true;
    }
    return false;
}

// The video (or audio) half of a variant's CODECS list, e.g. "avc1.64001f".
static char *codecs_part(const char *codecs, bool want_audio) {
    if (!codecs) return NULL;
    rs_buf b = RS_BUF_INIT;
    const char *p = codecs;
    while (*p) {
        const char *end = strchr(p, ',');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        while (n && *p == ' ') { p++; n--; }
        if (n && codec_is_audio(p, n) == want_audio) {
            if (b.len) rs_buf_append_char(&b, ',');
            rs_buf_append(&b, p, n);
        }
        if (!end) break;
        p = end + 1;
    }
    char *out = rs_buf_take(&b);
    if (out && !out[0]) { rs_free(out); return NULL; }
    return out;
}

void rs_hls_pick_dispose(rs_hls_pick *p) {
    if (!p) return;
    free(p->video_uri); free(p->video_id); free(p->video_codecs);
    free(p->audio_uri); free(p->audio_id); free(p->audio_codecs); free(p->audio_lang);
    memset(p, 0, sizeof(*p));
}

// A stable rendition id from its attributes (tokens in URIs rotate; these don't).
static char *hls_id(const char *prefix, const char *a, const char *b) {
    rs_buf out = RS_BUF_INIT;
    rs_buf_append_str(&out, prefix);
    for (const char *src = a; src && *src; src++)
        rs_buf_append_char(&out, isalnum((unsigned char)*src) ? *src : '_');
    if (b && b[0]) {
        rs_buf_append_char(&out, '-');
        for (const char *src = b; *src; src++)
            rs_buf_append_char(&out, isalnum((unsigned char)*src) ? *src : '_');
    }
    return rs_buf_take(&out);
}

int rs_hls_pick_renditions(const char *master, const char *base_url, const char *video_filter,
                           const char *audio_filter, rs_hls_pick *out) {
    if (!master || !base_url || !out) return -1;
    memset(out, 0, sizeof(*out));
    // One video variant: the best the provider's Default video rule allows.
    char *single = rs_m3u8_filter_master_video(master, video_filter ? video_filter : "", true);
    const char *text = single ? single : master;
    lines l;
    if (!split_lines(text, &l)) { rs_free(single); return -1; }

    char *audio_group = NULL;
    for (size_t i = 0; i < l.count && !out->video_uri; i++) {
        slice t = slice_trimmed(l.items[i]);
        if (!slice_has_prefix(t, "#EXT-X-STREAM-INF:")) continue;
        size_t uri = i + 1;
        for (; uri < l.count; uri++) {
            slice u = slice_trimmed(l.items[uri]);
            if (u.len && u.ptr[0] != '#') break;
        }
        if (uri >= l.count) break;
        attrs a;
        parse_attributes(t, &a);
        char *rel = slice_dup(slice_trimmed(l.items[uri]));
        out->video_uri = rel ? rs_url_resolve(base_url, rel) : NULL;
        free(rel);
        int64_t w, h;
        attr_resolution(&a, &w, &h);
        out->bandwidth = attr_int(&a, "BANDWIDTH");
        out->height = h;
        char bw[32], ht[32];
        snprintf(bw, sizeof(bw), "%lld", (long long)(out->bandwidth > 0 ? out->bandwidth : 0));
        snprintf(ht, sizeof(ht), "%lldp", (long long)(h > 0 ? h : 0));
        out->video_id = hls_id("hlsv-", ht, bw);
        out->video_codecs = codecs_part(attrs_get(&a, "CODECS"), false);
        out->audio_codecs = codecs_part(attrs_get(&a, "CODECS"), true);
        audio_group = dup_or_null(attrs_get(&a, "AUDIO"));
        attrs_dispose(&a);
    }

    // Its audio: the provider's lang= rule, else DEFAULT=YES, else the first
    // track of the variant's AUDIO group. No URI means the audio is muxed into
    // the video playlist, so there is no separate rendition to fetch.
    const char *want_lang = NULL;
    for (const char *p = audio_filter ? strstr(audio_filter, "lang=") : NULL; p; p = NULL)
        want_lang = p + 5;
    int best_rank = 99;
    for (size_t i = 0; audio_group && i < l.count; i++) {
        slice t = slice_trimmed(l.items[i]);
        if (!slice_has_prefix(t, "#EXT-X-MEDIA:")) continue;
        attrs a;
        parse_attributes(t, &a);
        const char *type = attrs_get(&a, "TYPE"), *group = attrs_get(&a, "GROUP-ID");
        const char *uri = attrs_get(&a, "URI"), *lang = attrs_get(&a, "LANGUAGE");
        const char *dflt = attrs_get(&a, "DEFAULT"), *name = attrs_get(&a, "NAME");
        if (type && !strcmp(type, "AUDIO") && group && !strcmp(group, audio_group) && uri) {
            int rank = 3;
            if (want_lang && lang) {
                size_t n = strcspn(want_lang, ",");
                if (!strncmp(lang, want_lang, n)) rank = 0;
            }
            if (rank > 1 && dflt && !strcmp(dflt, "YES")) rank = 1;
            if (rank < best_rank) {
                best_rank = rank;
                free(out->audio_uri); free(out->audio_id); free(out->audio_lang);
                out->audio_uri = rs_url_resolve(base_url, uri);
                out->audio_id = hls_id("hlsa-", group, name ? name : lang);
                out->audio_lang = dup_or_null(lang);
            }
        }
        attrs_dispose(&a);
    }
    if (!out->audio_uri) { free(out->audio_codecs); out->audio_codecs = NULL; }
    free(audio_group);
    lines_dispose(&l);
    rs_free(single);
    return out->video_uri ? 0 : -1;
}

void rs_hls_media_dispose(rs_hls_media *m) {
    if (!m) return;
    for (size_t i = 0; i < m->count; i++) free(m->segments[i].url);
    free(m->segments);
    free(m->init_url);
    memset(m, 0, sizeof(*m));
}

int rs_hls_media_parse(const char *text, const char *base_url, int want, rs_hls_media *out,
                       char *err, size_t err_len) {
    if (err && err_len) err[0] = '\0';
    if (!text || !base_url || !out) return -1;
    memset(out, 0, sizeof(*out));
    if (rs_m3u8_is_master(text)) {
        if (err) snprintf(err, err_len, "Expected an HLS media playlist, got a master.");
        return -1;
    }
    lines l;
    if (!split_lines(text, &l)) return -1;
    int64_t seq = rs_m3u8_media_sequence(text);
    double pending_duration = -1;
    size_t cap = 0;
    int rc = 0;
    for (size_t i = 0; i < l.count && rc == 0; i++) {
        slice t = slice_trimmed(l.items[i]);
        if (!t.len) continue;
        if (slice_has_prefix(t, "#EXT-X-TARGETDURATION:")) {
            out->target_duration = strtod(t.ptr + 22, NULL);
        } else if (slice_has_prefix(t, "#EXT-X-ENDLIST")) {
            out->ended = true;
        } else if (slice_has_prefix(t, "#EXT-X-MAP:")) {
            attrs a;
            parse_attributes(t, &a);
            const char *uri = attrs_get(&a, "URI");
            if (uri) { free(out->init_url); out->init_url = rs_url_resolve(base_url, uri); }
            if (attrs_get(&a, "BYTERANGE")) rc = -2;
            attrs_dispose(&a);
        } else if (slice_has_prefix(t, "#EXT-X-BYTERANGE")) {
            rc = -2;
        } else if (slice_has_prefix(t, "#EXT-X-KEY:")) {
            attrs a;
            parse_attributes(t, &a);
            const char *method = attrs_get(&a, "METHOD");
            // CENC (SAMPLE-AES-CTR, cbcs) lives inside fMP4 and the engine
            // decrypts it with the stream's keys; whole-segment AES-128 and
            // TS SAMPLE-AES do not.
            if (method && (!strcmp(method, "AES-128") ||
                           (!strcmp(method, "SAMPLE-AES") && !out->init_url))) rc = -3;
            attrs_dispose(&a);
        } else if (slice_has_prefix(t, "#EXTINF:")) {
            pending_duration = strtod(t.ptr + 8, NULL);
        } else if (t.ptr[0] != '#') {
            if (out->count == cap) {
                size_t next = cap ? cap * 2 : 64;
                rs_hls_segment *grown = (rs_hls_segment *)realloc(out->segments, next * sizeof(*grown));
                if (!grown) { rc = -1; break; }
                out->segments = grown;
                cap = next;
            }
            char *rel = slice_dup(t);
            rs_hls_segment *s = &out->segments[out->count++];
            s->url = rel ? rs_url_resolve(base_url, rel) : NULL;
            free(rel);
            s->sequence = seq++;
            s->duration = pending_duration > 0 ? pending_duration : out->target_duration;
            pending_duration = -1;
        }
    }
    lines_dispose(&l);
    if (rc == -2 && err) snprintf(err, err_len, "Byte-range HLS playlists aren't supported by the internal engine; use pass-through.");
    if (rc == -3 && err) snprintf(err, err_len, "AES-128 HLS isn't supported by the internal engine (only CENC fMP4); use pass-through or Buffered HLS.");
    if (rc != 0) { rs_hls_media_dispose(out); return -1; }
    // Keep only the newest `want` segments.
    if (want > 0 && out->count > (size_t)want) {
        size_t drop = out->count - (size_t)want;
        for (size_t i = 0; i < drop; i++) free(out->segments[i].url);
        memmove(out->segments, out->segments + drop, (size_t)want * sizeof(*out->segments));
        out->count = (size_t)want;
    }
    return 0;
}
