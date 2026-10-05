/* src/actions/welcome.c — A-welcome: `welcome#show` (route ID 1), the root
 * URL (docs/devel/implementation/contracts/controller-packets.md "A-welcome";
 * 03-application.md slice row "welcome#show").
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/welcome.rs.
 *
 *     pub async fn show(c: &mut Ctx) -> Result {
 *         concerns::before_actions(c, Before::default()).await?;
 *         let user = concerns::require_current_user(c)?.clone();
 *         let user_id = user.id;
 *         let any_rooms = ... !Room::for_user(conn, user_id)?.is_empty() ...;
 *         if any_rooms {
 *             let room = concerns::last_room_visited(c).await?
 *                 .ok_or_else(|| Error::internal(...))?;
 *             let location = c.url_for(&campfire_routes::room(room.id));
 *             return c.redirect_to(&location);
 *         }
 *         c.respond_to(&[&format::HTML])?;
 *         framed_page!(c, StatusCode::OK, |ctx| welcome::Show { ... }).await
 *     }
 *
 * `Before::default()` is authentication REQUIRED, deny_bots true, forgery
 * protection true (concerns.rs), so the welcome page is never public: an
 * unauthenticated request is redirected to sign-in by A01's chain before this
 * body runs.  A user with rooms is redirected to the last room; a user
 * without rooms gets the rendered page (or its turbo-rails frame for a
 * Turbo-Frame request).
 *
 * Absolute redirect URLs use the configured PUBLIC_ORIGIN, the convention
 * A01 established for the chain's sign-in redirect and A02 for the view
 * context base URL (the C port has no per-request url_for); the path is the
 * source's `room_url(room.id)`.
 *
 * last_room_visited is ported locally (Room::find_for_user, else
 * Room::original_for_user); the cookie's ruby_compat::integer_cast is the
 * shared cf_views_integer_cast (src/views/internal.h, implemented in
 * src/views/render.c) so A02's src/presenters/layout.c selection for
 * Layout::load cannot drift from this action's redirect.
 */
#include "cf.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "models/room.h"
#include "models/user.h"
#include "views.h"
#include "views/internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static cf_span welcome_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* concerns.rs `last_room_visited_in`: the `last_room` cookie's room when the
 * user is a member of it, else the user's original room. */
static cf_err welcome_last_room_visited(cf_ctx *ctx, int64_t user_id,
                                        bool *found, int64_t *room_id) {
    *found = false;
    cf_span cookie = {NULL, 0};
    cf_err rc = cf_ctx_cookie_get(ctx, welcome_span("last_room"), &cookie);
    if (rc == CF_NOT_FOUND) rc = CF_OK;
    if (rc != CF_OK) return rc;
    int64_t requested = 0;
    if (cookie.len != 0 && cf_views_integer_cast(cookie, &requested)) {
        cf_room room = {0};
        bool room_found = false;
        rc = cf_room_find_for_user(ctx->reader, user_id, requested, &room_found,
                                   &room);
        cf_room_dispose(&room);
        if (rc != CF_OK) return rc;
        if (room_found) {
            *found = true;
            *room_id = requested;
            return CF_OK;
        }
    }
    cf_room room = {0};
    bool room_found = false;
    rc = cf_room_original_for_user(ctx->reader, user_id, &room_found, &room);
    if (rc == CF_OK && room_found) {
        *found = true;
        *room_id = room.id;
    }
    cf_room_dispose(&room);
    return rc;
}

/* kit `is_turbo_frame_request`: the first `Turbo-Frame` header
 * (case-insensitive), whose bytes must pass http 1.5.0's
 * `HeaderValue::to_str` (HTAB or visible ASCII only; any other byte makes the
 * header read as absent), and whose value must be nonempty after
 * `str::trim()` (only spaces and tabs here). */
static bool welcome_turbo_frame_request(const cf_request *request) {
    if (request == NULL) return false;
    static const char target[] = "turbo-frame";
    const size_t target_len = sizeof target - 1;
    for (size_t i = 0; i < request->header_count; i++) {
        cf_span name = request->headers[i].name;
        if (name.len != target_len) continue;
        bool match = true;
        for (size_t k = 0; k < target_len; k++) {
            unsigned char c = name.ptr[k];
            if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
            if (c != (unsigned char)target[k]) {
                match = false;
                break;
            }
        }
        if (!match) continue;
        cf_span value = request->headers[i].value;
        bool blank = true;
        for (size_t k = 0; k < value.len; k++) {
            unsigned char b = value.ptr[k];
            /* Not through `HeaderValue::to_str` -> absent. */
            if (!((b >= 32 && b < 127) || b == '\t')) return false;
            if (b != ' ' && b != '\t') blank = false;
        }
        return !blank;
    }
    return false;
}

