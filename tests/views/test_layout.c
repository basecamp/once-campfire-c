/* A02 foundation: the application and frame layouts.
 *
 * The page fixtures above exercise the application layout against golden/a
 * (first_run, welcome, sessions).  These focused cases cover the layout
 * branches no page fixture reaches: the default title, the body-class
 * combinations, the notice flash, and the frame wrapper's exact bytes.
 */
#include "cf_test.h"

#include "support/facts.h"
#include "support/golden.h"
#include "views.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cf_span span_of(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

CF_TEST(layout_default_title_and_empty_body_class) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    cf_test_views_assets_ctx(&ctx);
    static cf_str default_logo = {(char *)"/account/logo", 13};
    ctx.account.logo_url = default_logo;
    cf_builder out = {0};
    CF_REQUIRE(cf_view_layout_page(&ctx, (cf_span){NULL, 0}, false,
                                   (cf_span){NULL, 0}, false, (cf_span){NULL, 0},
                                   (cf_span){NULL, 0}, (cf_span){NULL, 0},
                                   (cf_span){NULL, 0}, (cf_span){NULL, 0},
                                   &out) == CF_OK);
    CF_CHECK(cf_builder_contains(&out, "<title>Campfire</title>"));
    CF_CHECK(cf_builder_contains(&out, "<body class=\"\""));
    /* No vapid key: the content attribute is omitted, not empty. */
    CF_CHECK(cf_builder_contains(&out, "<meta name=\"vapid-public-key\">"));
    /* No account logo: the layout still renders its default paths. */
    CF_CHECK(cf_builder_contains(&out, "<link rel=\"icon\" href=\"/account/logo\""));
    cf_builder_dispose(&out);
}

CF_TEST(layout_body_classes_join_in_reference_order) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    cf_test_views_assets_ctx(&ctx);
    ctx.current_user.has_user = true;
    ctx.current_user.administrator = true;
    ctx.account.has_logo = true;
    cf_builder out = {0};
    CF_REQUIRE(cf_view_layout_page(&ctx, (cf_span){NULL, 0}, false,
                                   span_of("sidebar"), true, (cf_span){NULL, 0},
                                   (cf_span){NULL, 0}, (cf_span){NULL, 0},
                                   (cf_span){NULL, 0}, (cf_span){NULL, 0},
                                   &out) == CF_OK);
    CF_CHECK(cf_builder_contains(&out, "<body class=\"sidebar admin account-has-logo\""));
    cf_builder_dispose(&out);
}

CF_TEST(layout_notice_flash_uses_check_icon) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    cf_test_views_assets_ctx(&ctx);
    ctx.has_flash_notice = true;
    ctx.flash_notice = span_of("Saved & done");
    cf_builder out = {0};
    CF_REQUIRE(cf_view_layout_page(&ctx, (cf_span){NULL, 0}, false,
                                   (cf_span){NULL, 0}, false, (cf_span){NULL, 0},
                                   (cf_span){NULL, 0}, (cf_span){NULL, 0},
                                   (cf_span){NULL, 0}, (cf_span){NULL, 0},
                                   &out) == CF_OK);
    CF_CHECK(cf_builder_contains(&out, "class=\"flash\""));
    CF_CHECK(cf_builder_contains(&out, "check-"));
    CF_CHECK(!cf_builder_contains(&out, "flash-background"));
    /* The notice is escaped text. */
    CF_CHECK(cf_builder_contains(&out, "role=\"alert\" aria-atomic=\"true\">Saved &amp; done<"));
    cf_builder_dispose(&out);
}

CF_TEST(layout_alert_flash_uses_negative_style) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    cf_test_views_assets_ctx(&ctx);
    ctx.has_flash_alert = true;
    ctx.flash_alert = span_of("Nope");
    cf_builder out = {0};
    CF_REQUIRE(cf_view_layout_page(&ctx, (cf_span){NULL, 0}, false,
                                   (cf_span){NULL, 0}, false, (cf_span){NULL, 0},
                                   (cf_span){NULL, 0}, (cf_span){NULL, 0},
                                   (cf_span){NULL, 0}, (cf_span){NULL, 0},
                                   &out) == CF_OK);
    CF_CHECK(cf_builder_contains(&out, "--flash-background: var(--color-negative)"));
    CF_CHECK(cf_builder_contains(&out, "alert-"));
    cf_builder_dispose(&out);
}

CF_TEST(layout_custom_styles_render_a_reload_tracked_style) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    cf_test_views_assets_ctx(&ctx);
    ctx.has_custom_styles = true;
    ctx.custom_styles = span_of("a > b { color: red }");
    cf_builder out = {0};
    CF_REQUIRE(cf_view_layout_page(&ctx, (cf_span){NULL, 0}, false,
                                   (cf_span){NULL, 0}, false, (cf_span){NULL, 0},
                                   (cf_span){NULL, 0}, (cf_span){NULL, 0},
                                   (cf_span){NULL, 0}, (cf_span){NULL, 0},
                                   &out) == CF_OK);
    /* The account CSS is raw in a raw-text style element. */
    CF_CHECK(cf_builder_contains(&out,
                                 "<style data-turbo-track=\"reload\">a > "
                                 "b { color: red }</style>"));
    cf_builder_dispose(&out);
}

