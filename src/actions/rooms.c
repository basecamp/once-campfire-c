/* src/actions/rooms.c — A-rooms: `rooms#show` (route IDs 96, 101),
 * `rooms#index` (97) and `rooms#destroy` (104)
 * (docs/devel/implementation/contracts/controller-packets.md "A-rooms";
 * 03-application.md "First complete slice" rooms#show row).
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/rooms.rs, the pinned
 * port of reference/app/controllers/rooms_controller.rb:
 *
 *   before_action :set_room, only: %i[show destroy]
 *   before_action :remember_last_room_visited, only: %i[show]
 *   before_action :ensure_can_administer, only: %i[destroy edit update]
 *
 *   index:   `redirect_to room_url(Current.user.rooms.last)`; with no rooms
 *            `room_url(nil)` raises (Error::internal).
 *   show:    set_room(Scope::All) — room_scope.find_by(id: params[:room_id] ||
 *            params[:id]) through Room::find_for_user, else redirect to root
 *            with the "Room not found or inaccessible" alert; remember the
 *            last_room cookie when it changes; render_show.
 *   destroy: set_room(Scope::All); ensure_can_administer (administrator or
 *            the room's creator, else 403 head); Room#destroy through the
 *            writer; broadcast_remove_to :rooms, target: [@room, :list];
 *            redirect to root.
 *
 * The response conventions are the landed actions' (A-welcome/A-first_runs/
 * A-sessions): absolute URLs are PUBLIC_ORIGIN + the route path (the C port
 * has no per-request url_for), page responses append the `Link` preload
 * header of `Layout#page` and frame responses carry none, and a helper that
 * answers a request sets ctx->response and returns CF_OK (callers stop with
 * cf_auth_halted, src/auth.h).
 *
 * `rooms#show`'s presenter work (membership-scoped room lookup already done by
 * set_room; the page around a `message_id` that names a message of this room,
 * else the last page; the invitation condition; the account join code; the
 * signed room message stream) is A02's cf_presenter_room_show inside one read
 * transaction; render_show then negotiates HTML (page_or_frame's
 * find_template), loads the layout and renders the page or turbo-rails' frame
 * layout.  Translation order matches the source: presenter reads first, then
 * respond_to (page_or_frame), then Layout::load.
 *
 * Route registration: src/routes.c rows 96/97/101/104 bind
 * cf_action_rooms_show (96, 101), cf_action_rooms_index (97) and
 * cf_action_rooms_destroy (104) to this file's actions.
 *
 * destroy's post-commit `broadcast_remove_to :rooms, target: [@room, :list]`
 * goes through the app->cable accessor added to src/app.h
 * (cf_app_set_cable/cf_app_cable, called by main.c after cf_cable_create):
 * cf_broadcast_room_remove(cf_app_cable(ctx->app), &room), a CF_BUSY delivery
 * failure being recorded rather than failing the committed destroy.
 *
 * Reported dependencies (docs/devel/evidence/A-rooms.md):
 *  - A01's session commit does not yet serialize the flash map, so the
 *    set_room failure alert is set via A00's cf_ctx_flash_set (the C
 *    counterpart of kit's `flash.set_alert`) but cannot reach the redirect's
 *    cookie until A01 persists it.
 *  - `platform(c)` needs A01's UA parser; the layout is loaded with zeroed
 *    platform facts (the bell renders its generic branch), as A02 already
 *    reported.
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
#include <stdio.h>
#include <string.h>

/* ---- small helpers -------------------------------------------------------- */

static cf_span rooms_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `c.param_str(key)` (kit params.rs `ParamMap::str` -> `Param::as_str`):
 * Some only for a string param, never for null/array/object/number/bool. */
static bool rooms_param_str(cf_ctx *ctx, const char *name, cf_span *out) {
    const cf_param *param = cf_ctx_param(ctx, rooms_span(name));
    if (param == NULL || cf_param_type(param) != CF_PARAM_STRING) return false;
    return cf_param_string(param, out) == CF_OK;
}

