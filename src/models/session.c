/* src/models/session.c — Session model (D01).
 *
 * Source: tmp/rust-ref/crates/db/src/models/session.rs (8 inventoried
 * functions).  Table: sessions (docs/devel/implementation/contracts/schema.sql).
 *
 * Translation notes:
 *  - Reads take cf_db * (reader/writer connection); start/resume/destroy take
 *    cf_tx * and reach the transaction's connection through cf_tx_db(tx).
 *  - Rust `tx.now()` maps to cf_now_us(NULL): the process clock, which is the
 *    same source the writer's Tx::now delegates to in production and the only
 *    clock cf.h exposes; core/testclock.h fixes it in tests (no sleeps).
 *  - Datetimes are stored as the reference UTC SQL text
 *    (cf_db_time_to_text: ".ffffff" only when non-zero) and kept in memory as
 *    int64 UTC microseconds.
 *  - The token is a 24-character has_secure_token over the sql::base58
 *    alphabet ("123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"):
 *    one uniformly random character per draw, values >= 232 redrawn, exactly
 *    SecureRandom.base58(n) / rand::random_range(0..58) semantics.
 *  - SQL text, column order and statement order are copied from the source;
 *    every statement is fixed and comes from this module's statement set.
 */
#include "models/session.h"

#include "db/db_internal.h"

#include <stdlib.h>
#include <string.h>

/* --- fixed statements ----------------------------------------------------- */

enum session_stmt_id {
    SESSION_STMT_FIND = 0,
    SESSION_STMT_FIND_BY_TOKEN,
    SESSION_STMT_FOR_USER,
    SESSION_STMT_COUNT_FOR_USER,
    SESSION_STMT_START,
    SESSION_STMT_RESUME,
    SESSION_STMT_DESTROY,
    SESSION_STMT_COUNT
};

/* The selected column list is the reference session_columns!() expansion. */
#define SESSION_COLUMNS                                                       \
    "\"sessions\".\"id\", \"sessions\".\"user_id\", "                          \
    "\"sessions\".\"token\", \"sessions\".\"ip_address\", "                    \
    "\"sessions\".\"user_agent\", \"sessions\".\"last_active_at\", "           \
    "\"sessions\".\"created_at\", \"sessions\".\"updated_at\""

static const cf_stmt_def session_stmt_defs[SESSION_STMT_COUNT] = {
    [SESSION_STMT_FIND] = {
        "SELECT " SESSION_COLUMNS
        " FROM \"sessions\" WHERE \"sessions\".\"id\" = ? LIMIT 1"},
    [SESSION_STMT_FIND_BY_TOKEN] = {
        "SELECT " SESSION_COLUMNS
        " FROM \"sessions\" WHERE \"sessions\".\"token\" = ? LIMIT 1"},
    [SESSION_STMT_FOR_USER] = {
        "SELECT " SESSION_COLUMNS
        " FROM \"sessions\" WHERE \"sessions\".\"user_id\" = ?"},
    [SESSION_STMT_COUNT_FOR_USER] = {
        "SELECT COUNT(*) FROM \"sessions\" "
        "WHERE \"sessions\".\"user_id\" = ?"},
    [SESSION_STMT_START] = {
        "INSERT INTO \"sessions\" (\"created_at\", \"ip_address\", "
        "\"last_active_at\", \"token\", \"updated_at\", \"user_agent\", "
        "\"user_id\") VALUES (?, ?, ?, ?, ?, ?, ?) RETURNING \"id\""},
    [SESSION_STMT_RESUME] = {
        "UPDATE \"sessions\" SET \"ip_address\" = ?, \"last_active_at\" = ?, "
        "\"updated_at\" = ?, \"user_agent\" = ? "
        "WHERE \"sessions\".\"id\" = ?"},
    [SESSION_STMT_DESTROY] = {
        "DELETE FROM \"sessions\" WHERE \"sessions\".\"id\" = ?"},
};

static const cf_stmt_set session_stmts = {session_stmt_defs,
                                          SESSION_STMT_COUNT};

/* --- helpers -------------------------------------------------------------- */

