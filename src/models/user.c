/* src/models/user.c — D01 model family "user".
 *
 * Source: tmp/rust-ref/crates/db/src/models/user.rs (54 inventoried
 * functions; hashes in contracts/reference-files.json).  Tables: users
 * (contracts/schema.sql), plus dependent cleanup across memberships,
 * sessions, push_subscriptions, searches, bans and messages.
 *
 * Translation notes:
 *  - Reads take cf_db * (reader/writer connection); mutations take cf_tx *
 *    and reach the transaction's connection through cf_tx_db(tx).  Rust
 *    `tx.now()` maps to cf_now_us(NULL), the process clock core/testclock.h
 *    can fix in tests (same convention as the sibling model files).
 *  - Datetimes are stored as the reference UTC SQL text
 *    (cf_db_time_to_text: ".ffffff" only when non-zero) and kept in memory as
 *    int64 UTC microseconds.
 *  - User::create's after-commit grant to every open room runs in the same C
 *    transaction (02-data-auth.md D02 permits translating required local
 *    after-commit row changes into the transaction; final state unchanged).
 *    The reference batches a runtime-built multi-row INSERT; C uses one fixed
 *    single-row INSERT ... ON CONFLICT DO NOTHING per open room, in rooms.id
 *    order, because no SQL is assembled at runtime.
 *  - User::where_ids is the one reference query whose IN list is built at
 *    runtime.  C keeps a fixed statement by binding the JSON array to
 *    json_each(?); result order (users rowid = id order) and set semantics are
 *    the reference ones, including an empty list (SQLite tolerates IN ()).
 *  - User::update is a single dynamically assembled UPDATE in Rust; C applies
 *    the same per-column statements in the source set order, all inside the
 *    caller transaction, so the final row and the updated_at touch are
 *    identical.
 *  - PasswordDigest and bcrypt verification belong to A01 (src/auth).
 *    cf_password_verify below is the proposed boundary for the verify half
 *    (see docs/devel/evidence/D01-model-user.md); hashing never appears here:
 *    password_digest arrives already hashed, as the frozen header documents.
 *  - User::initials needs Rust's Unicode char::is_alphanumeric for the
 *    previous-character boundary.  There is no Unicode table in this project:
 *    ASCII plus Latin-1 is classified exactly and the common symbol/space
 *    blocks are excluded, with the rest treated as word characters (reported
 *    as an approximation in the evidence).
 */
#include "models/user.h"

#include "models/ban.h"
#include "models/message.h"
#include "models/membership.h"
#include "models/session.h"
#include "models/webhook.h"
#include "db/db_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Proposed A01 boundary (docs/devel/evidence/D01-model-user.md): bcrypt
 * verification of one password against one stored digest.  Never called with
 * an empty/absent digest; a malformed digest verifies false, as
 * rails_compat::password::verify does.  The definition lands with A01 in
 * src/auth and this declaration moves to its header then. */
bool cf_password_verify(cf_str password, cf_str digest);

/* `User::authenticated`'s constant dummy digest (user.rs). */
#define CF_USER_DUMMY_DIGEST \
    "$2a$12$FiKmSp4UhLvSB4Sd/ZUjQunyKP6.NjDRHdr5LnKUVk.BUn4Mq12WS"

/* Reference room type names (rooms.type). */
#define CF_USER_ROOMS_OPEN "Rooms::Open"
#define CF_USER_ROOMS_DIRECT "Rooms::Direct"

/* Room::MEMBERSHIP_INSERT_BATCH (room.rs, pub(crate)): the reference chunk
 * size for the runtime-built multi-row INSERT; unused by the fixed
 * single-row C statement, recorded here for the evidence. */
#define CF_USER_MEMBERSHIP_INSERT_BATCH 1000

/* --- fixed statements ----------------------------------------------------- */

enum cf_user_stmt_id {
    CF_USER_STMT_FIND_BY_ID = 0,
    CF_USER_STMT_FIND_ACTIVE,
    CF_USER_STMT_FIND_BY_EMAIL_ADDRESS,
    CF_USER_STMT_ALL,
    CF_USER_STMT_COUNT_USERS,
    CF_USER_STMT_WHERE_IDS,
    CF_USER_STMT_ACTIVE_ORDERED,
    CF_USER_STMT_ACTIVE,
    CF_USER_STMT_ACTIVE_FILTERED_ORDERED,
    CF_USER_STMT_ACTIVE_ORDERED_WITHOUT_BOTS,
    CF_USER_STMT_ACTIVE_BOTS_ORDERED,
    CF_USER_STMT_FIND_ACTIVE_BOT,
    CF_USER_STMT_FIND_ACTIVE_BY_EMAIL_ADDRESS,
    CF_USER_STMT_AUTHENTICATE_BOT,
    CF_USER_STMT_INSERT,
    CF_USER_STMT_UPDATE_NAME,
    CF_USER_STMT_UPDATE_EMAIL_ADDRESS,
    CF_USER_STMT_UPDATE_PASSWORD_DIGEST,
    CF_USER_STMT_UPDATE_ROLE,
    CF_USER_STMT_UPDATE_STATUS,
    CF_USER_STMT_UPDATE_BIO,
    CF_USER_STMT_UPDATE_BOT_TOKEN,
    CF_USER_STMT_DEACTIVATE_DELETE_MEMBERSHIPS,
    CF_USER_STMT_DEACTIVATE_DELETE_PUSH_SUBSCRIPTIONS,
    CF_USER_STMT_DEACTIVATE_DELETE_SEARCHES,
    CF_USER_STMT_DEACTIVATE_DELETE_SESSIONS,
    CF_USER_STMT_BAN_SESSION_IPS,
    CF_USER_STMT_BAN_DELETE_SESSIONS,
    CF_USER_STMT_UNBAN_DELETE_BANS,
    CF_USER_STMT_OPEN_ROOM_IDS,
    CF_USER_STMT_GRANT_MEMBERSHIP,
    CF_USER_STMT_COUNT
};

/* The reference user_columns!() expansion, byte for byte. */
#define CF_USER_COLUMNS                                                       \
    "\"users\".\"id\", \"users\".\"name\", \"users\".\"email_address\", "      \
    "\"users\".\"password_digest\", \"users\".\"role\", "                      \
    "\"users\".\"status\", \"users\".\"bio\", \"users\".\"bot_token\", "       \
    "\"users\".\"created_at\", \"users\".\"updated_at\""

