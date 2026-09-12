#include "rs_provider_options.h"
#include "rs_common.h"
#include "rs_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name, *label, *group;
    rs_json_type type;
    int value, min, max;
    const char *hint, *inactive;
} option_field;

#define B(n,l,h,i) {n,l,"Behaviour",RS_JSON_BOOL,0,0,0,h,i}
#define S(n,l,g,h,i) {n,l,g,RS_JSON_STR,0,0,4096,h,i}
#define N(n,l,g,v,lo,hi,h,i) {n,l,g,RS_JSON_NUM,v,lo,hi,h,i}
static const option_field fields[] = {
    B("alwaysResetSession", "Always reset session", "Clear saved session files before login or the first stream start while the provider is idle. Concurrent streams share their active session.", ""),
    B("noRestartOnError", "No restart on error", "DASH and FFmpeg: stop automatic recovery after an exhausted request, process failure or stall. Manual starts still work.", ""),
    B("restartFinishedBroadcast", "Restart finished broadcast", "DASH and FFmpeg: restart after the broadcast ends, using Restart delay. DASH lets its buffered media drain first.", ""),
    B("noRestartOnTrackChange", "No restart on track change", "Internal DASH: switch discovered tracks in the running engine. When off, changing track IDs schedules a clean restart.", ""),
    B("reuseEventIndex", "Reuse event index", "Reuse the public stream ID of a stopped, ended event for a new imported event. Clears the old source and keys; matching event names keep their existing ID.", ""),
    B("dontWaitForFullPlaylist", "Don't wait for full playlist", "Internal DASH: publish as soon as one complete segment is available. May increase buffering at startup.", ""),
    B("ignoreDashStaticFlag", "Ignore DASH static flag", "Internal DASH: keep polling MPDs tagged static for new segments. When off, a drained static manifest produces a finished HLS playlist.", ""),
    B("useDashDelay", "Use DASH delay", "Internal DASH: honor suggestedPresentationDelay from the MPD, up to 120s. The larger of this delay and the configured playback buffer is used.", ""),
    B("useSessionCookies", "Use session cookies", "Share the provider cookie jar with manifest and media downloads. Uses the built-in HTTP client and serializes cookie requests to protect the jar.", ""),
    B("legacyDashParser", "Legacy DASH parser", "Internal DASH: enable tolerant XML recovery for malformed legacy manifests. When off, malformed MPDs fail parsing.", ""),
    B("detectJsonRedirect", "Detect special JSON redirect URL", "Follow HTTP(S) URLs in JSON ManifestUrl, manifestUrl, url, redirect, location or redirectUrl fields, including a data object. Maximum five redirects.", ""),
    B("offAirFallback", "Off air fallback", "Internal DASH: switch to Off air fallback MPD URL after primary failure or broadcast completion. A fallback URL is required.", ""),
    B("coolDownAutoRestart", "Cool down streams auto-restart", "DASH and FFmpeg: progressively double the restart delay after consecutive failures, capped at 5 minutes or the configured delay if larger.", ""),
    B("autoRemoveMissingChannels", "Auto-remove missing channels", "After a successful channel import, remove imported channels absent from that list and stop their pipelines. Manually added streams are retained.", ""),
    B("autoRefreshEvents", "Auto-refresh events", "Periodically run the declared events script action, even with the panel closed. Uses Events refresh period and waits while another provider script job is running.", ""),
    B("autoRemoveFinishedEvents", "Auto-remove finished events", "Stop and remove imported events when their End timestamp is reached. Events without an end remain.", ""),
    S("userAgent", "User Agent", "Requests and scripts", "Blank uses the downloader's default. A User-Agent in Additional HTTP headers takes precedence.", ""),
    S("xForwardedFor", "X-Forwarded-For", "Requests and scripts", "Optional upstream request header. An X-Forwarded-For in Additional HTTP headers takes precedence.", ""),
    S("epgTimezone", "EPG timezone", "Requests and scripts", "Re-express full XMLTV programme timestamps in UTC or a fixed offset such as UTC+05:00. Blank preserves the original guide. Partial dates are retained.", ""),
    S("offAirFallbackUrl", "Off air fallback MPD URL", "Requests and scripts", "Internal DASH: live MPD to play when the primary is unavailable. Returns to the primary on manual or timed restart. Use a clear, publicly reachable fallback source.", ""),
    S("defaultCdn", "Default CDN", "Requests and scripts", "Name of the CDN entry to choose from a manifest script response. A missing name fails the start visibly. Blank uses the script primary URL.", ""),
    S("defaultVideo", "Default video", "Requests and scripts", "Internal DASH defaults: comma-separated preferences: best, worst, id=ID, codec=avc, height<=720, bandwidth<=2000000. Explicit stream selections take precedence.", ""),
    S("defaultAudio", "Default audio", "Requests and scripts", "Internal DASH defaults: comma-separated preferences: lang=ur, id=ID, codec=mp4a, best or worst. Explicit stream selections take precedence.", ""),
    S("pipeCommand", "Pipe command", "Requests and scripts", "Fallback command for streams using Program pipe input with an empty stream Pipe command. Applies on next start.", ""),
    N("scriptTimeoutSeconds", "Script timeout (s)", "Requests and scripts", 30,1,3600, "Maximum duration of each provider or stream script action. Applies to the next action.", ""),
    N("hlsPlaylistDurationSeconds", "HLS playlist duration (s)", "Output and downloads", 0,1,7200, "0 keeps each stream's playlist count. Otherwise converted to segment count using the target fragment duration, with a minimum of 3 segments. Output fragments count takes precedence.", ""),
    N("outputFragmentsCount", "Output fragments count", "Output and downloads", 0,3,240, "0 uses playlist duration or the stream setting. Otherwise overrides the advertised HLS window (3–240 fragments).", ""),
    N("hlsFragmentDurationSeconds", "HLS fragments duration (s)", "Output and downloads", 0,1,30, "0 keeps each stream's setting. Otherwise overrides internal DASH and FFmpeg HLS target duration (1–30s).", ""),
    N("maxStreamsConcurrency", "Max streams concurrency", "Output and downloads", 0,1,10000, "0 = unlimited. Caps streams marked running for this provider. Lowering the limit leaves already running streams active.", ""),
    N("maxDownloadConcurrency", "Max download concurrency", "Output and downloads", 50,1,1000, "Shared cap on provider manifest, media and probe requests through the built-in/selected downloaders. FFmpeg manages its own internal downloads.", ""),
    N("httpGetTimeoutSeconds", "HTTP get timeout (s)", "Output and downloads", 30,1,3600, "Maximum duration of each upstream request attempt, including built-in and external downloaders. Also sets FFmpeg socket timeout.", ""),
    N("httpGetAttempts", "HTTP get tentatives count", "Output and downloads", 2,1,100, "Maximum HTTP attempts per URL and proxy pool operation. Does not retry permanent 4xx responses except 408/429. FFmpeg uses its own reconnect policy.", ""),
    N("stalledStreamTimeoutSeconds", "Stalled stream timeout (s)", "Output and downloads", 60,1,3600, "DASH and FFmpeg: treat a lack of new media/output timestamps for this duration as an error, then apply the restart policy.", ""),
    N("eventsRefreshSeconds", "Events refresh period (s)", "Events and timing", 3600,1,604800, "Interval between automatic events script imports while Auto-refresh events is enabled.", ""),
    N("maxEventsCount", "Max events count", "Events and timing", 0,1,100000, "0 = unlimited. Limits valid events processed by each Load events import. Existing events beyond the limit are retained.", ""),
    N("restartDelaySeconds", "Restart delay (s)", "Events and timing", 30,0,3600, "Wait between stopping a failed/finished pipeline and restarting it; also applies to timed restarts.", ""),
    N("autoRestartPeriodSeconds", "Autorestart period (s)", "Events and timing", 0,1,604800, "Restart each running stream periodically; 0 disables. A timed restart returns an off-air fallback stream to its primary source.", ""),
    N("randomAutostartPeriodSeconds", "Random autostart period (s)", "Events and timing", 0,1,604800, "Every interval, start one randomly chosen stopped stream marked Include in provider autostart, within its event window. 0 disables.", ""),
    N("sequentialAutostartPeriodSeconds", "Seq autostart period (s)", "Events and timing", 0,1,604800, "Every interval, start the next eligible stopped stream marked Include in provider autostart, in provider order. 0 disables.", ""),
    N("playbackDelaySeconds", "Playback delay (s)", "Events and timing", 0,1,120, "Internal DASH: 0 keeps each stream's buffer. Otherwise overrides its playout delay (1–120s).", ""),
    N("retryNewManifestCount", "Retry new manifest count", "Events and timing", 1,0,100, "Internal DASH: additional fresh MPD fetches per failed manifest poll. 0 disables retries; every configured mirror still gets a chance.", ""),
};
#undef B
#undef S
#undef N

