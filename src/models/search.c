/* src/models/search.c — Search model (D01).
 *
 * Source: tmp/rust-ref/crates/db/src/models/search.rs (5 inventoried
 * functions).  Table: searches (docs/devel/implementation/contracts/schema.sql).
 *
 * Translation notes:
 *  - Reads take cf_db * (reader/writer connection); record and
 *    destroy_all_for_user take cf_tx * and reach the transaction's connection
 *    through cf_tx_db(tx), as the reference reaches tx.conn().
 *  - Rust `tx.now()` maps to cf_now_us(NULL): the process clock, the same
 *    source D02's Tx::now delegates to and the only clock cf.h exposes;
 *    core/testclock.h fixes it in tests (no sleeps).
 *  - The reference trims with `id NOT IN (placeholders(keep.len().max(1)))`,
 *    a placeholder list of at most RECENT_SEARCHES entries.  A fixed
 *    statement per count (1..10) keeps the SQL source-selected and prepared
 *    per connection; the empty-keep case binds 0 exactly as the source does.
 *  - Datetimes are stored as the reference UTC SQL text
 *    (cf_db_time_to_text: ".ffffff" only when non-zero) and kept in memory as
 *    int64 UTC microseconds.
 *  - SQL text and ordering are copied from the source; every statement is
 *    fixed and comes from this module's statement set.
 */
#include "models/search.h"

#include "db/db_internal.h"

#include <stdlib.h>
#include <string.h>

/* --- fixed statements ----------------------------------------------------- */

enum search_stmt_id {
    SEARCH_STMT_ORDERED_FOR_USER = 0,
    SEARCH_STMT_COUNT_ROWS,
    SEARCH_STMT_COUNT_FOR_USER,
    SEARCH_STMT_FIND_BY_QUERY,
    SEARCH_STMT_TOUCH,
    SEARCH_STMT_INSERT,
    SEARCH_STMT_KEEP_RECENT,
    /* One SELECT with n placeholders in NOT IN (?, ?, ...); ids 1..10 keep
     * every keep.len() value the LIMIT 10 can produce. */
    SEARCH_STMT_DOOMED_1,
    SEARCH_STMT_DOOMED_2,
    SEARCH_STMT_DOOMED_3,
    SEARCH_STMT_DOOMED_4,
    SEARCH_STMT_DOOMED_5,
    SEARCH_STMT_DOOMED_6,
    SEARCH_STMT_DOOMED_7,
    SEARCH_STMT_DOOMED_8,
    SEARCH_STMT_DOOMED_9,
    SEARCH_STMT_DOOMED_10,
    SEARCH_STMT_DELETE,
    SEARCH_STMT_IDS_FOR_USER,
    SEARCH_STMT_COUNT
};

_Static_assert(
    (size_t)(SEARCH_STMT_DOOMED_10 - SEARCH_STMT_DOOMED_1 + 1) ==
        (size_t)CF_SEARCH_RECENT_SEARCHES,
    "one NOT IN statement per kept search");

