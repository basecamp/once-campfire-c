/* src/presenters/users_profiles.c — the users/profiles#show presenter.
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/users/profiles.rs
 * (`show`) and the presenter layer it uses:
 *
 *   transfer_id   -> presenters::accounts::transfer_id
 *                    (signed id, purpose "transfer", 4-hour expiry)
 *   one read      -> attachments::attached_blob("User", id, "avatar") and
 *                    presenters::accounts::profile_memberships
 *   user_summary  -> presenters::user_summary (fresh_user_avatar_path)
 *
 * `profile_memberships` maps each `Membership::with_ordered_room` pair:
 * room_param_key from the STI type, `room_display_name(conn, &room, user)`
 * (a direct room is named after its other members, read here) and the
 * membership involvement's name ("" when the column is NULL).  The
 * direct/shared partition preserves the query's order within each side.
 *
 * Rows are read inside one cf_read_begin/cf_read_end transaction; rendering
 * afterwards performs no SQL.
 */
#include "presenters/users_profiles.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "db/db_internal.h"
#include "models/active_storage.h"
#include "models/membership.h"
#include "models/room.h"
#include "models/user.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- owned text ----------------------------------------------------------- */

static cf_err profile_str_dup(cf_span span, cf_str *out) {
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

static cf_err profile_str_set(cf_str *out, const char *text) {
    return profile_str_dup(
        (cf_span){(const unsigned char *)text, strlen(text)}, out);
}

/* ---- transfer_id / avatar_path -------------------------------------------- */

/* `TRANSFER_LINK_EXPIRY_DURATION`: 4 hours, in microseconds. */
#define PROFILES_TRANSFER_EXPIRY_US (INT64_C(4) * INT64_C(3600) * INT64_C(1000000))

/* `user.transfer_id`: signed_id(purpose: :transfer, expires_in: 4.hours). */
static cf_err profile_transfer_id(cf_ctx *ctx, int64_t user_id, cf_str *out) {
    memset(out, 0, sizeof *out);
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->secret_key_base == NULL) return CF_INTERNAL;
    return cf_auth_signed_id_generate(
        (cf_span){(const unsigned char *)config->secret_key_base,
                  config->secret_key_base_len},
        (cf_span){(const unsigned char *)"User", 4}, user_id,
        (cf_span){(const unsigned char *)"transfer", 8}, true, true,
        cf_now_us(ctx->app) + PROFILES_TRANSFER_EXPIRY_US, out);
}

/* `Time#to_fs(:number)`: %Y%m%d%H%M%S in UTC. */
static void profile_to_fs_number(int64_t us, char out[15]) {
    time_t seconds = (time_t)(us / 1000000);
    if (us < 0 && us % 1000000 != 0) seconds -= 1;
    struct tm tm;
    if (gmtime_r(&seconds, &tm) == NULL) {
        memcpy(out, "00000000000000", 14);
        return;
    }
    if (strftime(out, 15, "%Y%m%d%H%M%S", &tm) == 0) {
        memcpy(out, "00000000000000", 14);
    }
}

/* `fresh_user_avatar_path(user)`: "/users/<avatar token>/avatar?v=<number>". */
static cf_err profile_avatar_path(cf_ctx *ctx, const cf_user *user,
                                  cf_str *out) {
    memset(out, 0, sizeof *out);
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->secret_key_base == NULL) return CF_INTERNAL;
    cf_str token = {0};
    cf_err rc = cf_auth_signed_id_generate(
        (cf_span){(const unsigned char *)config->secret_key_base,
                  config->secret_key_base_len},
        (cf_span){(const unsigned char *)"User", 4}, user->id,
        (cf_span){(const unsigned char *)"avatar", 6}, true, false, 0, &token);
    if (rc != CF_OK) return rc;
    char number[15];
    profile_to_fs_number(user->updated_at, number);
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

/* ---- row -> view ---------------------------------------------------------- */

/* `user_summary(secrets, user)` (presenters.rs). */
static cf_err profile_fill_user(cf_ctx *ctx, const cf_user *user,
                                cf_view_profile_user *out) {
    memset(out, 0, sizeof *out);
    out->id = user->id;
    out->role = user->role;
    out->status = user->status;
    cf_err rc = profile_str_dup(
        (cf_span){(const unsigned char *)user->name.ptr, user->name.len},
        &out->name);
    if (rc == CF_OK && user->bio.present) {
        out->has_bio = true;
        rc = profile_str_dup(
            (cf_span){(const unsigned char *)user->bio.value.ptr,
                      user->bio.value.len},
            &out->bio);
    }
    if (rc == CF_OK && user->email_address.present) {
        out->has_email = true;
        rc = profile_str_dup(
            (cf_span){(const unsigned char *)user->email_address.value.ptr,
                      user->email_address.value.len},
            &out->email_address);
    }
    if (rc == CF_OK) rc = profile_avatar_path(ctx, user, &out->avatar_path);
    if (rc != CF_OK) cf_view_profile_user_dispose(out);
    return rc;
}

