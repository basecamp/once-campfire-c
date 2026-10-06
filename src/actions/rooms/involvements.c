/* src/actions/rooms/involvements.c — A-rooms-involvements:
 * `rooms/involvements#show` (route ID 93) and `rooms/involvements#update`
 * (route IDs 94, 95)
 * (docs/devel/implementation/contracts/controller-packets.md
 * "A-rooms-involvements").
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/rooms/
 * involvements.rs, the pinned port of reference/app/controllers/rooms/
 * involvements_controller.rb:
 *
 *   show:   before_actions(Before::default()) — authentication REQUIRED,
 *           deny_bots, forgery protection; concerns::set_room — RoomScoped
 *           (see refreshes.c: membership by params[:room_id], 404 when
 *           missing; a membership whose room row is gone is the reference's
 *           500); the InvolvementView (room id, room_kind, the membership's
 *           involvement name, "" when the column is NULL); page::content
 *           (the InvolvementShow template in the application layout, or
 *           turbo-rails' frame layout for a Turbo-Frame request).
 *   update: before_actions; set_room; the involvement param — blank
 *           (missing, "", whitespace-only, `[]`) is stored as nil, anything
 *           that is not one of the enum values raises (internal error);
 *           membership.update_involvement in the writer; the
 *           broadcast_visibility_changes (render_shared_room, then
 *           involvement_change with the previous value); redirect to the
 *           room involvement URL.
 *
 * The response conventions are the landed actions' (A-rooms/A-messages):
 * helpers that answer the request set ctx->response and return CF_OK
 * (callers stop with cf_auth_halted, src/auth.h); the page render carries
 * the layout `Link` preload header, the frame render none; redirects are
 * PUBLIC_ORIGIN + the route path.  A full loop queue (CF_BUSY) drops the
 * post-commit broadcast and is counted rather than failing the committed
 * update (00-contracts.md; the rooms.c/messages.c delivery policy).
 *
 * Route registration: src/routes.c rows 93/94/95 bind
 * cf_action_rooms_involvements_show (93) and
 * cf_action_rooms_involvements_update (94, 95) to this file's actions.
 *
 * Integrator requests (views packet; this file is self-contained meanwhile):
 *  R1. cf_view_room_involvement_show / cf_view_room_involvement_show_frame —
 *      the InvolvementShow template (rooms/involvements/show.html with the
 *      button_to_change_involvement helper) as shared renderers.  Proposed
 *      declarations (src/views.h):
 *        typedef struct { int64_t room_id; cf_room_type kind;
 *                         cf_str involvement; } cf_view_room_involvement;
 *        cf_err cf_view_room_involvement_show(const cf_view_ctx *ctx,
 *            const cf_view_room_involvement *model, cf_builder *out);
 *        cf_err cf_view_room_involvement_show_frame(const cf_view_ctx *ctx,
 *            const cf_view_room_involvement *model, cf_builder *out);
 *      Until they land, the static functions of the same names below render
 *      the template's turbo-frame shell byte for byte (frame id, url-param,
 *      data attributes) with a clearly marked placeholder button carrying
 *      the current involvement value where the helper's bell form goes.
 *  R2 (V02, done). The sidebar shared-room partial behind render_shared_room
 *      (rooms.rs render_shared_room: users/sidebars/rooms/_shared), which the
 *      update broadcast's prepend branch needs.  cf_broadcast_partials_views
 *      now installs the real renderer
 *      (cf_view_rooms_shared_room_partial), so an update *from* invisible
 *      prepends the golden-verified `_shared` markup to the member's own
 *      rooms stream.  Covered transitions (to invisible removes, to a
 *      visible value only redirects) need no partial.
 */
#include "cf.h"

