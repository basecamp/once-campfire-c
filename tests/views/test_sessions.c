/* VIEW-03 (A02 foundation): the sessions family against golden/a.
 *
 * Reference runner: tests/fixtures/crates/views/tests/parity_a.rs
 * (sessions_new, sessions_incompatible_browser, sessions_transfer); the
 * comparison is tests/views/support/golden.c, a port of the runner's DOM
 * normalization with only the runner's forgery masks.
 *
 * The inputs are the runner's own per-case facts: the case's path/account,
 * the literal email values the runner passes (sessions_new_email), and
 * User.administrator.first (David) for accounts/_help_contact.
 */
#include "cf_test.h"

#include "core/alloc.h"
#include "support/facts.h"
#include "support/golden.h"
#include "views.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *case_str(const char *case_name, const char *key) {
    return cf_facts_str(cf_facts_case(case_name), key);
}

static cf_str owned(const char *text) {
    if (text == NULL) text = "";
    return (cf_str){(char *)text, strlen(text)};
}

static cf_view_help_contact david_contact(const char *case_name) {
    yyjson_val *david = cf_facts_user(cf_facts_case(case_name), "David");
    cf_view_help_contact contact = {0};
    contact.name = owned(cf_facts_str(david, "name"));
    contact.email_address = owned(cf_facts_str(david, "email_address"));
    return contact;
}

static int render_session_new(const char *fixture, const char *email,
                              const char *alert, const char *variant) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, fixture, NULL, alert));
    cf_view_help_contact contact = david_contact(fixture);
    cf_view_session_new_model model = {0};
    if (email != NULL) {
        model.has_email_address = true;
        model.email_address = owned(email);
    }
    model.has_help_contact = true;
    model.help_contact = &contact;

    cf_builder out = {0};
    cf_err rc = strcmp(variant, "page") == 0
                    ? cf_view_session_new(&ctx, &model, &out)
                    : cf_view_session_new_frame(&ctx, &model, &out);
    if (rc != CF_OK) fprintf(stderr, "  %s: render rc=%d\n", fixture, (int)rc);
    CF_REQUIRE(rc == CF_OK);
    int ok = cf_golden_expect(fixture, (const char *)out.ptr, out.len);
    cf_builder_dispose(&out);
    return ok;
}

CF_TEST(sessions_new_matches_golden) {
    render_session_new("sessions_new", NULL, NULL, "page");
}

CF_TEST(sessions_new_email_matches_golden) {
    render_session_new("sessions_new_email", "x@y.com", NULL, "page");
}

CF_TEST(sessions_new_rejected_matches_golden) {
    render_session_new("sessions_new_rejected", "david@37signals.com",
                       "Too many requests or unauthorized.", "page");
}

CF_TEST(sessions_new_with_logo_matches_golden) {
    render_session_new("sessions_new_with_logo", NULL, NULL, "page");
}

CF_TEST(sessions_new_frame_renders_the_frame_layout) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "sessions_new", NULL, NULL));
    cf_view_session_new_model model = {0};
    cf_builder out = {0};
    CF_REQUIRE(cf_view_session_new_frame(&ctx, &model, &out) == CF_OK);
    CF_CHECK(out.len > 0);
    static const char frame_prefix[] = "<html>\n  <head>\n    ";
    CF_CHECK(out.len >= sizeof frame_prefix - 1 &&
             memcmp(out.ptr, frame_prefix, sizeof frame_prefix - 1) == 0);
    CF_CHECK(cf_builder_contains(&out, "<meta name=\"turbo-visit-control\" content=\"reload\">"));
    CF_CHECK(!cf_builder_contains(&out, "<!DOCTYPE html>"));
    CF_CHECK(!cf_builder_contains(&out, "<body class="));
    cf_builder_dispose(&out);
}

CF_TEST(sessions_incompatible_browser_matches_golden) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "incompatible_browser", NULL, NULL));
    cf_builder out = {0};
    CF_REQUIRE(cf_view_session_incompatible(&ctx, &out) == CF_OK);
    cf_golden_expect("incompatible_browser", (const char *)out.ptr, out.len);
    cf_builder_dispose(&out);
}

CF_TEST(sessions_incompatible_browser_apple_messages_matches_golden) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "incompatible_browser_apple_messages",
                                 NULL, NULL));
    CF_CHECK(ctx.platform.apple_messages);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_session_incompatible(&ctx, &out) == CF_OK);
    cf_golden_expect("incompatible_browser_apple_messages",
                     (const char *)out.ptr, out.len);
    cf_builder_dispose(&out);
}

CF_TEST(sessions_transfer_matches_golden) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "sessions_transfer", NULL, NULL));
    cf_view_session_transfer_model model = {0};
    model.action = (cf_span){
        (const unsigned char *)case_str("sessions_transfer", "path"),
        strlen(case_str("sessions_transfer", "path"))};
    cf_builder out = {0};
    CF_REQUIRE(cf_view_session_transfer(&ctx, &model, &out) == CF_OK);
    cf_golden_expect("sessions_transfer", (const char *)out.ptr, out.len);
    cf_builder_dispose(&out);
}

/* --- allocation-failure atomicity ------------------------------------------ */

static int64_t alloc_calls;
static int64_t fail_at = -1;

static void *test_alloc(size_t size) {
    if (alloc_calls++ == fail_at) return NULL;
    return malloc(size);
}

static void *test_realloc(void *ptr, size_t size) {
    if (alloc_calls++ == fail_at) return NULL;
    return realloc(ptr, size);
}

static void test_free(void *ptr) { free(ptr); }

static void fail_allocations_at(int64_t ordinal) {
    alloc_calls = 0;
    fail_at = ordinal;
    cf_core_set_allocator(test_alloc, test_realloc, test_free);
}

static void fail_allocations_off(void) {
    fail_at = -1;
    cf_core_reset_allocator();
}

CF_TEST(session_render_failure_keeps_the_builder) {
    /* 03-application.md A02: allocation failure destroys the partial page
     * rather than emitting half of one.  The builder starts with content; a
     * failed render must leave it byte-identical at the entry length. */
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "sessions_new", NULL, NULL));
    cf_view_session_new_model model = {0};
    cf_view_help_contact contact = david_contact("sessions_new");
    model.has_help_contact = true;
    model.help_contact = &contact;

    int failures_seen = 0;
    for (int64_t ordinal = 0; ordinal < 64; ordinal++) {
        cf_builder out = {0};
        CF_REQUIRE(cf_builder_append(
                       &out, (cf_span){(const unsigned char *)"keep", 4}) ==
                   CF_OK);
        size_t before = out.len;
        fail_allocations_at(ordinal);
        cf_err rc = cf_view_session_new(&ctx, &model, &out);
        fail_allocations_off();
        if (rc == CF_OK) {
            CF_CHECK(out.len > before);
            cf_builder_dispose(&out);
            break;
        }
        CF_CHECK(rc == CF_NOMEM || rc == CF_LIMIT);
        CF_CHECK(out.len == before);
        CF_CHECK(memcmp(out.ptr, "keep", 4) == 0);
        cf_builder_dispose(&out);
        failures_seen++;
    }
    CF_CHECK(failures_seen > 0);

    /* The builder stays usable after failures. */
    cf_builder out = {0};
    fail_allocations_at(0);
    CF_CHECK(cf_view_session_new(&ctx, &model, &out) == CF_NOMEM);
    fail_allocations_off();
    CF_REQUIRE(cf_view_session_new(&ctx, &model, &out) == CF_OK);
    cf_golden_expect("sessions_new", (const char *)out.ptr, out.len);
    cf_builder_dispose(&out);
}
