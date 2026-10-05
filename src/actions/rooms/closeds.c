/* src/actions/rooms/closeds.c — A-rooms-closeds: `rooms/closeds#index`
 * (route ID 113), `rooms/closeds#create` (114), `rooms/closeds#new` (115),
 * `rooms/closeds#edit` (116), `rooms/closeds#show` (117),
 * `rooms/closeds#update` (118, 119) and `rooms/closeds#destroy` (120)
 * (docs/devel/implementation/contracts/controller-packets.md
 * "A-rooms-closeds").
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/rooms.rs plus
 * tmp/rust-ref/crates/campfire/src/controllers/rooms/closeds.rs, the pinned
 * port of reference/app/controllers/rooms/closeds_controller.rb:
 *
 *   before_action :set_room, only: %i[show edit update]   (room_scope:
 *     Current.user.rooms.without_directs — direct rooms never in reach)
 *   before_action :ensure_can_administer, only: %i[update]
 *   before_action :remember_last_room_visited, only: :show
 *   before_action :force_room_type, only: %i[edit update]
 *   before_action :ensure_permission_to_create_rooms, only: %i[new create]
 *
 *   index:   RoomsController's `rooms::index` (redirect to the last room;
 *            `room_url(nil)` raises with no rooms).
 *   show:    set_room(WithoutDirects); remember_last_room_visited;
 *            `redirect_to room_url(@room)`.
 *   new:     ensure_permission_to_create_rooms; the ClosedFormView
 *            (`Rooms::Closed.new(name: DEFAULT_ROOM_NAME)`, can_administer
 *            true, the current user id, no selected users, `User.active.
 *            ordered` unselected).
 *   create:  ensure_permission_to_create_rooms; `params.require(:room).permit(
 *            :name)`; `params.fetch(:user_ids, [])` as existing-user ids;
 *            `Rooms::Closed.create_for(room_params, users: grantees)` — the
 *            creator is NOT granted unless submitted; the shared-room partial
 *            rendered once, prepended to every member's own rooms stream;
 *            redirect to the room.
 *   edit:    set_room(WithoutDirects); force_room_type (`becomes!(
 *            Rooms::Closed)` — an open room edits as closed and converts on
 *            save); the ClosedFormView with `User.active.ordered` partitioned
 *            into selected/unselected by membership.
 *   update:  set_room(WithoutDirects); ensure_can_administer;
 *            force_room_type; `@room.update! room_params` (name plus the
 *            closed type); `@room.memberships.revise(granted: grantees,
 *            revoked: revokees)` — granted are the existing users among the
 *            submitted ids, revoked the current members outside the submitted
 *            (raw) ids; the shared-room partial rendered once, replaced on
 *            every member's own rooms stream; redirect to the room.
 *   destroy: inherited from RoomsController WITHOUT `set_room`
 *            (`super::destroy_without_room`): `nil.destroy` raises
 *            NoMethodError (Error::internal) — never a redirect, never a row
 *            change.  There is deliberately no closeds#destroy success path.
 *
 * The response conventions are the landed actions' (A-rooms/A-messages, and
 * the sibling rooms/involvements.c + rooms/refreshes.c + rooms/opens.c
 * packets): helpers that answer the request set ctx->response and return
 * CF_OK (callers stop with cf_auth_halted, src/auth.h); page renders carry
 * the layout `Link` preload header and frame renders none; redirects are
 * PUBLIC_ORIGIN + the route path.  Small helpers duplicated from
 * src/actions/rooms.c and rooms/opens.c (redirects, the turbo-frame
 * predicate, current user, remember_last_room, the WithoutDirects set_room
 * arm, the create permission, room_name_param) are static copies clearly
 * marked for later dedup; shared files are integrator-owned and this packet
 * must not touch them.
 *
 * Route registration: src/routes.c rows 113-120 bind this file's actions
 * (integrator-owned; rows currently on the development 501 until this packet
 * verifies).
 *
 * Integrator requests (views packet; this file is self-contained meanwhile):
 *  R1. cf_view_rooms_closed_form / cf_view_rooms_closed_form_frame — the
 *      ClosedsNew/ClosedsEdit templates (rooms/closeds/new.html, rooms/
 *      closeds/edit.html: the room form, the selected/unselected user
 *      checkbox lists posting `user_ids[]`, the type-switch link and the
 *      delete-room section shown only when can_administer).  Proposed
 *      declarations (src/views.h):
 *        typedef struct { bool is_new; int64_t room_id; cf_str name;
 *                         bool can_administer; int64_t current_user_id;
 *                         cf_view_user *selected; size_t selected_count;
 *                         cf_view_user *unselected;
 *                         size_t unselected_count; } cf_view_rooms_closed_form;
 *        cf_err cf_view_rooms_closed_form(const cf_view_ctx *ctx,
 *            const cf_view_rooms_closed_form *model, cf_builder *out);
 *        cf_err cf_view_rooms_closed_form_frame(const cf_view_ctx *ctx,
 *            const cf_view_rooms_closed_form *model, cf_builder *out);
 *      Until they land, the static functions below render the same form
 *      skeleton (action, room[name] value, checked/unchecked `user_ids[]`
 *      boxes, conditional delete section) with a clearly marked placeholder
 *      body.
 *  R2. The sidebar shared-room partial behind render_shared_room
 *      (rooms.rs render_shared_room: users/sidebars/rooms/_shared), which
 *      the create/update member broadcasts carry.  The action supplies its
 *      own static shim partial (see closeds_shared_room_partial) so the
 *      broadcast plumbing stays exact; swap in the real renderer when it
 *      lands.
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

/* ---- small helpers (static copies of the rooms.c/opens.c base) ------------ */

