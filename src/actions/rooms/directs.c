/* src/actions/rooms/directs.c — A-rooms-directs: `rooms/directs#index`
 * (route ID 121), `rooms/directs#create` (122), `rooms/directs#new` (123),
 * `rooms/directs#edit` (124), `rooms/directs#show` (125) and
 * `rooms/directs#destroy` (128)
 * (docs/devel/implementation/contracts/controller-packets.md
 * "A-rooms-directs").  There is deliberately no directs#update: routes 126
 * and 127 are H03 reference errors (`action_not_found`), and this packet
 * implements nothing for them.
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/rooms.rs plus
 * tmp/rust-ref/crates/campfire/src/controllers/rooms/directs.rs, the pinned
 * port of reference/app/controllers/rooms/directs_controller.rb:
 *
 *   before_action :set_room, only: %i[edit destroy]   (room_scope:
 *     Current.user.rooms.directs — every other type is out of reach, which
 *     is why the relaxed administer check below is safe)
 *
 *   index:   RoomsController's `rooms::index` (redirect to the last room;
 *            `room_url(nil)` raises with no rooms).
 *   show:    inherited from RoomsController WITHOUT `set_room`, so
 *            `remember_last_room_visited` would raise on the nil `@room`;
 *            the port answers `redirect_to room_url(params[:id])` instead —
 *            an unparsable/missing id is NotFound, with no membership check
 *            and no last_room cookie.
 *   new:     the DirectsNew form (no create-permission gate: direct rooms
 *            need none).
 *   create:  `User.where(id: selected_users_ids.including(Current.user.id))`
 *            — the submitted `user_ids` plus the actor, filtered to existing
 *            users; `Rooms::Direct.find_or_create_direct_for` (the direct
 *            room whose members are exactly that set, created when missing);
 *            one direct-room partial per membership, prepended to its user's
 *            own rooms stream; redirect to the room.
 *   edit:    set_room(Directs); the DirectEditView (the display name for the
 *            current user, `@room.users.many? ? @room.users.without(
 *            Current.user) : @room.users`).
 *   destroy: RoomsController's `destroy` WITH this controller's `set_room`
 *            and its `ensure_can_administer` that always passes: every member
 *            of a direct room can destroy it — `Room#destroy` through the
 *            writer, `broadcast_remove_to :rooms, target: [@room, :list]`,
 *            redirect to root.
 *
 * The response conventions are the landed actions' (A-rooms/A-messages, and
 * the sibling rooms/involvements.c + rooms/refreshes.c + rooms/opens.c +
 * rooms/closeds.c packets): helpers that answer the request set
 * ctx->response and return CF_OK (callers stop with cf_auth_halted,
 * src/auth.h); page renders carry the layout `Link` preload header and frame
 * renders none; redirects are PUBLIC_ORIGIN + the route path.  Small helpers
 * duplicated from src/actions/rooms.c and rooms/opens.c are static copies
 * clearly marked for later dedup; shared files are integrator-owned and this
 * packet must not touch them.
 *
 * Route registration: src/routes.c rows 121-125 and 128 bind this file's
 * actions (integrator-owned; rows currently on the development 501 until
 * this packet verifies).  Rows 126/127 stay on the reference
 * `action_not_found` error and are not implemented here.
 *
 * V02 swaps: DirectsNew/DirectsEdit render through the real golden-verified
 * cf_view_rooms_direct_new/edit(_frame) (models in src/views.h), and the
 * create broadcast carries the production `Partials::direct_room` renderer
 * (cf_view_rooms_direct_room_partial via cf_broadcast_partials_views, once
 * per membership so each recipient gets its own unread state), so no
 * placeholder markup remains here.
 */
#include "cf.h"

#include "app.h"
#include "auth.h"
#include "cable/channels.h"
#include "config.h"
#include "context.h"
#include "db/writer.h"
#include "models/membership.h"
#include "models/room.h"
#include "models/user.h"
#include "views.h"
#include "views/internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- small helpers (static copies of the rooms.c/opens.c base) ------------ */

static cf_span directs_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `c.param_str(key)` (kit params.rs `ParamMap::str` -> `Param::as_str`):
 * Some only for a string param, never for null/array/object/number/bool. */
static bool directs_param_str(cf_ctx *ctx, const char *name, cf_span *out) {
    const cf_param *param = cf_ctx_param(ctx, directs_span(name));
    if (param == NULL || cf_param_type(param) != CF_PARAM_STRING) return false;
    return cf_param_string(param, out) == CF_OK;
}

