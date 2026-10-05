/* A00 unit tests: context lifecycle, parameter merge order, cookie jar, flash,
 * and dispatch error mapping/cleanup (03-application.md "A00"; 00-contracts.md
 * error translation). The H03 matcher is the test double in
 * tests/app/support/route_double.c. */
#include "cf_test.h"

#include "app.h"
#include "auth.h"
#include "cf.h"
#include "config.h"
#include "context.h"
#include "http/http_internal.h"
#include "http/params.h"

#include "support/route_double.h"
#include "support/test_request.h"

#include <string.h>

#ifndef CF_APP_VERSION
#define CF_APP_VERSION "0"
#endif

#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static cf_app *make_app(void) {
    static int seq;
    char origin[64];
    snprintf(origin, sizeof origin, "http://127.0.0.1:%d", 39000 + seq++);
    cf_config_entry entries[2] = {
        {"PUBLIC_ORIGIN", origin},
        {"SECRET_KEY_BASE", HEX64},
    };
    cf_config *config = NULL;
    CF_REQUIRE(cf_config_parse(entries, 2, NULL, &config) == CF_OK);
    cf_app *app = NULL;
    CF_REQUIRE(cf_app_create(config, &app) == CF_OK);
    return app;
}

static int g_calls;
static cf_err g_action_rc;
static unsigned g_action_status;
static bool g_action_body;
static unsigned g_action_error_status;
static char g_capture[4][48];

static void capture(cf_ctx *ctx, const char *name, char *out, size_t cap) {
    out[0] = '\0';
    cf_span key = {(const unsigned char *)name, strlen(name)};
    const cf_param *param = cf_ctx_param(ctx, key);
    cf_span value;
    if (param != NULL && cf_param_string(param, &value) == CF_OK) {
        size_t n = value.len < cap - 1 ? value.len : cap - 1;
        memcpy(out, value.ptr, n);
        out[n] = '\0';
    }
}

static cf_err action_probe(cf_ctx *ctx) {
    g_calls++;
    capture(ctx, "id", g_capture[0], sizeof g_capture[0]);
    capture(ctx, "b", g_capture[1], sizeof g_capture[1]);
    capture(ctx, "x", g_capture[2], sizeof g_capture[2]);
    capture(ctx, "a", g_capture[3], sizeof g_capture[3]);
    if (g_action_error_status != 0) {
        cf_ctx_set_error_status(ctx, g_action_error_status);
    }
    if (g_action_body) {
        cf_buf *body = NULL;
        CF_REQUIRE(cf_buf_copy(CF_TEST_SPAN("body"), &body) == CF_OK);
        CF_REQUIRE(cf_response_body(ctx->response, body) == CF_OK);
        cf_buf_release(body);
        CF_REQUIRE(cf_response_header(
                       ctx->response, CF_TEST_SPAN("Content-Type"),
                       CF_TEST_SPAN("text/plain")) == CF_OK);
    }
    ctx->response->status = g_action_status;
    return g_action_rc;
}

static void reset_action(void) {
    g_calls = 0;
    g_action_rc = CF_OK;
    g_action_status = 200;
    g_action_body = false;
    g_action_error_status = 0;
    memset(g_capture, 0, sizeof g_capture);
}

static cf_err run(cf_app *app, cf_request *req, cf_response *resp) {
    return cf_ctx_process(app, NULL, req, resp);
}

CF_TEST(ctx_merges_body_query_and_path_params) {
    reset_action();
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/rooms/:id", 11, action_probe) ==
               CF_OK);
    cf_app *app = make_app();

    cf_request req;
    cf_test_req_init(&req);
    req.path = CF_TEST_SPAN("/rooms/9");
    req.query = CF_TEST_SPAN("b=2&id=5");
    req.body = CF_TEST_SPAN("b=body&x=1");
    CF_REQUIRE(cf_test_req_header(&req, CF_TEST_SPAN("Content-Type"),
                                  CF_TEST_SPAN(
                                      "application/x-www-form-urlencoded")) ==
               CF_OK);

    cf_response resp;
    CF_CHECK(run(app, &req, &resp) == CF_OK);
    CF_CHECK(g_calls == 1);
    CF_CHECK(resp.status == 200);
    /* Query replaces the body value in place; the path capture wins last. */
    CF_CHECK(strcmp(g_capture[0], "9") == 0);
    CF_CHECK(strcmp(g_capture[1], "2") == 0);
    CF_CHECK(strcmp(g_capture[2], "1") == 0);
    cf_response_dispose(&resp);
    cf_app_destroy(app);
}

CF_TEST(ctx_path_param_percent_decoded_and_plus_literal) {
    reset_action();
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/rooms/:id", 15, action_probe) ==
               CF_OK);
    cf_app *app = make_app();
    cf_request req;
    cf_test_req_init(&req);
    req.path = CF_TEST_SPAN("/rooms/a%2Fb+c");
    cf_response resp;
    CF_CHECK(run(app, &req, &resp) == CF_OK);
    CF_CHECK(resp.status == 200);
    CF_CHECK(strcmp(g_capture[0], "a/b+c") == 0); /* '+' literal in paths */
    cf_response_dispose(&resp);

    /* Bad UTF-8 in a path capture is the H03 400 case (CF_INVALID from the
     * matcher). */
    reset_action();
    req.path = CF_TEST_SPAN("/rooms/%FF");
    CF_CHECK(run(app, &req, &resp) == CF_OK);
    CF_CHECK(resp.status == 400);
    CF_CHECK(g_calls == 0);
    cf_response_dispose(&resp);
    cf_app_destroy(app);
}

CF_TEST(ctx_unmatched_route_is_404) {
    reset_action();
    cf_test_routes_reset();
    cf_app *app = make_app();
    cf_request req;
    cf_test_req_init(&req);
    req.path = CF_TEST_SPAN("/nowhere");

    cf_response resp;
    CF_CHECK(run(app, &req, &resp) == CF_OK);
    CF_CHECK(resp.status == 404);
    CF_CHECK(g_calls == 0);
    cf_response_dispose(&resp);
    cf_app_destroy(app);
}

CF_TEST(ctx_route_without_action_is_404) {
    reset_action();
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/no-action", 12, NULL) == CF_OK);
    cf_app *app = make_app();
    cf_request req;
    cf_test_req_init(&req);
    req.path = CF_TEST_SPAN("/no-action");

    cf_response resp;
    CF_CHECK(run(app, &req, &resp) == CF_OK);
    CF_CHECK(resp.status == 404);
    CF_CHECK(g_calls == 0);
    cf_response_dispose(&resp);
    cf_app_destroy(app);
}

/* A matched handler that returns CF_NOT_FOUND must answer the same reference
 * 404 as the unmatched-route path (cf_finish_mapped delegates to
 * cf_action_reference_action_not_found, src/context.c): the reference renders
 * every action-level 404 through exceptions::render, and the 40
 * reference_error rows already bind that handler. The route double's
 * stand-in renders no body by design (route_double.c), so this case pins the
 * equivalence and the ownership; tests/app/test_dispatch_wiring.c links the
 * real routes.c, where the body bytes are public/404.html. */
static cf_err action_not_found(cf_ctx *ctx) {
    g_calls++;
    g_action_body = true; /* a partial response must be disposed */
    ctx->response->status = 200;
    return CF_NOT_FOUND;
}

