/* src/models/ban.c — D01 model family "ban".
 *
 * Source of truth: tmp/rust-ref/crates/db/src/models/ban.rs (pinned).
 * Translations, function by function:
 *  - cf_ban_banned   <- Ban::banned          (sql::exists, fixed SQL)
 *  - cf_ban_for_user <- Ban::for_user        (query_all, fixed SQL, no ORDER BY)
 *  - cf_ban_create   <- Ban::create          (validate, then INSERT ... RETURNING id)
 *  - cf_ban_validate <- Ban::validate        (parse_ipaddr + private predicates)
 *
 * Reference-mapping notes:
 *  - Script datetime binding: Timestamp::to_sql writes the UTC SQL text; the C
 *    code formats cf_now_us() with cf_db_time_to_text (D01 db-core helper).
 *  - Rust `tx.now()` has no cf_tx accessor in the frozen contract; the port has
 *    one process clock (src/core/clock.c, cf_now_us ignores its app argument),
 *    which is the value tests inject through core/testclock.h.  Equivalent to
 *    the reference TestClock when fixed.
 *  - parse_ipaddr replicates the Rust body exactly, including Ruby's
 *    IPv4-mapped IPv6 predicates, which only test the 0xffff bits (bits 32..47).
 */
#include "models/ban.h"

#include "db/db_internal.h"

#include <stdlib.h>
#include <string.h>

/* Shared release helpers (cf_str_dispose / cf_model_errors_dispose) are
 * defined once in src/models/types.c. */

/* Fixed statements of this module (02-data-auth.md D01: one cache per
 * connection, no SQL assembled at runtime).  Text is the pinned source text. */
enum {
    CF_BAN_STMT_BANNED = 0,
    CF_BAN_STMT_FOR_USER,
    CF_BAN_STMT_INSERT,
    CF_BAN_STMT_COUNT
};

static const cf_stmt_def cf_ban_stmt_defs[CF_BAN_STMT_COUNT] = {
    {"SELECT 1 AS one FROM \"bans\" WHERE \"bans\".\"ip_address\" = ? LIMIT 1"},
    {"SELECT * FROM \"bans\" WHERE \"bans\".\"user_id\" = ?"},
    {"INSERT INTO \"bans\" (\"created_at\", \"ip_address\", \"updated_at\", \"user_id\") "
     "VALUES (?, ?, ?, ?) RETURNING \"id\""},
};

static const cf_stmt_set cf_ban_stmt_set = {
    cf_ban_stmt_defs,
    CF_BAN_STMT_COUNT,
};

/* Column indexes of "SELECT *" against the frozen schema order
 * (id, created_at, ip_address, updated_at, user_id). */
enum {
    CF_BAN_COL_ID = 0,
    CF_BAN_COL_CREATED_AT,
    CF_BAN_COL_IP_ADDRESS,
    CF_BAN_COL_UPDATED_AT,
    CF_BAN_COL_USER_ID,
};

/* ---- disposal ----------------------------------------------------------- */

void cf_ban_dispose(cf_ban *ban) {
    if (ban == NULL) return;
    cf_str_dispose(&ban->ip_address);
    ban->id = 0;
    ban->user_id = 0;
    ban->created_at = 0;
    ban->updated_at = 0;
}

void cf_ban_vector_dispose(cf_ban_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len; i++) {
        cf_ban_dispose(&vector->items[i]);
    }
    free(vector->items);
    vector->items = NULL;
    vector->len = 0;
    vector->cap = 0;
}

/* ---- small helpers ------------------------------------------------------ */

static cf_err cf_ban_strdup_span(cf_span text, cf_str *out) {
    out->ptr = NULL;
    out->len = 0;
    if (text.len == 0) return CF_OK; /* empty state {NULL, 0} */
    if (text.ptr == NULL) return CF_INVALID;
    char *bytes = malloc(text.len + 1);
    if (bytes == NULL) return CF_NOMEM;
    memcpy(bytes, text.ptr, text.len);
    bytes[text.len] = '\0';
    out->ptr = bytes;
    out->len = text.len;
    return CF_OK;
}

static cf_err cf_ban_strdup_cstr(const char *text, cf_str *out) {
    return cf_ban_strdup_span(
        (cf_span){(const unsigned char *)text, strlen(text)}, out);
}

