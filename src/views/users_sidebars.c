/* src/views/users_sidebars.c — users/sidebars/show.html.erb and its
 * rooms/_direct, rooms/_direct_placeholder and rooms/_shared partials
 * (tmp/rust-ref/crates/views/templates/users/sidebars/ ...;
 * crates/views/src/users.rs SidebarShow/SidebarDirect/SidebarRoom and
 * helpers/{turbo,rooms,forms,users}.rs).
 *
 * The content block wraps itself in `sidebar_turbo_frame_tag` — the same
 * `user_sidebar` turbo-frame the welcome and room pages lazy-load through
 * `src`; here it carries no src and the whole room list, so the page's
 * `<aside id="sidebar">` block stays empty and a Turbo-Frame request renders
 * the content in turbo-rails' frame layout (page_or_frame).
 *
 * Rendering conventions are A02's (src/views/internal.h): attributes through
 * cf_view_attr*, dynamic text through cf_view_text, and every builder is
 * left at its entry length on failure.  Whitespace between tags follows the
 * golden render (the comparator collapses runs, so only presence matters);
 * the one place the reference has none is between the placeholder forms.
 *
 * The reference's fragment cache does not exist yet (the A02 message-item
 * precedent), so a direct membership is always rendered here rather than
 * read back as a stored fragment; the markup is the same either way.
 *
 * S02 boundary (disclosed in docs/devel/evidence/A-users-sidebars.md): the
 * pinned source maps every sidebar avatar through `fresh_user_avatar_path`
 * (the signed token URL) and never through an attachment variant, so this
 * renderer has no S02-dependent branch to leave out.  The presenter reads no
 * rich text, message or blob for the sidebar, and the absent attachment
 * variant path is asserted in the tests.
 */
#include "views/internal.h"

#include <stdio.h>
#include <string.h>

/* ---- small pieces --------------------------------------------------------- */

/* `h::turbo_stream_from(name)`: a turbo-cable-stream-source for one signed
 * stream name (builder_tag: open + close, attributes in call order). */
static cf_err sidebar_stream_tag(cf_span signed_stream_name, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_view_attrs attrs;
    cf_view_attrs_init(&attrs);
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "channel", "Turbo::StreamsChannel"));
    CF_VIEW_TRY(cf_view_attr(&attrs, "signed-stream-name", signed_stream_name));
    CF_VIEW_TRY(cf_view_open_start(out, "turbo-cable-stream-source", &attrs));
    CF_VIEW_TRY(cf_view_close_tag(out, "turbo-cable-stream-source"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* `{% filter sidebar_turbo_frame_tag %}`'s opening tag: the block form passes
 * no src, so the frame loads nothing and only wraps the content. */
static cf_err sidebar_frame_open(cf_builder *out) {
    cf_err rc = CF_OK;
    cf_view_attrs attrs;
    cf_view_attrs_init(&attrs);
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-turbo-permanent", "true"));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-controller",
                                  "rooms-list read-rooms turbo-frame"));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-rooms-list-unread-class",
                                  "unread"));
    /* html_safe in the reference so "->" isn't escaped. */
    CF_VIEW_TRY(cf_view_attr_raw(
        &attrs, "data-action",
        cf_span_of_lit("presence:present@window->rooms-list#read "
                       "read-rooms:read->rooms-list#read "
                       "turbo:frame-load->rooms-list#loaded "
                       "refresh-room:visible@window->turbo-frame#reload")));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "id", "user_sidebar"));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "target", "_top"));
    CF_VIEW_TRY(cf_view_open_start(out, "turbo-frame", &attrs));
fail:
    return rc;
}

/* `image_tag(ctx, src, attrs().size(size).aria_hidden())`. */
static cf_err sidebar_sized_image(const cf_view_ctx *ctx, cf_span src,
                                  int size, cf_builder *out) {
    cf_err rc;
    cf_view_attrs attrs;
    cf_view_attrs_init(&attrs);
    CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", size));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
    CF_VIEW_TRY(cf_view_image_tag(ctx, src, &attrs, out));
    return CF_OK;
fail:
    return rc;
}

