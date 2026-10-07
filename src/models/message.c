/* src/models/message.c — D01 model family "message".
 *
 * Source of truth: tmp/rust-ref/crates/db/src/models/message.rs (pinned).
 * Tables: messages; writes action_text_rich_texts through rich_text_record.h,
 * active_storage_attachments/blobs through active_storage.h, boosts through
 * boost.h, the room touch/receive through room.h, users through user.h, and
 * the message_search_index FTS table directly (rowid = message id).
 *
 * Fixed statement set (02-data-auth.md D01): every statement is a literal in
 * the table below, keyed by the local enum; no SQL text is assembled at
 * runtime.  Reads use cf_db *, mutations cf_tx * (the transaction's borrowed
 * db via cf_tx_db).
 *
 * Reference translation notes:
 *  - Rust `tx.now()` is the injected process clock; C uses cf_now_us(NULL)
 *    (cf.h: "Determinism belongs in injected test clocks").
 *  - Rust `tx.rich_text()` is the pipeline the FTS body is computed with;
 *    rich_text.rs RichText::{to_plain_text, mentioned_user_ids} and
 *    database.rs Tx::rich_text arrive through src/richtext.h (R02), the
 *    integrator-ratified boundary whose signatures are identical to the local
 *    declarations this file used to carry.  See
 *    docs/devel/evidence/D01-model-message.md.
 *  - The reference builds `IN (?, ?, ...)` / `NOT IN (?, ?, ...)` with
 *    runtime placeholders.  C forbids runtime SQL, so the variable-length
 *    lists are passed as a JSON array to SQLite's built-in json_each: same
 *    filtering semantics for any length with one fixed statement.  When the
 *    list would be empty the reference omits the clause; that exact-text
 *    statement is kept for page_updated_since, and the empty json array for
 *    mentionees_in_room is handled by an early return.
 */
#include "models/message.h"

#include "cf.h"
#include "db/db_internal.h"
#include "models/active_storage.h"
#include "models/boost.h"
#include "models/rich_text_record.h"
#include "models/room.h"
#include "models/sound.h"
#include "models/user.h"
#include "richtext.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- constants and fixed statements --------------------------------------- */

#define CF_MESSAGE_RECORD_TYPE "Message"
#define CF_MESSAGE_BODY_NAME "body"
#define CF_MESSAGE_ATTACHMENT_NAME "attachment"

#define CF_MESSAGE_COLUMNS                                                    \
    "\"messages\".\"id\", \"messages\".\"room_id\", "                        \
    "\"messages\".\"creator_id\", \"messages\".\"client_message_id\", "       \
    "\"messages\".\"created_at\", \"messages\".\"updated_at\""

#define CF_MESSAGE_IN_ROOM                                                    \
    "SELECT " CF_MESSAGE_COLUMNS " FROM \"messages\" WHERE "                 \
    "\"messages\".\"room_id\" = ?"

#define CF_MESSAGE_REACHABLE                                                  \
    "SELECT " CF_MESSAGE_COLUMNS " FROM \"messages\" INNER JOIN \"rooms\" "  \
    "ON \"messages\".\"room_id\" = \"rooms\".\"id\" INNER JOIN "             \
    "\"memberships\" ON \"rooms\".\"id\" = \"memberships\".\"room_id\""

enum {
    CF_MS_FIND_BY_ID,
    CF_MS_LAST,
    CF_MS_COUNT,
    CF_MS_BY_CREATOR,
    CF_MS_IN_ROOM,
    CF_MS_FIND_IN_ROOM,
    CF_MS_COUNT_IN_ROOM,
    CF_MS_FIND_REACHABLE,
    CF_MS_LAST_PAGE,
    CF_MS_FIRST_PAGE,
    CF_MS_PAGE_BEFORE,
    CF_MS_PAGE_AFTER,
    CF_MS_PAGE_CREATED_SINCE,
    CF_MS_PAGE_UPDATED_SINCE,
    CF_MS_PAGE_UPDATED_SINCE_EXCLUDING,
    CF_MS_EXISTS_BEFORE,
    CF_MS_EXISTS_AFTER,
    CF_MS_PAGED,
    CF_MS_SEARCH_IN_ROOM,
    CF_MS_SEARCH_REACHABLE,
    CF_MS_SEARCH_PROBE,
    CF_MS_SEARCH_SELECTED,
    CF_MS_MENTIONEES_IN_ROOM,
    CF_MS_INSERT,
    CF_MS_TOUCH,
    CF_MS_DELETE,
    CF_MS_INDEX_INSERT,
    CF_MS_INDEX_UPDATE,
    CF_MS_INDEX_DELETE,
    CF_MS_COUNT_
};