static cf_err cf_ban_vector_push(cf_ban_vector *vector, cf_ban row) {
    if (vector->len == vector->cap) {
        size_t cap = vector->cap == 0 ? 4 : vector->cap * 2;
        if (cap < vector->cap || cap > SIZE_MAX / sizeof(cf_ban)) {
            return CF_NOMEM;
        }
        cf_ban *items = realloc(vector->items, cap * sizeof *items);
        if (items == NULL) return CF_NOMEM;
        vector->items = items;
        vector->cap = cap;
    }
    vector->items[vector->len++] = row;
    return CF_OK;
}

/* Parse one "SELECT *" row; `ban` must be zero-initialized.  On failure the
 * caller disposes the partial record. */
static cf_err cf_ban_row(sqlite3_stmt *stmt, cf_ban *ban) {
    ban->id = cf_stmt_column_i64(stmt, CF_BAN_COL_ID);
    ban->user_id = cf_stmt_column_i64(stmt, CF_BAN_COL_USER_ID);
    cf_err rc = cf_db_time_from_text(cf_stmt_column_text(stmt, CF_BAN_COL_CREATED_AT),
                                     &ban->created_at);
    if (rc != CF_OK) return rc;
    rc = cf_db_time_from_text(cf_stmt_column_text(stmt, CF_BAN_COL_UPDATED_AT),
                              &ban->updated_at);
    if (rc != CF_OK) return rc;

    cf_buf *text = NULL;
    rc = cf_stmt_column_copy_text(stmt, CF_BAN_COL_IP_ADDRESS, &text);
    if (rc != CF_OK) return rc;
    if (text == NULL) {
        return cf_db_failf(CF_DB, "ban row has a NULL ip_address");
    }
    rc = cf_ban_strdup_span(cf_buf_span(text), &ban->ip_address);
    cf_buf_release(text);
    return rc;
}

/* ---- validations: Rust parse_ipaddr and the IPAddr predicates ----------- */

typedef struct {
    bool v6;
    uint32_t v4;   /* valid when v6 == false */
    uint8_t v6_bytes[16]; /* valid when v6 == true */
} cf_ban_ip;

static bool cf_ban_is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

static bool cf_ban_is_hex(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static unsigned cf_ban_hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return (unsigned)(c - '0');
    if (c >= 'a' && c <= 'f') return (unsigned)(c - 'a') + 10;
    return (unsigned)(c - 'A') + 10;
}

/* Rust u32::from_str: optional '+', then decimal digits (leading zeros are
 * accepted), no separators; overflow fails. */
static bool cf_ban_parse_u32(cf_span text, uint32_t *out) {
    if (text.ptr == NULL) return false;
    size_t i = 0;
    if (i < text.len && text.ptr[i] == '+') i++;
    if (i >= text.len) return false;
    uint64_t value = 0;
    for (; i < text.len; i++) {
        if (!cf_ban_is_digit(text.ptr[i])) return false;
        value = value * 10 + (uint64_t)(text.ptr[i] - '0');
        if (value > UINT32_MAX) return false;
    }
    *out = (uint32_t)value;
    return true;
}

/* Rust std Ipv4Addr::from_str (edition 2024): four decimal octets, leading
 * zeros rejected. */
static bool cf_ban_parse_ipv4(cf_span text, uint32_t *out) {
    if (text.ptr == NULL) return false;
    uint32_t value = 0;
    size_t parts = 0;
    size_t i = 0;
    while (true) {
        size_t start = i;
        while (i < text.len && text.ptr[i] != '.') i++;
        size_t len = i - start;
        if (len == 0 || len > 3) return false;
        if (len > 1 && text.ptr[start] == '0') return false;
        unsigned octet = 0;
        for (size_t k = 0; k < len; k++) {
            if (!cf_ban_is_digit(text.ptr[start + k])) return false;
            octet = octet * 10 + (unsigned)(text.ptr[start + k] - '0');
        }
        if (octet > 255) return false;
        value = (value << 8) | (uint32_t)octet;
        parts++;
        if (i == text.len) break;
        i++; /* skip '.'; a trailing dot yields an empty part next round */
    }
    if (parts != 4) return false;
    *out = value;
    return true;
}

static void cf_ban_v6_pack(const uint16_t groups[8], uint8_t out[16]) {
    for (size_t k = 0; k < 8; k++) {
        out[2 * k] = (uint8_t)(groups[k] >> 8);
        out[2 * k + 1] = (uint8_t)(groups[k] & 0xff);
    }
}