/* `image_tag(ctx, src, attrs().size(size).aria_hidden().style(style))`. */
static cf_err sidebar_sized_image_style(const cf_view_ctx *ctx, cf_span src,
                                        int size, cf_span style,
                                        cf_builder *out) {
    cf_err rc;
    cf_view_attrs attrs;
    cf_view_attrs_init(&attrs);
    CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", size));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
    CF_VIEW_TRY(cf_view_attr(&attrs, "style", style));
    CF_VIEW_TRY(cf_view_image_tag(ctx, src, &attrs, out));
    return CF_OK;
fail:
    return rc;
}

/* `image_tag(ctx, src, attrs().size(size).aria_hidden().class(class))`. */
static cf_err sidebar_sized_image_class(const cf_view_ctx *ctx, const char *src,
                                        int size, const char *class_attr,
                                        cf_builder *out) {
    cf_err rc;
    cf_view_attrs attrs;
    cf_view_attrs_init(&attrs);
    CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", size));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", class_attr));
    CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit(src), &attrs, out));
    return CF_OK;
fail:
    return rc;
}

/* `image_tag(ctx, src, attrs().aria_hidden())`. */
static cf_err sidebar_plain_image(const cf_view_ctx *ctx, cf_span src,
                                  cf_builder *out) {
    cf_err rc;
    cf_view_attrs attrs;
    cf_view_attrs_init(&attrs);
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
    CF_VIEW_TRY(cf_view_image_tag(ctx, src, &attrs, out));
    return CF_OK;
fail:
    return rc;
}

/* `class_names` of `users/sidebars/rooms/_shared`: "align-center gap room btn
 * txt-nowrap" plus "unread". */
static const char *sidebar_shared_class(bool unread) {
    return unread ? "align-center gap room btn txt-nowrap unread"
                  : "align-center gap room btn txt-nowrap";
}

/* `class_names` of `users/sidebars/rooms/_direct`: "direct" plus "unread". */
static const char *sidebar_direct_class(bool unread) {
    return unread ? "direct unread" : "direct";
}

static bool sidebar_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

/* `String#split(' ')`: the first whitespace-separated part. */
static cf_span sidebar_first_name(cf_span name) {
    size_t start = 0;
    while (start < name.len && sidebar_space(name.ptr[start])) start++;
    size_t end = start;
    while (end < name.len && !sidebar_space(name.ptr[end])) end++;
    return (cf_span){name.ptr + start, end - start};
}

/* `members.map { |m| m.name.split(' ')[0, 3].map { |s| s[0].capitalize }
 * .join }.to_sentence(two_words_connector: "+")`: each member contributes up
 * to three capitalized initials, and the members are joined by "+". */
