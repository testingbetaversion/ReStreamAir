#ifndef RS_LIVE_H
#define RS_LIVE_H

// The live DASH -> HLS engine: the C port of LiveMPDToM3U8.swift.
//
// The first C implementation translated the MPD inside the request handler —
// every playlist reload re-fetched the manifest, re-expanded the window, and
// re-derived EXT-X-MEDIA-SEQUENCE from the segment's $Time$. That is not what
// the Swift worker does, and it breaks playback in three separate ways:
//
//   * MEDIA-SEQUENCE jumped. RFC 8216 4.3.3.2 requires consecutive segments to
//     differ by exactly 1; deriving it from $Time$/duration does not hold for a
//     SegmentTimeline with varying @d, so spec-literal clients (ffmpeg's hls
//     demuxer, hls.js) read each reload as "thousands of segments moved" and
//     reload/reseek forever instead of settling.
//   * No discontinuity detection. A source that splices (SCTE-35 ad breaks)
//     jumps the media timeline while the manifest keeps advertising a uniform
//     duration. Without EXT-X-DISCONTINUITY the browser ends up with a hole in
//     the MSE buffer it can never fill and sits at the gap forever.
//   * No state across reloads. Nothing remembered which segments had already
//     been served, so a shifted manifest window silently dropped or repeated
//     segments — and every reload paid the origin's full latency on the
//     server's single-threaded event loop, which is what made the panel's
//     Start/Stop buttons feel frozen.
//
// This module restores the Swift design. One engine per stream owns:
//
//   * a director thread that polls the MPD for the rendition list and renders
//     the HLS master playlist, and
//   * per representation, three roles: a POLLER that re-reads the manifest on a
//     strict cadence and queues whatever is new, a small pool of persistent
//     DOWNLOAD threads that fetch queued segments concurrently, and a WRITER
//     that CENC-decrypts and commits them strictly in queue order, keeps them
//     in a bounded in-memory queue, and renders the media playlist.
//
// Those three used to be one thread doing poll -> download -> publish in a
// loop, and that coupling was the single worst thing about the engine. A poll
// whose downloads took 135 seconds did not merely deliver late: it stopped the
// manifest being re-read for 135 seconds, by which time the source window had
// moved on entirely, so the next poll asked for a wider window, which took
// longer again. Splitting the roles is what both reference implementations do
// (streamlink's HLSStreamWorker/HLSStreamWriter, N_m3u8DL-RE's
// PlayListProduceAsync feeding a BufferBlock), and it is why they hold a live
// stream on a path this engine used to lose.
//
// Committing in queue order without waiting for each batch is the other half:
// segment N+3 can finish downloading long before N and simply waits its turn,
// so a slow segment never leaves the network idle. The queue is bounded, and
// that bound is the catch-up policy — see rs_live_catch_up_drop.
//
// Request handlers never touch the network: a playlist reload is a string
// render under a mutex, and a segment request is a memcpy out of the queue.
//
// Threading: every public function is safe to call from the server's event
// loop thread. Workers only take the per-stream lock to publish results, never
// while a fetch is in flight.

#include "rs_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rs_live rs_live;

// Same signature as restream_fetch_fn / restream_dash_fn (see restream.h). The
// engine holds the pointers rather than the implementations so the core never
// links libcurl or libxml2.
typedef int (*rs_live_fetch_fn)(const char *url, const char *proxy, const char *headers,
                                const char *range, const char *downloader, const char *dl_params,
                                char **out, size_t *out_len, long *status,
                                char **content_type, char **content_range,
                                char **effective_url, char *errbuf, size_t errbuf_len,
                                long timeout_ms, int (*should_cancel)(void *), void *cancel_ctx);

typedef char *(*rs_live_dash_fn)(const char *url, const char *proxy, const char *headers,
                                 const char *downloader, const char *dl_params,
                                 const char *rep, int want,
                                 const char *segment_url_params, int inherit_url_params,
                                 char *errbuf, size_t errbuf_len);