static const cf_stmt_def search_stmt_defs[SEARCH_STMT_COUNT] = {
    [SEARCH_STMT_ORDERED_FOR_USER] = {
        "SELECT \"searches\".* FROM \"searches\" "
        "WHERE \"searches\".\"user_id\" = ? "
        "ORDER BY \"searches\".\"updated_at\" DESC"},
    [SEARCH_STMT_COUNT_ROWS] = {"SELECT COUNT(*) FROM \"searches\""},
    [SEARCH_STMT_COUNT_FOR_USER] = {
        "SELECT COUNT(*) FROM \"searches\" "
        "WHERE \"searches\".\"user_id\" = ?"},
    [SEARCH_STMT_FIND_BY_QUERY] = {
        "SELECT \"searches\".* FROM \"searches\" "
        "WHERE \"searches\".\"user_id\" = ? AND \"searches\".\"query\" = ? "
        "LIMIT 1"},
    [SEARCH_STMT_TOUCH] = {
        "UPDATE \"searches\" SET \"updated_at\" = ? "
        "WHERE \"searches\".\"id\" = ?"},
    [SEARCH_STMT_INSERT] = {
        "INSERT INTO \"searches\" (\"created_at\", \"query\", \"updated_at\", "
        "\"user_id\") VALUES (?, ?, ?, ?) RETURNING \"id\""},
    [SEARCH_STMT_KEEP_RECENT] = {
        "SELECT \"searches\".\"id\" FROM \"searches\" "
        "WHERE \"searches\".\"user_id\" = ? "
        "ORDER BY \"searches\".\"updated_at\" DESC LIMIT ?"},
    [SEARCH_STMT_DOOMED_1] = {
        "SELECT \"searches\".\"id\" FROM \"searches\" "
        "WHERE \"searches\".\"user_id\" = ? AND \"searches\".\"id\" "
        "NOT IN (?)"},
    [SEARCH_STMT_DOOMED_2] = {
        "SELECT \"searches\".\"id\" FROM \"searches\" "
        "WHERE \"searches\".\"user_id\" = ? AND \"searches\".\"id\" "
        "NOT IN (?, ?)"},
    [SEARCH_STMT_DOOMED_3] = {
        "SELECT \"searches\".\"id\" FROM \"searches\" "
        "WHERE \"searches\".\"user_id\" = ? AND \"searches\".\"id\" "
        "NOT IN (?, ?, ?)"},
    [SEARCH_STMT_DOOMED_4] = {
        "SELECT \"searches\".\"id\" FROM \"searches\" "
        "WHERE \"searches\".\"user_id\" = ? AND \"searches\".\"id\" "
        "NOT IN (?, ?, ?, ?)"},
    [SEARCH_STMT_DOOMED_5] = {
        "SELECT \"searches\".\"id\" FROM \"searches\" "
        "WHERE \"searches\".\"user_id\" = ? AND \"searches\".\"id\" "
        "NOT IN (?, ?, ?, ?, ?)"},
    [SEARCH_STMT_DOOMED_6] = {
        "SELECT \"searches\".\"id\" FROM \"searches\" "
        "WHERE \"searches\".\"user_id\" = ? AND \"searches\".\"id\" "
        "NOT IN (?, ?, ?, ?, ?, ?)"},
    [SEARCH_STMT_DOOMED_7] = {
        "SELECT \"searches\".\"id\" FROM \"searches\" "
        "WHERE \"searches\".\"user_id\" = ? AND \"searches\".\"id\" "
        "NOT IN (?, ?, ?, ?, ?, ?, ?)"},
    [SEARCH_STMT_DOOMED_8] = {
        "SELECT \"searches\".\"id\" FROM \"searches\" "
        "WHERE \"searches\".\"user_id\" = ? AND \"searches\".\"id\" "
        "NOT IN (?, ?, ?, ?, ?, ?, ?, ?)"},
    [SEARCH_STMT_DOOMED_9] = {
        "SELECT \"searches\".\"id\" FROM \"searches\" "
        "WHERE \"searches\".\"user_id\" = ? AND \"searches\".\"id\" "
        "NOT IN (?, ?, ?, ?, ?, ?, ?, ?, ?)"},
    [SEARCH_STMT_DOOMED_10] = {
        "SELECT \"searches\".\"id\" FROM \"searches\" "
        "WHERE \"searches\".\"user_id\" = ? AND \"searches\".\"id\" "
        "NOT IN (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"},
    [SEARCH_STMT_DELETE] = {
        "DELETE FROM \"searches\" WHERE \"searches\".\"id\" = ?"},
    [SEARCH_STMT_IDS_FOR_USER] = {
        "SELECT \"searches\".\"id\" FROM \"searches\" "
        "WHERE \"searches\".\"user_id\" = ?"},
};

static const cf_stmt_set search_stmts = {search_stmt_defs,
                                         SEARCH_STMT_COUNT};