#include "app.h"
#include "auth.h"
#include "cable/channels.h"
#include "config.h"
#include "context.h"
#include "models/membership.h"
#include "models/room.h"
#include "models/user.h"
#include "views.h"
#include "views/internal.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cf_span involvement_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `c.is_turbo_frame_request()` (kit ctx.rs): the first `turbo-frame`
 * header through http 1.5.0's `HeaderValue::to_str` (HTAB or visible ASCII
 * only, else the header reads as absent), nonempty after trimming spaces
 * and tabs.  Local copy of the helper the other action packets carry until
 * it has a shared home. */
static unsigned char involvement_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c - 'A' + 'a') : c;
}

static bool involvement_turbo_frame_request(const cf_request *request) {
    if (request == NULL) return false;
    cf_span value = {NULL, 0};
    bool found = false;
    for (size_t i = 0; i < request->header_count; i++) {
        const cf_header *header = &request->headers[i];
        if (header->name.len != 11) continue;
        static const char name[] = "turbo-frame";
        bool match = true;
        for (size_t k = 0; k < 11; k++) {
            if (involvement_lower(header->name.ptr[k]) !=
                (unsigned char)name[k]) {
                match = false;
                break;
            }
        }
        if (match) {
            value = header->value;
            found = true;
            break;
        }
    }
    if (!found) return false;
    bool blank = true;
    for (size_t i = 0; i < value.len; i++) {
        unsigned char b = value.ptr[i];
        if (!((b >= 32 && b < 127) || b == '\t')) return false;
        if (b != ' ' && b != '\t') blank = false;
    }
    return !blank;
}

/* `redirect_to location` (Rails default 302, the absolute location, the
 * reference redirect's content type). */
static cf_err involvement_redirect_to(cf_ctx *ctx, cf_span location) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    ctx->response->status = 302;
    cf_err rc = cf_response_header(ctx->response,
                                   involvement_span("Location"), location);
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response,
                                involvement_span("Content-Type"),
                                involvement_span("text/html; charset=utf-8"));
    }
    return rc;
}

/* `redirect_to room_involvement_url(room)`: PUBLIC_ORIGIN +
 * "/rooms/<id>/involvement" (routes room_involvement). */
static cf_err involvement_redirect_to_involvement(cf_ctx *ctx,
                                                 int64_t room_id) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    char path[64];
    int n = snprintf(path, sizeof path, "/rooms/%" PRId64 "/involvement",
                     room_id);
    if (n < 0 || (size_t)n >= sizeof path) return CF_INTERNAL;
    cf_builder location = {0};
    cf_err rc = cf_builder_append(&location,
                                  involvement_span(config->public_origin));
    if (rc == CF_OK) {
        rc = cf_builder_append(
            &location, (cf_span){(const unsigned char *)path, (size_t)n});
    }
    if (rc == CF_OK) {
        rc = involvement_redirect_to(
            ctx, (cf_span){location.ptr, location.len});
    }
    cf_builder_dispose(&location);
    return rc;
}

/* The layout page's preload `Link` header (rooms.c convention); the frame
 * layout carries none. */
static cf_err involvement_link_header(cf_ctx *ctx) {
    cf_builder links = {0};
    cf_err rc = cf_views_preload_links(&links);
    if (rc == CF_INVALID) return CF_OK;
    if (rc == CF_OK && links.len != 0) {
        rc = cf_response_header(ctx->response, involvement_span("Link"),
                                (cf_span){links.ptr, links.len});
    }
    cf_builder_dispose(&links);
    return rc;
}

/* Freeze a rendered show response: status, HTML content type, and (page
 * layout only) the preload header.  Consumes the builder. */
static cf_err involvement_page_response(cf_ctx *ctx, unsigned status,
                                        cf_builder *body, bool frame) {
    cf_buf *buf = NULL;
    cf_err rc = cf_builder_freeze(body, &buf);
    if (rc != CF_OK) {
        cf_builder_dispose(body);
        return rc;
    }
    ctx->response->status = status;
    rc = cf_response_body(ctx->response, buf);
    cf_buf_release(buf);
    if (rc != CF_OK) return rc;
    rc = cf_response_header(ctx->response, involvement_span("Content-Type"),
                            involvement_span("text/html; charset=utf-8"));
    if (rc != CF_OK) return rc;
    if (cf_ctx_vary_accept(ctx)) {
        rc = cf_response_header(ctx->response, involvement_span("Vary"),
                                involvement_span("Accept"));
    }
    if (rc != CF_OK) return rc;
    if (!frame) rc = involvement_link_header(ctx);
    return rc;
}

