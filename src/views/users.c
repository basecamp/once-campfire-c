/* src/views/users.c — users/new, users/show and autocompletable/users
 * (Phase 4 views packet V-A).
 *
 * Reference templates: tmp/rust-ref/crates/views/templates/users/new.html,
 * users/show.html, users/autocompletables/_template.html.  The show page
 * inlines two sub-partials owned by other packets exactly as Askama's
 * {% include %} would (marked INLINED below; no separate renderers are
 * defined here): users/_ban_button.html and users/profiles/_transfer.html.
 * users/new.html inlines accounts/_help_contact.html the same way (the
 * sessions/new renderer in src/views/session.c uses the same shape).
 *
 * Reference helpers: crates/views/src/helpers/{forms,filters,links,users,
 * assets,application}.rs; routes from tmp/rust-ref/crates/routes/src/lib.rs
 * (/join/:code, /session/new, /users/me/profile, /users/:id/ban,
 * /rooms/directs?user_ids[]=, /session/transfers/:id, /qr_code/:id).
 *
 * Ownership (00-contracts.md, 03-application.md A02): pure rendering only —
 * no SQL, mutation, filesystem or network.  Everything renders into the
 * caller's cf_builder under the 8 MiB cap; any failure leaves the builder at
 * its entry length (cf_view_guard).
 *
 * Known gaps (see the handoff): G1 cf_view_mention_user carries no
 * bio/title, so avatar titles render the bare name; G2 link_back has no
 * referrer in cf_view_ctx, so the show nav always targets root.
 */
#include "views/users_models.h"

#include "views/internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ small helpers */

static cf_span users_lit(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* urlsafe Base64 with padding (Base64.urlsafe_encode64), as rooms.c does
 * for the invite QR link. */
static cf_err users_urlsafe_base64(cf_span input, cf_builder *out) {
    static const char *const TABLE =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t i = 0;
    while (i + 2 < input.len) {
        unsigned value = ((unsigned)input.ptr[i] << 16) |
                         ((unsigned)input.ptr[i + 1] << 8) |
                         (unsigned)input.ptr[i + 2];
        char chunk[4] = {TABLE[(value >> 18) & 63], TABLE[(value >> 12) & 63],
                         TABLE[(value >> 6) & 63], TABLE[value & 63]};
        cf_err rc =
            cf_view_raw(out, (cf_span){(const unsigned char *)chunk, 4});
        if (rc != CF_OK) return rc;
        i += 3;
    }
    if (input.len - i == 1) {
        unsigned value = (unsigned)input.ptr[i] << 16;
        char chunk[4] = {TABLE[(value >> 18) & 63], TABLE[(value >> 12) & 63],
                         '=', '='};
        return cf_view_raw(out, (cf_span){(const unsigned char *)chunk, 4});
    }
    if (input.len - i == 2) {
        unsigned value = ((unsigned)input.ptr[i] << 16) |
                         ((unsigned)input.ptr[i + 1] << 8);
        char chunk[4] = {TABLE[(value >> 18) & 63], TABLE[(value >> 12) & 63],
                         TABLE[(value >> 6) & 63], '='};
        return cf_view_raw(out, (cf_span){(const unsigned char *)chunk, 4});
    }
    return CF_OK;
}

/* `ERB::Util.url_encode` (CGI.escape) for mail_to: unreserved bytes pass
 * through, space becomes `+`, the rest is %XX uppercase.  links.rs then
 * restores "@" (`replace("%40", "@")`), which keeping "@" literal matches
 * for every input. */
static cf_err users_cgi_escape(cf_span in, cf_builder *out) {
    static const char HEX[] = "0123456789ABCDEF";
    for (size_t i = 0; i < in.len; i++) {
        unsigned char c = in.ptr[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~' || c == '@') {
            cf_err rc = cf_builder_append(out, (cf_span){&in.ptr[i], 1});
            if (rc != CF_OK) return rc;
        } else if (c == ' ') {
            cf_err rc = cf_view_str(out, "+");
            if (rc != CF_OK) return rc;
        } else {
            char esc[3] = {'%', HEX[c >> 4], HEX[c & 15]};
            cf_err rc = cf_view_raw(out, (cf_span){
                                             (const unsigned char *)esc, 3});
            if (rc != CF_OK) return rc;
        }
    }
    return CF_OK;
}

/* `link_to(url, options) { content }`: the options in order, then href. */
static cf_err users_link_to(cf_span url, const cf_view_attrs *options,
                            cf_span content, cf_builder *out) {
    cf_view_attrs attrs;
    cf_view_attrs_init(&attrs);
    if (options != NULL) {
        for (size_t i = 0; i < options->count; i++) {
            cf_err rc =
                cf_view_attr_raw(&attrs, options->items[i].name,
                                 options->items[i].value);
            if (rc != CF_OK) return rc;
            /* cf_view_attr_raw keeps position but not flags; restore them
             * for boolean attributes (none of the callers pass flags, but
             * keep the merge honest). */
            attrs.items[attrs.count - 1].present = options->items[i].present;
            attrs.items[attrs.count - 1].trusted = options->items[i].trusted;
            attrs.items[attrs.count - 1].flag = options->items[i].flag;
        }
    }
    cf_err rc = cf_view_attr(&attrs, "href", url);
    if (rc != CF_OK) return rc;
    return cf_view_content(out, "a", &attrs, content);
}

/* `avatar_tag(user, options)`: a link to the user profile around the
 * 48px avatar image.  `title` is the caller's `User#title`. */
static cf_err users_avatar_tag(const cf_view_ctx *ctx, int64_t id,
                               cf_span title, cf_span avatar_path,
                               cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder image = {0};
    char id_buf[24];
    int n;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    n = snprintf(id_buf, sizeof id_buf, "%lld", (long long)id);
    if (n < 0 || (size_t)n >= sizeof id_buf) {
        rc = CF_INTERNAL;
        goto fail;
    }
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(
            cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 48));
        CF_VIEW_TRY(cf_view_image_tag(ctx, avatar_path, &img, &image));
    }
    {
        cf_builder href = {0};
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        rc = cf_view_str(&href, "/users/");
        if (rc == CF_OK) rc = cf_view_str(&href, id_buf);
        if (rc == CF_OK) {
            rc = cf_view_attr(&attrs, "title", title);
        }
        if (rc == CF_OK) {
            rc = cf_view_attr_cstr(&attrs, "class", "btn avatar");
        }
        if (rc == CF_OK) {
            rc = cf_view_attr_cstr(&attrs, "data-turbo-frame", "_top");
        }
        if (rc == CF_OK) {
            rc = cf_view_attr(&attrs, "href", cf_view_span_of(&href));
        }
        if (rc == CF_OK) {
            rc = cf_view_content(out, "a", &attrs, cf_view_span_of(&image));
        }
        cf_builder_dispose(&href);
        if (rc != CF_OK) goto fail;
    }
    cf_builder_dispose(&image);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&image);
    return cf_view_fail(&guard, rc);
}