static const cf_stmt_def cf_message_stmt_defs[] = {
    [CF_MS_FIND_BY_ID] =
        {"SELECT " CF_MESSAGE_COLUMNS " FROM \"messages\" WHERE "
         "\"messages\".\"id\" = ? LIMIT 1"},
    [CF_MS_LAST] =
        {"SELECT " CF_MESSAGE_COLUMNS " FROM \"messages\" ORDER BY "
         "\"messages\".\"id\" DESC LIMIT 1"},
    [CF_MS_COUNT] = {"SELECT COUNT(*) FROM \"messages\""},
    [CF_MS_BY_CREATOR] =
        {"SELECT " CF_MESSAGE_COLUMNS " FROM \"messages\" WHERE "
         "\"messages\".\"creator_id\" = ?"},
    [CF_MS_IN_ROOM] = {CF_MESSAGE_IN_ROOM},
    [CF_MS_FIND_IN_ROOM] =
        {CF_MESSAGE_IN_ROOM " AND \"messages\".\"id\" = ? LIMIT 1"},
    [CF_MS_COUNT_IN_ROOM] =
        {"SELECT COUNT(*) FROM \"messages\" WHERE \"messages\".\"room_id\" "
         "= ?"},
    [CF_MS_FIND_REACHABLE] =
        {CF_MESSAGE_REACHABLE " WHERE \"memberships\".\"user_id\" = ? AND "
         "\"messages\".\"id\" = ? LIMIT 1"},
    [CF_MS_LAST_PAGE] =
        {CF_MESSAGE_IN_ROOM " ORDER BY \"messages\".\"created_at\" DESC "
         "LIMIT 40"},
    [CF_MS_FIRST_PAGE] =
        {CF_MESSAGE_IN_ROOM " ORDER BY \"messages\".\"created_at\" ASC "
         "LIMIT 40"},
    [CF_MS_PAGE_BEFORE] =
        {CF_MESSAGE_IN_ROOM " AND (created_at < ?) ORDER BY "
         "\"messages\".\"created_at\" DESC LIMIT 40"},
    [CF_MS_PAGE_AFTER] =
        {CF_MESSAGE_IN_ROOM " AND (created_at > ?) ORDER BY "
         "\"messages\".\"created_at\" ASC LIMIT 40"},
    [CF_MS_PAGE_CREATED_SINCE] =
        {CF_MESSAGE_IN_ROOM " AND (created_at > ?) ORDER BY "
         "\"messages\".\"created_at\" ASC LIMIT 40"},
    [CF_MS_PAGE_UPDATED_SINCE] =
        {CF_MESSAGE_IN_ROOM " AND (updated_at > ?) ORDER BY "
         "+\"messages\".\"created_at\" DESC LIMIT 40"},
    [CF_MS_PAGE_UPDATED_SINCE_EXCLUDING] =
        {CF_MESSAGE_IN_ROOM " AND \"messages\".\"id\" NOT IN (SELECT value "
         "FROM json_each(?)) AND (updated_at > ?) ORDER BY "
         "+\"messages\".\"created_at\" DESC LIMIT 40"},
    [CF_MS_EXISTS_BEFORE] =
        {"SELECT 1 FROM \"messages\" WHERE \"messages\".\"room_id\" = ? AND "
         "(created_at < ?) LIMIT 1"},
    [CF_MS_EXISTS_AFTER] =
        {"SELECT 1 FROM \"messages\" WHERE \"messages\".\"room_id\" = ? AND "
         "(created_at > ?) LIMIT 1"},
    [CF_MS_PAGED] =
        {"SELECT 1 FROM \"messages\" WHERE \"messages\".\"room_id\" = ? "
         "LIMIT 1 OFFSET 40"},
    [CF_MS_SEARCH_IN_ROOM] =
        {"SELECT " CF_MESSAGE_COLUMNS " FROM \"messages\" join "
         "message_search_index idx on messages.id = idx.rowid WHERE "
         "\"messages\".\"room_id\" = ? AND (idx.body match ?) ORDER BY "
         "\"messages\".\"created_at\" ASC"},
    [CF_MS_SEARCH_REACHABLE] =
        {CF_MESSAGE_REACHABLE " join message_search_index idx on messages.id "
         "= idx.rowid WHERE \"memberships\".\"user_id\" = ? AND (idx.body "
         "match ?) ORDER BY \"messages\".\"id\" DESC LIMIT 100"},
    [CF_MS_SEARCH_PROBE] =
        {"SELECT m.id, mm.user_id IS NOT NULL FROM message_search_index idx "
         "JOIN messages m ON m.id=idx.rowid LEFT JOIN memberships mm "
         "ON mm.room_id=m.room_id AND mm.user_id=? WHERE idx.body MATCH ? "
         "ORDER BY idx.rowid DESC LIMIT 1000"},
    [CF_MS_SEARCH_SELECTED] =
        {CF_MESSAGE_REACHABLE " WHERE memberships.user_id=? AND messages.id "
         "IN (SELECT value FROM json_each(?)) ORDER BY messages.id DESC"},
    [CF_MS_MENTIONEES_IN_ROOM] =
        {"SELECT \"users\".\"id\", \"users\".\"name\", "
         "\"users\".\"email_address\", \"users\".\"password_digest\", "
         "\"users\".\"role\", \"users\".\"status\", \"users\".\"bio\", "
         "\"users\".\"bot_token\", \"users\".\"created_at\", "
         "\"users\".\"updated_at\" FROM \"users\" INNER JOIN \"memberships\" "
         "ON \"users\".\"id\" = \"memberships\".\"user_id\" WHERE "
         "\"memberships\".\"room_id\" = ? AND \"users\".\"id\" IN (SELECT "
         "value FROM json_each(?))"},
    [CF_MS_INSERT] =
        {"INSERT INTO \"messages\" (\"client_message_id\", \"created_at\", "
         "\"creator_id\", \"room_id\", \"updated_at\") VALUES (?, ?, ?, ?, ?) "
         "RETURNING \"id\""},
    [CF_MS_TOUCH] =
        {"UPDATE \"messages\" SET \"updated_at\" = ? WHERE "
         "\"messages\".\"id\" = ?"},
    [CF_MS_DELETE] =
        {"DELETE FROM \"messages\" WHERE \"messages\".\"id\" = ?"},
    [CF_MS_INDEX_INSERT] =
        {"insert into message_search_index(rowid, body) values (?, ?)"},
    [CF_MS_INDEX_UPDATE] =
        {"update message_search_index set body = ? where rowid = ?"},
    [CF_MS_INDEX_DELETE] =
        {"delete from message_search_index where rowid = ?"},
};

static const cf_stmt_set cf_message_stmts = {
    cf_message_stmt_defs,
    sizeof cf_message_stmt_defs / sizeof cf_message_stmt_defs[0],
};

/* --- small helpers -------------------------------------------------------- */

static cf_err fail_db(cf_db *db, cf_err rc, const char *what) {
    sqlite3 *handle = cf_db_handle(db);
    return cf_db_failf(rc, "%s: %s", what,
                       handle != NULL ? sqlite3_errmsg(handle) : "no database");
}

/* Owned NUL-terminated copy of a borrowed span (cf_str_dispose releases). */
static cf_err span_copy(cf_span span, cf_str *out) {
    char *bytes = malloc(span.len + 1);
    if (bytes == NULL) return CF_NOMEM;
    if (span.len != 0) memcpy(bytes, span.ptr, span.len);
    bytes[span.len] = '\0';
    out->ptr = bytes;
    out->len = span.len;
    return CF_OK;
}

static bool str_equal(cf_str a, cf_str b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.ptr, b.ptr, a.len) == 0);
}

/* NOT NULL text column -> owned cf_str. */
static cf_err column_text_required(sqlite3_stmt *stmt, int column,
                                   cf_str *out) {
    out->ptr = NULL;
    out->len = 0;
    if (cf_stmt_column_is_null(stmt, column)) {
        return cf_db_failf(CF_DB, "unexpected NULL in text column %d", column);
    }
    return span_copy(cf_stmt_column_text(stmt, column), out);
}

/* Nullable text column -> cf_optional_str. */
static cf_err column_text_optional(sqlite3_stmt *stmt, int column,
                                   cf_optional_str *out) {
    memset(out, 0, sizeof *out);
    if (cf_stmt_column_is_null(stmt, column)) return CF_OK;
    out->present = true;
    return span_copy(cf_stmt_column_text(stmt, column), &out->value);
}

static cf_err column_time(sqlite3_stmt *stmt, int column, int64_t *out_us) {
    if (cf_stmt_column_is_null(stmt, column)) {
        return cf_db_failf(CF_DB, "unexpected NULL in datetime column %d",
                           column);
    }
    return cf_db_time_from_text(cf_stmt_column_text(stmt, column), out_us);
}

static cf_err bind_time(sqlite3_stmt *stmt, int index, int64_t us) {
    char text[CF_DB_TIME_TEXT_CAP];
    cf_err rc = cf_db_time_to_text(us, text);
    if (rc != CF_OK) return rc;
    cf_span span = {(const unsigned char *)text, strlen(text)};
    return cf_stmt_bind_text(stmt, index, span);
}

static cf_err bind_str(sqlite3_stmt *stmt, int index, cf_str text) {
    cf_span span = {(const unsigned char *)text.ptr, text.len};
    return cf_stmt_bind_text(stmt, index, span);
}

/* cf_int64_vector_dispose is defined once in src/models/types.c. */

void cf_message_dispose(cf_message *message) {
    if (message == NULL) return;
    cf_str_dispose(&message->client_message_id);
    memset(message, 0, sizeof *message);
}

static cf_err message_copy(const cf_message *source, cf_message *out) {
    memset(out, 0, sizeof *out);
    out->id = source->id;
    out->room_id = source->room_id;
    out->creator_id = source->creator_id;
    out->created_at = source->created_at;
    out->updated_at = source->updated_at;
    cf_span span = {(const unsigned char *)source->client_message_id.ptr,
                    source->client_message_id.len};
    return span_copy(span, &out->client_message_id);
}

void cf_message_vector_dispose(cf_message_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len; i++) {
        cf_message_dispose(&vector->items[i]);
    }
    free(vector->items);
    memset(vector, 0, sizeof *vector);
}

static cf_err message_vector_push(cf_message_vector *vector,
                                  cf_message value) {
    if (vector->len == vector->cap) {
        size_t cap = vector->cap != 0 ? vector->cap * 2 : 4;
        if (cap < vector->cap || cap > SIZE_MAX / sizeof(cf_message)) {
            return CF_LIMIT;
        }
        cf_message *items = realloc(vector->items, cap * sizeof *items);
        if (items == NULL) return CF_NOMEM;
        vector->items = items;
        vector->cap = cap;
    }
    vector->items[vector->len++] = value;
    return CF_OK;
}