/* `require_current_user(c)` (concerns.rs). */
static cf_err involvement_current_user(cf_ctx *ctx, cf_user *out) {
    bool found = false;
    cf_err rc = cf_auth_current_user(ctx, &found, out);
    if (rc != CF_OK) return rc;
    if (!found) return CF_INTERNAL;
    return CF_OK;
}

/* `concerns::set_room` (RoomScoped; see refreshes.c). */
static cf_err involvement_set_room(cf_ctx *ctx, int64_t user_id,
                                   cf_membership *membership_out,
                                   cf_room *room_out) {
    const cf_param *param = cf_ctx_param(ctx, involvement_span("room_id"));
    cf_span text = {NULL, 0};
    if (param == NULL || cf_param_type(param) != CF_PARAM_STRING ||
        cf_param_string(param, &text) != CF_OK) {
        return CF_NOT_FOUND;
    }
    int64_t room_id = 0;
    if (!cf_views_integer_cast(text, &room_id)) return CF_NOT_FOUND;

    bool found = false;
    cf_err rc = cf_membership_find_by_room_and_user(ctx->reader, room_id,
                                                    user_id, &found,
                                                    membership_out);
    if (rc != CF_OK) return rc;
    if (!found) return CF_NOT_FOUND;

    bool room_found = false;
    rc = cf_room_find_by_id(ctx->reader, membership_out->room_id, &room_found,
                            room_out);
    if (rc != CF_OK) return rc;
    if (!room_found) return CF_INTERNAL;
    return CF_OK;
}

/* ---- the involvement param ---------------------------------------------- */

/* UTF-8 decoding for the blank check below (kit params.rs delegates to
 * ActiveSupport's `blank?`, the Unicode White_Space property). */
static bool involvement_utf8_next(cf_span text, size_t *index, uint32_t *cp) {
    size_t i = *index;
    if (i >= text.len) return false;
    unsigned char b = text.ptr[i];
    size_t len;
    uint32_t value;
    if (b < 0x80) {
        len = 1;
        value = b;
    } else if ((b & 0xE0) == 0xC0) {
        len = 2;
        value = b & 0x1F;
    } else if ((b & 0xF0) == 0xE0) {
        len = 3;
        value = b & 0x0F;
    } else if ((b & 0xF8) == 0xF0) {
        len = 4;
        value = b & 0x07;
    } else {
        return false;
    }
    if (i + len > text.len) return false;
    for (size_t k = 1; k < len; k++) {
        unsigned char c = text.ptr[i + k];
        if ((c & 0xC0) != 0x80) return false;
        value = (value << 6) | (c & 0x3F);
    }
    *index = i + len;
    *cp = value;
    return true;
}

