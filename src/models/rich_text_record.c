/* src/models/rich_text_record.c — D01 model "rich_text_record".
 *
 * Translation of tmp/rust-ref/crates/db/src/models/rich_text_record.rs
 * (action_text_rich_texts).  Campfire stores Message#body here under
 * (record_type "Message", name "body"); the uniqueness index is
 * index_action_text_rich_texts_uniqueness(record_type, record_id, name).
 *
 * Reference mapping:
 *  - find_for   -> cf_rich_text_record_find_for   (read: cf_db *)
 *  - create     -> cf_rich_text_record_create     (mutation: cf_tx *)
 *  - update_body-> cf_rich_text_record_update_body(mutation: cf_tx *)
 *  - delete     -> cf_rich_text_record_delete     (mutation: cf_tx *)
 *
 * SQL text, parameter order, ordering and LIMIT 1 are copied from the source;
 * the SELECT column list follows the source `columns!` order (id, name, body,
 * record_type, record_id, created_at, updated_at).  The reference has no
 * ORDER BY (the uniqueness index makes the lookup at most one row).
 *
 * Datetimes: the reference stores Timestamp::to_db text ("YYYY-MM-DD
 * HH:MM:SS" plus ".ffffff" only when microseconds are non-zero) and keeps
 * microseconds in memory; this module uses db-core's cf_db_time_to_text /
 * cf_db_time_from_text.  `tx.now()` in the reference is the environment clock
 * read at call time, so mutations use cf_now_us(NULL) (the process clock,
 * injectable in tests through core/testclock.h).
 */
#include "models/rich_text_record.h"

#include "db/db_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fixed statements for this module, one prepared-statement cache per
 * connection (02-data-auth.md D01).  The enum values are the cache keys. */
enum {
    CF_RTR_STMT_FIND_FOR = 0,
    CF_RTR_STMT_CREATE,
    CF_RTR_STMT_UPDATE_BODY,
    CF_RTR_STMT_DELETE
};

static const cf_stmt_def cf_rtr_stmts[] = {
    /* Rust: RichTextRecord::find_for */
    {"SELECT \"action_text_rich_texts\".\"id\", "
     "\"action_text_rich_texts\".\"name\", "
     "\"action_text_rich_texts\".\"body\", "
     "\"action_text_rich_texts\".\"record_type\", "
     "\"action_text_rich_texts\".\"record_id\", "
     "\"action_text_rich_texts\".\"created_at\", "
     "\"action_text_rich_texts\".\"updated_at\" "
     "FROM \"action_text_rich_texts\" "
     "WHERE \"action_text_rich_texts\".\"record_id\" = ? "
     "AND \"action_text_rich_texts\".\"record_type\" = ? "
     "AND \"action_text_rich_texts\".\"name\" = ? LIMIT 1"},
    /* Rust: RichTextRecord::create */
    {"INSERT INTO \"action_text_rich_texts\" "
     "(\"body\", \"created_at\", \"name\", \"record_id\", \"record_type\", "
     "\"updated_at\") VALUES (?, ?, ?, ?, ?, ?) RETURNING \"id\""},
    /* Rust: RichTextRecord::update_body */
    {"UPDATE \"action_text_rich_texts\" SET \"body\" = ?, \"updated_at\" = ? "
     "WHERE \"action_text_rich_texts\".\"id\" = ?"},
    /* Rust: RichTextRecord::delete */
    {"DELETE FROM \"action_text_rich_texts\" "
     "WHERE \"action_text_rich_texts\".\"id\" = ?"}};

static const cf_stmt_set cf_rtr_stmt_set = {
    cf_rtr_stmts, sizeof cf_rtr_stmts / sizeof cf_rtr_stmts[0]};

/* Borrowed span over an owned cf_str argument. */
static cf_span rtr_span(cf_str text) {
    cf_span span;
    span.ptr = (const unsigned char *)text.ptr;
    span.len = text.len;
    return span;
}

