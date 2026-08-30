#include "probe.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include "net.h"
#include "rs_common.h"
#include "rs_cdm.h"
#include "rs_json.h"
#include "rs_m3u8.h"

// --- MPD parsing (libxml2) -------------------------------------------------

// libxml2's node->name is the local name, so element matching ignores the
// document's default namespace without namespace-aware queries.
static bool node_is(xmlNode *node, const char *name) {
    return node->type == XML_ELEMENT_NODE && node->name && strcmp((const char *)node->name, name) == 0;
}

// Reads an attribute by local name (xmlGetProp ignores the namespace prefix, so
// cenc:default_KID is found as "default_KID"). Returns a fresh string or NULL.
static char *attr(xmlNode *node, const char *name) {
    xmlChar *value = xmlGetProp(node, (const xmlChar *)name);
    if (!value) return NULL;
    char *copy = rs_strdup((const char *)value);
    xmlFree(value);
    return copy;
}

// Adds a string attribute to obj, or null when absent.
static void set_attr_or_null(rs_json *obj, const char *key, char *value) {
    if (value) rs_json_obj_set_str(obj, key, value);
    else rs_json_obj_set(obj, key, rs_json_new_null());
    free(value);
}

static void set_int_or_null(rs_json *obj, const char *key, char *value) {
    if (value && value[0]) rs_json_obj_set_int(obj, key, strtoll(value, NULL, 10));
    else rs_json_obj_set(obj, key, rs_json_new_null());
    free(value);
}

// video/mp4 -> "video", audio/mp4 -> "audio", else the contentType, else "video".
static const char *classify(const char *mime, const char *content_type) {
    if (mime) {
        if (strncmp(mime, "video", 5) == 0) return "video";
        if (strncmp(mime, "audio", 5) == 0) return "audio";
        if (strncmp(mime, "text", 4) == 0 || strncmp(mime, "application", 11) == 0) return "text";
    }
    if (content_type && content_type[0]) return content_type;
    return "video";
}

// Dashless, lowercased KID — matching how the panel stores/pre-fills them.
static char *normalize_kid(const char *raw) {
    if (!raw) return NULL;
    size_t n = strlen(raw);
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char ch = raw[i];
        if (ch == '-') continue;
        out[o++] = (char)tolower((unsigned char)ch);
    }
    out[o] = '\0';
    return out;
}

// The default_KID of the last ContentProtection child of `node`, or NULL. The
// attribute is namespaced (cenc:default_KID) but xmlGetProp matches on the
// local name, so the prefix a manifest happens to use doesn't matter.
static char *content_protection_kid(xmlNode *node) {
    char *kid = NULL;
    for (xmlNode *cp = node->children; cp; cp = cp->next) {
        if (!node_is(cp, "ContentProtection")) continue;
        char *k = attr(cp, "default_KID");
        if (k) { free(kid); kid = k; }
    }
    return kid;
}

// Everything the manifest advertises about its DRM, in the shape the CDM
// pipeline already uses: every KID, every PSSH box (with Widevine/PlayReady
// split out), and any HLS key URIs. Attached to the probe result so the stream
// editor can show what a source is protected with, not just that it is.
static rs_json *build_drm(const rs_drm_challenge *ch) {
    rs_json *drm = rs_json_new_obj();
    rs_json *kids = rs_json_new_arr();
    for (size_t i = 0; i < ch->kids_count; i++) rs_json_arr_push(kids, rs_json_new_str(ch->kids[i]));
    rs_json_obj_set(drm, "kids", kids);
    rs_json *boxes = rs_json_new_arr();
    for (size_t i = 0; i < ch->pssh_all_count; i++)
        rs_json_arr_push(boxes, rs_json_new_str(ch->pssh_all[i]));
    rs_json_obj_set(drm, "pssh", boxes);
    if (ch->pssh_widevine) rs_json_obj_set_str(drm, "psshWidevine", ch->pssh_widevine);
    else rs_json_obj_set(drm, "psshWidevine", rs_json_new_null());
    if (ch->pssh_playready) rs_json_obj_set_str(drm, "psshPlayReady", ch->pssh_playready);
    else rs_json_obj_set(drm, "psshPlayReady", rs_json_new_null());
    rs_json *uris = rs_json_new_arr();
    for (size_t i = 0; i < ch->key_uris_count; i++)
        rs_json_arr_push(uris, rs_json_new_str(ch->key_uris[i]));
    rs_json_obj_set(drm, "keyUris", uris);
    return drm;
}