static unsigned char directs_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c - 'A' + 'a') : c;
}

/* `c.is_turbo_frame_request()` (kit ctx.rs; the rooms.c copy). */
static bool directs_turbo_frame_request(const cf_request *request) {
    if (request == NULL) return false;
    cf_span value = {NULL, 0};
    bool found = false;
    for (size_t i = 0; i < request->header_count; i++) {
        const cf_header *header = &request->headers[i];
        if (header->name.len != 11) continue;
        static const char name[] = "turbo-frame";
        bool match = true;
        for (size_t k = 0; k < 11; k++) {
            if (directs_lower(header->name.ptr[k]) !=
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
static cf_err directs_redirect_to(cf_ctx *ctx, cf_span location) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    ctx->response->status = 302;
    cf_err rc = cf_response_header(ctx->response, directs_span("Location"),
                                   location);
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response, directs_span("Content-Type"),
                                directs_span("text/html; charset=utf-8"));
    }
    return rc;
}

/* `url_for(<path>)` for the C port: PUBLIC_ORIGIN + the route path. */
static cf_err directs_redirect_path(cf_ctx *ctx, cf_span path) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    cf_builder location = {0};
    cf_err rc =
        cf_builder_append(&location, directs_span(config->public_origin));
    if (rc == CF_OK) rc = cf_builder_append(&location, path);
    if (rc == CF_OK) {
        rc = directs_redirect_to(ctx, (cf_span){location.ptr, location.len});
    }
    cf_builder_dispose(&location);
    return rc;
}

/* `redirect_to root_path` (campfire_routes::root() is "/"). */
static cf_err directs_redirect_to_root(cf_ctx *ctx) {
    return directs_redirect_path(ctx, directs_span("/"));
}

/* `redirect_to room_url(room_id)`. */
static cf_err directs_redirect_to_room(cf_ctx *ctx, int64_t room_id) {
    char path[64];
    int n = snprintf(path, sizeof path, "/rooms/%" PRId64, room_id);
    if (n < 0 || (size_t)n >= sizeof path) return CF_INTERNAL;
    return directs_redirect_path(ctx, (cf_span){(const unsigned char *)path,
                                                (size_t)n});
}

/* `Layout#page` appends the stylesheet preload links (`Link` header); the
 * frame layout carries none.  An unconfigured asset module has no links. */
static cf_err directs_link_header(cf_ctx *ctx) {
    cf_builder links = {0};
    cf_err rc = cf_views_preload_links(&links);
    if (rc == CF_INVALID) return CF_OK;
    if (rc == CF_OK && links.len != 0) {
        rc = cf_response_header(ctx->response, directs_span("Link"),
                                (cf_span){links.ptr, links.len});
    }
    cf_builder_dispose(&links);
    return rc;
}

/* Freeze a rendered form response: `status`, HTML content type, `Vary:
 * Accept` when negotiation ran (the involvements.c convention for framed
 * pages), and (page layout only) the preload header.  Consumes the builder. */
static cf_err directs_page_response(cf_ctx *ctx, unsigned status,
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
    rc = cf_response_header(ctx->response, directs_span("Content-Type"),
                            directs_span("text/html; charset=utf-8"));
    if (rc == CF_OK && cf_ctx_vary_accept(ctx)) {
        rc = cf_response_header(ctx->response, directs_span("Vary"),
                                directs_span("Accept"));
    }
    if (rc != CF_OK) return rc;
    if (!frame) rc = directs_link_header(ctx);
    return rc;
}

/* `require_current_user(c)` (concerns.rs): the authenticated user row.  The
 * chain guarantees a signed-in identity; a missing row is the reference's
 * internal error. */
static cf_err directs_current_user(cf_ctx *ctx, cf_user *out) {
    bool found = false;
    cf_err rc = cf_auth_current_user(ctx, &found, out);
    if (rc != CF_OK) return rc;
    if (!found) return CF_INTERNAL;
    return CF_OK;
}

/* `set_room(c, Scope::Directs)`: `Room::find_for_user(user, params[:room_id]
 * || params[:id])` filtered to direct rooms (every other type is out of
 * reach for this controller), else redirect to root with the alert.  On
 * success *out holds the room; on the failure arm the response is set and
 * the caller stops with cf_auth_halted. */