static bool involvement_cp_whitespace(uint32_t cp) {
    if (cp < 0x80) {
        return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\v' ||
               cp == '\f' || cp == '\r';
    }
    return cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
           cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

static bool involvement_str_blank(cf_span text) {
    size_t i = 0;
    while (i < text.len) {
        uint32_t cp = 0;
        if (!involvement_utf8_next(text, &i, &cp)) return false;
        if (!involvement_cp_whitespace(cp)) return false;
    }
    return true;
}

/* `Param::is_blank` (kit params.rs): nil, false, whitespace-only strings,
 * and empty collections. */
static bool involvement_param_blank(const cf_param *param) {
    if (param == NULL) return true;
    switch (cf_param_type(param)) {
    case CF_PARAM_NULL:
        return true;
    case CF_PARAM_BOOL: {
        bool present = false, value = false;
        if (cf_param_bool(param, &present, &value) != CF_OK) return true;
        return !value;
    }
    case CF_PARAM_NUMBER:
    case CF_PARAM_UPLOAD:
        return false;
    case CF_PARAM_STRING: {
        cf_span text = {NULL, 0};
        if (cf_param_string(param, &text) != CF_OK) return true;
        return involvement_str_blank(text);
    }
    case CF_PARAM_ARRAY:
    case CF_PARAM_OBJECT:
        return cf_param_count(param) == 0;
    }
    return true;
}

/* `involvement_param` (involvements.rs): a blank value (missing, "", "  ",
 * `[]`) is stored as nil; anything that is not one of the enum values
 * raises (internal error).  `present` mirrors the Option: false is nil. */
static cf_err involvement_param(cf_ctx *ctx, bool *present,
                                cf_involvement *out) {
    const cf_param *param = cf_ctx_param(ctx, involvement_span("involvement"));
    if (involvement_param_blank(param)) {
        *present = false;
        return CF_OK;
    }
    if (cf_param_type(param) != CF_PARAM_STRING) return CF_INTERNAL;
    cf_span text = {NULL, 0};
    cf_err rc = cf_param_string(param, &text);
    if (rc != CF_OK) return rc;
    if (!cf_involvement_from_name(
            (cf_str){(char *)text.ptr, text.len}, out)) {
        return CF_INTERNAL;
    }
    *present = true;
    return CF_OK;
}

/* ---- the InvolvementShow render (INTEGRATOR SHIM R1; see the header) ------ */

typedef struct {
    int64_t room_id;
    cf_room_type kind;
    cf_span involvement; /* borrowed: "" when the column is NULL */
} involvement_view;

/* `Room.model_name.param_key` ("rooms_open" etc.; see refreshes.c). */
static cf_err involvement_room_param_key(cf_room_type kind, cf_builder *out) {
    const char *class_name = cf_room_type_class_name(kind);
    if (class_name == NULL) return CF_INVALID;
    cf_err rc = CF_OK;
    for (const char *p = class_name; *p != '\0' && rc == CF_OK; p++) {
        if (p[0] == ':' && p[1] == ':') {
            rc = cf_builder_append(
                out, (cf_span){(const unsigned char *)"_", 1});
            p++;
            continue;
        }
        unsigned char c = (unsigned char)*p;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        rc = cf_builder_append(out, (cf_span){&c, 1});
    }
    return rc;
}

/* The show.html template's turbo-frame shell byte for byte (frame id,
 * url-param, data attributes); the button slot carries a clearly marked
 * placeholder with the current involvement value until the
 * button_to_change_involvement helper lands in the views packet (R1). */
static cf_err involvement_frame_content(const cf_view_ctx *ctx,
                                        const involvement_view *model,
                                        cf_builder *out) {
    (void)ctx;
    cf_err rc = cf_builder_append(
        out, involvement_span("<turbo-frame data-controller=\"turbo-frame\" "
                              "data-action=\"notifications:ready@window-&gt;"
                              "turbo-frame#load\" "
                              "data-turbo-frame-url-param=\"/rooms/"));
    if (rc == CF_OK) {
        char id[32];
        int n = snprintf(id, sizeof id, "%lld", (long long)model->room_id);
        if (n < 0 || (size_t)n >= sizeof id) return CF_INTERNAL;
        rc = cf_builder_append(
            out, (cf_span){(const unsigned char *)id, (size_t)n});
    }
    if (rc == CF_OK) rc = cf_builder_append(out, involvement_span("/involvement\" id=\"involvement_"));
    if (rc == CF_OK) rc = involvement_room_param_key(model->kind, out);
    if (rc == CF_OK) {
        char id[32];
        int n = snprintf(id, sizeof id, "_%lld", (long long)model->room_id);
        if (n < 0 || (size_t)n >= sizeof id) return CF_INTERNAL;
        rc = cf_builder_append(
            out, (cf_span){(const unsigned char *)id, (size_t)n});
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(out, involvement_span("\">\n  "
            "<!-- INTEGRATOR R1: button_to_change_involvement placeholder -->\n  "
            "<button class=\"btn "));
    }
    if (rc == CF_OK) rc = cf_html_attr(out, model->involvement);
    if (rc == CF_OK) rc = cf_builder_append(out, involvement_span("\" disabled>\n    "));
    if (rc == CF_OK) rc = cf_html_text(out, model->involvement);
    if (rc == CF_OK) {
        rc = cf_builder_append(out, involvement_span("\n  </button>\n</turbo-frame>"));
    }
    return rc;
}

/* page::content's two layouts over the InvolvementShow content. */
static cf_err cf_view_room_involvement_show(
    const cf_view_ctx *ctx, const involvement_view *model, cf_builder *out) {
    cf_builder content = {0};
    cf_err rc = involvement_frame_content(ctx, model, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_page(ctx, (cf_span){NULL, 0}, false,
                                 (cf_span){NULL, 0}, false,
                                 (cf_span){NULL, 0},
                                 (cf_span){content.ptr, content.len},
                                 (cf_span){NULL, 0}, (cf_span){NULL, 0},
                                 (cf_span){NULL, 0}, out);
    }
    cf_builder_dispose(&content);
    return rc;
}

static cf_err cf_view_room_involvement_show_frame(
    const cf_view_ctx *ctx, const involvement_view *model, cf_builder *out) {
    cf_builder content = {0};
    cf_err rc = involvement_frame_content(ctx, model, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_frame(ctx, (cf_span){NULL, 0},
                                  (cf_span){content.ptr, content.len}, out);
    }
    cf_builder_dispose(&content);
    return rc;
}

/* `show`: before_actions; set_room; the InvolvementView; page::content. */
cf_err cf_action_rooms_involvements_show(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = involvement_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    cf_membership membership = {0};
    cf_room room = {0};
    rc = involvement_set_room(ctx, user.id, &membership, &room);
    cf_user_dispose(&user);
    if (rc != CF_OK) {
        cf_membership_dispose(&membership);
        cf_room_dispose(&room);
        return rc;
    }

    /* `membership.involvement.map(|i| i.name()).unwrap_or_default()`. */
    cf_span involvement = {NULL, 0};
    if (membership.involvement.present) {
        const char *name = cf_involvement_name(membership.involvement.value);
        if (name == NULL) {
            cf_membership_dispose(&membership);
            cf_room_dispose(&room);
            return CF_INTERNAL;
        }
        involvement = involvement_span(name);
    }
    involvement_view model = {
        .room_id = room.id,
        .kind = room.room_type,
        .involvement = involvement,
    };
    cf_room_dispose(&room);
    cf_membership_dispose(&membership);

    const cf_format *offered[1] = {&cf_format_html};
    const cf_format *chosen = NULL;
    rc = cf_ctx_respond_to(ctx, offered, 1, &chosen);
    if (rc != CF_OK) return rc;

    cf_view_layout_model layout = {0};
    rc = cf_presenter_layout_load(ctx, cf_ctx_platform(ctx), &layout);
    if (rc != CF_OK) return rc;
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, &layout);

    bool frame = involvement_turbo_frame_request(ctx->request);
    cf_builder body = {0};
    rc = frame ? cf_view_room_involvement_show_frame(&view_ctx, &model, &body)
               : cf_view_room_involvement_show(&view_ctx, &model, &body);
    cf_view_layout_model_dispose(&layout);
    if (rc != CF_OK) {
        cf_builder_dispose(&body);
        return rc;
    }
    return involvement_page_response(ctx, 200, &body, frame);
}

