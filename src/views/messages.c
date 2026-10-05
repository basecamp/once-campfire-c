/* src/views/messages.c — messages/{_message,_actions,_template,
 * _presentation,_unrenderable,index,show,edit,create,destroy,room_not_found}
 * and messages/boosts (tmp/rust-ref/crates/views/templates/messages,
 * messages/{rs.rs,presentation.rs,support.rs}).
 *
 * Rich text arrives already sanitized (R02) in `message.text_html`; these
 * renderers only place it.  A render performs no SQL, mutation, filesystem or
 * network work.
 */
#include "views/internal.h"

#include <stdio.h>
#include <string.h>

/* `EmojiHelper::REACTIONS`. */
static const struct {
    const char *character;
    const char *title;
} REACTIONS[8] = {
    {"\xF0\x9F\x91\x8D", "Thumbs up"},
    {"\xF0\x9F\x91\x8F", "Clapping"},
    {"\xF0\x9F\x91\x8B", "Waving hand"},
    {"\xF0\x9F\x92\xAA", "Muscle"},
    {"\xE2\x9D\xA4\xEF\xB8\x8F", "Red heart"},
    {"\xF0\x9F\x98\x82", "Face with tears of joy"},
    {"\xF0\x9F\x8E\x89", "Party popper"},
    {"\xF0\x9F\x94\xA5", "Fire"},
};

/* ---------------------------------------------------------------- helpers */

/* `message.dom_id(prefix)`. */
static cf_err message_dom_id(const cf_view_message *message,
                             const char *prefix, cf_builder *out) {
    cf_err rc = CF_OK;
    if (prefix != NULL && *prefix != '\0') {
        rc = cf_view_str(out, prefix);
        if (rc == CF_OK) rc = cf_view_str(out, "_");
    }
    if (rc == CF_OK) rc = cf_view_str(out, "message_");
    if (rc == CF_OK) {
        rc = cf_view_raw(out, cf_str_span(message->client_message_id));
    }
    return rc;
}

/* `"/rooms/<room_id>/@<id>"`. */
static cf_err message_at_path(const cf_view_message *message,
                              cf_builder *out) {
    char buf[64];
    int n = snprintf(buf, sizeof buf, "/rooms/%lld/@%lld",
                     (long long)message->room_id, (long long)message->id);
    if (n < 0 || (size_t)n >= sizeof buf) return CF_INTERNAL;
    return cf_view_raw(out, (cf_span){(const unsigned char *)buf, (size_t)n});
}

/* `"/rooms/<room_id>/messages/<id><suffix>"`. */
static cf_err message_path(const cf_view_message *message, const char *suffix,
                           cf_builder *out) {
    char buf[96];
    int n = snprintf(buf, sizeof buf, "/rooms/%lld/messages/%lld%s",
                     (long long)message->room_id, (long long)message->id,
                     suffix);
    if (n < 0 || (size_t)n >= sizeof buf) return CF_INTERNAL;
    return cf_view_raw(out, (cf_span){(const unsigned char *)buf, (size_t)n});
}

static cf_err message_boosts_path(const cf_view_message *message,
                                  const char *suffix, cf_builder *out) {
    char buf[80];
    int n = snprintf(buf, sizeof buf, "/messages/%lld/boosts%s",
                     (long long)message->id, suffix);
    if (n < 0 || (size_t)n >= sizeof buf) return CF_INTERNAL;
    return cf_view_raw(out, (cf_span){(const unsigned char *)buf, (size_t)n});
}

static cf_err boost_path(const cf_view_boost *boost, cf_builder *out) {
    char buf[80];
    int n = snprintf(buf, sizeof buf, "/messages/%lld/boosts/%lld",
                     (long long)boost->message_id, (long long)boost->id);
    if (n < 0 || (size_t)n >= sizeof buf) return CF_INTERNAL;
    return cf_view_raw(out, (cf_span){(const unsigned char *)buf, (size_t)n});
}

static cf_err user_path(const cf_view_user *user, cf_builder *out) {
    char buf[40];
    int n = snprintf(buf, sizeof buf, "/users/%lld", (long long)user->id);
    if (n < 0 || (size_t)n >= sizeof buf) return CF_INTERNAL;
    return cf_view_raw(out, (cf_span){(const unsigned char *)buf, (size_t)n});
}

static cf_err write_iso(cf_builder *out, int64_t time_us) {
    char buf[21];
    cf_err rc = cf_view_iso8601(time_us, buf);
    if (rc != CF_OK) return rc;
    return cf_view_raw(out, (cf_span){(const unsigned char *)buf, 20});
}

static cf_err write_epoch(cf_builder *out, int64_t time_us) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%lld",
                     (long long)cf_view_epoch_ms(time_us));
    if (n < 0 || (size_t)n >= sizeof buf) return CF_INTERNAL;
    return cf_view_raw(out, (cf_span){(const unsigned char *)buf, (size_t)n});
}

/* `messages/_unrenderable.html` as `messages/_message.html` includes it: the
 * two-space indentation before the `{% include %}` is a literal of the parent
 * template (Askama emits it; verified with an askama 0.14 render of the same
 * comment/`{%- if %}`/include/`{%- else %}` shape), and the partial itself
 * carries no trailing newline. */
static cf_err unrenderable_partial(cf_builder *out) {
    return cf_view_str(
        out,
        "\n  <div class=\"message message--formatted message--failed center\">\n"
        "  <div class=\"message__body\">\n"
        "    <div class=\"message__body-content txt-align-center\">\n"
        "      Failed to load message content\n"
        "    </div>\n"
        "  </div>\n"
        "</div>");
}

/* ---------------------------------------------------------------- actions */

