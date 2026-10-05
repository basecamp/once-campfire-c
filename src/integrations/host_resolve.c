/* src/integrations/host_resolve.c — shared private-network host resolver
 * (packet V-G).
 *
 * Port of tmp/rust-ref/crates/campfire/src/integrations/net/guard.rs: the
 * host-syntax gates, the inet_aton numeric forms, the DISALLOWED tables and
 * the v4-before-v6 public-first selection. Section comments name the Rust
 * function each block translates.
 */
#include "integrations/host_resolve.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#define CF_HOST_MAX_BYTES 255
#define CF_HOST_MAX_LABEL 63
#define CF_HOST_MAX_ADDRESSES 256
/* One spare slot so an over-long answer list is detected, not truncated. */
#define CF_HOST_COLLECT_CAP (CF_HOST_MAX_ADDRESSES + 1)

/* --- small scanners ------------------------------------------------------- */

static bool host_is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

static bool host_is_alnum(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

static bool host_is_hexdigit(unsigned char c) {
    return host_is_digit(c) || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static bool host_has_colon(const unsigned char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (s[i] == ':') return true;
    }
    return false;
}

/* `legacy_ipv4_shape`: 1-4 dot-separated (empty parts ignored) decimal or
 * 0x-hex numbers. */
static bool host_legacy_ipv4_shape(const unsigned char *s, size_t len) {
    size_t parts = 0;
    size_t i = 0;
    while (i < len) {
        while (i < len && s[i] == '.') i++;
        if (i >= len) break;
        size_t start = i;
        while (i < len && s[i] != '.') i++;
        size_t plen = i - start;
        const unsigned char *part = s + start;
        if (plen >= 2 && part[0] == '0' &&
            (part[1] == 'x' || part[1] == 'X')) {
            if (plen == 2) return false;
            for (size_t k = 2; k < plen; k++) {
                if (!host_is_hexdigit(part[k])) return false;
            }
        } else {
            for (size_t k = 0; k < plen; k++) {
                if (!host_is_digit(part[k])) return false;
            }
        }
        parts++;
    }
    return parts >= 1 && parts <= 4;
}

static bool host_parse_v6(const unsigned char *s, size_t len,
                          unsigned char out[16]) {
    if (len >= 256) return false;
    char text[256];
    memcpy(text, s, len);
    text[len] = '\0';
    return inet_pton(AF_INET6, text, out) == 1;
}

/* `ip_literal`: dotted-quad IPv4 (four decimal octets, no leading zeros —
 * enforced here because some libc inet_pton spellings are laxer), or IPv6
 * with optional brackets. */
static bool host_ip_literal(const unsigned char *s, size_t len,
                            cf_host_addr *out) {
    if (len >= 2 && s[0] == '[' && s[len - 1] == ']') {
        unsigned char bytes[16];
        if (!host_parse_v6(s + 1, len - 2, bytes)) return false;
        out->family = AF_INET6;
        memcpy(out->bytes, bytes, 16);
        return true;
    }
    if (host_has_colon(s, len)) {
        unsigned char bytes[16];
        if (!host_parse_v6(s, len, bytes)) return false;
        out->family = AF_INET6;
        memcpy(out->bytes, bytes, 16);
        return true;
    }
    size_t parts = 0;
    size_t i = 0;
    while (i <= len) {
        size_t start = i;
        while (i < len && s[i] != '.') i++;
        size_t plen = i - start;
        if (plen == 0 || plen > 3) return false;
        for (size_t k = 0; k < plen; k++) {
            if (!host_is_digit(s[start + k])) return false;
        }
        /* IPAddr only takes octets without leading zeros ("01" rejected). */
        if (plen > 1 && s[start] == '0') return false;
        parts++;
        if (i >= len) break;
        i++;
    }
    if (parts != 4) return false;
    if (len >= 256) return false;
    char text[256];
    memcpy(text, s, len);
    text[len] = '\0';
    unsigned char bytes[4];
    if (inet_pton(AF_INET, text, bytes) != 1) return false;
    out->family = AF_INET;
    memset(out->bytes, 0, 16);
    memcpy(out->bytes, bytes, 4);
    return true;
}

static bool host_full_width_literal(const unsigned char *s, size_t len) {
    cf_host_addr ignored;
    return host_ip_literal(s, len, &ignored);
}

/* `valid_host_syntax`. */
static bool host_valid_syntax(const unsigned char *s, size_t len) {
    if (host_has_colon(s, len) || host_legacy_ipv4_shape(s, len) ||
        host_full_width_literal(s, len)) {
        return true;
    }
    size_t end = len;
    if (end > 0 && s[end - 1] == '.') end--; /* one trailing dot allowed */
    if (end == 0) return false;
    size_t i = 0;
    while (i < end) {
        size_t start = i;
        while (i < end && s[i] != '.') i++;
        size_t plen = i - start;
        if (plen == 0 || plen > CF_HOST_MAX_LABEL) return false;
        if (!host_is_alnum(s[start]) || !host_is_alnum(s[i - 1])) return false;
        for (size_t k = 0; k < plen; k++) {
            unsigned char c = s[start + k];
            if (!host_is_alnum(c) && c != '-') return false;
        }
        if (i < end) i++; /* skip '.' */
    }
    return true;
}

/* `malformed_numeric_host_candidate`. */
static bool host_malformed_numeric(const unsigned char *s, size_t len) {
    if (host_has_colon(s, len)) return false;
    size_t core_start = 0;
    while (core_start < len &&
           (s[core_start] == '%' || s[core_start] == '/')) {
        core_start++;
    }
    size_t core_end = core_start;
    while (core_end < len && s[core_end] != '%' && s[core_end] != '/') {
        core_end++;
    }
    size_t core_len = core_end - core_start;
    const unsigned char *core = s + core_start;
    /* `core != host || core.split('.').any(str::is_empty)`: a leading,
     * trailing or doubled dot yields an empty part (an empty core counts
     * as one empty part, as in Rust). */
    bool malformed = core_len != len;
    if (!malformed) {
        size_t i = 0;
        if (core_len == 0) {
            malformed = true;
        }
        while (!malformed && i <= core_len) {
            size_t start = i;
            while (i < core_len && core[i] != '.') i++;
            if (i == start) malformed = true; /* empty part */
            i++; /* skip '.'; a trailing dot ends past core_len, which the
                  * next round reports as empty */
        }
    }
    if (!malformed) return false;
    return host_legacy_ipv4_shape(core, core_len) &&
           !host_full_width_literal(s, len);
}

static bool host_numeric_candidate(const unsigned char *s, size_t len) {
    return host_has_colon(s, len) || host_legacy_ipv4_shape(s, len);
}

/* `inet_aton` (glibc __inet_aton_exact): 1-4 parts in decimal, octal
 * (leading 0) or hex (0x); the last part fills the remaining bytes. */
static bool host_inet_aton(const unsigned char *s, size_t len, uint32_t *out) {
    uint32_t values[4];
    size_t count = 0;
    size_t i = 0;
    while (true) {
        size_t start = i;
        while (i < len && s[i] != '.') i++;
        size_t plen = i - start;
        if (plen == 0) return false;
        const unsigned char *part = s + start;
        const unsigned char *digits = part;
        size_t dlen = plen;
        unsigned radix = 10;
        if (plen >= 2 && part[0] == '0' &&
            (part[1] == 'x' || part[1] == 'X')) {
            digits = part + 2;
            dlen = plen - 2;
            radix = 16;
        } else if (plen > 1 && part[0] == '0') {
            digits = part + 1;
            dlen = plen - 1;
            radix = 8;
        }
        /* An empty digit run ("0x") reads as 0; anything else must be all
         * valid digits for its radix. */
        uint64_t value = 0;
        for (size_t k = 0; k < dlen; k++) {
            unsigned d;
            if (radix == 16) {
                unsigned char c = digits[k];
                if (host_is_digit(c)) {
                    d = (unsigned)(c - '0');
                } else if (c >= 'a' && c <= 'f') {
                    d = (unsigned)(c - 'a') + 10;
                } else if (c >= 'A' && c <= 'F') {
                    d = (unsigned)(c - 'A') + 10;
                } else {
                    return false;
                }
            } else {
                if (!host_is_digit(digits[k]) ||
                    (unsigned)(digits[k] - '0') >= radix) {
                    return false;
                }
                d = (unsigned)(digits[k] - '0');
            }
            value = value * radix + d;
            if (value > UINT32_MAX) return false;
        }
        if (count >= 4) return false;
        values[count++] = (uint32_t)value;
        if (i >= len) break;
        i++; /* skip '.'; a trailing dot leaves an empty part, rejected */
    }
    if (count == 0 || count > 4) return false;
    for (size_t k = 0; k + 1 < count; k++) {
        if (values[k] > 0xff) return false;
    }
    uint32_t last = values[count - 1];
    if (count < 4) {
        uint32_t bits = 32 - 8 * (uint32_t)(count - 1);
        if (bits < 32 && last >= (1u << bits)) return false;
    }
    uint32_t addr = last;
    for (size_t k = 0; k + 1 < count; k++) {
        addr |= values[k] << (24 - 8 * (uint32_t)k);
    }
    *out = addr;
    return true;
}

/* `getaddrinfo_numeric`: inet_aton forms for IPv4, v6 parse for IPv6. */
static bool host_getaddrinfo_numeric(const unsigned char *s, size_t len,
                                     cf_host_addr *out) {
    if (host_has_colon(s, len)) {
        unsigned char bytes[16];
        if (!host_parse_v6(s, len, bytes)) return false;
        out->family = AF_INET6;
        memcpy(out->bytes, bytes, 16);
        return true;
    }
    uint32_t v4;
    if (!host_inet_aton(s, len, &v4)) return false;
    out->family = AF_INET;
    memset(out->bytes, 0, 16);
    out->bytes[0] = (unsigned char)(v4 >> 24);
    out->bytes[1] = (unsigned char)(v4 >> 16);
    out->bytes[2] = (unsigned char)(v4 >> 8);
    out->bytes[3] = (unsigned char)v4;
    return true;
}

/* --- classification (Surfguard.blocked_address?, default policy) ----------- */

static uint32_t host_v4_of(const cf_host_addr *addr) {
    return ((uint32_t)addr->bytes[0] << 24) |
           ((uint32_t)addr->bytes[1] << 16) |
           ((uint32_t)addr->bytes[2] << 8) | (uint32_t)addr->bytes[3];
}

static unsigned __int128 host_v6_of(const cf_host_addr *addr) {
    unsigned __int128 v = 0;
    for (size_t i = 0; i < 16; i++) {
        v = (v << 8) | addr->bytes[i];
    }
    return v;
}

/* `Surfguard::DISALLOWED_IPV4`, as (network, prefix) pairs. */
static const struct {
    uint32_t net;
    unsigned prefix;
} host_disallowed_v4[] = {
    {0x00000000u, 8},   /* 0.0.0.0/8 ("this network") */
    {0x0a000000u, 8},   /* 10.0.0.0/8 */
    {0x64400000u, 10},  /* 100.64.0.0/10 (shared CGNAT) */
    {0x7f000000u, 8},   /* 127.0.0.0/8 (loopback) */
    {0xa83f8110u, 32},  /* 168.63.129.16/32 (Azure IMDS) */
    {0xa9fe0000u, 16},  /* 169.254.0.0/16 (link-local) */
    {0xac100000u, 12},  /* 172.16.0.0/12 */
    {0xc0000000u, 24},  /* 192.0.0.0/24 (IETF assignments) */
    {0xc0000200u, 24},  /* 192.0.2.0/24 (TEST-NET-1) */
    {0xc0586300u, 24},  /* 192.88.99.0/24 (6to4 relay) */
    {0xc0a80000u, 16},  /* 192.168.0.0/16 */
    {0xc6120000u, 15},  /* 198.18.0.0/15 (benchmarking) */
    {0xc6336400u, 24},  /* 198.51.100.0/24 (TEST-NET-2) */
    {0xcb007100u, 24},  /* 203.0.113.0/24 (TEST-NET-3) */
    {0xe0000000u, 4},   /* 224.0.0.0/4 (multicast) */
    {0xf0000000u, 4},   /* 240.0.0.0/4 (reserved) */
};

static bool host_in_v4(uint32_t ip, uint32_t net, unsigned prefix) {
    if (prefix == 0) return true;
    return (ip ^ net) >> (32 - prefix) == 0;
}

static bool host_disallowed_ipv4(uint32_t ip) {
    for (size_t i = 0;
         i < sizeof host_disallowed_v4 / sizeof host_disallowed_v4[0];
         i++) {
        if (host_in_v4(ip, host_disallowed_v4[i].net,
                       host_disallowed_v4[i].prefix)) {
            return true;
        }
    }
    return false;
}

/* `Surfguard::DISALLOWED_IPV6`, `IANA_ALLOCATED_IPV6_UNICAST` and the
 * single-range constants, as top-64/bottom-64 halves (all listed prefixes
 * are <= 128; shifts of 64/128 are avoided by comparing halves). */
typedef struct {
    uint64_t hi, lo;
    unsigned prefix;
} host_v6range;

#define CF_V6R(a, b, c, d, e, f, g, h, p)                                      \
    {                                                                          \
        (((uint64_t)(a) << 48) | ((uint64_t)(b) << 32) | ((uint64_t)(c) << 16) | \
         (uint64_t)(d)),                                                       \
            (((uint64_t)(e) << 48) | ((uint64_t)(f) << 32) |                    \
             ((uint64_t)(g) << 16) | (uint64_t)(h)),                           \
            (p)                                                                \
    }

static const host_v6range host_disallowed_v6[] = {
    CF_V6R(0, 0, 0, 0, 0, 0, 0, 0, 128), /* ::/128 */
    CF_V6R(0x100, 0, 0, 0, 0, 0, 0, 0, 64),
    CF_V6R(0x100, 0, 0, 1, 0, 0, 0, 0, 64),
    CF_V6R(0x2001, 0, 0, 0, 0, 0, 0, 0, 32), /* TEREDO + IETF below */
    CF_V6R(0x2001, 2, 0, 0, 0, 0, 0, 0, 48), /* 6to4 */
    CF_V6R(0x2001, 0xdb8, 0, 0, 0, 0, 0, 0, 32), /* documentation */
    CF_V6R(0x2002, 0, 0, 0, 0, 0, 0, 0, 16), /* 6to4 */
    CF_V6R(0x3fff, 0, 0, 0, 0, 0, 0, 0, 20),
    CF_V6R(0x5f00, 0, 0, 0, 0, 0, 0, 0, 16), /* segment routing SRv6 */
    CF_V6R(0xfec0, 0, 0, 0, 0, 0, 0, 0, 10), /* site-local (deprecated) */
    CF_V6R(0xff00, 0, 0, 0, 0, 0, 0, 0, 8), /* multicast */
};

static const host_v6range host_iana_v6unicast[] = {
    CF_V6R(0x2001, 0, 0, 0, 0, 0, 0, 0, 23),
    CF_V6R(0x2001, 0x200, 0, 0, 0, 0, 0, 0, 23),
    CF_V6R(0x2001, 0x400, 0, 0, 0, 0, 0, 0, 23),
    CF_V6R(0x2001, 0x600, 0, 0, 0, 0, 0, 0, 23),
    CF_V6R(0x2001, 0x800, 0, 0, 0, 0, 0, 0, 22),
    CF_V6R(0x2001, 0xc00, 0, 0, 0, 0, 0, 0, 23),
    CF_V6R(0x2001, 0xe00, 0, 0, 0, 0, 0, 0, 23),
    CF_V6R(0x2001, 0x1200, 0, 0, 0, 0, 0, 0, 23),
    CF_V6R(0x2001, 0x1400, 0, 0, 0, 0, 0, 0, 22),
    CF_V6R(0x2001, 0x1800, 0, 0, 0, 0, 0, 0, 23),
    CF_V6R(0x2001, 0x1a00, 0, 0, 0, 0, 0, 0, 23),
    CF_V6R(0x2001, 0x1c00, 0, 0, 0, 0, 0, 0, 22),
    CF_V6R(0x2001, 0x2000, 0, 0, 0, 0, 0, 0, 19),
    CF_V6R(0x2001, 0x4000, 0, 0, 0, 0, 0, 0, 23),
    CF_V6R(0x2001, 0x4200, 0, 0, 0, 0, 0, 0, 23),
    CF_V6R(0x2001, 0x4400, 0, 0, 0, 0, 0, 0, 23),
    CF_V6R(0x2001, 0x4600, 0, 0, 0, 0, 0, 0, 23),
    CF_V6R(0x2001, 0x4800, 0, 0, 0, 0, 0, 0, 23),
    CF_V6R(0x2001, 0x4a00, 0, 0, 0, 0, 0, 0, 23),
    CF_V6R(0x2001, 0x4c00, 0, 0, 0, 0, 0, 0, 23),
    CF_V6R(0x2001, 0x5000, 0, 0, 0, 0, 0, 0, 20),
    CF_V6R(0x2001, 0x8000, 0, 0, 0, 0, 0, 0, 19),
    CF_V6R(0x2001, 0xa000, 0, 0, 0, 0, 0, 0, 20),
    CF_V6R(0x2001, 0xb000, 0, 0, 0, 0, 0, 0, 20),
    CF_V6R(0x2002, 0, 0, 0, 0, 0, 0, 0, 16),
    CF_V6R(0x2003, 0, 0, 0, 0, 0, 0, 0, 18),
    CF_V6R(0x2400, 0, 0, 0, 0, 0, 0, 0, 12),
    CF_V6R(0x2410, 0, 0, 0, 0, 0, 0, 0, 12),
    CF_V6R(0x2600, 0, 0, 0, 0, 0, 0, 0, 12),
    CF_V6R(0x2610, 0, 0, 0, 0, 0, 0, 0, 23),
    CF_V6R(0x2620, 0, 0, 0, 0, 0, 0, 0, 23),
    CF_V6R(0x2630, 0, 0, 0, 0, 0, 0, 0, 12),
    CF_V6R(0x2800, 0, 0, 0, 0, 0, 0, 0, 12),
    CF_V6R(0x2a00, 0, 0, 0, 0, 0, 0, 0, 12),
    CF_V6R(0x2a10, 0, 0, 0, 0, 0, 0, 0, 12),
    CF_V6R(0x2c00, 0, 0, 0, 0, 0, 0, 0, 12),
};

static bool host_in_v6(unsigned __int128 ip, const host_v6range *range) {
    if (range->prefix == 0) return true;
    unsigned __int128 net = ((unsigned __int128)range->hi << 64) | range->lo;
    return (ip ^ net) >> (128 - range->prefix) == 0;
}

static bool host_in_v6_table(unsigned __int128 ip, const host_v6range *table,
                             size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (host_in_v6(ip, &table[i])) return true;
    }
    return false;
}

