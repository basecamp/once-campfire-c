/* src/integrations/http.c — I01 outbound HTTP over libcurl.
 *
 * Reference: net/http.rs (Net::HTTP-shaped exchange: pinned address,
 * verified TLS against the host name, open_timeout over connect+handshake,
 * read_timeout over each read, Accept-Encoding inflated on read).
 *
 * Differences from the reference (explicit, integrator-visible):
 *  - Content decoding is done here with zlib instead of libcurl's built-in
 *    decoder, because the Fil-C libcurl is --without-zlib. The wire header
 *    is identical, and caps apply to decoded bytes in both builds.
 *  - DNS pinning uses CURLOPT_RESOLVE entries built by the caller (unfurl
 *    resolves+guards first, then pins; webhook resolves normally).
 */
#include "integrations/http.h"

#include <ctype.h>
#include <curl/curl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <zlib.h>

const char *cf_http_err_name(cf_http_err e) {
    switch (e) {
    case CF_HTTP_OK: return "ok";
    case CF_HTTP_NOMEM: return "nomem";
    case CF_HTTP_INVALID_URL: return "invalid-url";
    case CF_HTTP_INVALID_PART: return "invalid-part";
    case CF_HTTP_SCHEME: return "scheme";
    case CF_HTTP_DNS: return "dns";
    case CF_HTTP_CONNECT: return "connect";
    case CF_HTTP_TLS: return "tls";
    case CF_HTTP_TIMEOUT_CONN: return "connect-timeout";
    case CF_HTTP_TIMEOUT_READ: return "read-timeout";
    case CF_HTTP_TIMEOUT_ALL: return "deadline";
    case CF_HTTP_TOO_LARGE: return "too-large";
    case CF_HTTP_BAD_LENGTH: return "bad-length";
    case CF_HTTP_INFLATE: return "inflate";
    case CF_HTTP_IO: return "io";
    }
    return "unknown";
}

void cf_http_global_init(void);
static void curl_global_init_void(void);

void cf_http_global_init(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    /* No destructor: process-lifetime global like the reference TLS store. */
    pthread_once(&once, curl_global_init_void);
}

/* curl_global_init has signature int(long); adapt for pthread_once. */
static void curl_global_init_void(void) {
    (void)curl_global_init(CURL_GLOBAL_DEFAULT);
}

/* --- URI parsing (uri.rs slice) ------------------------------------------- */

static char *xstrndup(const char *s, size_t n) {
    char *o = malloc(n + 1);
    if (!o) return NULL;
    memcpy(o, s, n);
    o[n] = '\0';
    return o;
}

static bool scheme_char(char c, bool first) {
    if (isalpha((unsigned char)c)) return true;
    if (first) return false;
    return isdigit((unsigned char)c) || c == '+' || c == '-' || c == '.';
}

/* Ruby query= validation: rejects '%' followed by two NON-hex chars
 * (note the &&: exactly as the reference, bug-compatible). */
static bool query_escapes_ok(const char *q) {
    /* Strip tabs/CR/LF first like the reference, then scan triples. */
    size_t n = strlen(q);
    char *c = malloc(n + 1);
    if (!c) return false;
    size_t m = 0;
    for (size_t i = 0; i < n; i++)
        if (q[i] != '\t' && q[i] != '\r' && q[i] != '\n') c[m++] = q[i];
    c[m] = '\0';
    bool ok = true;
    for (size_t i = 0; i + 2 < m; i++) {
        if (c[i] == '%' && !isxdigit((unsigned char)c[i + 1]) && !isxdigit((unsigned char)c[i + 2])) {
            ok = false;
            break;
        }
    }
    free(c);
    return ok;
}

/* /\A(?:[^@,;]+@[^@,;]+(?:\z|[,;]))*\z/ over the mailto To-part. */
static bool mailto_to_valid(const char *to) {
    size_t n = strlen(to);
    size_t i = 0;
    if (n == 0) return false;
    for (;;) {
        size_t start = i;
        while (i < n && to[i] != '@' && to[i] != ',' && to[i] != ';') i++;
        if (i == start || i >= n || to[i] != '@') return false;
        i++;
        size_t ds = i;
        while (i < n && to[i] != '@' && to[i] != ',' && to[i] != ';') i++;
        if (i == ds) return false;
        if (i >= n) return true;
        if (to[i] == '@') return false;
        i++; /* [,;] */
        if (i >= n) return true;
    }
}

static uint64_t default_port_for(const char *scheme) {
    if (!scheme) return 0;
    if (!strcasecmp(scheme, "http") || !strcasecmp(scheme, "ws")) return 80;
    if (!strcasecmp(scheme, "https") || !strcasecmp(scheme, "wss")) return 443;
    if (!strcasecmp(scheme, "ftp")) return 21;
    if (!strcasecmp(scheme, "ldap")) return 389;
    if (!strcasecmp(scheme, "ldaps")) return 636;
    return 0;
}

