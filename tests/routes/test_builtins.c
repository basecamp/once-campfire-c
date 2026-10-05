/* H03 built-in and reference-error responses (D-C08, spec 01 H03): the 41
 * reference_error rows answer the reference's public 404/500 directly, and
 * the small health/turbo_native/mailbox handlers are explicit responses.
 * Bodies are byte-checked against the pinned fixtures and the reference
 * strings; the development 501 is test-visible and named. */
#include "cf_test.h"

#include "core/testclock.h"
#include "routes.h"
#include "support/h03_test_util.h"

/* 2026-10-05T12:34:56Z */
#define FIXED_US INT64_C(1791203696000000)

/* The browser-like default a real client sends, so the rendered format stays
 * deterministic. The absent/empty-Accept fallback (A00's former crash) is
 * covered by tests/app/test_formats.c. */
#define H03_BROWSER_ACCEPT \
    "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"

static void run_get(cf_app *app, const char *path, const char *accept,
                    cf_response *resp) {
    cf_request req;
    h03_req_init(&req);
    req.path = (cf_span){(const unsigned char *)path, strlen(path)};
    const char *text = accept != NULL ? accept : H03_BROWSER_ACCEPT;
    cf_span value = {(const unsigned char *)text, strlen(text)};
    CF_REQUIRE(h03_req_header(&req, CF_TEST_SPAN("Accept"), value) == CF_OK);
    CF_REQUIRE(h03_process(app, &req, resp) == CF_OK);
}

static void run_method(cf_app *app, cf_method method, const char *path,
                       cf_response *resp) {
    cf_request req;
    h03_req_init(&req);
    req.method = method;
    req.original_method = method;
    req.path = (cf_span){(const unsigned char *)path, strlen(path)};
    CF_REQUIRE(h03_req_header(&req, CF_TEST_SPAN("Accept"),
                              CF_TEST_SPAN(H03_BROWSER_ACCEPT)) == CF_OK);
    CF_REQUIRE(h03_process(app, &req, resp) == CF_OK);
}

static void expect_body(cf_response *resp, const char *expected) {
    size_t len = 0;
    unsigned char *body = h03_response_bytes(resp, &len);
    CF_REQUIRE(body != NULL);
    size_t n = strlen(expected);
    CF_CHECK(len == n);
    CF_CHECK(len == 0 || memcmp(body, expected, len) == 0);
    free(body);
}

static void expect_fixture_body(cf_response *resp, const char *fixture) {
    size_t want_len = 0;
    unsigned char *want = h03_read_file(fixture, &want_len);
    CF_REQUIRE(want != NULL); /* a missing pinned fixture fails the case */
    size_t len = 0;
    unsigned char *body = h03_response_bytes(resp, &len);
    CF_REQUIRE(body != NULL);
    CF_CHECK(len == want_len);
    CF_CHECK(len == 0 || memcmp(body, want, len) == 0);
    free(body);
    free(want);
}

static void expect_content_type(cf_response *resp, cf_request *req,
                                const char *value) {
    char *headers = h03_serialize_headers(resp, req);
    CF_REQUIRE(headers != NULL);
    CF_CHECK(h03_header_is(headers, "Content-Type", value));
    free(headers);
}

CF_TEST(reference_404_renders_every_request_format) {
    cf_app *app = h03_make_app();
    CF_REQUIRE(app != NULL);
    cf_response resp;

    /* HTML (no Accept): the public 404 page. */
    run_get(app, "/first_run/new", NULL, &resp);
    CF_CHECK(resp.status == 404);
    expect_fixture_body(&resp, "tests/fixtures/assets/public/404.html");
    {
        cf_request req;
        h03_req_init(&req);
        req.path = CF_TEST_SPAN("/first_run/new");
        expect_content_type(&resp, &req, "text/html; charset=UTF-8");
    }
    cf_response_dispose(&resp);

    /* JSON, XML and YAML bodies come from the extensions. */
    run_get(app, "/first_run/new.json", NULL, &resp);
    CF_CHECK(resp.status == 404);
    expect_body(&resp, "{\"status\":404,\"error\":\"Not Found\"}");
    cf_response_dispose(&resp);

    run_get(app, "/first_run/new.xml", NULL, &resp);
    CF_CHECK(resp.status == 404);
    expect_body(&resp,
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<hash>\n"
                "  <status type=\"integer\">404</status>\n"
                "  <error>Not Found</error>\n"
                "</hash>\n");
    cf_response_dispose(&resp);

    run_get(app, "/first_run/new.yaml", NULL, &resp);
    CF_CHECK(resp.status == 404);
    expect_body(&resp, "---\n:status: 404\n:error: Not Found\n");
    cf_response_dispose(&resp);

    /* A non-hash format (turbo_stream) falls back to the HTML page. */
    run_get(app, "/first_run/new", "text/vnd.turbo-stream.html", &resp);
    CF_CHECK(resp.status == 404);
    expect_fixture_body(&resp, "tests/fixtures/assets/public/404.html");
    cf_response_dispose(&resp);

    /* HEAD is empty in the request's format. */
    run_method(app, CF_HEAD, "/first_run/new", &resp);
    CF_CHECK(resp.status == 404);
    CF_CHECK(resp.body_kind == CF_BODY_NONE);
    {
        cf_request req;
        h03_req_init(&req);
        req.method = CF_HEAD;
        req.path = CF_TEST_SPAN("/first_run/new");
        expect_content_type(&resp, &req, "text/html; charset=UTF-8");
    }
    cf_response_dispose(&resp);

    /* The 40 action_not_found rows all dispatch to the same handler. */
    run_get(app, "/rooms/new", NULL, &resp);
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);
    run_get(app, "/messages/new", NULL, &resp);
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);

    cf_app_destroy(app);
}

