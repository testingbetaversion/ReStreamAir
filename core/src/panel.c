#include "rs_panel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rs_internal.h"

// The 2001 reference epoch that dates in state.json are recorded against.
#define RS_APPLE_EPOCH_OFFSET 978307200.0

static double apple_epoch_now(void) {
    return (double)time(NULL) - RS_APPLE_EPOCH_OFFSET;
}

// Formats an Apple-epoch timestamp as the ISO 8601 string the panel's *_view
// responses use (the stored value stays the Apple-epoch double).
static char *iso8601_from_apple(double apple_seconds) {
    time_t unix_time = (time_t)(apple_seconds + RS_APPLE_EPOCH_OFFSET);
    struct tm tm_utc;
#if defined(_WIN32)
    gmtime_s(&tm_utc, &unix_time);
#else
    gmtime_r(&unix_time, &tm_utc);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return rs_strdup(buf);
}

char *rs_panel_slugify(const char *name) {
    rs_buf out = RS_BUF_INIT;
    bool pending_dash = false;
    for (const unsigned char *p = (const unsigned char *)(name ? name : ""); *p; p++) {
        unsigned char c = *p;
        bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (alnum) {
            if (pending_dash) { rs_buf_append_char(&out, '-'); pending_dash = false; }
            rs_buf_append_char(&out, (char)(c >= 'A' && c <= 'Z' ? c + 32 : c));
        } else if (out.len > 0) {
            pending_dash = true;  // a run of non-alphanumerics becomes one dash
        }
    }
    char *result = rs_buf_take(&out);
    return result ? result : rs_strdup("");
}

// prefix_<millis>_<6 hex>, matching PanelServer.safeId closely enough to be
// unique and human-readable.
static char *make_id(const char *prefix) {
    uint8_t rnd[3];
    rs_random_bytes(rnd, sizeof(rnd));
    long long millis = (long long)time(NULL) * 1000;
    char buf[96];
    snprintf(buf, sizeof(buf), "%s_%lld_%02x%02x%02x", prefix, millis, rnd[0], rnd[1], rnd[2]);
    return rs_strdup(buf);
}

// --- lookups ---------------------------------------------------------------

static rs_json *providers_array(rs_state *st) {
    const rs_json *existing = rs_json_obj_get(st->root, "providers");
    if (existing && rs_json_type_of(existing) == RS_JSON_ARR) return (rs_json *)existing;
    rs_json *arr = rs_json_new_arr();
    rs_json_obj_set(st->root, "providers", arr);
    return arr;
}

static rs_json *keys_array(rs_state *st) {
    const rs_json *existing = rs_json_obj_get(st->root, "apiKeys");
    if (existing && rs_json_type_of(existing) == RS_JSON_ARR) return (rs_json *)existing;
    rs_json *arr = rs_json_new_arr();
    rs_json_obj_set(st->root, "apiKeys", arr);
    return arr;
}

static rs_json *find_provider(rs_state *st, const char *id) {
    rs_json *providers = providers_array(st);
    for (size_t i = 0; i < rs_json_arr_len(providers); i++) {
        rs_json *p = (rs_json *)rs_json_arr_at(providers, i);
        if (strcmp(rs_json_obj_str(p, "id", ""), id) == 0) return p;
    }
    return NULL;
}

// Locates a stream by id across every provider, returning its provider and the
// stream node.
static rs_json *find_stream(rs_state *st, const char *id, rs_json **out_provider) {
    rs_json *providers = providers_array(st);
    for (size_t i = 0; i < rs_json_arr_len(providers); i++) {
        rs_json *p = (rs_json *)rs_json_arr_at(providers, i);
        const rs_json *streams = rs_json_obj_get(p, "streams");
        for (size_t j = 0; j < rs_json_arr_len(streams); j++) {
            rs_json *s = (rs_json *)rs_json_arr_at(streams, j);
            if (strcmp(rs_json_obj_str(s, "id", ""), id) == 0) {
                if (out_provider) *out_provider = p;
                return s;
            }
        }
    }
    return NULL;
}

// --- URL validation --------------------------------------------------------

static bool is_http_url(const char *s) {
    return s && (strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0);
}

static const char *normalize_downloader(const char *d) {
    if (!d || d[0] == '\0') return "";
    if (strcmp(d, "wget") == 0) return "wget";
    if (strcmp(d, "aria2c") == 0) return "aria2c";
    if (strcmp(d, "native") == 0 || strcmp(d, "internal") == 0 || strcmp(d, "libcurl") == 0) return "native";
    return "curl";
}

// --- stream construction ---------------------------------------------------

static long long clamp_ll(long long v, long long lo) { return v < lo ? lo : v; }

