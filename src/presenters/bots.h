/* src/presenters/bots.h — accounts/bots presenter + view surface (packet V-B).
 *
 * Sources: tmp/rust-ref/crates/campfire/src/controllers/presenters/accounts.rs
 * (`bot`, `bot_form`) and tmp/rust-ref/crates/views/src/accounts.rs
 * (`Bot`, `BotRoom`, `BotForm`, `BotsIndex`, `BotsNew`, `BotsEdit`).
 *
 * Contract: presenters assemble view models from rows inside one read
 * transaction; rendering does no SQL, mutation, filesystem or network work.
 * Every renderer writes into a caller cf_builder with the 8 MiB output cap;
 * on any error the builder is left at its entry length.
 *
 * Temporary home for the view model/renderer declarations: the integrator
 * moves them into src/views.h (frozen for this packet) and wires the two
 * .c files into the Makefile; see the handoff note.
 */
#ifndef CF_PRESENTERS_BOTS_H
#define CF_PRESENTERS_BOTS_H

#include "views.h"

/* `accounts::BotRoom`: `bot.rooms.without_directs.ordered` as `_bot` sees
 * it.  Direct rooms are excluded upstream; `name` is the room's name (empty
 * for a nameless room).  Owned; dispose with the vector disposer. */
typedef struct {
    int64_t id;
    cf_str name; /* owned */
} cf_view_accounts_bot_room;

typedef struct {
    cf_view_accounts_bot_room *items;
    size_t len, cap;
} cf_view_accounts_bot_room_vector;

void cf_view_accounts_bot_room_vector_dispose(
    cf_view_accounts_bot_room_vector *vector);

/* `accounts::Bot`: a bot row as `accounts/bots/_bot` sees it.  `bot_key` is
 * `User#bot_key` ("id-token"); the key rotation display shows the current
 * key only (an old key no longer authenticates and is never rendered).
 * Owned; dispose with cf_view_accounts_bot_dispose. */
typedef struct {
    int64_t id;
    cf_str name;       /* owned */
    cf_str title;      /* owned: User#title */
    cf_str avatar_url; /* owned: fresh_user_avatar_path */
    cf_str bot_key;    /* owned: "id-token" */
    cf_view_accounts_bot_room_vector rooms; /* owned, LOWER(name) order */
} cf_view_accounts_bot;

void cf_view_accounts_bot_dispose(cf_view_accounts_bot *bot);

typedef struct {
    cf_view_accounts_bot *items;
    size_t len, cap;
} cf_view_accounts_bot_vector;

void cf_view_accounts_bot_vector_dispose(cf_view_accounts_bot_vector *vector);

/* `accounts::BotForm`: the fields `accounts/bots/_form` fills in.
 * `avatar_url` is `url_for(bot.avatar)` when attached, absent otherwise.
 * Owned; dispose with cf_view_accounts_bot_form_dispose. */
typedef struct {
    bool has_name;
    cf_str name; /* owned when has_name */
    bool has_webhook_url;
    cf_str webhook_url; /* owned when has_webhook_url */
    bool has_avatar_url;
    cf_str avatar_url; /* owned when has_avatar_url */
} cf_view_accounts_bot_form;

void cf_view_accounts_bot_form_dispose(cf_view_accounts_bot_form *form);

/* `accounts::BotsIndex`: `@bots = User.active_bots.ordered`. */
typedef struct {
    cf_view_accounts_bot_vector bots; /* owned */
} cf_view_accounts_bots_index_model;

void cf_view_accounts_bots_index_model_dispose(
    cf_view_accounts_bots_index_model *model);

/* `accounts::BotsNew`: a default (all-absent) form. */
typedef struct {
    cf_view_accounts_bot_form form; /* owned */
} cf_view_accounts_bots_new_model;

void cf_view_accounts_bots_new_model_dispose(
    cf_view_accounts_bots_new_model *model);

/* `accounts::BotsEdit`: the bot id plus its form. */
typedef struct {
    int64_t bot_id;
    cf_view_accounts_bot_form form; /* owned */
} cf_view_accounts_bots_edit_model;

void cf_view_accounts_bots_edit_model_dispose(
    cf_view_accounts_bots_edit_model *model);

/* `presenters::accounts::bot` for one already-resolved active-bot row,
 * inside one read transaction on ctx->reader: the user facts (name, title,
 * fresh avatar path), the current "id-token" key and the bot's non-direct
 * rooms in LOWER(name) order. */
cf_err cf_presenter_bot(cf_ctx *ctx, const cf_user *bot,
                        cf_view_accounts_bot *out);

/* `Accounts::BotsController#index`: `User.active_bots.ordered`, each mapped
 * as above, inside one read transaction on ctx->reader. */
cf_err cf_presenter_bots_index(cf_ctx *ctx,
                               cf_view_accounts_bots_index_model *out);

/* `presenters::accounts::bot_form` for one already-resolved active-bot row,
 * inside one read transaction on ctx->reader: the name, the webhook URL
 * (absent when the bot has no webhook row) and the absolute blob redirect
 * URL (absent when no avatar is attached). */
cf_err cf_presenter_bot_form(cf_ctx *ctx, const cf_user *bot,
                             cf_view_accounts_bot_form *out);

/* `accounts/bots/index.html.erb` (page and turbo-rails frame). */
cf_err cf_view_accounts_bots_index(
    const cf_view_ctx *ctx, const cf_view_accounts_bots_index_model *model,
    cf_builder *out);
cf_err cf_view_accounts_bots_index_frame(
    const cf_view_ctx *ctx, const cf_view_accounts_bots_index_model *model,
    cf_builder *out);

/* `accounts/bots/new.html.erb` (page and turbo-rails frame). */
cf_err cf_view_accounts_bots_new(const cf_view_ctx *ctx,
                                 const cf_view_accounts_bots_new_model *model,
                                 cf_builder *out);
cf_err cf_view_accounts_bots_new_frame(
    const cf_view_ctx *ctx, const cf_view_accounts_bots_new_model *model,
    cf_builder *out);

/* `accounts/bots/edit.html.erb` (page and turbo-rails frame). */
cf_err cf_view_accounts_bots_edit(
    const cf_view_ctx *ctx, const cf_view_accounts_bots_edit_model *model,
    cf_builder *out);
cf_err cf_view_accounts_bots_edit_frame(
    const cf_view_ctx *ctx, const cf_view_accounts_bots_edit_model *model,
    cf_builder *out);

#endif /* CF_PRESENTERS_BOTS_H */
