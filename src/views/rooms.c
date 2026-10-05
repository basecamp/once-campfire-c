/* src/views/rooms.c — rooms/show.html and its partials
 * (tmp/rust-ref/crates/views/templates/rooms/, rooms.rs).
 *
 * The page is the application layout (or turbo-rails' frame layout) around the
 * message area: nav with the involvement bell, the client-side message
 * template, the message list, the signed room message stream and the
 * composer.  rooms#index has no template in the reference (it redirects to the
 * last room), so it has no renderer here.
 */
#include "views/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* `Array#to_sentence(names, " and ")`, then the direct-room fallback. */
cf_err cf_view_room_display_name(cf_span name, bool direct,
                                 const cf_span *other_member_names,
                                 size_t other_member_name_count,
                                 cf_span for_user_name, cf_str *out) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    cf_builder sentence = {0};
    cf_err rc;
    if (!direct) {
        if (name.ptr == NULL) return CF_OK;
        cf_str copy = {0};
        rc = cf_view_str_dup(name, &copy);
        if (rc != CF_OK) return rc;
        *out = copy;
        return CF_OK;
    }
    rc = cf_view_to_sentence(other_member_names, other_member_name_count,
                             cf_span_of_lit(" and "), &sentence);
    if (rc != CF_OK) {
        cf_builder_dispose(&sentence);
        return rc;
    }
    /* `sentence.trim().is_empty?` */
    size_t start = 0, end = sentence.len;
    while (start < end && (sentence.ptr[start] == ' ' ||
                           sentence.ptr[start] == '\t' ||
                           sentence.ptr[start] == '\n' ||
                           sentence.ptr[start] == '\r')) {
        start++;
    }
    while (end > start && (sentence.ptr[end - 1] == ' ' ||
                           sentence.ptr[end - 1] == '\t' ||
                           sentence.ptr[end - 1] == '\n' ||
                           sentence.ptr[end - 1] == '\r')) {
        end--;
    }
    cf_span chosen = end > start
                         ? (cf_span){sentence.ptr + start, end - start}
                         : (for_user_name.ptr != NULL ? for_user_name
                                                      : (cf_span){NULL, 0});
    if (chosen.ptr == NULL) {
        cf_builder_dispose(&sentence);
        out->ptr = malloc(1);
        if (out->ptr == NULL) return CF_NOMEM;
        out->ptr[0] = '\0';
        out->len = 0;
        return CF_OK;
    }
    rc = cf_view_str_dup(chosen, out);
    cf_builder_dispose(&sentence);
    return rc;
}

/* ------------------------------------------------------------ fragments */

/* `room_dom_id(kind, id, prefix)`: "prefix_rooms_open_1" or "rooms_open_1". */
static cf_err room_dom_id(const cf_view_room *room, const char *prefix,
                          cf_builder *out) {
    char id_buf[24];
    int n = snprintf(id_buf, sizeof id_buf, "%lld", (long long)room->id);
    if (n < 0 || (size_t)n >= sizeof id_buf) return CF_INTERNAL;
    static const char *const KEYS[] = {"rooms_open", "rooms_closed",
                                       "rooms_direct"};
    const char *key = KEYS[room->kind == CF_ROOM_CLOSED
                               ? 1
                               : (room->kind == CF_ROOM_DIRECT ? 2 : 0)];
    cf_err rc = CF_OK;
    if (prefix != NULL && *prefix != '\0') {
        rc = cf_view_str(out, prefix);
        if (rc == CF_OK) rc = cf_view_str(out, "_");
    }
    if (rc == CF_OK) rc = cf_view_str(out, key);
    if (rc == CF_OK) rc = cf_view_str(out, "_");
    if (rc == CF_OK) rc = cf_view_str(out, id_buf);
    return rc;
}

/* `RoomView#edit_path`. */
static cf_err room_edit_path(const cf_view_room *room, cf_builder *out) {
    char id_buf[24];
    int n = snprintf(id_buf, sizeof id_buf, "%lld", (long long)room->id);
    if (n < 0 || (size_t)n >= sizeof id_buf) return CF_INTERNAL;
    const char *kind = room->kind == CF_ROOM_OPEN
                           ? "/rooms/opens/"
                           : (room->kind == CF_ROOM_CLOSED
                                  ? "/rooms/closeds/"
                                  : "/rooms/directs/");
    cf_err rc = cf_view_str(out, kind);
    if (rc == CF_OK) rc = cf_view_str(out, id_buf);
    if (rc == CF_OK) rc = cf_view_str(out, "/edit");
    return rc;
}

/* `RoomsHelper#mention_prompt_tag`'s src. */
static cf_err mention_prompt_src(int64_t room_id, cf_builder *out) {
    char buf[64];
    int n = snprintf(buf, sizeof buf, "/autocompletable/users?room_id=%lld",
                     (long long)room_id);
    if (n < 0 || (size_t)n >= sizeof buf) return CF_INTERNAL;
    return cf_view_raw(out, (cf_span){(const unsigned char *)buf, (size_t)n});
}