/* `mail_to(email)`: the address CGI-escaped (keeping "@") in the href, the
 * plain address as the link text. */
static cf_err users_mail_to(cf_span email, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder href = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(&href, "mailto:"));
    CF_VIEW_TRY(users_cgi_escape(email, &href));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr(&attrs, "href", cf_view_span_of(&href)));
        CF_VIEW_TRY(cf_view_content_text(out, "a", &attrs, email));
    }
    cf_builder_dispose(&href);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&href);
    return cf_view_fail(&guard, rc);
}

/* `button_to_direct_room_with(user_id)`: the POST button to
 * rooms_directs_with_user around the bare messages image. */
static cf_err users_direct_room_button(const cf_view_ctx *ctx, int64_t id,
                                       cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder url = {0}, content = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    {
        char buf[64];
        int n = snprintf(buf, sizeof buf,
                         "/rooms/directs?user_ids%%5B%%5D=%lld",
                         (long long)id);
        if (n < 0 || (size_t)n >= sizeof buf) {
            rc = CF_INTERNAL;
            goto fail;
        }
        CF_VIEW_TRY(cf_view_raw(&url, (cf_span){
                                          (const unsigned char *)buf,
                                          (size_t)n,
                                      }));
    }
    {
        /* `image_tag(ctx, "messages.svg", attrs())`: no options, so the tag
         * carries only the resolved src. */
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(
            cf_view_image_tag(ctx, users_lit("messages.svg"), &img, &content));
    }
    {
        cf_view_attrs options;
        cf_view_attrs_init(&options);
        CF_VIEW_TRY(cf_view_attr_cstr(&options, "class",
                                      "btn btn--primary full-width txt--large"));
        CF_VIEW_TRY(cf_view_button_to(out, cf_view_span_of(&url), &options,
                                      users_lit("post"), (cf_span){NULL, 0},
                                      cf_view_span_of(&content)));
    }
    cf_builder_dispose(&url);
    cf_builder_dispose(&content);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&url);
    cf_builder_dispose(&content);
    return cf_view_fail(&guard, rc);
}

/* `link_back(ctx)`: GAP G2 — cf_view_ctx carries no referrer/request_url,
 * so the show nav always targets root.  (Reference: the referrer unless it
 * is missing or the current page.)  The link itself is built inline in
 * users_show_nav; this note marks the contract gap for the integrator. */

/* INLINED accounts/_help_contact.html (users/new.html's trailing include;
 * same shape as the sessions/new renderer). */
static cf_err users_help_contact(const cf_view_ctx *ctx,
                                 const cf_view_help_contact *owner,
                                 cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder link = {0}, href = {0}, title = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    CF_VIEW_TRY(cf_view_str(&href, "mailto:\""));
    CF_VIEW_TRY(cf_view_raw(&href, cf_str_span(owner->name)));
    CF_VIEW_TRY(cf_view_str(&href, "\" <"));
    CF_VIEW_TRY(cf_view_raw(&href, cf_str_span(owner->email_address)));
    CF_VIEW_TRY(cf_view_str(&href, ">"));
    CF_VIEW_TRY(cf_view_str(&link, "\n      "));
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, users_lit("lifebuoy.svg"), &img,
                                      &link));
    }
    CF_VIEW_TRY(cf_view_str(&link, "\n      <span>"));
    CF_VIEW_TRY(cf_view_text(&link, cf_str_span(owner->email_address)));
    CF_VIEW_TRY(cf_view_str(&link, "</span>\n"));
    CF_VIEW_TRY(cf_view_str(
        out, "  <div class=\"txt-align-center margin-block-double "
             "full-width\">\n    "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "btn center"));
        CF_VIEW_TRY(cf_view_str(&title, "Email "));
        CF_VIEW_TRY(cf_view_raw(&title, cf_str_span(owner->name)));
        CF_VIEW_TRY(cf_view_attr(&attrs, "title", cf_view_span_of(&title)));
        CF_VIEW_TRY(cf_view_attr(&attrs, "href", cf_view_span_of(&href)));
        CF_VIEW_TRY(cf_view_content(out, "a", &attrs, cf_view_span_of(&link)));
    }
    CF_VIEW_TRY(cf_view_str(
        out, "\n    <div class=\"txt-align-center center margin-block "
             "txt-subtle\">Campfire&trade; version "));
    CF_VIEW_TRY(cf_view_version_badge(ctx, out));
    CF_VIEW_TRY(cf_view_str(out, "</div>\n  </div>\n"));
    cf_builder_dispose(&link);
    cf_builder_dispose(&href);
    cf_builder_dispose(&title);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&link);
    cf_builder_dispose(&href);
    cf_builder_dispose(&title);
    return cf_view_fail(&guard, rc);
}

/* INLINED users/_ban_button.html (users/show.html's conditional include).
 * Active users get the POST ban form; banned users the DELETE unban form.
 * (Deactivated users render no ban block: the caller gates on active.) */