/* Characters rejected in a relative reference (strict subset: anything the
 * RFC3986 parser would reject that our callers can produce). */
static bool relative_char_ok(unsigned char c) {
    if (c < 0x21 || c == 0x7f) return false;
    switch (c) {
    case '"': case '<': case '>': case '\\': case '^':
    case '{': case '|': case '}': case ' ': return false;
    default: return true;
    }
}

cf_http_err cf_http_uri_parse(const char *url, cf_http_uri *out) {
    memset(out, 0, sizeof *out);
    char *host = NULL; /* hoisted: failure labels below are function-scope */
    if (!url) return CF_HTTP_INVALID_URL;
    size_t n = strlen(url);
    if (n == 0) return CF_HTTP_INVALID_URL;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)url[i] > 0x7f) return CF_HTTP_INVALID_URL;

    /* Scheme probe: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ':'. */
    size_t sn = 0;
    if (isalpha((unsigned char)url[0])) {
        sn = 1;
        while (sn < n && scheme_char(url[sn], false)) sn++;
    }
    if (sn > 0 && sn < n && url[sn] == ':') {
        char *scheme = xstrndup(url, sn);
        if (!scheme) return CF_HTTP_NOMEM;
        out->scheme = scheme;
        const char *rest = url + sn + 1;
        if (rest[0] == '/' && rest[1] == '/') {
            /* Hierarchical: [userinfo@]host[:port] path?query#fragment. */
            const char *auth = rest + 2;
            size_t alen = strcspn(auth, "/?#");
            const char *after = auth + alen;
            const char *at = memchr(auth, '@', alen);
            const char *hostport = auth;
            size_t hlen = alen;
            if (at) {
                out->userinfo = xstrndup(auth, (size_t)(at - auth));
                if (!out->userinfo) goto nomem;
                hostport = at + 1;
                hlen = (size_t)(auth + alen - hostport);
            }
            uint64_t port = 0;
            bool have_port = false;
            if (hlen > 0 && hostport[0] == '[') {
                const char *close = memchr(hostport, ']', hlen);
                if (!close) goto invalid;
                size_t hn = (size_t)(close + 1 - hostport);
                host = xstrndup(hostport, hn);
                if (!host) goto nomem;
                const char *pr = close + 1;
                if (*pr == ':') {
                    pr++;
                    if (*pr == '\0') goto invalid_free_host;
                    for (const char *p = pr; *p; p++)
                        if (!isdigit((unsigned char)*p)) goto invalid_free_host;
                    have_port = true;
                    port = strtoull(pr, NULL, 10);
                } else if (*pr != '\0') {
                    goto invalid_free_host;
                }
            } else {
                const char *colon = memrchr(hostport, ':', hlen);
                if (colon) {
                    host = xstrndup(hostport, (size_t)(colon - hostport));
                    if (!host) goto nomem;
                    const char *pr = colon + 1;
                    size_t pn = hlen - (size_t)(pr - hostport);
                    if (pn == 0) {
                        /* "host:" — Ruby keeps nil port; treat as absent. */
                    } else {
                        for (size_t i = 0; i < pn; i++)
                            if (!isdigit((unsigned char)pr[i])) goto invalid_free_host;
                        have_port = true;
                        char *tmp = xstrndup(pr, pn);
                        if (!tmp) goto nomem_free_host;
                        port = strtoull(tmp, NULL, 10);
                        free(tmp);
                    }
                } else {
                    host = xstrndup(hostport, hlen);
                    if (!host) goto nomem;
                }
            }
            out->host = host;
            /* path?query#fragment */
            const char *frag = strchr(after, '#');
            const char *q = NULL;
            const char *pstart = after;
            size_t plen;
            if (frag) {
                out->fragment = strdup(frag + 1);
                if (!out->fragment) goto nomem;
                q = memchr(after, '?', (size_t)(frag - after));
                plen = (q ? (size_t)(q - after) : (size_t)(frag - after));
            } else {
                q = strchr(after, '?');
                plen = (q ? (size_t)(q - after) : strlen(after));
            }
            if (plen > 0 || *after == '/' || *after == '?' || *after == '#' || *after == '\0') {
                out->path = xstrndup(pstart, plen);
                if (!out->path) goto nomem;
            } else {
                out->path = strdup("");
                if (!out->path) goto nomem;
            }
            if (q) {
                size_t qlen = frag ? (size_t)(frag - (q + 1)) : strlen(q + 1);
                out->query = xstrndup(q + 1, qlen);
                if (!out->query) goto nomem;
                if (!query_escapes_ok(out->query)) goto invalid;
            }
            if (out->path) {
                for (const char *p = out->path; *p; p++)
                    if (!relative_char_ok((unsigned char)*p) && *p != '%' && *p != '/') {
                        /* Path shares the relative character rules. */
                        if ((unsigned char)*p <= 0x20 || (unsigned char)*p == 0x7f) goto invalid;
                    }
            }
            if (!strcasecmp(scheme, "ftp") && !out->path) goto invalid;
            if ((!strcasecmp(scheme, "ldap") || !strcasecmp(scheme, "ldaps")) &&
                (out->fragment || !out->path))
                goto invalid;
            if (!have_port) port = default_port_for(scheme);
            out->port = port;
            return CF_HTTP_OK;
        }
        /* Opaque form (mailto:foo, ...). */
        const char *hash = strchr(rest, '#');
        size_t olen = hash ? (size_t)(hash - rest) : strlen(rest);
        out->opaque = xstrndup(rest, olen);
        if (!out->opaque) goto nomem;
        if (hash) {
            out->fragment = strdup(hash + 1);
            if (!out->fragment) goto nomem;
        }
        if (!strcasecmp(scheme, "mailto")) {
            const char *opq = out->opaque;
            const char *qm = strchr(opq, '?');
            char *to = xstrndup(opq, qm ? (size_t)(qm - opq) : strlen(opq));
            if (!to) goto nomem;
            bool ok = mailto_to_valid(to);
            free(to);
            if (!ok) {
                /* InvalidComponent stays distinct from InvalidUri: the
                 * unfurl controller maps it to a 500 raise. */
                cf_http_uri_dispose(out);
                return CF_HTTP_INVALID_PART;
            }
        }
        return CF_HTTP_OK;
    }
    /* Relative reference: must be all acceptable chars. */
    for (size_t i = 0; i < n; i++)
        if (!relative_char_ok((unsigned char)url[i])) return CF_HTTP_INVALID_URL;
    {
        const char *hash = strchr(url, '#');
        const char *q = NULL;
        size_t plen;
        if (hash) {
            out->fragment = strdup(hash + 1);
            if (!out->fragment) return CF_HTTP_NOMEM;
            const char *qe = memchr(url, '?', (size_t)(hash - url));
            q = qe;
            plen = q ? (size_t)(q - url) : (size_t)(hash - url);
        } else {
            q = strchr(url, '?');
            plen = q ? (size_t)(q - url) : n;
        }
        out->path = xstrndup(url, plen);
        if (!out->path) goto nomem;
        if (q) {
            size_t qlen = hash ? (size_t)(hash - (q + 1)) : strlen(q + 1);
            out->query = xstrndup(q + 1, qlen);
            if (!out->query) goto nomem;
            if (!query_escapes_ok(out->query)) goto invalid;
        }
        return CF_HTTP_OK;
    }
