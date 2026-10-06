// Bounded static-file serving: an open-file limit with a wait queue, and a
// timeout on files held open by clients that stopped reading. See
// rs_file_gate.h for the why.
//
// mongoose's file callbacks (struct mg_fs) carry no context pointer, so the
// gate hands mongoose its own mg_fs whose open/close count against whichever
// gate is serving at that moment (s_serving). Serving is synchronous on the
// event-loop thread, so that pointer is only ever set for the duration of one
// mg_http_serve_* call.

#include "rs_file_gate.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/resource.h>
#endif

#define GATE_DEFAULT_QUEUE_TIMEOUT_MS 15000
#define GATE_DEFAULT_OPEN_TIMEOUT_MS  60000
#define GATE_MIN_OPEN 16
#define GATE_MAX_OPEN 8192

// One file mongoose has open on the gate's behalf.
typedef struct gate_file {
    FILE *fp;
    rs_file_gate *gate;       // NULL once the gate is gone
    unsigned long conn_id;    // 0 until the serve call that opened it returns
    uint64_t opened_ms;
    bool expired;             // its connection was told to close
    struct gate_file *prev, *next;
} gate_file;

// One request waiting for a slot. The request head is copied and re-parsed on
// dispatch because mongoose discards the original once the handler returns.
typedef struct gate_wait {
    unsigned long conn_id;
    uint64_t since_ms;
    bool dir;
    char *path;
    char *root_dir, *ssi_pattern, *extra_headers, *mime_types, *page404;
    char *head;
    size_t head_len;
    struct gate_wait *next;
} gate_wait;

struct rs_file_gate {
    struct mg_mgr *mgr;
    rs_file_gate_limits limits;   // effective, defaults applied
    size_t open;
    gate_file *files;
    gate_wait *queue_head, *queue_tail;
    size_t queued;
    uint64_t rejected, timed_out;
};

static rs_file_gate *s_serving;      // gate inside the current serve call
static gate_file *s_last_opened;     // still-open file from the current serve call
static FILE *s_preopen;              // already-open handle for s_preopen_path
static const char *s_preopen_path;

// --- the mg_fs mongoose reads through -----------------------------------------

static void *gate_op(const char *path, int flags) {
    FILE *fp = NULL;
    if (s_preopen && s_preopen_path && strcmp(path, s_preopen_path) == 0) {
        fp = s_preopen;
        s_preopen = NULL;
    } else {
        fp = fopen(path, (flags & MG_FS_WRITE) ? "wb" : "rb");
    }
    if (!fp) return NULL;
    gate_file *f = (gate_file *)calloc(1, sizeof(*f));
    if (!f) { fclose(fp); return NULL; }
    f->fp = fp;
    f->opened_ms = mg_millis();
    f->gate = s_serving;
    if (f->gate) {
        f->next = f->gate->files;
        if (f->next) f->next->prev = f;
        f->gate->files = f;
        f->gate->open++;
    }
    s_last_opened = f;
    return f;
}

static void gate_cl(void *fd) {
    gate_file *f = (gate_file *)fd;
    if (!f) return;
    if (s_last_opened == f) s_last_opened = NULL;
    if (f->gate) {
        if (f->prev) f->prev->next = f->next;
        else f->gate->files = f->next;
        if (f->next) f->next->prev = f->prev;
        f->gate->open--;
    }
    fclose(f->fp);
    free(f);
}

static size_t gate_rd(void *fd, void *buf, size_t len) {
    return fread(buf, 1, len, ((gate_file *)fd)->fp);
}

static size_t gate_wr(void *fd, const void *buf, size_t len) {
    return fwrite(buf, 1, len, ((gate_file *)fd)->fp);
}

static size_t gate_sk(void *fd, size_t offset) {
    FILE *fp = ((gate_file *)fd)->fp;
    if (fseek(fp, (long)offset, SEEK_SET) != 0) return 0;
    long pos = ftell(fp);
    return pos < 0 ? 0 : (size_t)pos;
}

static int gate_st(const char *path, size_t *size, time_t *mtime) {
    return mg_fs_posix.st(path, size, mtime);
}
static void gate_ls(const char *path, void (*fn)(const char *, void *), void *arg) {
    mg_fs_posix.ls(path, fn, arg);
}
static bool gate_mv(const char *from, const char *to) { return mg_fs_posix.mv(from, to); }
static bool gate_rm(const char *path) { return mg_fs_posix.rm(path); }
static bool gate_mkd(const char *path) { return mg_fs_posix.mkd(path); }

static struct mg_fs s_gate_fs = {
    gate_st, gate_ls, gate_op, gate_cl, gate_rd, gate_wr, gate_sk, gate_mv, gate_rm, gate_mkd,
};

// --- limits --------------------------------------------------------------------