static const cf_stmt_def user_stmt_defs[CF_USER_STMT_COUNT] = {
    [CF_USER_STMT_FIND_BY_ID] = {
        "SELECT " CF_USER_COLUMNS
        " FROM \"users\" WHERE \"users\".\"id\" = ? LIMIT 1"},
    [CF_USER_STMT_FIND_ACTIVE] = {
        "SELECT " CF_USER_COLUMNS
        " FROM \"users\" WHERE \"users\".\"status\" = 0 "
        "AND \"users\".\"id\" = ? LIMIT 1"},
    [CF_USER_STMT_FIND_BY_EMAIL_ADDRESS] = {
        "SELECT " CF_USER_COLUMNS
        " FROM \"users\" WHERE \"users\".\"email_address\" = ? LIMIT 1"},
    [CF_USER_STMT_ALL] = {"SELECT " CF_USER_COLUMNS " FROM \"users\""},
    [CF_USER_STMT_COUNT_USERS] = {"SELECT COUNT(*) FROM \"users\""},
    /* Fixed-statement form of the runtime-built IN list (see the file
     * comment): the bound text is the JSON array of ids. */
    [CF_USER_STMT_WHERE_IDS] = {
        "SELECT " CF_USER_COLUMNS
        " FROM \"users\" WHERE \"users\".\"id\" IN "
        "(SELECT value FROM json_each(?))"},
    [CF_USER_STMT_ACTIVE_ORDERED] = {
        "SELECT " CF_USER_COLUMNS
        " FROM \"users\" WHERE \"users\".\"status\" = 0 "
        "ORDER BY LOWER(name)"},
    [CF_USER_STMT_ACTIVE] = {
        "SELECT " CF_USER_COLUMNS
        " FROM \"users\" WHERE \"users\".\"status\" = 0"},
    [CF_USER_STMT_ACTIVE_FILTERED_ORDERED] = {
        "SELECT " CF_USER_COLUMNS
        " FROM \"users\" WHERE \"users\".\"status\" = 0 AND (name like ?) "
        "ORDER BY LOWER(name)"},
    [CF_USER_STMT_ACTIVE_ORDERED_WITHOUT_BOTS] = {
        "SELECT " CF_USER_COLUMNS
        " FROM \"users\" WHERE \"users\".\"status\" = 0 "
        "AND \"users\".\"role\" != 2 ORDER BY LOWER(name)"},
    [CF_USER_STMT_ACTIVE_BOTS_ORDERED] = {
        "SELECT " CF_USER_COLUMNS
        " FROM \"users\" WHERE \"users\".\"status\" = 0 "
        "AND \"users\".\"role\" = 2 ORDER BY LOWER(name)"},
    [CF_USER_STMT_FIND_ACTIVE_BOT] = {
        "SELECT " CF_USER_COLUMNS
        " FROM \"users\" WHERE \"users\".\"status\" = 0 "
        "AND \"users\".\"role\" = 2 AND \"users\".\"id\" = ? LIMIT 1"},
    [CF_USER_STMT_FIND_ACTIVE_BY_EMAIL_ADDRESS] = {
        "SELECT " CF_USER_COLUMNS
        " FROM \"users\" WHERE \"users\".\"status\" = 0 "
        "AND \"users\".\"email_address\" = ? LIMIT 1"},
    [CF_USER_STMT_AUTHENTICATE_BOT] = {
        "SELECT " CF_USER_COLUMNS
        " FROM \"users\" WHERE \"users\".\"status\" = 0 "
        "AND \"users\".\"role\" = 2 AND \"users\".\"id\" = ? "
        "AND \"users\".\"bot_token\" = ? LIMIT 1"},
    [CF_USER_STMT_INSERT] = {
        "INSERT INTO \"users\" (\"bio\", \"bot_token\", \"created_at\", "
        "\"email_address\", \"name\", \"password_digest\", \"role\", "
        "\"status\", \"updated_at\") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "RETURNING \"id\""},
    [CF_USER_STMT_UPDATE_NAME] = {
        "UPDATE \"users\" SET \"name\" = ?, \"updated_at\" = ? "
        "WHERE \"users\".\"id\" = ?"},
    [CF_USER_STMT_UPDATE_EMAIL_ADDRESS] = {
        "UPDATE \"users\" SET \"email_address\" = ?, \"updated_at\" = ? "
        "WHERE \"users\".\"id\" = ?"},
    [CF_USER_STMT_UPDATE_PASSWORD_DIGEST] = {
        "UPDATE \"users\" SET \"password_digest\" = ?, \"updated_at\" = ? "
        "WHERE \"users\".\"id\" = ?"},
    [CF_USER_STMT_UPDATE_ROLE] = {
        "UPDATE \"users\" SET \"role\" = ?, \"updated_at\" = ? "
        "WHERE \"users\".\"id\" = ?"},
    [CF_USER_STMT_UPDATE_STATUS] = {
        "UPDATE \"users\" SET \"status\" = ?, \"updated_at\" = ? "
        "WHERE \"users\".\"id\" = ?"},
    [CF_USER_STMT_UPDATE_BIO] = {
        "UPDATE \"users\" SET \"bio\" = ?, \"updated_at\" = ? "
        "WHERE \"users\".\"id\" = ?"},
    [CF_USER_STMT_UPDATE_BOT_TOKEN] = {
        "UPDATE \"users\" SET \"bot_token\" = ?, \"updated_at\" = ? "
        "WHERE \"users\".\"id\" = ?"},
    [CF_USER_STMT_DEACTIVATE_DELETE_MEMBERSHIPS] = {
        "DELETE FROM \"memberships\" WHERE (\"memberships\".\"id\") IN "
        "(SELECT \"memberships\".\"id\" FROM \"memberships\" INNER JOIN "
        "\"rooms\" AS \"room\" ON \"room\".\"id\" = \"memberships\".\"room_id\" "
        "WHERE \"memberships\".\"user_id\" = ? AND \"room\".\"type\" != ?)"},
    [CF_USER_STMT_DEACTIVATE_DELETE_PUSH_SUBSCRIPTIONS] = {
        "DELETE FROM \"push_subscriptions\" "
        "WHERE \"push_subscriptions\".\"user_id\" = ?"},
    [CF_USER_STMT_DEACTIVATE_DELETE_SEARCHES] = {
        "DELETE FROM \"searches\" WHERE \"searches\".\"user_id\" = ?"},
    [CF_USER_STMT_DEACTIVATE_DELETE_SESSIONS] = {
        "DELETE FROM \"sessions\" WHERE \"sessions\".\"user_id\" = ?"},
    [CF_USER_STMT_BAN_SESSION_IPS] = {
        "SELECT \"sessions\".\"ip_address\" FROM \"sessions\" "
        "WHERE \"sessions\".\"user_id\" = ?"},
    [CF_USER_STMT_BAN_DELETE_SESSIONS] = {
        "DELETE FROM \"sessions\" WHERE \"sessions\".\"user_id\" = ?"},
    [CF_USER_STMT_UNBAN_DELETE_BANS] = {
        "DELETE FROM \"bans\" WHERE \"bans\".\"user_id\" = ?"},
    [CF_USER_STMT_OPEN_ROOM_IDS] = {
        "SELECT \"rooms\".\"id\" FROM \"rooms\" WHERE \"rooms\".\"type\" = ?"},
    [CF_USER_STMT_GRANT_MEMBERSHIP] = {
        "INSERT INTO \"memberships\" "
        "(\"created_at\",\"room_id\",\"updated_at\",\"user_id\") "
        "VALUES (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW'), ?, "
        "STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW'), ?) ON CONFLICT  DO NOTHING "
        "RETURNING \"id\""},
};

static const cf_stmt_set user_stmts = {user_stmt_defs, CF_USER_STMT_COUNT};

/* --- small helpers -------------------------------------------------------- */

static cf_err user_step_failed(cf_db *db, int rc, const char *op) {
    if (rc == SQLITE_ROW) {
        return cf_db_failf(CF_INTERNAL, "%s: unexpected row", op);
    }
    return cf_db_failf(cf_db_err(rc), "%s failed: %s", op,
                       sqlite3_errmsg(cf_db_handle(db)));
}

static cf_span user_span_of(cf_str text) {
    return (cf_span){(const unsigned char *)text.ptr, text.len};
}

static cf_err user_strdup_span(cf_span span, cf_str *out) {
    out->ptr = NULL;
    out->len = 0;
    if (span.len != 0 && span.ptr == NULL) return CF_INVALID;
    if (span.len == SIZE_MAX) return CF_LIMIT;
    char *copy = malloc(span.len + 1);
    if (copy == NULL) return CF_NOMEM;
    if (span.len != 0) memcpy(copy, span.ptr, span.len);
    copy[span.len] = '\0';
    out->ptr = copy;
    out->len = span.len;
    return CF_OK;
}

/* Owned copy of a borrowed input string (no NULL sharing). */
static cf_err user_copy_input_text(cf_str in, cf_str *out) {
    return user_strdup_span(user_span_of(in), out);
}

/* Copy a text column into owned memory before reset.  A SQL NULL column
 * arrives here only through user_copy_optional_text. */
static cf_err user_copy_text(sqlite3_stmt *stmt, int column, cf_str *out) {
    return user_strdup_span(cf_stmt_column_text(stmt, column), out);
}

static cf_err user_copy_optional_text(sqlite3_stmt *stmt, int column,
                                      cf_optional_str *out) {
    out->present = false;
    out->value.ptr = NULL;
    out->value.len = 0;
    if (cf_stmt_column_is_null(stmt, column)) return CF_OK;
    cf_err rc = user_copy_text(stmt, column, &out->value);
    if (rc != CF_OK) return rc;
    out->present = true;
    return CF_OK;
}

/* Owned copy of a borrowed optional input (values written into a record). */
static cf_err user_copy_optional_input(cf_optional_str in,
                                       cf_optional_str *out) {
    out->present = false;
    out->value.ptr = NULL;
    out->value.len = 0;
    if (!in.present) return CF_OK;
    cf_err rc = user_strdup_span(user_span_of(in.value), &out->value);
    if (rc != CF_OK) return rc;
    out->present = true;
    return CF_OK;
}

/* "%<query>%" of User::active_filtered_by_ordered. */
static cf_err user_like_pattern(cf_str query, cf_str *out) {
    out->ptr = NULL;
    out->len = 0;
    if (query.len != 0 && query.ptr == NULL) return CF_INVALID;
    if (query.len > SIZE_MAX - 3) return CF_LIMIT;
    size_t len = query.len + 2;
    char *pattern = malloc(len + 1);
    if (pattern == NULL) return CF_NOMEM;
    pattern[0] = '%';
    if (query.len != 0) memcpy(pattern + 1, query.ptr, query.len);
    pattern[len - 1] = '%';
    pattern[len] = '\0';
    out->ptr = pattern;
    out->len = len;
    return CF_OK;
}

static cf_err user_i64_text(int64_t value, cf_str *out) {
    char buf[24];
    int len = snprintf(buf, sizeof buf, "%lld", (long long)value);
    if (len < 0 || (size_t)len >= sizeof buf) {
        return cf_db_failf(CF_INTERNAL, "cannot format integer");
    }
    return user_strdup_span((cf_span){(const unsigned char *)buf,
                                      (size_t)len},
                            out);
}

/* join(parts, sep): owned text; an empty result is an allocated "". */
static cf_err user_concat(cf_str first, cf_str sep, cf_str second,
                          cf_str *out) {
    out->ptr = NULL;
    out->len = 0;
    if ((first.len != 0 && first.ptr == NULL) ||
        (sep.len != 0 && sep.ptr == NULL) ||
        (second.len != 0 && second.ptr == NULL)) {
        return CF_INVALID;
    }
    if (first.len > SIZE_MAX - sep.len ||
        first.len + sep.len > SIZE_MAX - second.len ||
        first.len + sep.len + second.len == SIZE_MAX) {
        return CF_LIMIT;
    }
    size_t len = first.len + sep.len + second.len;
    char *text = malloc(len + 1);
    if (text == NULL) return CF_NOMEM;
    size_t at = 0;
    if (first.len != 0) {
        memcpy(text, first.ptr, first.len);
        at += first.len;
    }
    if (sep.len != 0) {
        memcpy(text + at, sep.ptr, sep.len);
        at += sep.len;
    }
    if (second.len != 0) memcpy(text + at, second.ptr, second.len);
    text[len] = '\0';
    out->ptr = text;
    out->len = len;
    return CF_OK;
}