/* urlsafe Base64 with padding (Base64.urlsafe_encode64). */
static cf_err urlsafe_base64(cf_span input, cf_builder *out) {
    static const char *const TABLE =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t i = 0;
    while (i + 2 < input.len) {
        unsigned value = ((unsigned)input.ptr[i] << 16) |
                         ((unsigned)input.ptr[i + 1] << 8) |
                         (unsigned)input.ptr[i + 2];
        char chunk[4] = {TABLE[(value >> 18) & 63], TABLE[(value >> 12) & 63],
                         TABLE[(value >> 6) & 63], TABLE[value & 63]};
        cf_err rc = cf_view_raw(out, (cf_span){(const unsigned char *)chunk, 4});
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

/* `<a href=...>` with the template's attribute sets. */
static cf_err link_tag(const cf_view_attrs *attrs, cf_span content,
                       cf_builder *out) {
    return cf_view_content(out, "a", attrs, content);
}

/* `accounts/_invite.html`: the join link, its QR/copy/share controls and the
 * regenerate button. */
static cf_err account_invite(const cf_view_ctx *ctx, cf_span join_code,
                             cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder url = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_ctx_url(ctx, cf_span_of_lit("/join/"), &url));
    CF_VIEW_TRY(cf_view_raw(&url, join_code));

    CF_VIEW_TRY(cf_view_str(
        out,
        "<div class=\"flex flex-column align-center gap\">\n"
        "  <label class=\"flex flex-column gap full-width\" "
        "style=\"--row-gap: 0.5em\">\n"
        "    <strong id=\"invite_label\" class=\"invite-label\">Share to "
        "invite more people</strong>\n"
        "    <span class=\"flex align-center gap input input--actor "
        "fill-white\">\n      "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 20));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "colorize--black"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("person-add.svg"),
                                      &attrs, out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n      <input type=\"text\" class=\"input\" id=\"invite_url\" "
        "value=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&url)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\" aria-labelledby=\"invite_label\" readonly>\n"
        "    </span>\n"
        "  </label>\n\n"
        "  <div class=\"flex align-center gap\">\n    "));
    /* link_to_zoom_qr_code(url) */
    {
        cf_builder qr = {0};
        rc = cf_view_str(&qr, "/qr_code/");
        if (rc == CF_OK) rc = urlsafe_base64(cf_view_span_of(&url), &qr);
        if (rc == CF_OK) {
            cf_view_attrs attrs;
            cf_view_attrs_init(&attrs);
            CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "btn"));
            CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-lightbox-target",
                                          "image"));
            CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-action",
                                          "lightbox#open"));
            CF_VIEW_TRY(cf_view_attr(&attrs, "data-lightbox-url-value",
                                     cf_view_span_of(&qr)));
            CF_VIEW_TRY(cf_view_attr(&attrs, "href", cf_view_span_of(&qr)));
            cf_builder body = {0};
            rc = cf_view_str(&body,
                             "\n      <span class=\"for-screen-reader\">Show "
                             "join link QR code</span>\n      ");
            if (rc == CF_OK) {
                cf_view_attrs img;
                cf_view_attrs_init(&img);
                rc = cf_view_attr_cstr(&img, "aria-hidden", "true");
                if (rc == CF_OK) rc = cf_view_attr_i64(&img, "size", 20);
                if (rc == CF_OK) {
                    rc = cf_view_attr_cstr(&img, "class", "colorize--black");
                }
                if (rc == CF_OK) {
                    rc = cf_view_image_tag(ctx, cf_span_of_lit("qr-code.svg"),
                                           &img, &body);
                }
            }
            if (rc == CF_OK) rc = cf_view_str(&body, "\n    ");
            if (rc == CF_OK) {
                rc = link_tag(&attrs, cf_view_span_of(&body), out);
            }
            cf_builder_dispose(&body);
        }
        cf_builder_dispose(&qr);
        if (rc != CF_OK) {
            cf_builder_dispose(&url);
            return cf_view_fail(&guard, rc);
        }
    }
    CF_VIEW_TRY(cf_view_str(out, "\n    "));
    /* button_to_copy_to_clipboard(url) */
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "btn"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-controller",
                                      "copy-to-clipboard"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-action",
                                      "copy-to-clipboard#copy"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs,
                                      "data-copy-to-clipboard-success-class",
                                      "btn--success"));
        CF_VIEW_TRY(cf_view_attr(&attrs,
                                 "data-copy-to-clipboard-content-value",
                                 cf_view_span_of(&url)));
        cf_builder body = {0};
        rc = cf_view_str(&body,
                         "\n      <span class=\"for-screen-reader\">Copy join "
                         "link</span>\n      ");
        if (rc == CF_OK) {
            cf_view_attrs img;
            cf_view_attrs_init(&img);
            rc = cf_view_attr_cstr(&img, "aria-hidden", "true");
            if (rc == CF_OK) rc = cf_view_attr_i64(&img, "size", 20);
            if (rc == CF_OK) {
                rc = cf_view_attr_cstr(&img, "class", "colorize--black");
            }
            if (rc == CF_OK) {
                rc = cf_view_image_tag(ctx, cf_span_of_lit("copy-paste.svg"),
                                       &img, &body);
            }
        }
        if (rc == CF_OK) rc = cf_view_str(&body, "\n    ");
        if (rc == CF_OK) rc = cf_view_content(out, "button", &attrs,
                                              cf_view_span_of(&body));
        cf_builder_dispose(&body);
        if (rc != CF_OK) {
            cf_builder_dispose(&url);
            return cf_view_fail(&guard, rc);
        }
    }
    CF_VIEW_TRY(cf_view_str(out, "\n    "));
    /* web_share_session_button(url, title, text) */
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "btn"));
        CF_VIEW_TRY(cf_view_attr_flag(&attrs, "hidden", true));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-controller", "web-share"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-action",
                                      "web-share#share"));
        CF_VIEW_TRY(cf_view_attr(&attrs, "data-web-share-url-value",
                                 cf_view_span_of(&url)));
        CF_VIEW_TRY(cf_view_attr_cstr(
            &attrs, "data-web-share-text-value",
            "Hit this link to join me in Campfire and start chatting."));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-web-share-title-value",
                                      "Link to join Campfire"));
        cf_builder body = {0};
        rc = cf_view_str(&body,
                         "\n      <span class=\"for-screen-reader\">Share "
                         "join link</span>\n      ");
        if (rc == CF_OK) {
            cf_view_attrs img;
            cf_view_attrs_init(&img);
            rc = cf_view_attr_cstr(&img, "aria-hidden", "true");
            if (rc == CF_OK) rc = cf_view_attr_i64(&img, "size", 20);
            if (rc == CF_OK) {
                rc = cf_view_attr_cstr(&img, "class", "colorize--black");
            }
            if (rc == CF_OK) {
                rc = cf_view_image_tag(ctx, cf_span_of_lit("share.svg"), &img,
                                       &body);
            }
        }
        if (rc == CF_OK) rc = cf_view_str(&body, "\n    ");
        if (rc == CF_OK) rc = cf_view_content(out, "button", &attrs,
                                              cf_view_span_of(&body));
        cf_builder_dispose(&body);
        if (rc != CF_OK) {
            cf_builder_dispose(&url);
            return cf_view_fail(&guard, rc);
        }
    }
    if (cf_view_ctx_can_administer(ctx)) {
        CF_VIEW_TRY(cf_view_str(out, "\n    "));
        cf_view_attrs button;
        cf_view_attrs_init(&button);
        CF_VIEW_TRY(cf_view_attr_cstr(&button, "class",
                                      "btn btn--regenerate"));
        cf_builder body = {0};
        rc = cf_view_str(&body, "\n        ");
        if (rc == CF_OK) {
            cf_view_attrs img;
            cf_view_attrs_init(&img);
            rc = cf_view_attr_cstr(&img, "aria-hidden", "true");
            if (rc == CF_OK) rc = cf_view_attr_i64(&img, "size", 20);
            if (rc == CF_OK) {
                rc = cf_view_attr_cstr(&img, "class", "colorize--black");
            }
            if (rc == CF_OK) {
                rc = cf_view_image_tag(ctx, cf_span_of_lit("refresh.svg"),
                                       &img, &body);
            }
        }
        if (rc == CF_OK) {
            rc = cf_view_str(&body,
                             "\n        <span "
                             "class=\"for-screen-reader\">Regenerate join "
                             "link</span>\n");
        }
        if (rc == CF_OK) {
            rc = cf_view_button_to(out, cf_span_of_lit("/account/join_code"),
                                   &button, (cf_span){NULL, 0},
                                   (cf_span){NULL, 0}, cf_view_span_of(&body));
        }
        cf_builder_dispose(&body);
        if (rc != CF_OK) {
            cf_builder_dispose(&url);
            return cf_view_fail(&guard, rc);
        }
    }
    CF_VIEW_TRY(cf_view_str(out, "\n  </div>\n</div>\n"));
    cf_builder_dispose(&url);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&url);
    return cf_view_fail(&guard, rc);
}

