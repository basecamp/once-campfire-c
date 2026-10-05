/* src/models/room.c — D01 model family "room".
 *
 * Source: tmp/rust-ref/crates/db/src/models/room.rs (pinned; SHA-256 in
 * docs/devel/implementation/contracts/reference-files.json).  Tables: rooms,
 * joined to memberships/users for the scoped queries (schema.sql).
 *
 * Translation notes:
 *  - Reads take cf_db * (reader or writer connection); mutations take cf_tx *
 *    and reach the transaction's connection through cf_tx_db(tx).
 *  - Rust `tx.now()` maps to cf_now_us(NULL): the process clock, which is the
 *    same source the writer's Tx::now delegates to in production and the only
 *    clock cf.h exposes; core/testclock.h fixes it in tests (no sleeps).
 *  - Datetimes are stored as the reference UTC SQL text
 *    (cf_db_time_to_text: ".ffffff" only when non-zero) and kept in memory as
 *    int64 UTC microseconds.
 *  - SQL text, column order and statement order are copied from the source;
 *    every statement is fixed and comes from this module's statement set.  Two
 *    reference statements are assembled at runtime from variable-length
 *    argument lists (`insert_memberships` batches of 1000 and `revoke_from`'s
 *    IN list); the C port uses fixed per-row statements with the same rows,
 *    conflict handling and effects (notes at those functions).
 *  - `create`/`update` schedule the open-room grant after commit in Rust;
 *    02-data-auth.md D02 has the C port run that required local row change in
 *    the same transaction, with the final state unchanged.
 */
#include "models/room.h"

#include "db/db_internal.h"
#include "models/membership.h"
#include "models/message.h"
#include "models/user.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- fixed statements ----------------------------------------------------- */

/* Column list expansion of the source `room_columns!()`. */
#define ROOM_COLUMNS                                                          \
    "\"rooms\".\"id\", \"rooms\".\"name\", \"rooms\".\"type\", "              \
    "\"rooms\".\"creator_id\", \"rooms\".\"created_at\", "                    \
    "\"rooms\".\"updated_at\""

/* Source `user_columns!()` (user.rs) and `membership_columns!()` (membership.rs). */
#define USER_COLUMNS                                                          \
    "\"users\".\"id\", \"users\".\"name\", \"users\".\"email_address\", "     \
    "\"users\".\"password_digest\", \"users\".\"role\", "                     \
    "\"users\".\"status\", \"users\".\"bio\", \"users\".\"bot_token\", "      \
    "\"users\".\"created_at\", \"users\".\"updated_at\""

#define MEMBERSHIP_COLUMNS                                                    \
    "\"memberships\".\"id\", \"memberships\".\"room_id\", "                   \
    "\"memberships\".\"user_id\", \"memberships\".\"involvement\", "          \
    "\"memberships\".\"unread_at\", \"memberships\".\"connected_at\", "       \
    "\"memberships\".\"connections\", \"memberships\".\"created_at\", "       \
    "\"memberships\".\"updated_at\""

/* The SELECT the scoped room queries share (source SELECT_FOR_USER). */
#define ROOM_SELECT_FOR_USER                                                  \
    "SELECT " ROOM_COLUMNS                                                    \
    " FROM \"rooms\" INNER JOIN \"memberships\" ON \"rooms\".\"id\" = "       \
    "\"memberships\".\"room_id\" WHERE \"memberships\".\"user_id\" = ?"

enum {
    ROOM_STMT_FIND_BY_ID = 0,
    ROOM_STMT_ALL,
    ROOM_STMT_OF_TYPE,
    ROOM_STMT_COUNT_OF_TYPE,
    ROOM_STMT_ORIGINAL,
    ROOM_STMT_FOR_USER,
    ROOM_STMT_FIND_FOR_USER,
    ROOM_STMT_FOR_USER_OF_TYPE,
    ROOM_STMT_FOR_USER_WITHOUT_DIRECTS,
    ROOM_STMT_ORIGINAL_FOR_USER,
    ROOM_STMT_LAST_FOR_USER,
    ROOM_STMT_CREATE,
    ROOM_STMT_DIRECT_CANDIDATES,
    ROOM_STMT_ACTIVE_USER_IDS,
    ROOM_STMT_UPDATE,
    ROOM_STMT_TOUCH,
    ROOM_STMT_DELETE_MEMBERSHIPS,
    ROOM_STMT_DELETE_ROOM,
    ROOM_STMT_INSERT_MEMBERSHIP,
    ROOM_STMT_MEMBERSHIP_FOR_USER,
    ROOM_STMT_USERS,
    ROOM_STMT_USER_IDS,
    ROOM_STMT_ACTIVE_BOTS,
    ROOM_STMT_UNREAD_MEMBERSHIPS,
    ROOM_STMT_COUNT
};

