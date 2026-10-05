/* src/views/layout.c — layouts/application.html and layouts/turbo_rails/frame
 * (tmp/rust-ref/crates/views/templates/layouts/, layouts.rs).
 *
 * The application layout is transcribed from the pinned askama template:
 * the same elements, attributes, text and whitespace presence (the golden
 * comparison keeps whitespace-only text runs, so a missing newline between
 * two tags is a difference).  The Rails-only CSRF meta tags do not exist in
 * this app and are not emitted (D-C02).
 */
#include "views/internal.h"

static cf_err layout_head(cf_view_guard *guard, const cf_view_ctx *ctx,
                          cf_span page_title, bool has_page_title,
                          cf_span head) {
    cf_builder *out = guard->out;
    cf_err rc = CF_OK;

    /* `page_title_tag`: @page_title || "Campfire". */
    cf_view_attrs title;
    cf_view_attrs_init(&title);
    CF_VIEW_TRY(cf_view_content_text(out, "title", &title,
                                     has_page_title ? page_title
                                                    : cf_span_of_lit("Campfire")));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n\n    <meta name=\"viewport\" content=\"width=device-width, "
        "initial-scale=1, user-scalable=no, "
        "interactive-widget=resizes-content\">\n"
        "    <meta name=\"view-transition\" content=\"same-origin\">\n"
        "    <meta name=\"color-scheme\" content=\"light dark\">\n"
        "    <meta name=\"theme-color\" content=\"#ffffff\" "
        "media=\"(prefers-color-scheme: light)\">\n"
        "    <meta name=\"theme-color\" content=\"#000000\" "
        "media=\"(prefers-color-scheme: dark)\">\n"
        "    <meta name=\"apple-mobile-web-app-capable\" content=\"yes\">\n"
        "    "));
    CF_VIEW_TRY(cf_view_current_user_meta_tags(ctx, out));
    CF_VIEW_TRY(cf_view_str(out, "\n    "));
    CF_VIEW_TRY(cf_view_action_cable_meta_tag(ctx, out));
    CF_VIEW_TRY(cf_view_str(out, "\n\n    "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "name", "vapid-public-key"));
        CF_VIEW_TRY(cf_view_attr_opt(
            &attrs, "content", ctx->has_vapid_public_key,
            ctx->vapid_public_key));
        CF_VIEW_TRY(cf_view_builder_tag(out, "meta", &attrs));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n    <meta name=\"turbo-prefetch\" content=\"true\">\n\n"
        "    <link rel=\"manifest\" href=\"/webmanifest.json\">\n    "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "rel", "icon"));
        CF_VIEW_TRY(cf_view_attr(&attrs, "href",
                                 cf_str_span(ctx->account.logo_url)));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "type", "image/png"));
        CF_VIEW_TRY(cf_view_builder_tag(out, "link", &attrs));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n    "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "rel", "apple-touch-icon"));
        CF_VIEW_TRY(cf_view_attr(&attrs, "href",
                                 cf_str_span(ctx->account.logo_url)));
        CF_VIEW_TRY(cf_view_builder_tag(out, "link", &attrs));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n\n    "));
    CF_VIEW_TRY(cf_view_raw(out, ctx->stylesheet_tags));
    CF_VIEW_TRY(cf_view_str(out, "\n    "));
    if (ctx->has_custom_styles) {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-turbo-track", "reload"));
        CF_VIEW_TRY(cf_view_content(out, "style", &attrs,
                                    ctx->custom_styles));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n\n    "));
    CF_VIEW_TRY(cf_view_raw(out, ctx->importmap_tags));
    CF_VIEW_TRY(cf_view_str(out, "\n\n"));
    CF_VIEW_TRY(cf_view_raw(out, head));
    CF_VIEW_TRY(cf_view_str(out, "  </head>\n"));
    return CF_OK;
fail:
    return rc;
}

static cf_err layout_flash(cf_view_guard *guard, const cf_view_ctx *ctx) {
    cf_builder *out = guard->out;
    cf_err rc = CF_OK;
    bool alert = ctx->has_flash_alert;
    bool notice = ctx->has_flash_notice;
    if (!alert && !notice) return CF_OK;
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n      <div class=\"flash\" data-controller=\"element-removal\" "
        "data-action=\"animationend->element-removal#remove\">\n"
        "        <div class=\"flash__inner shadow\" style=\""));
    if (alert) {
        CF_VIEW_TRY(cf_view_str(out,
                                "--flash-background: var(--color-negative)"));
    }
    CF_VIEW_TRY(cf_view_str(out, "\">\n            "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 24));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "colorize--white"));
        CF_VIEW_TRY(cf_view_image_tag(
            ctx, cf_span_of_lit(alert ? "alert.svg" : "check.svg"), &attrs,
            out));
    }
    /* The reference template's stray `</span>` after the image. */
    CF_VIEW_TRY(cf_view_str(out, "</span>\n        </div>\n"
                                 "        <span class=\"for-screen-reader\" "
                                 "role=\"alert\" aria-atomic=\"true\">"));
    CF_VIEW_TRY(cf_view_text(out, alert ? ctx->flash_alert
                                        : ctx->flash_notice));
    CF_VIEW_TRY(cf_view_str(out, "</span>\n      </div>\n"));
    return CF_OK;
