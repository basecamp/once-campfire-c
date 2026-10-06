/* src/actions/accounts/users.c — Accounts::UsersController (packet
 * A-accounts-users; route IDs 19 `index`, 24/25 `update`, 26 `destroy`;
 * contracts/routes.json).
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/accounts/users.rs,
 * the pinned port of reference/app/controllers/accounts/users_controller.rb:
 *
 *   index:   before_actions(Before::default()); respond_to TURBO_STREAM;
 *            User.active.ordered.without_bots; geared page (per_page 500);
 *            Layout::load; render accounts::UsersIndexTurboStream
 *            { users: page.records, next_page }; apply_headers (JSON only).
 *   update:  before_actions(Before::default()); ensure_can_administer;
 *            set_user (User.active.find(user_id || id)); role =
 *            user[role] == "administrator" ? administrator : member;
 *            write user.update(role); redirect_to edit_account.
 *   destroy: before_actions(Before::default()); ensure_can_administer;
 *            set_user; write user.deactivate; redirect_to edit_account.
 *
 * Callback order, permit behavior (update reads the role with no permit
 * call, so any non-"administrator" value — including absent — is member),
 * status/redirect and row effects are the reference's. destroy's deactivate
 * emits the mandatory DISCONNECT_USER(reconnect=false); cf_write returns
 * CF_OK only after the control barrier applies it.
 *
 * Index rows render the account users view with avatar, role and removal
 * controls from a loaded layout context. Page-number parsing uses the shared
 * integer caster; only turbo-stream is negotiated.
 *
 * c_symbols: cf_action_accounts_users_index,
 * cf_action_accounts_users_update, cf_action_accounts_users_destroy.
 */
#include "cf.h"

#include "http/params.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "models/user.h"
#include "views.h"
#include "presenters/accounts.h"
#include "views/internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cf_span accounts_users_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `c.param_str(key)`: Some only for a string param. */
static bool accounts_users_param_str(cf_ctx *ctx, const char *name,
                                     cf_span *out) {
    const cf_param *param = cf_ctx_param(ctx, accounts_users_span(name));
    if (param == NULL || cf_param_type(param) != CF_PARAM_STRING) return false;
    return cf_param_string(param, out) == CF_OK;
}

/* `ensure_can_administer`: 403 head unless the current user can administer
 * (no record); runs before set_user, so a non-admin is 403 even for an
 * unknown user. */
static cf_err accounts_users_ensure_can_administer(cf_ctx *ctx,
                                                   int64_t *actor_id) {
    bool found = false;
    cf_user current = {0};
    cf_err rc = cf_auth_current_user(ctx, &found, &current);
    bool allowed =
        rc == CF_OK && found &&
        cf_user_can_administer(&current, (cf_optional_i64){false, 0}, false);
    if (allowed && actor_id != NULL) *actor_id = current.id;
    cf_user_dispose(&current);
    if (rc != CF_OK) return rc;
    if (!allowed) {
        ctx->response->status = 403;
        const cf_format *format = cf_ctx_rendered_format(ctx);
        return cf_response_header(ctx->response,
                                  accounts_users_span("Content-Type"),
                                  accounts_users_span(format->string));
    }
    return CF_OK;
}

/* In-transaction administrator revalidation (02-data-auth.md D02). */
static cf_err accounts_users_revalidate_admin(cf_tx *tx, int64_t actor_id) {
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;
    bool found = false;
    cf_user actor = {0};
    cf_err rc = cf_user_find_by_id(db, actor_id, &found, &actor);
    if (rc != CF_OK) {
        cf_user_dispose(&actor);
        return rc;
    }
    bool allowed = found && cf_user_is_active(&actor) &&
                   cf_user_can_administer(&actor, (cf_optional_i64){false, 0},
                                          false);
    cf_user_dispose(&actor);
    return allowed ? CF_OK : CF_FORBIDDEN;
}

/* `User.active.find(params[:user_id] || params[:id])`: string params only,
 * ruby_compat::integer_cast, NotFound otherwise (or when no active user). */
static cf_err accounts_users_set_user(cf_ctx *ctx, cf_user *out) {
    memset(out, 0, sizeof *out);
    cf_span text = {NULL, 0};
    if (!accounts_users_param_str(ctx, "user_id", &text) &&
        !accounts_users_param_str(ctx, "id", &text)) {
        return CF_NOT_FOUND;
    }
    int64_t id = 0;
    if (!cf_views_integer_cast(text, &id)) return CF_NOT_FOUND;
    return cf_user_find_active(ctx->reader, id, out);
}

