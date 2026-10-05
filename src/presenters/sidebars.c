/* src/presenters/sidebars.c — the users/sidebars#show presenter
 * (tmp/rust-ref/crates/campfire/src/controllers/presenters/accounts.rs
 * `sidebar`, `sidebar_direct`, `direct_placeholder_users`, `room_param_key`,
 * `user_summary`; `Users::SidebarsController::DIRECT_PLACEHOLDERS`).
 *
 * One read transaction over ctx->reader; rendering afterwards performs no
 * SQL.  Rows come from D01's frozen model functions where they exist; the
 * placeholder query (the reference's one runtime-built IN list plus LIMIT) is
 * written here as a fixed statement with the id list bound as JSON, exactly
 * like `User::where_ids`.
 *
 * Faces: the reference's fragment cache does not exist in C yet, so a direct
 * membership is always the view arm (the A02 message-item precedent).
 * Avatars are `fresh_user_avatar_path` (the pinned source never maps an
 * attached avatar to a variant here; S02's boundary, disclosed in the
 * evidence).
 */
#include "views.h"
#include "views/internal.h"

#include "app.h"
#include "auth.h"
#include "cable/channels.h"
#include "config.h"
#include "db/db_internal.h"
#include "models/account.h"
#include "models/membership.h"
#include "models/room.h"
#include "models/user.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- owned text ----------------------------------------------------------- */

static cf_err str_dup(cf_span span, cf_str *out) {
    if (out == NULL) return CF_INVALID;
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

static cf_err str_set(cf_str *out, const char *text) {
    return str_dup((cf_span){(const unsigned char *)text, strlen(text)}, out);
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

/* `fresh_user_avatar_path(user)`: signed avatar token + updated_at number. */
static cf_err sidebar_avatar_url(const cf_ctx *ctx, int64_t user_id,
                                 int64_t updated_at_us, cf_str *out) {
    memset(out, 0, sizeof *out);
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->secret_key_base == NULL) return CF_OK;
    cf_str token = {0};
    cf_err rc = cf_auth_signed_id_generate(
        (cf_span){(const unsigned char *)config->secret_key_base,
                  config->secret_key_base_len},
        (cf_span){(const unsigned char *)"User", 4}, user_id,
        (cf_span){(const unsigned char *)"avatar", 6}, true, false, 0, &token);
    if (rc != CF_OK) return rc;
    char number[16];
    to_fs_number(updated_at_us, number);
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

/* ---- vectors -------------------------------------------------------------- */

static cf_err sidebar_user_push(cf_view_sidebar_user_vector *vector,
                                cf_view_sidebar_user *item) {
    if (vector->len == vector->cap) {
        size_t cap = vector->cap != 0 ? vector->cap * 2 : 4;
        if (cap < vector->cap || cap > SIZE_MAX / sizeof *vector->items) {
            return CF_LIMIT;
        }
        cf_view_sidebar_user *grown = realloc(vector->items,
                                              cap * sizeof *vector->items);
        if (grown == NULL) return CF_NOMEM;
        vector->items = grown;
        vector->cap = cap;
    }
    vector->items[vector->len++] = *item;
    memset(item, 0, sizeof *item);
    return CF_OK;
}

static cf_err sidebar_direct_push(cf_view_sidebar_direct_vector *vector,
                                  cf_view_sidebar_direct *item) {
    if (vector->len == vector->cap) {
        size_t cap = vector->cap != 0 ? vector->cap * 2 : 4;
        if (cap < vector->cap || cap > SIZE_MAX / sizeof *vector->items) {
            return CF_LIMIT;
        }
        cf_view_sidebar_direct *grown =
            realloc(vector->items, cap * sizeof *vector->items);
        if (grown == NULL) return CF_NOMEM;
        vector->items = grown;
        vector->cap = cap;
    }
    vector->items[vector->len++] = *item;
    memset(item, 0, sizeof *item);
    return CF_OK;
}

static cf_err sidebar_room_push(cf_view_sidebar_room_vector *vector,
                                cf_view_sidebar_room *item) {
    if (vector->len == vector->cap) {
        size_t cap = vector->cap != 0 ? vector->cap * 2 : 4;
        if (cap < vector->cap || cap > SIZE_MAX / sizeof *vector->items) {
            return CF_LIMIT;
        }
        cf_view_sidebar_room *grown =
            realloc(vector->items, cap * sizeof *vector->items);
        if (grown == NULL) return CF_NOMEM;
        vector->items = grown;
        vector->cap = cap;
    }
    vector->items[vector->len++] = *item;
    memset(item, 0, sizeof *item);
    return CF_OK;
}

void cf_view_sidebar_user_dispose(cf_view_sidebar_user *user) {
    if (user == NULL) return;
    cf_str_dispose(&user->name);
    cf_str_dispose(&user->avatar_path);
    memset(user, 0, sizeof *user);
}

void cf_view_sidebar_user_vector_dispose(cf_view_sidebar_user_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len; i++) {
        cf_view_sidebar_user_dispose(&vector->items[i]);
    }
    free(vector->items);
    memset(vector, 0, sizeof *vector);
}

void cf_view_sidebar_direct_dispose(cf_view_sidebar_direct *membership) {
    if (membership == NULL) return;
    cf_str_dispose(&membership->updated_at_epoch);
    cf_view_sidebar_user_vector_dispose(&membership->members);
    memset(membership, 0, sizeof *membership);
}

void cf_view_sidebar_direct_vector_dispose(
    cf_view_sidebar_direct_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len; i++) {
        cf_view_sidebar_direct_dispose(&vector->items[i]);
    }
    free(vector->items);
    memset(vector, 0, sizeof *vector);
}