static bool host_disallowed_ipv6(unsigned __int128 ip) {
    static const host_v6range globally_reachable[] = {
        CF_V6R(0x2001, 3, 0, 0, 0, 0, 0, 0, 32),
        CF_V6R(0x2001, 4, 0x112, 0, 0, 0, 0, 0, 48),
    };
    static const host_v6range unique_local =
        CF_V6R(0xfc00, 0, 0, 0, 0, 0, 0, 0, 7);
    static const host_v6range link_local =
        CF_V6R(0xfe80, 0, 0, 0, 0, 0, 0, 0, 10);
    static const host_v6range ietf_protocol =
        CF_V6R(0x2001, 0, 0, 0, 0, 0, 0, 0, 23);
    if (host_in_v6_table(ip, globally_reachable,
                         sizeof globally_reachable /
                             sizeof globally_reachable[0])) {
        return false;
    }
    if (host_in_v6(ip, &unique_local) || ip == 1 ||
        host_in_v6(ip, &link_local) || host_in_v6(ip, &ietf_protocol)) {
        return true;
    }
    if (host_in_v6_table(ip, host_disallowed_v6,
                         sizeof host_disallowed_v6 /
                             sizeof host_disallowed_v6[0])) {
        return true;
    }
    return !host_in_v6_table(ip, host_iana_v6unicast,
                             sizeof host_iana_v6unicast /
                                 sizeof host_iana_v6unicast[0]);
}