CF_TEST(reference_500_missing_controller) {
    cf_app *app = h03_make_app();
    CF_REQUIRE(app != NULL);
    cf_response resp;

    run_get(app, "/rooms/1/settings", NULL, &resp);
    CF_CHECK(resp.status == 500);
    expect_fixture_body(&resp, "tests/fixtures/assets/public/500.html");
    {
        cf_request req;
        h03_req_init(&req);
        req.path = CF_TEST_SPAN("/rooms/1/settings");
        expect_content_type(&resp, &req, "text/html; charset=UTF-8");
    }
    cf_response_dispose(&resp);

    run_get(app, "/rooms/1/settings.json", NULL, &resp);
    CF_CHECK(resp.status == 500);
    expect_body(&resp, "{\"status\":500,\"error\":\"Internal Server Error\"}");
    cf_response_dispose(&resp);

    cf_app_destroy(app);
}

CF_TEST(reference_404_without_a_page_is_an_empty_body) {
    cf_app *app = h03_make_app();
    CF_REQUIRE(app != NULL);
    CF_CHECK(cf_static_set_root("/nonexistent/h03/root") == CF_OK);
    cf_response resp;
    run_get(app, "/first_run/new", NULL, &resp);
    CF_CHECK(resp.status == 404);
    CF_CHECK(resp.body_kind == CF_BODY_NONE);
    {
        cf_request req;
        h03_req_init(&req);
        req.path = CF_TEST_SPAN("/first_run/new");
        expect_content_type(&resp, &req, "text/html; charset=UTF-8");
    }
    cf_response_dispose(&resp);
    CF_CHECK(cf_static_set_root(NULL) == CF_OK);
    CF_CHECK(strcmp(cf_static_root(), CF_STATIC_ROOT) == 0);
    cf_app_destroy(app);
}

CF_TEST(health_show_formats) {
    cf_app *app = h03_make_app();
    CF_REQUIRE(app != NULL);
    cf_test_clock_set_fixed_us(FIXED_US);
    cf_response resp;

    run_get(app, "/up", "text/html", &resp);
    CF_CHECK(resp.status == 200);
    expect_body(&resp,
                "<!DOCTYPE html><html><body style=\"background-color: "
                "green\"></body></html>");
    cf_response_dispose(&resp);

    run_get(app, "/up.json", NULL, &resp);
    CF_CHECK(resp.status == 200);
    expect_body(&resp,
                "{\"status\":\"up\",\"timestamp\":\"2026-10-05T12:34:56Z\"}");
    {
        cf_request req;
        h03_req_init(&req);
        req.path = CF_TEST_SPAN("/up.json");
        expect_content_type(&resp, &req, "application/json; charset=utf-8");
    }
    cf_response_dispose(&resp);

    /* A client format nobody offers is the reference UnknownFormat (406). */
    run_get(app, "/up", "application/xml", &resp);
    CF_CHECK(resp.status == 406);
    cf_response_dispose(&resp);

    cf_test_clock_clear();
    cf_app_destroy(app);
}

CF_TEST(turbo_native_navigation) {
    cf_app *app = h03_make_app();
    CF_REQUIRE(app != NULL);
    cf_response resp;

    run_get(app, "/recede_historical_location", NULL, &resp);
    CF_CHECK(resp.status == 200);
    expect_body(&resp, "Going back\xE2\x80\xA6");
    cf_response_dispose(&resp);

    run_get(app, "/resume_historical_location", NULL, &resp);
    CF_CHECK(resp.status == 200);
    expect_body(&resp, "Staying put\xE2\x80\xA6");
    cf_response_dispose(&resp);

    run_get(app, "/refresh_historical_location", NULL, &resp);
    CF_CHECK(resp.status == 200);
    expect_body(&resp, "Refreshing\xE2\x80\xA6");
    {
        cf_request req;
        h03_req_init(&req);
        req.path = CF_TEST_SPAN("/refresh_historical_location");
        expect_content_type(&resp, &req, "text/html; charset=utf-8");
    }
    cf_response_dispose(&resp);

    cf_app_destroy(app);
}

