/* src/actions/accounts/join_codes.c — Accounts::JoinCodesController
 * (packet A-accounts-join_codes; route ID 37 `create`;
 * contracts/routes.json).
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/accounts/
 * join_codes.rs, the pinned port of reference/app/controllers/accounts/
 * join_codes_controller.rb:
 *
 *   create: before_actions(Before::default()); ensure_can_administer;
 *           Current.account; write account.reset_join_code; redirect_to
 *           edit_account.
 *
 * Callback order, status/redirect and the row effect (one fresh
 * SecureRandom.alphanumeric(12) 4-4-4 join code through D01's
 * cf_account_reset_join_code) are the reference's.
 *
 * Integrator request: rebind src/routes.c row 37 ->
 * cf_action_accounts_join_codes_create, e.g.:
 *   {37, CF_POST, "/account/join_code(.:format)", ...,
 *    cf_action_accounts_join_codes_create},
 *
 * c_symbol: cf_action_accounts_join_codes_create.
 */
#include "cf.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "models/account.h"
#include "models/user.h"

#include <string.h>

static cf_span join_codes_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `ensure_can_administer`: 403 head unless the current user can administer
 * (no record); runs before the account lookup. */
static cf_err join_codes_ensure_can_administer(cf_ctx *ctx,
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
                                  join_codes_span("Content-Type"),
                                  join_codes_span(format->string));
    }
    return CF_OK;
}

/* `Current.account` dereference: Account::first, internal error when absent. */
static cf_err join_codes_current_account(cf_ctx *ctx, cf_account *out) {
    bool found = false;
    memset(out, 0, sizeof *out);
    cf_err rc = cf_account_first(ctx->reader, &found, out);
    if (rc != CF_OK) {
        cf_account_dispose(out);
        return rc;
    }
    if (!found) {
        cf_account_dispose(out);
        return CF_INTERNAL;
    }
    return CF_OK;
}

typedef struct {
    int64_t actor_id;
    int64_t account_id;
} join_codes_write_arg;

static cf_err join_codes_write_cb(cf_tx *tx, void *arg) {
    const join_codes_write_arg *write = arg;
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;

    /* In-transaction administrator revalidation (02-data-auth.md D02). */
    bool found = false;
    cf_user actor = {0};
    cf_err rc = cf_user_find_by_id(db, write->actor_id, &found, &actor);
    if (rc != CF_OK) {
        cf_user_dispose(&actor);
        return rc;
    }
    bool allowed = found && cf_user_is_active(&actor) &&
                   cf_user_can_administer(&actor, (cf_optional_i64){false, 0},
                                          false);
    cf_user_dispose(&actor);
    if (!allowed) return CF_FORBIDDEN;

    cf_account account = {0};
    rc = cf_account_find(db, write->account_id, &account);
    if (rc != CF_OK) {
        cf_account_dispose(&account);
        return rc;
    }
    rc = cf_account_reset_join_code(tx, &account);
    cf_account_dispose(&account);
    return rc;
}

cf_err cf_action_accounts_join_codes_create(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    int64_t actor_id = 0;
    rc = join_codes_ensure_can_administer(ctx, &actor_id);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_account account = {0};
    rc = join_codes_current_account(ctx, &account);
    if (rc != CF_OK) return rc;
    join_codes_write_arg arg = {.actor_id = actor_id,
                                .account_id = account.id};
    cf_account_dispose(&account);

    rc = cf_write(ctx->app, join_codes_write_cb, &arg);
    if (rc != CF_OK) return rc;

    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    cf_builder location = {0};
    rc = cf_builder_append(&location, join_codes_span(config->public_origin));
    if (rc == CF_OK) {
        rc = cf_builder_append(&location, join_codes_span("/account/edit"));
    }
    if (rc == CF_OK) {
        ctx->response->status = 302;
        rc = cf_response_header(ctx->response, join_codes_span("Location"),
                                (cf_span){location.ptr, location.len});
        if (rc == CF_OK) {
            rc = cf_response_header(
                ctx->response, join_codes_span("Content-Type"),
                join_codes_span("text/html; charset=utf-8"));
        }
    }
    cf_builder_dispose(&location);
    return rc;
}