/* `redirect_to edit_account`: 302, text/html; charset=utf-8, no body. */
static cf_err accounts_users_redirect_to_edit_account(cf_ctx *ctx) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    cf_builder location = {0};
    cf_err rc = cf_builder_append(&location,
                                  accounts_users_span(config->public_origin));
    if (rc == CF_OK) {
        rc = cf_builder_append(&location,
                               accounts_users_span("/account/edit"));
    }
    if (rc == CF_OK) {
        ctx->response->status = 302;
        rc = cf_response_header(ctx->response,
                                accounts_users_span("Location"),
                                (cf_span){location.ptr, location.len});
        if (rc == CF_OK) {
            rc = cf_response_header(
                ctx->response, accounts_users_span("Content-Type"),
                accounts_users_span("text/html; charset=utf-8"));
        }
    }
    cf_builder_dispose(&location);
    return rc;
}

/* ---- index --------------------------------------------------------------- */

#define ACCOUNTS_USERS_PER_PAGE INT64_C(500)
#define ACCOUNTS_USERS_MAX_PAGE INT64_C(1000000000)

static int64_t accounts_users_page_number(cf_ctx *ctx) {
    cf_span text = {NULL, 0};
    int64_t number = 0;
    if (!accounts_users_param_str(ctx, "page", &text)) return 1;
    if (!cf_views_integer_cast(text, &number)) return 1;
    if (number < 1) return 1;
    if (number > ACCOUNTS_USERS_MAX_PAGE) return ACCOUNTS_USERS_MAX_PAGE;
    return number;
}

cf_err cf_action_accounts_users_index(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    /* The only template is the turbo stream, so other formats are 406. */
    const cf_format *offered[1] = {&cf_format_turbo_stream};
    const cf_format *chosen = NULL;
    rc = cf_ctx_respond_to(ctx, offered, 1, &chosen);
    if (rc != CF_OK) return rc;

    cf_user_vector users = {0};
    rc = cf_user_active_ordered_without_bots(ctx->reader, &users);
    if (rc != CF_OK) return rc;

    int64_t total = (int64_t)users.len;
    int64_t number = accounts_users_page_number(ctx);
    int64_t offset = (number - 1) * ACCOUNTS_USERS_PER_PAGE;
    if (offset < 0) offset = 0;
    size_t start = (uint64_t)offset > (uint64_t)total ? (size_t)total
                                                      : (size_t)offset;
    size_t end = start + (size_t)ACCOUNTS_USERS_PER_PAGE < (size_t)total
                     ? start + (size_t)ACCOUNTS_USERS_PER_PAGE
                     : (size_t)total;
    bool last = end == (size_t)total;
    char nextbuf[32];
    const char *next_page = NULL;
    if (!last) {
        int n = snprintf(nextbuf, sizeof nextbuf, "%" PRId64, number + 1);
        if (n < 0 || (size_t)n >= sizeof nextbuf) {
            cf_user_vector_dispose(&users);
            return CF_INTERNAL;
        }
        next_page = nextbuf;
    }

    cf_view_accounts_users_model model = {0};
    for (size_t i = start; i < end && rc == CF_OK; i++)
        rc = cf_presenter_accounts_user(ctx, &users.items[i], &model.users);
    cf_user_vector_dispose(&users);
    if (rc == CF_OK && next_page != NULL) {
        model.has_next_page = true;
        rc = cf_view_str_dup(accounts_users_span(next_page), &model.next_page);
    }
    cf_view_layout_model layout = {0};
    if (rc == CF_OK) rc = cf_presenter_layout_load(ctx, cf_ctx_platform(ctx), &layout);
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, &layout);
    cf_builder body = {0};
    if (rc == CF_OK) rc = cf_view_accounts_users_stream(&view_ctx, &model, &body);
    cf_view_layout_model_dispose(&layout);
    cf_view_accounts_users_model_dispose(&model);
    if (rc != CF_OK) {
        cf_builder_dispose(&body);
        return rc;
    }
    cf_buf *buf = NULL;
    rc = cf_builder_freeze(&body, &buf);
    if (rc != CF_OK) {
        cf_builder_dispose(&body);
        return rc;
    }
    ctx->response->status = 200;
    rc = cf_response_body(ctx->response, buf);
    cf_buf_release(buf);
    if (rc != CF_OK) return rc;
    /* `c.turbo_stream(html)`: text/vnd.turbo-stream.html; charset=utf-8.
     * apply_headers is JSON-only, and this action never serves JSON. */
    return cf_response_header(
        ctx->response, accounts_users_span("Content-Type"),
        accounts_users_span("text/vnd.turbo-stream.html; charset=utf-8"));
}

