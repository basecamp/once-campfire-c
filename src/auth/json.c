/* src/auth/json.c — JSON text shaping and UTC timestamps for the auth layer
 * (rails_compat json.rs/metadata.rs).  ActiveSupport::JSON.encode escapes
 * `<`, `>` and `&` as </>/& inside strings; U+2028/U+2029 and
 * DEL are left alone.  Parsing goes through the pinned yyjson amalgamation.
 */
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

cf_err auth_str_dup(cf_span span, cf_str *out) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    if (span.len != 0 && span.ptr == NULL) return CF_INVALID;
    char *copy = malloc(span.len + 1);
    if (copy == NULL) return CF_NOMEM;
    if (span.len != 0) memcpy(copy, span.ptr, span.len);
    copy[span.len] = '\0';
    out->ptr = copy;
    out->len = span.len;
    return CF_OK;
}

bool auth_span_equal(cf_span a, cf_span b) {
    return a.len == b.len &&
           (a.len == 0 || memcmp(a.ptr, b.ptr, a.len) == 0);
}

cf_span auth_cstr_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* --- UTF-8 ----------------------------------------------------------------- */

bool auth_utf8_valid(cf_span bytes) {
    size_t i = 0;
    while (i < bytes.len) {
        unsigned char c = bytes.ptr[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        size_t need;
        unsigned cp;
        unsigned min_cp;
        if ((c & 0xe0) == 0xc0) {
            need = 1;
            cp = c & 0x1f;
            min_cp = 0x80;
        } else if ((c & 0xf0) == 0xe0) {
            need = 2;
            cp = c & 0x0f;
            min_cp = 0x800;
        } else if ((c & 0xf8) == 0xf0) {
            need = 3;
            cp = c & 0x07;
            min_cp = 0x10000;
        } else {
            return false;
        }
        if (i + need >= bytes.len) return false;
        for (size_t k = 1; k <= need; k++) {
            unsigned char cc = bytes.ptr[i + k];
            if ((cc & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (unsigned)(cc & 0x3f);
        }
        if (cp < min_cp) return false;
        if (cp > 0x10ffff) return false;
        if (cp >= 0xd800 && cp <= 0xdfff) return false;
        i += need + 1;
    }
    return true;
}

/* --- ActiveSupport::JSON string encoding ----------------------------------- */

cf_err auth_json_string_encode(cf_builder *builder, cf_span raw) {
    if (builder == NULL) return CF_INVALID;
    static const char hex[] = "0123456789abcdef";
    cf_err rc = cf_builder_append(builder, auth_cstr_span("\""));
    for (size_t i = 0; rc == CF_OK && i < raw.len; i++) {
        unsigned char c = raw.ptr[i];
        switch (c) {
        case '"':
            rc = cf_builder_append(builder, auth_cstr_span("\\\""));
            break;
        case '\\':
            rc = cf_builder_append(builder, auth_cstr_span("\\\\"));
            break;
        case '<':
            rc = cf_builder_append(builder, auth_cstr_span("\\u003c"));
            break;
        case '>':
            rc = cf_builder_append(builder, auth_cstr_span("\\u003e"));
            break;
        case '&':
            rc = cf_builder_append(builder, auth_cstr_span("\\u0026"));
            break;
        case '\b':
            rc = cf_builder_append(builder, auth_cstr_span("\\b"));
            break;
        case '\f':
            rc = cf_builder_append(builder, auth_cstr_span("\\f"));
            break;
        case '\n':
            rc = cf_builder_append(builder, auth_cstr_span("\\n"));
            break;
        case '\r':
            rc = cf_builder_append(builder, auth_cstr_span("\\r"));
            break;
        case '\t':
            rc = cf_builder_append(builder, auth_cstr_span("\\t"));
            break;
        default:
            if (c < 0x20) {
                char esc[7] = {'\\', 'u', '0', '0', hex[c >> 4],
                               hex[c & 0x0f], '\0'};
                rc = cf_builder_append(builder, auth_cstr_span(esc));
            } else {
                rc = cf_builder_append(builder,
                                       (cf_span){(const unsigned char *)&c, 1});
            }
            break;
        }
    }
    if (rc == CF_OK) rc = cf_builder_append(builder, auth_cstr_span("\""));
    return rc;
}

/* `::JSON.generate` of a string: the json gem escapes quote, backslash and
 * control characters (lowercase hex) and leaves `<`, `>`, `&`, `/` and
 * U+2028/U+2029 alone. */
cf_err auth_json_string_generate(cf_builder *builder, cf_span raw) {
    if (builder == NULL) return CF_INVALID;
    static const char hex[] = "0123456789abcdef";
    cf_err rc = cf_builder_append(builder, auth_cstr_span("\""));
    for (size_t i = 0; rc == CF_OK && i < raw.len; i++) {
        unsigned char c = raw.ptr[i];
        switch (c) {
        case '"':
            rc = cf_builder_append(builder, auth_cstr_span("\\\""));
            break;
        case '\\':
            rc = cf_builder_append(builder, auth_cstr_span("\\\\"));
            break;
        case '\b':
            rc = cf_builder_append(builder, auth_cstr_span("\\b"));
            break;
        case '\f':
            rc = cf_builder_append(builder, auth_cstr_span("\\f"));
            break;
        case '\n':
            rc = cf_builder_append(builder, auth_cstr_span("\\n"));
            break;
        case '\r':
            rc = cf_builder_append(builder, auth_cstr_span("\\r"));
            break;
        case '\t':
            rc = cf_builder_append(builder, auth_cstr_span("\\t"));
            break;
        default:
            if (c < 0x20) {
                char esc[7] = {'\\', 'u', '0', '0', hex[c >> 4],
                               hex[c & 0x0f], '\0'};
                rc = cf_builder_append(builder, auth_cstr_span(esc));
            } else {
                rc = cf_builder_append(builder,
                                       (cf_span){(const unsigned char *)&c, 1});
            }
            break;
        }
    }
    if (rc == CF_OK) rc = cf_builder_append(builder, auth_cstr_span("\""));
    return rc;
}

/* ActiveSupport::JSON.encode's escape_html_entities pass. */
static cf_err auth_escape_html_entities(cf_str *json) {
    if (json->ptr == NULL) return CF_OK;
    bool needed = false;
    for (size_t i = 0; i < json->len; i++) {
        char c = json->ptr[i];
        if (c == '<' || c == '>' || c == '&') {
            needed = true;
            break;
        }
    }
    if (!needed) return CF_OK;
    cf_builder b = {0};
    cf_err rc = CF_OK;
    for (size_t i = 0; rc == CF_OK && i < json->len; i++) {
        char c = json->ptr[i];
        if (c == '<') rc = cf_builder_append(&b, auth_cstr_span("\\u003c"));
        else if (c == '>') rc = cf_builder_append(&b, auth_cstr_span("\\u003e"));
        else if (c == '&') rc = cf_builder_append(&b, auth_cstr_span("\\u0026"));
        else rc = cf_builder_append(&b, (cf_span){(const unsigned char *)&c, 1});
    }
    if (rc != CF_OK) {
        cf_builder_dispose(&b);
        return rc;
    }
    cf_buf *frozen = NULL;
    rc = cf_builder_freeze(&b, &frozen);
    if (rc != CF_OK) return rc;
    cf_span span = cf_buf_span(frozen);
    free(json->ptr);
    json->ptr = malloc(span.len + 1);
    if (json->ptr == NULL) {
        cf_buf_release(frozen);
        return CF_NOMEM;
    }
    memcpy(json->ptr, span.ptr, span.len);
    json->ptr[span.len] = '\0';
    json->len = span.len;
    cf_buf_release(frozen);
    return CF_OK;
}

cf_err auth_json_write_value(yyjson_val *value, bool active_support_escape,
                             cf_str *out) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    size_t len = 0;
    char *text = yyjson_val_write(value, 0, &len);
    if (text == NULL) return CF_NOMEM;
    out->ptr = text;
    out->len = len;
    if (active_support_escape) {
        cf_err rc = auth_escape_html_entities(out);
        if (rc != CF_OK) {
            cf_str_dispose(out);
            return rc;
        }
    }
    return CF_OK;
}

/* --- timestamps ------------------------------------------------------------- */

static int64_t auth_floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

cf_err auth_iso8601_millis(int64_t us, char out[32]) {
    if (out == NULL) return CF_INVALID;
    int64_t secs = auth_floor_div(us, INT64_C(1000000));
    int64_t micro = us - secs * INT64_C(1000000);
    time_t t = (time_t)secs;
    struct tm tm;
    if (gmtime_r(&t, &tm) == NULL) return CF_INVALID;
    int millis = (int)(micro / 1000);
    int n = snprintf(out, 32, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                     tm.tm_min, tm.tm_sec, millis);
    return n > 0 && n < 32 ? CF_OK : CF_INVALID;
}

static bool auth_parse_int(cf_span text, size_t *at, size_t digits,
                           int *out) {
    if (*at + digits > text.len) return false;
    int value = 0;
    for (size_t i = 0; i < digits; i++) {
        unsigned char c = text.ptr[*at + i];
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    *at += digits;
    *out = value;
    return true;
}

static bool auth_is_leap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int auth_days_in_month(int year, int month) {
    static const int days[] = {31, 28, 31, 30, 31, 30,
                               31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && auth_is_leap(year)) return 29;
    return days[month - 1];
}

cf_err auth_iso8601_parse_ns(cf_span text, int64_t *out_ns) {
    if (out_ns == NULL) return CF_INVALID;
    *out_ns = 0;
    if (text.len < 20 || text.ptr == NULL) return CF_INVALID;
    size_t at = 0;
    int year, month, day, hour, minute, second;
    if (!auth_parse_int(text, &at, 4, &year) || text.ptr[at++] != '-' ||
        !auth_parse_int(text, &at, 2, &month) || text.ptr[at++] != '-' ||
        !auth_parse_int(text, &at, 2, &day) || text.ptr[at++] != 'T' ||
        !auth_parse_int(text, &at, 2, &hour) || text.ptr[at++] != ':' ||
        !auth_parse_int(text, &at, 2, &minute) || text.ptr[at++] != ':' ||
        !auth_parse_int(text, &at, 2, &second)) {
        return CF_INVALID;
    }
    if (month < 1 || month > 12 || day < 1 ||
        day > auth_days_in_month(year, month) || hour > 23 || minute > 59 ||
        second > 60) {
        return CF_INVALID;
    }
    int64_t nanos = 0;
    if (at < text.len && text.ptr[at] == '.') {
        at++;
        size_t digits = 0;
        int64_t frac = 0;
        while (at < text.len && text.ptr[at] >= '0' && text.ptr[at] <= '9') {
            if (digits < 9) frac = frac * 10 + (text.ptr[at] - '0');
            digits++;
            at++;
        }
        if (digits == 0 || digits > 9) return CF_INVALID;
        for (size_t d = digits; d < 9; d++) frac *= 10;
        nanos = frac;
    }
    if (at >= text.len || text.ptr[at] != 'Z' || at + 1 != text.len) {
        return CF_INVALID;
    }
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    time_t secs = timegm(&tm);
    if (secs == (time_t)-1) return CF_INVALID;
    *out_ns = (int64_t)secs * INT64_C(1000000000) + nanos;
    return CF_OK;
}

int64_t auth_years_from_us(int64_t us, int years) {
    int64_t secs = auth_floor_div(us, INT64_C(1000000));
    int64_t micro = us - secs * INT64_C(1000000);
    time_t t = (time_t)secs;
    struct tm tm;
    if (gmtime_r(&t, &tm) == NULL) return us;
    tm.tm_year += years;
    int max_day = auth_days_in_month(tm.tm_year + 1900, tm.tm_mon + 1);
    if (max_day != 0 && tm.tm_mday > max_day) tm.tm_mday = max_day;
    time_t moved = timegm(&tm);
    if (moved == (time_t)-1) return us;
    return (int64_t)moved * INT64_C(1000000) + micro;
}