static cf_err users_ban_button(const cf_view_ctx *ctx, int64_t id,
                               cf_span name, bool active, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder url = {0}, content = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    {
        char buf[64];
        int n =
            snprintf(buf, sizeof buf, "/users/%lld/ban", (long long)id);
        if (n < 0 || (size_t)n >= sizeof buf) {
            rc = CF_INTERNAL;
            goto fail;
        }
        CF_VIEW_TRY(cf_view_raw(&url, (cf_span){(const unsigned char *)buf,
                                                (size_t)n}));
    }
    CF_VIEW_TRY(cf_view_str(&content, "\n    "));
    {
        cf_view_attrs img;
        cf_builder label = {0};
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        if (active) {
            rc = cf_view_str(&label, "Ban ");
        } else {
            rc = cf_view_str(&label, "Remove Ban ");
        }
        if (rc == CF_OK) rc = cf_view_raw(&label, name);
        if (rc == CF_OK) {
            rc = cf_view_attr(&img, "aria-label", cf_view_span_of(&label));
        }
        if (rc == CF_OK) {
            rc = cf_view_image_tag(ctx, users_lit("cancel.svg"), &img,
                                   &content);
        }
        /* The attribute borrows `label`: keep it alive through the tag. */
        cf_builder_dispose(&label);
        if (rc != CF_OK) goto fail;
    }
    if (active) {
        CF_VIEW_TRY(cf_view_str(&content, "\n    <span>Ban "));
        CF_VIEW_TRY(cf_view_text(&content, name));
        CF_VIEW_TRY(cf_view_str(&content, "</span>\n"));
    } else {
        CF_VIEW_TRY(cf_view_str(&content, "\n    <span>Remove ban</span>\n"));
    }
    CF_VIEW_TRY(cf_view_str(out, "  "));
    {
        cf_view_attrs options;
        cf_view_attrs_init(&options);
        if (active) {
            CF_VIEW_TRY(cf_view_attr_cstr(&options, "class", "btn full-width"));
            CF_VIEW_TRY(cf_view_attr_cstr(
                &options, "data-turbo-confirm",
                "Are you sure you want to ban this user? This will log them "
                "out, delete their messages, and block their IP addresses."));
            CF_VIEW_TRY(cf_view_button_to(out, cf_view_span_of(&url),
                                         &options, users_lit("post"),
                                         (cf_span){NULL, 0},
                                         cf_view_span_of(&content)));
        } else {
            CF_VIEW_TRY(cf_view_attr_cstr(&options, "class",
                                         "btn btn--negative full-width"));
            CF_VIEW_TRY(cf_view_attr_cstr(
                &options, "data-turbo-confirm",
                "Are you sure you want to remove the ban on this user?"));
            CF_VIEW_TRY(cf_view_button_to(out, cf_view_span_of(&url),
                                         &options, users_lit("delete"),
                                         (cf_span){NULL, 0},
                                         cf_view_span_of(&content)));
        }
    }
    cf_builder_dispose(&url);
    cf_builder_dispose(&content);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&url);
    cf_builder_dispose(&content);
    return cf_view_fail(&guard, rc);
}

/* INLINED users/profiles/_transfer.html (users/show.html's admin-only
 * include): the auto-login link field with its QR/copy/share controls. */
