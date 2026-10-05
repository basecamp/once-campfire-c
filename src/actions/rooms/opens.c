/* src/actions/rooms/opens.c — A-rooms-opens: `rooms/opens#index` (route ID
 * 105), `rooms/opens#create` (106), `rooms/opens#new` (107),
 * `rooms/opens#edit` (108), `rooms/opens#show` (109), `rooms/opens#update`
 * (110, 111) and `rooms/opens#destroy` (112)
 * (docs/devel/implementation/contracts/controller-packets.md "A-rooms-opens").
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/rooms.rs plus
 * tmp/rust-ref/crates/campfire/src/controllers/rooms/opens.rs, the pinned
 * port of reference/app/controllers/rooms/opens_controller.rb:
 *
 *   before_action :set_room, only: %i[show edit update]   (room_scope:
 *     Current.user.rooms.without_directs — direct rooms never in reach)
 *   before_action :ensure_can_administer, only: %i[update]
 *   before_action :remember_last_room_visited, only: :show
 *   before_action :force_room_type, only: %i[edit update]
 *   before_action :ensure_permission_to_create_rooms, only: %i[new create]
 *
 *   index:   RoomsController's `rooms::index` — `redirect_to
 *            room_url(Current.user.rooms.last)`; with no rooms `room_url(nil)`
 *            raises (Error::internal).
 *   show:    set_room(WithoutDirects); remember_last_room_visited;
 *            `redirect_to room_url(@room)`.
 *   new:     ensure_permission_to_create_rooms; the OpenFormView
 *            (`Rooms::Open.new(name: DEFAULT_ROOM_NAME)`, can_administer
 *            true, `User.active.ordered`).
 *   create:  ensure_permission_to_create_rooms; `params.require(:room).permit(
 *            :name)`; `Rooms::Open.create_for(room_params, users:
 *            Current.user)`; broadcast_prepend_to :rooms (the shared-room
 *            partial); redirect to the room.
 *   edit:    set_room(WithoutDirects); force_room_type (`becomes!(
 *            Rooms::Open)` — a closed room edits as open and converts on
 *            save); the OpenFormView with can_administer computed.
 *   update:  set_room(WithoutDirects); ensure_can_administer;
 *            force_room_type; `@room.update! room_params` (name plus the open
 *            type — converting a closed room grants every active user after
 *            commit, per the model); broadcast_replace_to :rooms; redirect
 *            to the room.
 *   destroy: inherited from RoomsController WITHOUT `set_room`
 *            (`super::destroy_without_room`): `nil.destroy` raises
 *            NoMethodError (Error::internal) — never a redirect, never a row
 *            change.  There is deliberately no opens#destroy success path.
 *
 * The response conventions are the landed actions' (A-rooms/A-messages, and
 * the sibling rooms/involvements.c + rooms/refreshes.c packets): helpers that
 * answer the request set ctx->response and return CF_OK (callers stop with
 * cf_auth_halted, src/auth.h); page renders carry the layout `Link` preload
 * header and frame renders none; redirects are PUBLIC_ORIGIN + the route
 * path.  Small helpers duplicated from src/actions/rooms.c (redirects, the
 * turbo-frame predicate, current user, remember_last_room, the WithoutDirects
 * set_room arm) are static copies clearly marked for later dedup; shared
 * files are integrator-owned and this packet must not touch them.
 *
 * Route registration: src/routes.c rows 105-112 bind this file's actions
 * (integrator-owned; rows currently on the development 501 until this packet
 * verifies).
 *
 * Integrator requests (views packet; this file is self-contained meanwhile):
 *  R1. cf_view_rooms_open_form / cf_view_rooms_open_form_frame — the
 *      OpensNew/OpensEdit templates (rooms/opens/new.html, rooms/opens/
 *      edit.html with the room form, the Everyone/access row, the active-user
 *      list and the delete-room section shown only when can_administer).
 *      Proposed declarations (src/views.h):
 *        typedef struct { bool is_new; int64_t room_id; cf_str name;
 *                         bool can_administer; cf_view_user *users;
 *                         size_t user_count; } cf_view_rooms_open_form;
 *        cf_err cf_view_rooms_open_form(const cf_view_ctx *ctx,
 *            const cf_view_rooms_open_form *model, cf_builder *out);
 *        cf_err cf_view_rooms_open_form_frame(const cf_view_ctx *ctx,
 *            const cf_view_rooms_open_form *model, cf_builder *out);
 *      Until they land, the static functions below render the same form
 *      skeleton (action, room[name] value, user names, conditional delete
 *      section) with a clearly marked placeholder body.
 *  R2. The sidebar shared-room partial behind render_shared_room
 *      (rooms.rs render_shared_room: users/sidebars/rooms/_shared), which
 *      the create/update broadcasts carry.  The action supplies its own
 *      static shim partial (see opens_shared_room_partial) so the broadcast
 *      plumbing stays exact; swap in the real renderer when it lands.
 */