CF_TEST(ctx_handler_not_found_uses_the_reference_404_path) {
    reset_action();
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/not-found", 25, action_not_found) ==
               CF_OK);
    cf_app *app = make_app();

    cf_request req;
    cf_test_req_init(&req);
    req.path = CF_TEST_SPAN("/not-found");
    cf_response handler_resp;
    CF_CHECK(run(app, &req, &handler_resp) == CF_OK);
    CF_CHECK(handler_resp.status == 404);
    CF_CHECK(g_calls == 1);

    cf_request req2;
    cf_test_req_init(&req2);
    req2.path = CF_TEST_SPAN("/unmatched");
    cf_response unmatched_resp;
    CF_CHECK(run(app, &req2, &unmatched_resp) == CF_OK);
    CF_CHECK(unmatched_resp.status == 404);

    /* Byte-equal bodies on the two paths. */
    cf_span handler_body = cf_buf_span(handler_resp.body);
    cf_span unmatched_body = cf_buf_span(unmatched_resp.body);
    CF_CHECK(handler_body.len == unmatched_body.len);
    CF_CHECK(handler_body.len == 0 ||
             memcmp(handler_body.ptr, unmatched_body.ptr, handler_body.len) ==
                 0);

    cf_response_dispose(&handler_resp);
    cf_response_dispose(&unmatched_resp);
    cf_app_destroy(app);
}

CF_TEST(ctx_malformed_params_are_400) {
    reset_action();
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("POST", "/", 13, action_probe) == CF_OK);
    cf_app *app = make_app();

    cf_request req;
    cf_test_req_init(&req);
    req.method = CF_POST;
    req.path = CF_TEST_SPAN("/");
    req.body = CF_TEST_SPAN("{ this is not json");
    CF_REQUIRE(cf_test_req_header(&req, CF_TEST_SPAN("Content-Type"),
                                  CF_TEST_SPAN("application/json")) == CF_OK);

    cf_response resp;
    CF_CHECK(run(app, &req, &resp) == CF_OK);
    CF_CHECK(resp.status == 400);
    CF_CHECK(g_calls == 0); /* malformed params never reach the action */
    cf_response_dispose(&resp);
    cf_app_destroy(app);
}

/* ---- Rack method override (cf_effective_method wiring) ------------------ */

static cf_method g_override_method;
static cf_method g_override_original;
static uint32_t g_override_route;
static bool g_override_csrf_halted;
static char g_override_param[32];
static char g_override_x[32];

static void override_span_text(cf_span value, char *out, size_t cap) {
    size_t n = value.len < cap - 1 ? value.len : cap - 1;
    if (n > 0 && value.ptr != NULL) memcpy(out, value.ptr, n);
    out[n] = '\0';
}

static void override_capture_param(cf_ctx *ctx, const char *name, char *out,
                                   size_t cap) {
    out[0] = '\0';
    const cf_param *param = cf_ctx_param(
        ctx, (cf_span){(const unsigned char *)name, strlen(name)});
    cf_span value;
    if (param != NULL && cf_param_string(param, &value) == CF_OK) {
        override_span_text(value, out, cap);
    }
}

/* Records what the action observes the way a real controller before-action
 * chain would: the effective verb, the wire verb, the matched route and the
 * CSRF outcome for the effective verb. */
static cf_err action_method_probe(cf_ctx *ctx) {
    g_calls++;
    g_override_method = ctx->request->method;
    g_override_original = ctx->request->original_method;
    g_override_route = ctx->route.id;
    override_capture_param(ctx, "_method", g_override_param,
                           sizeof g_override_param);
    override_capture_param(ctx, "x", g_override_x, sizeof g_override_x);
    cf_err rc = cf_check_csrf(ctx, false);
    g_override_csrf_halted = rc == CF_OK && cf_auth_halted(ctx);
    if (rc != CF_OK || g_override_csrf_halted) return rc;
    ctx->response->status = 200;
    return CF_OK;
}

/* One POST /rows/7 built to the case's shape. */
static void override_request(cf_request *req, cf_method method,
                             const char *body, const char *content_type,
                             const char *header_name, const char *header_value,
                             const char *sec_fetch_site) {
    cf_test_req_init(req);
    req->method = method;
    req->original_method = method;
    req->path = CF_TEST_SPAN("/rows/7");
    req->target = CF_TEST_SPAN("/rows/7");
    req->body = (cf_span){(const unsigned char *)body, strlen(body)};
    if (content_type != NULL) {
        CF_REQUIRE(cf_test_req_header(
                       req, CF_TEST_SPAN("Content-Type"),
                       (cf_span){(const unsigned char *)content_type,
                                 strlen(content_type)}) == CF_OK);
    }
    if (header_name != NULL) {
        CF_REQUIRE(cf_test_req_header(
                       req,
                       (cf_span){(const unsigned char *)header_name,
                                 strlen(header_name)},
                       (cf_span){(const unsigned char *)header_value,
                                 strlen(header_value)}) == CF_OK);
    }
    if (sec_fetch_site != NULL) {
        CF_REQUIRE(cf_test_req_header(
                       req, CF_TEST_SPAN("Sec-Fetch-Site"),
                       (cf_span){(const unsigned char *)sec_fetch_site,
                                 strlen(sec_fetch_site)}) == CF_OK);
    }
}

/* The context carries a shallow copy with the effective verb; the caller's
 * request and every span stay untouched by cf_ctx_create (the copy borrows
 * them). The only write-back is cf_ctx_process's effective-HEAD record, which
 * H01's serializer needs; creation itself stays pure. */
CF_TEST(ctx_override_copy_keeps_the_wire_request_intact) {
    reset_action();
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("POST", "/rows/:id", 21,
                                  action_method_probe) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("PATCH", "/rows/:id", 23,
                                  action_method_probe) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("GET", "/rows/:id", 25,
                                  action_method_probe) == CF_OK);
    cf_app *app = make_app();

    cf_request req;
    override_request(&req, CF_POST, "_method=patch&x=1",
                     "application/x-www-form-urlencoded", NULL, NULL,
                     "same-origin");
    cf_response resp;
    cf_response_init(&resp);
    cf_ctx ctx;
    CF_REQUIRE(cf_ctx_create(&ctx, app, NULL, &req, &resp) == CF_OK);
    CF_CHECK(ctx.request != &req); /* the context owns the override copy */
    CF_CHECK(ctx.request->method == CF_PATCH);
    CF_CHECK(ctx.request->original_method == CF_POST);
    CF_CHECK(ctx.route.id == 23);
    CF_CHECK(req.method == CF_POST); /* cf_ctx_create never mutates it */
    CF_CHECK(req.original_method == CF_POST);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);

    /* Effective HEAD is the same copy, and cf_ctx_create still leaves the
     * caller's request untouched; cf_ctx_process records it for H01. */
    cf_request head_req;
    override_request(&head_req, CF_POST, "_method=HEAD",
                     "application/x-www-form-urlencoded", NULL, NULL,
                     "same-origin");
    cf_response head_resp;
    cf_response_init(&head_resp);
    cf_ctx head_ctx;
    CF_REQUIRE(cf_ctx_create(&head_ctx, app, NULL, &head_req, &head_resp) ==
               CF_OK);
    CF_CHECK(head_ctx.request != &head_req);
    CF_CHECK(head_ctx.request->method == CF_HEAD);
    CF_CHECK(head_ctx.route.id == 25); /* HEAD matches the GET row */
    CF_CHECK(head_req.method == CF_POST);
    cf_ctx_destroy(&head_ctx);
    cf_response_dispose(&head_resp);

    cf_app_destroy(app);
}

/* POST + a urlencoded `_method` field routes by the effective verb; the
 * action, CSRF and the error rendering all see it while the body/query
 * parameter tree itself is unaffected. */