nomem_free_host:
    free(host);
nomem:
    cf_http_uri_dispose(out);
    return CF_HTTP_NOMEM;
invalid_free_host:
    free(host);
invalid:
    cf_http_uri_dispose(out);
    return CF_HTTP_INVALID_URL;
}

void cf_http_uri_dispose(cf_http_uri *u) {
    if (!u) return;
    free(u->scheme);
    free(u->userinfo);
    free(u->host);
    free(u->path);
    free(u->opaque);
    free(u->query);
    free(u->fragment);
    memset(u, 0, sizeof *u);
}

bool cf_http_uri_is_http(const cf_http_uri *u) {
    if (!u || !u->scheme) return false;
    return !strcasecmp(u->scheme, "http") || !strcasecmp(u->scheme, "https");
}

char *cf_http_uri_target(const cf_http_uri *u) {
    const char *path = (u && u->path) ? u->path : "";
    const char *query = (u && u->query) ? u->query : NULL;
    size_t pn = strlen(path), qn = query ? strlen(query) : 0;
    size_t need = pn + (query ? 1 + qn : 0) + 2;
    char *t = malloc(need);
    if (!t) return NULL;
    if (path[0] == '/') {
        memcpy(t, path, pn);
        if (query) {
            t[pn] = '?';
            memcpy(t + pn + 1, query, qn + 1);
        } else {
            t[pn] = '\0';
        }
    } else {
        t[0] = '/';
        memcpy(t + 1, path, pn);
        if (query) {
            t[1 + pn] = '?';
            memcpy(t + 1 + pn + 1, query, qn + 1);
        } else {
            t[1 + pn] = '\0';
        }
    }
    return t;
}

char *cf_http_host_header(const char *host, uint64_t port, bool https) {
    if (!host) return NULL;
    /* Strip brackets for the bare form, then re-add for IPv6 literals. */
    const char *h = host;
    size_t hn = strlen(host);
    if (hn >= 2 && host[0] == '[' && host[hn - 1] == ']') {
        h = host + 1;
        hn -= 2;
    }
    bool v6 = memchr(h, ':', hn) != NULL;
    uint64_t def = https ? 443 : 80;
    char *o;
    if (port == def) {
        if (v6) {
            o = malloc(hn + 3);
            if (!o) return NULL;
            sprintf(o, "[%.*s]", (int)hn, h);
        } else {
            o = xstrndup(h, hn);
        }
    } else {
        char pb[32];
        snprintf(pb, sizeof pb, "%llu", (unsigned long long)port);
        if (v6) {
            size_t pn = strlen(pb);
            o = malloc(hn + pn + 4);
            if (!o) return NULL;
            sprintf(o, "[%.*s]:%s", (int)hn, h, pb);
        } else {
            size_t pn = strlen(pb);
            o = malloc(hn + pn + 2);
            if (!o) return NULL;
            sprintf(o, "%.*s:%s", (int)hn, h, pb);
        }
    }
    return o;
}

