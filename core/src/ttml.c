#include "rs_ttml.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// See rs_ttml.h for what this is and why it does not use libxml2.
//
// Layout: a growable buffer, fMP4 `mdat` extraction, TTML time expressions, the
// element scanner that produces cues, and the WebVTT renderer.

// Nesting depth the scanner tracks for time containers. TTML allows arbitrary
// nesting, but a subtitle fragment is <tt><body><div><p>, so anything past this
// is malformed or hostile and is simply not offset.
#define TTML_MAX_DEPTH 32
// Open inline style spans tracked per cue (<i>/<b> emitted into the payload).
#define TTML_MAX_SPANS 16

// --- growable buffer --------------------------------------------------------

typedef struct {
    char *p;
    size_t len, cap;
    bool err;
} buf;

static bool buf_reserve(buf *b, size_t extra) {
    if (b->err) return false;
    if (b->p && b->len + extra + 1 <= b->cap) return true;
    size_t want = b->cap ? b->cap : 256;
    while (want < b->len + extra + 1) want *= 2;
    char *np = (char *)realloc(b->p, want);
    if (!np) { b->err = true; return false; }
    b->p = np;
    b->cap = want;
    if (b->len == 0) b->p[0] = '\0';
    return true;
}

static void buf_add(buf *b, const char *s, size_t n) {
    if (!n || !buf_reserve(b, n)) return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void buf_addz(buf *b, const char *s) { buf_add(b, s, strlen(s)); }

static void buf_addc(buf *b, char c) { buf_add(b, &c, 1); }

static void buf_addf(buf *b, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0 && buf_reserve(b, (size_t)n)) {
        vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap2);
        b->len += (size_t)n;
    }
    va_end(ap2);
}

// Hands the buffer's storage to the caller, or NULL if any append failed.
static char *buf_take(buf *b) {
    if (b->err) { free(b->p); b->p = NULL; return NULL; }
    if (!b->p) return rs_strdup("");
    char *p = b->p;
    b->p = NULL;
    return p;
}

// --- small character helpers ------------------------------------------------

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static bool is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == '.' || c == ':';
}

