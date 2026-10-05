/* src/actions/accounts/bots.c — Accounts::BotsController (task A-accounts-bots;
 * route IDs 29 `index`, 30 `create`, 31 `new`, 32 `edit`, 34/35 `update`,
 * 36 `destroy`; docs/devel/implementation/contracts/controller-packets.md
 * "A-accounts-bots", contracts/routes.json).
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/accounts/bots.rs
 * (the port of reference/app/controllers/accounts/bots_controller.rb):
 *
 *   before_action :ensure_can_administer
 *   before_action :set_bot, only: %i[ edit update destroy ]
 *
 *   index:   `@bots = User.active_bots.ordered`, respond_to HTML, BotsIndex
 *   new:     respond_to HTML, BotsNew with a default form
 *   create:  `User.create_bot! bot_params` (avatar staged before the write,
 *            assigned inside it), redirect to account_bots
 *   edit:    set_bot, respond_to HTML, BotsEdit with bot_form
 *   update:  set_bot, `@bot.update_bot! bot_params` (webhook first, then the
 *            bot, in one transaction; avatar assigned inside it), redirect
 *   destroy: set_bot, `@bot.deactivate`, redirect
 *
 * The pinned Rust bodies, translated in order:
 *
 *   index:   before -> respond_to([HTML]) -> active_bots_ordered ->
 *            present each bot -> framed BotsIndex (200)
 *   new:     before -> respond_to([HTML]) -> framed BotsNew (200)
 *   create:  before -> bot_params (require user/permit name,avatar,
 *            webhook_url) -> missing name is Error::internal (500) ->
 *            stage avatar -> write(create_bot + avatar assign) -> redirect
 *   edit:    before -> set_bot (active bot or NotFound) -> respond_to([HTML])
 *            -> bot_form -> framed BotsEdit (200)
 *   update:  before -> set_bot -> bot_params -> write(update_bot +
 *            avatar assign) -> redirect
 *   destroy: before -> set_bot -> write(deactivate) -> redirect
 *
 * Permit/param notes (kit params.rs, strong parameters):
 *  - `require("user")` accepts a present value or the false literal
 *    (copied from messages.c msg_param_required/msg_param_present);
 *    otherwise ParameterMissing, which the dispatcher answers with 400.
 *  - `permit` on a non-object is the empty map (kit params.rs `permit`):
 *    a non-object `user` therefore reads as all-absent.
 *  - permit drops array/object values (`is_permitted_scalar`), so a
 *    nonscalar `name`/`webhook_url`/`avatar` reads as absent from the map.
 *  - `Param::to_s` is cf_param_to_s (http/params.h): null is "", bools and
 *    numbers render, strings pass through; arrays/objects/uploads are the
 *    reference None (CF_NOT_FOUND here).
 *  - create's name: `get().and_then(to_s).ok_or(Error::internal)` — an
 *    absent or nonscalar name is CF_INTERNAL (the reference 500), never a
 *    silent blank insert. update's name/webhook_url use
 *    `get().and_then(to_s)` — absent or nonscalar means "leave unchanged".
 *  - `webhook_url` Some("") creates/keeps a webhook row with url ""
 *    (reference `create_webhook!(url:)` on any non-nil value); only an
 *    absent/nonscalar value, or a blank one on update (`filter(!blank)`,
 *    mirrored by cf_user_update_bot), destroys or skips the row. The model
 *    owns that distinction; this file only forwards presence faithfully.
 *
 * Avatar assignments (presenters/attachments.rs Assignment::from_params):
 *  - key absent (or a nonscalar, dropped by permit) -> Unchanged;
 *  - null or "" -> Delete (destroy the User/bot/avatar attachment when one
 *    exists, purge its blob after commit);
 *  - an upload -> Create (stage before the write, attach inside it,
 *    analyze_later after commit);
 *  - any other scalar (e.g. a signed id string) -> Invalid, whose assign is
 *    the reference `Error::internal("Could not find or build blob:
 *    expected attachable")` (500), translated here as CF_INTERNAL with no
 *    row changes.
 *  Multipart bodies never build an upload param node in this C port
 *  (messages.c documents the same restriction), so the Create arm is
 *  unreachable: a CF_PARAM_UPLOAD avatar is answered CF_INTERNAL like the
 *  Invalid arm rather than faked (see integrator request R-BOTS-AVATAR).
 *  The Delete arm deletes the attachment row and emits the reference
 *  after-commit PurgeBlob as a best-effort CF_EVENT_PURGE_BLOB (dropped and
 *  counted without a consumer, per the writer contract); the reference's
 *  `touch` of the user row on avatar destroy has no C model helper yet
 *  (R-BOTS-TOUCH), so updated_at is unchanged by an avatar delete.
 *
 * Rendering (index/new/edit): A02 has not landed the accounts/bots views or
 * the presenters::accounts bot/bot_form presenters, which this packet may
 * not invent (exclusive ownership). Until the integrator lands them, the
 * static bots_render_* shims below answer 200 text/html with the same
 * status/content-type contract and the escaped bot name/key/url facts the
 * acceptance cases assert (integrator request R-BOTS-VIEWS). The edit shim
 * reads the webhook URL through cf_user_webhook_url, mirroring bot_form's
 * `webhook_url` field; bot rooms and the avatar URL need the unlanded
 * presenter/storage helpers and are omitted (R-BOTS-VIEWS).
 *
 * Row effects live in D01 (cf_user_create_bot/update_bot/deactivate,
 * cf_user_active_bots_ordered/find_active_bot). Like A-users-bans, every
 * write callback revalidates the actor (still an active administrator) and
 * reloads the target as an active bot inside the transaction (P12-01); the
 * pre-write checks are only the fast path. deactivate emits the mandatory
 * DISCONNECT_USER(reconnect=false), so the writer's control handler must be
 * registered for destroy to commit.
 *
 * c_symbols for the integrator's route rebind (src/routes.c):
 *   cf_action_accounts_bots_index (29), cf_action_accounts_bots_create (30),
 *   cf_action_accounts_bots_new (31), cf_action_accounts_bots_edit (32),
 *   cf_action_accounts_bots_update (34, 35),
 *   cf_action_accounts_bots_destroy (36).
 *
 * Integrator requests:
 *  - R-BOTS-VIEWS: land cf_view_accounts_bots_index/new/edit (+golden
 *    fixtures) and the presenters::accounts bot/bot_form presenters
 *    (bot_key "id-token", rooms without directs ordered, avatar blob URL);
 *    rebind these actions to render through them.
 *  - R-BOTS-AVATAR: land staged-upload avatar Create support for bots
 *    (S01/S02 Assignment::stage/assign/analyze_later equivalents); until
 *    then an upload avatar answers 500 without writing.
 *  - R-BOTS-TOUCH: land a user-row touch helper for the avatar-delete arm
 *    (presenters::accounts::touch equivalent).
 */