/* `messages/_actions.html`. */
static cf_err message_actions(const cf_view_ctx *ctx,
                              const cf_view_message *message,
                              cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder boosting_id = {0}, new_boost_id = {0}, edit_id = {0},
               boosts_path = {0}, new_boost_path = {0}, at_path = {0},
               edit_path = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(message_dom_id(message, "boosting", &boosting_id));
    CF_VIEW_TRY(message_dom_id(message, "new_boost", &new_boost_id));
    CF_VIEW_TRY(message_dom_id(message, "edit", &edit_id));
    CF_VIEW_TRY(message_boosts_path(message, "", &boosts_path));
    CF_VIEW_TRY(message_boosts_path(message, "/new", &new_boost_path));
    CF_VIEW_TRY(message_at_path(message, &at_path));
    CF_VIEW_TRY(message_path(message, "/edit", &edit_path));

    CF_VIEW_TRY(cf_view_str(
        out,
        "<div class=\"message__actions\" data-controller=\"soft-keyboard\">\n"
        "  <details class=\"position-relative\" data-controller=\"popup\" "
        "data-action=\"keydown.esc-&gt;popup#close "
        "toggle-&gt;popup#toggle "
        "click@document-&gt;popup#closeOnClickOutside\" "
        "data-popup-orientation-top-class=\"popup-orientation-top\">\n"
        "    <summary class=\"btn message__action-btn "
        "message__options-btn\">\n      "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "colorize--black"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 20));
        CF_VIEW_TRY(cf_view_image_tag(
            ctx, cf_span_of_lit("menu-dots-horizontal.svg"), &attrs, out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n      <span class=\"for-screen-reader\">Message "
        "options</span>\n"
        "    </summary>\n\n"
        "    <div class=\"message__actions-menu border shadow\" "
        "data-popup-target=\"menu\">\n"
        "      <div class=\"quick-boosts\">\n"));
    for (size_t i = 0; i < sizeof REACTIONS / sizeof REACTIONS[0]; i++) {
        CF_VIEW_TRY(cf_view_str(out, "        <form data-turbo-frame=\""));
        CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&boosting_id)));
        CF_VIEW_TRY(cf_view_str(
            out, "\" data-action=\"popup#close\" action=\""));
        CF_VIEW_TRY(cf_view_raw(out, cf_view_span_of(&boosts_path)));
        CF_VIEW_TRY(cf_view_str(
            out,
            "\" accept-charset=\"UTF-8\" method=\"post\">\n"
            "          <input type=\"hidden\" name=\"boost[content]\" "
            "id=\"boost_content\" value=\""));
        CF_VIEW_TRY(cf_view_text_cstr(out, REACTIONS[i].character));
        CF_VIEW_TRY(cf_view_str(
            out,
            "\" />\n"
            "          <button name=\"button\" type=\"submit\" title=\""));
        CF_VIEW_TRY(cf_view_text_cstr(out, REACTIONS[i].title));
        CF_VIEW_TRY(cf_view_str(
            out, "\" class=\"btn message__action-btn\" data-emoji=\""));
        CF_VIEW_TRY(cf_view_text_cstr(out, REACTIONS[i].character));
        CF_VIEW_TRY(cf_view_str(
            out,
            "\">\n"
            "            <figure class=\"margin-none boost-character\">"));
        CF_VIEW_TRY(cf_view_text_cstr(out, REACTIONS[i].character));
        CF_VIEW_TRY(cf_view_str(
            out,
            "</figure>\n"
            "            <span class=\"for-screen-reader\">"));
        CF_VIEW_TRY(cf_view_text_cstr(out, REACTIONS[i].title));
        CF_VIEW_TRY(cf_view_str(out, "</span>\n</button></form>\n"));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "        <a class=\"btn message__action-btn message__boost-btn\" "
        "data-turbo-frame=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&new_boost_id)));
    CF_VIEW_TRY(cf_view_str(
        out, "\" data-action=\"soft-keyboard#open popup#close\" href=\""));
    CF_VIEW_TRY(cf_view_raw(out, cf_view_span_of(&new_boost_path)));
    CF_VIEW_TRY(cf_view_str(out, "\">\n          "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "colorize--black"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 20));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("boost.svg"), &attrs,
                                      out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n          <span class=\"for-screen-reader\">New "
        "boost</span>\n"
        "</a>      </div>\n\n"
        "      <div class=\"flex flex-wrap border-top "
        "margin-block-start-half pad-block-start-half "
        "message__actions-grid\">\n"));
    if (message->content_kind == CF_VIEW_CONTENT_ATTACHMENT) {
        CF_VIEW_TRY(cf_view_str(
            out,
            "        <a class=\"btn message__action-btn center full-width "
            "hide-in-ios-pwa\" title=\"Download\" aria-label=\"Download\" "
            "href=\""));
        CF_VIEW_TRY(
            cf_view_html_attr(out, cf_str_span(message->attachment.download_path)));
        CF_VIEW_TRY(cf_view_str(out, "\">\n          "));
        {
            cf_view_attrs attrs;
            cf_view_attrs_init(&attrs);
            CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "colorize--black"));
            CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 20));
            CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("download.svg"),
                                          &attrs, out));
        }
        CF_VIEW_TRY(cf_view_str(
            out,
            "\n</a>\n"
            "        <button class=\"btn message__action-btn center "
            "full-width\" data-controller=\"web-share\" "
            "data-action=\"web-share#share\" data-web-share-files-value=\""));
        CF_VIEW_TRY(
            cf_view_html_attr(out, cf_str_span(message->attachment.blob_path)));
        CF_VIEW_TRY(cf_view_str(out, "\" data-web-share-title-value=\""));
        CF_VIEW_TRY(
            cf_view_html_attr(out, cf_str_span(message->attachment.filename)));
        CF_VIEW_TRY(cf_view_str(
            out, "\" title=\"Share\" aria-label=\"Share\">\n          "));
        {
            cf_view_attrs attrs;
            cf_view_attrs_init(&attrs);
            CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "colorize--black"));
            CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 20));
            CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("share.svg"),
                                          &attrs, out));
        }
        CF_VIEW_TRY(cf_view_str(out, "\n</button>\n"));
    } else {
        CF_VIEW_TRY(cf_view_str(
            out,
            "        <button class=\"btn message__action-btn center "
            "full-width\" data-action=\"reply#reply\" title=\"Reply\" "
            "aria-label=\"Reply\">\n          "));
        {
            cf_view_attrs attrs;
            cf_view_attrs_init(&attrs);
            CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "colorize--black"));
            CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 20));
            CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("reply.svg"),
                                          &attrs, out));
        }
        CF_VIEW_TRY(cf_view_str(out, "\n</button>\n"));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "        <button class=\"btn message__action-btn center "
        "full-width\" title=\"Copy link\" aria-label=\"Copy link\" "
        "data-controller=\"copy-to-clipboard\" "
        "data-action=\"copy-to-clipboard#copy\" "
        "data-copy-to-clipboard-success-class=\"btn--success\" "
        "data-copy-to-clipboard-url-value=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&at_path)));
    CF_VIEW_TRY(cf_view_str(out, "\">\n          "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "colorize--black"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 20));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("link.svg"), &attrs,
                                      out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n</button>\n"
        "        <a class=\"btn message__action-btn center full-width "
        "message__edit-btn\" data-turbo-frame=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&edit_id)));
    CF_VIEW_TRY(cf_view_str(out, "\" title=\"Edit\" aria-label=\"Edit\" href=\""));
    CF_VIEW_TRY(cf_view_raw(out, cf_view_span_of(&edit_path)));
    CF_VIEW_TRY(cf_view_str(out, "\">\n          "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "colorize--black"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 20));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("pencil.svg"), &attrs,
                                      out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n</a>      </div>\n"
        "    </div>\n"
        "</details></div>\n"));
    cf_builder_dispose(&boosting_id);
    cf_builder_dispose(&new_boost_id);
    cf_builder_dispose(&edit_id);
    cf_builder_dispose(&boosts_path);
    cf_builder_dispose(&new_boost_path);
    cf_builder_dispose(&at_path);
    cf_builder_dispose(&edit_path);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&boosting_id);
    cf_builder_dispose(&new_boost_id);
    cf_builder_dispose(&edit_id);
    cf_builder_dispose(&boosts_path);
    cf_builder_dispose(&new_boost_path);
    cf_builder_dispose(&at_path);
    cf_builder_dispose(&edit_path);
    return cf_view_fail(&guard, rc);
}

/* ---------------------------------------------------------- presentations */

/* `preview_dimensions`: the metadata size, scaled down to the thumbnail
 * bounds.  Sizes that already fit keep their Ruby type. */
static bool preview_dimensions(const cf_view_attachment *attachment,
                               cf_view_number *width, cf_view_number *height) {
    if (!attachment->has_width || !attachment->has_height) return false;
    cf_view_number w = attachment->width;
    cf_view_number h = attachment->height;
    double wf = w.is_float ? w.real : (double)w.integer;
    double hf = h.is_float ? h.real : (double)h.integer;
    if (wf <= 1200.0 && hf <= 800.0) {
        *width = w;
        *height = h;
        return true;
    }
    double scale = 1200.0 / wf;
    if (800.0 / hf < scale) scale = 800.0 / hf;
    *width = (cf_view_number){.is_float = true, .real = wf * scale};
    *height = (cf_view_number){.is_float = true, .real = hf * scale};
    return true;
}