static const option_field *field_named(const char *name) {
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
        if (strcmp(fields[i].name, name) == 0) return &fields[i];
    return NULL;
}

static rs_json *default_value(const option_field *f) {
    return f->type == RS_JSON_BOOL ? rs_json_new_bool(false) :
           f->type == RS_JSON_STR ? rs_json_new_str("") : rs_json_new_num(f->value);
}

rs_json *rs_provider_options_schema(void) {
    rs_json *out = rs_json_new_arr();
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        const option_field *f = &fields[i];
        rs_json *v = rs_json_new_obj();
        rs_json_obj_set_str(v, "name", f->name);
        rs_json_obj_set_str(v, "label", f->label);
        rs_json_obj_set_str(v, "group", f->group);
        rs_json_obj_set_str(v, "type", f->type == RS_JSON_BOOL ? "checkbox" : f->type == RS_JSON_STR ? "text" : "number");
        rs_json_obj_set(v, "default", default_value(f));
        rs_json_obj_set_int(v, "min", f->value == 0 ? 0 : f->min);
        rs_json_obj_set_int(v, "max", f->max);
        rs_json_obj_set_str(v, "hint", f->hint);
        rs_json_obj_set_str(v, "inactive", f->inactive);
        rs_json_arr_push(out, v);
    }
    return out;
}