/* `rooms/show/_invitation.html`. */
static cf_err room_invitation(const cf_view_ctx *ctx, cf_span join_code,
                              cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(
        out,
        "  <div id=\"system_welcome\" class=\"message message--formatted "
        "txt-align-center center\">\n"
        "    <div class=\"message__body center\">\n"
        "      <div class=\"message__body-content position-relative\">\n"
        "        "));
    CF_VIEW_TRY(cf_view_account_logo(ctx, "center margin-block-end txt-large",
                                     true, out));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n        <div class=\"flex align-center gap\">\n"
        "          <div class=\"system-welcome--translation\">\n"
        "            "));
    CF_VIEW_TRY(cf_view_translation_button(ctx, "invite_message", out));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n          </div>\n"
        "          <p>\n"
        "            <strong>Welcome to Campfire</strong><br>\n"
        "            To invite people to chat, share the join link below.\n"
        "          </p>\n"
        "        </div>\n"
        "        "));
    CF_VIEW_TRY(account_invite(ctx, join_code, out));
    CF_VIEW_TRY(cf_view_str(
        out, "\n      </div>\n    </div>\n  </div>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* `rooms/involvements/_bell.html`. */
cf_err cf_view_room_bell(const cf_view_ctx *ctx, const cf_view_room *room,
                         cf_builder *out) {
    if (ctx == NULL || room == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder involvement_id = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(room_dom_id(room, "involvement", &involvement_id));
    CF_VIEW_TRY(cf_view_str(
        out,
        "<span>\n"
        "  <span class=\"button_to_change_notifying\" "
        "data-controller=\"notifications\" "
        "data-notifications-subscriptions-url-value=\"/users/me/"
        "push_subscriptions\" "
        "data-notifications-attention-class=\"btn--pulsing\">\n"
        "    <turbo-frame data-controller=\"turbo-frame\" "
        "data-action=\"notifications:ready@window->turbo-frame#load\" "
        "data-turbo-frame-url-param=\""));
    {
        char buf[64];
        int n = snprintf(buf, sizeof buf, "/rooms/%lld/involvement",
                         (long long)room->id);
        if (n < 0 || (size_t)n >= sizeof buf) {
            rc = CF_INTERNAL;
            goto fail;
        }
        CF_VIEW_TRY(cf_view_raw(out,
                                (cf_span){(const unsigned char *)buf,
                                          (size_t)n}));
    }
    CF_VIEW_TRY(cf_view_str(out, "\" id=\""));
    CF_VIEW_TRY(cf_view_raw(out, cf_view_span_of(&involvement_id)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\">\n"
        "      <button class=\"btn\" "
        "data-action=\"click->notifications#attemptToSubscribe\" "
        "data-notifications-target=\"bell\">\n        "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 20));
        CF_VIEW_TRY(cf_view_image_tag(
            ctx, cf_span_of_lit("notification-bell-loading.svg"), &attrs,
            out));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n        "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 20));
        CF_VIEW_TRY(cf_view_attr_flag(&attrs, "hidden", true));
        CF_VIEW_TRY(cf_view_image_tag(
            ctx, cf_span_of_lit("notification-bell-alert.svg"), &attrs, out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n        <span class=\"for-screen-reader\">Notification settings "
        "for this "));
    CF_VIEW_TRY(cf_view_str(out, room->kind == CF_ROOM_DIRECT ? "Ping"
                                                              : "room"));
    CF_VIEW_TRY(cf_view_str(
        out,
        "</span>\n"
        "      </button>\n"
        "    </turbo-frame>\n"
        "    <dialog data-notifications-target=\"notAllowedNotice\" "
        "class=\"dialog pad center center-block border-radius border shadow\" "
        "style=\"--inline-space: var(--block-space)\">\n"
        "      <div class=\"flex flex-column txt-align-center\">\n"
        "        <span class=\"btn btn--faux center txt-x-large\">\n          "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 48));
        CF_VIEW_TRY(cf_view_image_tag(
            ctx, cf_span_of_lit("notification-bell-alert.svg"), &attrs, out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n          <span class=\"for-screen-reader\">Notifications "
        "alert</span>\n"
        "        </span>\n\n"
        "        <section>\n"
        "          <h1 class=\"txt-large margin-none\">Notifications "
        "aren\xE2\x80\x99t allowed</h1>\n"
        "          <div class=\"txt-align-start margin-block-start\">\n"
        "            "));
    CF_VIEW_TRY(cf_view_pwa_browser_settings(ctx, out));
    CF_VIEW_TRY(cf_view_str(out, "\n            "));
    CF_VIEW_TRY(cf_view_pwa_system_settings(ctx, out));
    CF_VIEW_TRY(cf_view_str(out, "\n            "));
    CF_VIEW_TRY(cf_view_pwa_install_instructions(ctx, out));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n          </div>\n"
        "        </section>\n\n"
        "        <form method=\"dialog\" class=\"flex align-center gap "
        "center\">\n"
        "          <button class=\"btn dialog__close\" "
        "autofocus=\"true\">\n"
        "            <span class=\"for-screen-reader\">Close</span>\n"
        "            "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 20));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("remove.svg"), &attrs,
                                      out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n          </button>\n"
        "        </form>\n"
        "      </div>\n"
        "    </dialog>\n"
        "  </span>\n"
        "</span>\n"));
    cf_builder_dispose(&involvement_id);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&involvement_id);
    return cf_view_fail(&guard, rc);
}