CF_TEST(ctx_post_method_override_routes_by_the_effective_verb) {
    reset_action();
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("POST", "/rows/:id", 21,
                                  action_method_probe) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("PUT", "/rows/:id", 22,
                                  action_method_probe) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("PATCH", "/rows/:id", 23,
                                  action_method_probe) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("DELETE", "/rows/:id", 24,
                                  action_method_probe) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("GET", "/rows/:id", 25,
                                  action_method_probe) == CF_OK);
    cf_app *app = make_app();

    static const struct {
        const char *body;
        cf_method want;
        uint32_t route;
    } cases[] = {
        {"_method=PATCH&x=1", CF_PATCH, 23},
        {"_method=put&x=1", CF_PUT, 22},
        {"_method=delete&x=1", CF_DELETE, 24},
        {"_method=HEAD&x=1", CF_HEAD, 25}, /* HEAD matches the GET row */
        {"_method=GET&x=1", CF_GET, 25},
        {"_method=POST&x=1", CF_POST, 21},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        reset_action();
        cf_request req;
        override_request(&req, CF_POST, cases[i].body,
                         "application/x-www-form-urlencoded", NULL, NULL,
                         "same-origin");
        cf_response resp;
        CF_CHECK(run(app, &req, &resp) == CF_OK);
        CF_CHECK(resp.status == 200);
        CF_CHECK(g_calls == 1);
        CF_CHECK(g_override_route == cases[i].route);
        CF_CHECK(g_override_method == cases[i].want);
        CF_CHECK(g_override_original == CF_POST);
        CF_CHECK(strcmp(g_override_x, "1") == 0); /* params unaffected */
        /* The task request keeps the wire verb, except that cf_ctx_process
         * records an effective HEAD for H01's serializer (cf_ctx_process
         * comment; the wire method stays in original_method). */
        CF_CHECK(req.method ==
                 (cases[i].want == CF_HEAD ? CF_HEAD : CF_POST));
        CF_CHECK(req.original_method == CF_POST);
        cf_response_dispose(&resp);
    }
    cf_app_destroy(app);
}

/* Header fallback and media contract, mirroring the helper's pinned matrix:
 * the header applies to any POST body; the form field wins when usable; a
 * present-but-unusable `_method` suppresses the header; malformed form data
 * is a bad request before routing; the query string never supplies it. */
CF_TEST(ctx_method_override_header_media_and_failure_contract) {
    reset_action();
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("POST", "/rows/:id", 21,
                                  action_method_probe) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("PUT", "/rows/:id", 22,
                                  action_method_probe) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("PATCH", "/rows/:id", 23,
                                  action_method_probe) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("DELETE", "/rows/:id", 24,
                                  action_method_probe) == CF_OK);
    cf_app *app = make_app();

    struct {
        const char *body;
        const char *content_type;
        const char *header; /* "X-HTTP-Method-Override: <value>" or NULL */
        const char *query;
        cf_method want;
        uint32_t route;
    } cases[] = {
        /* header applies to any POST, including a JSON body */
        {"{\"x\":1}", "application/json", "patch", NULL, CF_PATCH, 23},
        /* header value is uppercased before the fixed-list check */
        {"{\"x\":1}", "application/json", "delete", NULL, CF_DELETE, 24},
        /* no content type is form data (Rack's form_data? on POST) */
        {"_method=put&x=1", NULL, NULL, NULL, CF_PUT, 22},
        /* the header is the fallback when the form has no `_method` */
        {"x=1", "application/x-www-form-urlencoded", "DELETE", NULL,
         CF_DELETE, 24},
        /* a present but unusable field suppresses the header */
        {"_method=TRACE&x=1", "application/x-www-form-urlencoded", "DELETE",
         NULL, CF_POST, 21},
        /* a non-string field is not a candidate: the header applies */
        {"_method[]=PUT&x=1", "application/x-www-form-urlencoded", "DELETE",
         NULL, CF_DELETE, 24},
        /* non-form bodies never supply the field */
        {"_method=DELETE&x=1", "text/plain", NULL, NULL, CF_POST, 21},
        /* the query string never supplies it */
        {"x=1", "application/x-www-form-urlencoded", NULL, "_method=DELETE",
         CF_POST, 21},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        reset_action();
        char header_name[64] = "", header_value[64] = "";
        const char *name = NULL, *value = NULL;
        if (cases[i].header != NULL) {
            snprintf(header_name, sizeof header_name,
                     "X-HTTP-Method-Override");
            snprintf(header_value, sizeof header_value, "%s",
                     cases[i].header);
            name = header_name;
            value = header_value;
        }
        cf_request req;
        override_request(&req, CF_POST, cases[i].body, cases[i].content_type,
                         name, value, "same-origin");
        if (cases[i].query != NULL) {
            req.query = (cf_span){(const unsigned char *)cases[i].query,
                                  strlen(cases[i].query)};
        }
        cf_response resp;
        CF_CHECK(run(app, &req, &resp) == CF_OK);
        CF_CHECK(resp.status == 200);
        CF_CHECK(g_calls == 1);
        CF_CHECK(g_override_route == cases[i].route);
        CF_CHECK(g_override_method == cases[i].want);
        cf_response_dispose(&resp);
    }

    /* Malformed form data is the helper's CF_INVALID: a 400 before routing,
     * as adapter.rs's method_override answers its body parse error. */
    reset_action();
    cf_request bad;
    override_request(&bad, CF_POST, "a=%zz",
                     "application/x-www-form-urlencoded", NULL, NULL, NULL);
    cf_response bad_resp;
    CF_CHECK(run(app, &bad, &bad_resp) == CF_OK);
    CF_CHECK(bad_resp.status == 400);
    CF_CHECK(g_calls == 0);
    cf_response_dispose(&bad_resp);

    cf_app_destroy(app);
}

/* Never turns a non-POST request into anything: body and header are ignored
 * on GET/PUT. */
CF_TEST(ctx_non_post_method_override_is_ignored) {
    reset_action();
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/rows/:id", 25,
                                  action_method_probe) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("PUT", "/rows/:id", 22,
                                  action_method_probe) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("DELETE", "/rows/:id", 24,
                                  action_method_probe) == CF_OK);
    cf_app *app = make_app();

    struct {
        cf_method method;
        const char *body;
        const char *header;
        cf_method want;
        uint32_t route;
    } cases[] = {
        {CF_GET, "_method=DELETE", NULL, CF_GET, 25},
        {CF_GET, "_method=DELETE", "DELETE", CF_GET, 25},
        {CF_PUT, "_method=DELETE", NULL, CF_PUT, 22},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        reset_action();
        cf_request req;
        override_request(&req, cases[i].method, cases[i].body,
                         "application/x-www-form-urlencoded",
                         cases[i].header != NULL
                             ? "X-HTTP-Method-Override"
                             : NULL,
                         cases[i].header != NULL ? cases[i].header : NULL,
                         "same-origin");
        cf_response resp;
        CF_CHECK(run(app, &req, &resp) == CF_OK);
        CF_CHECK(resp.status == 200);
        CF_CHECK(g_calls == 1);
        CF_CHECK(g_override_route == cases[i].route);
        CF_CHECK(g_override_method == cases[i].want);
        cf_response_dispose(&resp);
    }
    cf_app_destroy(app);
}

/* CSRF follows the effective verb: an overridden unsafe verb runs the
 * unsafe-method path, an overridden safe verb skips it. */
