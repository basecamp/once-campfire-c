/* src/actions/users/bans.c — Users::BansController (task A-users-bans;
 * route ID 55 `destroy`, route ID 56 `create`; contracts/routes.json).
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/users/bans.rs (pinned
 * sha256 1ff81a834ab47b5fc9708c9715820f1700511dba35b57e63da9c1bdea35dc0de),
 * the port of reference/app/controllers/users/bans_controller.rb:
 *
 *   before_action :ensure_can_administer, :set_user
 *
 *   def create
 *     @user.ban
 *     redirect_to @user
 *   end
 *
 *   def destroy
 *     @user.unban
 *     redirect_to @user
 *   end
 *
 * The pinned Rust bodies, translated in order:
 *
 *   create:  before_actions(Before::default()) -> ensure_can_administer ->
 *            find_user(c, "user_id") -> app().write(|tx| user.ban(tx)) ->
 *            redirect_to_user(id)
 *   destroy: the same with user.unban(tx)
 *   find_user: `param_str(key).and_then(integer_cast)` (NotFound otherwise),
 *              then User::find_by_id (NotFound when absent)
 *   redirect_to_user: `url_for(user(id))` = PUBLIC_ORIGIN + "/users/<id>"
 *              through redirect_to (302, text/html; charset=utf-8, no body)
 *
 * The row effects live in D01's cf_user_ban/cf_user_unban, the exact port of
 * models/user.rs ban/unban: ban creates one bans row per distinct non-blank
 * session ip_address in session row order, emits DISCONNECT_USER(reconnect=
 * false), deletes every session of the user, emits REMOVE_BANNED_CONTENT, and
 * only then sets status banned.  unban deletes the user's bans and sets
 * status active; following the source, it is not the inverse of ban: it does
 * not restore sessions and does not disconnect anything.
 *
 * The DISCONNECT_USER event is D02's mandatory control event: cf_write
 * returns CF_OK only after C03's registered control handler applied the
 * revocation, and a missing or failing handler makes the committed write
 * report CF_INTERNAL, which this action propagates (the reference's `?`), so
 * a failed barrier is never a pretend redirect.  REMOVE_BANNED_CONTENT is
 * appended in-transaction but is a best-effort event: J02's consumer lands in
 * Phase 4, so until then the writer drops and counts it (per-kind counter)
 * without changing the committed outcome (src/db/writer.h).
 *
 * c_symbols for the integrator's route rebind (src/routes.c rows 55/56):
 *   cf_action_users_bans_destroy, cf_action_users_bans_create.
 */
#include "cf.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "models/user.h"
#include "views/internal.h" /* cf_views_integer_cast (ruby_compat::integer_cast) */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static cf_span users_bans_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `ensure_can_administer`: 403 unless Current.user.can_administer?(nil,
 * false).  `head :forbidden` (kit Ctx::head) is the status plus the rendered
 * format's content type and an empty body; the A01 halt convention
 * (src/auth.h) answers the request through ctx->response and returns CF_OK.
 * The callback order guarantees this runs before find_user: a non-admin
 * request is 403 even for an unknown user.  On success `actor_id` receives
 * the authenticated user's id for the write callback's in-transaction
 * revalidation (P12-01). */
static cf_err users_bans_ensure_can_administer(cf_ctx *ctx, int64_t *actor_id) {
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
                                  users_bans_span("Content-Type"),
                                  users_bans_span(format->string));
    }
    return CF_OK;
}

/* `c.param_str(key).and_then(integer_cast).ok_or(Error::NotFound)`: only a
 * string parameter casts, and only when ruby_compat::integer_cast accepts it
 * (the shared cf_views_integer_cast). */
static cf_err users_bans_find_user(cf_ctx *ctx, int64_t *out) {
    const cf_param *param = cf_ctx_param(ctx, users_bans_span("user_id"));
    if (param == NULL || cf_param_type(param) != CF_PARAM_STRING) {
        return CF_NOT_FOUND;
    }
    cf_span text = {NULL, 0};
    if (cf_param_string(param, &text) != CF_OK) return CF_NOT_FOUND;
    if (!cf_views_integer_cast(text, out)) return CF_NOT_FOUND;
    return CF_OK;
}

/* P12-01: the create/destroy callbacks carry the actor identity and
 * revalidate it with the target user on `cf_tx_db(tx)` before the first
 * mutation.  The action's pre-write ensure_can_administer/find_user pair is
 * only the fast path; this one is authoritative (02-data-auth.md D02:
 * "Revalidate relevant user status and ownership in the write transaction
 * even if request authentication happened earlier"; writer.h makes it the
 * caller's duty).  A ban, deactivation or role demotion of the actor
 * committed while the callback waited for the writer therefore rejects the
 * mutation with no row changes and no event.  `fresh_target` receives the
 * reloaded target row and is used for the mutation. */
