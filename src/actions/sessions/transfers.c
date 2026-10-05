/* src/actions/sessions/transfers.c — Sessions::TransfersController (task
 * A-sessions-transfers; route ID 9 `show`, route IDs 10/11 `update`;
 * contracts/routes.json).
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/sessions/transfers.rs,
 * the pinned port of
 * reference/app/controllers/sessions/transfers_controller.rb:
 *
 *   allow_unauthenticated_access
 *
 *   show:   respond_to HTML; framed_page TransferShow with `action` = the
 *           request's own path (`url_for({})`, i.e. session_transfer_path
 *           of params[:id]).
 *   update: transfer_id = param_str("id") ("" when absent/non-string);
 *           user_id_from_transfer_id (User signed id, purpose "transfer",
 *           4-hour expiry); the active user with that id
 *           (`User.active.find_by_transfer_id`); when found, start a session
 *           and redirect to post_authenticating_url, else head 400.
 *
 * Callback order, statuses and response shapes are the reference's.  The C
 * context conventions follow src/auth.h: an action is
 * `cf_err cf_action_NAME(cf_ctx *)`, a helper that answers the request sets
 * ctx->response and returns CF_OK, and the caller stops with
 * cf_auth_halted.  Absolute URLs use PUBLIC_ORIGIN (the C port's url_for).
 *
 * Transfer tokens are the A01 signed-id layer
 * (cf_auth_signed_id_verify/generate, model "User", purpose "transfer",
 * expiry embedded at generation): the source performs no database
 * one-use/invalidation step — `find_signed` only checks the signature and
 * the expiry — so neither does this port.  (The packet brief mentions
 * one-use behavior; the pinned Rust source has none: flagging, not adding.)
 *
 * c_symbols for the integrator's route rebind (src/routes.c rows 9/10/11):
 *   cf_action_sessions_transfers_show, cf_action_sessions_transfers_update.
 *
 * No integrator requests: every helper this packet needs has landed (A00
 * context/format/flash, A01 signed ids + session lifecycle, A02 transfer
 * template + layout presenter).
 */
#include "cf.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "models/user.h"
#include "views.h"

#include <string.h>

/* ---- small helpers (the landed actions' shared shapes) -------------------- */

static cf_span transfers_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `c.param_str(key)` (kit params.rs `ParamMap::str` -> `Param::as_str`):
 * Some only for a string param, never for null/array/object/number/bool. */
static bool transfers_param_str(cf_ctx *ctx, const char *name, cf_span *out) {
    const cf_param *param = cf_ctx_param(ctx, transfers_span(name));
    if (param == NULL || cf_param_type(param) != CF_PARAM_STRING) return false;
    return cf_param_string(param, out) == CF_OK;
}

static unsigned char transfers_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c - 'A' + 'a') : c;
}

/* `c.is_turbo_frame_request()` (kit ctx.rs + http 1.5.0 `HeaderValue::to_str`
 * readability: HTAB or visible ASCII only, first matching header wins, and
 * the value must be nonempty after trimming spaces/tabs). */
static bool transfers_turbo_frame_request(const cf_request *request) {
    if (request == NULL) return false;
    cf_span value = {NULL, 0};
    bool found = false;
    for (size_t i = 0; i < request->header_count; i++) {
        const cf_header *header = &request->headers[i];
        if (header->name.len != 11) continue;
        static const char name[] = "turbo-frame";
        bool match = true;
        for (size_t k = 0; k < 11; k++) {
            if (transfers_lower(header->name.ptr[k]) !=
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
        unsigned char c = value.ptr[i];
        if (!((c >= 32 && c < 127) || c == '\t')) return false;
        if (c != ' ' && c != '\t') blank = false;
    }
    return !blank;
}

/* `redirect_to location` (302, absolute location, the reference redirect's
 * content type). */
static cf_err transfers_redirect_to(cf_ctx *ctx, cf_span location) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    ctx->response->status = 302;
    cf_err rc = cf_response_header(ctx->response, transfers_span("Location"),
                                   location);
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response,
                                transfers_span("Content-Type"),
                                transfers_span("text/html; charset=utf-8"));
    }
    return rc;
}

/* `concerns::head(status)`: empty body with the rendered format's content
 * type (kit Ctx::head; for the HTML-only actions here that is text/html). */
static cf_err transfers_head(cf_ctx *ctx, unsigned status) {
    ctx->response->status = status;
    const cf_format *format = cf_ctx_rendered_format(ctx);
    return cf_response_header(ctx->response, transfers_span("Content-Type"),
                              transfers_span(format->string));
}