/* --- response helpers ------------------------------------------------------ */

void cf_http_response_dispose(cf_http_response *r) {
    if (!r) return;
    for (size_t i = 0; i < r->nheaders; i++) {
        free(r->headers[i].name);
        free(r->headers[i].value);
    }
    free(r->headers);
    free(r->body);
    memset(r, 0, sizeof *r);
}

const char *cf_http_header_get(const cf_http_response *r, const char *name) {
    if (!r) return NULL;
    for (size_t i = 0; i < r->nheaders; i++)
        if (!strcasecmp(r->headers[i].name, name)) return r->headers[i].value;
    return NULL;
}

char *cf_http_header_joined(const cf_http_response *r, const char *name) {
    size_t total = 0, count = 0;
    for (size_t i = 0; i < r->nheaders; i++) {
        if (!strcasecmp(r->headers[i].name, name)) {
            total += strlen(r->headers[i].value) + 2;
            count++;
        }
    }
    if (!count) return NULL;
    char *o = malloc(total + 1);
    if (!o) return NULL;
    o[0] = '\0';
    for (size_t i = 0; i < r->nheaders; i++) {
        if (!strcasecmp(r->headers[i].name, name)) {
            if (o[0]) strcat(o, ", ");
            strcat(o, r->headers[i].value);
        }
    }
    return o;
}

/* Ruby String#strip (ASCII whitespace + NUL) on both ends. */
static char *ruby_strip(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\v' || *s == '\f' || *s == '\r' || *s == '\0')
        s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\v' ||
                     s[n - 1] == '\f' || s[n - 1] == '\r' || s[n - 1] == '\0'))
        n--;
    return xstrndup(s, n);
}

char *cf_http_content_type(const cf_http_response *r) {
    char *joined = cf_http_header_joined(r, "content-type");
    if (!joined) return NULL;
    char *semi = strchr(joined, ';');
    if (semi) *semi = '\0';
    char *slash = strchr(joined, '/');
    char *o;
    if (slash) {
        *slash = '\0';
        char *main = ruby_strip(joined);
        char *sub = ruby_strip(slash + 1);
        if (!main || !sub) {
            free(main);
            free(sub);
            free(joined);
            return NULL;
        }
        o = malloc(strlen(main) + strlen(sub) + 2);
        if (o) sprintf(o, "%s/%s", main, sub);
        free(main);
        free(sub);
    } else {
        o = ruby_strip(joined);
    }
    free(joined);
    return o;
}

uint64_t cf_http_content_length(const cf_http_response *r, bool *ok, bool *bad) {
    *ok = false;
    *bad = false;
    char *joined = cf_http_header_joined(r, "content-length");
    if (!joined) return 0;
    const char *p = joined;
    while (*p && !isdigit((unsigned char)*p)) p++;
    if (!*p) {
        *bad = true;
        free(joined);
        return 0;
    }
    uint64_t v = 0;
    bool overflow = false;
    while (isdigit((unsigned char)*p)) {
        unsigned d = (unsigned)(*p - '0');
        if (v > (UINT64_MAX - d) / 10) overflow = true;
        else v = v * 10 + d;
        p++;
    }
    free(joined);
    *ok = true;
    return overflow ? UINT64_MAX : v;
}

/* --- exchange -------------------------------------------------------------- */

static long ms_now(void);

typedef struct {
    cf_http_response *res;
    unsigned char *body;
    size_t body_len, body_cap;
    size_t max_bytes;
    size_t length_cap;
    size_t header_bytes, max_header_bytes;
    bool length_abort;   /* Content-Length passed length_cap */
    bool length_bad;     /* malformed Content-Length with length_cap set */
    bool too_large;      /* decoded body passed max_bytes */
    bool inflate_err;
    bool head_done;
    bool is_head;
    /* decode state */
    bool decode;         /* method allows + gzip/deflate + no content-range */
    bool inflated;       /* zlib stream active */
    bool stream_end;     /* a gzip member finished; more may follow */
    bool gzip_mode;
    bool header_checked;
    bool have_pending;
    unsigned char pending[2];
    size_t npending;
    z_stream strm;
    bool strm_on;
    /* Read-inactivity enforcement (progress callback on real activity). */
    long read_timeout_ms;
    long last_activity_ms;
    bool read_stall;
} xfer;

static void xfer_dispose(xfer *x) {
    if (x->strm_on) {
        inflateEnd(&x->strm);
        x->strm_on = false;
    }
}

