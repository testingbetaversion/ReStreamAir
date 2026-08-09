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
// Renditions per stream (video + audio is the shape every source we handle
// uses; the extra slots leave room for a pinned representation alongside them).
#define RS_LIVE_MAX_REPS 4
// Parallel segment downloads per poll. The origin round-trip is the bottleneck
// and segments are independent files; serial fetching could not keep up with
// the live edge, which is exactly what left the player draining to the end of
// the playlist and stalling. Matches the Swift prefetch fan-out.
#define RS_LIVE_FETCH_PAR 6

// Attempts per segment before it is written off. A segment right at the live
// edge is routinely requested a moment before the origin has finished writing
// it; giving up on the first error dropped it permanently and tore a hole in
// the timeline that surfaced as an EXT-X-DISCONTINUITY.
#define RS_LIVE_FETCH_TRIES 3
#define RS_LIVE_FETCH_RETRY_DELAY 0.4

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

typedef struct {
    struct live_stream *owner;
    char *rep_id;
    char *kind;            // "video" | "audio"
    int index;             // slot in owner->reps, used as the public URL token

    pthread_t thread;
    bool thread_started;
    bool finished;

    live_seg *segs;
    size_t nsegs, cap;
    size_t bytes;          // total held by `segs`, for the memory ceiling

    long long total_queued;    // every segment ever appended — see writePlaylist
    long long dropped_disc;    // discontinuities that rolled off the queue

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
} live_rep;

typedef struct live_stream {
    struct rs_live *mgr;
    char *id;

    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool stop;

    // Config, replaced wholesale by rs_live_start. Guarded by `mu`.
    char *mpd_url, *representation;
    char *manifest_proxy, *media_proxy;
    char *manifest_headers, *media_headers;
    char *downloader, *dl_params, *keys;
    char *segment_url_params;
    int inherit_url_params;
    int reduced_manifest_polling;
    int playlist_segments, keep_segments, download_ahead;
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
    char *manifest_headers, *media_headers;
    char *downloader, *dl_params, *keys;
    char *segment_url_params;
    int inherit_url_params;
    int reduced_manifest_polling;
    int playlist_segments, keep_segments, download_ahead;
    int playback_delay_seconds, audio_delay_ms;
    double poll_interval;
} cfg_snap;

static void cfg_snap_dispose(cfg_snap *c) {
    free(c->mpd_url); free(c->manifest_proxy); free(c->media_proxy);
    free(c->manifest_headers); free(c->media_headers);
    free(c->downloader); free(c->dl_params); free(c->keys);
    free(c->segment_url_params);
    memset(c, 0, sizeof(*c));
}