typedef struct {
    int64_t actor_id;
    int64_t target_id;
} users_bans_write_arg;

static cf_err users_bans_revalidate(cf_tx *tx, int64_t actor_id,
                                    int64_t target_id, cf_user *fresh_target) {
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;

    /* The authenticated identity: a banned/deactivated actor is AUTH-05's
     * denied subsequent protected access; a non-administrator is the
     * handler's forbidden. */
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
    if (!allowed) return CF_FORBIDDEN;

    /* The fresh target: find_user's lookup, repeated in the transaction. */
    rc = cf_user_find_by_id(db, target_id, &found, fresh_target);
    if (rc != CF_OK) {
        cf_user_dispose(fresh_target);
        return rc;
    }
    if (!found) {
        cf_user_dispose(fresh_target);
        return CF_NOT_FOUND;
    }
    return CF_OK;
}

static cf_err users_bans_ban_write(cf_tx *tx, void *arg) {
    const users_bans_write_arg *write = arg;
    cf_user target = {0};
    cf_err rc = users_bans_revalidate(tx, write->actor_id, write->target_id,
                                      &target);
    if (rc != CF_OK) return rc;
    rc = cf_user_ban(tx, &target);
    cf_user_dispose(&target);
    return rc;
}

static cf_err users_bans_unban_write(cf_tx *tx, void *arg) {
    const users_bans_write_arg *write = arg;
    cf_user target = {0};
    cf_err rc = users_bans_revalidate(tx, write->actor_id, write->target_id,
                                      &target);
    if (rc != CF_OK) return rc;
    rc = cf_user_unban(tx, &target);
    cf_user_dispose(&target);
    return rc;
}

/* `redirect_to @user`: the absolute url_for(user(id)) location, Rails' 302
 * default, text/html; charset=utf-8 and no body (the same shape as
 * A-welcome's redirect helper). */
static cf_err users_bans_redirect_to_user(cf_ctx *ctx, int64_t user_id) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    char path[48];
    int n = snprintf(path, sizeof path, "/users/%" PRId64, user_id);
    if (n < 0 || (size_t)n >= sizeof path) return CF_INTERNAL;

    cf_builder location = {0};
    cf_err rc =
        cf_builder_append(&location, users_bans_span(config->public_origin));
    if (rc == CF_OK) {
        rc = cf_builder_append(
            &location, (cf_span){(const unsigned char *)path, (size_t)n});
    }
    if (rc == CF_OK) {
        ctx->response->status = 302;
        rc = cf_response_header(ctx->response, users_bans_span("Location"),
                                (cf_span){location.ptr, location.len});
        if (rc == CF_OK) {
            rc = cf_response_header(
                ctx->response, users_bans_span("Content-Type"),
                users_bans_span("text/html; charset=utf-8"));
        }
    }
    cf_builder_dispose(&location);
    return rc;
}

/* The shared create/destroy body: the pinned callbacks in declaration order,
 * then `app().write(|tx| user.<mutation>(tx))`, then the redirect. */
static cf_err users_bans_mutate(cf_ctx *ctx, cf_write_fn write_fn) {
    /* `before_actions(Before::default())`: authentication required, bots
     * denied, forgery protection on (concerns.rs). */
    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    int64_t actor_id = 0;
    rc = users_bans_ensure_can_administer(ctx, &actor_id);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    int64_t user_id = 0;
    rc = users_bans_find_user(ctx, &user_id);
    if (rc != CF_OK) return rc;

    cf_user user = {0};
    bool found = false;
    rc = cf_user_find_by_id(ctx->reader, user_id, &found, &user);
    if (rc == CF_OK && !found) rc = CF_NOT_FOUND;
    if (rc == CF_OK) {
        users_bans_write_arg arg = {.actor_id = actor_id,
                                    .target_id = user_id};
        rc = cf_write(ctx->app, write_fn, &arg);
    }
    cf_user_dispose(&user);
    if (rc != CF_OK) return rc;

    return users_bans_redirect_to_user(ctx, user_id);
}

cf_err cf_action_users_bans_create(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    return users_bans_mutate(ctx, users_bans_ban_write);
}

cf_err cf_action_users_bans_destroy(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    return users_bans_mutate(ctx, users_bans_unban_write);
}