fail:
    return rc;
}

static cf_err layout_lightbox(cf_view_guard *guard, const cf_view_ctx *ctx) {
    cf_builder *out = guard->out;
    cf_err rc = CF_OK;
    CF_VIEW_TRY(cf_view_str(
        out,
        "<dialog class=\"lightbox\" aria-label=\"Image Viewer (Press escape to "
        "close)\" data-lightbox-target=\"dialog\" "
        "data-action=\"close->lightbox#reset\">\n"
        "  <img src=\"\" class=\"lightbox__image\" "
        "data-lightbox-target=\"zoomedImage\" />\n\n"
        "  <form method=\"dialog\" class=\"lightbox__btn\">\n"
        "    <button class=\"btn\">\n      "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("remove.svg"), &attrs, out));
    }
    CF_VIEW_TRY(cf_view_str(out,
                            "\n      <span class=\"for-screen-reader\">Close "
                            "image viewer</span>\n    </button>\n  </form>\n\n"
                            "  <a href=\"\" class=\"lightbox__btn--download "
                            "btn hide-in-ios-pwa\" "
                            "data-lightbox-target=\"download\">\n    "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("download.svg"), &attrs, out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n    <span class=\"for-screen-reader\">Download file</span>\n  "
        "</a>\n\n  <button class=\"lightbox__btn--share btn\"\n"
        "      data-controller=\"web-share\"\n"
        "      data-action=\"web-share#share\"\n"
        "      data-web-share-files-value=\"\"\n"
        "      data-lightbox-target=\"share\">\n    "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("share.svg"), &attrs, out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n    <span class=\"for-screen-reader\">Share file</span>\n"
        "  </button>\n</dialog>\n"));
    return CF_OK;
fail:
    return rc;
}

static cf_err layout_body(cf_view_guard *guard, const cf_view_ctx *ctx,
                          cf_span body_class, bool has_body_class, cf_span head,
                          cf_span content, cf_span nav, cf_span footer,
                          cf_span sidebar) {
    cf_builder *out = guard->out;
    cf_err rc = CF_OK;
    CF_VIEW_TRY(cf_view_str(out, "\n  <body class=\""));
    CF_VIEW_TRY(
        cf_view_body_classes(ctx, body_class, has_body_class, out));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\" data-controller=\"local-time lightbox\">\n"
        "    <a href=\"#main-content\" class=\"skip-navigation btn\">Skip to "
        "main content</a>\n\n"
        "    <nav id=\"nav\">\n"));
    CF_VIEW_TRY(cf_view_raw(out, nav));
    CF_VIEW_TRY(cf_view_str(out, "    </nav>\n\n"));
    CF_VIEW_TRY(layout_flash(guard, ctx));
    CF_VIEW_TRY(cf_view_str(out, "    <main id=\"main-content\">\n"));
    CF_VIEW_TRY(cf_view_raw(out, content));
    CF_VIEW_TRY(cf_view_str(out, "\n      <footer id=\"footer\">\n"));
    CF_VIEW_TRY(cf_view_raw(out, footer));
    CF_VIEW_TRY(cf_view_str(
        out,
        "      </footer>\n    </main>\n\n"
        "    <aside id=\"sidebar\" data-controller=\"toggle-class\" "
        "data-toggle-class-toggle-class=\"open\">\n"));
    CF_VIEW_TRY(cf_view_raw(out, sidebar));
    CF_VIEW_TRY(cf_view_str(out, "    </aside>\n\n    "));
    CF_VIEW_TRY(layout_lightbox(guard, ctx));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n\n    <a href=\"https://once.com\" id=\"app-logo\" "
        "target=\"_blank\" aria-label=\"Once software from 37signals home "
        "page\">\n      "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "alt", "Campfire logo"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "width", 256));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "height", 216));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("campfire-icon.png"), &attrs, out));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n    </a>\n  </body>\n</html>\n"));
    (void)head;
    return CF_OK;
fail:
    return rc;
}

cf_err cf_view_layout_page(const cf_view_ctx *ctx, cf_span page_title,
                           bool has_page_title, cf_span body_class,
                           bool has_body_class, cf_span head, cf_span content,
                           cf_span nav, cf_span footer, cf_span sidebar,
                           cf_builder *out) {
    if (ctx == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(out, "<!DOCTYPE html>\n<html>\n  <head>\n    "));
    CF_VIEW_TRY(layout_head(&guard, ctx, page_title, has_page_title, head));
    CF_VIEW_TRY(layout_body(&guard, ctx, body_class, has_body_class, head,
                            content, nav, footer, sidebar));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_layout_frame(const cf_view_ctx *ctx, cf_span head,
                            cf_span content, cf_builder *out) {
    if (ctx == NULL || out == NULL) return CF_INVALID;
    (void)ctx;
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(out, "<html>\n  <head>\n    "));
    CF_VIEW_TRY(cf_view_raw(out, head));
    CF_VIEW_TRY(cf_view_str(out, "\n  </head>\n  <body>\n    "));
    CF_VIEW_TRY(cf_view_raw(out, content));
    CF_VIEW_TRY(cf_view_str(out, "\n  </body>\n</html>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}