static cf_err directs_set_room(cf_ctx *ctx, const cf_user *user, cf_room *out) {
    cf_span param = {NULL, 0};
    bool has = directs_param_str(ctx, "room_id", &param);
    if (!has) has = directs_param_str(ctx, "id", &param);
    int64_t id = 0;
    bool cast = has && cf_views_integer_cast(param, &id);
    bool found = false;
    if (cast) {
        cf_err rc = cf_room_find_for_user(ctx->reader, user->id, id, &found,
                                          out);
        if (rc != CF_OK) return rc;
        if (found && !cf_room_direct(out)) {
            cf_room_dispose(out);
            memset(out, 0, sizeof *out);
            found = false;
        }
    }
    if (found) return CF_OK;

    /* `redirect_to_with(root, alert: "Room not found or inaccessible")`. */
    cf_err rc = cf_ctx_flash_set(
        ctx, directs_span("alert"),
        directs_span("Room not found or inaccessible"));
    if (rc == CF_OK) rc = directs_redirect_to_root(ctx);
    return rc;
}

/* ---- parameters (directs.rs user_ids_param) -------------------------------- */

/* `params.fetch(:user_ids, [])` as ids `User.where(id:)` can match (the
 * closeds.c copy): an array contributes its string elements through
 * integer_cast; any other submitted value contributes at most itself; a
 * missing key contributes nothing. */
static cf_err directs_user_ids_param(cf_ctx *ctx, int64_t **out,
                                     size_t *count_out) {
    *out = NULL;
    *count_out = 0;
    const cf_param *param = cf_ctx_param(ctx, directs_span("user_ids"));
    if (param == NULL) return CF_OK;
    if (cf_param_type(param) == CF_PARAM_ARRAY) {
        size_t count = cf_param_count(param);
        int64_t *ids = malloc(count == 0 ? 1 : count * sizeof *ids);
        if (ids == NULL) return CF_NOMEM;
        size_t kept = 0;
        for (size_t i = 0; i < count; i++) {
            const cf_param *element = cf_param_at(param, i);
            if (element == NULL ||
                cf_param_type(element) != CF_PARAM_STRING) {
                continue;
            }
            cf_span text = {NULL, 0};
            int64_t id = 0;
            if (cf_param_string(element, &text) == CF_OK &&
                cf_views_integer_cast(text, &id)) {
                ids[kept++] = id;
            }
        }
        *out = ids;
        *count_out = kept;
        return CF_OK;
    }
    if (cf_param_type(param) != CF_PARAM_STRING) return CF_OK;
    cf_span text = {NULL, 0};
    int64_t id = 0;
    if (cf_param_string(param, &text) != CF_OK ||
        !cf_views_integer_cast(text, &id)) {
        return CF_OK;
    }
    int64_t *ids = malloc(sizeof *ids);
    if (ids == NULL) return CF_NOMEM;
    ids[0] = id;
    *out = ids;
    *count_out = 1;
    return CF_OK;
}

/* `User.where(id: ids)`, as ids of existing users (in id order, like the
 * query). */
static cf_err directs_existing_user_ids(cf_db *db, const int64_t *ids,
                                        size_t ids_len, int64_t **out,
                                        size_t *count_out) {
    *out = NULL;
    *count_out = 0;
    cf_user_vector users = {0};
    cf_err rc = cf_user_where_ids(db, ids, ids_len, &users);
    if (rc != CF_OK) {
        cf_user_vector_dispose(&users);
        return rc;
    }
    int64_t *existing =
        malloc(users.len == 0 ? 1 : users.len * sizeof *existing);
    if (existing == NULL) {
        cf_user_vector_dispose(&users);
        return CF_NOMEM;
    }
    for (size_t i = 0; i < users.len; i++) existing[i] = users.items[i].id;
    *out = existing;
    *count_out = users.len;
    cf_user_vector_dispose(&users);
    return CF_OK;
}

/* ---- the DirectsNew/DirectsEdit render ------------------------------------ */

static void directs_views_dispose(cf_view_user *views, size_t count) {
    if (views == NULL) return;
    for (size_t i = 0; i < count; i++) cf_view_user_dispose(&views[i]);
    free(views);
}

/* Render DirectsNew (`model` NULL) or DirectsEdit: respond_to (HTML only),
 * then Layout::load (its last_room_visited feeds the edit back link), then
 * the golden-verified cf_view_rooms_direct_new/edit renderer. */
