/* Per-connection prepared-statement cache (D01 db-core).
 *
 * One cache per connection, keyed by (owning module's fixed statement set,
 * local enum id).  The SQL text is always a fixed string selected in source;
 * nothing is assembled at runtime.  A cached statement is borrowed, not
 * owned, by the caller: after every use it must be returned with
 * cf_db_stmt_done, which resets it and clears its bindings.  Row bytes are
 * copied (cf_stmt_column_copy_text or a caller-side copy) before that reset,
 * and bound bytes are copied immediately (SQLITE_TRANSIENT), so no SQLite
 * pointer outlives its step.
 */
#include "db/db_internal.h"

#include <limits.h>
#include <string.h>

static cf_stmt_slot *find_slot(cf_db *db, const cf_stmt_set *set, size_t id) {
    for (size_t i = 0; i < db->stmt_count; i++) {
        if (db->stmts[i].set == set && db->stmts[i].id == id) {
            return &db->stmts[i];
        }
    }
    return NULL;
}

cf_err cf_db_stmt(cf_db *db, const cf_stmt_set *set, size_t id,
                  sqlite3_stmt **out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "no output pointer");
    *out = NULL;
    if (db == NULL) return cf_db_failf(CF_INVALID, "no database");
    if (set == NULL || set->defs == NULL) {
        return cf_db_failf(CF_INVALID, "no statement set");
    }
    if (id >= set->count) {
        return cf_db_failf(CF_INTERNAL, "statement id %zu out of range",
                           id);
    }
    if (!cf_db_thread_ok(db)) {
        return cf_db_failf(CF_INTERNAL,
                           "statement used from a non-owning thread");
    }

    cf_stmt_slot *slot = find_slot(db, set, id);
    if (slot != NULL) {
        *out = slot->stmt;
        return CF_OK;
    }

    if (db->stmt_count >= CF_DB_STMT_CACHE_CAPACITY) {
        return cf_db_failf(CF_LIMIT,
                           "statement cache full (%d statements on one "
                           "connection)",
                           CF_DB_STMT_CACHE_CAPACITY);
    }
    const char *sql = set->defs[id].sql;
    if (sql == NULL || sql[0] == '\0') {
        return cf_db_failf(CF_INTERNAL, "statement %zu has no SQL", id);
    }

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return cf_db_failf(cf_db_err(rc), "prepare failed: %s",
                           sqlite3_errmsg(db->handle));
    }
    if (stmt == NULL) {
        return cf_db_failf(CF_DB, "prepare returned no statement");
    }

    slot = &db->stmts[db->stmt_count++];
    slot->set = set;
    slot->id = id;
    slot->stmt = stmt;
    *out = stmt;
    return CF_OK;
}

void cf_db_stmt_done(sqlite3_stmt *stmt) {
    if (stmt == NULL) return;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
}

void cf_db_stmt_cache_clear(cf_db *db) {
    if (db == NULL) return;
    for (size_t i = 0; i < db->stmt_count; i++) {
        sqlite3_finalize(db->stmts[i].stmt);
        db->stmts[i].set = NULL;
        db->stmts[i].id = 0;
        db->stmts[i].stmt = NULL;
    }
    db->stmt_count = 0;
}

/* --- binding helpers ----------------------------------------------------- */

static cf_err bind_result(int rc) {
    switch (rc) {
    case SQLITE_OK:
        return CF_OK;
    case SQLITE_NOMEM:
        return CF_NOMEM;
    case SQLITE_RANGE:
    case SQLITE_MISUSE:
        return CF_INTERNAL;
    default:
        return CF_DB;
    }
}

cf_err cf_stmt_bind_i64(sqlite3_stmt *stmt, int index, int64_t value) {
    if (stmt == NULL || index <= 0) {
        return cf_db_failf(CF_INVALID, "invalid bind argument");
    }
    return bind_result(sqlite3_bind_int64(stmt, index, value));
}

cf_err cf_stmt_bind_null(sqlite3_stmt *stmt, int index) {
    if (stmt == NULL || index <= 0) {
        return cf_db_failf(CF_INVALID, "invalid bind argument");
    }
    return bind_result(sqlite3_bind_null(stmt, index));
}

cf_err cf_stmt_bind_text(sqlite3_stmt *stmt, int index, cf_span text) {
    if (stmt == NULL || index <= 0) {
        return cf_db_failf(CF_INVALID, "invalid bind argument");
    }
    if (text.len > (size_t)INT_MAX) {
        return cf_db_failf(CF_LIMIT, "text longer than SQLite can bind");
    }
    if (text.len != 0 && text.ptr == NULL) {
        return cf_db_failf(CF_INVALID, "text span has no bytes");
    }
    /* An empty span binds empty text, never NULL: sqlite3_bind_text with a
     * NULL pointer would bind NULL.  Use bind_null for absent values. */
    const char *bytes = text.len != 0 ? (const char *)text.ptr : "";
    return bind_result(
        sqlite3_bind_text(stmt, index, bytes, (int)text.len, SQLITE_TRANSIENT));
}

cf_err cf_stmt_bind_opt_i64(sqlite3_stmt *stmt, int index,
                            cf_optional_i64 value) {
    return value.present ? cf_stmt_bind_i64(stmt, index, value.value)
                         : cf_stmt_bind_null(stmt, index);
}

cf_err cf_stmt_bind_opt_text(sqlite3_stmt *stmt, int index, bool present,
                             cf_span text) {
    return present ? cf_stmt_bind_text(stmt, index, text)
                   : cf_stmt_bind_null(stmt, index);
}

/* --- column readers ------------------------------------------------------ */

bool cf_stmt_column_is_null(sqlite3_stmt *stmt, int column) {
    return stmt == NULL || column < 0 ||
           sqlite3_column_type(stmt, column) == SQLITE_NULL;
}

int64_t cf_stmt_column_i64(sqlite3_stmt *stmt, int column) {
    if (stmt == NULL || column < 0) return 0;
    return sqlite3_column_int64(stmt, column);
}

cf_span cf_stmt_column_text(sqlite3_stmt *stmt, int column) {
    cf_span span = {NULL, 0};
    if (stmt == NULL || column < 0) return span;
    const unsigned char *bytes = sqlite3_column_text(stmt, column);
    int len = sqlite3_column_bytes(stmt, column);
    if (bytes == NULL || len <= 0) return span;
    span.ptr = bytes;
    span.len = (size_t)len;
    return span;
}

cf_err cf_stmt_column_copy_text(sqlite3_stmt *stmt, int column,
                                cf_buf **out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "no output pointer");
    *out = NULL;
    if (stmt == NULL || column < 0) {
        return cf_db_failf(CF_INVALID, "invalid column argument");
    }
    if (sqlite3_column_type(stmt, column) == SQLITE_NULL) {
        return CF_OK; /* absent: *out stays NULL, distinct from empty */
    }
    return cf_buf_copy(cf_stmt_column_text(stmt, column), out);
}
