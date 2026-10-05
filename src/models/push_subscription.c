/* src/models/push_subscription.c — D01 model family push_subscription.
 *
 * Source of truth: tmp/rust-ref/crates/db/src/models/push_subscription.rs
 * (the 15 functions in contracts/model-functions.json plus the private
 * helpers EndpointUri::parse, permitted_endpoint_host, json_len and
 * truncate_json_string, which the public functions call).
 *
 * Translation rules applied here (docs/devel/implementation/00-contracts.md,
 * 02-data-auth.md D01, docs/devel/evidence/D01-db-core.md):
 *  - one fixed, source-selected SQL text per statement; nested modules keep
 *    their own enum + cf_stmt_set and share the per-connection cache;
 *  - every statement use ends with cf_db_stmt_done; row bytes are copied (and
 *    datetimes parsed) before that reset;
 *  - bound datetime parameters use the reference UTC text form
 *    (cf_db_time_to_text handles the conditional six-digit fraction);
 *  - reads return CF_NOT_FOUND where the reference `find` fails; Option reads
 *    return a found flag; outputs stay empty on failure.
 *
 * One deliberate deviation, forced by "no SQL generated at runtime"
 * (00-contracts.md): `for_mentioned_users` binds its mentionee ids as one JSON
 * array to the fixed statement
 *     ... "push_subscriptions"."user_id" IN (SELECT value FROM json_each(?))
 * where the reference formats `IN (?, ?, ...)` with placeholders(n). The
 * attribute list, filters and set semantics are identical; the pinned SQLite
 * amalgamation (3.53.4) has JSON1 built in (verified by probe). Reported in
 * docs/devel/evidence/D01-model-push_subscription.md.
 */
#include "models/push_subscription.h"

#include "db/db_internal.h"
#include "models/membership.h"
#include "models/message.h"
#include "models/room.h"
#include "models/user.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- shared model-runtime release helpers --------------------------------
 * cf_str_dispose / cf_optional_str_dispose / cf_model_errors_dispose are
 * defined once in src/models/types.c; the local ps_*_clear helpers below are
 * this module's internal call sites. */
static void ps_string_clear(cf_str *value);
static void ps_optional_clear(cf_optional_str *value);

/* --- fixed statements ---------------------------------------------------- */

enum {
    PS_STMT_FIND,
    PS_STMT_COUNT,
    PS_STMT_FOR_USER,
    PS_STMT_FIND_FOR_USER_BY_KEYS,
    PS_STMT_CREATE,
    PS_STMT_DESTROY,
    PS_STMT_IDS_BY_ENDPOINT,
    PS_STMT_EVERYTHING,
    PS_STMT_MENTIONED,
    PS_STMT__COUNT
};

static const cf_stmt_def ps_stmt_defs[PS_STMT__COUNT] = {
    [PS_STMT_FIND] = {
        "SELECT * FROM \"push_subscriptions\" WHERE \"push_subscriptions\".\"id\" = ? LIMIT 1"},
    [PS_STMT_COUNT] = {

        "SELECT COUNT(*) FROM \"push_subscriptions\""},
    [PS_STMT_FOR_USER] = {
        "SELECT * FROM \"push_subscriptions\" WHERE \"push_subscriptions\".\"user_id\" = ?"},
    [PS_STMT_FIND_FOR_USER_BY_KEYS] = {
        "SELECT * FROM \"push_subscriptions\" WHERE \"push_subscriptions\".\"user_id\" = ? AND \"push_subscriptions\".\"endpoint\" = ? AND \"push_subscriptions\".\"p256dh_key\" = ? AND \"push_subscriptions\".\"auth_key\" = ? LIMIT 1"},
    [PS_STMT_CREATE] = {
        "INSERT INTO \"push_subscriptions\" (\"auth_key\", \"created_at\", \"endpoint\", \"p256dh_key\", \"updated_at\", \"user_agent\", \"user_id\") VALUES (?, ?, ?, ?, ?, ?, ?) RETURNING \"id\""},
    [PS_STMT_DESTROY] = {
        "DELETE FROM \"push_subscriptions\" WHERE \"push_subscriptions\".\"id\" = ?"},
    [PS_STMT_IDS_BY_ENDPOINT] = {
        "SELECT \"push_subscriptions\".\"id\" FROM \"push_subscriptions\" WHERE \"push_subscriptions\".\"endpoint\" = ? AND \"push_subscriptions\".\"user_id\" = ?"},
    [PS_STMT_EVERYTHING] = {
        "SELECT \"push_subscriptions\".* FROM \"push_subscriptions\" INNER JOIN \"users\" ON \"users\".\"id\" = \"push_subscriptions\".\"user_id\" INNER JOIN \"memberships\" ON \"memberships\".\"user_id\" = \"users\".\"id\" WHERE (\"memberships\".\"connected_at\" IS NULL OR \"memberships\".\"connected_at\" < ?) AND \"memberships\".\"room_id\" = ? AND \"memberships\".\"user_id\" != ? AND \"memberships\".\"involvement\" = 'everything'"},
    [PS_STMT_MENTIONED] = {
        "SELECT \"push_subscriptions\".* FROM \"push_subscriptions\" INNER JOIN \"users\" ON \"users\".\"id\" = \"push_subscriptions\".\"user_id\" INNER JOIN \"memberships\" ON \"memberships\".\"user_id\" = \"users\".\"id\" WHERE (\"memberships\".\"connected_at\" IS NULL OR \"memberships\".\"connected_at\" < ?) AND \"memberships\".\"room_id\" = ? AND \"memberships\".\"user_id\" != ? AND \"memberships\".\"involvement\" = 'mentions' AND \"push_subscriptions\".\"user_id\" IN (SELECT value FROM json_each(?))"},
};

static const cf_stmt_set ps_stmts = {ps_stmt_defs, PS_STMT__COUNT};

/* `SELECT *` column order is the contract schema's, alphabetically after the
 * primary key: id, auth_key, created_at, endpoint, p256dh_key, updated_at,
 * user_agent, user_id (schema.sql push_subscriptions). The reference reads by
 * name; the indices below encode the same names. */
enum {
    PS_COL_ID = 0,
    PS_COL_AUTH_KEY = 1,
    PS_COL_CREATED_AT = 2,
    PS_COL_ENDPOINT = 3,
    PS_COL_P256DH_KEY = 4,
    PS_COL_UPDATED_AT = 5,
    PS_COL_USER_AGENT = 6,
    PS_COL_USER_ID = 7
};