static cf_err sidebar_member_initials(const cf_view_sidebar_direct *membership,
                                      cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    cf_builder joined = {0};
    for (size_t m = 0; m < membership->members.len; m++) {
        cf_span name = cf_str_span(membership->members.items[m].name);
        if (m != 0) {
            rc = cf_builder_append(&joined, cf_span_of_lit("+"));
            if (rc != CF_OK) {
                cf_builder_dispose(&joined);
                return cf_view_fail(&guard, rc);
            }
        }
        size_t at = 0;
        for (size_t part = 0; part < 3; part++) {
            while (at < name.len && sidebar_space(name.ptr[at])) at++;
            if (at >= name.len) break;
            size_t end = at;
            while (end < name.len && !sidebar_space(name.ptr[end])) end++;
            rc = cf_view_capitalize((cf_span){name.ptr + at, 1}, &joined);
            if (rc != CF_OK) {
                cf_builder_dispose(&joined);
                return cf_view_fail(&guard, rc);
            }
            at = end;
        }
    }
    if (joined.len != 0) rc = cf_view_text(out, cf_view_span_of(&joined));
    cf_builder_dispose(&joined);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

/* ---- partials ------------------------------------------------------------- */

/* users/sidebars/rooms/_shared.html.erb: `link_to_room(room.id, attrs()
 * .id(dom_id(param_key, id, "list")).data("sorted_list_name", name)
 * .style("--column-gap: 0.5em").class(class_names()))` around the name. */
cf_err cf_view_sidebar_shared_partial(const cf_view_sidebar_room *room,
                                      cf_builder *out) {
    if (room == NULL || out == NULL) return CF_INVALID;
    if (room->name.ptr == NULL || room->param_key.ptr == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder id = {0}, href = {0}, content = {0};
    cf_view_attrs attrs;
    cf_view_attrs_init(&attrs);
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    CF_VIEW_TRY(cf_view_str(&id, "list_"));
    CF_VIEW_TRY(cf_view_raw(&id, cf_str_span(room->param_key)));
    CF_VIEW_TRY(cf_view_str(&id, "_"));
    {
        char number[32];
        int n = snprintf(number, sizeof number, "%lld", (long long)room->id);
        if (n < 0 || (size_t)n >= sizeof number) CF_VIEW_TRY(CF_INTERNAL);
        CF_VIEW_TRY(cf_view_str(&id, number));
        CF_VIEW_TRY(cf_view_str(&href, "/rooms/"));
        CF_VIEW_TRY(cf_view_str(&href, number));
    }
    CF_VIEW_TRY(cf_view_attr(&attrs, "id", cf_view_span_of(&id)));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-rooms-list-target", "room"));
    CF_VIEW_TRY(cf_view_attr_i64(&attrs, "data-room-id", room->id));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-badge-dot-target", "unread"));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-sorted-list-target", "item"));
    CF_VIEW_TRY(
        cf_view_attr(&attrs, "data-sorted-list-name", cf_str_span(room->name)));
    CF_VIEW_TRY(
        cf_view_attr_cstr(&attrs, "style", "--column-gap: 0.5em"));
    CF_VIEW_TRY(
        cf_view_attr_cstr(&attrs, "class",
                          sidebar_shared_class(room->unread)));
    CF_VIEW_TRY(cf_view_attr(&attrs, "href", cf_view_span_of(&href)));
    CF_VIEW_TRY(cf_view_str(&content, "\n  <span class=\"overflow-ellipsis\">"));
    CF_VIEW_TRY(cf_view_text(&content, cf_str_span(room->name)));
    CF_VIEW_TRY(cf_view_str(&content, "</span>\n"));
    CF_VIEW_TRY(cf_view_content(out, "a", &attrs, cf_view_span_of(&content)));
    cf_builder_dispose(&id);
    cf_builder_dispose(&href);
    cf_builder_dispose(&content);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&id);
    cf_builder_dispose(&href);
    cf_builder_dispose(&content);
    return cf_view_fail(&guard, rc);
}

/* users/sidebars/rooms/_direct.html.erb.  `members` is never empty (the
 * presenter falls back to the membership's own user); an empty vector is the
 * reference's `members[0]` NoMethodError and stays CF_INVALID here. */
cf_err cf_view_sidebar_direct_partial(const cf_view_ctx *ctx,
                                      const cf_view_sidebar_direct *membership,
                                      cf_builder *out) {
    if (ctx == NULL || membership == NULL || out == NULL) return CF_INVALID;
    if (membership->members.len == 0) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder id = {0}, href = {0}, content = {0};
    cf_view_attrs attrs;
    cf_view_attrs_init(&attrs);
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    {
        char number[32];
        int n = snprintf(number, sizeof number, "%lld",
                         (long long)membership->room_id);
        if (n < 0 || (size_t)n >= sizeof number) CF_VIEW_TRY(CF_INTERNAL);
        CF_VIEW_TRY(cf_view_str(&id, "list_rooms_direct_"));
        CF_VIEW_TRY(cf_view_str(&id, number));
        CF_VIEW_TRY(cf_view_str(&href, "/rooms/"));
        CF_VIEW_TRY(cf_view_str(&href, number));
    }
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class",
                                  sidebar_direct_class(membership->unread)));
    CF_VIEW_TRY(cf_view_attr(&attrs, "id", cf_view_span_of(&id)));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-rooms-list-target", "room"));
    CF_VIEW_TRY(cf_view_attr_i64(&attrs, "data-room-id", membership->room_id));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-badge-dot-target", "unread"));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-sorted-list-target", "item"));
    CF_VIEW_TRY(cf_view_attr(&attrs, "data-sorted-list-number",
                             cf_str_span(membership->updated_at_epoch)));
    CF_VIEW_TRY(cf_view_attr(&attrs, "href", cf_view_span_of(&href)));

    CF_VIEW_TRY(cf_view_str(&content, "\n"));
    if (membership->members.len > 1) {
        CF_VIEW_TRY(
            cf_view_str(&content, "      <div class=\"avatar__group\">\n"));
        for (size_t i = 0; i < membership->members.len && i < 4; i++) {
            CF_VIEW_TRY(cf_view_str(&content,
                                    "          <span class=\"avatar\">\n"
                                    "            "));
            CF_VIEW_TRY(sidebar_sized_image(
                ctx, cf_str_span(membership->members.items[i].avatar_path), 20,
                &content));
            CF_VIEW_TRY(cf_view_str(&content, "\n          </span>\n"));
        }
        CF_VIEW_TRY(cf_view_str(&content, "      </div>\n"));
    } else {
        CF_VIEW_TRY(cf_view_str(&content, "      <span class=\"avatar\">\n"
                                           "        "));
        CF_VIEW_TRY(sidebar_sized_image(
            ctx, cf_str_span(membership->members.items[0].avatar_path), 48,
            &content));
        CF_VIEW_TRY(cf_view_str(&content, "\n      </span>\n"));
    }
    CF_VIEW_TRY(cf_view_str(
        &content,
        "\n    <span class=\"direct__author flex align-center gap max-width "
        "min-width border-radius txt-small\">\n"
        "      <span class=\"txt-nowrap overflow-ellipsis\">\n"
        "        <span class=\"for-screen-reader\">Ping with</span>\n"
        "          "));
    if (membership->members.len > 1) {
        CF_VIEW_TRY(sidebar_member_initials(membership, &content));
    } else {
        CF_VIEW_TRY(cf_view_text(&content,
                                 sidebar_first_name(cf_str_span(
                                     membership->members.items[0].name))));
    }
    CF_VIEW_TRY(cf_view_str(&content, "\n      </span>\n    </span>\n"));

    CF_VIEW_TRY(cf_view_str(out, "\n  "));
    CF_VIEW_TRY(cf_view_content(out, "a", &attrs, cf_view_span_of(&content)));
    cf_builder_dispose(&id);
    cf_builder_dispose(&href);
    cf_builder_dispose(&content);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&id);
    cf_builder_dispose(&href);
    cf_builder_dispose(&content);
    return cf_view_fail(&guard, rc);
}