/* `inline_media_dimension_constraints(content)` with dimensions present. */
static cf_err dimension_constraints(cf_view_number width,
                                    cf_view_number height,
                                    cf_span content, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(
        out,
        "<div class=\"max-inline-size center flex overflow-clip\" "
        "style=\"width: "));
    CF_VIEW_TRY(cf_view_number_format(cf_view_number_half(width), out));
    CF_VIEW_TRY(cf_view_str(out, "px; aspect-ratio: "));
    double wf = width.is_float ? width.real : (double)width.integer;
    double hf = height.is_float ? height.real : (double)height.integer;
    CF_VIEW_TRY(cf_view_float_format(wf / hf, out));
    CF_VIEW_TRY(cf_view_str(out, ";\">"));
    CF_VIEW_TRY(cf_view_raw(out, content));
    CF_VIEW_TRY(cf_view_str(out, "</div>"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

static cf_err no_dimension_constraints(cf_span content, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(
        out, "<div class=\"max-inline-size center overflow-clip\">"));
    CF_VIEW_TRY(cf_view_raw(out, content));
    CF_VIEW_TRY(cf_view_str(out, "</div>"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

static cf_err wrap_dimensions(const cf_view_attachment *attachment,
                              cf_span content, cf_builder *out) {
    cf_view_number width = {0}, height = {0};
    if (preview_dimensions(attachment, &width, &height)) {
        return dimension_constraints(width, height, content, out);
    }
    return no_dimension_constraints(content, out);
}

/* `Messages::AttachmentPresentation#render`. */
static cf_err attachment_presentation(const cf_view_ctx *ctx,
                                      const cf_view_attachment *attachment,
                                      cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    if (attachment->preview == CF_VIEW_PREVIEW_VIDEO) {
        cf_builder video = {0};
        rc = cf_view_str(&video, "<video src=\"");
        if (rc == CF_OK) {
            rc = cf_view_html_attr(&video, cf_str_span(attachment->blob_path));
        }
        if (rc == CF_OK) rc = cf_view_str(&video, "\" poster=\"");
        if (rc == CF_OK) {
            rc = cf_view_html_attr(&video, cf_str_span(attachment->preview_url));
        }
        if (rc == CF_OK) {
            rc = cf_view_str(&video,
                             "\" controls=\"controls\" preload=\"none\" "
                             "width=\"100%\" height=\"100%\" "
                             "class=\"message__attachment\"></video>");
        }
        if (rc == CF_OK) {
            rc = wrap_dimensions(attachment, cf_view_span_of(&video), out);
        }
        cf_builder_dispose(&video);
        if (rc != CF_OK) goto fail;
    } else if (attachment->preview == CF_VIEW_PREVIEW_IMAGE) {
        cf_builder link = {0};
        rc = cf_view_str(&link,
                         "<a class=\"flex\" data-lightbox-target=\"image\" "
                         "data-action=\"lightbox#open\" "
                         "data-lightbox-url-value=\"");
        if (rc == CF_OK) {
            rc = cf_view_html_attr(&link, cf_str_span(attachment->download_path));
        }
        if (rc == CF_OK) rc = cf_view_str(&link, "\" href=\"");
        if (rc == CF_OK) {
            rc = cf_view_html_attr(&link, cf_str_span(attachment->blob_path));
        }
        if (rc == CF_OK) rc = cf_view_str(&link, "\">");
        {
            cf_view_number width = {0}, height = {0};
            bool has_dims =
                preview_dimensions(attachment, &width, &height);
            if (rc == CF_OK) {
                rc = cf_view_str(&link, "<img");
            }
            if (rc == CF_OK && has_dims) {
                rc = cf_view_str(&link, " width=\"");
                if (rc == CF_OK) {
                    rc = cf_view_number_format(width, &link);
                }
                if (rc == CF_OK) rc = cf_view_str(&link, "\" height=\"");
                if (rc == CF_OK) {
                    rc = cf_view_number_format(height, &link);
                }
                if (rc == CF_OK) rc = cf_view_str(&link, "\"");
            }
            if (rc == CF_OK) {
                rc = cf_view_str(&link,
                                 " class=\"message__attachment\" "
                                 "loading=\"lazy\" src=\"");
            }
            if (rc == CF_OK) {
                rc = cf_view_html_attr(&link,
                                  cf_str_span(attachment->preview_url));
            }
            if (rc == CF_OK) rc = cf_view_str(&link, "\" />");
        }
        if (rc == CF_OK) rc = cf_view_str(&link, "</a>");
        if (rc == CF_OK) {
            rc = wrap_dimensions(attachment, cf_view_span_of(&link), out);
        }
        cf_builder_dispose(&link);
        if (rc != CF_OK) goto fail;
    } else {
        /* `render_link`: file icon, name, download link and share button,
         * with no whitespace between. */
        CF_VIEW_TRY(cf_view_str(
            out, "<div class=\"flex-inline align-center gap-half\">"));
        {
            cf_view_attrs attrs;
            cf_view_attrs_init(&attrs);
            CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "colorize--black"));
            CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 22));
            CF_VIEW_TRY(cf_view_image_tag(
                ctx, cf_span_of_lit("common-file-text.svg"), &attrs, out));
        }
        CF_VIEW_TRY(cf_view_str(out, "<span>"));
        CF_VIEW_TRY(cf_view_text(out, cf_str_span(attachment->filename)));
        CF_VIEW_TRY(cf_view_str(
            out,
            "</span>"
            "<a class=\"btn message__action-btn hide-in-ios-pwa\" "
            "style=\"--width: auto;\" href=\""));
        CF_VIEW_TRY(
            cf_view_html_attr(out, cf_str_span(attachment->download_path)));
        CF_VIEW_TRY(cf_view_str(out, "\">"));
        {
            cf_view_attrs attrs;
            cf_view_attrs_init(&attrs);
            CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 20));
            CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("download.svg"),
                                          &attrs, out));
        }
        CF_VIEW_TRY(cf_view_str(out,
                                "<span class=\"for-screen-reader\">Download "));
        CF_VIEW_TRY(cf_view_text(out, cf_str_span(attachment->filename)));
        CF_VIEW_TRY(cf_view_str(
            out,
            "</span></a>"
            "<button class=\"btn message__action-btn\" "
            "style=\"--width: auto;\" data-controller=\"web-share\" "
            "data-action=\"web-share#share\" "
            "data-web-share-files-value=\""));
        CF_VIEW_TRY(
            cf_view_html_attr(out, cf_str_span(attachment->download_path)));
        CF_VIEW_TRY(cf_view_str(out, "\">"));
        {
            cf_view_attrs attrs;
            cf_view_attrs_init(&attrs);
            CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 20));
            CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("share.svg"),
                                          &attrs, out));
        }
        CF_VIEW_TRY(cf_view_str(out,
                                "<span class=\"for-screen-reader\">Share "));
        CF_VIEW_TRY(cf_view_text(out, cf_str_span(attachment->filename)));
        CF_VIEW_TRY(cf_view_str(out, "</span></button></div>"));
    }
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* `message_sound_presentation`. */
static cf_err sound_presentation(const cf_view_sound *sound, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(
        out,
        "<div class=\"sound\" data-controller=\"sound\" "
        "data-action=\"messages:play-&gt;sound#play\" "
        "data-sound-url-value=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_str_span(sound->url)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\"><button class=\"btn btn--plain\" "
        "data-action=\"sound#play\">\xF0\x9F\x94\x8A</button>"));
    if (sound->has_image) {
        char buf[64];
        int n = snprintf(buf, sizeof buf,
                         "<img width=\"%lld\" height=\"%lld\" "
                         "class=\"align--middle\" src=\"",
                         (long long)sound->image.width,
                         (long long)sound->image.height);
        if (n < 0 || (size_t)n >= sizeof buf) {
            rc = CF_INTERNAL;
            goto fail;
        }
        CF_VIEW_TRY(cf_view_raw(out, (cf_span){
            (const unsigned char *)buf, (size_t)n}));
        CF_VIEW_TRY(cf_view_html_attr(out, cf_str_span(sound->image.src)));
        CF_VIEW_TRY(cf_view_str(out, "\" />"));
    } else if (sound->has_text) {
        CF_VIEW_TRY(cf_view_text(out, cf_str_span(sound->text)));
    }
    CF_VIEW_TRY(cf_view_str(out, "</div>"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* `MessagesHelper#message_presentation`. */
static cf_err presentation_html(const cf_view_ctx *ctx,
                                const cf_view_message *message,
                                cf_builder *out) {
    switch (message->content_kind) {
    case CF_VIEW_CONTENT_TEXT:
        return cf_view_raw(out, cf_str_span(message->text_html));
    case CF_VIEW_CONTENT_SOUND:
        return sound_presentation(&message->sound, out);
    case CF_VIEW_CONTENT_ATTACHMENT:
        return attachment_presentation(ctx, &message->attachment, out);
    case CF_VIEW_CONTENT_UNRENDERABLE:
    default:
        return CF_OK;
    }
}

/* --------------------------------------------------------------- partials */

/* `messages/_presentation.html`. */
static cf_err presentation_partial(const cf_view_ctx *ctx,
                                   const cf_view_message *message,
                                   cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder dom_id = {0}, body = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(message_dom_id(message, "presentation", &dom_id));
    rc = presentation_html(ctx, message, &body);
    if (rc == CF_OK) {
        rc = cf_view_str(out, "<div id=\"");
    }
    if (rc == CF_OK) rc = cf_view_html_attr(out, cf_view_span_of(&dom_id));
    if (rc == CF_OK) {
        rc = cf_view_str(out,
                         "\" dir=\"auto\" data-reply-target=\"body\" "
                         "data-messages-target=\"body\">\n  ");
    }
    if (rc == CF_OK) rc = cf_view_raw(out, cf_view_span_of(&body));
    if (rc == CF_OK) rc = cf_view_str(out, "\n</div>");
    cf_builder_dispose(&dom_id);
    cf_builder_dispose(&body);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&dom_id);
    cf_builder_dispose(&body);
    return cf_view_fail(&guard, rc);
}

/* `messages/boosts/_boost.html`. */
static cf_err boost_partial(const cf_view_ctx *ctx, const cf_view_boost *boost,
                            cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder dom_id = {0}, path = {0}, booster_path = {0}, label = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    {
        char buf[32];
        int n = snprintf(buf, sizeof buf, "boost_%lld", (long long)boost->id);
        if (n < 0 || (size_t)n >= sizeof buf) {
            rc = CF_INTERNAL;
            goto fail;
        }
        CF_VIEW_TRY(cf_view_raw(&dom_id, (cf_span){
            (const unsigned char *)buf, (size_t)n}));
    }
    CF_VIEW_TRY(boost_path(boost, &path));
    CF_VIEW_TRY(user_path(&boost->booster, &booster_path));
    /* `{{ booster.name }} boosted {{ boost.content }}`: built raw and
     * escaped once where the template escapes it. */
    rc = cf_view_raw(&label, cf_str_span(boost->booster.name));
    if (rc == CF_OK) rc = cf_view_str(&label, " boosted ");
    if (rc == CF_OK) rc = cf_view_raw(&label, cf_str_span(boost->content));
    if (rc != CF_OK) goto fail;

    CF_VIEW_TRY(cf_view_str(out, "  <div id=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&dom_id)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\"\n      class=\"boost boost-item flex-inline postion--relative "
        "max-width align-center fill-white gap\"\n"
        "      data-controller=\"boost-delete\" "
        "data-boost-delete-perform-class=\"boost--deleting\" "
        "data-boost-delete-reveal-class=\"expanded\" "
        "data-boost-delete-booster-id-value=\""));
    {
        char buf[24];
        int n = snprintf(buf, sizeof buf, "%lld",
                         (long long)boost->booster.id);
        if (n < 0 || (size_t)n >= sizeof buf) {
            rc = CF_INTERNAL;
            goto fail;
        }
        CF_VIEW_TRY(cf_view_raw(out, (cf_span){(const unsigned char *)buf,
                                               (size_t)n}));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\">\n"
        "    <figure class=\"avatar boost__avatar flex-item-no-shrink\">\n"
        "      <a title=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_str_span(boost->booster.title)));
    CF_VIEW_TRY(cf_view_str(out,
                            "\" class=\"btn avatar\" data-turbo-frame=\"_top\" "
                            "href=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&booster_path)));
    CF_VIEW_TRY(cf_view_str(out, "\"><img aria-label=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&label)));
    CF_VIEW_TRY(cf_view_str(out, "\" src=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_str_span(boost->booster.avatar_url)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\" width=\"48\" height=\"48\" /></a>\n"
        "    </figure>\n\n"
        "    <span role=\"button\" class=\""));
    CF_VIEW_TRY(cf_view_str(
        out, boost->all_emoji ? "txt-small txt-medium" : "txt-small"));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\" "
        "data-action=\"click-&gt;boost-delete#reveal "
        "keydown.enter-&gt;boost-delete#reveal:prevent\" "
        "data-boost-delete-target=\"content\">"));
    CF_VIEW_TRY(cf_view_text(out, cf_str_span(boost->content)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "</span>\n\n"
        "    <form class=\"button_to\" method=\"post\" action=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&path)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\"><input type=\"hidden\" name=\"_method\" value=\"delete\" />"
        "<button data-action=\"boost-delete#perform\" "
        "data-boost-delete-target=\"button\" class=\"btn btn--negative "
        "flex-item-justify-end boost__delete\" type=\"submit\">\n      "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 20));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("minus.svg"), &attrs,
                                      out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n      <span class=\"for-screen-reader\">Delete this "
        "boost</span>\n"
        "</button></form>  </div>\n"
        "  <span id=\"delete_boost_accessible_label\" "
        "class=\"for-screen-reader\">Press enter to delete this "
        "boost</span>\n"));
    cf_builder_dispose(&dom_id);
    cf_builder_dispose(&path);
    cf_builder_dispose(&booster_path);
    cf_builder_dispose(&label);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&dom_id);
    cf_builder_dispose(&path);
    cf_builder_dispose(&booster_path);
    cf_builder_dispose(&label);
    return cf_view_fail(&guard, rc);
}

/* `messages/boosts/_boosts.html`. */
static cf_err boosts_partial(const cf_view_ctx *ctx,
                             const cf_view_message *message,
                             cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder boosting_id = {0}, boosts_id = {0}, new_boost_id = {0},
               new_boost_path = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(message_dom_id(message, "boosting", &boosting_id));
    CF_VIEW_TRY(message_dom_id(message, "boosts", &boosts_id));
    CF_VIEW_TRY(message_dom_id(message, "new_boost", &new_boost_id));
    CF_VIEW_TRY(message_boosts_path(message, "/new", &new_boost_path));
    CF_VIEW_TRY(cf_view_str(out, "<turbo-frame id=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&boosting_id)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\">\n"
        "  <div class=\"boosts flex flex-wrap align-center gap full-width\" "
        "style=\"--column-gap: 0.4ch; --row-gap: 0\" "
        "data-controller=\"turbo-streaming\" "
        "data-action=\"turbo:submit-start-&gt;turbo-streaming#unsubscribe\">\n"
        "    <div class=\"flex-inline flex-wrap gap\" id=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&boosts_id)));
    CF_VIEW_TRY(cf_view_str(
        out, "\" data-turbo-streaming-target=\"container\">\n"));
    for (size_t i = 0; i < message->boosts.len; i++) {
        CF_VIEW_TRY(cf_view_str(out, "      "));
        CF_VIEW_TRY(boost_partial(ctx, &message->boosts.items[i], out));
    }
    CF_VIEW_TRY(cf_view_str(out, "    </div>\n\n    <turbo-frame id=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&new_boost_id)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\">\n"
        "      <div class=\"flex-inline message__boost-inline\" "
        "data-controller=\"soft-keyboard\">\n"
        "        <a class=\"boost__action txt-small btn\" "
        "action=\"soft-keyboard#open\" href=\""));
    CF_VIEW_TRY(cf_view_raw(out, cf_view_span_of(&new_boost_path)));
    CF_VIEW_TRY(cf_view_str(out, "\">\n          "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 20));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("boost.svg"), &attrs,
                                      out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n          <span class=\"for-screen-reader\">Add a "
        "boost</span>\n"
        "</a>      </div>\n"
        "    </turbo-frame>  </div>\n"
        "</turbo-frame>\n"));
    cf_builder_dispose(&boosting_id);
    cf_builder_dispose(&boosts_id);
    cf_builder_dispose(&new_boost_id);
    cf_builder_dispose(&new_boost_path);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&boosting_id);
    cf_builder_dispose(&boosts_id);
    cf_builder_dispose(&new_boost_id);
    cf_builder_dispose(&new_boost_path);
    return cf_view_fail(&guard, rc);
}