/* --- Unicode helpers ------------------------------------------------------ */

/* Decode one UTF-8 code point; invalid bytes surface as themselves (Rust
 * &str cannot hold them, but a C cf_str from a database could).  Returns the
 * bytes consumed, always >= 1 while remaining > 0. */
static size_t user_utf8_decode(const unsigned char *p, size_t remaining,
                               uint32_t *cp) {
    if (remaining == 0) {
        *cp = 0;
        return 0;
    }
    unsigned char b0 = p[0];
    if (b0 < 0x80) {
        *cp = b0;
        return 1;
    }
    size_t need;
    uint32_t value;
    if ((b0 & 0xE0) == 0xC0) {
        need = 2;
        value = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
        need = 3;
        value = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
        need = 4;
        value = b0 & 0x07;
    } else {
        *cp = b0;
        return 1;
    }
    if (remaining < need) {
        *cp = b0;
        return 1;
    }
    for (size_t i = 1; i < need; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *cp = b0;
            return 1;
        }
        value = (value << 6) | (p[i] & 0x3F);
    }
    *cp = value;
    return need;
}

/* Unicode White_Space (the 25 code points Rust's char::is_whitespace uses). */
static bool user_cp_is_space(uint32_t cp) {
    return (cp >= 0x0009 && cp <= 0x000D) || cp == 0x0020 || cp == 0x0085 ||
           cp == 0x00A0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
           cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

/* Rust `str::trim().is_empty()`: no non-White_Space code point. */
static bool user_str_is_blank(cf_str text) {
    const unsigned char *p = (const unsigned char *)text.ptr;
    size_t i = 0;
    while (i < text.len) {
        uint32_t cp;
        size_t used = user_utf8_decode(p + i, text.len - i, &cp);
        if (!user_cp_is_space(cp)) return false;
        i += used;
    }
    return true;
}

/* Approximation of Rust char::is_alphanumeric (see the file comment):
 * exact through Latin-1, word characters elsewhere except the common
 * non-word blocks.  Used only for User::initials' previous-character
 * boundary. */
static bool user_cp_is_word(uint32_t cp) {
    if (cp < 0x80) {
        return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') ||
               (cp >= 'a' && cp <= 'z') || cp == '_';
    }
    /* Latin-1: letters (C0..FF except the two symbols) and the few number
     * forms (ordinals, superscripts, fractions, U+00B5); all other Latin-1
     * code points are controls, punctuation or symbols. */
    if (cp <= 0xFF) {
        if (cp >= 0xC0 && cp != 0xD7 && cp != 0xF7) return true;
        return cp == 0xAA || cp == 0xB5 || cp == 0xB2 || cp == 0xB3 ||
               cp == 0xB9 || cp == 0xBA || cp == 0xBC || cp == 0xBD ||
               cp == 0xBE;
    }
    /* Common non-word blocks: punctuation, symbols, spaces, emoji. */
    if ((cp >= 0x2000 && cp <= 0x206F) || (cp >= 0x2070 && cp <= 0x209F) ||
        (cp >= 0x20A0 && cp <= 0x20CF) || (cp >= 0x2100 && cp <= 0x214F) ||
        (cp >= 0x2190 && cp <= 0x2BFF) || (cp >= 0x3000 && cp <= 0x303F) ||
        (cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0xFF00 && cp <= 0xFFEF) ||
        user_cp_is_space(cp)) {
        return false;
    }
    return true;
}

/* --- row mapping and vectors ---------------------------------------------- */

/* User::from_row: fields in user_columns!() order.  On failure *out is
 * disposed back to empty. */
static cf_err user_from_row(sqlite3_stmt *stmt, cf_user *out) {
    memset(out, 0, sizeof *out);
    out->id = cf_stmt_column_i64(stmt, 0);
    cf_err rc = user_copy_text(stmt, 1, &out->name);
    if (rc == CF_OK) rc = user_copy_optional_text(stmt, 2, &out->email_address);
    if (rc == CF_OK) {
        rc = user_copy_optional_text(stmt, 3, &out->password_digest);
    }
    if (rc == CF_OK) {
        int64_t role = cf_stmt_column_i64(stmt, 4);
        if (role < CF_ROLE_MEMBER || role > CF_ROLE_BOT) {
            rc = cf_db_failf(CF_INVALID, "users.role out of range: %lld",
                             (long long)role);
        } else {
            out->role = (cf_role)role;
        }
    }
    if (rc == CF_OK) {
        int64_t status = cf_stmt_column_i64(stmt, 5);
        if (status < CF_STATUS_ACTIVE || status > CF_STATUS_BANNED) {
            rc = cf_db_failf(CF_INVALID, "users.status out of range: %lld",
                             (long long)status);
        } else {
            out->status = (cf_status)status;
        }
    }
    if (rc == CF_OK) rc = user_copy_optional_text(stmt, 6, &out->bio);
    if (rc == CF_OK) rc = user_copy_optional_text(stmt, 7, &out->bot_token);
    if (rc == CF_OK) {
        rc = cf_db_time_from_text(cf_stmt_column_text(stmt, 8),
                                  &out->created_at);
    }
    if (rc == CF_OK) {
        rc = cf_db_time_from_text(cf_stmt_column_text(stmt, 9),
                                  &out->updated_at);
    }
    if (rc != CF_OK) cf_user_dispose(out);
    return rc;
}

static cf_err user_vector_push(cf_user_vector *vector, cf_user *item) {
    if (vector->len == vector->cap) {
        size_t cap = vector->cap != 0 ? vector->cap * 2 : 8;
        if (cap < vector->cap || cap > SIZE_MAX / sizeof *vector->items) {
            return CF_LIMIT;
        }
        cf_user *items = realloc(vector->items, cap * sizeof *items);
        if (items == NULL) return CF_NOMEM;
        vector->items = items;
        vector->cap = cap;
    }
    vector->items[vector->len++] = *item; /* ownership moves to the vector */
    memset(item, 0, sizeof *item);
    return CF_OK;
}

/* One lookup by zero or one bound parameter, mapped to a cf_user.  *out is
 * left empty unless a row was mapped. */
static cf_err user_lookup(cf_db *db, size_t stmt_id, bool bind_value,
                          bool value_is_text, int64_t i64_value,
                          cf_span text_value, const char *op, bool *found,
                          cf_user *out) {
    if (found == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "%s: no output", op);
    }
    *found = false;
    memset(out, 0, sizeof *out);

    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &user_stmts, stmt_id, &stmt);
    if (rc == CF_OK && bind_value) {
        rc = value_is_text ? cf_stmt_bind_text(stmt, 1, text_value)
                           : cf_stmt_bind_i64(stmt, 1, i64_value);
    }
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            rc = user_from_row(stmt, out);
            if (rc == CF_OK) *found = true;
        } else if (step != SQLITE_DONE) {
            rc = user_step_failed(db, step, op);
        }
    }
    cf_db_stmt_done(stmt);
    return rc;
}

/* The authenticate_bot lookup: id text and token text bindings. */
static cf_err user_lookup_bot(cf_db *db, cf_span id_text, cf_span token_text,
                              bool *found, cf_user *out) {
    if (found == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "authenticate bot: no output");
    }
    *found = false;
    memset(out, 0, sizeof *out);

    sqlite3_stmt *stmt = NULL;
    cf_err rc =
        cf_db_stmt(db, &user_stmts, CF_USER_STMT_AUTHENTICATE_BOT, &stmt);
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 1, id_text);
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 2, token_text);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            rc = user_from_row(stmt, out);
            if (rc == CF_OK) *found = true;
        } else if (step != SQLITE_DONE) {
            rc = user_step_failed(db, step, "authenticate bot");
        }
    }
    cf_db_stmt_done(stmt);
    return rc;
}

/* Vector query with zero or one binding, as above. */
static cf_err user_collect(cf_db *db, size_t stmt_id, bool bind_value,
                           bool value_is_text, int64_t i64_value,
                           cf_span text_value, const char *op,
                           cf_user_vector *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "%s: no output", op);
    memset(out, 0, sizeof *out);

    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &user_stmts, stmt_id, &stmt);
    if (rc == CF_OK && bind_value) {
        rc = value_is_text ? cf_stmt_bind_text(stmt, 1, text_value)
                           : cf_stmt_bind_i64(stmt, 1, i64_value);
    }
    while (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            rc = user_step_failed(db, step, op);
            break;
        }
        cf_user row;
        rc = user_from_row(stmt, &row);
        if (rc != CF_OK) break;
        rc = user_vector_push(out, &row);
        if (rc != CF_OK) cf_user_dispose(&row);
    }
    cf_db_stmt_done(stmt);
    if (rc != CF_OK) cf_user_vector_dispose(out);
    return rc;
}

/* --- record and vector disposal ------------------------------------------- */