static cf_span closeds_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `c.param_str(key)` (kit params.rs `ParamMap::str` -> `Param::as_str`):
 * Some only for a string param, never for null/array/object/number/bool. */
static bool closeds_param_str(cf_ctx *ctx, const char *name, cf_span *out) {
    const cf_param *param = cf_ctx_param(ctx, closeds_span(name));
    if (param == NULL || cf_param_type(param) != CF_PARAM_STRING) return false;
    return cf_param_string(param, out) == CF_OK;
}

static unsigned char closeds_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c - 'A' + 'a') : c;
}

/* `c.is_turbo_frame_request()` (kit ctx.rs; the rooms.c copy). */
static bool closeds_turbo_frame_request(const cf_request *request) {
    if (request == NULL) return false;
    cf_span value = {NULL, 0};
    bool found = false;
    for (size_t i = 0; i < request->header_count; i++) {
        const cf_header *header = &request->headers[i];
        if (header->name.len != 11) continue;
        static const char name[] = "turbo-frame";
        bool match = true;
        for (size_t k = 0; k < 11; k++) {
            if (closeds_lower(header->name.ptr[k]) !=
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
static cf_err closeds_redirect_to(cf_ctx *ctx, cf_span location) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    ctx->response->status = 302;
    cf_err rc = cf_response_header(ctx->response, closeds_span("Location"),
                                   location);
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response, closeds_span("Content-Type"),
                                closeds_span("text/html; charset=utf-8"));
    }
    return rc;
}

/* `url_for(<path>)` for the C port: PUBLIC_ORIGIN + the route path. */
static cf_err closeds_redirect_path(cf_ctx *ctx, cf_span path) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    cf_builder location = {0};
    cf_err rc =
        cf_builder_append(&location, closeds_span(config->public_origin));
    if (rc == CF_OK) rc = cf_builder_append(&location, path);
    if (rc == CF_OK) {
        rc = closeds_redirect_to(ctx, (cf_span){location.ptr, location.len});
    }
    cf_builder_dispose(&location);
    return rc;
}

/* `redirect_to root_path` (campfire_routes::root() is "/"). */
static cf_err closeds_redirect_to_root(cf_ctx *ctx) {
    return closeds_redirect_path(ctx, closeds_span("/"));
}

/* `redirect_to room_url(room_id)`. */
static cf_err closeds_redirect_to_room(cf_ctx *ctx, int64_t room_id) {
    char path[64];
    int n = snprintf(path, sizeof path, "/rooms/%" PRId64, room_id);
    if (n < 0 || (size_t)n >= sizeof path) return CF_INTERNAL;
    return closeds_redirect_path(ctx, (cf_span){(const unsigned char *)path,
                                                (size_t)n});
}

/* `concerns::head(status)`: an empty body with the before-action fallback
 * content type `text/html` (no charset), whatever the request format. */
static cf_err closeds_head(cf_ctx *ctx, unsigned status) {
    ctx->response->status = status;
    return cf_response_header(ctx->response, closeds_span("Content-Type"),
                              closeds_span("text/html"));
}

/* `Layout#page` appends the stylesheet preload links (`Link` header); the
 * frame layout carries none.  An unconfigured asset module has no links. */
static cf_err closeds_link_header(cf_ctx *ctx) {
    cf_builder links = {0};
    cf_err rc = cf_views_preload_links(&links);
    if (rc == CF_INVALID) return CF_OK;
    if (rc == CF_OK && links.len != 0) {
        rc = cf_response_header(ctx->response, closeds_span("Link"),
                                (cf_span){links.ptr, links.len});
    }
    cf_builder_dispose(&links);
    return rc;
}

/* Freeze a rendered form response: `status`, HTML content type, `Vary:
 * Accept` when negotiation ran (the involvements.c convention for framed
 * pages), and (page layout only) the preload header.  Consumes the builder. */