/* `c.is_turbo_frame_request()`: kit/src/ctx.rs:294 is
 * `turbo_frame_request_id().is_some_and(|id| !id.trim().is_empty())` over
 * `request.header("turbo-frame")` (kit/src/request.rs:113:
 * `headers.get(name).and_then(|v| v.to_str().ok())`).  http 1.5.0's
 * `HeaderValue::to_str` accepts a byte only when it is HTAB or visible ASCII
 * (`b >= 32 && b < 127 || b == b'\t'`), so any control byte, DEL or byte
 * >= 0x80 makes the header read as absent; the first matching header wins
 * (case-insensitive name match). */
static unsigned char rooms_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c - 'A' + 'a') : c;
}

static bool rooms_turbo_frame_request(const cf_request *request) {
    if (request == NULL) return false;
    cf_span value = {NULL, 0};
    bool found = false;
    for (size_t i = 0; i < request->header_count; i++) {
        const cf_header *header = &request->headers[i];
        if (header->name.len != 11) continue;
        static const char name[] = "turbo-frame";
        bool match = true;
        for (size_t k = 0; k < 11; k++) {
            if (rooms_lower(header->name.ptr[k]) != (unsigned char)name[k]) {
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
        /* Not through `HeaderValue::to_str` -> the header reads as absent. */
        if (!((b >= 32 && b < 127) || b == '\t')) return false;
        if (b != ' ' && b != '\t') blank = false;
    }
    return !blank;
}

/* `redirect_to location` (Rails default 302, the absolute location, the
 * reference redirect's content type). */
static cf_err rooms_redirect_to(cf_ctx *ctx, cf_span location) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    ctx->response->status = 302;
    cf_err rc = cf_response_header(ctx->response, rooms_span("Location"),
                                   location);
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response, rooms_span("Content-Type"),
                                rooms_span("text/html; charset=utf-8"));
    }
    return rc;
}

/* `url_for(<path>)` for the C port: PUBLIC_ORIGIN + the route path. */
static cf_err rooms_redirect_path(cf_ctx *ctx, cf_span path) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    cf_builder location = {0};
    cf_err rc = cf_builder_append(&location, rooms_span(config->public_origin));
    if (rc == CF_OK) rc = cf_builder_append(&location, path);
    if (rc == CF_OK) {
        rc = rooms_redirect_to(ctx, (cf_span){location.ptr, location.len});
    }
    cf_builder_dispose(&location);
    return rc;
}

/* `redirect_to root_path` (campfire_routes::root() is "/"). */
static cf_err rooms_redirect_to_root(cf_ctx *ctx) {
    return rooms_redirect_path(ctx, rooms_span("/"));
}

/* `redirect_to room_url(room_id)`. */
static cf_err rooms_redirect_to_room(cf_ctx *ctx, int64_t room_id) {
    char path[64];
    int n = snprintf(path, sizeof path, "/rooms/%" PRId64, room_id);
    if (n < 0 || (size_t)n >= sizeof path) return CF_INTERNAL;
    return rooms_redirect_path(ctx, (cf_span){(const unsigned char *)path,
                                              (size_t)n});
}

/* `concerns::head(status)`: an empty body with the before-action fallback
 * content type `text/html` (no charset), whatever the request format. */
static cf_err rooms_head(cf_ctx *ctx, unsigned status) {
    ctx->response->status = status;
    return cf_response_header(ctx->response, rooms_span("Content-Type"),
                              rooms_span("text/html"));
}

/* `Layout#page` appends the stylesheet preload links (`Link` header); the
 * frame layout carries none.  An unconfigured asset module has no links. */
static cf_err rooms_link_header(cf_ctx *ctx) {
    cf_builder links = {0};
    cf_err rc = cf_views_preload_links(&links);
    if (rc == CF_INVALID) return CF_OK;
    if (rc == CF_OK && links.len != 0) {
        rc = cf_response_header(ctx->response, rooms_span("Link"),
                                (cf_span){links.ptr, links.len});
    }
    cf_builder_dispose(&links);
    return rc;
}

