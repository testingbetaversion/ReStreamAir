#ifndef RS_MPEGTS_H
#define RS_MPEGTS_H

// Fragmented-MP4 -> MPEG-TS multiplexer.
//
// The live engine holds each rendition separately: video fMP4 fragments in one
// queue, audio in another, both already downloaded and CENC-decrypted. HLS hands
// those to the player as two renditions and lets the player do the muxing, which
// costs a playlist reload per segment per rendition plus a request per segment.
// A direct link has no playlist, so something has to interleave the two into one
// byte stream — that is this module. MPEG-TS is the container for it because it
// is designed to be joined mid-stream: no header at the front to have missed,
// PAT/PMT repeated inline, and every packet self-delimiting on a 188-byte grid.
//
// What it does, in order:
//   * reads the init segment for a track (timescale, codec, SPS/PPS or the
//     AudioSpecificConfig) — rs_ts_mux_add_track,
//   * parses each media fragment into samples with real DTS/PTS — rs_ts_mux_push,
//   * converts to elementary-stream framing (AVCC length prefixes to Annex B
//     start codes, raw AAC to ADTS), and
//   * emits PAT/PMT/PES packets in DTS order across both tracks — rs_ts_mux_take.
//
// Timestamps are rebased onto a common origin taken from the first sample seen,
// so a live tfdt counting from the epoch cannot overflow the 33-bit PTS field
// mid-session, and video and audio keep their relative offset (lip sync).
//
// Codecs: H.264, H.265 and AAC — what the DASH sources this serves actually
// carry. An unrecognised sample entry is refused by rs_ts_mux_add_track rather
// than muxed into something a player would fail on later.
//
// Threading: an rs_ts_mux is not internally locked. One owner at a time.

#include "rs_common.h"

#ifdef __cplusplus
extern "C" {
#endif

// The MPEG-TS packet grid. Output length is always a multiple of this.
#define RS_TS_PACKET_SIZE 188

// Video plus audio is the shape the live engine produces; the extra slots cost
// nothing and keep a multi-rendition caller from having to special-case.
#define RS_TS_MAX_TRACKS 4

typedef struct rs_ts_mux rs_ts_mux;

rs_ts_mux *rs_ts_mux_create(void);
void rs_ts_mux_destroy(rs_ts_mux *m);

// Registers a track from its fMP4 initialization segment. `kind` is "video" or
// "audio" (the live engine's own labelling); it only picks the default PID and
// PES stream id, since the real codec comes from the init segment. Returns a
// track handle to pass to rs_ts_mux_push, or negative when the init segment
// can't be parsed or carries a codec this muxer doesn't emit.
int rs_ts_mux_add_track(rs_ts_mux *m, const char *kind, const uint8_t *init, size_t init_len);

// Parses one media fragment (moof+mdat) and queues its samples. Returns the
// number of samples queued, or negative if the fragment couldn't be parsed.
// A fragment for a track_id the muxer doesn't hold is ignored, not an error.
int rs_ts_mux_push(rs_ts_mux *m, int track, const uint8_t *frag, size_t frag_len);

// Marks a track as having no more data coming, so interleaving stops waiting on
// it. Without this a stalled rendition would hold back the other one forever.
void rs_ts_mux_end_track(rs_ts_mux *m, int track);

// Hands over the TS bytes for every sample it is now safe to emit — that is,
// every queued sample whose DTS is old enough that no other track can still
// deliver something earlier. `flush` ignores that rule and drains everything,
// which is what a caller does when the stream is ending. NULL (with *out_len 0)
// when there is nothing to emit yet. Caller frees with rs_free.
uint8_t *rs_ts_mux_take(rs_ts_mux *m, bool flush, size_t *out_len);

// Offsets, within the buffer the last rs_ts_mux_take returned, at which a
// viewer can begin decoding: a PAT/PMT pair immediately followed by a video
// keyframe. Every keyframe gets one, because a viewer that joins mid-GOP has
// no parameter sets and no reference frame, and shows nothing until the next
// keyframe regardless — starting it at one is the difference between a picture
// now and a picture several seconds from now. Points into muxer-owned storage,
// valid until the next take. Returns the count; *out_offsets may be NULL.
size_t rs_ts_mux_join_points(const rs_ts_mux *m, const size_t **out_offsets);

// Samples queued but not yet emitted, across all tracks. For diagnostics and
// for a caller deciding whether a stalled track needs rs_ts_mux_end_track.
size_t rs_ts_mux_pending(const rs_ts_mux *m);

// True once a track has been registered and at least one sample emitted, i.e.
// the output so far is a joinable stream rather than an empty buffer.
bool rs_ts_mux_started(const rs_ts_mux *m);

// The MPEG-2 section CRC (poly 0x04C11DB7, init 0xFFFFFFFF, unreflected) PAT and
// PMT are stamped with. Exposed for the self-test's known-answer check.
uint32_t rs_ts_crc32(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif  // RS_MPEGTS_H
