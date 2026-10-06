/* src/presenters/accounts.c — the account/session presenters the foundation
 * families use (tmp/rust-ref/crates/campfire/src/controllers/presenters/
 * accounts.rs: help_contact, no_users) plus the accounts Edit presenter
 * (controllers/accounts.rs `edit`: account_users, the administrators-first
 * partition, user_summary and the geared page).
 *
 * Queries the model layer does not offer are written here against the
 * reference's SQL, through D01's fixed per-module statement cache (no SQL is
 * assembled at runtime).
 *
 * Rows are read inside one cf_read_begin/cf_read_end transaction; rendering
 * afterwards performs no SQL.
 */
#include "presenters/accounts.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "db/db_internal.h"
#include "models/account.h"
#include "models/room.h"
#include "models/user.h"
#include "views/internal.h" /* cf_views_integer_cast, cf_str_span */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Owned NUL-terminated copy of a span (the model layer's cf_str contract). */
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

enum { PRESENTER_STMT_HELP_CONTACT };

static const cf_stmt_def presenter_stmt_defs[] = {
    [PRESENTER_STMT_HELP_CONTACT] = {
        "SELECT \"users\".\"name\", \"users\".\"email_address\" "
        "FROM \"users\" WHERE \"users\".\"role\" = 1 "
        "ORDER BY \"users\".\"id\" ASC LIMIT 1"},
};

static const cf_stmt_set presenter_stmt_set = {
    presenter_stmt_defs,
    sizeof presenter_stmt_defs / sizeof presenter_stmt_defs[0]};