/* --- helpers -------------------------------------------------------------- */

/* A step that failed (not SQLITE_ROW / SQLITE_DONE) with a diagnostic. */
static cf_err search_step_failed(cf_db *db, int rc, const char *op) {
    if (rc == SQLITE_ROW) {
        return cf_db_failf(CF_INTERNAL, "%s: unexpected row", op);
    }
    return cf_db_failf(cf_db_err(rc), "%s failed: %s", op,
                       sqlite3_errmsg(cf_db_handle(db)));
}

/* Copy a text column into owned NUL-terminated memory before reset.  The
 * query column is NOT NULL; the span empty/NULL collapse matches an empty
 * string, which the model's own strings never are. */
static cf_err search_copy_text(sqlite3_stmt *stmt, int column, cf_str *out) {
    cf_span span = cf_stmt_column_text(stmt, column);
    out->ptr = NULL;
    out->len = 0;
    if (span.len != 0 && span.ptr == NULL) return CF_INVALID;
    if (span.len == SIZE_MAX) return CF_LIMIT; /* len + NUL overflow */
    char *copy = malloc(span.len + 1);
    if (copy == NULL) return CF_NOMEM;
    if (span.len != 0) memcpy(copy, span.ptr, span.len);
    copy[span.len] = '\0';
    out->ptr = copy;
    out->len = span.len;
    return CF_OK;
}

/* Owned copy of a borrowed input (the query written into a record). */
static cf_err search_copy_input(cf_str in, cf_str *out) {
    out->ptr = NULL;
    out->len = 0;
    if (in.len != 0 && in.ptr == NULL) return CF_INVALID;
    if (in.len == SIZE_MAX) return CF_LIMIT;
    char *copy = malloc(in.len + 1);
    if (copy == NULL) return CF_NOMEM;
    if (in.len != 0) memcpy(copy, in.ptr, in.len);
    copy[in.len] = '\0';
    out->ptr = copy;
    out->len = in.len;
    return CF_OK;
}

static cf_span span_of_text(const char *text) {
    cf_span span = {(const unsigned char *)text, strlen(text)};
    return span;
}

static cf_span span_of_str(cf_str text) {
    cf_span span = {(const unsigned char *)text.ptr, text.len};
    return span;
}

/* Search::from_row: fields by the searches DDL column order produced by
 * SELECT "searches".* — id(0), created_at(1), query(2), updated_at(3),
 * user_id(4).  On failure *out is disposed back to empty. */
static cf_err search_from_row(sqlite3_stmt *stmt, cf_search *out) {
    memset(out, 0, sizeof *out);
    out->id = cf_stmt_column_i64(stmt, 0);
    cf_err rc =
        cf_db_time_from_text(cf_stmt_column_text(stmt, 1), &out->created_at);
    if (rc == CF_OK) {
        rc = search_copy_text(stmt, 2, &out->query);
    }
    if (rc == CF_OK) {
        rc = cf_db_time_from_text(cf_stmt_column_text(stmt, 3),
                                  &out->updated_at);
    }
    out->user_id = cf_stmt_column_i64(stmt, 4);
    if (rc != CF_OK) cf_search_dispose(out);
    return rc;
}

static cf_err search_vector_push(cf_search_vector *vector, cf_search *item) {
    if (vector->len == vector->cap) {
        size_t cap = vector->cap != 0 ? vector->cap * 2 : 8;
        if (cap < vector->cap || cap > SIZE_MAX / sizeof *vector->items) {
            return CF_LIMIT;
        }
        cf_search *items = realloc(vector->items, cap * sizeof *items);
        if (items == NULL) return CF_NOMEM;
        vector->items = items;
        vector->cap = cap;
    }
    vector->items[vector->len++] = *item; /* ownership moves to the vector */
    memset(item, 0, sizeof *item);
    return CF_OK;
}

/* Growable int64 list for the ids a trim or destroy_all scans before
 * deleting (the source collects every id with query_all first). */