static const cf_stmt_def room_stmt_defs[ROOM_STMT_COUNT] = {
    /* Rust: Room::find_by_id */
    [ROOM_STMT_FIND_BY_ID] = {
        "SELECT " ROOM_COLUMNS
        " FROM \"rooms\" WHERE \"rooms\".\"id\" = ? LIMIT 1"},
    /* Rust: Room::all */
    [ROOM_STMT_ALL] = {"SELECT " ROOM_COLUMNS " FROM \"rooms\""},
    /* Rust: Room::of_type */
    [ROOM_STMT_OF_TYPE] = {
        "SELECT " ROOM_COLUMNS " FROM \"rooms\" WHERE \"rooms\".\"type\" = ?"},
    /* Rust: Room::count_of_type */
    [ROOM_STMT_COUNT_OF_TYPE] = {
        "SELECT COUNT(*) FROM \"rooms\" WHERE \"rooms\".\"type\" = ?"},
    /* Rust: Room::original */
    [ROOM_STMT_ORIGINAL] = {
        "SELECT " ROOM_COLUMNS
        " FROM \"rooms\" ORDER BY \"rooms\".\"created_at\" ASC LIMIT 1"},
    /* Rust: Room::for_user (SELECT_FOR_USER) */
    [ROOM_STMT_FOR_USER] = {ROOM_SELECT_FOR_USER},
    /* Rust: Room::find_for_user */
    [ROOM_STMT_FIND_FOR_USER] = {
        ROOM_SELECT_FOR_USER " AND \"rooms\".\"id\" = ? LIMIT 1"},
    /* Rust: Room::for_user_of_type */
    [ROOM_STMT_FOR_USER_OF_TYPE] = {
        ROOM_SELECT_FOR_USER " AND \"rooms\".\"type\" = ?"},
    /* Rust: Room::for_user_without_directs */
    [ROOM_STMT_FOR_USER_WITHOUT_DIRECTS] = {
        ROOM_SELECT_FOR_USER " AND \"rooms\".\"type\" != ?"},
    /* Rust: Room::original_for_user */
    [ROOM_STMT_ORIGINAL_FOR_USER] = {
        ROOM_SELECT_FOR_USER " ORDER BY \"rooms\".\"created_at\" ASC LIMIT 1"},
    /* Rust: Room::last_for_user */
    [ROOM_STMT_LAST_FOR_USER] = {
        ROOM_SELECT_FOR_USER " ORDER BY \"rooms\".\"id\" DESC LIMIT 1"},
    /* Rust: Room::create */
    [ROOM_STMT_CREATE] = {
        "INSERT INTO \"rooms\" (\"created_at\", \"creator_id\", \"name\", "
        "\"type\", \"updated_at\") VALUES (?, ?, ?, ?, ?) RETURNING \"id\""},
    /* Rust: Room::find_direct_for candidates (rooms repeat per member) */
    [ROOM_STMT_DIRECT_CANDIDATES] = {
        "SELECT " ROOM_COLUMNS
        " FROM \"rooms\" INNER JOIN \"memberships\" ON "
        "\"memberships\".\"room_id\" = \"rooms\".\"id\" INNER JOIN \"users\" "
        "ON \"users\".\"id\" = \"memberships\".\"user_id\" WHERE "
        "\"rooms\".\"type\" = ?"},
    /* Rust: grant_to_active_users user list */
    [ROOM_STMT_ACTIVE_USER_IDS] = {
        "SELECT \"users\".\"id\" FROM \"users\" WHERE \"users\".\"status\" = ?"},
    /* Rust: Room::update */
    [ROOM_STMT_UPDATE] = {
        "UPDATE \"rooms\" SET \"name\" = ?, \"type\" = ?, \"updated_at\" = ? "
        "WHERE \"rooms\".\"id\" = ?"},
    /* Rust: Room::touch */
    [ROOM_STMT_TOUCH] = {
        "UPDATE \"rooms\" SET \"updated_at\" = ? WHERE \"rooms\".\"id\" = ?"},
    /* Rust: Room::destroy step 1 */
    [ROOM_STMT_DELETE_MEMBERSHIPS] = {
        "DELETE FROM \"memberships\" WHERE \"memberships\".\"room_id\" = ?"},
    /* Rust: Room::destroy step 3 */
    [ROOM_STMT_DELETE_ROOM] = {
        "DELETE FROM \"rooms\" WHERE \"rooms\".\"id\" = ?"},
    /* Rust: insert_memberships: one row of the batched insert_all.  The
     * reference builds N placeholders per statement (batches of 1000) and
     * lets SQLite stamp created_at/updated_at with STRFTIME('NOW'); the fixed
     * C statement inserts the same row per user id, so rows, conflict
     * handling (ON CONFLICT DO NOTHING) and defaults are unchanged. */
    [ROOM_STMT_INSERT_MEMBERSHIP] = {
        "INSERT INTO \"memberships\" "
        "(\"created_at\",\"involvement\",\"room_id\",\"updated_at\",\"user_id\")"
        " VALUES (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW'), ?, ?, "
        "STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW'), ?) ON CONFLICT DO NOTHING "
        "RETURNING \"id\""},
    /* Rust: revoke_from lookup (the reference builds IN (...); the fixed C
     * statement looks up the unique (room_id, user_id) row per user id). */
    [ROOM_STMT_MEMBERSHIP_FOR_USER] = {
        "SELECT " MEMBERSHIP_COLUMNS
        " FROM \"memberships\" WHERE \"memberships\".\"room_id\" = ? AND "
        "\"memberships\".\"user_id\" = ?"},
    /* Rust: Room::users */
    [ROOM_STMT_USERS] = {
        "SELECT " USER_COLUMNS
        " FROM \"users\" INNER JOIN \"memberships\" ON \"users\".\"id\" = "
        "\"memberships\".\"user_id\" WHERE \"memberships\".\"room_id\" = ?"},
    /* Rust: Room::user_ids */
    [ROOM_STMT_USER_IDS] = {
        "SELECT \"users\".\"id\" FROM \"users\" INNER JOIN \"memberships\" ON "
        "\"users\".\"id\" = \"memberships\".\"user_id\" WHERE "
        "\"memberships\".\"room_id\" = ?"},
    /* Rust: Room::active_bots */
    [ROOM_STMT_ACTIVE_BOTS] = {
        "SELECT " USER_COLUMNS
        " FROM \"users\" INNER JOIN \"memberships\" ON \"users\".\"id\" = "
        "\"memberships\".\"user_id\" WHERE \"memberships\".\"room_id\" = ? "
        "AND \"users\".\"status\" = 0 AND \"users\".\"role\" = 2"},
    /* Rust: Room::unread_memberships */
    [ROOM_STMT_UNREAD_MEMBERSHIPS] = {
        "UPDATE \"memberships\" SET \"unread_at\" = ?, \"updated_at\" = ? "
        "WHERE \"memberships\".\"room_id\" = ? AND \"memberships\".\"involvement\""
        " != ? AND (\"memberships\".\"connected_at\" IS NULL OR "
        "\"memberships\".\"connected_at\" < ?) AND \"memberships\".\"user_id\" "
        "!= ?"}};

static const cf_stmt_set room_stmt_set = {
    room_stmt_defs, sizeof room_stmt_defs / sizeof room_stmt_defs[0]};

/* --- small helpers -------------------------------------------------------- */

/* Borrowed span over borrowed text. */
static cf_span room_span_of(cf_str text) {
    cf_span span;
    span.ptr = (const unsigned char *)text.ptr;
    span.len = text.len;
    return span;
}

/* Borrowed span over a NUL-terminated buffer (fixed datetime text). */
static cf_span room_cstr_span(const char *text) {
    cf_span span;
    span.ptr = (const unsigned char *)text;
    span.len = strlen(text);
    return span;
}

