/* VIEW-03 (A02 foundation): welcome/show against golden/a.
 *
 * Reference runner: tests/fixtures/crates/views/tests/parity_a.rs
 * (welcome_show).
 */
#include "cf_test.h"

#include "support/facts.h"
#include "support/golden.h"
#include "views.h"

#include <stdlib.h>
#include <string.h>

static cf_span span_of(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

CF_TEST(welcome_matches_golden) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "welcome", NULL, NULL));
    CF_REQUIRE(ctx.current_user.has_user);
    cf_view_welcome_model model = {0};
    model.current_user_name =
        (cf_span){(const unsigned char *)ctx.current_user.name.ptr,
                  ctx.current_user.name.len};
    cf_builder out = {0};
    CF_REQUIRE(cf_view_welcome(&ctx, &model, &out) == CF_OK);
    cf_golden_expect("welcome", (const char *)out.ptr, out.len);
    cf_builder_dispose(&out);
}

CF_TEST(welcome_sidebar_frame_and_meta) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "welcome", NULL, NULL));
    cf_view_welcome_model model = {0};
    model.current_user_name = span_of("New Bie");
    cf_builder out = {0};
    CF_REQUIRE(cf_view_welcome(&ctx, &model, &out) == CF_OK);
    CF_CHECK(cf_builder_contains(&out, "<title>No rooms yet</title>"));
    CF_CHECK(cf_builder_contains(&out, "<body class=\"sidebar\""));
    CF_CHECK(cf_builder_contains(&out, "<meta name=\"current-user-id\""));
    CF_CHECK(cf_builder_contains(&out, "<meta name=\"current-user-name\" content=\"New Bie\" />"));
    CF_CHECK(cf_builder_contains(&out, "id=\"user_sidebar\""));
    CF_CHECK(cf_builder_contains(&out, "src=\"/users/me/sidebar\""));
    /* The sidebar turbo-frame's data-action is html_safe: "->" stays raw. */
    CF_CHECK(cf_builder_contains(&out, "data-action=\"presence:present@window->rooms-list#read"));
    cf_builder_dispose(&out);
}

CF_TEST(welcome_frame_wraps_head_and_content_only) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "welcome", NULL, NULL));
    cf_view_welcome_model model = {0};
    model.current_user_name = span_of("New Bie");
    cf_builder out = {0};
    CF_REQUIRE(cf_view_welcome_frame(&ctx, &model, &out) == CF_OK);
    /* turbo-rails' frame layout: <html><head>head</head><body>content</body>. */
    static const char frame_prefix[] =
        "<html>\n  <head>\n    \n  </head>\n  <body>\n    ";
    CF_CHECK(out.len >= sizeof frame_prefix - 1 &&
             memcmp(out.ptr, frame_prefix, sizeof frame_prefix - 1) == 0);
    CF_CHECK(cf_builder_contains(&out, "</body>\n</html>\n"));
    CF_CHECK(cf_builder_contains(&out, "id=\"message-area\""));
    CF_CHECK(!cf_builder_contains(&out, "user_sidebar"));
    CF_CHECK(!cf_builder_contains(&out, "<title>"));
    cf_builder_dispose(&out);
}
