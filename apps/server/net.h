#ifndef RS_NET_H
#define RS_NET_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fetches `url` (through an optional proxy and "Name: value"-per-line headers)
// into a freshly malloc'd buffer (caller frees *out with free; always
// NUL-terminated), writing the byte length to *out_len.
// `proxy` may contain one proxy URL per line. They are tried in order on
// failure, and the last successful entry is preferred by later requests.
//
// `range` (e.g. "bytes=0-1023", or NULL) is forwarded as the Range request, so
// byte-range HLS and seeking fetch only the requested bytes. When non-NULL,
// *status receives the upstream HTTP code, and *content_type / *content_range
// receive malloc'd copies of those response headers (or NULL). Any of those
// out-params may be NULL if not needed.
//
// Returns 0 on success, -1 on failure with a message in errbuf. This is the one
// outbound-HTTP primitive the server app uses for probing and playback; it
// lives here (not in the core) because it links libcurl.
// `downloader` selects the mechanism: NULL/""/"internal"/"libcurl" uses the
// in-process libcurl fetch; "curl"/"wget"/"aria2c" shell out to that binary
// (with `dl_params` appended verbatim), falling back to libcurl if it is not
// installed. `dl_params` is a free-form extra-arguments string (may be NULL).
// `force_ipv6` makes hostname resolution IPv6-only; curl/wget receive their
// IPv6-only flag, while aria2c uses the internal client because it has no
// equivalent strict mode.
// With `rotate_proxies`, healthy entries are selected round-robin. Without it,
// configured order remains the priority. Both modes quarantine connection
// failures and periodically retry them, and initially exercise every entry.
// `effective_url` (may be NULL) receives the final URL after redirects (libcurl
// path only) — used to resolve a redirected DASH MPD's segment URLs.
// `timeout_ms` is the whole-request timeout (<= 0 means 30 seconds). The
// in-process client periodically calls `should_cancel(cancel_ctx, downloaded)`,
// when set, and aborts as soon as it returns non-zero. `downloaded` is the
// number of response-body bytes received on the current request, which lets a
// caller distinguish a slow but progressing transfer from a silent one.
// External tools use their own timeout and cannot call back into the process.
int rs_fetch_url(const char *url, const char *proxy, const char *headers, const char *range,
                 const char *downloader, const char *dl_params, int force_ipv6, int rotate_proxies,
                 char **out, size_t *out_len, long *status, char **content_type,
                 char **content_range, char **effective_url, char *errbuf, size_t errbuf_len,
                 long timeout_ms, int (*should_cancel)(void *, size_t), void *cancel_ctx);

// POSTs a JSON document to an HTTP(S) endpoint. Used by the provider error
// webhook worker; deliberately separate from rs_fetch_url so a configured
// downloader or stream proxy can never alter where operational alerts go.
int rs_post_json(const char *url, const char *json, long *status,
                 char *errbuf, size_t errbuf_len);

#ifdef __cplusplus
}
#endif

#endif  // RS_NET_H
