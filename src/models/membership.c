/* src/models/membership.c — D01 model family "membership".
 *
 * Translation of tmp/rust-ref/crates/db/src/models/membership.rs (pinned;
 * SHA-256 in docs/devel/implementation/contracts/reference-files.json).
 * Tables: memberships (schema.sql), joined to rooms for the ordered-room
 * queries.
 *
 * Reference mapping:
 *  - Involvement::{name,from_name}      -> cf_involvement_name / _from_name
 *                                          (declared in models/types.h)
 *  - Membership::{find,count,for_user,for_room,
 *                 find_by_room_and_user,
 *                 visible_with_ordered_room,with_ordered_room,
 *                 count_without_direct_rooms,unread_count,
 *                 connected_exists,disconnected_exists} -> reads (cf_db *)
 *  - connection_cutoff                  -> pure helper
 *  - room,user                          -> delegate to Room/User::find
 *  - involved_in,unread,is_connected    -> pure predicates
 *  - update_involvement,read,destroy,
 *    disconnect_all,connect,present,
 *    connected,disconnected,
 *    refresh_connection                 -> mutations (cf_tx *)
 *  - reload                             -> read of the same id
 *
 * SQL text, ordering, LIMIT 1, NULL handling and parameter order are copied
 * from the source `columns!`/statement strings.  There is no dynamic SQL: the
 * fixed statements below are the module's prepared-statement set.
 *
 * Datetimes: the reference stores Timestamp::to_db text ("YYYY-MM-DD
 * HH:MM:SS" plus ".ffffff" only when microseconds are non-zero) and keeps
 * microseconds in memory; this module uses db-core's cf_db_time_to_text /
 * cf_db_time_from_text.  `tx.now()` in the reference is the environment clock
 * read at call time (Env::now -> Clock::now), so mutations use
 * cf_now_us(NULL): F01's process clock is global and ignores the borrowed app
 * argument, and tests inject it through core/testclock.h (no sleeps).
 *
 * `destroy`'s reference after_commit does `User::find(...).reset_remote_connections`,
 * which emits Event::DisconnectUser { reconnect: true }; the C port appends
 * that event directly with cf_tx_event, exactly as the frozen header comment
 * says ("CF_EVENT_DISCONNECT_USER with reconnect=true").
 */
#include "models/membership.h"

#include "db/db_internal.h"
#include "models/user.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fixed statements for this module, one prepared-statement cache per
 * connection (02-data-auth.md D01).  The enum values are the cache keys. */
enum {
    CF_MEMBERSHIP_STMT_FIND = 0,
    CF_MEMBERSHIP_STMT_COUNT,
    CF_MEMBERSHIP_STMT_FOR_USER,
    CF_MEMBERSHIP_STMT_FOR_ROOM,
    CF_MEMBERSHIP_STMT_FIND_BY_ROOM_AND_USER,
    CF_MEMBERSHIP_STMT_VISIBLE_WITH_ORDERED_ROOM,
    CF_MEMBERSHIP_STMT_WITH_ORDERED_ROOM,
    CF_MEMBERSHIP_STMT_COUNT_WITHOUT_DIRECT_ROOMS,
    CF_MEMBERSHIP_STMT_UNREAD_COUNT,
    CF_MEMBERSHIP_STMT_CONNECTED_EXISTS,
    CF_MEMBERSHIP_STMT_DISCONNECTED_EXISTS,
    CF_MEMBERSHIP_STMT_UPDATE_INVOLVEMENT,
    CF_MEMBERSHIP_STMT_READ,
    CF_MEMBERSHIP_STMT_DESTROY,
    CF_MEMBERSHIP_STMT_DISCONNECT_ALL,
    CF_MEMBERSHIP_STMT_CONNECT,
    CF_MEMBERSHIP_STMT_CLEAR_CONNECTED,
    CF_MEMBERSHIP_STMT_COUNTER_INCREMENT,
    CF_MEMBERSHIP_STMT_COUNTER_DECREMENT,
    CF_MEMBERSHIP_STMT_UPDATE_CONNECTIONS,
    CF_MEMBERSHIP_STMT_TOUCH_CONNECTED_AT
};

/* `membership_columns!()` and `room_columns!()` exactly as the source
 * `columns!` macro expands them: "table"."column", in declaration order. */
#define CF_MEMBERSHIP_COLUMNS                                                 \
    "\"memberships\".\"id\", \"memberships\".\"room_id\", "                    \
    "\"memberships\".\"user_id\", \"memberships\".\"involvement\", "           \
    "\"memberships\".\"unread_at\", \"memberships\".\"connected_at\", "        \
    "\"memberships\".\"connections\", \"memberships\".\"created_at\", "        \
    "\"memberships\".\"updated_at\""

#define CF_MEMBERSHIP_ROOM_COLUMNS                                            \
    "\"rooms\".\"id\", \"rooms\".\"name\", \"rooms\".\"type\", "               \
    "\"rooms\".\"creator_id\", \"rooms\".\"created_at\", "                     \
    "\"rooms\".\"updated_at\""

#define CF_MEMBERSHIP_SELECT "SELECT " CF_MEMBERSHIP_COLUMNS