#include "cf.h"

#include "app.h"
#include "auth.h"
#include "cable/channels.h"
#include "config.h"
#include "context.h"
#include "db/writer.h"
#include "models/account.h"
#include "models/membership.h"
#include "models/room.h"
#include "models/user.h"
#include "views.h"
#include "views/internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- small helpers (static copies of the rooms.c base; see header) ------- */

static cf_span opens_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `c.param_str(key)` (kit params.rs `ParamMap::str` -> `Param::as_str`):
 * Some only for a string param, never for null/array/object/number/bool. */
static bool opens_param_str(cf_ctx *ctx, const char *name, cf_span *out) {
    const cf_param *param = cf_ctx_param(ctx, opens_span(name));
    if (param == NULL || cf_param_type(param) != CF_PARAM_STRING) return false;
    return cf_param_string(param, out) == CF_OK;
}

static unsigned char opens_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c - 'A' + 'a') : c;
}

/* `c.is_turbo_frame_request()` (kit ctx.rs; the rooms.c copy). */
static bool opens_turbo_frame_request(const cf_request *request) {
    if (request == NULL) return false;
    cf_span value = {NULL, 0};
    bool found = false;
    for (size_t i = 0; i < request->header_count; i++) {
        const cf_header *header = &request->headers[i];
        if (header->name.len != 11) continue;
        static const char name[] = "turbo-frame";
        bool match = true;
        for (size_t k = 0; k < 11; k++) {
            if (opens_lower(header->name.ptr[k]) != (unsigned char)name[k]) {
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
static cf_err opens_redirect_to(cf_ctx *ctx, cf_span location) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    ctx->response->status = 302;
    cf_err rc = cf_response_header(ctx->response, opens_span("Location"),
                                   location);
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response, opens_span("Content-Type"),
                                opens_span("text/html; charset=utf-8"));
    }
    return rc;
}

/* `url_for(<path>)` for the C port: PUBLIC_ORIGIN + the route path. */
static cf_err opens_redirect_path(cf_ctx *ctx, cf_span path) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    cf_builder location = {0};
    cf_err rc = cf_builder_append(&location, opens_span(config->public_origin));
    if (rc == CF_OK) rc = cf_builder_append(&location, path);
    if (rc == CF_OK) {
        rc = opens_redirect_to(ctx, (cf_span){location.ptr, location.len});
    }
    cf_builder_dispose(&location);
    return rc;
}

/* `redirect_to root_path` (campfire_routes::root() is "/"). */
static cf_err opens_redirect_to_root(cf_ctx *ctx) {
    return opens_redirect_path(ctx, opens_span("/"));
}

/* `redirect_to room_url(room_id)`. */
static cf_err opens_redirect_to_room(cf_ctx *ctx, int64_t room_id) {
    char path[64];
    int n = snprintf(path, sizeof path, "/rooms/%" PRId64, room_id);
    if (n < 0 || (size_t)n >= sizeof path) return CF_INTERNAL;
    return opens_redirect_path(ctx, (cf_span){(const unsigned char *)path,
                                              (size_t)n});
}

/* `concerns::head(status)`: an empty body with the before-action fallback
 * content type `text/html` (no charset), whatever the request format. */
static cf_err opens_head(cf_ctx *ctx, unsigned status) {
    ctx->response->status = status;
    return cf_response_header(ctx->response, opens_span("Content-Type"),
                              opens_span("text/html"));
}

/* `Layout#page` appends the stylesheet preload links (`Link` header); the
 * frame layout carries none.  An unconfigured asset module has no links. */
static cf_err opens_link_header(cf_ctx *ctx) {
    cf_builder links = {0};
    cf_err rc = cf_views_preload_links(&links);
    if (rc == CF_INVALID) return CF_OK;
    if (rc == CF_OK && links.len != 0) {
        rc = cf_response_header(ctx->response, opens_span("Link"),
                                (cf_span){links.ptr, links.len});
    }
    cf_builder_dispose(&links);
    return rc;
}

/* Freeze a rendered form response: `status`, HTML content type, `Vary:
 * Accept` when negotiation ran (the involvements.c convention for framed
 * pages), and (page layout only) the preload header.  Consumes the builder. */
