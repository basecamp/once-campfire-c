/* A00 format-negotiation tests: the cases of
 * tmp/rust-ref/crates/kit/src/format.rs (and ctx.rs::respond_to) ported to the
 * C context surface. */
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

struct neg {
    const char *accept;       /* NULL = no Accept header */
    const char *path;
    const char *format_param; /* ?format= */
    const char *content_type; /* NULL = none */
    bool xhr;
};

struct fmt_env {
    cf_app *app;
    cf_ctx ctx;
    cf_response resp;
    cf_request req;
    char storage[4][256];
};

static void env_init(struct fmt_env *e, const struct neg *in) {
    static int seq;
    char origin[64];
    snprintf(origin, sizeof origin, "http://127.0.0.1:%d", 41000 + seq++);
    cf_config_entry entries[2] = {
        {"PUBLIC_ORIGIN", origin},
        {"SECRET_KEY_BASE", HEX64},
    };
    cf_config *config = NULL;
    CF_REQUIRE(cf_config_parse(entries, 2, NULL, &config) == CF_OK);
    CF_REQUIRE(cf_app_create(config, &e->app) == CF_OK);

    /* Catch-all routes so cf_ctx_create's match step succeeds for any path
     * the negotiation cases use; the actions are never called here. */
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/", 1, NULL) == CF_OK);
    CF_REQUIRE(cf_test_routes_add("GET", "*path", 2, NULL) == CF_OK);

    cf_test_req_init(&e->req);
    snprintf(e->storage[0], sizeof e->storage[0], "%s",
             in->path != NULL ? in->path : "/");
    e->req.path = (cf_span){(const unsigned char *)e->storage[0],
                            strlen(e->storage[0])};
    if (in->format_param != NULL) {
        snprintf(e->storage[1], sizeof e->storage[1], "format=%s",
                 in->format_param);
        e->req.query = (cf_span){(const unsigned char *)e->storage[1],
                                 strlen(e->storage[1])};
    }
    if (in->accept != NULL) {
        CF_REQUIRE(cf_test_req_header(
                       &e->req, CF_TEST_SPAN("Accept"),
                       (cf_span){(const unsigned char *)in->accept,
                                 strlen(in->accept)}) == CF_OK);
    }
    if (in->content_type != NULL) {
        CF_REQUIRE(cf_test_req_header(
                       &e->req, CF_TEST_SPAN("Content-Type"),
                       (cf_span){(const unsigned char *)in->content_type,
                                 strlen(in->content_type)}) == CF_OK);
    }
    if (in->xhr) {
        CF_REQUIRE(cf_test_req_header(&e->req,
                                      CF_TEST_SPAN("X-Requested-With"),
                                      CF_TEST_SPAN("XMLHttpRequest")) ==
                   CF_OK);
    }
    cf_response_init(&e->resp);
    CF_REQUIRE(cf_ctx_create(&e->ctx, e->app, NULL, &e->req, &e->resp) ==
               CF_OK);
}

static void env_dispose(struct fmt_env *e) {
    cf_ctx_destroy(&e->ctx);
    cf_response_dispose(&e->resp);
    cf_app_destroy(e->app);
}

/* Join the negotiated symbols with commas. */
static void env_symbols(struct fmt_env *e, char *out, size_t cap) {
    const cf_format **list = NULL;
    size_t count = 0;
    cf_err rc = cf_ctx_formats(&e->ctx, &list, &count);
    CF_REQUIRE(rc == CF_OK);
    out[0] = '\0';
    for (size_t i = 0; i < count; i++) {
        if (i > 0) strncat(out, ",", cap - strlen(out) - 1);
        strncat(out, list[i]->symbol, cap - strlen(out) - 1);
    }
}

static void expect_formats(const struct neg *in, const char *expected) {
    struct fmt_env e;
    env_init(&e, in);
    char symbols[256];
    env_symbols(&e, symbols, sizeof symbols);
    CF_CHECK(strcmp(symbols, expected) == 0);
    env_dispose(&e);
}