/* Copy the current row of a message select (column order = CF_MESSAGE_COLUMNS). */
static cf_err message_from_row(sqlite3_stmt *stmt, cf_message *out) {
    memset(out, 0, sizeof *out);
    out->id = cf_stmt_column_i64(stmt, 0);
    out->room_id = cf_stmt_column_i64(stmt, 1);
    out->creator_id = cf_stmt_column_i64(stmt, 2);
    cf_err rc = column_text_required(stmt, 3, &out->client_message_id);
    if (rc == CF_OK) rc = column_time(stmt, 4, &out->created_at);
    if (rc == CF_OK) rc = column_time(stmt, 5, &out->updated_at);
    if (rc != CF_OK) cf_message_dispose(out);
    return rc;
}

/* Copy the current row of the mentionees select (users column order). */
static cf_err user_from_row(sqlite3_stmt *stmt, cf_user *out) {
    memset(out, 0, sizeof *out);
    out->id = cf_stmt_column_i64(stmt, 0);
    cf_err rc = column_text_required(stmt, 1, &out->name);
    if (rc == CF_OK) rc = column_text_optional(stmt, 2, &out->email_address);
    if (rc == CF_OK) {
        rc = column_text_optional(stmt, 3, &out->password_digest);
    }
    if (rc == CF_OK) out->role = (cf_role)cf_stmt_column_i64(stmt, 4);
    if (rc == CF_OK) out->status = (cf_status)cf_stmt_column_i64(stmt, 5);
    if (rc == CF_OK) rc = column_text_optional(stmt, 6, &out->bio);
    if (rc == CF_OK) rc = column_text_optional(stmt, 7, &out->bot_token);
    if (rc == CF_OK) rc = column_time(stmt, 8, &out->created_at);
    if (rc == CF_OK) rc = column_time(stmt, 9, &out->updated_at);
    if (rc != CF_OK) cf_user_dispose(out);
    return rc;
}

static cf_err user_vector_push(cf_user_vector *vector, cf_user value) {
    if (vector->len == vector->cap) {
        size_t cap = vector->cap != 0 ? vector->cap * 2 : 4;
        if (cap < vector->cap || cap > SIZE_MAX / sizeof(cf_user)) {
            return CF_LIMIT;
        }
        cf_user *items = realloc(vector->items, cap * sizeof *items);
        if (items == NULL) return CF_NOMEM;
        vector->items = items;
        vector->cap = cap;
    }
    vector->items[vector->len++] = value;
    return CF_OK;
}

/* --- generic statement runners -------------------------------------------- */

typedef cf_err (*cf_bind_fn)(sqlite3_stmt *stmt, void *context);

static cf_err collect_messages(cf_db *db, size_t statement_id, cf_bind_fn bind,
                               void *context, bool clear, bool reverse,
                               cf_message_vector *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "no output vector");
    if (clear) memset(out, 0, sizeof *out);
    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &cf_message_stmts, statement_id, &stmt);
    if (rc != CF_OK) return rc;
    if (bind != NULL && (rc = bind(stmt, context)) != CF_OK) {
        cf_db_stmt_done(stmt);
        return rc;
    }
    for (;;) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            cf_message row;
            rc = message_from_row(stmt, &row);
            if (rc == CF_OK) rc = message_vector_push(out, row);
            if (rc != CF_OK) {
                cf_message_dispose(&row);
                cf_db_stmt_done(stmt);
                cf_message_vector_dispose(out);
                return rc;
            }
        } else if (step == SQLITE_DONE) {
            break;
        } else {
            cf_db_stmt_done(stmt);
            cf_message_vector_dispose(out);
            return fail_db(db, cf_db_err(step), "message query failed");
        }
    }
    cf_db_stmt_done(stmt);
    if (reverse) {
        for (size_t i = 0, j = out->len; i + 1 < j; i++, j--) {
            cf_message swap = out->items[i];
            out->items[i] = out->items[j - 1];
            out->items[j - 1] = swap;
        }
    }
    return CF_OK;
}

static cf_err read_one_message(cf_db *db, size_t statement_id,
                               cf_bind_fn bind, void *context, bool *found,
                               cf_message *out) {
    if (found == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "no output pointer");
    }
    memset(out, 0, sizeof *out);
    *found = false;
    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &cf_message_stmts, statement_id, &stmt);
    if (rc != CF_OK) return rc;
    if (bind != NULL && (rc = bind(stmt, context)) != CF_OK) {
        cf_db_stmt_done(stmt);
        return rc;
    }
    int step = sqlite3_step(stmt);
    if (step == SQLITE_ROW) {
        rc = message_from_row(stmt, out);
        if (rc == CF_OK) *found = true;
    } else if (step != SQLITE_DONE) {
        rc = fail_db(db, cf_db_err(step), "message query failed");
    }
    cf_db_stmt_done(stmt);
    return rc;
}

static cf_err read_count(cf_db *db, size_t statement_id, cf_bind_fn bind,
                         void *context, int64_t *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "no output pointer");
    *out = 0;
    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &cf_message_stmts, statement_id, &stmt);
    if (rc != CF_OK) return rc;
    if (bind != NULL && (rc = bind(stmt, context)) != CF_OK) {
        cf_db_stmt_done(stmt);
        return rc;
    }
    int step = sqlite3_step(stmt);
    if (step == SQLITE_ROW) {
        *out = cf_stmt_column_i64(stmt, 0);
    } else if (step != SQLITE_DONE) {
        rc = fail_db(db, cf_db_err(step), "count query failed");
    }
    cf_db_stmt_done(stmt);
    return rc;
}

static cf_err read_exists(cf_db *db, size_t statement_id, cf_bind_fn bind,
                          void *context, bool *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "no output pointer");
    *out = false;
    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &cf_message_stmts, statement_id, &stmt);
    if (rc != CF_OK) return rc;
    if (bind != NULL && (rc = bind(stmt, context)) != CF_OK) {
        cf_db_stmt_done(stmt);
        return rc;
    }
    int step = sqlite3_step(stmt);
    if (step == SQLITE_ROW) {
        *out = true;
    } else if (step != SQLITE_DONE) {
        rc = fail_db(db, cf_db_err(step), "existence query failed");
    }
    cf_db_stmt_done(stmt);
    return rc;
}

/* --- bind contexts -------------------------------------------------------- */

struct bind_int64 {
    int64_t value;
};

struct bind_pair {
    int64_t first, second;
};

struct bind_time {
    int64_t room_id;
    int64_t time_us;
};

struct bind_text {
    int64_t room_id;
    cf_str text;
};

struct bind_excluding {
    int64_t room_id;
    cf_str ids_json;
    int64_t time_us;
};

static cf_err bind_int64(sqlite3_stmt *stmt, void *context) {
    return cf_stmt_bind_i64(stmt, 1, ((struct bind_int64 *)context)->value);
}

static cf_err bind_pair(sqlite3_stmt *stmt, void *context) {
    struct bind_pair *args = context;
    cf_err rc = cf_stmt_bind_i64(stmt, 1, args->first);
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 2, args->second);
    return rc;
}

static cf_err bind_room_time(sqlite3_stmt *stmt, void *context) {
    struct bind_time *args = context;
    cf_err rc = cf_stmt_bind_i64(stmt, 1, args->room_id);
    if (rc == CF_OK) rc = bind_time(stmt, 2, args->time_us);
    return rc;
}

static cf_err bind_room_text(sqlite3_stmt *stmt, void *context) {
    struct bind_text *args = context;
    cf_err rc = cf_stmt_bind_i64(stmt, 1, args->room_id);
    if (rc == CF_OK) rc = bind_str(stmt, 2, args->text);
    return rc;
}