static const cf_stmt_def cf_membership_stmts[] = {
    /* Rust: Membership::find */
    {CF_MEMBERSHIP_SELECT " FROM \"memberships\" "
                          "WHERE \"memberships\".\"id\" = ? LIMIT 1"},
    /* Rust: Membership::count */
    {"SELECT COUNT(*) FROM \"memberships\""},
    /* Rust: Membership::for_user */
    {CF_MEMBERSHIP_SELECT " FROM \"memberships\" "
                          "WHERE \"memberships\".\"user_id\" = ?"},
    /* Rust: Membership::for_room */
    {CF_MEMBERSHIP_SELECT " FROM \"memberships\" "
                          "WHERE \"memberships\".\"room_id\" = ?"},
    /* Rust: Membership::find_by_room_and_user */
    {CF_MEMBERSHIP_SELECT " FROM \"memberships\" "
                          "WHERE \"memberships\".\"room_id\" = ? "
                          "AND \"memberships\".\"user_id\" = ? LIMIT 1"},
    /* Rust: Membership::visible_with_ordered_room */
    {CF_MEMBERSHIP_SELECT ", " CF_MEMBERSHIP_ROOM_COLUMNS
                          " FROM \"memberships\" "
                          "INNER JOIN \"rooms\" "
                          "ON \"rooms\".\"id\" = \"memberships\".\"room_id\" "
                          "WHERE \"memberships\".\"user_id\" = ? "
                          "AND \"memberships\".\"involvement\" != 'invisible' "
                          "ORDER BY LOWER(rooms.name)"},
    /* Rust: Membership::with_ordered_room */
    {CF_MEMBERSHIP_SELECT ", " CF_MEMBERSHIP_ROOM_COLUMNS
                          " FROM \"memberships\" "
                          "INNER JOIN \"rooms\" "
                          "ON \"rooms\".\"id\" = \"memberships\".\"room_id\" "
                          "WHERE \"memberships\".\"user_id\" = ? "
                          "ORDER BY LOWER(rooms.name)"},
    /* Rust: Membership::count_without_direct_rooms */
    {"SELECT COUNT(*) FROM \"memberships\" "
     "INNER JOIN \"rooms\" \"room\" "
     "ON \"room\".\"id\" = \"memberships\".\"room_id\" "
     "WHERE \"memberships\".\"user_id\" = ? "
     "AND \"room\".\"type\" != 'Rooms::Direct'"},
    /* Rust: Membership::unread_count */
    {"SELECT COUNT(*) FROM \"memberships\" "
     "WHERE \"memberships\".\"user_id\" = ? "
     "AND \"memberships\".\"unread_at\" IS NOT NULL"},
    /* Rust: Membership::connected_exists */
    {"SELECT 1 FROM \"memberships\" "
     "WHERE \"memberships\".\"connected_at\" >= ? "
     "AND \"memberships\".\"id\" = ? LIMIT 1"},
    /* Rust: Membership::disconnected_exists */
    {"SELECT 1 FROM \"memberships\" "
     "WHERE (\"memberships\".\"connected_at\" IS NULL "
     "OR \"memberships\".\"connected_at\" < ?) "
     "AND \"memberships\".\"id\" = ? LIMIT 1"},
    /* Rust: Membership::update_involvement */
    {"UPDATE \"memberships\" SET \"involvement\" = ?, \"updated_at\" = ? "
     "WHERE \"memberships\".\"id\" = ?"},
    /* Rust: Membership::read */
    {"UPDATE \"memberships\" SET \"unread_at\" = ?, \"updated_at\" = ? "
     "WHERE \"memberships\".\"id\" = ?"},
    /* Rust: Membership::destroy */
    {"DELETE FROM \"memberships\" WHERE \"memberships\".\"id\" = ?"},
    /* Rust: Membership::disconnect_all */
    {"UPDATE \"memberships\" SET \"connected_at\" = ?, \"connections\" = ?, "
     "\"updated_at\" = ? WHERE \"memberships\".\"connected_at\" >= ?"},
    /* Rust: Membership::connect */
    {"UPDATE \"memberships\" SET \"connections\" = ?, \"connected_at\" = ?, "
     "\"unread_at\" = ? WHERE \"memberships\".\"id\" = ?"},
    /* Rust: Membership::disconnected (clear connected_at) */
    {"UPDATE \"memberships\" SET \"connected_at\" = ?, \"updated_at\" = ? "
     "WHERE \"memberships\".\"id\" = ?"},
    /* Rust: increment_connections -> increment!(:connections, touch: true) */
    {"UPDATE \"memberships\" SET \"connections\" = "
     "COALESCE(\"memberships\".\"connections\", 0) + ?, \"updated_at\" = ? "
     "WHERE \"memberships\".\"id\" = ?"},
    /* Rust: decrement_connections -> decrement!(:connections) */
    {"UPDATE \"memberships\" SET \"connections\" = "
     "COALESCE(\"memberships\".\"connections\", 0) - ?, \"updated_at\" = ? "
     "WHERE \"memberships\".\"id\" = ?"},
    /* Rust: update_connections -> update!(connections:) */
    {"UPDATE \"memberships\" SET \"connections\" = ?, \"updated_at\" = ? "
     "WHERE \"memberships\".\"id\" = ?"},
    /* Rust: touch_connected_at -> touch :connected_at */
    {"UPDATE \"memberships\" SET \"updated_at\" = ?, \"connected_at\" = ? "
     "WHERE \"memberships\".\"id\" = ?"}};

static const cf_stmt_set cf_membership_stmt_set = {
    cf_membership_stmts,
    sizeof cf_membership_stmts / sizeof cf_membership_stmts[0]};

/* --- involvement helpers (declared in models/types.h) --------------------- */

const char *cf_involvement_name(cf_involvement involvement) {
    switch (involvement) {
    case CF_INVOLVEMENT_INVISIBLE:
        return "invisible";
    case CF_INVOLVEMENT_NOTHING:
        return "nothing";
    case CF_INVOLVEMENT_MENTIONS:
        return "mentions";
    case CF_INVOLVEMENT_EVERYTHING:
        return "everything";
    }
    return NULL;
}

bool cf_involvement_from_name(cf_str name, cf_involvement *out) {
    if (out == NULL || (name.len != 0 && name.ptr == NULL)) return false;
    static const struct {
        const char *text;
        cf_involvement value;
    } names[] = {
        {"invisible", CF_INVOLVEMENT_INVISIBLE},
        {"nothing", CF_INVOLVEMENT_NOTHING},
        {"mentions", CF_INVOLVEMENT_MENTIONS},
        {"everything", CF_INVOLVEMENT_EVERYTHING},
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        size_t len = strlen(names[i].text);
        if (name.len == len && (len == 0 || memcmp(name.ptr, names[i].text, len) == 0)) {
            *out = names[i].value;
            return true;
        }
    }
    return false;
}

/* --- small shared helpers ------------------------------------------------- */

/* Borrowed span over a NUL-terminated buffer (fixed datetime text). */
static cf_span membership_cstr_span(const char *text) {
    cf_span span;
    span.ptr = (const unsigned char *)text;
    span.len = strlen(text);
    return span;
}