CF_TEST(browser_accept_falls_back_to_html) {
    const char *chrome =
        "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,"
        "image/webp,*/*;q=0.8";
    expect_formats(
        &(struct neg){.accept = chrome, .path = "/rooms/1"}, "html");
    expect_formats(&(struct neg){.path = "/rooms/1"}, "html");
    expect_formats(&(struct neg){.accept = "*/*", .path = "/rooms/1"}, "*/*");
}

CF_TEST(missing_or_empty_accept_uses_documented_fallback) {
    /* kit/src/format.rs: an absent (or empty) Accept header falls back to
     * the path extension, else HTML (JS for XHR); it must never read the
     * uninitialized span (A00-verify §8: valgrind uninitialized read, TSan
     * SEGV at tests/app/test_formats.c). */
    expect_formats(&(struct neg){.path = "/"}, "html");
    expect_formats(&(struct neg){.path = "/messages.json"}, "json");
    expect_formats(&(struct neg){.accept = "", .path = "/"}, "html");
    expect_formats(
        &(struct neg){.accept = "", .path = "/messages.json"}, "json");
    expect_formats(&(struct neg){.path = "/", .xhr = true}, "js");
    expect_formats(
        &(struct neg){.path = "/", .content_type = "application/json"}, "html");

    /* The Vary: Accept claim stays false without a present, nonempty
     * Accept header. */
    struct fmt_env e;
    env_init(&e, &(struct neg){.path = "/"});
    CF_CHECK(cf_ctx_vary_accept(&e.ctx) == false);
    env_dispose(&e);
    env_init(&e, &(struct neg){.accept = "", .path = "/"});
    CF_CHECK(cf_ctx_vary_accept(&e.ctx) == false);
    env_dispose(&e);
}

CF_TEST(turbo_form_submission) {
    const char *turbo =
        "text/vnd.turbo-stream.html, text/html, application/xhtml+xml";
    expect_formats(&(struct neg){.accept = turbo, .path = "/rooms/1/messages"},
                   "turbo_stream,html");
}

CF_TEST(quality_ordering) {
    expect_formats(&(struct neg){.accept = "text/html;q=0.5, application/json",
                                 .path = "/"},
                   "json,html");
    expect_formats(&(struct neg){.accept = "application/json, */*",
                                 .path = "/"},
                   "html");
    expect_formats(&(struct neg){.accept = "*/*, application/json;q=0.1",
                                 .path = "/"},
                   "html");
    expect_formats(&(struct neg){.accept = "application/json;q=0.1,text/html;q=0.1",
                                 .path = "/"},
                   "json,html");
}

