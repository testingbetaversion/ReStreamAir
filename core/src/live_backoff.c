// How long to wait before re-polling a manifest that just failed.
//
// Split out of live.c for the same reason as live_window.c: it is pure — no
// sockets, no threads, no clock — so it sits in restream_base where the
// self-test can check it with plain numbers.
//
// Without this the poll loop treated a failed manifest exactly like a
// successful one: sleep the configured period (commonly 2s) and ask again. A
// fast failure is *worse* than a slow success there, because the loop spends no
// time on the work and comes straight back. Against an origin that rate-limits
// by IP that is a feedback loop — livesim2.dashif.org answered a real stream's
// polls with HTTP 429 in 40ms, and because a video worker, an audio worker and
// the director each retried on their own 2s cadence, the limiter stayed armed
// indefinitely. The stream had gone 364 seconds without a successful poll while
// still generating ~1.5 requests/second.
//
// So: exponential backoff on *consecutive* failures, reset on the first
// success. Any repeated failure earns it, whatever the cause — hammering an
// origin that is timing out is no more useful than hammering one that is
// throttling. A response that explicitly says "too many requests" additionally
// starts at a floor well above the poll period, because with those the request
// rate is the thing being complained about and shaving it slightly will not
// clear the block.
//
// Backing off does not lose media. The window anchor only advances on a
// retrieved window (see rep_poll), so however long the gap, the next successful
// poll sizes its ask to the whole elapsed time and pulls the backlog.

#include "rs_live.h"

double rs_live_backoff_delay(int consecutive_failures, int throttled, double period) {
    if (consecutive_failures <= 0) return 0;  // last poll succeeded: normal cadence
    if (period <= 0) period = 2.0;

    // Double per consecutive failure: period, 2*period, 4*period, ...
    // Shift rather than pow() to keep this integer-exact, and stop shifting well
    // before the exponent could overflow — the cap below dominates long first.
    int steps = consecutive_failures - 1;
    if (steps > 20) steps = 20;
    double delay = period * (double)(1u << steps);

    // An explicit throttle response is about request *rate*, so start where it
    // can actually help rather than at the poll period we already know is too
    // fast.
    if (throttled && delay < RS_LIVE_THROTTLE_BACKOFF_MIN)
        delay = RS_LIVE_THROTTLE_BACKOFF_MIN;

    if (delay > RS_LIVE_BACKOFF_MAX) delay = RS_LIVE_BACKOFF_MAX;
    return delay;
}

int rs_live_status_is_throttle(long status) {
    // 429 is the explicit one. 503 is what a CDN edge returns when it is
    // shedding load, and 403 is what several IPTV origins return once a
    // concurrent-session or rate cap trips — all three mean "ask less often",
    // and none of them is fixed by asking again in two seconds.
    return status == 429 || status == 503 || status == 403;
}
