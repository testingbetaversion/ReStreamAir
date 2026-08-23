#include "ffrun.h"

#include "rs_common.h"

#include "rs_proc.h"
#include "rs_thread.h"   // clock_gettime, on the platforms that lack it

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RS_FFRUN_MAX_STREAMS 64

// A process that exits immediately, over and over, is misconfigured rather than
// unlucky — a wrong key, a source that 404s, a bad output target. Backing off
// keeps a broken stream from spawning hundreds of processes a minute while still
// recovering quickly from a genuine blip.
#define RS_FFRUN_BACKOFF_BASE   1.0
#define RS_FFRUN_BACKOFF_MAX   30.0
// Under this many seconds of uptime, an exit counts as "failed to start" and
// feeds the backoff. Above it, the process ran and the restart is immediate.
#define RS_FFRUN_HEALTHY_AFTER  10.0

// ffmpeg writes its progress to stderr, several lines a second. Logging all of
// it would bury everything else in the Logs tab, so a line is only recorded when
// it looks like a diagnostic rather than a progress tick.
#define RS_FFRUN_LINE_MAX 512

typedef struct {
    bool used;
    char *stream_id;

    rs_proc proc;       // the main process (ffmpeg)
    rs_proc feeder;     // optional producer piped into its stdin
    rs_fd stderr_fd;    // read end of its stderr, read without blocking

    char **argv;
    char **feeder_argv;
    char **env_keys;
    char **env_values;
    size_t env_count;

    double started_at;
    int restarts;
    int consecutive_failures;
    double retry_at;    // 0 = eligible now
    bool stopping;

    char line[RS_FFRUN_LINE_MAX];
    size_t line_len;
} ffrun_entry;

struct rs_ffrun {
    ffrun_entry entries[RS_FFRUN_MAX_STREAMS];
    rs_ffrun_log_fn log;
    void *log_ctx;
};

static double now_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void lg(rs_ffrun *r, const char *sid, const char *level, const char *event,
               const char *msg) {
    if (r && r->log) r->log(r->log_ctx, sid, level, event, msg ? msg : "");
}

static void lgf(rs_ffrun *r, const char *sid, const char *level, const char *event,
                const char *fmt, ...) {
    if (!r || !r->log) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lg(r, sid, level, event, buf);
}

static char **argv_copy(const char *const *argv) {
    if (!argv) return NULL;
    size_t n = 0;
    while (argv[n]) n++;
    char **out = (char **)calloc(n + 1, sizeof(char *));
    if (!out) return NULL;
    for (size_t i = 0; i < n; i++) {
        out[i] = rs_strdup(argv[i]);
        if (!out[i]) {
            for (size_t j = 0; j < i; j++) free(out[j]);
            free(out);
            return NULL;
        }
    }
    return out;
}

static void argv_free(char **argv) {
    if (!argv) return;
    for (size_t i = 0; argv[i]; i++) free(argv[i]);
    free(argv);
}

static char **strv_copy_n(const char *const *values, size_t count) {
    if (!values || count == 0) return NULL;
    char **out = (char **)calloc(count + 1, sizeof(char *));
    if (!out) return NULL;
    for (size_t i = 0; i < count; i++) {
        out[i] = rs_strdup(values[i] ? values[i] : "");
        if (!out[i]) { argv_free(out); return NULL; }
    }
    return out;
}

// Joins argv for a log line, with anything that looks like a key or a token
// elided — these lines go to the panel's Logs tab, which is a place an operator
// pastes into a bug report.
static char *argv_describe(char *const *argv) {
    if (!argv) return rs_strdup("");
    size_t need = 1;
    for (size_t i = 0; argv[i]; i++) need += strlen(argv[i]) + 1;
    char *out = (char *)malloc(need);
    if (!out) return NULL;
    out[0] = '\0';
    bool redact_next = false;
    for (size_t i = 0; argv[i]; i++) {
        if (i) strcat(out, " ");
        if (redact_next) {
            strcat(out, "<redacted>");
            redact_next = false;
            continue;
        }
        strcat(out, argv[i]);
        // The value after any of these is a secret.
        redact_next = strstr(argv[i], "decryption_key") != NULL ||
                      strstr(argv[i], "-key") != NULL ||
                      strcmp(argv[i], "-headers") == 0;
    }
    return out;
}