/* ---- update / destroy ----------------------------------------------------- */

/* `Params::require`: present (not blank), or the false literal. */
static bool accounts_users_param_present(const cf_param *param) {
    if (param == NULL) return false;
    switch (cf_param_type(param)) {
    case CF_PARAM_NULL:
        return false;
    case CF_PARAM_BOOL:
        return true;
    case CF_PARAM_STRING: {
        cf_span text = {NULL, 0};
        if (cf_param_string(param, &text) != CF_OK) return false;
        for (size_t i = 0; i < text.len; i++) {
            unsigned char c = text.ptr[i];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\v' &&
                c != '\f' && c != '\r') {
                return true;
            }
        }
        return false;
    }
    case CF_PARAM_NUMBER:
    case CF_PARAM_UPLOAD:
        return true;
    case CF_PARAM_ARRAY:
    case CF_PARAM_OBJECT:
        return cf_param_count(param) > 0;
    }
    return false;
}

typedef struct {
    int64_t actor_id;
    int64_t target_id;
    bool is_update;
    cf_role role;
} accounts_users_write_arg;

static cf_err accounts_users_write_cb(cf_tx *tx, void *arg) {
    const accounts_users_write_arg *write = arg;
    cf_err rc = accounts_users_revalidate_admin(tx, write->actor_id);
    if (rc != CF_OK) return rc;
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;
    /* The pre-write lookup used find_active; the authoritative reload takes
     * the row by id (the reference holds its loaded user across the write). */
    bool found = false;
    cf_user target = {0};
    rc = cf_user_find_by_id(db, write->target_id, &found, &target);
    if (rc != CF_OK) {
        cf_user_dispose(&target);
        return rc;
    }
    if (!found) {
        cf_user_dispose(&target);
        return CF_NOT_FOUND;
    }
    if (write->is_update) {
        cf_user_changes changes;
        memset(&changes, 0, sizeof changes);
        changes.role = &write->role;
        rc = cf_user_update(tx, &target, &changes);
    } else {
        rc = cf_user_deactivate(tx, &target);
    }
    cf_user_dispose(&target);
    return rc;
}

static cf_err accounts_users_mutate(cf_ctx *ctx, bool is_update) {
    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    int64_t actor_id = 0;
    rc = accounts_users_ensure_can_administer(ctx, &actor_id);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = accounts_users_set_user(ctx, &user);
    if (rc != CF_OK) return rc;
    int64_t target_id = user.id;
    cf_user_dispose(&user);

    cf_role role = CF_ROLE_MEMBER;
    if (is_update) {
        /* `params.require(:user)` with no permit call; 400 when missing. */
        const cf_param *params =
            cf_ctx_param(ctx, accounts_users_span("user"));
        if (!accounts_users_param_present(params)) return CF_INVALID;
        const cf_param *role_param = NULL;
        if (cf_param_type(params) == CF_PARAM_OBJECT) {
            role_param = cf_param_field(params, accounts_users_span("role"));
        }
        if (role_param != NULL &&
            cf_param_type(role_param) == CF_PARAM_STRING) {
            cf_span value = {NULL, 0};
            if (cf_param_string(role_param, &value) == CF_OK &&
                value.len == strlen("administrator") &&
                memcmp(value.ptr, "administrator", value.len) == 0) {
                role = CF_ROLE_ADMINISTRATOR;
            }
        }
    }

    accounts_users_write_arg arg;
    memset(&arg, 0, sizeof arg);
    arg.actor_id = actor_id;
    arg.target_id = target_id;
    arg.is_update = is_update;
    arg.role = role;
    rc = cf_write(ctx->app, accounts_users_write_cb, &arg);
    if (rc != CF_OK) return rc;

    return accounts_users_redirect_to_edit_account(ctx);
}

cf_err cf_action_accounts_users_update(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    return accounts_users_mutate(ctx, true);
}

cf_err cf_action_accounts_users_destroy(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    return accounts_users_mutate(ctx, false);
}
