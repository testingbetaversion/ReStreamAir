#include "rs_panel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rs_internal.h"

// The 2001 reference epoch Swift's JSONEncoder writes Date values against.
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

// --- stream construction ---------------------------------------------------

static long long clamp_ll(long long v, long long lo) { return v < lo ? lo : v; }

// Builds a stream object from a request body, applying the same defaults,
// clamps and validation as streamFromBody. `err` is set on failure.
static rs_json *stream_from_body(const rs_json *body, const char *id, const char **err) {
    const char *name = rs_json_obj_str(body, "name", "");
    const char *url = rs_json_obj_str(body, "url", "");
    // A trimmed-empty name is rejected.
    const char *trimmed = NULL;
    if (rs_trim(name, strlen(name), true, &trimmed) == 0) { *err = "Stream name is required."; return NULL; }
    if (!is_http_url(url)) { *err = "Stream URL must be http or https."; return NULL; }

    rs_json *s = rs_json_new_obj();
    rs_json_obj_set_str(s, "id", id);
    rs_json_obj_set_str(s, "name", name);
    rs_json_obj_set_str(s, "kind", strcmp(rs_json_obj_str(body, "kind", ""), "m3u8") == 0 ? "m3u8" : "mpd");
    rs_json_obj_set_str(s, "url", url);
    rs_json_obj_set_str(s, "representation", rs_json_obj_str(body, "representation", ""));
    rs_json_obj_set_str(s, "period", rs_json_obj_str(body, "period", ""));
    rs_json_obj_set_str(s, "proxy", rs_json_obj_str(body, "proxy", ""));
    rs_json_obj_set_int(s, "playlistSegments", clamp_ll(rs_json_obj_int(body, "playlistSegments", 6), 3));
    rs_json_obj_set_int(s, "keepSegments", clamp_ll(rs_json_obj_int(body, "keepSegments", 60), 1));
    rs_json_obj_set_int(s, "downloadAhead", clamp_ll(rs_json_obj_int(body, "downloadAhead", 8), 1));
    double poll = rs_json_obj_num(body, "pollInterval", 2.0);
    rs_json_obj_set(s, "pollInterval", rs_json_new_num(poll < 0.25 ? 0.25 : poll));
    rs_json_obj_set_bool(s, "forceOffline", rs_json_obj_bool(body, "forceOffline", false));
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

    rs_json_obj_set_str(s, "manifestHeaders", rs_json_obj_str(body, "manifestHeaders", ""));
    rs_json_obj_set_str(s, "mediaHeaders", rs_json_obj_str(body, "mediaHeaders", ""));
    rs_json_obj_set_str(s, "hlsKeyHeaders", rs_json_obj_str(body, "hlsKeyHeaders", ""));
    rs_json_obj_set_int(s, "audioDelayMs", rs_json_obj_int(body, "audioDelayMs", 0));

    // Enumerated fields fall back to their default when the value isn't valid.
    const char *input_mode = rs_json_obj_str(body, "inputMode", "internal");
    static const char *valid_inputs[] = {"internal", "ffmpegResident", "ffmpegTsHls",
                                         "ffmpegMultiTsHls", "ffmpegFmp4Hls", "nm3u8dlre"};
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

    // Slug for the play URL: the name slug when unique, else the id.
    char *slug = rs_panel_slugify(name);
    const char *play_id = (slug && slug[0] && slug_occurrences(st, slug) == 1) ? slug : id;

    // No worker layer yet, so nothing is ever running in the C server.
    rs_json_obj_set_bool(v, "running", false);
    if (!rs_json_obj_get(stream, "status")) rs_json_obj_set_str(v, "status", "stopped");
    rs_json_obj_set_int(v, "activeClients", 0);
    rs_json_obj_set(v, "bandwidth", zero_bandwidth());
    rs_json_obj_set(v, "inputBandwidth", zero_bandwidth());

    char url[1024];
    snprintf(url, sizeof(url), "http://%s/play/%s/index.m3u8", host, play_id);
    rs_json_obj_set_str(v, "playUrl", url);
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
    if (strcmp(kind, "m3u8") == 0) {
        snprintf(url, sizeof(url), "http://%s/source/%s", host, id);
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
    snprintf(dir, sizeof(dir), "runtime/scripts/%s", rs_json_obj_str(provider, "id", ""));
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
// and aria2c are supported; anything else (including empty) becomes "curl".
static const char *normalize_downloader(const char *d) {
    if (strcmp(d, "wget") == 0) return "wget";
    if (strcmp(d, "aria2c") == 0) return "aria2c";
    return "curl";
}

int rs_panel_create_provider(rs_state *st, const rs_json *body, const char **err) {
    const char *name = rs_json_obj_str(body, "name", "");
    const char *trimmed = NULL;
    if (rs_trim(name, strlen(name), true, &trimmed) == 0) { *err = "Provider name is required."; return -400; }

    rs_json *p = rs_json_new_obj();
    char *id = make_id("provider");
    rs_json_obj_set_str(p, "id", id);
    free(id);
    rs_json_obj_set_str(p, "name", name);
    rs_json_obj_set_str(p, "logo", rs_json_obj_str(body, "logo", ""));
    rs_json_obj_set_str(p, "proxy", rs_json_obj_str(body, "proxy", ""));
    rs_json_obj_set_str(p, "headers", rs_json_obj_str(body, "headers", ""));
    rs_json_obj_set_str(p, "downloader", normalize_downloader(rs_json_obj_str(body, "downloader", "")));
    rs_json_obj_set_str(p, "downloaderParams", rs_json_obj_str(body, "downloaderParams", ""));
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
    rs_json_obj_set(p, "scriptActions", rs_json_new_arr());  // no script yet
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
    const char *trimmed = NULL;
    if (rs_trim(name, strlen(name), true, &trimmed) == 0) { *err = "Provider name is required."; return -400; }

    rs_json_obj_set_str(p, "name", name);
    // logo keeps its existing value when the body omits it.
    if (rs_json_obj_get(body, "logo")) set_str_from(p, "logo", body, "");
    set_str_from(p, "proxy", body, "");
    set_str_from(p, "headers", body, "");
    rs_json_obj_set_str(p, "downloader", normalize_downloader(rs_json_obj_str(body, "downloader", "")));
    set_str_from(p, "downloaderParams", body, "");
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
    // the existing stream rather than resetting them (see the Swift PUT merge).
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

// --- user + key mutations --------------------------------------------------

int rs_panel_create_user(rs_state *st, const rs_json *body, const char **err) {
    const char *username = rs_json_obj_str(body, "username", "");
    const char *password = rs_json_obj_str(body, "password", "");
    const char *trimmed = NULL;
    if (rs_trim(username, strlen(username), true, &trimmed) == 0 || strlen(password) < 8) {
        *err = "Username and an 8+ character password are required.";
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
    rs_json_obj_set(u, "createdAt", rs_json_new_num(apple_epoch_now()));
    rs_free(hash);
    rs_free(salt);
    rs_json_arr_push(users, u);
    return 0;
}

int rs_panel_delete_user(rs_state *st, const char *id, const char **err) {
    rs_json *users = rs_state_admin_users(st);
    if (rs_json_arr_len(users) <= 1) { *err = "Cannot remove the last admin account."; return -400; }
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
