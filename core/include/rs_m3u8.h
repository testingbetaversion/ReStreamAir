#ifndef RS_M3U8_H
#define RS_M3U8_H

// Parsing and rewriting for HLS playlist text — the C port of
// M3U8Rewriter.swift, used by the panel's HLS proxy pipeline and by the probe
// endpoint. Like the Swift original it knows nothing about the caller's routing
// scheme: callers supply a transform that turns a resolved URI into whatever
// path they want.
//
// Swift's closures become function pointers plus a userdata pointer. A
// transform returns a newly allocated string that this module takes ownership
// of, or NULL to leave the URI as it was.

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
// the Int? the Swift version passes.
typedef char *(*rs_m3u8_transform_fn)(void *userdata, const char *absolute_uri,
                                      rs_m3u8_line_kind kind, int64_t seq);

// Rewrites every key/map/segment URI in a media playlist. When drop_key is set,
// #EXT-X-KEY lines are removed (the server is decrypting, so the player must
// not try to as well). Returns a new string, freed with rs_free, or NULL on
// allocation failure.
char *rs_m3u8_rewrite(const char *text, const char *base_url, bool drop_key,
                      rs_m3u8_transform_fn transform, void *userdata);

typedef char *(*rs_m3u8_master_transform_fn)(void *userdata, const char *absolute_uri);

// Rewrites a master playlist's variant and audio-track URIs; every other
// attribute passes through untouched.
char *rs_m3u8_rewrite_master(const char *text, const char *base_url,
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

#ifdef __cplusplus
}
#endif

#endif  // RS_M3U8_H