// Builds a stream object from a body, applying every default and clamp but no
// validation. Split out of stream_from_body so the channel/event import can
// mint a fully-formed stream from an empty body: an imported entry carries no
// URL of its own (the script's `manifest` action supplies one at play time),
// which stream_from_body would reject.
static rs_json *stream_build(const rs_json *body, const char *id) {
    const char *name = rs_json_obj_str(body, "name", "");
    const char *url = rs_json_obj_str(body, "url", "");
    const char *input_mode = rs_json_obj_str(body, "inputMode", "internal");
    rs_json *s = rs_json_new_obj();
    rs_json_obj_set_str(s, "id", id);
    rs_json_obj_set_str(s, "name", name);
    rs_json_obj_set_str(s, "kind", strcmp(rs_json_obj_str(body, "kind", ""), "m3u8") == 0 ? "m3u8" : "mpd");
    rs_json_obj_set_str(s, "url", url);
    rs_json_obj_set_str(s, "representation", rs_json_obj_str(body, "representation", ""));
    rs_json_obj_set_str(s, "period", rs_json_obj_str(body, "period", ""));
    rs_json_obj_set_str(s, "proxy", rs_json_obj_str(body, "proxy", ""));
    rs_json_obj_set_str(s, "downloader", normalize_downloader(rs_json_obj_str(body, "downloader", "")));
    rs_json_obj_set_str(s, "downloaderParams", rs_json_obj_str(body, "downloaderParams", ""));
    rs_json_obj_set_int(s, "playlistSegments", clamp_ll(rs_json_obj_int(body, "playlistSegments", 6), 3));
    long long hls_segment_seconds = clamp_ll(rs_json_obj_int(body, "hlsSegmentSeconds", 10), 1);
    if (hls_segment_seconds > 30) hls_segment_seconds = 30;
    rs_json_obj_set_int(s, "hlsSegmentSeconds", hls_segment_seconds);
    long long buffer_seconds = clamp_ll(rs_json_obj_int(body, "playbackDelaySeconds", 0), 0);
    if (buffer_seconds > 120) buffer_seconds = 120;
    rs_json_obj_set_int(s, "playbackDelaySeconds", buffer_seconds);
    long long keep_segments = clamp_ll(rs_json_obj_int(body, "keepSegments", 10), 1);
    if (keep_segments > 240) keep_segments = 240;
    rs_json_obj_set_int(s, "keepSegments", keep_segments);
    rs_json_obj_set_int(s, "downloadAhead", clamp_ll(rs_json_obj_int(body, "downloadAhead", 20), 1));
    long long parallel = clamp_ll(rs_json_obj_int(body, "parallelDownloads", 6), 1);
    if (parallel > 8) parallel = 8;
    rs_json_obj_set_int(s, "parallelDownloads", parallel);
    double poll = rs_json_obj_num(body, "pollInterval", 0.0);
    rs_json_obj_set(s, "pollInterval", rs_json_new_num(poll < 0 ? 0 : poll));
    rs_json_obj_set_bool(s, "forceOffline", rs_json_obj_bool(body, "forceOffline", false));
    rs_json_obj_set_bool(s, "reducedManifestPolling", rs_json_obj_bool(body, "reducedManifestPolling", false));
    rs_json_obj_set_bool(s, "prioritizeOldest", rs_json_obj_bool(body, "prioritizeOldest", false));
    rs_json_obj_set_str(s, "status", "stopped");
    rs_json_obj_set(s, "lastError", rs_json_new_null());
    rs_json_obj_set_str(s, "logo", rs_json_obj_str(body, "logo", ""));
    rs_json_obj_set_str(s, "decryptionKeys", rs_json_obj_str(body, "decryptionKeys", ""));
    rs_json_obj_set_str(s, "hlsKey", rs_json_obj_str(body, "hlsKey", ""));
    rs_json_obj_set_str(s, "hlsIV", rs_json_obj_str(body, "hlsIV", ""));

    // representations / representationMeta pass through as given.
    const rs_json *reps = rs_json_obj_get(body, "representations");
    rs_json_obj_set(s, "representations",
                    reps && rs_json_type_of(reps) == RS_JSON_ARR ? rs_json_clone(reps) : rs_json_new_arr());
    const rs_json *meta = rs_json_obj_get(body, "representationMeta");
    rs_json_obj_set(s, "representationMeta",
                    meta && rs_json_type_of(meta) == RS_JSON_OBJ ? rs_json_clone(meta) : rs_json_new_obj());
    const rs_json *order = rs_json_obj_get(body, "representationOrder");
    rs_json_obj_set(s, "representationOrder",
                    order && rs_json_type_of(order) == RS_JSON_ARR ? rs_json_clone(order) : rs_json_new_arr());

    rs_json_obj_set_str(s, "manifestHeaders", rs_json_obj_str(body, "manifestHeaders", ""));
    rs_json_obj_set_str(s, "mediaHeaders", rs_json_obj_str(body, "mediaHeaders", ""));
    rs_json_obj_set_str(s, "hlsKeyHeaders", rs_json_obj_str(body, "hlsKeyHeaders", ""));
    rs_json_obj_set_int(s, "audioDelayMs", rs_json_obj_int(body, "audioDelayMs", 0));
    // The XMLTV channel id, exported as tvg-id in the M3U so a player can bind
    // guide data to this stream. Blank falls back to the stream id.
    rs_json_obj_set_str(s, "tvgId", rs_json_obj_str(body, "tvgId", ""));

    // Enumerated fields fall back to their default when the value isn't valid.
    static const char *valid_inputs[] = {"internal", "ffmpegResident", "ffmpegTsHls",
                                         "ffmpegMultiTsHls", "ffmpegFmp4Hls", "pipe",
                                         "nm3u8dlre"};
    bool input_ok = false;
    for (size_t i = 0; i < sizeof(valid_inputs) / sizeof(valid_inputs[0]); i++) {
        if (strcmp(input_mode, valid_inputs[i]) == 0) { input_ok = true; break; }
    }
    rs_json_obj_set_str(s, "inputMode", input_ok ? input_mode : "internal");

    const char *output_mode = rs_json_obj_str(body, "outputMode", "hls");
    static const char *valid_outputs[] = {"hls", "srtServer", "udpSrt", "custom"};
    bool output_ok = false;
    for (size_t i = 0; i < sizeof(valid_outputs) / sizeof(valid_outputs[0]); i++) {
        if (strcmp(output_mode, valid_outputs[i]) == 0) { output_ok = true; break; }
    }
    rs_json_obj_set_str(s, "outputMode", output_ok ? output_mode : "hls");
    rs_json_obj_set_str(s, "outputTarget", rs_json_obj_str(body, "outputTarget", ""));
    rs_json_obj_set_str(s, "pipeCommand", rs_json_obj_str(body, "pipeCommand", ""));

    // cdnUrls: an array of strings or a newline-separated string, keeping only
    // valid http(s) URLs, trimmed.
    rs_json *cdn = rs_json_new_arr();
    const rs_json *cdn_in = rs_json_obj_get(body, "cdnUrls");
    if (cdn_in && rs_json_type_of(cdn_in) == RS_JSON_ARR) {
        for (size_t i = 0; i < rs_json_arr_len(cdn_in); i++) {
            char *v = rs_trim_dup(rs_json_as_str(rs_json_arr_at(cdn_in, i), ""),
                                  strlen(rs_json_as_str(rs_json_arr_at(cdn_in, i), "")), true);
            if (v && is_http_url(v)) rs_json_arr_push(cdn, rs_json_new_str(v));
            free(v);
        }
    }
    rs_json_obj_set(s, "cdnUrls", cdn);

    rs_json_obj_set_bool(s, "directSource", rs_json_obj_bool(body, "directSource", false));
    rs_json_obj_set_str(s, "nm3u8dlreParams", rs_json_obj_str(body, "nm3u8dlreParams", ""));

    // Scripting & DRM.
    rs_json_obj_set_bool(s, "useCdm", rs_json_obj_bool(body, "useCdm", false));
    rs_json_obj_set_str(s, "scriptParams", rs_json_obj_str(body, "scriptParams", ""));
    rs_json_obj_set_bool(s, "heartbeatEnabled", rs_json_obj_bool(body, "heartbeatEnabled", true));
    rs_json_obj_set_int(s, "heartbeatSeconds", clamp_ll(rs_json_obj_int(body, "heartbeatSeconds", 0), 0));
    rs_json_obj_set_str(s, "scriptOverride", rs_json_obj_str(body, "scriptOverride", ""));
    rs_json_obj_set_bool(s, "sessionManifest", rs_json_obj_bool(body, "sessionManifest", false));
    const rs_json *actions = rs_json_obj_get(body, "scriptActionsOverride");
    if (actions && rs_json_type_of(actions) == RS_JSON_ARR) {
        rs_json_obj_set(s, "scriptActionsOverride", rs_json_clone(actions));
    } else {
        rs_json_obj_set(s, "scriptActionsOverride", rs_json_new_null());
    }
    rs_json_obj_set_str(s, "cdmMode", "external");
    rs_json_obj_set_str(s, "cdmType", "");
    rs_json_obj_set_bool(s, "proxyScript", rs_json_obj_bool(body, "proxyScript", true));
    rs_json_obj_set_bool(s, "proxyManifest", rs_json_obj_bool(body, "proxyManifest", true));
    rs_json_obj_set_bool(s, "proxyMedia", rs_json_obj_bool(body, "proxyMedia", true));

    // Channel/event-import fields default off for a manually created stream;
    // an update carries the existing values over (see rs_panel_update_stream).
    rs_json_obj_set_str(s, "sourceType", "");
    rs_json_obj_set_str(s, "mode", "live");
    rs_json_obj_set_str(s, "scriptVideoSelector", "");
    rs_json_obj_set_str(s, "scriptAudioSelector", "");
    rs_json_obj_set_bool(s, "onDemand", false);
    rs_json_obj_set_bool(s, "speedUp", false);
    rs_json_obj_set_bool(s, "autostart", false);
    rs_json_obj_set(s, "scriptStart", rs_json_new_null());
    rs_json_obj_set(s, "scriptEnd", rs_json_new_null());
    rs_json_obj_set_bool(s, "recordEvent", false);
    return s;
}

// Builds a stream object from a request body, applying the same defaults,
// clamps and validation as streamFromBody. `err` is set on failure.
static rs_json *stream_from_body(const rs_json *body, const char *id, const char **err) {
    const char *name = rs_json_obj_str(body, "name", "");
    const char *url = rs_json_obj_str(body, "url", "");
    const char *input_mode = rs_json_obj_str(body, "inputMode", "internal");
    // A trimmed-empty name is rejected.
    const char *trimmed = NULL;
    if (rs_trim(name, strlen(name), true, &trimmed) == 0) { *err = "Stream name is required."; return NULL; }
    if (strcmp(input_mode, "pipe") != 0 && !is_http_url(url)) {
        *err = "Stream URL must be http or https.";
        return NULL;
    }
    if (strcmp(input_mode, "pipe") == 0 && !rs_json_obj_str(body, "pipeCommand", "")[0]) {
        *err = "Program pipe command is required.";
        return NULL;
    }
    return stream_build(body, id);
}