/* Owned copy of a borrowed span: NUL-terminated, len excludes the NUL. */
static cf_err membership_str_copy(cf_span src, cf_str *out) {
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

/* Copy a nullable text column into an optional string; NULL stays absent and
 * an empty string stays present-but-empty. */
static cf_err membership_read_optional_text(sqlite3_stmt *stmt, int column,
                                            cf_optional_str *out) {
    out->present = false;
    out->value.ptr = NULL;
    out->value.len = 0;
    if (cf_stmt_column_is_null(stmt, column)) return CF_OK;
    cf_err rc =
        membership_str_copy(cf_stmt_column_text(stmt, column), &out->value);
    if (rc == CF_OK) out->present = true;
    return rc;
}

/* Read a NOT NULL datetime(6) text column into UTC microseconds. */
static cf_err membership_read_required_time(sqlite3_stmt *stmt, int column,
                                            const char *name,
                                            int64_t *out_us) {
    if (cf_stmt_column_is_null(stmt, column)) {
        return cf_db_failf(CF_DB, "%s is NULL", name);
    }
    cf_span text = cf_stmt_column_text(stmt, column);
    if (text.ptr == NULL ||
        cf_db_time_from_text(text, out_us) != CF_OK) {
        return cf_db_failf(CF_DB, "%s is not valid datetime text", name);
    }
    return CF_OK;
}

/* Read a nullable datetime(6) text column into cf_optional_i64. */
static cf_err membership_read_optional_time(sqlite3_stmt *stmt, int column,
                                            const char *name,
                                            cf_optional_i64 *out) {
    out->present = false;
    out->value = 0;
    if (cf_stmt_column_is_null(stmt, column)) return CF_OK;
    cf_span text = cf_stmt_column_text(stmt, column);
    if (text.ptr == NULL || cf_db_time_from_text(text, &out->value) != CF_OK) {
        return cf_db_failf(CF_DB, "%s is not valid datetime text", name);
    }
    out->present = true;
    return CF_OK;
}

/* Read the nullable involvement enum text column. */
static cf_err membership_read_involvement(sqlite3_stmt *stmt, int column,
                                          cf_optional_involvement *out) {
    out->present = false;
    out->value = CF_INVOLVEMENT_MENTIONS;
    if (cf_stmt_column_is_null(stmt, column)) return CF_OK;
    cf_span text = cf_stmt_column_text(stmt, column);
    cf_str name = {text.len != 0 ? (char *)text.ptr : NULL, text.len};
    if (text.ptr == NULL ||
        !cf_involvement_from_name(name, &out->value)) {
        return cf_db_failf(CF_DB,
                           "memberships.involvement is not a known "
                           "involvement");
    }
    out->present = true;
    return CF_OK;
}

/* One failed step: copy the connection message before resetting the
 * statement, then report it with the mapped code. */
static cf_err membership_step_failure(cf_db *db, sqlite3_stmt *stmt,
                                      int sqlite_rc, const char *operation) {
    char message[CF_DB_ERROR_CAP];
    snprintf(message, sizeof message, "%s", sqlite3_errmsg(cf_db_handle(db)));
    cf_db_stmt_done(stmt);
    return cf_db_failf(cf_db_err(sqlite_rc), "%s: %s", operation, message);
}

/* Read one membership from columns offset..offset+8.  The caller passes the
 * statement to cf_db_stmt_done only after this returns. */
static cf_err membership_read_row(sqlite3_stmt *stmt, size_t offset,
                                  cf_membership *out) {
    out->id = cf_stmt_column_i64(stmt, (int)offset + 0);
    out->room_id = cf_stmt_column_i64(stmt, (int)offset + 1);
    out->user_id = cf_stmt_column_i64(stmt, (int)offset + 2);
    out->connections = cf_stmt_column_i64(stmt, (int)offset + 6);
    cf_err rc = membership_read_involvement(stmt, (int)offset + 3,
                                            &out->involvement);
    if (rc == CF_OK) {
        rc = membership_read_optional_time(stmt, (int)offset + 4,
                                           "memberships.unread_at",
                                           &out->unread_at);
    }
    if (rc == CF_OK) {
        rc = membership_read_optional_time(stmt, (int)offset + 5,
                                           "memberships.connected_at",
                                           &out->connected_at);
    }
    if (rc == CF_OK) {
        rc = membership_read_required_time(stmt, (int)offset + 7,
                                           "memberships.created_at",
                                           &out->created_at);
    }
    if (rc == CF_OK) {
        rc = membership_read_required_time(stmt, (int)offset + 8,
                                           "memberships.updated_at",
                                           &out->updated_at);
    }
    return rc;
}

/* Read one room from columns offset..offset+5 (source room_columns! order). */
static cf_err membership_read_room_row(sqlite3_stmt *stmt, size_t offset,
                                       cf_room *out) {
    out->id = cf_stmt_column_i64(stmt, (int)offset + 0);
    out->creator_id = cf_stmt_column_i64(stmt, (int)offset + 3);
    cf_err rc = membership_read_optional_text(stmt, (int)offset + 1,
                                              &out->name);
    if (rc == CF_OK) {
        cf_span text = cf_stmt_column_text(stmt, (int)offset + 2);
        cf_str class_name = {text.len != 0 ? (char *)text.ptr : NULL,
                             text.len};
        if (text.ptr == NULL ||
            !cf_room_type_from_class_name(class_name, &out->room_type)) {
            rc = cf_db_failf(CF_DB,
                             "rooms.type is not a known room type");
        }
    }
    if (rc == CF_OK) {
        rc = membership_read_required_time(stmt, (int)offset + 4,
                                           "rooms.created_at",
                                           &out->created_at);
    }
    if (rc == CF_OK) {
        rc = membership_read_required_time(stmt, (int)offset + 5,
                                           "rooms.updated_at",
                                           &out->updated_at);
    }
    return rc;
}

/* (Membership, Room) from membership_columns!() then room_columns!(). */
static cf_err membership_read_pair_row(sqlite3_stmt *stmt,
                                       cf_membership_room_pair *out) {
    cf_err rc = membership_read_row(stmt, 0, &out->membership);
    if (rc != CF_OK) return rc;
    rc = membership_read_room_row(stmt, 9, &out->room);
    if (rc != CF_OK) cf_membership_dispose(&out->membership);
    return rc;
}

static cf_err membership_vector_reserve(cf_membership_vector *vector) {
    size_t cap = vector->cap == 0 ? 4 : vector->cap * 2;
    if (cap < vector->cap || cap > SIZE_MAX / sizeof *vector->items) {
        return cf_db_failf(CF_LIMIT, "too many membership rows");
    }
    cf_membership *items = realloc(vector->items, cap * sizeof *items);
    if (items == NULL) return CF_NOMEM;
    vector->items = items;
    vector->cap = cap;
    return CF_OK;
}

static cf_err membership_pair_vector_reserve(
    cf_membership_room_pair_vector *vector) {
    size_t cap = vector->cap == 0 ? 4 : vector->cap * 2;
    if (cap < vector->cap || cap > SIZE_MAX / sizeof *vector->items) {
        return cf_db_failf(CF_LIMIT, "too many membership rows");
    }
    cf_membership_room_pair *items =
        realloc(vector->items, cap * sizeof *items);
    if (items == NULL) return CF_NOMEM;
    vector->items = items;
    vector->cap = cap;
    return CF_OK;
}

/* Bind an optional involvement: its stored name text, or SQL NULL. */
static cf_err membership_bind_involvement(sqlite3_stmt *stmt, int index,
                                          cf_optional_involvement involvement) {
    if (!involvement.present) return cf_stmt_bind_null(stmt, index);
    const char *name = cf_involvement_name(involvement.value);
    if (name == NULL) {
        return cf_db_failf(CF_INVALID, "unknown involvement value");
    }
    return cf_stmt_bind_text(stmt, index, membership_cstr_span(name));
}

/* --- disposal ------------------------------------------------------------- */

void cf_membership_dispose(cf_membership *membership) {
    if (membership == NULL) return;
    /* The record owns no heap (all fields are scalars/enums); reset it so a
     * double dispose is harmless. */
    *membership = (cf_membership){0};
}

void cf_membership_vector_dispose(cf_membership_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len; i++) {
        cf_membership_dispose(&vector->items[i]);
    }
    free(vector->items);
    vector->items = NULL;
    vector->len = 0;
    vector->cap = 0;
}

