/* VIEW-03 (A02 foundation): first_runs/show against golden/a.
 *
 * Reference runner: tests/fixtures/crates/views/tests/parity_a.rs
 * (first_runs_show).
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

CF_TEST(first_run_matches_golden) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "first_run", NULL, NULL));
    cf_builder out = {0};
    CF_REQUIRE(cf_view_first_run(&ctx, &out) == CF_OK);
    cf_golden_expect("first_run", (const char *)out.ptr, out.len);
    cf_builder_dispose(&out);
}

CF_TEST(first_run_has_the_signup_body_and_setup_title) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "first_run", NULL, NULL));
    cf_builder out = {0};
    CF_REQUIRE(cf_view_first_run(&ctx, &out) == CF_OK);
    CF_CHECK(cf_builder_contains(&out, "<title>Set up Campfire</title>"));
    CF_CHECK(cf_builder_contains(&out, "<body class=\"signup\""));
    /* The form is multipart because it carries the avatar file field. */
    CF_CHECK(cf_builder_contains(&out, "enctype=\"multipart/form-data\""));
    CF_CHECK(cf_builder_contains(&out, "name=\"user[avatar]\""));
    CF_CHECK(cf_builder_contains(&out, "action=\"/first_run\""));
    cf_builder_dispose(&out);
}

/* --- allocation-failure cleanup -------------------------------------------
 *
 * The tracked allocator counts live blocks, so a builder skipped by a failure
 * path (the old first_run `button` leak) is visible without a sanitizer.  The
 * sweep injects one allocation failure at every ordinal up to the ordinary
 * render's allocation count. */

static int64_t alloc_calls;
static int64_t fail_at = -1;
static int64_t live_allocs;

static void *test_alloc(size_t size) {
    if (alloc_calls++ == fail_at) return NULL;
    void *ptr = malloc(size);
    if (ptr != NULL) live_allocs++;
    return ptr;
}

static void *test_realloc(void *ptr, size_t size) {
    if (alloc_calls++ == fail_at) return NULL;
    if (ptr == NULL) {
        void *fresh = realloc(NULL, size);
        if (fresh != NULL) live_allocs++;
        return fresh;
    }
    return realloc(ptr, size);
}

static void test_free(void *ptr) {
    if (ptr != NULL) live_allocs--;
    free(ptr);
}

static int first_run_allocation_failure_sweep(int frame) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "first_run", NULL, NULL));
    int failures_seen = 0;
    int completed = 0;
    for (int64_t ordinal = 0; ordinal < 96; ordinal++) {
        cf_core_set_allocator(test_alloc, test_realloc, test_free);
        alloc_calls = 0;
        fail_at = -1;
        live_allocs = 0;
        cf_builder out = {0};
        CF_REQUIRE(cf_builder_append(
                       &out, (cf_span){(const unsigned char *)"keep", 4}) ==
                   CF_OK);
        size_t entry = out.len;
        alloc_calls = 0;
        fail_at = ordinal;
        cf_err rc = frame ? cf_view_first_run_frame(&ctx, &out)
                          : cf_view_first_run(&ctx, &out);
        int grew = out.len > entry;
        int kept = out.len == entry && memcmp(out.ptr, "keep", 4) == 0;
        cf_builder_dispose(&out);
        cf_core_reset_allocator();
        /* Every builder the render created must have been disposed. */
        if (live_allocs != 0) {
            fprintf(stderr, "  first_run leak at ordinal %lld (%lld block%s)\n",
                    (long long)ordinal, (long long)live_allocs,
                    live_allocs == 1 ? "" : "s");
        }
        CF_CHECK(live_allocs == 0);
        if (rc == CF_OK) {
            CF_CHECK(grew);
            completed = 1;
            break;
        }
        CF_CHECK(rc == CF_NOMEM || rc == CF_LIMIT);
        CF_CHECK(kept);
        failures_seen++;
    }
    CF_CHECK(failures_seen > 0);
    CF_CHECK(completed);
    return 0;
}

CF_TEST(first_run_render_failure_frees_every_builder) {
    first_run_allocation_failure_sweep(0);
}

CF_TEST(first_run_frame_render_failure_frees_every_builder) {
    first_run_allocation_failure_sweep(1);
}

CF_TEST(first_run_frame_renders_the_frame_layout) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "first_run", NULL, NULL));
    cf_builder out = {0};
    CF_REQUIRE(cf_view_first_run_frame(&ctx, &out) == CF_OK);
    static const char frame_prefix[] = "<html>\n  <head>\n";
    CF_CHECK(out.len >= sizeof frame_prefix - 1 &&
             memcmp(out.ptr, frame_prefix, sizeof frame_prefix - 1) == 0);
    CF_CHECK(cf_builder_contains(&out, "<body>\n    <form class=\"center max-width\""));
    CF_CHECK(!cf_builder_contains(&out, "<!DOCTYPE html>"));
    cf_builder_dispose(&out);
}
