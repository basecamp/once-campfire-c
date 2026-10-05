/* VIEW users_avatars: users/avatars/show.svg.erb against golden/a.
 *
 * Reference runner: tests/fixtures/crates/views/tests/parity_a.rs
 * (`users_avatars_show`), which renders users::AvatarSvg { user_id, initials }
 * and compares byte for byte.  The SVG is not HTML-normalized here: the
 * pinned fixture bytes are the assertion (template whitespace included).
 */
#include "cf_test.h"

#include "support/golden.h"
#include "views.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cf_span span_of(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

CF_TEST(users_avatar_svg_matches_golden_bytes) {
    struct {
        const char *name;
        int64_t user_id;
        const char *initials;
    } cases[] = {
        {"avatar_david", 127326141, "D"},
        {"avatar_three_initials", 773523956, "ABC"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        size_t want_len = 0;
        char *want =
            cf_golden_read("a", cases[i].name, "svg", &want_len);
        CF_REQUIRE(want != NULL);
        cf_builder out = {0};
        CF_REQUIRE(cf_view_users_avatar_svg(cases[i].user_id,
                                            span_of(cases[i].initials),
                                            &out) == CF_OK);
        if (out.len != want_len ||
            memcmp(out.ptr, want, want_len) != 0) {
            fprintf(stderr, "  %s: %zu bytes, want %zu\n", cases[i].name,
                    out.len, want_len);
        }
        CF_CHECK(out.len == want_len &&
                 memcmp(out.ptr, want, want_len) == 0);
        cf_builder_dispose(&out);
        free(want);
    }
}

CF_TEST(users_avatar_svg_textlength_threshold) {
    cf_builder out = {0};
    CF_REQUIRE(cf_view_users_avatar_svg(127326141, span_of("AB"), &out) ==
               CF_OK);
    CF_CHECK(!cf_builder_contains(&out, "textLength"));
    cf_builder_dispose(&out);

    memset(&out, 0, sizeof out);
    CF_REQUIRE(cf_view_users_avatar_svg(127326141, span_of("ABC"), &out) ==
               CF_OK);
    CF_CHECK(cf_builder_contains(
        &out, "textLength=\"85%\" lengthAdjust=\"spacingAndGlyphs\""));
    cf_builder_dispose(&out);
}

/* `avatar_background_color(user_id)`: crc32 of the decimal id modulo the 18
 * AVATAR_COLORS entries (values pinned by the Rust helper's test table). */
CF_TEST(users_avatar_svg_background_color) {
    struct {
        int64_t id;
        const char *fill;
    } cases[] = {
        {127326141, "#736356"}, /* David: golden avatar_david.svg */
        {773523956, "#5D618F"}, /* Anna Bea Cole: golden fixture */
        {1, "#BF7C2A"},         /* crc32("1") % 18 == 11 */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        cf_builder out = {0};
        CF_REQUIRE(cf_view_users_avatar_svg(cases[i].id, span_of("X"), &out) ==
                   CF_OK);
        char needle[32];
        snprintf(needle, sizeof needle, "fill=\"%s\"", cases[i].fill);
        if (!cf_builder_contains(&out, needle)) {
            fprintf(stderr, "  id %lld: want %s\n", (long long)cases[i].id,
                    needle);
        }
        CF_CHECK(cf_builder_contains(&out, needle));
        cf_builder_dispose(&out);
    }
}

CF_TEST(users_avatar_svg_rejects_null_out) {
    CF_CHECK(cf_view_users_avatar_svg(1, span_of("X"), NULL) == CF_INVALID);
}
