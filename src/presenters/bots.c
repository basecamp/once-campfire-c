/* src/presenters/bots.c — the accounts/bots presenters.
 *
 * Sources: tmp/rust-ref/crates/campfire/src/controllers/presenters/accounts.rs
 * (`bot`, `bot_form`, `sort_by_lower_name`) and the `User::Bot` model
 * (tmp/rails-ref/app/models/user/bot.rb: `bot_key`, `webhook_url`).
 *
 * Rows are read inside one read transaction on ctx->reader; rendering
 * afterwards performs no SQL.  The bot rows the controller already resolved
 * (index's `active_bots_ordered`, `set_bot`) are passed in.
 *
 * Key rotation display: the model carries the row's current `bot_key`
 * ("id-token") only.  `reset_bot_key` overwrites `bot_token`, so a
 * previously presented key no longer authenticates and is never rendered
 * again; there is no "previous key" field to preserve.
 */
#include "presenters/bots.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "db/db_internal.h"
#include "models/active_storage.h"
#include "models/room.h"
#include "models/user.h"
#include "storage/active_storage.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Owned NUL-terminated copy (the cf_str contract). */
static cf_err copy_span(cf_span span, cf_str *out) {
    memset(out, 0, sizeof *out);
    if (span.len != 0 && span.ptr == NULL) return CF_INVALID;
    char *copy = malloc(span.len + 1);
    if (copy == NULL) return CF_NOMEM;
    if (span.len != 0) memcpy(copy, span.ptr, span.len);
    copy[span.len] = '\0';
    out->ptr = copy;
    out->len = span.len;
    return CF_OK;
}

/* `Time#to_fs(:number)`: %Y%m%d%H%M%S in UTC (borrowed from
 * src/presenters/rooms.c; the fresh avatar path's `v` param). */
static void to_fs_number(int64_t us, char out[16]) {
    time_t seconds = (time_t)(us / 1000000);
    if (us < 0 && us % 1000000 != 0) seconds -= 1;
    struct tm tm;
    if (gmtime_r(&seconds, &tm) == NULL) {
        memcpy(out, "00000000000000", 15);
        return;
    }
    strftime(out, 16, "%Y%m%d%H%M%S", &tm);
}

/* `fresh_user_avatar_path(user)` (borrowed from src/presenters/rooms.c). */
static cf_err bot_avatar_url(const cf_ctx *ctx, const cf_user *user,
                             cf_str *out) {
    memset(out, 0, sizeof *out);
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->secret_key_base == NULL) return CF_OK;
    cf_str token = {0};
    cf_err rc = cf_auth_signed_id_generate(
        (cf_span){(const unsigned char *)config->secret_key_base,
                  config->secret_key_base_len},
        (cf_span){(const unsigned char *)"User", 4}, user->id,
        (cf_span){(const unsigned char *)"avatar", 6}, true, false, 0, &token);
    if (rc != CF_OK) return rc;
    char number[16];
    to_fs_number(user->updated_at, number);
    const char *prefix = "/users/";
    const char *middle = "/avatar?v=";
    size_t len = strlen(prefix) + token.len + strlen(middle) + 14;
    out->ptr = malloc(len + 1);
    if (out->ptr == NULL) {
        cf_str_dispose(&token);
        return CF_NOMEM;
    }
    size_t at = 0;
    memcpy(out->ptr + at, prefix, strlen(prefix));
    at += strlen(prefix);
    memcpy(out->ptr + at, token.ptr, token.len);
    at += token.len;
    memcpy(out->ptr + at, middle, strlen(middle));
    at += strlen(middle);
    memcpy(out->ptr + at, number, 14);
    at += 14;
    out->ptr[at] = '\0';
    out->len = at;
    cf_str_dispose(&token);
    return CF_OK;
}

/* ASCII-lowercased comparison (`sort_by_lower_name`: `to_ascii_lowercase`;
 * SQLite's LOWER is ASCII-only too). */