/* Owned copy of a borrowed span: NUL-terminated, len excludes the NUL. */
static cf_err room_copy_span(cf_span src, cf_str *out) {
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

/* One failed step: copy the connection message before resetting the
 * statement, then report it with the mapped code. */
static cf_err room_step_failure(cf_db *db, sqlite3_stmt *stmt, int sqlite_rc,
                                const char *operation) {
    char message[CF_DB_ERROR_CAP];
    snprintf(message, sizeof message, "%s", sqlite3_errmsg(cf_db_handle(db)));
    cf_db_stmt_done(stmt);
    return cf_db_failf(cf_db_err(sqlite_rc), "%s: %s", operation, message);
}

/* The stored class name of a room type; CF_INVALID for an out-of-range enum. */
static cf_err room_type_text(cf_room_type room_type, cf_str *out) {
    const char *name = cf_room_type_class_name(room_type);
    if (name == NULL) {
        return cf_db_failf(CF_INVALID, "unknown room type %d",
                           (int)room_type);
    }
    *out = (cf_str){(char *)name, strlen(name)};
    return CF_OK;
}

/* Bind `us` as the reference UTC SQL text on parameter `index`. */
static cf_err room_bind_time(sqlite3_stmt *stmt, int index, int64_t us) {
    char text[CF_DB_TIME_TEXT_CAP];
    cf_err rc = cf_db_time_to_text(us, text);
    if (rc != CF_OK) return rc;
    return cf_stmt_bind_text(stmt, index, room_cstr_span(text));
}

/* --- row readers ---------------------------------------------------------- */

/* Read a NOT NULL datetime(6) text column into UTC microseconds. */
static cf_err room_column_time(sqlite3_stmt *stmt, int column,
                               const char *qualified, int64_t *out_us) {
    *out_us = 0;
    cf_span text = cf_stmt_column_text(stmt, column);
    if (text.ptr == NULL) {
        return cf_db_failf(CF_DB, "%s is NULL", qualified);
    }
    if (cf_db_time_from_text(text, out_us) != CF_OK) {
        return cf_db_failf(CF_DB, "%s is not valid datetime text", qualified);
    }
    return CF_OK;
}

/* Read a nullable datetime(6) column into cf_optional_i64. */
static cf_err room_column_optional_time(sqlite3_stmt *stmt, int column,
                                        const char *qualified,
                                        cf_optional_i64 *out) {
    out->present = false;
    out->value = 0;
    if (cf_stmt_column_is_null(stmt, column)) return CF_OK;
    cf_span text = cf_stmt_column_text(stmt, column);
    if (text.ptr == NULL) {
        return cf_db_failf(CF_DB, "%s is not valid datetime text", qualified);
    }
    int64_t us = 0;
    if (cf_db_time_from_text(text, &us) != CF_OK) {
        return cf_db_failf(CF_DB, "%s is not valid datetime text", qualified);
    }
    out->present = true;
    out->value = us;
    return CF_OK;
}

/* Copy a NOT NULL text column; the schema says it cannot be NULL. */
static cf_err room_column_required_text(sqlite3_stmt *stmt, int column,
                                        const char *qualified, cf_str *out) {
    if (cf_stmt_column_is_null(stmt, column)) {
        return cf_db_failf(CF_DB, "%s is NULL", qualified);
    }
    return room_copy_span(cf_stmt_column_text(stmt, column), out);
}

/* Copy a nullable text column; NULL stays absent, empty stays empty. */
static cf_err room_column_optional_text(sqlite3_stmt *stmt, int column,
                                        cf_optional_str *out) {
    out->present = false;
    out->value.ptr = NULL;
    out->value.len = 0;
    if (cf_stmt_column_is_null(stmt, column)) return CF_OK;
    cf_err rc = room_copy_span(cf_stmt_column_text(stmt, column), &out->value);
    if (rc == CF_OK) out->present = true;
    return rc;
}

/* Decode a nullable involvement column (memberships.involvement). */
static cf_err room_column_optional_involvement(sqlite3_stmt *stmt, int column,
                                               cf_optional_involvement *out) {
    out->present = false;
    out->value = CF_INVOLVEMENT_MENTIONS;
    if (cf_stmt_column_is_null(stmt, column)) return CF_OK;
    cf_span text = cf_stmt_column_text(stmt, column);
    cf_involvement value = CF_INVOLVEMENT_MENTIONS;
    if (text.ptr == NULL ||
        !cf_involvement_from_name((cf_str){(char *)text.ptr, text.len},
                                  &value)) {
        return cf_db_failf(CF_DB,
                           "memberships.involvement is not a known involvement");
    }
    out->present = true;
    out->value = value;
    return CF_OK;
}

/* rooms row from the source `room_columns!()` order (offset 0). */
static cf_err room_read_row(sqlite3_stmt *stmt, cf_room *out) {
    *out = (cf_room){0};
    cf_str type_text = {0};
    cf_err rc = room_column_optional_text(stmt, 1, &out->name);
    if (rc == CF_OK) {
        rc = room_column_required_text(stmt, 2, "rooms.type", &type_text);
    }
    if (rc == CF_OK &&
        !cf_room_type_from_class_name(type_text, &out->room_type)) {
        rc = cf_db_failf(CF_DB, "rooms.type is not a known room type");
    }
    if (rc == CF_OK) {
        out->creator_id = cf_stmt_column_i64(stmt, 3);
        rc = room_column_time(stmt, 4, "rooms.created_at", &out->created_at);
    }
    if (rc == CF_OK) {
        rc = room_column_time(stmt, 5, "rooms.updated_at", &out->updated_at);
    }
    cf_str_dispose(&type_text);
    if (rc == CF_OK) {
        out->id = cf_stmt_column_i64(stmt, 0);
    } else {
        cf_room_dispose(out);
    }
    return rc;
}

/* users row from the source `user_columns!()` order (offset 0). */
static cf_err room_read_user_row(sqlite3_stmt *stmt, cf_user *out) {
    *out = (cf_user){0};
    cf_err rc = room_column_required_text(stmt, 1, "users.name", &out->name);
    if (rc == CF_OK) {
        rc = room_column_optional_text(stmt, 2, &out->email_address);
    }
    if (rc == CF_OK) {
        rc = room_column_optional_text(stmt, 3, &out->password_digest);
    }
    if (rc == CF_OK) {
        int64_t role = cf_stmt_column_i64(stmt, 4);
        if (role < (int64_t)CF_ROLE_MEMBER || role > (int64_t)CF_ROLE_BOT) {
            rc = cf_db_failf(CF_DB, "users.role is out of range");
        } else {
            out->role = (cf_role)role;
        }
    }
    if (rc == CF_OK) {
        int64_t status = cf_stmt_column_i64(stmt, 5);
        if (status < (int64_t)CF_STATUS_ACTIVE ||
            status > (int64_t)CF_STATUS_BANNED) {
            rc = cf_db_failf(CF_DB, "users.status is out of range");
        } else {
            out->status = (cf_status)status;
        }
    }
    if (rc == CF_OK) rc = room_column_optional_text(stmt, 6, &out->bio);
    if (rc == CF_OK) rc = room_column_optional_text(stmt, 7, &out->bot_token);
    if (rc == CF_OK) {
        rc = room_column_time(stmt, 8, "users.created_at", &out->created_at);
    }
    if (rc == CF_OK) {
        rc = room_column_time(stmt, 9, "users.updated_at", &out->updated_at);
    }
    if (rc == CF_OK) {
        out->id = cf_stmt_column_i64(stmt, 0);
    } else {
        cf_user_dispose(out);
    }
    return rc;
}

/* memberships row from the source `membership_columns!()` order (offset 0). */
static cf_err room_read_membership_row(sqlite3_stmt *stmt,
                                       cf_membership *out) {
    *out = (cf_membership){0};
    out->id = cf_stmt_column_i64(stmt, 0);
    out->room_id = cf_stmt_column_i64(stmt, 1);
    out->user_id = cf_stmt_column_i64(stmt, 2);
    cf_err rc = room_column_optional_involvement(stmt, 3, &out->involvement);
    if (rc == CF_OK) {
        rc = room_column_optional_time(stmt, 4, "memberships.unread_at",
                                       &out->unread_at);
    }
    if (rc == CF_OK) {
        rc = room_column_optional_time(stmt, 5, "memberships.connected_at",
                                       &out->connected_at);
    }
    if (rc == CF_OK) {
        out->connections = cf_stmt_column_i64(stmt, 6);
    }
    if (rc == CF_OK) {
        rc = room_column_time(stmt, 7, "memberships.created_at",
                              &out->created_at);
    }
    if (rc == CF_OK) {
        rc = room_column_time(stmt, 8, "memberships.updated_at",
                              &out->updated_at);
    }
    if (rc != CF_OK) *out = (cf_membership){0};
    return rc;
}

/* --- vector growth -------------------------------------------------------- */

static cf_err room_vector_push(cf_room_vector *vector, cf_room record) {
    if (vector->len == vector->cap) {
        size_t need = vector->len + 1;
        size_t cap = vector->cap != 0 ? vector->cap : 8;
        while (cap < need) {
            if (cap > SIZE_MAX / 2) {
                cap = need;
                break;
            }
            cap *= 2;
        }
        if (cap > SIZE_MAX / sizeof *vector->items) {
            return cf_db_failf(CF_LIMIT, "room vector too large");
        }
        cf_room *items = realloc(vector->items, cap * sizeof *items);
        if (items == NULL) {
            return cf_db_failf(CF_NOMEM, "room vector allocation failed");
        }
        vector->items = items;
        vector->cap = cap;
    }
    vector->items[vector->len++] = record;
    return CF_OK;
}

static cf_err user_vector_push(cf_user_vector *vector, cf_user record) {
    if (vector->len == vector->cap) {
        size_t need = vector->len + 1;
        size_t cap = vector->cap != 0 ? vector->cap : 8;
        while (cap < need) {
            if (cap > SIZE_MAX / 2) {
                cap = need;
                break;
            }
            cap *= 2;
        }
        if (cap > SIZE_MAX / sizeof *vector->items) {
            return cf_db_failf(CF_LIMIT, "user vector too large");
        }
        cf_user *items = realloc(vector->items, cap * sizeof *items);
        if (items == NULL) {
            return cf_db_failf(CF_NOMEM, "user vector allocation failed");
        }
        vector->items = items;
        vector->cap = cap;
    }
    vector->items[vector->len++] = record;
    return CF_OK;
}

static cf_err membership_vector_push(cf_membership_vector *vector,
                                     cf_membership record) {
    if (vector->len == vector->cap) {
        size_t need = vector->len + 1;
        size_t cap = vector->cap != 0 ? vector->cap : 8;
        while (cap < need) {
            if (cap > SIZE_MAX / 2) {
                cap = need;
                break;
            }
            cap *= 2;
        }
        if (cap > SIZE_MAX / sizeof *vector->items) {
            return cf_db_failf(CF_LIMIT, "membership vector too large");
        }
        cf_membership *items = realloc(vector->items, cap * sizeof *items);
        if (items == NULL) {
            return cf_db_failf(CF_NOMEM, "membership vector allocation failed");
        }
        vector->items = items;
        vector->cap = cap;
    }
    vector->items[vector->len++] = record;
    return CF_OK;
}

static cf_err int64_vector_push(cf_int64_vector *vector, int64_t value) {
    if (vector->len == vector->cap) {
        size_t need = vector->len + 1;
        size_t cap = vector->cap != 0 ? vector->cap : 8;
        while (cap < need) {
            if (cap > SIZE_MAX / 2) {
                cap = need;
                break;
            }
            cap *= 2;
        }
        if (cap > SIZE_MAX / sizeof *vector->items) {
            return cf_db_failf(CF_LIMIT, "id vector too large");
        }
        int64_t *items = realloc(vector->items, cap * sizeof *items);
        if (items == NULL) {
            return cf_db_failf(CF_NOMEM, "id vector allocation failed");
        }
        vector->items = items;
        vector->cap = cap;
    }
    vector->items[vector->len++] = value;
    return CF_OK;
}

/* --- statement drivers ---------------------------------------------------- */

/* One fixed parameter: an int64, a borrowed text span, or a skipped position
 * bound separately (nullable text / converted datetime). */
typedef struct {
    bool skip;
    bool is_text;
    int64_t i64;
    cf_str text;
} room_bind;

static room_bind room_bind_i64(int64_t value) {
    room_bind bind = {false, false, value, {NULL, 0}};
    return bind;
}

static room_bind room_bind_text(cf_str text) {
    room_bind bind = {false, true, 0, text};
    return bind;
}

static room_bind room_bind_skip(void) {
    room_bind bind = {true, false, 0, {NULL, 0}};
    return bind;
}

static cf_err room_stmt_prepare(cf_db *db, size_t stmt_id,
                                const room_bind *binds, size_t bind_count,
                                sqlite3_stmt **out) {
    cf_err rc = cf_db_stmt(db, &room_stmt_set, stmt_id, out);
    if (rc != CF_OK) return rc;
    for (size_t i = 0; i < bind_count; i++) {
        if (binds[i].skip) continue;
        rc = binds[i].is_text
                 ? cf_stmt_bind_text(*out, (int)i + 1, room_span_of(binds[i].text))
                 : cf_stmt_bind_i64(*out, (int)i + 1, binds[i].i64);
        if (rc != CF_OK) {
            cf_db_stmt_done(*out);
            *out = NULL;
            return rc;
        }
    }
    return CF_OK;
}

/* Collect every room row of one fixed SELECT. */
static cf_err room_query_rooms(cf_db *db, size_t stmt_id,
                               const room_bind *binds, size_t bind_count,
                               cf_room_vector *out) {
    sqlite3_stmt *stmt = NULL;
    cf_err rc = room_stmt_prepare(db, stmt_id, binds, bind_count, &stmt);
    if (rc != CF_OK) return rc;

    cf_room_vector built = {0};
    for (;;) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            cf_room_vector_dispose(&built);
            return room_step_failure(db, stmt, step, "rooms query");
        }
        cf_room record = {0};
        rc = room_read_row(stmt, &record);
        if (rc == CF_OK) rc = room_vector_push(&built, record);
        if (rc != CF_OK) {
            cf_room_dispose(&record);
            cf_room_vector_dispose(&built);
            cf_db_stmt_done(stmt);
            return rc;
        }
    }
    cf_db_stmt_done(stmt);
    *out = built;
    return CF_OK;
}

