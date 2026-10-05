/* src/models/boost.c — D01 model family "boost".
 *
 * Source: tmp/rust-ref/crates/db/src/models/boost.rs (pinned; SHA-256 in
 * docs/devel/implementation/contracts/reference-files.json).
 *
 * Rules that survive translation (02-data-auth.md D01, boost row):
 *  - readers take cf_db *, mutations take cf_tx *;
 *  - fixed SQL only, in this module's own statement set (one cache per
 *    connection, D01 db-core conventions);
 *  - Option<T> reads: Boost::find and Boost::find_by_message_and_booster
 *    return CF_NOT_FOUND when absent (boost.h comments);
 *  - datetimes use the reference UTC SQL text on the wire and int64 UTC
 *    microseconds in memory; create stamps tx.now() via cf_now_us (tests
 *    inject the fixed clock through core/testclock.h);
 *  - create/destroy touch the message, which touches its room; delete_row is
 *    the delete alone, for a message being destroyed.
 *
 * The controller-level rules named for boost in 02-data-auth.md (creator
 * scope, content permit/nil handling, render and removal broadcasts) live in
 * tmp/rust-ref/crates/campfire/src/controllers/messages/boosts.rs, not in the
 * pinned db model; Boost::create itself performs no validation.
 */
#include "models/boost.h"

#include "db/db_internal.h"
#include "models/message.h"

#include <stdlib.h>
#include <string.h>

/* Fixed statement ids for this module's set. */
enum {
    BOOST_STMT_FIND = 0,
    BOOST_STMT_FOR_MESSAGE,
    BOOST_STMT_FOR_MESSAGE_ORDERED,
    BOOST_STMT_FIND_BY_MESSAGE_AND_BOOSTER,
    BOOST_STMT_INSERT,
    BOOST_STMT_DELETE,
    BOOST_STMT_COUNT
};

/* boost_columns!() expands to "boosts"."id", ..., in declaration order; the
 * SQL text below matches the reference concat! results byte for byte. */
static const cf_stmt_def boost_stmts[BOOST_STMT_COUNT] = {
    [BOOST_STMT_FIND] = {
        "SELECT \"boosts\".\"id\", \"boosts\".\"message_id\", \"boosts\".\"booster_id\", \"boosts\".\"content\", \"boosts\".\"created_at\", \"boosts\".\"updated_at\" FROM \"boosts\" WHERE \"boosts\".\"id\" = ? LIMIT 1"
    },
    [BOOST_STMT_FOR_MESSAGE] = {
        "SELECT \"boosts\".\"id\", \"boosts\".\"message_id\", \"boosts\".\"booster_id\", \"boosts\".\"content\", \"boosts\".\"created_at\", \"boosts\".\"updated_at\" FROM \"boosts\" WHERE \"boosts\".\"message_id\" = ?"
    },
    [BOOST_STMT_FOR_MESSAGE_ORDERED] = {
        "SELECT \"boosts\".\"id\", \"boosts\".\"message_id\", \"boosts\".\"booster_id\", \"boosts\".\"content\", \"boosts\".\"created_at\", \"boosts\".\"updated_at\" FROM \"boosts\" WHERE \"boosts\".\"message_id\" = ? ORDER BY \"boosts\".\"created_at\" ASC"
    },
    [BOOST_STMT_FIND_BY_MESSAGE_AND_BOOSTER] = {
        "SELECT \"boosts\".\"id\", \"boosts\".\"message_id\", \"boosts\".\"booster_id\", \"boosts\".\"content\", \"boosts\".\"created_at\", \"boosts\".\"updated_at\" FROM \"boosts\" WHERE \"boosts\".\"message_id\" = ? AND \"boosts\".\"id\" = ? AND \"boosts\".\"booster_id\" = ? LIMIT 1"
    },
    [BOOST_STMT_INSERT] = {
        "INSERT INTO \"boosts\" (\"booster_id\", \"content\", \"created_at\", \"message_id\", \"updated_at\") VALUES (?, ?, ?, ?, ?) RETURNING \"id\""
    },
    [BOOST_STMT_DELETE] = {
        "DELETE FROM \"boosts\" WHERE \"boosts\".\"id\" = ?"
    },
};

static const cf_stmt_set boost_stmt_set = { boost_stmts, BOOST_STMT_COUNT };

/* --- small ownership helpers --------------------------------------------- */

static cf_err copy_span_to_str(cf_span span, cf_str *out) {
    if (span.len == SIZE_MAX) return cf_db_failf(CF_NOMEM, "text too long");
    char *bytes = malloc(span.len + 1);
    if (bytes == NULL) return CF_NOMEM;
    if (span.len != 0) memcpy(bytes, span.ptr, span.len);
    bytes[span.len] = '\0';
    out->ptr = bytes;
    out->len = span.len;
    return CF_OK;
}

static cf_span span_of_text(const char *text) {
    cf_span span = { (const unsigned char *)text, strlen(text) };
    return span;
}

