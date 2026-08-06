#include "rs_netmatch.h"

#include <stdlib.h>
#include <string.h>

// Every address is normalised to 16 bytes: IPv6 as-is, IPv4 as the IPv4-mapped
// form ::ffff:a.b.c.d. That makes one memcmp enough for both families, and it
// makes "is 127.0.0.1 inside 127.0.0.0/8" the same code path as the v6 case.
static const uint8_t V4_MAPPED_PREFIX[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};

static bool parse_ipv4(const char *text, size_t len, uint8_t out[4]) {
    size_t pos = 0;
    for (int octet = 0; octet < 4; octet++) {
        if (octet > 0) {
            if (pos >= len || text[pos] != '.') return false;
            pos++;
        }
        if (pos >= len || text[pos] < '0' || text[pos] > '9') return false;
        // Reject "01" and "0001": a leading zero means a different number to
        // some parsers (octal), so an address that is ambiguous is no address.
        bool leading_zero = text[pos] == '0';
        unsigned value = 0;
        size_t digits = 0;
        while (pos < len && text[pos] >= '0' && text[pos] <= '9') {
            value = value * 10 + (unsigned)(text[pos] - '0');
            if (value > 255) return false;
            pos++;
            digits++;
        }
        if (digits > 1 && leading_zero) return false;
        out[octet] = (uint8_t)value;
    }
    return pos == len;
}

// RFC 4291 textual form, including "::" compression and a trailing embedded
// IPv4 literal ("::ffff:192.0.2.1").
static bool parse_ipv6(const char *text, size_t len, uint8_t out[16]) {
    uint8_t head[16] = {0}, tail[16] = {0};
    size_t head_len = 0, tail_len = 0;
    bool seen_compressor = false;
    size_t pos = 0;

    if (len >= 2 && text[0] == ':' && text[1] == ':') {
        seen_compressor = true;
        pos = 2;
    } else if (len >= 1 && text[0] == ':') {
        return false;  // a single leading colon is not a valid address
    }

    while (pos < len) {
        // An embedded IPv4 literal is only legal as the final component, so it
        // starts here only when no colon comes before the first dot —
        // otherwise "::ffff:192.0.2.1" would try to read "ffff:192.0.2.1" as
        // one IPv4 address and reject the whole thing.
        const char *dot = memchr(text + pos, '.', len - pos);
        const char *colon = memchr(text + pos, ':', len - pos);
        if (dot && (!colon || colon > dot)) {
            uint8_t v4[4];
            if (!parse_ipv4(text + pos, len - pos, v4)) return false;
            uint8_t *dst = seen_compressor ? tail : head;
            size_t *dst_len = seen_compressor ? &tail_len : &head_len;
            if (*dst_len + 4 > 16) return false;
            memcpy(dst + *dst_len, v4, 4);
            *dst_len += 4;
            pos = len;
            break;
        }

        unsigned group = 0;
        size_t digits = 0;
        while (pos < len && digits < 4) {
            char ch = text[pos];
            unsigned nibble;
            if (ch >= '0' && ch <= '9') nibble = (unsigned)(ch - '0');
            else if (ch >= 'a' && ch <= 'f') nibble = (unsigned)(ch - 'a') + 10;
            else if (ch >= 'A' && ch <= 'F') nibble = (unsigned)(ch - 'A') + 10;
            else break;
            group = (group << 4) | nibble;
            pos++;
            digits++;
        }
        if (digits == 0) return false;

        uint8_t *dst = seen_compressor ? tail : head;
        size_t *dst_len = seen_compressor ? &tail_len : &head_len;
        if (*dst_len + 2 > 16) return false;
        dst[(*dst_len)++] = (uint8_t)(group >> 8);
        dst[(*dst_len)++] = (uint8_t)(group & 0xff);

        if (pos == len) break;
        if (text[pos] != ':') return false;
        pos++;
        if (pos < len && text[pos] == ':') {
            if (seen_compressor) return false;  // only one "::" per address
            seen_compressor = true;
            pos++;
            if (pos == len) break;  // trailing "::"
        } else if (pos == len) {
            return false;  // a trailing single colon is not valid
        }
    }

    if (seen_compressor) {
        if (head_len + tail_len > 14) return false;  // "::" must cover >= 1 group
    } else if (head_len != 16) {
        return false;
    }

    memset(out, 0, 16);
    memcpy(out, head, head_len);
    memcpy(out + 16 - tail_len, tail, tail_len);
    return true;
}