CF_TEST(ctx_csrf_uses_the_effective_method) {
    reset_action();
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("POST", "/rows/:id", 21,
                                  action_method_probe) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("DELETE", "/rows/:id", 24,
                                  action_method_probe) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("GET", "/rows/:id", 25,
                                  action_method_probe) == CF_OK);
    cf_app *app = make_app();

    struct {
        cf_method method;
        const char *body;
        const char *content_type;
        cf_method want;
        uint32_t route;
        unsigned status;
        bool halted;
    } cases[] = {
        /* POST overridden to DELETE is unsafe: cross-site is rejected */
        {CF_POST, "_method=delete", "application/x-www-form-urlencoded",
         CF_DELETE, 24, 422, true},
        /* POST overridden to GET is safe: CSRF is a no-op */
        {CF_POST, "_method=GET", "application/x-www-form-urlencoded", CF_GET,
         25, 200, false},
        /* control: the wire verb alone decides for non-overridden requests */
        {CF_GET, "_method=DELETE", "application/x-www-form-urlencoded", CF_GET,
         25, 200, false},
        {CF_DELETE, "", NULL, CF_DELETE, 24, 422, true},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        reset_action();
        cf_request req;
        override_request(&req, cases[i].method, cases[i].body,
                         cases[i].content_type, NULL, NULL, "cross-site");
        cf_response resp;
        CF_CHECK(run(app, &req, &resp) == CF_OK);
        CF_CHECK(g_calls == 1);
        CF_CHECK(g_override_route == cases[i].route);
        CF_CHECK(g_override_method == cases[i].want);
        CF_CHECK(g_override_csrf_halted == cases[i].halted);
        CF_CHECK(resp.status == cases[i].status);
        cf_response_dispose(&resp);
    }
    cf_app_destroy(app);
}

/* Multipart bodies go through the form parser: a text `_method` part
 * overrides, a body without one stays POST, and a malformed multipart body
 * is the helper's CF_INVALID (400 before routing). */
CF_TEST(ctx_multipart_method_override_uses_the_form_parser) {
    reset_action();
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("POST", "/rows/:id", 21,
                                  action_method_probe) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("PATCH", "/rows/:id", 23,
                                  action_method_probe) == CF_OK);
    cf_app *app = make_app();

    static const char with_method[] =
        "--B\r\nContent-Disposition: form-data; name=\"_method\"\r\n\r\n"
        "patch\r\n"
        "--B\r\nContent-Disposition: form-data; name=\"x\"\r\n\r\n1\r\n"
        "--B--\r\n";
    cf_request req;
    override_request(&req, CF_POST, with_method,
                     "multipart/form-data; boundary=B", NULL, NULL,
                     "same-origin");
    cf_response resp;
    CF_CHECK(run(app, &req, &resp) == CF_OK);
    CF_CHECK(resp.status == 200);
    CF_CHECK(g_calls == 1);
    CF_CHECK(g_override_route == 23);
    CF_CHECK(g_override_method == CF_PATCH);
    CF_CHECK(strcmp(g_override_param, "patch") == 0);
    CF_CHECK(strcmp(g_override_x, "1") == 0);
    cf_response_dispose(&resp);

    static const char without_method[] =
        "--B\r\nContent-Disposition: form-data; name=\"x\"\r\n\r\n1\r\n"
        "--B--\r\n";
    reset_action();
    cf_request plain;
    override_request(&plain, CF_POST, without_method,
                     "multipart/form-data; boundary=B", NULL, NULL,
                     "same-origin");
    cf_response plain_resp;
    CF_CHECK(run(app, &plain, &plain_resp) == CF_OK);
    CF_CHECK(plain_resp.status == 200);
    CF_CHECK(g_calls == 1);
    CF_CHECK(g_override_route == 21);
    CF_CHECK(g_override_method == CF_POST);
    cf_response_dispose(&plain_resp);

    /* Missing blank line before the value: H02's malformed multipart. */
    static const char malformed[] =
        "--B\r\nContent-Disposition: form-data; name=\"x\"\r\nvalue\r\n"
        "--B--\r\n";
    reset_action();
    cf_request bad;
    override_request(&bad, CF_POST, malformed,
                     "multipart/form-data; boundary=B", NULL, NULL, NULL);
    cf_response bad_resp;
    CF_CHECK(run(app, &bad, &bad_resp) == CF_OK);
    CF_CHECK(bad_resp.status == 400);
    CF_CHECK(g_calls == 0);
    cf_response_dispose(&bad_resp);

    cf_app_destroy(app);
}

CF_TEST(ctx_destroy_is_safe_and_zeroes) {
    reset_action();
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/", 14, action_probe) == CF_OK);
    cf_app *app = make_app();
    cf_request req;
    cf_test_req_init(&req);

    cf_response resp;
    cf_response_init(&resp);
    cf_ctx ctx;
    CF_REQUIRE(cf_ctx_create(&ctx, app, NULL, &req, &resp) == CF_OK);
    CF_CHECK(ctx.private_state != NULL);
    cf_ctx_destroy(&ctx);
    CF_CHECK(ctx.private_state == NULL);
    CF_CHECK(ctx.app == NULL && ctx.request == NULL);
    cf_ctx_destroy(&ctx); /* second destroy is a no-op */
    cf_ctx_destroy(NULL);
    cf_response_dispose(&resp);
    cf_app_destroy(app);
}

CF_TEST(ctx_generic_error_mapping) {
    static const struct {
        cf_err rc;
        unsigned status;
    } cases[] = {
        {CF_NOT_FOUND, 404}, {CF_FORBIDDEN, 403}, {CF_INVALID, 400},
        {CF_LIMIT, 400},     {CF_INTERNAL, 500},  {CF_DB, 500},
        {CF_IO, 500},        {CF_NOMEM, 500},
    };
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/", 20, action_probe) == CF_OK);
    cf_app *app = make_app();
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        reset_action();
        g_action_rc = cases[i].rc;
        g_action_status = 200;
        g_action_body = true; /* must be disposed and replaced */
        cf_request req;
        cf_test_req_init(&req);
        cf_response resp;
        CF_CHECK(run(app, &req, &resp) == CF_OK);
        CF_CHECK(resp.status == cases[i].status);
        CF_CHECK(resp.body == NULL);
        CF_CHECK(resp.body_kind == CF_BODY_NONE);
        cf_response_dispose(&resp);
    }
    cf_app_destroy(app);
}

CF_TEST(ctx_explicit_error_status_wins) {
    reset_action();
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("POST", "/", 21, action_probe) == CF_OK);
    cf_app *app = make_app();
    g_action_rc = CF_INVALID;
    g_action_error_status = 422; /* cf_check_csrf's mapping, e.g. */
    cf_request req;
    cf_test_req_init(&req);
    req.method = CF_POST;
    cf_response resp;
    CF_CHECK(run(app, &req, &resp) == CF_OK);
    CF_CHECK(resp.status == 422);
    cf_response_dispose(&resp);
    cf_app_destroy(app);
}

/* A failing action's body/headers are disposed exactly once: our retained
 * reference survives, the response is reset, and a later dispose is clean
 * (ASan/LSan cover double frees and leaks). */
