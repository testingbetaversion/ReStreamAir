#ifndef RESTREAM_H
#define RESTREAM_H

#include <stdint.h>
#include <stdbool.h>

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
