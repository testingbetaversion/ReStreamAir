#ifndef RS_FILE_GATE_H
#define RS_FILE_GATE_H

// Bounded static-file serving on top of mongoose.
//
// mongoose keeps every file it serves open until the client has read the last
// byte, so a burst of viewers — or a handful of stalled ones — can run the
// process out of descriptors ("Too many open files"), after which accept(),
// curl and ffmpeg pipes all start failing too. The gate puts three bounds on
// that:
//
//   * at most `max_open` served files are open at once; a request over the
//     limit (or one whose open() itself hit EMFILE/ENFILE) waits in a FIFO
//     queue and is served as soon as a slot frees up;
//   * a queued request that waits longer than `queue_timeout_ms` gets a
//     503 + Retry-After instead of hanging, as does one arriving to a full
//     queue;
//   * a served file held open longer than `open_timeout_ms` (a client that
//     stopped reading) has its connection closed, which releases the file.
//
// Everything here runs on the mongoose event-loop thread; the gate is not
// thread-safe and does not need to be.

#include "../deps/mongoose.h"
#include <stddef.h>
#include <stdint.h>

typedef struct rs_file_gate rs_file_gate;

typedef struct {
    size_t max_open;            // served files open at once; 0 = derive from RLIMIT_NOFILE
    size_t max_queued;          // requests allowed to wait; 0 = 4 x max_open
    uint64_t queue_timeout_ms;  // longest a request may wait for a slot; 0 = 15 s
    uint64_t open_timeout_ms;   // longest a served file may stay open; 0 = 60 s
} rs_file_gate_limits;

typedef struct {
    size_t open;          // served files open right now
    size_t queued;        // requests waiting for a slot
    size_t max_open;      // effective limits after defaults
    size_t max_queued;
    uint64_t rejected;    // 503s: queue full or waited too long (cumulative)
    uint64_t timed_out;   // connections closed for holding a file too long (cumulative)
} rs_file_gate_stats;

rs_file_gate *rs_file_gate_create(struct mg_mgr *mgr);
void rs_file_gate_destroy(rs_file_gate *g);
void rs_file_gate_set_limits(rs_file_gate *g, const rs_file_gate_limits *limits);
void rs_file_gate_get_stats(const rs_file_gate *g, rs_file_gate_stats *out);

// Drop-in replacements for mg_http_serve_file / mg_http_serve_dir. The reply
// may be deferred; opts' strings are copied, so they may live on the stack.
void rs_file_gate_serve_file(rs_file_gate *g, struct mg_connection *c,
                             struct mg_http_message *hm, const char *path,
                             const struct mg_http_serve_opts *opts);
void rs_file_gate_serve_dir(rs_file_gate *g, struct mg_connection *c,
                            struct mg_http_message *hm,
                            const struct mg_http_serve_opts *opts);

// Call often (every ~100 ms): serves queued requests that now fit, answers the
// ones that waited too long, and closes connections that held a file too long.
// Returns how many connections were closed for the open timeout on this call.
size_t rs_file_gate_tick(rs_file_gate *g);

// Call from MG_EV_CLOSE so a queued request for a closed connection is dropped.
void rs_file_gate_forget(rs_file_gate *g, unsigned long conn_id);

#endif