static cf_err bind_excluding(sqlite3_stmt *stmt, void *context) {
    struct bind_excluding *args = context;
    cf_err rc = cf_stmt_bind_i64(stmt, 1, args->room_id);
    if (rc == CF_OK) rc = bind_str(stmt, 2, args->ids_json);
    if (rc == CF_OK) rc = bind_time(stmt, 3, args->time_us);
    return rc;
}

/* --- enums and record ownership ------------------------------------------- */

const char *cf_content_type_name(cf_content_type content_type) {
    switch (content_type) {
    case CF_CONTENT_TYPE_ATTACHMENT:
        return "attachment";
    case CF_CONTENT_TYPE_SOUND:
        return "sound";
    case CF_CONTENT_TYPE_TEXT:
        return "text";
    }
    return NULL;
}

/* --- finders and counts --------------------------------------------------- */

cf_err cf_message_find_by_id(cf_db *db, int64_t id, bool *found,
                             cf_message *out) {
    struct bind_int64 args = {id};
    return read_one_message(db, CF_MS_FIND_BY_ID, bind_int64, &args, found,
                            out);
}

cf_err cf_message_find(cf_db *db, int64_t id, cf_message *out) {
    bool found = false;
    cf_err rc = cf_message_find_by_id(db, id, &found, out);
    if (rc != CF_OK) return rc;
    if (!found) return cf_db_failf(CF_NOT_FOUND, "Couldn't find Message");
    return CF_OK;
}

cf_err cf_message_last(cf_db *db, bool *found, cf_message *out) {
    return read_one_message(db, CF_MS_LAST, NULL, NULL, found, out);
}

cf_err cf_message_count(cf_db *db, int64_t *out) {
    return read_count(db, CF_MS_COUNT, NULL, NULL, out);
}

cf_err cf_message_by_creator(cf_db *db, int64_t creator_id,
                             cf_message_vector *out) {
    struct bind_int64 args = {creator_id};
    return collect_messages(db, CF_MS_BY_CREATOR, bind_int64, &args, true,
                            false, out);
}

cf_err cf_message_for_room(cf_db *db, int64_t room_id,
                           cf_message_vector *out) {
    struct bind_int64 args = {room_id};
    return collect_messages(db, CF_MS_IN_ROOM, bind_int64, &args, true, false,
                            out);
}

cf_err cf_message_find_in_room(cf_db *db, int64_t room_id, int64_t id,
                               cf_message *out) {
    struct bind_pair args = {room_id, id};
    bool found = false;
    cf_err rc = read_one_message(db, CF_MS_FIND_IN_ROOM, bind_pair, &args,
                                 &found, out);
    if (rc != CF_OK) return rc;
    if (!found) return cf_db_failf(CF_NOT_FOUND, "Couldn't find Message");
    return CF_OK;
}

cf_err cf_message_count_in_room(cf_db *db, int64_t room_id, int64_t *out) {
    struct bind_int64 args = {room_id};
    return read_count(db, CF_MS_COUNT_IN_ROOM, bind_int64, &args, out);
}

cf_err cf_message_find_reachable(cf_db *db, int64_t user_id, int64_t id,
                                 cf_message *out) {
    struct bind_pair args = {user_id, id};
    bool found = false;
    cf_err rc = read_one_message(db, CF_MS_FIND_REACHABLE, bind_pair, &args,
                                 &found, out);
    if (rc != CF_OK) return rc;
    if (!found) return cf_db_failf(CF_NOT_FOUND, "Couldn't find Message");
    return CF_OK;
}

/* --- pagination ----------------------------------------------------------- */

cf_err cf_message_last_page(cf_db *db, int64_t room_id,
                            cf_message_vector *out) {
    struct bind_int64 args = {room_id};
    return collect_messages(db, CF_MS_LAST_PAGE, bind_int64, &args, true, true,
                            out);
}

cf_err cf_message_first_page(cf_db *db, int64_t room_id,
                             cf_message_vector *out) {
    struct bind_int64 args = {room_id};
    return collect_messages(db, CF_MS_FIRST_PAGE, bind_int64, &args, true,
                            false, out);
}

cf_err cf_message_page_before(cf_db *db, int64_t room_id,
                              const cf_message *message,
                              cf_message_vector *out) {
    struct bind_time args = {room_id, message->created_at};
    return collect_messages(db, CF_MS_PAGE_BEFORE, bind_room_time, &args, true,
                            true, out);
}

cf_err cf_message_page_after(cf_db *db, int64_t room_id,
                             const cf_message *message,
                             cf_message_vector *out) {
    struct bind_time args = {room_id, message->created_at};
    return collect_messages(db, CF_MS_PAGE_AFTER, bind_room_time, &args, true,
                            false, out);
}

cf_err cf_message_page_around(cf_db *db, int64_t room_id,
                              const cf_message *message,
                              cf_message_vector *out) {
    struct bind_time args = {room_id, message->created_at};
    cf_err rc = collect_messages(db, CF_MS_PAGE_BEFORE, bind_room_time, &args,
                                 true, true, out);
    if (rc != CF_OK) return rc;
    cf_message copy;
    rc = message_copy(message, &copy);
    if (rc == CF_OK) rc = message_vector_push(out, copy);
    if (rc != CF_OK) {
        cf_message_dispose(&copy);
        cf_message_vector_dispose(out);
        return rc;
    }
    rc = collect_messages(db, CF_MS_PAGE_AFTER, bind_room_time, &args, false,
                          false, out);
    if (rc != CF_OK) {
        cf_message_vector_dispose(out);
        return rc;
    }
    return CF_OK;
}

cf_err cf_message_page_created_since(cf_db *db, int64_t room_id,
                                     int64_t time_us,
                                     cf_message_vector *out) {
    struct bind_time args = {room_id, time_us};
    return collect_messages(db, CF_MS_PAGE_CREATED_SINCE, bind_room_time,
                            &args, true, false, out);
}


cf_err cf_message_page_updated_since(cf_db *db, int64_t room_id,
                                     int64_t time_us,
                                     const int64_t *excluding,
                                     size_t excluding_len,
                                     cf_message_vector *out) {
    if (excluding_len == 0) {
        struct bind_time args = {room_id, time_us};
        return collect_messages(db, CF_MS_PAGE_UPDATED_SINCE, bind_room_time,
                                &args, true, true, out);
    }
    cf_str ids = {0};
    cf_err rc = cf_db_ids_json(excluding, excluding_len, &ids);
    if (rc != CF_OK) return rc;
    struct bind_excluding args = {room_id, ids, time_us};
    rc = collect_messages(db, CF_MS_PAGE_UPDATED_SINCE_EXCLUDING,
                          bind_excluding, &args, true, true, out);
    cf_str_dispose(&ids);
    return rc;
}

cf_err cf_message_exists_before(cf_db *db, int64_t room_id,
                                const cf_message *message, bool *out) {
    struct bind_time args = {room_id, message->created_at};
    return read_exists(db, CF_MS_EXISTS_BEFORE, bind_room_time, &args, out);
}

cf_err cf_message_exists_after(cf_db *db, int64_t room_id,
                               const cf_message *message, bool *out) {
    struct bind_time args = {room_id, message->created_at};
    return read_exists(db, CF_MS_EXISTS_AFTER, bind_room_time, &args, out);
}

cf_err cf_message_paged(cf_db *db, int64_t room_id, bool *out) {
    struct bind_int64 args = {room_id};
    return read_exists(db, CF_MS_PAGED, bind_int64, &args, out);
}

/* --- search --------------------------------------------------------------- */

/* Unicode White_Space (Rust char::is_whitespace), decoded from UTF-8; an
 * invalid byte is its own non-space code point (Rust &str input is valid). */
static bool cp_is_whitespace(uint32_t cp) {
    return (cp >= 0x09 && cp <= 0x0D) || cp == 0x20 || cp == 0x85 ||
           cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
           cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F ||
           cp == 0x3000;
}