/* The LIMIT 1 row of one fixed SELECT; absent leaves found=false. */
static cf_err room_query_one_room(cf_db *db, size_t stmt_id,
                                  const room_bind *binds, size_t bind_count,
                                  bool *found, cf_room *out) {
    sqlite3_stmt *stmt = NULL;
    cf_err rc = room_stmt_prepare(db, stmt_id, binds, bind_count, &stmt);
    if (rc != CF_OK) return rc;

    int step = sqlite3_step(stmt);
    if (step == SQLITE_DONE) {
        cf_db_stmt_done(stmt);
        return CF_OK;
    }
    if (step != SQLITE_ROW) {
        return room_step_failure(db, stmt, step, "room find");
    }
    cf_room record = {0};
    rc = room_read_row(stmt, &record);
    cf_db_stmt_done(stmt);
    if (rc != CF_OK) return rc;
    *out = record;
    *found = true;
    return CF_OK;
}

/* The int64 result of one fixed scalar SELECT (COUNT(*)). */
static cf_err room_query_count(cf_db *db, size_t stmt_id,
                               const room_bind *binds, size_t bind_count,
                               int64_t *out) {
    sqlite3_stmt *stmt = NULL;
    cf_err rc = room_stmt_prepare(db, stmt_id, binds, bind_count, &stmt);
    if (rc != CF_OK) return rc;

    int step = sqlite3_step(stmt);
    if (step == SQLITE_DONE) {
        cf_db_stmt_done(stmt);
        return cf_db_failf(CF_DB, "count query returned no row");
    }
    if (step != SQLITE_ROW) {
        return room_step_failure(db, stmt, step, "room count");
    }
    *out = cf_stmt_column_i64(stmt, 0);
    cf_db_stmt_done(stmt);
    return CF_OK;
}

/* Collect user rows of one fixed SELECT into an owned vector. */
static cf_err room_query_users(cf_db *db, size_t stmt_id,
                               const room_bind *binds, size_t bind_count,
                               cf_user_vector *out) {
    sqlite3_stmt *stmt = NULL;
    cf_err rc = room_stmt_prepare(db, stmt_id, binds, bind_count, &stmt);
    if (rc != CF_OK) return rc;

    cf_user_vector built = {0};
    for (;;) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            cf_user_vector_dispose(&built);
            return room_step_failure(db, stmt, step, "users query");
        }
        cf_user record = {0};
        rc = room_read_user_row(stmt, &record);
        if (rc == CF_OK) rc = user_vector_push(&built, record);
        if (rc != CF_OK) {
            cf_user_dispose(&record);
            cf_user_vector_dispose(&built);
            cf_db_stmt_done(stmt);
            return rc;
        }
    }
    cf_db_stmt_done(stmt);
    *out = built;
    return CF_OK;
}

/* Collect int64 rows of one fixed SELECT into an owned vector. */
static cf_err room_query_user_ids(cf_db *db, size_t stmt_id,
                                  const room_bind *binds, size_t bind_count,
                                  cf_int64_vector *out) {
    sqlite3_stmt *stmt = NULL;
    cf_err rc = room_stmt_prepare(db, stmt_id, binds, bind_count, &stmt);
    if (rc != CF_OK) return rc;

    cf_int64_vector built = {0};
    for (;;) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            cf_int64_vector_dispose(&built);
            return room_step_failure(db, stmt, step, "user ids query");
        }
        rc = int64_vector_push(&built, cf_stmt_column_i64(stmt, 0));
        if (rc != CF_OK) {
            cf_int64_vector_dispose(&built);
            cf_db_stmt_done(stmt);
            return rc;
        }
    }
    cf_db_stmt_done(stmt);
    *out = built;
    return CF_OK;
}

/* Run one fixed statement that must finish with SQLITE_DONE. */
static cf_err room_execute(cf_db *db, size_t stmt_id, const room_bind *binds,
                           size_t bind_count, const char *operation) {
    sqlite3_stmt *stmt = NULL;
    cf_err rc = room_stmt_prepare(db, stmt_id, binds, bind_count, &stmt);
    if (rc != CF_OK) return rc;
    int step = sqlite3_step(stmt);
    if (step != SQLITE_DONE) {
        return room_step_failure(db, stmt, step, operation);
    }
    cf_db_stmt_done(stmt);
    return CF_OK;
}

/* --- membership writes ---------------------------------------------------- */

/* `Membership.insert_all(...)`: one row per user id with the room's default
 * involvement, skipping existing (room, user) rows.  The reference groups the
 * rows into dynamic multi-row INSERTs (chunks of MEMBERSHIP_INSERT_BATCH,
 * bounded by SQLite's variable limit); the fixed single-row statement below
 * inserts the same values, lets SQLite stamp created_at/updated_at with
 * STRFTIME('NOW') per statement and skips conflicts identically. */