static cf_err directs_render(cf_ctx *ctx, bool is_new,
                             const cf_view_rooms_direct_edit_model *model) {
    const cf_format *offered[1] = {&cf_format_html};
    const cf_format *chosen = NULL;
    cf_err rc = cf_ctx_respond_to(ctx, offered, 1, &chosen);
    if (rc != CF_OK) return rc;

    cf_view_layout_model layout = {0};
    rc = cf_presenter_layout_load(ctx, cf_ctx_platform(ctx), &layout);
    if (rc != CF_OK) return rc;
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, &layout);

    /* The model's back link comes from the ViewContext selection the layout
     * presenter resolved (the edit template's `link_back_to_last_room`). */
    cf_view_rooms_direct_edit_model local;
    if (!is_new) {
        local = *model;
        local.has_last_room_id = layout.has_last_room_visited;
        local.last_room_id = layout.last_room_visited_id;
        model = &local;
    }

    bool frame = directs_turbo_frame_request(ctx->request);
    cf_builder body = {0};
    if (is_new) {
        rc = frame ? cf_view_rooms_direct_new_frame(&view_ctx, &body)
                   : cf_view_rooms_direct_new(&view_ctx, &body);
    } else {
        rc = frame ? cf_view_rooms_direct_edit_frame(&view_ctx, model, &body)
                   : cf_view_rooms_direct_edit(&view_ctx, model, &body);
    }
    cf_view_layout_model_dispose(&layout);
    if (rc != CF_OK) {
        cf_builder_dispose(&body);
        return rc;
    }
    return directs_page_response(ctx, 200, &body, frame);
}

/* ---- writer callbacks ----------------------------------------------------- */

/* `Rooms::Direct.find_or_create_direct_for(users)`: the submitted ids plus
 * the actor, filtered to existing users inside the transaction
 * (`User.where(id:)`).  The actor is revalidated (02-data-auth.md D02): a
 * banned/deactivated actor is AUTH-05's denied subsequent access. */
typedef struct {
    int64_t actor_id;
    const int64_t *user_ids; /* borrowed from the action; alive for cf_write */
    size_t user_ids_len;
    cf_room *out;
} directs_create_write_arg;

static cf_err directs_create_write(cf_tx *tx, void *arg) {
    const directs_create_write_arg *write = arg;
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;

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
    cf_user_dispose(&actor);

    int64_t *existing = NULL;
    size_t existing_len = 0;
    rc = directs_existing_user_ids(db, write->user_ids, write->user_ids_len,
                                   &existing, &existing_len);
    if (rc != CF_OK) {
        free(existing);
        return rc;
    }
    rc = cf_room_find_or_create_direct_for(tx, existing, existing_len,
                                           write->actor_id, write->out);
    free(existing);
    return rc;
}

/* `Room#destroy` through the writer with in-transaction revalidation (the
 * rooms.c destroy pattern, without the administer grant: every member of a
 * direct room can destroy it).  A banned/deactivated actor, a room that no
 * longer exists (honest 404), a room that left direct scope, or a lost
 * membership rejects the destroy with no row changes and no broadcast. */
typedef struct {
    int64_t actor_id;
    const cf_room *room; /* set_room(Directs)'s loaded room */
} directs_destroy_write_arg;

static cf_err directs_destroy_write(cf_tx *tx, void *arg) {
    const directs_destroy_write_arg *write = arg;
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;
    int64_t room_id = write->room->id;

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
    cf_user_dispose(&actor);

    cf_room fresh = {0};
    rc = cf_room_find_by_id(db, room_id, &found, &fresh);
    if (rc != CF_OK) {
        cf_room_dispose(&fresh);
        return rc;
    }
    if (!found) return CF_NOT_FOUND;
    bool direct = cf_room_direct(&fresh);
    cf_room_dispose(&fresh);
    if (!direct) return CF_FORBIDDEN;

    cf_membership membership = {0};
    bool member = false;
    rc = cf_membership_find_by_room_and_user(db, room_id, write->actor_id,
                                             &member, &membership);
    cf_membership_dispose(&membership);
    if (rc != CF_OK) return rc;
    if (!member) return CF_FORBIDDEN;

    return cf_room_destroy(tx, write->room);
}

/* ---- broadcasts ----------------------------------------------------------- */

/* `broadcast_create_room` (directs.rs): the direct partial per membership,
 * to its user's own rooms stream, through the app's cable.  Post-commit
 * delivery policy (00-contracts.md; the rooms.c convention): a full loop
 * queue (CF_BUSY, or a stopping cable) drops the broadcast and is counted;
 * it must not fail the committed mutation.  Other failures propagate.  A
 * missing cable is an internal error. */