static size_t utf8_decode(const unsigned char *bytes, size_t len,
                          uint32_t *out) {
    unsigned char b0 = bytes[0];
    if (b0 < 0x80 || len < 2) {
        *out = b0;
        return 1;
    }
    if ((b0 & 0xE0) == 0xC0 && (bytes[1] & 0xC0) == 0x80) {
        *out = ((uint32_t)(b0 & 0x1F) << 6) | (bytes[1] & 0x3F);
        return *out >= 0x80 ? 2 : 1;
    }
    if (len >= 3 && (b0 & 0xF0) == 0xE0 && (bytes[1] & 0xC0) == 0x80 &&
        (bytes[2] & 0xC0) == 0x80) {
        *out = ((uint32_t)(b0 & 0x0F) << 12) |
               ((uint32_t)(bytes[1] & 0x3F) << 6) | (bytes[2] & 0x3F);
        return *out >= 0x800 ? 3 : 1;
    }
    if (len >= 4 && (b0 & 0xF8) == 0xF0 && (bytes[1] & 0xC0) == 0x80 &&
        (bytes[2] & 0xC0) == 0x80 && (bytes[3] & 0xC0) == 0x80) {
        *out = ((uint32_t)(b0 & 0x07) << 18) |
               ((uint32_t)(bytes[1] & 0x3F) << 12) |
               ((uint32_t)(bytes[2] & 0x3F) << 6) | (bytes[3] & 0x3F);
        return *out >= 0x10000 && *out <= 0x10FFFF ? 4 : 1;
    }
    *out = b0;
    return 1;
}

/* Growable byte buffer used while assembling FTS query terms. */
struct bytes_builder {
    char *ptr;
    size_t len, cap;
};

static cf_err builder_put(struct bytes_builder *b, const char *bytes,
                          size_t len) {
    if (len > SIZE_MAX - b->len) return CF_LIMIT;
    size_t need = b->len + len;
    if (need > b->cap) {
        size_t cap = b->cap != 0 ? b->cap : 64;
        while (cap < need) {
            if (cap > SIZE_MAX / 2) {
                cap = need;
                break;
            }
            cap *= 2;
        }
        char *ptr = realloc(b->ptr, cap);
        if (ptr == NULL) return CF_NOMEM;
        b->ptr = ptr;
        b->cap = cap;
    }
    if (len != 0) memcpy(b->ptr + b->len, bytes, len);
    b->len = need;
    return CF_OK;
}

/* Rust match_terms: every whitespace/NUL-separated word as an FTS5 string
 * ("\"" escaped by doubling); words joined with a space. */
static cf_err match_terms(cf_str query, cf_str *out) {
    memset(out, 0, sizeof *out);
    struct bytes_builder b = {0};
    const unsigned char *bytes = (const unsigned char *)query.ptr;
    size_t i = 0, word_start = 0;
    bool in_word = false, first = true;
    cf_err rc = CF_OK;
    while (i < query.len) {
        uint32_t cp;
        size_t width = utf8_decode(bytes + i, query.len - i, &cp);
        if (cp == 0 || cp_is_whitespace(cp)) {
            if (in_word) {
                if (!first && (rc = builder_put(&b, " ", 1)) != CF_OK) break;
                first = false;
                if ((rc = builder_put(&b, "\"", 1)) != CF_OK) break;
                for (size_t k = word_start; k < i; k++) {
                    if (bytes[k] == '"' &&
                        (rc = builder_put(&b, "\"\"", 2)) != CF_OK) {
                        break;
                    }
                    if (bytes[k] != '"' && (rc = builder_put(&b, (char *)&bytes[k], 1)) != CF_OK) {
                        break;
                    }
                }
                if (rc != CF_OK) break;
                if ((rc = builder_put(&b, "\"", 1)) != CF_OK) break;
                in_word = false;
            }
        } else if (!in_word) {
            in_word = true;
            word_start = i;
        }
        i += width;
    }
    if (rc == CF_OK && in_word) {
        if (!first && (rc = builder_put(&b, " ", 1)) != CF_OK) goto done;
        if ((rc = builder_put(&b, "\"", 1)) != CF_OK) goto done;
        for (size_t k = word_start; k < query.len; k++) {
            if (bytes[k] == '"') {
                if ((rc = builder_put(&b, "\"\"", 2)) != CF_OK) goto done;
            } else if ((rc = builder_put(&b, (char *)&bytes[k], 1)) != CF_OK) {
                goto done;
            }
        }
        if ((rc = builder_put(&b, "\"", 1)) != CF_OK) goto done;
        in_word = false;
    }
done:
    if (rc != CF_OK) {
        free(b.ptr);
        return rc;
    }
    if (b.len != 0) {
        if ((rc = builder_put(&b, "", 1)) != CF_OK) { /* NUL terminator */
            free(b.ptr);
            return rc;
        }
        b.len--; /* the terminator is not part of the value */
    }
    out->ptr = b.ptr;
    out->len = b.len;
    return CF_OK;
}

cf_err cf_message_search_in_room(cf_db *db, int64_t room_id, cf_str query,
                                 cf_message_vector *out) {
    cf_str terms = {0};
    cf_err rc = match_terms(query, &terms);
    if (rc != CF_OK) return rc;
    if (terms.len == 0) {
        cf_str_dispose(&terms);
        if (out == NULL) return cf_db_failf(CF_INVALID, "no output vector");
        memset(out, 0, sizeof *out);
        return CF_OK;
    }
    struct bind_text args = {room_id, terms};
    rc = collect_messages(db, CF_MS_SEARCH_IN_ROOM, bind_room_text, &args, true,
                          false, out);
    cf_str_dispose(&terms);
    return rc;
}

cf_err cf_message_search_reachable(cf_db *db, int64_t user_id, cf_str query,
                                   cf_message_vector *out) {
    cf_str terms = {0};
    cf_err rc = match_terms(query, &terms);
    if (rc != CF_OK) return rc;
    if (terms.len == 0) {
        cf_str_dispose(&terms);
        if (out == NULL) return cf_db_failf(CF_INVALID, "no output vector");
        memset(out, 0, sizeof *out);
        return CF_OK;
    }
    /* Probe a bounded reverse FTS window without decoding inaccessible rows.
     * Sparse memberships use the scoped query, so older reachable hits survive. */
    int64_t ids[100];
    size_t examined = 0, selected = 0;
    sqlite3_stmt *stmt = NULL;
    rc = cf_db_stmt(db, &cf_message_stmts, CF_MS_SEARCH_PROBE, &stmt);
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 1, user_id);
    if (rc == CF_OK) rc = bind_str(stmt, 2, terms);
    while (rc == CF_OK && selected < 100) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            rc = fail_db(db, cf_db_err(step), "search probe failed");
            break;
        }
        examined++;
        if (cf_stmt_column_i64(stmt, 1) != 0) {
            ids[selected++] = cf_stmt_column_i64(stmt, 0);
        }
    }
    if (stmt != NULL) cf_db_stmt_done(stmt);
    if (rc == CF_OK) {
        struct bind_text args = {user_id, terms};
        if (examined == 1000 && selected < 100) {
            rc = collect_messages(db, CF_MS_SEARCH_REACHABLE, bind_room_text,
                                  &args, true, true, out);
        } else {
            cf_str json = {0};
            rc = cf_db_ids_json(ids, selected, &json);
            if (rc == CF_OK) {
                args.text = json;
                rc = collect_messages(db, CF_MS_SEARCH_SELECTED, bind_room_text,
                                      &args, true, true, out);
            }
            cf_str_dispose(&json);
        }
    }
    cf_str_dispose(&terms);
    return rc;
}

/* --- mentionees ----------------------------------------------------------- */