/* Freeze a rendered page into the response: `status`, HTML content type, and
 * (page layout only) the preload header.  Consumes the builder. */
static cf_err rooms_page_response(cf_ctx *ctx, unsigned status,
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
    rc = cf_response_header(ctx->response, rooms_span("Content-Type"),
                            rooms_span("text/html; charset=utf-8"));
    if (rc != CF_OK) return rc;
    if (!frame) rc = rooms_link_header(ctx);
    return rc;
}

/* ---- shared before-action helpers (rooms.rs) ------------------------------ */

/* `require_current_user(c)` (concerns.rs): the authenticated user row.  The
 * chain guarantees a signed-in identity; a missing row is the reference's
 * internal error. */
static cf_err rooms_current_user(cf_ctx *ctx, cf_user *out) {
    bool found = false;
    cf_err rc = cf_auth_current_user(ctx, &found, out);
    if (rc != CF_OK) return rc;
    if (!found) return CF_INTERNAL;
    return CF_OK;
}

/* `set_room(c, Scope::All)`: `Room::find_for_user(user, params[:room_id] ||
 * params[:id])`, else redirect to root with the alert.  Scope::All includes
 * every room the membership lookup can return, so no extra filter applies
 * (the room-type scopes live in the sibling packets).  On success *out holds
 * the room; on the failure arm the response is set and the caller stops with
 * cf_auth_halted. */
static cf_err rooms_set_room_all(cf_ctx *ctx, const cf_user *user,
                                 cf_room *out) {
    cf_span param = {NULL, 0};
    bool has = rooms_param_str(ctx, "room_id", &param);
    if (!has) has = rooms_param_str(ctx, "id", &param);
    int64_t id = 0;
    bool cast = has && cf_views_integer_cast(param, &id);
    bool found = false;
    if (cast) {
        cf_err rc = cf_room_find_for_user(ctx->reader, user->id, id, &found,
                                          out);
        if (rc != CF_OK) return rc;
    }
    if (found) return CF_OK;

    /* `redirect_to_with(root, alert: "Room not found or inaccessible")`. */
    cf_err rc = cf_ctx_flash_set(
        ctx, rooms_span("alert"),
        rooms_span("Room not found or inaccessible"));
    if (rc == CF_OK) rc = rooms_redirect_to_root(ctx);
    return rc;
}

/* `ensure_can_administer`: `head :forbidden unless
 * Current.user.can_administer?(@room)`. */
static bool rooms_can_administer(const cf_user *user, const cf_room *room) {
    return cf_user_can_administer(
        user, (cf_optional_i64){.present = true, .value = room->creator_id},
        false);
}

/* `remember_last_room_visited`: `cookies.permanent[:last_room] = @room.id`,
 * only when the cookie's value differs (the source's guard; A00 would
 * otherwise emit a Set-Cookie header for the expiry on every page). */
static cf_err rooms_remember_last_room(cf_ctx *ctx, int64_t room_id) {
    char text[32];
    int n = snprintf(text, sizeof text, "%" PRId64, room_id);
    if (n < 0 || (size_t)n >= sizeof text) return CF_INTERNAL;

    cf_span current = {NULL, 0};
    cf_err rc = cf_ctx_cookie_get(ctx, rooms_span("last_room"), &current);
    if (rc == CF_NOT_FOUND) rc = CF_OK;
    if (rc != CF_OK) return rc;
    if (current.len == (size_t)n &&
        (n == 0 || memcmp(current.ptr, text, (size_t)n) == 0)) {
        return CF_OK;
    }
    cf_cookie_options options = {0};
    options.permanent = true; /* Rails `cookies.permanent` (20 years) */
    return cf_ctx_cookie_set(ctx, rooms_span("last_room"),
                             (cf_span){(const unsigned char *)text,
                                       (size_t)n},
                             &options);
}

/* ---- actions -------------------------------------------------------------- */

