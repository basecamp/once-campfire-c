/* src/views/internal.h — renderer-internal helpers for the view
 * renderers in src/views/.
 *
 * Not a contract: the view renderers include this, plus the two call sites
 * that share the ruby-compatible integer cast (src/actions/welcome.c and
 * src/presenters/layout.c).  Everything here writes through cf_builder with
 * the 8 MiB cap and returns cf_err.  Callers use the CF_VIEW_TRY/CF_VIEW_FAIL
 * pattern so a failed render leaves the builder at its entry length
 * (cf_view_guard).
 */
#ifndef CF_VIEWS_INTERNAL_H
#define CF_VIEWS_INTERNAL_H

#include "views.h"

#include "core/alloc.h"

#include <string.h>

/* Borrowed views of owned strings/builders; the spans die with the owner. */
static inline cf_span cf_str_span(cf_str value) {
    return (cf_span){(const unsigned char *)value.ptr, value.len};
}
static inline cf_span cf_span_of_lit(const char *literal) {
    return (cf_span){(const unsigned char *)literal, strlen(literal)};
}
static inline cf_span cf_view_span_of(const cf_builder *builder) {
    return (cf_span){builder->ptr, builder->len};
}

#define CF_VIEW_TRY(expr)      \
    do {                       \
        rc = (expr);           \
        if (rc != CF_OK) goto fail; \
    } while (0)

#define CF_VIEW_TRY_BOOL(expr) \
    do {                       \
        if (!(expr)) {         \
            rc = CF_NOMEM;     \
            goto fail;         \
        }                      \
    } while (0)

/* Builder guard: remember the entry length and truncate back to it on
 * failure. */
typedef struct {
    cf_builder *out;
    size_t start;
    cf_err rc;
} cf_view_guard;

cf_err cf_view_begin(cf_view_guard *guard, cf_builder *out);
/* Truncate to the entry length and return `rc` (CF_NOMEM when rc is CF_OK,
 * which a fail label should never see). */
cf_err cf_view_fail(cf_view_guard *guard, cf_err rc);
cf_err cf_view_finish(cf_view_guard *guard);

/* Cap-checked writes. */
cf_err cf_view_raw(cf_builder *out, cf_span bytes);
cf_err cf_view_str(cf_builder *out, const char *text);
cf_err cf_view_text(cf_builder *out, cf_span text);   /* cf_html_text */
cf_err cf_view_text_cstr(cf_builder *out, const char *text);
/* cf_html_attr with the cap check: returns CF_LIMIT (writing nothing) when
 * the escaped value would push the builder past the cap.  Every attribute
 * value written through cf_html_attr must use this wrapper so an expanding
 * value cannot bypass the 8 MiB cap. */
cf_err cf_view_html_attr(cf_builder *out, cf_span value);

/* ------------------------------------------------------------- attributes */

#define CF_VIEW_ATTRS_MAX 24

typedef struct {
    struct {
        const char *name;
        cf_span value;
        char number[24];
        bool present;
        bool trusted; /* only '"' is escaped (SafeBuffer attribute) */
        bool flag;    /* is_boolean_attribute: render name="name" */
    } items[CF_VIEW_ATTRS_MAX];
    size_t count;
} cf_view_attrs;

void cf_view_attrs_init(cf_view_attrs *attrs);
cf_err cf_view_attr(cf_view_attrs *attrs, const char *name, cf_span value);
cf_err cf_view_attr_cstr(cf_view_attrs *attrs, const char *name,
                         const char *value);
cf_err cf_view_attr_i64(cf_view_attrs *attrs, const char *name, int64_t value);
cf_err cf_view_attr_opt(cf_view_attrs *attrs, const char *name, bool present,
                        cf_span value);
cf_err cf_view_attr_raw(cf_view_attrs *attrs, const char *name, cf_span value);
cf_err cf_view_attr_flag(cf_view_attrs *attrs, const char *name, bool flag);
/* Replace in place (Ruby hash assignment), else append. */
cf_err cf_view_attr_set(cf_view_attrs *attrs, const char *name, cf_span value);
cf_err cf_view_attr_set_i64(cf_view_attrs *attrs, const char *name,
                            int64_t value);