cf_err cf_presenter_help_contact(cf_db *db, bool *found,
                                 cf_view_help_contact *out) {
    if (db == NULL || found == NULL || out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    *found = false;

    cf_err rc = cf_read_begin(db);
    if (rc != CF_OK) return rc;
    sqlite3_stmt *stmt = NULL;
    rc = cf_db_stmt(db, &presenter_stmt_set, PRESENTER_STMT_HELP_CONTACT,
                    &stmt);
    if (rc != CF_OK) goto done;
    int step = sqlite3_step(stmt);
    if (step == SQLITE_DONE) {
        rc = CF_OK;
        goto done;
    }
    if (step != SQLITE_ROW) {
        rc = cf_db_err(step);
        goto done;
    }
    {
        cf_span name = cf_stmt_column_text(stmt, 0);
        rc = str_dup(name, &out->name);
        if (rc != CF_OK) goto done;
        if (cf_stmt_column_is_null(stmt, 1)) {
            out->email_address.ptr = malloc(1);
            if (out->email_address.ptr == NULL) {
                rc = CF_NOMEM;
                goto done;
            }
            out->email_address.ptr[0] = '\0';
            out->email_address.len = 0;
        } else {
            rc = str_dup(cf_stmt_column_text(stmt, 1),
                         &out->email_address);
            if (rc != CF_OK) goto done;
        }
        *found = true;
    }
done:
    if (stmt != NULL) cf_db_stmt_done(stmt);
    cf_err end_rc = cf_read_end(db);
    if (rc == CF_OK) rc = end_rc;
    if (rc != CF_OK && *found) {
        cf_view_help_contact_dispose(out);
        *found = false;
    }
    return rc;
}

cf_err cf_presenter_no_users(cf_db *db, bool *out) {
    if (db == NULL || out == NULL) return CF_INVALID;
    cf_err rc = cf_read_begin(db);
    if (rc != CF_OK) return rc;
    int64_t count = 0;
    rc = cf_user_count(db, &count);
    cf_err end_rc = cf_read_end(db);
    if (rc == CF_OK) rc = end_rc;
    if (rc == CF_OK) *out = count == 0;
    return rc;
}

/* ---- AccountsController#edit ---------------------------------------------- */

static cf_span accounts_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `Time#to_fs(:number)`: %Y%m%d%H%M%S in UTC (borrowed from
 * src/presenters/bots.c; the fresh avatar path's `v` param). */
static void accounts_to_fs_number(int64_t us, char out[16]) {
    time_t seconds = (time_t)(us / 1000000);
    if (us < 0 && us % 1000000 != 0) seconds -= 1;
    struct tm tm;
    if (gmtime_r(&seconds, &tm) == NULL) {
        memcpy(out, "00000000000000", 15);
        return;
    }
    strftime(out, 16, "%Y%m%d%H%M%S", &tm);
}

/* `fresh_user_avatar_path(user)`: "/users/<avatar token>/avatar?v=<number>"
 * (borrowed from src/presenters/bots.c).  An unconfigured secret key base
 * leaves the path empty, like the layout's user_avatar_url. */
static cf_err accounts_avatar_path(cf_ctx *ctx, const cf_user *user,
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
    accounts_to_fs_number(user->updated_at, number);
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

/* ASCII-only lowercase compare (SQLite LOWER is ASCII-only). */
static int accounts_name_cmp(const cf_user *a, const cf_user *b) {
    size_t i = 0;
    for (;;) {
        unsigned char ca = i < a->name.len ? (unsigned char)a->name.ptr[i] : 0;
        unsigned char cb = i < b->name.len ? (unsigned char)b->name.ptr[i] : 0;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return ca < cb ? -1 : 1;
        if (ca == 0) break;
        i++;
    }
    if (a->id != b->id) return a->id < b->id ? -1 : 1;
    return 0;
}

static int accounts_name_qsort(const void *pa, const void *pb) {
    return accounts_name_cmp(pa, pb);
}

/* `presenters::accounts::account_users`: active+banned for administrators,
 * active only otherwise; bots dropped; ORDER BY LOWER(name).  The
 * administrator arm filters cf_user_all in memory (SHIM moved verbatim from
 * src/actions/accounts.c: a D01 account_users query would replace the
 * filter; it selects/sorts the same rows the reference SQL does, so it
 * changes no rendered output). */
static cf_err accounts_account_users(cf_ctx *ctx, bool can_admin,
                                     cf_user_vector *out) {
    memset(out, 0, sizeof *out);
    if (!can_admin) {
        return cf_user_active_ordered_without_bots(ctx->reader, out);
    }
    cf_user_vector all = {0};
    cf_err rc = cf_user_all(ctx->reader, &all);
    if (rc != CF_OK) return rc;
    cf_user_vector kept = {0};
    for (size_t i = 0; i < all.len && rc == CF_OK; i++) {
        const cf_user *user = &all.items[i];
        bool status_ok = user->status == CF_STATUS_ACTIVE ||
                         user->status == CF_STATUS_BANNED;
        if (!status_ok || user->role == CF_ROLE_BOT) continue;
        if (kept.len == kept.cap) {
            size_t cap = kept.cap == 0 ? 16 : kept.cap * 2;
            if (cap < kept.cap) {
                rc = CF_NOMEM;
                break;
            }
            cf_user *items = realloc(kept.items, cap * sizeof *items);
            if (items == NULL) {
                rc = CF_NOMEM;
                break;
            }
            kept.items = items;
            kept.cap = cap;
        }
        kept.items[kept.len] = *user;
        memset(&all.items[i], 0, sizeof all.items[i]);
        kept.len++;
    }
    cf_user_vector_dispose(&all);
    if (rc != CF_OK) {
        cf_user_vector_dispose(&kept);
        return rc;
    }
    qsort(kept.items, kept.len, sizeof *kept.items, accounts_name_qsort);
    *out = kept;
    return CF_OK;
}

/* `c.param_str(key)`: Some only for a string param. */
static bool accounts_param_str(cf_ctx *ctx, const char *name, cf_span *out) {
    const cf_param *param = cf_ctx_param(ctx, accounts_span(name));
    if (param == NULL || cf_param_type(param) != CF_PARAM_STRING) return false;
    return cf_param_string(param, out) == CF_OK;
}

/* `Page::number`: `param_str("page")` through ruby_compat::integer_cast,
 * clamped to [1, 1_000_000_000] (pagination.rs page_number_from). */
#define ACCOUNTS_PER_PAGE INT64_C(500)
#define ACCOUNTS_MAX_PAGE INT64_C(1000000000)

static int64_t accounts_page_number(cf_ctx *ctx) {
    cf_span text = {NULL, 0};
    int64_t number = 0;
    if (!accounts_param_str(ctx, "page", &text)) return 1;
    if (!cf_views_integer_cast(text, &number)) return 1;
    if (number < 1) return 1;
    if (number > ACCOUNTS_MAX_PAGE) return ACCOUNTS_MAX_PAGE;
    return number;
}

/* `concerns::last_room_visited_in`: the `last_room` cookie's room when the
 * user is a member of it, else the user's original room; found=false leaves
 * link_back_to_last_room_visited at root (borrowed from
 * src/presenters/push_subscriptions.c).  The caller holds the read
 * transaction. */
static cf_err accounts_last_room_visited(cf_ctx *ctx, int64_t user_id,
                                         bool *found, int64_t *room_id) {
    *found = false;
    *room_id = 0;
    cf_span cookie = {NULL, 0};
    cf_err rc = cf_ctx_cookie_get(ctx, accounts_span("last_room"), &cookie);
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

cf_err cf_presenter_accounts_edit(cf_ctx *ctx, const cf_account *account,
                                  const cf_user *current_user,
                                  bool can_administer,
                                  cf_view_accounts_edit_model *out) {
    if (ctx == NULL || account == NULL || current_user == NULL || out == NULL) {
        return CF_INVALID;
    }
    memset(out, 0, sizeof *out);

    out->account_id = account->id;
    cf_err rc = str_dup(
        (cf_span){(const unsigned char *)account->join_code.ptr,
                  account->join_code.len},
        &out->join_code);
    if (rc != CF_OK) {
        cf_view_accounts_edit_model_dispose(out);
        return rc;
    }
    cf_account_settings settings = {0};
    rc = cf_account_settings_of(account, &settings);
    if (rc == CF_OK) {
        out->restrict_room_creation_to_administrators =
            cf_account_settings_restrict_room_creation_to_administrators(
                &settings);
    }
    cf_account_settings_dispose(&settings);
    if (rc != CF_OK) {
        cf_view_accounts_edit_model_dispose(out);
        return rc;
    }

    rc = cf_read_begin(ctx->reader);
    if (rc != CF_OK) {
        cf_view_accounts_edit_model_dispose(out);
        return rc;
    }

    /* `account_users(can_administer)` sorted LOWER(name), each row mapped
     * through `user_summary` and partitioned administrators-first (the
     * reference `.partition` preserves the sorted order within each side). */
    cf_user_vector users = {0};
    rc = accounts_account_users(ctx, can_administer, &users);
    for (size_t i = 0; rc == CF_OK && i < users.len; i++) {
        const cf_user *row = &users.items[i];
        cf_view_account_user_vector *side =
            row->role == CF_ROLE_ADMINISTRATOR ? &out->administrators
                                               : &out->members;
        cf_str title = {0};
        cf_str avatar = {0};
        rc = cf_user_title(row, &title);
        if (rc == CF_OK) rc = accounts_avatar_path(ctx, row, &avatar);
        if (rc == CF_OK) {
            rc = cf_view_account_user_vector_push(
                side, row->id,
                (cf_span){(const unsigned char *)row->name.ptr,
                          row->name.len},
                cf_str_span(title), cf_str_span(avatar), row->role,
                row->status);
        }
        cf_str_dispose(&title);
        cf_str_dispose(&avatar);
    }
    cf_user_vector_dispose(&users);
    if (rc != CF_OK) goto fail;

    rc = accounts_last_room_visited(ctx, current_user->id,
                                    &out->has_last_room_visited,
                                    &out->last_room_visited_id);
    if (rc != CF_OK) goto fail;

    /* `Page::new(page, users.len(), [500])`: `next_page` only when the
     * requested page is not the last. */
    {
        int64_t total = (int64_t)(out->administrators.len + out->members.len);
        int64_t number = accounts_page_number(ctx);
        int64_t residual = total;
        int64_t pages = 0;
        while (residual > 0) {
            pages++;
            residual -= ACCOUNTS_PER_PAGE;
        }
        if (pages < 1) pages = 1;
        if (number != pages) {
            char nextbuf[32];
            int n = snprintf(nextbuf, sizeof nextbuf, "%" PRId64, number + 1);
            if (n < 0 || (size_t)n >= sizeof nextbuf) {
                rc = CF_INTERNAL;
                goto fail;
            }
            rc = str_dup(accounts_span(nextbuf), &out->next_page);
            if (rc != CF_OK) goto fail;
            out->has_next_page = true;
        }
    }

    rc = cf_read_end(ctx->reader);
    if (rc != CF_OK) {
        cf_view_accounts_edit_model_dispose(out);
        return rc;
    }
    return CF_OK;
fail:
    {
        cf_err end_rc = cf_read_end(ctx->reader);
        (void)end_rc;
    }
    cf_view_accounts_edit_model_dispose(out);
    return rc;
}