/* A step that failed (not SQLITE_ROW / SQLITE_DONE) with a diagnostic. */
static cf_err session_step_failed(cf_db *db, int rc, const char *op) {
    if (rc == SQLITE_ROW) {
        return cf_db_failf(CF_INTERNAL, "%s: unexpected row", op);
    }
    return cf_db_failf(cf_db_err(rc), "%s failed: %s", op,
                       sqlite3_errmsg(cf_db_handle(db)));
}

/* Copy a text column into owned NUL-terminated memory before reset.  SQL NULL
 * and empty text both surface as len 0; callers that must distinguish NULL
 * check cf_stmt_column_is_null first (cf_stmt_column_text collapses them). */
static cf_err session_copy_text(sqlite3_stmt *stmt, int column, cf_str *out) {
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

static cf_err session_copy_optional_text(sqlite3_stmt *stmt, int column,
                                         cf_optional_str *out) {
    out->present = false;
    out->value.ptr = NULL;
    out->value.len = 0;
    if (cf_stmt_column_is_null(stmt, column)) return CF_OK;
    cf_err rc = session_copy_text(stmt, column, &out->value);
    if (rc != CF_OK) return rc;
    out->present = true;
    return CF_OK;
}

/* Owned copy of a borrowed optional input (values written into a record). */
static cf_err session_copy_optional_input(cf_optional_str in,
                                          cf_optional_str *out) {
    out->present = false;
    out->value.ptr = NULL;
    out->value.len = 0;
    if (!in.present) return CF_OK;
    if (in.value.len != 0 && in.value.ptr == NULL) return CF_INVALID;
    if (in.value.len == SIZE_MAX) return CF_LIMIT;
    char *copy = malloc(in.value.len + 1);
    if (copy == NULL) return CF_NOMEM;
    if (in.value.len != 0) memcpy(copy, in.value.ptr, in.value.len);
    copy[in.value.len] = '\0';
    out->present = true;
    out->value.ptr = copy;
    out->value.len = in.value.len;
    return CF_OK;
}

/* Session::from_row: fields in session_columns!() order.  On failure *out is
 * disposed back to empty. */
static cf_err session_from_row(sqlite3_stmt *stmt, cf_session *out) {
    memset(out, 0, sizeof *out);
    out->id = cf_stmt_column_i64(stmt, 0);
    out->user_id = cf_stmt_column_i64(stmt, 1);
    cf_err rc = session_copy_text(stmt, 2, &out->token);
    if (rc == CF_OK) {
        rc = session_copy_optional_text(stmt, 3, &out->ip_address);
    }
    if (rc == CF_OK) {
        rc = session_copy_optional_text(stmt, 4, &out->user_agent);
    }
    if (rc == CF_OK) {
        rc = cf_db_time_from_text(cf_stmt_column_text(stmt, 5),
                                  &out->last_active_at);
    }
    if (rc == CF_OK) {
        rc = cf_db_time_from_text(cf_stmt_column_text(stmt, 6),
                                  &out->created_at);
    }
    if (rc == CF_OK) {
        rc = cf_db_time_from_text(cf_stmt_column_text(stmt, 7),
                                  &out->updated_at);
    }
    if (rc != CF_OK) cf_session_dispose(out);
    return rc;
}

static cf_err session_vector_push(cf_session_vector *vector,
                                  cf_session *item) {
    if (vector->len == vector->cap) {
        size_t cap = vector->cap != 0 ? vector->cap * 2 : 8;
        if (cap < vector->cap || cap > SIZE_MAX / sizeof *vector->items) {
            return CF_LIMIT;
        }
        cf_session *items = realloc(vector->items, cap * sizeof *items);
        if (items == NULL) return CF_NOMEM;
        vector->items = items;
        vector->cap = cap;
    }
    vector->items[vector->len++] = *item; /* ownership moves to the vector */
    memset(item, 0, sizeof *item);
    return CF_OK;
}

/* sql::base58(24). */
static cf_err session_generate_token(cf_str *out) {
    static const char alphabet[] =
        "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    enum { TOKEN_LEN = 24, ACCEPT_BELOW = 232 }; /* 232 = 4 * 58 */
    out->ptr = NULL;
    out->len = 0;
    char *token = malloc(TOKEN_LEN + 1);
    if (token == NULL) return CF_NOMEM;

    size_t filled = 0;
    size_t rejected = 0;
    unsigned char batch[4];
    while (filled < (size_t)TOKEN_LEN) {
        cf_err rc = cf_random_bytes(batch, sizeof batch);
        if (rc != CF_OK) {
            free(token);
            return rc;
        }
        for (size_t i = 0; i < sizeof batch && filled < (size_t)TOKEN_LEN;
             i++) {
            if (batch[i] >= ACCEPT_BELOW) {
                /* Keep the character distribution uniform.  A source stuck
                 * above the threshold is broken; fail instead of spinning. */
                if (++rejected > 4096) {
                    free(token);
                    return cf_db_failf(CF_INTERNAL,
                                       "entropy source produced no usable "
                                       "base58 byte");
                }
                continue;
            }
            token[filled++] = alphabet[batch[i] % 58];
        }
    }
    token[TOKEN_LEN] = '\0';
    out->ptr = token;
    out->len = TOKEN_LEN;
    return CF_OK;
}

/* --- record and vector disposal ------------------------------------------- */

void cf_session_dispose(cf_session *session) {
    if (session == NULL) return;
    cf_str_dispose(&session->token);
    cf_optional_str_dispose(&session->ip_address);
    cf_optional_str_dispose(&session->user_agent);
    session->id = 0;
    session->user_id = 0;
    session->last_active_at = 0;
    session->created_at = 0;
    session->updated_at = 0;
}

void cf_session_vector_dispose(cf_session_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len; i++) {
        cf_session_dispose(&vector->items[i]);
    }
    free(vector->items);
    vector->items = NULL;
    vector->len = 0;
    vector->cap = 0;
}

