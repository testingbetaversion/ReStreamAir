#ifndef RS_M3U8_H
#define RS_M3U8_H

// Parsing and rewriting for HLS playlist text, used by the panel's HLS proxy
// pipeline and by the probe endpoint. It knows nothing about the caller's
// routing scheme: callers supply a transform that turns a resolved URI into
// whatever path they want.
//
// A transform is a function pointer plus a userdata pointer, and returns a
// newly allocated string that this module takes ownership of, or NULL to leave
// the URI as it was.

#include "rs_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RS_M3U8_LINE_KEY,
    RS_M3U8_LINE_MAP,
    RS_M3U8_LINE_SEGMENT,
} rs_m3u8_line_kind;

// Returns the #EXT-X-MEDIA-SEQUENCE value, or 0 if the tag is absent.
int64_t rs_m3u8_media_sequence(const char *text);

bool rs_m3u8_is_master(const char *text);

// `seq` is the segment's media sequence number when the rewrite was asked to
// drop keys (the caller needs it to derive a default AES-128 IV), and -1 for
// key and map lines and whenever keys are not being dropped — the C spelling of
// an absent value.
typedef char *(*rs_m3u8_transform_fn)(void *userdata, const char *absolute_uri,
                                      rs_m3u8_line_kind kind, int64_t seq);

// Rewrites every key/map/segment URI in a media playlist. When drop_key is set,
// #EXT-X-KEY lines are removed (the server is decrypting, so the player must
// not try to as well). Returns a new string, freed with rs_free, or NULL on
// allocation failure.
char *rs_m3u8_rewrite(const char *text, const char *base_url, bool drop_key,
                      rs_m3u8_transform_fn transform, void *userdata);

// Limits which variants of a master playlist a player may choose, using the
// provider "Default video" rule list (comma-separated, first rule that keeps
// at least one variant wins): height<=N, bandwidth<=N, codec=X, best, worst.
// If no rule keeps anything, nothing is removed. I-frame variants follow the
// same rule. With `single`, only the highest-bandwidth variant left after the
// rules survives (I-frame variants are dropped). Returns a new string
// (rs_free), or NULL when `text` is not a master, there is nothing to do
// (empty filter and not single), or on allocation failure — use the original.
char *rs_m3u8_filter_master_video(const char *text, const char *filter, bool single);

typedef char *(*rs_m3u8_master_transform_fn)(void *userdata, const char *absolute_uri);

// Rewrites a master playlist's variant and audio-track URIs. When
// drop_session_key is set, EXT-X-SESSION-KEY tags are removed because the
// server is returning already-decrypted media.
char *rs_m3u8_rewrite_master(const char *text, const char *base_url, bool drop_session_key,
                             rs_m3u8_master_transform_fn transform, void *userdata);

// Absent numeric attributes are -1; absent strings are NULL.
typedef struct {
    char *uri;
    int64_t bandwidth;
    int64_t width;
    int64_t height;
    char *codecs;
} rs_m3u8_variant;

typedef struct {
    char *group_id;
    char *name;
    char *language;
    char *uri;
} rs_m3u8_audio_track;

typedef struct {
    rs_m3u8_variant *variants;
    size_t variant_count;
    rs_m3u8_audio_track *audio_tracks;
    size_t audio_count;
} rs_m3u8_probe;

// Lists variant streams and audio tracks from a master playlist, for the
// stream-editor probe UI. Returns 0 on success; the result is released with
// rs_m3u8_probe_dispose.
int rs_m3u8_probe_master(const char *text, const char *base_url, rs_m3u8_probe *out);
void rs_m3u8_probe_dispose(rs_m3u8_probe *probe);

// --- HLS sources for the internal engine ---------------------------------

// The renditions the engine follows from a master playlist: one video variant
// (the best the Default video rule allows) and its audio track (lang= rule,
// else DEFAULT=YES, else the first in the variant's AUDIO group). Ids are
// stable across polls even when URIs carry rotating tokens. audio_* are NULL
// when the audio is muxed into the video playlist.
typedef struct {
    char *video_uri, *video_id, *video_codecs;
    int64_t bandwidth, height;
    char *audio_uri, *audio_id, *audio_codecs, *audio_lang;
} rs_hls_pick;

int rs_hls_pick_renditions(const char *master, const char *base_url, const char *video_filter,
                           const char *audio_filter, rs_hls_pick *out);
void rs_hls_pick_dispose(rs_hls_pick *p);

typedef struct {
    char *url;          // absolute
    int64_t sequence;   // media sequence number — stable identity across polls
    double duration;    // seconds (EXTINF)
} rs_hls_segment;

typedef struct {
    char *init_url;        // EXT-X-MAP, absolute, or NULL
    rs_hls_segment *segments;
    size_t count;
    double target_duration;
    bool ended;            // EXT-X-ENDLIST
} rs_hls_media;

// Reads a media playlist into its segment window, keeping the newest `want`
// (0 = all). Refuses byte-range playlists and whole-segment AES-128 (the engine
// decrypts CENC fMP4 only), with the reason in err. 0 on success.
int rs_hls_media_parse(const char *text, const char *base_url, int want, rs_hls_media *out,
                       char *err, size_t err_len);
void rs_hls_media_dispose(rs_hls_media *m);

#ifdef __cplusplus
}
#endif

#endif  // RS_M3U8_H
