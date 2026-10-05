/* src/actions/accounts/bots/keys.c — Accounts::Bots::KeysController (task
 * A-accounts-bots-keys; route IDs 27/28 `update`;
 * docs/devel/implementation/contracts/controller-packets.md
 * "A-accounts-bots-keys", contracts/routes.json).
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/accounts/bots/keys.rs
 * (the port of reference/app/controllers/accounts/bots/keys_controller.rb):
 *
 *   before_action :ensure_can_administer
 *
 *   def update
 *     User.active_bots.find(params[:bot_id]).reset_bot_key
 *     redirect_to account_bots_url
 *   end
 *
 * The pinned Rust body, translated in order:
 *
 *   before_actions(Before::default()) -> ensure_can_administer ->
 *   find_active_bot(c, "bot_id") (`param_str.and_then(integer_cast)`,
 *   NotFound otherwise; `User.active_bots.find`, NotFound when absent) ->
 *   app().write(|tx| bot.reset_bot_key(tx)) -> redirect to account_bots
 *
 * `reset_bot_key` writes a fresh `generate_bot_token` (SecureRandom
 * alphanumeric(12)) over bot_token, so the prior "id-token" key no longer
 * authenticates (`User.authenticate_bot` matches id AND token): rotation
 * invalidates prior keys, per the 03-application.md family boundary. The
 * key itself is never logged (00-contracts.md).
 *
 * Like A-users-bans, the write callback revalidates the actor (still an
 * active administrator) and reloads the target as an active bot inside the
 * transaction (P12-01); the pre-write checks are only the fast path.
 * No format negotiation and no parameters beyond the path segment: the
 * reference answers the redirect for any format.
 *
 * c_symbols for the integrator's route rebind (src/routes.c):
 *   cf_action_accounts_bots_keys_update (27, 28).
 */
#include "cf.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "models/user.h"
#include "views/internal.h" /* cf_views_integer_cast (ruby_compat::integer_cast) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cf_span bots_keys_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `ensure_can_administer`: 403 unless the current user can_administer?(nil,
 * false) (concerns.rs: `halt(head(FORBIDDEN))`), before any row lookup, so
 * a non-admin request is 403 even for an unknown bot. On success `actor_id`
 * receives the authenticated user's id for the write callback's
 * in-transaction revalidation. */
static cf_err bots_keys_ensure_can_administer(cf_ctx *ctx,
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
                                  bots_keys_span("Content-Type"),
                                  bots_keys_span(format->string));
    }
    return CF_OK;
}

/* `find_active_bot(c, "bot_id")`: only a string parameter casts
 * (cf_views_integer_cast); the scope is active bots only
 * (`User.active_bots.find`: CF_NOT_FOUND otherwise). */
static cf_err bots_keys_set_bot(cf_ctx *ctx, cf_user *out) {
    memset(out, 0, sizeof *out);
    const cf_param *param = cf_ctx_param(ctx, bots_keys_span("bot_id"));
    int64_t id = 0;
    if (param == NULL || cf_param_type(param) != CF_PARAM_STRING) {
        return CF_NOT_FOUND;
    }
    cf_span text = {NULL, 0};
    if (cf_param_string(param, &text) != CF_OK) return CF_NOT_FOUND;
    if (!cf_views_integer_cast(text, &id)) return CF_NOT_FOUND;
    return cf_user_find_active_bot(ctx->reader, id, out);
}

typedef struct {
    int64_t actor_id;
    int64_t bot_id;
} bots_keys_update_arg;

static cf_err bots_keys_revalidate_actor(cf_tx *tx, int64_t actor_id) {
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
                   cf_user_can_administer(&actor,
                                          (cf_optional_i64){false, 0}, false);
    cf_user_dispose(&actor);
    if (!allowed) return CF_FORBIDDEN;
    return CF_OK;
}

static cf_err bots_keys_update_write(cf_tx *tx, void *arg) {
    const bots_keys_update_arg *update = arg;
    cf_err rc = bots_keys_revalidate_actor(tx, update->actor_id);
    if (rc != CF_OK) return rc;
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;
    cf_user bot = {0};
    rc = cf_user_find_active_bot(db, update->bot_id, &bot);
    if (rc != CF_OK) {
        cf_user_dispose(&bot);
        return rc;
    }
    /* `reset_bot_key`: a fresh token in the same row write; the old
     * "id-token" stops matching `authenticate_bot` on commit. */
    rc = cf_user_reset_bot_key(tx, &bot);
    cf_user_dispose(&bot);
    return rc;
}

/* `redirect_to account_bots_url` (the bots.c redirect shape: absolute
 * location, Rails' 302 default, text/html; charset=utf-8, no body). */
static cf_err bots_keys_redirect_to_bots(cf_ctx *ctx) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    static const char path[] = "/account/bots";
    cf_builder location = {0};
    cf_err rc = cf_builder_append(&location,
                                  bots_keys_span(config->public_origin));
    if (rc == CF_OK) {
        rc = cf_builder_append(
            &location,
            (cf_span){(const unsigned char *)path, sizeof path - 1});
    }
    if (rc == CF_OK) {
        ctx->response->status = 302;
        rc = cf_response_header(ctx->response, bots_keys_span("Location"),
                                (cf_span){location.ptr, location.len});
        if (rc == CF_OK) {
            rc = cf_response_header(
                ctx->response, bots_keys_span("Content-Type"),
                bots_keys_span("text/html; charset=utf-8"));
        }
    }
    cf_builder_dispose(&location);
    return rc;
}

cf_err cf_action_accounts_bots_keys_update(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    /* `before_actions(Before::default())`: authentication required, bots
     * denied, forgery protection on. */
    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    int64_t actor_id = 0;
    rc = bots_keys_ensure_can_administer(ctx, &actor_id);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user bot = {0};
    rc = bots_keys_set_bot(ctx, &bot);
    if (rc != CF_OK) {
        cf_user_dispose(&bot);
        return rc;
    }
    bots_keys_update_arg arg = {.actor_id = actor_id, .bot_id = bot.id};
    cf_user_dispose(&bot);
    rc = cf_write(ctx->app, bots_keys_update_write, &arg);
    if (rc != CF_OK) return rc;

    return bots_keys_redirect_to_bots(ctx);
}