static cf_err room_insert_memberships(cf_db *db, int64_t room_id,
                                      cf_involvement involvement,
                                      const int64_t *user_ids, size_t count) {
    const char *involvement_name = cf_involvement_name(involvement);
    if (involvement_name == NULL) {
        return cf_db_failf(CF_INVALID, "unknown involvement %d",
                           (int)involvement);
    }
    cf_str involvement_text = {(char *)involvement_name,
                               strlen(involvement_name)};

    for (size_t i = 0; i < count; i++) {
        room_bind binds[3] = {
            room_bind_text(involvement_text),
            room_bind_i64(room_id),
            room_bind_i64(user_ids[i]),
        };
        sqlite3_stmt *stmt = NULL;
        cf_err rc =
            room_stmt_prepare(db, ROOM_STMT_INSERT_MEMBERSHIP, binds, 3, &stmt);
        if (rc != CF_OK) return rc;
        /* RETURNING makes an inserted row step as SQLITE_ROW; a conflict
         * steps straight to SQLITE_DONE.  Drain exactly as the reference
         * drains the RETURNING cursor. */
        int step;
        while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        }
        if (step != SQLITE_DONE) {
            return room_step_failure(db, stmt, step, "insert membership");
        }
        cf_db_stmt_done(stmt);
    }
    return CF_OK;
}

/* `grant_to_active_users` from `Rooms::Open`'s after-save commit: membership
 * rows for every active user, with the room's default involvement. */
static cf_err room_grant_to_active_users(cf_tx *tx, int64_t room_id) {
    cf_db *db = cf_tx_db(tx);
    room_bind bind = room_bind_i64((int64_t)CF_STATUS_ACTIVE);
    cf_int64_vector user_ids = {0};
    cf_err rc =
        room_query_user_ids(db, ROOM_STMT_ACTIVE_USER_IDS, &bind, 1, &user_ids);
    if (rc != CF_OK) return rc;

    cf_room room = {0};
    rc = cf_room_find(db, room_id, &room);
    if (rc == CF_OK) {
        rc = room_insert_memberships(db, room_id,
                                     cf_room_default_involvement(&room),
                                     user_ids.items, user_ids.len);
    }
    cf_room_dispose(&room);
    cf_int64_vector_dispose(&user_ids);
    return rc;
}

/* --- classification helpers (RoomType impl) ------------------------------- */

const char *cf_room_type_class_name(cf_room_type room_type) {
    switch (room_type) {
    case CF_ROOM_OPEN:
        return "Rooms::Open";
    case CF_ROOM_CLOSED:
        return "Rooms::Closed";
    case CF_ROOM_DIRECT:
        return "Rooms::Direct";
    }
    return NULL;
}

bool cf_room_type_from_class_name(cf_str name, cf_room_type *out) {
    if (out == NULL) return false;
    if (name.len != 0 && name.ptr == NULL) return false;
    static const struct {
        const char *text;
        cf_room_type value;
    } names[] = {
        {"Rooms::Open", CF_ROOM_OPEN},
        {"Rooms::Closed", CF_ROOM_CLOSED},
        {"Rooms::Direct", CF_ROOM_DIRECT},
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        size_t len = strlen(names[i].text);
        if (name.len == len &&
            (len == 0 || memcmp(name.ptr, names[i].text, len) == 0)) {
            *out = names[i].value;
            return true;
        }
    }
    return false;
}

cf_involvement cf_room_type_default_involvement(cf_room_type room_type) {
    /* Rust: "everything" in direct rooms, "mentions" elsewhere (the match
     * arm is a catch-all, so an out-of-range type is also "mentions"). */
    return room_type == CF_ROOM_DIRECT ? CF_INVOLVEMENT_EVERYTHING
                                       : CF_INVOLVEMENT_MENTIONS;
}

/* --- record lifecycle ----------------------------------------------------- */

void cf_room_dispose(cf_room *room) {
    if (room == NULL) return;
    cf_optional_str_dispose(&room->name);
    *room = (cf_room){0};
}

void cf_room_vector_dispose(cf_room_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len && vector->items != NULL; i++) {
        cf_room_dispose(&vector->items[i]);
    }
    free(vector->items);
    vector->items = NULL;
    vector->len = 0;
    vector->cap = 0;
}

/* --- finders and scopes --------------------------------------------------- */

cf_err cf_room_find(cf_db *db, int64_t id, cf_room *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "find: no output");
    *out = (cf_room){0};
    if (db == NULL) return cf_db_failf(CF_INVALID, "find: no database");

    bool found = false;
    cf_err rc = cf_room_find_by_id(db, id, &found, out);
    if (rc != CF_OK) return rc;
    /* Error::RecordNotFound display: "Couldn't find Room". */
    if (!found) return cf_db_failf(CF_NOT_FOUND, "Couldn't find Room");
    return CF_OK;
}

cf_err cf_room_find_by_id(cf_db *db, int64_t id, bool *found, cf_room *out) {
    if (found == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "find_by_id: no output");
    }
    *found = false;
    *out = (cf_room){0};
    if (db == NULL) return cf_db_failf(CF_INVALID, "find_by_id: no database");

    room_bind bind = room_bind_i64(id);
    return room_query_one_room(db, ROOM_STMT_FIND_BY_ID, &bind, 1, found, out);
}

cf_err cf_room_all(cf_db *db, cf_room_vector *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "all: no output");
    *out = (cf_room_vector){0};
    if (db == NULL) return cf_db_failf(CF_INVALID, "all: no database");
    return room_query_rooms(db, ROOM_STMT_ALL, NULL, 0, out);
}

cf_err cf_room_of_type(cf_db *db, cf_room_type room_type,
                       cf_room_vector *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "of_type: no output");
    *out = (cf_room_vector){0};
    if (db == NULL) return cf_db_failf(CF_INVALID, "of_type: no database");
    cf_str type_text = {0};
    cf_err rc = room_type_text(room_type, &type_text);
    if (rc != CF_OK) return rc;
    room_bind bind = room_bind_text(type_text);
    return room_query_rooms(db, ROOM_STMT_OF_TYPE, &bind, 1, out);
}

cf_err cf_room_count_of_type(cf_db *db, cf_room_type room_type, int64_t *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "count_of_type: no output");
    *out = 0;
    if (db == NULL) {
        return cf_db_failf(CF_INVALID, "count_of_type: no database");
    }
    cf_str type_text = {0};
    cf_err rc = room_type_text(room_type, &type_text);
    if (rc != CF_OK) return rc;
    room_bind bind = room_bind_text(type_text);
    return room_query_count(db, ROOM_STMT_COUNT_OF_TYPE, &bind, 1, out);
}

cf_err cf_room_original(cf_db *db, bool *found, cf_room *out) {
    if (found == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "original: no output");
    }
    *found = false;
    *out = (cf_room){0};
    if (db == NULL) return cf_db_failf(CF_INVALID, "original: no database");
    return room_query_one_room(db, ROOM_STMT_ORIGINAL, NULL, 0, found, out);
}

cf_err cf_room_for_user(cf_db *db, int64_t user_id, cf_room_vector *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "for_user: no output");
    *out = (cf_room_vector){0};
    if (db == NULL) return cf_db_failf(CF_INVALID, "for_user: no database");
    room_bind bind = room_bind_i64(user_id);
    return room_query_rooms(db, ROOM_STMT_FOR_USER, &bind, 1, out);
}

cf_err cf_room_find_for_user(cf_db *db, int64_t user_id, int64_t room_id,
                             bool *found, cf_room *out) {
    if (found == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "find_for_user: no output");
    }
    *found = false;
    *out = (cf_room){0};
    if (db == NULL) {
        return cf_db_failf(CF_INVALID, "find_for_user: no database");
    }
    room_bind binds[2] = {room_bind_i64(user_id), room_bind_i64(room_id)};
    return room_query_one_room(db, ROOM_STMT_FIND_FOR_USER, binds, 2, found,
                               out);
}

cf_err cf_room_for_user_of_type(cf_db *db, int64_t user_id,
                                cf_room_type room_type, cf_room_vector *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "for_user_of_type: no output");
    }
    *out = (cf_room_vector){0};
    if (db == NULL) {
        return cf_db_failf(CF_INVALID, "for_user_of_type: no database");
    }
    cf_str type_text = {0};
    cf_err rc = room_type_text(room_type, &type_text);
    if (rc != CF_OK) return rc;
    room_bind binds[2] = {room_bind_i64(user_id), room_bind_text(type_text)};
    return room_query_rooms(db, ROOM_STMT_FOR_USER_OF_TYPE, binds, 2, out);
}

