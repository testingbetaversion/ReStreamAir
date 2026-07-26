#ifndef RS_URL_H
#define RS_URL_H

// Relative-reference resolution (RFC 3986 section 5). This has no Swift
// counterpart in the repo: M3U8Rewriter.swift gets it from Foundation's
// URL(string:relativeTo:), which C does not have, so the playlist rewriter
// needs its own. Behaviour is matched to Foundation rather than to strict RFC
// where the two differ — see rs_url_resolve.

#include "rs_common.h"

#ifdef __cplusplus
extern "C" {
#endif

// True if s begins with a scheme ("http:", "data:", …).
bool rs_url_is_absolute(const char *s);

// Resolves `ref` against `base` and returns the absolute URL, which the caller
// frees with rs_free. Returns NULL where Foundation returns nil and callers
// therefore pass the playlist line through unchanged: a NULL argument, an empty
// reference, or a base that is not absolute.
//
// Characters that cannot appear literally in a URL — spaces, control
// characters, quotes, brackets, and every non-ASCII byte — are percent-encoded
// with uppercase hex rather than rejected, and an existing valid escape such as
// %3D is left alone instead of being double-encoded. Both arguments are
// normalised this way before resolution, matching what Foundation's
// absoluteString produces.
//
// An already-absolute `ref` is returned normalised but otherwise as written,
// dot segments included, because that is what Foundation does:
// URL(string: "http://a/b/../c") keeps its path. Dot segments are only removed
// while resolving a relative reference, per section 5.2.4.
char *rs_url_resolve(const char *base, const char *ref);

#ifdef __cplusplus
}
#endif

#endif  // RS_URL_H