// --- view builders ---------------------------------------------------------

// Number of streams whose name slugifies the same, across all providers — a
// slug is only used in a play URL when it's unique.
static int slug_occurrences(const rs_state *st, const char *slug) {
    int count = 0;
    const rs_json *providers = rs_json_obj_get(st->root, "providers");
    for (size_t i = 0; i < rs_json_arr_len(providers); i++) {
        const rs_json *streams = rs_json_obj_get(rs_json_arr_at(providers, i), "streams");
        for (size_t j = 0; j < rs_json_arr_len(streams); j++) {
            char *s = rs_panel_slugify(rs_json_obj_str(rs_json_arr_at(streams, j), "name", ""));
            if (s && s[0] && strcmp(s, slug) == 0) count++;
            free(s);
        }
    }
    return count;
}

static rs_json *zero_bandwidth(void) {
    rs_json *bw = rs_json_new_obj();
    rs_json_obj_set_int(bw, "bytesPerSecond", 0);
    rs_json_obj_set_int(bw, "allTimeBytes", 0);
    return bw;
}

static rs_json *stream_view(const rs_state *st, const rs_json *stream, const char *host) {
    rs_json *v = rs_json_clone(stream);  // stored fields carry through
    const char *id = rs_json_obj_str(stream, "id", "");
    const char *kind = rs_json_obj_str(stream, "kind", "mpd");
    const char *name = rs_json_obj_str(stream, "name", "");
    const char *input_mode = rs_json_obj_str(stream, "inputMode", "internal");
    const char *output_mode = rs_json_obj_str(stream, "outputMode", "hls");
    bool process_pipeline = strcmp(input_mode, "internal") != 0 || strcmp(output_mode, "hls") != 0;

    // Slug for the play URL: the name slug when unique, else the id.
    char *slug = rs_panel_slugify(name);
    const char *play_id = (slug && slug[0] && slug_occurrences(st, slug) == 1) ? slug : id;

    // Playback is on-demand (no worker), so "running" is simply whether the
    // stream has been started — reflected from the stored status.
    const char *status = rs_json_obj_str(stream, "status", "stopped");
    rs_json_obj_set_bool(v, "running", strcmp(status, "running") == 0);
    if (!rs_json_obj_get(stream, "status")) rs_json_obj_set_str(v, "status", "stopped");
    rs_json_obj_set_int(v, "activeClients", 0);
    rs_json_obj_set(v, "bandwidth", zero_bandwidth());
    rs_json_obj_set(v, "inputBandwidth", zero_bandwidth());

    char url[1024];
    snprintf(url, sizeof(url), "http://%s/play/%s/index.m3u8", host, play_id);
    rs_json_obj_set_str(v, "playUrl", strcmp(output_mode, "hls") == 0
                                      ? url : rs_json_obj_str(stream, "outputTarget", ""));
    snprintf(url, sizeof(url), "http://%s/source/%s", host, id);
    rs_json_obj_set_str(v, "sourceUrl", url);
    if (strcmp(kind, "m3u8") == 0) {
        snprintf(url, sizeof(url), "http://%s/play/%s/index.m3u8", host, play_id);
    } else {
        snprintf(url, sizeof(url), "http://%s/restream/%s/live.m3u8", host, id);
    }
    rs_json_obj_set_str(v, "directUrl", url);

    rs_json *direct = rs_json_new_obj();
    rs_json *download = rs_json_new_obj();
    if (process_pipeline) {
        if (strcmp(output_mode, "hls") == 0) {
            snprintf(url, sizeof(url), "http://%s/play/%s/index.m3u8", host, play_id);
            rs_json_obj_set_str(direct, "HLS", url);
            rs_json_obj_set_str(v, "directUrl", url);
        }
    } else if (strcmp(kind, "m3u8") == 0) {
        snprintf(url, sizeof(url), "http://%s/direct/%s", host, id);
        rs_json_obj_set_str(direct, "source", url);
    } else {
        // effectiveRepresentationIds: representations if any, else the single
        // representation field (possibly empty).
        const rs_json *reps = rs_json_obj_get(stream, "representations");
        size_t rep_count = rs_json_arr_len(reps);
        bool multi = rep_count > 1;
        if (rep_count == 0) {
            const char *single = rs_json_obj_str(stream, "representation", "");
            const char *disk = single[0] ? single : "default";
            snprintf(url, sizeof(url), "http://%s/direct/%s", host, id);
            rs_json_obj_set_str(direct, disk, url);
            snprintf(url, sizeof(url), "http://%s/download/%s.mp4", host, id);
            rs_json_obj_set_str(download, disk, url);
        } else {
            for (size_t i = 0; i < rep_count; i++) {
                const char *rep = rs_json_as_str(rs_json_arr_at(reps, i), "");
                const char *disk = rep[0] ? rep : "default";
                if (multi) {
                    snprintf(url, sizeof(url), "http://%s/direct/%s/%s", host, id, disk);
                    rs_json_obj_set_str(direct, disk, url);
                    snprintf(url, sizeof(url), "http://%s/download/%s/%s.mp4", host, id, disk);
                    rs_json_obj_set_str(download, disk, url);
                } else {
                    snprintf(url, sizeof(url), "http://%s/direct/%s", host, id);
                    rs_json_obj_set_str(direct, disk, url);
                    snprintf(url, sizeof(url), "http://%s/download/%s.mp4", host, id);
                    rs_json_obj_set_str(download, disk, url);
                }
            }
        }
    }
    // The muxed link. Unlike the per-rendition fMP4 tails above it carries
    // video and audio together, so it is the one to hand a player that wants a
    // single URL — hence one entry for the stream rather than one per
    // rendition. DASH only: an m3u8 source has no separate renditions here to
    // mux, and its /source link already is one stream.
    if (!process_pipeline && strcmp(kind, "m3u8") != 0) {
        snprintf(url, sizeof(url), "http://%s/direct/%s.ts", host, id);
        rs_json_obj_set_str(direct, "muxed (mpeg-ts)", url);
    }
    rs_json_obj_set(v, "directStreamUrls", direct);
    rs_json_obj_set(v, "downloadUrls", download);
    free(slug);
    return v;
}

static rs_json *provider_view(const rs_state *st, const rs_json *provider, const char *host) {
    rs_json *v = rs_json_clone(provider);
    // Expand each stream through stream_view.
    const rs_json *streams = rs_json_obj_get(provider, "streams");
    rs_json *out_streams = rs_json_new_arr();
    for (size_t i = 0; i < rs_json_arr_len(streams); i++) {
        rs_json_arr_push(out_streams, stream_view(st, rs_json_arr_at(streams, i), host));
    }
    rs_json_obj_set(v, "streams", out_streams);
    // The session dir the script UI shows (computed, not stored).
    char dir[1024];
    snprintf(dir, sizeof(dir), "runtime/sessions/%s", rs_json_obj_str(provider, "id", ""));
    rs_json_obj_set_str(v, "scriptSessionDir", dir);
    return v;
}

static rs_json *key_view(const rs_json *key) {
    rs_json *v = rs_json_new_obj();
    rs_json_obj_set_str(v, "id", rs_json_obj_str(key, "id", ""));
    rs_json_obj_set_str(v, "key", rs_json_obj_str(key, "key", ""));
    rs_json_obj_set_str(v, "label", rs_json_obj_str(key, "label", ""));
    char *created = iso8601_from_apple(rs_json_obj_num(key, "createdAt", 0));
    rs_json_obj_set_str(v, "createdAt", created);
    free(created);
    rs_json_obj_set_int(v, "requests", 0);
    rs_json_obj_set_int(v, "bytes", 0);
    rs_json_obj_set(v, "lastSeenAt", rs_json_new_null());
    return v;
}