/* Rust std Ipv6Addr::from_str: 1..4 hex digits per group, at most one "::",
 * exactly 8 units (an embedded IPv4 tail is two units), "::" must compress at
 * least one unit. */
static bool cf_ban_parse_ipv6(cf_span text, uint8_t out[16]) {
    if (text.ptr == NULL || text.len == 0) return false;
    uint16_t groups[8];
    size_t count = 0;
    size_t compress = SIZE_MAX;
    size_t i = 0;

    if (text.len >= 2 && text.ptr[0] == ':' && text.ptr[1] == ':') {
        compress = 0;
        i = 2;
    } else if (text.ptr[0] == ':') {
        return false;
    }

    while (i < text.len) {
        size_t start = i;
        while (i < text.len && text.ptr[i] != ':') i++;
        cf_span piece = {text.ptr + start, i - start};
        if (piece.len == 0) return false;
        bool last = i == text.len;

        bool has_dot = false;
        for (size_t k = 0; k < piece.len; k++) {
            if (piece.ptr[k] == '.') {
                has_dot = true;
                break;
            }
        }
        if (has_dot) {
            /* Embedded IPv4 is allowed only as the final 32 bits. */
            if (!last) return false;
            uint32_t v4;
            if (!cf_ban_parse_ipv4(piece, &v4)) return false;
            if (count + 2 > 8) return false;
            groups[count++] = (uint16_t)(v4 >> 16);
            groups[count++] = (uint16_t)(v4 & 0xffff);
        } else {
            if (piece.len > 4) return false;
            uint16_t value = 0;
            for (size_t k = 0; k < piece.len; k++) {
                if (!cf_ban_is_hex(piece.ptr[k])) return false;
                value = (uint16_t)((value << 4) | cf_ban_hex_value(piece.ptr[k]));
            }
            if (count >= 8) return false;
            groups[count++] = value;
        }

        if (last) break;
        i++; /* skip ':' */
        if (i == text.len) return false; /* trailing single ':' */
        if (text.ptr[i] == ':') {
            if (compress != SIZE_MAX) return false; /* a second "::" */
            compress = count;
            i++;
            if (i == text.len) break; /* trailing "::" */
        }
    }

    if (compress == SIZE_MAX) {
        if (count != 8) return false;
        cf_ban_v6_pack(groups, out);
        return true;
    }
    if (count >= 8) return false; /* "::" must compress at least one unit */
    uint16_t packed[8] = {0};
    for (size_t k = 0; k < compress; k++) packed[k] = groups[k];
    for (size_t k = compress; k < count; k++) {
        packed[k + (8 - count)] = groups[k];
    }
    cf_ban_v6_pack(packed, out);
    return true;
}

static bool cf_ban_parse_ipaddr(cf_span text, cf_ban_ip *out) {
    if (text.ptr == NULL) return false;

    cf_span address = text;
    bool has_prefix = false;
    uint32_t prefix = 0;
    for (size_t i = 0; i < text.len; i++) {
        if (text.ptr[i] == '/') {
            address = (cf_span){text.ptr, i};
            has_prefix = true;
            if (!cf_ban_parse_u32((cf_span){text.ptr + i + 1, text.len - i - 1},
                                  &prefix)) {
                return false;
            }
            break;
        }
    }

    /* Rust: strip_prefix('[').and_then(strip_suffix(']')).unwrap_or(address) */
    if (address.len >= 2 && address.ptr[0] == '[' &&
        address.ptr[address.len - 1] == ']') {
        address.ptr++;
        address.len -= 2;
    }

    uint32_t v4;
    if (cf_ban_parse_ipv4(address, &v4)) {
        if (has_prefix) {
            if (prefix > 32) return false;
            uint32_t mask = 0;
            if (prefix != 0) {
                mask = prefix == 32 ? UINT32_MAX : UINT32_MAX << (32 - prefix);
            }
            v4 &= mask;
        }
        out->v6 = false;
        out->v4 = v4;
        return true;
    }

    uint8_t v6[16];
    if (cf_ban_parse_ipv6(address, v6)) {
        if (has_prefix) {
            if (prefix > 128) return false;
            if (prefix < 128) {
                size_t full = prefix / 8;
                unsigned rem = (unsigned)(prefix % 8);
                if (rem != 0) {
                    v6[full] &= (uint8_t)(0xffu << (8 - rem));
                    full++;
                }
                for (size_t k = full; k < 16; k++) v6[k] = 0;
            }
        }
        out->v6 = true;
        memcpy(out->v6_bytes, v6, 16);
        return true;
    }
    return false;
}