static char *build_mpd(const char *text, size_t len) {
    xmlDoc *doc = xmlReadMemory(text, (int)len, "mpd.xml", NULL,
                               XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_RECOVER);
    if (!doc) return NULL;
    xmlNode *root = xmlDocGetRootElement(doc);
    if (!root || !node_is(root, "MPD")) { xmlFreeDoc(doc); return NULL; }

    rs_json *reps = rs_json_new_arr();
    rs_json *protection = rs_json_new_obj();

    for (xmlNode *period = root->children; period; period = period->next) {
        if (!node_is(period, "Period")) continue;
        for (xmlNode *adap = period->children; adap; adap = adap->next) {
            if (!node_is(adap, "AdaptationSet")) continue;
            char *mime = attr(adap, "mimeType");
            char *content_type = attr(adap, "contentType");
            char *lang = attr(adap, "lang");
            char *adap_codecs = attr(adap, "codecs");
            const char *type = classify(mime, content_type);

            // A default_KID on any ContentProtection marks the set as
            // encrypted. Sources put it in three different places — on the
            // Period, on the AdaptationSet, or on each Representation — and
            // reading only the AdaptationSet meant a manifest that scopes its
            // KIDs per representation came back with no protection at all.
            char *set_kid = content_protection_kid(period);
            char *adap_kid = content_protection_kid(adap);
            if (adap_kid) { free(set_kid); set_kid = adap_kid; }

            for (xmlNode *rep = adap->children; rep; rep = rep->next) {
                if (!node_is(rep, "Representation")) continue;
                char *id = attr(rep, "id");
                if (!id) continue;  // a representation with no id can't be selected
                char *codecs = attr(rep, "codecs");
                if (!codecs && adap_codecs) codecs = rs_strdup(adap_codecs);

                rs_json *r = rs_json_new_obj();
                rs_json_obj_set_str(r, "id", id);
                rs_json_obj_set_str(r, "type", type);
                if (lang) rs_json_obj_set_str(r, "language", lang);
                else rs_json_obj_set(r, "language", rs_json_new_null());
                set_int_or_null(r, "bandwidth", attr(rep, "bandwidth"));
                set_int_or_null(r, "width", attr(rep, "width"));
                set_int_or_null(r, "height", attr(rep, "height"));
                set_attr_or_null(r, "codecs", codecs);
                set_attr_or_null(r, "frameRate", attr(rep, "frameRate"));
                rs_json_arr_push(reps, r);

                char *rep_kid = content_protection_kid(rep);
                const char *kid = rep_kid ? rep_kid : set_kid;
                if (kid) {
                    char *norm = normalize_kid(kid);
                    rs_json *kids = rs_json_new_arr();
                    rs_json_arr_push(kids, rs_json_new_str(norm ? norm : kid));
                    rs_json_obj_set(protection, id, kids);
                    free(norm);
                }
                free(rep_kid);
                free(id);
            }
            free(mime);
            free(content_type);
            free(lang);
            free(adap_codecs);
            free(set_kid);
        }
    }
    xmlFreeDoc(doc);

    // The DOM walk above only sees default_KID attributes. The CDM scraper also
    // reads every cenc:pssh box — including the KIDs a version 1 box lists,
    // which is the only place some manifests name them — so run it too and let
    // its findings stand in for any representation the walk left unprotected.
    rs_drm_challenge ch = rs_cdm_challenge_from_mpd(text);
    if (ch.kids_count == 1 && rs_json_obj_len(protection) == 0) {
        for (size_t i = 0; i < rs_json_arr_len(reps); i++) {
            rs_json *kids = rs_json_new_arr();
            rs_json_arr_push(kids, rs_json_new_str(ch.kids[0]));
            rs_json_obj_set(protection, rs_json_obj_str(rs_json_arr_at(reps, i), "id", ""), kids);
        }
    }

    rs_json *out = rs_json_new_obj();
    rs_json_obj_set_str(out, "kind", "mpd");
    rs_json_obj_set(out, "representations", reps);
    rs_json_obj_set(out, "protection", protection);
    rs_json_obj_set(out, "drm", build_drm(&ch));
    rs_drm_challenge_free(&ch);
    char *json = rs_json_serialize(out, false);
    rs_json_free(out);
    return json;
}

// --- HLS parsing (existing rs_m3u8) ----------------------------------------