// Log sink. Called from worker threads, so the implementation must be
// thread-safe. `url`/`message` may be NULL; status 0 and bytes -1 mean absent.
typedef void (*rs_live_log_fn)(void *ctx, const char *stream_id, const char *level,
                               const char *event, const char *url, long status,
                               long long bytes, const char *message);

// A snapshot of one stream's playback settings. Strings are copied; NULL is
// treated as "". Mirrors the argv PanelServer.swift builds for the Swift worker.
typedef struct {
    const char *mpd_url;
    // Fallback sources carrying the same content on other CDNs, tried in order
    // when the primary manifest stops answering. The panel has always collected
    // these ("if the primary manifest fails, the stream fails over to these in
    // order") and the Swift worker rotated through them; the engine ignoring
    // them meant a stream whose origin refused simply stalled with a perfectly
    // good mirror configured and unused.
    const char *const *cdn_urls;
    size_t cdn_url_count;
    const char *representation;     // "" = auto-select video + audio renditions
    const char *manifest_proxy;
    const char *media_proxy;
    const char *manifest_headers;   // newline-separated "Name: value"
    const char *media_headers;
    const char *downloader;         // "curl" | "wget" | "aria2c" | "" (libcurl)
    const char *dl_params;
    const char *decryption_keys;    // "kid:key\nkid:key" ClearKey pairs
    const char *segment_url_params; // raw query-string fragment for every segment/init URL
    int inherit_url_params;         // non-zero: derive it from the MPD's redirect target instead
    // The director (rendition discovery) and each representation worker poll
    // the MPD independently — normally 3 simultaneous manifest fetches for one
    // video+audio stream. Some CDNs cap concurrent sessions per signed token
    // (e.g. a "cnt":3 claim) low enough that this 3-way overlap alone trips it.
    // Non-zero backs the director off to a slow keep-alive cadence once the
    // renditions are known, cutting steady-state concurrency to 2.
    int reduced_manifest_polling;
    int playlist_segments;          // advertised window (default 6)
    int keep_segments;              // segments held in memory (default 60)
    int download_ahead;             // segments requested per poll (default 16)
    int parallel_downloads;         // max concurrent segment fetches per stream (default 3)
    int prioritize_oldest;          // if non-zero, download the oldest segment sequentially first
    int playback_delay_seconds;     // extra live-edge hold-back, in seconds
    int audio_delay_ms;             // lip-sync shift, applied to audio only
    double poll_interval;           // 0 = follow the MPD's minimumUpdatePeriod
} rs_live_config;

// Segments asked for beyond the measured elapsed time, absorbing jitter in when
// the origin publishes.
#define RS_LIVE_WINDOW_MARGIN 3

// Ceiling on a single manifest request. The source window is finite anyway, and
// an unbounded ask after a long stall would try to pull the whole DVR buffer.
#define RS_LIVE_MAX_WINDOW 60

// How many segments one poll must request, given how long it has been since the
// previous request for the same representation (`since_last`, wall seconds; <=0
// on the first poll) and the representation's segment duration.
//
// The manifest request returns the NEWEST n segments, so n has to cover
// everything the source produced since the last request or the remainder is
// lost for good. Implemented in live_window.c; see there for the failure this
// exists to prevent. Overshooting is harmless — the caller already skips
// segments it holds.
int rs_live_window_size(int download_ahead, double since_last, double seg_duration);