bool rs_ip_parse(const char *text, uint8_t out[16]) {
    if (!text || !out) return false;
    // Tolerate the bracketed form and a zone id, both of which show up in
    // headers and log lines: [fe80::1%eth0] is still fe80::1 to us.
    size_t len = strlen(text);
    if (len >= 2 && text[0] == '[' && text[len - 1] == ']') { text++; len -= 2; }
    const char *percent = (const char *)memchr(text, '%', len);
    if (percent) len = (size_t)(percent - text);
    if (len == 0) return false;

    if (memchr(text, ':', len) == NULL) {
        uint8_t v4[4];
        if (!parse_ipv4(text, len, v4)) return false;
        memcpy(out, V4_MAPPED_PREFIX, sizeof(V4_MAPPED_PREFIX));
        memcpy(out + 12, v4, 4);
        return true;
    }
    return parse_ipv6(text, len, out);
}

static bool is_v4_mapped(const uint8_t addr[16]) {
    return memcmp(addr, V4_MAPPED_PREFIX, sizeof(V4_MAPPED_PREFIX)) == 0;
}

// True if `addr` and `net` agree on the first `bits` bits.
static bool prefix_equal(const uint8_t addr[16], const uint8_t net[16], unsigned bits) {
    unsigned whole = bits / 8, remainder = bits % 8;
    if (whole > 0 && memcmp(addr, net, whole) != 0) return false;
    if (remainder == 0) return true;
    uint8_t mask = (uint8_t)(0xff << (8 - remainder));
    return (addr[whole] & mask) == (net[whole] & mask);
}

// The named groups. Keeping these as literal CIDR lists rather than bit-twiddling
// means the definition of "private" is readable and auditable.
static const char *const LOOPBACK[] = {"127.0.0.0/8", "::1/128", NULL};
static const char *const PRIVATE[] = {
    "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "169.254.0.0/16",
    "127.0.0.0/8", "fc00::/7", "fe80::/10", "::1/128", NULL,
};

static bool matches_group(const uint8_t addr[16], const char *const *patterns);
static bool next_token(const char **cursor, char *out, size_t out_cap);

// Splits "net/bits" into a parsed network and a prefix length. `bits` comes back
// as the width counted from the start of the 16-byte form, so an IPv4 /24 is
// reported as 120. Returns false if the pattern is not a CIDR block or address.
static bool parse_cidr(const char *pattern, uint8_t net[16], unsigned *bits) {
    const char *slash = strchr(pattern, '/');
    char net_text[64];
    size_t net_len = slash ? (size_t)(slash - pattern) : strlen(pattern);
    if (net_len == 0 || net_len >= sizeof(net_text)) return false;
    memcpy(net_text, pattern, net_len);
    net_text[net_len] = '\0';
    if (!rs_ip_parse(net_text, net)) return false;

    bool net_is_v4 = is_v4_mapped(net);
    unsigned width = net_is_v4 ? 32u : 128u;
    if (slash) {
        const char *digits = slash + 1;
        if (!digits[0]) return false;
        unsigned parsed = 0;
        for (const char *p = digits; *p; p++) {
            if (*p < '0' || *p > '9') return false;
            parsed = parsed * 10 + (unsigned)(*p - '0');
            if (parsed > 128) return false;
        }
        if (parsed > width) return false;
        width = parsed;
    }
    *bits = net_is_v4 ? width + 96 : width;  // v4 bits count from the mapped prefix
    return true;
}

bool rs_ip_matches(const char *ip, const char *pattern) {
    if (!ip || !pattern || !pattern[0]) return false;
    uint8_t addr[16];
    if (!rs_ip_parse(ip, addr)) return false;

    if (strcmp(pattern, "any") == 0 || strcmp(pattern, "*") == 0) return true;
    if (strcmp(pattern, "loopback") == 0) return matches_group(addr, LOOPBACK);
    if (strcmp(pattern, "private") == 0) return matches_group(addr, PRIVATE);

    uint8_t net[16];
    unsigned bits = 0;
    if (!parse_cidr(pattern, net, &bits)) return false;

    // An IPv4 network only ever matches IPv4 (and vice versa) — otherwise
    // "0.0.0.0/0" would quietly cover the whole v6 space too. The /0 case is
    // still an explicit "everything of this family".
    if (is_v4_mapped(net) != is_v4_mapped(addr)) return false;
    return prefix_equal(addr, net, bits);
}

bool rs_ip_pattern_valid(const char *pattern) {
    if (!pattern || !pattern[0]) return false;
    if (strcmp(pattern, "any") == 0 || strcmp(pattern, "*") == 0 ||
        strcmp(pattern, "loopback") == 0 || strcmp(pattern, "private") == 0) {
        return true;
    }
    uint8_t net[16];
    unsigned bits = 0;
    return parse_cidr(pattern, net, &bits);
}

