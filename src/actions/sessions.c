/* src/actions/sessions.c — SessionsController (task A-sessions; route IDs 12
 * `new`, 17 `destroy`, 18 `create`; contracts/routes.json).
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/sessions.rs, the
 * pinned port of reference/app/controllers/sessions_controller.rb:
 *
 *   allow_unauthenticated_access only: %i[new create]
 *   before_action :ensure_user_exists, only: :new
 *   rate_limit to: 10, within: 3.minutes, only: :create
 *
 *   new:     redirect_to first_run_url if User.none?; otherwise render
 *            sessions/new in the application layout (turbo-rails' frame
 *            layout for a Turbo-Frame request) with the submitted
 *            email_address and User.administrator.first.
 *   create:  the fixed-window rate limit by remote IP first (attempt 11 ->
 *            429 rejection page); then root-level email_address/password;
 *            success starts a session and redirects to
 *            post_authenticating_url, missing/wrong credentials render the
 *            sign-in page with 401 and flash.now[:alert].
 *   destroy: authenticated; removes the push subscription named by
 *            push_subscription_endpoint, destroys the session row, resets the
 *            session, clears the cookies, disconnects the user's sockets
 *            (reconnect=true), then redirects to root.
 *
 * Callback order, permit-free parameter reads, statuses and the response
 * bodies are the reference's.  The C context conventions follow src/auth.h:
 * an action is `cf_err cf_action_NAME(cf_ctx *)`, a helper that answers the
 * request sets ctx->response and returns CF_OK, and the caller stops with
 * cf_auth_halted.  Absolute URLs use PUBLIC_ORIGIN (03-application.md,
 * "URL helpers ... use PUBLIC_ORIGIN for absolute URLs"; D-C07).
 *
 * The sessions.rs `remove_push_subscription` and
 * `concerns::terminate_current_session` bodies are both owned by A01's single
 * cf_auth_terminate_current_session (src/auth.h), which performs them in the
 * reference order through the writer; destroy calls it once.  The
 * DISCONNECT_USER(reconnect=true) event is mandatory in D02 and delivered by
 * C03's registered control handler (04-cable-jobs.md), so the write can no
 * longer fail for a missing handler.
 *
 * Page responses append the `Link` preload header of `Layout#page`
 * (cf_views_preload_links), like A-first_runs; frame responses carry none.
 *
 * c_symbols for the integrator's route rebind (src/routes.c rows 12/17/18):
 *   cf_action_sessions_new, cf_action_sessions_destroy,
 *   cf_action_sessions_create.
 */
#include "cf.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "views.h"

#include <stdio.h>
#include <string.h>

/* ---- small helpers -------------------------------------------------------- */

static cf_span sessions_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `c.param_str(key)` (kit params.rs `ParamMap::str` -> `Param::as_str`):
 * Some only for a string param, never for null/array/object/number/bool. */
static bool sessions_param_str(cf_ctx *ctx, const char *name, cf_span *out) {
    const cf_param *param = cf_ctx_param(ctx, sessions_span(name));
    if (param == NULL || cf_param_type(param) != CF_PARAM_STRING) return false;
    return cf_param_string(param, out) == CF_OK;
}

/* `c.is_turbo_frame_request()`: a non-blank `Turbo-Frame` header. */
static unsigned char sessions_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c - 'A' + 'a') : c;
}

