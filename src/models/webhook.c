/* src/models/webhook.c — D01 model family "webhook".
 *
 * Translation of tmp/rust-ref/crates/db/src/models/webhook.rs (5 functions).
 * Webhook::ENDPOINT_TIMEOUT (7 s) is header-only and unused here; HTTP
 * delivery belongs to I01.
 *
 * Rules carried from 02-data-auth.md D01:
 *  - reads take cf_db *, mutations take cf_tx * (the writer connection is
 *    reached through cf_tx_db, D02);
 *  - fixed SQL only, in this module's own statement set (one prepared-
 *    statement cache per connection);
 *  - Option<T> reads return found=false with an empty record, never an error;
 *    a missing reference row in payload propagates CF_NOT_FOUND (Room::find /
 *    User::find);
 *  - datetimes are the reference UTC SQL text on the wire and int64 UTC
 *    microseconds in memory; create/update_url stamp tx.now() through
 *    cf_now_us (tests inject the fixed clock via core/testclock.h);
 *  - update_url is a no-op when the stored url already equals the argument
 *    (the reference returns before reading the clock);
 *  - payload's JSON matches rails_compat::json::encode: keys in the written
 *    order, compact separators, and the HTML entities <, > and & escaped
 *    with backslash-u003c-style lowercase-hex escapes (R01's cf_json_string
 *    is that exact encoder).
 *
 * The delivery/ownership rules named for the webhook family in 02-data-auth.md
 * (bot delivery selection, per-user webhook lookup) live in user.rs
 * (cf_user_deliver_webhook_later / cf_user_webhook), not in these five
 * functions; webhook.rs itself performs no ownership check.
 */
#include "models/webhook.h"

#include "db/db_internal.h"
#include "models/message.h"
#include "models/room.h"
#include "models/user.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fixed statement ids for this module's set. */
enum {
    CF_WH_STMT_FIND_BY_USER = 0,
    CF_WH_STMT_CREATE,
    CF_WH_STMT_UPDATE_URL,
    CF_WH_STMT_DESTROY
};

/* SQL text copied from the reference.  The SELECT keeps the reference's
 * `"webhooks".*`; the column positions below are the schema.sql declaration
 * order (id, created_at, updated_at, url, user_id), and from_row's name-based
 * reads map onto exactly those columns. */
static const cf_stmt_def cf_wh_stmts[] = {
    /* Rust: Webhook::find_by_user */
    {"SELECT \"webhooks\".* FROM \"webhooks\" WHERE \"webhooks\".\"user_id\" = ? LIMIT 1"},
    /* Rust: Webhook::create */
    {"INSERT INTO \"webhooks\" (\"created_at\", \"updated_at\", \"url\", \"user_id\") VALUES (?, ?, ?, ?) RETURNING \"id\""},
    /* Rust: Webhook::update_url */
    {"UPDATE \"webhooks\" SET \"updated_at\" = ?, \"url\" = ? WHERE \"webhooks\".\"id\" = ?"},
    /* Rust: Webhook::destroy */
    {"DELETE FROM \"webhooks\" WHERE \"webhooks\".\"id\" = ?"}};

static const cf_stmt_set cf_wh_stmt_set = {
    cf_wh_stmts, sizeof cf_wh_stmts / sizeof cf_wh_stmts[0]};

/* columns of "webhooks".* (schema.sql declaration order). */
enum {
    CF_WH_COL_ID = 0,
    CF_WH_COL_CREATED_AT,
    CF_WH_COL_UPDATED_AT,
    CF_WH_COL_URL,
    CF_WH_COL_USER_ID
};

/* --- small ownership helpers --------------------------------------------- */

/* Borrowed span over an owned cf_str argument. */
static cf_span wh_span(cf_str text) {
    cf_span span;
    span.ptr = (const unsigned char *)text.ptr;
    span.len = text.len;
    return span;
}

/* Borrowed span over a NUL-terminated buffer (fixed datetime text). */
static cf_span wh_cstr_span(const char *text) {
    cf_span span;
    span.ptr = (const unsigned char *)text;
    span.len = strlen(text);
    return span;
}