static cf_err users_transfer(const cf_view_ctx *ctx, int64_t user_id,
                             cf_span transfer_id, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder url = {0}, qr = {0};
    bool is_self = ctx->current_user.has_user &&
                   ctx->current_user.id == user_id;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    CF_VIEW_TRY(cf_view_ctx_url(ctx, users_lit("/session/transfers/"), &url));
    CF_VIEW_TRY(cf_view_raw(&url, transfer_id));
    CF_VIEW_TRY(cf_view_str(&qr, "/qr_code/"));
    CF_VIEW_TRY(users_urlsafe_base64(cf_view_span_of(&url), &qr));

    CF_VIEW_TRY(cf_view_str(
        out,
        "<fieldset>\n  <legend class=\"gap\">\n    "));
    {
        static const char *const ICONS[] = {"laptop.svg", "transfer.svg",
                                            "mobile-phone.svg"};
        for (size_t i = 0; i < 3; i++) {
            cf_view_attrs img;
            cf_view_attrs_init(&img);
            CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 36));
            CF_VIEW_TRY(
                cf_view_attr_cstr(&img, "class", "colorize--black"));
            CF_VIEW_TRY(cf_view_image_tag(ctx, users_lit(ICONS[i]), &img,
                                          out));
            CF_VIEW_TRY(cf_view_str(out, "\n    "));
        }
    }
    CF_VIEW_TRY(cf_view_str(out, "</legend>\n\n\n  <div class=\"flex "
                                 "flex-column gap\">\n"));
    if (!is_self) {
        CF_VIEW_TRY(cf_view_str(
            out, "      <div class=\"flex align-center gap justify-center\">\n"
                 "        "));
        {
            cf_view_attrs img;
            cf_view_attrs_init(&img);
            CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 16));
            CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_attr_cstr(&img, "class",
                                         "flex-item-no-shrink "
                                         "colorize--black"));
            CF_VIEW_TRY(
                cf_view_image_tag(ctx, users_lit("crown.svg"), &img, out));
        }
        CF_VIEW_TRY(cf_view_str(
            out, "\n        <label for=\"session_transfer_url\">Share to get "
                 "them back into their account</label>\n      </div>\n"));
    } else {
        CF_VIEW_TRY(cf_view_str(
            out, "      <label for=\"session_transfer_url\" "
                 "class=\"for-screen-reader\">Use this link to login "
                 "automatically on another device</label>\n"));
    }
    CF_VIEW_TRY(cf_view_str(out, "    <input type=\"text\" class=\"input\" "
                                 "value=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&url)));
    CF_VIEW_TRY(cf_view_str(
        out, "\" id=\"session_transfer_url\" readonly>\n\n"
             "    <div class=\"flex align-center center gap\">\n      "));
    /* link_to_zoom_qr_code(url) */
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "btn"));
        CF_VIEW_TRY(
            cf_view_attr_cstr(&attrs, "data-lightbox-target", "image"));
        CF_VIEW_TRY(
            cf_view_attr_cstr(&attrs, "data-action", "lightbox#open"));
        CF_VIEW_TRY(cf_view_attr(&attrs, "data-lightbox-url-value",
                                 cf_view_span_of(&qr)));
        CF_VIEW_TRY(cf_view_attr(&attrs, "href", cf_view_span_of(&qr)));
        {
            cf_builder body = {0};
            rc = cf_view_str(&body, "\n        <span "
                                    "class=\"for-screen-reader\">Show "
                                    "auto-login QR code</span>\n        ");
            if (rc == CF_OK) {
                cf_view_attrs img;
                cf_view_attrs_init(&img);
                rc = cf_view_attr_cstr(&img, "aria-hidden", "true");
                if (rc == CF_OK) rc = cf_view_attr_i64(&img, "size", 20);
                if (rc == CF_OK) {
                    rc = cf_view_attr_cstr(&img, "class", "colorize--black");
                }
                if (rc == CF_OK) {
                    rc = cf_view_image_tag(ctx, users_lit("qr-code.svg"),
                                           &img, &body);
                }
            }
            if (rc == CF_OK) rc = cf_view_str(&body, "\n");
            if (rc == CF_OK) {
                rc = cf_view_content(out, "a", &attrs,
                                     cf_view_span_of(&body));
            }
            cf_builder_dispose(&body);
            if (rc != CF_OK) goto fail;
        }
    }
    CF_VIEW_TRY(cf_view_str(out, "\n      "));
    /* button_to_copy_to_clipboard(url): a plain button, not a form. */
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "btn"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-controller",
                                      "copy-to-clipboard"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-action",
                                      "copy-to-clipboard#copy"));
        CF_VIEW_TRY(cf_view_attr_cstr(
            &attrs, "data-copy-to-clipboard-success-class", "btn--success"));
        CF_VIEW_TRY(cf_view_attr(&attrs,
                                 "data-copy-to-clipboard-content-value",
                                 cf_view_span_of(&url)));
        {
            cf_builder body = {0};
            rc = cf_view_str(&body, "\n        <span "
                                    "class=\"for-screen-reader\">Copy "
                                    "auto-login link</span>\n        ");
            if (rc == CF_OK) {
                cf_view_attrs img;
                cf_view_attrs_init(&img);
                rc = cf_view_attr_cstr(&img, "aria-hidden", "true");
                if (rc == CF_OK) rc = cf_view_attr_i64(&img, "size", 20);
                if (rc == CF_OK) {
                    rc = cf_view_attr_cstr(&img, "class",
                                           "flex-item-no-shrink "
                                           "colorize--black");
                }
                if (rc == CF_OK) {
                    rc = cf_view_image_tag(ctx, users_lit("copy-paste.svg"),
                                           &img, &body);
                }
            }
            if (rc == CF_OK) rc = cf_view_str(&body, "\n");
            if (rc == CF_OK) {
                rc = cf_view_content(out, "button", &attrs,
                                     cf_view_span_of(&body));
            }
            cf_builder_dispose(&body);
            if (rc != CF_OK) goto fail;
        }
    }
    CF_VIEW_TRY(cf_view_str(out, "\n      "));
    /* web_share_session_button(url, title, text) */
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "btn"));
        CF_VIEW_TRY(cf_view_attr_flag(&attrs, "hidden", true));
        CF_VIEW_TRY(
            cf_view_attr_cstr(&attrs, "data-controller", "web-share"));
        CF_VIEW_TRY(
            cf_view_attr_cstr(&attrs, "data-action", "web-share#share"));
        CF_VIEW_TRY(cf_view_attr(&attrs, "data-web-share-url-value",
                                 cf_view_span_of(&url)));
        CF_VIEW_TRY(cf_view_attr_cstr(
            &attrs, "data-web-share-text-value",
            "This is your own private sign-in URL, DO NOT SHARE IT. Use it "
            "to sign-in on another device or if you get locked out."));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-web-share-title-value",
                                      "Your sign-in link"));
        {
            cf_builder body = {0};
            rc = cf_view_str(&body, "\n        <span "
                                    "class=\"for-screen-reader\">Share "
                                    "auto-login link</span>\n        ");
            if (rc == CF_OK) {
                cf_view_attrs img;
                cf_view_attrs_init(&img);
                rc = cf_view_attr_cstr(&img, "aria-hidden", "true");
                if (rc == CF_OK) rc = cf_view_attr_i64(&img, "size", 20);
                if (rc == CF_OK) {
                    rc = cf_view_attr_cstr(&img, "class",
                                           "flex-item-no-shrink "
                                           "colorize--black");
                }
                if (rc == CF_OK) {
                    rc = cf_view_image_tag(ctx, users_lit("share.svg"), &img,
                                           &body);
                }
            }
            if (rc == CF_OK) rc = cf_view_str(&body, "\n");
            if (rc == CF_OK) {
                rc = cf_view_content(out, "button", &attrs,
                                     cf_view_span_of(&body));
            }
            cf_builder_dispose(&body);
            if (rc != CF_OK) goto fail;
        }
    }
    CF_VIEW_TRY(cf_view_str(out, "    </div>\n  </div>\n</fieldset>\n"));
    cf_builder_dispose(&url);
    cf_builder_dispose(&qr);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&url);
    cf_builder_dispose(&qr);
    return cf_view_fail(&guard, rc);
}

/* ------------------------------------------------------------ users/new */

/* `users/new.html`'s nav block: the sign-in link. */
static cf_err users_new_nav(const cf_view_ctx *ctx, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder link = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    CF_VIEW_TRY(cf_view_str(&link, "\n      "));
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, users_lit("login-keys.svg"), &img,
                                      &link));
    }
    CF_VIEW_TRY(cf_view_str(
        &link, "\n      <span class=\"for-screen-reader\">Sign in</span>\n"));
    CF_VIEW_TRY(cf_view_str(
        out, "  <div class=\"flex-item-justify-end\">\n    "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class",
                                      "btn flex-item-justify-end"));
        CF_VIEW_TRY(users_link_to(users_lit("/session/new"), &attrs,
                                  cf_view_span_of(&link), out));
    }
    CF_VIEW_TRY(cf_view_str(out, "  </div>\n"));
    cf_builder_dispose(&link);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&link);
    return cf_view_fail(&guard, rc);
}