/* --- reads ---------------------------------------------------------------- */

cf_err cf_session_find(cf_db *db, int64_t id, cf_session *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "no output session");
    memset(out, 0, sizeof *out);

    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &session_stmts, SESSION_STMT_FIND, &stmt);
    if (rc != CF_OK) return rc;
    rc = cf_stmt_bind_i64(stmt, 1, id);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            rc = session_from_row(stmt, out);
        } else if (step == SQLITE_DONE) {
            /* Error::RecordNotFound display: "Couldn't find Session". */
            rc = cf_db_failf(CF_NOT_FOUND, "Couldn't find Session");
        } else {
            rc = session_step_failed(db, step, "find session");
        }
    }
    cf_db_stmt_done(stmt);
    return rc;
}

cf_err cf_session_find_by_token(cf_db *db, cf_str token, bool *found,
                                cf_session *out) {
    if (found == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "no output for find_by_token");
    }
    *found = false;
    memset(out, 0, sizeof *out);

    sqlite3_stmt *stmt = NULL;
    cf_err rc =
        cf_db_stmt(db, &session_stmts, SESSION_STMT_FIND_BY_TOKEN, &stmt);
    if (rc != CF_OK) return rc;
    rc = cf_stmt_bind_opt_text(stmt, 1, true,
                               (cf_span){(const unsigned char *)token.ptr,
                                         token.len});
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            rc = session_from_row(stmt, out);
            if (rc == CF_OK) *found = true;
        } else if (step != SQLITE_DONE) {
            rc = session_step_failed(db, step, "find session by token");
        }
    }
    cf_db_stmt_done(stmt);
    return rc;
}

cf_err cf_session_for_user(cf_db *db, int64_t user_id,
                           cf_session_vector *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "no output vector");
    memset(out, 0, sizeof *out);

    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &session_stmts, SESSION_STMT_FOR_USER, &stmt);
    if (rc != CF_OK) return rc;
    rc = cf_stmt_bind_i64(stmt, 1, user_id);
    while (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            rc = session_step_failed(db, step, "sessions for user");
            break;
        }
        cf_session row;
        rc = session_from_row(stmt, &row);
        if (rc != CF_OK) break;
        rc = session_vector_push(out, &row);
        if (rc != CF_OK) cf_session_dispose(&row);
    }
    cf_db_stmt_done(stmt);
    if (rc != CF_OK) cf_session_vector_dispose(out);
    return rc;
}