static cf_err boost_vector_push(cf_boost_vector *vector, cf_boost record) {
    if (vector->len == vector->cap) {
        size_t cap = vector->cap == 0 ? 4 : vector->cap * 2;
        if (cap < vector->cap || cap > SIZE_MAX / sizeof(cf_boost)) {
            return CF_LIMIT;
        }
        cf_boost *items = realloc(vector->items, cap * sizeof *items);
        if (items == NULL) return CF_NOMEM;
        vector->items = items;
        vector->cap = cap;
    }
    vector->items[vector->len++] = record;
    return CF_OK;
}

/* --- row decoding --------------------------------------------------------- */

/* Reads the six boost columns of the current row in reference order.  Builds
 * a private record so a failure leaves `out` untouched; the caller disposes a
 * record that was returned OK.  Column data is copied before any reset. */
static cf_err boost_read_row(sqlite3_stmt *stmt, cf_boost *out) {
    cf_boost record = {0};
    record.id = cf_stmt_column_i64(stmt, 0);
    record.message_id = cf_stmt_column_i64(stmt, 1);
    record.booster_id = cf_stmt_column_i64(stmt, 2);

    /* content is NOT NULL in the schema; NULL is a corrupt-row failure, not an
     * empty string (cf_stmt_column_copy_text keeps those distinct). */
    if (cf_stmt_column_is_null(stmt, 3)) {
        return cf_db_failf(CF_DB, "boosts.content is NULL");
    }
    cf_buf *content = NULL;
    cf_err rc = cf_stmt_column_copy_text(stmt, 3, &content);
    if (rc != CF_OK) return rc;
    rc = copy_span_to_str(cf_buf_span(content), &record.content);
    cf_buf_release(content);
    if (rc != CF_OK) return rc;

    int64_t *stamps[2] = { &record.created_at, &record.updated_at };
    for (int i = 0; i < 2; i++) {
        int column = 4 + i;
        if (cf_stmt_column_is_null(stmt, column)) {
            rc = cf_db_failf(CF_DB, "boosts.%s is NULL",
                             column == 4 ? "created_at" : "updated_at");
            break;
        }
        cf_buf *text = NULL;
        rc = cf_stmt_column_copy_text(stmt, column, &text);
        if (rc != CF_OK) break;
        rc = cf_db_time_from_text(cf_buf_span(text), stamps[i]);
        cf_buf_release(text);
        if (rc != CF_OK) break;
    }
    if (rc != CF_OK) {
        cf_boost_dispose(&record);
        return rc;
    }

    *out = record;
    return CF_OK;
}

/* --- disposal ------------------------------------------------------------- */

void cf_boost_dispose(cf_boost *boost) {
    if (boost == NULL) return;
    free(boost->content.ptr);
    *boost = (cf_boost){0};
}

void cf_boost_vector_dispose(cf_boost_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len; i++) {
        cf_boost_dispose(&vector->items[i]);
    }
    free(vector->items);
    *vector = (cf_boost_vector){0};
}

/* --- reads ---------------------------------------------------------------- */

cf_err cf_boost_find(cf_db *db, int64_t id, cf_boost *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "no output record");
    *out = (cf_boost){0};

    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &boost_stmt_set, BOOST_STMT_FIND, &stmt);
    if (rc != CF_OK) return rc;
    rc = cf_stmt_bind_i64(stmt, 1, id);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            rc = boost_read_row(stmt, out);
        } else if (step == SQLITE_DONE) {
            /* Error::RecordNotFound display: "Couldn't find Boost". */
            rc = cf_db_failf(CF_NOT_FOUND, "Couldn't find Boost");
        } else {
            rc = cf_db_failf(cf_db_err(step), "boost find failed: %s",
                             sqlite3_errmsg(cf_db_handle(db)));
        }
    }
    cf_db_stmt_done(stmt);
    if (rc != CF_OK) *out = (cf_boost){0};
    return rc;
}

/* for_message and for_message_ordered differ only by their fixed statement. */
static cf_err boost_for_message(cf_db *db, size_t stmt_id, int64_t message_id,
                                cf_boost_vector *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "no output vector");
    *out = (cf_boost_vector){0};

    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &boost_stmt_set, stmt_id, &stmt);
    if (rc != CF_OK) return rc;
    rc = cf_stmt_bind_i64(stmt, 1, message_id);
    while (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            rc = cf_db_failf(cf_db_err(step), "boost list failed: %s",
                             sqlite3_errmsg(cf_db_handle(db)));
            break;
        }
        cf_boost record = {0};
        rc = boost_read_row(stmt, &record);
        if (rc == CF_OK) rc = boost_vector_push(out, record);
        if (rc != CF_OK) cf_boost_dispose(&record);
    }
    cf_db_stmt_done(stmt);
    if (rc != CF_OK) cf_boost_vector_dispose(out);
    return rc;
}

cf_err cf_boost_for_message(cf_db *db, int64_t message_id,
                            cf_boost_vector *out) {
    return boost_for_message(db, BOOST_STMT_FOR_MESSAGE, message_id, out);
}