#include "cf.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "http/params.h"
#include "models/active_storage.h"
#include "models/user.h"
#include "views/internal.h" /* cf_views_integer_cast (ruby_compat::integer_cast) */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cf_span bots_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* Before::default(): authentication required, bots denied, forgery
 * protection on (concerns.rs; the same policy bans.c uses). */
static cf_before bots_before(void) {
    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    return policy;
}

/* `ensure_can_administer`: 403 unless the current user can_administer?(nil,
 * false) (concerns.rs: `halt(head(FORBIDDEN))`). `head` is the status plus
 * the rendered format's content type and an empty body; the A01 halt
 * convention answers through ctx->response and returns CF_OK. Runs after
 * before_actions and before any row lookup, so a non-admin request is 403
 * even for an unknown bot. On success `actor_id` receives the authenticated
 * user's id for the write callback's in-transaction revalidation. */
static cf_err bots_ensure_can_administer(cf_ctx *ctx, int64_t *actor_id) {
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
        return cf_response_header(ctx->response, bots_span("Content-Type"),
                                  bots_span(format->string));
    }
    return CF_OK;
}

/* `Param::is_present` (kit params.rs), copied from messages.c
 * msg_param_present: null is absent, false is absent, blank strings are
 * absent, empty collections are absent; numbers and uploads are present. */