/* Who Room::MessagePusher#build_payload will deliver to: the permitted push
 * service hosts (reference PERMITTED_ENDPOINT_HOSTS). */
static const char *const ps_permitted_hosts[] = {
    "jmt17.google.com",
    "fcm.googleapis.com",
    "updates.push.services.mozilla.com",
    "web.push.apple.com",
    "notify.windows.com",
};

/* --- small owned-text helpers ------------------------------------------- */

static void ps_string_clear(cf_str *value) {
    if (value == NULL) return;
    free(value->ptr);
    value->ptr = NULL;
    value->len = 0;
}

static void ps_optional_clear(cf_optional_str *value) {
    if (value == NULL) return;
    ps_string_clear(&value->value);
    value->present = false;
}

/* Owned copy of `len` bytes (may be 0); NUL-terminated for cf_str consumers. */
static cf_err ps_string_set(cf_str *out, const unsigned char *bytes,
                            size_t len) {
    if (len != 0 && bytes == NULL) {
        return cf_db_failf(CF_INVALID, "text has no bytes");
    }
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        return cf_db_failf(CF_NOMEM, "out of memory copying model text");
    }
    if (len != 0) memcpy(copy, bytes, len);
    copy[len] = '\0';
    out->ptr = copy;
    out->len = len;
    return CF_OK;
}

static cf_err ps_string_dup(cf_str *out, const cf_str *source) {
    return ps_string_set(out, (const unsigned char *)source->ptr, source->len);
}

/* first + middle + last, as the reference's `format!("{name}: {body}")`. */
static cf_err ps_string_concat(cf_str *out, const cf_str *first,
                               const char *middle, const cf_str *last) {
    size_t middle_len = strlen(middle);
    if (first->len > SIZE_MAX - middle_len ||
        first->len + middle_len > SIZE_MAX - last->len) {
        return cf_db_failf(CF_LIMIT, "concatenated text is too long");
    }
    size_t len = first->len + middle_len + last->len;
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        return cf_db_failf(CF_NOMEM, "out of memory concatenating text");
    }
    size_t at = 0;
    if (first->len != 0) {
        memcpy(copy, first->ptr, first->len);
        at += first->len;
    }
    if (middle_len != 0) {
        memcpy(copy + at, middle, middle_len);
        at += middle_len;
    }
    if (last->len != 0) memcpy(copy + at, last->ptr, last->len);
    copy[len] = '\0';
    out->ptr = copy;
    out->len = len;
    return CF_OK;
}

static cf_err ps_optional_copy(cf_optional_str *out,
                               const cf_optional_str *source) {
    if (!source->present) return CF_OK;
    out->present = true;
    return ps_string_dup(&out->value, &source->value);
}

static cf_span ps_span_of(const cf_str *text) {
    cf_span span = {(const unsigned char *)text->ptr, text->len};
    return span;
}

static cf_err ps_time_span(int64_t us, char buf[CF_DB_TIME_TEXT_CAP],
                           cf_span *out) {
    cf_err err = cf_db_time_to_text(us, buf);
    if (err != CF_OK) return err;
    out->ptr = (const unsigned char *)buf;
    out->len = strlen(buf);
    return CF_OK;
}

/* Copy one nullable column into an owned optional text (absent stays absent,
 * empty text stays present-and-empty). Copied before the statement resets. */
static cf_err ps_copy_column(sqlite3_stmt *stmt, int column,
                             cf_optional_str *out) {
    if (cf_stmt_column_is_null(stmt, column)) return CF_OK;
    cf_span text = cf_stmt_column_text(stmt, column);
    out->present = true;
    return ps_string_set(&out->value, text.ptr, text.len);
}

static cf_err ps_datetime_column(sqlite3_stmt *stmt, int column,
                                 int64_t *out) {
    if (cf_stmt_column_is_null(stmt, column)) {
        return cf_db_failf(CF_DB, "push_subscriptions datetime is NULL");
    }
    cf_span text = cf_stmt_column_text(stmt, column);
    if (text.ptr == NULL) {
        return cf_db_failf(CF_DB, "push_subscriptions datetime is empty");
    }
    return cf_db_time_from_text(text, out);
}

/* PushSubscription::from_row (by-name reads mapped to the schema order). */
static cf_err ps_row_from_stmt(sqlite3_stmt *stmt, cf_push_subscription *out) {
    cf_push_subscription row;
    memset(&row, 0, sizeof row);
    row.id = cf_stmt_column_i64(stmt, PS_COL_ID);
    row.user_id = cf_stmt_column_i64(stmt, PS_COL_USER_ID);
    cf_err err = ps_copy_column(stmt, PS_COL_ENDPOINT, &row.endpoint);
    if (err == CF_OK) {
        err = ps_copy_column(stmt, PS_COL_P256DH_KEY, &row.p256dh_key);
    }
    if (err == CF_OK) {
        err = ps_copy_column(stmt, PS_COL_AUTH_KEY, &row.auth_key);
    }
    if (err == CF_OK) {
        err = ps_copy_column(stmt, PS_COL_USER_AGENT, &row.user_agent);
    }
    if (err == CF_OK) {
        err = ps_datetime_column(stmt, PS_COL_CREATED_AT, &row.created_at);
    }
    if (err == CF_OK) {
        err = ps_datetime_column(stmt, PS_COL_UPDATED_AT, &row.updated_at);
    }
    if (err != CF_OK) {
        cf_push_subscription_dispose(&row);
        return err;
    }
    *out = row;
    return CF_OK;
}

static cf_err ps_vector_append(cf_push_subscription_vector *vector,
                               cf_push_subscription *item) {
    if (vector->len == vector->cap) {
        size_t cap = vector->cap == 0 ? 8 : vector->cap * 2;
        if (cap < vector->cap ||
            cap > SIZE_MAX / sizeof *vector->items) {
            return cf_db_failf(CF_NOMEM, "push subscription vector overflow");
        }
        cf_push_subscription *items =
            realloc(vector->items, cap * sizeof *items);
        if (items == NULL) {
            return cf_db_failf(CF_NOMEM,
                               "out of memory growing push subscriptions");
        }
        vector->items = items;
        vector->cap = cap;
    }
    vector->items[vector->len++] = *item;
    memset(item, 0, sizeof *item);
    return CF_OK;
}

/* Step a query to completion, appending every row. On failure the partially
 * filled vector is released, so the output stays empty as required. */