void cf_user_dispose(cf_user *user) {
    if (user == NULL) return;
    cf_str_dispose(&user->name);
    cf_optional_str_dispose(&user->email_address);
    cf_optional_str_dispose(&user->password_digest);
    cf_optional_str_dispose(&user->bio);
    cf_optional_str_dispose(&user->bot_token);
    memset(user, 0, sizeof *user);
}

void cf_user_vector_dispose(cf_user_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len; i++) {
        cf_user_dispose(&vector->items[i]);
    }
    free(vector->items);
    vector->items = NULL;
    vector->len = 0;
    vector->cap = 0;
}

/* --- Role / Status helpers (user.rs) -------------------------------------- */

const char *cf_role_name(cf_role role) {
    switch (role) {
    case CF_ROLE_MEMBER:
        return "member";
    case CF_ROLE_ADMINISTRATOR:
        return "administrator";
    case CF_ROLE_BOT:
        return "bot";
    }
    return NULL;
}

bool cf_role_from_name(cf_str name, cf_role *out) {
    if (name.len != 0 && name.ptr == NULL) return false;
    if (out == NULL) return false;
    if (name.len == 6 && memcmp(name.ptr, "member", 6) == 0) {
        *out = CF_ROLE_MEMBER;
        return true;
    }
    if (name.len == 13 && memcmp(name.ptr, "administrator", 13) == 0) {
        *out = CF_ROLE_ADMINISTRATOR;
        return true;
    }
    if (name.len == 3 && memcmp(name.ptr, "bot", 3) == 0) {
        *out = CF_ROLE_BOT;
        return true;
    }
    return false;
}

const char *cf_status_name(cf_status status) {
    switch (status) {
    case CF_STATUS_ACTIVE:
        return "active";
    case CF_STATUS_DEACTIVATED:
        return "deactivated";
    case CF_STATUS_BANNED:
        return "banned";
    }
    return NULL;
}

bool cf_status_from_name(cf_str name, cf_status *out) {
    if (name.len != 0 && name.ptr == NULL) return false;
    if (out == NULL) return false;
    if (name.len == 6 && memcmp(name.ptr, "active", 6) == 0) {
        *out = CF_STATUS_ACTIVE;
        return true;
    }
    if (name.len == 11 && memcmp(name.ptr, "deactivated", 11) == 0) {
        *out = CF_STATUS_DEACTIVATED;
        return true;
    }
    if (name.len == 6 && memcmp(name.ptr, "banned", 6) == 0) {
        *out = CF_STATUS_BANNED;
        return true;
    }
    return false;
}

/* --- finders and scopes --------------------------------------------------- */

cf_err cf_user_find_by_id(cf_db *db, int64_t id, bool *found, cf_user *out) {
    return user_lookup(db, CF_USER_STMT_FIND_BY_ID, true, false, id,
                       (cf_span){NULL, 0}, "find user", found, out);
}

cf_err cf_user_find(cf_db *db, int64_t id, cf_user *out) {
    bool found = false;
    cf_err rc = cf_user_find_by_id(db, id, &found, out);
    if (rc != CF_OK) return rc;
    if (!found) {
        /* Error::RecordNotFound display: "Couldn't find User". */
        return cf_db_failf(CF_NOT_FOUND, "Couldn't find User");
    }
    return CF_OK;
}

cf_err cf_user_find_active(cf_db *db, int64_t id, cf_user *out) {
    bool found = false;
    cf_err rc = user_lookup(db, CF_USER_STMT_FIND_ACTIVE, true, false, id,
                            (cf_span){NULL, 0}, "find active user", &found,
                            out);
    if (rc != CF_OK) return rc;
    if (!found) {
        /* Error::RecordNotFound display: "Couldn't find User". */
        return cf_db_failf(CF_NOT_FOUND, "Couldn't find User");
    }
    return CF_OK;
}

cf_err cf_user_find_by_email_address(cf_db *db, cf_str email_address,
                                     bool *found, cf_user *out) {
    return user_lookup(db, CF_USER_STMT_FIND_BY_EMAIL_ADDRESS, true, true, 0,
                       user_span_of(email_address), "find user by email",
                       found, out);
}

cf_err cf_user_all(cf_db *db, cf_user_vector *out) {
    return user_collect(db, CF_USER_STMT_ALL, false, false, 0,
                        (cf_span){NULL, 0}, "all users", out);
}

cf_err cf_user_count(cf_db *db, int64_t *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "count users: no output");
    }
    *out = 0;

    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &user_stmts, CF_USER_STMT_COUNT_USERS, &stmt);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            *out = cf_stmt_column_i64(stmt, 0);
        } else if (step == SQLITE_DONE) {
            rc = cf_db_failf(CF_INTERNAL, "count returned no row");
        } else {
            rc = user_step_failed(db, step, "count users");
        }
    }
    cf_db_stmt_done(stmt);
    return rc;
}


cf_err cf_user_where_ids(cf_db *db, const int64_t *ids, size_t ids_len,
                         cf_user_vector *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "where_ids: no output");
    }
    memset(out, 0, sizeof *out);

    cf_str json;
    cf_err rc = cf_db_ids_json(ids, ids_len, &json);
    if (rc != CF_OK) return rc;

    sqlite3_stmt *stmt = NULL;
    rc = cf_db_stmt(db, &user_stmts, CF_USER_STMT_WHERE_IDS, &stmt);
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 1, user_span_of(json));
    while (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            rc = user_step_failed(db, step, "users where ids");
            break;
        }
        cf_user row;
        rc = user_from_row(stmt, &row);
        if (rc != CF_OK) break;
        rc = user_vector_push(out, &row);
        if (rc != CF_OK) cf_user_dispose(&row);
    }
    cf_db_stmt_done(stmt);
    free(json.ptr);
    if (rc != CF_OK) cf_user_vector_dispose(out);
    return rc;
}

cf_err cf_user_active_ordered(cf_db *db, cf_user_vector *out) {
    return user_collect(db, CF_USER_STMT_ACTIVE_ORDERED, false, false, 0,
                        (cf_span){NULL, 0}, "active users ordered", out);
}

cf_err cf_user_active(cf_db *db, cf_user_vector *out) {
    return user_collect(db, CF_USER_STMT_ACTIVE, false, false, 0,
                        (cf_span){NULL, 0}, "active users", out);
}

cf_err cf_user_active_filtered_by_ordered(cf_db *db, cf_str query,
                                          cf_user_vector *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "active filtered: no output");
    }
    memset(out, 0, sizeof *out);

    cf_str pattern;
    cf_err rc = user_like_pattern(query, &pattern);
    if (rc != CF_OK) return rc;

    sqlite3_stmt *stmt = NULL;
    rc = cf_db_stmt(db, &user_stmts, CF_USER_STMT_ACTIVE_FILTERED_ORDERED,
                    &stmt);
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 1, user_span_of(pattern));
    while (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            rc = user_step_failed(db, step, "active users filtered");
            break;
        }
        cf_user row;
        rc = user_from_row(stmt, &row);
        if (rc != CF_OK) break;
        rc = user_vector_push(out, &row);
        if (rc != CF_OK) cf_user_dispose(&row);
    }
    cf_db_stmt_done(stmt);
    free(pattern.ptr);
    if (rc != CF_OK) cf_user_vector_dispose(out);
    return rc;
}

cf_err cf_user_active_ordered_without_bots(cf_db *db, cf_user_vector *out) {
    return user_collect(db, CF_USER_STMT_ACTIVE_ORDERED_WITHOUT_BOTS, false,
                        false, 0, (cf_span){NULL, 0}, "active users without bots",
                        out);
}

cf_err cf_user_active_bots_ordered(cf_db *db, cf_user_vector *out) {
    return user_collect(db, CF_USER_STMT_ACTIVE_BOTS_ORDERED, false, false, 0,
                        (cf_span){NULL, 0}, "active bots ordered", out);
}

cf_err cf_user_find_active_bot(cf_db *db, int64_t id, cf_user *out) {
    bool found = false;
    cf_err rc = user_lookup(db, CF_USER_STMT_FIND_ACTIVE_BOT, true, false, id,
                            (cf_span){NULL, 0}, "find active bot", &found,
                            out);
    if (rc != CF_OK) return rc;
    if (!found) {
        /* Error::RecordNotFound display: "Couldn't find User". */
        return cf_db_failf(CF_NOT_FOUND, "Couldn't find User");
    }
    return CF_OK;
}

cf_err cf_user_find_active_by_email_address(cf_db *db, cf_str email_address,
                                            bool *found, cf_user *out) {
    return user_lookup(db, CF_USER_STMT_FIND_ACTIVE_BY_EMAIL_ADDRESS, true, true,
                       0, user_span_of(email_address),
                       "find active user by email", found, out);
}

/* --- authentication ------------------------------------------------------- */

/* User::authenticate: an absent or empty digest never verifies. */
static bool user_authenticate_record(const cf_user *user, cf_str password) {
    if (!user->password_digest.present ||
        user->password_digest.value.len == 0) {
        return false;
    }
    return cf_password_verify(password, user->password_digest.value);
}

cf_err cf_user_authenticate(const cf_user *user, cf_str password, bool *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "authenticate: no output");
    }
    *out = false;
    if (password.len != 0 && password.ptr == NULL) return CF_INVALID;
    if (user == NULL) return CF_INVALID;
    *out = user_authenticate_record(user, password);
    return CF_OK;
}