rs_json *rs_panel_view(const rs_state *st, const char *host) {
    if (!host || !host[0]) host = "127.0.0.1";
    rs_json *out = rs_json_new_obj();

    const rs_json *providers = rs_json_obj_get(st->root, "providers");
    rs_json *providers_out = rs_json_new_arr();
    for (size_t i = 0; i < rs_json_arr_len(providers); i++) {
        rs_json_arr_push(providers_out, provider_view(st, rs_json_arr_at(providers, i), host));
    }
    rs_json_obj_set(out, "providers", providers_out);

    const rs_json *keys = rs_json_obj_get(st->root, "apiKeys");
    rs_json *keys_out = rs_json_new_arr();
    for (size_t i = 0; i < rs_json_arr_len(keys); i++) {
        rs_json_arr_push(keys_out, key_view(rs_json_arr_at(keys, i)));
    }
    rs_json_obj_set(out, "apiKeys", keys_out);

    const rs_json *settings = rs_json_obj_get(st->root, "settings");
    rs_json *settings_out = rs_json_new_obj();
    rs_json_obj_set_int(settings_out, "port", rs_json_obj_int(settings, "port", 8787));
    rs_json_obj_set(out, "settings", settings_out);

    rs_json_obj_set(out, "system", rs_json_new_obj());
    rs_json_obj_set(out, "bandwidth", rs_json_new_obj());
    rs_json_obj_set(out, "version", rs_json_new_obj());
    return out;
}

rs_json *rs_panel_users_view(const rs_state *st) {
    rs_json *out = rs_json_new_obj();
    rs_json *users = rs_json_new_arr();
    const rs_json *stored = rs_json_obj_get(st->root, "adminUsers");
    for (size_t i = 0; i < rs_json_arr_len(stored); i++) {
        const rs_json *u = rs_json_arr_at(stored, i);
        rs_json *view = rs_json_new_obj();
        rs_json_obj_set_str(view, "id", rs_json_obj_str(u, "id", ""));
        rs_json_obj_set_str(view, "username", rs_json_obj_str(u, "username", ""));
        rs_json_obj_set_str(view, "role", rs_panel_user_role(u));
        char *created = iso8601_from_apple(rs_json_obj_num(u, "createdAt", 0));
        rs_json_obj_set_str(view, "createdAt", created);
        free(created);
        rs_json_arr_push(users, view);
    }
    rs_json_obj_set(out, "users", users);
    return out;
}

rs_json *rs_panel_keys_view(const rs_state *st) {
    rs_json *out = rs_json_new_obj();
    rs_json *keys = rs_json_new_arr();
    const rs_json *stored = rs_json_obj_get(st->root, "apiKeys");
    for (size_t i = 0; i < rs_json_arr_len(stored); i++) {
        rs_json_arr_push(keys, key_view(rs_json_arr_at(stored, i)));
    }
    rs_json_obj_set(out, "keys", keys);
    return out;
}

// --- provider mutations ----------------------------------------------------

// The download tool for a provider's manifest/segment fetches. Only curl, wget
// and aria2c are supported; anything else becomes "curl" (or "" to inherit).

int rs_panel_create_provider(rs_state *st, const rs_json *body, const char **err) {
    const char *name = rs_json_obj_str(body, "name", "");
    const char *webhook = rs_json_obj_str(body, "errorWebhookUrl", "");
    const char *trimmed = NULL;
    if (rs_trim(name, strlen(name), true, &trimmed) == 0) { *err = "Provider name is required."; return -400; }
    if (webhook[0] && !is_http_url(webhook)) { *err = "Webhook URL must begin with http:// or https://."; return -400; }

    rs_json *p = rs_json_new_obj();
    char *id = make_id("provider");
    rs_json_obj_set_str(p, "id", id);
    free(id);
    rs_json_obj_set_str(p, "name", name);
    rs_json_obj_set_str(p, "logo", rs_json_obj_str(body, "logo", ""));
    rs_json_obj_set_str(p, "proxy", rs_json_obj_str(body, "proxy", ""));
    rs_json_obj_set_str(p, "errorWebhookUrl", rs_json_obj_str(body, "errorWebhookUrl", ""));
    rs_json_obj_set_str(p, "headers", rs_json_obj_str(body, "headers", ""));
    rs_json_obj_set_str(p, "downloader", normalize_downloader(rs_json_obj_str(body, "downloader", "")));
    rs_json_obj_set_str(p, "downloaderParams", rs_json_obj_str(body, "downloaderParams", ""));
    rs_json_obj_set_bool(p, "forceIpv6", rs_json_obj_bool(body, "forceIpv6", false));
    rs_json_obj_set_bool(p, "rotateProxies", rs_json_obj_bool(body, "rotateProxies", false));
    rs_json_obj_set_str(p, "segmentUrlParams", rs_json_obj_str(body, "segmentUrlParams", ""));
    rs_json_obj_set_bool(p, "inheritUrlParams", rs_json_obj_bool(body, "inheritUrlParams", false));
    rs_json_obj_set(p, "streams", rs_json_new_arr());
    rs_json_obj_set(p, "scriptAccounts", rs_json_new_arr());
    rs_json_obj_set_str(p, "scriptPath", "");
    rs_json_obj_set_str(p, "scriptBind", "");
    rs_json_obj_set_str(p, "scriptDoh", "");
    rs_json_obj_set_str(p, "scriptWorker", "");
    rs_json_obj_set_str(p, "activeScriptAccountId", "");
    rs_json_obj_set_str(p, "accountSelectionMode", "fixed");
    rs_json_obj_set(p, "scriptActions", rs_panel_default_script_actions());
    rs_json_arr_push(providers_array(st), p);
    (void)err;
    return 0;
}

// Replaces a key in `dst` with a clone of `src`'s value, or removes it when
// absent — the "?? default" pattern for optional body fields on update.
static void set_str_from(rs_json *dst, const char *key, const rs_json *body, const char *fallback) {
    rs_json_obj_set_str(dst, key, rs_json_obj_str(body, key, fallback));
}