/* ------------------------------------------------------------ room page */

/* `rooms/show/_nav.html`. */
static cf_err room_nav(const cf_view_ctx *ctx, const cf_view_room *room,
                       cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder edit_path = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    if (ctx->account.has_logo) {
        CF_VIEW_TRY(cf_view_str(out, "  "));
        CF_VIEW_TRY(cf_view_account_logo(ctx, NULL, false, out));
        CF_VIEW_TRY(cf_view_str(out, "\n\n"));
    } else {
        CF_VIEW_TRY(cf_view_str(out, "\n"));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "  <span class=\"btn btn--reversed btn--faux room--current\">\n"
        "    <h1 class=\"room__contents txt-medium overflow-ellipsis\">\n"));
    if (room->kind == CF_ROOM_DIRECT) {
        CF_VIEW_TRY(cf_view_str(
            out,
            "        <span class=\"for-screen-reader\">Ping with</span>\n"));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n      "));
    CF_VIEW_TRY(cf_view_text(out, cf_str_span(room->display_name)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n    </h1>\n"
        "</span>\n\n"
        "  <a class=\"btn\" style=\"view-transition-name: edit-room-"));
    {
        char buf[24];
        int n = snprintf(buf, sizeof buf, "%lld", (long long)room->id);
        if (n < 0 || (size_t)n >= sizeof buf) {
            rc = CF_INTERNAL;
            goto fail;
        }
        CF_VIEW_TRY(cf_view_raw(out, (cf_span){(const unsigned char *)buf,
                                               (size_t)n}));
    }
    CF_VIEW_TRY(cf_view_str(out, "\" data-room-id=\""));
    {
        char buf[24];
        int n = snprintf(buf, sizeof buf, "%lld", (long long)room->id);
        if (n < 0 || (size_t)n >= sizeof buf) {
            rc = CF_INTERNAL;
            goto fail;
        }
        CF_VIEW_TRY(cf_view_raw(out, (cf_span){(const unsigned char *)buf,
                                               (size_t)n}));
    }
    CF_VIEW_TRY(cf_view_str(out, "\" href=\""));
    rc = room_edit_path(room, &edit_path);
    if (rc != CF_OK) goto fail;
    CF_VIEW_TRY(cf_view_raw(out, cf_view_span_of(&edit_path)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\">\n    "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 20));
        CF_VIEW_TRY(cf_view_image_tag(ctx,
                                      cf_span_of_lit("menu-dots-horizontal.svg"),
                                      &attrs, out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n    <span class=\"for-screen-reader\">Settings for this "));
    CF_VIEW_TRY(cf_view_str(out, room->kind == CF_ROOM_DIRECT ? "Ping"
                                                              : "room"));
    CF_VIEW_TRY(cf_view_str(out, "</span>\n</a>\n\n  "));
    CF_VIEW_TRY(cf_view_room_bell(ctx, room, out));
    cf_builder_dispose(&edit_path);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&edit_path);
    return cf_view_fail(&guard, rc);
}