typedef struct {
    int64_t *items;
    size_t len, cap;
} search_id_list;

static cf_err search_id_list_push(search_id_list *list, int64_t id) {
    if (list->len == list->cap) {
        size_t cap = list->cap != 0 ? list->cap * 2 : 16;
        if (cap < list->cap || cap > SIZE_MAX / sizeof *list->items) {
            return CF_LIMIT;
        }
        int64_t *items = realloc(list->items, cap * sizeof *items);
        if (items == NULL) return CF_NOMEM;
        list->items = items;
        list->cap = cap;
    }
    list->items[list->len++] = id;
    return CF_OK;
}

static void search_id_list_dispose(search_id_list *list) {
    free(list->items);
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

/* --- record and vector disposal ------------------------------------------- */

void cf_search_dispose(cf_search *search) {
    if (search == NULL) return;
    cf_str_dispose(&search->query);
    search->id = 0;
    search->user_id = 0;
    search->created_at = 0;
    search->updated_at = 0;
}

void cf_search_vector_dispose(cf_search_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len; i++) {
        cf_search_dispose(&vector->items[i]);
    }
    free(vector->items);
    vector->items = NULL;
    vector->len = 0;
    vector->cap = 0;
}

/* --- reads ---------------------------------------------------------------- */

cf_err cf_search_ordered_for_user(cf_db *db, int64_t user_id,
                                  cf_search_vector *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "no output vector");
    memset(out, 0, sizeof *out);

    sqlite3_stmt *stmt = NULL;
    cf_err rc =
        cf_db_stmt(db, &search_stmts, SEARCH_STMT_ORDERED_FOR_USER, &stmt);
    if (rc != CF_OK) return rc;
    rc = cf_stmt_bind_i64(stmt, 1, user_id);
    while (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            rc = search_step_failed(db, step, "searches for user");
            break;
        }
        cf_search row;
        rc = search_from_row(stmt, &row);
        if (rc != CF_OK) break;
        rc = search_vector_push(out, &row);
        if (rc != CF_OK) cf_search_dispose(&row);
    }
    cf_db_stmt_done(stmt);
    if (rc != CF_OK) cf_search_vector_dispose(out);
    return rc;
}

cf_err cf_search_count(cf_db *db, int64_t *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "no output count");
    *out = 0;

    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &search_stmts, SEARCH_STMT_COUNT_ROWS, &stmt);
    if (rc != CF_OK) return rc;
    int step = sqlite3_step(stmt);
    if (step == SQLITE_ROW) {
        *out = cf_stmt_column_i64(stmt, 0);
    } else if (step == SQLITE_DONE) {
        rc = cf_db_failf(CF_INTERNAL, "count returned no row");
    } else {
        rc = search_step_failed(db, step, "count searches");
    }
    cf_db_stmt_done(stmt);
    return rc;
}

cf_err cf_search_count_for_user(cf_db *db, int64_t user_id, int64_t *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "no output count");
    *out = 0;

    sqlite3_stmt *stmt = NULL;
    cf_err rc =
        cf_db_stmt(db, &search_stmts, SEARCH_STMT_COUNT_FOR_USER, &stmt);
    if (rc != CF_OK) return rc;
    rc = cf_stmt_bind_i64(stmt, 1, user_id);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            *out = cf_stmt_column_i64(stmt, 0);
        } else if (step == SQLITE_DONE) {
            rc = cf_db_failf(CF_INTERNAL, "count returned no row");
        } else {
            rc = search_step_failed(db, step, "count searches for user");
        }
    }
    cf_db_stmt_done(stmt);
    return rc;
}

/* --- writes --------------------------------------------------------------- */