cf_err cf_boost_for_message_ordered(cf_db *db, int64_t message_id,
                                    cf_boost_vector *out) {
    return boost_for_message(db, BOOST_STMT_FOR_MESSAGE_ORDERED, message_id,
                             out);
}

cf_err cf_boost_find_by_message_and_booster(cf_db *db, int64_t message_id,
                                            int64_t id, int64_t booster_id,
                                            cf_boost *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "no output record");
    *out = (cf_boost){0};

    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &boost_stmt_set,
                           BOOST_STMT_FIND_BY_MESSAGE_AND_BOOSTER, &stmt);
    if (rc != CF_OK) return rc;
    rc = cf_stmt_bind_i64(stmt, 1, message_id);
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 2, id);
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 3, booster_id);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            rc = boost_read_row(stmt, out);
        } else if (step == SQLITE_DONE) {
            /* Error::RecordNotFound display: "Couldn't find Boost". */
            rc = cf_db_failf(CF_NOT_FOUND, "Couldn't find Boost");
        } else {
            rc = cf_db_failf(cf_db_err(step), "boost find failed: %s",
                             sqlite3_errmsg(cf_db_handle(db)));
        }
    }
    cf_db_stmt_done(stmt);
    if (rc != CF_OK) *out = (cf_boost){0};
    return rc;
}

/* --- mutations ------------------------------------------------------------ */

cf_err cf_boost_create(cf_tx *tx, int64_t message_id, int64_t booster_id,
                       cf_str content, cf_boost *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "no output record");
    *out = (cf_boost){0};
    if (tx == NULL) return cf_db_failf(CF_INVALID, "no transaction");
    if (content.len != 0 && content.ptr == NULL) {
        return cf_db_failf(CF_INVALID, "content has no bytes");
    }
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) {
        return cf_db_failf(CF_INVALID, "transaction has no connection");
    }

    int64_t now = cf_now_us(NULL);
    char now_text[CF_DB_TIME_TEXT_CAP];
    cf_err rc = cf_db_time_to_text(now, now_text);
    if (rc != CF_OK) return rc;

    sqlite3_stmt *stmt = NULL;
    rc = cf_db_stmt(db, &boost_stmt_set, BOOST_STMT_INSERT, &stmt);
    if (rc != CF_OK) return rc;
    cf_span content_span = { (const unsigned char *)content.ptr, content.len };
    rc = cf_stmt_bind_i64(stmt, 1, booster_id);
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 2, content_span);
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 3, span_of_text(now_text));
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 4, message_id);
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 5, span_of_text(now_text));
    int64_t id = 0;
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            id = cf_stmt_column_i64(stmt, 0);
        } else if (step == SQLITE_DONE) {
            rc = cf_db_failf(CF_DB, "boost insert returned no id");
        } else {
            rc = cf_db_failf(cf_db_err(step), "boost insert failed: %s",
                             sqlite3_errmsg(cf_db_handle(db)));
        }
    }
    cf_db_stmt_done(stmt);
    if (rc != CF_OK) return rc;

    /* `Message::find(tx.conn(), message_id)?.touch(tx)?` */
    cf_message message;
    rc = cf_message_find(db, message_id, &message);
    if (rc != CF_OK) return rc;
    rc = cf_message_touch(tx, &message);
    cf_message_dispose(&message);
    if (rc != CF_OK) return rc;

    cf_str stored = {0};
    rc = copy_span_to_str(content_span, &stored);
    if (rc != CF_OK) return rc;

    out->id = id;
    out->message_id = message_id;
    out->booster_id = booster_id;
    out->content = stored;
    out->created_at = now;
    out->updated_at = now;
    return CF_OK;
}

cf_err cf_boost_delete_row(cf_tx *tx, const cf_boost *boost) {
    if (boost == NULL) return cf_db_failf(CF_INVALID, "no boost");
    if (tx == NULL) return cf_db_failf(CF_INVALID, "no transaction");
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) {
        return cf_db_failf(CF_INVALID, "transaction has no connection");
    }

    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &boost_stmt_set, BOOST_STMT_DELETE, &stmt);
    if (rc != CF_OK) return rc;
    rc = cf_stmt_bind_i64(stmt, 1, boost->id);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        /* execute_cached ignores the affected-row count, so a missing row is
         * not an error here (Message.destroy deletes boosts it already read). */
        if (step != SQLITE_DONE) {
            rc = cf_db_failf(cf_db_err(step), "boost delete failed: %s",
                             sqlite3_errmsg(cf_db_handle(db)));
        }
    }
    cf_db_stmt_done(stmt);
    return rc;
}

cf_err cf_boost_destroy(cf_tx *tx, const cf_boost *boost) {
    if (boost == NULL) return cf_db_failf(CF_INVALID, "no boost");
    cf_err rc = cf_boost_delete_row(tx, boost);
    if (rc != CF_OK) return rc;

    cf_db *db = cf_tx_db(tx);
    cf_message message;
    rc = cf_message_find(db, boost->message_id, &message);
    if (rc != CF_OK) return rc;
    rc = cf_message_touch(tx, &message);
    cf_message_dispose(&message);
    return rc;
}
