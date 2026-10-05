/* src/integrations/unfurl.c — I01 link unfurling (part 1: guard + media filter).
 *
 * Reference: opengraph/location.rs + net/guard.rs (RestrictedHTTP::
 * PrivateNetworkGuard / surfguard default policy).
 */
#include "integrations/unfurl.h"

#include <ctype.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>

#include "integrations/http.h"

/* Richtext reuse (read-only): Unicode blank checks, UTF-8 decode, and the
 * DOM/sanitizer used by validate(). Owned by R02; linked, not modified. */
#include "richtext/internal.h"

/* --- small utils ------------------------------------------------------------ */

static char *xstrndup(const char *s, size_t n) {
    char *o = malloc(n + 1);
    if (!o) return NULL;
    memcpy(o, s, n);
    o[n] = '\0';
    return o;
}

static long ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* --- guard: classification tables (surfguard default policy) ---------------- */

typedef struct {
    uint32_t net;
    uint8_t prefix;
} v4range;
typedef struct {
    unsigned __int128 net;
    uint8_t prefix;
} v6range;

#define V4(a, b, c, d, p) {(uint32_t)(((a) << 24) | ((b) << 16) | ((c) << 8) | (d)), p}
#define V6(s0, s1, s2, s3, s4, s5, s6, s7, p)                              \
    {(((unsigned __int128)(s0) << 112) | ((unsigned __int128)(s1) << 96) | \
      ((unsigned __int128)(s2) << 80) | ((unsigned __int128)(s3) << 64) |  \
      ((unsigned __int128)(s4) << 48) | ((unsigned __int128)(s5) << 32) |  \
      ((unsigned __int128)(s6) << 16) | (unsigned __int128)(s7)),           \
     p}

static const v4range kDisV4[] = {
    V4(0, 0, 0, 0, 8), V4(10, 0, 0, 0, 8), V4(100, 64, 0, 0, 10),
    V4(127, 0, 0, 0, 8), V4(168, 63, 129, 16, 32), V4(169, 254, 0, 0, 16),
    V4(172, 16, 0, 0, 12), V4(192, 0, 0, 0, 24), V4(192, 0, 2, 0, 24),
    V4(192, 88, 99, 0, 24), V4(192, 168, 0, 0, 16), V4(198, 18, 0, 0, 15),
    V4(198, 51, 100, 0, 24), V4(203, 0, 113, 0, 24), V4(224, 0, 0, 0, 4),
    V4(240, 0, 0, 0, 4),
};

static const v6range kDisV6[] = {
    V6(0, 0, 0, 0, 0, 0, 0, 0, 128), V6(0x100, 0, 0, 0, 0, 0, 0, 0, 64),
    V6(0x100, 0, 0, 1, 0, 0, 0, 0, 64), V6(0x2001, 0, 0, 0, 0, 0, 0, 0, 32),
    V6(0x2001, 2, 0, 0, 0, 0, 0, 0, 48), V6(0x2001, 0xdb8, 0, 0, 0, 0, 0, 0, 32),
    V6(0x2002, 0, 0, 0, 0, 0, 0, 0, 16), V6(0x3fff, 0, 0, 0, 0, 0, 0, 0, 20),
    V6(0x5f00, 0, 0, 0, 0, 0, 0, 0, 16), V6(0xfec0, 0, 0, 0, 0, 0, 0, 0, 10),
    V6(0xff00, 0, 0, 0, 0, 0, 0, 0, 8),
};

static const v6range kIanaV6[] = {
    V6(0x2001, 0, 0, 0, 0, 0, 0, 0, 23), V6(0x2001, 0x200, 0, 0, 0, 0, 0, 0, 23),
    V6(0x2001, 0x400, 0, 0, 0, 0, 0, 0, 23), V6(0x2001, 0x600, 0, 0, 0, 0, 0, 0, 23),
    V6(0x2001, 0x800, 0, 0, 0, 0, 0, 0, 22), V6(0x2001, 0xc00, 0, 0, 0, 0, 0, 0, 23),
    V6(0x2001, 0xe00, 0, 0, 0, 0, 0, 0, 23), V6(0x2001, 0x1200, 0, 0, 0, 0, 0, 0, 23),
    V6(0x2001, 0x1400, 0, 0, 0, 0, 0, 0, 22), V6(0x2001, 0x1800, 0, 0, 0, 0, 0, 0, 23),
    V6(0x2001, 0x1a00, 0, 0, 0, 0, 0, 0, 23), V6(0x2001, 0x1c00, 0, 0, 0, 0, 0, 0, 22),
    V6(0x2001, 0x2000, 0, 0, 0, 0, 0, 0, 19), V6(0x2001, 0x4000, 0, 0, 0, 0, 0, 0, 23),
    V6(0x2001, 0x4200, 0, 0, 0, 0, 0, 0, 23), V6(0x2001, 0x4400, 0, 0, 0, 0, 0, 0, 23),
    V6(0x2001, 0x4600, 0, 0, 0, 0, 0, 0, 23), V6(0x2001, 0x4800, 0, 0, 0, 0, 0, 0, 23),
    V6(0x2001, 0x4a00, 0, 0, 0, 0, 0, 0, 23), V6(0x2001, 0x4c00, 0, 0, 0, 0, 0, 0, 23),
    V6(0x2001, 0x5000, 0, 0, 0, 0, 0, 0, 20), V6(0x2001, 0x8000, 0, 0, 0, 0, 0, 0, 19),
    V6(0x2001, 0xa000, 0, 0, 0, 0, 0, 0, 20), V6(0x2001, 0xb000, 0, 0, 0, 0, 0, 0, 20),
    V6(0x2002, 0, 0, 0, 0, 0, 0, 0, 16), V6(0x2003, 0, 0, 0, 0, 0, 0, 0, 18),
    V6(0x2400, 0, 0, 0, 0, 0, 0, 0, 12), V6(0x2410, 0, 0, 0, 0, 0, 0, 0, 12),
    V6(0x2600, 0, 0, 0, 0, 0, 0, 0, 12), V6(0x2610, 0, 0, 0, 0, 0, 0, 0, 23),
    V6(0x2620, 0, 0, 0, 0, 0, 0, 0, 23), V6(0x2630, 0, 0, 0, 0, 0, 0, 0, 12),
    V6(0x2800, 0, 0, 0, 0, 0, 0, 0, 12), V6(0x2a00, 0, 0, 0, 0, 0, 0, 0, 12),
    V6(0x2a10, 0, 0, 0, 0, 0, 0, 0, 12), V6(0x2c00, 0, 0, 0, 0, 0, 0, 0, 12),
};

static const v6range kGlobalIetf[] = {V6(0x2001, 3, 0, 0, 0, 0, 0, 0, 32), V6(0x2001, 4, 0x112, 0, 0, 0, 0, 0, 48)};
static const v6range kIetfProto = V6(0x2001, 0, 0, 0, 0, 0, 0, 0, 23);
static const v6range kNat64Well = V6(0x64, 0xff9b, 0, 0, 0, 0, 0, 0, 96);
static const v6range kNat64Local = V6(0x64, 0xff9b, 1, 0, 0, 0, 0, 0, 48);
static const v6range kV4Mapped = V6(0, 0, 0, 0, 0, 0xffff, 0, 0, 96);
static const v6range kV4Trans = V6(0, 0, 0, 0, 0xffff, 0, 0, 0, 96);
static const v6range kV4Compat = V6(0, 0, 0, 0, 0, 0, 0, 0, 96);
static const v6range kUniqueLocal = V6(0xfc00, 0, 0, 0, 0, 0, 0, 0, 7);
static const v6range kLinkLocalV6 = V6(0xfe80, 0, 0, 0, 0, 0, 0, 0, 10);

static bool in_v4(uint32_t ip, v4range r) {
    return r.prefix == 0 || (ip ^ r.net) >> (32 - r.prefix) == 0;
}

static bool in_v6(unsigned __int128 ip, v6range r) {
    return r.prefix == 0 || (ip ^ r.net) >> (128 - r.prefix) == 0;
}

static bool disallowed_v4(uint32_t ip) {
    for (size_t i = 0; i < sizeof kDisV4 / sizeof kDisV4[0]; i++)
        if (in_v4(ip, kDisV4[i])) return true;
    return false;
}

static bool disallowed_v6(unsigned __int128 ip) {
    for (size_t i = 0; i < sizeof kGlobalIetf / sizeof kGlobalIetf[0]; i++)
        if (in_v6(ip, kGlobalIetf[i])) return false;
    if (in_v6(ip, kUniqueLocal) || ip == 1 || in_v6(ip, kLinkLocalV6) || in_v6(ip, kIetfProto))
        return true;
    for (size_t i = 0; i < sizeof kDisV6 / sizeof kDisV6[0]; i++)
        if (in_v6(ip, kDisV6[i])) return true;
    for (size_t i = 0; i < sizeof kIanaV6 / sizeof kIanaV6[0]; i++)
        if (in_v6(ip, kIanaV6[i])) return false;
    return true;
}

/* Parse an IP literal into v4/v6 words. Brackets already stripped. */
typedef struct {
    bool v6;
    uint32_t v4;
    unsigned __int128 v6w;
} ipnum;