cf_host_addr cf_host_addr_v4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    cf_host_addr addr;
    addr.family = AF_INET;
    memset(addr.bytes, 0, sizeof addr.bytes);
    addr.bytes[0] = a;
    addr.bytes[1] = b;
    addr.bytes[2] = c;
    addr.bytes[3] = d;
    return addr;
}

bool cf_host_addr_parse(cf_span text, cf_host_addr *out) {
    if (out == NULL || text.ptr == NULL) return false;
    return host_ip_literal(text.ptr, text.len, out);
}

/* `Surfguard.blocked_address?`. */
bool cf_host_addr_blocked(cf_host_addr addr) {
    if (addr.family == AF_INET) {
        return host_disallowed_ipv4(host_v4_of(&addr));
    }
    if (addr.family != AF_INET6) return true;
    const unsigned char *b = addr.bytes;
    bool first8_zero = true;
    for (size_t i = 0; i < 8; i++) {
        if (b[i] != 0) {
            first8_zero = false;
            break;
        }
    }
    bool first10_zero = first8_zero && b[8] == 0 && b[9] == 0;
    bool mapped = first10_zero && b[10] == 0xff && b[11] == 0xff;
    bool compatible = first10_zero && b[10] == 0 && b[11] == 0;
    bool nat64_local = b[0] == 0 && b[1] == 0x64 && b[2] == 0xff &&
                       b[3] == 0x9b && b[4] == 0 && b[5] == 1;
    if (mapped || compatible || nat64_local) return true;
    unsigned __int128 ip = host_v6_of(&addr);
    bool nat64_known = true;
    static const unsigned char nat64_prefix[12] = {0, 0x64, 0xff, 0x9b, 0, 0,
                                                  0, 0,    0,    0,    0, 0};
    for (size_t i = 0; i < 12; i++) {
        if (b[i] != nat64_prefix[i]) {
            nat64_known = false;
            break;
        }
    }
    bool translatable =
        first8_zero && b[8] == 0xff && b[9] == 0xff && !mapped;
    if (nat64_known || translatable) {
        return host_disallowed_ipv4((uint32_t)(ip & 0xffffffffu));
    }
    return host_disallowed_ipv6(ip);
}