int rs_panel_update_provider(rs_state *st, const char *id, const rs_json *body, const char **err) {
    rs_json *p = find_provider(st, id);
    if (!p) { *err = "Provider not found."; return -404; }
    const char *name = rs_json_obj_str(body, "name", "");
    const char *webhook = rs_json_obj_str(body, "errorWebhookUrl", "");
    const char *trimmed = NULL;
    if (rs_trim(name, strlen(name), true, &trimmed) == 0) { *err = "Provider name is required."; return -400; }
    if (webhook[0] && !is_http_url(webhook)) { *err = "Webhook URL must begin with http:// or https://."; return -400; }

    rs_json_obj_set_str(p, "name", name);
    // logo keeps its existing value when the body omits it.
    if (rs_json_obj_get(body, "logo")) set_str_from(p, "logo", body, "");
    set_str_from(p, "proxy", body, "");
    set_str_from(p, "errorWebhookUrl", body, "");
    set_str_from(p, "headers", body, "");
    rs_json_obj_set_str(p, "downloader", normalize_downloader(rs_json_obj_str(body, "downloader", "")));
    set_str_from(p, "downloaderParams", body, "");
    rs_json_obj_set_bool(p, "forceIpv6", rs_json_obj_bool(body, "forceIpv6", false));
    rs_json_obj_set_bool(p, "rotateProxies", rs_json_obj_bool(body, "rotateProxies", false));
    set_str_from(p, "segmentUrlParams", body, "");
    rs_json_obj_set_bool(p, "inheritUrlParams", rs_json_obj_bool(body, "inheritUrlParams", false));
    set_str_from(p, "scriptPath", body, "");
    set_str_from(p, "scriptBind", body, "");
    set_str_from(p, "scriptDoh", body, "");
    set_str_from(p, "scriptWorker", body, "");

    const rs_json *accounts = rs_json_obj_get(body, "scriptAccounts");
    rs_json_obj_set(p, "scriptAccounts",
                    accounts && rs_json_type_of(accounts) == RS_JSON_ARR ? rs_json_clone(accounts) : rs_json_new_arr());

    const char *requested_active = rs_json_obj_str(body, "activeScriptAccountId", "");
    // Keep it only if it names one of the accounts, else fall back to the first.
    const rs_json *acc_arr = rs_json_obj_get(p, "scriptAccounts");
    const char *active = "";
    for (size_t i = 0; i < rs_json_arr_len(acc_arr); i++) {
        if (strcmp(rs_json_obj_str(rs_json_arr_at(acc_arr, i), "id", ""), requested_active) == 0) {
            active = requested_active;
            break;
        }
    }
    if (!active[0] && rs_json_arr_len(acc_arr) > 0) active = rs_json_obj_str(rs_json_arr_at(acc_arr, 0), "id", "");
    rs_json_obj_set_str(p, "activeScriptAccountId", active);

    const char *mode = rs_json_obj_str(body, "accountSelectionMode", "fixed");
    if (strcmp(mode, "rotate") != 0 && strcmp(mode, "random") != 0) mode = "fixed";
    rs_json_obj_set_str(p, "accountSelectionMode", mode);

    const rs_json *req_actions = rs_json_obj_get(body, "scriptActions");
    if (req_actions && rs_json_type_of(req_actions) == RS_JSON_ARR) {
        rs_json_obj_set(p, "scriptActions", rs_json_clone(req_actions));
    }
    return 0;
}

// Rebuilds an array of objects excluding the one whose id matches. Returns a new
// array (the caller replaces the old via rs_json_obj_set).
static rs_json *array_without_id(const rs_json *arr, const char *id, bool *found) {
    rs_json *keep = rs_json_new_arr();
    *found = false;
    for (size_t i = 0; i < rs_json_arr_len(arr); i++) {
        const rs_json *el = rs_json_arr_at(arr, i);
        if (strcmp(rs_json_obj_str(el, "id", ""), id) == 0) { *found = true; continue; }
        rs_json_arr_push(keep, rs_json_clone(el));
    }
    return keep;
}

int rs_panel_delete_provider(rs_state *st, const char *id, const char **err) {
    bool found = false;
    rs_json *kept = array_without_id(providers_array(st), id, &found);
    if (!found) { rs_json_free(kept); *err = "Provider not found."; return -404; }
    rs_json_obj_set(st->root, "providers", kept);
    return 0;
}

// --- script actions --------------------------------------------------------
//
// Nothing spawns a provider script without asking here first. An action the
// provider hasn't declared is never invoked, so a script that only implements
// `channels` never sees a `downloadmanifest` it would fail on — and the two
// download hooks, which are storable but not wired to anything yet, are never
// invoked whatever the configuration says.

static bool action_is(const char *action, const char *name) {
    return action && strcmp(action, name) == 0;
}

bool rs_panel_script_action_wired(const char *action) {
    // Routing every init/segment fetch through a spawned subprocess needs a
    // persistent worker rather than one process per fetch. The choice is stored
    // so enabling it later needs no reconfiguration, but it never runs.
    return !action_is(action, "downloadinit") && !action_is(action, "downloadmedia");
}

rs_json *rs_panel_default_script_actions(void) {
    // What a brand-new provider gets: the account and catalogue actions nearly
    // every script implements. The per-stream playback hooks stay off until
    // they are asked for.
    static const char *const defaults[] = {"login", "pair", "channels", "events"};
    rs_json *arr = rs_json_new_arr();
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++)
        rs_json_arr_push(arr, rs_json_new_str(defaults[i]));
    return arr;
}

// The set in effect for a stream: its own override when it has one, otherwise
// its provider's.
//
// An empty override on a stream really does mean "nothing" — that is what the
// editor's "untick everything to keep the script away from this stream
// entirely" offers. An empty (or absent) set on a *provider* means "not
// configured": providers created before this field was populated stored an
// empty array, and reading that as "nothing allowed" would silently stop
// working providers from importing their channels on upgrade. Those fall back
// to the defaults instead, which is what such a provider has always behaved as.
static const rs_json *effective_script_actions(const rs_json *provider, const rs_json *stream,
                                               rs_json **owned) {
    *owned = NULL;
    if (stream) {
        const rs_json *override = rs_json_obj_get(stream, "scriptActionsOverride");
        if (override && rs_json_type_of(override) == RS_JSON_ARR) return override;
    }
    const rs_json *declared = rs_json_obj_get(provider, "scriptActions");
    if (declared && rs_json_type_of(declared) == RS_JSON_ARR && rs_json_arr_len(declared) > 0)
        return declared;
    *owned = rs_panel_default_script_actions();
    return *owned;
}

bool rs_panel_script_action_allowed(const rs_json *provider, const rs_json *stream,
                                    const char *action) {
    if (!action || !action[0] || !rs_panel_script_action_wired(action)) return false;
    rs_json *owned = NULL;
    const rs_json *actions = effective_script_actions(provider, stream, &owned);
    bool found = false;
    for (size_t i = 0; i < rs_json_arr_len(actions) && !found; i++)
        found = strcmp(rs_json_as_str(rs_json_arr_at(actions, i), ""), action) == 0;
    rs_json_free(owned);
    return found;
}

const char *rs_panel_effective_script_path(const rs_json *provider, const rs_json *stream) {
    const char *override = stream ? rs_json_obj_str(stream, "scriptOverride", "") : "";
    if (override[0]) return override;
    return rs_json_obj_str(provider, "scriptPath", "");
}

// --- stream mutations ------------------------------------------------------

int rs_panel_create_stream(rs_state *st, const char *provider_id, const rs_json *body, const char **err) {
    rs_json *p = find_provider(st, provider_id);
    if (!p) { *err = "Provider not found."; return -404; }
    char *id = make_id("stream");
    rs_json *stream = stream_from_body(body, id, err);
    free(id);
    if (!stream) return -400;
    const rs_json *streams = rs_json_obj_get(p, "streams");
    if (!streams || rs_json_type_of(streams) != RS_JSON_ARR) {
        rs_json_obj_set(p, "streams", rs_json_new_arr());
        streams = rs_json_obj_get(p, "streams");
    }
    rs_json_arr_push((rs_json *)streams, stream);
    return 0;
}

int rs_panel_update_stream(rs_state *st, const char *stream_id, const rs_json *body, const char **err) {
    rs_json *provider = NULL;
    rs_json *existing = find_stream(st, stream_id, &provider);
    if (!existing) { *err = "Stream not found."; return -404; }
    rs_json *updated = stream_from_body(body, stream_id, err);
    if (!updated) return -400;

    // Channel/event-import fields have no editor inputs, so carry them over from
    // the existing stream rather than resetting them (a PUT merges, not replaces).
    static const char *carried[] = {"sourceType", "mode", "scriptVideoSelector", "scriptAudioSelector",
                                    "onDemand", "speedUp", "autostart", "scriptStart", "scriptEnd",
                                    "recordEvent"};
    for (size_t i = 0; i < sizeof(carried) / sizeof(carried[0]); i++) {
        const rs_json *v = rs_json_obj_get(existing, carried[i]);
        if (v) rs_json_obj_set(updated, carried[i], rs_json_clone(v));
    }
    // For an imported stream with no per-stream override, the scripting toggles
    // the editor didn't send stay as they were.
    bool imported = rs_json_obj_str(existing, "sourceType", "")[0] != '\0';
    const char *override = rs_json_obj_str(existing, "scriptOverride", "");
    if (imported && override[0] == '\0') {
        static const char *maybe[] = {"sessionManifest", "useCdm", "heartbeatEnabled",
                                      "scriptParams", "cdmType", "cdmMode"};
        for (size_t i = 0; i < sizeof(maybe) / sizeof(maybe[0]); i++) {
            if (!rs_json_obj_get(body, maybe[i])) {
                const rs_json *v = rs_json_obj_get(existing, maybe[i]);
                if (v) rs_json_obj_set(updated, maybe[i], rs_json_clone(v));
            }
        }
    }

    // Replace the stream in the provider's array, preserving order.
    rs_json *streams = (rs_json *)rs_json_obj_get(provider, "streams");
    rs_json *rebuilt = rs_json_new_arr();
    for (size_t i = 0; i < rs_json_arr_len(streams); i++) {
        const rs_json *s = rs_json_arr_at(streams, i);
        if (strcmp(rs_json_obj_str(s, "id", ""), stream_id) == 0) {
            rs_json_arr_push(rebuilt, updated);
        } else {
            rs_json_arr_push(rebuilt, rs_json_clone(s));
        }
    }
    rs_json_obj_set(provider, "streams", rebuilt);
    return 0;
}

