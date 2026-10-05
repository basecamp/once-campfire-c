/* H03 ordered route table, finite matcher and static front mount
 * (01-foundation-http.md "H03: exact route table and assets").
 *
 * Module-private H03 header: the shared contract (cf.h) already declares
 * cf_route_match_request/cf_route_match_dispose; everything else here is
 * consumed by H03's own tests and by the integrator's front-mount wiring.
 * New cross-module API needs the integrator's approval; this header is the
 * proposal (see docs/devel/evidence/H03.md).
 *
 * The 177-row table is generated mechanically from
 * docs/devel/implementation/contracts/routes.json by
 * tests/routes/tools/gen_routes.py (reference commit 64f86353). Rows are in
 * the artifact's exact order; recognition is first-match in that order.
 * Rows whose owning packet has not landed point at cf_action_not_landed_501:
 * a development-only, test-visible 501 (spec 01 H03: final completeness
 * requires zero such registrations). Rebinding a row when its packet lands
 * is route registration and belongs to the integrator: either update the
 * generator's handler map and regenerate, or edit the row's action field.
 */
#ifndef CF_ROUTES_H
#define CF_ROUTES_H

#include "cf.h"

/* ---- ordered table ---------------------------------------------------- */

typedef enum {
    CF_ROUTE_IMPLEMENT = 0,       /* routes.json disposition "implement" */
    CF_ROUTE_REFERENCE_ERROR = 1  /* "reference_error": exact reference 404/500 */
} cf_route_disposition;

typedef struct {
    const char *name;
    const char *value;
} cf_route_default;

typedef struct {
    uint32_t id;                 /* routes.json id, stable in this specification */
    cf_method method;            /* wire verb; HEAD matches a GET row */
    const char *pattern;         /* artifact pattern (finite matcher grammar) */
    const char *endpoint;        /* "controller#action", for provenance/debug */
    const char *c_symbol;        /* artifact's exact C entry-point name */
    const char *task;            /* packet that owns the action */
    cf_route_disposition disposition;
    const cf_route_default *defaults; /* path-parameter defaults from the artifact */
    size_t default_count;
    cf_action_fn action;         /* bound handler; never NULL in the table */
} cf_route;

/* The ordered table and its row count (never NULL; *count == 177). */
const cf_route *cf_routes(size_t *count);

/* First row with `id`, or NULL. */
const cf_route *cf_route_by_id(uint32_t id);

/* ---- H03's bound handlers --------------------------------------------- */

/* 40 action_not_found rows: the reference's public 404 response directly. */
cf_err cf_action_reference_action_not_found(cf_ctx *ctx);
/* The rooms/settings missing_controller row: the reference's public 500. */
cf_err cf_action_reference_missing_controller(cf_ctx *ctx);
/* Rails::HealthController#show for /up(.:format). */
cf_err cf_action_health_show(cf_ctx *ctx);
/* turbo-rails' navigation actions. */
cf_err cf_action_turbo_native_recede(cf_ctx *ctx);
cf_err cf_action_turbo_native_resume(cf_ctx *ctx);
cf_err cf_action_turbo_native_refresh(cf_ctx *ctx);
/* ActionMailbox ingress routes with no ingress configured: 404, no body. */
cf_err cf_action_mailbox_ingress_not_configured(cf_ctx *ctx);
/* GET conductor requests: CSRF is a no-op for safe methods, then 403. */
cf_err cf_action_mailbox_conductor_get(cf_ctx *ctx);
/* POST conductor requests: cf_check_csrf first (422 when it halts), then the
 * same head 403 as the GET rows. */
cf_err cf_action_mailbox_conductor_post(cf_ctx *ctx);

/* Development-only handler for rows whose packet has not landed: a
 * test-visible 501 (status, "text/plain; charset=utf-8", body naming the
 * route id and its future C symbol). Not a completion: the H03 acceptance
 * test pins the exact set of rows still using it. */
cf_err cf_action_not_landed_501(cf_ctx *ctx);

/* ---- static front mount (outside the 177 rows) ------------------------ */

/* Fixture root that holds the pinned `public/` tree (tests/fixtures/assets).
 * Overridable at compile time with -DCF_STATIC_ROOT='"..."' and at
 * boot/test time with cf_static_set_root. The front mount and the reference
 * error pages both resolve through it; it is never read from tmp/. */
#ifndef CF_STATIC_ROOT
#define CF_STATIC_ROOT "tests/fixtures/assets"
#endif

const char *cf_static_root(void);

/* Set the static root before loops start (boot) or in tests. NULL or an
 * empty string resets the compile-time default. The string is copied; the
 * copy is bounded by CF_STATIC_ROOT_MAX. */
#define CF_STATIC_ROOT_MAX 1024
cf_err cf_static_set_root(const char *root);

/* Try to answer one request from the pinned static tree (ActionDispatch::
 * Static / crates/assets/src/serve.rs): digested /assets paths and the
 * served public files, GET/HEAD only, immutable caching by max-age,
 * If-Modified-Since handling, Rack::Mime content types and .br/.gz sibling
 * negotiation for compressible types. This is a front mount outside the 177
 * routes; it runs on request/response only, before A00's context exists.
 *
 * CF_OK and *handled true: response holds the finished static response.
 * CF_OK and *handled false: no static file matched; the caller routes the
 * request through the application (which answers its own 404).
 * Non-OK: internal failure (allocation); the caller maps it like any other
 * resource failure. Never touches the dynamic response cache. */
cf_err cf_assets_serve(const cf_request *request, cf_response *response,
                       bool *handled);

#endif /* CF_ROUTES_H */