// Compares an element or attribute name against `want`, ignoring any namespace
// prefix and case. TTML in the wild is written both as <p> and <tt:p>, and the
// prefix is whatever the document happened to bind — matching on the local name
// is the only thing that works across packagers.
static bool name_is(const char *name, size_t len, const char *want) {
    const char *colon = (const char *)memchr(name, ':', len);
    if (colon) {
        len -= (size_t)(colon + 1 - name);
        name = colon + 1;
    }
    size_t wl = strlen(want);
    if (len != wl) return false;
    for (size_t i = 0; i < len; i++) {
        char a = name[i], b = want[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

// --- fMP4 mdat extraction ---------------------------------------------------

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t rd64(const uint8_t *p) {
    return ((uint64_t)rd32(p) << 32) | (uint64_t)rd32(p + 4);
}

// True when the buffer starts with something that is plausibly an ISO-BMFF box,
// which is how a `stpp` segment is told apart from a bare XML document.
static bool looks_like_mp4(const uint8_t *d, size_t len) {
    if (len < 8) return false;
    static const char *types[] = {"ftyp", "styp", "moof", "moov", "sidx",
                                  "mdat", "free", "skip", "emsg", "prft"};
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        if (memcmp(d + 4, types[i], 4) == 0) return true;
    return false;
}

// Concatenates every top-level `mdat` payload. A subtitle fragment normally
// holds one sample (one whole TTML document), but nothing forbids several, and
// the document scanner downstream handles a run of them.
static void collect_mdat(const uint8_t *d, size_t len, buf *out) {
    size_t off = 0;
    while (off + 8 <= len) {
        uint64_t size = rd32(d + off);
        size_t hdr = 8;
        if (size == 1) {
            if (off + 16 > len) return;
            size = rd64(d + off + 8);
            hdr = 16;
        } else if (size == 0) {
            size = len - off;
        }
        if (size < hdr || off + (size_t)size > len) return;
        if (memcmp(d + off + 4, "mdat", 4) == 0)
            buf_add(out, (const char *)(d + off + hdr), (size_t)size - hdr);
        off += (size_t)size;
    }
}

// --- TTML time expressions --------------------------------------------------

// Reads a decimal number at `s`, advancing `s` past it. Returns false when
// there is no number there.
static bool read_number(const char **s, const char *end, double *out) {
    const char *p = *s;
    bool any = false;
    double v = 0;
    while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; any = true; }
    if (p < end && *p == '.') {
        p++;
        double scale = 0.1;
        while (p < end && *p >= '0' && *p <= '9') { v += (*p - '0') * scale; scale *= 0.1; p++; any = true; }
    }
    if (!any) return false;
    *s = p;
    *out = v;
    return true;
}

// Parses a TTML time expression (TTML1 section 10.3.1) to seconds, or -1 when
// it is absent or malformed.
//
// Two shapes exist and both turn up in DASH: clock time "hh:mm:ss.fff" (and the
// frame-counting "hh:mm:ss:ff" variant, which is why the colon count has to be
// checked before parsing), and offset time — a number with a metric suffix,
// where "t" is ticks against ttp:tickRate and "f" is frames against
// ttp:frameRate.
static double parse_time(const char *s, size_t len, double tick_rate, double frame_rate) {
    while (len && is_space(*s)) { s++; len--; }
    while (len && is_space(s[len - 1])) len--;
    if (!len) return -1;

    const char *end = s + len;
    int colons = 0;
    for (size_t i = 0; i < len; i++) if (s[i] == ':') colons++;

    if (colons >= 2) {
        const char *p = s;
        double h, m, sec = 0;
        if (!read_number(&p, end, &h) || p >= end || *p != ':') return -1;
        p++;
        if (!read_number(&p, end, &m) || p >= end || *p != ':') return -1;
        p++;
        if (!read_number(&p, end, &sec)) return -1;
        double total = h * 3600 + m * 60 + sec;
        // "hh:mm:ss:ff" — the fourth field counts frames, not a fraction.
        if (p < end && *p == ':') {
            p++;
            double frames = 0;
            if (read_number(&p, end, &frames) && frame_rate > 0) total += frames / frame_rate;
        }
        return total;
    }

    const char *p = s;
    double v;
    if (!read_number(&p, end, &v)) return -1;
    size_t mlen = (size_t)(end - p);
    if (mlen == 0) return v;  // bare number: seconds
    if (mlen == 1 && *p == 'h') return v * 3600;
    if (mlen == 1 && *p == 'm') return v * 60;
    if (mlen == 1 && *p == 's') return v;
    if (mlen == 2 && p[0] == 'm' && p[1] == 's') return v / 1000.0;
    if (mlen == 1 && *p == 'f') return frame_rate > 0 ? v / frame_rate : -1;
    if (mlen == 1 && *p == 't') return tick_rate > 0 ? v / tick_rate : -1;
    return -1;
}

// --- entity decoding --------------------------------------------------------

// Appends `cp` to `out` as UTF-8.
static void add_utf8(buf *out, unsigned long cp) {
    if (cp < 0x80) {
        buf_addc(out, (char)cp);
    } else if (cp < 0x800) {
        buf_addc(out, (char)(0xC0 | (cp >> 6)));
        buf_addc(out, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        buf_addc(out, (char)(0xE0 | (cp >> 12)));
        buf_addc(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_addc(out, (char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        buf_addc(out, (char)(0xF0 | (cp >> 18)));
        buf_addc(out, (char)(0x80 | ((cp >> 12) & 0x3F)));
        buf_addc(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_addc(out, (char)(0x80 | (cp & 0x3F)));
    }
}

// Decodes one entity reference starting at `s` (which points at '&'), appending
// the result to `out`. Returns the number of bytes consumed, or 0 when this is
// not a well-formed reference — in which case the caller emits the '&' as text,
// which is what a browser does with a stray ampersand too.
static size_t decode_entity(const char *s, size_t len, buf *out) {
    if (len < 3 || s[0] != '&') return 0;
    const char *semi = (const char *)memchr(s, ';', len < 12 ? len : 12);
    if (!semi) return 0;
    size_t body = (size_t)(semi - s) - 1;
    const char *name = s + 1;

    if (body == 2 && memcmp(name, "lt", 2) == 0) { buf_addc(out, '<'); return body + 2; }
    if (body == 2 && memcmp(name, "gt", 2) == 0) { buf_addc(out, '>'); return body + 2; }
    if (body == 3 && memcmp(name, "amp", 3) == 0) { buf_addc(out, '&'); return body + 2; }
    if (body == 4 && memcmp(name, "quot", 4) == 0) { buf_addc(out, '"'); return body + 2; }
    if (body == 4 && memcmp(name, "apos", 4) == 0) { buf_addc(out, '\''); return body + 2; }

    if (body >= 2 && name[0] == '#') {
        unsigned long cp = 0;
        bool hex = name[1] == 'x' || name[1] == 'X';
        const char *p = name + (hex ? 2 : 1);
        const char *e = semi;
        if (p >= e) return 0;
        for (; p < e; p++) {
            unsigned d;
            if (*p >= '0' && *p <= '9') d = (unsigned)(*p - '0');
            else if (hex && *p >= 'a' && *p <= 'f') d = (unsigned)(*p - 'a' + 10);
            else if (hex && *p >= 'A' && *p <= 'F') d = (unsigned)(*p - 'A' + 10);
            else return 0;
            cp = cp * (hex ? 16u : 10u) + d;
            if (cp > 0x10FFFF) return 0;
        }
        add_utf8(out, cp);
        return body + 2;
    }
    return 0;
}

// --- the element scanner ----------------------------------------------------

typedef struct {
    rs_ttml_cue *cues;
    size_t count, cap;
} cue_list;

static bool cue_push(cue_list *l, double begin, double end, char *text) {
    if (l->count == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 16;
        rs_ttml_cue *n = (rs_ttml_cue *)realloc(l->cues, cap * sizeof(*n));
        if (!n) return false;
        l->cues = n;
        l->cap = cap;
    }
    l->cues[l->count].begin = begin;
    l->cues[l->count].end = end;
    l->cues[l->count].text = text;
    l->count++;
    return true;
}

// One attribute of a start tag.
typedef struct {
    const char *name;
    size_t name_len;
    const char *value;
    size_t value_len;
} attr;

// Reads attributes out of a start tag body (everything after the element name,
// up to but not including the closing '>').
static size_t scan_attrs(const char *s, size_t len, attr *out, size_t max) {
    size_t n = 0, i = 0;
    while (i < len && n < max) {
        while (i < len && is_space(s[i])) i++;
        if (i >= len || s[i] == '/' || s[i] == '>') break;
        size_t start = i;
        while (i < len && is_name_char(s[i])) i++;
        if (i == start) { i++; continue; }
        attr a;
        a.name = s + start;
        a.name_len = i - start;
        a.value = NULL;
        a.value_len = 0;
        while (i < len && is_space(s[i])) i++;
        if (i < len && s[i] == '=') {
            i++;
            while (i < len && is_space(s[i])) i++;
            if (i < len && (s[i] == '"' || s[i] == '\'')) {
                char q = s[i++];
                size_t vs = i;
                while (i < len && s[i] != q) i++;
                a.value = s + vs;
                a.value_len = i - vs;
                if (i < len) i++;
            } else {
                size_t vs = i;
                while (i < len && !is_space(s[i]) && s[i] != '>') i++;
                a.value = s + vs;
                a.value_len = i - vs;
            }
        }
        out[n++] = a;
    }
    return n;
}

// Finds an attribute by local name, or NULL.
static const attr *attr_find(const attr *a, size_t n, const char *want) {
    for (size_t i = 0; i < n; i++)
        if (name_is(a[i].name, a[i].name_len, want)) return &a[i];
    return NULL;
}

static double attr_time(const attr *a, size_t n, const char *want,
                        double tick_rate, double frame_rate) {
    const attr *f = attr_find(a, n, want);
    if (!f || !f->value) return -1;
    return parse_time(f->value, f->value_len, tick_rate, frame_rate);
}

// Appends character data to a cue payload, collapsing whitespace runs the way
// xml:space="default" requires and escaping the two characters WebVTT treats as
// markup. `pending_space` carries a collapsed run across a tag boundary so
// "a <span>b</span>" does not lose the space between the words.
static void add_text(buf *out, const char *s, size_t len, bool *pending_space) {
    size_t i = 0;
    while (i < len) {
        char c = s[i];
        if (is_space(c)) {
            // Not after a <br/>: a collapsed run either side of a hard break is
            // part of the break, not a leading space on the next line.
            if (out->len > 0 && out->p[out->len - 1] != '\n') *pending_space = true;
            i++;
            continue;
        }
        if (*pending_space) { buf_addc(out, ' '); *pending_space = false; }
        if (c == '&') {
            size_t used = decode_entity(s + i, len - i, out);
            if (used) {
                // The decoded character re-enters as text, so anything WebVTT
                // would read as markup has to be escaped again. Only '&' and
                // '<' can appear here in a form that matters.
                if (out->len && out->p[out->len - 1] == '&') { out->len--; buf_addz(out, "&amp;"); }
                else if (out->len && out->p[out->len - 1] == '<') { out->len--; buf_addz(out, "&lt;"); }
                i += used;
                continue;
            }
            buf_addz(out, "&amp;");
            i++;
            continue;
        }
        if (c == '<') { buf_addz(out, "&lt;"); i++; continue; }
        buf_addc(out, c);
        i++;
    }
}

// Maps a <span>'s styling to the two inline tags WebVTT actually has. Returns
// the tag name to open ("i", "b") or NULL when the span carries no styling this
// can express — positioning and colour are dropped rather than approximated.
static const char *span_tag(const attr *a, size_t n) {
    const attr *style = attr_find(a, n, "fontStyle");
    if (style && style->value && style->value_len == 6 && memcmp(style->value, "italic", 6) == 0)
        return "i";
    const attr *weight = attr_find(a, n, "fontWeight");
    if (weight && weight->value && weight->value_len == 4 && memcmp(weight->value, "bold", 4) == 0)
        return "b";
    return NULL;
}

// Parses one TTML document (the region from `<tt` to its matching close) into
// cues, appending to `list`.
static void parse_document(const char *s, size_t len, cue_list *list) {
    double tick_rate = 0, frame_rate = 0;

    // Time containers are `par` by default, so a <div begin=...> shifts every
    // <p> inside it. offsets[depth] accumulates that down the tree.
    double offsets[TTML_MAX_DEPTH];
    size_t depth = 0;
    offsets[0] = 0;

    int body_depth = -1;   // depth at which <body> opened, or -1 outside it
    bool in_p = false;
    double p_begin = -1, p_end = -1, p_offset = 0;
    buf text = {0};
    bool pending_space = false;
    const char *spans[TTML_MAX_SPANS];
    size_t nspans = 0;

    size_t i = 0;
    while (i < len) {
        const char *lt = (const char *)memchr(s + i, '<', len - i);
        if (!lt) {
            if (in_p) add_text(&text, s + i, len - i, &pending_space);
            break;
        }
        size_t tag_at = (size_t)(lt - s);
        if (in_p && tag_at > i) add_text(&text, s + i, tag_at - i, &pending_space);

        // Skip the constructs that carry no cue content. A comment has to be
        // matched on "-->" rather than the next '>', or a "<!-- a > b -->"
        // resumes parsing in the middle of the comment.
        if (tag_at + 4 <= len && memcmp(s + tag_at, "<!--", 4) == 0) {
            const char *e = NULL;
            for (size_t k = tag_at + 4; k + 3 <= len; k++)
                if (memcmp(s + k, "-->", 3) == 0) { e = s + k; break; }
            if (!e) break;
            i = (size_t)(e - s) + 3;
            continue;
        }
        if (tag_at + 1 < len && (s[tag_at + 1] == '?' || s[tag_at + 1] == '!')) {
            const char *gt = (const char *)memchr(s + tag_at, '>', len - tag_at);
            if (!gt) break;
            i = (size_t)(gt - s) + 1;
            continue;
        }

        const char *gt = (const char *)memchr(s + tag_at, '>', len - tag_at);
        if (!gt) break;
        size_t tag_end = (size_t)(gt - s);
        bool closing = tag_at + 1 < len && s[tag_at + 1] == '/';
        size_t name_at = tag_at + (closing ? 2 : 1);
        size_t name_end = name_at;
        while (name_end < tag_end && is_name_char(s[name_end])) name_end++;
        const char *name = s + name_at;
        size_t name_len = name_end - name_at;
        bool self_closing = tag_end > tag_at && s[tag_end - 1] == '/';
        i = tag_end + 1;
        if (name_len == 0) continue;

        if (closing) {
            if (in_p && name_is(name, name_len, "p")) {
                if (p_begin >= 0) {
                    // A cue with no end runs to the end of the fragment, which
                    // the renderer cannot know; TTML's own default is that an
                    // unspecified end is indefinite, so give it a bounded
                    // display time rather than dropping it.
                    double end = p_end >= 0 ? p_end : p_begin + 5.0;
                    while (nspans) { buf_addf(&text, "</%s>", spans[--nspans]); }
                    char *payload = buf_take(&text);
                    memset(&text, 0, sizeof(text));
                    if (!payload || !cue_push(list, p_begin + p_offset, end + p_offset, payload))
                        free(payload);
                } else {
                    free(text.p);
                    memset(&text, 0, sizeof(text));
                }
                in_p = false;
                nspans = 0;
                pending_space = false;
            } else if (in_p && name_is(name, name_len, "span")) {
                if (nspans) buf_addf(&text, "</%s>", spans[--nspans]);
            } else if (!in_p) {
                // Depth first, so it names the level the element opened at —
                // <body> recorded body_depth before its own push.
                if (depth > 0) depth--;
                if (body_depth >= 0 && (size_t)body_depth == depth && name_is(name, name_len, "body"))
                    body_depth = -1;
            }
            continue;
        }

        attr attrs[24];
        size_t nattrs = 0;
        if (name_end < tag_end) {
            size_t body_len = tag_end - name_end;
            if (self_closing && body_len) body_len--;
            nattrs = scan_attrs(s + name_end, body_len, attrs, sizeof(attrs) / sizeof(attrs[0]));
        }

        if (name_is(name, name_len, "tt")) {
            const attr *tr = attr_find(attrs, nattrs, "tickRate");
            if (tr && tr->value) tick_rate = strtod(tr->value, NULL);
            const attr *fr = attr_find(attrs, nattrs, "frameRate");
            if (fr && fr->value) frame_rate = strtod(fr->value, NULL);
            // frameRateMultiplier turns the integer frameRate into the real one
            // ("30" with "1000 1001" is 29.97), which matters because a whole
            // second of drift accumulates over half an hour.
            const attr *fm = attr_find(attrs, nattrs, "frameRateMultiplier");
            if (fm && fm->value && frame_rate > 0) {
                char tmp[64];
                size_t n = fm->value_len < sizeof(tmp) - 1 ? fm->value_len : sizeof(tmp) - 1;
                memcpy(tmp, fm->value, n);
                tmp[n] = '\0';
                char *rest = NULL;
                double num = strtod(tmp, &rest);
                double den = rest ? strtod(rest, NULL) : 0;
                if (num > 0 && den > 0) frame_rate = frame_rate * num / den;
            }
            if (!self_closing && depth + 1 < TTML_MAX_DEPTH) { offsets[depth + 1] = offsets[depth]; depth++; }
            continue;
        }

        if (in_p) {
            if (name_is(name, name_len, "br")) {
                pending_space = false;
                buf_addc(&text, '\n');
            } else if (name_is(name, name_len, "span")) {
                const char *tag = span_tag(attrs, nattrs);
                if (tag && !self_closing && nspans < TTML_MAX_SPANS) {
                    // The collapsed space that preceded this span belongs
                    // outside it — held back, it would open the styled run with
                    // a space ("a<i> whisper</i>") instead of separating words.
                    if (pending_space) { buf_addc(&text, ' '); pending_space = false; }
                    buf_addf(&text, "<%s>", tag);
                    spans[nspans++] = tag;
                }
            }
            continue;
        }

        if (name_is(name, name_len, "body")) {
            if (body_depth < 0) body_depth = (int)depth;
        }

        if (body_depth >= 0 && name_is(name, name_len, "p")) {
            p_begin = attr_time(attrs, nattrs, "begin", tick_rate, frame_rate);
            p_end = attr_time(attrs, nattrs, "end", tick_rate, frame_rate);
            double dur = attr_time(attrs, nattrs, "dur", tick_rate, frame_rate);
            if (p_end < 0 && dur >= 0 && p_begin >= 0) p_end = p_begin + dur;
            p_offset = offsets[depth];
            free(text.p);
            memset(&text, 0, sizeof(text));
            pending_space = false;
            nspans = 0;
            if (self_closing) {
                // <p begin=... /> with no content carries no cue text; nothing
                // to show, so it is skipped rather than emitted blank.
                p_begin = -1;
            } else {
                in_p = true;
            }
            continue;
        }

        // A container element: carry its begin down to its children.
        if (!self_closing && depth + 1 < TTML_MAX_DEPTH) {
            double b = attr_time(attrs, nattrs, "begin", tick_rate, frame_rate);
            offsets[depth + 1] = offsets[depth] + (b > 0 ? b : 0);
            depth++;
        }
    }

    free(text.p);
}

// --- public API -------------------------------------------------------------

int rs_ttml_parse(const uint8_t *data, size_t len, rs_ttml_cue **out, size_t *out_count) {
    if (out) *out = NULL;
    if (out_count) *out_count = 0;
    if (!data || !len || !out || !out_count) return -1;

    buf unwrapped = {0};
    const char *xml = (const char *)data;
    size_t xml_len = len;
    bool was_mp4 = looks_like_mp4(data, len);
    if (was_mp4) {
        collect_mdat(data, len, &unwrapped);
        if (unwrapped.err) { free(unwrapped.p); return -1; }
        xml = unwrapped.p ? unwrapped.p : "";
        xml_len = unwrapped.len;
    }

    cue_list list = {NULL, 0, 0};
    bool found_any = false;

    // A fragment can carry several samples, so walk every <tt> document in the
    // payload rather than assuming one.
    size_t pos = 0;
    while (pos < xml_len) {
        const char *tt = NULL;
        for (size_t k = pos; k + 3 <= xml_len; k++) {
            if (xml[k] != '<') continue;
            size_t nl = k + 1;
            while (nl < xml_len && is_name_char(xml[nl])) nl++;
            if (nl > k + 1 && name_is(xml + k + 1, nl - k - 1, "tt") &&
                (nl >= xml_len || is_space(xml[nl]) || xml[nl] == '>' || xml[nl] == '/')) {
                tt = xml + k;
                break;
            }
        }
        if (!tt) break;
        found_any = true;
        size_t start = (size_t)(tt - xml);

        // The document ends at its closing tag; without one, take the rest.
        size_t end = xml_len;
        for (size_t k = start; k + 4 <= xml_len; k++) {
            if (xml[k] != '<' || xml[k + 1] != '/') continue;
            size_t nl = k + 2;
            while (nl < xml_len && is_name_char(xml[nl])) nl++;
            if (name_is(xml + k + 2, nl - k - 2, "tt")) {
                const char *gt = (const char *)memchr(xml + nl, '>', xml_len - nl);
                end = gt ? (size_t)(gt - xml) + 1 : xml_len;
                break;
            }
        }
        parse_document(xml + start, end - start, &list);
        pos = end > start ? end : start + 1;
    }

    free(unwrapped.p);
    if (!found_any && !was_mp4) {
        rs_ttml_cues_free(list.cues, list.count);
        return -1;
    }
    *out = list.cues;
    *out_count = list.count;
    return 0;
}

void rs_ttml_cues_free(rs_ttml_cue *cues, size_t count) {
    if (!cues) return;
    for (size_t i = 0; i < count; i++) free(cues[i].text);
    free(cues);
}

// "hh:mm:ss.mmm", the long form, which every WebVTT parser accepts.
static void write_timestamp(buf *b, double t) {
    if (t < 0) t = 0;
    long long ms = (long long)(t * 1000.0 + 0.5);
    long long h = ms / 3600000;
    ms -= h * 3600000;
    long long m = ms / 60000;
    ms -= m * 60000;
    long long s = ms / 1000;
    ms -= s * 1000;
    buf_addf(b, "%02lld:%02lld:%02lld.%03lld", h, m, s, ms);
}

char *rs_ttml_render_webvtt(const rs_ttml_cue *cues, size_t count,
                            double shift, double map_seconds) {
    buf b = {0};
    buf_addz(&b, "WEBVTT\n");
    if (map_seconds >= 0) {
        // Anchors this fragment's local clock against the media timeline. Both
        // halves name the same instant — the cue times below are already on the
        // presentation timeline — so the mapping is an identity that tells the
        // player where the fragment sits without re-basing anything.
        buf_addz(&b, "X-TIMESTAMP-MAP=MPEGTS:");
        buf_addf(&b, "%lld", (long long)(map_seconds * 90000.0 + 0.5));
        buf_addz(&b, ",LOCAL:");
        write_timestamp(&b, map_seconds);
        buf_addc(&b, '\n');
    }
    for (size_t i = 0; i < count; i++) {
        if (!cues[i].text || !cues[i].text[0]) continue;
        double begin = cues[i].begin + shift;
        double end = cues[i].end + shift;
        if (end <= begin) end = begin + 0.001;
        buf_addc(&b, '\n');
        write_timestamp(&b, begin);
        buf_addz(&b, " --> ");
        write_timestamp(&b, end);
        buf_addc(&b, '\n');
        buf_addz(&b, cues[i].text);
        buf_addc(&b, '\n');
    }
    return buf_take(&b);
}

char *rs_ttml_to_webvtt(const uint8_t *data, size_t len,
                        double segment_start, double segment_duration) {
    if (!data || !len) return NULL;

    // Already WebVTT (a text/vtt DASH rendition, or an HLS sidecar): hand it
    // straight back, since re-parsing it could only lose information.
    size_t lead = 0;
    while (lead < len && (data[lead] == 0xEF || data[lead] == 0xBB || data[lead] == 0xBF ||
                          is_space((char)data[lead]))) lead++;
    if (len - lead >= 6 && memcmp(data + lead, "WEBVTT", 6) == 0) {
        char *copy = (char *)malloc(len + 1);
        if (!copy) return NULL;
        memcpy(copy, data, len);
        copy[len] = '\0';
        return copy;
    }

    rs_ttml_cue *cues = NULL;
    size_t count = 0;
    if (rs_ttml_parse(data, len, &cues, &count) != 0) return NULL;

    // Fragment-relative or presentation-relative? The document does not say,
    // and packagers do both. Cues that all sit inside one segment's worth of
    // time while the segment itself starts much later can only be the former,
    // so lift them onto the media timeline.
    double shift = 0;
    if (count > 0 && segment_start > 0 && segment_duration > 0) {
        double max_end = 0;
        for (size_t i = 0; i < count; i++)
            if (cues[i].end > max_end) max_end = cues[i].end;
        double window = segment_duration * 1.5;
        if (max_end <= window && segment_start > window) shift = segment_start;
    }

    char *vtt = rs_ttml_render_webvtt(cues, count, shift, segment_start >= 0 ? segment_start : -1);
    rs_ttml_cues_free(cues, count);
    return vtt;
}