int rs_panel_delete_stream(rs_state *st, const char *stream_id, const char **err) {
    rs_json *provider = NULL;
    if (!find_stream(st, stream_id, &provider)) { *err = "Stream not found."; return -404; }
    bool found = false;
    rs_json *kept = array_without_id(rs_json_obj_get(provider, "streams"), stream_id, &found);
    rs_json_obj_set(provider, "streams", kept);
    return 0;
}

// --- channel/event import --------------------------------------------------

// The import protocol capitalises its keys ("Name", "SessionManifest"), but
// scripts written against the panel's own JSON have always been accepted with
// the lowercase spelling too. Look for both, capitalised first.
static const rs_json *entry_get(const rs_json *entry, const char *upper, const char *lower) {
    const rs_json *v = rs_json_obj_get(entry, upper);
    return v ? v : rs_json_obj_get(entry, lower);
}

static const char *entry_str(const rs_json *entry, const char *upper, const char *lower) {
    return rs_json_as_str(entry_get(entry, upper, lower), "");
}

static bool entry_bool(const rs_json *entry, const char *upper, const char *lower, bool fallback) {
    const rs_json *v = entry_get(entry, upper, lower);
    return v ? rs_json_as_bool(v, fallback) : fallback;
}

// Older provider scripts call the manifest hook's argument string
// `ManifestScript`; the documented protocol calls it `ScriptParams`. They are
// the same stream field, so accept both without requiring working scripts to
// be rewritten. Prefer the documented name when both are present.
static const char *entry_script_params(const rs_json *entry) {
    const rs_json *v = entry_get(entry, "ScriptParams", "scriptParams");
    if (v) return rs_json_as_str(v, "");
    return entry_str(entry, "ManifestScript", "manifestScript");
}

// Matches an already-imported entry: (name, sourceType) is all the protocol
// gives us — entries carry no stable id — so that pair is the identity a
// re-import updates in place.
static rs_json *find_imported_stream(rs_json *streams, const char *name, const char *source_type) {
    for (size_t i = 0; i < rs_json_arr_len(streams); i++) {
        rs_json *s = (rs_json *)rs_json_arr_at(streams, i);
        if (strcmp(rs_json_obj_str(s, "name", ""), name) == 0 &&
            strcmp(rs_json_obj_str(s, "sourceType", ""), source_type) == 0)
            return s;
    }
    return NULL;
}

int rs_panel_import_script_entries(rs_state *st, const char *provider_id, const char *action,
                                   const rs_json *doc, const rs_json *logos,
                                   int *imported, const char **err) {
    if (imported) *imported = 0;
    rs_json *p = find_provider(st, provider_id);
    if (!p) { *err = "Provider not found."; return -404; }

    bool events = strcmp(action, "events") == 0;
    const rs_json *list = rs_json_obj_get(doc, events ? "Events" : "Channels");
    if (!list) list = rs_json_obj_get(doc, events ? "events" : "channels");
    if (!list || rs_json_type_of(list) != RS_JSON_ARR) {
        *err = events ? "Script output has no 'Events' array."
                      : "Script output has no 'Channels' array.";
        return -400;
    }
    const char *source_type = events ? "event" : "channel";

    rs_json *streams = (rs_json *)rs_json_obj_get(p, "streams");
    if (!streams || rs_json_type_of(streams) != RS_JSON_ARR) {
        rs_json_obj_set(p, "streams", rs_json_new_arr());
        streams = (rs_json *)rs_json_obj_get(p, "streams");
    }

    int count = 0;
    for (size_t i = 0; i < rs_json_arr_len(list); i++) {
        const rs_json *entry = rs_json_arr_at(list, i);
        if (rs_json_type_of(entry) != RS_JSON_OBJ) continue;
        const char *raw_name = entry_str(entry, "Name", "name");
        char *name = rs_trim_dup(raw_name, strlen(raw_name), true);
        if (!name || !name[0]) { free(name); continue; }

        rs_json *stream = find_imported_stream(streams, name, source_type);
        if (!stream) {
            char *id = make_id("stream");
            rs_json *empty = rs_json_new_obj();
            rs_json *fresh = stream_build(empty, id);
            rs_json_free(empty);
            free(id);
            size_t before = rs_json_arr_len(streams);
            rs_json_arr_push(streams, fresh);
            // rs_json_arr_push frees the value it could not store, so only
            // keep the pointer once the array really took it.
            if (rs_json_arr_len(streams) == before) { free(name); continue; }
            stream = fresh;
        }

        rs_json_obj_set_str(stream, "name", name);
        // A logo the operator set by hand (or a previous import resolved) wins
        // over the freshly looked-up one; only an empty slot is filled.
        const char *existing_logo = rs_json_obj_str(stream, "logo", "");
        if (!existing_logo[0] && logos) {
            const char *found = rs_json_obj_str(logos, name, "");
            if (found[0]) rs_json_obj_set_str(stream, "logo", found);
        }
        rs_json_obj_set_str(stream, "sourceType", source_type);
        const char *mode = entry_str(entry, "Mode", "mode");
        rs_json_obj_set_str(stream, "mode", mode[0] ? mode : "live");
        rs_json_obj_set_bool(stream, "sessionManifest",
                             entry_bool(entry, "SessionManifest", "sessionManifest", false));
        rs_json_obj_set_str(stream, "scriptParams", entry_script_params(entry));
        rs_json_obj_set_str(stream, "cdmType", entry_str(entry, "CdmType", "cdmType"));
        rs_json_obj_set_bool(stream, "useCdm", entry_bool(entry, "UseCdm", "useCdm", false));
        rs_json_obj_set_str(stream, "scriptVideoSelector", entry_str(entry, "Video", "video"));
        rs_json_obj_set_str(stream, "scriptAudioSelector", entry_str(entry, "Audio", "audio"));
        rs_json_obj_set_bool(stream, "onDemand", entry_bool(entry, "OnDemand", "onDemand", false));
        rs_json_obj_set_bool(stream, "speedUp", entry_bool(entry, "SpeedUp", "speedUp", false));
        rs_json_obj_set_bool(stream, "autostart", entry_bool(entry, "Autostart", "autostart", false));
        rs_json_obj_set_bool(stream, "recordEvent", entry_bool(entry, "RecordEvent", "recordEvent", false));
        // Start/End are epoch seconds and only meaningful for events; absent
        // (or non-numeric) leaves the stored null, which the grid reads as
        // "no window".
        static const char *const window[2][3] = {{"Start", "start", "scriptStart"},
                                                 {"End", "end", "scriptEnd"}};
        for (size_t w = 0; w < 2; w++) {
            const rs_json *v = entry_get(entry, window[w][0], window[w][1]);
            if (v && rs_json_type_of(v) == RS_JSON_NUM)
                rs_json_obj_set_int(stream, window[w][2], (long long)rs_json_as_num(v, 0));
            else
                rs_json_obj_set(stream, window[w][2], rs_json_new_null());
        }
        free(name);
        count++;
    }
    if (imported) *imported = count;
    return 0;
}

