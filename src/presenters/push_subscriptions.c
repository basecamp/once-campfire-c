/* src/presenters/push_subscriptions.c — the users/push_subscriptions#index
 * presenter.
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/users/push_subscriptions.rs
 * (`index`): `PushSubscription::for_user` mapped through
 * presenters::accounts::push_subscription (the UA parse lives with the
 * renderer's row mapper, cf_view_push_subscription_from_row), plus
 * `concerns::last_room_visited_in` for link_back_to_last_room_visited.
 *
 * Rows are read inside one cf_read_begin/cf_read_end transaction; rendering
 * afterwards performs no SQL.
 */
#include "presenters/push_subscriptions.h"

#include "db/db_internal.h"
#include "models/push_subscription.h"
#include "models/room.h"
#include "models/user.h"
#include "views/internal.h" /* cf_views_integer_cast (the last_room cookie) */

#include <stdlib.h>
#include <string.h>

static cf_span push_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

static void push_item_clear(cf_view_push_subscription *item) {
    cf_str_dispose(&item->endpoint);
    cf_str_dispose(&item->browser);
    cf_str_dispose(&item->version);
    cf_str_dispose(&item->platform);
    memset(item, 0, sizeof *item);
}

static cf_err push_item_push(cf_view_push_subscription_vector *vector,
                             cf_view_push_subscription *item) {
    if (vector->len == vector->cap) {
        size_t cap = vector->cap != 0 ? vector->cap * 2 : 4;
        if (cap < vector->cap || cap > SIZE_MAX / sizeof *vector->items) {
            return CF_LIMIT;
        }
        cf_view_push_subscription *grown =
            realloc(vector->items, cap * sizeof *vector->items);
        if (grown == NULL) return CF_NOMEM;
        vector->items = grown;
        vector->cap = cap;
    }
    vector->items[vector->len++] = *item;
    memset(item, 0, sizeof *item);
    return CF_OK;
}

/* `concerns::last_room_visited_in`: the `last_room` cookie's room when the
 * user is a member of it, else the user's original room; found=false leaves
 * link_back_to_last_room_visited at root. */
static cf_err push_last_room_visited(cf_ctx *ctx, int64_t user_id,
                                     bool *found, int64_t *room_id) {
    *found = false;
    *room_id = 0;
    cf_span cookie = {NULL, 0};
    cf_err rc = cf_ctx_cookie_get(ctx, push_span("last_room"), &cookie);
    if (rc == CF_NOT_FOUND) rc = CF_OK;
    if (rc != CF_OK) return rc;
    int64_t requested = 0;
    if (cookie.len != 0 && cf_views_integer_cast(cookie, &requested)) {
        cf_room room = {0};
        bool in_room = false;
        rc = cf_room_find_for_user(ctx->reader, user_id, requested, &in_room,
                                   &room);
        cf_room_dispose(&room);
        if (rc != CF_OK) return rc;
        if (in_room) {
            *found = true;
            *room_id = requested;
            return CF_OK;
        }
    }
    cf_room room = {0};
    bool is_original = false;
    rc = cf_room_original_for_user(ctx->reader, user_id, &is_original, &room);
    if (rc == CF_OK && is_original) {
        *found = true;
        *room_id = room.id;
    }
    cf_room_dispose(&room);
    return rc;
}

cf_err cf_presenter_users_push_index(cf_ctx *ctx, const cf_user *user,
                                     cf_view_users_push_index_model *out) {
    if (ctx == NULL || user == NULL || out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);

    cf_err rc = cf_read_begin(ctx->reader);
    if (rc != CF_OK) return rc;

    /* `PushSubscription::for_user(conn, user_id)`, model order. */
    cf_push_subscription_vector rows = {0};
    rc = cf_push_subscription_for_user(ctx->reader, user->id, &rows);
    for (size_t i = 0; rc == CF_OK && i < rows.len; i++) {
        const cf_push_subscription *row = &rows.items[i];
        cf_view_push_subscription item;
        rc = cf_view_push_subscription_from_row(
            row->id,
            row->endpoint.present
                ? (cf_span){(const unsigned char *)row->endpoint.value.ptr,
                            row->endpoint.value.len}
                : (cf_span){NULL, 0},
            row->user_agent.present
                ? (cf_span){(const unsigned char *)row->user_agent.value.ptr,
                            row->user_agent.value.len}
                : (cf_span){NULL, 0},
            &item);
        if (rc == CF_OK) rc = push_item_push(&out->subscriptions, &item);
        if (rc != CF_OK) push_item_clear(&item);
    }
    cf_push_subscription_vector_dispose(&rows);
    if (rc != CF_OK) goto fail;

    rc = push_last_room_visited(ctx, user->id, &out->has_last_room,
                                &out->last_room_id);
    if (rc != CF_OK) goto fail;

    rc = cf_read_end(ctx->reader);
    if (rc != CF_OK) {
        cf_view_users_push_index_model_dispose(out);
        return rc;
    }
    return CF_OK;
fail:
    {
        cf_err end_rc = cf_read_end(ctx->reader);
        (void)end_rc;
    }
    cf_view_users_push_index_model_dispose(out);
    return rc;
}