static bool xfer_grow(xfer *x, size_t extra) {
    if (x->body_len + extra < x->body_len) return false;
    size_t need = x->body_len + extra;
    if (need <= x->body_cap) return true;
    size_t cap = x->body_cap ? x->body_cap : 8192;
    while (cap < need) {
        if (cap > (size_t)1 << 30) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    unsigned char *nb = realloc(x->body, cap);
    if (!nb) return false;
    x->body = nb;
    x->body_cap = cap;
    return true;
}

/* Append decoded bytes, enforcing the cap. */
static bool xfer_emit(xfer *x, const unsigned char *p, size_t n) {
    if (n == 0) return true;
    if (x->body_len + n > x->max_bytes || x->body_len + n < x->body_len) {
        x->too_large = true;
        return false;
    }
    if (!xfer_grow(x, n)) return false; /* caller maps OOM via flag */
    memcpy(x->body + x->body_len, p, n);
    x->body_len += n;
    return true;
}

static size_t header_cb(char *ptr, size_t size, size_t nitems, void *ud) {
    xfer *x = ud;
    if (size && nitems > SIZE_MAX / size) { x->too_large = true; return 0; }
    size_t n = size * nitems;
    if (x->max_header_bytes &&
        (n > x->max_header_bytes - x->header_bytes)) {
        x->too_large = true;
        return 0;
    }
    if (x->max_header_bytes) x->header_bytes += n;
    x->last_activity_ms = ms_now();
    if (n >= 5 && !memcmp(ptr, "HTTP/", 5)) {
        /* HTTP/2 has no reason phrase. HTTP/1 status is three digits,
         * followed by an optional reason; never retain transport text past
         * the fixed diagnostic buffer. */
        x->res->reason[0] = '\0';
        const char *sp = memchr(ptr, ' ', n);
        if (sp && (size_t)(ptr + n - sp) > 4 && sp[4] == ' ') {
            const char *r = sp + 5;
            size_t len = (size_t)(ptr + n - r);
            while (len && (r[len-1] == '\r' || r[len-1] == '\n')) len--;
            if (len >= sizeof x->res->reason) len = sizeof x->res->reason - 1;
            memcpy(x->res->reason, r, len);
            x->res->reason[len] = '\0';
        }
        return n;
    }
    if (n == 2 && ptr[0] == '\r' && ptr[1] == '\n') {
        x->head_done = true;
        return n;
    }
    char *colon = memchr(ptr, ':', n);
    if (!colon) return n; /* status line */
    size_t nn = (size_t)(colon - ptr);
    while (nn > 0 && (ptr[nn - 1] == ' ' || ptr[nn - 1] == '\t')) nn--;
    const char *vs = colon + 1;
    size_t vn = n - (size_t)(vs - ptr);
    while (vn > 0 && (*vs == ' ' || *vs == '\t')) {
        vs++;
        vn--;
    }
    while (vn > 0 && (vs[vn - 1] == '\r' || vs[vn - 1] == '\n')) vn--;
    char *name = xstrndup(ptr, nn);
    char *value = xstrndup(vs, vn);
    if (!name || !value) {
        free(name);
        free(value);
        return 0;
    }
    cf_http_header *nh = realloc(x->res->headers, (x->res->nheaders + 1) * sizeof *nh);
    if (!nh) {
        free(name);
        free(value);
        return 0;
    }
    x->res->headers = nh;
    x->res->headers[x->res->nheaders].name = name;
    x->res->headers[x->res->nheaders].value = value;
    x->res->nheaders++;
    return n;
}

static bool encoding_is_deflate(const char *v) {
    char tmp[32];
    size_t i = 0;
    while (v[i] && i + 1 < sizeof tmp) {
        tmp[i] = (char)tolower((unsigned char)v[i]);
        i++;
    }
    tmp[i] = '\0';
    return !strcmp(tmp, "gzip") || !strcmp(tmp, "x-gzip") || !strcmp(tmp, "deflate");
}

/* Decide decoding once headers are complete (first body byte). */
static bool xfer_begin_body(xfer *x) {
    if (x->header_checked) return true;
    x->header_checked = true;
    if (x->is_head) return true;
    char *enc = cf_http_header_joined(x->res, "content-encoding");
    bool range = cf_http_header_get(x->res, "content-range") != NULL;
    if (enc && !range && encoding_is_deflate(enc)) x->decode = true;
    free(enc);
    /* Content-Length pre-check (reference fetch_document gate). */
    if (x->length_cap > 0) {
        bool ok = false, bad = false;
        uint64_t len = cf_http_content_length(x->res, &ok, &bad);
        if (bad) {
            x->length_bad = true;
            return false;
        }
        if (ok && len > x->length_cap) {
            x->length_abort = true;
            return false;
        }
    }
    return true;
}

static bool xfer_inflate_start(xfer *x, bool gzip) {
    memset(&x->strm, 0, sizeof x->strm);
    int bits = gzip ? 31 : 15;
    if (inflateInit2(&x->strm, bits) != Z_OK) {
        x->inflate_err = true;
        return false;
    }
    x->strm_on = true;
    x->inflated = true;
    x->gzip_mode = gzip;
    return true;
}

/* Feed raw bytes through the decoder (or directly when identity). */
static bool xfer_feed(xfer *x, const unsigned char *p, size_t n) {
    if (!x->decode) return xfer_emit(x, p, n);
    /* Buffer until the 2-byte magic is known (reference waits for 2 bytes). */
    if (!x->inflated && !x->stream_end) {
        while (n > 0 && x->npending < 2) {
            x->pending[x->npending++] = *p++;
            n--;
        }
        if (x->npending < 2) return true;
        if (!xfer_inflate_start(x, x->pending[0] == 0x1f && x->pending[1] == 0x8b)) return false;
        /* Inflate the 2 magic bytes first (they produce no output). */
        unsigned char head[2];
        head[0] = x->pending[0];
        head[1] = x->pending[1];
        x->npending = 0;
        x->strm.next_in = head;
        x->strm.avail_in = 2;
        unsigned char ob0[32];
        x->strm.next_out = ob0;
        x->strm.avail_out = sizeof ob0;
        int z0 = inflate(&x->strm, Z_NO_FLUSH);
        size_t got0 = sizeof ob0 - x->strm.avail_out;
        if (got0 && !xfer_emit(x, ob0, got0)) return false;
        if (z0 == Z_STREAM_END) {
            /* Degenerate 2-byte stream; further bytes start a new member. */
            inflateEnd(&x->strm);
            x->strm_on = false;
            x->inflated = false;
            x->stream_end = true;
        } else if (z0 != Z_OK && z0 != Z_BUF_ERROR) {
            x->inflate_err = true;
            return false;
        }
    }
    if (x->stream_end && x->gzip_mode && !x->inflated) {
        /* Next gzip member (reference MultiGzDecoder). */
        x->stream_end = false;
        if (!xfer_inflate_start(x, true)) return false;
    }
    if (!x->inflated) {
        /* Single zlib stream already ended; trailing bytes ignored. */
        return true;
    }
    x->strm.next_in = (unsigned char *)p;
    x->strm.avail_in = (unsigned int)n;
    /* NOTE: chunks larger than UINT_MAX are not expected from curl. */
    unsigned char ob[32768];
    for (;;) {
        x->strm.next_out = ob;
        x->strm.avail_out = sizeof ob;
        int z = inflate(&x->strm, Z_NO_FLUSH);
        size_t got = sizeof ob - x->strm.avail_out;
        if (got && !xfer_emit(x, ob, got)) return false;
        if (z == Z_STREAM_END) {
            x->stream_end = true;
            if (x->strm.avail_in > 0 && x->gzip_mode) {
                /* Another member follows in this same chunk. */
                const unsigned char *rest = x->strm.next_in;
                size_t rn = x->strm.avail_in;
                inflateEnd(&x->strm);
                x->strm_on = false;
                x->inflated = false;
                x->stream_end = false;
                return xfer_feed(x, rest, rn);
            }
            if (x->gzip_mode) {
                inflateEnd(&x->strm);
                x->strm_on = false;
                x->inflated = false;
            }
            return true;
        }
        if (z == Z_OK || z == Z_BUF_ERROR) {
            if (x->strm.avail_in == 0) return true;
            continue;
        }
        x->inflate_err = true;
        return false;
    }
}

static size_t write_cb(char *ptr, size_t size, size_t nitems, void *ud) {
    xfer *x = ud;
    size_t n = size * nitems;
    x->last_activity_ms = ms_now();
    if (!xfer_begin_body(x)) return 0;
    if (n == 0) return 0;
    if (!xfer_feed(x, (unsigned char *)ptr, n)) {
        if (!x->too_large && !x->inflate_err && !x->length_abort && !x->length_bad) {
            /* OOM in xfer_grow. */
            return 0;
        }
        return 0;
    }
    return n;
}

static int progress_cb(void *ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                       curl_off_t ulnow) {
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    xfer *x = ud;
    /* Read-inactivity deadline on actual transfer activity. curl invokes
     * this during stalls too (observed ~1/s), which its LOW_SPEED check
     * does not reliably cover while waiting for the response head. */
    if (x->read_timeout_ms > 0 && ms_now() - x->last_activity_ms > x->read_timeout_ms) {
        x->read_stall = true;
        return 1;
    }
    return 0;
}

static long ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

cf_http_err cf_http_exchange(const cf_http_config *cfg, const char *method, const char *url,
                             const char *const *req_headers, size_t nreq_headers,
                             const unsigned char *req_body, size_t req_body_len,
                             const char *const *resolve, size_t nresolve,
                             cf_http_response *out) {
    memset(out, 0, sizeof *out);
    if (!cfg || !method || !url) return CF_HTTP_IO;

    cf_http_uri uri;
    cf_http_err perr = cf_http_uri_parse(url, &uri);
    if (perr != CF_HTTP_OK) return perr;
    if (!cf_http_uri_is_http(&uri)) {
        cf_http_uri_dispose(&uri);
        return CF_HTTP_SCHEME;
    }
    if (!uri.host || !*uri.host) {
        cf_http_uri_dispose(&uri);
        return CF_HTTP_INVALID_URL; /* "no host component for URI" */
    }
    if (uri.port == 0 || uri.port > 65535) {
        cf_http_uri_dispose(&uri);
        return CF_HTTP_INVALID_PART; /* "invalid port" */
    }
    bool is_head = !strcmp(method, "HEAD");
    bool is_post = !strcmp(method, "POST");
    if (strcmp(method, "GET") && !is_head && !is_post) {
        cf_http_uri_dispose(&uri);
        return CF_HTTP_IO;
    }

    cf_http_global_init();
    CURL *h = curl_easy_init();
    if (!h) {
        cf_http_uri_dispose(&uri);
        return CF_HTTP_NOMEM;
    }

    xfer x;
    memset(&x, 0, sizeof x);
    x.res = out;
    x.max_bytes = cfg->max_bytes;
    x.length_cap = cfg->length_cap;
    x.max_header_bytes = cfg->max_header_bytes;
    x.is_head = is_head;
    x.read_timeout_ms = cfg->read_timeout_ms;
    x.last_activity_ms = ms_now();

    struct curl_slist *hdrs = NULL;
    struct curl_slist *reslv = NULL;
    cf_http_err rc = CF_HTTP_OK;

    /* Default headers in Net::HTTP order when the caller lacks them. */
    bool has_enc = false, has_accept = false, has_ua = false, has_conn = false, has_ct = false;
    for (size_t i = 0; i < nreq_headers; i++) {
        const char *c = strchr(req_headers[i], ':');
        size_t nl = c ? (size_t)(c - req_headers[i]) : strlen(req_headers[i]);
        if (nl == 15 && !strncasecmp(req_headers[i], "accept-encoding", 15)) has_enc = true;
        else if (nl == 6 && !strncasecmp(req_headers[i], "accept", 6)) has_accept = true;
        else if (nl == 10 && !strncasecmp(req_headers[i], "user-agent", 10)) has_ua = true;
        else if (nl == 10 && !strncasecmp(req_headers[i], "connection", 10)) has_conn = true;
        else if (nl == 12 && !strncasecmp(req_headers[i], "content-type", 12)) has_ct = true;
    }
    for (size_t i = 0; i < nreq_headers; i++) {
        struct curl_slist *t = curl_slist_append(hdrs, req_headers[i]);
        if (!t) {
            rc = CF_HTTP_NOMEM;
            goto done;
        }
        hdrs = t;
    }
    (void)has_ct;
    if (!has_enc && !is_head) {
        char e[128];
        snprintf(e, sizeof e, "Accept-Encoding: %s", CF_HTTP_ACCEPT_ENCODING);
        struct curl_slist *t = curl_slist_append(hdrs, e);
        if (!t) {
            rc = CF_HTTP_NOMEM;
            goto done;
        }
        hdrs = t;
    }
    if (!has_accept) {
        struct curl_slist *t = curl_slist_append(hdrs, "Accept: */*");
        if (!t) {
            rc = CF_HTTP_NOMEM;
            goto done;
        }
        hdrs = t;
    }
    if (!has_ua) {
        struct curl_slist *t = curl_slist_append(hdrs, "User-Agent: Ruby");
        if (!t) {
            rc = CF_HTTP_NOMEM;
            goto done;
        }
        hdrs = t;
    }
    if (!has_conn) {
        struct curl_slist *t = curl_slist_append(hdrs, "Connection: close");
        if (!t) {
            rc = CF_HTTP_NOMEM;
            goto done;
        }
        hdrs = t;
    }
    for (size_t i = 0; i < nresolve; i++) {
        struct curl_slist *t = curl_slist_append(reslv, resolve[i]);
        if (!t) {
            rc = CF_HTTP_NOMEM;
            goto done;
        }
        reslv = t;
    }

    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    if (reslv) {
        curl_easy_setopt(h, CURLOPT_RESOLVE, reslv);
        /* A proxy resolves the target itself and would bypass the caller's
         * checked address. Explicit address pins must use a direct socket. */
        curl_easy_setopt(h, CURLOPT_PROXY, "");
    }
    if (is_head) curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
    if (is_post) {
        curl_easy_setopt(h, CURLOPT_POST, 1L);
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, req_body ? (const char *)req_body : "");
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)req_body_len);
    }
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &x);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &x);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &x);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, cfg->connect_timeout_ms);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, (long)(cfg->read_timeout_ms / 1000 > 1 ? cfg->read_timeout_ms / 1000 : 1));
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, cfg->deadline_ms);
    /* TLS verification is always on; only the trust store is configurable. */
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
    if (cfg->ca_path) curl_easy_setopt(h, CURLOPT_CAINFO, cfg->ca_path);

    long start = ms_now();
    CURLcode cr = curl_easy_perform(h);
    long elapsed = ms_now() - start;
    long sc = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &sc);
    out->status = (unsigned)sc;

    if (cr == CURLE_OK) {
        if (x.too_large || x.length_abort) rc = CF_HTTP_TOO_LARGE;
        else if (x.inflate_err) rc = CF_HTTP_INFLATE;
        else if (x.length_bad) rc = CF_HTTP_BAD_LENGTH;
        else {
            /* Drain a trailing gzip/zlib stream (reference Inflater::finish). */
            if (x.decode && x.inflated) {
                unsigned char ob[32768];
                for (;;) {
                    x.strm.next_out = ob;
                    x.strm.avail_out = sizeof ob;
                    x.strm.next_in = NULL;
                    x.strm.avail_in = 0;
                    int z = inflate(&x.strm, Z_FINISH);
                    size_t got = sizeof ob - x.strm.avail_out;
                    if (got && !xfer_emit(&x, ob, got)) {
                        rc = CF_HTTP_TOO_LARGE;
                        break;
                    }
                    if (z == Z_STREAM_END) break;
                    if (z == Z_OK || z == Z_BUF_ERROR) {
                        if (x.strm.avail_out > 0) break;
                        continue;
                    }
                    /* Trailing garbage after a complete member: stop, keep
                     * what we have (MultiGzDecoder would try another
                     * member; raw trailing bytes fail it too). */
                    break;
                }
            } else if (x.decode && !x.inflated && x.npending > 0) {
                /* Short (<2 byte) encoded body: reference inflates pending
                 * as zlib; emulate by starting the stream now. */
                if (xfer_inflate_start(&x, false)) {
                    x.strm.next_in = x.pending;
                    x.strm.avail_in = (unsigned int)x.npending;
                    unsigned char ob[32768];
                    x.strm.next_out = ob;
                    x.strm.avail_out = sizeof ob;
                    int z = inflate(&x.strm, Z_FINISH);
                    size_t got = sizeof ob - x.strm.avail_out;
                    if (z == Z_STREAM_END || z == Z_OK) {
                        if (got && !xfer_emit(&x, ob, got)) rc = CF_HTTP_TOO_LARGE;
                    } else {
                        rc = CF_HTTP_INFLATE;
                    }
                } else {
                    rc = CF_HTTP_INFLATE;
                }
            }
            if (rc == CF_HTTP_OK) {
                out->body = x.body;
                out->body_len = x.body_len;
                x.body = NULL;
                rc = CF_HTTP_OK;
            }
        }
    } else if (cr == CURLE_WRITE_ERROR) {
        if (x.too_large || x.length_abort) rc = CF_HTTP_TOO_LARGE;
        else if (x.inflate_err) rc = CF_HTTP_INFLATE;
        else if (x.length_bad) rc = CF_HTTP_BAD_LENGTH;
        else rc = CF_HTTP_NOMEM;
    } else if (cr == CURLE_OPERATION_TIMEDOUT) {
        if (elapsed + 100 >= cfg->deadline_ms) rc = CF_HTTP_TIMEOUT_ALL;
        else if (x.head_done) rc = CF_HTTP_TIMEOUT_READ;
        else rc = CF_HTTP_TIMEOUT_CONN;
    } else if (cr == CURLE_ABORTED_BY_CALLBACK) {
        rc = x.read_stall ? CF_HTTP_TIMEOUT_READ : CF_HTTP_IO;
    } else if (cr == CURLE_COULDNT_RESOLVE_HOST) {
        rc = CF_HTTP_DNS;
    } else if (cr == CURLE_COULDNT_CONNECT) {
        rc = CF_HTTP_CONNECT;
    } else if (cr == CURLE_SSL_CONNECT_ERROR || cr == CURLE_SSL_CERTPROBLEM || cr == CURLE_SSL_CACERT ||
               cr == CURLE_SSL_CACERT_BADFILE || cr == CURLE_PEER_FAILED_VERIFICATION ||
               cr == CURLE_SSL_ISSUER_ERROR) {
        rc = CF_HTTP_TLS;
    } else if (cr == CURLE_UNSUPPORTED_PROTOCOL) {
        rc = CF_HTTP_SCHEME;
    } else if (cr == CURLE_URL_MALFORMAT || cr == CURLE_BAD_FUNCTION_ARGUMENT) {
        rc = CF_HTTP_INVALID_URL;
    } else if (cr == CURLE_OUT_OF_MEMORY) {
        rc = CF_HTTP_NOMEM;
    } else {
        rc = CF_HTTP_IO;
    }

done:
    curl_slist_free_all(hdrs);
    curl_slist_free_all(reslv);
    curl_easy_cleanup(h);
    cf_http_uri_dispose(&uri);
    xfer_dispose(&x);
    free(x.body);
    if (rc != CF_HTTP_OK) cf_http_response_dispose(out);
    return rc;
}
