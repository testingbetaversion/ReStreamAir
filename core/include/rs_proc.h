// rs_proc.h — spawning, supervising and killing child processes, on POSIX and
// on Windows.
//
// Five places in the tree run an external program: provider scripts
// (script.c), `ffmpeg -version` capability probing (ffargs.c), the external
// downloaders yt-dlp / curl / N_m3u8DL-RE (net.c), the supervised ffmpeg
// pipeline (ffrun.c), and systemctl (service.c — genuinely Linux-only, and the
// one caller that stays platform-specific). All four of the others want the
// same three things: start a program with its stdio pointed somewhere specific,
// read what it says without deadlocking, and be able to end it.
//
// posix_spawn and CreateProcess disagree about nearly every detail of that —
// argv versus a single quoted command line, fd inheritance versus HANDLE
// inheritance, SIGTERM versus TerminateProcess, non-blocking reads versus
// PeekNamedPipe. This header is where that disagreement is resolved once.
//
// Two layers:
//   rs_proc_spawn + rs_proc_try_wait + rs_fd_read   — supervision, for ffrun
//   rs_proc_run                                     — run-to-completion, for
//                                                     the other three
//
// Threading: every function here is safe to call from any thread, but a given
// rs_proc must be owned by one thread at a time.

#ifndef RS_PROC_H
#define RS_PROC_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- endpoints -------------------------------------------------------------
// One end of a pipe, or a file. An `rs_fd` is an int on POSIX and a HANDLE on
// Windows; treat it as opaque and compare against RS_FD_INVALID.

#ifdef _WIN32
typedef void *rs_fd;
#define RS_FD_INVALID ((rs_fd)(long long)-1)   /* INVALID_HANDLE_VALUE */
#else
typedef int rs_fd;
#define RS_FD_INVALID (-1)
#endif

typedef struct {
    rs_fd read_end;
    rs_fd write_end;
} rs_pipe;

// A pipe whose ends are both usable by a child. Returns 0 on success.
int  rs_pipe_open(rs_pipe *p);
void rs_fd_close(rs_fd fd);

// Reads what is available without blocking. Returns the byte count (> 0),
// 0 when the pipe is open but momentarily empty, or -1 at end-of-file or on
// error — which is how a caller learns the writer is gone.
long rs_fd_read_nonblocking(rs_fd fd, void *buf, size_t cap);

// ---- stdio disposition -----------------------------------------------------

typedef enum {
    RS_STDIO_INHERIT = 0,  // the parent's own stream
    RS_STDIO_NULL,         // /dev/null, or NUL on Windows
    RS_STDIO_FD,           // an endpoint the caller supplies (a pipe end)
    RS_STDIO_FILE,         // create/truncate a path — stdout and stderr only
} rs_stdio_kind;

typedef struct {
    rs_stdio_kind kind;
    rs_fd         fd;      // RS_STDIO_FD
    const char   *path;    // RS_STDIO_FILE
} rs_stdio;

// Constructors rather than compound literals: these get assigned, not just used
// to initialise, and a plain function works the same under every C compiler the
// tree is built with without depending on how completely each one implements
// C99 in its C mode.
static inline rs_stdio rs_stdio_null(void) {
    rs_stdio s; s.kind = RS_STDIO_NULL;    s.fd = RS_FD_INVALID; s.path = NULL; return s;
}
static inline rs_stdio rs_stdio_fd(rs_fd fd) {
    rs_stdio s; s.kind = RS_STDIO_FD;      s.fd = fd;            s.path = NULL; return s;
}
static inline rs_stdio rs_stdio_file(const char *path) {
    rs_stdio s; s.kind = RS_STDIO_FILE;    s.fd = RS_FD_INVALID; s.path = path; return s;
}
static inline rs_stdio rs_stdio_inherit(void) {
    rs_stdio s; s.kind = RS_STDIO_INHERIT; s.fd = RS_FD_INVALID; s.path = NULL; return s;
}

// ---- processes -------------------------------------------------------------

typedef struct {
#ifdef _WIN32
    void         *handle;  // HANDLE; must be released with rs_proc_release
    unsigned long id;      // for log lines only
#else
    int           pid;
#endif
    bool valid;
} rs_proc;

// Starts argv[0], searched on PATH (and PATHEXT on Windows), like posix_spawnp.
// `envp` is a NULL-terminated "K=V" array, or NULL to inherit the parent's.
// Any of the three stdio arguments may be NULL, meaning inherit.
//
// On Windows only the handles named here are inherited by the child — an
// explicit handle list, not bInheritHandles alone. That matters: ffrun restarts
// pipelines while others are running, and a child that accidentally inherited
// another stream's stderr write-end would hold that pipe open forever, so its
// reader would never see EOF and that stream would never be detected as dead.
//
// Returns 0 on success. On failure returns a non-zero errno-style code and, if
// errbuf is non-NULL, writes a human-readable reason into it.
int rs_proc_spawn(rs_proc *out, const char *const *argv, const char *const *envp,
                  const rs_stdio *in, const rs_stdio *out_io, const rs_stdio *err_io,
                  char *errbuf, size_t errlen);