/* Owned copy of a borrowed span: NUL-terminated, len excludes the NUL. */
static cf_err wh_str_copy(cf_span src, cf_str *out) {
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
static cf_err wh_str_from_buf(const cf_buf *buf, cf_str *out) {
    return wh_str_copy(cf_buf_span(buf), out);
}

/* One failed step: copy the connection message before resetting the
 * statement, then report it with the mapped code. */
static cf_err wh_step_failure(cf_db *db, sqlite3_stmt *stmt, int sqlite_rc,
                              const char *operation) {
    char message[CF_DB_ERROR_CAP];
    snprintf(message, sizeof message, "%s", sqlite3_errmsg(cf_db_handle(db)));
    cf_db_stmt_done(stmt);
    return cf_db_failf(cf_db_err(sqlite_rc), "%s: %s", operation, message);
}

/* Read a NOT NULL datetime(6) text column into UTC microseconds. */
static cf_err wh_column_time(sqlite3_stmt *stmt, int column, const char *name,
                             int64_t *out_us) {
    cf_span text = cf_stmt_column_text(stmt, column);
    if (text.ptr == NULL) {
        return cf_db_failf(CF_DB, "webhooks.%s is NULL", name);
    }
    if (cf_db_time_from_text(text, out_us) != CF_OK) {
        return cf_db_failf(CF_DB, "webhooks.%s is not valid datetime text",
                           name);
    }
    return CF_OK;
}

/* tx.now(): the same instant is used for both datetime columns and for the
 * returned record. */
static cf_err wh_now(int64_t *out_us, char out_text[CF_DB_TIME_TEXT_CAP]) {
    int64_t now = cf_now_us(NULL);
    cf_err rc = cf_db_time_to_text(now, out_text);
    if (rc != CF_OK) return rc;
    *out_us = now;
    return CF_OK;
}

/* --- disposal ------------------------------------------------------------- */

void cf_webhook_dispose(cf_webhook *webhook) {
    if (webhook == NULL) return;
    cf_optional_str_dispose(&webhook->url);
    *webhook = (cf_webhook){0};
}

void cf_webhook_vector_dispose(cf_webhook_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len; i++) {
        cf_webhook_dispose(&vector->items[i]);
    }
    free(vector->items);
    vector->items = NULL;
    vector->len = 0;
    vector->cap = 0;
}

/* --- find_by_user --------------------------------------------------------- */

cf_err cf_webhook_find_by_user(cf_db *db, int64_t user_id, bool *found,
                               cf_webhook *out) {
    if (found == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "find_by_user: no output pointer");
    }
    *found = false;
    *out = (cf_webhook){0};
    if (db == NULL) return cf_db_failf(CF_INVALID, "find_by_user: no database");

    sqlite3_stmt *stmt = NULL;
    cf_err rc =
        cf_db_stmt(db, &cf_wh_stmt_set, CF_WH_STMT_FIND_BY_USER, &stmt);
    if (rc != CF_OK) return rc;

    /* params![user_id] */
    rc = cf_stmt_bind_i64(stmt, 1, user_id);
    if (rc != CF_OK) {
        cf_db_stmt_done(stmt);
        return rc;
    }

    cf_buf *url_buf = NULL;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    cf_err read_rc;

    int step = sqlite3_step(stmt);
    if (step == SQLITE_DONE) {
        /* query_one(...).optional() -> None: not an error. */
        cf_db_stmt_done(stmt);
        return CF_OK;
    }
    if (step != SQLITE_ROW) {
        return wh_step_failure(db, stmt, step, "find_by_user");
    }

    /* Column data is copied (or scalar) before cf_db_stmt_done resets. */
    int64_t id = cf_stmt_column_i64(stmt, CF_WH_COL_ID);
    int64_t row_user_id = cf_stmt_column_i64(stmt, CF_WH_COL_USER_ID);
    read_rc = wh_column_time(stmt, CF_WH_COL_CREATED_AT, "created_at",
                             &created_at);
    if (read_rc == CF_OK) {
        read_rc = wh_column_time(stmt, CF_WH_COL_UPDATED_AT, "updated_at",
                                 &updated_at);
    }
    if (read_rc == CF_OK) {
        read_rc = cf_stmt_column_copy_text(stmt, CF_WH_COL_URL, &url_buf);
    }

    cf_db_stmt_done(stmt);

    if (read_rc == CF_OK) {
        out->id = id;
        out->user_id = row_user_id;
        out->created_at = created_at;
        out->updated_at = updated_at;
        if (url_buf != NULL) {
            out->url.present = true;
            read_rc = wh_str_from_buf(url_buf, &out->url.value);
        }
    }

    cf_buf_release(url_buf);

    if (read_rc != CF_OK) {
        cf_webhook_dispose(out);
        return read_rc;
    }
    *found = true;
    return CF_OK;
}