static bool parse_ipnum(const char *s, ipnum *out) {
    struct in_addr a4;
    struct in6_addr a6;
    if (inet_pton(AF_INET, s, &a4) == 1) {
        out->v6 = false;
        memcpy(&out->v4, &a4, 4);
        out->v4 = ntohl(out->v4);
        return true;
    }
    if (inet_pton(AF_INET6, s, &a6) == 1) {
        out->v6 = true;
        out->v6w = 0;
        for (int i = 0; i < 16; i++) out->v6w = (out->v6w << 8) | a6.s6_addr[i];
        return true;
    }
    return false;
}

static bool blocked_ipnum(ipnum ip) {
    if (!ip.v6) return disallowed_v4(ip.v4);
    unsigned __int128 v = ip.v6w;
    if (in_v6(v, kV4Mapped) || in_v6(v, kV4Compat) || in_v6(v, kNat64Local)) return true;
    if (in_v6(v, kNat64Well) || in_v6(v, kV4Trans)) return disallowed_v4((uint32_t)v);
    return disallowed_v6(v);
}

bool cf_unfurl_blocked_ip(const char *ip_literal) {
    ipnum ip;
    if (!parse_ipnum(ip_literal, &ip)) return true; /* unparsable => blocked */
    return blocked_ipnum(ip);
}

/* --- guard: host syntax (guard.rs) -------------------------------------------- */

#define MAX_HOST_BYTES 255
#define MAX_ADDRS 256

static bool normal_host(const char *h) {
    size_t n = strlen(h);
    if (n == 0 || n > MAX_HOST_BYTES) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)h[i];
        if (c > 0x7f || c == '\0' || c == '%') return false;
    }
    return true;
}

static bool legacy_ipv4_shape(const char *text) {
    /* 1-4 dot-separated (empty parts ignored) decimal or 0x-hex numbers. */
    int parts = 0;
    const char *p = text;
    if (*p == '\0') return false;
    for (;;) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (len > 0) {
            bool hex = false;
            const char *num = p;
            size_t nlen = len;
            if (nlen > 2 && num[0] == '0' && (num[1] == 'x' || num[1] == 'X')) {
                hex = true;
                num += 2;
                nlen -= 2;
            }
            if (nlen == 0) return false;
            for (size_t i = 0; i < nlen; i++) {
                unsigned char c = (unsigned char)num[i];
                if (hex ? !isxdigit(c) : !isdigit(c)) return false;
            }
            parts++;
        }
        if (!dot) break;
        p = dot + 1;
        if (*p == '\0') break; /* trailing dot: remaining empty parts ignored */
    }
    return parts >= 1 && parts <= 4;
}

/* Strict dotted-quad (no leading zeros) or bracketed/bare IPv6. */
static bool ip_literal_host(const char *text, ipnum *out) {
    size_t n = strlen(text);
    if (n >= 2 && text[0] == '[' && text[n - 1] == ']') {
        char *inner = xstrndup(text + 1, n - 2);
        if (!inner) return false;
        struct in6_addr a6;
        bool ok = inet_pton(AF_INET6, inner, &a6) == 1;
        if (ok && out) {
            out->v6 = true;
            out->v6w = 0;
            for (int i = 0; i < 16; i++) out->v6w = (out->v6w << 8) | a6.s6_addr[i];
        }
        free(inner);
        return ok;
    }
    if (strchr(text, ':')) {
        struct in6_addr a6;
        if (inet_pton(AF_INET6, text, &a6) != 1) return false;
        if (out) {
            out->v6 = true;
            out->v6w = 0;
            for (int i = 0; i < 16; i++) out->v6w = (out->v6w << 8) | a6.s6_addr[i];
        }
        return true;
    }
    /* Four decimal octets, "01" rejected. */
    const char *parts[4];
    int np = 0;
    const char *p = text;
    for (;;) {
        const char *dot = strchr(p, '.');
        if (np >= 4) return false;
        parts[np++] = p;
        if (!dot) break;
        p = dot + 1;
    }
    if (np != 4) return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        const char *e = (i < 3) ? strchr(parts[i], '.') : parts[i] + strlen(parts[i]);
        size_t len = (size_t)(e - parts[i]);
        if (len == 0 || len > 3) return false;
        for (size_t k = 0; k < len; k++)
            if (!isdigit((unsigned char)parts[i][k])) return false;
        if (len > 1 && parts[i][0] == '0') return false;
        unsigned o = 0;
        for (size_t k = 0; k < len; k++) o = o * 10 + (unsigned)(parts[i][k] - '0');
        if (o > 255) return false;
        v = (v << 8) | o;
    }
    if (out) {
        out->v6 = false;
        out->v4 = v;
    }
    return true;
}

/* glibc __inet_aton_exact: 1-4 parts decimal/octal/hex, last fills the rest. */
static bool inet_aton_c(const char *text, uint32_t *out) {
    const char *parts[4];
    int np = 0;
    const char *p = text;
    for (;;) {
        if (np >= 4) return false;
        parts[np++] = p;
        const char *dot = strchr(p, '.');
        if (!dot) break;
        p = dot + 1;
    }
    if (np == 0) return false;
    uint32_t vals[4];
    for (int i = 0; i < np; i++) {
        const char *e = (i < 3) ? strchr(parts[i], '.') : parts[i] + strlen(parts[i]);
        size_t len = (size_t)(e - parts[i]);
        if (len == 0) return false;
        unsigned radix = 10;
        const char *digits = parts[i];
        size_t dlen = len;
        if (len > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
            radix = 16;
            digits += 2;
            dlen -= 2;
        } else if (len > 1 && digits[0] == '0') {
            radix = 8;
            digits += 1;
            dlen -= 1;
        }
        if (radix != 10) {
            for (size_t k = 0; k < dlen; k++) {
                unsigned char c = (unsigned char)digits[k];
                if (radix == 16 ? !isxdigit(c) : (c < '0' || c > '7')) return false;
            }
        } else {
            for (size_t k = 0; k < dlen; k++)
                if (!isdigit((unsigned char)digits[k])) return false;
        }
        char *tmp = xstrndup(digits, dlen);
        if (!tmp) return false;
        unsigned long v = strtoul(tmp, NULL, (int)radix);
        free(tmp);
        if (v > 0xffffffffUL) return false;
        vals[i] = (uint32_t)v;
    }
    for (int i = 0; i < np - 1; i++)
        if (vals[i] > 0xff) return false;
    unsigned remaining = 32 - 8 * (unsigned)(np - 1);
    if (remaining < 32 && vals[np - 1] >= (1u << remaining)) return false;
    uint32_t addr = vals[np - 1];
    for (int i = 0; i < np - 1; i++) addr |= vals[i] << (24 - 8 * i);
    *out = addr;
    return true;
}

static bool valid_label(const char *s, size_t n) {
    if (n == 0 || n > 63) return false;
    if (!isalnum((unsigned char)s[0]) || !isalnum((unsigned char)s[n - 1])) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!isalnum(c) && c != '-') return false;
    }
    return true;
}

static bool valid_host_syntax(const char *host) {
    if (strchr(host, ':') || legacy_ipv4_shape(host)) return true;
    ipnum dummy;
    if (ip_literal_host(host, &dummy)) return true;
    const char *h = host;
    size_t n = strlen(h);
    if (n > 0 && h[n - 1] == '.') n--; /* strip one trailing dot */
    if (n == 0) return false;
    for (;;) {
        const char *dot = memchr(h, '.', n);
        size_t len = dot ? (size_t)(dot - h) : n;
        if (!valid_label(h, len)) return false;
        if (!dot) break;
        h = dot + 1;
        n -= len + 1;
    }
    return true;
}

static bool numeric_host_candidate(const char *host) {
    return strchr(host, ':') != NULL || legacy_ipv4_shape(host);
}

static bool malformed_numeric_candidate(const char *host) {
    if (strchr(host, ':')) return false;
    const char *core = host;
    while (*core == '%' || *core == '/') core++;
    size_t clen = strcspn(core, "%/");
    char *cs = xstrndup(core, clen);
    if (!cs) return false;
    bool empty_part = false;
    {
        const char *p = cs;
        for (;;) {
            const char *dot = strchr(p, '.');
            size_t len = dot ? (size_t)(dot - p) : strlen(p);
            if (len == 0) {
                empty_part = true;
                break;
            }
            if (!dot) break;
            p = dot + 1;
        }
    }
    bool malformed = (core != host || clen != strlen(host) || empty_part);
    bool r = malformed && legacy_ipv4_shape(cs);
    if (r) {
        ipnum dummy;
        r = !ip_literal_host(host, &dummy);
    }
    free(cs);
    return r;
}

/* --- guard: resolution ----------------------------------------------------------
 * Returns 0 ok + ip string, 1 violation (blocked/malformed), 2 unresolvable. */

static void free_ips(char **ips, size_t n);

static int test_map_lookup(const cf_unfurl_config *cfg, const char *host, char **ips, size_t *n) {
    *n = 0;
    for (size_t i = 0; i < cfg->ntest; i++) {
        if (!strcmp(cfg->test_hosts[i], host)) {
            /* Comma-separated answer list (single answer in our tests). */
            const char *a = cfg->test_addrs[i];
            for (;;) {
                const char *comma = strchr(a, ',');
                size_t len = comma ? (size_t)(comma - a) : strlen(a);
                if (*n >= MAX_ADDRS + 1) break;
                ips[*n] = xstrndup(a, len);
                if (!ips[*n]) {
                    free_ips(ips, *n);
                    return -1;
                }
                (*n)++;
                if (!comma) break;
                a = comma + 1;
            }
            return 0;
        }
    }
    return 2; /* not in map: unresolvable, no real-DNS fallback in tests */
}