/* users/sidebars/rooms/_direct_placeholder.html.erb: `button_to(
 * rooms_directs_with_user(user.id), attrs().class("direct borderless
 * fill-transparent unpad"))` around the avatar and first name. */
static cf_err sidebar_direct_placeholder(const cf_view_ctx *ctx,
                                         const cf_view_sidebar_user *user,
                                         cf_builder *out) {
    if (ctx == NULL || user == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder url = {0}, content = {0};
    cf_view_attrs options;
    cf_view_attrs_init(&options);
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    /* `rooms_directs_path(user_ids: [ user.id ])`: CGI.escape of the key's
     * brackets, then the id. */
    {
        char number[32];
        int n = snprintf(number, sizeof number, "%lld", (long long)user->id);
        if (n < 0 || (size_t)n >= sizeof number) CF_VIEW_TRY(CF_INTERNAL);
        CF_VIEW_TRY(cf_view_str(
            &url, "/rooms/directs?user_ids%5B%5D="));
        CF_VIEW_TRY(cf_view_str(&url, number));
    }
    CF_VIEW_TRY(cf_view_attr_cstr(&options, "class",
                                  "direct borderless fill-transparent unpad"));

    CF_VIEW_TRY(cf_view_str(&content, "\n  <span class=\"avatar\">\n    "));
    CF_VIEW_TRY(sidebar_plain_image(ctx, cf_str_span(user->avatar_path),
                                    &content));
    CF_VIEW_TRY(cf_view_str(
        &content,
        "\n  </span>\n\n"
        "  <span class=\"direct__author flex align-center gap max-width "
        "min-width border-radius txt-small\">\n"
        "    <span class=\"txt-nowrap overflow-ellipsis\">\n"
        "      <span class=\"for-screen-reader\">Start a ping with</span>\n"
        "      "));
    CF_VIEW_TRY(cf_view_text(
        &content, sidebar_first_name(cf_str_span(user->name))));
    CF_VIEW_TRY(cf_view_str(&content, "\n    </span>\n  </span>\n"));

    CF_VIEW_TRY(cf_view_button_to(out, cf_view_span_of(&url), &options,
                                  (cf_span){NULL, 0}, (cf_span){NULL, 0},
                                  cf_view_span_of(&content)));
    cf_builder_dispose(&url);
    cf_builder_dispose(&content);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&url);
    cf_builder_dispose(&content);
    return cf_view_fail(&guard, rc);
}