static cf_err opens_page_response(cf_ctx *ctx, unsigned status,
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
    rc = cf_response_header(ctx->response, opens_span("Content-Type"),
                            opens_span("text/html; charset=utf-8"));
    if (rc == CF_OK && cf_ctx_vary_accept(ctx)) {
        rc = cf_response_header(ctx->response, opens_span("Vary"),
                                opens_span("Accept"));
    }
    if (rc != CF_OK) return rc;
    if (!frame) rc = opens_link_header(ctx);
    return rc;
}

/* `require_current_user(c)` (concerns.rs): the authenticated user row.  The
 * chain guarantees a signed-in identity; a missing row is the reference's
 * internal error. */
static cf_err opens_current_user(cf_ctx *ctx, cf_user *out) {
    bool found = false;
    cf_err rc = cf_auth_current_user(ctx, &found, out);
    if (rc != CF_OK) return rc;
    if (!found) return CF_INTERNAL;
    return CF_OK;
}

/* `set_room(c, Scope::WithoutDirects)`: `Room::find_for_user(user,
 * params[:room_id] || params[:id])` filtered to non-direct rooms (opens and
 * closeds share this scope: promoting a direct would republish its history),
 * else redirect to root with the alert.  On success *out holds the room; on
 * the failure arm the response is set and the caller stops with
 * cf_auth_halted. */
static cf_err opens_set_room(cf_ctx *ctx, const cf_user *user, cf_room *out) {
    cf_span param = {NULL, 0};
    bool has = opens_param_str(ctx, "room_id", &param);
    if (!has) has = opens_param_str(ctx, "id", &param);
    int64_t id = 0;
    bool cast = has && cf_views_integer_cast(param, &id);
    bool found = false;
    if (cast) {
        cf_err rc = cf_room_find_for_user(ctx->reader, user->id, id, &found,
                                          out);
        if (rc != CF_OK) return rc;
        if (found && cf_room_direct(out)) {
            cf_room_dispose(out);
            memset(out, 0, sizeof *out);
            found = false;
        }
    }
    if (found) return CF_OK;

    /* `redirect_to_with(root, alert: "Room not found or inaccessible")`. */
    cf_err rc = cf_ctx_flash_set(
        ctx, opens_span("alert"),
        opens_span("Room not found or inaccessible"));
    if (rc == CF_OK) rc = opens_redirect_to_root(ctx);
    return rc;
}

/* `ensure_can_administer`: `head :forbidden unless
 * Current.user.can_administer?(@room)`. */
static bool opens_can_administer(const cf_user *user, const cf_room *room) {
    return cf_user_can_administer(
        user, (cf_optional_i64){.present = true, .value = room->creator_id},
        false);
}

/* `ensure_permission_to_create_rooms`: 403 when the account restricts room
 * creation to administrators and the user is not one.  Reads through the
 * request's reader (the reference's `Account::first` read). */
static cf_err opens_ensure_permission_to_create_rooms(cf_ctx *ctx,
                                                      const cf_user *user) {
    bool administrator = cf_user_is_administrator(user);
    bool found = false;
    cf_account account = {0};
    cf_err rc = cf_account_first(ctx->reader, &found, &account);
    if (rc != CF_OK) {
        cf_account_dispose(&account);
        return rc;
    }
    bool restricted = false;
    if (found) {
        cf_account_settings settings = {0};
        rc = cf_account_settings_of(&account, &settings);
        if (rc == CF_OK) {
            restricted =
                cf_account_settings_restrict_room_creation_to_administrators(
                    &settings);
        }
        cf_account_settings_dispose(&settings);
    }
    cf_account_dispose(&account);
    if (rc != CF_OK) return rc;
    if (restricted && !administrator) return opens_head(ctx, 403);
    return CF_OK;
}

/* `remember_last_room_visited`: `cookies.permanent[:last_room] = @room.id`,
 * only when the cookie's value differs (the rooms.c copy). */
static cf_err opens_remember_last_room(cf_ctx *ctx, int64_t room_id) {
    char text[32];
    int n = snprintf(text, sizeof text, "%" PRId64, room_id);
    if (n < 0 || (size_t)n >= sizeof text) return CF_INTERNAL;

    cf_span current = {NULL, 0};
    cf_err rc = cf_ctx_cookie_get(ctx, opens_span("last_room"), &current);
    if (rc == CF_NOT_FOUND) rc = CF_OK;
    if (rc != CF_OK) return rc;
    if (current.len == (size_t)n &&
        (n == 0 || memcmp(current.ptr, text, (size_t)n) == 0)) {
        return CF_OK;
    }
    cf_cookie_options options = {0};
    options.permanent = true; /* Rails `cookies.permanent` (20 years) */
    return cf_ctx_cookie_set(ctx, opens_span("last_room"),
                             (cf_span){(const unsigned char *)text,
                                       (size_t)n},
                             &options);
}