static cf_err directs_broadcast_create(cf_ctx *ctx, const cf_room *room) {
    cf_cable *cable = cf_app_cable(ctx->app);
    if (cable == NULL) return CF_INTERNAL;
    /* The production partials render `users/sidebars/rooms/_direct` once per
     * membership (its own unread state) through the views pair; the streams,
     * action, target and delivery path are `broadcast_prepend_to
     * membership.user, :rooms, target: :direct_rooms`. */
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, NULL);
    cf_broadcast_views views = {ctx, &view_ctx};
    cf_broadcast_partials partials;
    cf_broadcast_partials_views(&partials, &views);
    cf_err rc =
        cf_broadcast_direct_room_create(ctx->reader, cable, room, &partials);
    if (rc == CF_BUSY) {
        cf_cable_log("dropped broadcast", "rooms/directs#create");
        return CF_OK;
    }
    return rc;
}

/* `c.app().broadcasts.room_remove(&room)` (rooms.rs destroy_room) for the
 * directs destroy: remove `[room, :list]` from everyone's `:rooms` stream.
 * Same delivery policy as above. */
static cf_err directs_broadcast_room_remove(cf_ctx *ctx, const cf_room *room) {
    cf_cable *cable = cf_app_cable(ctx->app);
    if (cable == NULL) return CF_INTERNAL;
    cf_err rc = cf_broadcast_room_remove(cable, room);
    if (rc == CF_BUSY) {
        cf_cable_log("dropped broadcast", "rooms/directs#destroy");
        return CF_OK;
    }
    return rc;
}

/* ---- actions -------------------------------------------------------------- */

/* `index` (RoomsController's `rooms::index`, shared by the three room-type
 * controllers). */
cf_err cf_action_rooms_directs_index(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = directs_current_user(ctx, &user);
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
    rc = directs_redirect_to_room(ctx, room.id);
    cf_room_dispose(&room);
    return rc;
}

/* `create`: `before_actions`; the submitted `user_ids` plus the actor,
 * filtered to existing users; the `find_or_create_direct_for` write;
 * `broadcast_create_room`; redirect to the room — the source's order. */
cf_err cf_action_rooms_directs_create(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = directs_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    int64_t actor_id = user.id;
    cf_user_dispose(&user);

    /* `selected_users_ids.including(Current.user.id)`. */
    int64_t *submitted = NULL;
    size_t submitted_len = 0;
    rc = directs_user_ids_param(ctx, &submitted, &submitted_len);
    if (rc != CF_OK) {
        free(submitted);
        return rc;
    }
    int64_t *ids = malloc((submitted_len + 1 == 0 ? 1 : submitted_len + 1) *
                          sizeof *ids);
    if (ids == NULL) {
        free(submitted);
        return CF_NOMEM;
    }
    for (size_t i = 0; i < submitted_len; i++) ids[i] = submitted[i];
    ids[submitted_len] = actor_id;
    size_t ids_len = submitted_len + 1;
    free(submitted);

    cf_room room = {0};
    directs_create_write_arg write = {
        .actor_id = actor_id,
        .user_ids = ids,
        .user_ids_len = ids_len,
        .out = &room,
    };
    rc = cf_write(ctx->app, directs_create_write, &write);
    free(ids);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        return rc;
    }
    /* After the commit, before the redirect (directs.rs create). */
    rc = directs_broadcast_create(ctx, &room);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        return rc;
    }
    int64_t room_id = room.id;
    cf_room_dispose(&room);
    return directs_redirect_to_room(ctx, room_id);
}

/* `new`: `before_actions`; the DirectsNew form over `User.active.ordered`. */
cf_err cf_action_rooms_directs_new(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = directs_current_user(ctx, &user);
    cf_user_dispose(&user);
    if (rc != CF_OK) return rc;

    /* DirectsNew carries no locals (Rust `DirectsNew { ctx }`): the
     * autocomplete template needs no user list, so no read happens here. */
    return directs_render(ctx, true, NULL);
}

/* `edit`: `before_actions`; `set_room(Directs)`; the DirectEditView (the
 * display name for the current user; many members list everyone but the
 * current user, otherwise the sole member). */