static cf_err ps_read_vector(sqlite3_stmt *stmt,
                             cf_push_subscription_vector *out) {
    for (;;) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            cf_push_subscription row;
            cf_err err = ps_row_from_stmt(stmt, &row);
            if (err != CF_OK) {
                cf_push_subscription_vector_dispose(out);
                return err;
            }
            err = ps_vector_append(out, &row);
            if (err != CF_OK) {
                cf_push_subscription_dispose(&row);
                cf_push_subscription_vector_dispose(out);
                return err;
            }
        } else if (rc == SQLITE_DONE) {
            return CF_OK;
        } else {
            cf_push_subscription_vector_dispose(out);
            return cf_db_err(rc);
        }
    }
}

/* Growable int64 list for destroy_by_endpoint's two-phase delete, matching
 * the reference (select all ids, then delete each). */
typedef struct {
    int64_t *items;
    size_t len, cap;
} ps_i64_list;

static void ps_i64_list_dispose(ps_i64_list *list) {
    free(list->items);
    memset(list, 0, sizeof *list);
}

static cf_err ps_i64_list_append(ps_i64_list *list, int64_t value) {
    if (list->len == list->cap) {
        size_t cap = list->cap == 0 ? 8 : list->cap * 2;
        if (cap < list->cap || cap > SIZE_MAX / sizeof *list->items) {
            return cf_db_failf(CF_NOMEM, "id list overflow");
        }
        int64_t *items = realloc(list->items, cap * sizeof *items);
        if (items == NULL) {
            return cf_db_failf(CF_NOMEM, "out of memory growing id list");
        }
        list->items = items;
        list->cap = cap;
    }
    list->items[list->len++] = value;
    return CF_OK;
}

/* --- UTF-8 and JSON-string lengths (Rust chars) -------------------------- */

/* Sentinel for a byte that is not valid UTF-8: Rust `&str` cannot hold it, so
 * the C side counts it as one opaque byte. */
#define PS_UTF8_INVALID 0xFFFFFFFFu

/* Decode one UTF-8 scalar, returning the bytes consumed (1 for invalid). */
static size_t ps_utf8_next(const unsigned char *s, size_t len, uint32_t *cp) {
    unsigned char b = s[0];
    if (b < 0x80) {
        *cp = b;
        return 1;
    }
    if (len >= 2 && (b & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80 && b >= 0xC2) {
        *cp = ((uint32_t)(b & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
        return 2;
    }
    if (len >= 3 && b >= 0xE0 && b <= 0xEF && (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80) {
        uint32_t v = ((uint32_t)(b & 0x0F) << 12) |
                     ((uint32_t)(s[1] & 0x3F) << 6) |
                     (uint32_t)(s[2] & 0x3F);
        if (v >= 0x800 && !(v >= 0xD800 && v <= 0xDFFF)) {
            *cp = v;
            return 3;
        }
    }
    if (len >= 4 && b >= 0xF0 && b <= 0xF4 && (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        uint32_t v = ((uint32_t)(b & 0x07) << 18) |
                     ((uint32_t)(s[1] & 0x3F) << 12) |
                     ((uint32_t)(s[2] & 0x3F) << 6) |
                     (uint32_t)(s[3] & 0x3F);
        if (v >= 0x10000 && v <= 0x10FFFF) {
            *cp = v;
            return 4;
        }
    }
    *cp = PS_UTF8_INVALID;
    return 1;
}

/* The bytes `c` takes in a JSON string: `"`, `\` and the short escapes take
 * two, other control characters six (`\u001f`). */
static size_t ps_json_len_cp(uint32_t cp) {
    if (cp == PS_UTF8_INVALID) return 1;
    switch (cp) {
    case '"':
    case '\\':
    case '\n':
    case '\r':
    case '\t':
    case 0x08:
    case 0x0C:
        return 2;
    default:
        break;
    }
    if (cp < 0x20) return 6;
    if (cp < 0x80) return 1;
    if (cp < 0x800) return 2;
    if (cp < 0x10000) return 3;
    return 4;
}

/* `truncate_json_string`: cut short with an ellipsis when the JSON length of
 * the contents exceeds max_bytes. */
static cf_err ps_truncate_json(cf_str *text, size_t max_bytes) {
    const unsigned char *s = (const unsigned char *)text->ptr;
    size_t len = text->len;
    size_t total = 0;
    bool over = false;
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        i += ps_utf8_next(s + i, len - i, &cp);
        total += ps_json_len_cp(cp);
        if (total > max_bytes) {
            over = true;
            break;
        }
    }
    if (!over) return CF_OK;

    size_t used = 3; /* '…' is three UTF-8 bytes in a JSON string */
    size_t cut = len;
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        size_t step = ps_utf8_next(s + i, len - i, &cp);
        used += ps_json_len_cp(cp);
        if (used > max_bytes) {
            cut = i;
            break;
        }
        i += step;
    }
    char *replacement = malloc(cut + 4);
    if (replacement == NULL) {
        return cf_db_failf(CF_NOMEM, "out of memory truncating payload");
    }
    if (cut != 0) memcpy(replacement, s, cut);
    memcpy(replacement + cut, "\xE2\x80\xA6", 3); /* U+2026 HORIZONTAL ELLIPSIS */
    replacement[cut + 3] = '\0';
    free(text->ptr);
    text->ptr = replacement;
    text->len = cut + 3;
    return CF_OK;
}

/* Rust char::is_whitespace (Unicode White_Space). */
static bool ps_cp_is_whitespace(uint32_t cp) {
    return cp == 0x09 || cp == 0x0A || cp == 0x0B || cp == 0x0C ||
           cp == 0x0D || cp == 0x20 || cp == 0x85 || cp == 0xA0 ||
           cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
           cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F ||
           cp == 0x3000;
}

/* endpoint.trim().is_empty(): empty or every character whitespace. */
static bool ps_all_whitespace(const unsigned char *s, size_t len) {
    if (len == 0) return true;
    if (s == NULL) return false;
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        i += ps_utf8_next(s + i, len - i, &cp);
        if (!ps_cp_is_whitespace(cp)) return false;
    }
    return true;
}

/* --- EndpointUri::parse and the permitted-host check --------------------- */

/* The parts of URI.parse(endpoint) validation looks at. All views borrow the
 * endpoint text for the duration of the call, like `uri.host`'s borrow in
 * `resolve(&uri.host)`. */
typedef struct {
    const unsigned char *scheme;
    size_t scheme_len;
    const unsigned char *host;
    size_t host_len;
    bool https;
    bool port_known; /* explicit port, or the scheme's default */
    uint32_t port;
} ps_endpoint_uri;

static bool ps_ascii_ieq(const unsigned char *s, size_t len,
                         const char *literal) {
    size_t literal_len = strlen(literal);
    if (len != literal_len) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
        if (c != (unsigned char)literal[i]) return false;
    }
    return true;
}

/* Rust `port.parse::<u16>()`: optional '+', then digits, no overflow. */
static bool ps_parse_u16(const unsigned char *s, size_t len, uint32_t *out) {
    size_t i = 0;
    if (len == 0) return false;
    if (s[0] == '+') i = 1;
    if (i >= len) return false;
    uint32_t value = 0;
    for (; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        value = value * 10 + (uint32_t)(s[i] - '0');
        if (value > UINT16_MAX) return false;
    }
    *out = value;
    return true;
}

/* `EndpointUri::parse`: false when the reference returns None. */
static bool ps_endpoint_uri_parse(cf_str endpoint, ps_endpoint_uri *out) {
    memset(out, 0, sizeof *out);
    const unsigned char *s = (const unsigned char *)endpoint.ptr;
    size_t len = endpoint.len;
    /* A by-value cf_str with len != 0 and ptr == NULL violates the type's
     * invariant (frozen types.h); reject it without dereference, the sibling
     * convention (cf_ban_parse_ipaddr ban.c:293, cf_sound_find_by_name
     * sound.c:119). This makes validate report "is not a valid URL",
     * resolved_endpoint_ip return false and create return CF_INVALID. */
    if (s == NULL || len == 0) return false;
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        i += ps_utf8_next(s + i, len - i, &cp);
        if (ps_cp_is_whitespace(cp)) return false;
    }

    size_t sep = SIZE_MAX;
    for (size_t i = 0; i + 2 < len; i++) {
        if (s[i] == ':' && s[i + 1] == '/' && s[i + 2] == '/') {
            sep = i;
            break;
        }
    }
    if (sep == SIZE_MAX) return false;

    size_t scheme_len = sep;
    const unsigned char *rest = s + sep + 3;
    size_t rest_len = len - (sep + 3);
    size_t authority_len = rest_len;
    for (size_t i = 0; i < rest_len; i++) {
        if (rest[i] == '/' || rest[i] == '?' || rest[i] == '#') {
            authority_len = i;
            break;
        }
    }
    const unsigned char *authority = rest;
    for (size_t i = authority_len; i-- > 0;) {
        if (authority[i] == '@') {
            authority += i + 1;
            authority_len -= i + 1;
            break;
        }
    }

    const unsigned char *host = authority;
    size_t host_len = authority_len;
    bool has_port = false;
    uint32_t port = 0;
    if (authority_len != 0 && authority[authority_len - 1] == ']') {
        /* IPv6 literal: the reference keeps the brackets and no port. */
    } else {
        size_t colon = SIZE_MAX;
        for (size_t i = authority_len; i-- > 0;) {
            if (authority[i] == ':') {
                colon = i;
                break;
            }
        }
        if (colon != SIZE_MAX) {
            if (!ps_parse_u16(authority + colon + 1,
                              authority_len - colon - 1, &port)) {
                return false;
            }
            has_port = true;
            host_len = colon;
        }
    }

    bool https = ps_ascii_ieq(s, scheme_len, "https");
    bool http = ps_ascii_ieq(s, scheme_len, "http");
    if (!has_port) {
        if (https) {
            port = 443;
            has_port = true;
        } else if (http) {
            port = 80;
            has_port = true;
        }
    }

    out->scheme = s;
    out->scheme_len = scheme_len;
    out->host = host;
    out->host_len = host_len;
    out->https = https;
    out->port_known = has_port;
    out->port = port;
    return true;
}