cf_err cf_user_authenticated(const cf_user *candidate, cf_str password,
                             bool *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "authenticated: no output");
    }
    *out = false;
    if (password.len != 0 && password.ptr == NULL) return CF_INVALID;
    if (password.len == 0) return CF_OK; /* blank password: no lookup, no verify */
    if (candidate != NULL) {
        *out = user_authenticate_record(candidate, password);
        return CF_OK;
    }
    /* A missing account still runs the dummy verification so it takes as long
     * as a wrong password; the result is discarded. */
    (void)cf_password_verify(password, CF_STR_LIT(CF_USER_DUMMY_DIGEST));
    return CF_OK;
}

/* User.authenticate_bot: "#{id}-#{bot_token}".  Rust's str::split('-') keeps
 * trailing empty fields, so a key with a separator but no token binds an
 * empty token; a key with no '-' at all finds nothing. */
cf_err cf_user_authenticate_bot(cf_db *db, cf_str bot_key, bool *found,
                                cf_user *out) {
    if (found == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "authenticate bot: no output");
    }
    *found = false;
    memset(out, 0, sizeof *out);
    if (bot_key.len != 0 && bot_key.ptr == NULL) return CF_INVALID;
    if (bot_key.len == 0) return CF_OK; /* no '-' at all */

    const char *dash = memchr(bot_key.ptr, '-', bot_key.len);
    if (dash == NULL) return CF_OK;
    size_t id_len = (size_t)(dash - bot_key.ptr);
    cf_span id_text = {(const unsigned char *)bot_key.ptr, id_len};
    const char *rest = dash + 1;
    size_t rest_len = bot_key.len - id_len - 1;
    const char *token_end = memchr(rest, '-', rest_len);
    size_t token_len = token_end != NULL ? (size_t)(token_end - rest) : rest_len;
    cf_span token_text = {(const unsigned char *)rest, token_len};
    return user_lookup_bot(db, id_text, token_text, found, out);
}

/* --- creating ------------------------------------------------------------- */

/* `after_create_commit :grant_membership_to_open_rooms`, run in the caller
 * transaction (spec 02 D02).  One fixed single-row INSERT per open room,
 * rooms.id order (the reference's chunked multi-row INSERT is runtime-built
 * SQL, so C keeps a fixed statement); the row's created_at/updated_at are
 * stamped by SQLite's STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW') exactly as the
 * reference's SQLITE_NOW rows are (user.rs grant_membership_to_open_rooms,
 * crates/db/src/time.rs:16); ON CONFLICT DO NOTHING is the reference
 * conflict clause. */
static cf_err user_grant_memberships_to_open_rooms(cf_tx *tx, cf_db *db,
                                                   int64_t user_id) {
    sqlite3_stmt *rooms_stmt = NULL;
    cf_err rc = cf_db_stmt(db, &user_stmts, CF_USER_STMT_OPEN_ROOM_IDS,
                           &rooms_stmt);
    if (rc == CF_OK) {
        rc = cf_stmt_bind_text(rooms_stmt, 1,
                               user_span_of(CF_STR_LIT(CF_USER_ROOMS_OPEN)));
    }
    while (rc == CF_OK) {
        int step = sqlite3_step(rooms_stmt);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            rc = user_step_failed(db, step, "open rooms for membership grant");
            break;
        }
        int64_t room_id = cf_stmt_column_i64(rooms_stmt, 0);
        sqlite3_stmt *insert_stmt = NULL;
        rc = cf_db_stmt(db, &user_stmts, CF_USER_STMT_GRANT_MEMBERSHIP,
                        &insert_stmt);
        if (rc == CF_OK) rc = cf_stmt_bind_i64(insert_stmt, 1, room_id);
        if (rc == CF_OK) rc = cf_stmt_bind_i64(insert_stmt, 2, user_id);
        if (rc == CF_OK) {
            int ins = sqlite3_step(insert_stmt); /* ROW, or DONE on conflict */
            if (ins != SQLITE_ROW && ins != SQLITE_DONE) {
                rc = user_step_failed(db, ins, "grant membership");
            }
        }
        cf_db_stmt_done(insert_stmt);
    }
    cf_db_stmt_done(rooms_stmt);
    (void)tx; /* the caller owns the transaction; this helper only uses db */
    return rc;
}

cf_err cf_user_create(cf_tx *tx, const cf_new_user *attributes,
                      cf_user *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "create user: no output");
    }
    memset(out, 0, sizeof *out);
    if (tx == NULL || attributes == NULL) {
        return cf_db_failf(CF_INVALID, "create user: no transaction");
    }
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) {
        return cf_db_failf(CF_INTERNAL, "transaction has no db");
    }
    if (attributes->name.len != 0 && attributes->name.ptr == NULL) {
        return CF_INVALID;
    }
    if (attributes->role < CF_ROLE_MEMBER ||
        attributes->role > CF_ROLE_BOT) {
        return cf_db_failf(CF_INVALID, "create user: role out of range");
    }
    if ((attributes->email_address.present &&
         attributes->email_address.value.len != 0 &&
         attributes->email_address.value.ptr == NULL) ||
        (attributes->password_digest.present &&
         attributes->password_digest.value.len != 0 &&
         attributes->password_digest.value.ptr == NULL) ||
        (attributes->bio.present && attributes->bio.value.len != 0 &&
         attributes->bio.value.ptr == NULL) ||
        (attributes->bot_token.present && attributes->bot_token.value.len != 0 &&
         attributes->bot_token.value.ptr == NULL)) {
        return CF_INVALID;
    }

    int64_t now = cf_now_us(NULL);
    char now_text[CF_DB_TIME_TEXT_CAP];
    cf_err rc = cf_db_time_to_text(now, now_text);
    if (rc != CF_OK) return rc;

    sqlite3_stmt *stmt = NULL;
    rc = cf_db_stmt(db, &user_stmts, CF_USER_STMT_INSERT, &stmt);
    if (rc == CF_OK) {
        rc = cf_stmt_bind_opt_text(
            stmt, 1, attributes->bio.present,
            user_span_of(attributes->bio.value));
    }
    if (rc == CF_OK) {
        rc = cf_stmt_bind_opt_text(
            stmt, 2, attributes->bot_token.present,
            user_span_of(attributes->bot_token.value));
    }
    if (rc == CF_OK) {
        rc = cf_stmt_bind_text(stmt, 3,
                               (cf_span){(const unsigned char *)now_text,
                                         strlen(now_text)});
    }
    if (rc == CF_OK) {
        rc = cf_stmt_bind_opt_text(
            stmt, 4, attributes->email_address.present,
            user_span_of(attributes->email_address.value));
    }
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 5,
                                            user_span_of(attributes->name));
    if (rc == CF_OK) {
        rc = cf_stmt_bind_opt_text(
            stmt, 6, attributes->password_digest.present,
            user_span_of(attributes->password_digest.value));
    }
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 7, (int64_t)attributes->role);
    if (rc == CF_OK) {
        rc = cf_stmt_bind_i64(stmt, 8, (int64_t)CF_STATUS_ACTIVE);
    }
    if (rc == CF_OK) {
        rc = cf_stmt_bind_text(stmt, 9,
                               (cf_span){(const unsigned char *)now_text,
                                         strlen(now_text)});
    }

    int64_t id = 0;
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            id = cf_stmt_column_i64(stmt, 0);
        } else if (step == SQLITE_DONE) {
            rc = cf_db_failf(CF_INTERNAL, "insert returned no id");
        } else {
            rc = user_step_failed(db, step, "create user");
        }
    }
    cf_db_stmt_done(stmt);
    if (rc != CF_OK) return rc;

    rc = user_grant_memberships_to_open_rooms(tx, db, id);
    if (rc != CF_OK) return rc;
    return cf_user_find(db, id, out); /* reference re-reads the row */
}

cf_err cf_user_create_bot(cf_tx *tx, cf_str name, cf_optional_str webhook_url,
                          cf_user *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "create bot: no output");
    }
    memset(out, 0, sizeof *out);
    if (tx == NULL) {
        return cf_db_failf(CF_INVALID, "create bot: no transaction");
    }

    cf_str token = {NULL, 0};
    cf_err rc = cf_user_generate_bot_token(&token);
    if (rc != CF_OK) return rc;

    cf_new_user attributes = {
        .name = name,
        .bot_token = {.present = true, .value = token},
        .role = CF_ROLE_BOT,
    };
    rc = cf_user_create(tx, &attributes, out);
    cf_str_dispose(&token);
    if (rc != CF_OK) return rc;

    if (webhook_url.present) {
        cf_webhook webhook = {0};
        rc = cf_webhook_create(tx, out->id, webhook_url, &webhook);
        if (rc != CF_OK) {
            cf_user_dispose(out);
            return rc;
        }
        cf_webhook_dispose(&webhook);
    }
    return CF_OK;
}

/* --- updating ------------------------------------------------------------- */

