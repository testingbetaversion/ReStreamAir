#include "ffrun.h"

#include "rs_common.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef _WIN32
#include <spawn.h>
extern char **environ;
#endif

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

    pid_t pid;          // the main process (ffmpeg)
    pid_t feeder_pid;   // optional producer piped into its stdin, or -1
    int stderr_fd;      // read end of its stderr, O_NONBLOCK, or -1

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

#ifndef _WIN32

static bool env_is_overridden(const ffrun_entry *e, const char *item) {
    const char *eq = strchr(item, '=');
    size_t name_len = eq ? (size_t)(eq - item) : strlen(item);
    for (size_t i = 0; i < e->env_count; i++)
        if (strlen(e->env_keys[i]) == name_len && strncmp(e->env_keys[i], item, name_len) == 0)
            return true;
    return false;
}

static char **pipeline_env(const ffrun_entry *e) {
    size_t inherited = 0;
    while (environ[inherited]) inherited++;
    char **out = (char **)calloc(inherited + e->env_count + 1, sizeof(char *));
    if (!out) return NULL;
    size_t used = 0;
    for (size_t i = 0; i < inherited; i++) {
        if (env_is_overridden(e, environ[i])) continue;
        out[used++] = rs_strdup(environ[i]);
        if (!out[used - 1]) { argv_free(out); return NULL; }
    }
    for (size_t i = 0; i < e->env_count; i++) {
        size_t need = strlen(e->env_keys[i]) + strlen(e->env_values[i]) + 2;
        out[used] = (char *)malloc(need);
        if (!out[used]) { argv_free(out); return NULL; }
        snprintf(out[used++], need, "%s=%s", e->env_keys[i], e->env_values[i]);
    }
    return out;
}

// Spawns the pipeline. Returns 0 on success and fills pid/feeder_pid/stderr_fd.
static int spawn_pipeline(rs_ffrun *r, ffrun_entry *e) {
    int err_pipe[2] = {-1, -1};
    int feed_pipe[2] = {-1, -1};

    if (pipe(err_pipe) != 0) {
        lgf(r, e->stream_id, "error", "ffmpegSpawn", "could not create a pipe: %s", strerror(errno));
        return -1;
    }
    if (e->feeder_argv && pipe(feed_pipe) != 0) {
        lgf(r, e->stream_id, "error", "ffmpegSpawn", "could not create a pipe: %s", strerror(errno));
        close(err_pipe[0]); close(err_pipe[1]);
        return -1;
    }

    char **child_env = pipeline_env(e);
    if (!child_env) {
        close(err_pipe[0]); close(err_pipe[1]);
        if (feed_pipe[0] >= 0) close(feed_pipe[0]);
        if (feed_pipe[1] >= 0) close(feed_pipe[1]);
        return -1;
    }

    pid_t feeder = -1;
    if (e->feeder_argv) {
        // Producer: stdout -> the pipe ffmpeg reads, stderr -> the same log pipe
        // so a failing streamlink is as visible as a failing ffmpeg.
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, feed_pipe[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&fa, err_pipe[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&fa, feed_pipe[0]);
        posix_spawn_file_actions_addclose(&fa, err_pipe[0]);
        int rc = posix_spawnp(&feeder, e->feeder_argv[0], &fa, NULL, e->feeder_argv, child_env);
        posix_spawn_file_actions_destroy(&fa);
        if (rc != 0) {
            lgf(r, e->stream_id, "error", "ffmpegSpawn", "could not run \"%s\": %s",
                e->feeder_argv[0], strerror(rc));
            close(err_pipe[0]); close(err_pipe[1]);
            close(feed_pipe[0]); close(feed_pipe[1]);
            argv_free(child_env);
            return -1;
        }
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, err_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&fa, err_pipe[0]);
    if (e->feeder_argv) {
        posix_spawn_file_actions_adddup2(&fa, feed_pipe[0], STDIN_FILENO);
        posix_spawn_file_actions_addclose(&fa, feed_pipe[1]);
    } else {
        // No producer: give it /dev/null rather than the server's stdin, or
        // ffmpeg's "press [q] to stop" reader eats the terminal.
        posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    }
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, e->argv[0], &fa, NULL, e->argv, child_env);
    posix_spawn_file_actions_destroy(&fa);
    argv_free(child_env);

    // The parent keeps only the read end of stderr and none of the feed pipe.
    close(err_pipe[1]);
    if (feed_pipe[0] >= 0) close(feed_pipe[0]);
    if (feed_pipe[1] >= 0) close(feed_pipe[1]);

    if (rc != 0) {
        lgf(r, e->stream_id, "error", "ffmpegSpawn", "could not run \"%s\": %s",
            e->argv[0], strerror(rc));
        close(err_pipe[0]);
        if (feeder > 0) { kill(feeder, SIGKILL); waitpid(feeder, NULL, 0); }
        return -1;
    }

    fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);
    e->pid = pid;
    e->feeder_pid = feeder;
    e->stderr_fd = err_pipe[0];
    e->started_at = now_seconds();
    e->line_len = 0;

    char *desc = argv_describe(e->argv);
    lgf(r, e->stream_id, "info", "ffmpegStart", "pid %d: %s", (int)pid, desc ? desc : "");
    rs_free(desc);
    if (feeder > 0) {
        size_t feeder_argc = 0;
        while (e->feeder_argv[feeder_argc]) feeder_argc++;
        // A generic producer commonly carries account tokens or passwords in
        // its argv. Log the executable and shape, never its argument values.
        lgf(r, e->stream_id, "info", "ffmpegStart",
            "feeder pid %d: %s (%lu argument%s redacted)",
            (int)feeder, e->feeder_argv[0],
            (unsigned long)(feeder_argc > 0 ? feeder_argc - 1 : 0),
            feeder_argc == 2 ? "" : "s");
    }
    return 0;
}