/* permitted_endpoint_host: exact match or a ".permitted" suffix, ASCII
 * case-insensitive (Rust lowercases then compares). */
static bool ps_host_permitted(const unsigned char *host, size_t host_len) {
    if (host_len == 0) return false;
    for (size_t i = 0;
         i < sizeof ps_permitted_hosts / sizeof ps_permitted_hosts[0]; i++) {
        const char *permitted = ps_permitted_hosts[i];
        size_t permitted_len = strlen(permitted);
        if (ps_ascii_ieq(host, host_len, permitted)) return true;
        /* ends_with(".permitted"), including the boundary host
         * ".permitted" itself (the reference's strict suffix check). */
        if (host_len > permitted_len &&
            host[host_len - permitted_len - 1] == '.' &&
            ps_ascii_ieq(host + host_len - permitted_len, permitted_len,
                         permitted)) {
            return true;
        }
    }
    return false;
}

/* --- record disposal ----------------------------------------------------- */

void cf_push_subscription_dispose(cf_push_subscription *subscription) {
    if (subscription == NULL) return;
    ps_optional_clear(&subscription->endpoint);
    ps_optional_clear(&subscription->p256dh_key);
    ps_optional_clear(&subscription->auth_key);
    ps_optional_clear(&subscription->user_agent);
    memset(subscription, 0, sizeof *subscription);
}

void cf_push_subscription_vector_dispose(cf_push_subscription_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len; i++) {
        cf_push_subscription_dispose(&vector->items[i]);
    }
    free(vector->items);
    memset(vector, 0, sizeof *vector);
}

void cf_push_payload_dispose(cf_push_payload *payload) {
    if (payload == NULL) return;
    ps_string_clear(&payload->title);
    ps_string_clear(&payload->body);
    ps_string_clear(&payload->path);
    memset(payload, 0, sizeof *payload);
}

/* --- new ----------------------------------------------------------------- */

cf_err cf_push_subscription_new(int64_t user_id, cf_optional_str endpoint,
                                cf_optional_str p256dh_key,
                                cf_optional_str auth_key,
                                cf_optional_str user_agent,
                                cf_push_subscription *out) {
    /* Sibling convention (cf_membership_find, cf_webhook_destroy): a missing
     * output record is rejected, never written. */
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "new: no output record");
    }
    cf_push_subscription result;
    memset(&result, 0, sizeof result);
    result.user_id = user_id;
    /* Timestamp::from_second(0) — UTC microseconds. */
    result.created_at = 0;
    result.updated_at = 0;
    cf_err err = ps_optional_copy(&result.endpoint, &endpoint);
    if (err == CF_OK) err = ps_optional_copy(&result.p256dh_key, &p256dh_key);
    if (err == CF_OK) err = ps_optional_copy(&result.auth_key, &auth_key);
    if (err == CF_OK) err = ps_optional_copy(&result.user_agent, &user_agent);
    if (err != CF_OK) {
        cf_push_subscription_dispose(&result);
        return err;
    }
    *out = result;
    return CF_OK;
}

