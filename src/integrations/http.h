/* src/integrations/http.h — I01 outbound HTTP over libcurl (05-storage-integrations.md I01).
 *
 * Reference: tmp/rust-ref/crates/campfire/src/integrations/net/http.rs
 * (Net::HTTP-shaped exchange) and the URI slice in
 * tmp/rust-ref/crates/richtext/src/uri.rs used by opengraph/webhook.
 *
 * Contract:
 *  - Schemes http/https only, enforced before curl runs AND via
 *    CURLOPT_PROTOCOLS/REDIR_PROTOCOLS. Never disable TLS/host verification
 *    (no VERIFYPEER/VERIFYHOST overrides anywhere); local test servers use
 *    an explicit CA file plus CURLOPT_RESOLVE pinning (injected origin).
 *  - No automatic redirect following (FOLLOWLOCATION=0): unfurl follows
 *    manually per the reference (any 3xx, re-resolved and re-guarded);
 *    webhook posts once and never follows.
 *  - Accept-Encoding is sent manually and bodies are inflated in this file
 *    with zlib (windowBits auto-detect, gzip multi-member aware like
 *    MultiGzDecoder), so decoded-byte caps hold even against the Fil-C
 *    libcurl built --without-zlib, which cannot decode content itself.
 *    Caps and deadlines are enforced on actual received/decompressed bytes
 *    in curl callbacks, never on Content-Length alone.
 *  - No automatic retry of anything; one easy handle per exchange.
 */
#ifndef CF_INTEGRATIONS_HTTP_H
#define CF_INTEGRATIONS_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CF_HTTP_OK = 0,
    CF_HTTP_NOMEM,
    CF_HTTP_INVALID_URL,   /* unparsable URL (URI::InvalidURIError) */
    CF_HTTP_INVALID_PART,  /* parsable but rejected component (InvalidComponent, bad port...) */
    CF_HTTP_SCHEME,        /* not http/https */
    CF_HTTP_DNS,           /* unresolvable host */
    CF_HTTP_CONNECT,       /* TCP/connect failure (incl. refused) */
    CF_HTTP_TLS,           /* handshake/verification failure */
    CF_HTTP_TIMEOUT_CONN,  /* connect (open) timeout */
    CF_HTTP_TIMEOUT_READ,  /* read inactivity timeout */
    CF_HTTP_TIMEOUT_ALL,   /* overall deadline exceeded */
    CF_HTTP_TOO_LARGE,     /* decoded body (or Content-Length) passed the cap */
    CF_HTTP_BAD_LENGTH,    /* malformed Content-Length where one was required */
    CF_HTTP_INFLATE,       /* content-decoding failure */
    CF_HTTP_IO             /* other transport failure */
} cf_http_err;

const char *cf_http_err_name(cf_http_err e);

/* Parsed URL (subset of Ruby URI used by the integrations). All strings are
 * owned NUL-terminated copies; port is explicit-or-default for known schemes
 * (http/ws 80, https/wss 443, ftp 21, ldap 389, ldaps 636), 0 when unknown. */
typedef struct {
    char *scheme;    /* as written, may be NULL (relative) */
    char *userinfo;  /* may be NULL */
    char *host;      /* as written (IPv6 literals keep brackets), may be NULL */
    uint64_t port;   /* 0 when no known default and none given */
    char *path;      /* "" when absent, may be NULL for ftp/ldap without path */
    char *opaque;    /* non-hierarchical (mailto:) form, else NULL */
    char *query;     /* may be NULL */
    char *fragment;  /* may be NULL */
} cf_http_uri;

cf_http_err cf_http_uri_parse(const char *url, cf_http_uri *out);
void cf_http_uri_dispose(cf_http_uri *u);
bool cf_http_uri_is_http(const cf_http_uri *u);
/* Path+query request target, at least "/". Caller frees. NULL on OOM. */
char *cf_http_uri_target(const cf_http_uri *u);
/* Host header value (bare host, brackets stripped, :port unless default).
 * Caller frees. NULL on OOM. https_hint selects the default port. */
char *cf_http_host_header(const char *host, uint64_t port, bool https);

typedef struct {
    long connect_timeout_ms; /* connect incl. TLS handshake */
    long read_timeout_ms;    /* inactivity between bytes (low-speed) */
    long deadline_ms;        /* overall exchange */
    size_t max_bytes;        /* decoded body cap enforced in callbacks */
    size_t length_cap;       /* 0 = none; abort early when Content-Length exceeds this */
    const char *ca_path;     /* NULL = system default trust store */
} cf_http_config;

typedef struct {
    char *name;
    char *value;
} cf_http_header;

typedef struct {
    unsigned status;
    cf_http_header *headers;
    size_t nheaders;
    unsigned char *body;
    size_t body_len;
} cf_http_response;

void cf_http_response_dispose(cf_http_response *r);
/* First header value (case-insensitive name), or NULL. */
const char *cf_http_header_get(const cf_http_response *r, const char *name);
/* Joined ", " value like net/http.rs Response::header. buf/len scratch. */
char *cf_http_header_joined(const cf_http_response *r, const char *name);
/* Media type before any ';', parts stripped, NOT downcased (reference
 * content_type). NULL when absent. Caller frees. */
char *cf_http_content_type(const cf_http_response *r);
/* First run of digits in the joined Content-Length; ok=false when absent,
 * bad=true when no digits ("wrong Content-Length format"). Overflow clamps
 * to UINT64_MAX like the reference. */
uint64_t cf_http_content_length(const cf_http_response *r, bool *ok, bool *bad);

/* One exchange (GET, HEAD or POST). req_headers are "Name: value" lines
 * (defaults Accept/Accept-Encoding/User-Agent/Connection added when absent;
 * Host and Content-Length come from curl). req_body/req_body_len is the POST
 * body (NULL/0 for GET/HEAD). resolve entries are "HOST:PORT:IP" pins
 * (NULL/0 = system resolution). No redirect following, no retries.
 * Out is empty unless CF_HTTP_OK (or CF_HTTP_TOO_LARGE with partial state
 * discarded: out stays empty on any failure). */
cf_http_err cf_http_exchange(const cf_http_config *cfg, const char *method,
                             const char *url,
                             const char *const *req_headers, size_t nreq_headers,
                             const unsigned char *req_body, size_t req_body_len,
                             const char *const *resolve, size_t nresolve,
                             cf_http_response *out);

/* Process-wide libcurl init (idempotent, thread-safe). */
void cf_http_global_init(void);

/* Net::HTTP Accept-Encoding value sent on body-carrying requests. */
#define CF_HTTP_ACCEPT_ENCODING "gzip;q=1.0,deflate;q=0.6,identity;q=0.3"

#endif /* CF_INTEGRATIONS_HTTP_H */