cf_err cf_room_for_user_without_directs(cf_db *db, int64_t user_id,
                                        cf_room_vector *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "for_user_without_directs: no output");
    }
    *out = (cf_room_vector){0};
    if (db == NULL) {
        return cf_db_failf(CF_INVALID, "for_user_without_directs: no database");
    }
    cf_str direct_text = {(char *)"Rooms::Direct", sizeof "Rooms::Direct" - 1};
    room_bind binds[2] = {room_bind_i64(user_id),
                          room_bind_text(direct_text)};
    return room_query_rooms(db, ROOM_STMT_FOR_USER_WITHOUT_DIRECTS, binds, 2,
                            out);
}

cf_err cf_room_original_for_user(cf_db *db, int64_t user_id, bool *found,
                                 cf_room *out) {
    if (found == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "original_for_user: no output");
    }
    *found = false;
    *out = (cf_room){0};
    if (db == NULL) {
        return cf_db_failf(CF_INVALID, "original_for_user: no database");
    }
    room_bind bind = room_bind_i64(user_id);
    return room_query_one_room(db, ROOM_STMT_ORIGINAL_FOR_USER, &bind, 1,
                               found, out);
}

cf_err cf_room_last_for_user(cf_db *db, int64_t user_id, bool *found,
                             cf_room *out) {
    if (found == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "last_for_user: no output");
    }
    *found = false;
    *out = (cf_room){0};
    if (db == NULL) {
        return cf_db_failf(CF_INVALID, "last_for_user: no database");
    }
    room_bind bind = room_bind_i64(user_id);
    return room_query_one_room(db, ROOM_STMT_LAST_FOR_USER, &bind, 1, found,
                               out);
}

/* --- creation ------------------------------------------------------------- */

cf_err cf_room_create(cf_tx *tx, cf_room_type room_type, cf_optional_str name,
                      int64_t creator_id, cf_room *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "create: no output");
    *out = (cf_room){0};
    if (tx == NULL) return cf_db_failf(CF_INVALID, "create: no transaction");
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return cf_db_failf(CF_INVALID, "create: no database");

    cf_str type_text = {0};
    cf_err rc = room_type_text(room_type, &type_text);
    if (rc != CF_OK) return rc;

    int64_t now = cf_now_us(NULL);
    char now_text[CF_DB_TIME_TEXT_CAP];
    rc = cf_db_time_to_text(now, now_text);
    if (rc != CF_OK) return rc;

    room_bind binds[5] = {
        room_bind_text((cf_str){now_text, strlen(now_text)}),
        room_bind_i64(creator_id),
        room_bind_skip(),
        room_bind_text(type_text),
        room_bind_text((cf_str){now_text, strlen(now_text)}),
    };
    sqlite3_stmt *stmt = NULL;
    rc = room_stmt_prepare(db, ROOM_STMT_CREATE, binds, 5, &stmt);
    if (rc != CF_OK) return rc;
    /* name: NULL binds as SQL NULL, present=false is absent. */
    rc = cf_stmt_bind_opt_text(stmt, 3, name.present, room_span_of(name.value));
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
        return room_step_failure(db, stmt, step, "create");
    }
    int64_t id = cf_stmt_column_i64(stmt, 0);
    cf_db_stmt_done(stmt);

    /* Rooms::Open#grant_access_to_all_users was an after-commit hook in the
     * reference; 02 D02 keeps this required local row change in the same
     * transaction, so the committed state is unchanged. */
    if (room_type == CF_ROOM_OPEN) {
        rc = room_grant_to_active_users(tx, id);
        if (rc != CF_OK) return rc;
    }

    /* Self::find(tx.conn(), id) */
    return cf_room_find(db, id, out);
}

cf_err cf_room_create_for(cf_tx *tx, cf_room_type room_type, cf_optional_str name,
                          int64_t creator_id, const int64_t *user_ids,
                          size_t user_ids_len, cf_room *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "create_for: no output");
    *out = (cf_room){0};
    if (user_ids_len != 0 && user_ids == NULL) {
        return cf_db_failf(CF_INVALID, "create_for: user_ids are NULL");
    }
    cf_err rc = cf_room_create(tx, room_type, name, creator_id, out);
    if (rc != CF_OK) return rc;
    rc = cf_room_grant_to(tx, out, user_ids, user_ids_len);
    if (rc != CF_OK) {
        cf_room_dispose(out);
        return rc;
    }
    return CF_OK;
}

cf_err cf_room_find_or_create_direct_for(cf_tx *tx, const int64_t *user_ids,
                                         size_t user_ids_len,
                                         int64_t creator_id, cf_room *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "find_or_create_direct_for: no output");
    }
    *out = (cf_room){0};
    if (tx == NULL) {
        return cf_db_failf(CF_INVALID,
                           "find_or_create_direct_for: no transaction");
    }
    if (user_ids_len != 0 && user_ids == NULL) {
        return cf_db_failf(CF_INVALID,
                           "find_or_create_direct_for: user_ids are NULL");
    }
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) {
        return cf_db_failf(CF_INVALID, "find_or_create_direct_for: no database");
    }

    bool found = false;
    cf_err rc =
        cf_room_find_direct_for(db, user_ids, user_ids_len, &found, out);
    if (rc != CF_OK) return rc;
    if (found) return CF_OK;
    return cf_room_create_for(tx, CF_ROOM_DIRECT, (cf_optional_str){0},
                              creator_id, user_ids, user_ids_len, out);
}

/* --- direct-room matching ------------------------------------------------- */

static int room_i64_compare(const void *left, const void *right) {
    int64_t a = *(const int64_t *)left;
    int64_t b = *(const int64_t *)right;
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

/* Sort ascending and drop duplicates: the BTreeSet<i64> of the source. */
static void room_i64_sort_unique(int64_t *values, size_t *count) {
    if (*count < 2) return;
    qsort(values, *count, sizeof *values, room_i64_compare);
    size_t out = 1;
    for (size_t i = 1; i < *count; i++) {
        if (values[i] != values[out - 1]) values[out++] = values[i];
    }
    *count = out;
}

cf_err cf_room_find_direct_for(cf_db *db, const int64_t *user_ids,
                               size_t user_ids_len, bool *found, cf_room *out) {
    if (found == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "find_direct_for: no output");
    }
    *found = false;
    *out = (cf_room){0};
    if (db == NULL) {
        return cf_db_failf(CF_INVALID, "find_direct_for: no database");
    }
    if (user_ids_len != 0 && user_ids == NULL) {
        return cf_db_failf(CF_INVALID, "find_direct_for: user_ids are NULL");
    }
    if (user_ids_len > SIZE_MAX / sizeof(int64_t)) {
        return cf_db_failf(CF_LIMIT, "find_direct_for: too many user ids");
    }

    /* let wanted: BTreeSet<i64> = user_ids.collect() */
    size_t wanted_len = user_ids_len;
    int64_t *wanted = NULL;
    if (wanted_len != 0) {
        wanted = malloc(wanted_len * sizeof *wanted);
        if (wanted == NULL) return CF_NOMEM;
        memcpy(wanted, user_ids, wanted_len * sizeof *wanted);
        room_i64_sort_unique(wanted, &wanted_len);
    }

    cf_str direct_text = {(char *)"Rooms::Direct", sizeof "Rooms::Direct" - 1};
    room_bind bind = room_bind_text(direct_text);
    cf_room_vector candidates = {0};
    cf_err rc = room_query_rooms(db, ROOM_STMT_DIRECT_CANDIDATES, &bind, 1,
                                 &candidates);
    if (rc != CF_OK) {
        free(wanted);
        return rc;
    }

    for (size_t i = 0; i < candidates.len; i++) {
        cf_int64_vector members = {0};
        rc = cf_room_user_ids(db, &candidates.items[i], &members);
        if (rc != CF_OK) break;
        room_i64_sort_unique(members.items, &members.len);
        bool same = members.len == wanted_len;
        if (same && wanted_len != 0) {
            same = memcmp(members.items, wanted, wanted_len * sizeof *wanted) ==
                   0;
        }
        cf_int64_vector_dispose(&members);
        if (same) {
            /* Transfer the matching room out of the candidate vector. */
            *out = candidates.items[i];
            candidates.items[i] = (cf_room){0};
            *found = true;
            break;
        }
    }

    cf_room_vector_dispose(&candidates);
    free(wanted);
    return rc;
}