cf_err cf_message_mentionees_in_room(cf_db *db, int64_t room_id,
                                     const int64_t *user_ids,
                                     size_t user_ids_len,
                                     cf_user_vector *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "no output vector");
    memset(out, 0, sizeof *out);
    if (user_ids_len == 0) return CF_OK;
    if (user_ids == NULL) {
        return cf_db_failf(CF_INVALID, "no user id list");
    }
    cf_str ids = {0};
    cf_err rc = cf_db_ids_json(user_ids, user_ids_len, &ids);
    if (rc != CF_OK) return rc;
    struct bind_text args = {room_id, ids};
    sqlite3_stmt *stmt = NULL;
    rc = cf_db_stmt(db, &cf_message_stmts, CF_MS_MENTIONEES_IN_ROOM, &stmt);
    if (rc == CF_OK) rc = bind_room_text(stmt, &args);
    if (rc == CF_OK) {
        for (;;) {
            int step = sqlite3_step(stmt);
            if (step == SQLITE_ROW) {
                cf_user row = {0};
                rc = user_from_row(stmt, &row);
                if (rc == CF_OK) rc = user_vector_push(out, row);
                if (rc != CF_OK) {
                    cf_user_dispose(&row);
                    break;
                }
            } else if (step == SQLITE_DONE) {
                break;
            } else {
                rc = fail_db(db, cf_db_err(step), "mentionees query failed");
                break;
            }
        }
    }
    if (stmt != NULL) cf_db_stmt_done(stmt);
    cf_str_dispose(&ids);
    if (rc != CF_OK) cf_user_vector_dispose(out);
    return rc;
}

/* Rust plain_text_body.match(/\A\/play (?<name>\w+)\z/) then find_by_name. */
bool cf_message_sound_in(cf_str plain_text, const cf_sound **out) {
    if (out == NULL) return false;
    *out = NULL;
    static const char prefix[] = "/play ";
    size_t prefix_len = sizeof prefix - 1;
    if (plain_text.len <= prefix_len ||
        memcmp(plain_text.ptr, prefix, prefix_len) != 0) {
        return false;
    }
    size_t name_len = plain_text.len - prefix_len;
    const unsigned char *name = (const unsigned char *)plain_text.ptr +
                                prefix_len;
    for (size_t i = 0; i < name_len; i++) {
        bool word = (name[i] >= 'a' && name[i] <= 'z') ||
                    (name[i] >= 'A' && name[i] <= 'Z') ||
                    (name[i] >= '0' && name[i] <= '9') || name[i] == '_';
        if (!word) return false;
    }
    cf_str name_str = {(char *)name, name_len};
    return cf_sound_find_by_name(name_str, out);
}

/* --- creating, updating, destroying --------------------------------------- */

/* Rust sql::uuid(): a random v4 UUID in lowercase hyphenated form. */
static cf_err generate_uuid(cf_str *out) {
    unsigned char bytes[16];
    cf_err rc = cf_random_bytes(bytes, sizeof bytes);
    if (rc != CF_OK) return rc;
    bytes[6] = (unsigned char)((bytes[6] & 0x0F) | 0x40);
    bytes[8] = (unsigned char)((bytes[8] & 0x3F) | 0x80);
    static const char hex[] = "0123456789abcdef";
    char text[36];
    size_t pos = 0;
    for (size_t i = 0; i < sizeof bytes; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) text[pos++] = '-';
        text[pos++] = hex[bytes[i] >> 4];
        text[pos++] = hex[bytes[i] & 0x0F];
    }
    cf_span span = {(const unsigned char *)text, sizeof text};
    return span_copy(span, out);
}

static cf_err touch_row(cf_tx *tx, int64_t id, int64_t *out_us) {
    cf_db *db = cf_tx_db(tx);
    int64_t now = cf_now_us(NULL);
    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &cf_message_stmts, CF_MS_TOUCH, &stmt);
    if (rc == CF_OK) rc = bind_time(stmt, 1, now);
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 2, id);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step != SQLITE_DONE) {
            rc = fail_db(db, cf_db_err(step), "message touch failed");
        }
    }
    if (stmt != NULL) cf_db_stmt_done(stmt);
    if (rc == CF_OK) *out_us = now;
    return rc;
}

/* Rust Message::create_in_index (after commit in the reference; inside the
 * caller's transaction here, per D-C09). */
static cf_err create_in_index(cf_tx *tx, cf_db *db,
                              const cf_message *message) {
    cf_str body = {0};
    cf_err rc = cf_message_plain_text_body(db, message, cf_tx_rich_text(tx),
                                           &body);
    if (rc != CF_OK) return rc;
    sqlite3_stmt *stmt = NULL;
    rc = cf_db_stmt(db, &cf_message_stmts, CF_MS_INDEX_INSERT, &stmt);
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 1, message->id);
    if (rc == CF_OK) rc = bind_str(stmt, 2, body);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step != SQLITE_DONE) {
            rc = fail_db(db, cf_db_err(step), "search index insert failed");
        }
    }
    if (stmt != NULL) cf_db_stmt_done(stmt);
    cf_str_dispose(&body);
    return rc;
}

static cf_err update_in_index(cf_tx *tx, cf_db *db,
                              const cf_message *message) {
    cf_str body = {0};
    cf_err rc = cf_message_plain_text_body(db, message, cf_tx_rich_text(tx),
                                           &body);
    if (rc != CF_OK) return rc;
    sqlite3_stmt *stmt = NULL;
    rc = cf_db_stmt(db, &cf_message_stmts, CF_MS_INDEX_UPDATE, &stmt);
    if (rc == CF_OK) rc = bind_str(stmt, 1, body);
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 2, message->id);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step != SQLITE_DONE) {
            rc = fail_db(db, cf_db_err(step), "search index update failed");
        }
    }
    if (stmt != NULL) cf_db_stmt_done(stmt);
    cf_str_dispose(&body);
    return rc;
}

static cf_err remove_from_index(cf_tx *tx, int64_t id) {
    cf_db *db = cf_tx_db(tx);
    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &cf_message_stmts, CF_MS_INDEX_DELETE, &stmt);
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 1, id);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step != SQLITE_DONE) {
            rc = fail_db(db, cf_db_err(step), "search index delete failed");
        }
    }
    if (stmt != NULL) cf_db_stmt_done(stmt);
    return rc;
}

cf_err cf_message_create(cf_tx *tx, const cf_new_message *attributes,
                         cf_message *out) {
    if (tx == NULL || attributes == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "invalid create argument");
    }
    memset(out, 0, sizeof *out);
    cf_db *db = cf_tx_db(tx);
    int64_t now = cf_now_us(NULL);

    cf_str client_message_id = {0};
    cf_err rc;
    if (attributes->client_message_id.present) {
        rc = span_copy(attributes->client_message_id.value.ptr != NULL
                           ? (cf_span){(const unsigned char *)
                                           attributes->client_message_id
                                               .value.ptr,
                                       attributes->client_message_id.value.len}
                           : (cf_span){NULL, 0},
                       &client_message_id);
    } else {
        rc = generate_uuid(&client_message_id);
    }
    if (rc != CF_OK) return rc;

    sqlite3_stmt *stmt = NULL;
    rc = cf_db_stmt(db, &cf_message_stmts, CF_MS_INSERT, &stmt);
    if (rc == CF_OK) rc = bind_str(stmt, 1, client_message_id);
    if (rc == CF_OK) rc = bind_time(stmt, 2, now);
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 3, attributes->creator_id);
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 4, attributes->room_id);
    if (rc == CF_OK) rc = bind_time(stmt, 5, now);
    int64_t id = 0;
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            id = cf_stmt_column_i64(stmt, 0);
        } else {
            rc = fail_db(db, cf_db_err(step), "message insert failed");
        }
    }
    if (stmt != NULL) cf_db_stmt_done(stmt);
    if (rc != CF_OK) {
        cf_str_dispose(&client_message_id);
        return rc;
    }

    cf_message message = {id, attributes->room_id, attributes->creator_id,
                          client_message_id, now, now};

    bool touched = false;
    if (attributes->body.present) {
        cf_rich_text_record record = {0};
        rc = cf_rich_text_record_create(
            tx, CF_STR_LIT(CF_MESSAGE_RECORD_TYPE), id,
            CF_STR_LIT(CF_MESSAGE_BODY_NAME), attributes->body.value, &record);
        cf_rich_text_record_dispose(&record);
        if (rc != CF_OK) goto fail;
        touched = true;
    }
    if (attributes->attachment_blob_id.present) {
        cf_attachment attachment = {0};
        rc = cf_attachment_create(
            tx, CF_STR_LIT(CF_MESSAGE_RECORD_TYPE), id,
            CF_STR_LIT(CF_MESSAGE_ATTACHMENT_NAME),
            attributes->attachment_blob_id.value, &attachment);
        cf_attachment_dispose(&attachment);
        if (rc != CF_OK) goto fail;
        touched = true;
    }
    if (touched) {
        rc = touch_row(tx, id, &message.updated_at);
        if (rc != CF_OK) goto fail;
    }
    rc = cf_room_touch(tx, message.room_id);
    if (rc != CF_OK) goto fail;
    rc = create_in_index(tx, db, &message);
    if (rc != CF_OK) goto fail;
    rc = cf_room_receive(tx, message.room_id, &message);
    if (rc != CF_OK) goto fail;
    *out = message;
    return CF_OK;