void cf_membership_room_pair_dispose(cf_membership_room_pair *pair) {
    if (pair == NULL) return;
    cf_membership_dispose(&pair->membership);
    cf_room_dispose(&pair->room);
    *pair = (cf_membership_room_pair){0};
}

void cf_membership_room_pair_vector_dispose(
    cf_membership_room_pair_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len; i++) {
        cf_membership_room_pair_dispose(&vector->items[i]);
    }
    free(vector->items);
    vector->items = NULL;
    vector->len = 0;
    vector->cap = 0;
}

/* --- reads ---------------------------------------------------------------- */

cf_err cf_membership_find(cf_db *db, int64_t id, cf_membership *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "find: no output pointer");
    }
    *out = (cf_membership){0};
    if (db == NULL) return cf_db_failf(CF_INVALID, "find: no database");

    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &cf_membership_stmt_set, CF_MEMBERSHIP_STMT_FIND,
                           &stmt);
    if (rc != CF_OK) return rc;

    rc = cf_stmt_bind_i64(stmt, 1, id);
    if (rc != CF_OK) {
        cf_db_stmt_done(stmt);
        return rc;
    }

    int step = sqlite3_step(stmt);
    if (step == SQLITE_DONE) {
        /* query_one(...).or_not_found("Membership") */
        cf_db_stmt_done(stmt);
        return cf_db_failf(CF_NOT_FOUND, "Couldn't find Membership");
    }
    if (step != SQLITE_ROW) {
        return membership_step_failure(db, stmt, step, "find");
    }
    rc = membership_read_row(stmt, 0, out);
    cf_db_stmt_done(stmt);
    if (rc != CF_OK) {
        cf_membership_dispose(out);
        return rc;
    }
    return CF_OK;
}

cf_err cf_membership_count(cf_db *db, int64_t *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "count: no output pointer");
    }
    *out = 0;
    if (db == NULL) return cf_db_failf(CF_INVALID, "count: no database");

    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &cf_membership_stmt_set,
                           CF_MEMBERSHIP_STMT_COUNT, &stmt);
    if (rc != CF_OK) return rc;

    int step = sqlite3_step(stmt);
    if (step != SQLITE_ROW) {
        return membership_step_failure(db, stmt, step, "count");
    }
    *out = cf_stmt_column_i64(stmt, 0);
    cf_db_stmt_done(stmt);
    return CF_OK;
}

/* Shared body of for_user/for_room: one bound id, then every row. */
static cf_err membership_query_vector(cf_db *db, size_t stmt_id, int64_t bound,
                                      cf_membership_vector *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "query: no output pointer");
    }
    *out = (cf_membership_vector){0};
    if (db == NULL) return cf_db_failf(CF_INVALID, "query: no database");

    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &cf_membership_stmt_set, stmt_id, &stmt);
    if (rc != CF_OK) return rc;
    rc = cf_stmt_bind_i64(stmt, 1, bound);
    if (rc != CF_OK) {
        cf_db_stmt_done(stmt);
        return rc;
    }

    for (;;) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            cf_err failure =
                membership_step_failure(db, stmt, step, "query rows");
            cf_membership_vector_dispose(out);
            return failure;
        }
        if (out->len == out->cap) {
            rc = membership_vector_reserve(out);
            if (rc != CF_OK) {
                cf_db_stmt_done(stmt);
                cf_membership_vector_dispose(out);
                return rc;
            }
        }
        cf_membership *row = &out->items[out->len];
        *row = (cf_membership){0};
        rc = membership_read_row(stmt, 0, row);
        if (rc != CF_OK) {
            cf_db_stmt_done(stmt);
            cf_membership_vector_dispose(out);
            return rc;
        }
        out->len++;
    }

    cf_db_stmt_done(stmt);
    return CF_OK;
}

cf_err cf_membership_for_user(cf_db *db, int64_t user_id,
                              cf_membership_vector *out) {
    return membership_query_vector(db, CF_MEMBERSHIP_STMT_FOR_USER, user_id,
                                   out);
}

cf_err cf_membership_for_room(cf_db *db, int64_t room_id,
                              cf_membership_vector *out) {
    return membership_query_vector(db, CF_MEMBERSHIP_STMT_FOR_ROOM, room_id,
                                   out);
}