/* --- reads --------------------------------------------------------------- */

cf_err cf_push_subscription_find(cf_db *db, int64_t id,
                                 cf_push_subscription *out) {
    /* Sibling convention (cf_membership_find, cf_room_find,
     * cf_webhook_find_by_user): the output pointer and the database are
     * required; the output record starts zeroed. */
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "find: no output record");
    }
    *out = (cf_push_subscription){0};
    if (db == NULL) return cf_db_failf(CF_INVALID, "find: no database");

    sqlite3_stmt *stmt = NULL;
    cf_err err = cf_db_stmt(db, &ps_stmts, PS_STMT_FIND, &stmt);
    if (err == CF_OK) err = cf_stmt_bind_i64(stmt, 1, id);
    if (err == CF_OK) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            err = ps_row_from_stmt(stmt, out);
        } else if (rc == SQLITE_DONE) {
            /* Error::RecordNotFound display: "Couldn't find
             * Push::Subscription" (push_subscription.rs:73). */
            err = cf_db_failf(CF_NOT_FOUND,
                              "Couldn't find Push::Subscription");
        } else {
            err = cf_db_err(rc);
        }
    }
    cf_db_stmt_done(stmt);
    return err;
}

cf_err cf_push_subscription_count(cf_db *db, int64_t *out) {
    /* Sibling convention (cf_membership_count): output pointer and database
     * are required; the output starts at zero. */
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "count: no output pointer");
    }
    *out = 0;
    if (db == NULL) return cf_db_failf(CF_INVALID, "count: no database");

    sqlite3_stmt *stmt = NULL;
    cf_err err = cf_db_stmt(db, &ps_stmts, PS_STMT_COUNT, &stmt);
    if (err == CF_OK) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            *out = cf_stmt_column_i64(stmt, 0);
        } else if (rc == SQLITE_DONE) {
            err = cf_db_failf(CF_DB, "count returned no row");
        } else {
            err = cf_db_err(rc);
        }
    }
    cf_db_stmt_done(stmt);
    return err;
}

cf_err cf_push_subscription_for_user(cf_db *db, int64_t user_id,
                                     cf_push_subscription_vector *out) {
    if (db == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "for_user: no output");
    }
    /* Sibling convention (cf_ban_for_user): outputs start empty. */
    *out = (cf_push_subscription_vector){NULL, 0, 0};
    sqlite3_stmt *stmt = NULL;
    cf_err err = cf_db_stmt(db, &ps_stmts, PS_STMT_FOR_USER, &stmt);
    if (err == CF_OK) err = cf_stmt_bind_i64(stmt, 1, user_id);
    if (err == CF_OK) err = ps_read_vector(stmt, out);
    cf_db_stmt_done(stmt);
    return err;
}

cf_err cf_push_subscription_find_for_user_by_keys(
    cf_db *db, int64_t user_id, cf_str endpoint, cf_str p256dh_key,
    cf_str auth_key, bool *found, cf_push_subscription *out) {
    /* Sibling convention (cf_webhook_find_by_user, cf_room_find_by_id): the
     * found flag and the output record are required; both start cleared and
     * the database is checked after them. */
    if (found == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "find_for_user_by_keys: no output");
    }
    *found = false;
    *out = (cf_push_subscription){0};
    if (db == NULL) {
        return cf_db_failf(CF_INVALID, "find_for_user_by_keys: no database");
    }
    sqlite3_stmt *stmt = NULL;
    cf_err err = cf_db_stmt(db, &ps_stmts, PS_STMT_FIND_FOR_USER_BY_KEYS,
                            &stmt);
    if (err == CF_OK) err = cf_stmt_bind_i64(stmt, 1, user_id);
    if (err == CF_OK) err = cf_stmt_bind_text(stmt, 2, ps_span_of(&endpoint));
    if (err == CF_OK) err = cf_stmt_bind_text(stmt, 3, ps_span_of(&p256dh_key));
    if (err == CF_OK) err = cf_stmt_bind_text(stmt, 4, ps_span_of(&auth_key));
    if (err == CF_OK) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            err = ps_row_from_stmt(stmt, out);
            if (err == CF_OK) *found = true;
        } else if (rc != SQLITE_DONE) {
            err = cf_db_err(rc);
        }
    }
    cf_db_stmt_done(stmt);
    return err;
}

/* --- writes -------------------------------------------------------------- */

cf_err cf_push_subscription_create(cf_tx *tx,
                                   const cf_push_subscription *subscription,
                                   cf_push_resolve_fn resolve,
                                   void *resolve_arg,
                                   cf_push_subscription *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "create: no output record");
    }
    *out = (cf_push_subscription){0};
    if (tx == NULL || subscription == NULL) {
        return cf_db_failf(CF_INVALID, "create: no subscription");
    }
    cf_model_errors errors;
    memset(&errors, 0, sizeof errors);
    cf_err err = cf_push_subscription_validate(subscription, resolve,
                                               resolve_arg, &errors);
    if (err == CF_OK && errors.len != 0) err = CF_INVALID;
    for (size_t i = 0; i < errors.len; i++) {
        ps_string_clear(&errors.items[i].field);
        ps_string_clear(&errors.items[i].message);
    }
    free(errors.items);
    if (err != CF_OK) return err;

    /* Rust: tx.now(). cf_tx exposes no clock in the frozen contract (only
     * cf_tx_db), so this family — like the other landed D01 families — reads
     * the shared process clock (cf_now_us, test-injectable per F01), which is
     * the same Time.current value. Captured once, where the reference is. */
    int64_t now_us = cf_now_us(NULL);
    char timebuf[CF_DB_TIME_TEXT_CAP];
    cf_span time_text;
    err = ps_time_span(now_us, timebuf, &time_text);

    sqlite3_stmt *stmt = NULL;
    if (err == CF_OK) {
        err = cf_db_stmt(cf_tx_db(tx), &ps_stmts, PS_STMT_CREATE, &stmt);
    }
    if (err == CF_OK) {
        err = cf_stmt_bind_opt_text(stmt, 1, subscription->auth_key.present,
                                    ps_span_of(&subscription->auth_key.value));
    }
    if (err == CF_OK) err = cf_stmt_bind_text(stmt, 2, time_text);
    if (err == CF_OK) {
        err = cf_stmt_bind_opt_text(stmt, 3, subscription->endpoint.present,
                                    ps_span_of(&subscription->endpoint.value));
    }
    if (err == CF_OK) {
        err = cf_stmt_bind_opt_text(stmt, 4, subscription->p256dh_key.present,
                                    ps_span_of(&subscription->p256dh_key.value));
    }
    if (err == CF_OK) err = cf_stmt_bind_text(stmt, 5, time_text);
    if (err == CF_OK) {
        err = cf_stmt_bind_opt_text(stmt, 6, subscription->user_agent.present,
                                    ps_span_of(&subscription->user_agent.value));
    }
    if (err == CF_OK) err = cf_stmt_bind_i64(stmt, 7, subscription->user_id);
    int64_t id = 0;
    if (err == CF_OK) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            id = cf_stmt_column_i64(stmt, 0);
        } else if (rc == SQLITE_DONE) {
            err = cf_db_failf(CF_DB, "insert returned no id");
        } else {
            err = cf_db_err(rc);
        }
    }
    cf_db_stmt_done(stmt);
    if (err != CF_OK) return err;

    cf_push_subscription created;
    memset(&created, 0, sizeof created);
    created.id = id;
    created.user_id = subscription->user_id;
    created.created_at = now_us;
    created.updated_at = now_us;
    err = ps_optional_copy(&created.endpoint, &subscription->endpoint);
    if (err == CF_OK) {
        err = ps_optional_copy(&created.p256dh_key, &subscription->p256dh_key);
    }
    if (err == CF_OK) {
        err = ps_optional_copy(&created.auth_key, &subscription->auth_key);
    }
    if (err == CF_OK) {
        err = ps_optional_copy(&created.user_agent, &subscription->user_agent);
    }
    if (err != CF_OK) {
        cf_push_subscription_dispose(&created);
        return err;
    }
    *out = created;
    return CF_OK;
}