fail:
    cf_message_dispose(&message);
    return rc;
}

cf_err cf_message_update_body(cf_tx *tx, cf_message *message, cf_str body) {
    if (tx == NULL || message == NULL) {
        return cf_db_failf(CF_INVALID, "invalid update_body argument");
    }
    cf_db *db = cf_tx_db(tx);
    bool found = false;
    cf_rich_text_record record = {0};
    cf_err rc = cf_rich_text_record_find_for(
        db, CF_STR_LIT(CF_MESSAGE_RECORD_TYPE), message->id,
        CF_STR_LIT(CF_MESSAGE_BODY_NAME), &found, &record);
    if (rc != CF_OK) return rc;
    if (found) {
        if (record.body.present && str_equal(record.body.value, body)) {
            cf_rich_text_record_dispose(&record);
            return CF_OK; /* unchanged: no update, no touch */
        }
        rc = cf_rich_text_record_update_body(tx, &record, body);
        cf_rich_text_record_dispose(&record);
        if (rc != CF_OK) return rc;
    } else {
        cf_rich_text_record created = {0};
        rc = cf_rich_text_record_create(tx, CF_STR_LIT(CF_MESSAGE_RECORD_TYPE),
                                        message->id,
                                        CF_STR_LIT(CF_MESSAGE_BODY_NAME), body,
                                        &created);
        cf_rich_text_record_dispose(&created);
        if (rc != CF_OK) return rc;
    }
    return cf_message_touch(tx, message);
}

cf_err cf_message_touch(cf_tx *tx, cf_message *message) {
    if (tx == NULL || message == NULL) {
        return cf_db_failf(CF_INVALID, "invalid touch argument");
    }
    cf_err rc = touch_row(tx, message->id, &message->updated_at);
    if (rc != CF_OK) return rc;
    rc = cf_room_touch(tx, message->room_id);
    if (rc != CF_OK) return rc;
    cf_db *db = cf_tx_db(tx);
    cf_message fresh = {0};
    rc = cf_message_find(db, message->id, &fresh);
    if (rc != CF_OK) return rc;
    rc = update_in_index(tx, db, &fresh);
    cf_message_dispose(&fresh);
    return rc;
}

cf_err cf_message_replace_attachment(cf_tx *tx, cf_message *message,
                                     cf_optional_i64 blob_id) {
    if (tx == NULL || message == NULL) {
        return cf_db_failf(CF_INVALID, "invalid replace_attachment argument");
    }
    cf_db *db = cf_tx_db(tx);
    bool found = false;
    cf_attachment attachment = {0};
    cf_err rc = cf_attachment_find_for(
        db, CF_STR_LIT(CF_MESSAGE_RECORD_TYPE), message->id,
        CF_STR_LIT(CF_MESSAGE_ATTACHMENT_NAME), &found, &attachment);
    if (rc != CF_OK) return rc;
    if (found) {
        rc = cf_attachment_delete(tx, &attachment);
        if (rc == CF_OK) {
            cf_event event = {0};
            event.kind = CF_EVENT_PURGE_BLOB;
            event.blob_id = attachment.blob_id;
            rc = cf_tx_event(tx, event);
        }
        cf_attachment_dispose(&attachment);
        if (rc != CF_OK) return rc;
        rc = cf_message_touch(tx, message);
        if (rc != CF_OK) return rc;
    }
    if (blob_id.present) {
        cf_attachment created = {0};
        rc = cf_attachment_create(tx, CF_STR_LIT(CF_MESSAGE_RECORD_TYPE),
                                  message->id,
                                  CF_STR_LIT(CF_MESSAGE_ATTACHMENT_NAME),
                                  blob_id.value, &created);
        cf_attachment_dispose(&created);
        if (rc != CF_OK) return rc;
        rc = cf_message_touch(tx, message);
        if (rc != CF_OK) return rc;
    }
    return CF_OK;
}

cf_err cf_message_destroy(cf_tx *tx, const cf_message *message) {
    if (tx == NULL || message == NULL) {
        return cf_db_failf(CF_INVALID, "invalid destroy argument");
    }
    cf_db *db = cf_tx_db(tx);
    bool found = false;
    cf_attachment attachment = {0};
    cf_err rc = cf_attachment_find_for(
        db, CF_STR_LIT(CF_MESSAGE_RECORD_TYPE), message->id,
        CF_STR_LIT(CF_MESSAGE_ATTACHMENT_NAME), &found, &attachment);
    if (rc != CF_OK) return rc;
    if (found) {
        rc = cf_attachment_delete(tx, &attachment);
        if (rc == CF_OK) {
            cf_event event = {0};
            event.kind = CF_EVENT_PURGE_BLOB;
            event.blob_id = attachment.blob_id;
            rc = cf_tx_event(tx, event);
        }
        cf_attachment_dispose(&attachment);
        if (rc != CF_OK) return rc;
    }

    cf_boost_vector boosts = {0};
    rc = cf_boost_for_message(db, message->id, &boosts);
    if (rc != CF_OK) return rc;
    for (size_t i = 0; i < boosts.len; i++) {
        rc = cf_boost_delete_row(tx, &boosts.items[i]);
        if (rc != CF_OK) break;
    }
    cf_boost_vector_dispose(&boosts);
    if (rc != CF_OK) return rc;

    bool body_found = false;
    cf_rich_text_record body = {0};
    rc = cf_rich_text_record_find_for(
        db, CF_STR_LIT(CF_MESSAGE_RECORD_TYPE), message->id,
        CF_STR_LIT(CF_MESSAGE_BODY_NAME), &body_found, &body);
    if (rc != CF_OK) return rc;
    if (body_found) {
        rc = cf_rich_text_record_delete(tx, &body);
        cf_rich_text_record_dispose(&body);
        if (rc != CF_OK) return rc;
    }

    sqlite3_stmt *stmt = NULL;
    rc = cf_db_stmt(db, &cf_message_stmts, CF_MS_DELETE, &stmt);
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 1, message->id);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step != SQLITE_DONE) {
            rc = fail_db(db, cf_db_err(step), "message delete failed");
        }
    }
    if (stmt != NULL) cf_db_stmt_done(stmt);
    if (rc != CF_OK) return rc;

    rc = cf_room_touch(tx, message->room_id);
    if (rc != CF_OK) return rc;
    return remove_from_index(tx, message->id);
}

