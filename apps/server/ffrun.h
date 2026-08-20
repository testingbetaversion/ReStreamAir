#ifndef RS_FFRUN_H
#define RS_FFRUN_H

// Supervises the external processes an ffmpeg-backed stream needs.
//
// The argument builder (rs_ffargs_build) and the binary locator
// (rs_ffmpeg_resolve) already existed; nothing ever spawned the result, which is
// why every ffmpeg input mode was inert in the C build. This is that missing
// half: it starts the process (optionally with a producer command piped into its
// stdin), keeps it alive, surfaces its stderr in the panel's Logs tab, and stops
// it cleanly.
//
// It lives in apps/server rather than core/ for the same reason the fetch and
// DASH handlers do: the core must stay free of anything a core-only build would
// have to compile, and process spawning is platform-specific.
//
// Supervision runs off the server's existing one-second timer rather than a
// thread. That keeps every ffmpeg state change on the event-loop thread, so the
// log sink and the stream table need no locking, and it costs nothing: a
// waitpid(WNOHANG) plus a non-blocking read per running stream. The stderr pipe
// is therefore O_NONBLOCK — a blocking read here would stall the whole server
// behind ffmpeg's progress output.

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rs_ffrun rs_ffrun;

// Called on the event-loop thread whenever a supervised process says or does
// something worth recording. `message` is never NULL.
typedef void (*rs_ffrun_log_fn)(void *ctx, const char *stream_id, const char *level,
                                const char *event, const char *message);

rs_ffrun *rs_ffrun_create(rs_ffrun_log_fn log, void *log_ctx);
void rs_ffrun_destroy(rs_ffrun *r);

// Starts the pipeline for `stream_id`, replacing any pipeline already running
// for it. `argv` is a NULL-terminated command line whose argv[0] is the
// executable; `feeder_argv` is optional and, when given, is run with its stdout
// wired to `argv`'s stdin — that is the `streamlink --stdout | ffmpeg -i pipe:0`
// shape. Returns 0 on success, negative if the process could not be spawned.
//
// The arrays are copied, so the caller may free them on return.
int rs_ffrun_start(rs_ffrun *r, const char *stream_id,
                   const char *const *argv, const char *const *feeder_argv);

// Signals the pipeline to stop (SIGTERM, then SIGKILL if it ignores that) and
// forgets it. Safe to call for a stream that is not running.
void rs_ffrun_stop(rs_ffrun *r, const char *stream_id);

// True while a pipeline for `stream_id` exists and has not been asked to stop.
bool rs_ffrun_is_running(const rs_ffrun *r, const char *stream_id);

// Drains stderr, notices exits, and restarts what died. Call once a second.
void rs_ffrun_poll(rs_ffrun *r);

// One-line health summary for the diagnostics ("ffmpeg pid 1234, up 42s, 1
// restart"), or NULL when the stream has no pipeline. Caller frees with rs_free.
char *rs_ffrun_status_line(const rs_ffrun *r, const char *stream_id);

#ifdef __cplusplus
}
#endif

#endif  // RS_FFRUN_H
