#ifndef RESTREAM_H
#define RESTREAM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Server context handle
typedef struct restream_server restream_server_t;

// Initialize the C server
restream_server_t* restream_server_create(void);

// Point the server at a directory of static files (the panel's public/). Call
// before restream_server_start. When set, any request that isn't /ping or /api
// is served from here; when unset, those requests get a short explanatory
// message. The path is copied. NULL or "" clears it.
void restream_server_set_web_root(restream_server_t* server, const char* path);

// Mongoose logs at INFO by default, which is a wall of per-connection noise for
// anyone just running the binary. This turns it down to errors only (verbose =
// false, the default once main sets it) or restores the full trace (true).
void restream_server_set_verbose(bool verbose);

// A source-probe handler: given a source URL (with optional proxy and
// newline-separated "Name: value" headers), it fetches and inspects the source
// and returns a malloc'd JSON string (freed with rs_free) describing its
// representations — or NULL on failure, with a message written to errbuf.
//
// The handler lives in the C++ server (which links libcurl/libxml2); the core
// only holds the function pointer, so the Swift build never pulls in those
// libraries. When no handler is registered, /api/probe returns 501.
typedef char *(*restream_probe_fn)(const char *url, const char *proxy, const char *headers,
                                   char *errbuf, size_t errbuf_len);
void restream_server_set_probe_handler(restream_probe_fn handler);

// A URL fetch handler, used by the playback routes to pull remote playlists and
// segments. `range` (e.g. "bytes=0-1023", or NULL) is forwarded so byte-range
// HLS and seeking work without downloading whole files. Fills *out/*out_len
// (caller frees *out with free); and, when non-NULL, *status (the upstream HTTP
// code), *content_type and *content_range (malloc'd or NULL). Returns 0 on
// success, negative on failure with a message in errbuf. Registered by the C++
// app (libcurl); when unset, the fetch-based playback routes return 501.
// `downloader`/`dl_params` pick the per-provider download mechanism: NULL or ""
// means the in-process libcurl fetch; "curl"/"wget"/"aria2c" run that external
// tool with `dl_params` appended (and fall back to libcurl if it is missing).
typedef int (*restream_fetch_fn)(const char *url, const char *proxy, const char *headers,
                                 const char *range, const char *downloader, const char *dl_params,
                                 char **out, size_t *out_len,
                                 long *status, char **content_type, char **content_range,
                                 char *errbuf, size_t errbuf_len);
void restream_server_set_fetch_handler(restream_fetch_fn handler);

// Start the server (non-blocking, or spawns a background thread in C,
// or the Swift app just loops over a poll function).
// For simplicity in C-Swift integration, we can have a poll function.
bool restream_server_start(restream_server_t* server, uint16_t port, const char* bind_address);

// Poll the server (should be called in a loop if running in the same thread, 
// or we can manage a thread inside C)
void restream_server_poll(restream_server_t* server, int timeout_ms);

// Stop the server
void restream_server_stop(restream_server_t* server);

// Destroy the server context
void restream_server_destroy(restream_server_t* server);

#ifdef __cplusplus
}
#endif

#endif // RESTREAM_H