/* `user.searches.excluding(user.searches.ordered.limit(10)).destroy_all` */
static cf_err search_trim_recent(cf_tx *tx, int64_t user_id) {
    cf_db *db = cf_tx_db(tx);

    /* keep = ordered.limit(RECENT_SEARCHES): ids in updated_at DESC order. */
    int64_t keep[CF_SEARCH_RECENT_SEARCHES];
    size_t keep_len = 0;
    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &search_stmts, SEARCH_STMT_KEEP_RECENT, &stmt);
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 1, user_id);
    if (rc == CF_OK) {
        rc = cf_stmt_bind_i64(stmt, 2, CF_SEARCH_RECENT_SEARCHES);
    }
    while (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            rc = search_step_failed(db, step, "recent searches for user");
            break;
        }
        /* LIMIT RECENT_SEARCHES bounds the rows; ignore any beyond it. */
        if (keep_len < (size_t)CF_SEARCH_RECENT_SEARCHES) {
            keep[keep_len] = cf_stmt_column_i64(stmt, 0);
        }
        keep_len++;
    }
    cf_db_stmt_done(stmt);
    if (rc != CF_OK) return rc;
    if (keep_len > (size_t)CF_SEARCH_RECENT_SEARCHES) {
        keep_len = (size_t)CF_SEARCH_RECENT_SEARCHES;
    }

    /* The source binds placeholders(keep.len().max(1)); an empty keep binds
     * the literal 0, which no searches.id can equal. */
    size_t placeholders = keep_len != 0 ? keep_len : 1;
    stmt = NULL;
    rc = cf_db_stmt(db, &search_stmts,
                    (size_t)SEARCH_STMT_DOOMED_1 + placeholders - 1, &stmt);
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 1, user_id);
    if (rc == CF_OK && keep_len == 0) {
        rc = cf_stmt_bind_i64(stmt, 2, 0);
    }
    for (size_t i = 0; rc == CF_OK && i < keep_len; i++) {
        rc = cf_stmt_bind_i64(stmt, (int)(i + 2), keep[i]);
    }

    search_id_list doomed = {NULL, 0, 0};
    while (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            rc = search_step_failed(db, step, "trim searches for user");
            break;
        }
        rc = search_id_list_push(&doomed, cf_stmt_column_i64(stmt, 0));
    }
    cf_db_stmt_done(stmt);
    if (rc != CF_OK) {
        search_id_list_dispose(&doomed);
        return rc;
    }

    for (size_t i = 0; rc == CF_OK && i < doomed.len; i++) {
        stmt = NULL;
        rc = cf_db_stmt(db, &search_stmts, SEARCH_STMT_DELETE, &stmt);
        if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 1, doomed.items[i]);
        if (rc == CF_OK) {
            int step = sqlite3_step(stmt);
            if (step != SQLITE_DONE) {
                rc = search_step_failed(db, step, "delete trimmed search");
            }
        }
        if (stmt != NULL) cf_db_stmt_done(stmt);
    }
    search_id_list_dispose(&doomed);
    return rc;
}

static cf_err search_create(cf_tx *tx, int64_t user_id, cf_str query,
                            cf_search *out) {
    cf_db *db = cf_tx_db(tx);
    int64_t now = cf_now_us(NULL);
    char now_text[CF_DB_TIME_TEXT_CAP];
    cf_err rc = cf_db_time_to_text(now, now_text);
    if (rc != CF_OK) return rc;

    cf_str query_copy;
    rc = search_copy_input(query, &query_copy);
    if (rc != CF_OK) return rc;

    sqlite3_stmt *stmt = NULL;
    rc = cf_db_stmt(db, &search_stmts, SEARCH_STMT_INSERT, &stmt);
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 1, span_of_text(now_text));
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 2, span_of_str(query_copy));
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 3, span_of_text(now_text));
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 4, user_id);

    int64_t id = 0;
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            id = cf_stmt_column_i64(stmt, 0);
        } else if (step == SQLITE_DONE) {
            rc = cf_db_failf(CF_INTERNAL, "insert returned no id");
        } else {
            rc = search_step_failed(db, step, "create search");
        }
    }
    if (stmt != NULL) cf_db_stmt_done(stmt);
    if (rc != CF_OK) {
        cf_str_dispose(&query_copy);
        return rc;
    }

    rc = search_trim_recent(tx, user_id);
    if (rc != CF_OK) {
        cf_str_dispose(&query_copy);
        return rc;
    }

    out->id = id;
    out->user_id = user_id;
    out->query = query_copy; /* ownership moves */
    out->created_at = now;
    out->updated_at = now;
    return CF_OK;
}