static int lower_cmp(cf_span a, cf_span b) {
    size_t count = a.len < b.len ? a.len : b.len;
    for (size_t i = 0; i < count; i++) {
        unsigned char ca = a.ptr[i], cb = b.ptr[i];
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (a.len == b.len) return 0;
    return a.len < b.len ? -1 : 1;
}

/* `ORDER BY LOWER(name)`: stable insertion pass over the mapped rooms. */
static void sort_rooms_by_lower_name(cf_view_accounts_bot_room *items,
                                     size_t len) {
    for (size_t i = 1; i < len; i++) {
        cf_view_accounts_bot_room current = items[i];
        size_t j = i;
        while (j > 0 &&
               lower_cmp((cf_span){(const unsigned char *)current.name.ptr,
                                   current.name.len},
                         (cf_span){(const unsigned char *)items[j - 1].name.ptr,
                                   items[j - 1].name.len}) < 0) {
            items[j] = items[j - 1];
            j--;
        }
        items[j] = current;
    }
}

/* Append one mapped room (the caller holds the read transaction). */
static cf_err push_room(cf_db *db, const cf_room *room,
                        cf_view_accounts_bot_room_vector *vector) {
    (void)db;
    if (vector->len == vector->cap) {
        size_t cap = vector->cap == 0 ? 4 : vector->cap * 2;
        cf_view_accounts_bot_room *grown =
            realloc(vector->items, cap * sizeof *grown);
        if (grown == NULL) return CF_NOMEM;
        vector->items = grown;
        vector->cap = cap;
    }
    cf_view_accounts_bot_room *slot = &vector->items[vector->len];
    memset(slot, 0, sizeof *slot);
    slot->id = room->id;
    cf_err rc;
    if (room->name.present) {
        rc = copy_span((cf_span){(const unsigned char *)room->name.value.ptr,
                                 room->name.value.len},
                       &slot->name);
    } else {
        slot->name.ptr = malloc(1);
        if (slot->name.ptr == NULL) return CF_NOMEM;
        slot->name.ptr[0] = '\0';
        slot->name.len = 0;
        rc = CF_OK;
    }
    if (rc != CF_OK) return rc;
    vector->len++;
    return CF_OK;
}

/* Map one bot row; the caller holds the read transaction. */
static cf_err map_bot(cf_ctx *ctx, cf_db *db, const cf_user *row,
                      cf_view_accounts_bot *out) {
    memset(out, 0, sizeof *out);
    out->id = row->id;
    cf_err rc = copy_span(
        (cf_span){(const unsigned char *)row->name.ptr, row->name.len},
        &out->name);
    if (rc != CF_OK) goto fail;
    rc = cf_user_title(row, &out->title);
    if (rc != CF_OK) goto fail;
    rc = bot_avatar_url(ctx, row, &out->avatar_url);
    if (rc != CF_OK) goto fail;
    rc = cf_user_bot_key(row, &out->bot_key);
    if (rc != CF_OK) goto fail;
    {
        cf_room_vector rooms = {0};
        rc = cf_room_for_user_without_directs(db, row->id, &rooms);
        if (rc != CF_OK) goto fail;
        for (size_t i = 0; i < rooms.len; i++) {
            rc = push_room(db, &rooms.items[i], &out->rooms);
            if (rc != CF_OK) break;
        }
        cf_room_vector_dispose(&rooms);
        if (rc != CF_OK) goto fail;
        sort_rooms_by_lower_name(out->rooms.items, out->rooms.len);
    }
    return CF_OK;
fail:
    cf_view_accounts_bot_dispose(out);
    return rc;
}

/* `url_for(bot.avatar)`: the absolute blob redirect path for the attached
 * avatar, if any.  The caller holds the read transaction. */
static cf_err bot_form_avatar_url(cf_ctx *ctx, cf_db *db, const cf_user *bot,
                                  cf_view_accounts_bot_form *form) {
    bool found = false;
    cf_attachment attachment = {0};
    cf_err rc = cf_attachment_find_for(db, CF_STR_LIT("User"), bot->id,
                                       CF_STR_LIT("avatar"), &found,
                                       &attachment);
    if (rc != CF_OK) return rc;
    if (!found) return CF_OK;
    cf_blob blob = {0};
    rc = cf_attachment_blob(db, &attachment, &blob);
    cf_attachment_dispose(&attachment);
    if (rc != CF_OK) return rc;
    cf_builder sanitized = {0};
    rc = cf_active_filename_sanitize(
        (cf_span){(const unsigned char *)blob.filename.ptr, blob.filename.len},
        &sanitized);
    if (rc != CF_OK) {
        cf_blob_dispose(&blob);
        return rc;
    }
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL ||
        config->secret_key_base == NULL) {
        cf_builder_dispose(&sanitized);
        cf_blob_dispose(&blob);
        return CF_INVALID;
    }
    cf_builder url = {0};
    rc = cf_builder_append(
        &url, (cf_span){(const unsigned char *)config->public_origin,
                        strlen(config->public_origin)});
    if (rc == CF_OK) {
        rc = cf_active_blob_redirect_path(
            (cf_span){(const unsigned char *)config->secret_key_base,
                      config->secret_key_base_len},
            blob.id, (cf_span){sanitized.ptr, sanitized.len},
            (cf_span){NULL, 0}, false, &url);
    }
    if (rc == CF_OK) {
        rc = copy_span((cf_span){url.ptr, url.len}, &form->avatar_url);
        if (rc == CF_OK) form->has_avatar_url = true;
    }
    cf_builder_dispose(&url);
    cf_builder_dispose(&sanitized);
    cf_blob_dispose(&blob);
    return rc;
}

/* ------------------------------------------------------------ disposal */