/* `index`: `before_actions`; `redirect_to room_url(Current.user.rooms.last)`;
 * with no rooms `room_url(nil)` raises (Error::internal).  No cookie and no
 * format negotiation: the redirect is answered whatever the request format. */
cf_err cf_action_rooms_index(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = rooms_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }

    cf_room room = {0};
    bool found = false;
    rc = cf_room_last_for_user(ctx->reader, user.id, &found, &room);
    cf_user_dispose(&user);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        return rc;
    }
    if (!found) {
        /* `room_url(nil)` raises NoMethodError. */
        cf_room_dispose(&room);
        return CF_INTERNAL;
    }
    rc = rooms_redirect_to_room(ctx, room.id);
    cf_room_dispose(&room);
    return rc;
}

/* `show`, also `GET /rooms/:room_id/@:message_id`: `before_actions`;
 * `set_room(Scope::All)`; `remember_last_room_visited`; `render_show`. */
cf_err cf_action_rooms_show(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = rooms_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }

    cf_room room = {0};
    rc = rooms_set_room_all(ctx, &user, &room);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_room_dispose(&room);
        cf_user_dispose(&user);
        return rc;
    }

    rc = rooms_remember_last_room(ctx, room.id);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        cf_user_dispose(&user);
        return rc;
    }

    /* `render_show`: the presenter read (page around params[:message_id] when
     * it names a message of this room, else the last page; invitation; join
     * code; signed stream name), then page_or_frame's find_template(HTML),
     * then Layout::load. */
    cf_span message_param = {NULL, 0};
    bool has_message_id = rooms_param_str(ctx, "message_id", &message_param);
    int64_t message_id = 0;
    if (has_message_id) {
        has_message_id = cf_views_integer_cast(message_param, &message_id);
    }

    cf_view_room_show_model model = {0};
    rc = cf_presenter_room_show(ctx, &room, &user, has_message_id, message_id,
                                &model);
    cf_room_dispose(&room);
    cf_user_dispose(&user);
    if (rc != CF_OK) return rc;

    const cf_format *offered[1] = {&cf_format_html};
    const cf_format *chosen = NULL;
    rc = cf_ctx_respond_to(ctx, offered, 1, &chosen);
    if (rc != CF_OK) {
        cf_view_room_show_model_dispose(&model);
        return rc;
    }

    cf_view_layout_model layout = {0};
    rc = cf_presenter_layout_load(ctx, cf_ctx_platform(ctx), &layout);
    if (rc != CF_OK) {
        cf_view_room_show_model_dispose(&model);
        return rc;
    }
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, &layout);

    bool frame = rooms_turbo_frame_request(ctx->request);
    cf_builder body = {0};
    rc = frame ? cf_view_room_show_frame(&view_ctx, &model, &body)
               : cf_view_room_show(&view_ctx, &model, &body);
    if (rc == CF_OK) {
        rc = rooms_page_response(ctx, 200, &body, frame);
    } else {
        cf_builder_dispose(&body);
    }

    cf_view_layout_model_dispose(&layout);
    cf_view_room_show_model_dispose(&model);
    return rc;
}

/* `room.destroy` inside the writer transaction (P12-01).  The callback
 * carries the actor identity and revalidates the actor, the membership scope
 * and the room ownership on `cf_tx_db(tx)` before the first mutation; the
 * action's pre-write checks are only the fast path, this one is
 * authoritative (02-data-auth.md D02: "Revalidate relevant user status and
 * ownership in the write transaction even if request authentication
 * happened earlier"; writer.h makes it the caller's duty).  A ban,
 * deactivation, role demotion, membership removal or ownership change
 * committed while the callback waited for the writer therefore rejects the
 * destroy with no row changes and no broadcast.  The actor id and the room
 * row are borrowed from the action and stay alive until cf_write returns. */
typedef struct {
    int64_t actor_id;
    const cf_room *room; /* set_room(Scope::All)'s loaded room */
} rooms_destroy_write_arg;

