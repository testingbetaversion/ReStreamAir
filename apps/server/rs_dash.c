#include "rs_dash.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <pthread.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include "rs_common.h"
#include "rs_url.h"
#include "rs_json.h"
#include "net.h"

// A live-DASH SegmentTemplate/SegmentTimeline expander — the C port of the
// pieces of DASH manifest handling needed to turn one MPD poll
// into a per-representation download plan. Handles the common live cases:
// SegmentTemplate with a SegmentTimeline ($Time$) or with @duration ($Number$),
// template inheritance (AdaptationSet -> Representation), and BaseURL stacking.
// SegmentBase/SegmentList and $SubNumber$ low-latency chunks are not handled.

// --- libxml2 helpers (same idiom as probe.c) -------------------------------

static bool node_is(xmlNode *n, const char *name) {
    return n->type == XML_ELEMENT_NODE && n->name && strcmp((const char *)n->name, name) == 0;
}
static char *attr(xmlNode *n, const char *name) {
    xmlChar *v = xmlGetProp(n, (const xmlChar *)name);
    if (!v) return NULL;
    char *c = rs_strdup((const char *)v);
    xmlFree(v);
    return c;
}
// Direct-child text of the first <BaseURL> under `n` (whitespace-trimmed), or NULL.
static char *base_url_child(xmlNode *n) {
    for (xmlNode *c = n->children; c; c = c->next) {
        if (!node_is(c, "BaseURL")) continue;
        xmlChar *txt = xmlNodeGetContent(c);
        if (!txt) return NULL;
        const char *s = (const char *)txt;
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
        size_t len = strlen(s);
        while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) len--;
        char *out = NULL;
        if (len) { out = malloc(len + 1); if (out) { memcpy(out, s, len); out[len] = '\0'; } }
        xmlFree(txt);
        return out;
    }
    return NULL;
}

// ISO-8601 "PT#H#M#S" (and days) to seconds. 0 when absent/malformed.
static double parse_duration(const char *s) {
    if (!s || s[0] != 'P') return 0;
    double total = 0, num = 0; bool have = false, in_time = false;
    for (const char *p = s + 1; *p; p++) {
        if (*p == 'T') { in_time = true; continue; }
        if ((*p >= '0' && *p <= '9') || *p == '.') { char *end; num = strtod(p, &end); p = end - 1; have = true; continue; }
        if (!have) continue;
        switch (*p) {
            case 'D': total += num * 86400; break;
            case 'H': total += num * 3600; break;
            case 'M': total += num * (in_time ? 60 : 2592000); break;
            case 'S': total += num; break;
            default: break;
        }
        num = 0; have = false;
    }
    return total;
}

// ISO-8601 UTC timestamp ("2026-07-30T12:34:56.789Z") to seconds since the Unix
// epoch, or -1 when absent or unparseable. This is MPD@availabilityStartTime,
// which is what anchors a $Number$-addressed live stream to the wall clock.
// A trailing numeric zone offset is not honoured — DASH publishes these in UTC.
static double parse_datetime(const char *s) {
    if (!s || !s[0]) return -1;
    int y, mo, d, h, mi;
    double sec = 0;
    if (sscanf(s, "%d-%d-%dT%d:%d:%lf", &y, &mo, &d, &h, &mi, &sec) < 6) return -1;
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = 0;
#ifdef _WIN32
    time_t base = _mkgmtime(&tm);
#else
    time_t base = timegm(&tm);
#endif
    if (base == (time_t)-1) return -1;
    return (double)base + sec;
}

// --- template token substitution -------------------------------------------

// Replace every "$token$" in `in` with `val`; returns malloc'd result.
static char *replace_plain(const char *in, const char *token, const char *val) {
    char needle[64]; snprintf(needle, sizeof(needle), "$%s$", token);
    size_t nlen = strlen(needle), vlen = strlen(val);
    // Count occurrences for sizing.
    size_t count = 0;
    for (const char *p = strstr(in, needle); p; p = strstr(p + nlen, needle)) count++;
    char *out = malloc(strlen(in) + count * (vlen > nlen ? vlen - nlen : 0) + 1);
    if (!out) return rs_strdup(in);
    char *w = out;
    for (const char *p = in; *p; ) {
        if (strncmp(p, needle, nlen) == 0) { memcpy(w, val, vlen); w += vlen; p += nlen; }
        else *w++ = *p++;
    }
    *w = '\0';
    return out;
}

// Replace "$token%0Nd$" width-padded forms with the numeric value.
static char *replace_padded(const char *in, const char *token, long long value) {
    char prefix[64]; snprintf(prefix, sizeof(prefix), "$%s%%0", token);
    size_t plen = strlen(prefix);
    if (!strstr(in, prefix)) return rs_strdup(in);
    size_t cap = strlen(in) + 64;
    char *out = malloc(cap); if (!out) return rs_strdup(in);
    char *w = out;
    for (const char *p = in; *p; ) {
        if (strncmp(p, prefix, plen) == 0) {
            const char *q = p + plen;
            int width = 0; bool digits = false;
            while (*q >= '0' && *q <= '9') { width = width * 10 + (*q - '0'); q++; digits = true; }
            if (digits && *q == 'd' && *(q + 1) == '$') {
                char num[64]; snprintf(num, sizeof(num), "%0*lld", width, value);
                size_t used = (size_t)(w - out), need = used + strlen(num) + strlen(q + 2) + 1;
                if (need > cap) { cap = need * 2; char *g = realloc(out, cap); if (!g) { free(out); return rs_strdup(in); } out = g; w = out + used; }
                memcpy(w, num, strlen(num)); w += strlen(num);
                p = q + 2;
                continue;
            }
        }
        size_t used = (size_t)(w - out);
        if (used + 2 > cap) { cap *= 2; char *g = realloc(out, cap); if (!g) { free(out); return rs_strdup(in); } out = g; w = out + used; }
        *w++ = *p++;
    }
    *w = '\0';
    return out;
}