/* --- create --------------------------------------------------------------- */

cf_err cf_webhook_create(cf_tx *tx, int64_t user_id, cf_optional_str url,
                         cf_webhook *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "create: no output pointer");
    *out = (cf_webhook){0};
    if (tx == NULL) return cf_db_failf(CF_INVALID, "create: no transaction");
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return cf_db_failf(CF_INVALID, "create: no database");

    int64_t now = 0;
    char now_text[CF_DB_TIME_TEXT_CAP];
    cf_err rc = wh_now(&now, now_text);
    if (rc != CF_OK) return rc;

    /* The returned record owns its url copy; validate/copy before executing so
     * a failure leaves *out empty (it is already reset above). */
    cf_optional_str url_copy = {0};
    if (url.present) {
        url_copy.present = true;
        rc = wh_str_copy(wh_span(url.value), &url_copy.value);
        if (rc != CF_OK) return rc;
    }

    sqlite3_stmt *stmt = NULL;
    rc = cf_db_stmt(db, &cf_wh_stmt_set, CF_WH_STMT_CREATE, &stmt);
    if (rc != CF_OK) {
        cf_optional_str_dispose(&url_copy);
        return rc;
    }

    /* params![now, now, url, user_id] */
    rc = cf_stmt_bind_text(stmt, 1, wh_cstr_span(now_text));
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 2, wh_cstr_span(now_text));
    if (rc == CF_OK) {
        rc = cf_stmt_bind_opt_text(stmt, 3, url.present, wh_span(url.value));
    }
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 4, user_id);
    if (rc != CF_OK) {
        cf_db_stmt_done(stmt);
        cf_optional_str_dispose(&url_copy);
        return rc;
    }

    int step = sqlite3_step(stmt);
    if (step != SQLITE_ROW) {
        cf_optional_str_dispose(&url_copy);
        if (step == SQLITE_DONE) {
            cf_db_stmt_done(stmt);
            return cf_db_failf(CF_DB, "create: INSERT returned no id");
        }
        return wh_step_failure(db, stmt, step, "create");
    }
    int64_t id = cf_stmt_column_i64(stmt, 0);
    cf_db_stmt_done(stmt);

    out->id = id;
    out->user_id = user_id;
    out->url = url_copy;
    out->created_at = now;
    out->updated_at = now;
    return CF_OK;
}

/* --- update_url ----------------------------------------------------------- */

cf_err cf_webhook_update_url(cf_tx *tx, cf_webhook *webhook, cf_str url) {
    if (webhook == NULL) {
        return cf_db_failf(CF_INVALID, "update_url: no record");
    }
    if (tx == NULL) {
        return cf_db_failf(CF_INVALID, "update_url: no transaction");
    }
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return cf_db_failf(CF_INVALID, "update_url: no database");
    if (url.len != 0 && url.ptr == NULL) {
        return cf_db_failf(CF_INVALID, "update_url: url has no bytes");
    }

    /* `if self.url.as_deref() == Some(url) { return Ok(()) }`: an unchanged
     * url does not read the clock and does not touch the row. */
    if (webhook->url.present && webhook->url.value.len == url.len &&
        (url.len == 0 ||
         (webhook->url.value.ptr != NULL &&
          memcmp(webhook->url.value.ptr, url.ptr, url.len) == 0))) {
        return CF_OK;
    }

    int64_t now = 0;
    char now_text[CF_DB_TIME_TEXT_CAP];
    cf_err rc = wh_now(&now, now_text);
    if (rc != CF_OK) return rc;

    /* The reference mutates `record` only after execute succeeds; copy first
     * so a failed statement leaves the record untouched. */
    cf_str url_copy = {0};
    rc = wh_str_copy(wh_span(url), &url_copy);
    if (rc != CF_OK) return rc;

    sqlite3_stmt *stmt = NULL;
    rc = cf_db_stmt(db, &cf_wh_stmt_set, CF_WH_STMT_UPDATE_URL, &stmt);
    if (rc != CF_OK) {
        cf_str_dispose(&url_copy);
        return rc;
    }

    /* params![now, url, self.id] */
    rc = cf_stmt_bind_text(stmt, 1, wh_cstr_span(now_text));
    if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 2, wh_span(url_copy));
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 3, webhook->id);
    if (rc != CF_OK) {
        cf_db_stmt_done(stmt);
        cf_str_dispose(&url_copy);
        return rc;
    }

    int step = sqlite3_step(stmt);
    if (step != SQLITE_DONE) {
        cf_err failure = wh_step_failure(db, stmt, step, "update_url");
        cf_str_dispose(&url_copy);
        return failure;
    }
    cf_db_stmt_done(stmt);

    cf_optional_str_dispose(&webhook->url);
    webhook->url.present = true;
    webhook->url.value = url_copy;
    webhook->updated_at = now;
    return CF_OK;
}