static bool bots_param_present(const cf_param *param) {
    if (param == NULL) return false;
    switch (cf_param_type(param)) {
    case CF_PARAM_NULL:
        return false;
    case CF_PARAM_BOOL: {
        bool present = false, value = false;
        if (cf_param_bool(param, &present, &value) != CF_OK) return false;
        return value;
    }
    case CF_PARAM_NUMBER:
    case CF_PARAM_UPLOAD:
        return true;
    case CF_PARAM_STRING: {
        cf_span text = {NULL, 0};
        if (cf_param_string(param, &text) != CF_OK) return false;
        for (size_t i = 0; i < text.len; i++) {
            unsigned char c = text.ptr[i];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r' &&
                c != '\f' && c != '\v') {
                return true;
            }
        }
        return false;
    }
    case CF_PARAM_ARRAY:
    case CF_PARAM_OBJECT:
        return cf_param_count(param) > 0;
    }
    return false;
}

/* `params.require(:user)`: present, or the false literal (kit params.rs
 * `require`; messages.c msg_param_required). */
static bool bots_param_required(const cf_param *param) {
    if (param == NULL) return false;
    if (cf_param_type(param) == CF_PARAM_BOOL) {
        bool present = false, value = false;
        if (cf_param_bool(param, &present, &value) != CF_OK) return false;
        return true; /* false is what require accepts */
    }
    return bots_param_present(param);
}

/* The permitted scalar value for `key` under the packet's permit list
 * (name, avatar, webhook_url): NULL when the key is absent or its value is
 * an array/object (strong parameters `is_permitted_scalar`). */
static const cf_param *bots_permitted(const cf_param *object,
                                      const char *key) {
    if (object == NULL || cf_param_type(object) != CF_PARAM_OBJECT) {
        return NULL;
    }
    const cf_param *value = cf_param_field(object, bots_span(key));
    if (value == NULL) return NULL;
    cf_param_kind kind = cf_param_type(value);
    if (kind == CF_PARAM_ARRAY || kind == CF_PARAM_OBJECT) return NULL;
    return value;
}

/* `params.require(:user).permit(:name, :avatar, :webhook_url)`: the require
 * failure is ParameterMissing (CF_INVALID; dispatch answers 400). A
 * non-object `user` permits to the empty map (kit `permit`), so *out is
 * NULL and every field reads absent. */
static cf_err bots_bot_params(cf_ctx *ctx, const cf_param **out) {
    *out = NULL;
    const cf_param *user = cf_ctx_param(ctx, bots_span("user"));
    if (!bots_param_required(user)) return CF_INVALID;
    if (cf_param_type(user) == CF_PARAM_OBJECT) *out = user;
    return CF_OK;
}

/* `c.param_str(key).and_then(integer_cast).ok_or(Error::NotFound)` over the
 * merged params, then the active-bot scope: only a string parameter casts
 * (cf_views_integer_cast), and only an active bot row is found
 * (`User.active_bots.find`: CF_NOT_FOUND otherwise). */
static cf_err bots_set_bot(cf_ctx *ctx, const char *key, cf_user *out) {
    memset(out, 0, sizeof *out);
    const cf_param *param = cf_ctx_param(ctx, bots_span(key));
    int64_t id = 0;
    if (param == NULL || cf_param_type(param) != CF_PARAM_STRING) {
        return CF_NOT_FOUND;
    }
    cf_span text = {NULL, 0};
    if (cf_param_string(param, &text) != CF_OK) return CF_NOT_FOUND;
    if (!cf_views_integer_cast(text, &id)) return CF_NOT_FOUND;
    return cf_user_find_active_bot(ctx->reader, id, out);
}

/* Owned copy of a borrowed span (messages.c msg_str_dup). */
static cf_err bots_str_dup(cf_span text, cf_str *out) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    if (text.len != 0 && text.ptr == NULL) return CF_INVALID;
    char *copy = malloc(text.len + 1);
    if (copy == NULL) return CF_NOMEM;
    if (text.len != 0) memcpy(copy, text.ptr, text.len);
    copy[text.len] = '\0';
    out->ptr = copy;
    out->len = text.len;
    return CF_OK;
}