void cf_view_sidebar_room_dispose(cf_view_sidebar_room *room) {
    if (room == NULL) return;
    cf_str_dispose(&room->param_key);
    cf_str_dispose(&room->name);
    memset(room, 0, sizeof *room);
}

void cf_view_sidebar_room_vector_dispose(cf_view_sidebar_room_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len; i++) {
        cf_view_sidebar_room_dispose(&vector->items[i]);
    }
    free(vector->items);
    memset(vector, 0, sizeof *vector);
}

void cf_view_sidebar_model_dispose(cf_view_sidebar_model *model) {
    if (model == NULL) return;
    cf_view_sidebar_user_dispose(&model->current_user);
    cf_str_dispose(&model->rooms_stream);
    cf_str_dispose(&model->user_rooms_stream);
    cf_view_sidebar_direct_vector_dispose(&model->direct_memberships);
    cf_view_sidebar_user_vector_dispose(&model->direct_placeholder_users);
    cf_view_sidebar_room_vector_dispose(&model->other_memberships);
    memset(model, 0, sizeof *model);
}

/* ---- row -> view ---------------------------------------------------------- */

static cf_err sidebar_user_from(cf_ctx *ctx, int64_t id, cf_span name,
                                int64_t updated_at_us,
                                cf_view_sidebar_user *out) {
    memset(out, 0, sizeof *out);
    out->id = id;
    cf_err rc = str_dup(name, &out->name);
    if (rc != CF_OK) goto fail;
    rc = sidebar_avatar_url(ctx, id, updated_at_us, &out->avatar_path);
    if (rc != CF_OK) goto fail;
    return CF_OK;
fail:
    cf_view_sidebar_user_dispose(out);
    return rc;
}

static cf_err sidebar_user_from_row(cf_ctx *ctx, const cf_user *user,
                                    cf_view_sidebar_user *out) {
    return sidebar_user_from(
        ctx, user->id,
        (cf_span){(const unsigned char *)user->name.ptr, user->name.len},
        user->updated_at, out);
}

