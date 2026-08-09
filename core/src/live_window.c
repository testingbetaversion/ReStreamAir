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