CF_TEST(ctx_error_disposes_action_response_once) {
    reset_action();
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/", 22, action_probe) == CF_OK);
    cf_app *app = make_app();

    cf_buf *body = NULL;
    CF_REQUIRE(cf_buf_copy(CF_TEST_SPAN("payload"), &body) == CF_OK);
    /* The action installs a second reference through g_action_body; capture
     * one ourselves to prove it is released exactly once. */
    g_action_rc = CF_INTERNAL;
    g_action_body = true;
    cf_request req;
    cf_test_req_init(&req);
    cf_response resp;
    CF_CHECK(run(app, &req, &resp) == CF_OK);
    CF_CHECK(resp.status == 500);
    CF_CHECK(resp.body == NULL && resp.headers == NULL);
    CF_CHECK(cf_buf_span(body).len == 7);
    cf_response_dispose(&resp);
    cf_buf_release(body);
    cf_app_destroy(app);
}

CF_TEST(cookie_jar_reads_request_cookies) {
    reset_action();
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/", 30, action_probe) == CF_OK);
    cf_app *app = make_app();

    cf_request req;
    cf_test_req_init(&req);
    CF_REQUIRE(cf_test_req_header(&req, CF_TEST_SPAN("Cookie"),
                                  CF_TEST_SPAN("a=1; b=x%20y+z;c=3; a=2; d")) ==
               CF_OK);
    cf_response resp;
    cf_response_init(&resp);
    cf_ctx ctx;
    CF_REQUIRE(cf_ctx_create(&ctx, app, NULL, &req, &resp) == CF_OK);

    cf_span value;
    CF_CHECK(cf_ctx_cookie_get(&ctx, CF_TEST_SPAN("a"), &value) == CF_OK);
    CF_CHECK(value.len == 1 && value.ptr[0] == '1'); /* first wins */
    CF_CHECK(cf_ctx_cookie_get(&ctx, CF_TEST_SPAN("b"), &value) == CF_OK);
    CF_CHECK(value.len == 5 && memcmp(value.ptr, "x y z", 5) == 0);
    CF_CHECK(cf_ctx_cookie_get(&ctx, CF_TEST_SPAN("c"), &value) == CF_OK);
    CF_CHECK(value.len == 1 && value.ptr[0] == '3');
    CF_CHECK(cf_ctx_cookie_get(&ctx, CF_TEST_SPAN("d"), &value) == CF_OK);
    CF_CHECK(value.len == 0);
    CF_CHECK(cf_ctx_cookie_get(&ctx, CF_TEST_SPAN("zz"), &value) ==
             CF_NOT_FOUND);

    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);
    cf_app_destroy(app);
}

/* kit adapter.rs builds the jar from
 * `get_all(COOKIE).iter().filter_map(|v| v.to_str().ok())`: a Cookie header
 * whose bytes are unreadable under HeaderValue::to_str is skipped whole, and
 * later Cookie headers are still parsed. The first *readable* occurrence of a
 * name wins. Obs-text is the only unreadable class H01 admits, so that is the
 * class this test drives; control bytes (VT/FF and friends) are rejected at
 * the protocol layer and are exercised raw by the welcome/presenter
 * `last_room` ISSPACE vectors instead. */
CF_TEST(cookie_jar_skips_unreadable_cookie_headers) {
    reset_action();
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/", 33, action_probe) == CF_OK);
    cf_app *app = make_app();

    struct {
        const char *label;
        const unsigned char *bytes;
        size_t len;
    } unreadable[] = {
        {"obs-text UTF-8", (const unsigned char *)"a=1\xc3\xa9",
         sizeof "a=1\xc3\xa9" - 1},
        {"invalid UTF-8", (const unsigned char *)"a=1\xff",
         sizeof "a=1\xff" - 1},
    };
    for (size_t i = 0; i < sizeof unreadable / sizeof unreadable[0]; i++) {
        cf_request req;
        cf_test_req_init(&req);
        CF_REQUIRE(cf_test_req_header(
                       &req, CF_TEST_SPAN("Cookie"),
                       (cf_span){unreadable[i].bytes,
                                 unreadable[i].len}) == CF_OK);
        CF_REQUIRE(cf_test_req_header(&req, CF_TEST_SPAN("Cookie"),
                                      CF_TEST_SPAN("a=2")) == CF_OK);
        cf_response resp;
        cf_response_init(&resp);
        cf_ctx ctx;
        CF_REQUIRE(cf_ctx_create(&ctx, app, NULL, &req, &resp) == CF_OK);
        cf_span value;
        bool ok = cf_ctx_cookie_get(&ctx, CF_TEST_SPAN("a"), &value) == CF_OK;
        if (!ok || value.len != 1 || value.ptr[0] != '2') {
            printf("    %s: expected a=2, got %.2s (ok=%d)\n",
                   unreadable[i].label,
                   ok ? (const char *)value.ptr : "??", (int)ok);
            CF_CHECK(0);
        }
        cf_ctx_destroy(&ctx);
        cf_response_dispose(&resp);
    }

    /* An unreadable Cookie header alone contributes nothing at all. */
    cf_request req;
    cf_test_req_init(&req);
    CF_REQUIRE(cf_test_req_header(
                   &req, CF_TEST_SPAN("Cookie"),
                   (cf_span){unreadable[0].bytes, unreadable[0].len}) == CF_OK);
    cf_response resp;
    cf_response_init(&resp);
    cf_ctx ctx;
    CF_REQUIRE(cf_ctx_create(&ctx, app, NULL, &req, &resp) == CF_OK);
    CF_CHECK(cf_ctx_cookie_get(&ctx, CF_TEST_SPAN("a"), &(cf_span){0}) ==
             CF_NOT_FOUND);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);

    /* HTAB is readable (to_str, not ASCII-only): the value still parses. */
    cf_test_req_init(&req);
    CF_REQUIRE(cf_test_req_header(&req, CF_TEST_SPAN("Cookie"),
                                  CF_TEST_SPAN("a=1\t")) == CF_OK);
    cf_response_init(&resp);
    CF_REQUIRE(cf_ctx_create(&ctx, app, NULL, &req, &resp) == CF_OK);
    cf_span value;
    CF_CHECK(cf_ctx_cookie_get(&ctx, CF_TEST_SPAN("a"), &value) == CF_OK);
    CF_CHECK(value.len == 2 && value.ptr[0] == '1' && value.ptr[1] == '\t');
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);
    cf_app_destroy(app);
}

/* `_set_cookie_headers` (kit ctx.rs:615) asks `request.host()` for the
 * ".onion" special case, and `host()` reads `request.header("host")` through
 * to_str: an unreadable Host is absent, so a secure cookie is suppressed
 * exactly as for a non-onion host. The readable ".onion" control still
 * emits it. */
CF_TEST(cookie_jar_host_onion_check_reads_through_the_gate) {
    reset_action();
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/", 34, action_probe) == CF_OK);
    cf_app *app = make_app();
    const cf_cookie_options secure = {.secure = true};

    struct {
        const char *label;
        const unsigned char *host;
        size_t len;
        bool emitted;
    } hosts[] = {
        {"readable onion", (const unsigned char *)"x.onion",
         sizeof "x.onion" - 1, true},
        {"unreadable before .onion",
         (const unsigned char *)"\xc3\xa9x.onion",
         sizeof "\xc3\xa9x.onion" - 1, false},
        {"unreadable after .onion", (const unsigned char *)"x.onion\x7f",
         sizeof "x.onion\x7f" - 1, false},
        {"plain host", (const unsigned char *)"example.test",
         sizeof "example.test" - 1, false},
    };
    for (size_t i = 0; i < sizeof hosts / sizeof hosts[0]; i++) {
        cf_request req;
        cf_test_req_init(&req);
        CF_REQUIRE(cf_test_req_header(
                       &req, CF_TEST_SPAN("Host"),
                       (cf_span){hosts[i].host, hosts[i].len}) == CF_OK);
        cf_response resp;
        cf_response_init(&resp);
        cf_ctx ctx;
        CF_REQUIRE(cf_ctx_create(&ctx, app, NULL, &req, &resp) == CF_OK);
        CF_REQUIRE(cf_ctx_cookie_set(&ctx, CF_TEST_SPAN("s"),
                                     CF_TEST_SPAN("1"), &secure) == CF_OK);
        CF_REQUIRE(cf_finish_cookies(&ctx) == CF_OK);
        bool emitted = resp.headers != NULL;
        if (emitted != hosts[i].emitted) {
            printf("    host %s: expected emitted=%d got %d\n",
                   hosts[i].label, (int)hosts[i].emitted, (int)emitted);
            CF_CHECK(0);
        }
        cf_ctx_destroy(&ctx);
        cf_response_dispose(&resp);
    }
    cf_app_destroy(app);
}

