/* src/actions/users/sidebars.c — A-users-sidebars: `users/sidebars#show`
 * (route ID 57) per docs/devel/implementation/contracts/controller-packets.md
 * "A-users-sidebars".
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/users/sidebars.rs, the
 * pinned port of reference/app/controllers/users/sidebars_controller.rb:
 *
 *   before_action (Before::default: require_authentication, deny_bots,
 *                  verify_authenticity_token)
 *   show: c.respond_to(&[&format::HTML]);
 *         Current.user;
 *         one read: presenters::accounts::sidebar(conn, secrets, user) and
 *         Account.first.settings.restrict_room_creation_to_administrators;
 *         rooms_stream / user_rooms_stream signed stream names;
 *         can_create_rooms = user.administrator? || !restricted;
 *         view_context::page_or_frame(OK, page, frame)
 *
 * The action ignores `params[:user_id]` exactly as the source does: route 57
 * defaults it to "me" and the sidebar always belongs to the signed-in user.
 *
 * Response conventions are A-rooms': absolute URLs are PUBLIC_ORIGIN + the
 * route path, a page response appends the `Link` preload header and a
 * Turbo-Frame response carries none, and a helper that answers the request
 * sets ctx->response and returns CF_OK (callers stop with cf_auth_halted,
 * src/auth.h).
 *
 * Route registration: the integrator rebinds src/routes.c row 57 to
 * cf_action_users_sidebars_show (declared in src/actions/actions.h); the
 * action test declares the symbol locally until then.
 */
#include "cf.h"

#include "app.h"
#include "auth.h"
#include "context.h"
#include "views.h"

#include <string.h>

/* ---- small helpers -------------------------------------------------------- */

static cf_span sidebar_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `c.is_turbo_frame_request()`: kit/src/ctx.rs:294 is
 * `turbo_frame_request_id().is_some_and(|id| !id.trim().is_empty())` over
 * `request.header("turbo-frame")` (kit/src/request.rs:113:
 * `headers.get(name).and_then(|v| v.to_str().ok())`).  http 1.5.0's
 * `HeaderValue::to_str` accepts a byte only when it is HTAB or visible ASCII,
 * so any control byte, DEL or byte >= 0x80 makes the header read as absent;
 * the first matching header wins (case-insensitive name match). */
static unsigned char sidebar_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c - 'A' + 'a') : c;
}