// Spawns the pipeline. Returns 0 on success and fills proc/feeder/stderr_fd.
static int spawn_pipeline(rs_ffrun *r, ffrun_entry *e) {
    rs_pipe err_pipe = { RS_FD_INVALID, RS_FD_INVALID };
    rs_pipe feed_pipe = { RS_FD_INVALID, RS_FD_INVALID };
    char **child_env = NULL;
    char errbuf[256];
    rs_stdio in_io, out_io, err_io;
    int rc;

    if (rs_pipe_open(&err_pipe) != 0) {
        lg(r, e->stream_id, "error", "ffmpegSpawn", "could not create a pipe");
        return -1;
    }
    if (e->feeder_argv && rs_pipe_open(&feed_pipe) != 0) {
        lg(r, e->stream_id, "error", "ffmpegSpawn", "could not create a pipe");
        rs_fd_close(err_pipe.read_end); rs_fd_close(err_pipe.write_end);
        return -1;
    }

    child_env = rs_env_build((const char *const *)e->env_keys,
                             (const char *const *)e->env_values, e->env_count);
    if (!child_env) {
        rs_fd_close(err_pipe.read_end); rs_fd_close(err_pipe.write_end);
        rs_fd_close(feed_pipe.read_end); rs_fd_close(feed_pipe.write_end);
        return -1;
    }

    if (e->feeder_argv) {
        // Producer: stdout -> the pipe ffmpeg reads, stderr -> the same log pipe
        // so a failing streamlink is as visible as a failing ffmpeg.
        in_io  = rs_stdio_null();
        out_io = rs_stdio_fd(feed_pipe.write_end);
        err_io = rs_stdio_fd(err_pipe.write_end);
        rc = rs_proc_spawn(&e->feeder, (const char *const *)e->feeder_argv,
                           (const char *const *)child_env, &in_io, &out_io, &err_io,
                           errbuf, sizeof(errbuf));
        if (rc != 0) {
            lg(r, e->stream_id, "error", "ffmpegSpawn", errbuf);
            rs_fd_close(err_pipe.read_end); rs_fd_close(err_pipe.write_end);
            rs_fd_close(feed_pipe.read_end); rs_fd_close(feed_pipe.write_end);
            rs_env_free(child_env);
            return -1;
        }
    }

    // No producer: give ffmpeg the null device rather than the server's stdin,
    // or its "press [q] to stop" reader eats the terminal.
    in_io  = e->feeder_argv ? rs_stdio_fd(feed_pipe.read_end) : rs_stdio_null();
    out_io = rs_stdio_inherit();
    err_io = rs_stdio_fd(err_pipe.write_end);
    rc = rs_proc_spawn(&e->proc, (const char *const *)e->argv,
                       (const char *const *)child_env, &in_io, &out_io, &err_io,
                       errbuf, sizeof(errbuf));
    rs_env_free(child_env);

    // The parent keeps only the read end of stderr and neither end of the feed
    // pipe. Holding a write end here would keep stderr from ever reaching
    // end-of-file, so a dead ffmpeg would never be noticed.
    rs_fd_close(err_pipe.write_end);
    rs_fd_close(feed_pipe.read_end);
    rs_fd_close(feed_pipe.write_end);

    if (rc != 0) {
        lg(r, e->stream_id, "error", "ffmpegSpawn", errbuf);
        rs_fd_close(err_pipe.read_end);
        if (rs_proc_valid(&e->feeder)) {
            rs_proc_kill(&e->feeder);
            rs_proc_wait(&e->feeder, 0, NULL, NULL);
            rs_proc_release(&e->feeder);
        }
        return -1;
    }

    e->stderr_fd = err_pipe.read_end;
    e->started_at = now_seconds();
    e->line_len = 0;

    char *desc = argv_describe(e->argv);
    lgf(r, e->stream_id, "info", "ffmpegStart", "pid %ld: %s",
        rs_proc_id(&e->proc), desc ? desc : "");
    rs_free(desc);
    if (rs_proc_valid(&e->feeder)) {
        size_t feeder_argc = 0;
        while (e->feeder_argv[feeder_argc]) feeder_argc++;
        // A generic producer commonly carries account tokens or passwords in
        // its argv. Log the executable and shape, never its argument values.
        lgf(r, e->stream_id, "info", "ffmpegStart",
            "feeder pid %ld: %s (%lu argument%s redacted)",
            rs_proc_id(&e->feeder), e->feeder_argv[0],
            (unsigned long)(feeder_argc > 0 ? feeder_argc - 1 : 0),
            feeder_argc == 2 ? "" : "s");
    }
    return 0;
}

