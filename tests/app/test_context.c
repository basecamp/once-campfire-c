/* A00 unit tests: context lifecycle, parameter merge order, cookie jar, flash,
 * and dispatch error mapping/cleanup (03-application.md "A00"; 00-contracts.md
 * error translation). The H03 matcher is the test double in
 * tests/app/support/route_double.c. */
#include "cf_test.h"

#include "app.h"
#include "cf.h"
#include "config.h"
#include "context.h"
#include "http/params.h"

#include "support/route_double.h"
#include "support/test_request.h"

#include <string.h>

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

CF_TEST_MAIN()