static cf_err closeds_page_response(cf_ctx *ctx, unsigned status,
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
    rc = cf_response_header(ctx->response, closeds_span("Content-Type"),
                            closeds_span("text/html; charset=utf-8"));
    if (rc == CF_OK && cf_ctx_vary_accept(ctx)) {
        rc = cf_response_header(ctx->response, closeds_span("Vary"),
                                closeds_span("Accept"));
    }
    if (rc != CF_OK) return rc;
    if (!frame) rc = closeds_link_header(ctx);
    return rc;
}

/* `require_current_user(c)` (concerns.rs): the authenticated user row.  The
 * chain guarantees a signed-in identity; a missing row is the reference's
 * internal error. */
static cf_err closeds_current_user(cf_ctx *ctx, cf_user *out) {
    bool found = false;
    cf_err rc = cf_auth_current_user(ctx, &found, out);
    if (rc != CF_OK) return rc;
    if (!found) return CF_INTERNAL;
    return CF_OK;
}

/* `set_room(c, Scope::WithoutDirects)` (the opens.c copy): membership-scoped
 * lookup filtered to non-direct rooms, else the root redirect with the
 * alert. */
static cf_err closeds_set_room(cf_ctx *ctx, const cf_user *user, cf_room *out) {
    cf_span param = {NULL, 0};
    bool has = closeds_param_str(ctx, "room_id", &param);
    if (!has) has = closeds_param_str(ctx, "id", &param);
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
        ctx, closeds_span("alert"),
        closeds_span("Room not found or inaccessible"));
    if (rc == CF_OK) rc = closeds_redirect_to_root(ctx);
    return rc;
}

/* `ensure_can_administer`: `head :forbidden unless
 * Current.user.can_administer?(@room)`. */
static bool closeds_can_administer(const cf_user *user, const cf_room *room) {
    return cf_user_can_administer(
        user, (cf_optional_i64){.present = true, .value = room->creator_id},
        false);
}

/* `ensure_permission_to_create_rooms` (the opens.c copy). */
static cf_err closeds_ensure_permission_to_create_rooms(cf_ctx *ctx,
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
    if (restricted && !administrator) return closeds_head(ctx, 403);
    return CF_OK;
}

/* `remember_last_room_visited` (the rooms.c copy). */
static cf_err closeds_remember_last_room(cf_ctx *ctx, int64_t room_id) {
    char text[32];
    int n = snprintf(text, sizeof text, "%" PRId64, room_id);
    if (n < 0 || (size_t)n >= sizeof text) return CF_INTERNAL;

    cf_span current = {NULL, 0};
    cf_err rc = cf_ctx_cookie_get(ctx, closeds_span("last_room"), &current);
    if (rc == CF_NOT_FOUND) rc = CF_OK;
    if (rc != CF_OK) return rc;
    if (current.len == (size_t)n &&
        (n == 0 || memcmp(current.ptr, text, (size_t)n) == 0)) {
        return CF_OK;
    }
    cf_cookie_options options = {0};
    options.permanent = true; /* Rails `cookies.permanent` (20 years) */
    return cf_ctx_cookie_set(ctx, closeds_span("last_room"),
                             (cf_span){(const unsigned char *)text,
                                       (size_t)n},
                             &options);
}

/* ---- parameters (rooms.rs room_name_param + user_ids_param) --------------- */

/* `params.require(:room).permit(:name)` (the opens.c copy). */
typedef struct {
    bool given;
    bool present;
    cf_span value; /* borrowed from the request params while ctx lives */
} closeds_room_name;

static cf_err closeds_room_name_param(cf_ctx *ctx, closeds_room_name *out) {
    memset(out, 0, sizeof *out);
    const cf_param *room = cf_ctx_param(ctx, closeds_span("room"));
    if (room == NULL) return CF_INVALID;
    if (cf_param_type(room) == CF_PARAM_BOOL) {
        bool present = false, value = false;
        if (cf_param_bool(room, &present, &value) != CF_OK) return CF_INVALID;
        return CF_OK; /* `false` passes require with nothing permitted */
    }
    if (cf_param_type(room) != CF_PARAM_OBJECT) return CF_INVALID;
    out->given = true;
    const cf_param *name = cf_param_field(room, closeds_span("name"));
    if (name == NULL) return CF_OK;
    cf_param_kind kind = cf_param_type(name);
    if (kind == CF_PARAM_ARRAY || kind == CF_PARAM_OBJECT) return CF_OK;
    cf_span text = {NULL, 0};
    if (cf_param_string(name, &text) != CF_OK) {
        out->present = false;
        out->given = true;
        return CF_OK;
    }
    out->present = true;
    out->value = text;
    return CF_OK;
}