/* ---- parameters (rooms.rs room_name_param) -------------------------------- */

/* `params.require(:room).permit(:name)`: `given` is the outer Option (the
 * `room` hash was submitted), `present` the inner one (a `name` scalar was
 * permitted).  A missing `room` is ParameterMissing (CF_INVALID -> 400);
 * the `false` literal passes `require` with no permitted keys (the
 * messages.c convention).  Array/object `name` values are unpermitted and
 * read as absent; other scalars that are not strings read as an explicit
 * null (kit `as_str` -> None), exactly like the reference's
 * `permitted.get("name").map(|name| name.as_str().map(...))`. */
typedef struct {
    bool given;
    bool present;
    cf_span value; /* borrowed from the request params while ctx lives */
} opens_room_name;

static cf_err opens_room_name_param(cf_ctx *ctx, opens_room_name *out) {
    memset(out, 0, sizeof *out);
    const cf_param *room = cf_ctx_param(ctx, opens_span("room"));
    if (room == NULL) return CF_INVALID;
    if (cf_param_type(room) == CF_PARAM_BOOL) {
        bool present = false, value = false;
        if (cf_param_bool(room, &present, &value) != CF_OK) return CF_INVALID;
        return CF_OK; /* `false` passes require with nothing permitted */
    }
    if (cf_param_type(room) != CF_PARAM_OBJECT) return CF_INVALID;
    out->given = true;
    const cf_param *name = cf_param_field(room, opens_span("name"));
    if (name == NULL) return CF_OK;
    cf_param_kind kind = cf_param_type(name);
    if (kind == CF_PARAM_ARRAY || kind == CF_PARAM_OBJECT) return CF_OK;
    cf_span text = {NULL, 0};
    if (cf_param_string(name, &text) != CF_OK) {
        /* A permitted non-string scalar: `as_str` is None (explicit null). */
        out->present = false;
        out->given = true;
        return CF_OK;
    }
    out->present = true;
    out->value = text;
    return CF_OK;
}

/* `User.active.ordered` as presenter `UserView` rows (the `new`/`edit` user
 * list).  Reads through the request's reader; no rows are written. */
static cf_err opens_active_users(cf_ctx *ctx, cf_user_vector *users,
                                 cf_view_user * *views_out, size_t *count_out) {
    *views_out = NULL;
    *count_out = 0;
    cf_err rc = cf_user_active_ordered(ctx->reader, users);
    if (rc != CF_OK) return rc;
    if (users->len == 0) return CF_OK;
    cf_view_user *views =
        calloc(users->len == 0 ? 1 : users->len, sizeof *views);
    if (views == NULL) {
        cf_user_vector_dispose(users);
        return CF_NOMEM;
    }
    size_t done = 0;
    for (size_t i = 0; i < users->len; i++) {
        rc = cf_presenter_user_view(ctx, &users->items[i], &views[i]);
        if (rc != CF_OK) break;
        done++;
    }
    if (rc != CF_OK) {
        for (size_t i = 0; i < done; i++) cf_view_user_dispose(&views[i]);
        free(views);
        cf_user_vector_dispose(users);
        return rc;
    }
    *views_out = views;
    *count_out = users->len;
    return CF_OK;
}

static void opens_views_dispose(cf_view_user *views, size_t count) {
    if (views == NULL) return;
    for (size_t i = 0; i < count; i++) cf_view_user_dispose(&views[i]);
    free(views);
}

/* ---- the OpensNew/OpensEdit render (INTEGRATOR SHIM R1; see the header) --- */

typedef struct {
    bool is_new;
    int64_t room_id;
    cf_span name; /* borrowed: the room name, or the DEFAULT for new */
    bool has_name;
    bool can_administer;
    const cf_view_user *users;
    size_t user_count;
} opens_form;

/* The form skeleton the OpensNew/OpensEdit templates share (R1): the form
 * action, the `room[name]` value, one row per active user, and the
 * delete-room section only when can_administer.  Names are escaped; markup
 * is a placeholder until the views packet lands the templates. */