/* One fixed per-column UPDATE: value at 1, updated_at at 2, id at 3. */
static cf_err user_update_text_column(cf_db *db, size_t stmt_id, cf_str value,
                                      const char *now_text, int64_t id,
                                      const char *op) {
    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &user_stmts, stmt_id, &stmt);
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 1, user_span_of(value));
    if (rc == CF_OK) {
        rc = cf_stmt_bind_text(stmt, 2,
                               (cf_span){(const unsigned char *)now_text,
                                         strlen(now_text)});
    }
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 3, id);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step != SQLITE_DONE) rc = user_step_failed(db, step, op);
    }
    cf_db_stmt_done(stmt);
    return rc;
}

/* A present=false value writes SQL NULL (optional, top bit set in stmt). */
static cf_err user_update_opt_column(cf_db *db, size_t stmt_id,
                                     cf_optional_str value,
                                     const char *now_text, int64_t id,
                                     const char *op) {
    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &user_stmts, stmt_id, &stmt);
    if (rc == CF_OK) {
        rc = cf_stmt_bind_opt_text(stmt, 1, value.present,
                                   user_span_of(value.value));
    }
    if (rc == CF_OK) {
        rc = cf_stmt_bind_text(stmt, 2,
                               (cf_span){(const unsigned char *)now_text,
                                         strlen(now_text)});
    }
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 3, id);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step != SQLITE_DONE) rc = user_step_failed(db, step, op);
    }
    cf_db_stmt_done(stmt);
    return rc;
}

static cf_err user_update_i64_column(cf_db *db, size_t stmt_id, int64_t value,
                                     const char *now_text, int64_t id,
                                     const char *op) {
    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &user_stmts, stmt_id, &stmt);
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 1, value);
    if (rc == CF_OK) {
        rc = cf_stmt_bind_text(stmt, 2,
                               (cf_span){(const unsigned char *)now_text,
                                         strlen(now_text)});
    }
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 3, id);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step != SQLITE_DONE) rc = user_step_failed(db, step, op);
    }
    cf_db_stmt_done(stmt);
    return rc;
}

static bool user_str_equal(cf_str left, cf_str right) {
    return left.len == right.len &&
           (left.len == 0 || memcmp(left.ptr, right.ptr, left.len) == 0);
}

static bool user_opt_str_equal(cf_optional_str left, cf_optional_str right) {
    if (left.present != right.present) return false;
    if (!left.present) return true;
    return user_str_equal(left.value, right.value);
}

cf_err cf_user_update(cf_tx *tx, cf_user *user,
                      const cf_user_changes *changes) {
    if (tx == NULL || user == NULL) {
        return cf_db_failf(CF_INVALID, "update user: no transaction or user");
    }
    static const cf_user_changes no_changes = {0};
    if (changes == NULL) changes = &no_changes;
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) {
        return cf_db_failf(CF_INTERNAL, "transaction has no db");
    }

    /* The Rust filters: a Some value equal to the current one is dropped;
     * password_digest has no equality filter. */
    bool change_name =
        changes->name != NULL && !user_str_equal(*changes->name, user->name);
    bool change_email =
        changes->email_address != NULL &&
        !user_opt_str_equal(*changes->email_address, user->email_address);
    bool change_digest = changes->password_digest != NULL;
    bool change_role =
        changes->role != NULL && *changes->role != user->role;
    bool change_status =
        changes->status != NULL && *changes->status != user->status;
    bool change_bio = changes->bio != NULL &&
                      !user_opt_str_equal(*changes->bio, user->bio);
    bool change_bot_token = changes->bot_token != NULL &&
                            !user_opt_str_equal(*changes->bot_token,
                                                user->bot_token);
    if (!change_name && !change_email && !change_digest && !change_role &&
        !change_status && !change_bio && !change_bot_token) {
        return CF_OK; /* nothing changed: not even updated_at */
    }

    int64_t now = cf_now_us(NULL);
    char now_text[CF_DB_TIME_TEXT_CAP];
    cf_err rc = cf_db_time_to_text(now, now_text);
    if (rc != CF_OK) return rc;

    /* The reference assigns each field as it builds the SET list, before the
     * UPDATE runs, so a SQL failure leaves the new values in memory. */
    if (change_name) {
        cf_str copy;
        rc = user_copy_input_text(*changes->name, &copy);
        if (rc != CF_OK) return rc;
        cf_str_dispose(&user->name);
        user->name = copy;
    }
    if (change_email) {
        cf_optional_str copy;
        rc = user_copy_optional_input(*changes->email_address, &copy);
        if (rc != CF_OK) return rc;
        cf_optional_str_dispose(&user->email_address);
        user->email_address = copy;
    }
    if (change_digest) {
        cf_str copy;
        rc = user_copy_input_text(*changes->password_digest, &copy);
        if (rc != CF_OK) return rc;
        cf_optional_str_dispose(&user->password_digest);
        user->password_digest =
            (cf_optional_str){.present = true, .value = copy};
    }
    if (change_role) user->role = *changes->role;
    if (change_status) user->status = *changes->status;
    if (change_bio) {
        cf_optional_str copy;
        rc = user_copy_optional_input(*changes->bio, &copy);
        if (rc != CF_OK) return rc;
        cf_optional_str_dispose(&user->bio);
        user->bio = copy;
    }
    if (change_bot_token) {
        cf_optional_str copy;
        rc = user_copy_optional_input(*changes->bot_token, &copy);
        if (rc != CF_OK) return rc;
        cf_optional_str_dispose(&user->bot_token);
        user->bot_token = copy;
    }
    user->updated_at = now;

    /* Source set order: name, email_address, password_digest, role, status,
     * bio, bot_token (+ updated_at). */
    if (change_name) {
        rc = user_update_text_column(db, CF_USER_STMT_UPDATE_NAME,
                                     user->name, now_text, user->id,
                                     "update user name");
    }
    if (rc == CF_OK && change_email) {
        rc = user_update_opt_column(db, CF_USER_STMT_UPDATE_EMAIL_ADDRESS,
                                    user->email_address, now_text, user->id,
                                    "update user email");
    }
    if (rc == CF_OK && change_digest) {
        rc = user_update_opt_column(db, CF_USER_STMT_UPDATE_PASSWORD_DIGEST,
                                    user->password_digest, now_text, user->id,
                                    "update user password_digest");
    }
    if (rc == CF_OK && change_role) {
        rc = user_update_i64_column(db, CF_USER_STMT_UPDATE_ROLE,
                                    (int64_t)user->role, now_text, user->id,
                                    "update user role");
    }
    if (rc == CF_OK && change_status) {
        rc = user_update_i64_column(db, CF_USER_STMT_UPDATE_STATUS,
                                    (int64_t)user->status, now_text, user->id,
                                    "update user status");
    }
    if (rc == CF_OK && change_bio) {
        rc = user_update_opt_column(db, CF_USER_STMT_UPDATE_BIO, user->bio,
                                    now_text, user->id, "update user bio");
    }
    if (rc == CF_OK && change_bot_token) {
        rc = user_update_opt_column(db, CF_USER_STMT_UPDATE_BOT_TOKEN,
                                    user->bot_token, now_text, user->id,
                                    "update user bot_token");
    }
    return rc;
}

cf_err cf_user_update_bot(cf_tx *tx, cf_user *user,
                          const cf_user_changes *changes,
                          cf_optional_str webhook_url) {
    if (tx == NULL || user == NULL) {
        return cf_db_failf(CF_INVALID, "update bot: no transaction or user");
    }
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) {
        return cf_db_failf(CF_INTERNAL, "transaction has no db");
    }
    if (webhook_url.present && webhook_url.value.len != 0 &&
        webhook_url.value.ptr == NULL) {
        return CF_INVALID;
    }

    bool found = false;
    cf_webhook webhook = {0};
    cf_err rc = cf_webhook_find_by_user(db, user->id, &found, &webhook);
    if (rc != CF_OK) return rc;
    bool url_present = webhook_url.present &&
                       !user_str_is_blank(webhook_url.value);
    if (url_present && found) {
        rc = cf_webhook_update_url(tx, &webhook, webhook_url.value);
    } else if (url_present && !found) {
        cf_webhook created = {0};
        rc = cf_webhook_create(tx, user->id, webhook_url, &created);
        if (rc == CF_OK) cf_webhook_dispose(&created);
    } else if (!url_present && found) {
        rc = cf_webhook_destroy(tx, &webhook);
    }
    if (found) cf_webhook_dispose(&webhook);
    if (rc != CF_OK) return rc;

    return cf_user_update(tx, user, changes);
}

cf_err cf_user_reset_bot_key(cf_tx *tx, cf_user *user) {
    if (tx == NULL || user == NULL) {
        return cf_db_failf(CF_INVALID, "reset bot key: no transaction or user");
    }
    cf_str token = {NULL, 0};
    cf_err rc = cf_user_generate_bot_token(&token);
    if (rc != CF_OK) return rc;
    const cf_optional_str new_token = {.present = true, .value = token};
    const cf_user_changes changes = {.bot_token = &new_token};
    rc = cf_user_update(tx, user, &changes);
    cf_str_dispose(&token);
    return rc;
}

/* --- deactivate / ban / unban --------------------------------------------- */

static cf_err user_execute_delete(cf_db *db, size_t stmt_id, int64_t user_id,
                                  const char *op) {
    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &user_stmts, stmt_id, &stmt);
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 1, user_id);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step != SQLITE_DONE) rc = user_step_failed(db, step, op);
    }
    cf_db_stmt_done(stmt);
    return rc;
}