// What to throw away so a representation's pending queue always holds the
// NEWEST segments it knows about, never more than `depth` of them.
//
// This is the catch-up policy, and it is the difference between a bad minute
// and a stream that never recovers. Fetching every segment the manifest ever
// showed sounds like the conservative choice; it is the opposite. The window a
// poll must request grows with how late the poll is (see rs_live_window_size),
// so a slow cycle makes the next request bigger, which makes it slower — the
// production symptom was a poll asking for 60 segments because it was 139s
// late, taking 135s, and asking for 60 again.
//
// Which end to drop from is the subtle part, and getting it backwards is worse
// than not dropping at all. Refusing the NEW segments because the queue is full
// of old ones pins the engine at a fixed distance behind the live edge forever:
// it keeps publishing forty-second-old media, keeps discarding everything
// current, and never recovers even when the path does. So old queued segments
// are evicted to make room for new ones, not the other way round.
//
//   `inflight`  items already being fetched, or fetched and awaiting commit.
//               Never dropped: the work is paid for, and a download thread
//               holds a pointer to its slot.
//   `waiting`   queued but not yet started. Droppable, oldest first.
//   `fresh`     what this poll just found. Droppable, oldest first.
//
// Both reference implementations amount to the same rule: streamlink's
// HLSStreamWorker.valid_segment ignores anything below its cursor and merely
// logs a sequence gap, and N_m3u8DL-RE only ever takes what the current
// manifest window holds. What is dropped leaves a tfdt jump behind, which
// surfaces honestly as an EXT-X-DISCONTINUITY.
void rs_live_catch_up_plan(int depth, int inflight, int waiting, int fresh,
                           int *evict_waiting, int *drop_fresh);

// Floor for the backoff after an origin explicitly refuses for rate reasons.
// Below this there is no point: the complaint is about how often we ask, and a
// poll period is exactly the quantity that is already too small.
#define RS_LIVE_THROTTLE_BACKOFF_MIN 15.0

// Attempts one manifest poll makes before giving up and backing off. Segment
// fetches have always retried (an abandoned segment is a permanent hole); the
// manifest did not, so one transient refusal failed the whole poll — and since
// 403 counts as a throttle, the engine then waited 15s+ and did it again.
// Against an origin behind a rotating-exit proxy, where a refusal is random
// rather than a real rate cap, that alone is enough to stall a stream
// indefinitely: at a 35% per-request failure rate, one attempt fails a poll 35%
// of the time, three attempts 4%. A genuine rate cap refuses every attempt, so
// the backoff still engages where it should.
#define RS_LIVE_MANIFEST_TRIES 3

// Ceiling on the backoff. A live stream that has been failing this long is
// broken, but it must still notice promptly when the origin comes back.
#define RS_LIVE_BACKOFF_MAX 60.0

// How long to wait before re-polling a manifest after `consecutive_failures`
// failed polls in a row (0 = the last one succeeded, so no backoff and the
// caller uses its normal period). `throttled` marks a response that explicitly
// refused for rate reasons; it raises the floor. Implemented in live_backoff.c;
// see there for the failure this exists to prevent.
double rs_live_backoff_delay(int consecutive_failures, int throttled, double period);

// Whether an HTTP status means "you are asking too often" rather than a plain
// error, and so should be backed off from harder.
int rs_live_status_is_throttle(long status);

rs_live *rs_live_create(rs_live_fetch_fn fetch, rs_live_dash_fn dash,
                        rs_live_log_fn log, void *log_ctx);
void rs_live_destroy(rs_live *live);

// Starts the engine for `stream_id`, or re-applies `cfg` to an already-running
// one (an edit in the panel takes effect on the next poll). Returns 0 on
// success, negative if the engine could not be started.
int rs_live_start(rs_live *live, const char *stream_id, const rs_live_config *cfg);

// Signals the engine to wind down. Returns immediately — threads notice the
// flag at their next checkpoint and are joined later by rs_live_reap, so a
// Stop click is never blocked behind an in-flight segment download.
void rs_live_stop(rs_live *live, const char *stream_id);

// True while an engine for `stream_id` exists and has not been asked to stop.
bool rs_live_is_running(rs_live *live, const char *stream_id);

// Joins and frees engines whose threads have exited. Cheap; call it from the
// server's one-second timer.
void rs_live_reap(rs_live *live);