static int sys_lookup(const char *host, char **ips, size_t *n) {
    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(host, NULL, &hints, &res) != 0) return 2;
    *n = 0;
    int rc = 0;
    for (ai = res; ai; ai = ai->ai_next) {
        if (*n >= MAX_ADDRS + 1) break;
        char buf[INET6_ADDRSTRLEN];
        if (ai->ai_family == AF_INET) {
            struct sockaddr_in *s = (struct sockaddr_in *)ai->ai_addr;
            if (!inet_ntop(AF_INET, &s->sin_addr, buf, sizeof buf)) continue;
        } else if (ai->ai_family == AF_INET6) {
            struct sockaddr_in6 *s = (struct sockaddr_in6 *)ai->ai_addr;
            if (!inet_ntop(AF_INET6, &s->sin6_addr, buf, sizeof buf)) continue;
        } else {
            continue;
        }
        ips[*n] = strdup(buf);
        if (!ips[*n]) {
            free_ips(ips, *n);
            rc = -1;
            break;
        }
        (*n)++;
    }
    freeaddrinfo(res);
    if (rc == 0 && *n == 0) return 2;
    return rc;
}

static void free_ips(char **ips, size_t n) {
    for (size_t i = 0; i < n; i++) free(ips[i]);
}

/* resolve_public_ips: 0 ok (possibly empty), 2 unresolvable, -1 nomem. */
static int resolve_public_ips(const cf_unfurl_config *cfg, const char *host, char **out, size_t *nout) {
    *nout = 0;
    if (!normal_host(host)) return 0;
    /* Strip brackets for the numeric checks (reference resolves "[v6]"). */
    const char *bare = host;
    char *unbr = NULL;
    size_t hn = strlen(host);
    if (hn >= 2 && host[0] == '[' && host[hn - 1] == ']') {
        unbr = xstrndup(host + 1, hn - 2);
        if (!unbr) return -1;
        bare = unbr;
    }
    int rc;
    char *ips[MAX_ADDRS + 1];
    size_t n = 0;
    if (!valid_host_syntax(bare) || malformed_numeric_candidate(bare)) {
        rc = 0; /* Invalid: no addresses. */
    } else {
        uint32_t a4;
        ipnum lit;
        if (inet_aton_c(bare, &a4)) {
            /* getaddrinfo(AI_NUMERICHOST) equivalent: success implies a
             * numeric form (names never parse as addresses). */
            struct in_addr a;
            a.s_addr = htonl(a4);
            char buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &a, buf, sizeof buf);
            ips[0] = strdup(buf);
            if (!ips[0]) {
                free(unbr);
                return -1;
            }
            n = 1;
            rc = 0;
        } else if (ip_literal_host(bare, &lit)) {
            char buf[INET6_ADDRSTRLEN];
            if (!lit.v6) {
                struct in_addr a;
                a.s_addr = htonl(lit.v4);
                inet_ntop(AF_INET, &a, buf, sizeof buf);
            } else {
                struct in6_addr a6;
                unsigned __int128 v = lit.v6w;
                for (int i = 15; i >= 0; i--) {
                    a6.s6_addr[i] = (uint8_t)v;
                    v >>= 8;
                }
                inet_ntop(AF_INET6, &a6, buf, sizeof buf);
            }
            ips[0] = strdup(buf);
            if (!ips[0]) {
                free(unbr);
                return -1;
            }
            n = 1;
            rc = 0;
        } else if (numeric_host_candidate(bare)) {
            rc = 0; /* looks numeric but parses as nothing: no addresses */
        } else if (cfg->ntest > 0) {
            rc = test_map_lookup(cfg, bare, ips, &n);
        } else {
            rc = sys_lookup(bare, ips, &n);
        }
    }
    free(unbr);
    if (rc != 0) return rc;
    if (n > MAX_ADDRS) {
        free_ips(ips, n);
        return 2;
    }
    /* Dedupe. */
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        bool seen = false;
        for (size_t j = 0; j < m; j++)
            if (!strcmp(ips[j], ips[i])) {
                seen = true;
                break;
            }
        if (seen) {
            free(ips[i]);
        } else {
            ips[m++] = ips[i];
        }
    }
    n = m;
    if (n == 0) return 0;
    /* Partition v4-first, dropping blocked (unless the test override). */
    size_t w = 0;
    for (size_t pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < n; i++) {
            if (!ips[i]) continue;
            bool v4 = !strchr(ips[i], ':');
            if ((pass == 0) != !v4) continue;
            if (!cfg->test_allow_private && cf_unfurl_blocked_ip(ips[i])) {
                free(ips[i]);
                ips[i] = NULL;
                continue;
            }
            out[w++] = ips[i];
            ips[i] = NULL;
        }
    }
    for (size_t i = 0; i < n; i++) free(ips[i]);
    *nout = w;
    return 0;
}

/* --- media-URL pre-filter (FILES_AND_MEDIA_URL_REGEX) --------------------------
 * \bhttps?://\S+\.(ext)\b with the reference extension list (case-sensitive).
 */
static const char *const kMediaExt[] = {
    "zip", "tar", "tar.gz", "tar.bz2", "tar.xz", "gz", "bz2", "rar", "7z",
    "dmg", "exe", "msi", "pkg", "deb", "iso", "jpg", "jpeg", "png", "gif",
    "bmp", "mp4", "mov", "avi", "mkv", "wmv", "flv", "heic", "heif", "mp3",
    "wav", "ogg", "aac", "wma", "webm", "ogv", "mpg", "mpeg",
};

