// The live DASH -> HLS engine. See rs_live.h for what this is and why the
// request-time translation it replaces could not work.
//
// Layout of this file:
//   1. small helpers (string builder, percent-encoding, seen-URL set)
//   2. the data model (segment / representation / stream / manager)
//   3. the representation worker: poll -> parallel download -> decrypt ->
//      append -> prune, i.e. the port of LiveMPDToM3U8.downloadNewSegments
//   4. the director: rendition discovery + master playlist
//   5. playlist rendering (the port of LiveMPDToM3U8.writePlaylist)
//   6. the public API

#include "rs_live.h"

#include "rs_audio_delay.h"
#include "rs_cenc.h"
#include "rs_json.h"
#include "rs_ttml.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32

#include <pthread.h>

// Live engines tracked at once.
#define RS_LIVE_MAX_STREAMS 64
// Renditions per stream (video + audio + sidecar subtitles is the shape every
// source we handle uses; the extra slots leave room for a pinned
// representation alongside them).
#define RS_LIVE_MAX_REPS 6
// Active segment downloads across one stream. Video and audio share this
// budget: treating the value as a separate allowance for every rendition can
// turn a configured value of four into eight simultaneous origin requests.
// Several signed CDNs and proxies slow down sharply under that fan-out.
#define RS_LIVE_FETCH_PAR 3

// Ceiling on a representation's persistent download threads, whatever
// parallelDownloads is set to. Each thread keeps its own HTTP connection alive
// for the life of the stream (see the per-thread handle in the server's net.c),
// so the useful figure is "how many requests should be in flight at once", not
// "how many segments are outstanding" — and past a handful that stops buying
// throughput and starts costing congestion and CDN concurrency refusals.
// streamlink defaults to one download thread and keeps up fine.
#define RS_LIVE_MAX_DL_THREADS 3
// Hard ceiling on segments queued but not yet committed. The configured
// downloadAhead sets the working depth; this only bounds the allocation.
#define RS_LIVE_PENDING_MAX 128
// How often the writer reports throughput. Long enough to average over several
// segments, short enough to notice a stream going under within one player's
// buffer.
#define RS_LIVE_REPORT_INTERVAL 5.0
// The window "am I keeping up?" is judged over. Deliberately much longer than
// the report interval: real sources publish in bursts, and a source that emits
// six segments and then nothing for four seconds is perfectly healthy but reads
// as 0.0x realtime over any window short enough to sit inside the quiet part.
// Judging that over five seconds turned fallingBehind — which is supposed to
// mean "this stream is going to stall" — into noise.
#define RS_LIVE_SUSTAIN_INTERVAL 30.0
// How long the segment at the head of the queue may hold up everything behind
// it before it is abandoned. The writer commits in queue order, which is what
// keeps the timeline honest — but it also means one unanswerable request stops
// all publishing even when the next twenty segments are downloaded and waiting.
// Seen in production: an expired URL that the CDN simply never answered held a
// rendition at "+0 segments, 20 in flight" for a minute at a time, then dumped
// +19 at once. Scaled off the segment duration. Six times leaves enough room
// for a large video segment that is progressing through a slow proxy, while a
// request that is truly silent is still removed before it can age out a signed
// live URL.
#define RS_LIVE_HEAD_STALL_FACTOR 6.0
#define RS_LIVE_HEAD_STALL_MIN 12.0

// Attempts per segment before it is written off. A segment right at the live
// edge is routinely requested a moment before the origin has finished writing
// it; giving up on the first error dropped it permanently and tore a hole in
// the timeline that surfaced as an EXT-X-DISCONTINUITY.
//
// Two, not three. Every attempt can cost a full transfer timeout, and against a
// CDN that answers an expired URL with silence rather than a refusal that is
// the difference between a bounded 12-30 seconds per try and spending a minute
// and a half on it. A second attempt still covers the case this exists for —
// the live edge arriving a moment late — and nothing beyond that is worth the
// wall clock on a stream that has to stay in real time.
#define RS_LIVE_FETCH_TRIES 2
#define RS_LIVE_FETCH_RETRY_DELAY 0.4
#define RS_LIVE_MEDIA_TIMEOUT_MIN_MS 12000L
#define RS_LIVE_MEDIA_TIMEOUT_MAX_MS 30000L

// The window-sizing rule itself lives in live_window.c (portable, pure, and
// therefore covered by the self-test) — see rs_live_window_size.
// Upper bound on remembered segment URLs. This set is what stops a segment that
// has aged out of our queue — but is still inside the manifest's much larger
// timeShiftBufferDepth window — from being downloaded and appended a second
// time, which would inflate the media sequence without the window advancing.
// The Swift worker never prunes it; 8192 entries is over four hours at two
// second segments, so nothing the manifest can still advertise is ever
// forgotten, and the memory stays bounded in a long-lived server.
#define RS_LIVE_SEEN_MAX 8192
#define RS_LIVE_SEEN_BUCKETS 2048
// Per-representation memory ceiling for decrypted segments held in RAM. A
// backstop for the segment-count limit, for sources whose segments are far
// larger than the couple of megabytes this is sized around.
#define RS_LIVE_MAX_BYTES (64u * 1024u * 1024u)
// How often the director re-reads the rendition list. Representation ids
// effectively never change mid-stream, so this is deliberately much slower than
// the segment poll.
#define RS_LIVE_DIRECTOR_INTERVAL 15.0
// With reduced_manifest_polling on, the interval the director backs off to
// once it has already found the renditions — just often enough to notice a
// genuine rendition-list change, rarely enough to stop being a 3rd concurrent
// manifest fetch alongside the two representation workers.
#define RS_LIVE_DIRECTOR_IDLE_INTERVAL 300.0
// Base delay before the director re-reads a rendition list that failed. Doubles
// per consecutive failure (see live_backoff.c) rather than retrying flat, which
// is what let a rate-limited origin keep refusing indefinitely.
#define RS_LIVE_DIRECTOR_RETRY 3.0

// --- 1. helpers -------------------------------------------------------------

typedef struct {
    char *p;
    size_t len, cap;
} sbuf;

static void sb_init(sbuf *b) { b->p = NULL; b->len = 0; b->cap = 0; }

static bool sb_reserve(sbuf *b, size_t extra) {
    if (b->p && b->len + extra + 1 <= b->cap) return true;
    size_t want = b->cap ? b->cap : 512;
    while (want < b->len + extra + 1) want *= 2;
    char *np = (char *)realloc(b->p, want);
    if (!np) return false;
    b->p = np;
    b->cap = want;
    if (b->len == 0) b->p[0] = '\0';
    return true;
}

static void sb_add(sbuf *b, const char *s) {
    size_t n = strlen(s);
    if (!sb_reserve(b, n)) return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

// printf into the buffer. Sized from the formatted length rather than a fixed
// scratch array, because an encoded segment URL is routinely over a kilobyte.
static void sb_addf(sbuf *b, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0 && sb_reserve(b, (size_t)n)) {
        vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap2);
        b->len += (size_t)n;
    }
    va_end(ap2);
}

// Percent-encodes everything outside the RFC 3986 unreserved set, so a segment
// URL carrying its own '?', '&' or '=' cannot corrupt the query it is embedded
// in. Same rule as the server's query_encode.
static char *qenc(const char *s) {
    static const char hex[] = "0123456789ABCDEF";
    if (!s) s = "";
    size_t n = strlen(s);
    char *out = (char *)malloc(n * 3 + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0f];
        }
    }
    out[o] = '\0';
    return out;
}