bool rs_provider_timezone_offset(const char *value, int *minutes) {
    const char *s = value ? value : "";
    if (!strncmp(s, "UTC", 3)) s += 3;
    if (!s[0] || !strcmp(s, "Z")) { *minutes = 0; return true; }
    if (*s != '+' && *s != '-') return false;
    int sign = *s++ == '-' ? -1 : 1;
    size_t n = strlen(s);
    if (n != 4 && n != 5) return false;
    size_t m = n == 5 ? 3 : 2;
    if (n == 5 && s[2] != ':') return false;
    if (!isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1]) ||
        !isdigit((unsigned char)s[m]) || !isdigit((unsigned char)s[m + 1])) return false;
    int hours = (s[0] - '0') * 10 + s[1] - '0';
    int mins = (s[m] - '0') * 10 + s[m + 1] - '0';
    if (hours > 14 || mins > 59 || (hours == 14 && mins)) return false;
    *minutes = sign * (hours * 60 + mins);
    return true;
}

bool rs_provider_options_valid(const rs_json *options, const char **err) {
    if (!options) return true;
    if (rs_json_type_of(options) != RS_JSON_OBJ) {
        *err = "Provider options must be a JSON object."; return false;
    }
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        const option_field *f = &fields[i];
        const rs_json *v = rs_json_obj_get(options, f->name);
        if (!v) continue;
        bool valid = rs_json_type_of(v) == f->type;
        if (valid && f->type == RS_JSON_NUM) {
            double n = rs_json_as_num(v, -1);
            valid = ((n >= f->min && n <= f->max) || (n == 0 && f->value == 0)) &&
                n == (double)(long long)n;
        } else if (valid && f->type == RS_JSON_STR) {
            const char *s = rs_json_as_str(v, "");
            valid = strlen(s) <= (size_t)f->max && !strpbrk(s, "\r\n");
            if (valid && !strcmp(f->name, "offAirFallbackUrl") && s[0] && strncmp(s, "http://", 7) && strncmp(s, "https://", 8)) {
                *err = "Off air fallback URL must begin with http:// or https://."; return false;
            }
            if (valid && !strcmp(f->name, "epgTimezone")) {
                int minutes;
                if (!rs_provider_timezone_offset(s, &minutes)) { *err = "EPG timezone must be UTC or a fixed offset such as UTC+05:00."; return false; }
            }
        }
        if (!valid) { *err = "Invalid provider option: check value types, whole-number ranges, and single-line text (maximum 4096 characters)."; return false; }
    }
    return true;
}

rs_json *rs_provider_options_merge(const rs_json *saved, const rs_json *patch) {
    rs_json *out = rs_json_new_obj();
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        const option_field *f = &fields[i];
        const rs_json *v = rs_json_obj_get(patch, f->name);
        if (!v) v = rs_json_obj_get(saved, f->name);
        rs_json_obj_set(out, f->name, v ? rs_json_clone(v) : default_value(f));
    }
    return out;
}

bool rs_provider_options_patch_valid(const rs_json *saved, const rs_json *patch, const char **err) {
    if (!rs_provider_options_valid(patch, err)) return false;
    rs_json *merged = rs_provider_options_merge(saved, patch);
    bool valid = !rs_json_obj_bool(merged, "offAirFallback", false) || rs_json_obj_str(merged, "offAirFallbackUrl", "")[0];
    if (!valid) *err = "Set an off air fallback MPD URL before enabling fallback.";
    rs_json_free(merged);
    return valid;
}