/* Borrowed span over a NUL-terminated buffer (the fixed datetime text). */
static cf_span rtr_cstr_span(const char *text) {
    cf_span span;
    span.ptr = (const unsigned char *)text;
    span.len = strlen(text);
    return span;
}

/* Owned copy of a borrowed span: NUL-terminated, len excludes the NUL. */
static cf_err rtr_str_copy(cf_span src, cf_str *out) {
    out->ptr = NULL;
    out->len = 0;
    if (src.len != 0 && src.ptr == NULL) {
        return cf_db_failf(CF_INVALID, "text span has no bytes");
    }
    if (src.len == SIZE_MAX) {
        return cf_db_failf(CF_LIMIT, "text too long to copy");
    }
    char *copy = malloc(src.len + 1);
    if (copy == NULL) return CF_NOMEM;
    if (src.len != 0) memcpy(copy, src.ptr, src.len);
    copy[src.len] = '\0';
    out->ptr = copy;
    out->len = src.len;
    return CF_OK;
}

/* Owned copy of a copied SQLite column buffer; NULL remains absent. */
static cf_err rtr_str_from_buf(const cf_buf *buf, cf_str *out) {
    return rtr_str_copy(cf_buf_span(buf), out);
}

/* Read a NOT NULL datetime(6) text column into UTC microseconds. */
static cf_err rtr_column_time(sqlite3_stmt *stmt, int column, const char *name,
                              int64_t *out_us) {
    cf_span text = cf_stmt_column_text(stmt, column);
    if (text.ptr == NULL) {
        return cf_db_failf(CF_DB, "action_text_rich_texts.%s is NULL", name);
    }
    if (cf_db_time_from_text(text, out_us) != CF_OK) {
        return cf_db_failf(CF_DB,
                           "action_text_rich_texts.%s is not valid datetime "
                           "text",
                           name);
    }
    return CF_OK;
}

/* Copy a required text column; the schema says it cannot be NULL. */
static cf_err rtr_column_required_text(sqlite3_stmt *stmt, int column,
                                       const char *name, cf_buf **out) {
    cf_err rc = cf_stmt_column_copy_text(stmt, column, out);
    if (rc != CF_OK) return rc;
    if (*out == NULL) {
        return cf_db_failf(CF_DB, "action_text_rich_texts.%s is NULL", name);
    }
    return CF_OK;
}

/* One failed step: copy the connection message before resetting the
 * statement, then report it with the mapped code. */
static cf_err rtr_step_failure(cf_db *db, sqlite3_stmt *stmt, int sqlite_rc,
                               const char *operation) {
    char message[CF_DB_ERROR_CAP];
    snprintf(message, sizeof message, "%s", sqlite3_errmsg(cf_db_handle(db)));
    cf_db_stmt_done(stmt);
    return cf_db_failf(cf_db_err(sqlite_rc), "%s: %s", operation, message);
}

void cf_rich_text_record_dispose(cf_rich_text_record *record) {
    if (record == NULL) return;
    cf_str_dispose(&record->name);
    cf_optional_str_dispose(&record->body);
    cf_str_dispose(&record->record_type);
    *record = (cf_rich_text_record){0};
}

void cf_rich_text_record_vector_dispose(cf_rich_text_record_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len; i++) {
        cf_rich_text_record_dispose(&vector->items[i]);
    }
    free(vector->items);
    vector->items = NULL;
    vector->len = 0;
    vector->cap = 0;
}