void cf_view_accounts_bot_room_vector_dispose(
    cf_view_accounts_bot_room_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len; i++) {
        cf_str_dispose(&vector->items[i].name);
    }
    free(vector->items);
    memset(vector, 0, sizeof *vector);
}

void cf_view_accounts_bot_dispose(cf_view_accounts_bot *bot) {
    if (bot == NULL) return;
    cf_str_dispose(&bot->name);
    cf_str_dispose(&bot->title);
    cf_str_dispose(&bot->avatar_url);
    cf_str_dispose(&bot->bot_key);
    cf_view_accounts_bot_room_vector_dispose(&bot->rooms);
    memset(bot, 0, sizeof *bot);
}

void cf_view_accounts_bot_vector_dispose(cf_view_accounts_bot_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len; i++) {
        cf_view_accounts_bot_dispose(&vector->items[i]);
    }
    free(vector->items);
    memset(vector, 0, sizeof *vector);
}

void cf_view_accounts_bot_form_dispose(cf_view_accounts_bot_form *form) {
    if (form == NULL) return;
    cf_str_dispose(&form->name);
    cf_str_dispose(&form->webhook_url);
    cf_str_dispose(&form->avatar_url);
    memset(form, 0, sizeof *form);
}

void cf_view_accounts_bots_index_model_dispose(
    cf_view_accounts_bots_index_model *model) {
    if (model == NULL) return;
    cf_view_accounts_bot_vector_dispose(&model->bots);
    memset(model, 0, sizeof *model);
}

void cf_view_accounts_bots_new_model_dispose(
    cf_view_accounts_bots_new_model *model) {
    if (model == NULL) return;
    cf_view_accounts_bot_form_dispose(&model->form);
    memset(model, 0, sizeof *model);
}

void cf_view_accounts_bots_edit_model_dispose(
    cf_view_accounts_bots_edit_model *model) {
    if (model == NULL) return;
    cf_view_accounts_bot_form_dispose(&model->form);
    memset(model, 0, sizeof *model);
}

/* ----------------------------------------------------------- presenters */

cf_err cf_presenter_bot(cf_ctx *ctx, const cf_user *bot,
                        cf_view_accounts_bot *out) {
    if (ctx == NULL || bot == NULL || out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    cf_err rc = cf_read_begin(ctx->reader);
    if (rc != CF_OK) return rc;
    rc = map_bot(ctx, ctx->reader, bot, out);
    cf_err end_rc = cf_read_end(ctx->reader);
    if (rc == CF_OK) rc = end_rc;
    return rc;
}

cf_err cf_presenter_bots_index(cf_ctx *ctx,
                               cf_view_accounts_bots_index_model *out) {
    if (ctx == NULL || out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    cf_err rc = cf_read_begin(ctx->reader);
    if (rc != CF_OK) return rc;
    cf_user_vector bots = {0};
    rc = cf_user_active_bots_ordered(ctx->reader, &bots);
    if (rc != CF_OK) goto done;
    if (bots.len != 0) {
        out->bots.items = calloc(bots.len, sizeof *out->bots.items);
        if (out->bots.items == NULL) {
            rc = CF_NOMEM;
            goto done;
        }
        out->bots.cap = bots.len;
        for (size_t i = 0; i < bots.len; i++) {
            rc = map_bot(ctx, ctx->reader, &bots.items[i],
                         &out->bots.items[out->bots.len]);
            if (rc != CF_OK) break;
            out->bots.len++;
        }
    }
done:
    cf_user_vector_dispose(&bots);
    if (rc != CF_OK) cf_view_accounts_bots_index_model_dispose(out);
    cf_err end_rc = cf_read_end(ctx->reader);
    if (rc == CF_OK) rc = end_rc;
    return rc;
}

cf_err cf_presenter_bot_form(cf_ctx *ctx, const cf_user *bot,
                             cf_view_accounts_bot_form *out) {
    if (ctx == NULL || bot == NULL || out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    cf_err rc = cf_read_begin(ctx->reader);
    if (rc != CF_OK) return rc;
    rc = copy_span(
        (cf_span){(const unsigned char *)bot->name.ptr, bot->name.len},
        &out->name);
    if (rc != CF_OK) goto done;
    out->has_name = true;
    {
        bool found = false;
        cf_str url = {0};
        rc = cf_user_webhook_url(ctx->reader, bot, &found, &url);
        if (rc != CF_OK) goto done;
        if (found) {
            out->webhook_url = url;
            out->has_webhook_url = true;
        }
    }
    rc = bot_form_avatar_url(ctx, ctx->reader, bot, out);
done:
    if (rc != CF_OK) cf_view_accounts_bot_form_dispose(out);
    cf_err end_rc = cf_read_end(ctx->reader);
    if (rc == CF_OK) rc = end_rc;
    return rc;
}