cf_err cf_membership_find_by_room_and_user(cf_db *db, int64_t room_id,
                                           int64_t user_id, bool *found,
                                           cf_membership *out) {
    if (found == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "find_by_room_and_user: no output");
    }
    *found = false;
    *out = (cf_membership){0};
    if (db == NULL) {
        return cf_db_failf(CF_INVALID,
                           "find_by_room_and_user: no database");
    }

    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &cf_membership_stmt_set,
                           CF_MEMBERSHIP_STMT_FIND_BY_ROOM_AND_USER, &stmt);
    if (rc != CF_OK) return rc;

    rc = cf_stmt_bind_i64(stmt, 1, room_id);
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 2, user_id);
    if (rc != CF_OK) {
        cf_db_stmt_done(stmt);
        return rc;
    }

    int step = sqlite3_step(stmt);
    if (step == SQLITE_DONE) {
        /* query_one(...) -> None is not an error */
        cf_db_stmt_done(stmt);
        return CF_OK;
    }
    if (step != SQLITE_ROW) {
        return membership_step_failure(db, stmt, step,
                                       "find_by_room_and_user");
    }
    rc = membership_read_row(stmt, 0, out);
    cf_db_stmt_done(stmt);
    if (rc != CF_OK) {
        cf_membership_dispose(out);
        return rc;
    }
    *found = true;
    return CF_OK;
}

/* Shared body of visible_with_ordered_room/with_ordered_room. */
static cf_err membership_ordered_room_query(
    cf_db *db, size_t stmt_id, int64_t user_id,
    cf_membership_room_pair_vector *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "ordered rooms: no output pointer");
    }
    *out = (cf_membership_room_pair_vector){0};
    if (db == NULL) {
        return cf_db_failf(CF_INVALID, "ordered rooms: no database");
    }

    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &cf_membership_stmt_set, stmt_id, &stmt);
    if (rc != CF_OK) return rc;
    rc = cf_stmt_bind_i64(stmt, 1, user_id);
    if (rc != CF_OK) {
        cf_db_stmt_done(stmt);
        return rc;
    }

    for (;;) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            cf_err failure =
                membership_step_failure(db, stmt, step, "ordered rooms");
            cf_membership_room_pair_vector_dispose(out);
            return failure;
        }
        if (out->len == out->cap) {
            rc = membership_pair_vector_reserve(out);
            if (rc != CF_OK) {
                cf_db_stmt_done(stmt);
                cf_membership_room_pair_vector_dispose(out);
                return rc;
            }
        }
        cf_membership_room_pair *row = &out->items[out->len];
        *row = (cf_membership_room_pair){0};
        rc = membership_read_pair_row(stmt, row);
        if (rc != CF_OK) {
            cf_db_stmt_done(stmt);
            cf_membership_room_pair_vector_dispose(out);
            return rc;
        }
        out->len++;
    }

    cf_db_stmt_done(stmt);
    return CF_OK;
}

cf_err cf_membership_visible_with_ordered_room(
    cf_db *db, int64_t user_id, cf_membership_room_pair_vector *out) {
    return membership_ordered_room_query(
        db, CF_MEMBERSHIP_STMT_VISIBLE_WITH_ORDERED_ROOM, user_id, out);
}

cf_err cf_membership_with_ordered_room(cf_db *db, int64_t user_id,
                                       cf_membership_room_pair_vector *out) {
    return membership_ordered_room_query(
        db, CF_MEMBERSHIP_STMT_WITH_ORDERED_ROOM, user_id, out);
}

cf_err cf_membership_count_without_direct_rooms(cf_db *db, int64_t user_id,
                                                int64_t *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID,
                           "count_without_direct_rooms: no output pointer");
    }
    *out = 0;
    if (db == NULL) {
        return cf_db_failf(CF_INVALID,
                           "count_without_direct_rooms: no database");
    }

    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &cf_membership_stmt_set,
                           CF_MEMBERSHIP_STMT_COUNT_WITHOUT_DIRECT_ROOMS,
                           &stmt);
    if (rc != CF_OK) return rc;
    rc = cf_stmt_bind_i64(stmt, 1, user_id);
    if (rc != CF_OK) {
        cf_db_stmt_done(stmt);
        return rc;
    }

    int step = sqlite3_step(stmt);
    if (step != SQLITE_ROW) {
        return membership_step_failure(db, stmt, step,
                                       "count_without_direct_rooms");
    }
    *out = cf_stmt_column_i64(stmt, 0);
    cf_db_stmt_done(stmt);
    return CF_OK;
}

cf_err cf_membership_unread_count(cf_db *db, int64_t user_id, int64_t *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "unread_count: no output pointer");
    }
    *out = 0;
    if (db == NULL) {
        return cf_db_failf(CF_INVALID, "unread_count: no database");
    }

    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &cf_membership_stmt_set,
                           CF_MEMBERSHIP_STMT_UNREAD_COUNT, &stmt);
    if (rc != CF_OK) return rc;
    rc = cf_stmt_bind_i64(stmt, 1, user_id);
    if (rc != CF_OK) {
        cf_db_stmt_done(stmt);
        return rc;
    }

    int step = sqlite3_step(stmt);
    if (step != SQLITE_ROW) {
        return membership_step_failure(db, stmt, step, "unread_count");
    }
    *out = cf_stmt_column_i64(stmt, 0);
    cf_db_stmt_done(stmt);
    return CF_OK;
}

/* Shared body of connected_exists/disconnected_exists. */
static cf_err membership_exists_query(cf_db *db, size_t stmt_id, int64_t id,
                                      int64_t now_us, const char *operation,
                                      bool *out) {
    int64_t cutoff = cf_membership_connection_cutoff(now_us);
    char cutoff_text[CF_DB_TIME_TEXT_CAP];
    cf_err rc = cf_db_time_to_text(cutoff, cutoff_text);
    if (rc != CF_OK) return rc;

    sqlite3_stmt *stmt = NULL;
    rc = cf_db_stmt(db, &cf_membership_stmt_set, stmt_id, &stmt);
    if (rc != CF_OK) return rc;

    /* params![Self::connection_cutoff(now), id] */
    rc = cf_stmt_bind_text(stmt, 1, membership_cstr_span(cutoff_text));
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 2, id);
    if (rc != CF_OK) {
        cf_db_stmt_done(stmt);
        return rc;
    }

    int step = sqlite3_step(stmt);
    if (step == SQLITE_DONE) {
        *out = false;
        cf_db_stmt_done(stmt);
        return CF_OK;
    }
    if (step != SQLITE_ROW) {
        return membership_step_failure(db, stmt, step, operation);
    }
    *out = true;
    cf_db_stmt_done(stmt);
    return CF_OK;
}