cf_err cf_session_count_for_user(cf_db *db, int64_t user_id, int64_t *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "no output count");
    *out = 0;

    sqlite3_stmt *stmt = NULL;
    cf_err rc =
        cf_db_stmt(db, &session_stmts, SESSION_STMT_COUNT_FOR_USER, &stmt);
    if (rc != CF_OK) return rc;
    rc = cf_stmt_bind_i64(stmt, 1, user_id);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            *out = cf_stmt_column_i64(stmt, 0);
        } else if (step == SQLITE_DONE) {
            rc = cf_db_failf(CF_INTERNAL, "count returned no row");
        } else {
            rc = session_step_failed(db, step, "count sessions for user");
        }
    }
    cf_db_stmt_done(stmt);
    return rc;
}

/* --- writes --------------------------------------------------------------- */

cf_err cf_session_start(cf_tx *tx, int64_t user_id, cf_optional_str user_agent,
                        cf_optional_str ip_address, cf_session *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "no output session");
    memset(out, 0, sizeof *out);
    if (tx == NULL) return cf_db_failf(CF_INVALID, "no transaction");
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return cf_db_failf(CF_INTERNAL, "transaction has no db");

    int64_t now = cf_now_us(NULL);
    int64_t last_active_at = cf_now_us(NULL);
    char now_text[CF_DB_TIME_TEXT_CAP];
    char last_active_text[CF_DB_TIME_TEXT_CAP];
    cf_err rc = cf_db_time_to_text(now, now_text);
    if (rc != CF_OK) return rc;
    rc = cf_db_time_to_text(last_active_at, last_active_text);
    if (rc != CF_OK) return rc;

    cf_str token;
    rc = session_generate_token(&token);
    if (rc != CF_OK) return rc;

    cf_optional_str ua_copy;
    rc = session_copy_optional_input(user_agent, &ua_copy);
    if (rc != CF_OK) {
        cf_str_dispose(&token);
        return rc;
    }
    cf_optional_str ip_copy;
    rc = session_copy_optional_input(ip_address, &ip_copy);
    if (rc != CF_OK) {
        cf_optional_str_dispose(&ua_copy);
        cf_str_dispose(&token);
        return rc;
    }

    sqlite3_stmt *stmt = NULL;
    rc = cf_db_stmt(db, &session_stmts, SESSION_STMT_START, &stmt);
    if (rc == CF_OK) {
        rc = cf_stmt_bind_text(stmt, 1,
                               (cf_span){(const unsigned char *)now_text,
                                         strlen(now_text)});
    }
    if (rc == CF_OK) {
        rc = cf_stmt_bind_opt_text(
            stmt, 2, ip_copy.present,
            (cf_span){(const unsigned char *)ip_copy.value.ptr,
                      ip_copy.value.len});
    }
    if (rc == CF_OK) {
        rc = cf_stmt_bind_text(stmt, 3,
                               (cf_span){(const unsigned char *)last_active_text,
                                         strlen(last_active_text)});
    }
    if (rc == CF_OK) {
        rc = cf_stmt_bind_text(
            stmt, 4,
            (cf_span){(const unsigned char *)token.ptr, token.len});
    }
    if (rc == CF_OK) {
        rc = cf_stmt_bind_text(stmt, 5,
                               (cf_span){(const unsigned char *)now_text,
                                         strlen(now_text)});
    }
    if (rc == CF_OK) {
        rc = cf_stmt_bind_opt_text(
            stmt, 6, ua_copy.present,
            (cf_span){(const unsigned char *)ua_copy.value.ptr,
                      ua_copy.value.len});
    }
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 7, user_id);

    int64_t id = 0;
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            id = cf_stmt_column_i64(stmt, 0);
            rc = CF_OK;
        } else if (step == SQLITE_DONE) {
            rc = cf_db_failf(CF_INTERNAL, "insert returned no id");
        } else {
            rc = session_step_failed(db, step, "start session");
        }
    }
    if (stmt != NULL) cf_db_stmt_done(stmt);

    if (rc != CF_OK) {
        cf_optional_str_dispose(&ua_copy);
        cf_optional_str_dispose(&ip_copy);
        cf_str_dispose(&token);
        return rc;
    }

    out->id = id;
    out->user_id = user_id;
    out->token = token; /* ownership moves */
    out->ip_address = ip_copy;
    out->user_agent = ua_copy;
    out->last_active_at = last_active_at;
    out->created_at = now;
    out->updated_at = now;
    return CF_OK;
}