cf_err cf_push_subscription_destroy(cf_tx *tx,
                                    const cf_push_subscription *subscription) {
    if (tx == NULL || subscription == NULL) {
        return cf_db_failf(CF_INVALID, "destroy: no subscription");
    }
    sqlite3_stmt *stmt = NULL;
    cf_err err = cf_db_stmt(cf_tx_db(tx), &ps_stmts, PS_STMT_DESTROY, &stmt);
    if (err == CF_OK) err = cf_stmt_bind_i64(stmt, 1, subscription->id);
    if (err == CF_OK) {
        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) err = cf_db_err(rc);
    }
    cf_db_stmt_done(stmt);
    return err;
}

cf_err cf_push_subscription_destroy_by_endpoint(cf_tx *tx, int64_t user_id,
                                                cf_str endpoint) {
    /* Sibling convention (cf_push_subscription_destroy, cf_webhook_destroy):
     * a NULL transaction is rejected before cf_tx_db. */
    if (tx == NULL) {
        return cf_db_failf(CF_INVALID, "destroy_by_endpoint: no transaction");
    }
    cf_db *db = cf_tx_db(tx);
    ps_i64_list ids;
    memset(&ids, 0, sizeof ids);
    sqlite3_stmt *stmt = NULL;
    cf_err err = cf_db_stmt(db, &ps_stmts, PS_STMT_IDS_BY_ENDPOINT, &stmt);
    if (err == CF_OK) err = cf_stmt_bind_text(stmt, 1, ps_span_of(&endpoint));
    if (err == CF_OK) err = cf_stmt_bind_i64(stmt, 2, user_id);
    if (err == CF_OK) {
        for (;;) {
            int rc = sqlite3_step(stmt);
            if (rc == SQLITE_ROW) {
                err = ps_i64_list_append(&ids, cf_stmt_column_i64(stmt, 0));
                if (err != CF_OK) break;
            } else if (rc == SQLITE_DONE) {
                break;
            } else {
                err = cf_db_err(rc);
                break;
            }
        }
    }
    cf_db_stmt_done(stmt);

    for (size_t i = 0; err == CF_OK && i < ids.len; i++) {
        sqlite3_stmt *del = NULL;
        err = cf_db_stmt(db, &ps_stmts, PS_STMT_DESTROY, &del);
        if (err == CF_OK) err = cf_stmt_bind_i64(del, 1, ids.items[i]);
        if (err == CF_OK) {
            int rc = sqlite3_step(del);
            if (rc != SQLITE_DONE && rc != SQLITE_ROW) err = cf_db_err(rc);
        }
        cf_db_stmt_done(del);
    }
    ps_i64_list_dispose(&ids);
    return err;
}

/* --- badge --------------------------------------------------------------- */

cf_err cf_push_subscription_badge(cf_db *db,
                                  const cf_push_subscription *subscription,
                                  int64_t *out) {
    /* Sibling convention (cf_membership_unread_count,
     * cf_webhook_destroy): output pointer, database and borrowed record are
     * all required; the output starts at zero. */
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "badge: no output pointer");
    }
    *out = 0;
    if (db == NULL) return cf_db_failf(CF_INVALID, "badge: no database");
    if (subscription == NULL) {
        return cf_db_failf(CF_INVALID, "badge: no subscription");
    }
    /* `user.memberships.unread.count` (Membership::unread_count). */
    return cf_membership_unread_count(db, subscription->user_id, out);
}

/* --- validation ---------------------------------------------------------- */

static cf_err ps_errors_add(cf_model_errors *errors, const char *field,
                            const char *message) {
    if (errors->len == errors->cap) {
        size_t cap = errors->cap == 0 ? 4 : errors->cap * 2;
        if (cap < errors->cap || cap > SIZE_MAX / sizeof *errors->items) {
            return cf_db_failf(CF_NOMEM, "error list overflow");
        }
        cf_model_error *items = realloc(errors->items, cap * sizeof *items);
        if (items == NULL) {
            return cf_db_failf(CF_NOMEM, "out of memory growing errors");
        }
        errors->items = items;
        errors->cap = cap;
    }
    cf_model_error item;
    memset(&item, 0, sizeof item);
    cf_err err = ps_string_set(&item.field, (const unsigned char *)field,
                               strlen(field));
    if (err == CF_OK) {
        err = ps_string_set(&item.message, (const unsigned char *)message,
                            strlen(message));
    }
    if (err != CF_OK) {
        ps_string_clear(&item.field);
        ps_string_clear(&item.message);
        return err;
    }
    errors->items[errors->len++] = item;
    return CF_OK;
}

