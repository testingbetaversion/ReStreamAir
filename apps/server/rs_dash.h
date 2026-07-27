#ifndef RS_DASH_H
#define RS_DASH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One resolved media segment.
typedef struct {
    char *url;          // absolute, fully-resolved segment URL
    long long number;   // DASH segment number, or -1 if none
    long long time;     // $Time$ / tfdt value in `timescale` units, or -1
    double duration;    // seconds
} rs_dash_segment;

// A per-representation download plan expanded from one MPD poll. Mirrors the
// Swift DashSegments.expandSegments output for a single representation: the
// initialization URL plus the media-segment window (newest `count`, in timeline
// order), together with the stream-level fields the live worker needs to pace
// itself (dynamic flag, poll interval, buffer depth).
typedef struct {
    char *representation_id;   // the representation this plan is for
    char *adaptation_type;     // "video" | "audio" | "text"
    char *init_url;            // resolved initialization URL, or NULL
    unsigned long timescale;   // SegmentTemplate@timescale (1 if unset)

    rs_dash_segment *segments; // media segments, ascending timeline order
    size_t count;

    int dynamic;                    // 1 when MPD@type == "dynamic"
    double minimum_update_period;   // seconds, or 0 if unset
    double time_shift_buffer_depth; // seconds, or 0 if unset
} rs_dash_plan;

// Parses `mpd_xml` (`len` bytes), fetched from `mpd_url`, and builds a segment
// plan for representation `representation_id` (NULL selects the first video
// representation, or the first representation of any kind if there is no video
// one). At most `want` of the newest media segments are returned (0 = the whole
// in-manifest window). Returns 0 on success and fills *out (caller disposes with
// rs_dash_plan_dispose); on failure returns -1 with a message in errbuf.
int rs_dash_plan_build(const char *mpd_xml, size_t len, const char *mpd_url,
                       const char *representation_id, int want,
                       rs_dash_plan *out, char *errbuf, size_t errbuf_len);

void rs_dash_plan_dispose(rs_dash_plan *plan);

#ifdef __cplusplus
}
#endif

#endif  // RS_DASH_H