/* --- destroy -------------------------------------------------------------- */

cf_err cf_webhook_destroy(cf_tx *tx, const cf_webhook *webhook) {
    if (webhook == NULL) {
        return cf_db_failf(CF_INVALID, "destroy: no record");
    }
    if (tx == NULL) return cf_db_failf(CF_INVALID, "destroy: no transaction");
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return cf_db_failf(CF_INVALID, "destroy: no database");

    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &cf_wh_stmt_set, CF_WH_STMT_DESTROY, &stmt);
    if (rc != CF_OK) return rc;

    rc = cf_stmt_bind_i64(stmt, 1, webhook->id);
    if (rc != CF_OK) {
        cf_db_stmt_done(stmt);
        return rc;
    }

    int step = sqlite3_step(stmt);
    if (step != SQLITE_DONE) {
        return wh_step_failure(db, stmt, step, "destroy");
    }
    cf_db_stmt_done(stmt);
    return CF_OK;
}

/* --- payload -------------------------------------------------------------- */

/* cf_json_string appends the JSON-quoted, ActiveSupport::JSON-escaped text. */
static cf_err wh_json_text(cf_builder *builder, cf_str text) {
    return cf_json_string(builder, wh_span(text));
}

static cf_err wh_json_opt_text(cf_builder *builder, const cf_optional_str *text) {
    if (!text->present) {
        cf_span null_lit = {(const unsigned char *)"null", 4};
        return cf_builder_append(builder, null_lit);
    }
    return wh_json_text(builder, text->value);
}

static cf_err wh_json_i64(cf_builder *builder, int64_t value) {
    char digits[24];
    int len = snprintf(digits, sizeof digits, "%" PRId64, value);
    if (len < 0 || (size_t)len >= sizeof digits) {
        return cf_db_failf(CF_LIMIT, "payload: integer does not format");
    }
    cf_span span = {(const unsigned char *)digits, (size_t)len};
    return cf_builder_append(builder, span);
}

/* Append a fixed JSON fragment (structural text and key names only). */
static cf_err wh_json_raw(cf_builder *builder, const char *text) {
    cf_span span = {(const unsigned char *)text, strlen(text)};
    return cf_builder_append(builder, span);
}

/* Strict UTF-8 decode of one scalar at p; returns its byte length (1..4) and
 * sets *cp, or 0 when the bytes are not a valid sequence. */