CF_TEST(layout_output_cap_rejects_oversized_pages) {
    /* A02: rendering is bounded by the 8 MiB output cap; over it the render
     * fails CF_LIMIT and destroys the partial page. */
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    cf_test_views_assets_ctx(&ctx);
    size_t huge = CF_VIEWS_MAX_OUTPUT + 1;
    char *styles = malloc(huge);
    CF_REQUIRE(styles != NULL);
    memset(styles, 'a', huge);
    ctx.has_custom_styles = true;
    ctx.custom_styles = (cf_span){(const unsigned char *)styles, huge};
    cf_builder out = {0};
    cf_err rc = cf_view_layout_page(&ctx, (cf_span){NULL, 0}, false,
                                    (cf_span){NULL, 0}, false,
                                    (cf_span){NULL, 0}, (cf_span){NULL, 0},
                                    (cf_span){NULL, 0}, (cf_span){NULL, 0},
                                    (cf_span){NULL, 0}, &out);
    if (rc != CF_LIMIT) fprintf(stderr, "  cap render rc=%d\n", (int)rc);
    CF_CHECK(rc == CF_LIMIT);
    CF_CHECK(out.len == 0);
    free(styles);
    cf_builder_dispose(&out);
}

CF_TEST(layout_cap_counts_escaped_text_expansion) {
    /* The cap bounds the bytes written, not the input length.  A flash alert
     * of `cap - 32768` '&' characters fits the unescaped check but expands to
     * ~5x, so the render must fail CF_LIMIT with no partial output instead of
     * overshooting the 8 MiB cap. */
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    cf_test_views_assets_ctx(&ctx);
    size_t len = CF_VIEWS_MAX_OUTPUT - 32768;
    char *alert = malloc(len);
    CF_REQUIRE(alert != NULL);
    memset(alert, '&', len);
    ctx.has_flash_alert = true;
    ctx.flash_alert = (cf_span){(const unsigned char *)alert, len};
    cf_builder out = {0};
    CF_REQUIRE(cf_builder_append(
                   &out, (cf_span){(const unsigned char *)"keep", 4}) == CF_OK);
    cf_err rc = cf_view_layout_page(&ctx, (cf_span){NULL, 0}, false,
                                    (cf_span){NULL, 0}, false,
                                    (cf_span){NULL, 0}, (cf_span){NULL, 0},
                                    (cf_span){NULL, 0}, (cf_span){NULL, 0},
                                    (cf_span){NULL, 0}, &out);
    if (rc != CF_LIMIT) fprintf(stderr, "  escaped text rc=%d len=%zu\n",
                                (int)rc, out.len);
    CF_CHECK(rc == CF_LIMIT);
    CF_CHECK(out.len == 4 && memcmp(out.ptr, "keep", 4) == 0);
    free(alert);
    cf_builder_dispose(&out);
}

CF_TEST(layout_cap_counts_escaped_attribute_expansion) {
    /* Same expansion on the attribute path (account logo href): the escaped
     * value must be accounted for before cf_html_attr writes. */
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    cf_test_views_assets_ctx(&ctx);
    size_t len = CF_VIEWS_MAX_OUTPUT - 32768;
    char *logo = malloc(len);
    CF_REQUIRE(logo != NULL);
    memset(logo, '&', len);
    ctx.account.logo_url = (cf_str){logo, len};
    cf_builder out = {0};
    CF_REQUIRE(cf_builder_append(
                   &out, (cf_span){(const unsigned char *)"keep", 4}) == CF_OK);
    cf_err rc = cf_view_layout_page(&ctx, (cf_span){NULL, 0}, false,
                                    (cf_span){NULL, 0}, false,
                                    (cf_span){NULL, 0}, (cf_span){NULL, 0},
                                    (cf_span){NULL, 0}, (cf_span){NULL, 0},
                                    (cf_span){NULL, 0}, &out);
    if (rc != CF_LIMIT) fprintf(stderr, "  escaped attr rc=%d len=%zu\n",
                                (int)rc, out.len);
    CF_CHECK(rc == CF_LIMIT);
    CF_CHECK(out.len == 4 && memcmp(out.ptr, "keep", 4) == 0);
    free(logo);
    cf_builder_dispose(&out);
}