/* Ruby IPAddr predicates, including the IPv4-mapped quirk: only bits 32..47
 * are checked for 0xffff (bytes 10..11 of the network-order address). */
static bool cf_ban_v6_mapped_v4(const uint8_t bytes[16], uint32_t *out) {
    if (bytes[10] != 0xff || bytes[11] != 0xff) return false;
    *out = ((uint32_t)bytes[12] << 24) | ((uint32_t)bytes[13] << 16) |
           ((uint32_t)bytes[14] << 8) | (uint32_t)bytes[15];
    return true;
}

static bool cf_ban_private_v4(uint32_t a) {
    return (a & 0xff000000u) == 0x0a000000u ||
           (a & 0xfff00000u) == 0xac100000u ||
           (a & 0xffff0000u) == 0xc0a80000u;
}

static bool cf_ban_loopback(const cf_ban_ip *ip) {
    if (!ip->v6) return (ip->v4 & 0xff000000u) == 0x7f000000u;
    bool native = true;
    for (size_t k = 0; k < 15; k++) {
        if (ip->v6_bytes[k] != 0) {
            native = false;
            break;
        }
    }
    if (native && ip->v6_bytes[15] == 1) return true;
    uint32_t mapped;
    return cf_ban_v6_mapped_v4(ip->v6_bytes, &mapped) &&
           (mapped & 0xff000000u) == 0x7f000000u;
}

static bool cf_ban_private(const cf_ban_ip *ip) {
    if (!ip->v6) return cf_ban_private_v4(ip->v4);
    /* Ruby: addr >> 121 == 0xfc >> 1, i.e. the fc00::/7 prefix. */
    if ((ip->v6_bytes[0] & 0xfe) == 0xfc) return true;
    uint32_t mapped;
    return cf_ban_v6_mapped_v4(ip->v6_bytes, &mapped) &&
           cf_ban_private_v4(mapped);
}

static bool cf_ban_link_local(const cf_ban_ip *ip) {
    if (!ip->v6) return (ip->v4 & 0xffff0000u) == 0xa9fe0000u;
    /* Ruby: addr >> 118 == 0xfe80 >> 6, i.e. the fe80::/10 prefix. */
    if (ip->v6_bytes[0] == 0xfe && (ip->v6_bytes[1] & 0xc0) == 0x80) return true;
    uint32_t mapped;
    return cf_ban_v6_mapped_v4(ip->v6_bytes, &mapped) &&
           (mapped & 0xffff0000u) == 0xa9fe0000u;
}

static cf_err cf_ban_errors_add(cf_model_errors *errors, const char *field,
                                const char *message) {
    cf_str field_copy = {0};
    cf_str message_copy = {0};
    cf_err rc = cf_ban_strdup_cstr(field, &field_copy);
    if (rc != CF_OK) return rc;
    rc = cf_ban_strdup_cstr(message, &message_copy);
    if (rc != CF_OK) {
        cf_str_dispose(&field_copy);
        return rc;
    }
    if (errors->len == errors->cap) {
        size_t cap = errors->cap == 0 ? 2 : errors->cap * 2;
        cf_model_error *items = NULL;
        if (cap >= errors->cap && cap <= SIZE_MAX / sizeof(cf_model_error)) {
            items = realloc(errors->items, cap * sizeof *items);
        }
        if (items == NULL) {
            cf_str_dispose(&field_copy);
            cf_str_dispose(&message_copy);
            return CF_NOMEM;
        }
        errors->items = items;
        errors->cap = cap;
    }
    errors->items[errors->len].field = field_copy;
    errors->items[errors->len].message = message_copy;
    errors->len++;
    return CF_OK;
}

/* ---- model functions ---------------------------------------------------- */

cf_err cf_ban_banned(cf_db *db, cf_str ip_address, bool *out) {
    if (db == NULL || out == NULL) return CF_INVALID;
    *out = false;

    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &cf_ban_stmt_set, CF_BAN_STMT_BANNED, &stmt);
    if (rc != CF_OK) return rc;

    rc = cf_stmt_bind_text(
        stmt, 1, (cf_span){(const unsigned char *)ip_address.ptr,
                           ip_address.len});
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            *out = true;
        } else if (step != SQLITE_DONE) {
            rc = cf_db_err(step);
        }
    }
    cf_db_stmt_done(stmt);
    return rc;
}