static void bots_str_free(cf_str *value) {
    if (value == NULL) return;
    free(value->ptr);
    value->ptr = NULL;
    value->len = 0;
}

/* What `record.avatar = value` does with a permitted param value once the
 * unreachable upload arm is set aside (presenters/attachments.rs
 * Assignment::from_params): absent -> Unchanged; null or "" -> Delete; any
 * other scalar -> Invalid (a CF_PARAM_UPLOAD would be Create, which has no
 * staging support in this port; answered like Invalid, see R-BOTS-AVATAR). */
typedef enum {
    BOTS_AVATAR_UNCHANGED = 0,
    BOTS_AVATAR_DELETE,
    BOTS_AVATAR_INVALID
} bots_avatar;

static bots_avatar bots_avatar_assignment(const cf_param *permitted) {
    const cf_param *value = bots_permitted(permitted, "avatar");
    if (value == NULL) return BOTS_AVATAR_UNCHANGED;
    if (cf_param_type(value) == CF_PARAM_NULL) return BOTS_AVATAR_DELETE;
    if (cf_param_type(value) == CF_PARAM_STRING) {
        cf_span text = {NULL, 0};
        if (cf_param_string(value, &text) == CF_OK && text.len == 0) {
            return BOTS_AVATAR_DELETE;
        }
    }
    return BOTS_AVATAR_INVALID;
}

/* The reference Invalid arm: `Error::internal("Could not find or build
 * blob: expected attachable")` (messages.c msg_invalid_attachment). */
static cf_err bots_invalid_avatar(void) {
    return CF_INTERNAL;
}

/* P12-01: the write callbacks carry the actor identity and revalidate it on
 * `cf_tx_db(tx)` before the first mutation (02-data-auth.md D02). A ban,
 * deactivation or role demotion of the actor committed while the callback
 * waited for the writer rejects the mutation with no row changes. */
static cf_err bots_revalidate_actor(cf_tx *tx, int64_t actor_id) {
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

/* The fresh target: set_bot's lookup, repeated in the transaction. */
static cf_err bots_reload_bot(cf_tx *tx, int64_t bot_id, cf_user *out) {
    memset(out, 0, sizeof *out);
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;
    return cf_user_find_active_bot(db, bot_id, out);
}

/* The Delete arm of `attachments::assign` for Record::user(bot)/"avatar":
 * destroy the attachment row when one exists and purge its blob after
 * commit. A missing attachment is the reference no-op. (The reference also
 * touches the user row; no C helper exists yet, R-BOTS-TOUCH.) */
static cf_err bots_avatar_delete_write(cf_tx *tx, int64_t bot_id) {
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;
    bool found = false;
    cf_attachment attachment = {0};
    cf_err rc = cf_attachment_find_for(db, CF_STR_LIT("User"), bot_id,
                                       CF_STR_LIT("avatar"), &found,
                                       &attachment);
    if (rc != CF_OK) return rc;
    if (!found) return CF_OK;
    int64_t blob_id = attachment.blob_id;
    rc = cf_attachment_delete(tx, &attachment);
    cf_attachment_dispose(&attachment);
    if (rc != CF_OK) return rc;
    return cf_tx_event(tx, (cf_event){.kind = CF_EVENT_PURGE_BLOB,
                                      .blob_id = blob_id});
}

/* `redirect_to account_bots_url`: the absolute account_bots location, Rails'
 * 302 default, text/html; charset=utf-8 and no body (the A-welcome/bans
 * redirect shape; kit ctx.rs redirect_to). */
static cf_err bots_redirect_to_bots(cf_ctx *ctx) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    static const char path[] = "/account/bots";
    cf_builder location = {0};
    cf_err rc = cf_builder_append(&location, bots_span(config->public_origin));
    if (rc == CF_OK) {
        rc = cf_builder_append(
            &location,
            (cf_span){(const unsigned char *)path, sizeof path - 1});
    }
    if (rc == CF_OK) {
        ctx->response->status = 302;
        rc = cf_response_header(ctx->response, bots_span("Location"),
                                (cf_span){location.ptr, location.len});
        if (rc == CF_OK) {
            rc = cf_response_header(
                ctx->response, bots_span("Content-Type"),
                bots_span("text/html; charset=utf-8"));
        }
    }
    cf_builder_dispose(&location);
    return rc;
}