long long rs_provider_option_int(const rs_json *provider, const char *name) {
    const option_field *f = field_named(name);
    long long n = rs_json_obj_int(rs_json_obj_get(provider, "options"), name, f ? f->value : 0);
    if (f && !((n >= f->min && n <= f->max) || (n == 0 && f->value == 0))) return f->value;
    return n;
}
bool rs_provider_option_bool(const rs_json *provider, const char *name) {
    return rs_json_obj_bool(rs_json_obj_get(provider, "options"), name, false);
}
const char *rs_provider_option_str(const rs_json *provider, const char *name) {
    return rs_json_obj_str(rs_json_obj_get(provider, "options"), name, "");
}

static bool has_header(const char *headers, const char *name) {
    size_t n = strlen(name);
    for (const char *p = headers; p && *p;) {
        while (*p == ' ' || *p == '\t') p++;
        size_t i = 0;
        while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)name[i])) i++;
        if (i == n && p[i] == ':') return true;
        p = strchr(p, '\n');
        if (p) p++;
    }
    return false;
}

char *rs_provider_headers(const rs_json *provider) {
    const char *headers = rs_json_obj_str(provider, "headers", "");
    rs_buf out = RS_BUF_INIT;
    static const char *names[] = {"User-Agent", "X-Forwarded-For"};
    static const char *keys[] = {"userAgent", "xForwardedFor"};
    for (size_t i = 0; i < 2; i++) {
        const char *v = rs_provider_option_str(provider, keys[i]);
        if (v[0] && !has_header(headers, names[i])) {
            rs_buf_append_str(&out, names[i]); rs_buf_append_str(&out, ": ");
            rs_buf_append_str(&out, v); rs_buf_append_char(&out, '\n');
        }
    }
    rs_buf_append_str(&out, headers);
    char *result = rs_buf_take(&out);
    return result ? result : rs_strdup("");
}

int rs_provider_segment_seconds(const rs_json *provider, const rs_json *stream) {
    int n = (int)rs_provider_option_int(provider, "hlsFragmentDurationSeconds");
    return n > 0 ? n : (int)rs_json_obj_int(stream, "hlsSegmentSeconds", 10);
}
int rs_provider_playlist_segments(const rs_json *provider, const rs_json *stream) {
    int n = (int)rs_provider_option_int(provider, "outputFragmentsCount");
    if (n > 0) return n;
    int duration = (int)rs_provider_option_int(provider, "hlsPlaylistDurationSeconds");
    int segment = rs_provider_segment_seconds(provider, stream);
    if (duration > 0 && segment > 0) {
        n = (duration + segment - 1) / segment;
        return n < 3 ? 3 : n > 240 ? 240 : n;
    }
    return (int)rs_json_obj_int(stream, "playlistSegments", 6);
}

void rs_provider_source_policy(const rs_json *provider, rs_source_policy *out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->provider_id, sizeof(out->provider_id), "%s", rs_json_obj_str(provider, "id", ""));
    snprintf(out->cookie_file, sizeof(out->cookie_file), "runtime/sessions/%s/cookies.txt", out->provider_id);
    snprintf(out->video_filter, sizeof(out->video_filter), "%s", rs_provider_option_str(provider, "defaultVideo"));
    snprintf(out->audio_filter, sizeof(out->audio_filter), "%s", rs_provider_option_str(provider, "defaultAudio"));
    out->timeout_seconds = (int)rs_provider_option_int(provider, "httpGetTimeoutSeconds");
    out->attempts = (int)rs_provider_option_int(provider, "httpGetAttempts");
    out->max_downloads = (int)rs_provider_option_int(provider, "maxDownloadConcurrency");
    out->use_cookies = rs_provider_option_bool(provider, "useSessionCookies");
    out->detect_json_redirect = rs_provider_option_bool(provider, "detectJsonRedirect");
    out->legacy_dash = rs_provider_option_bool(provider, "legacyDashParser");
    out->no_restart_error = rs_provider_option_bool(provider, "noRestartOnError");
    out->restart_finished = rs_provider_option_bool(provider, "restartFinishedBroadcast");
    out->no_restart_track = rs_provider_option_bool(provider, "noRestartOnTrackChange");
    out->cooldown = rs_provider_option_bool(provider, "coolDownAutoRestart");
    out->restart_delay = (int)rs_provider_option_int(provider, "restartDelaySeconds");
    out->stalled_seconds = (int)rs_provider_option_int(provider, "stalledStreamTimeoutSeconds");
    out->manifest_retries = (int)rs_provider_option_int(provider, "retryNewManifestCount");
    out->ignore_static = rs_provider_option_bool(provider, "ignoreDashStaticFlag");
    out->use_dash_delay = rs_provider_option_bool(provider, "useDashDelay");
}
