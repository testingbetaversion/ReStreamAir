#ifndef RS_NETMATCH_H
#define RS_NETMATCH_H

// IP literal parsing and CIDR matching, for deciding whether a request arrived
// through a reverse proxy we trust.
//
// This lives in the core rather than in either server because getting it wrong
// is a security bug in both directions: trust nothing and every client behind a
// proxy shares one IP (so the login throttle and the viewer count are useless),
// trust everything and any client can forge X-Forwarded-For and walk past the
// throttle. One implementation, one set of self-test vectors, both servers.
//
// No sockets and no platform headers: addresses are parsed by hand so this
// compiles into restream_base, which must not pull in Winsock.

#include "rs_common.h"

#ifdef __cplusplus
extern "C" {
#endif

// Parses an IPv4 or IPv6 literal (no port, no zone id, no brackets) into 16
// bytes. IPv4 is stored as an IPv4-mapped IPv6 address (::ffff:a.b.c.d), so a
// single 16-byte compare works for both families. Returns false if `text` is
// not an address literal.
bool rs_ip_parse(const char *text, uint8_t out[16]);

// True if `ip` (an address literal) matches `pattern`, which is one of:
//
//   loopback         127.0.0.0/8 and ::1
//   private          RFC 1918 + RFC 4193 + link-local — the usual LAN ranges
//   any or *         every address
//   10.0.0.0/8       a CIDR block, either family
//   192.168.1.7      a single address
//
// An IPv4 prefix length matches IPv4-mapped addresses as written (/24 means 24
// IPv4 bits), so "10.0.0.0/8" behaves the way an operator expects.
bool rs_ip_matches(const char *ip, const char *pattern);

// True if `ip` matches any pattern in `list` — comma- or space-separated, empty
// or NULL meaning "trust nothing".
bool rs_ip_in_list(const char *ip, const char *list);

// True if `pattern` is one rs_ip_matches understands. A mistyped trusted-proxy
// entry otherwise fails silently — it simply never matches, leaving an operator
// convinced their proxy is trusted when it is not — so the settings form
// validates with this on the way in.
bool rs_ip_pattern_valid(const char *pattern);

// Validates every entry of a list. On failure returns false and copies the
// offending token into `bad` (which may be NULL).
bool rs_ip_list_valid(const char *list, char *bad, size_t bad_cap);

// Resolves the real client address for a request that may have crossed a proxy.
//
// `peer` is the address the connection actually came from, `xff` the raw
// X-Forwarded-For header (may be NULL), `trusted` the configured trusted-proxy
// list. Returns a fresh string (rs_free) that is:
//
//   * `peer`, if `peer` is not in `trusted` — an untrusted peer's forwarding
//     header is a claim about itself and is ignored outright;
//   * otherwise the rightmost entry of `xff` that is not itself trusted, which
//     is the closest hop the trusted chain can actually vouch for;
//   * `peer` again if `xff` is absent, empty or entirely unparseable.
//
// Returns NULL only on allocation failure.
char *rs_client_ip(const char *peer, const char *xff, const char *trusted);

// True if the request should be treated as having reached the user over TLS:
// either it did (`direct_tls`), or a trusted proxy said so through
// X-Forwarded-Proto / Forwarded: proto=. An untrusted peer's header is ignored.
bool rs_request_is_secure(const char *peer, const char *forwarded_proto,
                          const char *trusted, bool direct_tls);

#ifdef __cplusplus
}
#endif

#endif  // RS_NETMATCH_H
