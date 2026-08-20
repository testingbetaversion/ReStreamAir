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
// Expanded segment list for a single representation: the
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

// Fetches the MPD at `url` (through the given proxy / "Name: value" headers /
// downloader) and returns a malloc'd JSON description the C server turns into
// HLS playlists (caller frees with free), or NULL with a message in errbuf:
//
//   { "dynamic":bool, "mup":num, "tsb":num,
//     "video": {"id","codecs","bandwidth"} | null,   // default video rendition
//     "audio": {"id","codecs","lang"} | null,        // default audio rendition
//     "text":  {"id","codecs","lang","mime"} | null, // default subtitle rendition
//     "cc":    [ {"instreamId","lang"} ... ],        // in-band CEA-608/708 services
//     "plan":  {                                       // only when `rep` matched
//        "repId","type","timescale","initUrl",
//        "segments":[ {"url","time","duration"} ... ] } }
//
// "text" is a sidecar subtitle AdaptationSet (TTML/stpp or WebVTT), which the
// caller fetches as its own rendition. "cc" is captions embedded in the video
// elementary stream, which need no fetching at all — only the HLS tag that
// tells a player they are in there.
//
// Redirects are followed and segment URLs resolve against the final URL.
//
// `segment_url_params` (NULL/"" for none) is a raw query-string fragment
// appended to every returned init/segment URL. When `inherit_url_params` is
// non-zero, that fragment is instead taken from the query string of the URL
// that actually answered the fetch (the redirect target, when there was one)
// — needed for CDNs that sign a token only onto the 302 target, never onto
// the configured MPD URL itself.
char *rs_dash_describe(const char *url, const char *proxy, const char *headers,
                       const char *downloader, const char *dl_params,
                       const char *rep, int want,
                       const char *segment_url_params, int inherit_url_params,
                       char *errbuf, size_t errbuf_len);

#ifdef __cplusplus
}
#endif

#endif  // RS_DASH_H