cf_err cf_action_rooms_directs_edit(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user current = {0};
    rc = directs_current_user(ctx, &current);
    if (rc != CF_OK) {
        cf_user_dispose(&current);
        return rc;
    }

    cf_room room = {0};
    rc = directs_set_room(ctx, &current, &room);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_room_dispose(&room);
        cf_user_dispose(&current);
        return rc;
    }

    /* `@room.users.many? ? @room.users.without(Current.user) : @room.users`
     * plus `room_display_name(room, for_user: Current.user)` (the
     * presenter rooms.c copy for direct rooms). */
    cf_user_vector members = {0};
    rc = cf_room_users(ctx->reader, &room, &members);
    if (rc != CF_OK) {
        cf_user_vector_dispose(&members);
        cf_room_dispose(&room);
        cf_user_dispose(&current);
        return rc;
    }
    bool many = members.len > 1;
    size_t listed = 0;
    for (size_t i = 0; i < members.len; i++) {
        if (many && members.items[i].id == current.id) continue;
        listed++;
    }
    cf_view_user *views = NULL;
    cf_span *other_names = NULL;
    if (listed != 0) {
        views = calloc(listed, sizeof *views);
        other_names = calloc(members.len == 0 ? 1 : members.len,
                             sizeof *other_names);
        if (views == NULL || other_names == NULL) {
            free(views);
            free(other_names);
            cf_user_vector_dispose(&members);
            cf_room_dispose(&room);
            cf_user_dispose(&current);
            return CF_NOMEM;
        }
    }
    size_t done = 0, others = 0;
    for (size_t i = 0; i < members.len && rc == CF_OK; i++) {
        if (current.id != members.items[i].id) {
            other_names[others++] = (cf_span){
                (const unsigned char *)members.items[i].name.ptr,
                members.items[i].name.len};
        }
        if (many && members.items[i].id == current.id) continue;
        rc = cf_presenter_user_view(ctx, &members.items[i], &views[done]);
        if (rc == CF_OK) done++;
    }
    cf_str display_name = {NULL, 0};
    if (rc == CF_OK) {
        cf_span own = {(const unsigned char *)current.name.ptr,
                       current.name.len};
        rc = cf_view_room_display_name((cf_span){NULL, 0}, true, other_names,
                                       others, own, &display_name);
    }
    free(other_names);
    if (rc != CF_OK) {
        directs_views_dispose(views, done);
        cf_user_vector_dispose(&members);
        cf_room_dispose(&room);
        cf_user_dispose(&current);
        return rc;
    }

    cf_view_rooms_direct_edit_model model;
    memset(&model, 0, sizeof model);
    model.room_id = room.id;
    model.display_name = display_name;
    model.users = views;
    model.user_count = done;
    rc = directs_render(ctx, false, &model);
    cf_view_rooms_direct_edit_dispose(&model);
    directs_views_dispose(views, done);
    cf_user_vector_dispose(&members);
    cf_room_dispose(&room);
    cf_user_dispose(&current);
    return rc;
}

/* `show`: `before_actions`; `redirect_to room_url(params[:id])` — a missing
 * or unparsable id is NotFound.  No membership check and no last_room cookie
 * (the port's answer where Rails raised on the nil `@room`). */
cf_err cf_action_rooms_directs_show(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = directs_current_user(ctx, &user);
    cf_user_dispose(&user);
    if (rc != CF_OK) return rc;

    cf_span param = {NULL, 0};
    int64_t id = 0;
    if (!directs_param_str(ctx, "id", &param) ||
        !cf_views_integer_cast(param, &id)) {
        return CF_NOT_FOUND;
    }
    return directs_redirect_to_room(ctx, id);
}

/* `destroy` (RoomsController's with this controller's `set_room` and its
 * always-passing `ensure_can_administer`): `before_actions`;
 * `set_room(Directs)`; `destroy_room` — the `Room#destroy` write, then
 * `broadcast_remove_to :rooms, target: [@room, :list]`, then the root
 * redirect. */
cf_err cf_action_rooms_directs_destroy(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = directs_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }

    cf_room room = {0};
    rc = directs_set_room(ctx, &user, &room);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_room_dispose(&room);
        cf_user_dispose(&user);
        return rc;
    }
    /* `ensure_can_administer`: every member of a direct room can. */
    int64_t actor_id = user.id;
    cf_user_dispose(&user);

    directs_destroy_write_arg write = {.actor_id = actor_id, .room = &room};
    rc = cf_write(ctx->app, directs_destroy_write, &write);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        return rc;
    }
    /* After the commit, before the redirect (rooms.rs destroy_room). */
    rc = directs_broadcast_room_remove(ctx, &room);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        return rc;
    }
    rc = directs_redirect_to_root(ctx);
    cf_room_dispose(&room);
    return rc;
}