/* `users/new.html`'s content block: the join form plus the help contact. */
static cf_err users_new_content(const cf_view_ctx *ctx,
                                const cf_view_users_new_model *model,
                                cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    /* The fields render first (into `body`) so the file field can turn the
     * form multipart, then the tag wraps the body (form_with block). */
    cf_builder body = {0}, button = {0}, action = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    cf_view_form form;
    cf_view_form_init(&form, &body);
    {
        cf_builder join = {0};
        rc = cf_view_str(&join, "/join/");
        if (rc == CF_OK) {
            rc = cf_view_raw(&join, cf_str_span(model->join_code));
        }
        if (rc == CF_OK) {
            /* The action URL is path-shaped; keep the bytes for the open
             * tag (lifetimes: `action` outlives the open call). */
            rc = cf_builder_append(&action, cf_view_span_of(&join));
        }
        cf_builder_dispose(&join);
        if (rc != CF_OK) goto fail;
    }
    form.action = cf_view_span_of(&action);
    form.param_key = "user";
    form.class_attr = "center";

    CF_VIEW_TRY(cf_view_str(
        &body,
        "\n  <section class=\"nametag u-relative\">\n"
        "    <div class=\"flex justify-center align-center pad-block\">\n"
        "      "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(
            cf_view_attr_cstr(&attrs, "class", "nametag__lanyard"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, users_lit("lanyard.svg"), &attrs,
                                      &body));
    }
    CF_VIEW_TRY(cf_view_str(
        &body,
        "\n    </div>\n\n"
        "    <div class=\"nametag__inner flex flex-column gap\">\n"
        "      <fieldset class=\"flex flex-column center-block\">\n"
        "        <legend class=\"txt-align-center flex gap\">\n"
        "          "));
    CF_VIEW_TRY(cf_view_account_logo(ctx, NULL, false, &body));
    CF_VIEW_TRY(cf_view_str(&body, "\n          <strong class=\"txt-large\">"));
    CF_VIEW_TRY(cf_view_text(&body, cf_str_span(ctx->account.name)));
    CF_VIEW_TRY(cf_view_str(
        &body,
        "</strong>\n        </legend>\n\n"
        "        <label class=\"align-center center avatar__form gap\" "
        "data-controller=\"upload-preview\">\n"
        "          <div class=\"btn input--file\">\n"
        "            "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, users_lit("camera.svg"), &attrs,
                                      &body));
    }
    CF_VIEW_TRY(cf_view_str(&body, "\n            "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "input"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "accept", "image/*"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-upload-preview-target",
                                      "input"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-action",
                                      "upload-preview#previewImage"));
        bool multipart = false;
        CF_VIEW_TRY(
            cf_view_form_file_field(&form, "avatar", &attrs, &multipart));
        if (!multipart) {
            rc = CF_INTERNAL;
            goto fail;
        }
    }
    CF_VIEW_TRY(cf_view_str(
        &body,
        "\n            <span class=\"for-screen-reader\">Upload "
        "avatar</span>\n"
        "          </div>\n\n"
        "          <div class=\"btn avatar input--file txt-xx-large\">\n"
        "            "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-upload-preview-target",
                                      "image"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, users_lit("default-avatar.svg"),
                                      &attrs, &body));
    }
    CF_VIEW_TRY(cf_view_str(
        &body,
        "\n            <span class=\"for-screen-reader\">Avatar</span>\n"
        "          </div>\n"
        "        </label>\n"
        "      </fieldset>\n\n"
        "      <div class=\"flex align-center gap\">\n"
        "        "));
    CF_VIEW_TRY(cf_view_translation_button(ctx, "user_name", &body));
    CF_VIEW_TRY(cf_view_str(
        &body,
        "\n        <label class=\"flex align-center gap flex-item-grow "
        "txt-large input input--actor\">\n"
        "          "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "input"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "autocomplete", "name"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "placeholder", "Name"));
        CF_VIEW_TRY(cf_view_attr_flag(&attrs, "autofocus", true));
        CF_VIEW_TRY(cf_view_attr_flag(&attrs, "required", true));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-1p-ignore", "true"));
        CF_VIEW_TRY(cf_view_form_field(&form, "text", "name",
                                       (cf_span){NULL, 0}, false, &attrs));
    }
    CF_VIEW_TRY(cf_view_str(&body, "\n          "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 24));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "colorize--black"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, users_lit("person.svg"), &attrs,
                                      &body));
    }
    CF_VIEW_TRY(cf_view_str(
        &body,
        "\n        </label>\n      </div>\n\n"
        "      <div class=\"flex align-center gap\">\n"
        "        "));
    CF_VIEW_TRY(cf_view_translation_button(ctx, "email_address", &body));
    CF_VIEW_TRY(cf_view_str(
        &body,
        "\n        <label class=\"flex align-center gap flex-item-grow "
        "txt-large input input--actor\">\n"
        "          "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "input"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "autocomplete", "username"));
        CF_VIEW_TRY(
            cf_view_attr_cstr(&attrs, "placeholder", "Email address"));
        CF_VIEW_TRY(cf_view_attr_flag(&attrs, "required", true));
        CF_VIEW_TRY(cf_view_form_field(&form, "email", "email_address",
                                       (cf_span){NULL, 0}, false, &attrs));
    }
    CF_VIEW_TRY(cf_view_str(&body, "\n          "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 24));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "colorize--black"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, users_lit("email.svg"), &attrs,
                                      &body));
    }
    CF_VIEW_TRY(cf_view_str(
        &body,
        "\n        </label>\n      </div>\n\n"
        "      <div class=\"flex align-center gap\">\n"
        "        "));
    CF_VIEW_TRY(cf_view_translation_button(ctx, "password", &body));
    CF_VIEW_TRY(cf_view_str(
        &body,
        "\n        <label class=\"flex align-center gap flex-item-grow "
        "txt-large input input--actor\">\n"
        "          "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "input"));
        CF_VIEW_TRY(
            cf_view_attr_cstr(&attrs, "autocomplete", "new-password"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "placeholder", "Password"));
        CF_VIEW_TRY(cf_view_attr_flag(&attrs, "required", true));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "maxlength", 72));
        CF_VIEW_TRY(cf_view_form_password_field(&form, "password", &attrs));
    }
    CF_VIEW_TRY(cf_view_str(&body, "\n          "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 24));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "colorize--black"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, users_lit("password.svg"), &attrs,
                                      &body));
    }
    CF_VIEW_TRY(cf_view_str(&body, "\n        </label>\n      </div>\n\n"
                                   "      "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_str(&button, "\n        "));
        {
            cf_view_attrs img;
            cf_view_attrs_init(&img);
            CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_image_tag(ctx, users_lit("check.svg"), &img,
                                          &button));
        }
        CF_VIEW_TRY(cf_view_str(
            &button, "\n        <span class=\"for-screen-reader\">Save</span>\n"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class",
                                      "btn btn--reversed center txt-large"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "type", "submit"));
        CF_VIEW_TRY(
            cf_view_button(&body, &attrs, cf_view_span_of(&button)));
    }
    CF_VIEW_TRY(cf_view_str(&body, "    </div>\n  </section>\n"));

    /* The multipart flag is known only now: render the form tag, the body,
     * then the closing tag (form_with's block behavior). */
    form.out = out;
    form.multipart = true;
    CF_VIEW_TRY(cf_view_form_open(&form));
    CF_VIEW_TRY(cf_view_raw(out, cf_view_span_of(&body)));
    CF_VIEW_TRY(cf_view_str(out, "</form>"));
    if (model->has_help_contact && model->help_contact != NULL) {
        CF_VIEW_TRY(cf_view_str(out, "\n"));
        CF_VIEW_TRY(users_help_contact(ctx, model->help_contact, out));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n"));
    cf_builder_dispose(&body);
    cf_builder_dispose(&button);
    cf_builder_dispose(&action);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&body);
    cf_builder_dispose(&button);
    cf_builder_dispose(&action);
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_users_new(const cf_view_ctx *ctx,
                         const cf_view_users_new_model *model,
                         cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_view_guard guard;
    cf_err rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_builder nav = {0}, content = {0};
    rc = users_new_nav(ctx, &nav);
    if (rc == CF_OK) rc = users_new_content(ctx, model, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_page(ctx, users_lit("Sign up"), true,
                                 users_lit("signup"), true,
                                 (cf_span){NULL, 0},
                                 cf_view_span_of(&content),
                                 cf_view_span_of(&nav), (cf_span){NULL, 0},
                                 (cf_span){NULL, 0}, out);
    }
    cf_builder_dispose(&nav);
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_users_new_frame(const cf_view_ctx *ctx,
                               const cf_view_users_new_model *model,
                               cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_view_guard guard;
    cf_err rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_builder content = {0};
    rc = users_new_content(ctx, model, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_frame(ctx, (cf_span){NULL, 0},
                                  cf_view_span_of(&content), out);
    }
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

/* ----------------------------------------------------------- users/show */

/* `users/show.html`'s nav block: the back link plus the profile-edit link
 * for the current user. */
static cf_err users_show_nav(const cf_view_ctx *ctx, int64_t user_id,
                             cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder back = {0}, edit = {0};
    bool is_self =
        ctx->current_user.has_user && ctx->current_user.id == user_id;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    /* link_back (GAP G2: root only): the image and label concatenate with
     * no surrounding whitespace (an expression, not a filter block). */
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 20));
        CF_VIEW_TRY(cf_view_image_tag(ctx, users_lit("arrow-left.svg"), &img,
                                      &back));
    }
    CF_VIEW_TRY(cf_view_str(
        &back, "<span class=\"for-screen-reader\">Go Back</span>"));
    CF_VIEW_TRY(cf_view_str(
        out, "  <div class=\"flex-item-justify-start\">\n    "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "btn"));
        CF_VIEW_TRY(
            users_link_to(users_lit("/"), &attrs, cf_view_span_of(&back),
                          out));
    }
    CF_VIEW_TRY(cf_view_str(
        out, "\n  </div>\n\n"
             "  <div class=\"flex align-center gap flex-item-justify-end\">\n"));
    if (is_self) {
        CF_VIEW_TRY(cf_view_str(&edit, "\n        "));
        {
            cf_view_attrs img;
            cf_view_attrs_init(&img);
            CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_image_tag(ctx, users_lit("pencil.svg"), &img,
                                          &edit));
        }
        CF_VIEW_TRY(cf_view_str(
            &edit, "\n        <span class=\"for-screen-reader\">Edit my "
                   "profile</span>\n"));
        CF_VIEW_TRY(cf_view_str(out, "      "));
        {
            cf_view_attrs attrs;
            cf_view_attrs_init(&attrs);
            CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "btn"));
            CF_VIEW_TRY(users_link_to(users_lit("/users/me/profile"), &attrs,
                                      cf_view_span_of(&edit), out));
        }
    }
    CF_VIEW_TRY(cf_view_str(out, "  </div>\n"));
    cf_builder_dispose(&back);
    cf_builder_dispose(&edit);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&back);
    cf_builder_dispose(&edit);
    return cf_view_fail(&guard, rc);
}