bool cf_push_subscription_resolved_endpoint_ip(
    const cf_push_subscription *subscription, cf_push_resolve_fn resolve,
    void *resolve_arg, cf_optional_str *out) {
    if (out == NULL) return false;
    ps_optional_clear(out);
    /* A NULL record has no endpoint, so it cannot resolve; the output stays
     * cleared, matching the absent-endpoint path below. */
    if (subscription == NULL || !subscription->endpoint.present) return false;

    ps_endpoint_uri uri;
    if (!ps_endpoint_uri_parse(subscription->endpoint.value, &uri)) {
        return false;
    }
    if (!uri.https || !uri.port_known || uri.port != 443 ||
        !ps_host_permitted(uri.host, uri.host_len) || resolve == NULL) {
        return false;
    }
    cf_str host = {(char *)uri.host, uri.host_len};
    cf_optional_str resolved = resolve(resolve_arg, host);
    if (!resolved.present) {
        ps_optional_clear(&resolved);
        return false;
    }
    *out = resolved;
    return true;
}

cf_err cf_push_subscription_validate(const cf_push_subscription *subscription,
                                     cf_push_resolve_fn resolve,
                                     void *resolve_arg,
                                     cf_model_errors *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "validate: no output errors");
    }
    memset(out, 0, sizeof *out);
    if (subscription == NULL) {
        return cf_db_failf(CF_INVALID, "validate: no subscription");
    }
    cf_model_errors errors;
    memset(&errors, 0, sizeof errors);
    cf_str endpoint = {NULL, 0};
    if (subscription->endpoint.present) {
        endpoint = subscription->endpoint.value;
    } else {
        /* as_deref().unwrap_or("") */
        endpoint.ptr = NULL;
        endpoint.len = 0;
    }

    cf_err err = CF_OK;
    if (ps_all_whitespace((const unsigned char *)endpoint.ptr, endpoint.len)) {
        err = ps_errors_add(&errors, "endpoint", "can't be blank");
    }

    ps_endpoint_uri uri;
    bool parsed = false;
    if (err == CF_OK) {
        parsed = ps_endpoint_uri_parse(endpoint, &uri);
    }
    if (err == CF_OK) {
        if (!parsed) {
            err = ps_errors_add(&errors, "endpoint", "is not a valid URL");
        } else {
            const char *message = NULL;
            if (!uri.https) {
                message = "must use HTTPS";
            } else if (!uri.port_known || uri.port != 443) {
                message = "must use the default HTTPS port";
            } else if (!ps_host_permitted(uri.host, uri.host_len)) {
                message = "is not a permitted push service";
            } else {
                cf_optional_str ip;
                memset(&ip, 0, sizeof ip);
                if (!cf_push_subscription_resolved_endpoint_ip(
                        subscription, resolve, resolve_arg, &ip)) {
                    message = "resolves to a private or invalid IP address";
                } else {
                    ps_optional_clear(&ip);
                }
            }
            if (message != NULL) {
                err = ps_errors_add(&errors, "endpoint", message);
            }
        }
    }

    if (err != CF_OK) {
        for (size_t i = 0; i < errors.len; i++) {
            ps_string_clear(&errors.items[i].field);
            ps_string_clear(&errors.items[i].message);
        }
        free(errors.items);
        return err;
    }
    *out = errors;
    return CF_OK;
}

/* --- payload ------------------------------------------------------------- */

cf_err cf_push_subscription_payload_for(cf_db *db,
                                        const cf_richtext *rich_text,
                                        const cf_room *room,
                                        const cf_message *message,
                                        cf_push_payload *out) {
    /* Sibling convention (cf_webhook_payload): output pointer, database and
     * borrowed records are required and the output starts zeroed. A NULL
     * rich_text is not guarded: it means the message has no rich-text record
     * (the reference's None) and cf_message_plain_text_body accepts it. */
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "payload_for: no output");
    }
    memset(out, 0, sizeof *out);
    if (db == NULL) return cf_db_failf(CF_INVALID, "payload_for: no database");
    if (room == NULL) {
        return cf_db_failf(CF_INVALID, "payload_for: no room");
    }
    if (message == NULL) {
        return cf_db_failf(CF_INVALID, "payload_for: no message");
    }

    cf_user creator;
    memset(&creator, 0, sizeof creator);
    cf_str body;
    memset(&body, 0, sizeof body);
    cf_push_payload payload;
    memset(&payload, 0, sizeof payload);

    cf_err err = cf_message_creator(db, message, &creator);
    if (err == CF_OK) {
        err = cf_message_plain_text_body(db, message, rich_text, &body);
    }
    if (err == CF_OK) {
        if (cf_room_direct(room)) {
            /* direct rooms show the sender */
            err = ps_string_dup(&payload.title, &creator.name);
            if (err == CF_OK) {
                payload.body = body; /* move */
                memset(&body, 0, sizeof body);
            }
        } else {
            if (room->name.present) {
                err = ps_string_dup(&payload.title, &room->name.value);
            } else {
                err = ps_string_set(&payload.title, NULL, 0);
            }
            if (err == CF_OK) {
                /* others the room and "Sender: body" */
                err = ps_string_concat(&payload.body, &creator.name, ": ",
                                       &body);
            }
        }
    }
    if (err == CF_OK) {
        char path[32];
        int written = snprintf(path, sizeof path, "/rooms/%lld",
                               (long long)room->id);
        if (written < 0 || (size_t)written >= sizeof path) {
            err = cf_db_failf(CF_INTERNAL, "room path overflow");
        } else {
            err = ps_string_set(&payload.path, (const unsigned char *)path,
                                (size_t)written);
        }
    }
    if (err == CF_OK) {
        err = ps_truncate_json(&payload.title,
                               CF_PUSH_SUBSCRIPTION_MAX_PAYLOAD_TITLE_BYTES);
    }
    if (err == CF_OK) {
        err = ps_truncate_json(&payload.body,
                               CF_PUSH_SUBSCRIPTION_MAX_PAYLOAD_BODY_BYTES);
    }

    ps_string_clear(&body);
    cf_user_dispose(&creator);
    if (err != CF_OK) {
        cf_push_payload_dispose(&payload);
        return err;
    }
    *out = payload;
    return CF_OK;
}

/* --- delivery selection -------------------------------------------------- */