/* `room_param_key(room.room_type)`. */
static const char *sidebar_param_key(cf_room_type room_type) {
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

/* ---- the presenter -------------------------------------------------------- */

/* `users/sidebars/rooms/_direct` locals: `room.users.without(membership.user)
 * .presence || [ membership.user ]`. */
static cf_err sidebar_fill_direct(cf_ctx *ctx, const cf_membership *membership,
                                  const cf_room *room,
                                  cf_view_sidebar_direct *out) {
    memset(out, 0, sizeof *out);
    cf_err rc = CF_OK;
    out->room_id = room->id;
    out->unread = cf_membership_unread(membership);
    {
        char number[32];
        int n = snprintf(number, sizeof number, "%lld",
                         (long long)cf_view_epoch_ms(room->updated_at));
        if (n < 0 || (size_t)n >= sizeof number) return CF_INTERNAL;
        rc = str_set(&out->updated_at_epoch, number);
        if (rc != CF_OK) goto fail;
    }

    cf_user_vector users = {0};
    rc = cf_room_users(ctx->reader, room, &users);
    if (rc != CF_OK) goto fail;
    for (size_t i = 0; i < users.len; i++) {
        if (users.items[i].id == membership->user_id) continue;
        cf_view_sidebar_user item;
        rc = sidebar_user_from_row(ctx, &users.items[i], &item);
        if (rc == CF_OK) rc = sidebar_user_push(&out->members, &item);
        if (rc != CF_OK) {
            cf_view_sidebar_user_dispose(&item);
            cf_user_vector_dispose(&users);
            goto fail;
        }
    }
    cf_user_vector_dispose(&users);

    if (out->members.len == 0) {
        /* The membership's own user, when the room has no other member. */
        cf_user own = {0};
        bool found = false;
        rc = cf_user_find_by_id(ctx->reader, membership->user_id, &found, &own);
        if (rc != CF_OK) goto fail;
        if (!found) {
            cf_user_dispose(&own);
            rc = CF_NOT_FOUND;
            goto fail;
        }
        cf_view_sidebar_user item;
        rc = sidebar_user_from_row(ctx, &own, &item);
        cf_user_dispose(&own);
        if (rc == CF_OK) rc = sidebar_user_push(&out->members, &item);
        if (rc != CF_OK) {
            cf_view_sidebar_user_dispose(&item);
            goto fail;
        }
    }
    return CF_OK;
fail:
    cf_view_sidebar_direct_dispose(out);
    return rc;
}

/* The reference's `find_direct_placeholder_users`: every member of the
 * current user's direct rooms (uniq) plus the user's own id — the duplicate
 * append counts toward the limit — then active users outside that set,
 * oldest first, at most `DIRECT_PLACEHOLDERS - |exclude|`.
 *
 * The fixed statement's `LIMIT 20` is `CF_VIEW_SIDEBAR_DIRECT_PLACEHOLDERS`
 * (the reference puts the computed limit in the SQL; the effective limit is
 * never larger than that constant, so the result set is the same and the
 * truncation is done here instead of binding a LIMIT). */
enum { SIDEBAR_STMT_PLACEHOLDERS = 0, SIDEBAR_STMT_COUNT };

static const cf_stmt_def sidebar_stmt_defs[SIDEBAR_STMT_COUNT] = {
    [SIDEBAR_STMT_PLACEHOLDERS] = {
        "SELECT \"users\".\"id\", \"users\".\"name\", \"users\".\"updated_at\" "
        "FROM \"users\" WHERE \"users\".\"status\" = 0 "
        "AND \"users\".\"id\" NOT IN (SELECT value FROM json_each(?)) "
        "ORDER BY \"users\".\"created_at\" ASC LIMIT 20"},
};

static const cf_stmt_set sidebar_stmt_set = {
    sidebar_stmt_defs, sizeof sidebar_stmt_defs / sizeof sidebar_stmt_defs[0]};

static bool sidebar_ids_contain(const int64_t *ids, size_t len, int64_t id) {
    for (size_t i = 0; i < len; i++) {
        if (ids[i] == id) return true;
    }
    return false;
}

static cf_err sidebar_placeholders(cf_ctx *ctx, const cf_user *user,
                                   cf_view_sidebar_user_vector *out) {
    bool ok = false;
    int64_t *exclude = NULL;
    size_t exclude_len = 0, exclude_cap = 0;
    cf_room_vector rooms = {0};
    cf_err rc = CF_OK;

    rc = cf_room_for_user_of_type(ctx->reader, user->id, CF_ROOM_DIRECT, &rooms);
    if (rc != CF_OK) goto done;

    for (size_t i = 0; i < rooms.len; i++) {
        cf_membership_vector memberships = {0};
        rc = cf_membership_for_room(ctx->reader, rooms.items[i].id,
                                    &memberships);
        if (rc != CF_OK) {
            cf_membership_vector_dispose(&memberships);
            goto done;
        }
        for (size_t m = 0; m < memberships.len; m++) {
            int64_t id = memberships.items[m].user_id;
            if (sidebar_ids_contain(exclude, exclude_len, id)) continue;
            if (exclude_len == exclude_cap) {
                size_t cap = exclude_cap != 0 ? exclude_cap * 2 : 8;
                if (cap > SIZE_MAX / sizeof *exclude) {
                    rc = CF_LIMIT;
                    cf_membership_vector_dispose(&memberships);
                    goto done;
                }
                int64_t *grown = realloc(exclude, cap * sizeof *exclude);
                if (grown == NULL) {
                    rc = CF_NOMEM;
                    cf_membership_vector_dispose(&memberships);
                    goto done;
                }
                exclude = grown;
                exclude_cap = cap;
            }
            exclude[exclude_len++] = id;
        }
        cf_membership_vector_dispose(&memberships);
    }

    /* `exclude_user_ids.push(user.id)`: always appended, duplicate included,
     * and the limit counts the appended length. */
    if (exclude_len == exclude_cap) {
        size_t cap = exclude_cap != 0 ? exclude_cap * 2 : 8;
        if (cap > SIZE_MAX / sizeof *exclude) {
            rc = CF_LIMIT;
            goto done;
        }
        int64_t *grown = realloc(exclude, cap * sizeof *exclude);
        if (grown == NULL) {
            rc = CF_NOMEM;
            goto done;
        }
        exclude = grown;
        exclude_cap = cap;
    }
    exclude[exclude_len++] = user->id;

    int64_t limit = CF_VIEW_SIDEBAR_DIRECT_PLACEHOLDERS - (int64_t)exclude_len;
    if (limit < 0) limit = 0;
    if (limit == 0) {
        ok = true;
        goto done;
    }

    cf_builder json = {0};
    rc = cf_view_str(&json, "[");
    for (size_t i = 0; rc == CF_OK && i < exclude_len; i++) {
        char number[32];
        int n = snprintf(number, sizeof number, "%s%lld", i == 0 ? "" : ",",
                         (long long)exclude[i]);
        if (n < 0 || (size_t)n >= sizeof number) {
            rc = CF_INTERNAL;
            break;
        }
        rc = cf_view_str(&json, number);
    }
    if (rc == CF_OK) rc = cf_view_str(&json, "]");
    if (rc != CF_OK) {
        cf_builder_dispose(&json);
        goto done;
    }

    sqlite3_stmt *stmt = NULL;
    rc = cf_db_stmt(ctx->reader, &sidebar_stmt_set, SIDEBAR_STMT_PLACEHOLDERS,
                    &stmt);
    if (rc == CF_OK) {
        rc = cf_stmt_bind_text(stmt, 1, cf_view_span_of(&json));
    }
    int64_t produced = 0;
    while (rc == CF_OK && produced < limit) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            rc = cf_db_err(step);
            break;
        }
        cf_view_sidebar_user item;
        {
            int64_t updated_at = 0;
            rc = cf_db_time_from_text(cf_stmt_column_text(stmt, 2), &updated_at);
            if (rc == CF_OK) {
                rc = sidebar_user_from(
                    ctx, cf_stmt_column_i64(stmt, 0),
                    cf_stmt_column_text(stmt, 1), updated_at, &item);
            }
        }
        if (rc == CF_OK) rc = sidebar_user_push(out, &item);
        if (rc != CF_OK) cf_view_sidebar_user_dispose(&item);
        produced++;
    }
    cf_db_stmt_done(stmt);
    cf_builder_dispose(&json);
    if (rc != CF_OK) goto done;

    ok = true;