/* --- resolution ------------------------------------------------------------ */

/* `normal_host`: ASCII, no NUL, not empty, at most 255 bytes, no zone. */
static bool host_normal(const unsigned char *s, size_t len) {
    if (len == 0 || len > CF_HOST_MAX_BYTES) return false;
    for (size_t i = 0; i < len; i++) {
        if (s[i] >= 0x80 || s[i] == '%') return false;
    }
    return true;
}

static bool host_addr_equal(cf_host_addr a, cf_host_addr b) {
    return a.family == b.family && memcmp(a.bytes, b.bytes, 16) == 0;
}

static cf_err host_lookup_system(void *arg, const char *host,
                                 cf_host_addr *out, size_t cap,
                                 size_t *out_len) {
    (void)arg;
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0) return CF_NOT_FOUND;
    size_t n = 0;
    cf_err rc = CF_OK;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        cf_host_addr addr;
        memset(&addr, 0, sizeof addr);
        if (ai->ai_family == AF_INET) {
            struct sockaddr_in *v4 = (struct sockaddr_in *)ai->ai_addr;
            addr.family = AF_INET;
            memcpy(addr.bytes, &v4->sin_addr, 4);
        } else if (ai->ai_family == AF_INET6) {
            struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)ai->ai_addr;
            addr.family = AF_INET6;
            memcpy(addr.bytes, &v6->sin6_addr, 16);
        } else {
            continue;
        }
        bool seen = false;
        for (size_t i = 0; i < n; i++) {
            if (host_addr_equal(out[i], addr)) {
                seen = true;
                break;
            }
        }
        if (seen) continue;
        if (n >= cap) {
            rc = CF_NOT_FOUND; /* over CF_HOST_MAX_ADDRESSES answers */
            n = 0;
            break;
        }
        out[n++] = addr;
    }
    freeaddrinfo(res);
    if (rc != CF_OK) return rc;
    *out_len = n;
    return CF_OK;
}

