// How wide a manifest window one poll has to ask for.
//
// Split out of live.c because it is the whole of a bug that was invisible for
// as long as it lived inside a worker thread, and because it is pure: no
// sockets, no threads, no clock. That lets it sit in restream_base and be
// checked by the self-test with plain numbers.
//
// The DASH describe call is anchored at the live edge — it returns the NEWEST
// `n` segments of the representation. A fixed `n` therefore only ever covers
// `n * segment_duration` seconds of source output. Whenever one poll cycle took
// longer than that (a slow origin, large segments, a proxy in the path), every
// segment older than the returned window was never requested, and never would
// be: the next poll re-anchors at the live edge, further along again. The
// engine published less media than real time, so its advertised window shrank
// until players drained their buffers and stalled, and each hole surfaced as an
// EXT-X-DISCONTINUITY that looked exactly like ordinary ad-break splicing.
//
// So the window has to be a function of elapsed time, not a constant.
// Overshooting is free: the caller's `seen` set skips anything already held, so
// the only cost is a slightly larger manifest response.

#include "rs_live.h"

int rs_live_window_size(int download_ahead, double since_last, double seg_duration) {
    int want = download_ahead > 0 ? download_ahead : 1;
    if (seg_duration <= 0) seg_duration = 2.0;
    // since_last <= 0 is a representation's first poll: there is no backlog to
    // cover, so start at the live edge with the configured ask rather than
    // dragging in the whole DVR buffer.
    if (since_last > 0) {
        // +1 because a partly elapsed segment period still means one more
        // segment has been published, and the margin absorbs jitter in when the
        // origin actually publishes.
        int needed = (int)(since_last / seg_duration) + 1 + RS_LIVE_WINDOW_MARGIN;
        if (needed > want) want = needed;
    }
    if (want > RS_LIVE_MAX_WINDOW) want = RS_LIVE_MAX_WINDOW;
    return want;
}

int rs_live_lag_level(int started, double realtime, long long newly_skipped,
                      int *consecutive_slow_windows) {
    if (!consecutive_slow_windows) return 0;
    if (!started || realtime >= 0.95) {
        *consecutive_slow_windows = 0;
        return 0;
    }
    if (*consecutive_slow_windows < 1000) (*consecutive_slow_windows)++;
    if (newly_skipped > 0 || *consecutive_slow_windows >= 2) return 2;
    return 1;
}

void rs_live_catch_up_plan(int depth, int inflight, int waiting, int fresh,
                           int *evict_waiting, int *drop_fresh) {
    if (evict_waiting) *evict_waiting = 0;
    if (drop_fresh) *drop_fresh = 0;
    if (depth < 1) depth = 1;
    if (inflight < 0) inflight = 0;
    if (waiting < 0) waiting = 0;
    if (fresh < 0) fresh = 0;

    // What is in flight cannot be given back, so it eats into the budget.
    int room = depth - inflight;
    if (room < 0) room = 0;

    int total = waiting + fresh;
    if (total <= room) return;  // it all fits; nothing to throw away

    // Keep the newest `room` of (waiting ++ fresh), which are ordered oldest to
    // newest. Fresh are newer than anything already queued, so they are kept
    // first and the older queued ones give way.
    int keep = room;
    int fresh_kept = fresh < keep ? fresh : keep;
    int waiting_kept = keep - fresh_kept;
    if (waiting_kept > waiting) waiting_kept = waiting;

    if (drop_fresh) *drop_fresh = fresh - fresh_kept;
    if (evict_waiting) *evict_waiting = waiting - waiting_kept;
}
