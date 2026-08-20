#ifndef RS_PROBE_H
#define RS_PROBE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The source-probe handler registered with the C server (restream_probe_fn):
// fetches `url` (through an optional proxy and "Name: value"-per-line headers),
// detects DASH vs HLS, and returns a malloc'd JSON string (freed with rs_free)
// of the form {kind, representations:[...], protection:{...}} — or NULL on
// failure, with a message written to errbuf. Uses libcurl + libxml2, which is
// why it lives in the server app rather than the core.
char *rs_probe_source(const char *url, const char *proxy, const char *headers,
                      char *errbuf, size_t errbuf_len);

#ifdef __cplusplus
}
#endif

#endif  // RS_PROBE_H