cf_err cf_membership_connected_exists(cf_db *db, int64_t id, int64_t now_us,
                                      bool *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID,
                           "connected_exists: no output pointer");
    }
    *out = false;
    if (db == NULL) {
        return cf_db_failf(CF_INVALID, "connected_exists: no database");
    }
    return membership_exists_query(db, CF_MEMBERSHIP_STMT_CONNECTED_EXISTS, id,
                                   now_us, "connected_exists", out);
}

cf_err cf_membership_disconnected_exists(cf_db *db, int64_t id, int64_t now_us,
                                         bool *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID,
                           "disconnected_exists: no output pointer");
    }
    *out = false;
    if (db == NULL) {
        return cf_db_failf(CF_INVALID, "disconnected_exists: no database");
    }
    return membership_exists_query(db, CF_MEMBERSHIP_STMT_DISCONNECTED_EXISTS,
                                   id, now_us, "disconnected_exists", out);
}

int64_t cf_membership_connection_cutoff(int64_t now_us) {
    /* CONNECTION_TTL.ago in the source. */
    return now_us - CF_MEMBERSHIP_CONNECTION_TTL_US;
}

cf_err cf_membership_room(cf_db *db, const cf_membership *membership,
                          cf_room *out) {
    if (membership == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "room: no membership or output");
    }
    return cf_room_find(db, membership->room_id, out);
}

cf_err cf_membership_user(cf_db *db, const cf_membership *membership,
                          cf_user *out) {
    if (membership == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "user: no membership or output");
    }
    return cf_user_find(db, membership->user_id, out);
}

bool cf_membership_involved_in(const cf_membership *membership,
                               cf_involvement involvement) {
    return membership != NULL && membership->involvement.present &&
           membership->involvement.value == involvement;
}

bool cf_membership_unread(const cf_membership *membership) {
    return membership != NULL && membership->unread_at.present;
}

/* --- mutations ------------------------------------------------------------ */

/* Shared frame of the simple UPDATE ... WHERE id = ? mutations: prepare,
 * bind the trailing id, step to DONE, reset.  The caller binds any leading
 * parameters first. */
static cf_err membership_execute_update(cf_db *db, size_t stmt_id,
                                        sqlite3_stmt **stmt_out) {
    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &cf_membership_stmt_set, stmt_id, &stmt);
    if (rc != CF_OK) return rc;
    *stmt_out = stmt;
    return CF_OK;
}

static cf_err membership_finish_update(cf_db *db, sqlite3_stmt *stmt,
                                       int step, const char *operation) {
    if (step != SQLITE_DONE) {
        return membership_step_failure(db, stmt, step, operation);
    }
    cf_db_stmt_done(stmt);
    return CF_OK;
}

cf_err cf_membership_update_involvement(
    cf_tx *tx, cf_membership *membership, cf_optional_involvement involvement) {
    if (membership == NULL) {
        return cf_db_failf(CF_INVALID,
                           "update_involvement: no membership");
    }
    if (tx == NULL) {
        return cf_db_failf(CF_INVALID,
                           "update_involvement: no transaction");
    }
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) {
        return cf_db_failf(CF_INVALID, "update_involvement: no database");
    }

    /* `if self.involvement == involvement { return Ok(()) }` — no touch. */
    if (membership->involvement.present == involvement.present &&
        (!involvement.present ||
         membership->involvement.value == involvement.value)) {
        return CF_OK;
    }

    int64_t now = cf_now_us(NULL);
    char now_text[CF_DB_TIME_TEXT_CAP];
    cf_err rc = cf_db_time_to_text(now, now_text);
    if (rc != CF_OK) return rc;

    sqlite3_stmt *stmt = NULL;
    rc = membership_execute_update(db, CF_MEMBERSHIP_STMT_UPDATE_INVOLVEMENT,
                                   &stmt);
    if (rc != CF_OK) return rc;

    /* params![involvement, now, self.id] */
    rc = membership_bind_involvement(stmt, 1, involvement);
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 2,
                                            membership_cstr_span(now_text));
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 3, membership->id);
    if (rc != CF_OK) {
        cf_db_stmt_done(stmt);
        return rc;
    }

    rc = membership_finish_update(db, stmt, sqlite3_step(stmt),
                                  "update_involvement");
    if (rc != CF_OK) return rc;

    membership->involvement = involvement;
    membership->updated_at = now;
    return CF_OK;
}

cf_err cf_membership_read(cf_tx *tx, cf_membership *membership) {
    if (membership == NULL) {
        return cf_db_failf(CF_INVALID, "read: no membership");
    }
    if (tx == NULL) return cf_db_failf(CF_INVALID, "read: no transaction");
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return cf_db_failf(CF_INVALID, "read: no database");

    /* `if self.unread_at.is_none() { return Ok(()) }` — no touch. */
    if (!membership->unread_at.present) return CF_OK;

    int64_t now = cf_now_us(NULL);
    char now_text[CF_DB_TIME_TEXT_CAP];
    cf_err rc = cf_db_time_to_text(now, now_text);
    if (rc != CF_OK) return rc;

    sqlite3_stmt *stmt = NULL;
    rc = membership_execute_update(db, CF_MEMBERSHIP_STMT_READ, &stmt);
    if (rc != CF_OK) return rc;

    /* params![None::<Timestamp>, now, self.id] */
    rc = cf_stmt_bind_null(stmt, 1);
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 2,
                                            membership_cstr_span(now_text));
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 3, membership->id);
    if (rc != CF_OK) {
        cf_db_stmt_done(stmt);
        return rc;
    }

    rc = membership_finish_update(db, stmt, sqlite3_step(stmt), "read");
    if (rc != CF_OK) return rc;

    membership->unread_at.present = false;
    membership->unread_at.value = 0;
    membership->updated_at = now;
    return CF_OK;
}