static size_t default_max_open(void) {
#ifdef _WIN32
    return 256;  // the CRT allows 512 FILE streams by default
#else
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0 || rl.rlim_cur == RLIM_INFINITY) return GATE_MAX_OPEN;
    // Every served file comes with its socket, and the rest of the server
    // (curl, ffmpeg pipes, logs, live segments) needs descriptors too.
    size_t n = (size_t)rl.rlim_cur / 4;
    if (n < GATE_MIN_OPEN) n = GATE_MIN_OPEN;
    if (n > GATE_MAX_OPEN) n = GATE_MAX_OPEN;
    return n;
#endif
}

void rs_file_gate_set_limits(rs_file_gate *g, const rs_file_gate_limits *in) {
    if (!g) return;
    rs_file_gate_limits l = {0};
    if (in) l = *in;
    if (!l.max_open) l.max_open = default_max_open();
    if (!l.max_queued) l.max_queued = l.max_open * 4;
    if (!l.queue_timeout_ms) l.queue_timeout_ms = GATE_DEFAULT_QUEUE_TIMEOUT_MS;
    if (!l.open_timeout_ms) l.open_timeout_ms = GATE_DEFAULT_OPEN_TIMEOUT_MS;
    g->limits = l;
}

rs_file_gate *rs_file_gate_create(struct mg_mgr *mgr) {
    rs_file_gate *g = (rs_file_gate *)calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->mgr = mgr;
    rs_file_gate_set_limits(g, NULL);
    return g;
}

void rs_file_gate_get_stats(const rs_file_gate *g, rs_file_gate_stats *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!g) return;
    out->open = g->open;
    out->queued = g->queued;
    out->max_open = g->limits.max_open;
    out->max_queued = g->limits.max_queued;
    out->rejected = g->rejected;
    out->timed_out = g->timed_out;
}

// --- serving -------------------------------------------------------------------

static void wait_free(gate_wait *w) {
    if (!w) return;
    free(w->path); free(w->root_dir); free(w->ssi_pattern);
    free(w->extra_headers); free(w->mime_types); free(w->page404);
    free(w->head);
    free(w);
}

void rs_file_gate_destroy(rs_file_gate *g) {
    if (!g) return;
    while (g->queue_head) {
        gate_wait *w = g->queue_head;
        g->queue_head = w->next;
        wait_free(w);
    }
    // Normally none are left (mg_mgr_free closes them first); any that are
    // stay valid for mongoose but stop counting against a freed gate.
    for (gate_file *f = g->files; f; f = f->next) f->gate = NULL;
    free(g);
}

static struct mg_connection *find_conn(rs_file_gate *g, unsigned long id) {
    for (struct mg_connection *c = g->mgr->conns; c; c = c->next)
        if (c->id == id) return c;
    return NULL;
}

static void run_serve(rs_file_gate *g, struct mg_connection *c, struct mg_http_message *hm,
                      const char *path, const struct mg_http_serve_opts *opts) {
    struct mg_http_serve_opts o = *opts;
    o.fs = &s_gate_fs;
    s_serving = g;
    s_last_opened = NULL;
    if (path) mg_http_serve_file(c, hm, path, &o);
    else mg_http_serve_dir(c, hm, &o);
    // Anything opened and closed again (404, 304, HEAD) already cleared
    // s_last_opened; what is left is the file mongoose is now streaming.
    if (s_last_opened) s_last_opened->conn_id = c->id;
    s_last_opened = NULL;
    s_serving = NULL;
}

// Serves now unless the process is out of descriptors, in which case it returns
// false without replying so the caller can queue the request.
static bool try_serve(rs_file_gate *g, struct mg_connection *c, struct mg_http_message *hm,
                      const char *path, const struct mg_http_serve_opts *opts) {
    if (path) {
        // Open first so EMFILE/ENFILE can be told apart from a missing file;
        // mongoose itself would answer both with a 404.
        FILE *fp = fopen(path, "rb");
        if (!fp && (errno == EMFILE || errno == ENFILE)) return false;
        s_preopen = fp;
        s_preopen_path = path;
        run_serve(g, c, hm, path, opts);
        if (s_preopen) fclose(s_preopen);  // mongoose served the .gz instead
        s_preopen = NULL;
        s_preopen_path = NULL;
    } else {
        run_serve(g, c, hm, NULL, opts);
    }
    return true;
}