static cf_err rooms_destroy_write(cf_tx *tx, void *arg) {
    const rooms_destroy_write_arg *write = arg;
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;
    int64_t room_id = write->room->id;

    /* The authenticated identity: a banned/deactivated actor is AUTH-05's
     * denied subsequent protected access. */
    bool found = false;
    cf_user actor = {0};
    cf_err rc = cf_user_find_by_id(db, write->actor_id, &found, &actor);
    if (rc != CF_OK) {
        cf_user_dispose(&actor);
        return rc;
    }
    if (!found || !cf_user_is_active(&actor)) {
        cf_user_dispose(&actor);
        return CF_FORBIDDEN;
    }

    /* set_room is membership-scoped: the room must still exist and still be
     * reachable by the actor.  A missing room is an honest 404 (nothing left
     * to destroy). */
    cf_room fresh = {0};
    rc = cf_room_find_by_id(db, room_id, &found, &fresh);
    if (rc != CF_OK) {
        cf_room_dispose(&fresh);
        cf_user_dispose(&actor);
        return rc;
    }
    if (!found) {
        cf_user_dispose(&actor);
        return CF_NOT_FOUND;
    }
    cf_membership membership = {0};
    bool member = false;
    rc = cf_membership_find_by_room_and_user(db, room_id, write->actor_id,
                                             &member, &membership);
    cf_membership_dispose(&membership);
    /* `ensure_can_administer`: administrator or the room's creator, over the
     * fresh row's creator_id (an ownership change is caught here). */
    bool allowed =
        rc == CF_OK && member &&
        cf_user_can_administer(
            &actor,
            (cf_optional_i64){.present = true, .value = fresh.creator_id},
            false);
    cf_room_dispose(&fresh);
    cf_user_dispose(&actor);
    if (rc != CF_OK) return rc;
    if (!allowed) return CF_FORBIDDEN;

    return cf_room_destroy(tx, write->room);
}

/* `c.app().broadcasts.room_remove(&room)` (C02): remove `[room, :list]` from
 * everyone's `:rooms` stream through the app's cable. C02's post-commit
 * delivery policy: a full loop queue (CF_BUSY, or a stopping cable) drops the
 * broadcast and is counted; it must not become an error for a committed
 * destroy (00-contracts.md). Other failures (render/allocation) propagate.
 * A missing cable is an internal error, never a silently dropped effect. */
static cf_err rooms_broadcast_room_remove(cf_ctx *ctx, const cf_room *room) {
    cf_cable *cable = cf_app_cable(ctx->app);
    if (cable == NULL) return CF_INTERNAL;
    cf_err rc = cf_broadcast_room_remove(cable, room);
    if (rc == CF_BUSY) {
        cf_cable_log("dropped broadcast", "rooms#destroy");
        return CF_OK;
    }
    return rc;
}

/* `destroy` (RoomsController and Rooms::DirectsController): `before_actions`;
 * `set_room(Scope::All)`; `ensure_can_administer`; `destroy_room`: the
 * Room#destroy write, then `broadcast_remove_to :rooms, target: [@room,
 * :list]` (cf_broadcast_room_remove through the app's cable), then the root
 * redirect — the source's order. */
cf_err cf_action_rooms_destroy(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = rooms_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }

    cf_room room = {0};
    rc = rooms_set_room_all(ctx, &user, &room);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_room_dispose(&room);
        cf_user_dispose(&user);
        return rc;
    }

    if (!rooms_can_administer(&user, &room)) {
        cf_room_dispose(&room);
        cf_user_dispose(&user);
        return rooms_head(ctx, 403);
    }
    int64_t actor_id = user.id;
    cf_user_dispose(&user);

    rooms_destroy_write_arg write = {.actor_id = actor_id, .room = &room};
    rc = cf_write(ctx->app, rooms_destroy_write, &write);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        return rc;
    }
    /* After the commit, before the redirect (rooms.rs destroy_room). */
    rc = rooms_broadcast_room_remove(ctx, &room);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        return rc;
    }
    rc = rooms_redirect_to_root(ctx);
    cf_room_dispose(&room);
    return rc;
}