/* `params.fetch(:user_ids, [])` as ids `User.where(id:)` can match: an array
 * contributes its string elements through integer_cast (anything else is
 * skipped); any other submitted value contributes at most itself through
 * `as_str` + integer_cast; a missing key contributes nothing. */
static cf_err closeds_user_ids_param(cf_ctx *ctx, int64_t **out,
                                     size_t *count_out) {
    *out = NULL;
    *count_out = 0;
    const cf_param *param = cf_ctx_param(ctx, closeds_span("user_ids"));
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
static cf_err closeds_existing_user_ids(cf_db *db, const int64_t *ids,
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

/* `User.active.ordered` as presenter `UserView` rows (the `new`/`edit`
 * lists). */
static cf_err closeds_active_users(cf_ctx *ctx, cf_user_vector *users,
                                   cf_view_user **views_out,
                                   size_t *count_out) {
    *views_out = NULL;
    *count_out = 0;
    cf_err rc = cf_user_active_ordered(ctx->reader, users);
    if (rc != CF_OK) return rc;
    if (users->len == 0) return CF_OK;
    cf_view_user *views = calloc(users->len, sizeof *views);
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

static void closeds_views_dispose(cf_view_user *views, size_t count) {
    if (views == NULL) return;
    for (size_t i = 0; i < count; i++) cf_view_user_dispose(&views[i]);
    free(views);
}

/* ---- the ClosedsNew/ClosedsEdit render (INTEGRATOR SHIM R1) --------------- */

typedef struct {
    bool is_new;
    int64_t room_id;
    cf_span name; /* borrowed: the room name, or the DEFAULT for new */
    bool has_name;
    bool can_administer;
    int64_t current_user_id;
    /* Partition of User.active.ordered by membership (new: all unselected). */
    const cf_view_user *selected;
    size_t selected_count;
    const cf_view_user *unselected;
    size_t unselected_count;
} closeds_form;

static cf_err closeds_user_checkbox(cf_builder *out, const cf_view_user *user,
                                    bool checked) {
    char id[40];
    int n = snprintf(id, sizeof id, "%" PRId64, user->id);
    if (n < 0 || (size_t)n >= sizeof id) return CF_INTERNAL;
    cf_err rc = cf_builder_append(out, closeds_span("  <label><input "
                                                   "type=\"checkbox\" "
                                                   "name=\"user_ids[]\" "
                                                   "value=\""));
    if (rc == CF_OK) {
        rc = cf_builder_append(
            out, (cf_span){(const unsigned char *)id, (size_t)n});
    }
    if (rc == CF_OK && checked) {
        rc = cf_builder_append(out, closeds_span("\" checked=\"checked"));
    }
    if (rc == CF_OK) rc = cf_builder_append(out, closeds_span("\">"));
    if (rc == CF_OK) {
        rc = cf_html_text(out, (cf_span){
                                   (const unsigned char *)user->name.ptr,
                                   user->name.len});
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(out, closeds_span("</label>\n"));
    }
    return rc;
}

/* The form skeleton the ClosedsNew/ClosedsEdit templates share (R1): the
 * form action, the `room[name]` value, the selected/unselected `user_ids[]`
 * boxes, and the delete-room section only when can_administer. */
static cf_err closeds_form_content(const closeds_form *form, cf_builder *out) {
    cf_err rc;
    if (form->is_new) {
        rc = cf_builder_append(out, closeds_span("<form action=\"/rooms/"
                                                "closeds\" method=\"post\">\n"));
    } else {
        char action[80];
        int n = snprintf(action, sizeof action, "<form action=\"/rooms/"
                                                "closeds/%" PRId64
                                                "\" method=\"post\">\n",
                         form->room_id);
        if (n < 0 || (size_t)n >= sizeof action) return CF_INTERNAL;
        rc = cf_builder_append(
            out, (cf_span){(const unsigned char *)action, (size_t)n});
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(out, closeds_span("  <!-- INTEGRATOR R1: "
                                                "rooms/closeds form "
                                                "placeholder -->\n  <input "
                                                "name=\"room[name]\" "
                                                "value=\""));
    }
    if (rc == CF_OK && form->has_name) rc = cf_html_attr(out, form->name);
    if (rc == CF_OK) rc = cf_builder_append(out, closeds_span("\">\n"));
    for (size_t i = 0; rc == CF_OK && i < form->selected_count; i++) {
        rc = closeds_user_checkbox(out, &form->selected[i], true);
    }
    for (size_t i = 0; rc == CF_OK && i < form->unselected_count; i++) {
        rc = closeds_user_checkbox(out, &form->unselected[i], false);
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
            rc = cf_builder_append(out, closeds_span("  <input type=\"hidden\" "
                                                    "name=\"_method\" "
                                                    "value=\"delete\">\n"
                                                    "  </form>\n"));
        }
    }
    if (rc == CF_OK) rc = cf_builder_append(out, closeds_span("</form>\n"));
    return rc;
}

static cf_err closeds_form_page(const cf_view_ctx *ctx,
                                const closeds_form *form, cf_builder *out) {
    cf_builder content = {0};
    cf_err rc = closeds_form_content(form, &content);
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

static cf_err closeds_form_frame(const cf_view_ctx *ctx,
                                 const closeds_form *form, cf_builder *out) {
    cf_builder content = {0};
    cf_err rc = closeds_form_content(form, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_frame(ctx, (cf_span){NULL, 0},
                                  (cf_span){content.ptr, content.len}, out);
    }
    cf_builder_dispose(&content);
    return rc;
}

/* Render the new/edit form: presenter reads (active users, partitioned by
 * membership for edit) first, then respond_to (HTML only), then
 * Layout::load.  `selected_ids` partitions membership for edit; NULL selects
 * nobody (new). */
static cf_err closeds_render_form(cf_ctx *ctx, bool is_new, int64_t room_id,
                                  cf_span name, bool has_name,
                                  bool can_administer,
                                  int64_t current_user_id,
                                  const int64_t *selected_ids,
                                  size_t selected_count) {
    (void)current_user_id;
    cf_user_vector users = {0};
    cf_view_user *views = NULL;
    size_t view_count = 0;
    cf_err rc = closeds_active_users(ctx, &users, &views, &view_count);
    if (rc != CF_OK) {
        closeds_views_dispose(views, view_count);
        cf_user_vector_dispose(&users);
        return rc;
    }

    const cf_format *offered[1] = {&cf_format_html};
    const cf_format *chosen = NULL;
    rc = cf_ctx_respond_to(ctx, offered, 1, &chosen);
    if (rc != CF_OK) {
        closeds_views_dispose(views, view_count);
        cf_user_vector_dispose(&users);
        return rc;
    }

    /* Partition `User.active.ordered` into selected/unselected by
     * membership (closeds.rs edit); the partition preserves query order on
     * both sides. */
    cf_view_user *selected = NULL;
    cf_view_user *unselected = NULL;
    size_t selected_len = 0, unselected_len = 0;
    if (view_count != 0) {
        selected = calloc(view_count == 0 ? 1 : view_count, sizeof *selected);
        unselected =
            calloc(view_count == 0 ? 1 : view_count, sizeof *unselected);
        if (selected == NULL || unselected == NULL) {
            free(selected);
            free(unselected);
            closeds_views_dispose(views, view_count);
            cf_user_vector_dispose(&users);
            return CF_NOMEM;
        }
        for (size_t i = 0; i < view_count; i++) {
            bool is_member = false;
            for (size_t k = 0; k < selected_count; k++) {
                if (users.items[i].id == selected_ids[k]) {
                    is_member = true;
                    break;
                }
            }
            if (is_member) {
                selected[selected_len++] = views[i];
            } else {
                unselected[unselected_len++] = views[i];
            }
        }
        free(views);
        views = NULL;
    }

    cf_view_layout_model layout = {0};
    rc = cf_presenter_layout_load(ctx, cf_ctx_platform(ctx), &layout);
    if (rc != CF_OK) {
        closeds_views_dispose(selected, selected_len);
        closeds_views_dispose(unselected, unselected_len);
        cf_user_vector_dispose(&users);
        return rc;
    }
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, &layout);

    closeds_form form = {
        .is_new = is_new,
        .room_id = room_id,
        .name = name,
        .has_name = has_name,
        .can_administer = can_administer,
        .current_user_id = current_user_id,
        .selected = selected,
        .selected_count = selected_len,
        .unselected = unselected,
        .unselected_count = unselected_len,
    };
    bool frame = closeds_turbo_frame_request(ctx->request);
    cf_builder body = {0};
    rc = frame ? closeds_form_frame(&view_ctx, &form, &body)
               : closeds_form_page(&view_ctx, &form, &body);
    cf_view_layout_model_dispose(&layout);
    closeds_views_dispose(selected, selected_len);
    closeds_views_dispose(unselected, unselected_len);
    cf_user_vector_dispose(&users);
    if (rc != CF_OK) {
        cf_builder_dispose(&body);
        return rc;
    }
    return closeds_page_response(ctx, 200, &body, frame);
}

/* ---- writer callbacks ----------------------------------------------------- */

/* Revalidate the actor and the create permission in the write transaction
 * (02-data-auth.md D02; the opens.c pattern). */
static cf_err closeds_revalidate_creator(cf_db *db, int64_t actor_id,
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

/* `Rooms::Closed.create_for(room_params, users: grantees)`: the grantee ids
 * are submitted ids filtered to existing users inside the transaction
 * (`User.where(id:)`); the creator joins only when submitted. */
typedef struct {
    int64_t actor_id;
    bool has_name;
    cf_span name; /* borrowed from ctx params; ctx outlives cf_write */
    const int64_t *grantee_ids; /* borrowed from the action; alive for cf_write */
    size_t grantee_ids_len;
    cf_room *out;
} closeds_create_write_arg;

static cf_err closeds_create_write(cf_tx *tx, void *arg) {
    const closeds_create_write_arg *write = arg;
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;

    bool administrator = false;
    cf_err rc = closeds_revalidate_creator(db, write->actor_id, &administrator);
    if (rc != CF_OK) return rc;

    int64_t *grantees = NULL;
    size_t grantees_len = 0;
    rc = closeds_existing_user_ids(db, write->grantee_ids,
                                   write->grantee_ids_len, &grantees,
                                   &grantees_len);
    if (rc != CF_OK) {
        free(grantees);
        return rc;
    }

    cf_optional_str name = {0};
    if (write->has_name) {
        name.present = true;
        name.value.ptr = (char *)write->name.ptr;
        name.value.len = write->name.len;
    }
    rc = cf_room_create_for(tx, CF_ROOM_CLOSED, name, write->actor_id,
                            grantees, grantees_len, write->out);
    free(grantees);
    return rc;
}

/* `@room.update! room_params` with force_room_type (the opens.c update
 * pattern, forcing closed instead of open).  The room row is updated in
 * place by the model; the action owns it throughout. */
typedef struct {
    int64_t actor_id;
    cf_room *room; /* set_room's loaded room; updated in place by the model */
    bool name_given;
    bool name_present;
    cf_span name; /* borrowed from ctx params; ctx outlives cf_write */
} closeds_update_write_arg;

static cf_err closeds_revalidate_administer(cf_db *db, int64_t actor_id,
                                            const cf_room *room) {
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

    cf_room fresh = {0};
    rc = cf_room_find_by_id(db, room->id, &found, &fresh);
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
    rc = cf_membership_find_by_room_and_user(db, room->id, actor_id, &member,
                                             &membership);
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
    return CF_OK;
}

static cf_err closeds_update_write(cf_tx *tx, void *arg) {
    const closeds_update_write_arg *write = arg;
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;

    cf_err rc = closeds_revalidate_administer(db, write->actor_id, write->room);
    if (rc != CF_OK) return rc;

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
    cf_room_type closed = CF_ROOM_CLOSED;
    return cf_room_update(tx, write->room, name, &closed);
}

/* `@room.memberships.revise(granted: grantees, revoked: revokees)`:
 * granted are the existing users among the submitted ids, revoked the
 * current members outside the submitted (raw) ids — exactly the reference's
 * two queries, revalidated in this second transaction. */
typedef struct {
    int64_t actor_id;
    const cf_room *room;
    const int64_t *grantee_ids; /* borrowed from the action; alive for cf_write */
    size_t grantee_ids_len;
} closeds_revise_write_arg;

static cf_err closeds_revise_write(cf_tx *tx, void *arg) {
    const closeds_revise_write_arg *write = arg;
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;

    cf_err rc =
        closeds_revalidate_administer(db, write->actor_id, write->room);
    if (rc != CF_OK) return rc;

    int64_t *granted = NULL;
    size_t granted_len = 0;
    rc = closeds_existing_user_ids(db, write->grantee_ids,
                                   write->grantee_ids_len, &granted,
                                   &granted_len);
    if (rc != CF_OK) {
        free(granted);
        return rc;
    }

    cf_int64_vector current = {0};
    rc = cf_room_user_ids(db, write->room, &current);
    int64_t *revoked = NULL;
    size_t revoked_len = 0;
    if (rc == CF_OK) {
        revoked = malloc(current.len == 0 ? 1 : current.len * sizeof *revoked);
        if (revoked == NULL) rc = CF_NOMEM;
    }
    if (rc == CF_OK) {
        for (size_t i = 0; i < current.len; i++) {
            bool submitted = false;
            for (size_t k = 0; k < write->grantee_ids_len; k++) {
                if (current.items[i] == write->grantee_ids[k]) {
                    submitted = true;
                    break;
                }
            }
            if (!submitted) revoked[revoked_len++] = current.items[i];
        }
    }
    if (rc == CF_OK) {
        rc = cf_room_revise(tx, write->room, granted, granted_len, revoked,
                            revoked_len);
    }
    free(revoked);
    cf_int64_vector_dispose(&current);
    free(granted);
    return rc;
}

/* ---- broadcasts ----------------------------------------------------------- */

/* INTEGRATOR SHIM R2 (see the header): the shared-room partial as a
 * deterministic placeholder, until the sidebar packet lands.  Only the markup
 * is a shim; the per-member streams, action, target and delivery path are
 * the reference's (`broadcast_prepend_to user, :rooms, target:
 * :shared_rooms` for create, `broadcast_replace_to` for update). */
static cf_err closeds_shared_room_partial(void *user, const cf_room *room,
                                          cf_builder *out) {
    (void)user;
    cf_err rc = cf_builder_append(out, closeds_span("<div class=\"shared-room"
                                                   "\" data-room-id=\""));
    if (rc == CF_OK) {
        char id[32];
        int n = snprintf(id, sizeof id, "%" PRId64, room->id);
        if (n < 0 || (size_t)n >= sizeof id) return CF_INTERNAL;
        rc = cf_builder_append(
            out, (cf_span){(const unsigned char *)id, (size_t)n});
    }
    if (rc == CF_OK) rc = cf_builder_append(out, closeds_span("\">"));
    if (rc == CF_OK && room->name.present) {
        rc = cf_html_text(out, (cf_span){
                                   (const unsigned char *)room->name.value.ptr,
                                   room->name.value.len});
    }
    if (rc == CF_OK) rc = cf_builder_append(out, closeds_span("</div>"));
    return rc;
}

/* `broadcast_to_members` (closeds.rs): the shared-room partial rendered
 * once, to every member's own rooms stream, through the app's cable.
 * Post-commit delivery policy (00-contracts.md; the rooms.c convention): a
 * full loop queue (CF_BUSY, or a stopping cable) drops the broadcast and is
 * counted; it must not fail the committed mutation.  Other failures
 * propagate.  A missing cable is an internal error. */
static cf_err closeds_broadcast(cf_ctx *ctx, const cf_room *room, bool update) {
    cf_cable *cable = cf_app_cable(ctx->app);
    if (cable == NULL) return CF_INTERNAL;
    cf_broadcast_partials partials;
    memset(&partials, 0, sizeof partials);
    partials.shared_room = closeds_shared_room_partial;
    cf_err rc = update ? cf_broadcast_closed_room_update(ctx->reader, cable,
                                                         room, &partials)
                       : cf_broadcast_closed_room_create(ctx->reader, cable,
                                                         room, &partials);
    if (rc == CF_BUSY) {
        cf_cable_log("dropped broadcast",
                     update ? "rooms/closeds#update" : "rooms/closeds#create");
        return CF_OK;
    }
    return rc;
}

/* ---- actions -------------------------------------------------------------- */

/* `index` (RoomsController's `rooms::index`, shared by the three room-type
 * controllers). */
cf_err cf_action_rooms_closeds_index(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = closeds_current_user(ctx, &user);
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
    rc = closeds_redirect_to_room(ctx, room.id);
    cf_room_dispose(&room);
    return rc;
}

/* `create`: `before_actions`; `ensure_permission_to_create_rooms`;
 * `params.require(:room).permit(:name)`; the submitted `user_ids` filtered
 * to existing users; the `Rooms::Closed.create_for` write;
 * `broadcast_create_room`; redirect to the room — the source's order. */
cf_err cf_action_rooms_closeds_create(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = closeds_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    rc = closeds_ensure_permission_to_create_rooms(ctx, &user);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_user_dispose(&user);
        return rc;
    }

    closeds_room_name params = {0};
    rc = closeds_room_name_param(ctx, &params);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    int64_t *grantee_ids = NULL;
    size_t grantee_ids_len = 0;
    rc = closeds_user_ids_param(ctx, &grantee_ids, &grantee_ids_len);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    int64_t actor_id = user.id;
    cf_user_dispose(&user);

    cf_room room = {0};
    /* Only a submitted string names the room (the opens.c create rule). */
    closeds_create_write_arg write = {
        .actor_id = actor_id,
        .has_name = params.present,
        .name = params.value,
        .grantee_ids = grantee_ids,
        .grantee_ids_len = grantee_ids_len,
        .out = &room,
    };
    rc = cf_write(ctx->app, closeds_create_write, &write);
    free(grantee_ids);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        return rc;
    }
    /* After the commit, before the redirect (closeds.rs create). */
    rc = closeds_broadcast(ctx, &room, false);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        return rc;
    }
    int64_t room_id = room.id;
    cf_room_dispose(&room);
    return closeds_redirect_to_room(ctx, room_id);
}