cf_err cf_view_attr_set_opt(cf_view_attrs *attrs, const char *name,
                            bool present, cf_span value);
cf_err cf_view_attr_remove(cf_view_attrs *attrs, const char *name);
const cf_span *cf_view_attr_get(const cf_view_attrs *attrs, const char *name);

/* -------------------------------------------------------------- elements */

cf_err cf_view_open_start(cf_builder *out, const char *name,
                          const cf_view_attrs *attrs); /* "<name ...>" */
cf_err cf_view_legacy_tag(cf_builder *out, const char *name,
                          const cf_view_attrs *attrs); /* "<name ... />" */
cf_err cf_view_builder_tag(cf_builder *out, const char *name,
                           const cf_view_attrs *attrs); /* void: no end tag */
cf_err cf_view_content(cf_builder *out, const char *name,
                       const cf_view_attrs *attrs, cf_span content);
cf_err cf_view_content_text(cf_builder *out, const char *name,
                            const cf_view_attrs *attrs, cf_span text);
cf_err cf_view_close_tag(cf_builder *out, const char *name);

/* `image_tag(ctx, source, options)`: resolve the logical source, then render
 * the options in order with src (and size-expanded width/height) appended.
 * `logical` is a borrowed span, never a C string: callers pass literals
 * through cf_span_of_lit and builder content through cf_view_span_of. */
cf_err cf_view_image_tag(const cf_view_ctx *ctx, cf_span logical,
                         const cf_view_attrs *attrs, cf_builder *out);

/* ------------------------------------------------------------ fragments */

/* `account_logo_tag(ctx, style)` (helpers/users.rs). */
cf_err cf_view_account_logo(const cf_view_ctx *ctx, const char *style,
                            bool has_style, cf_builder *out);

/* `translation_button(ctx, key)` (helpers/translations.rs). */
cf_err cf_view_translation_button(const cf_view_ctx *ctx, const char *key,
                                  cf_builder *out);

/* `version_badge(ctx)`. */
cf_err cf_view_version_badge(const cf_view_ctx *ctx, cf_builder *out);

/* The layout's shared fragments (helpers/application.rs). */
cf_err cf_view_current_user_meta_tags(const cf_view_ctx *ctx, cf_builder *out);
cf_err cf_view_action_cable_meta_tag(const cf_view_ctx *ctx, cf_builder *out);
cf_err cf_view_body_classes(const cf_view_ctx *ctx, cf_span body_class,
                            bool has_body_class, cf_builder *out);

/* `form_with` open tag (helpers/forms.rs FormWith::open) for the foundation's
 * two forms.  The fields make attribute order match the reference; `multipart`
 * is set by the caller when a file field rendered. */
typedef struct {
    cf_builder *out;
    cf_span action;      /* absolute URL, borrowed until cf_view_form_open */
    const char *method;  /* "post", "put", ... */
    const char *param_key; /* "user" or NULL */
    const char *class_attr; /* NULL omits */
    cf_view_attrs data;    /* data-* attributes, in order */
    bool multipart;
    bool has_data;
} cf_view_form;

void cf_view_form_init(cf_view_form *form, cf_builder *out);
cf_err cf_view_form_open(const cf_view_form *form);
cf_err cf_view_form_field(const cf_view_form *form, const char *type,
                          const char *method, cf_span value, bool has_value,
                          const cf_view_attrs *options);
cf_err cf_view_form_password_field(const cf_view_form *form,
                                   const char *method,
                                   const cf_view_attrs *options);
cf_err cf_view_form_file_field(const cf_view_form *form, const char *method,
                               const cf_view_attrs *options,
                               bool *multipart_out);
cf_err cf_view_button(cf_builder *out, const cf_view_attrs *options,
                      cf_span content);
cf_err cf_view_method_tag(cf_builder *out, const char *method);

/* `button_to(url, options) { content }` (helpers/forms.rs).  `method` and
 * `form_class` are consumed by the form; the remaining options are the
 * button's own attributes, with `type="submit"` set (appended when absent).
 * `method` is one of "post"/"get"/"delete"/"patch"/"put". */
cf_err cf_view_button_to(cf_builder *out, cf_span url,
                         const cf_view_attrs *options, cf_span method,
                         cf_span form_class, cf_span content);