/* Negotiation reads Accept / Content-Type / X-Requested-With through
 * `request.header()`, so the to_str gate applies: an unreadable value is the
 * reference's None.  Accept = None and Content-Type = None drop the value
 * from the negotiation input; X-Requested-With = None makes `is_xhr` false.
 * The observable outcomes: the documented fallback (HTML, or JS for a
 * readable XHR) with no Vary: Accept, and -- for a readable XHR plus an
 * unreadable Content-Type -- no 406 from parsing the raw bytes. */
CF_TEST(negotiation_reads_unreadable_headers_as_absent) {
    reset_action();
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/", 35, action_probe) == CF_OK);
    cf_app *app = make_app();

    struct {
        const char *label;
        const unsigned char *accept;
        size_t accept_len;
        const unsigned char *accept2;
        size_t accept2_len;
        const unsigned char *xhr;
        size_t xhr_len;
        const unsigned char *ct;
        size_t ct_len;
        const char *symbols;
        bool vary;
    } cases[] = {
        {"readable accept", (const unsigned char *)"application/json",
         sizeof "application/json" - 1, NULL, 0, NULL, 0, NULL, 0, "json",
         true},
        {"obs-text accept", (const unsigned char *)"application/json\xc3\xa9",
         sizeof "application/json\xc3\xa9" - 1, NULL, 0, NULL, 0, NULL, 0,
         "html", false},
        {"DEL accept", (const unsigned char *)"application/json\x7f",
         sizeof "application/json\x7f" - 1, NULL, 0, NULL, 0, NULL, 0,
         "html", false},
        {"control accept", (const unsigned char *)"application/json\x01",
         sizeof "application/json\x01" - 1, NULL, 0, NULL, 0, NULL, 0,
         "html", false},
        {"NUL accept", (const unsigned char *)"application/json\0",
         sizeof "application/json\0" - 1, NULL, 0, NULL, 0, NULL, 0, "html",
         false},
        {"unreadable first accept hides readable second",
         (const unsigned char *)"application/json\xc3\xa9",
         sizeof "application/json\xc3\xa9" - 1,
         (const unsigned char *)"application/json",
         sizeof "application/json" - 1, NULL, 0, NULL, 0, "html", false},
        {"readable first accept hides unreadable second",
         (const unsigned char *)"application/json",
         sizeof "application/json" - 1,
         (const unsigned char *)"application/json\xc3\xa9",
         sizeof "application/json\xc3\xa9" - 1, NULL, 0, NULL, 0, "json",
         true},
        {"obs-text xhr", NULL, 0, NULL, 0,
         (const unsigned char *)"XMLHttpRequest\xc3\xa9",
         sizeof "XMLHttpRequest\xc3\xa9" - 1, NULL, 0, "html", false},
        {"DEL xhr", NULL, 0, NULL, 0, (const unsigned char *)"XMLHttpRequest\x7f",
         sizeof "XMLHttpRequest\x7f" - 1, NULL, 0, "html", false},
        {"readable xhr without accept", NULL, 0, NULL, 0,
         (const unsigned char *)"XMLHttpRequest",
         sizeof "XMLHttpRequest" - 1, NULL, 0, "js", false},
        {"xhr plus obs-text content-type", NULL, 0, NULL, 0,
         (const unsigned char *)"XMLHttpRequest",
         sizeof "XMLHttpRequest" - 1,
         (const unsigned char *)"application/json\xc3\xa9",
         sizeof "application/json\xc3\xa9" - 1, "js", false},
        {"xhr plus readable content-type", NULL, 0, NULL, 0,
         (const unsigned char *)"XMLHttpRequest",
         sizeof "XMLHttpRequest" - 1,
         (const unsigned char *)"application/json",
         sizeof "application/json" - 1, "json", true},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        cf_request req;
        cf_test_req_init(&req);
        if (cases[i].accept != NULL) {
            CF_REQUIRE(cf_test_req_header(
                           &req, CF_TEST_SPAN("Accept"),
                           (cf_span){cases[i].accept,
                                     cases[i].accept_len}) == CF_OK);
        }
        if (cases[i].accept2 != NULL) {
            CF_REQUIRE(cf_test_req_header(
                           &req, CF_TEST_SPAN("Accept"),
                           (cf_span){cases[i].accept2,
                                     cases[i].accept2_len}) == CF_OK);
        }
        if (cases[i].xhr != NULL) {
            CF_REQUIRE(cf_test_req_header(
                           &req, CF_TEST_SPAN("X-Requested-With"),
                           (cf_span){cases[i].xhr, cases[i].xhr_len}) ==
                       CF_OK);
        }
        if (cases[i].ct != NULL) {
            CF_REQUIRE(cf_test_req_header(
                           &req, CF_TEST_SPAN("Content-Type"),
                           (cf_span){cases[i].ct, cases[i].ct_len}) == CF_OK);
        }
        cf_response resp;
        cf_response_init(&resp);
        cf_ctx ctx;
        CF_REQUIRE(cf_ctx_create(&ctx, app, NULL, &req, &resp) == CF_OK);
        const cf_format **list = NULL;
        size_t count = 0;
        cf_err rc = cf_ctx_formats(&ctx, &list, &count);
        char symbols[64] = "";
        if (rc == CF_OK) {
            for (size_t k = 0; k < count; k++) {
                if (k > 0) strncat(symbols, ",", sizeof symbols - strlen(symbols) - 1);
                strncat(symbols, list[k]->symbol,
                        sizeof symbols - strlen(symbols) - 1);
            }
        }
        if (rc != CF_OK || strcmp(symbols, cases[i].symbols) != 0 ||
            cf_ctx_vary_accept(&ctx) != cases[i].vary) {
            printf("    %s: rc=%d formats=\"%s\" expected \"%s\" vary=%d/%d\n",
                   cases[i].label, (int)rc, symbols, cases[i].symbols,
                   (int)cf_ctx_vary_accept(&ctx), (int)cases[i].vary);
            CF_CHECK(0);
        }
        cf_ctx_destroy(&ctx);
        cf_response_dispose(&resp);
    }
    cf_app_destroy(app);
}

CF_TEST(cookie_jar_malformed_escape_keeps_raw) {
    reset_action();
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/", 31, action_probe) == CF_OK);
    cf_app *app = make_app();
    cf_request req;
    cf_test_req_init(&req);
    CF_REQUIRE(cf_test_req_header(&req, CF_TEST_SPAN("Cookie"),
                                  CF_TEST_SPAN("bad=%zz")) == CF_OK);
    cf_response resp;
    cf_response_init(&resp);
    cf_ctx ctx;
    CF_REQUIRE(cf_ctx_create(&ctx, app, NULL, &req, &resp) == CF_OK);
    cf_span value;
    CF_CHECK(cf_ctx_cookie_get(&ctx, CF_TEST_SPAN("bad"), &value) == CF_OK);
    CF_CHECK(value.len == 3 && memcmp(value.ptr, "%zz", 3) == 0);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);
    cf_app_destroy(app);
}