/* `new`: `before_actions`; `ensure_permission_to_create_rooms`; the
 * ClosedFormView (`DEFAULT_ROOM_NAME`, can_administer true, the current user
 * id, no selected users, `User.active.ordered` unselected). */
cf_err cf_action_rooms_closeds_new(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = closeds_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    rc = closeds_ensure_permission_to_create_rooms(ctx, &user);
    int64_t current_user_id = user.id;
    cf_user_dispose(&user);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    /* `DEFAULT_ROOM_NAME` (closeds_controller.rb). */
    return closeds_render_form(ctx, true, 0, closeds_span("New room"), true,
                               true, current_user_id, NULL, 0);
}

/* `edit`: `before_actions`; `set_room(WithoutDirects)`; `force_room_type`
 * (the room edits as closed); the ClosedFormView with `User.active.ordered`
 * partitioned into selected/unselected by membership. */
cf_err cf_action_rooms_closeds_edit(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = closeds_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }

    cf_room room = {0};
    rc = closeds_set_room(ctx, &user, &room);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_room_dispose(&room);
        cf_user_dispose(&user);
        return rc;
    }
    bool can_administer = closeds_can_administer(&user, &room);
    int64_t current_user_id = user.id;
    cf_user_dispose(&user);

    /* `selected_user_ids = @room.users.pluck(:id)` for the partition. */
    cf_int64_vector selected_ids = {0};
    rc = cf_room_user_ids(ctx->reader, &room, &selected_ids);
    if (rc != CF_OK) {
        cf_int64_vector_dispose(&selected_ids);
        cf_room_dispose(&room);
        return rc;
    }

    /* `force_room_type`: `@room.becomes!(Rooms::Closed)` for the form. */
    room.room_type = CF_ROOM_CLOSED;
    cf_span name = {NULL, 0};
    bool has_name = room.name.present;
    if (has_name) {
        name.ptr = (const unsigned char *)room.name.value.ptr;
        name.len = room.name.value.len;
    }
    int64_t room_id = room.id;
    rc = closeds_render_form(ctx, false, room_id, name, has_name,
                             can_administer, current_user_id,
                             selected_ids.items, selected_ids.len);
    cf_int64_vector_dispose(&selected_ids);
    cf_room_dispose(&room);
    return rc;
}

