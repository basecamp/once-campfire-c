/* src/richtext/rt_util.c — buffers, UTF-8, Ruby string behavior, escaping,
 * base64 and JSON/float coercion for the R02 pipeline.
 *
 * Ports: tmp/rust-ref/crates/richtext/src/ruby.rs (is_blank, presence, chomp,
 * truncate, JSON value coercion), crates/ruby/src/{string,uri,erb,float}.rs,
 * crates/rails_compat/src/{encoding,json}.rs.
 */
#include "richtext/internal.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yyjson.h"

/* ---- buffer -------------------------------------------------------------- */

void rt_buf_init(rt_buf *buf) {
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

void rt_buf_dispose(rt_buf *buf) {
    if (buf == NULL) return;
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

void rt_buf_clear(rt_buf *buf) {
    buf->len = 0;
}

rt_status rt_buf_reserve(rt_buf *buf, size_t extra) {
    if (extra > SIZE_MAX - buf->len) return RT_NOMEM;
    size_t want = buf->len + extra;
    if (want <= buf->cap) return RT_OK;
    size_t cap = buf->cap != 0 ? buf->cap : 64;
    while (cap < want) {
        if (cap > SIZE_MAX / 2) {
            cap = want;
            break;
        }
        cap *= 2;
    }
    unsigned char *grown = realloc(buf->data, cap);
    if (grown == NULL) return RT_NOMEM;
    buf->data = grown;
    buf->cap = cap;
    return RT_OK;
}

rt_status rt_buf_append(rt_buf *buf, const void *bytes, size_t len) {
    if (len == 0) return RT_OK;
    rt_status rc = rt_buf_reserve(buf, len);
    if (rc != RT_OK) return rc;
    memcpy(buf->data + buf->len, bytes, len);
    buf->len += len;
    return RT_OK;
}

rt_status rt_buf_puts(rt_buf *buf, const char *text) {
    return rt_buf_append(buf, text, strlen(text));
}

rt_status rt_buf_putc(rt_buf *buf, unsigned char byte) {
    return rt_buf_append(buf, &byte, 1);
}

size_t rt_utf8_encode(uint32_t code, unsigned char out[4]) {
    if (code < 0x80) {
        out[0] = (unsigned char)code;
        return 1;
    }
    if (code < 0x800) {
        out[0] = (unsigned char)(0xC0 | (code >> 6));
        out[1] = (unsigned char)(0x80 | (code & 0x3F));
        return 2;
    }
    if (code < 0x10000) {
        out[0] = (unsigned char)(0xE0 | (code >> 12));
        out[1] = (unsigned char)(0x80 | ((code >> 6) & 0x3F));
        out[2] = (unsigned char)(0x80 | (code & 0x3F));
        return 3;
    }
    out[0] = (unsigned char)(0xF0 | (code >> 18));
    out[1] = (unsigned char)(0x80 | ((code >> 12) & 0x3F));
    out[2] = (unsigned char)(0x80 | ((code >> 6) & 0x3F));
    out[3] = (unsigned char)(0x80 | (code & 0x3F));
    return 4;
}

rt_status rt_buf_put_utf8(rt_buf *buf, uint32_t code) {
    unsigned char bytes[4];
    size_t width = rt_utf8_encode(code, bytes);
    return rt_buf_append(buf, bytes, width);
}

rt_status rt_buf_printf(rt_buf *buf, const char *format, ...) {
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return RT_NOMEM;
    }
    rt_status rc = rt_buf_reserve(buf, (size_t)needed + 1);
    if (rc != RT_OK) {
        va_end(args);
        return rc;
    }
    vsnprintf((char *)buf->data + buf->len, (size_t)needed + 1, format, args);
    va_end(args);
    buf->len += (size_t)needed;
    return RT_OK;
}

cf_span rt_buf_span(const rt_buf *buf) {
    cf_span span = {buf->data, buf->len};
    return span;
}

rt_status rt_buf_to_str(rt_buf *buf, cf_str *out) {
    rt_status rc = rt_buf_reserve(buf, 1);
    if (rc != RT_OK) return rc;
    buf->data[buf->len] = '\0';
    out->ptr = (char *)buf->data;
    out->len = buf->len;
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
    return RT_OK;
}

rt_status rt_buf_to_cstr(rt_buf *buf, char **out) {
    rt_status rc = rt_buf_reserve(buf, 1);
    if (rc != RT_OK) return rc;
    buf->data[buf->len] = '\0';
    *out = (char *)buf->data;
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
    return RT_OK;
}

/* ---- node vectors -------------------------------------------------------- */

void rt_node_vec_init(rt_node_vec *vec) {
    vec->items = NULL;
    vec->len = 0;
    vec->cap = 0;
}

void rt_node_vec_dispose(rt_node_vec *vec) {
    if (vec == NULL) return;
    free(vec->items);
    vec->items = NULL;
    vec->len = 0;
    vec->cap = 0;
}

rt_status rt_node_vec_push(rt_node_vec *vec, rt_node node) {
    if (vec->len == vec->cap) {
        size_t cap = vec->cap != 0 ? vec->cap * 2 : 8;
        if (cap < vec->cap) return RT_NOMEM;
        rt_node *grown = realloc(vec->items, cap * sizeof *grown);
        if (grown == NULL) return RT_NOMEM;
        vec->items = grown;
        vec->cap = cap;
    }
    vec->items[vec->len++] = node;
    return RT_OK;
}

bool rt_node_vec_contains(const rt_node_vec *vec, rt_node node) {
    for (size_t i = 0; i < vec->len; i++) {
        if (vec->items[i] == node) return true;
    }
    return false;
}



/* ---- UTF-8 --------------------------------------------------------------- */

size_t rt_utf8_decode(const unsigned char *bytes, size_t len, uint32_t *out) {
    if (len == 0) {
        *out = 0;
        return 0;
    }
    unsigned char b0 = bytes[0];
    if (b0 < 0x80) {
        *out = b0;
        return 1;
    }
    if ((b0 & 0xE0) == 0xC0) {
        if (len >= 2 && (bytes[1] & 0xC0) == 0x80 && (b0 & 0x1E) != 0) {
            *out = ((uint32_t)(b0 & 0x1F) << 6) | (bytes[1] & 0x3F);
            return 2;
        }
    } else if ((b0 & 0xF0) == 0xE0) {
        if (len >= 3 && (bytes[1] & 0xC0) == 0x80 && (bytes[2] & 0xC0) == 0x80) {
            uint32_t code = ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(bytes[1] & 0x3F) << 6) |
                            (bytes[2] & 0x3F);
            if (code >= 0x800 && !(code >= 0xD800 && code <= 0xDFFF)) {
                *out = code;
                return 3;
            }
        }
    } else if ((b0 & 0xF8) == 0xF0) {
        if (len >= 4 && (bytes[1] & 0xC0) == 0x80 && (bytes[2] & 0xC0) == 0x80 &&
            (bytes[3] & 0xC0) == 0x80) {
            uint32_t code = ((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)(bytes[1] & 0x3F) << 12) |
                            ((uint32_t)(bytes[2] & 0x3F) << 6) | (bytes[3] & 0x3F);
            if (code >= 0x10000 && code <= 0x10FFFF) {
                *out = code;
                return 4;
            }
        }
    }
    *out = 0xFFFD;
    return 1;
}