static cf_err opens_form_content(const opens_form *form, cf_builder *out) {
    cf_err rc;
    if (form->is_new) {
        rc = cf_builder_append(out, opens_span("<form action=\"/rooms/opens\" "
                                               "method=\"post\">\n"));
    } else {
        char action[80];
        int n = snprintf(action, sizeof action, "<form action=\"/rooms/opens/"
                                                "%" PRId64 "\" method=\"post"
                                                "\">\n",
                         form->room_id);
        if (n < 0 || (size_t)n >= sizeof action) return CF_INTERNAL;
        rc = cf_builder_append(
            out, (cf_span){(const unsigned char *)action, (size_t)n});
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(out, opens_span("  <!-- INTEGRATOR R1: "
                                               "rooms/opens form "
                                               "placeholder -->\n  <input "
                                               "name=\"room[name]\" "
                                               "value=\""));
    }
    if (rc == CF_OK && form->has_name) rc = cf_html_attr(out, form->name);
    if (rc == CF_OK) rc = cf_builder_append(out, opens_span("\">\n"));
    for (size_t i = 0; rc == CF_OK && i < form->user_count; i++) {
        rc = cf_builder_append(out, opens_span("  <span class=\"user\">"));
        if (rc == CF_OK) {
            rc = cf_html_text(
                out, (cf_span){(const unsigned char *)form->users[i].name.ptr,
                               form->users[i].name.len});
        }
        if (rc == CF_OK) rc = cf_builder_append(out, opens_span("</span>\n"));
    }
    if (rc == CF_OK && !form->is_new && form->can_administer) {
        char action[80];
        int n = snprintf(action, sizeof action,
                         "  <form action=\"/rooms/%" PRId64
                         "\" method=\"post\">\n",
                         form->room_id);
        if (n < 0 || (size_t)n >= sizeof action) return CF_INTERNAL;
        rc = cf_builder_append(
            out, (cf_span){(const unsigned char *)action, (size_t)n});
        if (rc == CF_OK) {
            rc = cf_builder_append(out, opens_span("  <input type=\"hidden\" "
                                                   "name=\"_method\" "
                                                   "value=\"delete\">\n"
                                                   "  </form>\n"));
        }
    }
    if (rc == CF_OK) rc = cf_builder_append(out, opens_span("</form>\n"));
    return rc;
}

static cf_err opens_form_page(const cf_view_ctx *ctx, const opens_form *form,
                              cf_builder *out) {
    cf_builder content = {0};
    cf_err rc = opens_form_content(form, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_page(ctx, (cf_span){NULL, 0}, false,
                                 (cf_span){NULL, 0}, false, (cf_span){NULL, 0},
                                 (cf_span){content.ptr, content.len},
                                 (cf_span){NULL, 0}, (cf_span){NULL, 0},
                                 (cf_span){NULL, 0}, out);
    }
    cf_builder_dispose(&content);
    return rc;
}

static cf_err opens_form_frame(const cf_view_ctx *ctx, const opens_form *form,
                               cf_builder *out) {
    cf_builder content = {0};
    cf_err rc = opens_form_content(form, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_frame(ctx, (cf_span){NULL, 0},
                                  (cf_span){content.ptr, content.len}, out);
    }
    cf_builder_dispose(&content);
    return rc;
}

/* Render the new/edit form: presenter reads (active users) first, then
 * respond_to (HTML only), then Layout::load — the rooms.c translation order.
 * `name`/`has_name` select the input value (DEFAULT_ROOM_NAME for new). */
static cf_err opens_render_form(cf_ctx *ctx, bool is_new, int64_t room_id,
                                cf_span name, bool has_name,
                                bool can_administer) {
    cf_user_vector users = {0};
    cf_view_user *views = NULL;
    size_t view_count = 0;
    cf_err rc = opens_active_users(ctx, &users, &views, &view_count);
    if (rc != CF_OK) {
        opens_views_dispose(views, view_count);
        cf_user_vector_dispose(&users);
        return rc;
    }

    const cf_format *offered[1] = {&cf_format_html};
    const cf_format *chosen = NULL;
    rc = cf_ctx_respond_to(ctx, offered, 1, &chosen);
    if (rc != CF_OK) {
        opens_views_dispose(views, view_count);
        cf_user_vector_dispose(&users);
        return rc;
    }

    cf_view_layout_model layout = {0};
    rc = cf_presenter_layout_load(ctx, cf_ctx_platform(ctx), &layout);
    if (rc != CF_OK) {
        opens_views_dispose(views, view_count);
        cf_user_vector_dispose(&users);
        return rc;
    }
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, &layout);

    opens_form form = {
        .is_new = is_new,
        .room_id = room_id,
        .name = name,
        .has_name = has_name,
        .can_administer = can_administer,
        .users = views,
        .user_count = view_count,
    };
    bool frame = opens_turbo_frame_request(ctx->request);
    cf_builder body = {0};
    rc = frame ? opens_form_frame(&view_ctx, &form, &body)
               : opens_form_page(&view_ctx, &form, &body);
    cf_view_layout_model_dispose(&layout);
    opens_views_dispose(views, view_count);
    cf_user_vector_dispose(&users);
    if (rc != CF_OK) {
        cf_builder_dispose(&body);
        return rc;
    }
    return opens_page_response(ctx, 200, &body, frame);
}