static void kill_pipeline(ffrun_entry *e) {
    if (rs_proc_valid(&e->feeder)) rs_proc_terminate(&e->feeder);
    if (rs_proc_valid(&e->proc)) {
        // Ask first, then insist. ffmpeg exits on the first signal where there
        // is one; a wedged one would otherwise linger holding the output. On
        // Windows rs_proc_terminate is already the hard kill, so the wait below
        // simply returns at once.
        rs_proc_terminate(&e->proc);
        if (rs_proc_wait(&e->proc, 0.2, NULL, NULL) != 1) {
            rs_proc_kill(&e->proc);
            rs_proc_wait(&e->proc, 0, NULL, NULL);
        }
        rs_proc_release(&e->proc);
    }
    if (rs_proc_valid(&e->feeder)) {
        if (rs_proc_try_wait(&e->feeder, NULL, NULL) != 1) {
            rs_proc_kill(&e->feeder);
            rs_proc_wait(&e->feeder, 0, NULL, NULL);
        }
        rs_proc_release(&e->feeder);
    }
    if (e->stderr_fd != RS_FD_INVALID) { rs_fd_close(e->stderr_fd); e->stderr_fd = RS_FD_INVALID; }
}

// True when an ffmpeg stderr line is worth putting in the log. Progress lines
// ("frame= 1234 fps= 25 …") arrive several times a second and say nothing.
static bool line_is_interesting(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return false;
    if (strncmp(s, "frame=", 6) == 0) return false;
    if (strncmp(s, "size=", 5) == 0) return false;
    if (strstr(s, "bitrate=") && strstr(s, "time=")) return false;
    return true;
}

static void drain_stderr(rs_ffrun *r, ffrun_entry *e) {
    if (e->stderr_fd == RS_FD_INVALID) return;
    char buf[1024];
    for (;;) {
        long n = rs_fd_read_nonblocking(e->stderr_fd, buf, sizeof(buf));
        if (n <= 0) break;
        for (long i = 0; i < n; i++) {
            char c = buf[i];
            // ffmpeg separates progress updates with '\r' and real messages
            // with '\n'; both end a line for our purposes.
            if (c == '\n' || c == '\r') {
                e->line[e->line_len] = '\0';
                if (e->line_len && line_is_interesting(e->line)) {
                    bool bad = strstr(e->line, "Error") || strstr(e->line, "error") ||
                               strstr(e->line, "Invalid") || strstr(e->line, "failed") ||
                               strstr(e->line, "Unable");
                    lg(r, e->stream_id, bad ? "error" : "info", "ffmpeg", e->line);
                }
                e->line_len = 0;
            } else if (e->line_len + 1 < sizeof(e->line)) {
                e->line[e->line_len++] = c;
            }
        }
    }
}

static ffrun_entry *find(rs_ffrun *r, const char *stream_id) {
    for (size_t i = 0; i < RS_FFRUN_MAX_STREAMS; i++)
        if (r->entries[i].used && strcmp(r->entries[i].stream_id, stream_id) == 0)
            return &r->entries[i];
    return NULL;
}

static void entry_release(ffrun_entry *e) {
    kill_pipeline(e);
    argv_free(e->argv);
    argv_free(e->feeder_argv);
    argv_free(e->env_keys);
    argv_free(e->env_values);
    free(e->stream_id);
    memset(e, 0, sizeof(*e));
    e->stderr_fd = RS_FD_INVALID;
}

rs_ffrun *rs_ffrun_create(rs_ffrun_log_fn log, void *log_ctx) {
    rs_ffrun *r = (rs_ffrun *)calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->log = log;
    r->log_ctx = log_ctx;
    for (size_t i = 0; i < RS_FFRUN_MAX_STREAMS; i++)
        r->entries[i].stderr_fd = RS_FD_INVALID;
    return r;
}

void rs_ffrun_destroy(rs_ffrun *r) {
    if (!r) return;
    for (size_t i = 0; i < RS_FFRUN_MAX_STREAMS; i++)
        if (r->entries[i].used) entry_release(&r->entries[i]);
    free(r);
}

int rs_ffrun_start(rs_ffrun *r, const char *stream_id,
                   const char *const *argv, const char *const *feeder_argv,
                   const char *const *env_keys, const char *const *env_values,
                   size_t env_count) {
    if (!r || !stream_id || !argv || !argv[0]) return -1;

    // Replacing a running pipeline: the argument list is what a config edit
    // changes, and ffmpeg cannot be reconfigured in flight, so it restarts.
    ffrun_entry *e = find(r, stream_id);
    if (e) entry_release(e);
    if (!e) {
        for (size_t i = 0; i < RS_FFRUN_MAX_STREAMS; i++)
            if (!r->entries[i].used) { e = &r->entries[i]; break; }
    }
    if (!e) {
        lg(r, stream_id, "error", "ffmpegStart", "too many ffmpeg streams running");
        return -1;
    }

    memset(e, 0, sizeof(*e));
    e->used = true;
    e->stderr_fd = RS_FD_INVALID;
    e->stream_id = rs_strdup(stream_id);
    e->argv = argv_copy(argv);
    e->feeder_argv = feeder_argv && feeder_argv[0] ? argv_copy(feeder_argv) : NULL;
    e->env_keys = strv_copy_n(env_keys, env_count);
    e->env_values = strv_copy_n(env_values, env_count);
    e->env_count = env_count;
    if (!e->stream_id || !e->argv || (env_count && (!e->env_keys || !e->env_values))) {
        entry_release(e);
        return -1;
    }

    if (spawn_pipeline(r, e) != 0) {
        // Keep the entry so the supervisor retries with backoff rather than
        // failing the operator's Start click outright — a source that is not up
        // yet is the common case, not an error.
        e->consecutive_failures = 1;
        e->retry_at = now_seconds() + RS_FFRUN_BACKOFF_BASE;
        return -1;
    }
    return 0;
}