// Fill a media/initialization template for one segment. `number`/`time` < 0 = absent.
static char *fill_template(const char *tmpl, const char *rep_id, const char *bandwidth,
                           long long number, long long time) {
    char *a = replace_plain(tmpl, "RepresentationID", rep_id ? rep_id : "");
    char *b = replace_plain(a, "Bandwidth", bandwidth ? bandwidth : ""); free(a);
    if (number >= 0) { char n[32]; snprintf(n, sizeof(n), "%lld", number); char *c = replace_plain(b, "Number", n); free(b); b = c; c = replace_padded(b, "Number", number); free(b); b = c; }
    if (time >= 0) { char t[32]; snprintf(t, sizeof(t), "%lld", time); char *c = replace_plain(b, "Time", t); free(b); b = c; c = replace_padded(b, "Time", time); free(b); b = c; }
    return b;
}

// --- SegmentTemplate model -------------------------------------------------

typedef struct { long long t; long long d; long long r; } tl_entry;

typedef struct {
    char *media;
    char *initialization;
    double duration;      // @duration (0 if absent)
    unsigned long timescale;
    long long start_number;
    long long pto;        // presentationTimeOffset
    tl_entry *timeline;
    size_t timeline_count;
    bool have;
} seg_template;

static void template_from_node(xmlNode *st, seg_template *out) {
    char *v;
    if ((v = attr(st, "media"))) { out->media = v; }
    if ((v = attr(st, "initialization"))) { out->initialization = v; }
    if ((v = attr(st, "duration"))) { out->duration = strtod(v, NULL); free(v); }
    if ((v = attr(st, "timescale"))) { out->timescale = strtoul(v, NULL, 10); free(v); }
    if ((v = attr(st, "startNumber"))) { out->start_number = strtoll(v, NULL, 10); free(v); }
    if ((v = attr(st, "presentationTimeOffset"))) { out->pto = strtoll(v, NULL, 10); free(v); }
    out->have = true;
    for (xmlNode *c = st->children; c; c = c->next) {
        if (!node_is(c, "SegmentTimeline")) continue;
        for (xmlNode *s = c->children; s; s = s->next) {
            if (!node_is(s, "S")) continue;
            tl_entry e = { -1, 0, 0 };
            char *a;
            if ((a = attr(s, "t"))) { e.t = strtoll(a, NULL, 10); free(a); }
            if ((a = attr(s, "d"))) { e.d = strtoll(a, NULL, 10); free(a); }
            if ((a = attr(s, "r"))) { e.r = strtoll(a, NULL, 10); free(a); }
            out->timeline = realloc(out->timeline, (out->timeline_count + 1) * sizeof(tl_entry));
            out->timeline[out->timeline_count++] = e;
        }
    }
}

// Child overrides parent field-by-field; child timeline wins if non-empty.
static void merge_template(const seg_template *parent, const seg_template *child, seg_template *out) {
    memset(out, 0, sizeof(*out));
    out->media = rs_strdup(child->media ? child->media : (parent->media ? parent->media : ""));
    out->initialization = rs_strdup(child->initialization ? child->initialization
                                    : (parent->initialization ? parent->initialization : ""));
    out->duration = child->duration > 0 ? child->duration : parent->duration;
    out->timescale = child->timescale ? child->timescale : (parent->timescale ? parent->timescale : 1);
    out->start_number = child->start_number ? child->start_number : parent->start_number;
    out->pto = child->pto ? child->pto : parent->pto;
    const seg_template *tl = child->timeline_count ? child : parent;
    out->timeline_count = tl->timeline_count;
    if (out->timeline_count) {
        out->timeline = malloc(out->timeline_count * sizeof(tl_entry));
        memcpy(out->timeline, tl->timeline, out->timeline_count * sizeof(tl_entry));
    }
    out->have = parent->have || child->have;
}

static void template_dispose(seg_template *t) {
    if (!t) return;
    free(t->media); free(t->initialization); free(t->timeline);
    memset(t, 0, sizeof(*t));
}

// --- adaptation type classification ----------------------------------------

static const char *classify(const char *mime, const char *content_type) {
    if (content_type && content_type[0]) {
        if (strncmp(content_type, "video", 5) == 0) return "video";
        if (strncmp(content_type, "audio", 5) == 0) return "audio";
        if (strncmp(content_type, "text", 4) == 0) return "text";
    }
    if (mime) {
        if (strncmp(mime, "video", 5) == 0) return "video";
        if (strncmp(mime, "audio", 5) == 0) return "audio";
        if (strncmp(mime, "text", 4) == 0 || strncmp(mime, "application", 11) == 0) return "text";
    }
    return "video";
}