CF_TEST(layout_output_cap_rejects_an_over_cap_entry_builder) {
    /* A builder that already holds more than the cap has no remaining room;
     * the render must return CF_LIMIT and leave the entry bytes untouched
     * (the old subtraction underflowed and appended anyway). */
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    cf_test_views_assets_ctx(&ctx);
    cf_builder out = {0};
    size_t pre = CF_VIEWS_MAX_OUTPUT + 4096;
    char *buf = malloc(pre);
    CF_REQUIRE(buf != NULL);
    memset(buf, 'k', pre);
    CF_REQUIRE(cf_builder_append(&out, (cf_span){(const unsigned char *)buf,
                                                 pre}) == CF_OK);
    free(buf);
    size_t entry = out.len;
    cf_err rc = cf_view_layout_page(&ctx, (cf_span){NULL, 0}, false,
                                    (cf_span){NULL, 0}, false,
                                    (cf_span){NULL, 0}, (cf_span){NULL, 0},
                                    (cf_span){NULL, 0}, (cf_span){NULL, 0},
                                    (cf_span){NULL, 0}, &out);
    if (rc != CF_LIMIT) fprintf(stderr, "  over-cap entry rc=%d len=%zu\n",
                                (int)rc, out.len);
    CF_CHECK(rc == CF_LIMIT);
    CF_CHECK(out.len == entry);
    cf_builder_dispose(&out);
}

CF_TEST(layout_frame_is_exact) {
    cf_view_ctx ctx = {0};
    /* The frame layout does not read assets: no setup needed. */
    cf_builder out = {0};
    CF_REQUIRE(cf_view_layout_frame(&ctx, span_of("<meta name=\"x\">"),
                                    span_of("<p>hi</p>"), &out) == CF_OK);
    static const char expected[] =
        "<html>\n  <head>\n    <meta name=\"x\">\n  </head>\n  <body>\n"
        "    <p>hi</p>\n  </body>\n</html>\n";
    CF_CHECK(out.len == sizeof expected - 1);
    CF_CHECK(out.len == sizeof expected - 1 &&
             memcmp(out.ptr, expected, sizeof expected - 1) == 0);
    cf_builder_dispose(&out);
}

CF_TEST(layout_asset_paths_come_from_the_manifest) {
    CF_REQUIRE(cf_test_views_setup());
    cf_builder out = {0};
    CF_REQUIRE(cf_views_asset_path(span_of("campfire-icon.png"), &out) ==
               CF_OK);
    CF_CHECK(cf_builder_equals(&out, "/assets/campfire-icon-3d9986c5.png"));
    cf_builder_dispose(&out);

    /* URLs and absolute paths pass through, tails are kept. */
    cf_builder pass = {0};
    CF_REQUIRE(cf_views_asset_path(span_of("https://x/a.png"), &pass) == CF_OK);
    CF_CHECK(cf_builder_equals(&pass, "https://x/a.png"));
    cf_builder_dispose(&pass);
    cf_builder tail = {0};
    CF_REQUIRE(cf_views_asset_path(span_of("bot.svg?v=1#x"), &tail) == CF_OK);
    CF_CHECK(cf_builder_contains(&tail, "?v=1#x"));
    cf_builder_dispose(&tail);

    /* A missing asset is an error, not an empty path. */
    cf_builder missing = {0};
    CF_CHECK(cf_views_asset_path(span_of("nope.png"), &missing) ==
             CF_NOT_FOUND);
    CF_CHECK(missing.len == 0);
    cf_builder_dispose(&missing);
}

CF_TEST(layout_stylesheet_and_importmap_tags_come_from_the_build) {
    CF_REQUIRE(cf_test_views_setup());
    cf_builder css = {0};
    CF_REQUIRE(cf_views_stylesheet_tags(&css) == CF_OK);
    /* Sorted logical paths: _reset first, utilities last. */
    CF_CHECK(cf_builder_contains(
        &css, "<link rel=\"stylesheet\" href=\"/assets/_reset-"
              "9c3efd7b.css\" data-turbo-track=\"reload\" />"));
    CF_CHECK(cf_builder_contains(&css, "/assets/utilities-e2f32466.css\" "
                                       "data-turbo-track=\"reload\" />"));
    CF_CHECK(!cf_builder_contains(&css, "&amp;"));
    cf_builder_dispose(&css);

    cf_builder map = {0};
    CF_REQUIRE(cf_views_importmap_tags(&map) == CF_OK);
    CF_CHECK(cf_builder_contains(&map, "<script type=\"importmap\" data-turbo-track=\"reload\">"));
    CF_CHECK(cf_builder_contains(&map, "<script type=\"module\">import \"application\"</script>"));
    cf_builder_dispose(&map);

    cf_builder links = {0};
    CF_REQUIRE(cf_views_preload_links(&links) == CF_OK);
    CF_CHECK(links.len > 16 &&
             memcmp(links.ptr, "</assets/_reset-", 16) == 0);
    CF_CHECK(links.len <= 1000);
    cf_builder_dispose(&links);
}