/* ---- writer callbacks ----------------------------------------------------- */

/* `Rooms::Open.create_for(room_params, users: Current.user)`: the actor is
 * revalidated in the write transaction (02-data-auth.md D02 — the rooms.c
 * destroy pattern): a banned/deactivated actor is AUTH-05's denied
 * subsequent access, and the create permission is rechecked over the fresh
 * account row.  The granted list is the single actor id. */
typedef struct {
    int64_t actor_id;
    bool has_name;
    cf_span name; /* borrowed from ctx params; ctx outlives cf_write */
    cf_room *out;
} opens_create_write_arg;

static cf_err opens_revalidate_creator(cf_db *db, int64_t actor_id,
                                       bool *administrator_out) {
    *administrator_out = false;
    bool found = false;
    cf_user actor = {0};
    cf_err rc = cf_user_find_by_id(db, actor_id, &found, &actor);
    if (rc != CF_OK) {
        cf_user_dispose(&actor);
        return rc;
    }
    if (!found || !cf_user_is_active(&actor)) {
        cf_user_dispose(&actor);
        return CF_FORBIDDEN;
    }
    *administrator_out = cf_user_is_administrator(&actor);
    cf_user_dispose(&actor);

    bool account_found = false;
    cf_account account = {0};
    rc = cf_account_first(db, &account_found, &account);
    if (rc != CF_OK) {
        cf_account_dispose(&account);
        return rc;
    }
    bool restricted = false;
    if (account_found) {
        cf_account_settings settings = {0};
        rc = cf_account_settings_of(&account, &settings);
        if (rc == CF_OK) {
            restricted =
                cf_account_settings_restrict_room_creation_to_administrators(
                    &settings);
        }
        cf_account_settings_dispose(&settings);
    }
    cf_account_dispose(&account);
    if (rc != CF_OK) return rc;
    if (restricted && !*administrator_out) return CF_FORBIDDEN;
    return CF_OK;
}

static cf_err opens_create_write(cf_tx *tx, void *arg) {
    const opens_create_write_arg *write = arg;
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;

    bool administrator = false;
    cf_err rc = opens_revalidate_creator(db, write->actor_id, &administrator);
    if (rc != CF_OK) return rc;

    cf_optional_str name = {0};
    if (write->has_name) {
        name.present = true;
        name.value.ptr = (char *)write->name.ptr;
        name.value.len = write->name.len;
    }
    return cf_room_create_for(tx, CF_ROOM_OPEN, name, write->actor_id,
                              &write->actor_id, 1, write->out);
}

/* `@room.update! room_params` with force_room_type: the name (NULL when the
 * key was absent, explicit null when a non-string scalar was submitted) plus
 * the open type.  The actor, the membership scope and the administer grant
 * are revalidated over fresh rows before the first mutation (the rooms.c
 * destroy pattern); an ownership change or a concurrent type change that
 * moved the room out of scope rejects with no row changes. */
typedef struct {
    int64_t actor_id;
    cf_room *room; /* set_room's loaded room; updated in place by the model */
    bool name_given;
    bool name_present;
    cf_span name; /* borrowed from ctx params; ctx outlives cf_write */
} opens_update_write_arg;

static cf_err opens_update_write(cf_tx *tx, void *arg) {
    const opens_update_write_arg *write = arg;
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
    bool allowed =
        rc == CF_OK && member && !cf_room_direct(&fresh) &&
        cf_user_can_administer(
            &actor,
            (cf_optional_i64){.present = true, .value = fresh.creator_id},
            false);
    cf_room_dispose(&fresh);
    cf_user_dispose(&actor);
    if (rc != CF_OK) return rc;
    if (!allowed) return CF_FORBIDDEN;

    const cf_optional_str *name = NULL;
    cf_optional_str name_value = {0};
    if (write->name_given) {
        if (write->name_present) {
            name_value.present = true;
            name_value.value.ptr = (char *)write->name.ptr;
            name_value.value.len = write->name.len;
        }
        name = &name_value;
    }
    cf_room_type open = CF_ROOM_OPEN;
    return cf_room_update(tx, write->room, name, &open);
}