/* ---- update --------------------------------------------------------------- */

/* The writer callback borrows the action's membership row (alive until
 * cf_write returns, the rooms.c destroy pattern) and stores the parsed
 * involvement through the model — exactly the source's
 * `membership.update_involvement(tx, involvement)`, including its no-change
 * short-circuit and its in-place update of the row. */
typedef struct {
    cf_membership *membership;
    cf_optional_involvement involvement;
} involvements_update_write_arg;

static cf_err involvements_update_write(cf_tx *tx, void *arg) {
    involvements_update_write_arg *write = arg;
    return cf_membership_update_involvement(tx, write->membership,
                                           write->involvement);
}

/* `c.app().broadcasts.involvement_change` (channels/broadcasts.rs) through
 * the app's cable with the production partials.  C02's post-commit delivery
 * policy: a full loop queue (CF_BUSY) drops the broadcast and is counted;
 * it must not fail the committed update (00-contracts.md).  Other failures
 * (including the unavailable sidebar partial on the prepend branch, R2)
 * propagate.  A missing cable is an internal error, never a dropped effect. */
static cf_err involvements_broadcast_change(
    cf_ctx *ctx, const cf_room *room, const cf_membership *membership,
    bool has_previous, cf_involvement previous) {
    cf_cable *cable = cf_app_cable(ctx->app);
    if (cable == NULL) return CF_INTERNAL;
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, NULL);
    cf_broadcast_views views = {ctx, &view_ctx};
    cf_broadcast_partials partials;
    cf_broadcast_partials_views(&partials, &views);
    cf_err rc = cf_broadcast_involvement_change(cable, room, membership,
                                                has_previous, previous,
                                                &partials);
    if (rc == CF_BUSY) {
        cf_cable_log("dropped broadcast", "rooms/involvements#update");
        return CF_OK;
    }
    /* `.map_err(Error::internal)`: the NilInquiry failure (a nil previous
     * involvement raising after the update has saved) is a 500, like any
     * other broadcast construction failure. */
    if (rc == CF_INVALID) return CF_INTERNAL;
    return rc;
}