/* `messages/_message.html`. */
static cf_err message_partial(const cf_view_ctx *ctx,
                              const cf_view_message *message,
                              cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder dom_id = {0}, edit_id = {0}, creator_path = {0}, at_path = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    if (message->content_kind == CF_VIEW_CONTENT_UNRENDERABLE) {
        rc = unrenderable_partial(out);
        if (rc != CF_OK) return cf_view_fail(&guard, rc);
        return cf_view_finish(&guard);
    }
    CF_VIEW_TRY(message_dom_id(message, "", &dom_id));
    CF_VIEW_TRY(message_dom_id(message, "edit", &edit_id));
    CF_VIEW_TRY(user_path(&message->creator, &creator_path));
    CF_VIEW_TRY(message_at_path(message, &at_path));

    CF_VIEW_TRY(cf_view_str(out, "\n  <div id=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&dom_id)));
    CF_VIEW_TRY(cf_view_str(out, "\" class=\"message "));
    if (message->all_emoji) {
        CF_VIEW_TRY(cf_view_str(out, "message--emoji"));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\" data-controller=\"reply\" data-user-id=\""));
    {
        char buf[24];
        int n = snprintf(buf, sizeof buf, "%lld",
                         (long long)message->creator.id);
        if (n < 0 || (size_t)n >= sizeof buf) {
            rc = CF_INTERNAL;
            goto fail;
        }
        CF_VIEW_TRY(cf_view_raw(out, (cf_span){(const unsigned char *)buf,
                                               (size_t)n}));
    }
    CF_VIEW_TRY(cf_view_str(out, "\" data-message-id=\""));
    {
        char buf[24];
        int n = snprintf(buf, sizeof buf, "%lld", (long long)message->id);
        if (n < 0 || (size_t)n >= sizeof buf) {
            rc = CF_INTERNAL;
            goto fail;
        }
        CF_VIEW_TRY(cf_view_raw(out, (cf_span){(const unsigned char *)buf,
                                               (size_t)n}));
    }
    CF_VIEW_TRY(cf_view_str(out, "\" data-message-timestamp=\""));
    CF_VIEW_TRY(write_epoch(out, message->created_at_us));
    CF_VIEW_TRY(cf_view_str(out, "\" data-message-updated-at=\""));
    CF_VIEW_TRY(write_epoch(out, message->updated_at_us));
    CF_VIEW_TRY(cf_view_str(out, "\" data-sort-value=\""));
    CF_VIEW_TRY(write_epoch(out, message->created_at_us));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\" data-messages-target=\"message\" "
        "data-search-results-target=\"message\" "
        "data-refresh-room-target=\"message\" "
        "data-reply-composer-outlet=\"#composer\">\n"
        "    <h2 class=\"message__day-separator\"><time datetime=\""));
    CF_VIEW_TRY(write_iso(out, message->created_at_us));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\" data-local-time-target=\"date\"></time></h2>\n\n"
        "    <figure class=\"avatar message__avatar\">\n"
        "      <a title=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_str_span(message->creator.title)));
    CF_VIEW_TRY(cf_view_str(
        out, "\" class=\"btn avatar\" data-turbo-frame=\"_top\" href=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&creator_path)));
    CF_VIEW_TRY(cf_view_str(out, "\"><img aria-hidden=\"true\" src=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_str_span(message->creator.avatar_url)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\" width=\"48\" height=\"48\" /></a>\n"
        "    </figure>\n\n"
        "    <turbo-frame id=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&edit_id)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\">\n"
        "      <div class=\"message__body\">\n"
        "        <div class=\"message__body-content\">\n"
        "          <div class=\"message__meta\">\n"
        "            <h3 class=\"message__heading\">\n"
        "              <span class=\"message__author\" title=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_str_span(message->creator.title)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\">\n"
        "                <strong data-reply-target=\"author\">"));
    CF_VIEW_TRY(cf_view_text(out, cf_str_span(message->creator.name)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "</strong>\n"
        "              </span>\n"
        "              <a target=\"_top\" class=\"message__permalink\" "
        "href=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&at_path)));
    CF_VIEW_TRY(cf_view_str(
        out, "\"><time class=\"message__timestamp\" datetime=\""));
    CF_VIEW_TRY(write_iso(out, message->created_at_us));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\" data-local-time-target=\"time\"></time></a>\n"
        "              <span class=\"message__room\">\n"
        "                <a target=\"_top\" data-reply-target=\"link\" "
        "href=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&at_path)));
    CF_VIEW_TRY(cf_view_str(out, "\">"));
    CF_VIEW_TRY(cf_view_text(out, cf_str_span(message->room_name)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "</a>\n"
        "              </span>\n"
        "            </h3>\n"
        "            "));
    CF_VIEW_TRY(message_actions(ctx, message, out));
    CF_VIEW_TRY(cf_view_str(
        out,
        "          </div>\n"
        "          "));
    CF_VIEW_TRY(presentation_partial(ctx, message, out));
    CF_VIEW_TRY(cf_view_str(out, "          "));
    CF_VIEW_TRY(boosts_partial(ctx, message, out));
    CF_VIEW_TRY(cf_view_str(
        out,
        "        </div>\n"
        "      </div>\n"
        "    </turbo-frame>\n"
        "</div>"));
    cf_builder_dispose(&dom_id);
    cf_builder_dispose(&edit_id);
    cf_builder_dispose(&creator_path);
    cf_builder_dispose(&at_path);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&dom_id);
    cf_builder_dispose(&edit_id);
    cf_builder_dispose(&creator_path);
    cf_builder_dispose(&at_path);
    return cf_view_fail(&guard, rc);
}

/* `messages/_template.html`: the client-side template, raw inside a
 * `<script type="text/template">` (the golden tokenizer keeps its text
 * unde coded, so the escaping must match the reference's exactly). */
cf_err cf_view_messages_template(const cf_view_ctx *ctx,
                                 const cf_view_user *user, cf_builder *out) {
    if (user == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder user_path_buf = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(user_path(user, &user_path_buf));
    CF_VIEW_TRY(cf_view_str(
        out,
        "<script type=\"text/template\" data-messages-target=\"template\">\n"
        "  <div class=\"message message--me $messageClasses$\"\n"
        "      id=\"message_$clientMessageId$\"\n"
        "      data-format-message-target=\"message\"\n"
        "      data-user-id=\""));
    {
        char buf[24];
        int n = snprintf(buf, sizeof buf, "%lld", (long long)user->id);
        if (n < 0 || (size_t)n >= sizeof buf) {
            rc = CF_INTERNAL;
            goto fail;
        }
        CF_VIEW_TRY(cf_view_raw(out, (cf_span){(const unsigned char *)buf,
                                               (size_t)n}));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\"\n"
        "      data-message-timestamp=\"$messageTimestamp$\"\n"
        "      data-messages-target=\"message\">\n"
        "    <div class=\"message__day-separator\"><time "
        "class=\"message__timestamp\" datetime=\"$messageDatetime$\" "
        "data-local-time-target=\"date\"></time></div>\n\n"
        "    <figure class=\"avatar message__avatar\">\n"
        "      <a title=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_str_span(user->title)));
    CF_VIEW_TRY(cf_view_str(
        out, "\" class=\"btn avatar\" data-turbo-frame=\"_top\" href=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&user_path_buf)));
    CF_VIEW_TRY(cf_view_str(out, "\"><img aria-hidden=\"true\" src=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_str_span(user->avatar_url)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\" width=\"48\" height=\"48\" /></a>\n"
        "    </figure>\n\n"
        "    <div class=\"message__body\">\n"
        "      <div class=\"message__body-content\">\n"
        "        <div class=\"message__meta\">\n"
        "          <h3 class=\"message__heading\">\n"
        "            <span class=\"message__author\"><strong>"));
    CF_VIEW_TRY(cf_view_text(out, cf_str_span(user->name)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "</strong></span>\n"
        "            <span class=\"message__permalink\"><time "
        "class=\"message__timestamp\" datetime=\"$messageDatetime$\" "
        "data-local-time-target=\"time\"></time></span>\n"
        "          </h3>\n"
        "          <div class=\"message__actions\">\n"
        "            <div class=\"position-relative\">\n"
        "              <span class=\"btn message__action-btn "
        "message__options-btn\">\n"
        "                <img class=\"colorize--black\" aria-hidden=\"true\" "
        "src=\""));
    {
        cf_builder asset = {0};
        rc = cf_view_ctx_asset(ctx, cf_span_of_lit("menu-dots-horizontal.svg"),
                               &asset);
        if (rc == CF_OK) {
            rc = cf_view_html_attr(out, cf_view_span_of(&asset));
        }
        cf_builder_dispose(&asset);
        if (rc != CF_OK) goto fail;
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\" />\n"
        "                <span class=\"for-screen-reader\">Message "
        "options</span>\n"
        "              </span>\n"
        "            </div class=\"position-relative\">\n"
        "          </div>\n"
        "        </div>\n"
        "        $body$\n"
        "      </div>\n"
        "    </div>\n"
        "  </div>\n"
        "</script>"));
    cf_builder_dispose(&user_path_buf);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&user_path_buf);
    return cf_view_fail(&guard, rc);
}

/* ------------------------------------------------------------- public API */

cf_err cf_view_message_partial(const cf_view_ctx *ctx,
                               const cf_view_message *message,
                               cf_builder *out) {
    if (ctx == NULL || message == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = message_partial(ctx, message, out);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_message_item_partial(const cf_view_ctx *ctx,
                                    const cf_view_message_item *item,
                                    cf_builder *out) {
    if (ctx == NULL || item == NULL || out == NULL) return CF_INVALID;
    if (item->is_fragment) {
        return cf_view_raw(out, cf_str_span(item->fragment_html));
    }
    return cf_view_message_partial(ctx, &item->message, out);
}

cf_err cf_view_message_presentation(const cf_view_ctx *ctx,
                                    const cf_view_message *message,
                                    cf_builder *out) {
    if (ctx == NULL || message == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = presentation_partial(ctx, message, out);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_boost_partial(const cf_view_ctx *ctx,
                             const cf_view_boost *boost, cf_builder *out) {
    if (ctx == NULL || boost == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = boost_partial(ctx, boost, out);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_boosts_partial(const cf_view_ctx *ctx,
                              const cf_view_message *message,
                              cf_builder *out) {
    if (ctx == NULL || message == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = boosts_partial(ctx, message, out);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_message_index(const cf_view_ctx *ctx,
                             const cf_view_message_item *items, size_t count,
                             cf_builder *out) {
    if (ctx == NULL || out == NULL) return CF_INVALID;
    if (count != 0 && items == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    for (size_t i = 0; i < count; i++) {
        CF_VIEW_TRY(cf_view_str(out, "\n"));
        CF_VIEW_TRY(cf_view_message_item_partial(ctx, &items[i], out));
    }
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* The application layout around a content-only messages template. */
static cf_err messages_page(const cf_view_ctx *ctx, cf_span content,
                            cf_builder *out) {
    return cf_view_layout_page(ctx, (cf_span){NULL, 0}, false,
                               (cf_span){NULL, 0}, false, (cf_span){NULL, 0},
                               content, (cf_span){NULL, 0}, (cf_span){NULL, 0},
                               (cf_span){NULL, 0}, out);
}

cf_err cf_view_message_show(const cf_view_ctx *ctx,
                            const cf_view_message *message, cf_builder *out) {
    if (ctx == NULL || message == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder content = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = message_partial(ctx, message, &content);
    if (rc == CF_OK) {
        rc = messages_page(ctx, cf_view_span_of(&content), out);
    }
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

/* `messages/edit.html`: the turbo-frame editor, with the attachment variant. */
cf_err cf_view_message_edit(const cf_view_ctx *ctx,
                            const cf_view_message_edit_model *model,
                            cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    const cf_view_message *message = &model->message;
    cf_err rc;
    cf_view_guard guard;
    cf_builder content = {0}, edit_id = {0}, delete_id = {0}, form_id = {0},
               path = {0}, input_name = {0}, direct_upload = {0},
               blob_template = {0}, prompt_src = {0}, input_id = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(message_dom_id(message, "edit", &edit_id));
    CF_VIEW_TRY(message_dom_id(message, "delete_form", &delete_id));
    CF_VIEW_TRY(message_dom_id(message, "form", &form_id));
    CF_VIEW_TRY(message_dom_id(message, "", &input_id));
    CF_VIEW_TRY(message_path(message, "", &path));
    {
        rc = cf_view_str(&input_name, "message_body_trix_input_");
        if (rc == CF_OK) {
            rc = cf_view_raw(&input_name, cf_view_span_of(&input_id));
        }
        if (rc != CF_OK) goto fail;
    }
    if (rc == CF_OK) {
        rc = cf_view_ctx_url(
            ctx,
            cf_span_of_lit("/rails/active_storage/direct_uploads"),
            &direct_upload);
    }
    if (rc == CF_OK) {
        rc = cf_view_ctx_url(
            ctx,
            cf_span_of_lit(
                "/rails/active_storage/blobs/redirect/:signed_id/:filename"),
            &blob_template);
    }
    if (rc == CF_OK) {
        char buf[64];
        int n = snprintf(buf, sizeof buf,
                         "/autocompletable/users?room_id=%lld",
                         (long long)message->room_id);
        if (n < 0 || (size_t)n >= sizeof buf) {
            rc = CF_INTERNAL;
        } else {
            rc = cf_view_raw(&prompt_src, (cf_span){
                (const unsigned char *)buf, (size_t)n});
        }
    }
    if (rc != CF_OK) goto fail;

    CF_VIEW_TRY(cf_view_str(&content, "<turbo-frame id=\""));
    CF_VIEW_TRY(cf_view_html_attr(&content, cf_view_span_of(&edit_id)));
    CF_VIEW_TRY(cf_view_str(
        &content,
        "\">\n"
        "  <div class=\"message__body position-relative\" "
        "data-controller=\"scroll-into-view\">\n"
        "    <div class=\"message__body-content "
        "message__body-content--editing gap\">\n"));
    if (message->content_kind == CF_VIEW_CONTENT_ATTACHMENT) {
        CF_VIEW_TRY(cf_view_str(&content, "        "));
        CF_VIEW_TRY(attachment_presentation(ctx, &message->attachment,
                                            &content));
        CF_VIEW_TRY(cf_view_str(
            &content,
            "\n\n"
            "        <div class=\"message__edit-btns flex align-center "
            "justify-space-between gap full-width pad-block-start-half\">\n"
            "          <button name=\"button\" type=\"submit\" class=\"btn "
            "btn--negative center margin-block-end\" form=\""));
        CF_VIEW_TRY(cf_view_html_attr(&content, cf_view_span_of(&delete_id)));
        CF_VIEW_TRY(cf_view_str(
            &content,
            "\" data-turbo-confirm=\"Are you sure you want to delete this "
            "message?\">\n            "));
        {
            cf_view_attrs attrs;
            cf_view_attrs_init(&attrs);
            CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("trash.svg"),
                                          &attrs, &content));
        }
        CF_VIEW_TRY(cf_view_str(
            &content,
            "\n            <span class=\"for-screen-reader\">Delete "
            "message</span>\n"
            "</button>        </div>\n"));
    } else {
        CF_VIEW_TRY(cf_view_str(
            &content,
            "        <div class=\"composer--edit composer--rich-text\">\n"
            "          <form id=\""));
        CF_VIEW_TRY(cf_view_html_attr(&content, cf_view_span_of(&form_id)));
        CF_VIEW_TRY(cf_view_str(
            &content,
            "\" data-controller=\"form\" "
            "data-action=\"lexxy:file-accept-&gt;form#preventAttachment "
            "keydown.esc-&gt;form#cancel "
            "keydown.ctrl+enter-&gt;form#submit:prevent "
            "keydown.meta+enter-&gt;form#submit:prevent\" action=\""));
        CF_VIEW_TRY(cf_view_html_attr(&content, cf_view_span_of(&path)));
        CF_VIEW_TRY(cf_view_str(
            &content,
            "\" accept-charset=\"UTF-8\" method=\"post\"><input "
            "type=\"hidden\" name=\"_method\" value=\"patch\" />\n"
            "            <div class=\"full-width input input--actor min-width "
            "fill-white\">\n"
            "              <lexxy-editor rows=\"1\" class=\"input "
            "lexxy-content\" aria-multiline=\"true\" aria-label=\"Edit "
            "message\" autofocus=\"autofocus\" "
            "permitted-attachment-types=\"application/vnd.campfire.mention "
            "application/vnd.actiontext.opengraph-embed\" "
            "data-action=\"lexxy:change-&gt;typing-notifications#start "
            "keydown-&gt;composer#submitByKeyboard:capture\" "
            "data-direct-upload-url=\""));
        CF_VIEW_TRY(cf_view_html_attr(&content,
                                 cf_view_span_of(&direct_upload)));
        CF_VIEW_TRY(cf_view_str(&content, "\" data-blob-url-template=\""));
        CF_VIEW_TRY(cf_view_html_attr(&content,
                                 cf_view_span_of(&blob_template)));
        CF_VIEW_TRY(cf_view_str(
            &content,
            "\" id=\"message_body\" input=\""));
        CF_VIEW_TRY(cf_view_html_attr(&content, cf_view_span_of(&input_name)));
        CF_VIEW_TRY(cf_view_str(&content, "\" name=\"message[body]\" value=\""));
        CF_VIEW_TRY(cf_view_html_attr(&content,
                                 cf_str_span(model->editable_body_html)));
        CF_VIEW_TRY(cf_view_str(
            &content,
            "\">\n"
            "                <lexxy-prompt trigger=\"@\" name=\"mention\" "
            "src=\""));
        CF_VIEW_TRY(cf_view_html_attr(&content, cf_view_span_of(&prompt_src)));
        CF_VIEW_TRY(cf_view_str(
            &content,
            "\" remote-filtering=\"true\" empty-results=\"No "
            "matches\"></lexxy-prompt>\n"
            "</lexxy-editor>            </div>\n\n"
            "            <a data-form-target=\"cancel\" "
            "hidden=\"hidden\" href=\""));
        CF_VIEW_TRY(cf_view_html_attr(&content, cf_view_span_of(&path)));
        CF_VIEW_TRY(cf_view_str(
            &content,
            "\">Close editor and discard changes</a>\n\n"
            "            <div class=\"message__edit-btns flex align-center "
            "justify-space-between gap full-width pad-block-start-half\">\n"
            "              <button name=\"button\" type=\"submit\" "
            "class=\"btn btn--reversed\">\n                "));
        {
            cf_view_attrs attrs;
            cf_view_attrs_init(&attrs);
            CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("check.svg"),
                                          &attrs, &content));
        }
        CF_VIEW_TRY(cf_view_str(
            &content,
            "\n                <span class=\"for-screen-reader\">Save "
            "changes</span>\n"
            "</button>\n"
            "              <button name=\"button\" type=\"submit\" "
            "class=\"btn btn--negative\" form=\""));
        CF_VIEW_TRY(cf_view_html_attr(&content, cf_view_span_of(&delete_id)));
        CF_VIEW_TRY(cf_view_str(
            &content,
            "\" data-turbo-confirm=\"Are you sure you want to delete this "
            "message?\">\n                "));
        {
            cf_view_attrs attrs;
            cf_view_attrs_init(&attrs);
            CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("trash.svg"),
                                          &attrs, &content));
        }
        CF_VIEW_TRY(cf_view_str(
            &content,
            "\n                <span class=\"for-screen-reader\">Delete "
            "message</span>\n"
            "</button>            </div>\n"
            "</form>        </div>\n"));
    }
    CF_VIEW_TRY(cf_view_str(
        &content,
        "    </div>\n\n"
        "    <div class=\"message__actions flex flex-wrap\">\n"
        "      <a class=\"message__action-btn message__edit-close-btn "
        "txt-small btn btn--borderless\" href=\""));
    CF_VIEW_TRY(cf_view_html_attr(&content, cf_view_span_of(&path)));
    CF_VIEW_TRY(cf_view_str(
        &content,
        "\">\n        "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "colorize--black"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("remove.svg"),
                                      &attrs, &content));
    }
    CF_VIEW_TRY(cf_view_str(
        &content,
        "\n        <span class=\"for-screen-reader\">Close editor and "
        "discard changes</span>\n"
        "</a>    </div>\n\n"
        "    <form id=\""));
    CF_VIEW_TRY(cf_view_html_attr(&content, cf_view_span_of(&delete_id)));
    CF_VIEW_TRY(cf_view_str(&content, "\" data-turbo-frame=\""));
    CF_VIEW_TRY(cf_view_html_attr(&content, cf_view_span_of(&edit_id)));
    CF_VIEW_TRY(cf_view_str(&content, "\" action=\""));
    CF_VIEW_TRY(cf_view_html_attr(&content, cf_view_span_of(&path)));
    CF_VIEW_TRY(cf_view_str(
        &content,
        "\" accept-charset=\"UTF-8\" method=\"post\"><input type=\"hidden\" "
        "name=\"_method\" value=\"delete\" />\n"
        "  </div>\n"
        "</turbo-frame>\n"));
    rc = messages_page(ctx, cf_view_span_of(&content), out);
    cf_builder_dispose(&content);
    cf_builder_dispose(&edit_id);
    cf_builder_dispose(&delete_id);
    cf_builder_dispose(&form_id);
    cf_builder_dispose(&path);
    cf_builder_dispose(&input_name);
    cf_builder_dispose(&direct_upload);
    cf_builder_dispose(&blob_template);
    cf_builder_dispose(&prompt_src);
    cf_builder_dispose(&input_id);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&content);
    cf_builder_dispose(&edit_id);
    cf_builder_dispose(&delete_id);
    cf_builder_dispose(&form_id);
    cf_builder_dispose(&path);
    cf_builder_dispose(&input_name);
    cf_builder_dispose(&direct_upload);
    cf_builder_dispose(&blob_template);
    cf_builder_dispose(&prompt_src);
    cf_builder_dispose(&input_id);
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_message_create_stream(const cf_view_ctx *ctx,
                                     const cf_view_message_item *item,
                                     cf_room_type room_kind,
                                     cf_builder *out) {
    if (ctx == NULL || item == NULL || out == NULL) return CF_INVALID;
    static const char *const KEYS[] = {"rooms_open", "rooms_closed",
                                       "rooms_direct"};
    int64_t room_id = item->is_fragment ? item->fragment_room_id
                                        : item->message.room_id;
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    {
        char buf[80];
        int n = snprintf(buf, sizeof buf, "messages_%s_%lld",
                         KEYS[room_kind == CF_ROOM_CLOSED
                                  ? 1
                                  : (room_kind == CF_ROOM_DIRECT ? 2 : 0)],
                         (long long)room_id);
        if (n < 0 || (size_t)n >= sizeof buf) {
            rc = CF_INTERNAL;
            goto fail;
        }
        CF_VIEW_TRY(cf_view_str(out,
                                "<turbo-stream action=\"append\" target=\""));
        CF_VIEW_TRY(cf_view_raw(out, (cf_span){
            (const unsigned char *)buf, (size_t)n}));
    }
    CF_VIEW_TRY(cf_view_str(out, "\"><template>"));
    CF_VIEW_TRY(cf_view_message_item_partial(ctx, item, out));
    CF_VIEW_TRY(cf_view_str(out, "</template></turbo-stream>"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_message_destroy_stream(const cf_view_message *message,
                                      cf_builder *out) {
    if (message == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder dom_id = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(message_dom_id(message, "", &dom_id));
    CF_VIEW_TRY(cf_view_str(out, "<turbo-stream action=\"remove\" target=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&dom_id)));
    CF_VIEW_TRY(cf_view_str(out, "\"></turbo-stream>"));
    cf_builder_dispose(&dom_id);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&dom_id);
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_message_room_not_found(const cf_view_ctx *ctx, cf_builder *out) {
    if (ctx == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder content = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = cf_view_str(
        &content,
        "<turbo-frame id=\"composer-frame\">\n"
        "  <span class=\"composer__input input input--actor shake "
        "margin-block-end txt-negative txt-align-center\" "
        "style=\"--input-border-color: var(--color-negative)\">\n"
        "      <span>This room was deleted.</span>\n"
        "  </span>\n"
        "</turbo-frame>\n");
    if (rc == CF_OK) {
        rc = messages_page(ctx, cf_view_span_of(&content), out);
    }
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

/* --------------------------------------------------------- boosts pages */

cf_err cf_view_boosts_index(const cf_view_ctx *ctx,
                            const cf_view_message *message, cf_builder *out) {
    if (ctx == NULL || message == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder content = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = boosts_partial(ctx, message, &content);
    if (rc == CF_OK) {
        rc = messages_page(ctx, cf_view_span_of(&content), out);
    }
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_boosts_index_frame(const cf_view_ctx *ctx,
                                  const cf_view_message *message,
                                  cf_builder *out) {
    if (ctx == NULL || message == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder content = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = boosts_partial(ctx, message, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_frame(ctx, (cf_span){NULL, 0},
                                  cf_view_span_of(&content), out);
    }
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

/* The content of messages/boosts/new.html. */
static cf_err new_boost_content(const cf_view_ctx *ctx,
                                const cf_view_message *message,
                                const cf_view_user *user, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder new_boost_id = {0}, boosting_id = {0}, boosts_id = {0},
               boosts_path = {0}, user_path_buf = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(message_dom_id(message, "new_boost", &new_boost_id));
    CF_VIEW_TRY(message_dom_id(message, "boosting", &boosting_id));
    CF_VIEW_TRY(message_dom_id(message, "boosts", &boosts_id));
    CF_VIEW_TRY(message_boosts_path(message, "", &boosts_path));
    CF_VIEW_TRY(user_path(user, &user_path_buf));
    CF_VIEW_TRY(cf_view_str(out, "<turbo-frame id=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&new_boost_id)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\">\n"
        "  <div class=\"boost flex-inline postion--relative max-width "
        "fill-white\" style=\"--column-gap: var(--inline-space-half)\">\n"
        "    <form class=\"boost__form flex align-center gap expanded\" "
        "data-controller=\"form scroll-into-view\" data-turbo-frame=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&boosting_id)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\" data-action=\"keydown.esc-&gt;form#cancel\" action=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&boosts_path)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\" accept-charset=\"UTF-8\" method=\"post\">\n"
        "      <label class=\"boost__form-label flex gap\" "
        "style=\"--column-gap: 0.7ch;\" role=\"button\" tabindex=\"0\" "
        "aria-label=\"Add a boost\">\n"
        "        <figure class=\"avatar boost__avatar "
        "flex-item-no-shrink\">\n"
        "          <a title=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_str_span(user->title)));
    CF_VIEW_TRY(cf_view_str(
        out, "\" class=\"btn avatar\" data-turbo-frame=\"_top\" href=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&user_path_buf)));
    CF_VIEW_TRY(cf_view_str(out, "\"><img aria-hidden=\"true\" src=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_str_span(user->avatar_url)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\" width=\"48\" height=\"48\" /></a>\n"
        "          <span class=\"for-screen-reader\">"));
    CF_VIEW_TRY(cf_view_text(out, cf_str_span(user->name)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "</span>\n"
        "        </figure>\n\n"
        "        <input autofocus=\"autofocus\" autocomplete=\"off\" "
        "autocorrect=\"off\" maxlength=\"16\" required=\"required\" "
        "pattern=\"\\S+.*\" data-boost-form-target=\"input\" "
        "class=\"input input--boost txt-small\" size=\"16\" "
        "type=\"text\" name=\"boost[content]\" />\n"
        "      </label>\n\n"
        "      <button name=\"button\" type=\"submit\" class=\"btn "
        "btn--reversed\">\n        "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("check.svg"), &attrs,
                                      out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n        <span class=\"for-screen-reader\">Submit</span>\n"
        "</button>\n"
        "      <a data-turbo-frame=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&boosts_id)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\" data-form-target=\"cancel\" class=\"btn btn--negative\" "
        "href=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&boosts_path)));
    CF_VIEW_TRY(cf_view_str(out, "\">\n        "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("minus.svg"), &attrs,
                                      out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n        <span class=\"for-screen-reader\">Cancel</span>\n"
        "</a></form>  </div>\n"
        "</turbo-frame>\n"));
    cf_builder_dispose(&new_boost_id);
    cf_builder_dispose(&boosting_id);
    cf_builder_dispose(&boosts_id);
    cf_builder_dispose(&boosts_path);
    cf_builder_dispose(&user_path_buf);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&new_boost_id);
    cf_builder_dispose(&boosting_id);
    cf_builder_dispose(&boosts_id);
    cf_builder_dispose(&boosts_path);
    cf_builder_dispose(&user_path_buf);
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_new_boost(const cf_view_ctx *ctx,
                         const cf_view_message *message,
                         const cf_view_user *user, cf_builder *out) {
    if (ctx == NULL || message == NULL || user == NULL || out == NULL) {
        return CF_INVALID;
    }
    cf_err rc;
    cf_view_guard guard;
    cf_builder content = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = new_boost_content(ctx, message, user, &content);
    if (rc == CF_OK) {
        rc = messages_page(ctx, cf_view_span_of(&content), out);
    }
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_new_boost_frame(const cf_view_ctx *ctx,
                               const cf_view_message *message,
                               const cf_view_user *user, cf_builder *out) {
    if (ctx == NULL || message == NULL || user == NULL || out == NULL) {
        return CF_INVALID;
    }
    cf_err rc;
    cf_view_guard guard;
    cf_builder content = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = new_boost_content(ctx, message, user, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_frame(ctx, (cf_span){NULL, 0},
                                  cf_view_span_of(&content), out);
    }
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}