/* Freeze a rendered shim page: 200, text/html; charset=utf-8, no preload
 * Link header (the shims own no digested assets; first_runs.c
 * fr_page_response without the asset leg). Consumes the builder. */
static cf_err bots_page_response(cf_ctx *ctx, cf_builder *body) {
    cf_buf *buf = NULL;
    cf_err rc = cf_builder_freeze(body, &buf);
    if (rc != CF_OK) {
        cf_builder_dispose(body);
        return rc;
    }
    ctx->response->status = 200;
    rc = cf_response_body(ctx->response, buf);
    cf_buf_release(buf);
    if (rc != CF_OK) return rc;
    return cf_response_header(ctx->response, bots_span("Content-Type"),
                              bots_span("text/html; charset=utf-8"));
}

/* SHIM (R-BOTS-VIEWS): BotsIndex facts until A02 lands the template — the
 * page title plus one escaped row per bot (name and "id-token" key, the
 * fields the _bot partial renders for copying). */
static cf_err bots_render_index(cf_ctx *ctx, const cf_user *bots,
                                size_t count) {
    cf_builder body = {0};
    cf_err rc = cf_builder_append(
        &body, bots_span("<!DOCTYPE html><html><head><title>Chat bots</title>"
                         "</head><body><h1>Chat bots</h1><ul>"));
    for (size_t i = 0; i < count && rc == CF_OK; i++) {
        char id_text[32];
        int n = snprintf(id_text, sizeof id_text, "%" PRId64, bots[i].id);
        if (n < 0 || (size_t)n >= sizeof id_text) {
            rc = CF_INTERNAL;
            break;
        }
        rc = cf_builder_append(&body, bots_span("<li data-bot-id=\""));
        if (rc == CF_OK) {
            rc = cf_builder_append(
                &body, (cf_span){(const unsigned char *)id_text,
                                 (size_t)n});
        }
        if (rc == CF_OK) rc = cf_builder_append(&body, bots_span("\">"));
        if (rc == CF_OK) {
            rc = cf_html_text(
                &body,
                (cf_span){(const unsigned char *)bots[i].name.ptr,
                          bots[i].name.len});
        }
        if (rc == CF_OK) {
            cf_str key = {NULL, 0};
            rc = cf_user_bot_key(&bots[i], &key);
            if (rc == CF_OK) {
                rc = cf_builder_append(&body, bots_span(" <code>"));
                if (rc == CF_OK) {
                    rc = cf_html_text(
                        &body,
                        (cf_span){(const unsigned char *)key.ptr, key.len});
                }
                if (rc == CF_OK) {
                    rc = cf_builder_append(&body, bots_span("</code>"));
                }
                cf_str_dispose(&key);
            }
        }
        if (rc == CF_OK) rc = cf_builder_append(&body, bots_span("</li>"));
    }
    if (rc == CF_OK) rc = cf_builder_append(&body, bots_span("</ul></body></html>"));
    if (rc != CF_OK) {
        cf_builder_dispose(&body);
        return rc;
    }
    return bots_page_response(ctx, &body);
}

/* SHIM (R-BOTS-VIEWS): BotsNew/BotsEdit form facts until A02 lands the
 * template — the nested `user` fields bot_params permits. `bot` is NULL for
 * new; `webhook` is the bot_form webhook_url (absent for new). */