// Stack a relative/absolute BaseURL onto `base`, returning a directory URL
// (trailing slash) the way Foundation's relativeTo resolution does.
static char *stack_base(const char *base, const char *piece) {
    if (!piece || !piece[0]) return rs_strdup(base);
    char *resolved = rs_url_resolve(base, piece);
    if (!resolved) return rs_strdup(base);
    size_t n = strlen(resolved);
    if (n && resolved[n - 1] != '/') {
        char *slash = malloc(n + 2);
        memcpy(slash, resolved, n); slash[n] = '/'; slash[n + 1] = '\0';
        free(resolved); resolved = slash;
    }
    return resolved;
}

// --- plan build ------------------------------------------------------------

int rs_dash_plan_build(const char *mpd_xml, size_t len, const char *mpd_url,
                       const char *representation_id, int want,
                       rs_dash_plan *out, char *errbuf, size_t errbuf_len) {
    memset(out, 0, sizeof(*out));
    xmlDoc *doc = xmlReadMemory(mpd_xml, (int)len, "mpd.xml", NULL,
                                XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_RECOVER);
    if (!doc) { snprintf(errbuf, errbuf_len, "Could not parse the MPD."); return -1; }
    xmlNode *root = xmlDocGetRootElement(doc);
    if (!root || !node_is(root, "MPD")) { xmlFreeDoc(doc); snprintf(errbuf, errbuf_len, "Not an MPD."); return -1; }

    char *type = attr(root, "type");
    out->dynamic = type && strcmp(type, "dynamic") == 0;
    free(type);
    char *mup = attr(root, "minimumUpdatePeriod");
    out->minimum_update_period = parse_duration(mup); free(mup);

    // Anchors for $Number$-addressed streams, which carry no explicit timeline:
    // the wall-clock origin of the presentation, and (for a static MPD) how long
    // it runs.
    char *ast_attr = attr(root, "availabilityStartTime");
    double availability_start = parse_datetime(ast_attr);
    free(ast_attr);
    char *mpd_dur_attr = attr(root, "mediaPresentationDuration");
    double mpd_duration = parse_duration(mpd_dur_attr);
    free(mpd_dur_attr);
    double now_utc = (double)time(NULL);

    char *tsb = attr(root, "timeShiftBufferDepth");
    out->time_shift_buffer_depth = parse_duration(tsb); free(tsb);

    char *mpd_base = base_url_child(root);
    char *base_after_mpd = stack_base(mpd_url, mpd_base); free(mpd_base);

    bool found = false;
    for (xmlNode *period = root->children; period && !found; period = period->next) {
        if (!node_is(period, "Period")) continue;
        char *period_base_piece = base_url_child(period);
        char *base_after_period = stack_base(base_after_mpd, period_base_piece);
        free(period_base_piece);

        char *pstart_attr = attr(period, "start");
        double period_start = parse_duration(pstart_attr);
        free(pstart_attr);
        char *pdur_attr = attr(period, "duration");
        double period_duration = parse_duration(pdur_attr);
        free(pdur_attr);

        for (xmlNode *adap = period->children; adap && !found; adap = adap->next) {
            if (!node_is(adap, "AdaptationSet")) continue;
            char *mime = attr(adap, "mimeType");
            char *content_type = attr(adap, "contentType");
            const char *atype = classify(mime, content_type);
            char *adap_base_piece = base_url_child(adap);
            char *base_after_adap = stack_base(base_after_period, adap_base_piece);
            free(adap_base_piece);

            seg_template adap_tmpl; memset(&adap_tmpl, 0, sizeof(adap_tmpl));
            for (xmlNode *c = adap->children; c; c = c->next)
                if (node_is(c, "SegmentTemplate")) { template_from_node(c, &adap_tmpl); break; }

            for (xmlNode *rep = adap->children; rep && !found; rep = rep->next) {
                if (!node_is(rep, "Representation")) continue;
                char *rid = attr(rep, "id");
                if (!rid) continue;
                bool match = representation_id && representation_id[0]
                    ? strcmp(rid, representation_id) == 0
                    : strcmp(atype, "video") == 0;  // default: first video representation
                if (!match) { free(rid); continue; }

                char *rep_base_piece = base_url_child(rep);
                char *base = stack_base(base_after_adap, rep_base_piece);
                free(rep_base_piece);
                char *bandwidth = attr(rep, "bandwidth");

                seg_template rep_tmpl; memset(&rep_tmpl, 0, sizeof(rep_tmpl));
                for (xmlNode *c = rep->children; c; c = c->next)
                    if (node_is(c, "SegmentTemplate")) { template_from_node(c, &rep_tmpl); break; }

                seg_template t; merge_template(&adap_tmpl, &rep_tmpl, &t);
                template_dispose(&rep_tmpl);

                if (t.have && t.media && t.media[0]) {
                    out->representation_id = rs_strdup(rid);
                    out->adaptation_type = rs_strdup(atype);
                    out->timescale = t.timescale ? t.timescale : 1;

                    if (t.initialization && t.initialization[0]) {
                        char *path = fill_template(t.initialization, rid, bandwidth, -1, -1);
                        out->init_url = rs_url_resolve(base, path);
                        free(path);
                    }

                    // Expand the timeline (or a @duration window) to media segments.
                    size_t cap = 0;
                    if (t.timeline_count) {
                        long long number = t.start_number ? t.start_number : 1;
                        long long cur = t.pto;
                        for (size_t i = 0; i < t.timeline_count; i++) {
                            tl_entry *e = &t.timeline[i];
                            if (e->t >= 0) cur = e->t;
                            long long reps = e->r < 0 ? 0 : e->r;
                            for (long long k = 0; k <= reps; k++) {
                                if (out->count >= cap) { cap = cap ? cap * 2 : 64; out->segments = realloc(out->segments, cap * sizeof(rs_dash_segment)); }
                                char *path = fill_template(t.media, rid, bandwidth, number, cur);
                                rs_dash_segment *seg = &out->segments[out->count++];
                                seg->url = rs_url_resolve(base, path);
                                seg->number = number;
                                seg->time = cur;
                                seg->duration = t.timescale ? (double)e->d / (double)t.timescale : 0;
                                free(path);
                                number++; cur += e->d;
                            }
                        }
                    } else if (t.duration > 0) {
                        // SegmentTemplate@duration with $Number$ and no
                        // SegmentTimeline. This is the other half of live DASH —
                        // DASH-IF's own reference streams and plenty of real
                        // CDNs publish it — and it used to fall straight through
                        // to `found = true` with an empty segment list, so the
                        // engine polled forever reporting "0 segments in the
                        // manifest window" and no player ever got a playlist.
                        //
                        // There is no timeline to walk: segment N covers media
                        // time [N*duration, (N+1)*duration) from the period's
                        // start, so the live edge has to be derived from the
                        // wall clock against MPD@availabilityStartTime.
                        double ts = t.timescale ? (double)t.timescale : 1.0;
                        double seg_dur = t.duration / ts;
                        long long first_number = t.start_number ? t.start_number : 1;
                        long long newest = -1;   // 0-based index of the newest complete segment

                        if (out->dynamic) {
                            if (availability_start >= 0) {
                                double elapsed = now_utc - availability_start - period_start;
                                if (elapsed > 0) newest = (long long)(elapsed / seg_dur) - 1;
                            }
                        } else {
                            double total = period_duration > 0 ? period_duration : mpd_duration;
                            if (total > 0) newest = (long long)(total / seg_dur) - 1;
                        }

                        if (newest >= 0 && seg_dur > 0) {
                            // How far back to go: the caller trims to `want`, so
                            // honour that when given and otherwise fall back to
                            // the advertised time-shift buffer.
                            long long n = want > 0 ? want
                                        : (out->time_shift_buffer_depth > 0
                                               ? (long long)(out->time_shift_buffer_depth / seg_dur)
                                               : 10);
                            if (n < 1) n = 1;
                            if (n > 5000) n = 5000;
                            long long start_idx = newest - n + 1;
                            if (start_idx < 0) start_idx = 0;

                            for (long long idx = start_idx; idx <= newest; idx++) {
                                if (out->count >= cap) { cap = cap ? cap * 2 : 64; out->segments = realloc(out->segments, cap * sizeof(rs_dash_segment)); }
                                long long number = first_number + idx;
                                long long media_time = t.pto + idx * (long long)t.duration;
                                char *path = fill_template(t.media, rid, bandwidth, number, media_time);
                                rs_dash_segment *seg = &out->segments[out->count++];
                                seg->url = rs_url_resolve(base, path);
                                seg->number = number;
                                seg->time = media_time;
                                seg->duration = seg_dur;
                                free(path);
                            }
                        }
                    }
                    found = true;
                    free(bandwidth);
                    free(base);
                    template_dispose(&t);
                    free(rid);
                    break;
                }
                template_dispose(&t);
                free(bandwidth);
                free(base);
                free(rid);
            }
            template_dispose(&adap_tmpl);
            free(mime); free(content_type); free(base_after_adap);
        }
        free(base_after_period);
    }
    free(base_after_mpd);
    xmlFreeDoc(doc);

    if (!found) { snprintf(errbuf, errbuf_len, "No matching representation with a SegmentTemplate."); rs_dash_plan_dispose(out); return -1; }

    // Keep only the newest `want` media segments (the live edge), like the
    // dynamic branch of DashSegments.expandSegments.
    if (want > 0 && out->count > (size_t)want) {
        size_t drop = out->count - (size_t)want;
        for (size_t i = 0; i < drop; i++) free(out->segments[i].url);
        memmove(out->segments, out->segments + drop, (size_t)want * sizeof(rs_dash_segment));
        out->count = (size_t)want;
    }
    return 0;
}