/* SecureRandom.uuid / Uuid::new_v4 text (lowercase 8-4-4-4-12). */
static cf_err user_generate_uuid(cf_str *out) {
    out->ptr = NULL;
    out->len = 0;
    unsigned char bytes[16];
    cf_err rc = cf_random_bytes(bytes, sizeof bytes);
    if (rc != CF_OK) return rc;
    bytes[6] = (unsigned char)((bytes[6] & 0x0F) | 0x40);
    bytes[8] = (unsigned char)((bytes[8] & 0x3F) | 0x80);
    static const char hex[] = "0123456789abcdef";
    char text[36];
    size_t at = 0;
    for (size_t i = 0; i < sizeof bytes; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) text[at++] = '-';
        text[at++] = hex[bytes[i] >> 4];
        text[at++] = hex[bytes[i] & 0x0F];
    }
    return user_strdup_span(
        (cf_span){(const unsigned char *)text, sizeof text}, out);
}

/* deactivated_email_address: String#replace('@', "-deactivated-<uuid>@"). */
static cf_err user_deactivated_email(const cf_optional_str *email,
                                     cf_optional_str *out) {
    out->present = false;
    out->value.ptr = NULL;
    out->value.len = 0;
    cf_str uuid = {NULL, 0};
    cf_err rc = user_generate_uuid(&uuid); /* generated even when absent */
    if (rc != CF_OK) return rc;
    if (email->present) {
        if ((email->value.len != 0 && email->value.ptr == NULL) ||
            uuid.len != 36) {
            cf_str_dispose(&uuid);
            return CF_INVALID;
        }
        /* "-deactivated-" + uuid + "@" */
        size_t replacement_len = 13 + uuid.len + 1;
        size_t at_signs = 0;
        for (size_t i = 0; i < email->value.len; i++) {
            if (email->value.ptr[i] == '@') at_signs++;
        }
        if (at_signs != 0 &&
            (replacement_len > (SIZE_MAX - email->value.len) / at_signs)) {
            cf_str_dispose(&uuid);
            return CF_LIMIT;
        }
        size_t len = email->value.len + at_signs * replacement_len;
        char *text = malloc(len + 1);
        if (text == NULL) {
            cf_str_dispose(&uuid);
            return CF_NOMEM;
        }
        size_t at = 0;
        for (size_t i = 0; i < email->value.len; i++) {
            if (email->value.ptr[i] == '@') {
                memcpy(text + at, "-deactivated-", 13);
                at += 13;
                memcpy(text + at, uuid.ptr, uuid.len);
                at += uuid.len;
                text[at++] = '@';
            } else {
                text[at++] = email->value.ptr[i];
            }
        }
        text[at] = '\0';
        out->present = true;
        out->value = (cf_str){text, at};
    }
    cf_str_dispose(&uuid);
    return CF_OK;
}

cf_err cf_user_deactivate(cf_tx *tx, cf_user *user) {
    if (tx == NULL || user == NULL) {
        return cf_db_failf(CF_INVALID, "deactivate: no transaction or user");
    }
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) {
        return cf_db_failf(CF_INTERNAL, "transaction has no db");
    }
    int64_t user_id = user->id;

    /* close_remote_connections first, as Rails does mid-transaction. */
    cf_err rc = cf_tx_event(
        tx, (cf_event){.kind = CF_EVENT_DISCONNECT_USER,
                       .user_id = user_id,
                       .reconnect = false});
    if (rc != CF_OK) return rc;

    sqlite3_stmt *stmt = NULL;
    rc = cf_db_stmt(db, &user_stmts, CF_USER_STMT_DEACTIVATE_DELETE_MEMBERSHIPS,
                    &stmt);
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 1, user_id);
    if (rc == CF_OK) {
        rc = cf_stmt_bind_text(stmt, 2, user_span_of(CF_STR_LIT(
                                              CF_USER_ROOMS_DIRECT)));
    }
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step != SQLITE_DONE) {
            rc = user_step_failed(db, step, "deactivate memberships");
        }
    }
    cf_db_stmt_done(stmt);
    if (rc == CF_OK) {
        rc = user_execute_delete(
            db, CF_USER_STMT_DEACTIVATE_DELETE_PUSH_SUBSCRIPTIONS, user_id,
            "deactivate push subscriptions");
    }
    if (rc == CF_OK) {
        rc = user_execute_delete(db, CF_USER_STMT_DEACTIVATE_DELETE_SEARCHES,
                                 user_id, "deactivate searches");
    }
    if (rc == CF_OK) {
        rc = user_execute_delete(db, CF_USER_STMT_DEACTIVATE_DELETE_SESSIONS,
                                 user_id, "deactivate sessions");
    }
    if (rc != CF_OK) return rc;

    cf_optional_str email;
    rc = user_deactivated_email(&user->email_address, &email);
    if (rc != CF_OK) return rc;
    const cf_user_changes changes = {
        .status = &(cf_status){CF_STATUS_DEACTIVATED},
        .email_address = &email,
    };
    rc = cf_user_update(tx, user, &changes);
    cf_optional_str_dispose(&email);
    return rc;
}

/* Session::ip_address values, in row order. */
typedef struct {
    cf_optional_str *items;
    size_t len, cap;
} user_opt_str_vector;

static void user_opt_str_vector_dispose(user_opt_str_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len; i++) {
        cf_optional_str_dispose(&vector->items[i]);
    }
    free(vector->items);
    vector->items = NULL;
    vector->len = 0;
    vector->cap = 0;
}

static cf_err user_opt_str_vector_push(user_opt_str_vector *vector,
                                       cf_optional_str *item) {
    if (vector->len == vector->cap) {
        size_t cap = vector->cap != 0 ? vector->cap * 2 : 8;
        if (cap < vector->cap || cap > SIZE_MAX / sizeof *vector->items) {
            return CF_LIMIT;
        }
        cf_optional_str *items =
            realloc(vector->items, cap * sizeof *items);
        if (items == NULL) return CF_NOMEM;
        vector->items = items;
        vector->cap = cap;
    }
    vector->items[vector->len++] = *item;
    memset(item, 0, sizeof *item);
    return CF_OK;
}

cf_err cf_user_ban(cf_tx *tx, cf_user *user) {
    if (tx == NULL || user == NULL) {
        return cf_db_failf(CF_INVALID, "ban: no transaction or user");
    }
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) {
        return cf_db_failf(CF_INTERNAL, "transaction has no db");
    }
    int64_t user_id = user->id;

    /* create_bans_from_sessions: sessions.pluck(:ip_address)
     * .compact_blank.uniq, in row order. */
    user_opt_str_vector ips = {0};
    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &user_stmts, CF_USER_STMT_BAN_SESSION_IPS,
                           &stmt);
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 1, user_id);
    while (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            rc = user_step_failed(db, step, "ban session ips");
            break;
        }
        cf_optional_str ip;
        rc = user_copy_optional_text(stmt, 0, &ip);
        if (rc != CF_OK) break;
        rc = user_opt_str_vector_push(&ips, &ip);
        if (rc != CF_OK) cf_optional_str_dispose(&ip);
    }
    cf_db_stmt_done(stmt);
    if (rc != CF_OK) {
        user_opt_str_vector_dispose(&ips);
        return rc;
    }

    user_opt_str_vector seen = {0};
    for (size_t i = 0; i < ips.len && rc == CF_OK; i++) {
        cf_optional_str ip = ips.items[i];
        if (!ip.present || user_str_is_blank(ip.value)) continue;
        bool duplicate = false;
        for (size_t j = 0; j < seen.len; j++) {
            if (user_str_equal(seen.items[j].value, ip.value)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        cf_ban ban = {0};
        rc = cf_ban_create(tx, user_id, ip.value, &ban);
        if (rc != CF_OK) break;
        cf_ban_dispose(&ban);
        cf_optional_str copy;
        rc = user_copy_optional_input(ip, &copy);
        if (rc != CF_OK) break;
        rc = user_opt_str_vector_push(&seen, &copy);
        if (rc != CF_OK) cf_optional_str_dispose(&copy);
    }
    user_opt_str_vector_dispose(&ips);
    user_opt_str_vector_dispose(&seen);
    if (rc != CF_OK) return rc;

    rc = cf_tx_event(tx, (cf_event){.kind = CF_EVENT_DISCONNECT_USER,
                                    .user_id = user_id,
                                    .reconnect = false});
    if (rc != CF_OK) return rc;
    rc = user_execute_delete(db, CF_USER_STMT_BAN_DELETE_SESSIONS, user_id,
                             "ban delete sessions");
    if (rc != CF_OK) return rc;
    rc = cf_tx_event(tx, (cf_event){.kind = CF_EVENT_REMOVE_BANNED_CONTENT,
                                    .user_id = user_id});
    if (rc != CF_OK) return rc;

    const cf_user_changes changes = {
        .status = &(cf_status){CF_STATUS_BANNED},
    };
    return cf_user_update(tx, user, &changes);
}

cf_err cf_user_unban(cf_tx *tx, cf_user *user) {
    if (tx == NULL || user == NULL) {
        return cf_db_failf(CF_INVALID, "unban: no transaction or user");
    }
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) {
        return cf_db_failf(CF_INTERNAL, "transaction has no db");
    }
    cf_err rc = user_execute_delete(db, CF_USER_STMT_UNBAN_DELETE_BANS,
                                    user->id, "unban delete bans");
    if (rc != CF_OK) return rc;
    const cf_user_changes changes = {
        .status = &(cf_status){CF_STATUS_ACTIVE},
    };
    return cf_user_update(tx, user, &changes);
}