/* parse_cookie_header (kit cookies.rs:269-289) is byte-exact: the first part
 * is never trimmed, later parts lose only leading 0x20, a part without '='
 * is an occurrence with an empty value, empty parts contribute nothing, and
 * a malformed escape or a non-UTF-8 percent decode keeps the raw value. The
 * HTTP jar already followed these rules; this matrix pins them against the
 * cable lookup, which is aligned to the same parser. */
CF_TEST(cookie_jar_parse_is_pin_exact) {
    reset_action();
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/", 36, action_probe) == CF_OK);
    cf_app *app = make_app();

    struct {
        const char *label;
        const char *h1;
        const char *h2; /* NULL = no second Cookie header */
        const char *name;
        const char *want; /* NULL = CF_NOT_FOUND */
        size_t want_len;
    } cases[] = {
        {"bare blocks a later duplicate", "a; a=b", NULL, "a", "", 0},
        {"a value blocks a later bare name", "a=b; a", NULL, "a", "b", 1},
        {"bare header blocks a later header", "a", "a=b", "a", "", 0},
        {"value header blocks a later bare header", "a=b", "a", "a", "b", 1},
        {"name space is another name", "a =b", NULL, "a", NULL, 0},
        {"name-space pair skipped, later pair wins", "a =b; a=c", NULL, "a",
         "c", 1},
        {"name-space header skipped, later header wins", "a =b", "a=c", "a",
         "c", 1},
        {"leading space on the first part is kept", " a=b", NULL, "a", NULL,
         0},
        {"leading-space first part skipped", " a=b; a=c", NULL, "a", "c", 1},
        {"later part trims leading spaces", "x=1; a=b", NULL, "a", "b", 1},
        {"later part trims two leading spaces", "x=1;  a=b", NULL, "a", "b",
         1},
        {"value keeps the leading space", "a= b", NULL, "a", " b", 2},
        {"value keeps the trailing space", "a=b ", NULL, "a", "b ", 2},
        {"value keeps the leading HTAB", "a=\tb", NULL, "a", "\tb", 2},
        {"value keeps the trailing HTAB", "a=b\t", NULL, "a", "b\t", 2},
        {"name keeps the trailing HTAB", "a\t=b", NULL, "a", NULL, 0},
        {"empty part then trimmed part", "; a=b", NULL, "a", "b", 1},
        {"bare name after a semicolon", "x=1; a", NULL, "a", "", 0},
        {"malformed percent keeps raw", "bad=%", NULL, "bad", "%", 1},
        {"non-UTF-8 percent keeps raw", "bad=%FF", NULL, "bad", "%FF", 3},
        {"percent decodes UTF-8", "u=%C3%A9", NULL, "u", "\xc3\xa9", 2},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        cf_request req;
        cf_test_req_init(&req);
        CF_REQUIRE(cf_test_req_header(
                       &req, CF_TEST_SPAN("Cookie"),
                       (cf_span){(const unsigned char *)cases[i].h1,
                                 strlen(cases[i].h1)}) == CF_OK);
        if (cases[i].h2 != NULL) {
            CF_REQUIRE(cf_test_req_header(
                           &req, CF_TEST_SPAN("Cookie"),
                           (cf_span){(const unsigned char *)cases[i].h2,
                                     strlen(cases[i].h2)}) == CF_OK);
        }
        cf_response resp;
        cf_response_init(&resp);
        cf_ctx ctx;
        CF_REQUIRE(cf_ctx_create(&ctx, app, NULL, &req, &resp) == CF_OK);
        cf_span name = {(const unsigned char *)cases[i].name,
                        strlen(cases[i].name)};
        cf_span value = {0};
        cf_err rc = cf_ctx_cookie_get(&ctx, name, &value);
        bool ok = rc == CF_OK;
        bool want_ok = cases[i].want != NULL;
        if (ok != want_ok ||
            (want_ok &&
             (value.len != cases[i].want_len ||
              memcmp(value.ptr, cases[i].want, cases[i].want_len) != 0))) {
            fprintf(stderr, "    %s: rc=%d len=%zu\n", cases[i].label, (int)rc,
                    ok ? value.len : (size_t)0);
            CF_CHECK(0);
        } else {
            CF_CHECK(1);
        }
        cf_ctx_destroy(&ctx);
        cf_response_dispose(&resp);
    }
    cf_app_destroy(app);
}

CF_TEST(cookie_jar_set_and_delete_state) {
    reset_action();
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/", 32, action_probe) == CF_OK);
    cf_app *app = make_app();
    cf_request req;
    cf_test_req_init(&req);
    CF_REQUIRE(cf_test_req_header(&req, CF_TEST_SPAN("Cookie"),
                                  CF_TEST_SPAN("session_token=abc")) == CF_OK);
    cf_response resp;
    cf_response_init(&resp);
    cf_ctx ctx;
    CF_REQUIRE(cf_ctx_create(&ctx, app, NULL, &req, &resp) == CF_OK);

    /* unchanged value: no-op, jar keeps the request value */
    CF_CHECK(cf_ctx_cookie_set(&ctx, CF_TEST_SPAN("session_token"),
                               CF_TEST_SPAN("abc"), NULL) == CF_OK);
    cf_span value;
    CF_CHECK(cf_ctx_cookie_get(&ctx, CF_TEST_SPAN("session_token"), &value) ==
             CF_OK);
    CF_CHECK(value.len == 3 && memcmp(value.ptr, "abc", 3) == 0);
    /* changed value */
    CF_CHECK(cf_ctx_cookie_set(&ctx, CF_TEST_SPAN("last_room"),
                               CF_TEST_SPAN("42"), NULL) == CF_OK);
    CF_CHECK(cf_ctx_cookie_get(&ctx, CF_TEST_SPAN("last_room"), &value) ==
             CF_OK);
    /* delete present -> absent; delete absent is a no-op */
    CF_CHECK(cf_ctx_cookie_delete(&ctx, CF_TEST_SPAN("session_token"), NULL) ==
             CF_OK);
    CF_CHECK(cf_ctx_cookie_get(&ctx, CF_TEST_SPAN("session_token"), &value) ==
             CF_NOT_FOUND);
    CF_CHECK(cf_ctx_cookie_delete(&ctx, CF_TEST_SPAN("session_token"), NULL) ==
             CF_OK);
    /* invalid arguments */
    CF_CHECK(cf_ctx_cookie_set(&ctx, (cf_span){NULL, 0}, CF_TEST_SPAN("x"),
                               NULL) == CF_INVALID);
    CF_CHECK(cf_ctx_cookie_set(&ctx, CF_TEST_SPAN("x"), CF_TEST_SPAN("v"),
                               NULL) == CF_OK);
    /* finish flushes the queue and is idempotent */
    CF_CHECK(cf_finish_cookies(&ctx) == CF_OK);
    CF_CHECK(cf_finish_cookies(&ctx) == CF_OK);

    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);
    cf_app_destroy(app);
}