cf_err cf_membership_destroy(cf_tx *tx, const cf_membership *membership) {
    if (membership == NULL) {
        return cf_db_failf(CF_INVALID, "destroy: no membership");
    }
    if (tx == NULL) return cf_db_failf(CF_INVALID, "destroy: no transaction");
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return cf_db_failf(CF_INVALID, "destroy: no database");

    sqlite3_stmt *stmt = NULL;
    cf_err rc = membership_execute_update(db, CF_MEMBERSHIP_STMT_DESTROY, &stmt);
    if (rc != CF_OK) return rc;

    rc = cf_stmt_bind_i64(stmt, 1, membership->id);
    if (rc != CF_OK) {
        cf_db_stmt_done(stmt);
        return rc;
    }

    rc = membership_finish_update(db, stmt, sqlite3_step(stmt), "destroy");
    if (rc != CF_OK) return rc;

    /* `tx.after_commit(move |tx| User::find(...).reset_remote_connections(tx))`
     * delivers Event::DisconnectUser { reconnect: true } after commit; the
     * C event is appended to the transaction by cf_tx_event. */
    cf_event event = {0};
    event.kind = CF_EVENT_DISCONNECT_USER;
    event.user_id = membership->user_id;
    event.reconnect = true;
    return cf_tx_event(tx, event);
}

cf_err cf_membership_disconnect_all(cf_tx *tx, size_t *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "disconnect_all: no output pointer");
    }
    *out = 0;
    if (tx == NULL) {
        return cf_db_failf(CF_INVALID, "disconnect_all: no transaction");
    }
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) {
        return cf_db_failf(CF_INVALID, "disconnect_all: no database");
    }

    int64_t now = cf_now_us(NULL);
    int64_t cutoff = cf_membership_connection_cutoff(now);
    char now_text[CF_DB_TIME_TEXT_CAP];
    char cutoff_text[CF_DB_TIME_TEXT_CAP];
    cf_err rc = cf_db_time_to_text(now, now_text);
    if (rc == CF_OK) rc = cf_db_time_to_text(cutoff, cutoff_text);
    if (rc != CF_OK) return rc;

    sqlite3_stmt *stmt = NULL;
    rc = membership_execute_update(db, CF_MEMBERSHIP_STMT_DISCONNECT_ALL,
                                   &stmt);
    if (rc != CF_OK) return rc;

    /* params![None::<Timestamp>, 0, now, Self::connection_cutoff(now)] */
    rc = cf_stmt_bind_null(stmt, 1);
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 2, 0);
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 3,
                                            membership_cstr_span(now_text));
    if (rc == CF_OK) {
        rc = cf_stmt_bind_text(stmt, 4, membership_cstr_span(cutoff_text));
    }
    if (rc != CF_OK) {
        cf_db_stmt_done(stmt);
        return rc;
    }

    int step = sqlite3_step(stmt);
    if (step != SQLITE_DONE) {
        return membership_step_failure(db, stmt, step, "disconnect_all");
    }
    int changed = sqlite3_changes(cf_db_handle(db));
    cf_db_stmt_done(stmt);
    *out = changed > 0 ? (size_t)changed : 0;
    return CF_OK;
}

cf_err cf_membership_connect(cf_tx *tx, int64_t id, int64_t connections) {
    if (tx == NULL) return cf_db_failf(CF_INVALID, "connect: no transaction");
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return cf_db_failf(CF_INVALID, "connect: no database");

    /* No updated_at write on purpose: the source does not touch it here. */
    int64_t now = cf_now_us(NULL);
    char now_text[CF_DB_TIME_TEXT_CAP];
    cf_err rc = cf_db_time_to_text(now, now_text);
    if (rc != CF_OK) return rc;

    sqlite3_stmt *stmt = NULL;
    rc = membership_execute_update(db, CF_MEMBERSHIP_STMT_CONNECT, &stmt);
    if (rc != CF_OK) return rc;

    /* params![connections, tx.now(), None::<Timestamp>, id] */
    rc = cf_stmt_bind_i64(stmt, 1, connections);
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 2,
                                            membership_cstr_span(now_text));
    if (rc == CF_OK) rc = cf_stmt_bind_null(stmt, 3);
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 4, id);
    if (rc != CF_OK) {
        cf_db_stmt_done(stmt);
        return rc;
    }
    return membership_finish_update(db, stmt, sqlite3_step(stmt), "connect");
}

bool cf_membership_is_connected(const cf_membership *membership,
                                int64_t now_us) {
    /* at >= Self::connection_cutoff(now) */
    return membership != NULL && membership->connected_at.present &&
           membership->connected_at.value >=
               cf_membership_connection_cutoff(now_us);
}

cf_err cf_membership_present(cf_tx *tx, cf_membership *membership) {
    if (membership == NULL) {
        return cf_db_failf(CF_INVALID, "present: no membership");
    }
    if (tx == NULL) return cf_db_failf(CF_INVALID, "present: no transaction");

    int64_t now = cf_now_us(NULL);
    /* `let connections = if self.is_connected(tx.now()) { self.connections + 1 } else { 1 };`
     * Self::connect writes the row only; the in-memory record is not touched. */
    int64_t connections = cf_membership_is_connected(membership, now)
                              ? membership->connections + 1
                              : 1;
    return cf_membership_connect(tx, membership->id, connections);
}

/* `increment!(:connections, touch: true)` / `decrement!` */
static cf_err membership_update_counter(cf_tx *tx, cf_membership *membership,
                                        int64_t by) {
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) {
        return cf_db_failf(CF_INVALID, "update_counter: no database");
    }

    int64_t now = cf_now_us(NULL);
    char now_text[CF_DB_TIME_TEXT_CAP];
    cf_err rc = cf_db_time_to_text(now, now_text);
    if (rc != CF_OK) return rc;

    size_t stmt_id = by >= 0 ? CF_MEMBERSHIP_STMT_COUNTER_INCREMENT
                             : CF_MEMBERSHIP_STMT_COUNTER_DECREMENT;
    sqlite3_stmt *stmt = NULL;
    rc = membership_execute_update(db, stmt_id, &stmt);
    if (rc != CF_OK) return rc;

    /* params![by.abs(), now, self.id] */
    rc = cf_stmt_bind_i64(stmt, 1, by >= 0 ? by : -by);
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 2,
                                            membership_cstr_span(now_text));
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 3, membership->id);
    if (rc != CF_OK) {
        cf_db_stmt_done(stmt);
        return rc;
    }

    rc = membership_finish_update(db, stmt, sqlite3_step(stmt),
                                  "update_counter");
    if (rc != CF_OK) return rc;

    membership->connections += by;
    membership->updated_at = now;
    return CF_OK;
}