/* `show`: `before_actions`; `set_room(WithoutDirects)`;
 * `remember_last_room_visited`; `redirect_to room_url(@room)`. */
cf_err cf_action_rooms_closeds_show(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = closeds_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }

    cf_room room = {0};
    rc = closeds_set_room(ctx, &user, &room);
    cf_user_dispose(&user);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_room_dispose(&room);
        return rc;
    }

    rc = closeds_remember_last_room(ctx, room.id);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        return rc;
    }
    int64_t room_id = room.id;
    cf_room_dispose(&room);
    return closeds_redirect_to_room(ctx, room_id);
}

/* `update`: `before_actions`; `set_room(WithoutDirects)`;
 * `ensure_can_administer`; `force_room_type`; `@room.update! room_params`;
 * `@room.memberships.revise(granted:, revoked:)`; `broadcast_update_room`;
 * redirect to the room — the source's order. */
cf_err cf_action_rooms_closeds_update(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = closeds_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }

    cf_room room = {0};
    rc = closeds_set_room(ctx, &user, &room);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_room_dispose(&room);
        cf_user_dispose(&user);
        return rc;
    }

    if (!closeds_can_administer(&user, &room)) {
        cf_room_dispose(&room);
        cf_user_dispose(&user);
        return closeds_head(ctx, 403);
    }

    closeds_room_name params = {0};
    rc = closeds_room_name_param(ctx, &params);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        cf_user_dispose(&user);
        return rc;
    }
    int64_t *grantee_ids = NULL;
    size_t grantee_ids_len = 0;
    rc = closeds_user_ids_param(ctx, &grantee_ids, &grantee_ids_len);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        cf_user_dispose(&user);
        return rc;
    }
    int64_t actor_id = user.id;
    cf_user_dispose(&user);

    closeds_update_write_arg update = {
        .actor_id = actor_id,
        .room = &room,
        .name_given = params.given,
        .name_present = params.present,
        .name = params.value,
    };
    rc = cf_write(ctx->app, closeds_update_write, &update);
    if (rc != CF_OK) {
        free(grantee_ids);
        cf_room_dispose(&room);
        return rc;
    }
    closeds_revise_write_arg revise = {
        .actor_id = actor_id,
        .room = &room,
        .grantee_ids = grantee_ids,
        .grantee_ids_len = grantee_ids_len,
    };
    rc = cf_write(ctx->app, closeds_revise_write, &revise);
    free(grantee_ids);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        return rc;
    }
    /* After the commit, before the redirect (closeds.rs update). */
    rc = closeds_broadcast(ctx, &room, true);
    if (rc != CF_OK) {
        cf_room_dispose(&room);
        return rc;
    }
    int64_t room_id = room.id;
    cf_room_dispose(&room);
    return closeds_redirect_to_room(ctx, room_id);
}

/* `destroy` inherited without `set_room` (`rooms::destroy_without_room`):
 * `nil.destroy` raises NoMethodError.  `before_actions` still runs (an
 * unauthenticated request redirects to sign-in); anything authenticated
 * answers the reference's internal error with no row effects. */
cf_err cf_action_rooms_closeds_destroy(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    return CF_INTERNAL;
}