done:
    free(exclude);
    cf_room_vector_dispose(&rooms);
    return ok ? CF_OK : rc;
}

cf_err cf_presenter_sidebar(cf_ctx *ctx, const cf_user *user,
                            cf_view_sidebar_model *out) {
    if (ctx == NULL || user == NULL || out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);

    cf_err rc = cf_read_begin(ctx->reader);
    if (rc != CF_OK) return rc;

    rc = sidebar_user_from_row(ctx, user, &out->current_user);
    if (rc != CF_OK) goto fail;

    /* `Membership::visible_with_ordered_room`: the direct rooms are the same
     * list filtered by `room.direct?`, sorted by `room.updated_at` (stable)
     * and reversed; the rest keep the query's LOWER(name) order. */
    cf_membership_room_pair_vector pairs = {0};
    rc = cf_membership_visible_with_ordered_room(ctx->reader, user->id, &pairs);
    if (rc != CF_OK) goto fail;

    size_t direct_count = 0;
    for (size_t i = 0; i < pairs.len; i++) {
        if (cf_room_direct(&pairs.items[i].room)) direct_count++;
    }
    size_t *order = NULL;
    int64_t *direct_ids = NULL;
    if (direct_count != 0) {
        order = malloc(direct_count * sizeof *order);
        direct_ids = malloc(direct_count * sizeof *direct_ids);
        if (order == NULL || direct_ids == NULL) {
            free(order);
            free(direct_ids);
            cf_membership_room_pair_vector_dispose(&pairs);
            rc = CF_NOMEM;
            goto fail;
        }
        size_t at = 0;
        for (size_t i = 0; i < pairs.len; i++) {
            if (cf_room_direct(&pairs.items[i].room)) {
                direct_ids[at] = pairs.items[i].membership.id;
                order[at] = i;
                at++;
            }
        }
        /* Stable insertion sort by updated_at ascending, then reverse. */
        for (size_t i = 1; i < direct_count; i++) {
            size_t key = order[i];
            int64_t key_at = pairs.items[key].room.updated_at;
            size_t j = i;
            while (j > 0 &&
                   pairs.items[order[j - 1]].room.updated_at > key_at) {
                order[j] = order[j - 1];
                j--;
            }
            order[j] = key;
        }
    }

    for (size_t n = direct_count; n > 0; n--) {
        const cf_membership_room_pair *pair = &pairs.items[order[n - 1]];
        cf_view_sidebar_direct item;
        rc = sidebar_fill_direct(ctx, &pair->membership, &pair->room, &item);
        if (rc == CF_OK) rc = sidebar_direct_push(&out->direct_memberships,
                                                  &item);
        if (rc != CF_OK) {
            cf_view_sidebar_direct_dispose(&item);
            free(order);
            free(direct_ids);
            cf_membership_room_pair_vector_dispose(&pairs);
            goto fail;
        }
    }
    free(order);

    for (size_t i = 0; i < pairs.len; i++) {
        const cf_membership_room_pair *pair = &pairs.items[i];
        if (sidebar_ids_contain(direct_ids, direct_count, pair->membership.id)) {
            continue;
        }
        cf_view_sidebar_room item;
        memset(&item, 0, sizeof item);
        item.id = pair->room.id;
        item.unread = cf_membership_unread(&pair->membership);
        rc = str_set(&item.param_key,
                     sidebar_param_key(pair->room.room_type));
        if (rc == CF_OK) {
            rc = str_dup(
                (cf_span){(const unsigned char *)(pair->room.name.present
                                                      ? pair->room.name.value.ptr
                                                      : NULL),
                          pair->room.name.present ? pair->room.name.value.len
                                                  : 0},
                &item.name);
        }
        if (rc == CF_OK) rc = sidebar_room_push(&out->other_memberships, &item);
        if (rc != CF_OK) {
            cf_view_sidebar_room_dispose(&item);
            free(direct_ids);
            cf_membership_room_pair_vector_dispose(&pairs);
            goto fail;
        }
    }
    free(direct_ids);
    cf_membership_room_pair_vector_dispose(&pairs);

    rc = sidebar_placeholders(ctx, user, &out->direct_placeholder_users);
    if (rc != CF_OK) goto fail;

    /* `Current.user.administrator? ||
     * !Current.account.settings.restrict_room_creation_to_administrators?`. */
    {
        cf_account account = {0};
        bool found = false;
        rc = cf_account_first(ctx->reader, &found, &account);
        if (rc != CF_OK) {
            cf_account_dispose(&account);
            goto fail;
        }
        bool restricted = false;
        if (found) {
            cf_account_settings settings = {0};
            rc = cf_account_settings_of(&account, &settings);
            if (rc == CF_OK) {
                restricted =
                    cf_account_settings_restrict_room_creation_to_administrators(
                        &settings);
            }
            cf_account_settings_dispose(&settings);
        }
        cf_account_dispose(&account);
        if (rc != CF_OK) goto fail;
        out->can_create_rooms = cf_user_is_administrator(user) || !restricted;
    }

    /* `turbo_stream_from :rooms` / `turbo_stream_from Current.user, :rooms`. */
    {
        const cf_config *config = cf_app_config(ctx->app);
        if (config == NULL || config->secret_key_base == NULL) {
            rc = CF_INVALID;
            goto fail;
        }
        cf_span secret = {
            (const unsigned char *)config->secret_key_base,
            config->secret_key_base_len};
        cf_span rooms_part = {(const unsigned char *)"rooms", 5};
        rc = cf_auth_turbo_signed_stream_name(secret, &rooms_part, 1,
                                              &out->rooms_stream);
        if (rc != CF_OK) goto fail;

        cf_str gid = {0};
        rc = cf_cable_user_gid_param(user->id, &gid);
        if (rc == CF_OK) {
            cf_span parts[2] = {
                {(const unsigned char *)gid.ptr, gid.len}, rooms_part};
            rc = cf_auth_turbo_signed_stream_name(secret, parts, 2,
                                                  &out->user_rooms_stream);
        }
        cf_str_dispose(&gid);
        if (rc != CF_OK) goto fail;
    }

    rc = cf_read_end(ctx->reader);
    if (rc != CF_OK) {
        cf_view_sidebar_model_dispose(out);
        return rc;
    }
    return CF_OK;
fail:
    {
        cf_err end_rc = cf_read_end(ctx->reader);
        (void)end_rc;
    }
    cf_view_sidebar_model_dispose(out);
    return rc;
}