static void kill_pipeline(ffrun_entry *e) {
    if (e->feeder_pid > 0) {
        kill(e->feeder_pid, SIGTERM);
    }
    if (e->pid > 0) {
        kill(e->pid, SIGTERM);
        // Give it a moment to flush, then insist. ffmpeg normally exits on the
        // first signal; a wedged one would otherwise linger holding the output.
        for (int i = 0; i < 20; i++) {
            if (waitpid(e->pid, NULL, WNOHANG) == e->pid) { e->pid = -1; break; }
            struct timespec ts = {0, 10 * 1000 * 1000};
            nanosleep(&ts, NULL);
        }
        if (e->pid > 0) {
            kill(e->pid, SIGKILL);
            waitpid(e->pid, NULL, 0);
            e->pid = -1;
        }
    }
    if (e->feeder_pid > 0) {
        if (waitpid(e->feeder_pid, NULL, WNOHANG) != e->feeder_pid) {
            kill(e->feeder_pid, SIGKILL);
            waitpid(e->feeder_pid, NULL, 0);
        }
        e->feeder_pid = -1;
    }
    if (e->stderr_fd >= 0) { close(e->stderr_fd); e->stderr_fd = -1; }
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
    if (e->stderr_fd < 0) return;
    char buf[1024];
    for (;;) {
        ssize_t n = read(e->stderr_fd, buf, sizeof(buf));
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
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

#else  // _WIN32 — process supervision is not ported here yet.

static int spawn_pipeline(rs_ffrun *r, ffrun_entry *e) {
    lg(r, e->stream_id, "error", "ffmpegSpawn", "ffmpeg modes are not supported on this platform yet");
    return -1;
}
static void kill_pipeline(ffrun_entry *e) { (void)e; }
static void drain_stderr(rs_ffrun *r, ffrun_entry *e) { (void)r; (void)e; }

#endif

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
    e->pid = -1;
    e->feeder_pid = -1;
    e->stderr_fd = -1;
}

rs_ffrun *rs_ffrun_create(rs_ffrun_log_fn log, void *log_ctx) {
    rs_ffrun *r = (rs_ffrun *)calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->log = log;
    r->log_ctx = log_ctx;
    for (size_t i = 0; i < RS_FFRUN_MAX_STREAMS; i++) {
        r->entries[i].pid = -1;
        r->entries[i].feeder_pid = -1;
        r->entries[i].stderr_fd = -1;
    }
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
    e->pid = -1;
    e->feeder_pid = -1;
    e->stderr_fd = -1;
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
    lgf(r, stream_id, "info", "ffmpegStop", "stopping pid %d", (int)e->pid);
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

#ifndef _WIN32
        // A dead feeder with a live ffmpeg is a stalled pipeline: ffmpeg will sit
        // on an stdin that will never produce another byte. Treat the pair as one
        // unit and restart both.
        if (e->feeder_pid > 0 && waitpid(e->feeder_pid, NULL, WNOHANG) == e->feeder_pid) {
            e->feeder_pid = -1;
            lg(r, e->stream_id, "error", "ffmpegExit", "the input command exited — restarting the pipeline");
            kill_pipeline(e);
        }

        if (e->pid > 0) {
            int status = 0;
            pid_t done = waitpid(e->pid, &status, WNOHANG);
            if (done == e->pid) {
                double uptime = now - e->started_at;
                e->pid = -1;
                drain_stderr(r, e);   // whatever it said on the way out
                if (WIFEXITED(status))
                    lgf(r, e->stream_id, "error", "ffmpegExit",
                        "exited with status %d after %.0fs", WEXITSTATUS(status), uptime);
                else
                    lgf(r, e->stream_id, "error", "ffmpegExit",
                        "killed by signal %d after %.0fs", WTERMSIG(status), uptime);
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
#endif

        if (e->pid <= 0 && (e->retry_at == 0 || now >= e->retry_at)) {
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
    if (e->pid > 0)
        snprintf(buf, sizeof(buf), "ffmpeg pid %d, up %.0fs, %d restart%s",
                 (int)e->pid, now_seconds() - e->started_at, e->restarts,
                 e->restarts == 1 ? "" : "s");
    else
        snprintf(buf, sizeof(buf), "ffmpeg not running, %d failed start%s in a row",
                 e->consecutive_failures, e->consecutive_failures == 1 ? "" : "s");
    return rs_strdup(buf);
}