/* `Layout#page` preload links (`Link` header); frame responses carry none. */
static cf_err transfers_link_header(cf_ctx *ctx) {
    cf_builder links = {0};
    cf_err rc = cf_views_preload_links(&links);
    if (rc == CF_INVALID) return CF_OK;
    if (rc == CF_OK && links.len != 0) {
        rc = cf_response_header(ctx->response, transfers_span("Link"),
                                (cf_span){links.ptr, links.len});
    }
    cf_builder_dispose(&links);
    return rc;
}

static cf_err transfers_page_response(cf_ctx *ctx, unsigned status,
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
    rc = cf_response_header(ctx->response, transfers_span("Content-Type"),
                            transfers_span("text/html; charset=utf-8"));
    if (rc != CF_OK) return rc;
    if (!frame) rc = transfers_link_header(ctx);
    return rc;
}

/* `User.active.find_by_transfer_id(params[:id])`: the signed user id, if the
 * signature and the 4-hour expiry check out, then the active user row. */
static cf_err transfers_find_user(cf_ctx *ctx, cf_span transfer_id,
                                  bool *found, cf_user *out) {
    *found = false;
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->secret_key_base == NULL) return CF_INTERNAL;
    cf_span secret = {(const unsigned char *)config->secret_key_base,
                      config->secret_key_base_len};
    cf_optional_i64 id = {false, 0};
    bool verified = false;
    cf_err rc = cf_auth_signed_id_verify(
        secret, transfers_span("User"), transfer_id,
        transfers_span("transfer"), true, cf_now_us(ctx->app), &id, &verified);
    if (rc != CF_OK) return rc;
    if (!verified || !id.present) return CF_OK;
    bool present = false;
    rc = cf_user_find_by_id(ctx->reader, id.value, &present, out);
    if (rc != CF_OK) return rc;
    if (!present || !cf_user_is_active(out)) {
        cf_user_dispose(out);
        memset(out, 0, sizeof *out);
        return CF_OK;
    }
    *found = true;
    return CF_OK;
}

/* ---- actions -------------------------------------------------------------- */

cf_err cf_action_sessions_transfers_show(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    /* `before_actions(allow_unauthenticated_access())`. */
    cf_before policy = {CF_AUTH_SKIPPED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    const cf_format *offered[1] = {&cf_format_html};
    const cf_format *chosen = NULL;
    rc = cf_ctx_respond_to(ctx, offered, 1, &chosen);
    if (rc != CF_OK) return rc;

    /* `url_for({})`: this request's own path. */
    cf_span action = ctx->request != NULL ? ctx->request->path
                                          : (cf_span){NULL, 0};

    cf_view_layout_model layout = {0};
    rc = cf_presenter_layout_load(ctx, cf_ctx_platform(ctx), &layout);
    if (rc != CF_OK) return rc;

    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, &layout);

    cf_view_session_transfer_model model = {0};
    model.action = action;

    bool frame = transfers_turbo_frame_request(ctx->request);
    cf_builder body = {0};
    rc = frame ? cf_view_session_transfer_frame(&view_ctx, &model, &body)
               : cf_view_session_transfer(&view_ctx, &model, &body);
    if (rc == CF_OK) {
        rc = transfers_page_response(ctx, 200, &body, frame);
    } else {
        cf_builder_dispose(&body);
    }
    cf_view_layout_model_dispose(&layout);
    return rc;
}

cf_err cf_action_sessions_transfers_update(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_SKIPPED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    /* `c.param_str("id").unwrap_or_default()`. */
    cf_span transfer_id = {NULL, 0};
    (void)transfers_param_str(ctx, "id", &transfer_id);

    bool found = false;
    cf_user user = {0};
    rc = transfers_find_user(ctx, transfer_id, &found, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    if (!found) {
        cf_user_dispose(&user);
        /* `head :bad_request` (no respond_to ran; the HALT-free head). */
        return transfers_head(ctx, 400);
    }

    cf_session session = {0};
    rc = cf_auth_start_new_session_for(ctx, &user, &session);
    cf_session_dispose(&session);
    cf_user_dispose(&user);
    if (rc != CF_OK) return rc;

    /* `post_authenticating_url` (the stored return-to, else root). */
    cf_str location = {0};
    rc = cf_auth_post_authenticating_url(ctx, &location);
    if (rc == CF_OK) {
        rc = transfers_redirect_to(
            ctx, (cf_span){(const unsigned char *)location.ptr,
                           location.len});
    }
    cf_str_dispose(&location);
    return rc;
}