static size_t wh_utf8_decode(const unsigned char *p, size_t remaining,
                             uint32_t *cp) {
    if (remaining == 0) return 0;
    unsigned char b = p[0];
    if (b < 0x80) {
        *cp = b;
        return 1;
    }
    if (b >= 0xC2 && b <= 0xDF) {
        if (remaining < 2 || (p[1] & 0xC0) != 0x80) return 0;
        *cp = ((uint32_t)(b & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
        return 2;
    }
    if (b >= 0xE0 && b <= 0xEF) {
        if (remaining < 3 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) {
            return 0;
        }
        if (b == 0xE0 && p[1] < 0xA0) return 0;       /* overlong */
        if (b == 0xED && p[1] >= 0xA0) return 0;      /* surrogate */
        *cp = ((uint32_t)(b & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
              (uint32_t)(p[2] & 0x3F);
        return 3;
    }
    if (b >= 0xF0 && b <= 0xF4) {
        if (remaining < 4 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 ||
            (p[3] & 0xC0) != 0x80) {
            return 0;
        }
        if (b == 0xF0 && p[1] < 0x90) return 0;       /* overlong */
        if (b == 0xF4 && p[1] >= 0x90) return 0;      /* above U+10FFFF */
        *cp = ((uint32_t)(b & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
              ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
        return 4;
    }
    return 0;
}

/* Rust `char::is_whitespace` (Unicode White_Space property). */
static bool wh_codepoint_is_whitespace(uint32_t cp) {
    if (cp >= 0x0009 && cp <= 0x000D) return true;
    if (cp == 0x0020 || cp == 0x0085 || cp == 0x00A0 || cp == 0x1680) {
        return true;
    }
    if (cp >= 0x2000 && cp <= 0x200A) return true;
    if (cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F ||
        cp == 0x3000) {
        return true;
    }
    return false;
}

/* without_recipient_mentions: remove every literal occurrence of `mention`
 * ("@name") and trim Unicode whitespace from both ends, exactly like
 * `body.replace(mention, "").trim_matches(char::is_whitespace)`. */
static cf_err wh_without_mentions(cf_str body, cf_str mention, cf_str *out) {
    out->ptr = NULL;
    out->len = 0;
    if (mention.len != 0 && mention.ptr == NULL) {
        return cf_db_failf(CF_INVALID, "payload: mention has no bytes");
    }
    if (body.len != 0 && body.ptr == NULL) {
        return cf_db_failf(CF_INVALID, "payload: body has no bytes");
    }

    cf_builder replaced = {0};
    size_t i = 0;
    while (i < body.len) {
        if (mention.len != 0 && mention.len <= body.len - i &&
            memcmp(body.ptr + i, mention.ptr, mention.len) == 0) {
            i += mention.len;
            continue;
        }
        cf_span byte = {(const unsigned char *)body.ptr + i, 1};
        cf_err rc = cf_builder_append(&replaced, byte);
        if (rc != CF_OK) {
            cf_builder_dispose(&replaced);
            return rc;
        }
        i++;
    }

    const unsigned char *empty = (const unsigned char *)"";
    const unsigned char *bytes = replaced.ptr != NULL ? replaced.ptr : empty;
    size_t len = replaced.len;
    size_t start = 0;
    size_t end = len;
    while (start < end) {
        uint32_t cp = 0;
        size_t n = wh_utf8_decode(bytes + start, end - start, &cp);
        if (n == 0 || !wh_codepoint_is_whitespace(cp)) break;
        start += n;
    }
    while (end > start) {
        size_t prev = end - 1;
        while (prev > start && (bytes[prev] & 0xC0) == 0x80) prev--;
        uint32_t cp = 0;
        size_t n = wh_utf8_decode(bytes + prev, end - prev, &cp);
        if (n != end - prev || !wh_codepoint_is_whitespace(cp)) break;
        end = prev;
    }

    cf_span trimmed = {bytes + start, end - start};
    cf_err rc = wh_str_copy(trimmed, out);
    cf_builder_dispose(&replaced);
    return rc;
}

/* JSON body, key order as written in Webhook#payload:
 * {"user":{"id":i,"name":s},"room":{"id":i,"name":s|null,"path":s},
 *  "message":{"id":i,"body":{"html":s|null,"plain":s},"path":s}} */
static cf_err wh_payload_json(const cf_user *creator, const cf_room *room,
                              const cf_message *message, bool have_html,
                              cf_str html, cf_str plain,
                              cf_str room_bot_messages_path,
                              cf_str message_path, cf_str *out) {
    cf_builder json = {0};
    cf_buf *json_buf = NULL;
    cf_err rc = wh_json_raw(&json, "{\"user\":{\"id\":");
    if (rc == CF_OK) rc = wh_json_i64(&json, creator->id);
    if (rc == CF_OK) rc = wh_json_raw(&json, ",\"name\":");
    if (rc == CF_OK) rc = wh_json_text(&json, creator->name);
    if (rc == CF_OK) rc = wh_json_raw(&json, "},\"room\":{\"id\":");
    if (rc == CF_OK) rc = wh_json_i64(&json, room->id);
    if (rc == CF_OK) rc = wh_json_raw(&json, ",\"name\":");
    if (rc == CF_OK) rc = wh_json_opt_text(&json, &room->name);
    if (rc == CF_OK) rc = wh_json_raw(&json, ",\"path\":");
    if (rc == CF_OK) rc = wh_json_text(&json, room_bot_messages_path);
    if (rc == CF_OK) rc = wh_json_raw(&json, "},\"message\":{\"id\":");
    if (rc == CF_OK) rc = wh_json_i64(&json, message->id);
    if (rc == CF_OK) rc = wh_json_raw(&json, ",\"body\":{\"html\":");
    if (rc == CF_OK) {
        if (have_html) {
            rc = wh_json_text(&json, html);
        } else {
            rc = wh_json_raw(&json, "null");
        }
    }
    if (rc == CF_OK) rc = wh_json_raw(&json, ",\"plain\":");
    if (rc == CF_OK) rc = wh_json_text(&json, plain);
    if (rc == CF_OK) rc = wh_json_raw(&json, "},\"path\":");
    if (rc == CF_OK) rc = wh_json_text(&json, message_path);
    if (rc == CF_OK) rc = wh_json_raw(&json, "}}");
    if (rc == CF_OK) rc = cf_builder_freeze(&json, &json_buf);
    if (rc == CF_OK) rc = wh_str_copy(cf_buf_span(json_buf), out);
    cf_buf_release(json_buf);
    cf_builder_dispose(&json);
    return rc;
}

cf_err cf_webhook_payload(cf_db *db, const cf_webhook *webhook,
                          const cf_richtext *rich_text,
                          const cf_message *message,
                          cf_str room_bot_messages_path, cf_str message_path,
                          cf_str *out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "payload: no output");
    *out = (cf_str){0};
    if (db == NULL) return cf_db_failf(CF_INVALID, "payload: no database");
    if (webhook == NULL) return cf_db_failf(CF_INVALID, "payload: no webhook");
    if (message == NULL) return cf_db_failf(CF_INVALID, "payload: no message");
    if (rich_text == NULL) {
        return cf_db_failf(CF_INVALID, "payload: no rich text");
    }

    cf_user creator = {0};
    cf_room room = {0};
    cf_user recipient = {0};
    bool have_html = false;
    cf_str html = {0};
    cf_str plain = {0};
    cf_str mention = {0};
    cf_str trimmed = {0};
    cf_err rc = CF_OK;

    /* let creator = message.creator(conn)?; */
    rc = cf_message_creator(db, message, &creator);
    /* let room = Room::find(conn, message.room_id)?; */
    if (rc == CF_OK) rc = cf_room_find(db, message->room_id, &room);
    /* let recipient = User::find(conn, self.user_id)?; */
    if (rc == CF_OK) rc = cf_user_find(db, webhook->user_id, &recipient);
    /* let html = message.body_html(conn)?; */
    if (rc == CF_OK) rc = cf_message_body_html(db, message, &have_html, &html);
    /* let plain = without_recipient_mentions(&message.plain_text_body(...)?,
     *                                         &recipient); */
    if (rc == CF_OK) {
        rc = cf_message_plain_text_body(db, message, rich_text, &plain);
    }
    if (rc == CF_OK) {
        rc = cf_user_attachable_plain_text_representation(&recipient, &mention);
    }
    if (rc == CF_OK) rc = wh_without_mentions(plain, mention, &trimmed);
    if (rc == CF_OK) {
        rc = wh_payload_json(&creator, &room, message, have_html, html, trimmed,
                             room_bot_messages_path, message_path, out);
    }

    cf_user_dispose(&creator);
    cf_room_dispose(&room);
    cf_user_dispose(&recipient);
    cf_str_dispose(&html);
    cf_str_dispose(&plain);
    cf_str_dispose(&mention);
    cf_str_dispose(&trimmed);
    return rc;
}