static char *dup_or_null(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static void reply_busy(struct mg_connection *c, const char *extra_headers) {
    char headers[1024];
    snprintf(headers, sizeof(headers), "%sContent-Type: application/json\r\nRetry-After: 1\r\n",
             extra_headers ? extra_headers : "");
    mg_http_reply(c, 503, headers, "{\"error\":\"Server is busy serving files. Retry shortly.\"}");
}

static void enqueue(rs_file_gate *g, struct mg_connection *c, struct mg_http_message *hm,
                    const char *path, const struct mg_http_serve_opts *opts) {
    if (g->queued >= g->limits.max_queued) {
        g->rejected++;
        reply_busy(c, opts->extra_headers);
        return;
    }
    gate_wait *w = (gate_wait *)calloc(1, sizeof(*w));
    if (w) {
        w->conn_id = c->id;
        w->since_ms = mg_millis();
        w->dir = path == NULL;
        w->path = dup_or_null(path);
        w->root_dir = dup_or_null(opts->root_dir);
        w->ssi_pattern = dup_or_null(opts->ssi_pattern);
        w->extra_headers = dup_or_null(opts->extra_headers);
        w->mime_types = dup_or_null(opts->mime_types);
        w->page404 = dup_or_null(opts->page404);
        w->head = (char *)malloc(hm->head.len + 1);
        w->head_len = hm->head.len;
    }
    if (!w || !w->head || (path && !w->path) || (opts->root_dir && !w->root_dir) ||
        (opts->extra_headers && !w->extra_headers)) {
        wait_free(w);
        g->rejected++;
        reply_busy(c, opts->extra_headers);
        return;
    }
    memcpy(w->head, hm->head.buf, hm->head.len);
    w->head[hm->head.len] = '\0';
    // mongoose leaves c->is_resp set while the reply is outstanding, which
    // also holds back any pipelined request on this connection until then.
    if (g->queue_tail) g->queue_tail->next = w;
    else g->queue_head = w;
    g->queue_tail = w;
    g->queued++;
}

static void serve(rs_file_gate *g, struct mg_connection *c, struct mg_http_message *hm,
                  const char *path, const struct mg_http_serve_opts *opts) {
    // FIFO: nobody jumps ahead of a request that is already waiting.
    if (g->queue_head || g->open >= g->limits.max_open || !try_serve(g, c, hm, path, opts))
        enqueue(g, c, hm, path, opts);
}

void rs_file_gate_serve_file(rs_file_gate *g, struct mg_connection *c,
                             struct mg_http_message *hm, const char *path,
                             const struct mg_http_serve_opts *opts) {
    if (!g) { mg_http_serve_file(c, hm, path, opts); return; }
    serve(g, c, hm, path, opts);
}

void rs_file_gate_serve_dir(rs_file_gate *g, struct mg_connection *c,
                            struct mg_http_message *hm,
                            const struct mg_http_serve_opts *opts) {
    if (!g) { mg_http_serve_dir(c, hm, opts); return; }
    serve(g, c, hm, NULL, opts);
}

void rs_file_gate_forget(rs_file_gate *g, unsigned long conn_id) {
    if (!g) return;
    gate_wait **link = &g->queue_head, *prev = NULL;
    while (*link) {
        gate_wait *w = *link;
        if (w->conn_id == conn_id) {
            *link = w->next;
            if (g->queue_tail == w) g->queue_tail = prev;
            g->queued--;
            wait_free(w);
        } else {
            prev = w;
            link = &w->next;
        }
    }
}

static gate_wait *pop_head(rs_file_gate *g) {
    gate_wait *w = g->queue_head;
    if (!w) return NULL;
    g->queue_head = w->next;
    if (!g->queue_head) g->queue_tail = NULL;
    w->next = NULL;
    g->queued--;
    return w;
}

size_t rs_file_gate_tick(rs_file_gate *g) {
    if (!g) return 0;
    uint64_t now = mg_millis();

    // Requests that waited too long get an answer rather than a hung player.
    while (g->queue_head && now - g->queue_head->since_ms >= g->limits.queue_timeout_ms) {
        gate_wait *w = pop_head(g);
        struct mg_connection *c = find_conn(g, w->conn_id);
        if (c) { g->rejected++; reply_busy(c, w->extra_headers); }
        wait_free(w);
    }

    // Serve waiting requests, oldest first, while there is room.
    while (g->queue_head && g->open < g->limits.max_open) {
        gate_wait *w = pop_head(g);
        struct mg_connection *c = find_conn(g, w->conn_id);
        struct mg_http_message hm;
        if (!c || c->is_closing || mg_http_parse(w->head, w->head_len, &hm) <= 0) {
            wait_free(w);
            continue;
        }
        struct mg_http_serve_opts opts = {0};
        opts.root_dir = w->root_dir;
        opts.ssi_pattern = w->ssi_pattern;
        opts.extra_headers = w->extra_headers;
        opts.mime_types = w->mime_types;
        opts.page404 = w->page404;
        if (!try_serve(g, c, &hm, w->dir ? NULL : w->path, &opts)) {
            // Still out of descriptors (something other than served files is
            // holding them): put it back at the front and try next tick.
            w->next = g->queue_head;
            g->queue_head = w;
            if (!g->queue_tail) g->queue_tail = w;
            g->queued++;
            break;
        }
        wait_free(w);
    }

    // Files held open too long: close their connections, which closes the file.
    size_t closed = 0;
    for (gate_file *f = g->files; f; f = f->next) {
        if (!f->conn_id || f->expired || now - f->opened_ms < g->limits.open_timeout_ms) continue;
        f->expired = true;
        struct mg_connection *c = find_conn(g, f->conn_id);
        if (c) { c->is_closing = 1; closed++; }
    }
    g->timed_out += closed;
    return closed;
}