CF_TEST(q_values_read_like_rails) {
    static const struct {
        const char *accept;
        const char *expected;
    } cases[] = {
        {"text/html;q=, application/json", "html,json"},
        {"text/html; q=, application/json;q=0.5", "html,json"},
        {"text/html;q=;q=, application/json;q=0.5", "html,json"},
        {"text/html;q=0.5, application/json;q=", "json,html"},
        {"text/html;q=;x=1, application/json", "json,html"},
        {"text/html;q=;q=0.9, application/json;q=0.5", "json,html"},
        {"text/html;q=0.4;q=0.9, application/json;q=0.5", "json,html"},
        {"text/html;q=0.5.1, application/json;q=0.6", "json,html"},
        {"text/html;q=0.5.1.2, application/json;q=0.49", "html,json"},
        {"text/html;q=\"0.9\", application/json;q=0.5", "html,json"},
        {"text/html;q=\"\"0.9, application/json;q=0.5", "json,html"},
        {"text/html;q=1e-1, application/json;q=0.5", "json,html"},
        {"text/html;q=1_0, application/json;q=5", "html,json"},
        {"text/html;q=abc, application/json;q=0.1", "json,html"},
        {"text/html;q=+0.3, application/json;q=0.2", "html,json"},
        {"text/html;q= 0.3, application/json;q=0.2", "html,json"},
        {"text/html;q=1e17, application/json;q=1e18", "json,html"},
        {"text/html;q=1e19, application/json;q=1e20", "json,html"},
        {"text/html;q=-0.001, application/json;q=0", "html,json"},
        {"application/json;q=0, text/html;q=-0.001", "json,html"},
        {"text/html;q=0.5, application/json;q=+0x1", "json,html"},
        {"text/html;q=0.5, application/json;q=0x1", "html,json"},
        {"*/*;q=, application/json;q=0.5", "json,*/*"},
        /* Rails raises FloatDomainError (a 500) on an infinite q-value; the
         * pinned Rust port orders +inf first and -inf last. */
        {"text/html;q=1e400, application/json", "html,json"},
        {"text/html;q=-1e400, application/json", "json,html"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        expect_formats(
            &(struct neg){.accept = cases[i].accept, .path = "/"},
            cases[i].expected);
    }
}

CF_TEST(single_types) {
    expect_formats(&(struct neg){.accept = "application/json", .path = "/"},
                   "json");
    expect_formats(&(struct neg){.accept = "application/json; charset=utf-8",
                                 .path = "/"},
                   "json");
    expect_formats(&(struct neg){.accept = "image/svg+xml", .path = "/"},
                   "svg");
    expect_formats(
        &(struct neg){.accept = "application/x-unknown", .path = "/"}, "");
    const struct neg garbage = {.accept = "garbage", .path = "/"};
    struct fmt_env e;
    env_init(&e, &garbage);
    const cf_format **list = NULL;
    size_t count = 0;
    CF_CHECK(cf_ctx_formats(&e.ctx, &list, &count) == CF_INVALID);
    env_dispose(&e);
}

CF_TEST(wildcards_expand_in_registration_order) {
    struct fmt_env e;
    env_init(&e, &(struct neg){.accept = "text/*", .path = "/"});
    char symbols[512];
    env_symbols(&e, symbols, sizeof symbols);
    CF_CHECK(strncmp(symbols, "html,text,js", 12) == 0);
    CF_CHECK(strstr(symbols, "turbo_stream") != NULL);
    env_dispose(&e);
}

CF_TEST(xml_folding) {
    expect_formats(&(struct neg){.accept = "text/xml, application/rss+xml",
                                 .path = "/"},
                   "xml,rss");
    expect_formats(&(struct neg){.accept = "application/xml, application/rss+xml",
                                 .path = "/"},
                   "rss,xml");
    expect_formats(
        &(struct neg){
            .accept = "text/xml;q=0.9, application/xml;q=0.5, text/html",
            .path = "/"},
        "html,xml");
}

CF_TEST(path_extension_and_format_param) {
    expect_formats(&(struct neg){.path = "/users/1/avatar.svg"}, "svg");
    expect_formats(&(struct neg){.path = "/messages.json"}, "json");
    expect_formats(&(struct neg){.path = "/x.unknownext"}, "html");
    expect_formats(&(struct neg){.accept = "text/html",
                                 .path = "/",
                                 .format_param = "json"},
                   "json");
    expect_formats(&(struct neg){.path = "/", .format_param = "nope"}, "");
}

/* The requested suffix comes from the router's (.:format) capture, merged
 * into params as `format` (H03 captures it; the Accept header must not win
 * over it). */
CF_TEST(path_suffix_capture_beats_accept) {
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/messages(.:format)", 21, NULL) ==
               CF_OK);
    static char origin[] = "http://127.0.0.1:41999";
    cf_config_entry entries[2] = {
        {"PUBLIC_ORIGIN", origin},
        {"SECRET_KEY_BASE", HEX64},
    };
    cf_config *config = NULL;
    CF_REQUIRE(cf_config_parse(entries, 2, NULL, &config) == CF_OK);
    cf_app *app = NULL;
    CF_REQUIRE(cf_app_create(config, &app) == CF_OK);

    cf_request req;
    cf_test_req_init(&req);
    req.path = CF_TEST_SPAN("/messages.json");
    CF_REQUIRE(cf_test_req_header(&req, CF_TEST_SPAN("Accept"),
                                  CF_TEST_SPAN("text/html")) == CF_OK);
    cf_response resp;
    cf_response_init(&resp);
    cf_ctx ctx;
    CF_REQUIRE(cf_ctx_create(&ctx, app, NULL, &req, &resp) == CF_OK);
    const cf_format **list = NULL;
    size_t count = 0;
    CF_REQUIRE(cf_ctx_formats(&ctx, &list, &count) == CF_OK);
    CF_REQUIRE(count == 1);
    CF_CHECK(list[0] == &cf_format_json);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);
    cf_app_destroy(app);
}