/* The ping button: button_to(rooms_directs_with_user(id)) around the
 * messages image with its "Ping <name>" label. */
static cf_err users_ping_button(const cf_view_ctx *ctx, int64_t id,
                                cf_span name, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder url = {0}, content = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    {
        char buf[64];
        int n = snprintf(buf, sizeof buf,
                         "/rooms/directs?user_ids%%5B%%5D=%lld",
                         (long long)id);
        if (n < 0 || (size_t)n >= sizeof buf) {
            rc = CF_INTERNAL;
            goto fail;
        }
        CF_VIEW_TRY(cf_view_raw(&url, (cf_span){(const unsigned char *)buf,
                                                (size_t)n}));
    }
    CF_VIEW_TRY(cf_view_str(&content, "\n              "));
    {
        cf_view_attrs img;
        cf_builder label = {0};
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        rc = cf_view_str(&label, "Ping ");
        if (rc == CF_OK) rc = cf_view_raw(&label, name);
        if (rc == CF_OK) {
            rc = cf_view_attr(&img, "aria-label",
                              cf_view_span_of(&label));
        }
        if (rc == CF_OK) {
            rc = cf_view_image_tag(ctx, users_lit("messages.svg"), &img,
                                   &content);
        }
        /* The attribute borrows `label`: keep it alive through the tag. */
        cf_builder_dispose(&label);
        if (rc != CF_OK) goto fail;
    }
    CF_VIEW_TRY(cf_view_str(&content, "\n"));
    {
        cf_view_attrs options;
        cf_view_attrs_init(&options);
        CF_VIEW_TRY(cf_view_attr_cstr(&options, "class",
                                      "btn btn--reversed full-width "
                                      "txt-large"));
        CF_VIEW_TRY(cf_view_button_to(out, cf_view_span_of(&url), &options,
                                      users_lit("post"), (cf_span){NULL, 0},
                                      cf_view_span_of(&content)));
    }
    cf_builder_dispose(&url);
    cf_builder_dispose(&content);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&url);
    cf_builder_dispose(&content);
    return cf_view_fail(&guard, rc);
}