bool cf_session_needs_resume(const cf_session *session, int64_t now_us) {
    if (session == NULL) return false;
    return session->last_active_at <
           now_us - CF_SESSION_ACTIVITY_REFRESH_RATE_US;
}

cf_err cf_session_resume(cf_tx *tx, cf_session *session,
                         cf_optional_str user_agent,
                         cf_optional_str ip_address) {
    if (tx == NULL || session == NULL) {
        return cf_db_failf(CF_INVALID, "no transaction or session");
    }
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return cf_db_failf(CF_INTERNAL, "transaction has no db");

    int64_t now = cf_now_us(NULL);
    if (!cf_session_needs_resume(session, now)) return CF_OK;

    cf_optional_str ua_copy;
    cf_err rc = session_copy_optional_input(user_agent, &ua_copy);
    if (rc != CF_OK) return rc;
    cf_optional_str ip_copy;
    rc = session_copy_optional_input(ip_address, &ip_copy);
    if (rc != CF_OK) {
        cf_optional_str_dispose(&ua_copy);
        return rc;
    }

    /* Reference assigns the fields before executing the UPDATE; on SQL
     * failure the refreshed values stay in memory. */
    cf_optional_str_dispose(&session->user_agent);
    session->user_agent = ua_copy;
    cf_optional_str_dispose(&session->ip_address);
    session->ip_address = ip_copy;
    session->last_active_at = now;
    session->updated_at = cf_now_us(NULL);

    char last_active_text[CF_DB_TIME_TEXT_CAP];
    char updated_text[CF_DB_TIME_TEXT_CAP];
    rc = cf_db_time_to_text(session->last_active_at, last_active_text);
    if (rc != CF_OK) return rc;
    rc = cf_db_time_to_text(session->updated_at, updated_text);
    if (rc != CF_OK) return rc;

    sqlite3_stmt *stmt = NULL;
    rc = cf_db_stmt(db, &session_stmts, SESSION_STMT_RESUME, &stmt);
    if (rc == CF_OK) {
        rc = cf_stmt_bind_opt_text(
            stmt, 1, session->ip_address.present,
            (cf_span){(const unsigned char *)session->ip_address.value.ptr,
                      session->ip_address.value.len});
    }
    if (rc == CF_OK) {
        rc = cf_stmt_bind_text(stmt, 2,
                               (cf_span){(const unsigned char *)last_active_text,
                                         strlen(last_active_text)});
    }
    if (rc == CF_OK) {
        rc = cf_stmt_bind_text(stmt, 3,
                               (cf_span){(const unsigned char *)updated_text,
                                         strlen(updated_text)});
    }
    if (rc == CF_OK) {
        rc = cf_stmt_bind_opt_text(
            stmt, 4, session->user_agent.present,
            (cf_span){(const unsigned char *)session->user_agent.value.ptr,
                      session->user_agent.value.len});
    }
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 5, session->id);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step != SQLITE_DONE) {
            rc = session_step_failed(db, step, "resume session");
        }
    }
    if (stmt != NULL) cf_db_stmt_done(stmt);
    return rc;
}

cf_err cf_session_destroy(cf_tx *tx, const cf_session *session) {
    if (tx == NULL || session == NULL) {
        return cf_db_failf(CF_INVALID, "no transaction or session");
    }
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return cf_db_failf(CF_INTERNAL, "transaction has no db");

    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &session_stmts, SESSION_STMT_DESTROY, &stmt);
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 1, session->id);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        /* SQLite reports affected rows only through sqlite3_changes; the
         * reference ignores the count, so deleting an absent row is OK. */
        if (step != SQLITE_DONE) {
            rc = session_step_failed(db, step, "destroy session");
        }
    }
    if (stmt != NULL) cf_db_stmt_done(stmt);
    return rc;
}