/* `rooms/show/_composer.html` (content_for :footer). */
static cf_err room_composer(const cf_view_ctx *ctx, const cf_view_room *room,
                            cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder action = {0}, direct_upload = {0}, blob_template = {0},
               prompt_src = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    {
        char buf[64];
        int n = snprintf(buf, sizeof buf, "/rooms/%lld/messages",
                         (long long)room->id);
        if (n < 0 || (size_t)n >= sizeof buf) {
            rc = CF_INTERNAL;
            goto fail;
        }
        rc = cf_view_str(&action, "/rooms/");
        if (rc == CF_OK) {
            int m = snprintf(buf, sizeof buf, "%lld/messages",
                             (long long)room->id);
            if (m < 0 || (size_t)m >= sizeof buf) {
                rc = CF_INTERNAL;
                goto fail;
            }
            rc = cf_view_raw(&action, (cf_span){(const unsigned char *)buf,
                                                (size_t)m});
        }
    }
    if (rc == CF_OK) {
        rc = cf_view_ctx_url(ctx, cf_span_of_lit("/rails/active_storage/direct_uploads"),
                             &direct_upload);
    }
    if (rc == CF_OK) {
        rc = cf_view_ctx_url(
            ctx,
            cf_span_of_lit("/rails/active_storage/blobs/redirect/:signed_id/:filename"),
            &blob_template);
    }
    if (rc == CF_OK) rc = mention_prompt_src(room->id, &prompt_src);
    if (rc != CF_OK) goto fail;

    CF_VIEW_TRY(cf_view_str(
        out,
        "  <div class=\"composer flex align-end gap position-relative\"\n"
        "      data-controller=\"typing-notifications\" "
        "data-typing-notifications-active-class=\"typing-indicator--active\">\n"
        "    <a class=\"btn flex-item-no-shrink margin-block-end "
        "composer__context-btn\" style=\"view-transition-name: input-switcher\" "
        "href=\"/searches\">\n      "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 20));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("search.svg"), &attrs,
                                      out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n      <span class=\"for-screen-reader\">Search</span>\n"
        "</a>\n"
        "    <turbo-frame id=\"composer-frame\">\n"
        "      <form id=\"composer\" class=\"margin-block flex-item-grow "
        "contain\" data-controller=\"composer drop-target\" "
        "data-action=\"dragenter-&gt;drop-target#dragenter "
        "dragover-&gt;drop-target#dragover "
        "drop-&gt;drop-target#drop "
        "drop-target:drop@window-&gt;composer#dropFiles "
        "lexxy:file-accept-&gt;composer#preventAttachment "
        "refresh-room:online@window-&gt;composer#online "
        "typing-notifications#stop paste-&gt;composer#pasteFiles "
        "turbo:submit-end-&gt;composer#submitEnd "
        "refresh-room:offline@window-&gt;composer#offline\" "
        "data-composer-messages-outlet=\"#message-area\" "
        "data-composer-toolbar-class=\"composer--rich-text\" "
        "data-composer-room-id-value=\""));
    {
        char buf[24];
        int n = snprintf(buf, sizeof buf, "%lld", (long long)room->id);
        if (n < 0 || (size_t)n >= sizeof buf) {
            rc = CF_INTERNAL;
            goto fail;
        }
        CF_VIEW_TRY(cf_view_raw(out, (cf_span){(const unsigned char *)buf,
                                               (size_t)n}));
    }
    CF_VIEW_TRY(cf_view_str(out, "\" action=\""));
    CF_VIEW_TRY(cf_view_raw(out, cf_view_span_of(&action)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\" accept-charset=\"UTF-8\" method=\"post\">\n"
        "        <fieldset data-composer-target=\"fields\" contents=\"\">\n"
        "          <div class=\"flex flex-column\">\n"
        "            <div class=\"composer__filelist flex flex--align-center "
        "gap flex-wrap\" data-composer-target=\"fileList\"></div>\n\n"
        "            <div class=\"flex composer__input input input--actor "
        "fill-white min-width\" style=\"--input-border-radius: 1.3rem\">\n"
        "              <div class=\"flex align-end gap full-width\">\n"
        "                "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class",
                                      "composer__input-hint colorize--black"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "style",
                                      "view-transition-name: input-btn;"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 22));
        CF_VIEW_TRY(cf_view_image_tag(
            ctx, cf_span_of_lit("messages-outlined.svg"), &attrs, out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n\n                <div class=\"flex flex-column flex-item-grow "
        "min-width gap\">\n"
        "                  <lexxy-editor rows=\"1\" class=\"input "
        "lexxy-content\" style=\"order: -1\" aria-multiline=\"true\" "
        "aria-label=\"Write a message\" "
        "permitted-attachment-types=\"application/vnd.campfire.mention "
        "application/vnd.actiontext.opengraph-embed\" "
        "data-controller=\"unfurl\" "
        "data-action=\"lexxy:change-&gt;typing-notifications#start "
        "keydown-&gt;composer#submitByKeyboard:capture "
        "lexxy:change-&gt;composer#saveDraft "
        "lexxy:insert-link-&gt;unfurl#unfurl\" "
        "data-composer-target=\"text\" data-direct-upload-url=\""));
    CF_VIEW_TRY(cf_view_raw(out, cf_view_span_of(&direct_upload)));
    CF_VIEW_TRY(cf_view_str(out, "\" data-blob-url-template=\""));
    CF_VIEW_TRY(cf_view_raw(out, cf_view_span_of(&blob_template)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\" id=\"message_body\" input=\"message_body_trix_input_message\" "
        "name=\"message[body]\">\n"
        "                    <lexxy-prompt trigger=\"@\" name=\"mention\" "
        "src=\""));
    CF_VIEW_TRY(cf_view_raw(out, cf_view_span_of(&prompt_src)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\" remote-filtering=\"true\" empty-results=\"No "
        "matches\"></lexxy-prompt>\n"
        "</lexxy-editor>                </div>\n\n"
        "                <label class=\"btn btn--borderless txt-small "
        "flex-item-no-shrink composer__attachment-btn input--file\">\n"
        "                  "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "colorize--black"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 22));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("attachment.svg"),
                                      &attrs, out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n                  <input type=\"file\" "
        "data-action=\"composer#filePicked\" multiple=\"\" />\n"
        "                  <span class=\"for-screen-reader\">Attach a "
        "file</span>\n"
        "                </label>\n\n"
        "                <button class=\"btn btn--borderless txt-small "
        "flex-item-no-shrink composer__rich-text-btn\" type=\"button\" "
        "data-action=\"composer#toggleToolbar\">\n"
        "                  "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "colorize--black"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 20));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("text-options.svg"),
                                      &attrs, out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n                  <span class=\"for-screen-reader\">Rich "
        "text</span>\n"
        "                </button>\n\n"
        "                <button name=\"send\" type=\"submit\" "
        "data-action=\"composer#submit\" class=\"btn btn--reversed "
        "flex-item-no-shrink txt-small\">\n"
        "                  "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 20));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("arrow-up.svg"),
                                      &attrs, out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n                  <span class=\"for-screen-reader\">Send "
        "Message</span>\n"
        "</button>              </div>\n"
        "            </div>\n"
        "          </div>\n"
        "        </fieldset>\n\n"
        "        <div class=\"typing-indicator gap txt-small align-center "
        "flex-inline\" data-typing-notifications-target=\"indicator\">\n"
        "          <div class=\"typing-indicator__author spinner\" "
        "data-typing-notifications-target=\"author\"></div>\n"
        "        </div>\n\n"
        "        <input data-composer-target=\"clientid\" type=\"hidden\" "
        "name=\"message[client_message_id]\" "
        "id=\"message_client_message_id\" />\n"
        "</form>    </turbo-frame>\n"
        "  </div>\n"));
    cf_builder_dispose(&action);
    cf_builder_dispose(&direct_upload);
    cf_builder_dispose(&blob_template);
    cf_builder_dispose(&prompt_src);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&action);
    cf_builder_dispose(&direct_upload);
    cf_builder_dispose(&blob_template);
    cf_builder_dispose(&prompt_src);
    return cf_view_fail(&guard, rc);
}