static char *build_hls(const char *text, const char *base_url) {
    rs_m3u8_probe probe;
    rs_json *reps = rs_json_new_arr();
    if (rs_m3u8_probe_master(text, base_url, &probe) == 0) {
        for (size_t i = 0; i < probe.variant_count; i++) {
            const rs_m3u8_variant *v = &probe.variants[i];
            rs_json *r = rs_json_new_obj();
            rs_json_obj_set_str(r, "id", v->uri);
            rs_json_obj_set_str(r, "type", "video");
            if (v->bandwidth >= 0) rs_json_obj_set_int(r, "bandwidth", v->bandwidth);
            else rs_json_obj_set(r, "bandwidth", rs_json_new_null());
            if (v->width >= 0) rs_json_obj_set_int(r, "width", v->width);
            else rs_json_obj_set(r, "width", rs_json_new_null());
            if (v->height >= 0) rs_json_obj_set_int(r, "height", v->height);
            else rs_json_obj_set(r, "height", rs_json_new_null());
            if (v->codecs) rs_json_obj_set_str(r, "codecs", v->codecs);
            else rs_json_obj_set(r, "codecs", rs_json_new_null());
            rs_json_obj_set(r, "language", rs_json_new_null());
            rs_json_arr_push(reps, r);
        }
        for (size_t i = 0; i < probe.audio_count; i++) {
            const rs_m3u8_audio_track *t = &probe.audio_tracks[i];
            rs_json *r = rs_json_new_obj();
            rs_json_obj_set_str(r, "id", t->uri ? t->uri : t->group_id);
            rs_json_obj_set_str(r, "type", "audio");
            rs_json_obj_set(r, "bandwidth", rs_json_new_null());
            if (t->language) rs_json_obj_set_str(r, "language", t->language);
            else rs_json_obj_set(r, "language", rs_json_new_null());
            rs_json_arr_push(reps, r);
        }
        rs_m3u8_probe_dispose(&probe);
    }
    // HLS carries its DRM on #EXT-X-KEY / #EXT-X-SESSION-KEY rather than per
    // rendition, so whatever the master advertises applies to every variant.
    // This used to report no protection at all for encrypted HLS.
    rs_drm_challenge ch = rs_cdm_challenge_from_hls(text);
    rs_json *protection = rs_json_new_obj();
    if (ch.kids_count) {
        for (size_t i = 0; i < rs_json_arr_len(reps); i++) {
            rs_json *kids = rs_json_new_arr();
            for (size_t k = 0; k < ch.kids_count; k++)
                rs_json_arr_push(kids, rs_json_new_str(ch.kids[k]));
            rs_json_obj_set(protection, rs_json_obj_str(rs_json_arr_at(reps, i), "id", ""), kids);
        }
    }
    rs_json *out = rs_json_new_obj();
    rs_json_obj_set_str(out, "kind", "m3u8");
    rs_json_obj_set(out, "representations", reps);
    rs_json_obj_set(out, "protection", protection);
    rs_json_obj_set(out, "drm", build_drm(&ch));
    rs_drm_challenge_free(&ch);
    char *json = rs_json_serialize(out, false);
    rs_json_free(out);
    return json;
}

// --- dispatch --------------------------------------------------------------

static bool ends_with(const char *s, const char *suffix) {
    size_t sl = strlen(s), fl = strlen(suffix);
    return sl >= fl && strcasecmp(s + sl - fl, suffix) == 0;
}

// The URL path without query, for extension sniffing.
static bool path_ends_with(const char *url, const char *suffix) {
    const char *q = strchr(url, '?');
    size_t path_len = q ? (size_t)(q - url) : strlen(url);
    char *path = (char *)malloc(path_len + 1);
    if (!path) return false;
    memcpy(path, url, path_len);
    path[path_len] = '\0';
    bool result = ends_with(path, suffix);
    free(path);
    return result;
}

char *rs_probe_source(const char *url, const char *proxy, const char *headers,
                      int force_ipv6, int rotate_proxies,
                      char *errbuf, size_t errbuf_len) {
    if (!url || !url[0]) { snprintf(errbuf, errbuf_len, "A source URL is required."); return NULL; }
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        snprintf(errbuf, errbuf_len, "The source URL must be http or https.");
        return NULL;
    }

    char *data = NULL;
    size_t len = 0;
    if (rs_fetch_url(url, proxy, headers, NULL, NULL, NULL, force_ipv6, rotate_proxies, &data, &len,
                     NULL, NULL, NULL, NULL, errbuf, errbuf_len, 30000, NULL, NULL) != 0)
        return NULL;

    char *result = NULL;
    bool is_hls = path_ends_with(url, ".m3u8") ||
                  (!path_ends_with(url, ".mpd") && data && strstr(data, "#EXTM3U") != NULL);
    if (is_hls) {
        result = build_hls(data, url);
    } else {
        result = build_mpd(data, len);
    }
    free(data);

    if (!result) snprintf(errbuf, errbuf_len, "Could not parse the source manifest.");
    return result;
}