static bool is_word_c(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

bool cf_unfurl_is_media_url(const char *url) {
    if (!url) return false;
    size_t n = strlen(url);
    for (size_t i = 0; i < n; i++) {
        size_t sl = 0;
        if (!strncmp(url + i, "http://", 7)) sl = 7;
        else if (!strncmp(url + i, "https://", 8)) sl = 8;
        else continue;
        if (i > 0 && is_word_c(url[i - 1])) continue; /* \b */
        size_t e = i + sl;
        while (e < n && url[e] != ' ' && url[e] != '\t' && url[e] != '\n' && url[e] != '\v' &&
               url[e] != '\f' && url[e] != '\r')
            e++;
        for (size_t k = 0; k < sizeof kMediaExt / sizeof kMediaExt[0]; k++) {
            const char *ext = kMediaExt[k];
            size_t el = strlen(ext);
            /* Find "." + ext inside [i, e). */
            for (size_t j = i + sl; j + 1 + el <= e; j++) {
                if (url[j] != '.') continue;
                if (strncmp(url + j + 1, ext, el)) continue;
                size_t after = j + 1 + el;
                if (after < e && is_word_c(url[after])) continue; /* \b */
                return true;
            }
        }
    }
    return false;
}
/* src/integrations/unfurl.c — part 2: HTML scanner (html.rs) + document. */

/* libxml2 html40EntitiesTable + apos (case-sensitive, need ';'). Sorted for
 * binary search. Generated from the pinned entities.rs (do not hand-edit). */
typedef struct {
    const char *name;
    uint32_t cp;
} html_entity;

static const html_entity kEnts[] = {
    {"AElig", 198},
    {"Aacute", 193},
    {"Acirc", 194},
    {"Agrave", 192},
    {"Alpha", 913},
    {"Aring", 197},
    {"Atilde", 195},
    {"Auml", 196},
    {"Beta", 914},
    {"Ccedil", 199},
    {"Chi", 935},
    {"Dagger", 8225},
    {"Delta", 916},
    {"ETH", 208},
    {"Eacute", 201},
    {"Ecirc", 202},
    {"Egrave", 200},
    {"Epsilon", 917},
    {"Eta", 919},
    {"Euml", 203},
    {"Gamma", 915},
    {"Iacute", 205},
    {"Icirc", 206},
    {"Igrave", 204},
    {"Iota", 921},
    {"Iuml", 207},
    {"Kappa", 922},
    {"Lambda", 923},
    {"Mu", 924},
    {"Ntilde", 209},
    {"Nu", 925},
    {"OElig", 338},
    {"Oacute", 211},
    {"Ocirc", 212},
    {"Ograve", 210},
    {"Omega", 937},
    {"Omicron", 927},
    {"Oslash", 216},
    {"Otilde", 213},
    {"Ouml", 214},
    {"Phi", 934},
    {"Pi", 928},
    {"Prime", 8243},
    {"Psi", 936},
    {"Rho", 929},
    {"Scaron", 352},
    {"Sigma", 931},
    {"THORN", 222},
    {"Tau", 932},
    {"Theta", 920},
    {"Uacute", 218},
    {"Ucirc", 219},
    {"Ugrave", 217},
    {"Upsilon", 933},
    {"Uuml", 220},
    {"Xi", 926},
    {"Yacute", 221},
    {"Yuml", 376},
    {"Zeta", 918},
    {"aacute", 225},
    {"acirc", 226},
    {"acute", 180},
    {"aelig", 230},
    {"agrave", 224},
    {"alefsym", 8501},
    {"alpha", 945},
    {"amp", 38},
    {"and", 8743},
    {"ang", 8736},
    {"apos", 39},
    {"aring", 229},
    {"asymp", 8776},
    {"atilde", 227},
    {"auml", 228},
    {"bdquo", 8222},
    {"beta", 946},
    {"brvbar", 166},
    {"bull", 8226},
    {"cap", 8745},
    {"ccedil", 231},
    {"cedil", 184},
    {"cent", 162},
    {"chi", 967},
    {"circ", 710},
    {"clubs", 9827},
    {"cong", 8773},
    {"copy", 169},
    {"crarr", 8629},
    {"cup", 8746},
    {"curren", 164},
    {"dArr", 8659},
    {"dagger", 8224},
    {"darr", 8595},
    {"deg", 176},
    {"delta", 948},
    {"diams", 9830},
    {"divide", 247},
    {"eacute", 233},
    {"ecirc", 234},
    {"egrave", 232},
    {"empty", 8709},
    {"emsp", 8195},
    {"ensp", 8194},
    {"epsilon", 949},
    {"equiv", 8801},
    {"eta", 951},
    {"eth", 240},
    {"euml", 235},
    {"euro", 8364},
    {"exist", 8707},
    {"fnof", 402},
    {"forall", 8704},
    {"frac12", 189},
    {"frac14", 188},
    {"frac34", 190},
    {"frasl", 8260},
    {"gamma", 947},
    {"ge", 8805},
    {"gt", 62},
    {"hArr", 8660},
    {"harr", 8596},
    {"hearts", 9829},
    {"hellip", 8230},
    {"iacute", 237},
    {"icirc", 238},
    {"iexcl", 161},
    {"igrave", 236},
    {"image", 8465},
    {"infin", 8734},
    {"int", 8747},
    {"iota", 953},
    {"iquest", 191},
    {"isin", 8712},
    {"iuml", 239},
    {"kappa", 954},
    {"lArr", 8656},
    {"lambda", 955},
    {"lang", 9001},
    {"laquo", 171},
    {"larr", 8592},
    {"lceil", 8968},
    {"ldquo", 8220},
    {"le", 8804},
    {"lfloor", 8970},
    {"lowast", 8727},
    {"loz", 9674},
    {"lrm", 8206},
    {"lsaquo", 8249},
    {"lsquo", 8216},
    {"lt", 60},
    {"macr", 175},
    {"mdash", 8212},
    {"micro", 181},
    {"middot", 183},
    {"minus", 8722},
    {"mu", 956},
    {"nabla", 8711},
    {"nbsp", 160},
    {"ndash", 8211},
    {"ne", 8800},
    {"ni", 8715},
    {"not", 172},
    {"notin", 8713},
    {"nsub", 8836},
    {"ntilde", 241},
    {"nu", 957},
    {"oacute", 243},
    {"ocirc", 244},
    {"oelig", 339},
    {"ograve", 242},
    {"oline", 8254},
    {"omega", 969},
    {"omicron", 959},
    {"oplus", 8853},
    {"or", 8744},
    {"ordf", 170},
    {"ordm", 186},
    {"oslash", 248},
    {"otilde", 245},
    {"otimes", 8855},
    {"ouml", 246},
    {"para", 182},
    {"part", 8706},
    {"permil", 8240},
    {"perp", 8869},
    {"phi", 966},
    {"pi", 960},
    {"piv", 982},
    {"plusmn", 177},
    {"pound", 163},
    {"prime", 8242},
    {"prod", 8719},
    {"prop", 8733},
    {"psi", 968},
    {"quot", 34},
    {"rArr", 8658},
    {"radic", 8730},
    {"rang", 9002},
    {"raquo", 187},
    {"rarr", 8594},
    {"rceil", 8969},
    {"rdquo", 8221},
    {"real", 8476},
    {"reg", 174},
    {"rfloor", 8971},
    {"rho", 961},
    {"rlm", 8207},
    {"rsaquo", 8250},
    {"rsquo", 8217},
    {"sbquo", 8218},
    {"scaron", 353},
    {"sdot", 8901},
    {"sect", 167},
    {"shy", 173},
    {"sigma", 963},
    {"sigmaf", 962},
    {"sim", 8764},
    {"spades", 9824},
    {"sub", 8834},
    {"sube", 8838},
    {"sum", 8721},
    {"sup", 8835},
    {"sup1", 185},
    {"sup2", 178},
    {"sup3", 179},
    {"supe", 8839},
    {"szlig", 223},
    {"tau", 964},
    {"there4", 8756},
    {"theta", 952},
    {"thetasym", 977},
    {"thinsp", 8201},
    {"thorn", 254},
    {"tilde", 732},
    {"times", 215},
    {"trade", 8482},
    {"uArr", 8657},
    {"uacute", 250},
    {"uarr", 8593},
    {"ucirc", 251},
    {"ugrave", 249},
    {"uml", 168},
    {"upsih", 978},
    {"upsilon", 965},
    {"uuml", 252},
    {"weierp", 8472},
    {"xi", 958},
    {"yacute", 253},
    {"yen", 165},
    {"yuml", 255},
    {"zeta", 950},
    {"zwj", 8205},
    {"zwnj", 8204},

};

static int ent_cmp(const void *k, const void *e) {
    return strcmp((const char *)k, ((const html_entity *)e)->name);
}

static const html_entity *ent_lookup(const char *name, size_t len) {
    char tmp[32];
    if (len >= sizeof tmp) return NULL;
    memcpy(tmp, name, len);
    tmp[len] = '\0';
    return bsearch(tmp, kEnts, sizeof kEnts / sizeof kEnts[0], sizeof kEnts[0], ent_cmp);
}

/* libxml2 reading a UTF-8 buffer: valid sequences decode, any other byte is
 * taken as Latin-1. Output is UTF-8. */
static char *html_decode(const unsigned char *bytes, size_t n) {
    char *o = malloc(n * 3 + 1);
    if (!o) return NULL;
    size_t w = 0, i = 0;
    while (i < n) {
        unsigned char c = bytes[i];
        if (c < 0x80) {
            o[w++] = (char)c;
            i++;
            continue;
        }
        size_t want = 0;
        uint32_t cp = 0;
        if ((c & 0xe0) == 0xc0) {
            want = 2;
            cp = c & 0x1f;
        } else if ((c & 0xf0) == 0xe0) {
            want = 3;
            cp = c & 0x0f;
        } else if ((c & 0xf8) == 0xf0) {
            want = 4;
            cp = c & 0x07;
        } else {
            /* lone continuation/invalid: Latin-1. */
            o[w++] = (char)(0xc0 | (c >> 6));
            o[w++] = (char)(0x80 | (c & 0x3f));
            i++;
            continue;
        }
        bool ok = i + want <= n;
        for (size_t k = 1; ok && k < want; k++)
            if ((bytes[i + k] & 0xc0) != 0x80) ok = false;
        if (ok) {
            for (size_t k = 1; k < want; k++) cp = (cp << 6) | (bytes[i + k] & 0x3f);
            /* Reject overlong/surrogate/out-of-range as invalid. */
            if ((want == 2 && cp < 0x80) || (want == 3 && cp < 0x800) ||
                (want == 4 && (cp < 0x10000 || cp > 0x10ffff)) || (cp >= 0xd800 && cp <= 0xdfff))
                ok = false;
        }
        if (ok) {
            memcpy(o + w, bytes + i, want);
            w += want;
            i += want;
        } else {
            o[w++] = (char)(0xc0 | (c >> 6));
            o[w++] = (char)(0x80 | (c & 0x3f));
            i++;
        }
    }
    o[w] = '\0';
    return o;
}

#define MAX_ATTRS 256

typedef struct {
    char (*names)[64];
    char **values;
    size_t n;
} meta_el;

typedef struct {
    meta_el *els;
    size_t n, cap;
} meta_vec;

static void meta_vec_dispose(meta_vec *v) {
    for (size_t i = 0; i < v->n; i++) {
        free(v->els[i].names);
        for (size_t k = 0; k < v->els[i].n; k++) free(v->els[i].values[k]);
        free(v->els[i].values);
    }
    free(v->els);
    memset(v, 0, sizeof *v);
}

static const char *meta_attr(const meta_el *e, const char *name) {
    for (size_t i = 0; i < e->n; i++)
        if (!strcmp(e->names[i], name)) return e->values[i];
    return NULL;
}

static bool meta_has(const meta_el *e, const char *name) {
    return meta_attr(e, name) != NULL;
}

typedef struct {
    const unsigned char *b;
    size_t n, pos;
    meta_vec *out;
    bool oom;
} scanner;

static int sc_peek(scanner *s, size_t ahead) {
    size_t p = s->pos + ahead;
    return p < s->n ? s->b[p] : -1;
}

static bool sc_starts_with_ci(scanner *s, const char *t) {
    size_t m = strlen(t);
    if (s->pos + m > s->n) return false;
    for (size_t i = 0; i < m; i++)
        if (tolower(s->b[s->pos + i]) != tolower((unsigned char)t[i])) return false;
    return true;
}

static bool sc_blank_c(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* htmlParseHTMLName: [A-Za-z_:.][A-Za-z0-9:_.-]*, lowercased. -1 when absent. */
static int sc_html_name(scanner *s, char *dst, size_t cap) {
    int first = sc_peek(s, 0);
    if (first < 0 || !(isalpha(first) || first == '_' || first == ':' || first == '.')) return -1;
    size_t w = 0;
    for (;;) {
        int c = sc_peek(s, 0);
        if (c < 0 || !(isalnum(c) || c == ':' || c == '-' || c == '_' || c == '.')) break;
        if (w + 1 < cap) dst[w++] = (char)tolower(c);
        s->pos++;
    }
    dst[w] = '\0';
    return 0;
}

static void sc_skip_blanks(scanner *s) {
    while (sc_blank_c(sc_peek(s, 0))) s->pos++;
}

static void sc_skip_past(scanner *s, unsigned char c) {
    for (;;) {
        int next = sc_peek(s, 0);
        if (next < 0) break;
        s->pos++;
        if (next == c) break;
    }
}

static void sc_end_tag(scanner *s) {
    char tmp[16];
    s->pos += 2;
    if (sc_html_name(s, tmp, sizeof tmp) == 0) sc_skip_past(s, '>');
}

static void sc_markup_decl(scanner *s) {
    if (sc_peek(s, 2) == '-' && sc_peek(s, 3) == '-') {
        s->pos += 4;
        if (sc_peek(s, 0) == '>') {
            s->pos++;
            return;
        }
        if (sc_peek(s, 0) == '-' && sc_peek(s, 1) == '>') {
            s->pos += 2;
            return;
        }
        while (s->pos < s->n) {
            if (sc_starts_with_ci(s, "-->")) {
                s->pos += 3;
                return;
            }
            if (sc_starts_with_ci(s, "--!>")) {
                s->pos += 4;
                return;
            }
            s->pos++;
        }
    } else {
        sc_skip_past(s, '>');
    }
}

/* Encode one code point as UTF-8 into buf (returns bytes). */
static size_t utf8_enc(uint32_t cp, char *o) {
    if (cp < 0x80) {
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (char)(0xc0 | (cp >> 6));
        o[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (char)(0xe0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        o[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    o[0] = (char)(0xf0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    o[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

typedef struct {
    char *data;
    size_t len, cap;
    bool oom;
} sbuf;

static void sb_putn(sbuf *b, const char *p, size_t n) {
    if (b->oom || n == 0) return;
    if (b->len + n < b->len || b->len + n + 1 < b->len) {
        b->oom = true;
        return;
    }
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->len + n + 1) {
            if (cap > (size_t)1 << 30) {
                cap = b->len + n + 1;
                break;
            }
            cap *= 2;
        }
        char *nb = realloc(b->data, cap);
        if (!nb) {
            b->oom = true;
            return;
        }
        b->data = nb;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
}

/* htmlParseCharRef: valid XML char or -1 (invalid). Advances past digits. */
static int32_t sc_char_ref(scanner *s) {
    bool hex = sc_peek(s, 2) == 'x' || sc_peek(s, 2) == 'X';
    s->pos += hex ? 3 : 2;
    unsigned radix = hex ? 16 : 10;
    uint32_t value = 0;
    for (;;) {
        int c = sc_peek(s, 0);
        if (c == ';') {
            s->pos++;
            break;
        }
        unsigned digit;
        if (c >= '0' && c <= '9') digit = (unsigned)(c - '0');
        else if (hex && c >= 'a' && c <= 'f') digit = (unsigned)(c - 'a' + 10);
        else if (hex && c >= 'A' && c <= 'F') digit = (unsigned)(c - 'A' + 10);
        else break;
        if (digit >= radix) break;
        if (value < 0x110000) value = value * radix + digit;
        s->pos++;
    }
    bool is_char = value == 0x9 || value == 0xa || value == 0xd || (value >= 0x20 && value <= 0xd7ff) ||
                   (value >= 0xe000 && value <= 0xfffd) || (value >= 0x10000 && value <= 0x10ffff);
    return is_char ? (int32_t)value : -1;
}

/* htmlParseEntityRef into sb (decoded char or literal "&name"). */
static void sc_entity_ref(scanner *s, sbuf *sb) {
    s->pos++; /* '&' */
    size_t start = s->pos;
    int c0 = sc_peek(s, 0);
    if (c0 >= 0 && (isalpha(c0) || c0 == '_' || c0 == ':')) {
        for (;;) {
            int c = sc_peek(s, 0);
            if (c >= 0 && (isalnum(c) || c == '_' || c == ':' || c == '.' || c == '-')) s->pos++;
            else break;
        }
    }
    size_t nlen = s->pos - start;
    const html_entity *e = ent_lookup((const char *)s->b + start, nlen);
    if (e && nlen > 0 && sc_peek(s, 0) == ';') {
        s->pos++;
        char tmp[4];
        sb_putn(sb, tmp, utf8_enc(e->cp, tmp));
    } else {
        sb_putn(sb, "&", 1);
        sb_putn(sb, (const char *)s->b + start, nlen);
    }
}

/* Attribute text up to stop quote (or blank/> when unquoted). */
static void sc_attr_text(scanner *s, int stop, sbuf *sb) {
    bool truncated = false;
    sbuf disc;
    memset(&disc, 0, sizeof disc);
    for (;;) {
        size_t start = s->pos;
        for (;;) {
            int c = sc_peek(s, 0);
            if (c < 0 || c == '&' || c == stop) break;
            if (stop < 0 && (c == '>' || sc_blank_c(c))) break;
            s->pos++;
        }
        if (!truncated) sb_putn(sb, (const char *)s->b + start, s->pos - start);
        if (sc_peek(s, 0) != '&') break;
        if (sc_peek(s, 1) == '#') {
            int32_t cp = sc_char_ref(s);
            if (cp >= 0) {
                if (!truncated) {
                    char tmp[4];
                    sb_putn(sb, tmp, utf8_enc((uint32_t)cp, tmp));
                }
            } else {
                truncated = true;
            }
        } else {
            /* entity_ref always advances; sink output when truncated. */
            sc_entity_ref(s, truncated ? &disc : sb);
            if (disc.len > 4096) {
                free(disc.data);
                memset(&disc, 0, sizeof disc);
            }
        }
        if (sb->oom) break;
    }
    free(disc.data);
}

static const char *sb_cstr(sbuf *b) {
    return b->data ? b->data : "";
}

/* htmlParseAttValue. */
static void sc_attr_value(scanner *s, sbuf *sb) {
    int q = sc_peek(s, 0);
    if (q == '"' || q == '\'') {
        s->pos++;
        sc_attr_text(s, q, sb);
        if (sc_peek(s, 0) == q) s->pos++;
    } else {
        sc_attr_text(s, -1, sb);
    }
}

/* htmlParseStartTag: name + element + self-closing flag. */
static void sc_start_tag(scanner *s, char *name, size_t namecap, meta_el *el, bool *self_closing) {
    s->pos++;
    if (sc_html_name(s, name, namecap) != 0) name[0] = '\0';
    el->names = NULL;
    el->values = NULL;
    el->n = 0;
    sc_skip_blanks(s);
    for (;;) {
        int c = sc_peek(s, 0);
        if (c < 0 || c == '>') break;
        if (c == '/' && sc_peek(s, 1) == '>') break;
        char aname[128];
        if (sc_html_name(s, aname, sizeof aname) == 0) {
            sc_skip_blanks(s);
            sbuf v;
            memset(&v, 0, sizeof v);
            if (sc_peek(s, 0) == '=') {
                s->pos++;
                sc_skip_blanks(s);
                sc_attr_value(s, &v);
            }
            if (v.oom) s->oom = true;
            bool dup = false;
            for (size_t i = 0; i < el->n; i++)
                if (!strcmp(el->names[i], aname)) {
                    dup = true;
                    break;
                }
            if (!dup && el->n < MAX_ATTRS) {
                char (*nn)[64] = realloc(el->names, (el->n + 1) * sizeof *nn);
                char **vv = realloc(el->values, (el->n + 1) * sizeof *vv);
                if (!nn || !vv) {
                    /* A failed realloc leaves the original block alive; free
                     * only a replacement that differs from it. */
                    if (nn && nn != el->names) free(nn);
                    if (vv && vv != el->values) free(vv);
                    free(v.data);
                    s->oom = true;
                    break;
                }
                el->names = nn;
                el->values = vv;
                snprintf(el->names[el->n], 64, "%s", aname);
                el->values[el->n] = strdup(sb_cstr(&v));
                if (!el->values[el->n]) s->oom = true;
                else el->n++;
            }
            free(v.data);
        } else {
            /* Bogus attribute: dump to next blank or tag end. */
            for (;;) {
                int d = sc_peek(s, 0);
                if (d < 0 || sc_blank_c(d) || d == '>') break;
                if (d == '/' && sc_peek(s, 1) == '>') break;
                s->pos++;
            }
        }
        if (s->oom) break;
        sc_skip_blanks(s);
    }
    *self_closing = sc_peek(s, 0) == '/';
    if (*self_closing) {
        s->pos += 2;
    } else if (sc_peek(s, 0) == '>') {
        s->pos++;
    }
}

/* htmlParseScript: everything up to </name (any case). */
static void sc_raw_text(scanner *s, const char *name) {
    char end[16];
    snprintf(end, sizeof end, "</%s", name);
    while (s->pos < s->n && !sc_starts_with_ci(s, end)) s->pos++;
}

/* The <meta> elements of the document, in document order. */
static void meta_elements(const char *html, meta_vec *v) {
    memset(v, 0, sizeof *v);
    /* A NUL ends libxml2's input; decoded bodies can carry one (a raw 0x00
     * byte decodes to U+0000), so strlen already truncates like it. */
    size_t n = strlen(html);
    scanner s;
    memset(&s, 0, sizeof s);
    s.b = (const unsigned char *)html;
    s.n = n;
    s.out = v;
    while (s.pos < s.n) {
        if (s.b[s.pos] != '<') {
            s.pos++;
            continue;
        }
        int c1 = sc_peek(&s, 1);
        if (c1 == '/') {
            sc_end_tag(&s);
        } else if (c1 == '!') {
            s.pos++;
            sc_markup_decl(&s);
        } else if (c1 == '?') {
            s.pos++;
            sc_skip_past(&s, '>');
        } else if (c1 >= 0 && isalpha(c1)) {
            char name[64];
            meta_el el;
            bool sc2;
            sc_start_tag(&s, name, sizeof name, &el, &sc2);
            if (s.oom) {
                free(el.names);
                /* values freed below via count; el.n may be partial — the
                 * failing append was not counted, so this is safe. */
                for (size_t i = 0; i < el.n; i++) free(el.values[i]);
                free(el.values);
                return;
            }
            if (!strcmp(name, "meta")) {
                if (v->n == v->cap) {
                    size_t cap = v->cap ? v->cap * 2 : 16;
                    meta_el *ne = realloc(v->els, cap * sizeof *ne);
                    if (!ne) {
                        for (size_t i = 0; i < el.n; i++) free(el.values[i]);
                        free(el.values);
                        free(el.names);
                        return;
                    }
                    v->els = ne;
                    v->cap = cap;
                }
                v->els[v->n++] = el;
            } else {
                for (size_t i = 0; i < el.n; i++) free(el.values[i]);
                free(el.values);
                free(el.names);
                if ((!strcmp(name, "script") || !strcmp(name, "style")) && !sc2)
                    sc_raw_text(&s, name);
            }
        } else {
            s.pos++;
        }
    }
}

/* content[/charset\s*=\s*([\w-]+)/i, 1] */
static char *charset_in(const char *content) {
    static const char *ws = " \t\n\x0b\x0c\r";
    for (const char *p = content; *p; p++) {
        size_t k = 0;
        while (k < 7 && tolower((unsigned char)p[k]) == "charset"[k]) k++;
        if (k < 7) continue;
        const char *q = p + 7;
        while (*q && strchr(ws, *q)) q++;
        if (*q != '=') continue;
        q++;
        while (*q && strchr(ws, *q)) q++;
        size_t m = 0;
        while (q[m] && (isalnum((unsigned char)q[m]) || q[m] == '_' || q[m] == '-')) m++;
        if (m == 0) continue;
        return xstrndup(q, m);
    }
    return NULL;
}

/* Nokogiri meta_encoding: first meta[charset], else charset of the first
 * http-equiv=Content-Type meta with content. has = whether any encoding
 * was declared (empty charset counts as declared). */
static char *meta_encoding_of(const meta_vec *v, bool *has) {
    *has = false;
    for (size_t i = 0; i < v->n; i++) {
        if (meta_has(&v->els[i], "charset")) {
            *has = true;
            return strdup(meta_attr(&v->els[i], "charset"));
        }
    }
    for (size_t i = 0; i < v->n; i++) {
        const char *he = meta_attr(&v->els[i], "http-equiv");
        const char *c = meta_attr(&v->els[i], "content");
        if (c && he && !strcasecmp(he, "content-type")) {
            char *cs = charset_in(c);
            if (cs) {
                *has = true;
                return cs;
            }
            return NULL; /* first match without charset: none */
        }
    }
    return NULL;
}

/* String#blank?: empty or only Unicode whitespace (via R02 helper). */
static bool str_blank(const char *s) {
    return rt_is_blank((const unsigned char *)s, strlen(s));
}

static bool is_og_tag(const meta_el *m) {
    const char *p = meta_attr(m, "property");
    const char *nm = meta_attr(m, "name");
    return (p && !strncmp(p, "og:", 3)) || (nm && !strncmp(nm, "og:", 3));
}

/* Remove every "og:" from s (replaces document.rs `replace("og:", "")`). */
static char *strip_og(const char *s) {
    sbuf b;
    memset(&b, 0, sizeof b);
    for (const char *p = s; *p;) {
        if (!strncmp(p, "og:", 3)) p += 3;
        else {
            sb_putn(&b, p, 1);
            p++;
        }
    }
    char *o = b.data ? b.data : strdup("");
    return o;
}

static const char *kAttrs[] = {"title", "url", "image", "description"};

typedef struct {
    char *values[4]; /* NULL when absent */
} og_found;

/* opengraph_attributes: later tags win; non-ASCII dropped without a meta
 * charset; only ATTRIBUTES keys kept, in ATTRIBUTES order. */
static void opengraph_attributes(const unsigned char *body, size_t body_len, og_found *f,
                                 bool *url_tag, bool *img_tag) {
    memset(f, 0, sizeof *f);
    *url_tag = false;
    *img_tag = false;
    char *html = html_decode(body ? body : (const unsigned char *)"", body ? body_len : 0);
    if (!html) return;
    meta_vec v;
    meta_elements(html, &v);
    bool has_enc = false;
    char *enc = meta_encoding_of(&v, &has_enc);
    free(enc);
    for (size_t i = 0; i < v.n; i++) {
        const meta_el *m = &v.els[i];
        if (!is_og_tag(m)) continue;
        const char *key = meta_has(m, "property") ? "property" : "name";
        char *name = strip_og(meta_attr(m, key));
        int idx = -1;
        for (int k = 0; k < 4; k++)
            if (!strcmp(name, kAttrs[k])) idx = k;
        free(name);
        if (idx < 0) continue;
        const char *content = meta_attr(m, "content");
        if (!content || str_blank(content)) continue;
        char *val;
        if (has_enc) {
            val = strdup(content);
        } else {
            /* Drop non-ASCII characters. */
            sbuf b;
            memset(&b, 0, sizeof b);
            for (const unsigned char *p = (const unsigned char *)content; *p; p++)
                if (*p < 0x80) sb_putn(&b, (const char *)p, 1);
            val = b.data ? b.data : strdup("");
        }
        if (!val) continue;
        free(f->values[idx]);
        f->values[idx] = val;
        if (idx == 1) *url_tag = true;
        if (idx == 2) *img_tag = true;
    }
    meta_vec_dispose(&v);
    free(html);
}
/* src/integrations/unfurl.c — part 3: fetch, location, metadata, entry. */

void cf_unfurl_default_config(cf_unfurl_config *c) {
    memset(c, 0, sizeof *c);
    c->connect_timeout_ms = 5000;
    c->read_timeout_ms = 5000;
    c->deadline_ms = CF_UNFURL_DEADLINE_MS;
    c->ca_path = NULL;
}

void cf_unfurl_out_dispose(cf_unfurl_out *o) {
    if (!o) return;
    free(o->json);
    memset(o, 0, sizeof *o);
}

/* --- concurrency: at most 16 unfurls at once ------------------------------- */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_inflight = 0;

static bool sem_acquire(long deadline_ms) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    long end_ns = now.tv_sec * 1000000000L + now.tv_nsec + deadline_ms * 1000000L;
    pthread_mutex_lock(&g_mu);
    for (;;) {
        if (g_inflight < CF_UNFURL_MAX_CONCURRENT) {
            g_inflight++;
            pthread_mutex_unlock(&g_mu);
            return true;
        }
        struct timespec abs;
        abs.tv_sec = end_ns / 1000000000L;
        abs.tv_nsec = end_ns % 1000000000L;
        if (pthread_cond_timedwait(&g_cv, &g_mu, &abs) != 0) {
            pthread_mutex_unlock(&g_mu);
            return false;
        }
    }
}

static void sem_release(void) {
    pthread_mutex_lock(&g_mu);
    g_inflight--;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

/* --- location --------------------------------------------------------------- */

typedef struct {
    const cf_unfurl_config *cfg;
    cf_http_uri uri; /* parsed URL or empty when unparsable */
    bool parsed;
    bool resolved_run;
    bool resolved_ok;
    char resolved_ip[INET6_ADDRSTRLEN];
} location;

static void location_init(location *l, const cf_unfurl_config *cfg, const char *url) {
    memset(l, 0, sizeof *l);
    l->cfg = cfg;
    if (url && cf_http_uri_parse(url, &l->uri) == CF_HTTP_OK) l->parsed = true;
}

/* PrivateNetworkGuard.resolve(host): first public address (memoized). */
static bool location_resolve(location *l) {
    if (l->resolved_run) return l->resolved_ok;
    l->resolved_run = true;
    l->resolved_ok = false;
    if (!l->parsed || !l->uri.host) return false;
    char *ips[MAX_ADDRS + 1];
    size_t n = 0;
    int rc = resolve_public_ips(l->cfg, l->uri.host, ips, &n);
    if (rc != 0 || n == 0) return false; /* Unresolvable or Violation */
    snprintf(l->resolved_ip, sizeof l->resolved_ip, "%s", ips[0]);
    for (size_t i = 0; i < n; i++) free(ips[i]);
    l->resolved_ok = true;
    return true;
}

static bool location_valid(location *l) {
    bool http = l->parsed && cf_http_uri_is_http(&l->uri);
    bool pub = location_resolve(l);
    return http && pub;
}

/* Build a "HOST:PORT:IP" pin for curl from the memoized address. */
static char *location_pin(location *l, uint64_t port) {
    if (!l->parsed || !l->uri.host || !l->resolved_ok) return NULL;
    if (strchr(l->resolved_ip, ':')) return NULL; /* v6: system resolution */
    const char *h = l->uri.host;
    size_t hn = strlen(h);
    if (hn >= 2 && h[0] == '[' && h[hn - 1] == ']') return NULL;
    char *pin = malloc(hn + 32 + strlen(l->resolved_ip));
    if (!pin) return NULL;
    sprintf(pin, "%s:%llu:%s", h, (unsigned long long)port, l->resolved_ip);
    return pin;
}

static void location_dispose(location *l) {
    cf_http_uri_dispose(&l->uri);
}

/* --- fetch ------------------------------------------------------------------ */

typedef struct {
    unsigned status;
    cf_http_response res; /* owned when ok */
    bool ok;
} fetched;

static void fetched_dispose(fetched *f) {
    if (f->ok) cf_http_response_dispose(&f->res);
    memset(f, 0, sizeof *f);
}

/* Single pinned exchange with the remaining overall budget. */
static cf_http_err fetch_once(const cf_unfurl_config *cfg, const char *url, location *loc,
                              long budget_ms, fetched *f, bool head) {
    memset(f, 0, sizeof *f);
    uint64_t port = loc->uri.port ? loc->uri.port : 80;
    char *pin = location_pin(loc, port);
    const char *pins[1] = {pin};
    cf_http_config hc;
    memset(&hc, 0, sizeof hc);
    hc.connect_timeout_ms = cfg->connect_timeout_ms;
    hc.read_timeout_ms = cfg->read_timeout_ms;
    hc.deadline_ms = budget_ms > 0 ? budget_ms : 1;
    hc.max_bytes = head ? 65536 : CF_UNFURL_MAX_BODY_SIZE;
    hc.length_cap = (!head) ? CF_UNFURL_MAX_BODY_SIZE : 0;
    hc.ca_path = cfg->ca_path;
    cf_http_err e = cf_http_exchange(&hc, head ? "HEAD" : "GET", url, NULL, 0, NULL, 0,
                                    pin ? pins : NULL, pin ? 1 : 0, &f->res);
    free(pin);
    if (e == CF_HTTP_OK) {
        f->ok = true;
        f->status = f->res.status;
    }
    return e;
}

/* Follow up to 10 responses (any 3xx is a redirect). Returns true with the
 * terminal response in f, false for no-unfurl (guard/fetch/redirect fail). */
static bool fetch_loop(const cf_unfurl_config *cfg, const char *url, bool head, fetched *f,
                       long end_ms) {
    char *cur = strdup(url);
    if (!cur) return false;
    bool done = false;
    for (int hop = 0; hop < CF_UNFURL_MAX_REDIRECTS; hop++) {
        if (ms_now() >= end_ms) break;
        location loc;
        location_init(&loc, cfg, cur);
        bool valid = location_valid(&loc);
        if (!valid) {
            location_dispose(&loc);
            break;
        }
        long budget = end_ms - ms_now();
        fetched one;
        memset(&one, 0, sizeof one);
        cf_http_err e = fetch_once(cfg, cur, &loc, budget, &one, head);
        location_dispose(&loc);
        if (e != CF_HTTP_OK) break;
        if (one.status >= 300 && one.status <= 399) {
            char *next = cf_http_header_joined(&one.res, "location");
            fetched_dispose(&one);
            if (!next) break;
            /* Redirect targets must parse as absolute http(s) and go back
             * through the guard (relative targets are denied). */
            cf_http_uri u;
            bool ok = cf_http_uri_parse(next, &u) == CF_HTTP_OK && cf_http_uri_is_http(&u);
            cf_http_uri_dispose(&u);
            free(cur);
            if (!ok) {
                free(next);
                cur = NULL;
                break;
            }
            cur = next;
            continue;
        }
        *f = one;
        done = true;
        break;
    }
    /* 10 redirects without a terminal response: TooManyRedirects => none. */
    free(cur);
    return done;
}

static bool fetch_document(const cf_unfurl_config *cfg, const char *url, unsigned char **body,
                           size_t *body_len, long end_ms) {
    *body = NULL;
    *body_len = 0;
    fetched f;
    memset(&f, 0, sizeof f);
    if (!fetch_loop(cfg, url, false, &f, end_ms)) return false;
    bool ok = false;
    if (f.status == 200) {
        char *ct = cf_http_content_type(&f.res);
        if (ct && !strcmp(ct, "text/html")) {
            *body = f.res.body;
            *body_len = f.res.body_len;
            f.res.body = NULL;
            ok = true;
        }
        free(ct);
    }
    fetched_dispose(&f);
    return ok;
}

static char *fetch_content_type(const cf_unfurl_config *cfg, const char *url, long end_ms) {
    fetched f;
    memset(&f, 0, sizeof f);
    if (!fetch_loop(cfg, url, true, &f, end_ms)) return NULL;
    /* Status is NOT checked for images; the raw joined Content-Type is. */
    char *ct = cf_http_header_joined(&f.res, "content-type");
    fetched_dispose(&f);
    return ct; /* may be NULL */
}

/* --- metadata ----------------------------------------------------------------- */

static const char *kTwitter[] = {"twitter.com", "www.twitter.com", "x.com", "www.x.com"};
static const char *kFxTwitter = "fxtwitter.com";

static bool is_twitter_url(const char *url, bool *raise) {
    *raise = false;
    cf_http_uri u;
    cf_http_err e = cf_http_uri_parse(url, &u);
    if (e == CF_HTTP_INVALID_PART) {
        *raise = true;
        return false;
    }
    if (e != CF_HTTP_OK) return false;
    bool tw = false;
    if (u.host) {
        for (size_t i = 0; i < sizeof kTwitter / sizeof kTwitter[0]; i++)
            if (!strcmp(u.host, kTwitter[i])) tw = true;
    }
    bool path_ok = u.host && u.path && !str_blank(u.path) && strcmp(u.path, "/");
    cf_http_uri_dispose(&u);
    return tw && path_ok;
}

/* Replace the twitter host with fxtwitter (to_s normalization). */
static char *fxtwitter_url(const char *url) {
    cf_http_uri u;
    if (cf_http_uri_parse(url, &u) != CF_HTTP_OK) return NULL;
    free(u.host);
    u.host = strdup(kFxTwitter);
    if (!u.host) {
        cf_http_uri_dispose(&u);
        return NULL;
    }
    sbuf b;
    memset(&b, 0, sizeof b);
    if (u.scheme) {
        sb_putn(&b, u.scheme, strlen(u.scheme));
        sb_putn(&b, ":", 1);
    }
    if (u.opaque) {
        sb_putn(&b, u.opaque, strlen(u.opaque));
    } else if (u.host) {
        sb_putn(&b, "//", 2);
        if (u.userinfo) {
            sb_putn(&b, u.userinfo, strlen(u.userinfo));
            sb_putn(&b, "@", 1);
        }
        sb_putn(&b, u.host, strlen(u.host));
        uint64_t def = 0;
        if (u.scheme) {
            if (!strcasecmp(u.scheme, "http") || !strcasecmp(u.scheme, "ws")) def = 80;
            else if (!strcasecmp(u.scheme, "https") || !strcasecmp(u.scheme, "wss")) def = 443;
        }
        if (u.port && u.port != def) {
            char pb[32];
            snprintf(pb, sizeof pb, ":%llu", (unsigned long long)u.port);
            sb_putn(&b, pb, strlen(pb));
        }
        sb_putn(&b, u.path ? u.path : "", u.path ? strlen(u.path) : 0);
        if (u.query) {
            sb_putn(&b, "?", 1);
            sb_putn(&b, u.query, strlen(u.query));
        }
    }
    if (u.fragment) {
        sb_putn(&b, "#", 1);
        sb_putn(&b, u.fragment, strlen(u.fragment));
    }
    cf_http_uri_dispose(&u);
    if (b.oom) {
        free(b.data);
        return NULL;
    }
    return b.data ? b.data : strdup("");
}

/* Canonical URL: kept only when it is itself a valid location. */
static char *valid_canonical(const cf_unfurl_config *cfg, const char *og_url, const char *fallback) {
    if (og_url) {
        location l;
        location_init(&l, cfg, og_url);
        bool ok = location_valid(&l);
        location_dispose(&l);
        if (ok) return strdup(og_url);
    }
    return strdup(fallback);
}

static const char *kAllowedImages[] = {"image/jpeg", "image/png", "image/gif", "image/webp"};

static char *valid_image(const cf_unfurl_config *cfg, const char *image, long end_ms) {
    if (!image || str_blank(image)) return NULL;
    cf_http_uri u;
    if (cf_http_uri_parse(image, &u) != CF_HTTP_OK) return NULL; /* warn in log */
    cf_http_uri_dispose(&u);
    char *ct = fetch_content_type(cfg, image, end_ms);
    if (!ct) return NULL;
    for (char *p = ct; *p; p++) *p = (char)tolower((unsigned char)*p);
    bool ok = false;
    for (size_t i = 0; i < sizeof kAllowedImages / sizeof kAllowedImages[0]; i++)
        if (!strcmp(ct, kAllowedImages[i])) ok = true;
    free(ct);
    if (!ok) return NULL;
    return strdup(image);
}

/* rails json encode for a string (generate + <>& escaping). */
static void json_encode_str(sbuf *b, const char *s) {
    sb_putn(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': sb_putn(b, "\\\"", 2); break;
        case '\\': sb_putn(b, "\\\\", 2); break;
        case '\b': sb_putn(b, "\\b", 2); break;
        case '\f': sb_putn(b, "\\f", 2); break;
        case '\n': sb_putn(b, "\\n", 2); break;
        case '\r': sb_putn(b, "\\r", 2); break;
        case '\t': sb_putn(b, "\\t", 2); break;
        case '<': sb_putn(b, "\\u003c", 6); break;
        case '>': sb_putn(b, "\\u003e", 6); break;
        case '&': sb_putn(b, "\\u0026", 6); break;
        default:
            if (*p < 0x20) {
                char e[7];
                snprintf(e, sizeof e, "\\u%04x", *p);
                sb_putn(b, e, 6);
            } else {
                sb_putn(b, (const char *)p, 1);
            }
        }
    }
    sb_putn(b, "\"", 1);
}

static void json_encode_opt(sbuf *b, const char *key, const char *s) {
    json_encode_str(b, key);
    sb_putn(b, ":", 1);
    if (!s) sb_putn(b, "null", 4);
    else json_encode_str(b, s);
}

/* --- validate: strip_tags + sanitize via R02 ---------------------------------- */

/* FullSanitizer strip_tags: text of the HTML5 fragment, re-serialized. */
static char *strip_tags(const char *html, bool *raised) {
    *raised = false;
    rt_dom dom;
    rt_dom_init(&dom);
    rt_node root = RT_NODE_NONE;
    if (rt_dom_parse_fragment(&dom, (const unsigned char *)html, strlen(html), &root) != RT_OK) {
        rt_dom_dispose(&dom);
        *raised = true; /* ArgumentError */
        return NULL;
    }
    rt_node_vec nodes = rt_dom_descendants(&dom, root);
    rt_buf text;
    rt_buf_init(&text);
    rt_status rc = RT_OK;
    for (size_t i = 0; i < nodes.len && rc == RT_OK; i++) {
        if (!rt_dom_is_text(&dom, nodes.items[i])) continue;
        const char *t = rt_dom_text(&dom, nodes.items[i]);
        if (!t) continue;
        size_t tl = strlen(t);
        if (text.len + tl < text.len || text.len + tl + 1 <= text.len) {
            rc = RT_NOMEM;
            break;
        }
        if (text.len + tl + 1 > text.cap) {
            size_t cap = text.cap ? text.cap : 64;
            while (cap < text.len + tl + 1) cap *= 2;
            unsigned char *nb = realloc(text.data, cap);
            if (!nb) {
                rc = RT_NOMEM;
                break;
            }
            text.data = nb;
            text.cap = cap;
        }
        memcpy(text.data + text.len, t, tl);
        text.len += tl;
    }
    rt_node_vec_dispose(&nodes);
    char *o = NULL;
    if (rc == RT_OK) {
        rt_node t = rt_dom_create_text(&dom, text.data ? text.data : (const unsigned char *)"",
                                      text.len);
        rt_buf ser;
        rt_buf_init(&ser);
        rc = rt_dom_serialize(&dom, t, false, &ser);
        if (rc == RT_OK) {
            o = malloc(ser.len + 1);
            if (o) {
                memcpy(o, ser.data ? ser.data : (const unsigned char *)"", ser.len);
                o[ser.len] = '\0';
            } else {
                rc = RT_NOMEM;
            }
        }
        rt_buf_dispose(&ser);
    }
    rt_buf_dispose(&text);
    rt_dom_dispose(&dom);
    if (rc != RT_OK) {
        free(o);
        *raised = true;
        return NULL;
    }
    return o ? o : strdup("");
}

static char *sanitize_html(const char *html, bool *raised) {
    *raised = false;
    rt_buf o;
    rt_buf_init(&o);
    rt_status rc = rt_sanitize((const unsigned char *)html, strlen(html), rt_safe_list_defaults(), &o);
    if (rc != RT_OK) {
        rt_buf_dispose(&o);
        *raised = true; /* ArgumentError */
        return NULL;
    }
    char *s = malloc(o.len + 1);
    if (!s) {
        rt_buf_dispose(&o);
        *raised = true;
        return NULL;
    }
    memcpy(s, o.data ? o.data : (const unsigned char *)"", o.len);
    s[o.len] = '\0';
    rt_buf_dispose(&o);
    return s;
}

/* --- entry ---------------------------------------------------------------------- */

int cf_unfurl(const cf_unfurl_config *cfg, const char *url, cf_unfurl_out *out) {
    memset(out, 0, sizeof *out);
    if (!cfg || !url) return -1;
    long end = ms_now() + cfg->deadline_ms;
    if (!sem_acquire(cfg->deadline_ms)) {
        out->kind = CF_UNFURL_NONE; /* timeout waiting => no unfurl */
        return 0;
    }
    int r = -1;
    out->kind = CF_UNFURL_NONE;

    bool raise = false;
    bool tweet = is_twitter_url(url, &raise);
    if (raise) {
        out->kind = CF_UNFURL_RAISED;
        out->raised = "URI::InvalidComponentError";
        r = 0;
        goto done;
    }
    /* Fetch the document (tweets via fxtwitter; a missing fxtwitter page
     * raises NoMethodError like nil.force_encoding). */
    unsigned char *body = NULL;
    size_t body_len = 0;
    if (tweet) {
        char *fx = fxtwitter_url(url);
        bool fetched = false;
        if (fx) {
            location l;
            location_init(&l, cfg, fx);
            bool valid = location_valid(&l) && !cf_unfurl_is_media_url(fx);
            location_dispose(&l);
            if (valid) {
                unsigned char *b = NULL;
                size_t bl = 0;
                if (fetch_document(cfg, fx, &b, &bl, end)) {
                    body = b;
                    body_len = bl;
                    fetched = true;
                }
            }
            free(fx);
        }
        if (!fetched) {
            out->kind = CF_UNFURL_RAISED;
            out->raised = "NoMethodError";
            r = 0;
            goto done;
        }
    } else {
        location l;
        location_init(&l, cfg, url);
        bool valid = location_valid(&l) && !cf_unfurl_is_media_url(url);
        location_dispose(&l);
        if (valid) {
            unsigned char *b = NULL;
            size_t bl = 0;
            if (fetch_document(cfg, url, &b, &bl, end)) {
                body = b;
                body_len = bl;
            }
        }
    }

    og_found found;
    bool url_tag = false, img_tag = false;
    opengraph_attributes(body, body_len, &found, &url_tag, &img_tag);
    free(body);

    /* Canonical URL + image checks (each re-resolves through the guard). */
    char *canon = valid_canonical(cfg, found.values[1], url);
    char *img = valid_image(cfg, found.values[2], end);
    free(found.values[1]);
    found.values[1] = canon;
    free(found.values[2]);
    found.values[2] = img;
    if (!canon) {
        for (int i = 0; i < 4; i++) free(found.values[i]);
        goto done; /* NOMEM */
    }

    /* validate(): sanitize title/description, require presence; a non-blank
     * image must re-validate as a location. */
    for (int pass = 0; pass < 2; pass++) {
        int idx = pass == 0 ? 0 : 3; /* title, description */
        if (!found.values[idx]) continue;
        bool rs = false;
        char *stripped = strip_tags(found.values[idx], &rs);
        char *san = NULL;
        if (!rs) san = sanitize_html(stripped ? stripped : "", &rs);
        free(stripped);
        if (rs) {
            free(san);
            for (int i = 0; i < 4; i++) free(found.values[i]);
            out->kind = CF_UNFURL_RAISED;
            out->raised = "ArgumentError";
            r = 0;
            goto done;
        }
        free(found.values[idx]);
        found.values[idx] = san;
    }
    bool ok = found.values[0] && !str_blank(found.values[0]) && found.values[1] &&
              !str_blank(found.values[1]) && found.values[3] && !str_blank(found.values[3]);
    if (ok && found.values[2] && !str_blank(found.values[2])) {
        location l;
        location_init(&l, cfg, found.values[2]);
        ok = location_valid(&l);
        location_dispose(&l);
    }
    if (!ok) {
        for (int i = 0; i < 4; i++) free(found.values[i]);
        out->kind = CF_UNFURL_NONE;
        r = 0;
        goto done;
    }
    /* render json: instance values in first-assigned order (found tags in
     * ATTRIBUTES order, then appended url/image), plus the validation
     * context and empty errors. */
    sbuf b;
    memset(&b, 0, sizeof b);
    sb_putn(&b, "{", 1);
    bool first = true;
    /* Slot order with appended-key rule: title(0), url(1), image(2)... the
     * reference emits found keys first. Reproduce: emitted = for each
     * present tag in ATTRIBUTES order, then url/image when not tags. */
    int order[4];
    int no = 0;
    for (int i = 0; i < 4; i++) {
        /* url/image emit in place only when the page had the tag; title
         * and description always emit in place when found. */
        if (i == 1 && !url_tag) continue;
        if (i == 2 && !img_tag) continue;
        if (!found.values[i]) continue;
        order[no++] = i;
    }
    if (!url_tag && found.values[1]) order[no++] = 1;
    if (!img_tag) order[no++] = 2; /* image always present (may be null) */
    for (int k = 0; k < no; k++) {
        if (!first) sb_putn(&b, ",", 1);
        first = false;
        json_encode_opt(&b, kAttrs[order[k]], found.values[order[k]]);
    }
    {
        static const char tail[] = ",\"context_for_validation\":{\"context\":null},\"errors\":{}}";
        sb_putn(&b, tail, strlen(tail));
    }
    for (int i = 0; i < 4; i++) free(found.values[i]);
    if (b.oom || !b.data) {
        free(b.data);
        goto done; /* NOMEM */
    }
    out->kind = CF_UNFURL_JSON;
    out->json = b.data;
    r = 0;
done:
    sem_release();
    return r;
}