// Persists what the script's `manifest` action just handed back, so every
// pipeline — the DASH engine, the m3u8 passthrough, ffmpeg — reads the live
// session URL rather than the expired one it was started with, and so the panel
// shows the operator what the stream is actually playing. Empty header text and
// a zero heartbeat mean "the script didn't say", and leave what was configured
// alone rather than clearing it.
int rs_panel_apply_session_manifest(rs_state *st, const char *stream_id, const char *url,
                                    const rs_json *cdn_urls, const char *manifest_headers,
                                    const char *media_headers, int heartbeat_seconds,
                                    const char **err) {
    rs_json *provider = NULL;
    rs_json *stream = find_stream(st, stream_id, &provider);
    if (!stream) { *err = "Stream not found."; return -404; }
    if (!is_http_url(url)) { *err = "The manifest action returned no usable ManifestUrl."; return -400; }
    rs_json_obj_set_str(stream, "url", url);
    // Imported script streams have no URL when they are created and default to
    // MPD. Once manifest supplies the real source, keep playback in sync with
    // its actual format.
    if (strstr(url, ".m3u8")) rs_json_obj_set_str(stream, "kind", "m3u8");
    else if (strstr(url, ".mpd")) rs_json_obj_set_str(stream, "kind", "mpd");
    rs_json_obj_set(stream, "cdnUrls",
                    cdn_urls && rs_json_type_of(cdn_urls) == RS_JSON_ARR
                        ? rs_json_clone(cdn_urls) : rs_json_new_arr());
    if (manifest_headers && manifest_headers[0])
        rs_json_obj_set_str(stream, "manifestHeaders", manifest_headers);
    if (media_headers && media_headers[0])
        rs_json_obj_set_str(stream, "mediaHeaders", media_headers);
    if (heartbeat_seconds > 0)
        rs_json_obj_set_int(stream, "heartbeatSeconds", heartbeat_seconds);
    return 0;
}

// Stores the clear keys the `cdm` action returned. A stream with useCdm enabled
// treats these as session keys and refreshes them on every start.
int rs_panel_set_stream_keys(rs_state *st, const char *stream_id, const char *keys,
                             const char **err) {
    rs_json *provider = NULL;
    rs_json *stream = find_stream(st, stream_id, &provider);
    if (!stream) { *err = "Stream not found."; return -404; }
    rs_json_obj_set_str(stream, "decryptionKeys", keys ? keys : "");
    return 0;
}

int rs_panel_set_stream_running(rs_state *st, const char *stream_id, bool running, const char **err) {
    rs_json *provider = NULL;
    rs_json *stream = find_stream(st, stream_id, &provider);
    if (!stream) { *err = "Stream not found."; return -404; }
    rs_json_obj_set_str(stream, "status", running ? "running" : "stopped");
    rs_json_obj_set(stream, "lastError", rs_json_new_null());
    return 0;
}

// --- user + key mutations --------------------------------------------------

const char *rs_panel_user_role(const rs_json *user) {
    // Accounts created before roles existed have no field and are admins, which
    // is what they have always been — a migration that silently demoted the
    // only account would lock the operator out of their own panel.
    const char *role = rs_json_obj_str(user, "role", "admin");
    return strcmp(role, "viewer") == 0 ? "viewer" : "admin";
}

size_t rs_panel_admin_count(const rs_state *st) {
    const rs_json *users = rs_json_obj_get(st->root, "adminUsers");
    size_t admins = 0;
    for (size_t i = 0; i < rs_json_arr_len(users); i++) {
        if (strcmp(rs_panel_user_role(rs_json_arr_at(users, i)), "admin") == 0) admins++;
    }
    return admins;
}

int rs_panel_create_user(rs_state *st, const rs_json *body, const char **err) {
    const char *username = rs_json_obj_str(body, "username", "");
    const char *password = rs_json_obj_str(body, "password", "");
    const char *role = rs_json_obj_str(body, "role", "admin");
    const char *trimmed = NULL;
    if (rs_trim(username, strlen(username), true, &trimmed) == 0 || strlen(password) < 8) {
        *err = "Username and an 8+ character password are required.";
        return -400;
    }
    if (strcmp(role, "admin") != 0 && strcmp(role, "viewer") != 0) {
        *err = "Role must be 'admin' or 'viewer'.";
        return -400;
    }
    rs_json *users = rs_state_admin_users(st);
    for (size_t i = 0; i < rs_json_arr_len(users); i++) {
        if (strcmp(rs_json_obj_str(rs_json_arr_at(users, i), "username", ""), username) == 0) {
            *err = "That username is already taken.";
            return -400;
        }
    }
    char *hash = NULL, *salt = NULL;
    if (rs_auth_hash_password(password, &hash, &salt) != 0) { *err = "Could not hash the password."; return -500; }
    rs_json *u = rs_json_new_obj();
    char *id = make_id("user");
    rs_json_obj_set_str(u, "id", id);
    free(id);
    rs_json_obj_set_str(u, "username", username);
    rs_json_obj_set_str(u, "passwordHash", hash);
    rs_json_obj_set_str(u, "salt", salt);
    rs_json_obj_set_str(u, "role", role);
    rs_json_obj_set(u, "createdAt", rs_json_new_num(apple_epoch_now()));
    rs_free(hash);
    rs_free(salt);
    rs_json_arr_push(users, u);
    return 0;
}

// --- provider export / import ----------------------------------------------

rs_json *rs_panel_export_provider(const rs_state *st, const char *provider_id) {
    const rs_json *providers = rs_json_obj_get(st->root, "providers");
    const rs_json *found = NULL;
    for (size_t i = 0; i < rs_json_arr_len(providers); i++) {
        if (strcmp(rs_json_obj_str(rs_json_arr_at(providers, i), "id", ""), provider_id) == 0) {
            found = rs_json_arr_at(providers, i);
            break;
        }
    }
    if (!found) return NULL;

    // Mirrors the provider export envelope field for
    // field, so a file written by either server imports into the other. Ids are
    // deliberately omitted: they mean nothing on the importing install, which
    // regenerates them. The active account is carried by *name* for the same
    // reason.
    rs_json *provider = rs_json_new_obj();
    static const char *const copied[] = {
        "name", "logo", "proxy", "errorWebhookUrl", "headers", "segmentUrlParams",
        "scriptBind", "scriptDoh", "scriptWorker", "accountSelectionMode", NULL,
    };
    for (size_t i = 0; copied[i]; i++) {
        rs_json_obj_set_str(provider, copied[i],
                            rs_json_obj_str(found, copied[i], strcmp(copied[i], "accountSelectionMode") == 0 ? "fixed" : ""));
    }
    rs_json_obj_set_bool(provider, "inheritUrlParams", rs_json_obj_bool(found, "inheritUrlParams", false));
    rs_json_obj_set_bool(provider, "forceIpv6", rs_json_obj_bool(found, "forceIpv6", false));
    rs_json_obj_set_bool(provider, "rotateProxies", rs_json_obj_bool(found, "rotateProxies", false));

    const rs_json *accounts = rs_json_obj_get(found, "scriptAccounts");
    rs_json_obj_set(provider, "scriptAccounts",
                    accounts ? rs_json_clone(accounts) : rs_json_new_arr());

    const char *active_id = rs_json_obj_str(found, "activeScriptAccountId", "");
    const char *active_name = "";
    for (size_t i = 0; i < rs_json_arr_len(accounts); i++) {
        const rs_json *account = rs_json_arr_at(accounts, i);
        if (strcmp(rs_json_obj_str(account, "id", ""), active_id) == 0) {
            active_name = rs_json_obj_str(account, "name", "");
            break;
        }
    }
    rs_json_obj_set_str(provider, "activeScriptAccountName", active_name);

    rs_json *out = rs_json_new_obj();
    rs_json_obj_set_int(out, "restreamairExport", 1);
    rs_json_obj_set(out, "provider", provider);
    const rs_json *streams = rs_json_obj_get(found, "streams");
    rs_json_obj_set(out, "streams", streams ? rs_json_clone(streams) : rs_json_new_arr());
    return out;
}