/* `users/show.html`'s content block. */
static cf_err users_show_content(const cf_view_ctx *ctx,
                                 const cf_view_users_show_model *model,
                                 cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    bool is_bot = model->role == CF_ROLE_BOT;
    bool is_banned = model->status == CF_STATUS_BANNED;
    bool is_deactivated = model->status == CF_STATUS_DEACTIVATED;
    bool is_active = model->status == CF_STATUS_ACTIVE;
    bool can_administer = cf_view_ctx_can_administer(ctx);
    bool is_self =
        ctx->current_user.has_user && ctx->current_user.id == model->id;
    cf_span name = cf_str_span(model->name);
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    CF_VIEW_TRY(cf_view_str(out, "<section class=\"panel txt-align-center\">\n"
                                 "  <div class=\"flex flex-column gap"));
    CF_VIEW_TRY(cf_view_str(out, is_banned ? " banned" : " "));
    CF_VIEW_TRY(cf_view_str(
        out, "\">\n    <div class=\"avatar txt-xx-large center\" "
             "style=\"background: white\">\n      "));
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "alt", "Profile avatar"));
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "class", "avatar"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_str_span(model->avatar_path),
                                      &img, out));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n    </div>\n"));

    if (is_bot) {
        CF_VIEW_TRY(cf_view_str(
            out, "      <div class=\"pad-double--inline push--inline "
                 "push--block-start\">\n"));
        if (is_active) {
            CF_VIEW_TRY(cf_view_str(out, "          "));
            CF_VIEW_TRY(users_direct_room_button(ctx, model->id, out));
            CF_VIEW_TRY(cf_view_str(out, "\n"));
        } else {
            CF_VIEW_TRY(cf_view_str(out, "          <div>"));
            CF_VIEW_TRY(cf_view_text(out, name));
            CF_VIEW_TRY(cf_view_str(
                out, " is no longer on this account</div>\n"));
        }
        CF_VIEW_TRY(cf_view_str(out, "      </div>\n"));
    } else if (!is_deactivated) {
        CF_VIEW_TRY(cf_view_str(
            out,
            "        <div class=\"flex flex-column gap\" style=\"--row-gap: "
            "calc(var(--block-space) / 3)\">\n"
            "          <h1 class=\"txt-x-large txt-tight-lines "
            "margin-none\">"));
        CF_VIEW_TRY(cf_view_text(out, name));
        CF_VIEW_TRY(cf_view_str(out, "</h1>\n"));
        if (can_administer) {
            cf_span email = model->email_address.present
                                ? cf_str_span(model->email_address.value)
                                : (cf_span){NULL, 0};
            CF_VIEW_TRY(cf_view_str(out, "            <div>"));
            CF_VIEW_TRY(users_mail_to(email, out));
            CF_VIEW_TRY(cf_view_str(out, "</div>\n"));
        }
        CF_VIEW_TRY(cf_view_str(out, "          <div>"));
        if (model->bio.present) {
            CF_VIEW_TRY(cf_view_text(out, cf_str_span(model->bio.value)));
        }
        CF_VIEW_TRY(cf_view_str(out, "</div>\n        </div>\n"));
        if (is_active) {
            CF_VIEW_TRY(cf_view_str(
                out, "\n          <div class=\"pad-inline-double "
                     "margin-inline margin-block-start\">\n"
                     "            "));
            CF_VIEW_TRY(users_ping_button(ctx, model->id, name, out));
            CF_VIEW_TRY(cf_view_str(out, "          </div>\n"));
            if (can_administer) {
                CF_VIEW_TRY(cf_view_str(
                    out, "\n            <hr class=\"margin-block-start "
                         "borderless\">\n\n            "));
                CF_VIEW_TRY(users_transfer(ctx, model->id,
                                           cf_str_span(model->transfer_id),
                                           out));
                CF_VIEW_TRY(cf_view_str(out, "\n"));
            }
        }
        if (can_administer && !is_self) {
            CF_VIEW_TRY(cf_view_str(
                out, "          <div class=\"margin-block-start\">\n"
                     "            "));
            CF_VIEW_TRY(users_ban_button(ctx, model->id, name, is_active,
                                         out));
            CF_VIEW_TRY(cf_view_str(out, "\n          </div>\n"));
        }
    } else {
        CF_VIEW_TRY(cf_view_str(
            out, "        <div>\n          <h1 class=\"txt-x-large "
                 "margin-none\">"));
        CF_VIEW_TRY(cf_view_text(out, name));
        CF_VIEW_TRY(cf_view_str(out, "</h1>\n          <div>"));
        CF_VIEW_TRY(cf_view_text(out, name));
        CF_VIEW_TRY(
            cf_view_str(out, " is no longer on this account</div>\n"
                             "        </div>\n"));
    }
    CF_VIEW_TRY(cf_view_str(out, "  </div>\n</section>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_users_show(const cf_view_ctx *ctx,
                          const cf_view_users_show_model *model,
                          cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_view_guard guard;
    cf_err rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_builder nav = {0}, content = {0}, title = {0};
    rc = users_show_nav(ctx, model->id, &nav);
    if (rc == CF_OK) rc = users_show_content(ctx, model, &content);
    if (rc == CF_OK) {
        rc = cf_view_raw(&title, cf_str_span(model->name));
    }
    if (rc == CF_OK) {
        rc = cf_view_layout_page(ctx, cf_view_span_of(&title), true,
                                 (cf_span){NULL, 0}, false,
                                 (cf_span){NULL, 0},
                                 cf_view_span_of(&content),
                                 cf_view_span_of(&nav), (cf_span){NULL, 0},
                                 (cf_span){NULL, 0}, out);
    }
    cf_builder_dispose(&nav);
    cf_builder_dispose(&content);
    cf_builder_dispose(&title);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_users_show_frame(const cf_view_ctx *ctx,
                                const cf_view_users_show_model *model,
                                cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_view_guard guard;
    cf_err rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_builder content = {0};
    rc = users_show_content(ctx, model, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_frame(ctx, (cf_span){NULL, 0},
                                  cf_view_span_of(&content), out);
    }
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

/* ------------------------------------- autocompletable/users (no layout) */

/* INLINED users/_mention.html: the mention span the prompt item's editor
 * template carries. */
static cf_err users_mention(const cf_view_ctx *ctx,
                            const cf_view_mention_user *user, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(out, "<span class=\"mention\" sgid=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_str_span(user->attachable_sgid)));
    CF_VIEW_TRY(cf_view_str(out, "\">"));
    /* `User#title`, falling back to the bare name when the caller leaves
     * the title empty (closed G1: the action fills it from the row). */
    cf_span mention_title = user->title.len != 0
                                ? cf_str_span(user->title)
                                : cf_str_span(user->name);
    CF_VIEW_TRY(users_avatar_tag(ctx, user->id, mention_title,
                                 cf_str_span(user->avatar_path), out));
    CF_VIEW_TRY(cf_view_str(out, " "));
    CF_VIEW_TRY(cf_view_text(out, cf_str_span(user->name)));
    /* The template file ends in a blank line; the reference render keeps a
     * single trailing newline (as the standalone mention golden shows). */
    CF_VIEW_TRY(cf_view_str(out, "</span>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* `autocompletable/users/_prompt_item.html`: one <lexxy-prompt-item>. */
static cf_err users_prompt_item(const cf_view_ctx *ctx,
                                const cf_view_mention_user *user,
                                cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(out, "<lexxy-prompt-item search=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_str_span(user->name)));
    CF_VIEW_TRY(cf_view_str(out, "\" sgid=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_str_span(user->attachable_sgid)));
    CF_VIEW_TRY(cf_view_str(
        out, "\">\n  <template type=\"menu\">\n    <span "
             "class=\"autocomplete__item flex align-center gap unpad\">\n"
             "      "));
    /* `User#title`, falling back to the bare name (see users_mention). */
    cf_span item_title = user->title.len != 0 ? cf_str_span(user->title)
                                              : cf_str_span(user->name);
    CF_VIEW_TRY(users_avatar_tag(ctx, user->id, item_title,
                                 cf_str_span(user->avatar_path), out));
    CF_VIEW_TRY(cf_view_str(
        out, "\n      <span class=\"autocompletable__name\">"));
    CF_VIEW_TRY(cf_view_text(out, cf_str_span(user->name)));
    CF_VIEW_TRY(cf_view_str(out, "</span>\n    </span>\n  </template>\n  "
                                 "<template type=\"editor\">\n    "));
    CF_VIEW_TRY(users_mention(ctx, user, out));
    CF_VIEW_TRY(cf_view_str(out, "\n  </template>\n</lexxy-prompt-item>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_autocompletable_users_index(
    const cf_view_ctx *ctx, const cf_view_mention_user *users,
    size_t users_len, cf_builder *out) {
    if (ctx == NULL || out == NULL) return CF_INVALID;
    if (users_len != 0 && users == NULL) return CF_INVALID;
    cf_view_guard guard;
    cf_err rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    /* `render layout: false`: the prompt items only. */
    for (size_t i = 0; i < users_len; i++) {
        rc = users_prompt_item(ctx, &users[i], out);
        if (rc != CF_OK) return cf_view_fail(&guard, rc);
    }
    return cf_view_finish(&guard);
}

/* `users/autocompletables/_template.html`: the client-side prompt template
 * the mentions input carries (included by rooms/directs/new.html, so this
 * renderer is public for that packet's use; pending integrator approval). */
cf_err cf_view_autocompletable_template(const cf_view_ctx *ctx,
                                        cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(
        out,
        "<template id=\"autocompletable-user\">\n"
        "  <div class=\"autocomplete__pill max-width\" data-value=\"\" "
        "tabindex=\"0\">\n"
        "    <img class=\"avatar flex-item-no-shrink\" data-content=\"avatar\" "
        "src=\"\" />\n"
        "    <span class=\"autocomplete-field__selected-value-text "
        "overflow-ellipsis flex-item-grow\" "
        "data-content=\"label\"></span>\n\n"
        "    <button type=\"button\" "
        "data-action=\"autocomplete#remove:prevent\" data-value=\"\" "
        "tabindex=\"-1\" class=\"btn btn--plain txt-small translucent "
        "flex-item-no-shrink\">\n"
        "      "));
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "class", "colorize--black"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, users_lit("remove-circle.svg"),
                                      &img, out));
    }
    CF_VIEW_TRY(cf_view_str(
        out, "\n      <span class=\"for-screen-reader\">Remove <span "
             "data-content=\"screenReaderLabel\"></span></span>\n"
             "    </button>\n  </div>\n</template>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}