cf_err cf_user_remove_banned_content(cf_tx *tx, const cf_user *user,
                                     cf_message_vector *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "remove banned content: no output");
    }
    memset(out, 0, sizeof *out);
    if (tx == NULL || user == NULL) {
        return cf_db_failf(CF_INVALID, "remove banned content: no user");
    }
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) {
        return cf_db_failf(CF_INTERNAL, "transaction has no db");
    }
    cf_err rc = cf_message_by_creator(db, user->id, out);
    if (rc != CF_OK) return rc;
    /* Destroy every message but return the copies, as the reference does. */
    for (size_t i = 0; i < out->len; i++) {
        rc = cf_message_destroy(tx, &out->items[i]);
        if (rc != CF_OK) {
            cf_message_vector_dispose(out);
            return rc;
        }
    }
    return CF_OK;
}

cf_err cf_user_reset_remote_connections(cf_tx *tx, const cf_user *user) {
    if (tx == NULL || user == NULL) {
        return cf_db_failf(CF_INVALID, "reset remote connections: no user");
    }
    return cf_tx_event(tx, (cf_event){.kind = CF_EVENT_DISCONNECT_USER,
                                      .user_id = user->id,
                                      .reconnect = true});
}

cf_err cf_user_deliver_webhook_later(cf_tx *tx, const cf_user *user,
                                     int64_t message_id) {
    if (tx == NULL || user == NULL) {
        return cf_db_failf(CF_INVALID, "deliver webhook: no user");
    }
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) {
        return cf_db_failf(CF_INTERNAL, "transaction has no db");
    }
    bool found = false;
    cf_webhook webhook;
    cf_err rc = cf_webhook_find_by_user(db, user->id, &found, &webhook);
    if (rc != CF_OK) return rc;
    if (found) {
        cf_webhook_dispose(&webhook);
        rc = cf_tx_event(tx, (cf_event){.kind = CF_EVENT_DELIVER_WEBHOOK,
                                        .user_id = user->id,
                                        .message_id = message_id});
    }
    return rc;
}

/* --- associations --------------------------------------------------------- */

cf_err cf_user_memberships(cf_db *db, const cf_user *user,
                           cf_membership_vector *out) {
    if (out == NULL || user == NULL) {
        return cf_db_failf(CF_INVALID, "memberships: no output or user");
    }
    return cf_membership_for_user(db, user->id, out);
}

cf_err cf_user_sessions(cf_db *db, const cf_user *user,
                        cf_session_vector *out) {
    if (out == NULL || user == NULL) {
        return cf_db_failf(CF_INVALID, "sessions: no output or user");
    }
    return cf_session_for_user(db, user->id, out);
}

cf_err cf_user_webhook(cf_db *db, const cf_user *user, bool *found,
                       cf_webhook *out) {
    if (user == NULL) {
        return cf_db_failf(CF_INVALID, "webhook: no user");
    }
    return cf_webhook_find_by_user(db, user->id, found, out);
}

cf_err cf_user_webhook_url(cf_db *db, const cf_user *user, bool *found,
                           cf_str *out) {
    if (found == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "webhook url: no output");
    }
    *found = false;
    out->ptr = NULL;
    out->len = 0;
    if (user == NULL) {
        return cf_db_failf(CF_INVALID, "webhook url: no user");
    }
    bool have_webhook = false;
    cf_webhook webhook = {0};
    cf_err rc = cf_webhook_find_by_user(db, user->id, &have_webhook, &webhook);
    if (rc != CF_OK) return rc;
    if (have_webhook && webhook.url.present) {
        rc = user_copy_input_text(webhook.url.value, out);
        if (rc == CF_OK) *found = true;
    }
    if (have_webhook) cf_webhook_dispose(&webhook);
    return rc;
}

/* --- attributes ----------------------------------------------------------- */

cf_err cf_user_initials(const cf_user *user, cf_str *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "initials: no output");
    }
    out->ptr = NULL;
    out->len = 0;
    if (user == NULL) return CF_INVALID;

    /* name.scan(/\b\w/).join: ASCII word characters at a boundary, where the
     * boundary test is Unicode (see user_cp_is_word). */
    size_t cap = user->name.len + 1;
    char *initials = malloc(cap);
    if (initials == NULL) return CF_NOMEM;
    size_t at = 0;
    const unsigned char *p = (const unsigned char *)user->name.ptr;
    size_t i = 0;
    bool have_previous = false;
    bool previous_word = false;
    while (i < user->name.len) {
        uint32_t cp;
        size_t used = user_utf8_decode(p + i, user->name.len - i, &cp);
        bool boundary = !have_previous || !previous_word;
        if (cp < 0x80 && user_cp_is_word(cp) && boundary) {
            initials[at++] = (char)cp;
        }
        previous_word = user_cp_is_word(cp);
        have_previous = true;
        i += used;
    }
    initials[at] = '\0';
    out->ptr = initials;
    out->len = at;
    return CF_OK;
}

cf_err cf_user_title(const cf_user *user, cf_str *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "title: no output");
    }
    out->ptr = NULL;
    out->len = 0;
    if (user == NULL) return CF_INVALID;

    /* [name, bio].compact_blank.join(" – ") */
    bool name_present = !user_str_is_blank(user->name);
    bool bio_present =
        user->bio.present && !user_str_is_blank(user->bio.value);
    if (name_present && bio_present) {
        return user_concat(user->name, CF_STR_LIT(" – "), user->bio.value,
                           out);
    }
    if (name_present) return user_copy_input_text(user->name, out);
    if (bio_present) return user_copy_input_text(user->bio.value, out);
    return user_strdup_span((cf_span){NULL, 0}, out); /* empty string */
}

cf_err cf_user_bot_key(const cf_user *user, cf_str *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "bot key: no output");
    }
    out->ptr = NULL;
    out->len = 0;
    if (user == NULL) return CF_INVALID;

    cf_str id_text = {NULL, 0};
    cf_err rc = user_i64_text(user->id, &id_text);
    if (rc != CF_OK) return rc;
    cf_str token = {NULL, 0};
    if (user->bot_token.present) token = user->bot_token.value;
    rc = user_concat(id_text, CF_STR_LIT("-"), token, out);
    cf_str_dispose(&id_text);
    return rc;
}

bool cf_user_can_administer(const cf_user *user,
                            cf_optional_i64 record_creator_id,
                            bool record_is_new) {
    if (user == NULL) return false;
    return user->role == CF_ROLE_ADMINISTRATOR ||
           (record_creator_id.present &&
            record_creator_id.value == user->id) ||
           record_is_new;
}

bool cf_user_is_member(const cf_user *user) {
    return user != NULL && user->role == CF_ROLE_MEMBER;
}

bool cf_user_is_administrator(const cf_user *user) {
    return user != NULL && user->role == CF_ROLE_ADMINISTRATOR;
}

bool cf_user_is_bot(const cf_user *user) {
    return user != NULL && user->role == CF_ROLE_BOT;
}

bool cf_user_is_active(const cf_user *user) {
    return user != NULL && user->status == CF_STATUS_ACTIVE;
}

bool cf_user_is_deactivated(const cf_user *user) {
    return user != NULL && user->status == CF_STATUS_DEACTIVATED;
}

bool cf_user_is_banned(const cf_user *user) {
    return user != NULL && user->status == CF_STATUS_BANNED;
}

cf_err cf_user_attachable_plain_text_representation(const cf_user *user,
                                                    cf_str *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "attachable text: no output");
    }
    out->ptr = NULL;
    out->len = 0;
    if (user == NULL) return CF_INVALID;
    return user_concat(CF_STR_LIT("@"), (cf_str){NULL, 0}, user->name, out);
}

cf_err cf_user_reload(cf_db *db, cf_user *user) {
    if (user == NULL) {
        return cf_db_failf(CF_INVALID, "reload: no user");
    }
    cf_user fresh;
    cf_err rc = cf_user_find(db, user->id, &fresh);
    if (rc != CF_OK) return rc;
    cf_user_dispose(user);
    *user = fresh;
    return CF_OK;
}

/* --- generate_bot_token --------------------------------------------------- */

cf_err cf_user_generate_bot_token(cf_str *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "generate bot token: no output");
    }
    out->ptr = NULL;
    out->len = 0;
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    enum { TOKEN_LEN = 12, ACCEPT_BELOW = 248 }; /* 248 = 4 * 62 */
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
                /* Uniform over 62 characters; a stuck entropy source fails
                 * instead of spinning. */
                if (++rejected > 4096) {
                    free(token);
                    return cf_db_failf(CF_INTERNAL,
                                       "entropy source produced no usable "
                                       "alphanumeric byte");
                }
                continue;
            }
            token[filled++] = alphabet[batch[i] % 62];
        }
    }
    token[TOKEN_LEN] = '\0';
    out->ptr = token;
    out->len = TOKEN_LEN;
    return CF_OK;
}