// Caller must hold st->mu.
static void cfg_snapshot_locked(const live_stream *st, cfg_snap *out) {
    memset(out, 0, sizeof(*out));
    out->mpd_url = rs_strdup(st->mpd_url ? st->mpd_url : "");
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

// One queued download and its result.
typedef struct {
    char *url;
    long long time_val;   // the manifest's $Time$, for the stable identity
    double duration;
    uint8_t *data;
    size_t len;
    bool ok;
    int attempts;
    char err[192];
} batch_item;

typedef struct {
    batch_item *items;
    size_t n, next;
    pthread_mutex_t mu;
    rs_live_fetch_fn fetch;
    const char *proxy, *headers, *downloader, *dl_params;
    live_stream *st;
} batch_ctx;

static void *batch_worker(void *arg) {
    batch_ctx *b = (batch_ctx *)arg;
    for (;;) {
        pthread_mutex_lock(&b->mu);
        size_t i = b->next < b->n ? b->next++ : b->n;
        pthread_mutex_unlock(&b->mu);
        if (i >= b->n) break;
        if (live_stopping(b->st)) break;

        batch_item *it = &b->items[i];
        // Retry before writing the segment off. Every abandoned segment becomes
        // a permanent hole in the media timeline (the window slides past it and
        // it is never offered again), so a single transient 404/timeout at the
        // live edge used to cost a real EXT-X-DISCONTINUITY.
        for (int attempt = 0; attempt < RS_LIVE_FETCH_TRIES; attempt++) {
            if (attempt > 0 && !live_wait(b->st, RS_LIVE_FETCH_RETRY_DELAY)) break;
            char *body = NULL;
            size_t len = 0;
            long status = 0;
            it->err[0] = '\0';
            int rc = b->fetch(it->url, b->proxy, b->headers, NULL, b->downloader, b->dl_params,
                              &body, &len, &status, NULL, NULL, NULL, it->err, sizeof(it->err));
            it->attempts = attempt + 1;
            if (rc == 0 && body) {
                it->data = (uint8_t *)body;
                it->len = len;
                it->ok = true;
                break;
            }
            free(body);
            if (!it->err[0]) snprintf(it->err, sizeof(it->err), "fetch failed (status %ld)", status);
        }
    }
    return NULL;
}

// Downloads every item in `items` with a bounded fan-out. Only the transfer is
// parallel; the caller still processes the results serially in timeline order,
// so decrypt / discontinuity / media-sequence behaviour is order-independent of
// how the network happened to schedule things.
static void batch_download(live_stream *st, batch_item *items, size_t n, const cfg_snap *cfg) {
    if (n == 0) return;
    batch_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.items = items;
    ctx.n = n;
    ctx.next = 0;
    ctx.fetch = st->mgr->fetch;
    ctx.proxy = cfg->media_proxy;
    ctx.headers = cfg->media_headers;
    ctx.downloader = cfg->downloader;
    ctx.dl_params = cfg->dl_params;
    ctx.st = st;
    pthread_mutex_init(&ctx.mu, NULL);

    size_t nthreads = n < RS_LIVE_FETCH_PAR ? n : RS_LIVE_FETCH_PAR;
    pthread_t threads[RS_LIVE_FETCH_PAR];
    size_t started = 0;
    for (size_t i = 0; i < nthreads; i++)
        if (pthread_create(&threads[started], NULL, batch_worker, &ctx) == 0) started++;
    if (started == 0) batch_worker(&ctx);  // no threads available: do it inline
    for (size_t i = 0; i < started; i++) pthread_join(threads[i], NULL);
    pthread_mutex_destroy(&ctx.mu);
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
                            &body, &len, &status, NULL, NULL, NULL, err, sizeof(err));
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

    lgf(st, "info", "pollStart", cfg->mpd_url, 0, -1,
        "%s: requesting %d segments (%.1fs since last poll, %.3fs each)",
        rep->rep_id, want, since_last, seg_dur);
    char *json = st->mgr->dash(cfg->mpd_url, cfg->manifest_proxy, cfg->manifest_headers,
                               cfg->downloader, cfg->dl_params, rep->rep_id,
                               want, cfg->segment_url_params, cfg->inherit_url_params,
                               err, sizeof(err));
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

    // Collect the segments this poll has not already served.
    batch_item *items = (batch_item *)calloc(total ? total : 1, sizeof(batch_item));
    if (!items) { rs_json_free(root); return; }
    size_t n = 0;
    pthread_mutex_lock(&st->mu);
    for (size_t i = 0; i < total; i++) {
        const rs_json *s = rs_json_arr_at(segs, i);
        const char *u = rs_json_obj_str(s, "url", "");
        if (!u[0]) continue;
        long long tv = (long long)rs_json_as_num(rs_json_obj_get(s, "time"), -1);
        char idkey[288];
        stable_key(u, tv, idkey, sizeof(idkey));
        if (seen_has(&rep->seen, idkey)) continue;
        items[n].url = rs_strdup(u);
        items[n].time_val = tv;
        items[n].duration = rs_json_as_num(rs_json_obj_get(s, "duration"), 0);
        n++;
    }
    pthread_mutex_unlock(&st->mu);
    rs_json_free(root);

    if (n == 0) {
        lgf(st, "info", "pollIdle", NULL, 0, -1, "%s: no new segments this poll", rep->rep_id);
        free(items);
        return;
    }

    double dl0 = now_seconds();
    batch_download(st, items, n, cfg);

    // Serial pass, strictly in timeline order: decrypt, read the media timeline
    // position, decide discontinuity, append.
    size_t added = 0, failed = 0;
    long long added_bytes = 0;
    double added_seconds = 0;
    for (size_t i = 0; i < n; i++) {
        batch_item *it = &items[i];
        char idkey[288];
        stable_key(it->url, it->time_val, idkey, sizeof(idkey));
        if (!it->ok) {
            failed++;
            // A single miss at the live edge is routine — the origin may simply
            // never produce this one. Mark it seen so we do not re-request it
            // forever, and let the window slide past it.
            lgf(st, "error", "downloadSegment", it->url, 0, -1,
                "%s: gave up after %d attempts: %s", rep->rep_id, it->attempts, it->err);
            pthread_mutex_lock(&st->mu);
            seen_add(&rep->seen, idkey);
            pthread_mutex_unlock(&st->mu);
            free(it->url);
            continue;
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
        seen_add(&rep->seen, idkey);
        pthread_mutex_unlock(&st->mu);

        added++;
        added_bytes += (long long)len;
        added_seconds += it->duration > 0 ? it->duration : 2.0;
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
            lgf(st, failed > 0 ? "error" : "info", "discontinuity", it->url, 0, -1,
                "%s: media timeline jumped %+.3fs (~%.1f segments)%s", rep->rep_id, gap,
                seg_dur > 0 ? gap / seg_dur : 0,
                failed > 0 ? " — after a failed download, so this is content we dropped"
                           : " — no download failed, so the source spliced");
        free(it->url);
    }
    free(items);

    pthread_mutex_lock(&st->mu);
    size_t before = rep->nsegs;
    rep_prune_locked(rep, cfg->keep_segments);
    size_t pruned = before - rep->nsegs;
    size_t held = rep->nsegs;
    size_t held_bytes = rep->bytes;
    long long skipped = rep->skipped;
    pthread_mutex_unlock(&st->mu);

    // Media published per second of wall clock. This is THE number for whether
    // the engine is keeping up: sustained below 1.0 means the output window
    // grows shorter than real time, so every player drains its buffer and
    // stalls regardless of how correct the segments are. Measured across the
    // whole inter-poll interval, not just the download, because the sleep
    // counts against the budget too.
    double realtime = since_last > 0 ? added_seconds / since_last : 0;
    lgf(st, added > 0 ? "info" : "error", "pollDone", NULL, 0, added_bytes,
        "%s: +%lu segments (%.1fs media) %.2fx realtime, window %d, %lu failed, %lu pruned, "
        "%lu held (%.1f MB) in %.2fs",
        rep->rep_id, (unsigned long)added, added_seconds, realtime, want,
        (unsigned long)failed, (unsigned long)pruned,
        (unsigned long)held, (double)held_bytes / (1024.0 * 1024.0), now_seconds() - dl0);

    // Only meaningful once there is a previous poll to measure against, and
    // only worth reporting when segments were actually produced (a poll that
    // legitimately found nothing new is not falling behind).
    if (since_last > 0 && added > 0 && realtime < 0.95)
        lgf(st, "error", "fallingBehind", NULL, 0, -1,
            "%s: publishing %.2fx realtime (%.1fs of media in %.1fs) — the advertised window "
            "will shrink and players will stall; %lld segments skipped so far",
            rep->rep_id, realtime, added_seconds, since_last, skipped);
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
    if (rep->init_data && rep->init_len) {
        sb_addf(&b, "#EXT-X-MAP:URI=\"/restream/%s/%d_init.mp4\"\n",
                rep->owner->id, rep->index);
    }
    for (long long i = window_start; i < window_end; i++) {
        live_seg *s = &rep->segs[i];
        // The tag marks a break *between* two segments, so it is meaningless on
        // the first one in the window — that break is already carried by
        // EXT-X-DISCONTINUITY-SEQUENCE above.
        if (s->disc && i > window_start) sb_add(&b, "#EXT-X-DISCONTINUITY\n");
        sb_addf(&b, "#EXTINF:%.3f,\n/restream/%s/%d_%lld.m4s\n",
                s->duration, rep->owner->id, rep->index, s->seq);
    }
    return b.p;
}

// --- the representation thread ---------------------------------------------

static void *rep_main(void *arg) {
    live_rep *rep = (live_rep *)arg;
    live_stream *st = rep->owner;
    double interval = 2.0;

    lgf(st, "info", "repStart", NULL, 0, -1, "%s (%s): worker started", rep->rep_id, rep->kind);
    for (;;) {
        if (live_stopping(st)) break;
        double cycle_start = now_seconds();

        cfg_snap cfg;
        pthread_mutex_lock(&st->mu);
        cfg_snapshot_locked(st, &cfg);
        rep->polls++;
        rep->last_poll = now_seconds();
        pthread_mutex_unlock(&st->mu);

        rep_poll(rep, &cfg, &interval);

        // Re-render immediately so the next playlist request is served from
        // memory with the freshest window.
        pthread_mutex_lock(&st->mu);
        char *rendered = rep_render_locked(rep, &cfg);
        if (rendered) {
            free(rep->playlist);
            rep->playlist = rendered;
            bool first = !rep->ready;
            rep->ready = true;
            pthread_cond_broadcast(&st->cv);
            pthread_mutex_unlock(&st->mu);
            if (first)
                lgf(st, "info", "playlistReady", NULL, 0, -1,
                    "%s: first media playlist is ready", rep->rep_id);
        } else {
            pthread_mutex_unlock(&st->mu);
        }

        // The poll interval is a PERIOD, not a delay. Sleeping the full interval
        // after the work meant the real cycle was interval + work: with a two
        // second minimumUpdatePeriod and a poll that takes one to three seconds
        // to fetch and decrypt its segments, the engine advanced four seconds of
        // media every four to five seconds of wall clock. That is exactly
        // break-even, so it never built a cushion, and the first slow origin
        // response put it permanently behind the live edge — the player then
        // requests segments the engine has not fetched yet, and every one of
        // those is a miss.
        double period = cfg.poll_interval > 0 ? cfg.poll_interval : interval;
        if (period > 10.0) period = 10.0;
        double spent = now_seconds() - cycle_start;
        double sleep_for = period - spent;
        // Never busy-loop, but do go straight back to the origin when a poll
        // took longer than its own period: at that point we are behind and the
        // useful thing to do is fetch again, not wait.
        if (sleep_for < 0.25) sleep_for = 0.25;
        if (spent > period)
            lgf(st, "info", "pollSlow", NULL, 0, -1,
                "%s: poll took %.2fs for a %.2fs period — polling again immediately",
                rep->rep_id, spent, period);

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
        char *json = st->mgr->dash(cfg.mpd_url, cfg.manifest_proxy, cfg.manifest_headers,
                                   cfg.downloader, cfg.dl_params, "", 0,
                                   cfg.segment_url_params, cfg.inherit_url_params, err, sizeof(err));
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
        bool have_video = video && rs_json_type_of(video) == RS_JSON_OBJ;
        bool have_audio = audio && rs_json_type_of(audio) == RS_JSON_OBJ;
        bool dynamic = rs_json_as_bool(rs_json_obj_get(root, "dynamic"), true);

        // A pinned representation overrides the auto-selected video rendition;
        // the audio rendition is still picked up so the master keeps its
        // separate audio group.
        const char *vid = have_video ? rs_json_obj_str(video, "id", "") : "";
        if (pinned && pinned[0]) vid = pinned;
        const char *vcodecs = have_video ? rs_json_obj_str(video, "codecs", "") : "";
        long long vbw = have_video ? (long long)rs_json_as_num(rs_json_obj_get(video, "bandwidth"), 0) : 0;
        const char *aid = have_audio ? rs_json_obj_str(audio, "id", "") : "";
        const char *acodecs = have_audio ? rs_json_obj_str(audio, "codecs", "") : "";
        if (aid[0] && vid[0] && strcmp(aid, vid) == 0) { aid = ""; acodecs = ""; have_audio = false; }

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
            sb_addf(&b, "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"Audio\",DEFAULT=YES,"
                        "AUTOSELECT=YES,URI=\"/play/%s/index.m3u8?rep=%s&mtype=audio\"\n",
                    st->id, aenc ? aenc : "");
            free(aenc);
        }
        sb_addf(&b, "#EXT-X-STREAM-INF:BANDWIDTH=%lld", vbw > 0 ? vbw : 3000000);
        if (vcodecs[0] && acodecs[0]) sb_addf(&b, ",CODECS=\"%s,%s\"", vcodecs, acodecs);
        else if (vcodecs[0]) sb_addf(&b, ",CODECS=\"%s\"", vcodecs);
        if (have_audio && aid[0]) sb_add(&b, ",AUDIO=\"aud\"");
        char *venc = qenc(vid);
        sb_addf(&b, "\n/play/%s/index.m3u8?rep=%s&mtype=video\n", st->id, venc ? venc : "");
        free(venc);

        pthread_mutex_lock(&st->mu);
        bool first = st->master == NULL;
        free(st->master);
        st->master = b.p;  // ownership moves to the stream
        rep_ensure_locked(st, vid, "video");
        if (have_audio && aid[0]) rep_ensure_locked(st, aid, "audio");
        pthread_cond_broadcast(&st->cv);
        pthread_mutex_unlock(&st->mu);

        if (first)
            lgf(st, "info", "renditions", cfg.mpd_url, 0, -1,
                "%s MPD — video \"%s\"%s%s%s, master playlist ready",
                dynamic ? "dynamic" : "static", vid,
                (have_audio && aid[0]) ? ", audio \"" : "",
                (have_audio && aid[0]) ? aid : "",
                (have_audio && aid[0]) ? "\"" : "");

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
    st->inherit_url_params = cfg->inherit_url_params;
    st->reduced_manifest_polling = cfg->reduced_manifest_polling;
    st->playlist_segments = cfg->playlist_segments > 0 ? cfg->playlist_segments : 6;
    if (st->playlist_segments < 3) st->playlist_segments = 3;
    st->download_ahead = cfg->download_ahead > 0 ? cfg->download_ahead : 8;
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
        // Already running: re-apply the config, which the workers pick up on
        // their next poll. Restarting would throw away a warm segment queue.
        pthread_mutex_lock(&st->mu);
        stream_apply_config_locked(st, cfg);
        pthread_mutex_unlock(&st->mu);
        pthread_mutex_unlock(&live->mu);
        return 0;
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
static bool stream_ready_locked(const live_stream *st) {
    if (!st->master || st->nreps == 0) return false;
    for (size_t i = 0; i < st->nreps; i++)
        if (!st->reps[i]->ready) return false;
    return true;
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
char *rs_live_status_line(rs_live *live, const char *stream_id) { (void)live; (void)stream_id; return NULL; }

#endif  // _WIN32