static cf_err bots_render_form(cf_ctx *ctx, const cf_user *bot,
                               const cf_optional_str *webhook, bool is_new) {
    cf_builder body = {0};
    cf_err rc = cf_builder_append(
        &body,
        bots_span("<!DOCTYPE html><html><head><title>"));
    if (rc == CF_OK) {
        rc = cf_builder_append(
            &body, is_new ? bots_span("New chat bot") : bots_span("Edit bot"));
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(&body, bots_span("</title></head><body><h1>"));
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(
            &body, is_new ? bots_span("New chat bot") : bots_span("Edit bot"));
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(
            &body, bots_span("</h1><form method=\"post\"><label>Name"
                             "<input type=\"text\" name=\"user[name]\" value=\""));
    }
    if (rc == CF_OK && bot != NULL) {
        rc = cf_html_attr(
            &body,
            (cf_span){(const unsigned char *)bot->name.ptr, bot->name.len});
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(
            &body, bots_span("\" /></label><label>Webhook URL"
                             "<input type=\"url\" name=\"user[webhook_url]\" value=\""));
    }
    if (rc == CF_OK && webhook != NULL && webhook->present) {
        rc = cf_html_attr(
            &body, (cf_span){(const unsigned char *)webhook->value.ptr,
                             webhook->value.len});
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(
            &body, bots_span("\" /></label><button type=\"submit\">Save"
                             "</button></form></body></html>"));
    }
    if (rc != CF_OK) {
        cf_builder_dispose(&body);
        return rc;
    }
    return bots_page_response(ctx, &body);
}

/* `c.respond_to(&[format::HTML])`. */
static cf_err bots_respond_html(cf_ctx *ctx) {
    const cf_format *offered[1] = {&cf_format_html};
    const cf_format *chosen = NULL;
    return cf_ctx_respond_to(ctx, offered, 1, &chosen);
}

/* ---- index -------------------------------------------------------------- */

cf_err cf_action_accounts_bots_index(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_err rc = cf_before_actions(ctx, bots_before());
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    int64_t actor_id = 0;
    rc = bots_ensure_can_administer(ctx, &actor_id);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    rc = bots_respond_html(ctx);
    if (rc != CF_OK) return rc;

    cf_user_vector bots = {0, 0, 0};
    rc = cf_user_active_bots_ordered(ctx->reader, &bots);
    if (rc != CF_OK) return rc;
    rc = bots_render_index(ctx, bots.items, bots.len);
    cf_user_vector_dispose(&bots);
    return rc;
}

/* ---- new ---------------------------------------------------------------- */

cf_err cf_action_accounts_bots_new(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_err rc = cf_before_actions(ctx, bots_before());
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    int64_t actor_id = 0;
    rc = bots_ensure_can_administer(ctx, &actor_id);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    rc = bots_respond_html(ctx);
    if (rc != CF_OK) return rc;

    return bots_render_form(ctx, NULL, NULL, true);
}

/* ---- create -------------------------------------------------------------- */

typedef struct {
    int64_t actor_id;
    cf_str name; /* owned */
    bool has_webhook;
    cf_str webhook_url; /* owned when has_webhook */
    bots_avatar avatar;
} bots_create_arg;

static void bots_create_arg_dispose(bots_create_arg *arg) {
    if (arg == NULL) return;
    bots_str_free(&arg->name);
    bots_str_free(&arg->webhook_url);
    arg->has_webhook = false;
}

static cf_err bots_create_write(cf_tx *tx, void *arg) {
    bots_create_arg *create = arg;
    cf_err rc = bots_revalidate_actor(tx, create->actor_id);
    if (rc != CF_OK) return rc;
    cf_user bot = {0};
    cf_optional_str url = {false, {NULL, 0}};
    if (create->has_webhook) {
        url.present = true;
        url.value = create->webhook_url;
    }
    rc = cf_user_create_bot(tx, create->name, url, &bot);
    if (rc != CF_OK) {
        cf_user_dispose(&bot);
        return rc;
    }
    int64_t bot_id = bot.id;
    cf_user_dispose(&bot);
    /* `attachments::assign` inside the save: Unchanged and Delete (a new bot
     * has no attachment, so Delete is the reference no-op lookup); Invalid
     * never reaches the write (answered before it). */
    if (create->avatar == BOTS_AVATAR_DELETE) {
        rc = bots_avatar_delete_write(tx, bot_id);
    }
    return rc;
}

