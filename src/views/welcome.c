/* src/views/welcome.c — welcome/show.html.erb: shown to users who aren't in
 * any room yet (tmp/rust-ref/crates/views/templates/welcome/show.html,
 * welcome.rs).
 */
#include "views/internal.h"

static cf_err welcome_content(const cf_view_ctx *ctx,
                              const cf_view_welcome_model *model, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    CF_VIEW_TRY(cf_view_str(
        out,
        "<div id=\"message-area\" class=\"message-area\">\n"
        "  <div class=\"message-area--empty min-width center\">\n"
        "    <figure class=\"center pad\">\n      "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class",
                                      "colorize--black translucent"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("messages-empty.svg"), &attrs, out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n      <span class=\"for-screen-reader\">"));
    CF_VIEW_TRY(cf_view_text(out, model->current_user_name));
    CF_VIEW_TRY(cf_view_str(out, "</span>\n    </figure>\n  </div>\n</div>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* `sidebar_turbo_frame_tag(Some(user_sidebar), "")`. */
static cf_err welcome_sidebar(cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_view_attrs attrs;
    cf_view_attrs_init(&attrs);
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-turbo-permanent", "true"));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-controller",
                                  "rooms-list read-rooms turbo-frame"));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-rooms-list-unread-class",
                                  "unread"));
    /* html_safe in the reference: "->" is not escaped. */
    CF_VIEW_TRY(cf_view_attr_raw(
        &attrs, "data-action",
        cf_span_of_lit("presence:present@window->rooms-list#read "
                       "read-rooms:read->rooms-list#read "
                       "turbo:frame-load->rooms-list#loaded "
                       "refresh-room:visible@window->turbo-frame#reload")));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "id", "user_sidebar"));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "src", "/users/me/sidebar"));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "target", "_top"));
    CF_VIEW_TRY(cf_view_content(out, "turbo-frame", &attrs,
                                (cf_span){NULL, 0}));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_welcome(const cf_view_ctx *ctx,
                       const cf_view_welcome_model *model,
                       cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_view_guard guard;
    cf_err rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_builder content = {0}, sidebar = {0};
    rc = welcome_content(ctx, model, &content);
    if (rc == CF_OK) rc = welcome_sidebar(&sidebar);
    if (rc == CF_OK) {
        rc = cf_view_layout_page(ctx, cf_span_of_lit("No rooms yet"), true,
                                 cf_span_of_lit("sidebar"), true,
                                 (cf_span){NULL, 0}, cf_view_span_of(&content),
                                 (cf_span){NULL, 0}, (cf_span){NULL, 0},
                                 cf_view_span_of(&sidebar), out);
    }
    cf_builder_dispose(&content);
    cf_builder_dispose(&sidebar);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_welcome_frame(const cf_view_ctx *ctx,
                             const cf_view_welcome_model *model,
                             cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_view_guard guard;
    cf_err rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_builder content = {0};
    rc = welcome_content(ctx, model, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_frame(ctx, (cf_span){NULL, 0},
                                  cf_view_span_of(&content), out);
    }
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}