CF_TEST(xhr_defaults_to_js) {
    expect_formats(&(struct neg){.path = "/", .xhr = true}, "js");
    expect_formats(&(struct neg){.path = "/",
                                 .content_type = "application/json",
                                 .xhr = true},
                   "json");
}

CF_TEST(respond_to_negotiation) {
    /* formats [turbo_stream, html], offered [html, json] -> html */
    struct fmt_env e;
    env_init(&e, &(struct neg){
                     .accept = "text/vnd.turbo-stream.html, text/html",
                     .path = "/"});
    const cf_format *offered[2] = {&cf_format_html, &cf_format_json};
    const cf_format *chosen = NULL;
    CF_CHECK(cf_ctx_respond_to(&e.ctx, offered, 2, &chosen) == CF_OK);
    CF_CHECK(chosen == &cf_format_html);
    CF_CHECK(cf_ctx_rendered_format(&e.ctx) == &cf_format_html);
    env_dispose(&e);

    /* formats [wildcard], offered [html, json] -> html (the first offered) */
    env_init(&e, &(struct neg){.accept = "*/*", .path = "/"});
    CF_CHECK(cf_ctx_respond_to(&e.ctx, offered, 2, &chosen) == CF_OK);
    CF_CHECK(chosen == &cf_format_html);
    env_dispose(&e);

    /* formats [png], offered [html, json] -> none (406 recorded) */
    env_init(&e, &(struct neg){.accept = "image/png", .path = "/"});
    CF_CHECK(cf_ctx_respond_to(&e.ctx, offered, 2, &chosen) == CF_NOT_FOUND);
    env_dispose(&e);

    /* formats [png], offered [html, wildcard] -> png */
    env_init(&e, &(struct neg){.accept = "image/png", .path = "/"});
    const cf_format *with_all[2] = {&cf_format_html, &cf_format_all};
    CF_CHECK(cf_ctx_respond_to(&e.ctx, with_all, 2, &chosen) == CF_OK);
    CF_CHECK(chosen != NULL && strcmp(chosen->symbol, "png") == 0);
    env_dispose(&e);

    /* empty formats -> none */
    struct neg no_accept_and_extension = {.path = "/"};
    env_init(&e, &no_accept_and_extension);
    const cf_format **list = NULL;
    size_t count = 0;
    /* A plain path with no Accept still negotiates HTML, matching the
     * reference default; force an empty result through the format param. */
    CF_REQUIRE(cf_ctx_formats(&e.ctx, &list, &count) == CF_OK);
    CF_CHECK(count == 1 && list[0] == &cf_format_html);
    env_dispose(&e);

    struct neg empty_format = {.path = "/", .format_param = "nope"};
    env_init(&e, &empty_format);
    CF_CHECK(cf_ctx_respond_to(&e.ctx, offered, 2, &chosen) == CF_NOT_FOUND);
    env_dispose(&e);
}

CF_TEST(formats_render_and_vary_helpers) {
    /* rendered_format falls back to the first request format, then HTML. */
    struct fmt_env e;
    env_init(&e, &(struct neg){.accept = "application/json", .path = "/"});
    CF_CHECK(cf_ctx_format(&e.ctx) == &cf_format_json);
    CF_CHECK(cf_ctx_rendered_format(&e.ctx) == &cf_format_json);
    /* The format came from Accept, not a format param. */
    CF_CHECK(cf_ctx_vary_accept(&e.ctx) == true);
    env_dispose(&e);

    env_init(&e, &(struct neg){.accept = "application/json",
                               .path = "/",
                               .format_param = "xml"});
    /* format param disables the Vary: Accept path */
    CF_CHECK(cf_ctx_vary_accept(&e.ctx) == false);
    env_dispose(&e);
}

CF_TEST_MAIN()