/* `Room.model_name.param_key` for the room's STI class. */
static const char *profile_param_key(cf_room_type room_type) {
    switch (room_type) {
    case CF_ROOM_CLOSED:
        return "rooms_closed";
    case CF_ROOM_DIRECT:
        return "rooms_direct";
    case CF_ROOM_OPEN:
    default:
        return "rooms_open";
    }
}

static void profile_membership_clear(cf_view_profile_membership *item) {
    cf_str_dispose(&item->room_param_key);
    cf_str_dispose(&item->room_display_name);
    cf_str_dispose(&item->involvement);
    memset(item, 0, sizeof *item);
}

static cf_err profile_membership_push(
    cf_view_profile_membership_vector *vector,
    cf_view_profile_membership *item) {
    if (vector->len == vector->cap) {
        size_t cap = vector->cap != 0 ? vector->cap * 2 : 4;
        if (cap < vector->cap || cap > SIZE_MAX / sizeof *vector->items) {
            return CF_LIMIT;
        }
        cf_view_profile_membership *grown =
            realloc(vector->items, cap * sizeof *vector->items);
        if (grown == NULL) return CF_NOMEM;
        vector->items = grown;
        vector->cap = cap;
    }
    vector->items[vector->len++] = *item;
    memset(item, 0, sizeof *item);
    return CF_OK;
}

/* `presenters::accounts::profile_memberships`' per-pair mapping. */
static cf_err profile_fill_membership(cf_db *db, const cf_user *for_user,
                                      const cf_membership_room_pair *pair,
                                      cf_view_profile_membership *out) {
    memset(out, 0, sizeof *out);
    out->room_id = pair->room.id;
    out->direct = cf_room_direct(&pair->room);
    cf_err rc = profile_str_set(&out->room_param_key,
                                profile_param_key(pair->room.room_type));
    if (rc == CF_OK) {
        /* `membership.involvement.map(|i| i.name()).unwrap_or_default()`. */
        const char *name =
            pair->membership.involvement.present
                ? cf_involvement_name(pair->membership.involvement.value)
                : "";
        if (name == NULL) name = "";
        rc = profile_str_set(&out->involvement, name);
    }
    if (rc == CF_OK) {
        rc = cf_presenter_room_display_name(db, &pair->room, for_user,
                                            &out->room_display_name);
    }
    if (rc != CF_OK) profile_membership_clear(out);
    return rc;
}

/* ---- the presenter -------------------------------------------------------- */

cf_err cf_presenter_users_profile(cf_ctx *ctx, const cf_user *user,
                                  cf_view_users_profile_model *out) {
    if (ctx == NULL || user == NULL || out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);

    cf_err rc = profile_transfer_id(ctx, user->id, &out->transfer_id);
    if (rc != CF_OK) {
        cf_view_users_profile_model_dispose(out);
        return rc;
    }

    rc = cf_read_begin(ctx->reader);
    if (rc != CF_OK) {
        cf_view_users_profile_model_dispose(out);
        return rc;
    }

    /* `attachments::attached_blob(conn, "User", user.id, "avatar").is_some()`. */
    {
        cf_attachment attachment = {0};
        bool attached = false;
        rc = cf_attachment_find_for(ctx->reader, CF_STR_LIT("User"), user->id,
                                    CF_STR_LIT("avatar"), &attached,
                                    &attachment);
        cf_attachment_dispose(&attachment);
        if (rc != CF_OK) goto fail;
        out->avatar_attached = attached;
    }

    rc = profile_fill_user(ctx, user, &out->user);
    if (rc != CF_OK) goto fail;

    /* `Current.user.memberships.with_ordered_room.partition { m.room.direct? }`:
     * `(direct, shared)`; the view renders shared then direct. */
    cf_membership_room_pair_vector pairs = {0};
    rc = cf_membership_with_ordered_room(ctx->reader, user->id, &pairs);
    if (rc != CF_OK) goto fail;
    for (size_t i = 0; i < pairs.len && rc == CF_OK; i++) {
        cf_view_profile_membership item;
        rc = profile_fill_membership(ctx->reader, user, &pairs.items[i], &item);
        if (rc == CF_OK) {
            cf_view_profile_membership_vector *side =
                cf_room_direct(&pairs.items[i].room) ? &out->direct_memberships
                                                     : &out->shared_memberships;
            rc = profile_membership_push(side, &item);
        }
        if (rc != CF_OK) profile_membership_clear(&item);
    }
    cf_membership_room_pair_vector_dispose(&pairs);
    if (rc != CF_OK) goto fail;

    rc = cf_read_end(ctx->reader);
    if (rc != CF_OK) {
        cf_view_users_profile_model_dispose(out);
        return rc;
    }
    return CF_OK;
fail:
    {
        cf_err end_rc = cf_read_end(ctx->reader);
        (void)end_rc;
    }
    cf_view_users_profile_model_dispose(out);
    return rc;
}