cf_err cf_action_accounts_bots_create(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_err rc = cf_before_actions(ctx, bots_before());
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    int64_t actor_id = 0;
    rc = bots_ensure_can_administer(ctx, &actor_id);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    const cf_param *params = NULL;
    rc = bots_bot_params(ctx, &params);
    if (rc != CF_OK) return rc;

    /* `name.ok_or(Error::internal)`: absent or nonscalar is the reference
     * 500, never a blank insert. */
    bots_create_arg arg;
    memset(&arg, 0, sizeof arg);
    arg.actor_id = actor_id;
    const cf_param *name = bots_permitted(params, "name");
    cf_span name_text = {NULL, 0};
    rc = cf_param_to_s(name, &name_text);
    if (rc == CF_NOT_FOUND) {
        rc = CF_INTERNAL;
    }
    if (rc == CF_OK) rc = bots_str_dup(name_text, &arg.name);
    const cf_param *webhook = bots_permitted(params, "webhook_url");
    if (rc == CF_OK && webhook != NULL) {
        cf_span url_text = {NULL, 0};
        rc = cf_param_to_s(webhook, &url_text);
        if (rc == CF_OK) {
            rc = bots_str_dup(url_text, &arg.webhook_url);
            if (rc == CF_OK) arg.has_webhook = true;
        } else if (rc == CF_NOT_FOUND) {
            rc = CF_OK; /* nonscalar: the reference None (no webhook) */
        }
    }
    if (rc == CF_OK) {
        arg.avatar = bots_avatar_assignment(params);
        if (arg.avatar == BOTS_AVATAR_INVALID) rc = bots_invalid_avatar();
    }
    if (rc == CF_OK) rc = cf_write(ctx->app, bots_create_write, &arg);
    bots_create_arg_dispose(&arg);
    if (rc != CF_OK) return rc;

    return bots_redirect_to_bots(ctx);
}

/* ---- edit ---------------------------------------------------------------- */

cf_err cf_action_accounts_bots_edit(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_err rc = cf_before_actions(ctx, bots_before());
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    int64_t actor_id = 0;
    rc = bots_ensure_can_administer(ctx, &actor_id);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user bot = {0};
    rc = bots_set_bot(ctx, "id", &bot);
    if (rc != CF_OK) {
        cf_user_dispose(&bot);
        return rc;
    }
    rc = bots_respond_html(ctx);
    if (rc == CF_OK) {
        bool found = false;
        cf_str url = {NULL, 0};
        rc = cf_user_webhook_url(ctx->reader, &bot, &found, &url);
        if (rc == CF_OK) {
            cf_optional_str webhook = {found, url};
            rc = bots_render_form(ctx, &bot, &webhook, false);
            cf_str_dispose(&url);
        }
    }
    cf_user_dispose(&bot);
    return rc;
}

/* ---- update -------------------------------------------------------------- */

typedef struct {
    int64_t actor_id;
    int64_t bot_id;
    bool has_name;
    cf_str name; /* owned when has_name */
    bool has_webhook;
    cf_str webhook_url; /* owned when has_webhook */
    bots_avatar avatar;
} bots_update_arg;

static void bots_update_arg_dispose(bots_update_arg *arg) {
    if (arg == NULL) return;
    bots_str_free(&arg->name);
    bots_str_free(&arg->webhook_url);
    arg->has_name = false;
    arg->has_webhook = false;
}

static cf_err bots_update_write(cf_tx *tx, void *arg) {
    bots_update_arg *update = arg;
    cf_err rc = bots_revalidate_actor(tx, update->actor_id);
    if (rc != CF_OK) return rc;
    cf_user bot = {0};
    rc = bots_reload_bot(tx, update->bot_id, &bot);
    if (rc != CF_OK) {
        cf_user_dispose(&bot);
        return rc;
    }
    /* `UserChanges { name, ..default }`: only a scalar name changes the row;
     * absent or nonscalar leaves it unchanged (the reference None). */
    cf_user_changes changes;
    memset(&changes, 0, sizeof changes);
    if (update->has_name) changes.name = &update->name;
    cf_optional_str url = {false, {NULL, 0}};
    if (update->has_webhook) {
        url.present = true;
        url.value = update->webhook_url;
    }
    /* `update_bot!`: the webhook first, then the bot, in one transaction. */
    rc = cf_user_update_bot(tx, &bot, &changes, url);
    int64_t bot_id = bot.id;
    cf_user_dispose(&bot);
    if (rc != CF_OK) return rc;
    if (update->avatar == BOTS_AVATAR_DELETE) {
        rc = bots_avatar_delete_write(tx, bot_id);
    }
    return rc;
}

