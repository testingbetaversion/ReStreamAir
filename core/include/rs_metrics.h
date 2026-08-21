#ifndef RS_METRICS_H
#define RS_METRICS_H

#include "rs_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rs_metrics rs_metrics;

rs_metrics* rs_metrics_create(void);
void rs_metrics_destroy(rs_metrics *m);

// Record bytes served to a client
void rs_metrics_record(rs_metrics *m, const char *stream_id, const char *identity,
                       const char *client_ip, const char *user_agent, int bytes);

// Record bytes pulled FROM the origin for a stream: segment and init downloads,
// whether a viewer asked for them or the live engine fetched them with nobody
// watching. Kept separate from rs_metrics_record because the two are genuinely
// different quantities — an engine holding a stream open with no clients
// connected costs inbound bandwidth and serves nothing — and reporting the
// served rate as both is what made the panel's down arrow read 0 B/s while the
// engine was pulling a live stream at several megabits.
void rs_metrics_record_input(rs_metrics *m, const char *stream_id, long long bytes);

// Record an error for a client
void rs_metrics_record_error(rs_metrics *m, const char *identity);

// Prune expired connections (call periodically, e.g. every second)
void rs_metrics_prune(rs_metrics *m);

// Get active client count for a stream (seen within 45s window)
int rs_metrics_active_clients(const rs_metrics *m, const char *stream_id);

// Get current bytes/sec rate for a stream (10s sliding window)
double rs_metrics_bytes_per_sec(const rs_metrics *m, const char *stream_id);

// Get total bytes served for a stream since metrics creation
int64_t rs_metrics_total_bytes(const rs_metrics *m, const char *stream_id);

// The inbound counterparts of the two calls above, over the same windows.
double rs_metrics_input_bytes_per_sec(const rs_metrics *m, const char *stream_id);
int64_t rs_metrics_input_total_bytes(const rs_metrics *m, const char *stream_id);

typedef void (*rs_metrics_connection_fn)(void *ctx, const char *stream_id,
                                         const char *identity, const char *client_ip,
                                         const char *user_agent, long long uptime_seconds,
                                         int errors, double bytes_per_second,
                                         int64_t total_bytes);
void rs_metrics_each_connection(const rs_metrics *m, rs_metrics_connection_fn fn, void *ctx);

#ifdef __cplusplus
}
#endif

#endif  // RS_METRICS_H