/* Write the first public address (v4 before v6, lookup order within each
 * family) as inet_ntop text. */
static cf_err host_first_public_text(cf_host_addr *addrs, size_t count,
                                     char *out, size_t out_cap) {
    if (out == NULL || out_cap < INET6_ADDRSTRLEN) return CF_LIMIT;
    for (int pass = 0; pass < 2; pass++) {
        int want = pass == 0 ? AF_INET : AF_INET6;
        for (size_t i = 0; i < count; i++) {
            if (addrs[i].family != want) continue;
            if (cf_host_addr_blocked(addrs[i])) continue;
            char text[INET6_ADDRSTRLEN];
            const char *printed =
                inet_ntop((unsigned)addrs[i].family, addrs[i].bytes, text,
                          sizeof text);
            if (printed == NULL) return CF_IO;
            size_t len = strlen(text) + 1;
            if (len > out_cap) return CF_LIMIT;
            memcpy(out, text, len);
            return CF_OK;
        }
    }
    return CF_FORBIDDEN;
}

cf_err cf_host_resolve_public(const cf_host_resolver *resolver,
                              const char *host, char *out, size_t out_cap) {
    if (host == NULL || out == NULL) return CF_INVALID;
    size_t len = strlen(host);
    const unsigned char *s = (const unsigned char *)host;
    if (!host_normal(s, len)) return CF_FORBIDDEN; /* malformed: Violation */
    if (!host_valid_syntax(s, len) || host_malformed_numeric(s, len)) {
        return CF_FORBIDDEN;
    }
    cf_host_addr literal;
    memset(&literal, 0, sizeof literal);
    if (host_getaddrinfo_numeric(s, len, &literal) ||
        host_ip_literal(s, len, &literal)) {
        /* Numeric hosts never reach DNS. */
        if (cf_host_addr_blocked(literal)) return CF_FORBIDDEN;
        if (out_cap < INET6_ADDRSTRLEN) return CF_LIMIT;
        const char *printed =
            inet_ntop((unsigned)literal.family, literal.bytes, out,
                      (socklen_t)out_cap);
        return printed != NULL ? CF_OK : CF_IO;
    }
    if (host_numeric_candidate(s, len)) return CF_FORBIDDEN;
    cf_host_lookup_fn lookup = host_lookup_system;
    void *arg = NULL;
    if (resolver != NULL && resolver->lookup != NULL) {
        lookup = resolver->lookup;
        arg = resolver->arg;
    }
    cf_host_addr answers[CF_HOST_COLLECT_CAP];
    size_t count = 0;
    /* normalize_answers: lookup failure or empty is Unresolvable. */
    if (lookup(arg, host, answers, CF_HOST_COLLECT_CAP, &count) != CF_OK) {
        return CF_NOT_FOUND;
    }
    if (count == 0 || count > CF_HOST_MAX_ADDRESSES) return CF_NOT_FOUND;
    size_t unique = 0;
    for (size_t i = 0; i < count; i++) {
        bool seen = false;
        for (size_t k = 0; k < unique; k++) {
            if (host_addr_equal(answers[k], answers[i])) {
                seen = true;
                break;
            }
        }
        if (!seen) answers[unique++] = answers[i];
    }
    if (unique == 0) return CF_NOT_FOUND;
    return host_first_public_text(answers, unique, out, out_cap);
}

cf_optional_str cf_host_resolve_fn(void *arg, cf_str host) {
    cf_optional_str absent = {false, {NULL, 0}};
    if (host.ptr == NULL || host.len == 0 ||
        host.len > CF_HOST_MAX_BYTES) {
        return absent;
    }
    /* normal_host rejects embedded NUL (a cf_str can carry one, a C string
     * cannot); fail closed rather than resolving a truncated name. */
    for (size_t i = 0; i < host.len; i++) {
        if (host.ptr[i] == '\0') return absent;
    }
    char name[CF_HOST_MAX_BYTES + 1];
    memcpy(name, host.ptr, host.len);
    name[host.len] = '\0';
    const cf_host_resolver *resolver = NULL;
    cf_host_resolver system = {NULL, NULL};
    if (arg != NULL) {
        resolver = (const cf_host_resolver *)arg;
    } else {
        resolver = &system;
    }
    char text[INET6_ADDRSTRLEN];
    if (cf_host_resolve_public(resolver, name, text, sizeof text) != CF_OK) {
        return absent;
    }
    size_t len = strlen(text);
    char *copy = malloc(len + 1);
    if (copy == NULL) return absent;
    memcpy(copy, text, len + 1);
    return (cf_optional_str){true, {copy, len}};
}