cf_err cf_search_record(cf_tx *tx, int64_t user_id, cf_str query,
                        cf_search *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "no output search");
    memset(out, 0, sizeof *out);
    if (tx == NULL) return cf_db_failf(CF_INVALID, "no transaction");
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return cf_db_failf(CF_INTERNAL, "transaction has no db");

    /* find_or_create_by(query:): exact user_id + query match, LIMIT 1. */
    bool found = false;
    sqlite3_stmt *stmt = NULL;
    cf_err rc =
        cf_db_stmt(db, &search_stmts, SEARCH_STMT_FIND_BY_QUERY, &stmt);
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 1, user_id);
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 2, span_of_str(query));
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            rc = search_from_row(stmt, out);
            if (rc == CF_OK) found = true;
        } else if (step != SQLITE_DONE) {
            rc = search_step_failed(db, step, "find search by query");
        }
    }
    if (stmt != NULL) cf_db_stmt_done(stmt);
    if (rc != CF_OK) {
        cf_search_dispose(out);
        return rc;
    }

    if (!found) {
        rc = search_create(tx, user_id, query, out);
        if (rc != CF_OK) return rc;
    }

    /* .touch: unconditional, including right after a create, exactly as the
     * source runs UPDATE + `search.updated_at = tx.now()` for both arms (the
     * row keeps its created_at). */
    int64_t now = cf_now_us(NULL);
    char now_text[CF_DB_TIME_TEXT_CAP];
    rc = cf_db_time_to_text(now, now_text);
    if (rc != CF_OK) {
        cf_search_dispose(out);
        return rc;
    }
    stmt = NULL;
    rc = cf_db_stmt(db, &search_stmts, SEARCH_STMT_TOUCH, &stmt);
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 1, span_of_text(now_text));
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 2, out->id);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step != SQLITE_DONE) {
            rc = search_step_failed(db, step, "touch search");
        }
    }
    if (stmt != NULL) cf_db_stmt_done(stmt);
    if (rc != CF_OK) {
        cf_search_dispose(out);
        return rc;
    }
    out->updated_at = now;
    return CF_OK;
}

cf_err cf_search_destroy_all_for_user(cf_tx *tx, int64_t user_id) {
    if (tx == NULL) return cf_db_failf(CF_INVALID, "no transaction");
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return cf_db_failf(CF_INTERNAL, "transaction has no db");

    /* user.searches.destroy_all: collect the ids, then delete each row. */
    search_id_list ids = {NULL, 0, 0};
    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &search_stmts, SEARCH_STMT_IDS_FOR_USER, &stmt);
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 1, user_id);
    while (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            rc = search_step_failed(db, step, "search ids for user");
            break;
        }
        rc = search_id_list_push(&ids, cf_stmt_column_i64(stmt, 0));
    }
    cf_db_stmt_done(stmt);
    if (rc != CF_OK) {
        search_id_list_dispose(&ids);
        return rc;
    }

    for (size_t i = 0; rc == CF_OK && i < ids.len; i++) {
        stmt = NULL;
        rc = cf_db_stmt(db, &search_stmts, SEARCH_STMT_DELETE, &stmt);
        if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 1, ids.items[i]);
        if (rc == CF_OK) {
            int step = sqlite3_step(stmt);
            if (step != SQLITE_DONE) {
                rc = search_step_failed(db, step, "destroy search");
            }
        }
        if (stmt != NULL) cf_db_stmt_done(stmt);
    }
    search_id_list_dispose(&ids);
    return rc;
}