/* ---- broadcasts ----------------------------------------------------------- */

/* INTEGRATOR SHIM R2 (see the header): the shared-room partial
 * (users/sidebars/rooms/_shared) as a deterministic placeholder carrying
 * the room id and escaped name, until the sidebar packet lands.  Only the
 * markup is a shim; the stream, action, target and delivery path are the
 * reference's (`broadcast_prepend_to :rooms, target: :shared_rooms` for
 * create, `broadcast_replace_to :rooms, target: [room, :list]` for update). */
static cf_err opens_shared_room_partial(void *user, const cf_room *room,
                                        cf_builder *out) {
    (void)user;
    cf_err rc = cf_builder_append(out, opens_span("<div class=\"shared-room\" "
                                                 "data-room-id=\""));
    if (rc == CF_OK) {
        char id[32];
        int n = snprintf(id, sizeof id, "%" PRId64, room->id);
        if (n < 0 || (size_t)n >= sizeof id) return CF_INTERNAL;
        rc = cf_builder_append(
            out, (cf_span){(const unsigned char *)id, (size_t)n});
    }
    if (rc == CF_OK) rc = cf_builder_append(out, opens_span("\">"));
    if (rc == CF_OK && room->name.present) {
        rc = cf_html_text(out, (cf_span){
                                   (const unsigned char *)room->name.value.ptr,
                                   room->name.value.len});
    }
    if (rc == CF_OK) rc = cf_builder_append(out, opens_span("</div>"));
    return rc;
}

/* `c.app().broadcasts.open_room_create/update(&room, &partials)` through the
 * app's cable.  Post-commit delivery policy (00-contracts.md; the rooms.c
 * convention): a full loop queue (CF_BUSY, or a stopping cable) drops the
 * broadcast and is counted; it must not fail the committed mutation.  Other
 * failures propagate.  A missing cable is an internal error. */
static cf_err opens_broadcast_create(cf_ctx *ctx, const cf_room *room) {
    cf_cable *cable = cf_app_cable(ctx->app);
    if (cable == NULL) return CF_INTERNAL;
    cf_broadcast_partials partials;
    memset(&partials, 0, sizeof partials);
    partials.shared_room = opens_shared_room_partial;
    cf_err rc = cf_broadcast_open_room_create(cable, room, &partials);
    if (rc == CF_BUSY) {
        cf_cable_log("dropped broadcast", "rooms/opens#create");
        return CF_OK;
    }
    return rc;
}

static cf_err opens_broadcast_update(cf_ctx *ctx, const cf_room *room) {
    cf_cable *cable = cf_app_cable(ctx->app);
    if (cable == NULL) return CF_INTERNAL;
    cf_broadcast_partials partials;
    memset(&partials, 0, sizeof partials);
    partials.shared_room = opens_shared_room_partial;
    cf_err rc = cf_broadcast_open_room_update(cable, room, &partials);
    if (rc == CF_BUSY) {
        cf_cable_log("dropped broadcast", "rooms/opens#update");
        return CF_OK;
    }
    return rc;
}

/* ---- actions -------------------------------------------------------------- */

/* `index` (RoomsController's `rooms::index`, shared by the three room-type
 * controllers): `before_actions`; `redirect_to
 * room_url(Current.user.rooms.last)`; with no rooms `room_url(nil)` raises
 * (Error::internal). */
cf_err cf_action_rooms_opens_index(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = opens_current_user(ctx, &user);
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
    rc = opens_redirect_to_room(ctx, room.id);
    cf_room_dispose(&room);
    return rc;
}

/* `create`: `before_actions`; `ensure_permission_to_create_rooms`;
 * `params.require(:room).permit(:name)`; the
 * `Rooms::Open.create_for` write; `broadcast_create_room`; redirect to the
 * room — the source's order. */