bool rs_proc_valid(const rs_proc *p);
long rs_proc_id(const rs_proc *p);       // pid, or the Windows process id

// Reaps the child if it has finished. Returns 1 when it exited (filling
// *exit_code, and *term_signal with the signal that killed it or 0), 0 when it
// is still running, -1 on error. On Windows *term_signal is always 0 — there
// are no signals, and a TerminateProcess'd child reports its exit code instead.
//
// After this returns 1 the handle is released and rs_proc_valid() is false.
int rs_proc_try_wait(rs_proc *p, int *exit_code, int *term_signal);

// Blocks until the child exits, or, when timeout_s > 0, until the deadline
// passes. Returns 1 on exit, 0 on timeout (the child is still running), -1 on
// error.
int rs_proc_wait(rs_proc *p, double timeout_s, int *exit_code, int *term_signal);

// Asks the child to stop. On POSIX this is SIGTERM, which ffmpeg handles by
// flushing and exiting cleanly. Windows has no such signal for a non-console
// child, so this is TerminateProcess: the process ends at once and whatever it
// was writing is truncated. Callers that care (ffrun) already tolerate this,
// because a hard kill is also what happens on POSIX when SIGTERM is ignored.
void rs_proc_terminate(rs_proc *p);
void rs_proc_kill(rs_proc *p);        // SIGKILL / TerminateProcess, no grace
void rs_proc_release(rs_proc *p);     // drop our handle without waiting

// ---- environment -----------------------------------------------------------
// A child environment: the parent's, minus any name in `keys`, plus the given
// key/value pairs. Free with rs_env_free. Returns NULL on allocation failure.
char **rs_env_build(const char *const *keys, const char *const *values, size_t count);
void   rs_env_free(char **env);

// ---- run to completion -----------------------------------------------------

typedef struct {
    bool  spawned;      // false when the program could not be started at all
    int   spawn_error;  // errno-style reason when !spawned; ENOENT = not on PATH
    bool  timed_out;    // true when it was killed at the deadline
    int   exit_code;
    int   term_signal;  // POSIX only; 0 otherwise
    char *out;          // captured stdout, NUL-terminated; caller frees
    char *err;          // captured stderr, NUL-terminated; caller frees
} rs_run_result;

// Optional live output sink for rs_proc_run_stream. Chunks are delivered as
// soon as they are drained from the child pipe; callers that need lines keep a
// small partial-line buffer between calls.
typedef void (*rs_proc_output_fn)(void *ctx, bool is_stderr,
                                  const char *bytes, size_t len);

// Runs argv to completion. stdout and stderr are captured when the
// corresponding capture flag is set, discarded otherwise — except that a
// non-NULL `stderr_path` sends stderr to that file instead (which is what the
// external-downloader path wants, since a downloader's stderr can be large and
// is only read on failure).
//
// Both streams are drained while the child runs, so a child that fills one pipe
// while the caller waits on the other cannot deadlock.
//
// timeout_s <= 0 waits indefinitely. On timeout the child is terminated, then
// killed, and res->timed_out is set.
//
// Returns 0 if the child ran (check res->exit_code), -1 if it could not be
// started. `res` is always initialised; free res->out and res->err either way.
int rs_proc_run(const char *const *argv, const char *const *envp, double timeout_s,
                bool capture_stdout, bool capture_stderr, const char *stderr_path,
                rs_run_result *res, char *errbuf, size_t errlen);

int rs_proc_run_stream(const char *const *argv, const char *const *envp, double timeout_s,
                       bool capture_stdout, bool capture_stderr, const char *stderr_path,
                       rs_run_result *res, char *errbuf, size_t errlen,
                       rs_proc_output_fn output_fn, void *output_ctx);

void rs_run_result_dispose(rs_run_result *res);

// Sleeps for `ms` milliseconds. Here because supervising a child means waiting,
// and nanosleep and Sleep disagree about both name and units.
void rs_proc_sleep_ms(int ms);

// ---- interpreters ----------------------------------------------------------
// Given a script path, the argv prefix needed to execute it: "python3 -u" for .py
// on POSIX and "python -u" on Windows, a shell for .sh, cmd.exe for .bat/.cmd and
// PowerShell for .ps1 (both Windows-only), or nothing at all for a file that is
// directly executable. Writes up to `max` pointers into `prefix` and returns how
// many; the strings are static literals and must not be freed.
size_t rs_proc_interpreter_for(const char *script_path, const char **prefix, size_t max);

#ifdef __cplusplus
}
#endif
#endif  // RS_PROC_H