/* ---- the page ------------------------------------------------------------- */

/* The `sidebar_turbo_frame_tag` block `show.html.erb`'s content block is. */
static cf_err sidebar_content(const cf_view_ctx *ctx,
                              const cf_view_sidebar_model *model,
                              cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    CF_VIEW_TRY(sidebar_frame_open(out));
    CF_VIEW_TRY(cf_view_str(out, "\n  "));
    CF_VIEW_TRY(sidebar_stream_tag(cf_str_span(model->rooms_stream), out));
    CF_VIEW_TRY(cf_view_str(out, "\n  "));
    CF_VIEW_TRY(sidebar_stream_tag(cf_str_span(model->user_rooms_stream), out));

    CF_VIEW_TRY(cf_view_str(
        out,
        "\n\n"
        "  <div class=\"sidebar__container overflow-y "
        "overflow-hide-scrollbar\"\n"
        "      data-controller=\"badge-dot\"\n"
        "      data-badge-dot-unread-class=\"unread\"\n"
        "      data-action=\"rooms-list:unread@window->badge-dot#update "
        "rooms-list:read@window->badge-dot#update "
        "turbo:submit-start->turbo-frame#unpermanize\">\n"
        "    <turbo-frame id=\"direct_rooms_control\" target=\"_top\">\n"
        "      <div class=\"directs gap overflow-x "
        "overflow-hide-scrollbar\">\n"
        "        "));
    {
        /* `link_to(routes::new_rooms_direct(), attrs().class("direct
         * direct__new").data("turbo_frame", "_self"))`. */
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(
            cf_view_attr_cstr(&attrs, "class", "direct direct__new"));
        CF_VIEW_TRY(
            cf_view_attr_cstr(&attrs, "data-turbo-frame", "_self"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "href", "/rooms/directs/new"));
        CF_VIEW_TRY(cf_view_open_start(out, "a", &attrs));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n          <span class=\"avatar avatar--icon\">\n"
        "            "));
    CF_VIEW_TRY(sidebar_sized_image_class(ctx, "messages-add.svg", 20,
                                          "colorize--black", out));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n          </span>\n\n"
        "          <span class=\"direct__author flex max-width min-width "
        "border-radius pad-inline-half\">\n"
        "            <span class=\"for-screen-reader\">New</span>\n"
        "            <span class=\"txt-small overflow-clip\">Ping</span>\n"
        "          </span>\n"));
    CF_VIEW_TRY(cf_view_close_tag(out, "a"));

    CF_VIEW_TRY(cf_view_str(
        out,
        "\n        <div id=\"direct_rooms\" contents "
        "data-controller=\"sorted-list\" "
        "data-action=\"rooms-list:unread@window->sorted-list#updateItem\">\n"
        "          "));
    for (size_t i = 0; i < model->direct_memberships.len; i++) {
        CF_VIEW_TRY(cf_view_sidebar_direct_partial(
            ctx, &model->direct_memberships.items[i], out));
        CF_VIEW_TRY(cf_view_str(out, "\n"));
    }
    CF_VIEW_TRY(cf_view_str(out, "        </div>\n\n        <div contents>\n"
                                 "          "));
    for (size_t i = 0; i < model->direct_placeholder_users.len; i++) {
        CF_VIEW_TRY(sidebar_direct_placeholder(
            ctx, &model->direct_placeholder_users.items[i], out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n        </div>\n"
        "      </div>\n"
        "    </turbo-frame>\n\n"
        "    <div class=\"rooms position-relative flex flex-column gap\">\n"
        "      <div id=\"shared_rooms\" contents "
        "data-controller=\"sorted-list\">\n"));

    for (size_t i = 0; i < model->other_memberships.len; i++) {
        CF_VIEW_TRY(cf_view_str(out, "          "));
        CF_VIEW_TRY(cf_view_sidebar_shared_partial(
            &model->other_memberships.items[i], out));
        CF_VIEW_TRY(cf_view_str(out, "\n"));
    }
    CF_VIEW_TRY(cf_view_str(out, "      </div>\n\n"));
    if (model->can_create_rooms) {
        /* `link_to(routes::new_rooms_open(), attrs().class("rooms__new-btn
         * btn room align-center gap txt-reversed").aria("label", "New Chat
         * Room"))`. */
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(
            &attrs, "class", "rooms__new-btn btn room align-center gap "
                             "txt-reversed"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-label", "New Chat Room"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "href", "/rooms/opens/new"));
        CF_VIEW_TRY(cf_view_str(out, "        "));
        CF_VIEW_TRY(cf_view_open_start(out, "a", &attrs));
        CF_VIEW_TRY(cf_view_str(out, "\n          "));
        {
            cf_view_attrs img;
            cf_view_attrs_init(&img);
            CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 20));
            CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
            CF_VIEW_TRY(
                cf_view_attr_cstr(&img, "style", "view-transition-name: "
                                                 "new-room"));
            CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("add.svg"), &img,
                                          out));
        }
        CF_VIEW_TRY(cf_view_str(out, "\n"));
        CF_VIEW_TRY(cf_view_close_tag(out, "a"));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "    </div>\n\n"
        "    <button class=\"btn sidebar__toggle\" "
        "data-action=\"toggle-class#toggle\">\n"
        "      "));
    CF_VIEW_TRY(sidebar_sized_image(ctx, cf_span_of_lit("menu.svg"), 20, out));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n      <span class=\"for-screen-reader\">Open menu</span>\n"
        "    </button>\n"
        "  </div>\n\n"
        "  <div class=\"flex align-end sidebar__tools gap justify-end\">\n"
        "    "));
    {
        /* `link_to(routes::user_profile(), attrs().class("btn avatar
         * flex-item-no-shrink sidebar__tool"))`. */
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(
            &attrs, "class", "btn avatar flex-item-no-shrink sidebar__tool"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "href", "/users/me/profile"));
        CF_VIEW_TRY(cf_view_open_start(out, "a", &attrs));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n      "));
    {
        char style[64];
        int n = snprintf(style, sizeof style,
                         "view-transition-name: avatar-%lld",
                         (long long)model->current_user.id);
        if (n < 0 || (size_t)n >= sizeof style) CF_VIEW_TRY(CF_INTERNAL);
        CF_VIEW_TRY(sidebar_sized_image_style(
            ctx, cf_str_span(model->current_user.avatar_path), 48,
            (cf_span){(const unsigned char *)style, (size_t)n}, out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n      <span class=\"for-screen-reader\">My Settings</span>\n"));
    CF_VIEW_TRY(cf_view_close_tag(out, "a"));
    CF_VIEW_TRY(cf_view_str(out, "\n    "));
    {
        /* `link_to(routes::edit_account(), attrs().class("btn align-center gap
         * txt-reversed sidebar__tool"))`. */
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(
            &attrs, "class", "btn align-center gap txt-reversed sidebar__tool"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "href", "/account/edit"));
        CF_VIEW_TRY(cf_view_open_start(out, "a", &attrs));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n      "));
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 20));
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_cstr(
            &img, "style", "view-transition-name: account-settings"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("settings.svg"), &img,
                                      out));
    }
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n      <span class=\"for-screen-reader\">Account "
        "Settings</span>\n"));
    CF_VIEW_TRY(cf_view_close_tag(out, "a"));
    CF_VIEW_TRY(cf_view_str(out, "  </div>\n"));
    CF_VIEW_TRY(cf_view_close_tag(out, "turbo-frame"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_users_sidebar_show(const cf_view_ctx *ctx,
                                  const cf_view_sidebar_model *model,
                                  cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder content = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = sidebar_content(ctx, model, &content);
    if (rc == CF_OK) {
        /* SidebarShow implements Page with no page title and no body class. */
        rc = cf_view_layout_page(ctx, (cf_span){NULL, 0}, false,
                                 (cf_span){NULL, 0}, false, (cf_span){NULL, 0},
                                 cf_view_span_of(&content), (cf_span){NULL, 0},
                                 (cf_span){NULL, 0}, (cf_span){NULL, 0}, out);
    }
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_users_sidebar_show_frame(const cf_view_ctx *ctx,
                                        const cf_view_sidebar_model *model,
                                        cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder content = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = sidebar_content(ctx, model, &content);
    if (rc == CF_OK) {
        /* `layouts::frame(ctx, page.as_head(), page.as_content())`: the
         * template defines no head block. */
        rc = cf_view_layout_frame(ctx, (cf_span){NULL, 0},
                                  cf_view_span_of(&content), out);
    }
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}