cf_err cf_action_rooms_opens_create(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = opens_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    rc = opens_ensure_permission_to_create_rooms(ctx, &user);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_user_dispose(&user);
        return rc;
    }

    opens_room_name params = {0};
    rc = opens_room_name_param(ctx, &params);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    int64_t actor_id = user.id;
    cf_user_dispose(&user);

    cf_room room = {0};
    /* Only a submitted string names the room: an absent key, an
     * unpermitted array/object, or a non-string scalar (`as_str` -> None)
     * all create with a NULL name (the reference's flatten). */
    opens_create_write_arg write = {
        .actor_id = actor_id,
        .has_name = params.present,
        .name = params.value,
        .out = &room,
    };
    rc = cf_write(ctx->app, opens_create_write, &write);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        return rc;
    }
    /* After the commit, before the redirect (opens.rs create). */
    rc = opens_broadcast_create(ctx, &room);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        return rc;
    }
    int64_t room_id = room.id;
    cf_room_dispose(&room);
    return opens_redirect_to_room(ctx, room_id);
}

/* `new`: `before_actions`; `ensure_permission_to_create_rooms`; the
 * OpenFormView (`DEFAULT_ROOM_NAME`, can_administer true, active users). */
cf_err cf_action_rooms_opens_new(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = opens_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    rc = opens_ensure_permission_to_create_rooms(ctx, &user);
    cf_user_dispose(&user);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    /* `DEFAULT_ROOM_NAME` (opens_controller.rb). */
    return opens_render_form(ctx, true, 0, opens_span("New room"), true, true);
}

/* `edit`: `before_actions`; `set_room(WithoutDirects)`; `force_room_type`
 * (the room edits as open); the OpenFormView with can_administer computed. */
cf_err cf_action_rooms_opens_edit(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = opens_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }

    cf_room room = {0};
    rc = opens_set_room(ctx, &user, &room);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_room_dispose(&room);
        cf_user_dispose(&user);
        return rc;
    }
    bool can_administer = opens_can_administer(&user, &room);
    cf_user_dispose(&user);

    /* `force_room_type`: `@room.becomes!(Rooms::Open)` for the form. */
    room.room_type = CF_ROOM_OPEN;
    cf_span name = {NULL, 0};
    bool has_name = room.name.present;
    if (has_name) {
        name.ptr = (const unsigned char *)room.name.value.ptr;
        name.len = room.name.value.len;
    }
    int64_t room_id = room.id;
    rc = opens_render_form(ctx, false, room_id, name, has_name,
                           can_administer);
    cf_room_dispose(&room);
    return rc;
}

/* `show`: `before_actions`; `set_room(WithoutDirects)`;
 * `remember_last_room_visited`; `redirect_to room_url(@room)`. */
cf_err cf_action_rooms_opens_show(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = opens_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }

    cf_room room = {0};
    rc = opens_set_room(ctx, &user, &room);
    cf_user_dispose(&user);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_room_dispose(&room);
        return rc;
    }

    rc = opens_remember_last_room(ctx, room.id);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        return rc;
    }
    int64_t room_id = room.id;
    cf_room_dispose(&room);
    return opens_redirect_to_room(ctx, room_id);
}

/* `update`: `before_actions`; `set_room(WithoutDirects)`;
 * `ensure_can_administer`; `force_room_type`; `@room.update! room_params`;
 * `broadcast_update_room`; redirect to the room — the source's order. */
cf_err cf_action_rooms_opens_update(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = opens_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }

    cf_room room = {0};
    rc = opens_set_room(ctx, &user, &room);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_room_dispose(&room);
        cf_user_dispose(&user);
        return rc;
    }

    if (!opens_can_administer(&user, &room)) {
        cf_room_dispose(&room);
        cf_user_dispose(&user);
        return opens_head(ctx, 403);
    }

    opens_room_name params = {0};
    rc = opens_room_name_param(ctx, &params);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        cf_user_dispose(&user);
        return rc;
    }
    int64_t actor_id = user.id;
    cf_user_dispose(&user);

    opens_update_write_arg write = {
        .actor_id = actor_id,
        .room = &room,
        .name_given = params.given,
        .name_present = params.present,
        .name = params.value,
    };
    rc = cf_write(ctx->app, opens_update_write, &write);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        return rc;
    }
    /* After the commit, before the redirect (opens.rs update). */
    rc = opens_broadcast_update(ctx, &room);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        return rc;
    }
    int64_t room_id = room.id;
    cf_room_dispose(&room);
    return opens_redirect_to_room(ctx, room_id);
}

/* `destroy` inherited without `set_room` (`rooms::destroy_without_room`):
 * `nil.destroy` raises NoMethodError.  `before_actions` still runs (an
 * unauthenticated request redirects to sign-in); anything authenticated
 * answers the reference's internal error with no row effects. */
cf_err cf_action_rooms_opens_destroy(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    return CF_INTERNAL;
}
