/* src/presenters/rooms.c — the rooms#show presenter and the shared user/room
 * view mapping (tmp/rust-ref/crates/campfire/src/controllers/presenters.rs
 * Presenter::user_view / room_view / room_display_name, and
 * controllers/rooms.rs render_show).
 *
 * Rows are read inside one read transaction; rendering afterwards performs no
 * SQL.  The room and user rows the controller already resolved are passed in.
 */
#include "views.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "db/db_internal.h"
#include "models/account.h"
#include "models/message.h"
#include "models/room.h"
#include "models/user.h"

#include <stdio.h>
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

/* `Time#to_fs(:number)`: %Y%m%d%H%M%S in UTC. */
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

/* `fresh_user_avatar_path(user)`. */
static cf_err user_avatar_url(const cf_ctx *ctx, const cf_user *user,
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

cf_err cf_presenter_user_view(cf_ctx *ctx, const cf_user *user,
                              cf_view_user *out) {
    if (ctx == NULL || user == NULL || out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    out->id = user->id;
    cf_err rc = copy_span((cf_span){(const unsigned char *)user->name.ptr,
                                    user->name.len},
                          &out->name);
    if (rc != CF_OK) goto fail;
    {
        cf_str title = {0};
        rc = cf_user_title(user, &title);
        if (rc != CF_OK) goto fail;
        out->title = title;
    }
    rc = user_avatar_url(ctx, user, &out->avatar_url);
    if (rc != CF_OK) goto fail;
    return CF_OK;
fail:
    cf_view_user_dispose(out);
    return rc;
}

/* `room_display_name(room, for_user:)` (presenters.rs); shared with the
 * users/profiles presenter through views.h. */
cf_err cf_presenter_room_display_name(cf_db *db, const cf_room *room,
                                      const cf_user *for_user, cf_str *out) {
    memset(out, 0, sizeof *out);
    cf_span *names = NULL;
    size_t name_count = 0;
    cf_err rc = CF_OK;
    if (cf_room_direct(room)) {
        cf_user_vector users = {0};
        rc = cf_room_users(db, room, &users);
        if (rc != CF_OK) return rc;
        names = malloc((users.len != 0 ? users.len : 1) * sizeof *names);
        if (names == NULL) {
            cf_user_vector_dispose(&users);
            return CF_NOMEM;
        }
        for (size_t i = 0; i < users.len; i++) {
            if (for_user != NULL && users.items[i].id == for_user->id) continue;
            names[name_count++] = (cf_span){
                (const unsigned char *)users.items[i].name.ptr,
                users.items[i].name.len};
        }
        cf_span own = for_user != NULL
                          ? (cf_span){(const unsigned char *)for_user->name.ptr,
                                      for_user->name.len}
                          : (cf_span){NULL, 0};
        rc = cf_view_room_display_name(
            (cf_span){(const unsigned char *)(room->name.present
                                                  ? room->name.value.ptr
                                                  : NULL),
                      room->name.present ? room->name.value.len : 0},
            true, names, name_count, own, out);
        free(names);
        cf_user_vector_dispose(&users);
        return rc;
    }
    if (room->name.present) {
        rc = copy_span((cf_span){(const unsigned char *)room->name.value.ptr,
                                 room->name.value.len},
                       out);
    } else {
        out->ptr = malloc(1);
        if (out->ptr == NULL) return CF_NOMEM;
        out->ptr[0] = '\0';
        out->len = 0;
    }
    return rc;
}

cf_err cf_presenter_room_show(cf_ctx *ctx, const cf_room *room,
                              const cf_user *user, bool has_message_id,
                              int64_t message_id,
                              cf_view_room_show_model *out) {
    if (ctx == NULL || room == NULL || user == NULL || out == NULL) {
        return CF_INVALID;
    }
    memset(out, 0, sizeof *out);
    cf_err rc = cf_read_begin(ctx->reader);
    if (rc != CF_OK) return rc;

    out->room.id = room->id;
    out->room.kind = room->room_type;
    out->room.has_name = room->name.present;
    if (room->name.present) {
        rc = copy_span((cf_span){(const unsigned char *)room->name.value.ptr,
                                 room->name.value.len},
                       &out->room.name);
        if (rc != CF_OK) goto fail;
    }
    rc = cf_presenter_room_display_name(ctx->reader, room, user, &out->room.display_name);
    if (rc != CF_OK) goto fail;
    out->updated_at_us = room->updated_at;
    rc = cf_presenter_user_view(ctx, user, &out->user);
    if (rc != CF_OK) goto fail;

    /* The page around params[:message_id] when it names a message of this
     * room, else the last page. */
    cf_message_vector messages = {0};
    if (has_message_id) {
        cf_message target = {0};
        bool found = false;
        rc = cf_message_find_by_id(ctx->reader, message_id, &found, &target);
        if (rc != CF_OK) goto fail;
        if (found && target.room_id == room->id) {
            rc = cf_message_page_around(ctx->reader, room->id, &target,
                                        &messages);
        } else {
            rc = cf_message_last_page(ctx->reader, room->id, &messages);
        }
        cf_message_dispose(&target);
        if (rc != CF_OK) goto fail;
    } else {
        rc = cf_message_last_page(ctx->reader, room->id, &messages);
        if (rc != CF_OK) goto fail;
    }
    rc = cf_presenter_messages_in_transaction(ctx, messages.items,
                                               messages.len, &out->messages);
    cf_message_vector_dispose(&messages);
    if (rc != CF_OK) goto fail;

    {
        cf_room original = {0};
        bool found = false;
        rc = cf_room_original(ctx->reader, &found, &original);
        if (rc != CF_OK) goto fail;
        bool original_room = found && original.id == room->id;
        cf_room_dispose(&original);
        bool paged = false;
        rc = cf_message_paged(ctx->reader, room->id, &paged);
        if (rc != CF_OK) goto fail;
        out->invitation = original_room && !paged;
    }

    {
        cf_account account = {0};
        bool found = false;
        rc = cf_account_first(ctx->reader, &found, &account);
        if (rc != CF_OK) goto fail;
        if (found) {
            rc = copy_span((cf_span){(const unsigned char *)account.join_code.ptr,
                                     account.join_code.len},
                           &out->join_code);
        } else {
            out->join_code.ptr = malloc(1);
            if (out->join_code.ptr == NULL) {
                cf_account_dispose(&account);
                rc = CF_NOMEM;
                goto fail;
            }
            out->join_code.ptr[0] = '\0';
            out->join_code.len = 0;
            rc = CF_OK;
        }
        cf_account_dispose(&account);
        if (rc != CF_OK) goto fail;
    }

    /* `Turbo::StreamsChannel.signed_stream_name([room_gid.to_param, "messages"])`. */
    {
        const cf_config *config = cf_app_config(ctx->app);
        if (config == NULL || config->secret_key_base == NULL) {
            rc = CF_INVALID;
            goto fail;
        }
        static const char *const CLASSES[] = {"Rooms::Open", "Rooms::Closed",
                                              "Rooms::Direct"};
        char gid[80];
        int n = snprintf(gid, sizeof gid, "gid://campfire/%s/%lld",
                         CLASSES[room->room_type == CF_ROOM_CLOSED
                                     ? 1
                                     : (room->room_type == CF_ROOM_DIRECT ? 2
                                                                          : 0)],
                         (long long)room->id);
        if (n < 0 || (size_t)n >= sizeof gid) {
            rc = CF_INTERNAL;
            goto fail;
        }
        cf_str param = {0};
        rc = cf_auth_global_id_param(
            (cf_span){(const unsigned char *)gid, (size_t)n}, &param);
        if (rc != CF_OK) goto fail;
        cf_span parts[2] = {
            {(const unsigned char *)param.ptr, param.len},
            {(const unsigned char *)"messages", 8}};
        rc = cf_auth_turbo_signed_stream_name(
            (cf_span){(const unsigned char *)config->secret_key_base,
                      config->secret_key_base_len},
            parts, 2, &out->messages_stream_name);
        cf_str_dispose(&param);
        if (rc != CF_OK) goto fail;
    }

    rc = cf_read_end(ctx->reader);
    if (rc != CF_OK) {
        cf_view_room_show_model_dispose(out);
        return rc;
    }
    return CF_OK;
fail:
    {
        cf_err end_rc = cf_read_end(ctx->reader);
        (void)end_rc;
    }
    cf_view_room_show_model_dispose(out);
    return rc;
}