cf_err cf_action_accounts_bots_update(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_err rc = cf_before_actions(ctx, bots_before());
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    int64_t actor_id = 0;
    rc = bots_ensure_can_administer(ctx, &actor_id);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user bot = {0};
    rc = bots_set_bot(ctx, "id", &bot);
    if (rc != CF_OK) {
        cf_user_dispose(&bot);
        return rc;
    }
    int64_t bot_id = bot.id;
    cf_user_dispose(&bot);

    const cf_param *params = NULL;
    rc = bots_bot_params(ctx, &params);
    if (rc != CF_OK) return rc;

    bots_update_arg arg;
    memset(&arg, 0, sizeof arg);
    arg.actor_id = actor_id;
    arg.bot_id = bot_id;
    const cf_param *name = bots_permitted(params, "name");
    if (name != NULL) {
        cf_span name_text = {NULL, 0};
        rc = cf_param_to_s(name, &name_text);
        if (rc == CF_OK) {
            rc = bots_str_dup(name_text, &arg.name);
            if (rc == CF_OK) arg.has_name = true;
        } else if (rc == CF_NOT_FOUND) {
            rc = CF_OK; /* nonscalar: the reference None (unchanged) */
        }
    }
    const cf_param *webhook = bots_permitted(params, "webhook_url");
    if (rc == CF_OK && webhook != NULL) {
        cf_span url_text = {NULL, 0};
        rc = cf_param_to_s(webhook, &url_text);
        if (rc == CF_OK) {
            rc = bots_str_dup(url_text, &arg.webhook_url);
            if (rc == CF_OK) arg.has_webhook = true;
        } else if (rc == CF_NOT_FOUND) {
            rc = CF_OK; /* nonscalar: the reference None (unchanged) */
        }
    }
    if (rc == CF_OK) {
        arg.avatar = bots_avatar_assignment(params);
        if (arg.avatar == BOTS_AVATAR_INVALID) rc = bots_invalid_avatar();
    }
    if (rc == CF_OK) rc = cf_write(ctx->app, bots_update_write, &arg);
    bots_update_arg_dispose(&arg);
    if (rc != CF_OK) return rc;

    return bots_redirect_to_bots(ctx);
}

/* ---- destroy ------------------------------------------------------------- */

typedef struct {
    int64_t actor_id;
    int64_t bot_id;
} bots_destroy_arg;

static cf_err bots_destroy_write(cf_tx *tx, void *arg) {
    const bots_destroy_arg *destroy = arg;
    cf_err rc = bots_revalidate_actor(tx, destroy->actor_id);
    if (rc != CF_OK) return rc;
    cf_user bot = {0};
    rc = bots_reload_bot(tx, destroy->bot_id, &bot);
    if (rc != CF_OK) {
        cf_user_dispose(&bot);
        return rc;
    }
    /* `@bot.deactivate`: disconnects first (the mandatory DISCONNECT_USER),
     * then removes memberships/push/searches/sessions and scrambles the
     * email. The reference keeps direct memberships and the webhook row. */
    rc = cf_user_deactivate(tx, &bot);
    cf_user_dispose(&bot);
    return rc;
}

cf_err cf_action_accounts_bots_destroy(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_err rc = cf_before_actions(ctx, bots_before());
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    int64_t actor_id = 0;
    rc = bots_ensure_can_administer(ctx, &actor_id);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user bot = {0};
    rc = bots_set_bot(ctx, "id", &bot);
    if (rc != CF_OK) {
        cf_user_dispose(&bot);
        return rc;
    }
    bots_destroy_arg arg = {.actor_id = actor_id, .bot_id = bot.id};
    cf_user_dispose(&bot);
    rc = cf_write(ctx->app, bots_destroy_write, &arg);
    if (rc != CF_OK) return rc;

    return bots_redirect_to_bots(ctx);
}
