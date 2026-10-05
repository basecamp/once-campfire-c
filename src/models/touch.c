/* src/models/touch.c — shared record-touch helper (packet V-G).
 *
 * Single implementation of the users.updated_at bump inlined at
 * actions/users/avatars_destroy.c:72 and actions/users/profiles.c:412 and
 * requested by actions/accounts/bots.c R-BOTS-TOUCH. One fixed statement,
 * no SQL assembled at runtime:
 *
 *   UPDATE "users" SET "updated_at" = ? WHERE "users"."id" = ?
 *
 * with the reference UTC SQL text at 1 (cf_db_time_to_text, the same form
 * user.c's per-column updates bind) and the id at 2. The record form sets
 * the in-memory updated_at to the same `now`, mirroring cf_message_touch's
 * touch_row contract.
 */
#include "models/touch.h"

#include "db/db_internal.h"

#include <sqlite3.h>
#include <string.h>

enum {
    CF_TOUCH_STMT_TOUCH_USER,
    CF_TOUCH_STMT_COUNT
};

static const cf_stmt_def touch_stmt_defs[CF_TOUCH_STMT_COUNT] = {
    [CF_TOUCH_STMT_TOUCH_USER] = {
        "UPDATE \"users\" SET \"updated_at\" = ? WHERE "
        "\"users\".\"id\" = ?"},
};

static const cf_stmt_set touch_stmts = {touch_stmt_defs,
                                         CF_TOUCH_STMT_COUNT};

/* The one implementation: stamp the row, reporting the `now` used. */
static cf_err touch_user_row(cf_tx *tx, int64_t user_id, int64_t *out_now) {
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) {
        return cf_db_failf(CF_INVALID, "touch user: no database");
    }
    int64_t now = cf_now_us(NULL);
    char now_text[CF_DB_TIME_TEXT_CAP];
    cf_err rc = cf_db_time_to_text(now, now_text);
    if (rc != CF_OK) return rc;

    sqlite3_stmt *stmt = NULL;
    rc = cf_db_stmt(db, &touch_stmts, CF_TOUCH_STMT_TOUCH_USER, &stmt);
    if (rc == CF_OK) {
        rc = cf_stmt_bind_text(stmt, 1,
                               (cf_span){(const unsigned char *)now_text,
                                         strlen(now_text)});
    }
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 2, user_id);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step != SQLITE_DONE) {
            rc = cf_db_failf(cf_db_err(step), "touch user failed: %s",
                             sqlite3_errmsg(cf_db_handle(db)));
        }
    }
    cf_db_stmt_done(stmt);
    if (rc != CF_OK) return rc;
    *out_now = now;
    return CF_OK;
}

cf_err cf_touch_user_id(cf_tx *tx, int64_t user_id) {
    if (tx == NULL) {
        return cf_db_failf(CF_INVALID, "touch user: no transaction");
    }
    int64_t now = 0;
    return touch_user_row(tx, user_id, &now);
}

cf_err cf_touch_user(cf_tx *tx, cf_user *user) {
    if (tx == NULL || user == NULL) {
        return cf_db_failf(CF_INVALID, "touch user: no transaction or user");
    }
    int64_t now = 0;
    cf_err rc = touch_user_row(tx, user->id, &now);
    if (rc != CF_OK) return rc;
    user->updated_at = now;
    return CF_OK;
}