/* --- updates -------------------------------------------------------------- */

/* Option<String> equality: absent equals absent; present compares bytes. */
static bool room_name_equal(const cf_optional_str *left,
                            const cf_optional_str *right) {
    if (left->present != right->present) return false;
    if (!left->present) return true;
    if (left->value.len != right->value.len) return false;
    if (left->value.len == 0) return true;
    if (left->value.ptr == NULL || right->value.ptr == NULL) return false;
    return memcmp(left->value.ptr, right->value.ptr, left->value.len) == 0;
}

cf_err cf_room_update(cf_tx *tx, cf_room *room, const cf_optional_str *name,
                      const cf_room_type *room_type) {
    if (room == NULL) return cf_db_failf(CF_INVALID, "update: no room");
    if (tx == NULL) return cf_db_failf(CF_INVALID, "update: no transaction");
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return cf_db_failf(CF_INVALID, "update: no database");

    /* name.map(...).filter(|n| *n != self.name); room_type.filter(...) */
    bool name_change = name != NULL && !room_name_equal(name, &room->name);
    bool type_change = room_type != NULL && *room_type != room->room_type;

    /* direct_rooms_keep_their_type */
    if (type_change && room->room_type == CF_ROOM_DIRECT) {
        return cf_db_failf(CF_INVALID,
                           "type can't be changed for a direct room");
    }
    if (!name_change && !type_change) return CF_OK;

    /* Build the new values before touching the record, so a failed statement
     * leaves it unchanged (the committed state is the reference's). */
    cf_optional_str new_name = {0};
    cf_err rc = CF_OK;
    if (name_change && name->present) {
        rc = room_copy_span(room_span_of(name->value), &new_name.value);
        if (rc != CF_OK) return rc;
        new_name.present = true;
    }
    cf_str type_text = {0};
    rc = room_type_text(type_change ? *room_type : room->room_type,
                        &type_text);
    if (rc != CF_OK) {
        cf_optional_str_dispose(&new_name);
        return rc;
    }

    int64_t now = cf_now_us(NULL);
    char now_text[CF_DB_TIME_TEXT_CAP];
    rc = cf_db_time_to_text(now, now_text);
    if (rc != CF_OK) {
        cf_optional_str_dispose(&new_name);
        return rc;
    }

    const cf_optional_str *bound_name =
        name_change ? &new_name : &room->name;
    room_bind binds[4] = {
        room_bind_skip(),
        room_bind_text(type_text),
        room_bind_text((cf_str){now_text, strlen(now_text)}),
        room_bind_i64(room->id),
    };
    sqlite3_stmt *stmt = NULL;
    rc = room_stmt_prepare(db, ROOM_STMT_UPDATE, binds, 4, &stmt);
    if (rc != CF_OK) {
        cf_optional_str_dispose(&new_name);
        return rc;
    }
    rc = cf_stmt_bind_opt_text(stmt, 1, bound_name->present,
                               room_span_of(bound_name->value));
    if (rc != CF_OK) {
        cf_db_stmt_done(stmt);
        cf_optional_str_dispose(&new_name);
        return rc;
    }
    int step = sqlite3_step(stmt);
    if (step != SQLITE_DONE) {
        cf_err failure = room_step_failure(db, stmt, step, "update");
        cf_optional_str_dispose(&new_name);
        return failure;
    }
    cf_db_stmt_done(stmt);

    /* Commit the in-memory changes (the reference assigns before execute). */
    if (name_change) {
        cf_optional_str_dispose(&room->name);
        room->name = new_name;
    }
    if (type_change) room->room_type = *room_type;
    room->updated_at = now;

    /* Becoming open grants every active user (after-commit in the reference,
     * in-transaction per 02 D02). */
    if (type_change && *room_type == CF_ROOM_OPEN) {
        return room_grant_to_active_users(tx, room->id);
    }
    return CF_OK;
}

cf_err cf_room_touch(cf_tx *tx, int64_t room_id) {
    if (tx == NULL) return cf_db_failf(CF_INVALID, "touch: no transaction");
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return cf_db_failf(CF_INVALID, "touch: no database");

    int64_t now = cf_now_us(NULL);
    char now_text[CF_DB_TIME_TEXT_CAP];
    cf_err rc = cf_db_time_to_text(now, now_text);
    if (rc != CF_OK) return rc;

    room_bind binds[2] = {room_bind_text((cf_str){now_text, strlen(now_text)}),
                          room_bind_i64(room_id)};
    return room_execute(db, ROOM_STMT_TOUCH, binds, 2, "touch");
}

cf_err cf_room_destroy(cf_tx *tx, const cf_room *room) {
    if (room == NULL) return cf_db_failf(CF_INVALID, "destroy: no room");
    if (tx == NULL) return cf_db_failf(CF_INVALID, "destroy: no transaction");
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return cf_db_failf(CF_INVALID, "destroy: no database");

    /* Memberships are deleted without callbacks. */
    room_bind bind = room_bind_i64(room->id);
    cf_err rc = room_execute(db, ROOM_STMT_DELETE_MEMBERSHIPS, &bind, 1,
                             "destroy memberships");
    if (rc != CF_OK) return rc;

    /* Messages are destroyed one by one (dependent rows, touches, events). */
    cf_message_vector messages = {0};
    rc = cf_message_for_room(db, room->id, &messages);
    if (rc != CF_OK) return rc;
    for (size_t i = 0; i < messages.len; i++) {
        rc = cf_message_destroy(tx, &messages.items[i]);
        if (rc != CF_OK) {
            cf_message_vector_dispose(&messages);
            return rc;
        }
    }
    cf_message_vector_dispose(&messages);

    return room_execute(db, ROOM_STMT_DELETE_ROOM, &bind, 1,
                        "destroy room");
}

/* --- memberships ---------------------------------------------------------- */

cf_err cf_room_memberships(cf_db *db, const cf_room *room,
                           cf_membership_vector *out) {
    if (room == NULL) {
        return cf_db_failf(CF_INVALID, "memberships: no room");
    }
    return cf_membership_for_room(db, room->id, out);
}

cf_err cf_room_grant_to(cf_tx *tx, const cf_room *room,
                        const int64_t *user_ids, size_t user_ids_len) {
    if (room == NULL) return cf_db_failf(CF_INVALID, "grant_to: no room");
    if (tx == NULL) return cf_db_failf(CF_INVALID, "grant_to: no transaction");
    if (user_ids_len != 0 && user_ids == NULL) {
        return cf_db_failf(CF_INVALID, "grant_to: user_ids are NULL");
    }
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return cf_db_failf(CF_INVALID, "grant_to: no database");
    /* An empty slice inserts nothing (the reference's chunk loop is empty). */
    if (user_ids_len == 0) return CF_OK;

    return room_insert_memberships(db, room->id,
                                   cf_room_default_involvement(room), user_ids,
                                   user_ids_len);
}

/* Order replacements by membership id: deterministic and independent of the
 * per-user lookups (the reference returns its IN-list rows in whatever order
 * the unique (room_id, user_id) index scan yields). */
static int room_membership_compare(const void *left, const void *right) {
    const cf_membership *a = left;
    const cf_membership *b = right;
    if (a->id < b->id) return -1;
    if (a->id > b->id) return 1;
    return 0;
}