/* `update!(connections:)`, a no-op when unchanged. */
static cf_err membership_update_connections(cf_tx *tx,
                                            cf_membership *membership,
                                            int64_t connections) {
    if (membership->connections == connections) return CF_OK;
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) {
        return cf_db_failf(CF_INVALID, "update_connections: no database");
    }

    int64_t now = cf_now_us(NULL);
    char now_text[CF_DB_TIME_TEXT_CAP];
    cf_err rc = cf_db_time_to_text(now, now_text);
    if (rc != CF_OK) return rc;

    sqlite3_stmt *stmt = NULL;
    rc = membership_execute_update(db, CF_MEMBERSHIP_STMT_UPDATE_CONNECTIONS,
                                   &stmt);
    if (rc != CF_OK) return rc;

    /* params![connections, now, self.id] */
    rc = cf_stmt_bind_i64(stmt, 1, connections);
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 2,
                                            membership_cstr_span(now_text));
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 3, membership->id);
    if (rc != CF_OK) {
        cf_db_stmt_done(stmt);
        return rc;
    }

    rc = membership_finish_update(db, stmt, sqlite3_step(stmt),
                                  "update_connections");
    if (rc != CF_OK) return rc;

    membership->connections = connections;
    membership->updated_at = now;
    return CF_OK;
}

static cf_err membership_increment_connections(cf_tx *tx,
                                               cf_membership *membership) {
    int64_t now = cf_now_us(NULL);
    return cf_membership_is_connected(membership, now)
               ? membership_update_counter(tx, membership, 1)
               : membership_update_connections(tx, membership, 1);
}

static cf_err membership_decrement_connections(cf_tx *tx,
                                               cf_membership *membership) {
    int64_t now = cf_now_us(NULL);
    return cf_membership_is_connected(membership, now)
               ? membership_update_counter(tx, membership, -1)
               : membership_update_connections(tx, membership, 0);
}

/* `touch :connected_at` */
static cf_err membership_touch_connected_at(cf_tx *tx,
                                            cf_membership *membership) {
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) {
        return cf_db_failf(CF_INVALID, "touch_connected_at: no database");
    }

    int64_t now = cf_now_us(NULL);
    char now_text[CF_DB_TIME_TEXT_CAP];
    cf_err rc = cf_db_time_to_text(now, now_text);
    if (rc != CF_OK) return rc;

    sqlite3_stmt *stmt = NULL;
    rc = membership_execute_update(db, CF_MEMBERSHIP_STMT_TOUCH_CONNECTED_AT,
                                   &stmt);
    if (rc != CF_OK) return rc;

    /* params![now, now, self.id] */
    rc = cf_stmt_bind_text(stmt, 1, membership_cstr_span(now_text));
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 2,
                                            membership_cstr_span(now_text));
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 3, membership->id);
    if (rc != CF_OK) {
        cf_db_stmt_done(stmt);
        return rc;
    }

    rc = membership_finish_update(db, stmt, sqlite3_step(stmt),
                                  "touch_connected_at");
    if (rc != CF_OK) return rc;

    membership->connected_at.present = true;
    membership->connected_at.value = now;
    membership->updated_at = now;
    return CF_OK;
}

cf_err cf_membership_connected(cf_tx *tx, cf_membership *membership) {
    if (membership == NULL) {
        return cf_db_failf(CF_INVALID, "connected: no membership");
    }
    if (tx == NULL) return cf_db_failf(CF_INVALID, "connected: no transaction");

    cf_err rc = membership_increment_connections(tx, membership);
    if (rc != CF_OK) return rc;
    return membership_touch_connected_at(tx, membership);
}

cf_err cf_membership_disconnected(cf_tx *tx, cf_membership *membership) {
    if (membership == NULL) {
        return cf_db_failf(CF_INVALID, "disconnected: no membership");
    }
    if (tx == NULL) {
        return cf_db_failf(CF_INVALID, "disconnected: no transaction");
    }

    cf_err rc = membership_decrement_connections(tx, membership);
    if (rc != CF_OK) return rc;

    if (membership->connections < 1 && membership->connected_at.present) {
        cf_db *db = cf_tx_db(tx);
        if (db == NULL) {
            return cf_db_failf(CF_INVALID, "disconnected: no database");
        }
        int64_t now = cf_now_us(NULL);
        char now_text[CF_DB_TIME_TEXT_CAP];
        rc = cf_db_time_to_text(now, now_text);
        if (rc != CF_OK) return rc;

        sqlite3_stmt *stmt = NULL;
        rc = membership_execute_update(db,
                                       CF_MEMBERSHIP_STMT_CLEAR_CONNECTED,
                                       &stmt);
        if (rc != CF_OK) return rc;

        /* params![None::<Timestamp>, now, self.id] */
        rc = cf_stmt_bind_null(stmt, 1);
        if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 2,
                                                membership_cstr_span(now_text));
        if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 3, membership->id);
        if (rc != CF_OK) {
            cf_db_stmt_done(stmt);
            return rc;
        }

        rc = membership_finish_update(db, stmt, sqlite3_step(stmt),
                                      "disconnected");
        if (rc != CF_OK) return rc;

        membership->connected_at.present = false;
        membership->connected_at.value = 0;
        membership->updated_at = now;
    }
    return CF_OK;
}

cf_err cf_membership_refresh_connection(cf_tx *tx,
                                        cf_membership *membership) {
    if (membership == NULL) {
        return cf_db_failf(CF_INVALID, "refresh_connection: no membership");
    }
    if (tx == NULL) {
        return cf_db_failf(CF_INVALID,
                           "refresh_connection: no transaction");
    }

    if (!cf_membership_is_connected(membership, cf_now_us(NULL))) {
        cf_err rc = membership_increment_connections(tx, membership);
        if (rc != CF_OK) return rc;
    }
    return membership_touch_connected_at(tx, membership);
}

cf_err cf_membership_reload(cf_db *db, cf_membership *membership) {
    if (membership == NULL) {
        return cf_db_failf(CF_INVALID, "reload: no membership");
    }
    if (db == NULL) return cf_db_failf(CF_INVALID, "reload: no database");

    cf_membership fresh = {0};
    cf_err rc = cf_membership_find(db, membership->id, &fresh);
    if (rc != CF_OK) return rc;
    cf_membership_dispose(membership);
    *membership = fresh;
    return CF_OK;
}