/* ---- Ruby string behavior ------------------------------------------------ */

bool rt_is_whitespace(uint32_t code) {
    switch (code) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20:
    case 0x85: case 0xA0: case 0x1680:
    case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004:
    case 0x2005: case 0x2006: case 0x2007: case 0x2008: case 0x2009:
    case 0x200A: case 0x2028: case 0x2029: case 0x202F: case 0x205F:
    case 0x3000:
        return true;
    default:
        return false;
    }
}

bool rt_is_blank(const unsigned char *bytes, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint32_t code;
        size_t width = rt_utf8_decode(bytes + i, len - i, &code);
        if (width == 0) return true;
        if (!rt_is_whitespace(code)) return false;
        i += width;
    }
    return true;
}

bool rt_cstr_is_blank(const char *text) {
    return rt_is_blank((const unsigned char *)text, strlen(text));
}

bool rt_cstr_present(const char *text) {
    return text != NULL && !rt_cstr_is_blank(text);
}

void rt_ruby_strip(const unsigned char *bytes, size_t len, const unsigned char **out_ptr,
                   size_t *out_len) {
    size_t start = 0;
    while (start < len && (bytes[start] == '\0' || bytes[start] == ' ' || bytes[start] == '\t' ||
                           bytes[start] == '\n' || bytes[start] == '\v' || bytes[start] == '\f' ||
                           bytes[start] == '\r')) {
        start++;
    }
    size_t end = len;
    while (end > start && (bytes[end - 1] == '\0' || bytes[end - 1] == ' ' || bytes[end - 1] == '\t' ||
                           bytes[end - 1] == '\n' || bytes[end - 1] == '\v' ||
                           bytes[end - 1] == '\f' || bytes[end - 1] == '\r')) {
        end--;
    }
    *out_ptr = bytes + start;
    *out_len = end - start;
}