static bool sessions_turbo_frame_request(const cf_request *request) {
    if (request == NULL) return false;
    cf_span value = {NULL, 0};
    bool found = false;
    for (size_t i = 0; i < request->header_count; i++) {
        const cf_header *header = &request->headers[i];
        if (header->name.len != 11) continue;
        static const char name[] = "turbo-frame";
        bool match = true;
        for (size_t k = 0; k < 11; k++) {
            if (sessions_lower(header->name.ptr[k]) !=
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
    for (size_t i = 0; i < value.len; i++) {
        unsigned char c = value.ptr[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return true;
    }
    return false;
}

/* `redirect_to location` (Rails default 302, the absolute location, the
 * reference redirect's content type). */
static cf_err sessions_redirect_to(cf_ctx *ctx, cf_span location) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    ctx->response->status = 302;
    cf_err rc = cf_response_header(ctx->response, sessions_span("Location"),
                                   location);
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response,
                                sessions_span("Content-Type"),
                                sessions_span("text/html; charset=utf-8"));
    }
    return rc;
}

/* `url_for(<path>)` for the C port: PUBLIC_ORIGIN + the route path. */
static cf_err sessions_redirect_path(cf_ctx *ctx, cf_span path) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    cf_builder location = {0};
    cf_err rc =
        cf_builder_append(&location, sessions_span(config->public_origin));
    if (rc == CF_OK) rc = cf_builder_append(&location, path);
    if (rc == CF_OK) {
        rc = sessions_redirect_to(ctx, (cf_span){location.ptr, location.len});
    }
    cf_builder_dispose(&location);
    return rc;
}

/* `Layout#page` appends the stylesheet preload links (`Link` header); the
 * frame layout carries none.  An unconfigured asset module has no links,
 * which is the same state in which the layout renders no tag blocks. */
static cf_err sessions_link_header(cf_ctx *ctx) {
    cf_builder links = {0};
    cf_err rc = cf_views_preload_links(&links);
    if (rc == CF_INVALID) return CF_OK;
    if (rc == CF_OK && links.len != 0) {
        rc = cf_response_header(ctx->response, sessions_span("Link"),
                                (cf_span){links.ptr, links.len});
    }
    cf_builder_dispose(&links);
    return rc;
}

/* Freeze a rendered page into the response: `status`, HTML content type, and
 * (page layout only) the preload header.  Consumes the builder. */
static cf_err sessions_page_response(cf_ctx *ctx, unsigned status,
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
    rc = cf_response_header(ctx->response, sessions_span("Content-Type"),
                            sessions_span("text/html; charset=utf-8"));
    if (rc != CF_OK) return rc;
    if (!frame) rc = sessions_link_header(ctx);
    return rc;
}

/* `respond_to([HTML])` + `framed_page!(c, status, sessions::New { ... })`
 * (render_new): the parameter email_address, User.administrator.first through
 * A02's presenter, Layout::load, then the page in the application layout or
 * turbo-rails' frame layout for a Turbo-Frame request. */
static cf_err sessions_render_new(cf_ctx *ctx, unsigned status) {
    const cf_format *offered[1] = {&cf_format_html};
    const cf_format *chosen = NULL;
    cf_err rc = cf_ctx_respond_to(ctx, offered, 1, &chosen);
    if (rc != CF_OK) return rc;

    cf_span email = {NULL, 0};
    bool has_email = sessions_param_str(ctx, "email_address", &email);

    cf_view_help_contact contact = {0};
    bool has_contact = false;
    rc = cf_presenter_help_contact(ctx->reader, &has_contact, &contact);
    if (rc != CF_OK) return rc;

    cf_view_layout_model layout = {0};
    rc = cf_presenter_layout_load(ctx, NULL, &layout);
    if (rc != CF_OK) {
        cf_view_help_contact_dispose(&contact);
        return rc;
    }

    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, &layout);

    cf_view_session_new_model model = {0};
    if (has_email) {
        model.has_email_address = true;
        model.email_address =
            (cf_str){(char *)(uintptr_t)email.ptr, email.len};
    }
    if (has_contact) {
        model.has_help_contact = true;
        model.help_contact = &contact;
    }

    bool frame = sessions_turbo_frame_request(ctx->request);
    cf_builder body = {0};
    rc = frame ? cf_view_session_new_frame(&view_ctx, &model, &body)
               : cf_view_session_new(&view_ctx, &model, &body);
    if (rc == CF_OK) {
        rc = sessions_page_response(ctx, status, &body, frame);
    } else {
        cf_builder_dispose(&body);
    }

    cf_view_layout_model_dispose(&layout);
    cf_view_help_contact_dispose(&contact);
    return rc;
}