/* `redirect_to` (Rails default status 302, text/html; charset=utf-8,
 * empty body); the location is PUBLIC_ORIGIN + the route path. */
static cf_err welcome_redirect_to(cf_ctx *ctx, cf_span path) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    cf_builder location = {0};
    cf_err rc = cf_builder_append(
        &location,
        (cf_span){(const unsigned char *)config->public_origin,
                  strlen(config->public_origin)});
    if (rc == CF_OK) rc = cf_builder_append(&location, path);
    if (rc == CF_OK) {
        ctx->response->status = 302;
        rc = cf_response_header(ctx->response, welcome_span("Location"),
                                (cf_span){location.ptr, location.len});
        if (rc == CF_OK) {
            rc = cf_response_header(ctx->response,
                                    welcome_span("Content-Type"),
                                    welcome_span("text/html; charset=utf-8"));
        }
    }
    cf_builder_dispose(&location);
    return rc;
}

/* `respond_to([HTML])` + `framed_page!`: the page in the application layout,
 * or its head/content blocks in turbo-rails' frame layout for a Turbo-Frame
 * request.  A format the action cannot answer is CF_NOT_FOUND with the 406
 * status A00's respond_to records (ActionController::UnknownFormat). */
static cf_err welcome_render(cf_ctx *ctx, const cf_user *user) {
    const cf_format *offered[1] = {&cf_format_html};
    const cf_format *chosen = NULL;
    cf_err rc = cf_ctx_respond_to(ctx, offered, 1, &chosen);
    if (rc != CF_OK) return rc;

    cf_view_layout_model layout = {0};
    rc = cf_presenter_layout_load(ctx, cf_ctx_platform(ctx), &layout);
    if (rc != CF_OK) return rc;

    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, &layout);
    cf_view_welcome_model model = {0};
    model.current_user_name =
        (cf_span){(const unsigned char *)user->name.ptr, user->name.len};

    cf_builder out = {0};
    if (welcome_turbo_frame_request(ctx->request)) {
        rc = cf_view_welcome_frame(&view_ctx, &model, &out);
    } else {
        rc = cf_view_welcome(&view_ctx, &model, &out);
    }
    if (rc == CF_OK) {
        cf_buf *body = NULL;
        rc = cf_builder_freeze(&out, &body);
        if (rc == CF_OK) {
            ctx->response->status = 200;
            rc = cf_response_header(ctx->response,
                                    welcome_span("Content-Type"),
                                    welcome_span("text/html; charset=utf-8"));
            if (rc == CF_OK) rc = cf_response_body(ctx->response, body);
            cf_buf_release(body);
        }
    }
    cf_builder_dispose(&out);
    cf_view_layout_model_dispose(&layout);
    return rc;
}

cf_err cf_action_welcome_show(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    /* `concerns::before_actions(c, Before::default())`: version headers,
     * current request, banned IP, require_authentication, deny_bots, CSRF,
     * browser check (A01's chain). */
    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    /* `concerns::require_current_user`: the chain guarantees one. */
    bool user_found = false;
    cf_user user = {0};
    rc = cf_auth_current_user(ctx, &user_found, &user);
    if (rc != CF_OK) return rc;
    if (!user_found) return CF_INTERNAL;

    /* `Room::for_user(user_id).any?` */
    cf_room_vector rooms = {0};
    rc = cf_room_for_user(ctx->reader, user.id, &rooms);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    bool any_rooms = rooms.len != 0;
    cf_room_vector_dispose(&rooms);

    if (any_rooms) {
        bool found = false;
        int64_t room_id = 0;
        rc = welcome_last_room_visited(ctx, user.id, &found, &room_id);
        if (rc == CF_OK && !found) {
            rc = CF_INTERNAL; /* "no last room" (any_rooms said otherwise) */
        }
        if (rc == CF_OK) {
            char path[64];
            int n = snprintf(path, sizeof path, "/rooms/%" PRId64, room_id);
            if (n < 0 || (size_t)n >= sizeof path) {
                rc = CF_INTERNAL;
            } else {
                rc = welcome_redirect_to(
                    ctx, (cf_span){(const unsigned char *)path, (size_t)n});
            }
        }
        cf_user_dispose(&user);
        return rc;
    }

    rc = welcome_render(ctx, &user);
    cf_user_dispose(&user);
    return rc;
}