void rs_dash_plan_dispose(rs_dash_plan *plan) {
    if (!plan) return;
    free(plan->representation_id);
    free(plan->adaptation_type);
    free(plan->init_url);
    for (size_t i = 0; i < plan->count; i++) free(plan->segments[i].url);
    free(plan->segments);
    memset(plan, 0, sizeof(*plan));
}

// --- describe (fetch + enumerate + expand) ---------------------------------

// Closed captions carried inside the video elementary stream, as announced by
// SCTE 214's Accessibility descriptor. Nothing has to be demuxed for these —
// the caption bytes ride in H.264/H.265 SEI messages and survive our
// byte-for-byte segment passthrough — so all that is missing downstream is the
// HLS tag that tells a player to go looking for them.
#define RS_DASH_MAX_CC 8

typedef struct {
    char *instream_id;  // HLS INSTREAM-ID: "CC1".."CC4" or "SERVICE1".."SERVICE63"
    char *lang;         // may be NULL
} cc_track;

typedef struct {
    char *video_id, *video_codecs;
    long long video_bw;
    char *audio_id, *audio_codecs, *audio_lang;
    char *text_id, *text_codecs, *text_lang, *text_mime;
    cc_track cc[RS_DASH_MAX_CC];
    size_t cc_count;
} rendition_set;