cf_err cf_room_revoke_from(cf_tx *tx, const cf_room *room,
                           const int64_t *user_ids, size_t user_ids_len) {
    if (room == NULL) return cf_db_failf(CF_INVALID, "revoke_from: no room");
    if (tx == NULL) {
        return cf_db_failf(CF_INVALID, "revoke_from: no transaction");
    }
    if (user_ids_len != 0 && user_ids == NULL) {
        return cf_db_failf(CF_INVALID, "revoke_from: user_ids are NULL");
    }
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) {
        return cf_db_failf(CF_INVALID, "revoke_from: no database");
    }
    /* The reference builds `user_id IN (...)` at runtime; the fixed C form
     * looks up the unique (room_id, user_id) row per user id and destroys the
     * matches in membership id order (same rows and events). */
    if (user_ids_len == 0) return CF_OK;

    cf_membership_vector matches = {0};
    for (size_t i = 0; i < user_ids_len; i++) {
        room_bind binds[2] = {room_bind_i64(room->id),
                              room_bind_i64(user_ids[i])};
        sqlite3_stmt *stmt = NULL;
        cf_err rc = room_stmt_prepare(db, ROOM_STMT_MEMBERSHIP_FOR_USER, binds,
                                      2, &stmt);
        if (rc != CF_OK) {
            cf_membership_vector_dispose(&matches);
            return rc;
        }
        int step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            cf_membership membership = {0};
            rc = room_read_membership_row(stmt, &membership);
            cf_db_stmt_done(stmt);
            if (rc == CF_OK) rc = membership_vector_push(&matches, membership);
            if (rc != CF_OK) {
                cf_membership_vector_dispose(&matches);
                return rc;
            }
        } else if (step != SQLITE_DONE) {
            cf_membership_vector_dispose(&matches);
            return room_step_failure(db, stmt, step, "revoke_from");
        } else {
            cf_db_stmt_done(stmt);
        }
    }

    if (matches.len > 1) {
        qsort(matches.items, matches.len, sizeof *matches.items,
              room_membership_compare);
    }
    int64_t last_id = 0;
    bool have_last = false;
    cf_err rc = CF_OK;
    for (size_t i = 0; i < matches.len; i++) {
        /* Duplicate user ids in the argument must remove each row once
         * (`IN (a, a)` matches one row). */
        if (have_last && matches.items[i].id == last_id) continue;
        last_id = matches.items[i].id;
        have_last = true;
        rc = cf_membership_destroy(tx, &matches.items[i]);
        if (rc != CF_OK) break;
    }
    cf_membership_vector_dispose(&matches);
    return rc;
}

cf_err cf_room_revise(cf_tx *tx, const cf_room *room, const int64_t *granted,
                      size_t granted_len, const int64_t *revoked,
                      size_t revoked_len) {
    if (granted_len != 0 && granted == NULL) {
        return cf_db_failf(CF_INVALID, "revise: granted is NULL");
    }
    if (revoked_len != 0 && revoked == NULL) {
        return cf_db_failf(CF_INVALID, "revise: revoked is NULL");
    }
    cf_err rc = CF_OK;
    if (granted_len != 0) {
        rc = cf_room_grant_to(tx, room, granted, granted_len);
    }
    if (rc == CF_OK && revoked_len != 0) {
        rc = cf_room_revoke_from(tx, room, revoked, revoked_len);
    }
    return rc;
}

/* --- associations --------------------------------------------------------- */

cf_err cf_room_users(cf_db *db, const cf_room *room, cf_user_vector *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "users: no output");
    *out = (cf_user_vector){0};
    if (room == NULL) return cf_db_failf(CF_INVALID, "users: no room");
    if (db == NULL) return cf_db_failf(CF_INVALID, "users: no database");
    room_bind bind = room_bind_i64(room->id);
    return room_query_users(db, ROOM_STMT_USERS, &bind, 1, out);
}

cf_err cf_room_user_ids(cf_db *db, const cf_room *room, cf_int64_vector *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "user_ids: no output");
    *out = (cf_int64_vector){0};
    if (room == NULL) return cf_db_failf(CF_INVALID, "user_ids: no room");
    if (db == NULL) return cf_db_failf(CF_INVALID, "user_ids: no database");
    room_bind bind = room_bind_i64(room->id);
    return room_query_user_ids(db, ROOM_STMT_USER_IDS, &bind, 1, out);
}

cf_err cf_room_active_bots(cf_db *db, const cf_room *room,
                           cf_user_vector *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "active_bots: no output");
    *out = (cf_user_vector){0};
    if (room == NULL) {
        return cf_db_failf(CF_INVALID, "active_bots: no room");
    }
    if (db == NULL) {
        return cf_db_failf(CF_INVALID, "active_bots: no database");
    }
    room_bind bind = room_bind_i64(room->id);
    return room_query_users(db, ROOM_STMT_ACTIVE_BOTS, &bind, 1, out);
}

/* --- message receive ------------------------------------------------------ */

cf_err cf_room_receive(cf_tx *tx, int64_t room_id, const cf_message *message) {
    if (message == NULL) {
        return cf_db_failf(CF_INVALID, "receive: no message");
    }
    if (tx == NULL) return cf_db_failf(CF_INVALID, "receive: no transaction");
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return cf_db_failf(CF_INVALID, "receive: no database");

    int64_t now = cf_now_us(NULL);
    char now_text[CF_DB_TIME_TEXT_CAP];
    cf_err rc = cf_db_time_to_text(now, now_text);
    if (rc != CF_OK) return rc;
    int64_t cutoff = cf_membership_connection_cutoff(now);
    char cutoff_text[CF_DB_TIME_TEXT_CAP];
    rc = cf_db_time_to_text(cutoff, cutoff_text);
    if (rc != CF_OK) return rc;

    cf_str invisible_text = {(char *)"invisible", sizeof "invisible" - 1};
    room_bind binds[6] = {
        room_bind_skip(),
        room_bind_text((cf_str){now_text, strlen(now_text)}),
        room_bind_i64(room_id),
        room_bind_text(invisible_text),
        room_bind_text((cf_str){cutoff_text, strlen(cutoff_text)}),
        room_bind_i64(message->creator_id),
    };
    sqlite3_stmt *stmt = NULL;
    rc = room_stmt_prepare(db, ROOM_STMT_UNREAD_MEMBERSHIPS, binds, 6, &stmt);
    if (rc != CF_OK) return rc;
    /* params![message.created_at, now, room_id, "invisible", cutoff, creator] */
    rc = room_bind_time(stmt, 1, message->created_at);
    if (rc != CF_OK) {
        cf_db_stmt_done(stmt);
        return rc;
    }
    int step = sqlite3_step(stmt);
    if (step != SQLITE_DONE) {
        return room_step_failure(db, stmt, step, "receive");
    }
    cf_db_stmt_done(stmt);

    /* tx.emit_after_commit(Event::PushMessage { room_id, message_id }) */
    cf_event event = {0};
    event.kind = CF_EVENT_PUSH_MESSAGE;
    event.room_id = room_id;
    event.message_id = message->id;
    return cf_tx_event(tx, event);
}

/* --- predicates ----------------------------------------------------------- */

bool cf_room_open(const cf_room *room) {
    return room != NULL && room->room_type == CF_ROOM_OPEN;
}

bool cf_room_closed(const cf_room *room) {
    return room != NULL && room->room_type == CF_ROOM_CLOSED;
}

bool cf_room_direct(const cf_room *room) {
    return room != NULL && room->room_type == CF_ROOM_DIRECT;
}

cf_involvement cf_room_default_involvement(const cf_room *room) {
    if (room == NULL) return CF_INVOLVEMENT_MENTIONS;
    return cf_room_type_default_involvement(room->room_type);
}

cf_err cf_room_reload(cf_db *db, cf_room *room) {
    if (room == NULL) return cf_db_failf(CF_INVALID, "reload: no room");
    if (db == NULL) return cf_db_failf(CF_INVALID, "reload: no database");

    cf_room fresh = {0};
    cf_err rc = cf_room_find(db, room->id, &fresh);
    if (rc != CF_OK) return rc;
    cf_room_dispose(room);
    *room = fresh;
    return CF_OK;
}