cf_err cf_rich_text_record_find_for(cf_db *db, cf_str record_type,
                                    int64_t record_id, cf_str name, bool *found,
                                    cf_rich_text_record *out) {
    /* Clear every output the caller provided, then validate (out pointers
     * stay empty on failure). */
    if (found != NULL) *found = false;
    if (out != NULL) *out = (cf_rich_text_record){0};
    if (found == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "find_for: no output pointer");
    }
    if (db == NULL) return cf_db_failf(CF_INVALID, "find_for: no database");

    sqlite3_stmt *stmt = NULL;
    cf_err rc =
        cf_db_stmt(db, &cf_rtr_stmt_set, CF_RTR_STMT_FIND_FOR, &stmt);
    if (rc != CF_OK) return rc;

    /* params![record_id, record_type, name] */
    rc = cf_stmt_bind_i64(stmt, 1, record_id);
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 2, rtr_span(record_type));
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 3, rtr_span(name));
    if (rc != CF_OK) {
        cf_db_stmt_done(stmt);
        return rc;
    }

    cf_buf *name_buf = NULL;
    cf_buf *body_buf = NULL;
    cf_buf *type_buf = NULL;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    cf_err read_rc;

    int step = sqlite3_step(stmt);
    if (step == SQLITE_DONE) {
        /* query_one(...).optional() -> None: not an error */
        cf_db_stmt_done(stmt);
        return CF_OK;
    }
    if (step != SQLITE_ROW) {
        return rtr_step_failure(db, stmt, step, "find_for");
    }

    /* Column order is the source `columns!` order.  Every value is copied or
     * scalar before cf_db_stmt_done resets the statement. */
    int64_t id = cf_stmt_column_i64(stmt, 0);
    int64_t row_record_id = cf_stmt_column_i64(stmt, 4);
    read_rc = rtr_column_required_text(stmt, 1, "name", &name_buf);
    if (read_rc == CF_OK) {
        read_rc = cf_stmt_column_copy_text(stmt, 2, &body_buf);
    }
    if (read_rc == CF_OK) {
        read_rc = rtr_column_required_text(stmt, 3, "record_type", &type_buf);
    }
    if (read_rc == CF_OK) {
        read_rc = rtr_column_time(stmt, 5, "created_at", &created_at);
    }
    if (read_rc == CF_OK) {
        read_rc = rtr_column_time(stmt, 6, "updated_at", &updated_at);
    }

    cf_db_stmt_done(stmt);

    if (read_rc == CF_OK) {
        out->id = id;
        out->record_id = row_record_id;
        out->created_at = created_at;
        out->updated_at = updated_at;
        read_rc = rtr_str_from_buf(name_buf, &out->name);
    }
    if (read_rc == CF_OK && body_buf != NULL) {
        out->body.present = true;
        read_rc = rtr_str_from_buf(body_buf, &out->body.value);
    }
    if (read_rc == CF_OK) {
        read_rc = rtr_str_from_buf(type_buf, &out->record_type);
    }

    cf_buf_release(name_buf);
    cf_buf_release(body_buf);
    cf_buf_release(type_buf);

    if (read_rc != CF_OK) {
        cf_rich_text_record_dispose(out);
        return read_rc;
    }
    *found = true;
    return CF_OK;
}

cf_err cf_rich_text_record_create(cf_tx *tx, cf_str record_type,
                                  int64_t record_id, cf_str name, cf_str body,
                                  cf_rich_text_record *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "create: no output pointer");
    }
    *out = (cf_rich_text_record){0};
    if (tx == NULL) return cf_db_failf(CF_INVALID, "create: no transaction");
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return cf_db_failf(CF_INVALID, "create: no database");

    /* let now = tx.now(); the same instant is used for both datetime columns
     * and for the returned record. */
    int64_t now = cf_now_us(NULL);
    char now_text[CF_DB_TIME_TEXT_CAP];
    cf_err rc = cf_db_time_to_text(now, now_text);
    if (rc != CF_OK) return rc;

    sqlite3_stmt *stmt = NULL;
    rc = cf_db_stmt(db, &cf_rtr_stmt_set, CF_RTR_STMT_CREATE, &stmt);
    if (rc != CF_OK) return rc;

    /* params![body, now, name, record_id, record_type, now] */
    rc = cf_stmt_bind_text(stmt, 1, rtr_span(body));
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 2, rtr_cstr_span(now_text));
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 3, rtr_span(name));
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 4, record_id);
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 5, rtr_span(record_type));
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 6, rtr_cstr_span(now_text));
    if (rc != CF_OK) {
        cf_db_stmt_done(stmt);
        return rc;
    }

    int step = sqlite3_step(stmt);
    if (step != SQLITE_ROW) {
        if (step == SQLITE_DONE) {
            cf_db_stmt_done(stmt);
            return cf_db_failf(CF_DB, "create: INSERT returned no id");
        }
        return rtr_step_failure(db, stmt, step, "create");
    }
    int64_t id = cf_stmt_column_i64(stmt, 0);
    cf_db_stmt_done(stmt);

    rc = rtr_str_copy(rtr_span(name), &out->name);
    if (rc == CF_OK) {
        out->body.present = true;
        rc = rtr_str_copy(rtr_span(body), &out->body.value);
    }
    if (rc == CF_OK) {
        rc = rtr_str_copy(rtr_span(record_type), &out->record_type);
    }
    if (rc != CF_OK) {
        cf_rich_text_record_dispose(out);
        return rc;
    }

    out->id = id;
    out->record_id = record_id;
    out->created_at = now;
    out->updated_at = now;
    return CF_OK;
}