/* --- attributes and associations ------------------------------------------ */

cf_err cf_message_room(cf_db *db, const cf_message *message, cf_room *out) {
    if (message == NULL) {
        return cf_db_failf(CF_INVALID, "no message");
    }
    return cf_room_find(db, message->room_id, out);
}

cf_err cf_message_creator(cf_db *db, const cf_message *message,
                          cf_user *out) {
    if (message == NULL) {
        return cf_db_failf(CF_INVALID, "no message");
    }
    return cf_user_find(db, message->creator_id, out);
}

cf_err cf_message_boosts(cf_db *db, const cf_message *message,
                         cf_boost_vector *out) {
    if (message == NULL) {
        return cf_db_failf(CF_INVALID, "no message");
    }
    return cf_boost_for_message_ordered(db, message->id, out);
}

cf_err cf_message_body(cf_db *db, const cf_message *message, bool *found,
                       cf_rich_text_record *out) {
    if (message == NULL) {
        return cf_db_failf(CF_INVALID, "no message");
    }
    return cf_rich_text_record_find_for(
        db, CF_STR_LIT(CF_MESSAGE_RECORD_TYPE), message->id,
        CF_STR_LIT(CF_MESSAGE_BODY_NAME), found, out);
}

cf_err cf_message_body_html(cf_db *db, const cf_message *message,
                            bool *found, cf_str *out) {
    if (found == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "no output pointer");
    }
    out->ptr = NULL;
    out->len = 0;
    *found = false;
    bool record_found = false;
    cf_rich_text_record record = {0};
    cf_err rc = cf_message_body(db, message, &record_found, &record);
    if (rc != CF_OK) return rc;
    if (record_found) {
        if (record.body.present) {
            rc = span_copy((cf_span){(const unsigned char *)record.body.value.ptr,
                                     record.body.value.len},
                           out);
            if (rc == CF_OK) *found = true;
        }
        cf_rich_text_record_dispose(&record);
    }
    return rc;
}

cf_err cf_message_attachment(cf_db *db, const cf_message *message,
                             bool *found, cf_attachment *out_attachment,
                             cf_blob *out_blob) {
    if (found == NULL || out_attachment == NULL || out_blob == NULL) {
        return cf_db_failf(CF_INVALID, "no output pointer");
    }
    memset(out_attachment, 0, sizeof *out_attachment);
    memset(out_blob, 0, sizeof *out_blob);
    *found = false;
    if (message == NULL) return cf_db_failf(CF_INVALID, "no message");
    bool attachment_found = false;
    cf_attachment attachment = {0};
    cf_err rc = cf_attachment_find_for(
        db, CF_STR_LIT(CF_MESSAGE_RECORD_TYPE), message->id,
        CF_STR_LIT(CF_MESSAGE_ATTACHMENT_NAME), &attachment_found,
        &attachment);
    if (rc != CF_OK) return rc;
    if (!attachment_found) return CF_OK;
    rc = cf_attachment_blob(db, &attachment, out_blob);
    if (rc == CF_OK) {
        *out_attachment = attachment;
        *found = true;
        return CF_OK;
    }
    cf_attachment_dispose(&attachment);
    cf_blob_dispose(out_blob);
    return rc;
}

/* The reference trims plain text with Rust str::trim (Unicode White_Space). */
static bool str_trim_is_empty(cf_str text) {
    const unsigned char *bytes = (const unsigned char *)text.ptr;
    size_t i = 0;
    while (i < text.len) {
        uint32_t cp;
        size_t width = utf8_decode(bytes + i, text.len - i, &cp);
        if (!cp_is_whitespace(cp)) return false;
        i += width;
    }
    return true;
}

cf_err cf_message_plain_text_body(cf_db *db, const cf_message *message,
                                  const cf_richtext *rich_text,
                                  cf_str *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "no output pointer");
    memset(out, 0, sizeof *out);
    if (message == NULL) return cf_db_failf(CF_INVALID, "no message");
    bool html_found = false;
    cf_str html = {0};
    cf_err rc = cf_message_body_html(db, message, &html_found, &html);
    if (rc != CF_OK) return rc;
    if (html_found) {
        cf_str text = {0};
        cf_span span = {(const unsigned char *)html.ptr, html.len};
        rc = cf_richtext_to_plain_text(db, rich_text, span, &text);
        cf_str_dispose(&html);
        if (rc != CF_OK) return rc;
        if (!str_trim_is_empty(text)) {
            *out = text;
            return CF_OK;
        }
        cf_str_dispose(&text);
    }
    bool attachment_found = false;
    cf_attachment attachment = {0};
    cf_blob blob = {0};
    rc = cf_message_attachment(db, message, &attachment_found, &attachment,
                               &blob);
    if (rc != CF_OK) return rc;
    if (attachment_found) {
        cf_span span = {(const unsigned char *)blob.filename.ptr,
                        blob.filename.len};
        rc = span_copy(span, out);
        cf_blob_dispose(&blob);
        cf_attachment_dispose(&attachment);
    }
    return rc;
}

cf_err cf_message_content_type(cf_db *db, const cf_message *message,
                               const cf_richtext *rich_text,
                               cf_content_type *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "no output pointer");
    *out = CF_CONTENT_TYPE_TEXT;
    bool attachment_found = false;
    cf_attachment attachment = {0};
    cf_blob blob = {0};
    cf_err rc = cf_message_attachment(db, message, &attachment_found,
                                      &attachment, &blob);
    if (rc != CF_OK) return rc;
    if (attachment_found) {
        cf_blob_dispose(&blob);
        cf_attachment_dispose(&attachment);
        *out = CF_CONTENT_TYPE_ATTACHMENT;
        return CF_OK;
    }
    const cf_sound *sound = NULL;
    bool sound_found = false;
    rc = cf_message_sound(db, message, rich_text, &sound_found, &sound);
    if (rc != CF_OK) return rc;
    if (sound_found) *out = CF_CONTENT_TYPE_SOUND;
    return CF_OK;
}

cf_err cf_message_sound(cf_db *db, const cf_message *message,
                        const cf_richtext *rich_text, bool *found,
                        const cf_sound **out) {
    if (found == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "no output pointer");
    }
    *found = false;
    *out = NULL;
    cf_str plain = {0};
    cf_err rc = cf_message_plain_text_body(db, message, rich_text, &plain);
    if (rc != CF_OK) return rc;
    *found = cf_message_sound_in(plain, out);
    cf_str_dispose(&plain);
    return CF_OK;
}

cf_err cf_message_mentionees(cf_db *db, const cf_message *message,
                             const cf_richtext *rich_text,
                             cf_user_vector *out) {
    if (message == NULL) return cf_db_failf(CF_INVALID, "no message");
    bool html_found = false;
    cf_str html = {0};
    cf_err rc = cf_message_body_html(db, message, &html_found, &html);
    if (rc != CF_OK) return rc;
    cf_int64_vector ids = {0};
    if (html_found) {
        cf_span span = {(const unsigned char *)html.ptr, html.len};
        rc = cf_richtext_mentioned_user_ids(db, rich_text, span, &ids);
        cf_str_dispose(&html);
        if (rc != CF_OK) return rc;
    }
    rc = cf_message_mentionees_in_room(db, message->room_id, ids.items,
                                       ids.len, out);
    cf_int64_vector_dispose(&ids);
    return rc;
}

cf_err cf_message_reload(cf_db *db, cf_message *message) {
    if (message == NULL) return cf_db_failf(CF_INVALID, "no message");
    cf_message fresh = {0};
    cf_err rc = cf_message_find(db, message->id, &fresh);
    if (rc != CF_OK) return rc;
    cf_message_dispose(message);
    *message = fresh;
    return CF_OK;
}
