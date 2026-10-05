/* src/views/users_avatars.c — users/avatars/show.svg.erb: the initials
 * avatar for users without an uploaded one (`users::AvatarsController#show`'s
 * fallback render).
 *
 * Reference: tmp/rust-ref/crates/views/src/users.rs (AvatarSvg struct),
 * tmp/rust-ref/crates/views/templates/users/avatars/show.svg and
 * tmp/rust-ref/crates/views/src/helpers/users.rs (AVATAR_COLORS,
 * avatar_background_color).  The template takes `user_id` and the user's
 * `initials`; the `textLength="85%" lengthAdjust="spacingAndGlyphs"` branch
 * applies only when `initials.chars().count() >= 3`.  The rendered bytes are
 * pinned byte for byte by tests/fixtures/crates/views/tests/golden/a/
 * avatar_david.svg and avatar_three_initials.svg.
 */
#include "views/internal.h"

#include <inttypes.h>
#include <stdio.h>

#include <zlib.h>

/* `Users::AvatarsHelper::AVATAR_COLORS`. */
static const char *const AVATAR_COLORS[18] = {
    "#AF2E1B", "#CC6324", "#3B4B59", "#BFA07A", "#ED8008", "#ED3F1C",
    "#BF1B1B", "#736B1E", "#D07B53", "#736356", "#AD1D1D", "#BF7C2A",
    "#C09C6F", "#698F9C", "#7C956B", "#5D618F", "#3B3633", "#67695E"};

/* `avatar_background_color(user)`: `Zlib.crc32(user.to_param)` picks the
 * color.  zlib's crc32(0, ...) is the same IEEE CRC-32 crc32fast computes;
 * user.to_param for an integer is its decimal spelling. */
static const char *avatar_background_color(int64_t user_id) {
    char text[24];
    int n = snprintf(text, sizeof text, "%" PRId64, user_id);
    if (n <= 0) return AVATAR_COLORS[0];
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const Bytef *)text, (uInt)n);
    return AVATAR_COLORS[(size_t)(crc % 18u)];
}

/* `initials.chars().count()`: UTF-8 code points (the initials are ASCII, but
 * the template counts characters, not bytes). */
static size_t avatar_char_count(cf_span text) {
    size_t count = 0;
    for (size_t i = 0; i < text.len; i++) {
        if ((text.ptr[i] & 0xC0) != 0x80) count++;
    }
    return count;
}

cf_err cf_view_users_avatar_svg(int64_t user_id, cf_span initials,
                                cf_builder *out) {
    if (out == NULL) return CF_INVALID;
    cf_view_guard guard;
    cf_err rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    CF_VIEW_TRY(cf_view_str(
        out,
        "<svg version=\"1.1\" xmlns=\"http://www.w3.org/2000/svg\" "
        "xmlns:xlink=\"http://www.w3.org/1999/xlink\"\n"
        "  viewBox=\"0 0 512 512\" class=\"avatar\" aria-hidden=\"true\">\n"
        "  <defs>\n"
        "    <clipPath id=\"porthole\">\n"
        "      <circle cx=\"50%\" cy=\"50%\" r=\"50%\" />\n"
        "    </clipPath>\n"
        "  </defs>\n"
        "\n"
        "  <g>\n"
        "    <rect width=\"100%\" height=\"100%\" rx=\"50\" fill=\""));
    CF_VIEW_TRY(cf_view_str(out, avatar_background_color(user_id)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\" />\n"
        "\n"
        "    <text x=\"50%\" y=\"50%\" fill=\"#FFFFFF\"\n"
        "      text-anchor=\"middle\" dy=\"0.35em\"\n"));
    if (avatar_char_count(initials) >= 3) {
        CF_VIEW_TRY(cf_view_str(out,
                                "      textLength=\"85%\" "
                                "lengthAdjust=\"spacingAndGlyphs\"\n"));
    } else {
        /* The Askama `{% if %}` line leaves its six-space indent and the
         * newline behind, exactly as the golden fixture shows. */
        CF_VIEW_TRY(cf_view_str(out, "      \n"));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "      font-family=\"-apple-system, BlinkMacSystemFont, Segoe UI, "
        "Roboto, Helvetica, Arial, sans-serif\"\n"
        "      font-size=\"230\"\n"
        "      font-weight=\"800\"\n"
        "      letter-spacing=\"-5\">\n"
        "      "));
    /* `{{ initials }}` escapes like html_escape. */
    CF_VIEW_TRY(cf_view_text(out, initials));
    CF_VIEW_TRY(cf_view_str(out, "\n    </text>\n  </g>\n</svg>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}