cf_err cf_rich_text_record_update_body(cf_tx *tx, cf_rich_text_record *record,
                                       cf_str body) {
    if (record == NULL) {
        return cf_db_failf(CF_INVALID, "update_body: no record");
    }
    if (tx == NULL) {
        return cf_db_failf(CF_INVALID, "update_body: no transaction");
    }
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) {
        return cf_db_failf(CF_INVALID, "update_body: no database");
    }

    /* The reference only mutates `record` after execute succeeds; copy the
     * new text first so a failed statement leaves the record untouched. */
    cf_str body_copy = {0};
    cf_err rc = rtr_str_copy(rtr_span(body), &body_copy);
    if (rc != CF_OK) return rc;

    int64_t now = cf_now_us(NULL);
    char now_text[CF_DB_TIME_TEXT_CAP];
    rc = cf_db_time_to_text(now, now_text);
    if (rc != CF_OK) {
        cf_str_dispose(&body_copy);
        return rc;
    }

    sqlite3_stmt *stmt = NULL;
    rc = cf_db_stmt(db, &cf_rtr_stmt_set, CF_RTR_STMT_UPDATE_BODY, &stmt);
    if (rc != CF_OK) {
        cf_str_dispose(&body_copy);
        return rc;
    }

    /* params![body, now, self.id] */
    rc = cf_stmt_bind_text(stmt, 1, rtr_span(body_copy));
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 2, rtr_cstr_span(now_text));
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 3, record->id);
    if (rc != CF_OK) {
        cf_db_stmt_done(stmt);
        cf_str_dispose(&body_copy);
        return rc;
    }

    int step = sqlite3_step(stmt);
    if (step != SQLITE_DONE) {
        cf_err failure = rtr_step_failure(db, stmt, step, "update_body");
        cf_str_dispose(&body_copy);
        return failure;
    }
    cf_db_stmt_done(stmt);

    cf_str_dispose(&record->body.value);
    record->body.present = true;
    record->body.value = body_copy;
    record->updated_at = now;
    return CF_OK;
}

cf_err cf_rich_text_record_delete(cf_tx *tx,
                                  const cf_rich_text_record *record) {
    if (record == NULL) {
        return cf_db_failf(CF_INVALID, "delete: no record");
    }
    if (tx == NULL) return cf_db_failf(CF_INVALID, "delete: no transaction");
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return cf_db_failf(CF_INVALID, "delete: no database");

    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &cf_rtr_stmt_set, CF_RTR_STMT_DELETE, &stmt);
    if (rc != CF_OK) return rc;

    rc = cf_stmt_bind_i64(stmt, 1, record->id);
    if (rc != CF_OK) {
        cf_db_stmt_done(stmt);
        return rc;
    }

    int step = sqlite3_step(stmt);
    if (step != SQLITE_DONE) {
        return rtr_step_failure(db, stmt, step, "delete");
    }
    cf_db_stmt_done(stmt);
    return CF_OK;
}
