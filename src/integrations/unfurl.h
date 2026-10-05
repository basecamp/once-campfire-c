/* src/integrations/unfurl.h — I01 link unfurling (05-storage-integrations.md I01).
 *
 * Reference: tmp/rust-ref/crates/campfire/src/integrations/opengraph.rs and
 * opengraph/{fetch,location,metadata,document,html,entities}.rs
 * (`UnfurlLinksController#create` over Opengraph::Metadata).
 *
 * Contract (mirrored):
 *  - At most 16 concurrent unfurls; 5 s connect/read inactivity, 10 s
 *    overall; a URL that runs out of time unfurls nothing (204, not an
 *    error page). At most first 256 attributes of a meta tag; 5 MiB
 *    decoded body; at most 10 redirects.
 *  - Every address resolved through the private-network guard and pinned
 *    (CURLOPT_RESOLVE) per hop; every redirect re-checked (any 3xx is a
 *    redirect; target must parse as absolute http(s) and re-resolve
 *    public). No browser/JS, no local-file URLs.
 *  - Documents must be 200 text/html (exact, params/case rejected) within
 *    the size cap, by Content-Length AND by decoded bytes read.
 *  - Tweets resolve through fxtwitter; canonical og:url falls back to the
 *    page URL when invalid; og:image needs a HEAD-verified JPEG/PNG/GIF/
 *    WebP (status ignored, redirects followed+guarded).
 *  - validate() sanitizes title/description (strip_tags + SafeList, via
 *    the R02 rt_dom/rt_sanitize entry points, read-only) and requires
 *    non-blank title+url+description; a non-blank image must re-validate.
 */
#ifndef CF_INTEGRATIONS_UNFURL_H
#define CF_INTEGRATIONS_UNFURL_H

#include <stdbool.h>
#include <stddef.h>

/* Unfurl deadline (10 s) and concurrency (16). */
#define CF_UNFURL_DEADLINE_MS 10000L
#define CF_UNFURL_MAX_CONCURRENT 16
/* Document cap (5 MiB) and redirect cap (10). */
#define CF_UNFURL_MAX_BODY_SIZE ((size_t)5 * 1024 * 1024)
#define CF_UNFURL_MAX_REDIRECTS 10

typedef struct {
    long connect_timeout_ms; /* 5 s per connect */
    long read_timeout_ms;    /* 5 s read inactivity */
    long deadline_ms;        /* 10 s whole unfurl (overrides http deadline) */
    const char *ca_path;     /* NULL = system trust store */
    /* Test-only DNS override: parallel host->IP-literal arrays. When set,
     * names outside the map are unresolvable (no real-DNS fallback). */
    const char *const *test_hosts;
    const char *const *test_addrs;
    size_t ntest;
    /* Test-only: let loopback/private addresses through the guard. */
    bool test_allow_private;
} cf_unfurl_config;

void cf_unfurl_default_config(cf_unfurl_config *c);

typedef enum {
    CF_UNFURL_JSON = 0, /* 200 + JSON body */
    CF_UNFURL_NONE,     /* 204: invalid, unacceptable, oversized, timeout */
    CF_UNFURL_RAISED    /* 500: NoMethodError / URI::InvalidComponentError */
} cf_unfurl_kind;

typedef struct {
    cf_unfurl_kind kind;
    char *json;            /* JSON kind only */
    const char *raised;    /* RAISED kind only ("NoMethodError", ...) */
} cf_unfurl_out;

void cf_unfurl_out_dispose(cf_unfurl_out *o);

/* unfurl(url): exactly one of JSON/NONE/RAISED. Missing/blank input is the
 * controller's 400 and never reaches here. Returns 0 on success (any kind),
 * -1 on resource failure (out untouched). */
int cf_unfurl(const cf_unfurl_config *cfg, const char *url, cf_unfurl_out *out);

/* Guard classification, exposed for tests (surfguard default policy). */
bool cf_unfurl_blocked_ip(const char *ip_literal);
/* Media-URL pre-filter (FILES_AND_MEDIA_URL_REGEX), exposed for tests. */
bool cf_unfurl_is_media_url(const char *url);

#endif /* CF_INTEGRATIONS_UNFURL_H */