void rt_chomp_newlines(const unsigned char *bytes, size_t len, const unsigned char **out_ptr,
                       size_t *out_len) {
    size_t end = len;
    while (end >= 2 && bytes[end - 1] == '\n' && bytes[end - 2] == '\r') {
        end -= 2;
    }
    while (end >= 1 && bytes[end - 1] == '\n') {
        end--;
    }
    *out_ptr = bytes;
    *out_len = end;
}

void rt_chomp(const unsigned char *bytes, size_t len, const unsigned char **out_ptr, size_t *out_len) {
    size_t end = len;
    if (end >= 2 && bytes[end - 2] == '\r' && bytes[end - 1] == '\n') {
        end -= 2;
    } else if (end >= 1 && (bytes[end - 1] == '\n' || bytes[end - 1] == '\r')) {
        end -= 1;
    }
    *out_ptr = bytes;
    *out_len = end;
}

void rt_trim_whitespace(const unsigned char *bytes, size_t len, const unsigned char **out_ptr,
                        size_t *out_len) {
    size_t start = 0;
    while (start < len) {
        uint32_t code;
        size_t width = rt_utf8_decode(bytes + start, len - start, &code);
        if (width == 0 || !rt_is_whitespace(code)) break;
        start += width;
    }
    size_t end = len;
    while (end > start) {
        /* Walk backwards to the previous code point start. */
        size_t prev = end - 1;
        while (prev > start && (bytes[prev] & 0xC0) == 0x80) prev--;
        uint32_t code;
        size_t width = rt_utf8_decode(bytes + prev, end - prev, &code);
        if (width == 0 || prev + width != end || !rt_is_whitespace(code)) break;
        end = prev;
    }
    *out_ptr = bytes + start;
    *out_len = end - start;
}

rt_status rt_truncate(const char *text, size_t length, const char *omission, rt_buf *out) {
    size_t chars = 0;
    const unsigned char *bytes = (const unsigned char *)text;
    size_t len = strlen(text);
    for (size_t i = 0; i < len;) {
        uint32_t code;
        size_t width = rt_utf8_decode(bytes + i, len - i, &code);
        if (width == 0) width = 1;
        chars++;
        i += width;
    }
    if (chars <= length) return rt_buf_puts(out, text);
    size_t omission_chars = 0;
    size_t omission_len = strlen(omission);
    for (size_t i = 0; i < omission_len;) {
        uint32_t code;
        size_t width = rt_utf8_decode((const unsigned char *)omission + i, omission_len - i, &code);
        if (width == 0) width = 1;
        omission_chars++;
        i += width;
    }
    size_t keep = length > omission_chars ? length - omission_chars : 0;
    size_t i = 0;
    size_t taken = 0;
    while (i < len && taken < keep) {
        uint32_t code;
        size_t width = rt_utf8_decode(bytes + i, len - i, &code);
        if (width == 0) width = 1;
        i += width;
        taken++;
    }
    rt_status rc = rt_buf_append(out, bytes, i);
    if (rc == RT_OK) rc = rt_buf_puts(out, omission);
    return rc;
}