// Appends a LANGUAGE attribute for an EXT-X-MEDIA line, or nothing when the
// manifest gave no language. The value comes from the MPD, so it is filtered to
// the RFC 5646 subset a tag can hold rather than interpolated as-is — an
// embedded quote would otherwise end the attribute early and corrupt every
// attribute after it on that line.
static void sb_add_lang(sbuf *b, const char *lang) {
    if (!lang || !lang[0]) return;
    char safe[32];
    size_t o = 0;
    for (size_t i = 0; lang[i] && o + 1 < sizeof(safe); i++) {
        char c = lang[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-')
            safe[o++] = c;
    }
    safe[o] = '\0';
    if (o) sb_addf(b, ",LANGUAGE=\"%s\"", safe);
}

static double now_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

// A segment's identity, stable across session-token rotation.
//
// Token-authenticated origins mint a fresh URL for the same segment on every
// manifest poll — the path carries a per-request token (…/bpk-token/1aa@<40
// chars>/…) and the CDN hostname rotates between mirrors as well. Keying "have
// I already downloaded this?" on the URL therefore makes the entire advertised
// window look new on every single poll: the queue fills with duplicates of the
// same media, and EXT-X-MEDIA-SEQUENCE (the append counter) races ahead of the
// real timeline by the width of the window each time. Players report that as
// "skipping N segments ahead, expired from playlists" and then stall with an
// empty buffer, having never been given two consecutive segments that agree.
//
// What does not rotate is the manifest's own $Time$ value and the segment
// filename, so the identity is those two together. `time` is -1 for the
// initialization segment, whose filename alone is already unique per
// representation.
static void stable_key(const char *url, long long time_val, char *out, size_t outlen) {
    if (!url) url = "";
    const char *q = strchr(url, '?');
    size_t path_len = q ? (size_t)(q - url) : strlen(url);
    const char *base = url;
    for (size_t i = 0; i < path_len; i++)
        if (url[i] == '/') base = url + i + 1;
    size_t base_len = path_len - (size_t)(base - url);
    // Keep the tail if the filename is implausibly long: that end carries the
    // per-segment number/timestamp, the head is the common prefix.
    if (base_len > 192) { base += base_len - 192; base_len = 192; }
    snprintf(out, outlen, "%lld|%.*s", time_val, (int)base_len, base);
}

// --- the seen-URL set -------------------------------------------------------

typedef struct seen_node {
    struct seen_node *next;
    char *url;
    uint32_t hash;
} seen_node;

typedef struct {
    seen_node *buckets[RS_LIVE_SEEN_BUCKETS];
    seen_node *order[RS_LIVE_SEEN_MAX];  // insertion order, for FIFO eviction
    size_t head, count;
} seen_set;

static uint32_t seen_hash(const char *s) {
    uint32_t h = 2166136261u;  // FNV-1a
    for (; *s; s++) {
        h ^= (uint32_t)(unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

static bool seen_has(const seen_set *s, const char *url) {
    uint32_t h = seen_hash(url);
    for (const seen_node *n = s->buckets[h % RS_LIVE_SEEN_BUCKETS]; n; n = n->next)
        if (n->hash == h && strcmp(n->url, url) == 0) return true;
    return false;
}

static void seen_unlink(seen_set *s, seen_node *victim) {
    seen_node **slot = &s->buckets[victim->hash % RS_LIVE_SEEN_BUCKETS];
    while (*slot) {
        if (*slot == victim) { *slot = victim->next; break; }
        slot = &(*slot)->next;
    }
    free(victim->url);
    free(victim);
}

static void seen_add(seen_set *s, const char *url) {
    if (seen_has(s, url)) return;
    seen_node *n = (seen_node *)calloc(1, sizeof(*n));
    if (!n) return;
    n->url = rs_strdup(url);
    if (!n->url) { free(n); return; }
    n->hash = seen_hash(url);
    size_t b = n->hash % RS_LIVE_SEEN_BUCKETS;
    n->next = s->buckets[b];
    s->buckets[b] = n;

    if (s->count == RS_LIVE_SEEN_MAX) {
        seen_node *old = s->order[s->head];
        s->order[s->head] = n;
        s->head = (s->head + 1) % RS_LIVE_SEEN_MAX;
        if (old) seen_unlink(s, old);
    } else {
        s->order[(s->head + s->count) % RS_LIVE_SEEN_MAX] = n;
        s->count++;
    }
}

static void seen_dispose(seen_set *s) {
    for (size_t i = 0; i < RS_LIVE_SEEN_BUCKETS; i++) {
        seen_node *n = s->buckets[i];
        while (n) {
            seen_node *next = n->next;
            free(n->url);
            free(n);
            n = next;
        }
        s->buckets[i] = NULL;
    }
    s->head = s->count = 0;
}

// --- 2. data model ----------------------------------------------------------

// One downloaded, decrypted, ready-to-serve segment.
typedef struct {
    char *url;             // upstream URL — the identity a /proxy request carries
    uint8_t *data;
    size_t len;
    double duration;       // seconds, from the manifest
    long long seq;         // monotonic; the source of EXT-X-MEDIA-SEQUENCE
    uint64_t start_time;   // tfdt baseMediaDecodeTime, in `timescale` units
    bool have_start;
    bool disc;             // needs an EXT-X-DISCONTINUITY before it
    double disc_gap;       // seconds the timeline jumped, for the log line
} live_seg;

struct live_stream;

// A segment on its way in. The poller appends one of these, a download thread
// fills in the bytes, and the writer commits them — see the comment on the
// pending queue below.
typedef enum {
    PEND_WAITING,   // queued, nobody has started fetching it
    PEND_FETCHING,  // a download thread owns it
    PEND_READY,     // bytes are in hand, waiting its turn to be committed
    PEND_FAILED,    // every attempt failed; commits as a hole in the timeline
} pend_state;

typedef struct {
    char *url;               // refreshed from every poll until the bytes arrive
    char idkey[288];         // token-independent identity (see stable_key)
    long long time_val;      // the manifest's $Time$, for the stable identity
    double duration;
    unsigned long plan_ts;   // manifest timescale, for anchoring text cues
    uint8_t *data;
    size_t len;
    int attempts;
    pend_state state;
    double started;          // when a download thread claimed it
    // Stamped when the slot is filled and re-stamped whenever it is reused. A
    // download thread captures it alongside the slot pointer and only writes
    // its result back if it still matches, so a slot can be abandoned out from
    // under an in-flight fetch (see the head-of-line rule in writer_main)
    // without the late result landing on whatever occupies the slot next.
    uint64_t gen;
    // The writer increments this to abort one slow network attempt without
    // abandoning the segment. The download thread then retries the same stable
    // segment identity using the URL most recently read from the MPD.
    uint64_t request_gen;
    char err[192];
} pend_item;

typedef struct {
    struct live_stream *owner;
    char *rep_id;
    char *kind;            // "video" | "audio" | "text"
    int index;             // slot in owner->reps, used as the public URL token

    pthread_t thread;
    bool thread_started;
    bool finished;

    // --- the pending queue --------------------------------------------------
    //
    // The engine used to do everything on this one thread: fetch the manifest,
    // download the whole batch, decrypt it, publish, sleep. That coupling is
    // what made a slow origin unrecoverable. A poll that took 135s did not just
    // deliver its segments late — it stopped the manifest being re-read for
    // 135s, by which time the origin's window had moved on entirely, so the
    // next poll asked for a wider window, which took longer still.
    //
    // Split three ways, the way streamlink and N_m3u8DL-RE both do it:
    //
    //   poller (this rep's `thread`)  re-reads the manifest on a strict cadence
    //                                 and appends new segments here. Never
    //                                 waits for a download.
    //   download threads              take the oldest WAITING item, fetch it,
    //                                 mark it READY. All in flight at once.
    //   writer                        waits for the item at the head to be
    //                                 READY, commits it, moves on.
    //
    // The writer consuming strictly in queue order is what keeps the timeline
    // correct without a barrier: segment N+3 can finish downloading long before
    // N, and simply waits its turn instead of holding the network idle. (This
    // is streamlink's trick of queueing the future rather than the result —
    // SegmentedStreamWriter.put in segmented/segmented.py.)
    //
    // The queue is bounded, and that bound is the whole catch-up policy: when
    // more new segments arrive than there is room for, the OLDEST are dropped
    // and the newest kept, so the engine snaps to the live edge instead of
    // trying to fetch a backlog it can never finish. Guarded by owner->mu,
    // signalled on owner->cv.
    pend_item *pending;
    size_t pend_head;      // index into `pending` of the next item to commit
    size_t pend_count;     // items live in the ring
    size_t pend_cap;
    uint64_t pend_gen;     // monotonic stamp source for pend_item.gen
    pthread_t *dl_threads;
    size_t ndl;
    int active_downloads;  // requests currently owned by this rendition
    pthread_t wr_thread;
    bool wr_started;
    bool poller_done;      // no more items will be queued; writer may drain+exit

    live_seg *segs;
    size_t nsegs, cap;
    size_t bytes;          // total held by `segs`, for the memory ceiling

    long long total_queued;    // every segment ever appended — see writePlaylist
    long long dropped_disc;    // discontinuities that rolled off the queue
    size_t pruned_total;       // pruned since the writer's last report

    char *init_url;        // the URL the held init was actually fetched from
    char *init_key;        // its token-independent identity (see stable_key)
    uint8_t *init_data;
    size_t init_len;

    uint32_t timescale;    // mdhd timescale of the media track
    uint8_t key[16];
    bool have_key;
    int iv_size;

    seen_set seen;
    char *playlist;        // last rendered media playlist, or NULL
    bool ready;            // a playlist has been rendered at least once
    double last_poll;
    long long polls;

    // Wall clock at which the previous manifest window was requested. The gap
    // to the next request is exactly how much media the source produced in the
    // meantime, which is what the window has to be wide enough to cover.
    double window_anchor;
    double seg_duration;       // last observed segment duration, for sizing
    long long skipped;         // segments the window moved past, never fetched

    // Consecutive failed manifest polls, and whether the last one was an
    // explicit rate refusal. Drives the backoff in rep_main; reset to 0 by the
    // first poll that retrieves a window. Only touched by this rep's own
    // worker thread.
    int manifest_failures;
    bool manifest_throttled;
    long long highest_time_val;
} live_rep;

typedef struct live_stream {
    struct rs_live *mgr;
    char *id;

    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool stop;

    // Config, replaced wholesale by rs_live_start. Guarded by `mu`.
    char *mpd_url, *representation;
    // [primary] + cdnUrls. `source` is the one currently in use; it advances
    // when a manifest poll exhausts its retries, and stays there — a mirror
    // that answers is as good as the primary, and flapping back to a source
    // that just failed would only re-pay the failure every poll.
    char **sources;
    size_t nsources;
    size_t source;
    char *manifest_proxy, *media_proxy;
    char *manifest_headers, *media_headers;
    char *downloader, *dl_params, *keys;
    char *segment_url_params;
    int inherit_url_params;
    int reduced_manifest_polling;
    int playlist_segments, keep_segments, download_ahead;
    int parallel_downloads;
    // Effective stream-wide request budget captured when this engine instance
    // was created. Each rendition has enough waiting workers to use the budget,
    // but active_downloads below prevents the pools multiplying it.
    int worker_parallel_downloads;
    int active_downloads;  // active media requests across every rendition
    int prioritize_oldest;
    int playback_delay_seconds, audio_delay_ms;
    double poll_interval;

    pthread_t dir_thread;
    bool dir_started, dir_finished;

    char *master;          // rendered master playlist, or NULL until ready
    live_rep *reps[RS_LIVE_MAX_REPS];
    size_t nreps;
} live_stream;

struct rs_live {
    rs_live_fetch_fn fetch;
    rs_live_dash_fn dash;
    rs_live_log_fn log;
    void *log_ctx;
    pthread_mutex_t mu;                 // guards the stream table only
    live_stream *streams[RS_LIVE_MAX_STREAMS];
    size_t nstreams;
};

// A worker's private copy of the config, so a panel edit mid-poll cannot free
// strings out from under an in-flight fetch.
typedef struct {
    char *mpd_url, *manifest_proxy, *media_proxy;
    // [primary] + cdnUrls, in the order they are tried. Always at least one.
    char **sources;
    size_t nsources;
    char *manifest_headers, *media_headers;
    char *downloader, *dl_params, *keys;
    char *segment_url_params;
    int inherit_url_params;
    int reduced_manifest_polling;
    int playlist_segments, keep_segments, download_ahead;
    int parallel_downloads;
    int prioritize_oldest;
    int playback_delay_seconds, audio_delay_ms;
    double poll_interval;
} cfg_snap;

static void cfg_snap_dispose(cfg_snap *c) {
    free(c->mpd_url); free(c->manifest_proxy); free(c->media_proxy);
    free(c->manifest_headers); free(c->media_headers);
    free(c->downloader); free(c->dl_params); free(c->keys);
    free(c->segment_url_params);
    for (size_t i = 0; i < c->nsources; i++) free(c->sources[i]);
    free(c->sources);
    memset(c, 0, sizeof(*c));
}

// Caller must hold st->mu.
static void cfg_snapshot_locked(const live_stream *st, cfg_snap *out) {
    memset(out, 0, sizeof(*out));
    out->mpd_url = rs_strdup(st->mpd_url ? st->mpd_url : "");
    // The primary always leads; the mirrors follow in configured order.
    out->sources = (char **)calloc(st->nsources ? st->nsources : 1, sizeof(char *));
    if (out->sources) {
        for (size_t i = 0; i < st->nsources; i++)
            out->sources[out->nsources++] = rs_strdup(st->sources[i] ? st->sources[i] : "");
        if (out->nsources == 0) {
            out->sources[0] = rs_strdup(out->mpd_url);
            out->nsources = 1;
        }
    }
    out->manifest_proxy = rs_strdup(st->manifest_proxy ? st->manifest_proxy : "");
    out->media_proxy = rs_strdup(st->media_proxy ? st->media_proxy : "");
    out->manifest_headers = rs_strdup(st->manifest_headers ? st->manifest_headers : "");
    out->media_headers = rs_strdup(st->media_headers ? st->media_headers : "");
    out->downloader = rs_strdup(st->downloader ? st->downloader : "");
    out->dl_params = rs_strdup(st->dl_params ? st->dl_params : "");
    out->keys = rs_strdup(st->keys ? st->keys : "");
    out->segment_url_params = rs_strdup(st->segment_url_params ? st->segment_url_params : "");
    out->inherit_url_params = st->inherit_url_params;
    out->reduced_manifest_polling = st->reduced_manifest_polling;
    out->playlist_segments = st->playlist_segments;
    out->keep_segments = st->keep_segments;
    out->download_ahead = st->download_ahead;
    out->parallel_downloads = st->parallel_downloads;
    out->prioritize_oldest = st->prioritize_oldest;
    out->playback_delay_seconds = st->playback_delay_seconds;
    out->audio_delay_ms = st->audio_delay_ms;
    out->poll_interval = st->poll_interval;
}

// --- logging ----------------------------------------------------------------

static void lg(live_stream *st, const char *level, const char *event, const char *url,
               long status, long long bytes, const char *message) {
    if (st && st->mgr && st->mgr->log)
        st->mgr->log(st->mgr->log_ctx, st->id, level, event, url, status, bytes, message);
}

static void lgf(live_stream *st, const char *level, const char *event, const char *url,
                long status, long long bytes, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    lg(st, level, event, url, status, bytes, msg);
}

// Sleeps up to `seconds`, waking early if the stream is asked to stop. Returns
// false when the worker should exit. This is why Stop is instant: no worker
// ever sits in an uninterruptible sleep.
static bool live_wait(live_stream *st, double seconds) {
    if (seconds < 0) seconds = 0;
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    long whole = (long)seconds;
    long nsec = (long)((seconds - (double)whole) * 1e9);
    deadline.tv_sec += (time_t)whole;
    deadline.tv_nsec += nsec;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&st->mu);
    while (!st->stop) {
        if (pthread_cond_timedwait(&st->cv, &st->mu, &deadline) == ETIMEDOUT) break;
    }
    bool go = !st->stop;
    pthread_mutex_unlock(&st->mu);
    return go;
}

static bool live_stopping(live_stream *st) {
    pthread_mutex_lock(&st->mu);
    bool s = st->stop;
    pthread_mutex_unlock(&st->mu);
    return s;
}

// --- 3. the representation worker ------------------------------------------

// The writer publishes as soon as a segment lands, so it needs the renderer and
// the pruner, both of which belong with the rest of the playlist code further
// down. Declared here rather than moved, so section 5 stays one thing.
static char *rep_render_locked(live_rep *rep, const cfg_snap *cfg);
static void rep_prune_locked(live_rep *rep, int keep);

// --- the pending queue ------------------------------------------------------

// Ring accessors. All of these run with owner->mu held.

static pend_item *pend_at_locked(live_rep *rep, size_t i) {
    return &rep->pending[(rep->pend_head + i) % rep->pend_cap];
}

// Appends one segment. Caller has already checked there is room.
static void pend_push_locked(live_rep *rep, const char *url, const char *idkey,
                             long long time_val, double duration, unsigned long plan_ts) {
    pend_item *it = pend_at_locked(rep, rep->pend_count);
    memset(it, 0, sizeof(*it));
    it->url = rs_strdup(url);
    snprintf(it->idkey, sizeof(it->idkey), "%s", idkey);
    it->time_val = time_val;
    it->duration = duration;
    it->plan_ts = plan_ts;
    it->state = PEND_WAITING;
    it->gen = ++rep->pend_gen;
    rep->pend_count++;
}

// The queued item with this identity, whatever state it is in.
static pend_item *pend_find_locked(live_rep *rep, const char *idkey) {
    for (size_t i = 0; i < rep->pend_count; i++) {
        pend_item *it = pend_at_locked(rep, i);
        if (strcmp(it->idkey, idkey) == 0) return it;
    }
    return NULL;
}

static void pend_item_dispose(pend_item *it) {
    free(it->url);
    free(it->data);
    memset(it, 0, sizeof(*it));
}

// Drops every queued item, for teardown.
static void pend_clear_locked(live_rep *rep) {
    for (size_t i = 0; i < rep->pend_count; i++) pend_item_dispose(pend_at_locked(rep, i));
    rep->pend_count = 0;
    rep->pend_head = 0;
}

// Throws away the oldest `evict` segments that are still WAITING, to make room
// for newer ones. Items already being fetched are left alone: the work is
// already paid for, and a download thread holds a pointer to its slot for the
// duration of the fetch — moving one would invalidate that pointer.
//
// Downloads always claim the oldest WAITING item, so everything in progress is
// a prefix of the ring and the WAITING items are a contiguous suffix. Only
// those get shifted, which is exactly the set no thread holds a pointer into.
static size_t pend_evict_waiting_locked(live_rep *rep, size_t evict) {
    if (evict == 0 || rep->pend_count == 0) return 0;
    size_t first = 0;
    while (first < rep->pend_count && pend_at_locked(rep, first)->state != PEND_WAITING) first++;
    size_t avail = rep->pend_count - first;
    if (evict > avail) evict = avail;
    if (evict == 0) return 0;
    for (size_t i = 0; i < evict; i++) {
        // Mark seen on the way out. Without this the very next poll finds it
        // in the manifest again, queues it again, and the engine spends its
        // whole budget re-fetching what it just decided to skip.
        pend_item *it = pend_at_locked(rep, first + i);
        seen_add(&rep->seen, it->idkey);
        pend_item_dispose(it);
    }
    for (size_t i = first + evict; i < rep->pend_count; i++)
        *pend_at_locked(rep, i - evict) = *pend_at_locked(rep, i);
    rep->pend_count -= evict;
    return evict;
}

// --- download threads -------------------------------------------------------

// Takes the oldest item nobody has started yet. Oldest-first matters: the head
// of the queue is what the writer is blocked on, so fetching in queue order is
// what keeps segments committing steadily rather than in bursts. (This also
// subsumes the old prioritizeOldest option, which used to buy the same ordering
// by running one fetch synchronously ahead of the parallel batch — a whole
// extra round trip per poll on exactly the slow paths that needed it least.)
static long pend_claim_locked(live_rep *rep) {
    for (size_t i = 0; i < rep->pend_count; i++) {
        pend_item *it = pend_at_locked(rep, i);
        if (it->state == PEND_WAITING) {
            it->state = PEND_FETCHING;
            return (long)i;
        }
    }
    return -1;
}

typedef struct {
    live_stream *st;
    pend_item *slot;
    uint64_t slot_gen;
    uint64_t request_gen;
} download_cancel_ctx;

// Called by libcurl's progress hook. It turns a logical queue cancellation into
// a real network cancellation, so a dead CDN request no longer owns a worker
// connection until the request timeout expires.
static int download_should_cancel(void *opaque) {
    download_cancel_ctx *ctx = (download_cancel_ctx *)opaque;
    pthread_mutex_lock(&ctx->st->mu);
    bool cancel = ctx->st->stop || ctx->slot->gen != ctx->slot_gen ||
                  ctx->slot->state != PEND_FETCHING ||
                  ctx->slot->request_gen != ctx->request_gen;
    pthread_mutex_unlock(&ctx->st->mu);
    return cancel ? 1 : 0;
}

static long media_timeout_ms(double duration) {
    long timeout = duration > 0 ? (long)(duration * 6000.0) : RS_LIVE_MEDIA_TIMEOUT_MIN_MS;
    if (timeout < RS_LIVE_MEDIA_TIMEOUT_MIN_MS) timeout = RS_LIVE_MEDIA_TIMEOUT_MIN_MS;
    if (timeout > RS_LIVE_MEDIA_TIMEOUT_MAX_MS) timeout = RS_LIVE_MEDIA_TIMEOUT_MAX_MS;
    return timeout;
}

static int effective_parallel_downloads(int configured) {
    int want = configured > 0 ? configured : RS_LIVE_FETCH_PAR;
    if (want > RS_LIVE_MAX_DL_THREADS) want = RS_LIVE_MAX_DL_THREADS;
    if (want < 1) want = 1;
    return want;
}

// Keep one slot available to audio when both media kinds are present. Without
// this split, the video queue (normally much deeper and slower) can repeatedly
// win every stream-wide slot and starve audio. Text shares the non-video side
// of the global budget and is uncommon enough not to need another reservation.
// Caller holds st->mu.
static int rep_active_limit_locked(const live_rep *rep) {
    const live_stream *st = rep->owner;
    int budget = st->worker_parallel_downloads;
    bool have_audio = false;
    for (size_t i = 0; i < st->nreps; i++) {
        if (st->reps[i]->kind && strcmp(st->reps[i]->kind, "audio") == 0) {
            have_audio = true;
            break;
        }
    }
    if (budget > 1 && have_audio) {
        if (rep->kind && strcmp(rep->kind, "video") == 0) return budget - 1;
        return 1;
    }
    return budget;
}

// Caller holds st->mu.
static bool pend_has_waiting_locked(live_rep *rep) {
    for (size_t i = 0; i < rep->pend_count; i++)
        if (pend_at_locked(rep, i)->state == PEND_WAITING) return true;
    return false;
}

static void *download_main(void *arg) {
    live_rep *rep = (live_rep *)arg;
    live_stream *st = rep->owner;

    for (;;) {
        pthread_mutex_lock(&st->mu);
        long idx = -1;
        while (!st->stop) {
            if (st->active_downloads < st->worker_parallel_downloads &&
                rep->active_downloads < rep_active_limit_locked(rep))
                idx = pend_claim_locked(rep);
            if (idx >= 0) break;
            if (rep->poller_done && !pend_has_waiting_locked(rep)) {
                pthread_mutex_unlock(&st->mu);
                return NULL;
            }
            pthread_cond_wait(&st->cv, &st->mu);
        }
        if (st->stop) { pthread_mutex_unlock(&st->mu); return NULL; }
        st->active_downloads++;
        rep->active_downloads++;
        // The claimed item cannot move while we work: the ring only ever grows
        // at the tail and shrinks at the head, and the writer will not advance
        // past an item that is still FETCHING.
        pend_item *slot = pend_at_locked(rep, (size_t)idx);
        slot->started = now_seconds();
        uint64_t slot_gen = slot->gen;
        cfg_snap cfg;
        cfg_snapshot_locked(st, &cfg);
        pthread_mutex_unlock(&st->mu);

        uint8_t *data = NULL;
        size_t len = 0;
        int attempts = 0;
        char err[192] = {0};
        bool ok = false;

        // Retry before writing the segment off. Every abandoned segment becomes
        // a permanent hole in the media timeline, so a single transient 404 at
        // the live edge used to cost a real EXT-X-DISCONTINUITY.
        for (int attempt = 0; attempt < RS_LIVE_FETCH_TRIES; attempt++) {
            if (attempt > 0 && !live_wait(st, RS_LIVE_FETCH_RETRY_DELAY)) break;

            // Re-read the URL for every attempt. Segment identity is based on
            // $Time$/path, while the query string is a short-lived signature;
            // the poller can therefore replace this URL safely while the item
            // remains queued. In particular, attempt two must not repeat the
            // expired URL that made attempt one time out.
            pthread_mutex_lock(&st->mu);
            if (st->stop || slot->gen != slot_gen || slot->state != PEND_FETCHING) {
                pthread_mutex_unlock(&st->mu);
                break;
            }
            char *url = rs_strdup(slot->url);
            uint64_t request_gen = slot->request_gen;
            slot->attempts = attempt + 1;
            slot->started = now_seconds();
            double duration = slot->duration;
            pthread_mutex_unlock(&st->mu);
            if (!url) {
                snprintf(err, sizeof(err), "out of memory copying the current segment URL");
                break;
            }

            char *body = NULL;
            size_t blen = 0;
            long status = 0;
            err[0] = '\0';
            download_cancel_ctx cancel = {st, slot, slot_gen, request_gen};
            int rc = st->mgr->fetch(url, cfg.media_proxy, cfg.media_headers, NULL,
                                    cfg.downloader, cfg.dl_params,
                                    &body, &blen, &status, NULL, NULL, NULL, err, sizeof(err),
                                    media_timeout_ms(duration), download_should_cancel, &cancel);
            free(url);
            attempts = attempt + 1;
            if (rc == 0 && body) {
                data = (uint8_t *)body;
                len = blen;
                ok = true;
                break;
            }
            if (!err[0]) snprintf(err, sizeof(err), "fetch failed (status %ld)", status);
            free(body);
        }
        cfg_snap_dispose(&cfg);

        pthread_mutex_lock(&st->mu);
        if (st->active_downloads > 0) st->active_downloads--;
        if (rep->active_downloads > 0) rep->active_downloads--;
        // The slot may no longer be ours: a stop can have cleared the queue,
        // and the writer can have abandoned this item for holding up everything
        // behind it. The generation stamp is what makes that safe to check —
        // the slot address alone would still match after the slot was reused.
        pend_item *dst = NULL;
        for (size_t i = 0; i < rep->pend_count; i++) {
            pend_item *c = pend_at_locked(rep, i);
            if (c == slot && c->gen == slot_gen && c->state == PEND_FETCHING) { dst = c; break; }
        }
        if (dst) {
            dst->data = data;
            dst->len = len;
            dst->attempts = attempts;
            snprintf(dst->err, sizeof(dst->err), "%s", err);
            dst->state = ok ? PEND_READY : PEND_FAILED;
        } else {
            free(data);
        }
        pthread_cond_broadcast(&st->cv);
        pthread_mutex_unlock(&st->mu);
    }
}

// A segment continues the previous one only if it starts, on the media
// timeline, where the previous one ended. Sources that splice (SCTE-35 ad
// insertion) jump the timeline while the manifest goes on advertising a uniform
// duration, and the tfdt is the only signal that reveals it. Getting this wrong
// is what makes playback "stick" in a browser: the playlist claims the segment
// is 2.000s, the media actually resumes a second later, and MSE is left with a
// hole it can never fill. ffmpeg silently re-offsets instead, which is why the
// same stream plays from the CLI and stalls in the page.
// Signed distance, in seconds, between where `prev` ends on the media timeline
// and where the next segment starts. Zero when either side has no usable tfdt,
// which is indistinguishable from "perfectly contiguous" and is deliberately
// treated as such — see the comment on start==0 in rep_poll.
static double timeline_gap(const live_rep *rep, const live_seg *prev, uint64_t next_start,
                           bool next_has_start) {
    if (!prev || !prev->have_start || !next_has_start) return 0;
    if (rep->timescale == 0) return 0;
    double expected = (double)prev->start_time / (double)rep->timescale + prev->duration;
    double actual = (double)next_start / (double)rep->timescale;
    return actual - expected;
}

// Encoder jitter (AAC frame alignment) is single-digit milliseconds; a real
// splice — or a segment we failed to fetch — moves the timeline by about a
// second or more.
#define RS_LIVE_SPLICE_THRESHOLD 0.25

// Drops the oldest segments until the queue fits both the segment count and the
// memory ceiling. Caller holds st->mu.
static void rep_prune_locked(live_rep *rep, int keep) {
    if (keep < 1) keep = 1;
    while (rep->nsegs > (size_t)keep || (rep->bytes > RS_LIVE_MAX_BYTES && rep->nsegs > 1)) {
        live_seg *g = &rep->segs[0];
        // A discontinuity that rolls off the queue still has to be counted, or
        // EXT-X-DISCONTINUITY-SEQUENCE drifts and players lose track of which
        // timeline they are on across reloads.
        if (g->disc) rep->dropped_disc++;
        rep->bytes -= g->len;
        free(g->url);
        free(g->data);
        memmove(rep->segs, rep->segs + 1, (rep->nsegs - 1) * sizeof(live_seg));
        rep->nsegs--;
    }
}

// Caller holds st->mu. Takes ownership of `data`.
static void rep_append_locked(live_rep *rep, const char *url, uint8_t *data, size_t len,
                              double duration, uint64_t start, bool have_start) {
    if (rep->nsegs == rep->cap) {
        size_t ncap = rep->cap ? rep->cap * 2 : 32;
        live_seg *ns = (live_seg *)realloc(rep->segs, ncap * sizeof(live_seg));
        if (!ns) { free(data); return; }
        rep->segs = ns;
        rep->cap = ncap;
    }
    const live_seg *prev = rep->nsegs > 0 ? &rep->segs[rep->nsegs - 1] : NULL;
    live_seg *s = &rep->segs[rep->nsegs++];
    memset(s, 0, sizeof(*s));
    s->url = rs_strdup(url);
    s->data = data;
    s->len = len;
    s->duration = duration;
    s->start_time = start;
    s->have_start = have_start;
    s->disc_gap = timeline_gap(rep, prev, start, have_start);
    s->disc = s->disc_gap > RS_LIVE_SPLICE_THRESHOLD || s->disc_gap < -RS_LIVE_SPLICE_THRESHOLD;
    s->seq = rep->total_queued++;
    rep->bytes += len;
    // Segment durations drive the window sizing in rep_poll. Track the newest
    // rather than assuming 2s, since sources vary (and vary per rendition).
    if (duration > 0) rep->seg_duration = duration;
}

// Fetches and installs the initialization segment: patch it to a clear codec,
// pick the ClearKey that matches its default KID, and read the media timescale
// the discontinuity check needs.
static void rep_load_init(live_rep *rep, const char *init_url, const cfg_snap *cfg,
                          unsigned long plan_timescale) {
    live_stream *st = rep->owner;
    char *body = NULL;
    size_t len = 0;
    long status = 0;
    char err[192] = {0};
    lg(st, "info", "initFetch", init_url, 0, -1, rep->rep_id);
    int rc = st->mgr->fetch(init_url, cfg->media_proxy, cfg->media_headers, NULL,
                            cfg->downloader, cfg->dl_params,
                            &body, &len, &status, NULL, NULL, NULL, err, sizeof(err),
                            30000, NULL, NULL);
    if (rc != 0 || !body) {
        free(body);
        lgf(st, "error", "initFetch", init_url, status, -1, "%s: %s", rep->rep_id,
            err[0] ? err : "init segment fetch failed");
        return;
    }

    size_t out_len = 0;
    char *kid_hex = NULL;
    int iv_size = 0;
    uint8_t *patched = rs_cenc_patch_init((uint8_t *)body, len, &out_len, &kid_hex, &iv_size);
    if (patched) { free(body); } else { patched = (uint8_t *)body; out_len = len; }

    uint32_t ts = rs_audio_mdhd_timescale(patched, out_len);
    if (ts == 0) ts = (uint32_t)(plan_timescale ? plan_timescale : 1);

    // Pick the key whose KID the init actually declares, rather than assuming
    // the first configured pair — a stream can carry several tracks with
    // different KIDs, and decrypting with the wrong one yields noise. When the
    // init names a KID and none of the configured keys match it, that used to
    // fall back to the first configured key anyway — silently decrypting with
    // the wrong key (logged as "key yes") instead of leaving the segment
    // alone and saying so. `key_mismatch` now distinguishes that case so it
    // shows up as an error instead of looking like it worked.
    uint8_t key[16];
    bool have_key = false;
    bool key_mismatch = false;
    rs_cenc_keys keys = rs_cenc_parse_keys(cfg->keys ? cfg->keys : "");
    if (keys.count > 0) {
        size_t pick = 0;
        bool matched = false;
        if (kid_hex) {
            for (size_t i = 0; i < keys.count; i++) {
                if (keys.kids[i] && strcmp(keys.kids[i], kid_hex) == 0) { pick = i; matched = true; break; }
            }
        }
        if (matched || !kid_hex || keys.count == 1) {
            memcpy(key, keys.keys[pick], 16);
            have_key = true;
        } else {
            key_mismatch = true;
        }
    }
    rs_cenc_keys_free(&keys);

    char idkey[288];
    stable_key(init_url, -1, idkey, sizeof(idkey));

    pthread_mutex_lock(&st->mu);
    free(rep->init_url);
    free(rep->init_key);
    free(rep->init_data);
    rep->init_url = rs_strdup(init_url);
    rep->init_key = rs_strdup(idkey);
    rep->init_data = patched;
    rep->init_len = out_len;
    rep->timescale = ts;
    rep->iv_size = iv_size > 0 ? iv_size : 8;
    rep->have_key = have_key;
    if (have_key) memcpy(rep->key, key, 16);
    pthread_mutex_unlock(&st->mu);

    lgf(st, key_mismatch ? "error" : "info", "initReady", init_url, status, (long long)out_len,
        "%s: timescale %u, iv %d, key %s%s%s%s", rep->rep_id, ts, rep->iv_size,
        have_key ? "yes" : "no", kid_hex ? ", kid " : "", kid_hex ? kid_hex : "",
        key_mismatch ? " — none of the configured keys match this KID; segments will stay encrypted" : "");
    free(kid_hex);
}

// One poll of one representation: read the manifest window, download whatever
// is new, decrypt it, and append it in timeline order.
// The dash callback reports failures as a message only — its signature has no
// status out-param, and widening it would reach into restream.h, the server's
// describe implementation and the Windows stub for one integer. But the message
// is ours: both fetch paths in the server's net.c render an HTTP refusal as
// "Upstream returned HTTP <n>.", so recover the code from there. A message that
// does not carry one (a timeout, a parse failure) reads back as 0, which is
// simply "failed, but not a rate refusal" — the generic backoff still applies.
static long status_from_error(const char *msg) {
    if (!msg) return 0;
    const char *p = strstr(msg, "HTTP ");
    if (!p) return 0;
    p += 5;
    long code = 0;
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') code = code * 10 + (*p++ - '0');
    return code;
}

// Fetches and expands the manifest, retrying a refusal before writing the poll
// off and rotating to a CDN mirror when the current source keeps failing.
//
// This is the one place both the director and the representation workers read
// the MPD, so the retry and the failover apply to every manifest read in the
// engine. `rep_id` is "" for the director (rendition discovery); `want` is
// ignored there. Returns malloc'd JSON (caller frees) or NULL once every
// attempt has failed, with `err` describing the last failure.
static char *manifest_fetch(live_stream *st, const cfg_snap *cfg, const char *rep_id,
                            int want, char *err, size_t errlen) {
    if (cfg->nsources == 0) return NULL;

    pthread_mutex_lock(&st->mu);
    size_t idx = st->source < cfg->nsources ? st->source : 0;
    pthread_mutex_unlock(&st->mu);

    // Enough attempts to retry a flaky source and still give every mirror a
    // turn, so a dead primary cannot hide a working mirror behind the retries.
    size_t tries = RS_LIVE_MANIFEST_TRIES;
    if (cfg->nsources > tries) tries = cfg->nsources;

    for (size_t attempt = 0; attempt < tries; attempt++) {
        if (attempt > 0 && !live_wait(st, RS_LIVE_FETCH_RETRY_DELAY)) return NULL;
        const char *url = cfg->sources[idx];
        err[0] = '\0';
        char *json = st->mgr->dash(url, cfg->manifest_proxy, cfg->manifest_headers,
                                   cfg->downloader, cfg->dl_params, rep_id, want,
                                   cfg->segment_url_params, cfg->inherit_url_params,
                                   err, errlen);
        if (json) {
            // Stick to whatever answered, and say so when it is not the primary.
            pthread_mutex_lock(&st->mu);
            bool moved = st->source != idx;
            st->source = idx;
            pthread_mutex_unlock(&st->mu);
            if (moved)
                lgf(st, "info", "cdnFailover", url, 0, -1,
                    "source %lu of %lu answered — staying on it",
                    (unsigned long)(idx + 1), (unsigned long)cfg->nsources);
            if (attempt > 0)
                lgf(st, "info", "manifestRetry", url, 0, -1,
                    "%s%smanifest recovered on attempt %lu",
                    rep_id && rep_id[0] ? rep_id : "renditions",
                    rep_id && rep_id[0] ? ": " : ": ", (unsigned long)(attempt + 1));
            return json;
        }
        // Move to the next mirror for the following attempt. With a single
        // source this is a plain retry, which is the case that matters most:
        // a refusal there is usually transient, not a verdict.
        if (cfg->nsources > 1) idx = (idx + 1) % cfg->nsources;
    }
    return NULL;
}

// Records the outcome of a manifest poll for the backoff in rep_main.
static void rep_note_manifest(live_rep *rep, bool ok, const char *err) {
    if (ok) {
        rep->manifest_failures = 0;
        rep->manifest_throttled = false;
        return;
    }
    if (rep->manifest_failures < 1000) rep->manifest_failures++;
    rep->manifest_throttled = rs_live_status_is_throttle(status_from_error(err)) != 0;
}

static void rep_poll(live_rep *rep, const cfg_snap *cfg, double *out_next_interval) {
    live_stream *st = rep->owner;
    char err[256] = {0};
    double t0 = now_seconds();

    // The manifest request is anchored at the live edge: it returns the NEWEST
    // `want` segments. So `want` has to cover everything the source produced
    // since the previous request, and a fixed download_ahead only ever covers
    // download_ahead * segment_duration seconds. Whenever a cycle took longer
    // than that — a slow origin, large segments, a proxy in the path — every
    // segment older than the window was never requested, and the next one to
    // arrive landed a few seconds further down the media timeline. That is what
    // put a permanent EXT-X-DISCONTINUITY every few segments into the output
    // and left the engine publishing well under 1x realtime, so any player
    // drained its buffer and stalled no matter how good the bytes were.
    //
    // Widening the ask is free: `seen` skips anything already held, so the only
    // cost of overshooting is a slightly larger manifest response.
    int want = cfg->download_ahead;
    pthread_mutex_lock(&st->mu);
    double anchor = rep->window_anchor;
    double seg_dur = rep->seg_duration > 0 ? rep->seg_duration : 2.0;
    pthread_mutex_unlock(&st->mu);
    // NB: the anchor only advances once a window has actually been retrieved
    // (below, after the plan is in hand). Advancing it here would mean a failed
    // manifest fetch silently reset the clock, so the next poll would ask for a
    // window sized to that short interval and lose the backlog the failure
    // created — the very hole this sizing exists to prevent.

    // Wall time since the previous request == media the source produced in the
    // meantime. Zero on the first poll, which deliberately starts at the live
    // edge with just download_ahead rather than pulling the whole DVR window.
    double since_last = anchor > 0 ? t0 - anchor : 0;
    want = rs_live_window_size(cfg->download_ahead, since_last, seg_dur);

    // First poll: take a short run-up at the live edge rather than the whole
    // downloadAhead window. Asking for twenty segments before a single byte has
    // been fetched hands the download threads twenty seconds of backlog on a
    // path that manages roughly one segment a second, so the stream opens
    // already behind and spends its first minute clawing back. Enough to render
    // a playlist and no more; the window widens by itself from the next poll,
    // which is sized off elapsed time. streamlink does the same thing with
    // --hls-live-edge, defaulting to three segments.
    if (anchor <= 0) {
        int start = cfg->playlist_segments > 0 ? cfg->playlist_segments + 2 : 8;
        if (start < 4) start = 4;
        if (want > start) want = start;
    }

    lgf(st, "info", "pollStart", cfg->mpd_url, 0, -1,
        "%s: requesting %d segments (%.1fs since last poll, %.3fs each)",
        rep->rep_id, want, since_last, seg_dur);
    char *json = manifest_fetch(st, cfg, rep->rep_id, want, err, sizeof(err));
    if (!json) {
        rep_note_manifest(rep, false, err);
        lgf(st, "error", "manifest", cfg->mpd_url, 0, -1, "%s: %s", rep->rep_id,
            err[0] ? err : "could not read the MPD");
        return;
    }
    rs_json *root = rs_json_parse(json, strlen(json));
    free(json);
    if (!root) {
        rep_note_manifest(rep, false, NULL);
        lgf(st, "error", "manifest", cfg->mpd_url, 0, -1, "%s: malformed DASH description", rep->rep_id);
        return;
    }

    double mup = rs_json_as_num(rs_json_obj_get(root, "mup"), 0);
    if (mup > 0 && cfg->poll_interval <= 0) *out_next_interval = mup;

    const rs_json *plan = rs_json_obj_get(root, "plan");
    if (!plan || rs_json_type_of(plan) != RS_JSON_OBJ) {
        rep_note_manifest(rep, false, NULL);
        lgf(st, "error", "manifest", cfg->mpd_url, 0, -1,
            "%s: representation not present in the MPD", rep->rep_id);
        rs_json_free(root);
        return;
    }
    rep_note_manifest(rep, true, NULL);

    // A window is genuinely in hand now, so this poll becomes the reference
    // point the next one measures its backlog against.
    pthread_mutex_lock(&st->mu);
    rep->window_anchor = t0;
    pthread_mutex_unlock(&st->mu);

    unsigned long plan_ts = (unsigned long)rs_json_as_num(rs_json_obj_get(plan, "timescale"), 1);
    const char *init_url = rs_json_obj_str(plan, "initUrl", "");
    const rs_json *segs = rs_json_obj_get(plan, "segments");
    size_t total = rs_json_arr_len(segs);

    lgf(st, "info", "manifest", cfg->mpd_url, 0, -1,
        "%s: %lu segments in the manifest window (%.2fs)", rep->rep_id,
        (unsigned long)total, now_seconds() - t0);

    // The init segment is fetched once, and again only if the source genuinely
    // rotates it — compared by stable identity, so a re-tokenized URL for the
    // same init does not trigger a pointless refetch every poll.
    char init_key[288];
    stable_key(init_url, -1, init_key, sizeof(init_key));
    bool need_init;
    pthread_mutex_lock(&st->mu);
    need_init = init_url[0] && (!rep->init_key || strcmp(rep->init_key, init_key) != 0);
    pthread_mutex_unlock(&st->mu);
    if (need_init) rep_load_init(rep, init_url, cfg, plan_ts);
    if (live_stopping(st)) { rs_json_free(root); return; }

    // Collect the segments this poll has not already served, in timeline order.
    typedef struct { const char *url; long long tv; double dur; char idkey[288]; } cand;
    cand *found = (cand *)calloc(total ? total : 1, sizeof(cand));
    if (!found) { rs_json_free(root); return; }
    size_t n = 0;
    unsigned long refreshed = 0;
    // Which manifest entries correspond to something already queued, so the
    // sweep below can tell "still offered by the origin" from "gone".
    bool *still_advertised = (bool *)calloc(total ? total : 1, sizeof(bool));
    if (!still_advertised) { free(found); rs_json_free(root); return; }
    pthread_mutex_lock(&st->mu);
    for (size_t i = 0; i < total; i++) {
        const rs_json *sg = rs_json_arr_at(segs, i);
        const char *u = rs_json_obj_str(sg, "url", "");
        if (!u[0]) continue;
        long long tv = (long long)rs_json_as_num(rs_json_obj_get(sg, "time"), -1);
        char idkey[288];
        stable_key(u, tv, idkey, sizeof(idkey));
        if (seen_has(&rep->seen, idkey)) continue;
        // Already queued? Then this is not new — but the URL almost certainly
        // is. Claro (and every CDN like it) signs each segment URL with a
        // short-lived token; the ones in this manifest are valid for about a
        // minute. Freezing the URL at queue time and fetching it later meant
        // requesting links that had expired 37-81 seconds earlier, which the
        // CDN does not refuse cleanly — it just never answers, so the fetch
        // burned its whole timeout and blocked everything behind it. Take the
        // fresh URL every poll for anything whose bytes are not already in
        // hand. FETCHING is safe too: the request owns a private URL copy, and
        // a retry will snapshot this replacement.
        pend_item *queued_already = pend_find_locked(rep, idkey);
        if (queued_already) {
            if ((queued_already->state == PEND_WAITING ||
                 queued_already->state == PEND_FETCHING) &&
                strcmp(queued_already->url, u) != 0) {
                free(queued_already->url);
                queued_already->url = rs_strdup(u);
                refreshed++;
            }
            still_advertised[i] = true;
            continue;
        }
        if (tv >= 0) {
            // A stale manifest from a rotating proxy might return segments from the past.
            // If they jump backward by a small amount, ignore them so the timeline stays clean.
            if (rep->highest_time_val >= 0 && tv < rep->highest_time_val) {
                // If it jumped backward by an absurd amount (e.g. > 1 day), it's probably
                // an origin reboot rather than a stale proxy, so we let it through.
                // Otherwise, skip it.
                long long diff = rep->highest_time_val - tv;
                // Timescales are usually around 90000 or 10000000. 10^12 handles large timescales.
                if (diff < 1000000000000LL) continue;
            }
            if (tv > rep->highest_time_val) rep->highest_time_val = tv;
        }
        found[n].url = u;
        found[n].tv = tv;
        found[n].dur = rs_json_as_num(rs_json_obj_get(sg, "duration"), 0);
        memcpy(found[n].idkey, idkey, sizeof(idkey));
        n++;
    }

    // How far behind the live edge this representation is allowed to work.
    // downloadAhead already means "how many segments to keep fetched ahead of
    // the advertised playlist", so it is the right knob: the queue may hold
    // that much and no more.
    size_t depth = (size_t)(cfg->download_ahead > 0 ? cfg->download_ahead : 8);
    if (depth < 4) depth = 4;
    if (depth > rep->pend_cap) depth = rep->pend_cap;

    // THE catch-up rule. If more is new than there is room for, the engine is
    // behind, and the one thing it must not do is try to fetch its way out:
    // asking for a bigger window next time is what turned a slow minute into a
    // spiral that never converged (a 139s poll asking for 60 segments, taking
    // 135s, asking for 60 again). Drop the OLDEST and keep the newest, so the
    // next thing published is at the live edge.
    //
    // This is what both reference implementations do — streamlink's
    // valid_segment() simply ignores anything below its cursor and logs a
    // sequence gap; N_m3u8DL-RE only ever takes what is in the current window.
    // The dropped segments are marked seen so the engine never comes back for
    // them, and the tfdt jump they leave behind surfaces honestly as an
    // EXT-X-DISCONTINUITY rather than as silent corruption.
    // Anything still WAITING that the origin no longer advertises is
    // unreachable, so drop it now. An item already FETCHING is different: its
    // worker owns a private URL and may still complete even after the manifest
    // slides. Cancelling it here caused slow requests to be killed just before
    // completion and turned ordinary lag into a permanent discontinuity. The
    // request timeout and the writer's head-of-line rule already bound it.
    size_t stale = 0;
    if (total > 0) {
        for (size_t i = 0; i < rep->pend_count;) {
            pend_item *it = pend_at_locked(rep, i);
            bool advertised = false;
            for (size_t j = 0; j < total && !advertised; j++)
                if (still_advertised[j]) {
                    const rs_json *sg = rs_json_arr_at(segs, j);
                    const char *u = rs_json_obj_str(sg, "url", "");
                    long long tv = (long long)rs_json_as_num(rs_json_obj_get(sg, "time"), -1);
                    char k[288];
                    stable_key(u, tv, k, sizeof(k));
                    if (strcmp(k, it->idkey) == 0) advertised = true;
                }
            if (!advertised) {
                if (it->state == PEND_WAITING) {
                    seen_add(&rep->seen, it->idkey);
                    pend_item_dispose(it);
                    for (size_t m = i + 1; m < rep->pend_count; m++)
                        *pend_at_locked(rep, m - 1) = *pend_at_locked(rep, m);
                    rep->pend_count--;
                    stale++;
                    continue;
                }
            }
            i++;
        }
    }

    size_t inflight = 0;
    for (size_t i = 0; i < rep->pend_count; i++)
        if (pend_at_locked(rep, i)->state != PEND_WAITING) inflight++;
    int evict_n = 0, drop_n = 0;
    rs_live_catch_up_plan((int)depth, (int)inflight, (int)(rep->pend_count - inflight), (int)n,
                          &evict_n, &drop_n);
    size_t evicted = pend_evict_waiting_locked(rep, (size_t)(evict_n > 0 ? evict_n : 0));
    size_t dropped = (size_t)(drop_n > 0 ? drop_n : 0);
    if (dropped > n) dropped = n;
    // Everything discarded is marked seen, so the engine never comes back for
    // it — the point is to be at the live edge, not to retry history.
    for (size_t i = 0; i < dropped; i++) seen_add(&rep->seen, found[i].idkey);
    rep->skipped += (long long)(dropped + evicted + stale);
    unsigned long queued = 0;
    for (size_t i = dropped; i < n; i++) {
        // Deliberately NOT marked seen here. A queued segment has to stay
        // visible to later polls so its signed URL can be refreshed; the writer
        // marks it seen at the moment it leaves the queue, under the same lock,
        // so it is never both un-queued and un-seen.
        pend_push_locked(rep, found[i].url, found[i].idkey, found[i].tv, found[i].dur, plan_ts);
        queued++;
    }
    size_t queue_depth = rep->pend_count;
    if (queued || evicted || stale) pthread_cond_broadcast(&st->cv);
    pthread_mutex_unlock(&st->mu);
    free(found);
    free(still_advertised);
    rs_json_free(root);

    if (dropped || evicted || stale)
        lgf(st, "error", "liveEdgeSkip", NULL, 0, -1,
            "%s: skipped %lu segments (%.1fs) to stay at the live edge — %lu never queued, "
            "%lu evicted from a full queue, %lu dropped out of the source's window. Queue is "
            "%lu/%lu deep. This rendition is arriving slower than it plays; a smaller "
            "downloadAhead keeps the delay down, a lower bitrate is the real fix",
            rep->rep_id, (unsigned long)(dropped + evicted + stale),
            (double)(dropped + evicted + stale) * seg_dur,
            (unsigned long)dropped, (unsigned long)evicted, (unsigned long)stale,
            (unsigned long)rep->pend_count, (unsigned long)depth);

    if (queued == 0 && dropped == 0 && evicted == 0 && stale == 0) {
        lgf(st, "info", "pollIdle", NULL, 0, -1, "%s: no new segments this poll", rep->rep_id);
        return;
    }
    lgf(st, "info", "pollQueued", NULL, 0, -1,
        "%s: queued %lu segments, refreshed %lu stale URLs (%lu in flight, depth %lu) in %.2fs",
        rep->rep_id, queued, refreshed, (unsigned long)queue_depth, (unsigned long)depth,
        now_seconds() - t0);
}

// --- the writer -------------------------------------------------------------

// Commits one downloaded segment: decrypt, read where it sits on the media
// timeline, decide whether it continues the previous one, and append. Runs on
// the writer thread only, strictly in queue order, so none of this depends on
// the order the network happened to deliver things in.
//
// Takes ownership of it->data.
static void commit_one(live_rep *rep, const cfg_snap *cfg, pend_item *it,
                       double seg_dur, size_t *failed, size_t *added,
                       long long *added_bytes, double *added_seconds,
                       bool *prev_failed) {
    live_stream *st = rep->owner;

    if (it->state == PEND_FAILED) {
        (*failed)++;
        *prev_failed = true;
        // A single miss at the live edge is routine — the origin may simply
        // never produce this one. The window slides past it.
        if (it->attempts > 0)
            lgf(st, "error", "downloadSegment", it->url, 0, -1,
                "%s: gave up after %d attempt%s: %s", rep->rep_id, it->attempts,
                it->attempts == 1 ? "" : "s", it->err);
        else
            lgf(st, "error", "downloadSegment", it->url, 0, -1,
                "%s: %s", rep->rep_id, it->err);
        return;
    }

    pthread_mutex_lock(&st->mu);
    bool have_key = rep->have_key;
    int iv_size = rep->iv_size > 0 ? rep->iv_size : 8;
    uint8_t key[16];
    if (have_key) memcpy(key, rep->key, 16);
    uint32_t ts = rep->timescale;
    pthread_mutex_unlock(&st->mu);

    uint8_t *data = it->data;
    size_t len = it->len;
    it->data = NULL;  // ownership moves here
    if (have_key) {
        size_t out_len = 0;
        uint8_t *clear = rs_cenc_decrypt_segment(data, len, &out_len, key, iv_size);
        if (clear) { free(data); data = clear; len = out_len; }
        else lgf(st, "error", "decryptSegment", it->url, 0, -1,
                 "%s: CENC decrypt failed, serving as-is", rep->rep_id);
    }

    // Lip-sync offset, audio track only — the same knob the Swift worker
    // applies to whichever worker owns the audio representation.
    if (cfg->audio_delay_ms != 0 && ts > 0 && strcmp(rep->kind, "audio") == 0) {
        int64_t delta = (int64_t)cfg->audio_delay_ms * (int64_t)ts / 1000;
        size_t out_len = 0;
        uint8_t *shifted = rs_audio_shift_segment(data, len, delta, &out_len);
        if (shifted) { free(data); data = shifted; len = out_len; }
    }

    // tfdt baseMediaDecodeTime: where this segment actually starts on the
    // media timeline. A missing box reads back as 0, which is
    // indistinguishable from a genuine zero start, so 0 is treated as
    // "unknown" throughout — timeline_gap then reports no jump
    // rather than reporting a jump. Missing one real splice is recoverable;
    // a spurious EXT-X-DISCONTINUITY on every segment is not.
    uint64_t start = rs_audio_base_decode_time(data, len);
    bool have_start = start != 0;

    // Timed text becomes WebVTT here, once, rather than on every
    // request: TTML — plain or wrapped in stpp fMP4 — is legal to hand
    // an HLS player directly but only Safari and recent hls.js render
    // it, while every player made this century reads WebVTT. The cues
    // have to be anchored to the media timeline, so the segment's own
    // start comes from its tfdt where there is one and from the
    // manifest's $Time$ otherwise (a bare TTML or .vtt segment has no
    // fMP4 boxes to read a tfdt out of).
    if (strcmp(rep->kind, "text") == 0) {
        double sstart = -1;
        if (have_start && ts > 0) sstart = (double)start / (double)ts;
        else if (it->time_val >= 0 && it->plan_ts > 0)
            sstart = (double)it->time_val / (double)it->plan_ts;
        char *vtt = rs_ttml_to_webvtt(data, len, sstart, it->duration > 0 ? it->duration : 2.0);
        if (vtt) {
            free(data);
            data = (uint8_t *)vtt;
            len = strlen(vtt);
        } else {
            // Not timed text we recognise. Serving the original bytes
            // under a .vtt name would only give the player something it
            // cannot parse, so drop the segment and say so.
            lgf(st, "error", "subtitleConvert", it->url, 0, (long long)len,
                "%s: segment is not TTML or WebVTT — skipped", rep->rep_id);
            free(data);
            (*failed)++;
            return;
        }
    }

    pthread_mutex_lock(&st->mu);
    bool was_disc = false;
    double gap = 0;
    rep_append_locked(rep, it->url, data, len,
                      it->duration > 0 ? it->duration : 2.0, start, have_start);
    if (rep->nsegs > 0) {
        was_disc = rep->segs[rep->nsegs - 1].disc;
        gap = rep->segs[rep->nsegs - 1].disc_gap;
    }
    // Only a forward jump means missing content. A backward one is the
    // source rewinding its own timeline, which loses us nothing.
    if (was_disc && gap > 0 && seg_dur > 0)
        rep->skipped += (long long)(gap / seg_dur + 0.5);
    pthread_mutex_unlock(&st->mu);

    (*added)++;
    *added_bytes += (long long)len;
    *added_seconds += it->duration > 0 ? it->duration : 2.0;
    if (it->attempts > 1)
        lgf(st, "info", "downloadSegment", it->url, 200, (long long)len,
            "%s: %.3fs (recovered on attempt %d)", rep->rep_id, it->duration, it->attempts);
    else
        lgf(st, "info", "downloadSegment", it->url, 200, (long long)len,
            "%s: %.3fs%s", rep->rep_id, it->duration, was_disc ? " (discontinuity)" : "");
    // Say which kind of break this is. A forward jump of a whole number of
    // segments after a failed download is content we lost; anything else is
    // the source splicing its own timeline. They look identical in the
    // playlist, so without this the log cannot tell you which you have.
    if (was_disc)
        lgf(st, *prev_failed ? "error" : "info", "discontinuity", it->url, 0, -1,
            "%s: media timeline jumped %+.3fs (~%.1f segments)%s", rep->rep_id, gap,
            seg_dur > 0 ? gap / seg_dur : 0,
            *prev_failed ? " — after a failed download, so this is content we dropped"
                         : " — no download failed, so the source spliced");
    *prev_failed = false;
}

// Drains the pending queue in order and publishes. One per representation.
static void *writer_main(void *arg) {
    live_rep *rep = (live_rep *)arg;
    live_stream *st = rep->owner;

    // Throughput accounting over a reporting window, not over a poll: the
    // number that decides whether playback survives is media published per
    // second of wall clock, and now that publishing is not tied to polling
    // there is no poll to measure it against.
    double report_anchor = now_seconds();
    // Whether the segment committed immediately before this one failed — the
    // only thing that distinguishes "we dropped content" from "the source
    // spliced" when the timeline jumps.
    bool prev_failed = false;
    size_t added = 0, failed = 0;
    long long added_bytes = 0;
    double added_seconds = 0;
    // A second, longer accumulator, used only to decide whether the stream is
    // genuinely falling behind. See RS_LIVE_SUSTAIN_INTERVAL.
    double sustain_anchor = report_anchor;
    double sustain_seconds = 0;
    size_t sustain_added = 0;

    for (;;) {
        pthread_mutex_lock(&st->mu);
        for (;;) {
            if (st->stop) { pthread_mutex_unlock(&st->mu); return NULL; }
            if (rep->pend_count > 0) {
                pend_state hs = pend_at_locked(rep, 0)->state;
                if (hs == PEND_READY || hs == PEND_FAILED) break;
            } else if (rep->poller_done) {
                pthread_mutex_unlock(&st->mu);
                return NULL;
            }
            // A timed wait, so the report below still goes out on a stream that
            // has gone quiet — silence is exactly when it matters.
            struct timespec ts_deadline;
            clock_gettime(CLOCK_REALTIME, &ts_deadline);
            ts_deadline.tv_sec += 1;
            pthread_cond_timedwait(&st->cv, &st->mu, &ts_deadline);
            if (now_seconds() - report_anchor >= RS_LIVE_REPORT_INTERVAL) break;
        }

        double seg_dur = rep->seg_duration > 0 ? rep->seg_duration : 2.0;

        // Head-of-line rule. If the head has been fetching far longer than the
        // segment is worth, and there is finished work stuck behind it, give up
        // on it and let the rest through. Re-stamping the generation is what
        // makes this safe: the download thread that still owns this slot will
        // find its stamp no longer matches and drop its result instead of
        // writing it into whatever occupies the slot by then.
        bool abandoned = false;
        if (rep->pend_count > 1) {
            pend_item *head = pend_at_locked(rep, 0);
            double stall = seg_dur * RS_LIVE_HEAD_STALL_FACTOR;
            if (stall < RS_LIVE_HEAD_STALL_MIN) stall = RS_LIVE_HEAD_STALL_MIN;
            bool ready_behind = false;
            for (size_t i = 1; i < rep->pend_count && !ready_behind; i++)
                if (pend_at_locked(rep, i)->state == PEND_READY) ready_behind = true;
            double waited = head->started > 0 ? now_seconds() - head->started : 0;
            // Normally we only give up on the head once something behind it is
            // finished — that is the proof it is the head, not the path, that
            // is the problem. But when every thread is stuck on unanswerable
            // requests nothing ever becomes ready, so there is a second, looser
            // bound: past a few times the stall threshold the segment is too
            // old to be worth having whatever the rest of the queue is doing.
            bool overdue = ready_behind ? waited > stall : waited > stall * 3.0;
            if (head->state == PEND_FETCHING && overdue && head->started > 0) {
                if (head->attempts < RS_LIVE_FETCH_TRIES) {
                    // Abort only this request. The worker keeps ownership of
                    // the segment and its next attempt takes the URL refreshed
                    // by the newest MPD poll.
                    head->request_gen++;
                    head->started = now_seconds();
                } else {
                    head->gen = ++rep->pend_gen;
                    head->state = PEND_FAILED;
                    snprintf(head->err, sizeof(head->err),
                             ready_behind
                                 ? "abandoned after %.1fs — it was holding up segments already downloaded"
                                 : "abandoned after %.1fs — no answer, and the whole queue is behind it",
                             waited);
                    abandoned = true;
                }
            }
        }

        pend_item item;
        bool have_item = false;
        if (rep->pend_count > 0) {
            pend_state hs = pend_at_locked(rep, 0)->state;
            if (hs == PEND_READY || hs == PEND_FAILED) {
                item = *pend_at_locked(rep, 0);
                // Seen at the moment it leaves the queue, under the same lock
                // that removes it: the poller refreshes the URL of anything
                // still queued, so an item must be either in the queue or in
                // the seen set at every instant, never in neither.
                seen_add(&rep->seen, item.idkey);
                memset(pend_at_locked(rep, 0), 0, sizeof(pend_item));
                rep->pend_head = (rep->pend_head + 1) % rep->pend_cap;
                rep->pend_count--;
                have_item = true;
            }
        }

        cfg_snap cfg;
        cfg_snapshot_locked(st, &cfg);
        pthread_mutex_unlock(&st->mu);
        // A slot came free, so the poller may have room it did not have before.
        if (have_item) pthread_cond_broadcast(&st->cv);

        if (have_item) {
            if (abandoned)
                lgf(st, "error", "headOfLine", item.url, 0, -1,
                    "%s: %s", rep->rep_id, item.err);
            commit_one(rep, &cfg, &item, seg_dur, &failed, &added, &added_bytes,
                       &added_seconds, &prev_failed);
            free(item.url);
            free(item.data);

            // Re-render straight away so the next playlist request is served
            // from memory with this segment already in it.
            pthread_mutex_lock(&st->mu);
            size_t before = rep->nsegs;
            rep_prune_locked(rep, cfg.keep_segments);
            rep->pruned_total += before - rep->nsegs;
            char *rendered = rep_render_locked(rep, &cfg);
            bool first = false;
            if (rendered) {
                free(rep->playlist);
                rep->playlist = rendered;
                first = !rep->ready;
                rep->ready = true;
                pthread_cond_broadcast(&st->cv);
            }
            pthread_mutex_unlock(&st->mu);
            if (first)
                lgf(st, "info", "playlistReady", NULL, 0, -1,
                    "%s: first media playlist is ready", rep->rep_id);
        }

        double elapsed = now_seconds() - report_anchor;
        if (elapsed >= RS_LIVE_REPORT_INTERVAL) {
            pthread_mutex_lock(&st->mu);
            size_t held = rep->nsegs, held_bytes = rep->bytes, inflight = rep->pend_count;
            size_t pruned = rep->pruned_total;
            rep->pruned_total = 0;
            long long skipped = rep->skipped;
            pthread_mutex_unlock(&st->mu);

            // Media published per second of wall clock. This is THE number for
            // whether the engine is keeping up: sustained below 1.0 means the
            // output window grows shorter than real time, so every player
            // drains its buffer and stalls regardless of how correct the
            // segments are.
            double realtime = elapsed > 0 ? added_seconds / elapsed : 0;
            lgf(st, "info", "pollDone", NULL, 0, added_bytes,
                "%s: +%lu segments (%.1fs media) %.2fx realtime, %lu in flight, %lu failed, "
                "%lu pruned, %lu held (%.1f MB) in %.2fs",
                rep->rep_id, (unsigned long)added, added_seconds, realtime,
                (unsigned long)inflight, (unsigned long)failed, (unsigned long)pruned,
                (unsigned long)held, (double)held_bytes / (1024.0 * 1024.0), elapsed);

            sustain_seconds += added_seconds;
            sustain_added += added;
            double sustained = now_seconds() - sustain_anchor;
            if (sustained >= RS_LIVE_SUSTAIN_INTERVAL) {
                double srt = sustained > 0 ? sustain_seconds / sustained : 0;
                if (sustain_added > 0 && srt < 0.95)
                    lgf(st, "error", "fallingBehind", NULL, 0, -1,
                        "%s: publishing %.2fx realtime (%.1fs of media in %.1fs) — the advertised "
                        "window will shrink and players will stall; %lld segments skipped so far",
                        rep->rep_id, srt, sustain_seconds, sustained, skipped);
                sustain_anchor = now_seconds();
                sustain_seconds = 0;
                sustain_added = 0;
            }

            report_anchor = now_seconds();
            added = failed = 0;
            added_bytes = 0;
            added_seconds = 0;
        }
        cfg_snap_dispose(&cfg);
    }
}

// --- 5. playlist rendering --------------------------------------------------

// Renders this representation's media playlist. Caller holds st->mu.
//
// The port of LiveMPDToM3U8.writePlaylist: a window of `playlist_segments`
// ending `hold_back` segments before the newest one on disk, a media sequence
// that advances by exactly one per segment, and a discontinuity sequence that
// accounts for every break that has already scrolled out of the window.
static char *rep_render_locked(live_rep *rep, const cfg_snap *cfg) {
    if (rep->nsegs == 0) return NULL;

    // Average duration, for turning the configured delay in seconds into a
    // number of segments.
    double sum = 0;
    for (size_t i = 0; i < rep->nsegs; i++) sum += rep->segs[i].duration;
    double avg = sum / (double)rep->nsegs;

    // Live-edge hold-back: end the advertised window this many segments before
    // the newest one we hold, so the player always has that many segments
    // already downloaded and queued ahead of the playlist end and cannot drain
    // to the true edge and stall.
    int base_hold = cfg->download_ahead - cfg->playlist_segments;
    if (base_hold < 0) base_hold = 0;
    int delay_hold = (cfg->playback_delay_seconds > 0 && avg > 0.01)
                         ? (int)((double)cfg->playback_delay_seconds / avg + 0.5)
                         : 0;
    long long hold = base_hold + delay_hold;
    // Never hold back so far that a full window is not available yet (startup).
    long long slack = (long long)rep->nsegs - cfg->playlist_segments;
    if (hold > slack) hold = slack;
    if (hold < 0) hold = 0;

    long long window_end = (long long)rep->nsegs - hold;
    if (window_end < 1) window_end = (long long)rep->nsegs;
    long long window_start = window_end - cfg->playlist_segments;
    if (window_start < 0) window_start = 0;

    double maxdur = 0;
    for (long long i = window_start; i < window_end; i++)
        if (rep->segs[i].duration > maxdur) maxdur = rep->segs[i].duration;
    int target = (int)(maxdur + 0.999);
    if (target < 1) target = 1;

    // Every break that isn't rendered as an inline tag still has to be folded
    // into the count: the ones that fell off the queue entirely, the ones
    // still held but behind the window's first segment, and — this has to be
    // "<=", not "<" — window_start's own break. A break landing exactly on
    // window_start gets no tag (see the loop below: meaningless on the first
    // segment) and used to get skipped here too, so it vanished from this
    // poll's playlist entirely, only to reappear a poll later once that
    // segment scrolled one further back and the "<" finally included it. A
    // client that reloads across that boundary sees the same segment's
    // discontinuity generation change retroactively and (rightly) treats the
    // two playlists as inconsistent — hls.js calls this a "discontinuity
    // sequence mismatch" and stops loading rather than risk misaligning A/V.
    long long disc_seq = rep->dropped_disc;
    for (long long i = 0; i <= window_start; i++)
        if (rep->segs[i].disc) disc_seq++;

    sbuf b;
    sb_init(&b);
    sb_add(&b, "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-INDEPENDENT-SEGMENTS\n");
    sb_addf(&b, "#EXT-X-TARGETDURATION:%d\n", target);
    // Advances by exactly one per segment (RFC 8216 4.3.3.2) because it is the
    // append counter, not anything derived from the manifest's $Time$.
    sb_addf(&b, "#EXT-X-MEDIA-SEQUENCE:%lld\n", rep->segs[window_start].seq);
    if (disc_seq > 0) sb_addf(&b, "#EXT-X-DISCONTINUITY-SEQUENCE:%lld\n", disc_seq);

    // Opaque URLs: "<repIndex>_<sequence>.m4s", resolved against this queue.
    //
    // These used to embed the upstream URL as ?u=<encoded>, which handed every
    // viewer the origin address complete with its session token — anyone with
    // the playlist could fetch from the CDN directly and skip the restream. The
    // segment is already downloaded and decrypted in memory by the time it is
    // advertised, so the playlist has no reason to name where it came from.
    //
    // The filename still ends in a real media extension because ffmpeg's HLS
    // demuxer rejects any segment URL whose extension is not on its allow-list.
    //
    // A subtitle rendition is the exception: its segments left the worker as
    // WebVTT documents, which are self-contained text with no initialization
    // segment to map. Advertising the source's stpp init here would hand the
    // player an fMP4 header for a track it is about to receive as text.
    bool is_text = rep->kind && strcmp(rep->kind, "text") == 0;
    const char *ext = is_text ? "vtt" : "m4s";
    if (!is_text && rep->init_data && rep->init_len) {
        sb_addf(&b, "#EXT-X-MAP:URI=\"/restream/%s/%d_init.mp4\"\n",
                rep->owner->id, rep->index);
    }
    for (long long i = window_start; i < window_end; i++) {
        live_seg *s = &rep->segs[i];
        // The tag marks a break *between* two segments, so it is meaningless on
        // the first one in the window — that break is already carried by
        // EXT-X-DISCONTINUITY-SEQUENCE above.
        if (s->disc && i > window_start) sb_add(&b, "#EXT-X-DISCONTINUITY\n");
        sb_addf(&b, "#EXTINF:%.3f,\n/restream/%s/%d_%lld.%s\n",
                s->duration, rep->owner->id, rep->index, s->seq, ext);
    }
    return b.p;
}

// --- the representation thread ---------------------------------------------

// Starts this representation's download pool and writer. Both live for as long
// as the rep does: the whole point of persistent download threads is that each
// one keeps its HTTP connection open across segments, so the TLS handshake is
// paid once per thread rather than once per segment.
static bool rep_start_workers(live_rep *rep, const cfg_snap *cfg) {
    live_stream *st = rep->owner;

    rep->pend_cap = RS_LIVE_PENDING_MAX;
    rep->pending = (pend_item *)calloc(rep->pend_cap, sizeof(pend_item));
    if (!rep->pending) return false;

    size_t want = (size_t)effective_parallel_downloads(cfg->parallel_downloads);

    rep->dl_threads = (pthread_t *)calloc(want, sizeof(pthread_t));
    if (!rep->dl_threads) return false;
    for (size_t i = 0; i < want; i++)
        if (pthread_create(&rep->dl_threads[rep->ndl], NULL, download_main, rep) == 0) rep->ndl++;
    if (rep->ndl == 0) return false;

    if (pthread_create(&rep->wr_thread, NULL, writer_main, rep) == 0) rep->wr_started = true;
    else return false;

    if ((size_t)cfg->parallel_downloads > RS_LIVE_MAX_DL_THREADS)
        lgf(st, "info", "repStart", NULL, 0, -1,
            "%s: parallelDownloads is %d, using %lu persistent connections — each one is kept "
            "open across segments, so more than this buys congestion, not throughput",
            rep->rep_id, cfg->parallel_downloads, (unsigned long)rep->ndl);
    return true;
}

static void rep_stop_workers(live_rep *rep) {
    live_stream *st = rep->owner;
    pthread_mutex_lock(&st->mu);
    rep->poller_done = true;
    pthread_cond_broadcast(&st->cv);
    pthread_mutex_unlock(&st->mu);

    for (size_t i = 0; i < rep->ndl; i++) pthread_join(rep->dl_threads[i], NULL);
    if (rep->wr_started) pthread_join(rep->wr_thread, NULL);

    pthread_mutex_lock(&st->mu);
    pend_clear_locked(rep);
    pthread_mutex_unlock(&st->mu);
    free(rep->dl_threads);
    rep->dl_threads = NULL;
    rep->ndl = 0;
    rep->wr_started = false;
    free(rep->pending);
    rep->pending = NULL;
    rep->pend_cap = 0;
}

// The poller. Re-reads the manifest on a strict cadence and queues whatever is
// new; it never downloads, decrypts or publishes, so nothing the network does
// can stop it keeping its place in the live window.
static void *rep_main(void *arg) {
    live_rep *rep = (live_rep *)arg;
    live_stream *st = rep->owner;
    double interval = 2.0;

    cfg_snap boot;
    pthread_mutex_lock(&st->mu);
    cfg_snapshot_locked(st, &boot);
    pthread_mutex_unlock(&st->mu);
    bool ok = rep_start_workers(rep, &boot);
    lgf(st, ok ? "info" : "error", "repStart", NULL, 0, -1,
        ok ? "%s (%s): worker started, %lu download threads"
           : "%s (%s): worker could not start its download threads",
        rep->rep_id, rep->kind, (unsigned long)rep->ndl);
    cfg_snap_dispose(&boot);

    while (ok) {
        if (live_stopping(st)) break;
        double cycle_start = now_seconds();

        cfg_snap cfg;
        pthread_mutex_lock(&st->mu);
        cfg_snapshot_locked(st, &cfg);
        rep->polls++;
        rep->last_poll = now_seconds();
        pthread_mutex_unlock(&st->mu);

        rep_poll(rep, &cfg, &interval);

        // The poll interval is a PERIOD, not a delay: sleeping the full
        // interval after the work made the real cycle interval + work, which at
        // a two second minimumUpdatePeriod was exactly break-even and never
        // built a cushion.
        double period = cfg.poll_interval > 0 ? cfg.poll_interval : interval;
        if (period > 10.0) period = 10.0;

        // Never poll so slowly that segments can age out of the manifest window
        // between reads. N_m3u8DL-RE takes the same guarantee from the other
        // direction, setting its refresh to half the window's duration minus a
        // two second margin; here the period is already short, so this only
        // caps a source that advertises an absurd minimumUpdatePeriod.
        double window_seconds = (double)(cfg.download_ahead > 0 ? cfg.download_ahead : 8)
                              * (rep->seg_duration > 0 ? rep->seg_duration : 2.0);
        double safety = window_seconds / 2.0 - 2.0;
        if (safety < 1.0) safety = 1.0;
        if (period > safety) period = safety;

        double spent = now_seconds() - cycle_start;
        double sleep_for = period - spent;
        if (sleep_for < 0.1) sleep_for = 0.1;
        // A manifest read that on its own overruns the safety margin is the one
        // case where segments can still be missed, because the window moves on
        // while the read is in flight. Downloads no longer share this thread,
        // so this now means the manifest path itself is too slow — a different
        // and much more specific problem than the old pollSlow, which fired
        // whenever a batch of segments took a while.
        if (spent > safety)
            lgf(st, "error", "pollSlow", NULL, 0, -1,
                "%s: the manifest read alone took %.2fs, over the %.2fs safety margin for a "
                "%.0fs window — segments can age out between reads",
                rep->rep_id, spent, safety, window_seconds);

        // A failed poll overrides all of that. The rule above reasons about a
        // poll that did work, and a failure that returns in 40ms looks to it
        // like the fastest possible success — so it retried hardest exactly
        // when it should have retried least, and against an origin that limits
        // by IP the retries alone kept the block in place. See live_backoff.c.
        double backoff = rs_live_backoff_delay(rep->manifest_failures, rep->manifest_throttled, period);
        if (backoff > sleep_for) {
            sleep_for = backoff;
            lgf(st, "info", "pollBackoff", NULL, 0, -1,
                "%s: %d failed poll%s in a row%s — waiting %.1fs before the next one",
                rep->rep_id, rep->manifest_failures, rep->manifest_failures == 1 ? "" : "s",
                rep->manifest_throttled ? " (origin is refusing for rate reasons)" : "",
                sleep_for);
        }
        cfg_snap_dispose(&cfg);
        if (!live_wait(st, sleep_for)) break;
    }

    rep_stop_workers(rep);
    lgf(st, "info", "repStop", NULL, 0, -1, "%s: worker stopped after %lld polls",
        rep->rep_id, rep->polls);

    pthread_mutex_lock(&st->mu);
    rep->finished = true;
    pthread_mutex_unlock(&st->mu);
    return NULL;
}

// --- 4. the director --------------------------------------------------------

// Caller holds st->mu.
static live_rep *rep_find_locked(live_stream *st, const char *rep_id) {
    for (size_t i = 0; i < st->nreps; i++)
        if (strcmp(st->reps[i]->rep_id, rep_id) == 0) return st->reps[i];
    return NULL;
}

// Creates and starts a representation worker if it does not exist yet.
// Caller holds st->mu.
static void rep_ensure_locked(live_stream *st, const char *rep_id, const char *kind) {
    if (!rep_id || !rep_id[0]) return;
    if (rep_find_locked(st, rep_id)) return;
    if (st->nreps >= RS_LIVE_MAX_REPS) return;

    live_rep *rep = (live_rep *)calloc(1, sizeof(*rep));
    if (!rep) return;
    rep->owner = st;
    rep->rep_id = rs_strdup(rep_id);
    rep->kind = rs_strdup(kind && kind[0] ? kind : "video");
    rep->iv_size = 8;
    rep->highest_time_val = -1;
    rep->index = (int)st->nreps;
    st->reps[st->nreps++] = rep;
    if (pthread_create(&rep->thread, NULL, rep_main, rep) == 0) rep->thread_started = true;
    else rep->finished = true;
}

// The director's retry-after-failure delay, on the same rule as the
// representation workers. It polls the same MPD as they do, so a flat retry
// here would keep an IP-based rate limit armed no matter how well the workers
// behaved — it is the third of the three request streams that trips these
// limits in the first place.
static double director_retry_delay(int failures, bool throttled) {
    double d = rs_live_backoff_delay(failures, throttled, RS_LIVE_DIRECTOR_RETRY);
    return d > 0 ? d : RS_LIVE_DIRECTOR_RETRY;
}

static void *director_main(void *arg) {
    live_stream *st = (live_stream *)arg;
    lg(st, "info", "liveStart", NULL, 0, -1, "live DASH engine started");

    int fails = 0;              // consecutive failed rendition polls
    bool throttled = false;     // ...and whether the last was a rate refusal

    for (;;) {
        if (live_stopping(st)) break;

        cfg_snap cfg;
        char *pinned = NULL;
        pthread_mutex_lock(&st->mu);
        cfg_snapshot_locked(st, &cfg);
        pinned = rs_strdup(st->representation ? st->representation : "");
        pthread_mutex_unlock(&st->mu);

        char err[256] = {0};
        char *json = manifest_fetch(st, &cfg, "", 0, err, sizeof(err));
        if (!json) {
            fails++;
            throttled = rs_live_status_is_throttle(status_from_error(err)) != 0;
            double wait = director_retry_delay(fails, throttled);
            lgf(st, "error", "renditions", cfg.mpd_url, 0, -1, "%s — retrying in %.1fs",
                err[0] ? err : "could not read the MPD", wait);
            free(pinned);
            cfg_snap_dispose(&cfg);
            if (!live_wait(st, wait)) break;
            continue;
        }

        rs_json *root = rs_json_parse(json, strlen(json));
        free(json);
        if (!root) {
            fails++;
            throttled = false;
            double wait = director_retry_delay(fails, throttled);
            lgf(st, "error", "renditions", cfg.mpd_url, 0, -1,
                "malformed DASH description — retrying in %.1fs", wait);
            free(pinned);
            cfg_snap_dispose(&cfg);
            if (!live_wait(st, wait)) break;
            continue;
        }
        fails = 0;
        throttled = false;

        const rs_json *video = rs_json_obj_get(root, "video");
        const rs_json *audio = rs_json_obj_get(root, "audio");
        const rs_json *text = rs_json_obj_get(root, "text");
        const rs_json *cc = rs_json_obj_get(root, "cc");
        bool have_video = video && rs_json_type_of(video) == RS_JSON_OBJ;
        bool have_audio = audio && rs_json_type_of(audio) == RS_JSON_OBJ;
        bool have_text = text && rs_json_type_of(text) == RS_JSON_OBJ;
        size_t ncc = (cc && rs_json_type_of(cc) == RS_JSON_ARR) ? rs_json_arr_len(cc) : 0;
        bool dynamic = rs_json_as_bool(rs_json_obj_get(root, "dynamic"), true);

        // A pinned representation overrides the auto-selected video rendition;
        // the audio and subtitle renditions are still picked up so the master
        // keeps its separate groups.
        const char *vid = have_video ? rs_json_obj_str(video, "id", "") : "";
        if (pinned && pinned[0]) vid = pinned;
        const char *vcodecs = have_video ? rs_json_obj_str(video, "codecs", "") : "";
        long long vbw = have_video ? (long long)rs_json_as_num(rs_json_obj_get(video, "bandwidth"), 0) : 0;
        const char *aid = have_audio ? rs_json_obj_str(audio, "id", "") : "";
        const char *acodecs = have_audio ? rs_json_obj_str(audio, "codecs", "") : "";
        const char *alang = have_audio ? rs_json_obj_str(audio, "lang", "") : "";
        if (aid[0] && vid[0] && strcmp(aid, vid) == 0) { aid = ""; acodecs = ""; have_audio = false; }
        const char *tid = have_text ? rs_json_obj_str(text, "id", "") : "";
        const char *tlang = have_text ? rs_json_obj_str(text, "lang", "") : "";
        if (tid[0] && ((vid[0] && strcmp(tid, vid) == 0) || (aid[0] && strcmp(tid, aid) == 0))) {
            tid = ""; have_text = false;
        }

        if (!vid[0]) {
            lg(st, "error", "renditions", cfg.mpd_url, 0, -1, "no video representation in the MPD");
            rs_json_free(root);
            free(pinned);
            cfg_snap_dispose(&cfg);
            if (!live_wait(st, 5.0)) break;
            continue;
        }

        // Render the master playlist.
        sbuf b;
        sb_init(&b);
        sb_add(&b, "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-INDEPENDENT-SEGMENTS\n");
        if (have_audio && aid[0]) {
            char *aenc = qenc(aid);
            sb_add(&b, "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"Audio\"");
            sb_add_lang(&b, alang);
            sb_addf(&b, ",DEFAULT=YES,AUTOSELECT=YES,"
                        "URI=\"/play/%s/index.m3u8?rep=%s&mtype=audio\"\n",
                    st->id, aenc ? aenc : "");
            free(aenc);
        }
        // Sidecar subtitles (TTML/stpp or WebVTT), converted to WebVTT by the
        // rendition worker. DEFAULT=NO so a player does not switch them on
        // unasked; AUTOSELECT=YES so it still picks them when the viewer's
        // language preference asks for them.
        if (have_text && tid[0]) {
            char *tenc = qenc(tid);
            sb_add(&b, "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"subs\",NAME=\"Subtitles\"");
            sb_add_lang(&b, tlang);
            sb_addf(&b, ",DEFAULT=NO,AUTOSELECT=YES,FORCED=NO,"
                        "URI=\"/play/%s/index.m3u8?rep=%s&mtype=text\"\n",
                    st->id, tenc ? tenc : "");
            free(tenc);
        }
        // In-band CEA-608/708. These have no URI: the caption bytes are already
        // inside the video segments we pass through untouched, so this tag is
        // the whole of the work — without it a player has no reason to look for
        // them and shows nothing.
        for (size_t i = 0; i < ncc; i++) {
            const rs_json *entry = rs_json_arr_at(cc, i);
            const char *iid = rs_json_obj_str(entry, "instreamId", "");
            const char *clang = rs_json_obj_str(entry, "lang", "");
            if (!iid[0]) continue;
            sb_addf(&b, "#EXT-X-MEDIA:TYPE=CLOSED-CAPTIONS,GROUP-ID=\"cc\",NAME=\"%s\"", iid);
            sb_add_lang(&b, clang);
            sb_addf(&b, ",AUTOSELECT=YES,INSTREAM-ID=\"%s\"\n", iid);
        }

        sb_addf(&b, "#EXT-X-STREAM-INF:BANDWIDTH=%lld", vbw > 0 ? vbw : 3000000);
        if (vcodecs[0] && acodecs[0]) sb_addf(&b, ",CODECS=\"%s,%s\"", vcodecs, acodecs);
        else if (vcodecs[0]) sb_addf(&b, ",CODECS=\"%s\"", vcodecs);
        if (have_audio && aid[0]) sb_add(&b, ",AUDIO=\"aud\"");
        if (have_text && tid[0]) sb_add(&b, ",SUBTITLES=\"subs\"");
        // RFC 8216 4.3.4.2: the attribute must be NONE when there are no
        // in-band captions, or a player is entitled to go looking for them
        // anyway — which some do, on every segment, for nothing.
        sb_add(&b, ncc > 0 ? ",CLOSED-CAPTIONS=\"cc\"" : ",CLOSED-CAPTIONS=NONE");
        char *venc = qenc(vid);
        sb_addf(&b, "\n/play/%s/index.m3u8?rep=%s&mtype=video\n", st->id, venc ? venc : "");
        free(venc);

        pthread_mutex_lock(&st->mu);
        bool first = st->master == NULL;
        free(st->master);
        st->master = b.p;  // ownership moves to the stream
        rep_ensure_locked(st, vid, "video");
        if (have_audio && aid[0]) rep_ensure_locked(st, aid, "audio");
        if (have_text && tid[0]) rep_ensure_locked(st, tid, "text");
        pthread_cond_broadcast(&st->cv);
        pthread_mutex_unlock(&st->mu);

        if (first)
            lgf(st, "info", "renditions", cfg.mpd_url, 0, -1,
                "%s MPD — video \"%s\"%s%s%s%s%s%s%s, master playlist ready",
                dynamic ? "dynamic" : "static", vid,
                (have_audio && aid[0]) ? ", audio \"" : "",
                (have_audio && aid[0]) ? aid : "",
                (have_audio && aid[0]) ? "\"" : "",
                (have_text && tid[0]) ? ", subtitles \"" : "",
                (have_text && tid[0]) ? tid : "",
                (have_text && tid[0]) ? "\"" : "",
                ncc > 0 ? ", in-band closed captions" : "");

        bool reduced = cfg.reduced_manifest_polling != 0;
        rs_json_free(root);
        free(pinned);
        cfg_snap_dispose(&cfg);
        if (!live_wait(st, reduced ? RS_LIVE_DIRECTOR_IDLE_INTERVAL : RS_LIVE_DIRECTOR_INTERVAL)) break;
    }

    lg(st, "info", "liveStop", NULL, 0, -1, "live DASH engine stopped");
    pthread_mutex_lock(&st->mu);
    st->dir_finished = true;
    pthread_mutex_unlock(&st->mu);
    return NULL;
}

// --- 6. public API ----------------------------------------------------------

static void rep_dispose(live_rep *rep) {
    for (size_t i = 0; i < rep->nsegs; i++) {
        free(rep->segs[i].url);
        free(rep->segs[i].data);
    }
    free(rep->segs);
    // Normally already released by rep_stop_workers; a representation whose
    // thread never started never gets there.
    if (rep->pending) {
        for (size_t i = 0; i < rep->pend_count; i++)
            pend_item_dispose(&rep->pending[(rep->pend_head + i) % rep->pend_cap]);
        free(rep->pending);
    }
    free(rep->dl_threads);
    free(rep->init_url);
    free(rep->init_key);
    free(rep->init_data);
    free(rep->rep_id);
    free(rep->kind);
    free(rep->playlist);
    seen_dispose(&rep->seen);
    free(rep);
}

static void stream_dispose(live_stream *st) {
    for (size_t i = 0; i < st->nreps; i++) rep_dispose(st->reps[i]);
    free(st->id);
    free(st->mpd_url); free(st->representation);
    free(st->manifest_proxy); free(st->media_proxy);
    free(st->manifest_headers); free(st->media_headers);
    free(st->downloader); free(st->dl_params); free(st->keys);
    free(st->segment_url_params);
    for (size_t i = 0; i < st->nsources; i++) free(st->sources[i]);
    free(st->sources);
    free(st->master);
    pthread_mutex_destroy(&st->mu);
    pthread_cond_destroy(&st->cv);
    free(st);
}

// Caller holds live->mu.
//
// An engine that has been asked to stop is deliberately invisible here: its
// threads may take a moment to unwind, and until rs_live_reap joins them the
// object is still in the table. Treating it as absent is what lets a stream be
// stopped and started again straight away — the restart gets a fresh engine
// beside the retiring one instead of being refused for the second or so the old
// one takes to notice the flag.
static live_stream *stream_find_locked(rs_live *live, const char *stream_id) {
    for (size_t i = 0; i < live->nstreams; i++) {
        live_stream *st = live->streams[i];
        if (strcmp(st->id, stream_id) != 0) continue;
        pthread_mutex_lock(&st->mu);
        bool stopping = st->stop;
        pthread_mutex_unlock(&st->mu);
        if (!stopping) return st;
    }
    return NULL;
}

// Replaces every configured string on `st`. Caller holds st->mu.
static void stream_apply_config_locked(live_stream *st, const rs_live_config *cfg) {
#define SET(field, value) do { free(st->field); st->field = rs_strdup((value) ? (value) : ""); } while (0)
    SET(mpd_url, cfg->mpd_url);
    SET(representation, cfg->representation);
    SET(manifest_proxy, cfg->manifest_proxy);
    SET(media_proxy, cfg->media_proxy);
    SET(manifest_headers, cfg->manifest_headers);
    SET(media_headers, cfg->media_headers);
    SET(downloader, cfg->downloader);
    SET(dl_params, cfg->dl_params);
    SET(keys, cfg->decryption_keys);
    SET(segment_url_params, cfg->segment_url_params);
#undef SET

    // Rebuild the source list: primary first, then the mirrors. A config edit
    // resets the cursor to the primary, since the operator changing the sources
    // is the one case where "whatever answered last" is not the right memory.
    for (size_t i = 0; i < st->nsources; i++) free(st->sources[i]);
    free(st->sources);
    st->sources = NULL;
    st->nsources = 0;
    st->source = 0;
    size_t want_sources = 1 + cfg->cdn_url_count;
    st->sources = (char **)calloc(want_sources, sizeof(char *));
    if (st->sources) {
        st->sources[st->nsources++] = rs_strdup(cfg->mpd_url ? cfg->mpd_url : "");
        for (size_t i = 0; i < cfg->cdn_url_count; i++) {
            const char *u = cfg->cdn_urls ? cfg->cdn_urls[i] : NULL;
            if (u && u[0]) st->sources[st->nsources++] = rs_strdup(u);
        }
    }

    st->inherit_url_params = cfg->inherit_url_params;
    st->reduced_manifest_polling = cfg->reduced_manifest_polling;
    st->playlist_segments = cfg->playlist_segments > 0 ? cfg->playlist_segments : 6;
    if (st->playlist_segments < 3) st->playlist_segments = 3;
    st->download_ahead = cfg->download_ahead > 0 ? cfg->download_ahead : 16;
    st->parallel_downloads = cfg->parallel_downloads;
    st->prioritize_oldest = cfg->prioritize_oldest;
    st->playback_delay_seconds = cfg->playback_delay_seconds > 0 ? cfg->playback_delay_seconds : 0;
    st->audio_delay_ms = cfg->audio_delay_ms;
    st->poll_interval = cfg->poll_interval;

    // The queue has to be able to hold the advertised window plus the whole
    // hold-back, or the playlist would be rebuilt from segments that were
    // already pruned.
    //
    // The upper clamp is the one place this deliberately departs from the Swift
    // worker. That one kept `keepSegments` (60 by default) on disk, where the
    // count costs nothing; here the segments are decrypted and held in RAM so
    // they can be served without touching the network, and 60 of them per
    // rendition would be well over a hundred megabytes per stream. Everything
    // past the window plus a lag allowance only serves players that have fallen
    // badly behind, which the hold-back already exists to prevent.
    int hold = st->download_ahead - st->playlist_segments;
    if (hold < 0) hold = 0;
    int need = st->playlist_segments + hold + 8;
    st->keep_segments = cfg->keep_segments > 0 ? cfg->keep_segments : 60;
    if (st->keep_segments < need) st->keep_segments = need;
    if (st->keep_segments > need + 12) st->keep_segments = need + 12;
}

rs_live *rs_live_create(rs_live_fetch_fn fetch, rs_live_dash_fn dash,
                        rs_live_log_fn log, void *log_ctx) {
    if (!fetch || !dash) return NULL;
    rs_live *live = (rs_live *)calloc(1, sizeof(*live));
    if (!live) return NULL;
    live->fetch = fetch;
    live->dash = dash;
    live->log = log;
    live->log_ctx = log_ctx;
    pthread_mutex_init(&live->mu, NULL);
    return live;
}

int rs_live_start(rs_live *live, const char *stream_id, const rs_live_config *cfg) {
    if (!live || !stream_id || !stream_id[0] || !cfg) return -1;
    if (!cfg->mpd_url || !cfg->mpd_url[0]) return -1;

    pthread_mutex_lock(&live->mu);
    live_stream *st = stream_find_locked(live, stream_id);
    if (st) {
        pthread_mutex_lock(&st->mu);
        int desired_workers = effective_parallel_downloads(cfg->parallel_downloads);
        if (st->worker_parallel_downloads == desired_workers) {
            // Polling, headers and window settings are snapshots and can change
            // in place without throwing away the warm segment queue.
            stream_apply_config_locked(st, cfg);
            pthread_mutex_unlock(&st->mu);
            pthread_mutex_unlock(&live->mu);
            return 0;
        }

        // The download pool is structural: changing parallelDownloads after a
        // stream started used to update only the number stored in the config,
        // leaving the old pthread count alive indefinitely. Mark this instance
        // for asynchronous teardown and create its replacement immediately.
        // stream_find_locked deliberately hides stopping instances, so playback
        // and later config updates address the replacement while rs_live_reap
        // joins the old one in the background.
        if (live->nstreams >= RS_LIVE_MAX_STREAMS) {
            pthread_mutex_unlock(&st->mu);
            pthread_mutex_unlock(&live->mu);
            return -1;
        }
        st->stop = true;
        pthread_cond_broadcast(&st->cv);
        pthread_mutex_unlock(&st->mu);
        st = NULL;
    }
    if (live->nstreams >= RS_LIVE_MAX_STREAMS) {
        pthread_mutex_unlock(&live->mu);
        return -1;
    }

    st = (live_stream *)calloc(1, sizeof(*st));
    if (!st) { pthread_mutex_unlock(&live->mu); return -1; }
    st->mgr = live;
    st->id = rs_strdup(stream_id);
    pthread_mutex_init(&st->mu, NULL);
    pthread_cond_init(&st->cv, NULL);
    stream_apply_config_locked(st, cfg);
    st->worker_parallel_downloads = effective_parallel_downloads(cfg->parallel_downloads);
    live->streams[live->nstreams++] = st;
    pthread_mutex_unlock(&live->mu);

    if (pthread_create(&st->dir_thread, NULL, director_main, st) == 0) {
        st->dir_started = true;
        return 0;
    }
    st->dir_finished = true;
    lg(st, "error", "liveStart", NULL, 0, -1, "could not start the live engine thread");
    return -1;
}

void rs_live_stop(rs_live *live, const char *stream_id) {
    if (!live || !stream_id) return;
    pthread_mutex_lock(&live->mu);
    live_stream *st = stream_find_locked(live, stream_id);
    if (st) {
        // Flag and wake, then return. Threads unwind on their own and are
        // joined by rs_live_reap, so the Stop request never waits on a segment
        // download that is already in flight.
        pthread_mutex_lock(&st->mu);
        st->stop = true;
        pthread_cond_broadcast(&st->cv);
        pthread_mutex_unlock(&st->mu);
    }
    pthread_mutex_unlock(&live->mu);
}

bool rs_live_is_running(rs_live *live, const char *stream_id) {
    if (!live || !stream_id) return false;
    pthread_mutex_lock(&live->mu);
    live_stream *st = stream_find_locked(live, stream_id);
    bool running = false;
    if (st) {
        pthread_mutex_lock(&st->mu);
        running = !st->stop;
        pthread_mutex_unlock(&st->mu);
    }
    pthread_mutex_unlock(&live->mu);
    return running;
}

void rs_live_reap(rs_live *live) {
    if (!live) return;
    pthread_mutex_lock(&live->mu);
    for (size_t i = 0; i < live->nstreams;) {
        live_stream *st = live->streams[i];
        pthread_mutex_lock(&st->mu);
        bool done = st->stop && (!st->dir_started || st->dir_finished);
        for (size_t r = 0; done && r < st->nreps; r++)
            if (st->reps[r]->thread_started && !st->reps[r]->finished) done = false;
        pthread_mutex_unlock(&st->mu);
        if (!done) { i++; continue; }

        if (st->dir_started) pthread_join(st->dir_thread, NULL);
        for (size_t r = 0; r < st->nreps; r++)
            if (st->reps[r]->thread_started) pthread_join(st->reps[r]->thread, NULL);
        live->streams[i] = live->streams[live->nstreams - 1];
        live->nstreams--;
        pthread_mutex_unlock(&live->mu);
        stream_dispose(st);
        pthread_mutex_lock(&live->mu);
    }
    pthread_mutex_unlock(&live->mu);
}

void rs_live_destroy(rs_live *live) {
    if (!live) return;
    pthread_mutex_lock(&live->mu);
    for (size_t i = 0; i < live->nstreams; i++) {
        live_stream *st = live->streams[i];
        pthread_mutex_lock(&st->mu);
        st->stop = true;
        pthread_cond_broadcast(&st->cv);
        pthread_mutex_unlock(&st->mu);
    }
    size_t n = live->nstreams;
    live_stream *all[RS_LIVE_MAX_STREAMS];
    for (size_t i = 0; i < n; i++) all[i] = live->streams[i];
    live->nstreams = 0;
    pthread_mutex_unlock(&live->mu);

    for (size_t i = 0; i < n; i++) {
        live_stream *st = all[i];
        if (st->dir_started) pthread_join(st->dir_thread, NULL);
        for (size_t r = 0; r < st->nreps; r++)
            if (st->reps[r]->thread_started) pthread_join(st->reps[r]->thread, NULL);
        stream_dispose(st);
    }
    pthread_mutex_destroy(&live->mu);
    free(live);
}

// Caller holds st->mu. The master is only usable once every rendition it points
// at can actually answer: a player that fetches a variant playlist and gets a
// 404 abandons the stream instead of retrying.
// Subtitles are the exception: they are an optional, DEFAULT=NO rendition a
// player only fetches once a viewer asks for them, so a slow or broken text
// track must not hold back the video and audio the stream actually exists to
// deliver. Its EXT-X-MEDIA line is advertised either way — a player that finds
// it not ready yet retries, which is the same thing it does for any rendition.
static bool stream_ready_locked(const live_stream *st) {
    if (!st->master || st->nreps == 0) return false;
    bool any_media = false;
    for (size_t i = 0; i < st->nreps; i++) {
        if (strcmp(st->reps[i]->kind, "text") == 0) continue;
        if (!st->reps[i]->ready) return false;
        any_media = true;
    }
    return any_media;
}

bool rs_live_is_ready(rs_live *live, const char *stream_id) {
    if (!live || !stream_id) return false;
    pthread_mutex_lock(&live->mu);
    live_stream *st = stream_find_locked(live, stream_id);
    bool ready = false;
    if (st) {
        pthread_mutex_lock(&st->mu);
        ready = stream_ready_locked(st);
        pthread_mutex_unlock(&st->mu);
    }
    pthread_mutex_unlock(&live->mu);
    return ready;
}

char *rs_live_master_playlist(rs_live *live, const char *stream_id) {
    if (!live || !stream_id) return NULL;
    pthread_mutex_lock(&live->mu);
    live_stream *st = stream_find_locked(live, stream_id);
    char *copy = NULL;
    if (st) {
        pthread_mutex_lock(&st->mu);
        if (stream_ready_locked(st)) copy = rs_strdup(st->master);
        pthread_mutex_unlock(&st->mu);
    }
    pthread_mutex_unlock(&live->mu);
    return copy;
}

char *rs_live_media_playlist(rs_live *live, const char *stream_id, const char *rep) {
    if (!live || !stream_id || !rep) return NULL;
    pthread_mutex_lock(&live->mu);
    live_stream *st = stream_find_locked(live, stream_id);
    char *copy = NULL;
    if (st) {
        pthread_mutex_lock(&st->mu);
        live_rep *r = rep_find_locked(st, rep);
        if (r && r->playlist) copy = rs_strdup(r->playlist);
        pthread_mutex_unlock(&st->mu);
    }
    pthread_mutex_unlock(&live->mu);
    return copy;
}

uint8_t *rs_live_take_indexed(rs_live *live, const char *stream_id, int rep_index,
                              long long seq, bool want_init, size_t *out_len) {
    if (!live || !stream_id || !out_len || rep_index < 0) return NULL;
    pthread_mutex_lock(&live->mu);
    live_stream *st = stream_find_locked(live, stream_id);
    uint8_t *copy = NULL;
    if (st) {
        pthread_mutex_lock(&st->mu);
        if ((size_t)rep_index < st->nreps) {
            live_rep *r = st->reps[rep_index];
            if (want_init) {
                if (r->init_data) {
                    copy = (uint8_t *)malloc(r->init_len ? r->init_len : 1);
                    if (copy) { memcpy(copy, r->init_data, r->init_len); *out_len = r->init_len; }
                }
            } else {
                // The queue is small (a couple of dozen entries) and ordered, so
                // a scan costs nothing next to the memcpy that follows.
                for (size_t j = 0; j < r->nsegs; j++) {
                    if (r->segs[j].seq != seq) continue;
                    size_t len = r->segs[j].len;
                    copy = (uint8_t *)malloc(len ? len : 1);
                    if (copy) { memcpy(copy, r->segs[j].data, len); *out_len = len; }
                    break;
                }
            }
        }
        pthread_mutex_unlock(&st->mu);
    }
    pthread_mutex_unlock(&live->mu);
    return copy;
}

size_t rs_live_reps(rs_live *live, const char *stream_id, rs_live_rep_desc **out) {
    if (out) *out = NULL;
    if (!live || !stream_id || !out) return 0;
    pthread_mutex_lock(&live->mu);
    live_stream *st = stream_find_locked(live, stream_id);
    size_t n = 0;
    if (st) {
        pthread_mutex_lock(&st->mu);
        if (st->nreps) {
            rs_live_rep_desc *descs = (rs_live_rep_desc *)calloc(st->nreps, sizeof(*descs));
            if (descs) {
                for (size_t i = 0; i < st->nreps; i++) {
                    descs[i].rep_id = rs_strdup(st->reps[i]->rep_id ? st->reps[i]->rep_id : "");
                    descs[i].kind = rs_strdup(st->reps[i]->kind ? st->reps[i]->kind : "");
                    descs[i].index = st->reps[i]->index;
                    descs[i].timescale = st->reps[i]->timescale;
                    descs[i].held = st->reps[i]->nsegs;
                    if (st->reps[i]->nsegs) {
                        descs[i].oldest_seq = st->reps[i]->segs[0].seq;
                        descs[i].newest_seq = st->reps[i]->segs[st->reps[i]->nsegs - 1].seq;
                    }
                }
                *out = descs;
                n = st->nreps;
            }
        }
        pthread_mutex_unlock(&st->mu);
    }
    pthread_mutex_unlock(&live->mu);
    return n;
}

void rs_live_reps_free(rs_live_rep_desc *reps, size_t count) {
    if (!reps) return;
    for (size_t i = 0; i < count; i++) {
        free(reps[i].rep_id);
        free(reps[i].kind);
    }
    free(reps);
}

uint8_t *rs_live_take_after(rs_live *live, const char *stream_id, int rep_index,
                            long long after_seq, long long *out_seq,
                            double *out_duration, size_t *out_len) {
    if (!live || !stream_id || !out_len || !out_seq || rep_index < 0) return NULL;
    pthread_mutex_lock(&live->mu);
    live_stream *st = stream_find_locked(live, stream_id);
    uint8_t *copy = NULL;
    if (st) {
        pthread_mutex_lock(&st->mu);
        if ((size_t)rep_index < st->nreps) {
            live_rep *r = st->reps[rep_index];
            // segs is kept in ascending sequence order by rep_append_locked, so
            // the first entry past the cursor is the one to send. A tail that
            // fell behind far enough for its next segment to have been pruned
            // lands on the oldest one still held instead — the gap is real and
            // already lost, and stalling forever would be worse.
            for (size_t j = 0; j < r->nsegs; j++) {
                if (r->segs[j].seq <= after_seq) continue;
                size_t len = r->segs[j].len;
                copy = (uint8_t *)malloc(len ? len : 1);
                if (copy) {
                    memcpy(copy, r->segs[j].data, len);
                    *out_len = len;
                    *out_seq = r->segs[j].seq;
                    if (out_duration) *out_duration = r->segs[j].duration;
                }
                break;
            }
        }
        pthread_mutex_unlock(&st->mu);
    }
    pthread_mutex_unlock(&live->mu);
    return copy;
}

char *rs_live_status_line(rs_live *live, const char *stream_id) {
    if (!live || !stream_id) return NULL;
    pthread_mutex_lock(&live->mu);
    live_stream *st = stream_find_locked(live, stream_id);
    char *out = NULL;
    if (st) {
        sbuf b;
        sb_init(&b);
        pthread_mutex_lock(&st->mu);
        sb_addf(&b, "master %s", st->master ? "ready" : "pending");
        for (size_t i = 0; i < st->nreps; i++) {
            live_rep *r = st->reps[i];
            sb_addf(&b, "; %s (%s) %lu held, seq %lld-%lld, %lld disc dropped, %.1f MB, %lld polls",
                    r->rep_id, r->kind, (unsigned long)r->nsegs,
                    r->nsegs ? r->segs[0].seq : 0,
                    r->nsegs ? r->segs[r->nsegs - 1].seq : 0, r->dropped_disc,
                    (double)r->bytes / (1024.0 * 1024.0), r->polls);
        }
        pthread_mutex_unlock(&st->mu);
        out = b.p;
    }
    pthread_mutex_unlock(&live->mu);
    return out;
}

#else  // _WIN32 — no pthreads here yet; DASH playback falls back to the
       // request-time path in server.c.

rs_live *rs_live_create(rs_live_fetch_fn fetch, rs_live_dash_fn dash,
                        rs_live_log_fn log, void *log_ctx) {
    (void)fetch; (void)dash; (void)log; (void)log_ctx;
    return NULL;
}
void rs_live_destroy(rs_live *live) { (void)live; }
int rs_live_start(rs_live *live, const char *stream_id, const rs_live_config *cfg) {
    (void)live; (void)stream_id; (void)cfg; return -1;
}
void rs_live_stop(rs_live *live, const char *stream_id) { (void)live; (void)stream_id; }
bool rs_live_is_running(rs_live *live, const char *stream_id) { (void)live; (void)stream_id; return false; }
void rs_live_reap(rs_live *live) { (void)live; }
bool rs_live_is_ready(rs_live *live, const char *stream_id) { (void)live; (void)stream_id; return false; }
char *rs_live_master_playlist(rs_live *live, const char *stream_id) { (void)live; (void)stream_id; return NULL; }
char *rs_live_media_playlist(rs_live *live, const char *stream_id, const char *rep) {
    (void)live; (void)stream_id; (void)rep; return NULL;
}
uint8_t *rs_live_take_indexed(rs_live *live, const char *stream_id, int rep_index,
                              long long seq, bool want_init, size_t *out_len) {
    (void)live; (void)stream_id; (void)rep_index; (void)seq; (void)want_init; (void)out_len;
    return NULL;
}
size_t rs_live_reps(rs_live *live, const char *stream_id, rs_live_rep_desc **out) {
    (void)live; (void)stream_id;
    if (out) *out = NULL;
    return 0;
}
void rs_live_reps_free(rs_live_rep_desc *reps, size_t count) { (void)reps; (void)count; }
uint8_t *rs_live_take_after(rs_live *live, const char *stream_id, int rep_index,
                            long long after_seq, long long *out_seq,
                            double *out_duration, size_t *out_len) {
    (void)live; (void)stream_id; (void)rep_index; (void)after_seq;
    (void)out_seq; (void)out_duration; (void)out_len;
    return NULL;
}
char *rs_live_status_line(rs_live *live, const char *stream_id) { (void)live; (void)stream_id; return NULL; }

#endif  // _WIN32