cf_err cf_push_subscription_for_users_involved_in_everything(
    cf_db *db, int64_t room_id, int64_t creator_id, int64_t now_us,
    cf_push_subscription_vector *out) {
    if (db == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "everything: no output");
    }
    /* Sibling convention (cf_ban_for_user): outputs start empty. */
    *out = (cf_push_subscription_vector){NULL, 0, 0};
    int64_t cutoff = cf_membership_connection_cutoff(now_us);
    char timebuf[CF_DB_TIME_TEXT_CAP];
    cf_span cutoff_text;
    cf_err err = ps_time_span(cutoff, timebuf, &cutoff_text);
    sqlite3_stmt *stmt = NULL;
    if (err == CF_OK) {
        err = cf_db_stmt(db, &ps_stmts, PS_STMT_EVERYTHING, &stmt);
    }
    if (err == CF_OK) err = cf_stmt_bind_text(stmt, 1, cutoff_text);
    if (err == CF_OK) err = cf_stmt_bind_i64(stmt, 2, room_id);
    if (err == CF_OK) err = cf_stmt_bind_i64(stmt, 3, creator_id);
    if (err == CF_OK) err = ps_read_vector(stmt, out);
    cf_db_stmt_done(stmt);
    return err;
}

/* One JSON array text ("[1,2,3]") for the fixed IN (json_each(?)) statement;
 * the reference assembles a placeholder list instead. See the file header. */
static cf_err ps_mentionees_json(const int64_t *ids, size_t len, cf_str *out) {
    if (len > (SIZE_MAX - 3) / 21) {
        return cf_db_failf(CF_LIMIT, "too many mentionees");
    }
    size_t cap = len * 21 + 3;
    char *text = malloc(cap);
    if (text == NULL) {
        return cf_db_failf(CF_NOMEM, "out of memory building mentionee ids");
    }
    size_t at = 0;
    text[at++] = '[';
    for (size_t i = 0; i < len; i++) {
        if (i != 0) text[at++] = ',';
        int written = snprintf(text + at, cap - at, "%lld",
                               (long long)ids[i]);
        if (written < 0 || (size_t)written >= cap - at) {
            free(text);
            return cf_db_failf(CF_INTERNAL, "mentionee id overflow");
        }
        at += (size_t)written;
    }
    text[at++] = ']';
    text[at] = '\0';
    out->ptr = text;
    out->len = at;
    return CF_OK;
}

cf_err cf_push_subscription_for_mentioned_users(
    cf_db *db, int64_t room_id, int64_t creator_id,
    const int64_t *mentionee_ids, size_t mentionee_ids_len, int64_t now_us,
    cf_push_subscription_vector *out) {
    if (db == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "mentioned: no output");
    }
    /* Sibling convention (cf_ban_for_user): outputs start empty. */
    *out = (cf_push_subscription_vector){NULL, 0, 0};
    if (mentionee_ids_len == 0) {
        /* The reference returns Ok(Vec::new()) without querying. */
        return CF_OK;
    }
    if (mentionee_ids == NULL) {
        return cf_db_failf(CF_INVALID, "mentionee list has no ids");
    }

    int64_t cutoff = cf_membership_connection_cutoff(now_us);
    char timebuf[CF_DB_TIME_TEXT_CAP];
    cf_span cutoff_text;
    cf_err err = ps_time_span(cutoff, timebuf, &cutoff_text);

    cf_str ids_json;
    memset(&ids_json, 0, sizeof ids_json);
    if (err == CF_OK) {
        err = ps_mentionees_json(mentionee_ids, mentionee_ids_len, &ids_json);
    }

    sqlite3_stmt *stmt = NULL;
    if (err == CF_OK) {
        err = cf_db_stmt(db, &ps_stmts, PS_STMT_MENTIONED, &stmt);
    }
    if (err == CF_OK) err = cf_stmt_bind_text(stmt, 1, cutoff_text);
    if (err == CF_OK) err = cf_stmt_bind_i64(stmt, 2, room_id);
    if (err == CF_OK) err = cf_stmt_bind_i64(stmt, 3, creator_id);
    if (err == CF_OK) err = cf_stmt_bind_text(stmt, 4, ps_span_of(&ids_json));
    if (err == CF_OK) err = ps_read_vector(stmt, out);
    cf_db_stmt_done(stmt);
    ps_string_clear(&ids_json);
    return err;
}

cf_err cf_push_subscription_pushes_for(cf_db *db,
                                       const cf_richtext *rich_text,
                                       const cf_message *message,
                                       int64_t now_us,
                                       cf_push_payload *out_payload,
                                       cf_push_subscription_vector *out_everything,
                                       cf_push_subscription_vector *out_mentions) {
    if (out_payload == NULL || out_everything == NULL ||
        out_mentions == NULL) {
        return cf_db_failf(CF_INVALID, "pushes_for: no output");
    }
    /* Sibling convention (cf_ban_for_user): outputs start empty and stay
     * empty on failure. */
    memset(out_payload, 0, sizeof *out_payload);
    *out_everything = (cf_push_subscription_vector){NULL, 0, 0};
    *out_mentions = (cf_push_subscription_vector){NULL, 0, 0};
    if (db == NULL || message == NULL) {
        return cf_db_failf(CF_INVALID, "pushes_for: no message");
    }
    cf_room room;
    memset(&room, 0, sizeof room);
    cf_err err = cf_room_find(db, message->room_id, &room);
    if (err != CF_OK) return err;

    cf_push_payload payload;
    memset(&payload, 0, sizeof payload);
    cf_push_subscription_vector everything;
    memset(&everything, 0, sizeof everything);
    cf_push_subscription_vector mentions;
    memset(&mentions, 0, sizeof mentions);
    cf_user_vector mentionees;
    memset(&mentionees, 0, sizeof mentionees);
    ps_i64_list ids;
    memset(&ids, 0, sizeof ids);

    err = cf_push_subscription_payload_for(db, rich_text, &room, message,
                                           &payload);
    if (err == CF_OK) {
        err = cf_push_subscription_for_users_involved_in_everything(
            db, room.id, message->creator_id, now_us, &everything);
    }
    if (err == CF_OK) {
        err = cf_message_mentionees(db, message, rich_text, &mentionees);
    }
    for (size_t i = 0; err == CF_OK && i < mentionees.len; i++) {
        err = ps_i64_list_append(&ids, mentionees.items[i].id);
    }
    if (err == CF_OK) {
        err = cf_push_subscription_for_mentioned_users(
            db, room.id, message->creator_id, ids.items, ids.len, now_us,
            &mentions);
    }

    cf_user_vector_dispose(&mentionees);
    ps_i64_list_dispose(&ids);
    cf_room_dispose(&room);
    if (err != CF_OK) {
        cf_push_payload_dispose(&payload);
        cf_push_subscription_vector_dispose(&everything);
        cf_push_subscription_vector_dispose(&mentions);
        return err;
    }
    *out_payload = payload;
    *out_everything = everything;
    *out_mentions = mentions;
    return CF_OK;
}