bool rt_ascii_ieq(const unsigned char *bytes, size_t len, const char *literal) {
    size_t n = strlen(literal);
    if (len != n) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char a = bytes[i];
        unsigned char b = (unsigned char)literal[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

bool rt_ascii_istarts(const unsigned char *bytes, size_t len, const char *prefix) {
    size_t n = strlen(prefix);
    if (len < n) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char a = bytes[i];
        unsigned char b = (unsigned char)prefix[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

bool rt_span_contains(const unsigned char *bytes, size_t len, const char *needle) {
    size_t n = strlen(needle);
    if (n == 0) return true;
    if (len < n) return false;
    for (size_t i = 0; i + n <= len; i++) {
        if (memcmp(bytes + i, needle, n) == 0) return true;
    }
    return false;
}

/* ---- escaping ------------------------------------------------------------ */

rt_status rt_html_escape(const unsigned char *bytes, size_t len, rt_buf *out) {
    for (size_t i = 0; i < len; i++) {
        const char *replacement = NULL;
        switch (bytes[i]) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        case '"': replacement = "&quot;"; break;
        case '\'': replacement = "&#39;"; break;
        default: break;
        }
        rt_status rc =
            replacement != NULL ? rt_buf_puts(out, replacement) : rt_buf_putc(out, bytes[i]);
        if (rc != RT_OK) return rc;
    }
    return RT_OK;
}

rt_status rt_url_encode(const unsigned char *bytes, size_t len, rt_buf *out) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        unsigned char b = bytes[i];
        bool safe = (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') ||
                    b == '_' || b == '.' || b == '-' || b == '~';
        rt_status rc;
        if (safe) {
            rc = rt_buf_putc(out, b);
        } else {
            unsigned char encoded[3] = {'%', (unsigned char)hex[b >> 4], (unsigned char)hex[b & 0xF]};
            rc = rt_buf_append(out, encoded, 3);
        }
        if (rc != RT_OK) return rc;
    }
    return RT_OK;
}

/* ---- base64 -------------------------------------------------------------- */

static int base64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Base64.strict_decode64 semantics (the `base64` crate's STANDARD engine):
 * canonical padding required, no data after padding, non-zero trailing bits
 * rejected. */
static bool base64_strict_decode(const unsigned char *bytes, size_t len, rt_buf *out) {
    if (len % 4 != 0) return false;
    size_t i = 0;
    while (i < len) {
        unsigned char c0 = bytes[i];
        bool last = i + 4 == len;
        if (c0 == '=') return false;
        int v0 = base64_value(c0);
        if (v0 < 0) return false;
        int v1 = base64_value(bytes[i + 1]);
        if (v1 < 0) return false;
        int v2 = -1;
        int v3 = -1;
        int pad = 0;
        if (bytes[i + 2] == '=') {
            if (!last || bytes[i + 3] != '=') return false;
            pad = 2;
        } else {
            v2 = base64_value(bytes[i + 2]);
            if (v2 < 0) return false;
            if (bytes[i + 3] == '=') {
                if (!last) return false;
                pad = 1;
            } else {
                v3 = base64_value(bytes[i + 3]);
                if (v3 < 0) return false;
            }
        }
        if (pad == 2 && (v1 & 0x0F) != 0) return false;
        if (pad == 1 && (v2 & 0x03) != 0) return false;
        unsigned char decoded[3];
        decoded[0] = (unsigned char)((v0 << 2) | (v1 >> 4));
        size_t count = 1;
        if (pad < 2) {
            decoded[1] = (unsigned char)(((v1 & 0x0F) << 4) | (v2 >> 2));
            count = 2;
        }
        if (pad < 1 && v3 >= 0) {
            decoded[2] = (unsigned char)(((v2 & 0x03) << 6) | v3);
            count = 3;
        }
        if (rt_buf_append(out, decoded, count) != RT_OK) return false;
        i += 4;
    }
    return true;
}

bool rt_base64_decode(const unsigned char *bytes, size_t len, rt_buf *out) {
    size_t saved = out->len;
    if (base64_strict_decode(bytes, len, out)) return true;
    out->len = saved;
    /* Base64.urlsafe_decode64: translate -_ to +/, pad when the input does not
     * end with '=' and is not a multiple of four, then strict-decode. */
    rt_buf translated;
    rt_buf_init(&translated);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = bytes[i];
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
        if (rt_buf_putc(&translated, c) != RT_OK) {
            rt_buf_dispose(&translated);
            return false;
        }
    }
    if ((len == 0 || bytes[len - 1] != '=') && translated.len % 4 != 0) {
        while (translated.len % 4 != 0) {
            if (rt_buf_putc(&translated, '=') != RT_OK) {
                rt_buf_dispose(&translated);
                return false;
            }
        }
    }
    bool ok = base64_strict_decode(translated.data, translated.len, out);
    rt_buf_dispose(&translated);
    if (!ok) out->len = saved;
    return ok;
}

/* ---- Ruby JSON ----------------------------------------------------------- */

/* Ruby's JSON.parse accepts C-style and line comments (crates/richtext
 * ruby.rs strip_json_comments); unterminated comments make the parse fail. */
static rt_status strip_json_comments(const unsigned char *bytes, size_t len, rt_buf *out) {
    size_t i = 0;
    bool in_string = false;
    while (i < len) {
        unsigned char c = bytes[i];
        if (in_string) {
            if (rt_buf_putc(out, c) != RT_OK) return RT_NOMEM;
            i++;
            if (c == '\\') {
                if (i >= len) return RT_RAISED;
                if (rt_buf_putc(out, bytes[i]) != RT_OK) return RT_NOMEM;
                i++;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
            if (rt_buf_putc(out, c) != RT_OK) return RT_NOMEM;
            i++;
            continue;
        }
        if (c == '/' && i + 1 < len && bytes[i + 1] == '*') {
            i += 2;
            bool closed = false;
            while (i < len) {
                if (bytes[i] == '*' && i + 1 < len && bytes[i + 1] == '/') {
                    i += 2;
                    closed = true;
                    break;
                }
                i++;
            }
            if (!closed) return RT_RAISED;
            if (rt_buf_putc(out, ' ') != RT_OK) return RT_NOMEM;
            continue;
        }
        if (c == '/' && i + 1 < len && bytes[i + 1] == '/') {
            while (i < len && bytes[i] != '\n') i++;
            if (rt_buf_putc(out, ' ') != RT_OK) return RT_NOMEM;
            continue;
        }
        if (rt_buf_putc(out, c) != RT_OK) return RT_NOMEM;
        i++;
    }
    return RT_OK;
}

struct yyjson_doc *rt_json_parse(const unsigned char *bytes, size_t len) {
    rt_buf cleaned;
    rt_buf_init(&cleaned);
    if (strip_json_comments(bytes, len, &cleaned) != RT_OK) {
        rt_buf_dispose(&cleaned);
        return NULL;
    }
    yyjson_read_err err;
    yyjson_doc *doc = yyjson_read_opts((char *)cleaned.data, cleaned.len, YYJSON_READ_NOFLAG, NULL, &err);
    rt_buf_dispose(&cleaned);
    return (struct yyjson_doc *)doc;
}

struct yyjson_val *rt_json_doc_root(struct yyjson_doc *doc) {
    return (struct yyjson_val *)yyjson_doc_get_root((yyjson_doc *)doc);
}

void rt_json_doc_free(struct yyjson_doc *doc) {
    yyjson_doc_free((yyjson_doc *)doc);
}

/* Ruby Float#to_s and Integer#to_s for a parsed JSON number. */
static rt_status json_number_to_s(const yyjson_val *value, rt_buf *out) {
    if (yyjson_is_sint(value)) {
        return rt_buf_printf(out, "%lld", (long long)yyjson_get_sint(value));
    }
    if (yyjson_is_uint(value)) {
        return rt_buf_printf(out, "%llu", (unsigned long long)yyjson_get_uint(value));
    }
    return rt_float_to_s(yyjson_get_real(value), out);
}

/* Ruby String#inspect (ruby.rs string_inspect). */
static rt_status ruby_string_inspect(const char *text, rt_buf *out) {
    if (rt_buf_putc(out, '"') != RT_OK) return RT_NOMEM;
    const unsigned char *bytes = (const unsigned char *)text;
    size_t len = strlen(text);
    size_t i = 0;
    while (i < len) {
        uint32_t code;
        size_t width = rt_utf8_decode(bytes + i, len - i, &code);
        if (width == 0) width = 1;
        rt_status rc = RT_OK;
        switch (code) {
        case '"': rc = rt_buf_puts(out, "\\\""); break;
        case '\\': rc = rt_buf_puts(out, "\\\\"); break;
        case '\n': rc = rt_buf_puts(out, "\\n"); break;
        case '\t': rc = rt_buf_puts(out, "\\t"); break;
        case '\r': rc = rt_buf_puts(out, "\\r"); break;
        case '\f': rc = rt_buf_puts(out, "\\f"); break;
        case '\v': rc = rt_buf_puts(out, "\\v"); break;
        case '\b': rc = rt_buf_puts(out, "\\b"); break;
        case '\a': rc = rt_buf_puts(out, "\\a"); break;
        case 0x1B: rc = rt_buf_puts(out, "\\e"); break;
        case '#': {
            uint32_t next = 0;
            if (i + width < len) rt_utf8_decode(bytes + i + width, len - i - width, &next);
            if (next == '{' || next == '$' || next == '@') {
                rc = rt_buf_puts(out, "\\#");
            } else {
                rc = rt_buf_putc(out, '#');
            }
            break;
        }
        default:
            if (code < 0x20 || code == 0x7F) {
                rc = rt_buf_printf(out, "\\x%02X", (unsigned)code);
            } else {
                rc = rt_buf_append(out, bytes + i, width);
            }
            break;
        }
        if (rc != RT_OK) return rc;
        i += width;
    }
    return rt_buf_putc(out, '"');
}

rt_status rt_json_value_inspect(const struct yyjson_val *value_v, rt_buf *out) {
    const yyjson_val *value = (const yyjson_val *)value_v;
    if (yyjson_is_null(value)) return rt_buf_puts(out, "nil");
    if (yyjson_is_bool(value)) return rt_buf_puts(out, yyjson_get_bool(value) ? "true" : "false");
    if (yyjson_is_num(value)) return json_number_to_s(value, out);
    if (yyjson_is_str(value)) return ruby_string_inspect(yyjson_get_str(value), out);
    if (yyjson_is_arr(value)) {
        if (rt_buf_putc(out, '[') != RT_OK) return RT_NOMEM;
        yyjson_val *item;
        yyjson_arr_iter iter = yyjson_arr_iter_with((yyjson_val *)value);
        bool first = true;
        while ((item = yyjson_arr_iter_next(&iter)) != NULL) {
            if (!first && rt_buf_puts(out, ", ") != RT_OK) return RT_NOMEM;
            first = false;
            rt_status rc = rt_json_value_inspect(item, out);
            if (rc != RT_OK) return rc;
        }
        return rt_buf_putc(out, ']');
    }
    if (yyjson_is_obj(value)) {
        if (yyjson_obj_size(value) == 0) return rt_buf_puts(out, "{}");
        if (rt_buf_putc(out, '{') != RT_OK) return RT_NOMEM;
        yyjson_obj_iter iter = yyjson_obj_iter_with((yyjson_val *)value);
        yyjson_val *key;
        bool first = true;
        while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
            yyjson_val *item = yyjson_obj_iter_get_val(key);
            if (!first && rt_buf_puts(out, ", ") != RT_OK) return RT_NOMEM;
            first = false;
            rt_status rc = ruby_string_inspect(yyjson_get_str(key), out);
            if (rc == RT_OK) rc = rt_buf_puts(out, " => ");
            if (rc == RT_OK) rc = rt_json_value_inspect(item, out);
            if (rc != RT_OK) return rc;
        }
        return rt_buf_putc(out, '}');
    }
    return rt_buf_puts(out, "nil");
}

rt_status rt_json_value_to_s(const struct yyjson_val *value, rt_buf *out) {
    if (yyjson_is_str(value)) {
        const char *text = yyjson_get_str(value);
        return rt_buf_puts(out, text != NULL ? text : "");
    }
    if (yyjson_is_null(value)) return rt_buf_puts(out, "");
    if (yyjson_is_num(value)) return json_number_to_s(value, out);
    return rt_json_value_inspect(value, out);
}

rt_status rt_json_encode_string(const unsigned char *bytes, size_t len, rt_buf *out) {
    if (rt_buf_putc(out, '"') != RT_OK) return RT_NOMEM;
    size_t i = 0;
    while (i < len) {
        uint32_t code;
        size_t width = rt_utf8_decode(bytes + i, len - i, &code);
        if (width == 0) width = 1;
        rt_status rc = RT_OK;
        switch (code) {
        case '"': rc = rt_buf_puts(out, "\\\""); break;
        case '\\': rc = rt_buf_puts(out, "\\\\"); break;
        case '\n': rc = rt_buf_puts(out, "\\n"); break;
        case '\t': rc = rt_buf_puts(out, "\\t"); break;
        case '\r': rc = rt_buf_puts(out, "\\r"); break;
        case '\b': rc = rt_buf_puts(out, "\\b"); break;
        case '\f': rc = rt_buf_puts(out, "\\f"); break;
        case '<': rc = rt_buf_puts(out, "\\u003c"); break;
        case '>': rc = rt_buf_puts(out, "\\u003e"); break;
        case '&': rc = rt_buf_puts(out, "\\u0026"); break;
        default:
            if (code < 0x20) {
                rc = rt_buf_printf(out, "\\u%04x", (unsigned)code);
            } else {
                rc = rt_buf_append(out, bytes + i, width);
            }
            break;
        }
        if (rc != RT_OK) return rc;
        i += width;
    }
    return rt_buf_putc(out, '"');
}

/* ---- Ruby Float#to_s ----------------------------------------------------- */

/* The shortest decimal digits that read back as magnitude, as the scientific
 * form's mantissa digits, and the decimal exponent + 1. */
static rt_status shortest_digits(double magnitude, char digits[32], int *decpt) {
    char buffer[64];
    int precision = 1;
    for (; precision <= 17; precision++) {
        snprintf(buffer, sizeof buffer, "%.*e", precision - 1, magnitude);
        if (strtod(buffer, NULL) == magnitude) break;
    }
    if (precision > 17) precision = 17;
    /* Digits before the exponent marker. */
    size_t digits_len = 0;
    size_t i = 0;
    while (buffer[i] != '\0' && buffer[i] != 'e' && buffer[i] != 'E') {
        if (buffer[i] != '.') {
            if (digits_len >= 31) return RT_RAISED;
            digits[digits_len++] = buffer[i];
        }
        i++;
    }
    digits[digits_len] = '\0';
    /* Trim trailing zeros, keeping at least one digit. */
    while (digits_len > 1 && digits[digits_len - 1] == '0') digits[--digits_len] = '\0';
    int exponent = 0;
    if (buffer[i] == 'e' || buffer[i] == 'E') exponent = atoi(buffer + i + 1);
    *decpt = exponent + 1;
    return RT_OK;
}

rt_status rt_float_to_s(double value, rt_buf *out) {
    if (isnan(value)) return rt_buf_puts(out, "NaN");
    if (isinf(value)) return rt_buf_puts(out, value > 0 ? "Infinity" : "-Infinity");
    if (value == 0.0) return rt_buf_puts(out, signbit(value) ? "-0.0" : "0.0");
    char digits[32];
    int decpt = 0;
    rt_status rc = shortest_digits(fabs(value), digits, &decpt);
    if (rc != RT_OK) return rc;
    const char *sign = value < 0.0 ? "-" : "";
    size_t digits_len = strlen(digits);
    if (decpt < -3 || (decpt > 15 && (int)digits_len <= decpt)) {
        const char *rest = digits + 1;
        if (*rest == '\0') rest = "0";
        int e = decpt - 1;
        char sign_char = e < 0 ? '-' : '+';
        if (e < 0) e = -e;
        return rt_buf_printf(out, "%s%c.%se%c%02d", sign, digits[0], rest, sign_char, e);
    }
    if (decpt <= 0) {
        rc = rt_buf_printf(out, "%s0.", sign);
        for (int i = 0; i < -decpt && rc == RT_OK; i++) rc = rt_buf_putc(out, '0');
        if (rc == RT_OK) rc = rt_buf_puts(out, digits);
        return rc;
    }
    if ((size_t)decpt >= digits_len) {
        rc = rt_buf_printf(out, "%s%s", sign, digits);
        for (int i = 0; i < decpt - (int)digits_len && rc == RT_OK; i++) rc = rt_buf_putc(out, '0');
        if (rc == RT_OK) rc = rt_buf_puts(out, ".0");
        return rc;
    }
    rc = rt_buf_puts(out, sign);
    if (rc == RT_OK) rc = rt_buf_append(out, digits, (size_t)decpt);
    if (rc == RT_OK) rc = rt_buf_putc(out, '.');
    if (rc == RT_OK) rc = rt_buf_puts(out, digits + decpt);
    return rc;
}