void rs_ffrun_stop(rs_ffrun *r, const char *stream_id) {
    if (!r || !stream_id) return;
    ffrun_entry *e = find(r, stream_id);
    if (!e) return;
    e->stopping = true;
    lgf(r, stream_id, "info", "ffmpegStop", "stopping pid %ld", rs_proc_id(&e->proc));
    entry_release(e);
}

bool rs_ffrun_is_running(const rs_ffrun *r, const char *stream_id) {
    if (!r || !stream_id) return false;
    ffrun_entry *e = find((rs_ffrun *)r, stream_id);
    return e && !e->stopping;
}

void rs_ffrun_poll(rs_ffrun *r) {
    if (!r) return;
    double now = now_seconds();
    for (size_t i = 0; i < RS_FFRUN_MAX_STREAMS; i++) {
        ffrun_entry *e = &r->entries[i];
        if (!e->used || e->stopping) continue;

        drain_stderr(r, e);

        // A dead feeder with a live ffmpeg is a stalled pipeline: ffmpeg will sit
        // on an stdin that will never produce another byte. Treat the pair as one
        // unit and restart both.
        if (rs_proc_valid(&e->feeder) && rs_proc_try_wait(&e->feeder, NULL, NULL) == 1) {
            lg(r, e->stream_id, "error", "ffmpegExit", "the input command exited — restarting the pipeline");
            kill_pipeline(e);
        }

        if (rs_proc_valid(&e->proc)) {
            int status = 0, sig = 0;
            if (rs_proc_try_wait(&e->proc, &status, &sig) == 1) {
                double uptime = now - e->started_at;
                drain_stderr(r, e);   // whatever it said on the way out
                if (sig)
                    lgf(r, e->stream_id, "error", "ffmpegExit",
                        "killed by signal %d after %.0fs", sig, uptime);
                else
                    lgf(r, e->stream_id, "error", "ffmpegExit",
                        "exited with status %d after %.0fs", status, uptime);
                kill_pipeline(e);
                if (uptime >= RS_FFRUN_HEALTHY_AFTER) {
                    e->consecutive_failures = 0;   // it ran; this is a blip
                    e->retry_at = 0;
                } else {
                    e->consecutive_failures++;
                    double wait = RS_FFRUN_BACKOFF_BASE;
                    for (int k = 1; k < e->consecutive_failures && wait < RS_FFRUN_BACKOFF_MAX; k++)
                        wait *= 2;
                    if (wait > RS_FFRUN_BACKOFF_MAX) wait = RS_FFRUN_BACKOFF_MAX;
                    e->retry_at = now + wait;
                    lgf(r, e->stream_id, "error", "ffmpegBackoff",
                        "%d starts in a row ended early — waiting %.0fs before the next",
                        e->consecutive_failures, wait);
                }
            }
        }

        if (!rs_proc_valid(&e->proc) && (e->retry_at == 0 || now >= e->retry_at)) {
            e->retry_at = 0;
            if (spawn_pipeline(r, e) == 0) {
                e->restarts++;
            } else {
                e->consecutive_failures++;
                double wait = RS_FFRUN_BACKOFF_BASE * (double)e->consecutive_failures;
                if (wait > RS_FFRUN_BACKOFF_MAX) wait = RS_FFRUN_BACKOFF_MAX;
                e->retry_at = now + wait;
            }
        }
    }
}

char *rs_ffrun_status_line(const rs_ffrun *r, const char *stream_id) {
    if (!r || !stream_id) return NULL;
    ffrun_entry *e = find((rs_ffrun *)r, stream_id);
    if (!e) return NULL;
    char buf[256];
    if (rs_proc_valid(&e->proc))
        snprintf(buf, sizeof(buf), "ffmpeg pid %ld, up %.0fs, %d restart%s",
                 rs_proc_id(&e->proc), now_seconds() - e->started_at, e->restarts,
                 e->restarts == 1 ? "" : "s");
    else
        snprintf(buf, sizeof(buf), "ffmpeg not running, %d failed start%s in a row",
                 e->consecutive_failures, e->consecutive_failures == 1 ? "" : "s");
    return rs_strdup(buf);
}