int rs_panel_import_provider(rs_state *st, const rs_json *doc, const char *script_path,
                             char **new_id, const char **err) {
    if (new_id) *new_id = NULL;
    const rs_json *source = rs_json_obj_get(doc, "provider");
    const char *name = rs_json_obj_str(source, "name", "");
    const char *trimmed = NULL;
    if (!source || rs_trim(name, strlen(name), true, &trimmed) == 0) {
        *err = "Import file has no provider name.";
        return -400;
    }

    rs_json *p = rs_json_clone(source);
    if (!p) { *err = "Out of memory."; return -500; }

    // Every id is regenerated: the exporting install's ids mean nothing here,
    // and reusing them would collide with whatever already holds them.
    char *id = make_id("provider");
    rs_json_obj_set_str(p, "id", id);

    rs_json *accounts = (rs_json *)rs_json_obj_get(p, "scriptAccounts");
    if (!accounts || rs_json_type_of(accounts) != RS_JSON_ARR) {
        accounts = rs_json_new_arr();
        rs_json_obj_set(p, "scriptAccounts", accounts);
    }
    const char *wanted = rs_json_obj_str(source, "activeScriptAccountName", "");
    char *active_id = NULL;
    for (size_t i = 0; i < rs_json_arr_len(accounts); i++) {
        rs_json *account = (rs_json *)rs_json_arr_at(accounts, i);
        char *account_id = make_id("account");
        rs_json_obj_set_str(account, "id", account_id);
        // Re-resolve the active account by name, since the id it referenced on
        // the exporting install does not exist here.
        if (!active_id && wanted[0] && strcmp(rs_json_obj_str(account, "name", ""), wanted) == 0) {
            active_id = rs_strdup(account_id);
        }
        free(account_id);
    }
    rs_json_obj_set_str(p, "activeScriptAccountId", active_id ? active_id : "");
    rs_free(active_id);
    rs_json_obj_remove(p, "activeScriptAccountName");

    rs_json_obj_set_str(p, "scriptPath", script_path ? script_path : "");

    // Streams come across with fresh ids and stopped, never mid-run.
    rs_json *streams = rs_json_new_arr();
    const rs_json *exported_streams = rs_json_obj_get(doc, "streams");
    for (size_t i = 0; i < rs_json_arr_len(exported_streams); i++) {
        rs_json *stream = rs_json_clone(rs_json_arr_at(exported_streams, i));
        if (!stream) continue;
        char *stream_id = make_id("stream");
        rs_json_obj_set_str(stream, "id", stream_id);
        free(stream_id);
        rs_json_obj_set_str(stream, "status", "stopped");
        rs_json_arr_push(streams, stream);
    }
    rs_json_obj_set(p, "streams", streams);

    rs_json_arr_push(providers_array(st), p);
    if (new_id) *new_id = id; else free(id);
    return 0;
}

int rs_panel_delete_user(rs_state *st, const char *id, const char **err) {
    rs_json *users = rs_state_admin_users(st);
    // Only admins count towards the floor: deleting the last one would leave a
    // panel nobody can administer, but any number of viewers may come and go.
    const rs_json *target = NULL;
    for (size_t i = 0; i < rs_json_arr_len(users); i++) {
        if (strcmp(rs_json_obj_str(rs_json_arr_at(users, i), "id", ""), id) == 0) {
            target = rs_json_arr_at(users, i);
            break;
        }
    }
    if (target && strcmp(rs_panel_user_role(target), "admin") == 0 && rs_panel_admin_count(st) <= 1) {
        *err = "Cannot remove the last admin account.";
        return -400;
    }
    bool found = false;
    rs_json *kept = array_without_id(users, id, &found);
    rs_json_obj_set(st->root, "adminUsers", kept);
    return 0;
}

int rs_panel_create_key(rs_state *st, const rs_json *body, const char **err) {
    rs_json *keys = keys_array(st);
    const char *label = rs_json_obj_str(body, "label", "");
    const char *trimmed = NULL;
    char label_buf[128];
    if (rs_trim(label, strlen(label), true, &trimmed) == 0) {
        snprintf(label_buf, sizeof(label_buf), "Key %zu", rs_json_arr_len(keys) + 1);
        label = label_buf;
    }
    // rsa_ + 32 hex, matching generateKey.
    uint8_t rnd[16];
    rs_random_bytes(rnd, sizeof(rnd));
    char key[64] = "rsa_";
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(rnd); i++) {
        key[4 + i * 2] = hex[rnd[i] >> 4];
        key[4 + i * 2 + 1] = hex[rnd[i] & 0xf];
    }
    key[4 + sizeof(rnd) * 2] = '\0';

    rs_json *k = rs_json_new_obj();
    char *id = make_id("key");
    rs_json_obj_set_str(k, "id", id);
    free(id);
    rs_json_obj_set_str(k, "key", key);
    rs_json_obj_set_str(k, "label", label);
    rs_json_obj_set(k, "createdAt", rs_json_new_num(apple_epoch_now()));
    rs_json_arr_push(keys, k);
    (void)err;
    return 0;
}

int rs_panel_delete_key(rs_state *st, const char *id, const char **err) {
    bool found = false;
    rs_json *kept = array_without_id(keys_array(st), id, &found);
    rs_json_obj_set(st->root, "apiKeys", kept);
    (void)err;
    return 0;
}

const rs_json *rs_panel_find_stream(const rs_state *st, const char *id_or_slug) {
    if (!id_or_slug || !id_or_slug[0]) return NULL;
    const rs_json *providers = rs_json_obj_get(st->root, "providers");

    // Exact id first.
    for (size_t i = 0; i < rs_json_arr_len(providers); i++) {
        const rs_json *streams = rs_json_obj_get(rs_json_arr_at(providers, i), "streams");
        for (size_t j = 0; j < rs_json_arr_len(streams); j++) {
            const rs_json *s = rs_json_arr_at(streams, j);
            if (strcmp(rs_json_obj_str(s, "id", ""), id_or_slug) == 0) return s;
        }
    }
    // Else a unique name slug.
    const rs_json *match = NULL;
    int matches = 0;
    for (size_t i = 0; i < rs_json_arr_len(providers); i++) {
        const rs_json *streams = rs_json_obj_get(rs_json_arr_at(providers, i), "streams");
        for (size_t j = 0; j < rs_json_arr_len(streams); j++) {
            const rs_json *s = rs_json_arr_at(streams, j);
            char *slug = rs_panel_slugify(rs_json_obj_str(s, "name", ""));
            if (slug && slug[0] && strcmp(slug, id_or_slug) == 0) { match = s; matches++; }
            free(slug);
        }
    }
    return matches == 1 ? match : NULL;
}

bool rs_panel_playback_allowed(const rs_state *st, const char *provided_key) {
    const rs_json *keys = rs_json_obj_get(st->root, "apiKeys");
    size_t count = rs_json_arr_len(keys);
    if (count == 0) return true;  // no keys configured: playback is open
    if (!provided_key || !provided_key[0]) return false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(rs_json_obj_str(rs_json_arr_at(keys, i), "key", ""), provided_key) == 0) return true;
    }
    return false;
}