/* `flash.now[:alert] = REJECTION; render :new, status:`. */
static cf_err sessions_render_rejection(cf_ctx *ctx, unsigned status) {
    cf_err rc = cf_ctx_flash_set(
        ctx, sessions_span("alert"),
        sessions_span(CF_AUTH_SIGN_IN_REJECTION));
    if (rc != CF_OK) return rc;
    return sessions_render_new(ctx, status);
}

/* `redirect_to first_run_url if User.none?` (ensure_user_exists). */
static cf_err sessions_ensure_user_exists(cf_ctx *ctx) {
    bool none = false;
    cf_err rc = cf_presenter_no_users(ctx->reader, &none);
    if (rc != CF_OK) return rc;
    if (none) return sessions_redirect_path(ctx, sessions_span("/first_run"));
    return CF_OK;
}

/* ---- actions -------------------------------------------------------------- */

cf_err cf_action_sessions_new(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    /* `before_actions(allow_unauthenticated_access())`: skip
     * require_authentication, keep deny_bots and forgery protection. */
    cf_before policy = {CF_AUTH_SKIPPED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    rc = sessions_ensure_user_exists(ctx);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    return sessions_render_new(ctx, 200);
}

cf_err cf_action_sessions_create(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_SKIPPED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    /* `rate_limit to: 10, within: 3.minutes, only: :create`: the fixed
     * per-IP window; the helper records 429 for a caller that propagates. */
    bool limited = false;
    rc = cf_auth_sign_in_rate_limit(ctx, &limited);
    if (rc != CF_OK) return rc;
    if (limited) return sessions_render_rejection(ctx, 429);

    /* Root-level params (no require/permit): `c.param_str` yields a value
     * only for a string param; anything else is the failure arm. */
    cf_span email = {NULL, 0};
    cf_span password = {NULL, 0};
    bool has_email = sessions_param_str(ctx, "email_address", &email);
    bool has_password = sessions_param_str(ctx, "password", &password);

    bool authenticated = false;
    cf_user user = {0};
    if (has_email && has_password) {
        rc = cf_auth_authenticate_by(ctx, email, password, &authenticated,
                                     &user);
        if (rc != CF_OK) return rc;
    }

    if (authenticated) {
        cf_session session = {0};
        rc = cf_auth_start_new_session_for(ctx, &user, &session);
        cf_session_dispose(&session);
        cf_user_dispose(&user);
        if (rc != CF_OK) return rc;

        /* `post_authenticating_url`: the stored return-to under
         * PUBLIC_ORIGIN, else `<PUBLIC_ORIGIN>/`. */
        cf_str location = {0};
        rc = cf_auth_post_authenticating_url(ctx, &location);
        if (rc == CF_OK) {
            rc = sessions_redirect_to(
                ctx, (cf_span){(const unsigned char *)location.ptr,
                               location.len});
        }
        cf_str_dispose(&location);
        return rc;
    }

    cf_user_dispose(&user);
    return sessions_render_rejection(ctx, 401);
}

cf_err cf_action_sessions_destroy(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    /* `before_actions(Before::default())`: authentication required. */
    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    /* remove_push_subscription + terminate_current_session (A01's
     * cf_auth_terminate_current_session performs both in the reference
     * order): destroy the push row by endpoint and Current.user, destroy the
     * session row, reset the session and clear the cookies, then
     * DISCONNECT_USER(reconnect=true) through the writer (errors logged, as
     * the reference logs them). */
    rc = cf_auth_terminate_current_session(ctx);
    if (rc != CF_OK) return rc;

    return sessions_redirect_path(ctx, sessions_span("/"));
}
