#ifndef RS_TTML_H
#define RS_TTML_H

// TTML (and its DASH `stpp` fMP4 carriage) converted to WebVTT.
//
// DASH publishes timed text either as plain `application/ttml+xml` segments or,
// far more often, as IMSC1/TTML documents wrapped one-per-sample in fMP4
// (`mimeType="application/mp4" codecs="stpp.ttml.im1t"`). Passing either of
// those through to an HLS player as-is is legal but only Safari and recent
// hls.js builds render it, so the live engine converts to WebVTT instead —
// every HLS player made in the last decade reads that.
//
// This module is deliberately libxml2-free: it lives in restream_base, which
// the Swift build and the self-test both link, and neither may drag in the
// server's XML dependency. TTML cue extraction needs a fraction of an XML
// parser (elements, attributes, entities, character data) and none of the rest
// (DTDs, namespaces as anything but a prefix to ignore, validation), so the
// scanner below is that fraction and nothing more.

#include "rs_common.h"

#ifdef __cplusplus
extern "C" {
#endif

// One subtitle cue, with times in seconds on whatever timeline the source
// document authored them against.
typedef struct {
    double begin;
    double end;
    char *text;  // WebVTT cue payload; may contain '\n', may be ""
} rs_ttml_cue;

// Parses a TTML document into cues, in document order.
//
// `data` is either a raw XML document or a whole fMP4 segment — when it looks
// like MP4, the `mdat` payloads are extracted first and each TTML document
// found inside is parsed in turn (a fragment can legally carry more than one
// sample). Cues with no resolvable begin time are skipped rather than guessed
// at.
//
// Returns 0 and fills *out / *out_count on success (which includes finding zero
// cues — an empty subtitle fragment is normal), negative when the input holds
// no TTML at all. Release with rs_ttml_cues_free.
int rs_ttml_parse(const uint8_t *data, size_t len, rs_ttml_cue **out, size_t *out_count);

void rs_ttml_cues_free(rs_ttml_cue *cues, size_t count);

// Renders cues as a WebVTT document.
//
// `shift` is added to every cue time, for sources that author each fragment
// from zero instead of on the presentation timeline. `map_seconds` is the
// segment's own start on the media timeline: when it is >= 0 the document gets
// an `X-TIMESTAMP-MAP` header naming that instant in both MPEG-TS 90 kHz ticks
// and local clock time, which is how an HLS player anchors a WebVTT segment
// against the video it is displayed over. Pass a negative value to omit it.
//
// Returns a NUL-terminated document (rs_free), or NULL on allocation failure.
char *rs_ttml_render_webvtt(const rs_ttml_cue *cues, size_t count,
                            double shift, double map_seconds);

// Parse plus render, with the fragment-relative correction applied.
//
// `segment_start` is the segment's start on the media timeline (from tfdt), and
// `segment_duration` its length; both in seconds. When the parsed cues all fall
// inside a window the length of one segment while `segment_start` is far past
// it, the document is taken to be fragment-relative and shifted onto the media
// timeline — packagers split on this and the document itself does not say which
// convention it used.
//
// Input that is already WebVTT is returned as-is, so a `text/vtt` DASH
// rendition passes through this same path untouched.
//
// Returns a NUL-terminated WebVTT document (rs_free), or NULL when the input
// held no timed text.
char *rs_ttml_to_webvtt(const uint8_t *data, size_t len,
                        double segment_start, double segment_duration);

#ifdef __cplusplus
}
#endif

#endif  // RS_TTML_H
