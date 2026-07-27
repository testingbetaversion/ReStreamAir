#include "rs_dash.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include "rs_common.h"
#include "rs_url.h"

// A live-DASH SegmentTemplate/SegmentTimeline expander — the C port of the
// pieces of DashSegments.swift / LiveMPDToM3U8.swift needed to turn one MPD poll
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