bool rs_ip_list_valid(const char *list, char *bad, size_t bad_cap) {
    if (bad && bad_cap > 0) bad[0] = '\0';
    if (!list) return true;
    char token[80];
    const char *cursor = list;
    while (next_token(&cursor, token, sizeof(token))) {
        if (!rs_ip_pattern_valid(token)) {
            if (bad && bad_cap > 0) {
                size_t len = strlen(token);
                if (len >= bad_cap) len = bad_cap - 1;
                memcpy(bad, token, len);
                bad[len] = '\0';
            }
            return false;
        }
    }
    return true;
}

static bool matches_group(const uint8_t addr[16], const char *const *patterns) {
    char text[64];
    // Re-render the address so the shared rs_ip_matches path can be reused for
    // each member of the group without a second matcher.
    for (size_t i = 0; patterns[i]; i++) {
        const char *slash = strchr(patterns[i], '/');
        size_t net_len = slash ? (size_t)(slash - patterns[i]) : strlen(patterns[i]);
        if (net_len >= sizeof(text)) continue;
        memcpy(text, patterns[i], net_len);
        text[net_len] = '\0';
        uint8_t net[16];
        if (!rs_ip_parse(text, net)) continue;
        unsigned bits = (unsigned)atoi(slash ? slash + 1 : "128");
        bool net_is_v4 = is_v4_mapped(net);
        if (net_is_v4 != is_v4_mapped(addr)) continue;
        if (net_is_v4) bits += 96;
        if (prefix_equal(addr, net, bits)) return true;
    }
    return false;
}

// Copies the next comma/space-separated token of `*cursor` into `out`, advancing
// the cursor. Returns false when the list is exhausted.
static bool next_token(const char **cursor, char *out, size_t out_cap) {
    const char *p = *cursor;
    while (*p == ' ' || *p == '\t' || *p == ',') p++;
    if (!*p) { *cursor = p; return false; }
    const char *start = p;
    while (*p && *p != ',' && *p != ' ' && *p != '\t') p++;
    size_t len = (size_t)(p - start);
    if (len >= out_cap) len = out_cap - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    *cursor = p;
    return true;
}

bool rs_ip_in_list(const char *ip, const char *list) {
    if (!ip || !list) return false;
    char token[80];
    const char *cursor = list;
    while (next_token(&cursor, token, sizeof(token))) {
        if (rs_ip_matches(ip, token)) return true;
    }
    return false;
}

char *rs_client_ip(const char *peer, const char *xff, const char *trusted) {
    const char *fallback = peer ? peer : "";
    if (!peer || !rs_ip_in_list(peer, trusted)) return rs_strdup(fallback);
    if (!xff || !xff[0]) return rs_strdup(fallback);

    // Collect the hops, then walk right-to-left. The rightmost entry was
    // appended by our own trusted proxy and is therefore the one it vouches
    // for; anything further left was supplied by whoever came before and can be
    // forged. Skipping trusted entries handles a chain of our own proxies.
    char hops[16][80];
    size_t count = 0;
    const char *cursor = xff;
    while (count < 16 && next_token(&cursor, hops[count], sizeof(hops[0]))) {
        uint8_t parsed[16];
        if (rs_ip_parse(hops[count], parsed)) count++;
    }
    for (size_t i = count; i > 0; i--) {
        if (!rs_ip_in_list(hops[i - 1], trusted)) return rs_strdup(hops[i - 1]);
    }
    // Every hop was one of ours: the leftmost is as close to the client as this
    // header gets.
    return rs_strdup(count > 0 ? hops[0] : fallback);
}

bool rs_request_is_secure(const char *peer, const char *forwarded_proto,
                          const char *trusted, bool direct_tls) {
    if (direct_tls) return true;
    if (!forwarded_proto || !forwarded_proto[0]) return false;
    if (!peer || !rs_ip_in_list(peer, trusted)) return false;
    // "https", or the first entry of "https, http" when several proxies each
    // appended one.
    const char *p = forwarded_proto;
    while (*p == ' ' || *p == '\t') p++;
    return (p[0] == 'h' || p[0] == 'H') &&
           (p[1] == 't' || p[1] == 'T') && (p[2] == 't' || p[2] == 'T') &&
           (p[3] == 'p' || p[3] == 'P') && (p[4] == 's' || p[4] == 'S') &&
           (p[5] == '\0' || p[5] == ',' || p[5] == ' ' || p[5] == ';');
}