CF_TEST(mailbox_ingress_is_404_and_conductor_get_is_403) {
    cf_app *app = h03_make_app();
    CF_REQUIRE(app != NULL);
    cf_response resp;

    run_method(app, CF_POST, "/rails/action_mailbox/postmark/inbound_emails",
               &resp);
    CF_CHECK(resp.status == 404);
    CF_CHECK(resp.body_kind == CF_BODY_NONE);
    {
        cf_request req;
        h03_req_init(&req);
        req.method = CF_POST;
        expect_content_type(&resp, &req, "text/html");
    }
    cf_response_dispose(&resp);

    run_get(app, "/rails/action_mailbox/mandrill/inbound_emails", NULL,
            &resp);
    CF_CHECK(resp.status == 404);
    CF_CHECK(resp.body_kind == CF_BODY_NONE);
    cf_response_dispose(&resp);

    run_get(app, "/rails/conductor/action_mailbox/inbound_emails", NULL,
            &resp);
    CF_CHECK(resp.status == 403);
    CF_CHECK(resp.body_kind == CF_BODY_NONE);
    cf_response_dispose(&resp);

    /* The POST conductor rows run A01's cf_check_csrf first. Plain HTTP with
     * SSL disabled (the config default) and no Origin/Sec-Fetch-Site passes
     * the check, then the handler answers the same head 403 as the GET rows. */
    run_method(app, CF_POST, "/rails/conductor/action_mailbox/inbound_emails",
               &resp);
    CF_CHECK(resp.status == 403);
    CF_CHECK(resp.body_kind == CF_BODY_NONE);
    {
        cf_request req;
        h03_req_init(&req);
        req.method = CF_POST;
        expect_content_type(&resp, &req, "text/html");
    }
    cf_response_dispose(&resp);

    /* A cross-site Origin fails the CSRF check: 422, not 403. */
    {
        cf_request req;
        h03_req_init(&req);
        req.method = CF_POST;
        req.original_method = CF_POST;
        req.path = CF_TEST_SPAN(
            "/rails/conductor/action_mailbox/inbound_emails");
        CF_REQUIRE(h03_req_header(&req, CF_TEST_SPAN("Accept"),
                                  CF_TEST_SPAN(H03_BROWSER_ACCEPT)) == CF_OK);
        CF_REQUIRE(h03_req_header(&req, CF_TEST_SPAN("Origin"),
                                  CF_TEST_SPAN("https://evil.example")) ==
                   CF_OK);
        CF_REQUIRE(h03_process(app, &req, &resp) == CF_OK);
        CF_CHECK(resp.status == 422);
        cf_response_dispose(&resp);
    }

    cf_app_destroy(app);
}

CF_TEST(dev_501_is_test_visible_and_named) {
    cf_app *app = h03_make_app();
    CF_REQUIRE(app != NULL);
    cf_response resp;

    /* A still-unlanded packet row (accounts/users#index). */
    run_get(app, "/account/users", NULL, &resp);
    CF_CHECK(resp.status == 501);
    expect_body(&resp, "501 Not Implemented: route 19 cf_action_accounts_users_index (/account/users(.:format))\n");
    {
        cf_request req;
        h03_req_init(&req);
        expect_content_type(&resp, &req, "text/plain; charset=utf-8");
    }
    cf_response_dispose(&resp);

    /* A known not-yet-landed packet action. */
    run_get(app, "/qr_code/aGVsbG8", NULL, &resp);
    CF_CHECK(resp.status == 501);
    cf_response_dispose(&resp);

    /* Unmatched routes answer the reference public 404, not 501. */
    run_get(app, "/nope", NULL, &resp);
    CF_CHECK(resp.status == 404);
    expect_fixture_body(&resp, "tests/fixtures/assets/public/404.html");
    {
        cf_request req;
        h03_req_init(&req);
        req.path = CF_TEST_SPAN("/nope");
        expect_content_type(&resp, &req, "text/html; charset=UTF-8");
    }
    cf_response_dispose(&resp);
    run_method(app, CF_PATCH, "/rooms", &resp);
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);

    cf_app_destroy(app);
}

CF_TEST(bad_utf8_path_params_are_400_end_to_end) {
    cf_app *app = h03_make_app();
    CF_REQUIRE(app != NULL);
    cf_response resp;
    run_get(app, "/rooms/%FF", NULL, &resp);
    CF_CHECK(resp.status == 400);
    CF_CHECK(resp.body_kind == CF_BODY_NONE);
    cf_response_dispose(&resp);
    /* A malformed escape is literal and still routes; rooms#show is bound
     * now, so the request runs the action and answers its redirect (no
     * session => sign-in redirect), not the development 501. */
    run_get(app, "/rooms/%zz", NULL, &resp);
    CF_CHECK(resp.status == 302);
    cf_response_dispose(&resp);
    cf_app_destroy(app);
}

CF_TEST_MAIN()