/* `update`: before_actions; set_room; involvement_param; the writer's
 * update_involvement; broadcast_visibility_changes; redirect to the room
 * involvement URL — the source's order. */
cf_err cf_action_rooms_involvements_update(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = involvement_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    cf_membership membership = {0};
    cf_room room = {0};
    rc = involvement_set_room(ctx, user.id, &membership, &room);
    cf_user_dispose(&user);
    if (rc != CF_OK) {
        cf_membership_dispose(&membership);
        cf_room_dispose(&room);
        return rc;
    }

    bool present = false;
    cf_involvement involvement = CF_INVOLVEMENT_MENTIONS;
    rc = involvement_param(ctx, &present, &involvement);
    if (rc != CF_OK) {
        cf_membership_dispose(&membership);
        cf_room_dispose(&room);
        return rc;
    }

    /* `involvement_previously_was`: the value before the update (or nil,
     * whose later `nil.inquiry` is the reference's 500 — the broadcast maps
     * it to CF_INVALID after the write has committed). */
    bool has_previous = membership.involvement.present;
    cf_involvement previous = membership.involvement.value;

    involvements_update_write_arg write = {
        .membership = &membership,
        .involvement =
            {.present = present, .value = involvement},
    };
    rc = cf_write(ctx->app, involvements_update_write, &write);
    if (rc != CF_OK) {
        cf_membership_dispose(&membership);
        cf_room_dispose(&room);
        return rc;
    }

    /* broadcast_visibility_changes: render_shared_room's partials are the
     * production shared_room slot (the real _shared render since V02);
     * involvement_change with previous. */
    rc = involvements_broadcast_change(ctx, &room, &membership, has_previous,
                                       previous);
    int64_t room_id = room.id;
    cf_membership_dispose(&membership);
    cf_room_dispose(&room);
    if (rc != CF_OK) return rc;

    return involvement_redirect_to_involvement(ctx, room_id);
}