/* The `content` block: the message area with the template, the page of
 * messages, the signed stream and the return-to-latest button. */
static cf_err room_content(const cf_view_ctx *ctx,
                           const cf_view_room_show_model *model,
                           cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder messages_id = {0}, page_url = {0}, refresh_url = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = room_dom_id(&model->room, "messages", &messages_id);
    if (rc == CF_OK) {
        char buf[64];
        int n = snprintf(buf, sizeof buf, "/rooms/%lld/messages",
                         (long long)model->room.id);
        if (n < 0 || (size_t)n >= sizeof buf) {
            rc = CF_INTERNAL;
        } else {
            rc = cf_view_ctx_url(ctx, (cf_span){
                (const unsigned char *)buf, (size_t)n}, &page_url);
        }
    }
    if (rc == CF_OK) {
        char buf[64];
        int n = snprintf(buf, sizeof buf, "/rooms/%lld/refresh",
                         (long long)model->room.id);
        if (n < 0 || (size_t)n >= sizeof buf) {
            rc = CF_INTERNAL;
        } else {
            rc = cf_view_ctx_url(ctx, (cf_span){
                (const unsigned char *)buf, (size_t)n}, &refresh_url);
        }
    }
    if (rc != CF_OK) goto fail;

    CF_VIEW_TRY(cf_view_str(
        out,
        "<div id=\"message-area\" class=\"message-area\" contents=\"true\" "
        "data-controller=\"messages presence drop-target\" "
        "data-action=\"turbo:before-stream-render@document-&gt;messages#"
        "beforeStreamRender "
        "keydown.up@document-&gt;messages#editMyLastMessage "
        "dragenter-&gt;drop-target#dragenter "
        "dragover-&gt;drop-target#dragover drop-&gt;drop-target#drop "
        "visibilitychange@document-&gt;presence#visibilityChanged\" "
        "data-messages-first-of-day-class=\"message--first-of-day\" "
        "data-messages-formatted-class=\"message--formatted\" "
        "data-messages-me-class=\"message--me\" "
        "data-messages-mentioned-class=\"message--mentioned\" "
        "data-messages-threaded-class=\"message--threaded\" "
        "data-messages-page-url-value=\""));
    CF_VIEW_TRY(cf_view_raw(out, cf_view_span_of(&page_url)));
    CF_VIEW_TRY(cf_view_str(out, "\">\n  "));
    CF_VIEW_TRY(cf_view_messages_template(ctx, &model->user, out));
    CF_VIEW_TRY(cf_view_str(out, "\n\n  <div id=\""));
    CF_VIEW_TRY(cf_view_raw(out, cf_view_span_of(&messages_id)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\" class=\"messages\" data-controller=\"maintain-scroll "
        "refresh-room\" "
        "data-action=\"turbo:before-stream-render@document-&gt;maintain-scroll#"
        "beforeStreamRender "
        "visibilitychange@document-&gt;refresh-room#visibilityChanged "
        "online@window-&gt;refresh-room#online\" "
        "data-messages-target=\"messages\" "
        "data-refresh-room-loaded-at-value=\""));
    {
        char buf[32];
        int n = snprintf(buf, sizeof buf, "%lld",
                         (long long)cf_view_epoch_ms(model->updated_at_us));
        if (n < 0 || (size_t)n >= sizeof buf) {
            rc = CF_INTERNAL;
            goto fail;
        }
        CF_VIEW_TRY(cf_view_raw(out, (cf_span){(const unsigned char *)buf,
                                               (size_t)n}));
    }
    CF_VIEW_TRY(cf_view_str(out, "\" data-refresh-room-url-value=\""));
    CF_VIEW_TRY(cf_view_raw(out, cf_view_span_of(&refresh_url)));
    CF_VIEW_TRY(cf_view_str(out, "\">\n"));
    if (model->invitation) {
        CF_VIEW_TRY(room_invitation(ctx, cf_str_span(model->join_code), out));
    }
    for (size_t i = 0; i < model->messages.len; i++) {
        CF_VIEW_TRY(cf_view_str(out, "\n    "));
        CF_VIEW_TRY(cf_view_message_item_partial(ctx, &model->messages.items[i],
                                                 out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "  </div>\n\n"
        "  <turbo-cable-stream-source channel=\"RoomMessagesChannel\" "
        "signed-stream-name=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_str_span(model->messages_stream_name)));
    CF_VIEW_TRY(cf_view_str(out, "\"></turbo-cable-stream-source>\n  "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class",
                                      "message-area__return-to-latest btn"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-action",
                                      "messages#returnToLatest"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-messages-target",
                                      "latest"));
        CF_VIEW_TRY(cf_view_attr_flag(&attrs, "hidden", true));
        cf_builder body = {0};
        {
            cf_view_attrs img;
            cf_view_attrs_init(&img);
            rc = cf_view_attr_cstr(&img, "aria-hidden", "true");
            if (rc == CF_OK) rc = cf_view_attr_i64(&img, "size", 20);
            if (rc == CF_OK) {
                rc = cf_view_image_tag(ctx, cf_span_of_lit("arrow-down.svg"),
                                       &img, &body);
            }
        }
        if (rc == CF_OK) {
            rc = cf_view_str(&body,
                             "<span class=\"for-screen-reader\">Jump to "
                             "newest message</span>");
        }
        if (rc == CF_OK) {
            rc = cf_view_content(out, "button", &attrs,
                                 cf_view_span_of(&body));
        }
        cf_builder_dispose(&body);
        if (rc != CF_OK) goto fail;
    }
    CF_VIEW_TRY(cf_view_str(out, "\n</div>\n"));
    cf_builder_dispose(&messages_id);
    cf_builder_dispose(&page_url);
    cf_builder_dispose(&refresh_url);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&messages_id);
    cf_builder_dispose(&page_url);
    cf_builder_dispose(&refresh_url);
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_room_show(const cf_view_ctx *ctx,
                         const cf_view_room_show_model *model,
                         cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder head = {0}, content = {0}, nav = {0}, footer = {0},
               sidebar = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = cf_view_sidebar_frame(cf_span_of_lit("/users/me/sidebar"), &sidebar);
    if (rc == CF_OK) {
        rc = cf_view_str(&head,
                         "  <meta name=\"turbo-cache-control\" "
                         "content=\"no-preview\">"
                         "\n  <meta name=\"current-room-id\" content=\""); 
    }
    if (rc == CF_OK) {
        char buf[24];
        int n = snprintf(buf, sizeof buf, "%lld", (long long)model->room.id);
        if (n < 0 || (size_t)n >= sizeof buf) {
            rc = CF_INTERNAL;
        } else {
            rc = cf_view_raw(&head, (cf_span){(const unsigned char *)buf,
                                              (size_t)n});
        }
    }
    if (rc == CF_OK) rc = cf_view_str(&head, "\">");
    if (rc == CF_OK) rc = room_content(ctx, model, &content);
    if (rc == CF_OK) rc = room_nav(ctx, &model->room, &nav);
    if (rc == CF_OK) rc = room_composer(ctx, &model->room, &footer);
    if (rc == CF_OK) {
        rc = cf_view_layout_page(ctx, cf_str_span(model->room.display_name),
                                 true, cf_span_of_lit("sidebar"), true,
                                 cf_view_span_of(&head),
                                 cf_view_span_of(&content),
                                 cf_view_span_of(&nav),
                                 cf_view_span_of(&footer),
                                 cf_view_span_of(&sidebar), out);
    }
    cf_builder_dispose(&head);
    cf_builder_dispose(&content);
    cf_builder_dispose(&nav);
    cf_builder_dispose(&footer);
    cf_builder_dispose(&sidebar);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_room_show_frame(const cf_view_ctx *ctx,
                               const cf_view_room_show_model *model,
                               cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder head = {0}, content = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = cf_view_str(&head,
                     "  <meta name=\"turbo-cache-control\" content=\"no-preview\">"
                     "\n  <meta name=\"current-room-id\" content=\"");
    if (rc == CF_OK) {
        char buf[24];
        int n = snprintf(buf, sizeof buf, "%lld", (long long)model->room.id);
        if (n < 0 || (size_t)n >= sizeof buf) {
            rc = CF_INTERNAL;
        } else {
            rc = cf_view_raw(&head, (cf_span){(const unsigned char *)buf,
                                              (size_t)n});
        }
    }
    if (rc == CF_OK) rc = cf_view_str(&head, "\">");
    if (rc == CF_OK) rc = room_content(ctx, model, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_frame(ctx, cf_view_span_of(&head),
                                  cf_view_span_of(&content), out);
    }
    cf_builder_dispose(&head);
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}