cf_err cf_ban_for_user(cf_db *db, int64_t user_id, cf_ban_vector *out) {
    if (db == NULL || out == NULL) return CF_INVALID;
    *out = (cf_ban_vector){NULL, 0, 0};

    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &cf_ban_stmt_set, CF_BAN_STMT_FOR_USER, &stmt);
    if (rc != CF_OK) return rc;

    rc = cf_stmt_bind_i64(stmt, 1, user_id);
    cf_ban_vector rows = {NULL, 0, 0};
    if (rc == CF_OK) {
        for (;;) {
            int step = sqlite3_step(stmt);
            if (step == SQLITE_ROW) {
                cf_ban row;
                memset(&row, 0, sizeof row);
                rc = cf_ban_row(stmt, &row);
                if (rc == CF_OK) rc = cf_ban_vector_push(&rows, row);
                if (rc != CF_OK) {
                    cf_ban_dispose(&row);
                    break;
                }
            } else if (step == SQLITE_DONE) {
                rc = CF_OK;
                break;
            } else {
                rc = cf_db_err(step);
                break;
            }
        }
    }
    cf_db_stmt_done(stmt);
    if (rc != CF_OK) {
        cf_ban_vector_dispose(&rows);
        return rc;
    }
    *out = rows;
    return CF_OK;
}

cf_err cf_ban_create(cf_tx *tx, int64_t user_id, cf_str ip_address,
                     cf_ban *out) {
    if (tx == NULL || out == NULL) return CF_INVALID;
    *out = (cf_ban){0, 0, {NULL, 0}, 0, 0};

    cf_model_errors errors = {NULL, 0, 0};
    cf_err rc = cf_ban_validate(ip_address, &errors);
    bool valid = rc == CF_OK && errors.len == 0;
    cf_model_errors_dispose(&errors);
    if (rc != CF_OK) return rc;
    if (!valid) return CF_INVALID;

    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;

    cf_span ip = {(const unsigned char *)ip_address.ptr, ip_address.len};
    cf_str ip_copy = {NULL, 0};
    rc = cf_ban_strdup_span(ip, &ip_copy);
    if (rc != CF_OK) return rc;

    /* Rust tx.now(): the port's single injected process clock. */
    int64_t now = cf_now_us(NULL);
    char stamp[CF_DB_TIME_TEXT_CAP];
    rc = cf_db_time_to_text(now, stamp);
    if (rc != CF_OK) {
        cf_str_dispose(&ip_copy);
        return rc;
    }
    cf_span stamp_span = {(const unsigned char *)stamp, strlen(stamp)};

    sqlite3_stmt *stmt = NULL;
    rc = cf_db_stmt(db, &cf_ban_stmt_set, CF_BAN_STMT_INSERT, &stmt);
    if (rc == CF_OK) {
        rc = cf_stmt_bind_text(stmt, 1, stamp_span);
        if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 2, ip);
        if (rc == CF_OK) rc = cf_stmt_bind_text(stmt, 3, stamp_span);
        if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 4, user_id);
    }
    int64_t id = 0;
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            id = cf_stmt_column_i64(stmt, 0);
        } else if (step == SQLITE_DONE) {
            rc = cf_db_failf(CF_DB, "ban insert returned no id");
        } else {
            rc = cf_db_err(step);
        }
    }
    cf_db_stmt_done(stmt);
    if (rc != CF_OK) {
        cf_str_dispose(&ip_copy);
        return rc;
    }

    out->id = id;
    out->user_id = user_id;
    out->ip_address = ip_copy;
    out->created_at = now;
    out->updated_at = now;
    return CF_OK;
}

cf_err cf_ban_validate(cf_str ip_address, cf_model_errors *out) {
    if (out == NULL) return CF_INVALID;
    *out = (cf_model_errors){NULL, 0, 0};

    cf_ban_ip ip;
    bool parsed = cf_ban_parse_ipaddr(
        (cf_span){(const unsigned char *)ip_address.ptr, ip_address.len}, &ip);
    if (parsed) {
        if (cf_ban_loopback(&ip) || cf_ban_private(&ip) ||
            cf_ban_link_local(&ip)) {
            return cf_ban_errors_add(out, "ip_address",
                                     "cannot be a private or internal IP address");
        }
        return CF_OK;
    }
    return cf_ban_errors_add(out, "ip_address", "is not a valid IP address");
}