static bool sidebar_turbo_frame_request(const cf_request *request) {
    if (request == NULL) return false;
    cf_span value = {NULL, 0};
    bool found = false;
    for (size_t i = 0; i < request->header_count; i++) {
        const cf_header *header = &request->headers[i];
        if (header->name.len != 11) continue;
        static const char name[] = "turbo-frame";
        bool match = true;
        for (size_t k = 0; k < 11; k++) {
            if (sidebar_lower(header->name.ptr[k]) != (unsigned char)name[k]) {
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

/* `require_current_user(c)`: the authenticated row; a missing row is the
 * reference's internal error. */
static cf_err sidebar_current_user(cf_ctx *ctx, cf_user *out) {
    bool found = false;
    cf_err rc = cf_auth_current_user(ctx, &found, out);
    if (rc != CF_OK) return rc;
    if (!found) return CF_INTERNAL;
    return CF_OK;
}

/* `Layout#page` appends the stylesheet preload links (`Link` header); the
 * frame layout carries none.  An unconfigured asset module has no links. */
static cf_err sidebar_link_header(cf_ctx *ctx) {
    cf_builder links = {0};
    cf_err rc = cf_views_preload_links(&links);
    if (rc == CF_INVALID) return CF_OK;
    if (rc == CF_OK && links.len != 0) {
        rc = cf_response_header(ctx->response, sidebar_span("Link"),
                                (cf_span){links.ptr, links.len});
    }
    cf_builder_dispose(&links);
    return rc;
}

/* Freeze a rendered page into the response: status, HTML content type, and
 * (page layout only) the preload header.  Consumes the builder. */
static cf_err sidebar_page_response(cf_ctx *ctx, unsigned status,
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
    rc = cf_response_header(ctx->response, sidebar_span("Content-Type"),
                            sidebar_span("text/html; charset=utf-8"));
    if (rc != CF_OK) return rc;
    if (!frame) rc = sidebar_link_header(ctx);
    return rc;
}

/* `users/sidebars#show` (route 57, all formats HTML). */
cf_err cf_action_users_sidebars_show(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    /* K01c: capture the version before authentication (06 step 1); the round
     * serves a hit after authorization, before the one-trip presenter read. */
    cf_cache_round round;
    cf_cache_round_init(ctx, &round);
    /* The Turbo-Frame header selects the frame layout and is a key field. */
    bool frame = sidebar_turbo_frame_request(ctx->request);

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        cf_cache_round_dispose(&round);
        return rc;
    }

    /* `c.respond_to(&[&format::HTML])` before the current-user lookup, as the
     * source orders it: an unacceptable format is the reference's
     * UnknownFormat even for a signed-in user. */
    const cf_format *offered[1] = {&cf_format_html};
    const cf_format *chosen = NULL;
    rc = cf_ctx_respond_to(ctx, offered, 1, &chosen);
    if (rc != CF_OK) {
        cf_cache_round_dispose(&round);
        return rc;
    }

    cf_user user = {0};
    rc = sidebar_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        cf_cache_round_dispose(&round);
        return rc;
    }

    /* Cache lookup after authorization and before the gather. The sidebar
     * ignores params[:user_id] like the source, so a different target is a
     * different (conservative) key for the same body. */
    cf_cached_body cached = {0};
    bool hit = round.cache != NULL &&
               cf_cache_round_lookup(
                   ctx, &round,
                   sidebar_span("text/html; charset=utf-8"), &cached) == CF_OK;
    if (hit) {
        cf_user_dispose(&user);
        rc = cf_cache_serve_hit(ctx, &round, &cached);
        cf_cached_body_dispose(&cached);
        /* The page layout's preload Link header is not part of the cached
         * representation; the same deterministic links are re-emitted. */
        if (rc == CF_OK && !frame) rc = sidebar_link_header(ctx);
        cf_cache_round_dispose(&round);
        return rc;
    }
    cf_cached_body_dispose(&cached);

    /* One read: the sidebar model (memberships, placeholders, the account's
     * room-creation setting and the two signed stream names). */
    cf_view_sidebar_model model = {0};
    rc = cf_presenter_sidebar(ctx, &user, &model);
    cf_user_dispose(&user);
    if (rc != CF_OK) {
        cf_cache_round_dispose(&round);
        return rc;
    }

    /* `page_or_frame`'s find_template(HTML) is the respond_to above (the
     * same negotiation, already satisfied); then Layout::load and the frame
     * decision. */
    cf_view_layout_model layout = {0};
    rc = cf_presenter_layout_load(ctx, cf_ctx_platform(ctx), &layout);
    if (rc != CF_OK) {
        cf_view_sidebar_model_dispose(&model);
        cf_cache_round_dispose(&round);
        return rc;
    }
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, &layout);

    cf_builder body = {0};
    rc = frame ? cf_view_users_sidebar_show_frame(&view_ctx, &model, &body)
               : cf_view_users_sidebar_show(&view_ctx, &model, &body);
    if (rc == CF_OK) {
        rc = sidebar_page_response(ctx, 200, &body, frame);
    } else {
        cf_builder_dispose(&body);
    }

    cf_view_layout_model_dispose(&layout);
    cf_view_sidebar_model_dispose(&model);
    /* K01c: representation + admission after the render; the presenter's read
     * transaction is closed and no cache lock is held here. */
    cf_cache_round_finish(ctx, &round);
    cf_cache_round_dispose(&round);
    return rc;
}