/* Owned NUL-terminated copy of a span (cf_str contract); an empty span
 * yields an empty non-NULL string. */
cf_err cf_view_str_dup(cf_span span, cf_str *out);

/* ------------------------------------------------------- ruby compatibility */

/* ruby_compat::integer_cast (tmp/rust-ref/crates/ruby/src/integer.rs:17-53),
 * the cast concerns.rs `last_room_cookie` applies to the `last_room` cookie:
 *
 *   - trim Ruby ISSPACE, then strip one optional '+'/'-'; nil unless a digit
 *     follows (integer_cast's `/\A\s*[+-]?\d/` gate);
 *   - `String#to_i` (to_i128): an optional `0d`/`0D` prefix, then digits with
 *     a single `_` allowed between two, stopping at the first other byte
 *     (a second `_` ends the number too);
 *   - nil when the value does not fit i64 (i64::try_from of the i128), the
 *     ActiveModel::Type::Integer serialize limit.
 *
 * `text` is borrowed; `out` is written only when true is returned.  Shared by
 * src/actions/welcome.c (the redirect to the last room) and
 * src/presenters/layout.c (Layout::load's last-room selection) so the two
 * cannot drift; the implementation lives in src/views/render.c. */
bool cf_views_integer_cast(cf_span text, int64_t *out);

/* --------------------------------------------------------------- fragments */

/* `ctx.url(path)`: the render context's base URL joined with an absolute
 * path (helpers/url.rs `url`). */
cf_err cf_view_ctx_url(const cf_view_ctx *ctx, cf_span path, cf_builder *out);

/* `ctx.asset(logical)`: resolve through the context's asset closure. */
cf_err cf_view_ctx_asset(const cf_view_ctx *ctx, cf_span logical,
                         cf_builder *out);

/* `time.to_fs(:epoch)` (messages/support.rs epoch_ms): the float round trip
 * is deliberate (it truncates some millisecond values down by one). */
int64_t cf_view_epoch_ms(int64_t time_us);
/* `time.iso8601` for a UTC time: seconds precision, Z suffix; writes the
 * 20-byte form into out[21] (NUL-terminated). */
cf_err cf_view_iso8601(int64_t time_us, char out[21]);

/* Ruby Float#to_s for a raw double (ruby_compat::float_to_s). */
cf_err cf_view_float_format(double value, cf_builder *out);

/* Ruby number printing (messages/support.rs RubyNumber): `Integer#to_s`,
 * or `Float#to_s` with Ruby 3.4's shortest-round-trip digits, the ".0"
 * fraction and the exponent form outside 1e-4..1e15. */
cf_err cf_view_number_format(cf_view_number value, cf_builder *out);
/* `number / 2`: integer division for integers, float division otherwise. */
cf_view_number cf_view_number_half(cf_view_number value);

/* `String#capitalize` for the ASCII browser/OS names (`capitalize`). */
cf_err cf_view_capitalize(cf_span text, cf_builder *out);

/* `Array#to_sentence(items, two_words_connector)`. */
cf_err cf_view_to_sentence(const cf_span *items, size_t count,
                           cf_span connector, cf_builder *out);

/* `ctx.can_administer?` (ViewContext): an administrator or bot user. */
bool cf_view_ctx_can_administer(const cf_view_ctx *ctx);

/* `sidebar_turbo_frame_tag(src, content)`: the shared user-sidebar frame the
 * welcome and room pages carry.  `src` NULL omits the attribute. */
cf_err cf_view_sidebar_frame(cf_span src, cf_builder *out);

/* messages/_template.html: the client-side message template the room page's
 * script block carries. */
cf_err cf_view_messages_template(const cf_view_ctx *ctx,
                                 const cf_view_user *user, cf_builder *out);

/* The pwa notification-help partials (pwa/_browser_settings.html,
 * pwa/_system_settings.html, pwa/_install_instructions.html). */
cf_err cf_view_pwa_browser_settings(const cf_view_ctx *ctx, cf_builder *out);
cf_err cf_view_pwa_system_settings(const cf_view_ctx *ctx, cf_builder *out);
cf_err cf_view_pwa_install_instructions(const cf_view_ctx *ctx, cf_builder *out);

#endif /* CF_VIEWS_INTERNAL_H */
