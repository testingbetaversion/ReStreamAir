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

// Service management, registered by the app so core/ stays free of anything
// platform-specific (the same arrangement as the probe/fetch/dash handlers).
// `status` returns a malloc'd JSON object describing the service, or NULL when
// this build cannot manage one. `action` performs "restart" or "install" and
// returns 0 on success, filling errbuf otherwise.
typedef char* (*restream_service_status_fn)(void);
typedef int (*restream_service_action_fn)(const char* action, unsigned port,
                                         const char* bind, char* errbuf, size_t errbuf_len);
void restream_server_set_service_handler(restream_service_status_fn status,
                                         restream_service_action_fn action);

// The panel's stored Server Settings, read after create and before start so the
// caller can honour them when no command-line override was given — the panel
// tells the operator these "take effect after restart", which is only true if
// startup actually consults them. Returns 0 when unset. `out_bind` points at
// server-owned storage valid until the next state write; NULL to skip it.
uint16_t restream_server_stored_port(const restream_server_t* server);
const char* restream_server_stored_bind(const restream_server_t* server);

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
// `effective_url` (may be NULL) receives a malloc'd copy of the final URL after
// redirects — needed to resolve a DASH MPD's relative segment URLs when the
// manifest 302s to a session-tokenized host. Only the libcurl path fills it.
// `timeout_ms` is the whole-request timeout (<= 0 uses 30 seconds). For the
// in-process client, `should_cancel(cancel_ctx)` is polled during the transfer;
// returning non-zero aborts it. External downloader processes cannot use that
// callback and retain their own timeout behaviour.
typedef int (*restream_fetch_fn)(const char *url, const char *proxy, const char *headers,
                                 const char *range, const char *downloader, const char *dl_params,
                                 char **out, size_t *out_len,
                                 long *status, char **content_type, char **content_range,
                                 char **effective_url, char *errbuf, size_t errbuf_len,
                                 long timeout_ms, int (*should_cancel)(void *), void *cancel_ctx);
void restream_server_set_fetch_handler(restream_fetch_fn handler);

// A DASH describe handler: fetches an MPD (through proxy/headers/downloader),
// follows redirects, and returns a malloc'd JSON description (freed with
// rs_free) — default video/audio renditions plus, when `rep` is given, that
// representation's init + newest-`want` segment URLs. NULL on failure with a
// message in errbuf. Lives in the C++ server (libxml2); the core only holds the
// pointer, so the Swift build never links libxml2. When unset, DASH playback
// returns 501. See rs_dash_describe.
//
// `segment_url_params` (may be NULL/"") is a raw query-string fragment
// appended to every returned init/segment URL. `inherit_url_params`, when
// non-zero, overrides that with the query string of the MPD URL that actually
// answered (i.e. the redirect target) — some CDNs mint a signed token only on
// the 302 target, so the configured `url` itself carries no query string for
// segment_url_params to have captured ahead of time.
typedef char *(*restream_dash_fn)(const char *url, const char *proxy, const char *headers,
                                  const char *downloader, const char *dl_params,
                                  const char *rep, int want,
                                  const char *segment_url_params, int inherit_url_params,
                                  char *errbuf, size_t errbuf_len);
void restream_server_set_dash_handler(restream_dash_fn handler);

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