CF_TEST(flash_set_get_replace) {
    reset_action();
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/", 33, action_probe) == CF_OK);
    cf_app *app = make_app();
    cf_request req;
    cf_test_req_init(&req);
    cf_response resp;
    cf_response_init(&resp);
    cf_ctx ctx;
    CF_REQUIRE(cf_ctx_create(&ctx, app, NULL, &req, &resp) == CF_OK);

    cf_span value;
    CF_CHECK(cf_ctx_flash_get(&ctx, CF_TEST_SPAN("notice"), &value) ==
             CF_NOT_FOUND);
    CF_CHECK(cf_ctx_flash_set(&ctx, CF_TEST_SPAN("notice"),
                              CF_TEST_SPAN("hello")) == CF_OK);
    CF_CHECK(cf_ctx_flash_get(&ctx, CF_TEST_SPAN("notice"), &value) == CF_OK);
    CF_CHECK(value.len == 5 && memcmp(value.ptr, "hello", 5) == 0);
    CF_CHECK(cf_ctx_flash_set(&ctx, CF_TEST_SPAN("notice"),
                              CF_TEST_SPAN("bye")) == CF_OK);
    CF_CHECK(cf_ctx_flash_get(&ctx, CF_TEST_SPAN("notice"), &value) == CF_OK);
    CF_CHECK(value.len == 3 && memcmp(value.ptr, "bye", 3) == 0);
    CF_CHECK(cf_ctx_flash_set(&ctx, CF_TEST_SPAN("alert"),
                              CF_TEST_SPAN("careful")) == CF_OK);
    CF_CHECK(cf_ctx_flash_get(&ctx, CF_TEST_SPAN("alert"), &value) == CF_OK);
    CF_CHECK(cf_ctx_flash_set(&ctx, (cf_span){NULL, 0}, CF_TEST_SPAN("x")) ==
             CF_INVALID);

    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);
    cf_app_destroy(app);
}

CF_TEST(dispatch_ok_response_ownership) {
    reset_action();
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/", 34, action_probe) == CF_OK);
    cf_app *app = make_app();
    g_action_body = true;
    cf_request req;
    cf_test_req_init(&req);
    cf_response resp;
    CF_CHECK(run(app, &req, &resp) == CF_OK);
    CF_CHECK(resp.status == 200);
    cf_span body = cf_buf_span(resp.body);
    CF_CHECK(body.len == 4 && memcmp(body.ptr, "body", 4) == 0);
    cf_response_dispose(&resp); /* caller disposes the owned response once */
    cf_app_destroy(app);
}

/* -------------------------------------------- A01 r2 (F4): mapped headers */

/* Observe one response header through H01's serializer (cf_headers is
 * opaque): the line must start a line in the serialized header block. */
static bool resp_header_line(const cf_response *resp, const cf_request *req,
                             const char *line) {
    cf_http_serialized out;
    if (cf_http_response_serialize(resp, req, &out) != CF_OK) return false;
    cf_span headers = cf_buf_span(out.headers);
    size_t n = strlen(line);
    bool found = false;
    for (size_t i = 0; i + n <= headers.len && !found; i++) {
        if (i != 0 && headers.ptr[i - 1] != '\n') continue;
        if (memcmp(headers.ptr + i, line, n) == 0) found = true;
    }
    if (out.headers != NULL) cf_buf_release(out.headers);
    return found;
}

/* Run the real before chain (step 1 queues X-Version/X-Rev on the response),
 * then answer like the dispatch-mapped paths: g_action_status plus
 * g_action_rc, with an optional explicit error status. */
static cf_err action_chain_then(cf_ctx *ctx) {
    g_calls++;
    cf_before policy = {CF_AUTH_SKIPPED, false, false};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;
    if (g_action_error_status != 0) {
        cf_ctx_set_error_status(ctx, g_action_error_status);
    }
    ctx->response->status = g_action_status;
    return g_action_rc;
}

/* The dev-501 shape (src/routes.c cf_action_not_landed_501): a direct receipt
 * that never ran the chain. */
static cf_err action_not_landed_shape(cf_ctx *ctx) {
    g_calls++;
    ctx->response->status = 501;
    return cf_response_header(ctx->response, CF_TEST_SPAN("Content-Type"),
                              CF_TEST_SPAN("text/plain; charset=utf-8"));
}

/* The reference drops the before chain's headers on an error response:
 * kit adapter.rs `dispatch` hands `Err(error)` to `Ctx::finish`, which goes
 * straight to `error_response` (ErrorPages::render) without merging the
 * `Ctx::headers` map; only Ok/Halt responses merge it.  An unmatched route
 * (adapter.rs `not_found`) never ran the chain.  So: mapped 404, negotiation
 * 406 and the dev 501 carry neither X-Version nor X-Rev, while a successful
 * action through the same chain carries both. */
CF_TEST(mapped_errors_carry_no_before_chain_headers) {
    reset_action();
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/mapped", 40, action_chain_then) ==
               CF_OK);
    CF_REQUIRE(cf_test_routes_add("GET", "/mapped406", 41, action_chain_then) ==
               CF_OK);
    CF_REQUIRE(cf_test_routes_add("GET", "/dev501", 42,
                                  action_not_landed_shape) == CF_OK);
    cf_app *app = make_app();

    struct {
        const char *path;
        cf_err rc;
        unsigned error_status;
        unsigned action_status;
        unsigned status;
    } cases[] = {
        {"/mapped", CF_NOT_FOUND, 0, 200, 404},
        {"/mapped406", CF_INVALID, 406, 200, 406}, /* cf_ctx_respond_to */
        {"/dev501", CF_OK, 0, 501, 501},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        reset_action();
        g_action_rc = cases[i].rc;
        g_action_error_status = cases[i].error_status;
        g_action_status = cases[i].action_status;
        cf_request req;
        cf_test_req_init(&req);
        req.path = (cf_span){(const unsigned char *)cases[i].path,
                             strlen(cases[i].path)};
        cf_response resp;
        CF_CHECK(run(app, &req, &resp) == CF_OK);
        CF_CHECK(resp.status == cases[i].status);
        CF_CHECK(!resp_header_line(&resp, &req, "X-Version: "));
#ifdef CF_GIT_REVISION
        CF_CHECK(!resp_header_line(&resp, &req, "X-Rev: "));
#endif
        cf_response_dispose(&resp);
    }

    /* Control: the same chain + a successful action keeps the headers. */
    reset_action();
    cf_request req;
    cf_test_req_init(&req);
    req.path = CF_TEST_SPAN("/mapped");
    g_action_status = 200;
    cf_response resp;
    CF_CHECK(run(app, &req, &resp) == CF_OK);
    CF_CHECK(resp.status == 200);
    CF_CHECK(resp_header_line(&resp, &req, "X-Version: " CF_APP_VERSION));
#ifdef CF_GIT_REVISION
    CF_CHECK(resp_header_line(&resp, &req, "X-Rev: " CF_GIT_REVISION));
#endif
    cf_response_dispose(&resp);

    /* Unmatched route: the chain never ran, so there is nothing to carry. */
    reset_action();
    cf_request unrouted;
    cf_test_req_init(&unrouted);
    unrouted.path = CF_TEST_SPAN("/nowhere-at-all");
    cf_response unmatched;
    CF_CHECK(run(app, &unrouted, &unmatched) == CF_OK);
    CF_CHECK(unmatched.status == 404);
    CF_CHECK(g_calls == 0);
    CF_CHECK(!resp_header_line(&unmatched, &unrouted, "X-Version: "));
#ifdef CF_GIT_REVISION
    CF_CHECK(!resp_header_line(&unmatched, &unrouted, "X-Rev: "));
#endif
    cf_response_dispose(&unmatched);

    cf_app_destroy(app);
}

CF_TEST_MAIN()