static void rendition_set_dispose(rendition_set *r) {
    free(r->video_id); free(r->video_codecs);
    free(r->audio_id); free(r->audio_codecs); free(r->audio_lang);
    free(r->text_id); free(r->text_codecs); free(r->text_lang); free(r->text_mime);
    for (size_t i = 0; i < r->cc_count; i++) { free(r->cc[i].instream_id); free(r->cc[i].lang); }
    memset(r, 0, sizeof(*r));
}

// Copies `len` bytes of `s`, trimmed of surrounding whitespace, or NULL if
// nothing is left.
static char *dup_trimmed(const char *s, size_t len) {
    while (len && (*s == ' ' || *s == '\t')) { s++; len--; }
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t')) len--;
    if (!len) return NULL;
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

// Parses an Accessibility@value listing caption services and appends them.
//
// SCTE 214-1 writes these two ways and both are in the wild:
//   cea-608: "CC1=eng;CC3=swe"        or a bare "eng;swe" (services in order)
//   cea-708: "1=lang:eng;2=lang:spa"  or a bare "eng;spa"
// The right-hand side of a 708 entry can carry more than the language
// ("1=lang:eng,war:1,er:1"), so only the lang token is read out of it.
static void parse_cc_value(const char *value, bool is_708, rendition_set *out) {
    if (!value || !value[0]) {
        // A descriptor with no value still asserts that captions are present.
        // CC1 is where a single 608 service always lives, and service 1 is the
        // 708 equivalent, so advertising that beats advertising nothing.
        if (out->cc_count < RS_DASH_MAX_CC) {
            out->cc[out->cc_count].instream_id = rs_strdup(is_708 ? "SERVICE1" : "CC1");
            out->cc[out->cc_count].lang = NULL;
            out->cc_count++;
        }
        return;
    }

    int implicit = 0;
    const char *p = value;
    while (*p && out->cc_count < RS_DASH_MAX_CC) {
        const char *semi = strchr(p, ';');
        size_t tok_len = semi ? (size_t)(semi - p) : strlen(p);
        const char *tok = p;
        p = semi ? semi + 1 : p + tok_len;

        char *id = NULL, *lang = NULL;
        const char *eq = (const char *)memchr(tok, '=', tok_len);
        if (eq) {
            char *left = dup_trimmed(tok, (size_t)(eq - tok));
            const char *rhs = eq + 1;
            size_t rhs_len = tok_len - (size_t)(eq + 1 - tok);
            // Take the lang token out of a comma-separated parameter list.
            const char *comma = (const char *)memchr(rhs, ',', rhs_len);
            size_t first_len = comma ? (size_t)(comma - rhs) : rhs_len;
            if (first_len > 5 && strncmp(rhs, "lang:", 5) == 0) {
                lang = dup_trimmed(rhs + 5, first_len - 5);
            } else {
                lang = dup_trimmed(rhs, first_len);
            }
            if (left) {
                // 608 names the service ("CC1"); 708 numbers it ("1").
                if (left[0] >= '0' && left[0] <= '9') {
                    char buf[32];
                    snprintf(buf, sizeof(buf), is_708 ? "SERVICE%s" : "CC%s", left);
                    id = rs_strdup(buf);
                } else {
                    id = left;
                    left = NULL;
                }
                free(left);
            }
        } else {
            lang = dup_trimmed(tok, tok_len);
            if (lang) {
                char buf[32];
                snprintf(buf, sizeof(buf), is_708 ? "SERVICE%d" : "CC%d", implicit + 1);
                id = rs_strdup(buf);
            }
        }
        implicit++;

        if (!id) { free(lang); continue; }
        // A service already listed by an earlier descriptor wins; the same
        // stream is often tagged with both the 608 and 708 scheme.
        bool dup = false;
        for (size_t i = 0; i < out->cc_count; i++)
            if (strcmp(out->cc[i].instream_id, id) == 0) { dup = true; break; }
        if (dup) { free(id); free(lang); continue; }
        out->cc[out->cc_count].instream_id = id;
        out->cc[out->cc_count].lang = lang;
        out->cc_count++;
    }
}

// Reads the SCTE caption descriptors off one node (an AdaptationSet or a
// Representation).
static void scan_cc_descriptors(xmlNode *node, rendition_set *out) {
    for (xmlNode *c = node->children; c; c = c->next) {
        if (!node_is(c, "Accessibility")) continue;
        char *scheme = attr(c, "schemeIdUri");
        if (!scheme) continue;
        bool is_608 = strcmp(scheme, "urn:scte:dash:cc:cea-608:2015") == 0;
        bool is_708 = strcmp(scheme, "urn:scte:dash:cc:cea-708:2015") == 0;
        if (is_608 || is_708) {
            char *value = attr(c, "value");
            parse_cc_value(value, is_708, out);
            free(value);
        }
        free(scheme);
    }
}

// Picks the default renditions: the highest-bandwidth video Representation, the
// first audio one, and the first timed-text one, with their codecs — plus any
// in-band closed captions the video AdaptationSet declares.
static void pick_default_reps(xmlNode *root, rendition_set *out) {
    memset(out, 0, sizeof(*out));
    out->video_bw = -1;
    for (xmlNode *period = root->children; period; period = period->next) {
        if (!node_is(period, "Period")) continue;
        for (xmlNode *adap = period->children; adap; adap = adap->next) {
            if (!node_is(adap, "AdaptationSet")) continue;
            char *mime = attr(adap, "mimeType");
            char *ctype = attr(adap, "contentType");
            char *adap_codecs = attr(adap, "codecs");
            char *lang = attr(adap, "lang");
            const char *type = classify(mime, ctype);
            if (strcmp(type, "video") == 0) scan_cc_descriptors(adap, out);
            for (xmlNode *rep = adap->children; rep; rep = rep->next) {
                if (!node_is(rep, "Representation")) continue;
                char *rid = attr(rep, "id");
                if (!rid) continue;
                char *codecs = attr(rep, "codecs");
                if (!codecs && adap_codecs) codecs = rs_strdup(adap_codecs);
                char *bw = attr(rep, "bandwidth");
                long long bwv = bw ? strtoll(bw, NULL, 10) : 0;
                if (strcmp(type, "video") == 0) {
                    scan_cc_descriptors(rep, out);
                    if (bwv > out->video_bw) {
                        free(out->video_id); free(out->video_codecs);
                        out->video_id = rs_strdup(rid);
                        out->video_codecs = codecs ? rs_strdup(codecs) : NULL;
                        out->video_bw = bwv;
                    }
                } else if (strcmp(type, "audio") == 0 && !out->audio_id) {
                    out->audio_id = rs_strdup(rid);
                    out->audio_codecs = codecs ? rs_strdup(codecs) : NULL;
                    out->audio_lang = lang ? rs_strdup(lang) : NULL;
                } else if (strcmp(type, "text") == 0 && !out->text_id) {
                    char *rep_mime = attr(rep, "mimeType");
                    out->text_id = rs_strdup(rid);
                    out->text_codecs = codecs ? rs_strdup(codecs) : NULL;
                    out->text_lang = lang ? rs_strdup(lang) : NULL;
                    out->text_mime = rep_mime ? rep_mime : (mime ? rs_strdup(mime) : NULL);
                }
                free(codecs); free(bw); free(rid);
            }
            free(mime); free(ctype); free(adap_codecs); free(lang);
        }
    }
}

// --- segment URL query-string inheritance -----------------------------------
//
// Appends a raw
// query-string fragment (with or without a leading '?'/'&') to a URL, ahead of
// any fragment. Caller frees the result; returns a copy of `url` unchanged
// when `params` is empty. Some signed CDNs put an auth token in the query of
// every segment URL but only stamp it onto the manifest's *redirect target*,
// never a segment path pulled straight out of the MPD — this is what stands
// in for that per-segment token.
static char *append_query(const char *url, const char *params) {
    if (!params) params = "";
    while (*params == ' ' || *params == '?' || *params == '&') params++;
    size_t plen = strlen(params);
    while (plen && params[plen - 1] == ' ') plen--;
    if (!plen) return rs_strdup(url);

    const char *hash = strchr(url, '#');
    size_t head_len = hash ? (size_t)(hash - url) : strlen(url);
    bool has_query = memchr(url, '?', head_len) != NULL;
    size_t tail_len = hash ? strlen(hash) : 0;
    char *out = (char *)malloc(head_len + 1 + plen + tail_len + 1);
    if (!out) return rs_strdup(url);
    memcpy(out, url, head_len);
    size_t pos = head_len;
    out[pos++] = has_query ? '&' : '?';
    memcpy(out + pos, params, plen);
    pos += plen;
    if (hash) memcpy(out + pos, hash, tail_len);
    out[pos + tail_len] = '\0';
    return out;
}

// The query component of `url` (between '?' and '#'/end), or "" if it has
// none. Caller frees the result.
static char *url_query(const char *url) {
    const char *q = strchr(url, '?');
    if (!q) return rs_strdup("");
    q++;
    const char *hash = strchr(q, '#');
    size_t len = hash ? (size_t)(hash - q) : strlen(q);
    char *out = (char *)malloc(len + 1);
    if (!out) return rs_strdup("");
    memcpy(out, q, len);
    out[len] = '\0';
    return out;
}

char *rs_dash_describe(const char *url, const char *proxy, const char *headers,
                       const char *downloader, const char *dl_params,
                       const char *rep, int want,
                       const char *segment_url_params, int inherit_url_params,
                       char *errbuf, size_t errbuf_len) {
    // Fetch the MPD via libcurl (downloader forced internal so we get the final
    // URL after any redirect — segment URLs must resolve against it).
    (void)downloader; (void)dl_params;
    
    #define DASH_CACHE_SIZE 8
    typedef struct {
        char *url;
        char *proxy;
        char *headers;
        char *xml;
        size_t len;
        char *effurl;
        time_t time;
        bool fetching;
        bool failed;
        char error[256];
        pthread_cond_t cv;
    } dash_cache_entry;

    static dash_cache_entry g_dash_cache[DASH_CACHE_SIZE] = {0};
    static pthread_mutex_t g_dash_mu = PTHREAD_MUTEX_INITIALIZER;
    static bool g_dash_cache_init = false;

    char *xml = NULL; size_t len = 0; char *effurl = NULL;
    
    pthread_mutex_lock(&g_dash_mu);
    if (!g_dash_cache_init) {
        for (int i = 0; i < DASH_CACHE_SIZE; i++) pthread_cond_init(&g_dash_cache[i].cv, NULL);
        g_dash_cache_init = true;
    }

    dash_cache_entry *entry = NULL;
    dash_cache_entry *empty = NULL;
    dash_cache_entry *oldest = NULL;

    for (int i = 0; i < DASH_CACHE_SIZE; i++) {
        if (g_dash_cache[i].url && strcmp(g_dash_cache[i].url, url) == 0) {
            entry = &g_dash_cache[i];
            break;
        }
        if (!g_dash_cache[i].url && !empty) empty = &g_dash_cache[i];
        if (g_dash_cache[i].url && (!oldest || g_dash_cache[i].time < oldest->time)) oldest = &g_dash_cache[i];
    }

    if (!entry) {
        entry = empty ? empty : oldest;
        while (entry->fetching) pthread_cond_wait(&entry->cv, &g_dash_mu);
        free(entry->url); free(entry->proxy); free(entry->headers);
        free(entry->xml); free(entry->effurl);
        entry->url = rs_strdup(url);
        entry->proxy = NULL; entry->headers = NULL; entry->xml = NULL; entry->effurl = NULL;
        entry->len = 0; entry->time = 0; entry->failed = false; entry->error[0] = '\0';
    }

    while (entry->fetching) pthread_cond_wait(&entry->cv, &g_dash_mu);

    bool match = ((!proxy && !entry->proxy) || (proxy && entry->proxy && strcmp(proxy, entry->proxy) == 0)) &&
                 ((!headers && !entry->headers) || (headers && entry->headers && strcmp(headers, entry->headers) == 0));
    
    if (match && (time(NULL) - entry->time <= 2) && (entry->xml || entry->failed)) {
        if (entry->failed) {
            snprintf(errbuf, errbuf_len, "%s",
                     entry->error[0] ? entry->error : "The shared MPD fetch failed.");
            pthread_mutex_unlock(&g_dash_mu);
            return NULL;
        }
        xml = rs_strdup(entry->xml);
        len = entry->len;
        effurl = entry->effurl ? rs_strdup(entry->effurl) : NULL;
        pthread_mutex_unlock(&g_dash_mu);
    } else {
        entry->fetching = true;
        pthread_mutex_unlock(&g_dash_mu);

        int rc = rs_fetch_url(url, proxy, headers, NULL, NULL, NULL, &xml, &len,
                              NULL, NULL, NULL, &effurl, errbuf, errbuf_len,
                              30000, NULL, NULL);

        pthread_mutex_lock(&g_dash_mu);
        entry->fetching = false;
        if (rc == 0) {
            free(entry->proxy); free(entry->headers);
            free(entry->xml); free(entry->effurl);
            entry->proxy = proxy ? rs_strdup(proxy) : NULL;
            entry->headers = headers ? rs_strdup(headers) : NULL;
            entry->xml = xml ? rs_strdup(xml) : NULL;
            entry->len = len;
            entry->effurl = effurl ? rs_strdup(effurl) : NULL;
            entry->time = time(NULL);
            entry->failed = false;
            entry->error[0] = '\0';
        } else {
            // Keep a short negative cache just like the successful MPD cache.
            // Video, audio and the director all ask for the same manifest; if
            // one 30-second fetch fails, the waiters must receive that result
            // instead of serially starting two more identical 30-second calls.
            free(entry->proxy); free(entry->headers);
            free(entry->xml); free(entry->effurl);
            entry->proxy = proxy ? rs_strdup(proxy) : NULL;
            entry->headers = headers ? rs_strdup(headers) : NULL;
            entry->xml = NULL; entry->effurl = NULL; entry->len = 0;
            entry->time = time(NULL);
            entry->failed = true;
            snprintf(entry->error, sizeof(entry->error), "%s",
                     errbuf[0] ? errbuf : "The MPD fetch failed.");
        }
        pthread_cond_broadcast(&entry->cv);
        pthread_mutex_unlock(&g_dash_mu);

        if (rc != 0) return NULL;
    }

    const char *base = (effurl && effurl[0]) ? effurl : url;

    // inherit_url_params wins when set: some CDNs sign a token only onto the
    // redirect target (base), never onto the configured `url` a fixed
    // segment_url_params would have to have been typed from ahead of time.
    char *inherited_params = inherit_url_params ? url_query(base) : NULL;
    const char *append_params = inherit_url_params
        ? (inherited_params && inherited_params[0] ? inherited_params : NULL)
        : ((segment_url_params && segment_url_params[0]) ? segment_url_params : NULL);

    xmlDoc *doc = xmlReadMemory(xml, (int)len, "mpd.xml", NULL,
                                XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_RECOVER);
    if (!doc) { free(xml); free(effurl); free(inherited_params); snprintf(errbuf, errbuf_len, "Could not parse the MPD."); return NULL; }
    xmlNode *root = xmlDocGetRootElement(doc);
    if (!root || !node_is(root, "MPD")) { xmlFreeDoc(doc); free(xml); free(effurl); free(inherited_params); snprintf(errbuf, errbuf_len, "Not an MPD."); return NULL; }

    rs_json *obj = rs_json_new_obj();
    char *mtype = attr(root, "type");
    rs_json_obj_set_bool(obj, "dynamic", mtype && strcmp(mtype, "dynamic") == 0);
    free(mtype);
    char *mup = attr(root, "minimumUpdatePeriod"); rs_json_obj_set(obj, "mup", rs_json_new_num(parse_duration(mup))); free(mup);
    char *tsb = attr(root, "timeShiftBufferDepth"); rs_json_obj_set(obj, "tsb", rs_json_new_num(parse_duration(tsb))); free(tsb);

    rendition_set reps;
    pick_default_reps(root, &reps);
    if (reps.video_id) {
        rs_json *v = rs_json_new_obj();
        rs_json_obj_set_str(v, "id", reps.video_id);
        if (reps.video_codecs) rs_json_obj_set_str(v, "codecs", reps.video_codecs);
        else rs_json_obj_set(v, "codecs", rs_json_new_null());
        rs_json_obj_set_int(v, "bandwidth", reps.video_bw > 0 ? reps.video_bw : 0);
        rs_json_obj_set(obj, "video", v);
    } else rs_json_obj_set(obj, "video", rs_json_new_null());
    if (reps.audio_id) {
        rs_json *a = rs_json_new_obj();
        rs_json_obj_set_str(a, "id", reps.audio_id);
        if (reps.audio_codecs) rs_json_obj_set_str(a, "codecs", reps.audio_codecs);
        else rs_json_obj_set(a, "codecs", rs_json_new_null());
        if (reps.audio_lang) rs_json_obj_set_str(a, "lang", reps.audio_lang);
        else rs_json_obj_set(a, "lang", rs_json_new_null());
        rs_json_obj_set(obj, "audio", a);
    } else rs_json_obj_set(obj, "audio", rs_json_new_null());
    if (reps.text_id) {
        rs_json *t = rs_json_new_obj();
        rs_json_obj_set_str(t, "id", reps.text_id);
        if (reps.text_codecs) rs_json_obj_set_str(t, "codecs", reps.text_codecs);
        else rs_json_obj_set(t, "codecs", rs_json_new_null());
        if (reps.text_lang) rs_json_obj_set_str(t, "lang", reps.text_lang);
        else rs_json_obj_set(t, "lang", rs_json_new_null());
        if (reps.text_mime) rs_json_obj_set_str(t, "mime", reps.text_mime);
        else rs_json_obj_set(t, "mime", rs_json_new_null());
        rs_json_obj_set(obj, "text", t);
    } else rs_json_obj_set(obj, "text", rs_json_new_null());
    rs_json *ccs = rs_json_new_arr();
    for (size_t i = 0; i < reps.cc_count; i++) {
        rs_json *cc = rs_json_new_obj();
        rs_json_obj_set_str(cc, "instreamId", reps.cc[i].instream_id);
        if (reps.cc[i].lang) rs_json_obj_set_str(cc, "lang", reps.cc[i].lang);
        else rs_json_obj_set(cc, "lang", rs_json_new_null());
        rs_json_arr_push(ccs, cc);
    }
    rs_json_obj_set(obj, "cc", ccs);
    xmlFreeDoc(doc);

    // Expand the requested representations' segment windows. `rep` is a
    // comma-separated list so one MPD read serves every rendition of a stream:
    // the live engine polls once and hands each rendition its own plan, instead
    // of one thread — and one held-open connection — per rendition all fetching
    // the same document. A single id is just a list of one, and still lands in
    // "plan" for the request-time DASH path that asks for exactly one.
    if (rep && rep[0]) {
        rs_json *plans = rs_json_new_arr();
        const char *cursor = rep;
        while (*cursor) {
            const char *comma = strchr(cursor, ',');
            size_t idlen = comma ? (size_t)(comma - cursor) : strlen(cursor);
            char one[512];
            if (idlen == 0 || idlen >= sizeof(one)) {
                if (!comma) break;
                cursor = comma + 1;
                continue;
            }
            memcpy(one, cursor, idlen);
            one[idlen] = '\0';
            cursor = comma ? comma + 1 : cursor + idlen;

            rs_dash_plan plan; char perr[256] = {0};
            if (rs_dash_plan_build(xml, len, base, one, want, &plan, perr, sizeof(perr)) != 0) continue;
            rs_json *p = rs_json_new_obj();
            rs_json_obj_set_str(p, "repId", plan.representation_id ? plan.representation_id : one);
            rs_json_obj_set_str(p, "type", plan.adaptation_type ? plan.adaptation_type : "video");
            rs_json_obj_set_int(p, "timescale", (long long)plan.timescale);
            if (plan.init_url) {
                char *init_url = append_params ? append_query(plan.init_url, append_params) : rs_strdup(plan.init_url);
                rs_json_obj_set_str(p, "initUrl", init_url);
                free(init_url);
            } else rs_json_obj_set(p, "initUrl", rs_json_new_null());
            rs_json *segs = rs_json_new_arr();
            for (size_t i = 0; i < plan.count; i++) {
                rs_json *s = rs_json_new_obj();
                char *seg_url = append_params ? append_query(plan.segments[i].url, append_params) : rs_strdup(plan.segments[i].url);
                rs_json_obj_set_str(s, "url", seg_url);
                free(seg_url);
                rs_json_obj_set_int(s, "time", plan.segments[i].time);
                rs_json_obj_set(s, "duration", rs_json_new_num(plan.segments[i].duration));
                rs_json_arr_push(segs, s);
            }
            rs_json_obj_set(p, "segments", segs);
            // "plan" is the first match, kept so single-representation callers
            // need no change; "plans" carries every match in the order asked.
            if (rs_json_arr_len(plans) == 0) rs_json_obj_set(obj, "plan", rs_json_clone(p));
            rs_json_arr_push(plans, p);
            rs_dash_plan_dispose(&plan);
        }
        rs_json_obj_set(obj, "plans", plans);
    }

    rendition_set_dispose(&reps);
    free(xml); free(effurl); free(inherited_params);
    char *json = rs_json_serialize(obj, false);
    rs_json_free(obj);
    if (!json) snprintf(errbuf, errbuf_len, "Out of memory building the DASH description.");
    return json;
}
