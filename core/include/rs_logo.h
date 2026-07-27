#ifndef RS_LOGO_H
#define RS_LOGO_H

#include "rs_common.h"

#ifdef __cplusplus
extern "C" {
#endif

// Fetch function type. Returns a newly allocated string for the response body, or NULL.
// It is up to the caller to free the returned string with rs_free. (Named
// distinctly from restream.h's restream_fetch_fn — different signature and role.)
typedef char* (*rs_logo_fetch_fn)(const char *url, void *ctx);

typedef struct rs_logo_cache rs_logo_cache;

rs_logo_cache* rs_logo_cache_create(const char *cache_file_path);
void rs_logo_cache_destroy(rs_logo_cache *lc);

// Returns malloc'd URL string or NULL. Uses cached results.
// NOTE: This requires HTTP fetching which depends on the fetch handler.
char* rs_logo_lookup(rs_logo_cache *lc, const char *name, rs_logo_fetch_fn fetch, void *fetch_ctx);

#ifdef __cplusplus
}
#endif

#endif  // RS_LOGO_H