// True once the master playlist exists AND every representation it references
// has a media playlist. Both are required: handing a player a master whose
// variant playlists 404 makes it give up on the whole stream rather than retry.
bool rs_live_is_ready(rs_live *live, const char *stream_id);

// There is deliberately no "wait until ready" call. The server is a
// single-threaded event loop, so blocking a request handler stops every other
// stream too — a stream whose origin never answers would freeze the whole
// server on every request for it. Callers answer 503 and let the player retry.

// The rendered HLS master playlist, or NULL until the engine is ready (see
// rs_live_is_ready). Caller frees with rs_free.
char *rs_live_master_playlist(rs_live *live, const char *stream_id);

// The rendered media playlist for one representation, or NULL when that
// representation has no advertisable window yet. Caller frees with rs_free.
char *rs_live_media_playlist(rs_live *live, const char *stream_id, const char *rep);

// Hands out a copy of an already-downloaded, already-decrypted segment, or the
// patched init segment when `want_init` is set. Addressed the way the playlist
// advertises it — representation slot plus media sequence — so the public URL
// never has to carry the origin address. NULL on a miss (the segment has aged
// out of the queue). Caller frees with rs_free; `*out_len` receives the length.
uint8_t *rs_live_take_indexed(rs_live *live, const char *stream_id, int rep_index,
                              long long seq, bool want_init, size_t *out_len);

// --- tailing a rendition ----------------------------------------------------
//
// The HLS routes address segments the way a playlist advertises them: the
// player reads a media playlist, learns the sequence numbers, and asks for one
// at a time. A direct link has no playlist to read — it opens one connection
// and expects bytes to keep arriving — so it needs the opposite shape: "what is
// the next thing after the one I already sent?" That is what the two calls
// below add, and they are the whole reason /direct can work off the in-memory
// queue instead of the on-disk one the Swift build tailed.

// Description of one of a stream's renditions. `index` is the same slot
// rs_live_take_indexed takes and the public URLs carry.
typedef struct {
    char *rep_id;        // the manifest's Representation@id
    char *kind;          // "video" | "audio"
    int index;
    uint32_t timescale;  // mdhd timescale of the media track, 0 until the init lands
    // The sequence range currently held. A viewer that wants the live edge
    // rather than the whole buffered window starts from `newest_seq` and works
    // back, instead of from -1 — the queue holds keepSegments of history, which
    // is a minute or more of latency to hand someone who just connected.
    long long oldest_seq, newest_seq;
    size_t held;         // segments in the queue; 0 means the range is meaningless
} rs_live_rep_desc;

// Enumerates a running stream's renditions, newest config first. Returns the
// count and points *out at a malloc'd array released with rs_live_reps_free;
// 0 (and *out NULL) when the stream isn't running or has no renditions yet.
size_t rs_live_reps(rs_live *live, const char *stream_id, rs_live_rep_desc **out);
void rs_live_reps_free(rs_live_rep_desc *reps, size_t count);

// Hands out a copy of the oldest held segment whose sequence is greater than
// `after_seq` — pass a negative value for "the oldest still held" — or NULL
// when the rendition has nothing newer yet. `*out_seq` receives the sequence
// actually taken, so the caller passes it back as `after_seq` next time;
// `*out_duration` (optional) the segment's duration in seconds. A caller that
// falls far enough behind for its next segment to be pruned skips the gap
// rather than stalling, which is the right failure for a live tail. Caller
// frees the bytes with rs_free; `*out_len` receives the length.
uint8_t *rs_live_take_after(rs_live *live, const char *stream_id, int rep_index,
                            long long after_seq, long long *out_seq,
                            double *out_duration, size_t *out_len);

// A one-line health summary for the logs/diagnostics ("video 12 segs, seq 431,
// 3 disc; audio 12 segs …"). Caller frees with rs_free, or NULL if unknown.
char *rs_live_status_line(rs_live *live, const char *stream_id);

#ifdef __cplusplus
}
#endif

#endif  // RS_LIVE_H
