/* Request context, dispatch, cookies/flash/formats (task A00; see context.h
 * and 03-application.md "A00: context and dispatch").
 *
 * Ports:
 *  - cookies: tmp/rust-ref/crates/kit/src/cookies.rs (CookieJar set/delete/
 *    finish semantics, Rack header spelling, form escaping, httpdate) plus
 *    rails_compat::cookies::{escape,unescape};
 *  - formats: tmp/rust-ref/crates/kit/src/format.rs and ctx.rs::{formats,
 *    format, respond_to, rendered_format, set_vary_header} with their tests;
 *  - flash/session state: kit/src/session.rs (A00 owns the state container;
 *    A01 owns session persistence and cookie values);
 *  - error mapping: 00-contracts.md "Error translation".
 *
 * The context owns cf_ctx.private_state (cookie jar, flash map, cached
 * negotiation). It never writes a socket: cf_finish_cookies appends
 * Set-Cookie response headers, and the worker submits the finished response.
 */
#include "context.h"

#include "app.h"
#include "http/params.h"
#include "routes.h" /* cf_action_reference_action_not_found (fallback 404) */

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ small utils */

static bool cf_span_eq_span(cf_span a, cf_span b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.ptr, b.ptr, a.len) == 0);
}

static bool cf_span_eq_lit(cf_span a, const char *b) {
    size_t n = strlen(b);
    return a.len == n && (n == 0 || memcmp(a.ptr, b, n) == 0);
}

static bool cf_span_ieq_lit(cf_span a, const char *b) {
    size_t n = strlen(b);
    if (a.len != n) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = a.ptr[i];
        unsigned char d = (unsigned char)b[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (d >= 'A' && d <= 'Z') d += 'a' - 'A';
        if (c != d) return false;
    }
    return true;
}

static bool cf_span_contains(cf_span hay, cf_span needle) {
    if (needle.len == 0) return true;
    if (hay.len < needle.len) return false;
    for (size_t i = 0; i + needle.len <= hay.len; i++) {
        if (memcmp(hay.ptr + i, needle.ptr, needle.len) == 0) return true;
    }
    return false;
}

static bool cf_is_ascii_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

static cf_span cf_span_trim(cf_span s) {
    size_t start = 0, end = s.len;
    while (start < end && cf_is_ascii_space(s.ptr[start])) start++;
    while (end > start && cf_is_ascii_space(s.ptr[end - 1])) end--;
    return (cf_span){s.ptr + start, end - start};
}

static cf_span cf_span_trim_end(cf_span s) {
    size_t end = s.len;
    while (end > 0 && cf_is_ascii_space(s.ptr[end - 1])) end--;
    return (cf_span){s.ptr, end};
}

/* First value of a request header (H01 validates names; comparison is
 * case-insensitive). Returns false when absent. */
static bool cf_request_header(const cf_request *req, const char *name,
                              cf_span *out) {
    size_t n = strlen(name);
    for (size_t i = 0; i < req->header_count; i++) {
        cf_span hname = req->headers[i].name;
        if (hname.len != n) continue;
        bool same = true;
        for (size_t j = 0; j < n; j++) {
            unsigned char c = hname.ptr[j];
            unsigned char d = (unsigned char)name[j];
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c != d) {
                same = false;
                break;
            }
        }
        if (same) {
            *out = req->headers[i].value;
            return true;
        }
    }
    return false;
}

static bool cf_utf8_valid(const unsigned char *p, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = p[i];
        size_t need;
        uint32_t cp;
        if (c < 0x80) {
            i++;
            continue;
        } else if (c >= 0xc2 && c <= 0xdf) {
            need = 1;
            cp = c & 0x1f;
        } else if (c >= 0xe0 && c <= 0xef) {
            need = 2;
            cp = c & 0x0f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            need = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + need >= n) return false;
        for (size_t k = 1; k <= need; k++) {
            unsigned char cc = p[i + k];
            if ((cc & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3f);
        }
        if (need == 2 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) {
            return false;
        }
        if (need == 3 && (cp < 0x10000 || cp > 0x10ffff)) return false;
        i += need + 1;
    }
    return true;
}

/* Copy a span into a fresh NUL-terminated allocation. Empty spans still get a
 * one-byte allocation so callers can rely on ptr != NULL. */
static char *cf_span_dup(cf_span s) {
    char *p = malloc(s.len + 1);
    if (p == NULL) return NULL;
    if (s.len > 0) memcpy(p, s.ptr, s.len);
    p[s.len] = '\0';
    return p;
}

/* ----------------------------------------------------- cookies: calendar */

static int64_t cf_floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

static int64_t cf_floor_mod(int64_t a, int64_t b) {
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}

/* Howard Hinnant's civil-from-days: days since 1970-01-01 -> y/m/d. */
static void cf_civil_from_days(int64_t z, int64_t *year, unsigned *month,
                               unsigned *day) {
    z += 719468;
    int64_t era = cf_floor_div(z >= 0 ? z : z - 146096, 146097);
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    unsigned d = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
    unsigned m = (unsigned)(mp < 10 ? mp + 3 : mp - 9);
    *year = y + (m <= 2 ? 1 : 0);
    *month = m;
    *day = d;
}

static int64_t cf_days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = cf_floor_div(y, 400);
    int64_t yoe = y - era * 400;
    int64_t doy = (int64_t)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5) + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static bool cf_is_leap_year(int64_t y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static unsigned cf_days_in_month(int64_t y, unsigned m) {
    static const unsigned lengths[12] = {31, 28, 31, 30, 31, 30,
                                         31, 31, 30, 31, 30, 31};
    if (m == 2 && cf_is_leap_year(y)) return 29;
    return lengths[m - 1];
}

/* RFC 9110 IMF-fixdate: "Thu, 01 Jan 1970 00:00:00 GMT". */
static void cf_httpdate(int64_t us, char out[64]) {
    static const char *const days[7] = {"Sun", "Mon", "Tue", "Wed",
                                        "Thu", "Fri", "Sat"};
    static const char *const months[12] = {"Jan", "Feb", "Mar", "Apr",
                                           "May", "Jun", "Jul", "Aug",
                                           "Sep", "Oct", "Nov", "Dec"};
    int64_t secs = cf_floor_div(us, 1000000);
    int64_t days_since_epoch = cf_floor_div(secs, 86400);
    int64_t rem = cf_floor_mod(secs, 86400);
    int64_t year;
    unsigned month, day;
    cf_civil_from_days(days_since_epoch, &year, &month, &day);
    size_t wd = (size_t)cf_floor_mod(days_since_epoch + 4, 7);
    unsigned hh = (unsigned)(rem / 3600);
    unsigned mm = (unsigned)((rem % 3600) / 60);
    unsigned ss = (unsigned)(rem % 60);
    snprintf(out, 64, "%s, %02u %s %04d %02u:%02u:%02u GMT", days[wd], day,
             months[month - 1], (int)year, hh, mm, ss);
}

/* Rails `20.years.from_now`: add calendar years, clamping Feb 29 into a
 * non-leap target year (jiff's `years_from`; years_from test in clock.rs). */
static int64_t cf_years_from(int64_t us, int64_t years) {
    int64_t secs = cf_floor_div(us, 1000000);
    int64_t micro = cf_floor_mod(us, 1000000);
    int64_t days_since_epoch = cf_floor_div(secs, 86400);
    int64_t rem = cf_floor_mod(secs, 86400);
    int64_t year;
    unsigned month, day;
    cf_civil_from_days(days_since_epoch, &year, &month, &day);
    year += years;
    unsigned max_day = cf_days_in_month(year, month);
    if (day > max_day) day = max_day;
    int64_t result_days = cf_days_from_civil(year, month, day);
    return result_days * INT64_C(86400) * INT64_C(1000000) +
           rem * INT64_C(1000000) + micro;
}

/* rails_compat::cookies::escape / form_urlencoded::byte_serialize: space
 * becomes '+', alphanumerics and "*-._" pass, everything else %XX (uppercase).
 */
static cf_err cf_cookie_escape(cf_builder *b, cf_span raw) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < raw.len; i++) {
        unsigned char c = raw.ptr[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '*' || c == '-' || c == '.' ||
            c == '_') {
            cf_err rc = cf_builder_append(b, (cf_span){&c, 1});
            if (rc != CF_OK) return rc;
        } else if (c == ' ') {
            cf_err rc = cf_builder_append(b, (cf_span){(const unsigned char *)"+", 1});
            if (rc != CF_OK) return rc;
        } else {
            unsigned char out[3] = {'%', (unsigned char)hex[c >> 4],
                                    (unsigned char)hex[c & 0x0f]};
            cf_err rc = cf_builder_append(b, (cf_span){out, 3});
            if (rc != CF_OK) return rc;
        }
    }
    return CF_OK;
}

static int cf_hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Rack::Utils.unescape semantics as used by parse_cookie_header: '+' is a
 * space, %XX decodes, a malformed escape leaves the raw value untouched, and
 * a non-UTF-8 result keeps the raw value (params::decode_www_form_component
 * then String::from_utf8). Returns an owned value or NULL on allocation
 * failure; `*ok` reports the raw fallback is not needed. */
static char *cf_cookie_unescape(cf_span wire, size_t *out_len) {
    unsigned char *decoded = malloc(wire.len + 1);
    if (decoded == NULL) return NULL;
    size_t n = 0;
    bool malformed = false;
    for (size_t i = 0; i < wire.len;) {
        unsigned char c = wire.ptr[i];
        if (c == '+') {
            decoded[n++] = ' ';
            i++;
        } else if (c == '%') {
            int hi = i + 1 < wire.len ? cf_hex_value(wire.ptr[i + 1]) : -1;
            int lo = i + 2 < wire.len ? cf_hex_value(wire.ptr[i + 2]) : -1;
            if (hi < 0 || lo < 0) {
                malformed = true;
                break;
            }
            decoded[n++] = (unsigned char)((hi << 4) | lo);
            i += 3;
        } else {
            decoded[n++] = c;
            i++;
        }
    }
    if (malformed || !cf_utf8_valid(decoded, n)) {
        free(decoded);
        *out_len = wire.len;
        return cf_span_dup(wire);
    }
    decoded[n] = '\0';
    *out_len = n;
    return (char *)decoded;
}

struct cf_cookie_value {
    char *name;
    size_t name_len;
    char *value;
    size_t value_len;
};

struct cf_pending_set {
    char *name;
    size_t name_len;
    char *value;
    size_t value_len;
    char *path;
    size_t path_len;
    char *domain;
    size_t domain_len;
    bool has_expires;
    int64_t expires_us;
    bool secure;
    bool httponly;
    bool partitioned;
    cf_cookie_samesite samesite;
};

struct cf_pending_delete {
    char *name;
    size_t name_len;
    char *path;
    size_t path_len;
    char *domain;
    size_t domain_len;
    cf_cookie_samesite samesite;
};

struct cf_cookie_jar {
    struct cf_cookie_value *current;
    size_t current_len, current_cap;
    struct cf_pending_set *sets;
    size_t sets_len, sets_cap;
    struct cf_pending_delete *deletes;
    size_t deletes_len, deletes_cap;
};

static size_t cf_cookie_current_find(const struct cf_cookie_jar *jar,
                                     cf_span name) {
    for (size_t i = 0; i < jar->current_len; i++) {
        if (cf_span_eq_span(
                (cf_span){(const unsigned char *)jar->current[i].name,
                          jar->current[i].name_len},
                name)) {
            return i;
        }
    }
    return SIZE_MAX;
}

static size_t cf_cookie_set_find(const struct cf_cookie_jar *jar,
                                 cf_span name) {
    for (size_t i = 0; i < jar->sets_len; i++) {
        if (cf_span_eq_span(
                (cf_span){(const unsigned char *)jar->sets[i].name,
                          jar->sets[i].name_len},
                name)) {
            return i;
        }
    }
    return SIZE_MAX;
}

static size_t cf_cookie_delete_find(const struct cf_cookie_jar *jar,
                                    cf_span name) {
    for (size_t i = 0; i < jar->deletes_len; i++) {
        if (cf_span_eq_span(
                (cf_span){(const unsigned char *)jar->deletes[i].name,
                          jar->deletes[i].name_len},
                name)) {
            return i;
        }
    }
    return SIZE_MAX;
}

static cf_err cf_jar_parse_cookie_header(struct cf_cookie_jar *jar,
                                         cf_span header) {
    size_t part_start = 0;
    int part_index = 0;
    for (size_t i = 0; i <= header.len; i++) {
        if (i != header.len && header.ptr[i] != ';') continue;
        cf_span part = {header.ptr + part_start, i - part_start};
        part_start = i + 1;
        if (part_index > 0) {
            while (part.len > 0 && part.ptr[0] == ' ') {
                part.ptr++;
                part.len--;
            }
        }
        part_index++;
        if (part.len == 0) continue;
        cf_span key = part;
        cf_span wire = {part.ptr + part.len, 0};
        for (size_t k = 0; k < part.len; k++) {
            if (part.ptr[k] == '=') {
                key = (cf_span){part.ptr, k};
                wire = (cf_span){part.ptr + k + 1, part.len - k - 1};
                break;
            }
        }
        /* Rack: first occurrence wins. */
        if (cf_cookie_current_find(jar, key) != SIZE_MAX) continue;
        size_t value_len = 0;
        char *value = cf_cookie_unescape(wire, &value_len);
        if (value == NULL) return CF_NOMEM;
        char *name = cf_span_dup(key);
        if (name == NULL) {
            free(value);
            return CF_NOMEM;
        }
        if (jar->current_len == jar->current_cap) {
            size_t cap = jar->current_cap == 0 ? 8 : jar->current_cap * 2;
            struct cf_cookie_value *grown =
                realloc(jar->current, cap * sizeof *grown);
            if (grown == NULL) {
                free(value);
                free(name);
                return CF_NOMEM;
            }
            jar->current = grown;
            jar->current_cap = cap;
        }
        jar->current[jar->current_len++] =
            (struct cf_cookie_value){name, key.len, value, value_len};
    }
    return CF_OK;
}

static void cf_pending_set_clear(struct cf_pending_set *s) {
    free(s->name);
    free(s->value);
    free(s->path);
    free(s->domain);
    memset(s, 0, sizeof *s);
}

static void cf_pending_delete_clear(struct cf_pending_delete *d) {
    free(d->name);
    free(d->path);
    free(d->domain);
    memset(d, 0, sizeof *d);
}

static void cf_cookie_jar_dispose(struct cf_cookie_jar *jar) {
    for (size_t i = 0; i < jar->current_len; i++) {
        free(jar->current[i].name);
        free(jar->current[i].value);
    }
    free(jar->current);
    for (size_t i = 0; i < jar->sets_len; i++) {
        cf_pending_set_clear(&jar->sets[i]);
    }
    free(jar->sets);
    for (size_t i = 0; i < jar->deletes_len; i++) {
        cf_pending_delete_clear(&jar->deletes[i]);
    }
    free(jar->deletes);
    memset(jar, 0, sizeof *jar);
}

/* Copy name plus every option that is consumed at finish time. path/domain
 * default to empty (meaning absent); the default "/" is applied when the
 * header is built, like Rails' Cookie::new path. */
static cf_err cf_pending_set_assign(struct cf_pending_set *slot, cf_span name,
                                    cf_span value,
                                    const cf_cookie_options *opts) {
    struct cf_pending_set fresh;
    memset(&fresh, 0, sizeof fresh);
    fresh.name = cf_span_dup(name);
    fresh.value = cf_span_dup(value);
    if (fresh.name == NULL || fresh.value == NULL) {
        cf_pending_set_clear(&fresh);
        return CF_NOMEM;
    }
    fresh.name_len = name.len;
    fresh.value_len = value.len;
    if (opts != NULL) {
        if (opts->path.len > 0) {
            fresh.path = cf_span_dup(opts->path);
            if (fresh.path == NULL) {
                cf_pending_set_clear(&fresh);
                return CF_NOMEM;
            }
            fresh.path_len = opts->path.len;
        }
        if (opts->domain.len > 0) {
            fresh.domain = cf_span_dup(opts->domain);
            if (fresh.domain == NULL) {
                cf_pending_set_clear(&fresh);
                return CF_NOMEM;
            }
            fresh.domain_len = opts->domain.len;
        }
        fresh.has_expires = opts->has_expires;
        fresh.expires_us = opts->expires_us;
        fresh.secure = opts->secure;
        fresh.httponly = opts->httponly;
        fresh.partitioned = opts->partitioned;
        fresh.samesite = opts->samesite;
    }
    cf_pending_set_clear(slot);
    *slot = fresh;
    return CF_OK;
}

static cf_err cf_cookie_jar_set(struct cf_cookie_jar *jar, cf_span name,
                                cf_span value,
                                const cf_cookie_options *opts,
                                int64_t now_us) {
    bool explicit_expiry = opts != NULL && opts->has_expires;
    bool permanent = opts != NULL && opts->permanent;
    int64_t expires_us = opts != NULL ? opts->expires_us : 0;
    if (permanent) {
        expires_us = cf_years_from(now_us, 20);
        explicit_expiry = true;
    }

    /* CookieJar::write_value: emit only when the value changed or an expiry
     * was given. */
    size_t cur = cf_cookie_current_find(jar, name);
    bool changed = cur == SIZE_MAX ||
                   !cf_span_eq_span(
                       (cf_span){(const unsigned char *)jar->current[cur].value,
                                 jar->current[cur].value_len},
                       value);
    if (changed) {
        if (cur != SIZE_MAX) {
            free(jar->current[cur].value);
            jar->current[cur].value = cf_span_dup(value);
            if (jar->current[cur].value == NULL) return CF_NOMEM;
            jar->current[cur].value_len = value.len;
        } else {
            if (jar->current_len == jar->current_cap) {
                size_t cap = jar->current_cap == 0 ? 8 : jar->current_cap * 2;
                struct cf_cookie_value *grown =
                    realloc(jar->current, cap * sizeof *grown);
                if (grown == NULL) return CF_NOMEM;
                jar->current = grown;
                jar->current_cap = cap;
            }
            char *ncopy = cf_span_dup(name);
            char *vcopy = cf_span_dup(value);
            if (ncopy == NULL || vcopy == NULL) {
                free(ncopy);
                free(vcopy);
                return CF_NOMEM;
            }
            jar->current[jar->current_len++] =
                (struct cf_cookie_value){ncopy, name.len, vcopy, value.len};
        }
    }
    if (!changed && !explicit_expiry) {
        /* Unchanged without expiry: no header, but a pending delete of the
         * same name is still cancelled by a set (write_value always removes
         * the delete only when it writes; unchanged values keep it). Rails
         * writes nothing at all here. */
        return CF_OK;
    }

    cf_cookie_options resolved;
    if (opts != NULL) {
        resolved = *opts;
    } else {
        memset(&resolved, 0, sizeof resolved);
    }
    resolved.has_expires = explicit_expiry;
    resolved.expires_us = expires_us;
    resolved.permanent = false;

    size_t slot = cf_cookie_set_find(jar, name);
    if (slot == SIZE_MAX) {
        if (jar->sets_len == jar->sets_cap) {
            size_t cap = jar->sets_cap == 0 ? 8 : jar->sets_cap * 2;
            struct cf_pending_set *grown =
                realloc(jar->sets, cap * sizeof *grown);
            if (grown == NULL) return CF_NOMEM;
            jar->sets = grown;
            jar->sets_cap = cap;
        }
        slot = jar->sets_len;
        memset(&jar->sets[slot], 0, sizeof jar->sets[slot]);
        jar->sets_len++;
    }
    cf_err rc = cf_pending_set_assign(&jar->sets[slot], name, value, &resolved);
    if (rc != CF_OK) return rc;

    size_t del = cf_cookie_delete_find(jar, name);
    if (del != SIZE_MAX) {
        cf_pending_delete_clear(&jar->deletes[del]);
        jar->deletes[del] = jar->deletes[jar->deletes_len - 1];
        jar->deletes_len--;
    }
    return CF_OK;
}

static cf_err cf_cookie_jar_delete(struct cf_cookie_jar *jar, cf_span name,
                                   const cf_cookie_options *opts) {
    size_t cur = cf_cookie_current_find(jar, name);
    if (cur == SIZE_MAX) return CF_OK; /* delete is a no-op unless present */

    free(jar->current[cur].name);
    free(jar->current[cur].value);
    jar->current[cur] = jar->current[jar->current_len - 1];
    jar->current_len--;

    size_t slot = cf_cookie_set_find(jar, name);
    if (slot != SIZE_MAX) {
        cf_pending_set_clear(&jar->sets[slot]);
        jar->sets[slot] = jar->sets[jar->sets_len - 1];
        jar->sets_len--;
    }

    slot = cf_cookie_delete_find(jar, name);
    if (slot == SIZE_MAX) {
        if (jar->deletes_len == jar->deletes_cap) {
            size_t cap = jar->deletes_cap == 0 ? 8 : jar->deletes_cap * 2;
            struct cf_pending_delete *grown =
                realloc(jar->deletes, cap * sizeof *grown);
            if (grown == NULL) return CF_NOMEM;
            jar->deletes = grown;
            jar->deletes_cap = cap;
        }
        slot = jar->deletes_len++;
        memset(&jar->deletes[slot], 0, sizeof jar->deletes[slot]);
    } else {
        cf_pending_delete_clear(&jar->deletes[slot]);
        memset(&jar->deletes[slot], 0, sizeof jar->deletes[slot]);
    }
    struct cf_pending_delete *d = &jar->deletes[slot];
    d->name = cf_span_dup(name);
    if (d->name == NULL) return CF_NOMEM;
    d->name_len = name.len;
    if (opts != NULL) {
        if (opts->path.len > 0) {
            d->path = cf_span_dup(opts->path);
            if (d->path == NULL) return CF_NOMEM;
            d->path_len = opts->path.len;
        }
        if (opts->domain.len > 0) {
            d->domain = cf_span_dup(opts->domain);
            if (d->domain == NULL) return CF_NOMEM;
            d->domain_len = opts->domain.len;
        }
        d->samesite = opts->samesite;
    }
    return CF_OK;
}

static const char *cf_samesite_attribute(cf_cookie_samesite ss) {
    switch (ss) {
    case CF_COOKIE_SAMESITE_STRICT:
        return "; samesite=strict";
    case CF_COOKIE_SAMESITE_NONE:
        return "; samesite=none";
    case CF_COOKIE_SAMESITE_DEFAULT:
    case CF_COOKIE_SAMESITE_LAX:
    default:
        return "; samesite=lax";
    }
}

static cf_err cf_cookie_append_span(cf_builder *b, cf_span s) {
    if (s.len == 0) return CF_OK;
    return cf_builder_append(b, s);
}

static cf_err cf_cookie_append_lit(cf_builder *b, const char *s) {
    return cf_builder_append(
        b, (cf_span){(const unsigned char *)s, strlen(s)});
}

/* Rack::Utils.set_cookie_header. */
static cf_err cf_cookie_build_set(cf_builder *b,
                                  const struct cf_pending_set *s) {
    cf_err rc = cf_cookie_append_span(
        b, (cf_span){(const unsigned char *)s->name, s->name_len});
    if (rc == CF_OK) rc = cf_cookie_append_lit(b, "=");
    if (rc == CF_OK) {
        rc = cf_cookie_escape(b, (cf_span){(const unsigned char *)s->value,
                                           s->value_len});
    }
    if (rc == CF_OK && s->domain_len > 0) {
        rc = cf_cookie_append_lit(b, "; domain=");
        if (rc == CF_OK) {
            rc = cf_cookie_append_span(
                b, (cf_span){(const unsigned char *)s->domain, s->domain_len});
        }
    }
    if (rc == CF_OK) {
        rc = cf_cookie_append_lit(b, "; path=");
        if (rc == CF_OK) {
            if (s->path_len > 0) {
                rc = cf_cookie_append_span(
                    b, (cf_span){(const unsigned char *)s->path, s->path_len});
            } else {
                rc = cf_cookie_append_lit(b, "/");
            }
        }
    }
    if (rc == CF_OK && s->has_expires) {
        char httpdate[64];
        cf_httpdate(s->expires_us, httpdate);
        rc = cf_cookie_append_lit(b, "; expires=");
        if (rc == CF_OK) rc = cf_cookie_append_lit(b, httpdate);
    }
    if (rc == CF_OK && s->secure) rc = cf_cookie_append_lit(b, "; secure");
    if (rc == CF_OK && s->httponly) rc = cf_cookie_append_lit(b, "; httponly");
    if (rc == CF_OK) rc = cf_cookie_append_lit(b, cf_samesite_attribute(s->samesite));
    if (rc == CF_OK && s->partitioned) {
        rc = cf_cookie_append_lit(b, "; partitioned");
    }
    return rc;
}

/* Rack::Utils.delete_set_cookie_header. */
static cf_err cf_cookie_build_delete(cf_builder *b,
                                     const struct cf_pending_delete *d) {
    cf_err rc = cf_cookie_append_span(
        b, (cf_span){(const unsigned char *)d->name, d->name_len});
    if (rc == CF_OK) rc = cf_cookie_append_lit(b, "=");
    if (rc == CF_OK && d->domain_len > 0) {
        rc = cf_cookie_append_lit(b, "; domain=");
        if (rc == CF_OK) {
            rc = cf_cookie_append_span(
                b, (cf_span){(const unsigned char *)d->domain, d->domain_len});
        }
    }
    if (rc == CF_OK) {
        rc = cf_cookie_append_lit(b, "; path=");
        if (rc == CF_OK) {
            if (d->path_len > 0) {
                rc = cf_cookie_append_span(
                    b, (cf_span){(const unsigned char *)d->path, d->path_len});
            } else {
                rc = cf_cookie_append_lit(b, "/");
            }
        }
    }
    if (rc == CF_OK) {
        rc = cf_cookie_append_lit(
            b, "; max-age=0; expires=Thu, 01 Jan 1970 00:00:00 GMT");
    }
    if (rc == CF_OK) rc = cf_cookie_append_lit(b, cf_samesite_attribute(d->samesite));
    return rc;
}

static bool cf_host_is_onion(const cf_request *req) {
    cf_span host;
    if (!cf_request_header(req, "host", &host)) return false;
    const char *suffix = ".onion";
    size_t n = strlen(suffix);
    return host.len >= n && memcmp(host.ptr + host.len - n, suffix, n) == 0;
}

/* ------------------------------------------------------------- flash map */

struct cf_flash_entry {
    char *key;
    size_t key_len;
    char *value;
    size_t value_len;
};

struct cf_flash_map {
    struct cf_flash_entry *entries;
    size_t len, cap;
};

static size_t cf_flash_find(const struct cf_flash_map *map, cf_span key) {
    for (size_t i = 0; i < map->len; i++) {
        if (cf_span_eq_span((cf_span){
                                (const unsigned char *)map->entries[i].key,
                                map->entries[i].key_len},
                            key)) {
            return i;
        }
    }
    return SIZE_MAX;
}

static void cf_flash_dispose(struct cf_flash_map *map) {
    for (size_t i = 0; i < map->len; i++) {
        free(map->entries[i].key);
        free(map->entries[i].value);
    }
    free(map->entries);
    memset(map, 0, sizeof *map);
}

/* --------------------------------------------------------------- formats */

const cf_format cf_format_html = {"html", "text/html"};
const cf_format cf_format_text = {"text", "text/plain"};
const cf_format cf_format_js = {"js", "text/javascript"};
const cf_format cf_format_json = {"json", "application/json"};
const cf_format cf_format_turbo_stream = {"turbo_stream",
                                          "text/vnd.turbo-stream.html"};
const cf_format cf_format_xml = {"xml", "application/xml"};
const cf_format cf_format_all = {"*/*", "*/*"};

/* The full format.rs REGISTERED table; entries not exported above stay
 * static. Metadata (synonyms/extensions) is parallel to this table. */
static const cf_format cf_mime_css = {"css", "text/css"};
static const cf_format cf_mime_ics = {"ics", "text/calendar"};
static const cf_format cf_mime_csv = {"csv", "text/csv"};
static const cf_format cf_mime_vcf = {"vcf", "text/vcard"};
static const cf_format cf_mime_vtt = {"vtt", "text/vtt"};
static const cf_format cf_mime_md = {"md", "text/markdown"};
static const cf_format cf_mime_png = {"png", "image/png"};
static const cf_format cf_mime_jpeg = {"jpeg", "image/jpeg"};
static const cf_format cf_mime_gif = {"gif", "image/gif"};
static const cf_format cf_mime_bmp = {"bmp", "image/bmp"};
static const cf_format cf_mime_tiff = {"tiff", "image/tiff"};
static const cf_format cf_mime_svg = {"svg", "image/svg+xml"};
static const cf_format cf_mime_webp = {"webp", "image/webp"};
static const cf_format cf_mime_mpeg = {"mpeg", "video/mpeg"};
static const cf_format cf_mime_mp3 = {"mp3", "audio/mpeg"};
static const cf_format cf_mime_ogg = {"ogg", "audio/ogg"};
static const cf_format cf_mime_m4a = {"m4a", "audio/aac"};
static const cf_format cf_mime_webm = {"webm", "video/webm"};
static const cf_format cf_mime_mp4 = {"mp4", "video/mp4"};
static const cf_format cf_mime_otf = {"otf", "font/otf"};
static const cf_format cf_mime_ttf = {"ttf", "font/ttf"};
static const cf_format cf_mime_woff = {"woff", "font/woff"};
static const cf_format cf_mime_woff2 = {"woff2", "font/woff2"};
static const cf_format cf_mime_rss = {"rss", "application/rss+xml"};
static const cf_format cf_mime_atom = {"atom", "application/atom+xml"};
static const cf_format cf_mime_yaml = {"yaml", "application/x-yaml"};
static const cf_format cf_mime_multipart = {"multipart_form",
                                            "multipart/form-data"};
static const cf_format cf_mime_urlencoded = {
    "url_encoded_form", "application/x-www-form-urlencoded"};
static const cf_format cf_mime_pdf = {"pdf", "application/pdf"};
static const cf_format cf_mime_zip = {"zip", "application/zip"};
static const cf_format cf_mime_gzip = {"gzip", "application/gzip"};

static const char *const syn_html[] = {"application/xhtml+xml"};
static const char *const syn_js[] = {"application/javascript",
                                     "application/x-javascript"};
static const char *const syn_m4a[] = {"audio/mp4"};
static const char *const syn_xml[] = {"text/xml", "application/x-xml"};
static const char *const syn_json[] = {"text/x-json", "application/jsonrequest",
                                       "application/problem+json"};
static const char *const syn_yaml[] = {"text/yaml"};
static const char *const syn_gzip[] = {"application/x-gzip"};

static const char *const ext_html[] = {"xhtml"};
static const char *const ext_text[] = {"txt"};
static const char *const ext_vtt[] = {"vtt"};
static const char *const ext_md[] = {"md", "markdown"};
static const char *const ext_png[] = {"png"};
static const char *const ext_jpeg[] = {"jpg", "jpeg", "jpe", "pjpeg"};
static const char *const ext_gif[] = {"gif"};
static const char *const ext_bmp[] = {"bmp"};
static const char *const ext_tiff[] = {"tif", "tiff"};
static const char *const ext_webp[] = {"webp"};
static const char *const ext_mpeg[] = {"mpg", "mpeg", "mpe"};
static const char *const ext_mp3[] = {"mp1", "mp2", "mp3"};
static const char *const ext_ogg[] = {"oga", "ogg", "spx", "opus"};
static const char *const ext_m4a[] = {"m4a", "mpg4", "aac"};
static const char *const ext_webm[] = {"webm"};
static const char *const ext_mp4[] = {"mp4", "m4v"};
static const char *const ext_otf[] = {"otf"};
static const char *const ext_ttf[] = {"ttf"};
static const char *const ext_woff[] = {"woff"};
static const char *const ext_woff2[] = {"woff2"};
static const char *const ext_yaml[] = {"yml", "yaml"};
static const char *const ext_pdf[] = {"pdf"};
static const char *const ext_zip[] = {"zip"};
static const char *const ext_gzip[] = {"gz"};

struct cf_mime_meta {
    const cf_format *fmt;
    const char *const *syn;
    size_t syn_count;
    const char *const *ext;
    size_t ext_count;
};

#define CF_SYN_COUNT(syn) (sizeof(syn) / sizeof((syn)[0]))
#define CF_MIME(fmt, syn, ext) \
    { &fmt, syn, CF_SYN_COUNT(syn), ext, CF_SYN_COUNT(ext) }
#define CF_MIME_EXT(fmt, ext) { &fmt, NULL, 0, ext, CF_SYN_COUNT(ext) }
#define CF_MIME_SYN(fmt, syn) { &fmt, syn, CF_SYN_COUNT(syn), NULL, 0 }
#define CF_MIME_PLAIN(fmt) { &fmt, NULL, 0, NULL, 0 }

/* format.rs REGISTERED, in registration order (order matters for the
 * trailing-star expansion). */
static const struct cf_mime_meta cf_registered[] = {
    CF_MIME(cf_format_html, syn_html, ext_html),
    CF_MIME_EXT(cf_format_text, ext_text),
    CF_MIME_SYN(cf_format_js, syn_js),
    CF_MIME_PLAIN(cf_mime_css),
    CF_MIME_PLAIN(cf_mime_ics),
    CF_MIME_PLAIN(cf_mime_csv),
    CF_MIME_PLAIN(cf_mime_vcf),
    CF_MIME_EXT(cf_mime_vtt, ext_vtt),
    CF_MIME_EXT(cf_mime_md, ext_md),
    CF_MIME_EXT(cf_mime_png, ext_png),
    CF_MIME_EXT(cf_mime_jpeg, ext_jpeg),
    CF_MIME_EXT(cf_mime_gif, ext_gif),
    CF_MIME_EXT(cf_mime_bmp, ext_bmp),
    CF_MIME_EXT(cf_mime_tiff, ext_tiff),
    CF_MIME_PLAIN(cf_mime_svg),
    CF_MIME_EXT(cf_mime_webp, ext_webp),
    CF_MIME_EXT(cf_mime_mpeg, ext_mpeg),
    CF_MIME_EXT(cf_mime_mp3, ext_mp3),
    CF_MIME_EXT(cf_mime_ogg, ext_ogg),
    CF_MIME(cf_mime_m4a, syn_m4a, ext_m4a),
    CF_MIME_EXT(cf_mime_webm, ext_webm),
    CF_MIME_EXT(cf_mime_mp4, ext_mp4),
    CF_MIME_EXT(cf_mime_otf, ext_otf),
    CF_MIME_EXT(cf_mime_ttf, ext_ttf),
    CF_MIME_EXT(cf_mime_woff, ext_woff),
    CF_MIME_EXT(cf_mime_woff2, ext_woff2),
    CF_MIME_SYN(cf_format_xml, syn_xml),
    CF_MIME_PLAIN(cf_mime_rss),
    CF_MIME_PLAIN(cf_mime_atom),
    CF_MIME(cf_mime_yaml, syn_yaml, ext_yaml),
    CF_MIME_PLAIN(cf_mime_multipart),
    CF_MIME_PLAIN(cf_mime_urlencoded),
    CF_MIME_SYN(cf_format_json, syn_json),
    CF_MIME_EXT(cf_mime_pdf, ext_pdf),
    CF_MIME_EXT(cf_mime_zip, ext_zip),
    CF_MIME(cf_mime_gzip, syn_gzip, ext_gzip),
    CF_MIME_PLAIN(cf_format_turbo_stream),
};
#undef CF_MIME
#undef CF_MIME_EXT
#undef CF_MIME_SYN
#undef CF_MIME_PLAIN
#undef CF_SYN_COUNT

#define CF_REGISTERED_COUNT \
    (sizeof(cf_registered) / sizeof(cf_registered[0]))

/* Mime::Display equality is by symbol. */
static bool cf_format_is(const cf_format *a, const cf_format *b) {
    return a != NULL && b != NULL && strcmp(a->symbol, b->symbol) == 0;
}

const cf_format *cf_format_lookup_symbol(const char *symbol) {
    if (symbol == NULL) return NULL;
    for (size_t i = 0; i < CF_REGISTERED_COUNT; i++) {
        const cf_format *fmt = cf_registered[i].fmt;
        if (strcmp(fmt->symbol, symbol) == 0) return fmt;
    }
    if (strcmp(cf_format_all.symbol, symbol) == 0) return &cf_format_all;
    return NULL;
}

static const cf_format *cf_lookup_by_extension(cf_span ext) {
    if (ext.len == 0) return NULL;
    for (size_t i = 0; i < CF_REGISTERED_COUNT; i++) {
        const cf_format *fmt = cf_registered[i].fmt;
        if (cf_span_eq_lit(ext, fmt->symbol)) return fmt;
        for (size_t k = 0; k < cf_registered[i].ext_count; k++) {
            if (cf_span_eq_lit(ext, cf_registered[i].ext[k])) return fmt;
        }
    }
    return NULL;
}

static const cf_format *cf_lookup_exact(cf_span s) {
    for (size_t i = 0; i < CF_REGISTERED_COUNT; i++) {
        const cf_format *fmt = cf_registered[i].fmt;
        if (cf_span_eq_lit(s, fmt->string)) return fmt;
        for (size_t k = 0; k < cf_registered[i].syn_count; k++) {
            if (cf_span_eq_lit(s, cf_registered[i].syn[k])) return fmt;
        }
    }
    return NULL;
}

static bool cf_name_ok(cf_span s) {
    if (s.len == 0 || s.len > 127) return false;
    unsigned char first = s.ptr[0];
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') ||
          (first >= '0' && first <= '9'))) {
        return false;
    }
    static const char *const extra = "!#$&-^_.+";
    for (size_t i = 0; i < s.len; i++) {
        unsigned char c = s.ptr[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || strchr(extra, (int)c) != NULL;
        if (!ok) return false;
    }
    return true;
}

/* format.rs valid_mime_type. */
static bool cf_valid_mime_type(cf_span string) {
    cf_span base = string;
    for (size_t i = 0; i < string.len; i++) {
        if (string.ptr[i] == ';') {
            base = (cf_span){string.ptr, i};
            break;
        }
    }
    base = cf_span_trim_end(base);
    if (cf_span_eq_lit(base, "*/*")) return true;
    for (size_t i = 0; i < base.len; i++) {
        if (base.ptr[i] != '/') continue;
        cf_span kind = {base.ptr, i};
        cf_span sub = {base.ptr + i + 1, base.len - i - 1};
        if (!cf_name_ok(kind)) return false;
        if (cf_span_eq_lit(sub, "*")) return true;
        return cf_name_ok(sub);
    }
    return false;
}

/* format.rs Mime::Type.lookup: exact type or synonym, then the part before
 * ';', then the wildcard. Ok(None) is a valid but unregistered type; invalid
 * input returns CF_INVALID. */
static cf_err cf_lookup_mime(cf_span string, const cf_format **out) {
    *out = NULL;
    const cf_format *exact = cf_lookup_exact(string);
    if (exact != NULL) {
        *out = exact;
        return CF_OK;
    }
    cf_span base = string;
    for (size_t i = 0; i < string.len; i++) {
        if (string.ptr[i] == ';') {
            base = (cf_span){string.ptr, i};
            break;
        }
    }
    base = cf_span_trim_end(base);
    exact = cf_lookup_exact(base);
    if (exact != NULL) {
        *out = exact;
        return CF_OK;
    }
    if (cf_span_eq_lit(base, "*/*")) {
        *out = &cf_format_all;
        return CF_OK;
    }
    if (cf_valid_mime_type(string)) return CF_OK;
    return CF_INVALID;
}

/* format.rs `matches`: pattern appears in the type or one of its synonyms. */
static bool cf_mime_matches(const struct cf_mime_meta *m, cf_span pattern) {
    const cf_format *fmt = m->fmt;
    if (cf_span_contains((cf_span){(const unsigned char *)fmt->string,
                                   strlen(fmt->string)},
                         pattern)) {
        return true;
    }
    for (size_t i = 0; i < m->syn_count; i++) {
        if (cf_span_contains(
                (cf_span){(const unsigned char *)m->syn[i], strlen(m->syn[i])},
                pattern)) {
            return true;
        }
    }
    return false;
}

/* format.rs trailing_star: a "text/(star)" or "application/(star)" media
 * range matches its prefix (the prefix keeps the trailing '/', like the Rust
 * slice). */
static bool cf_star_prefix(cf_span accept, cf_span *prefix) {
    if (accept.len >= 6 && memcmp(accept.ptr, "text/*", 6) == 0) {
        *prefix = (cf_span){accept.ptr, 5};
        return true;
    }
    if (accept.len >= 13 && memcmp(accept.ptr, "application/*", 13) == 0) {
        *prefix = (cf_span){accept.ptr, 12};
        return true;
    }
    return false;
}

/* Expand a trailing-star prefix into `out` (capacity cap) in registration
 * order; returns the count. */
static size_t cf_expand_star(cf_span prefix, const cf_format **out,
                             size_t cap) {
    size_t count = 0;
    for (size_t i = 0; i < CF_REGISTERED_COUNT && count < cap; i++) {
        if (cf_mime_matches(&cf_registered[i], prefix)) {
            out[count++] = cf_registered[i].fmt;
        }
    }
    return count;
}

#define CF_MIME_COUNT_MAX 64

/* ------------------------------------------------- Accept header parsing */

struct cf_accept_item {
    size_t index;
    cf_span name;
    double q;
};

struct cf_accept_list {
    struct cf_accept_item *items;
    size_t len, cap;
};

static cf_err cf_accept_push(struct cf_accept_list *list, size_t index,
                             cf_span name, double q) {
    if (list->len == list->cap) {
        size_t cap = list->cap == 0 ? 8 : list->cap * 2;
        struct cf_accept_item *grown =
            realloc(list->items, cap * sizeof *grown);
        if (grown == NULL) return CF_NOMEM;
        list->items = grown;
        list->cap = cap;
    }
    list->items[list->len++] = (struct cf_accept_item){index, name, q};
    return CF_OK;
}

/* PARAMETER_SEPARATOR_REGEXP = /;\s*q="?/ location: start index of ';' and
 * the index just past the separator prefix. */
static bool cf_find_q_separator(cf_span s, size_t *start, size_t *end) {
    for (size_t i = 0; i < s.len; i++) {
        if (s.ptr[i] != ';') continue;
        size_t j = i + 1;
        while (j < s.len && cf_is_ascii_space(s.ptr[j])) j++;
        if (j + 1 >= s.len || s.ptr[j] != 'q' || s.ptr[j + 1] != '=') {
            continue;
        }
        *start = i;
        *end = (j + 2 < s.len && s.ptr[j + 2] == '"') ? j + 3 : j + 2;
        return true;
    }
    return false;
}

/* String#to_f via the ruby_compat behavior the reference tests pin:
 * leading whitespace and a sign, hexadecimal only after a sign, underscores
 * between digits, prefix parsing that stops at the first junk byte. */
static double cf_ruby_to_f(cf_span s) {
    size_t i = 0;
    while (i < s.len && cf_is_ascii_space(s.ptr[i])) i++;
    bool sign_present = false;
    double sign = 1.0;
    if (i < s.len && (s.ptr[i] == '+' || s.ptr[i] == '-')) {
        sign_present = true;
        if (s.ptr[i] == '-') sign = -1.0;
        i++;
    }
    if (sign_present && i + 1 < s.len && s.ptr[i] == '0' &&
        (s.ptr[i + 1] == 'x' || s.ptr[i + 1] == 'X')) {
        i += 2;
        uint64_t v = 0;
        bool any = false;
        while (i < s.len) {
            int d = cf_hex_value(s.ptr[i]);
            if (d < 0) {
                if (s.ptr[i] == '_' && any) {
                    i++;
                    continue;
                }
                break;
            }
            v = v * 16 + (uint64_t)d;
            any = true;
            i++;
        }
        return sign * (double)v;
    }
    char clean[64];
    size_t n = 0;
    while (i < s.len && n + 1 < sizeof clean) {
        unsigned char c = s.ptr[i];
        if (c == '_') {
            bool prev_digit = n > 0 &&
                              ((clean[n - 1] >= '0' && clean[n - 1] <= '9'));
            bool next_digit = i + 1 < s.len && s.ptr[i + 1] >= '0' &&
                              s.ptr[i + 1] <= '9';
            if (prev_digit && next_digit) {
                i++;
                continue;
            }
            break;
        }
        if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
            c == '+' || c == '-') {
            clean[n++] = (char)c;
            i++;
            continue;
        }
        break;
    }
    if (n == 0) return 0.0;
    clean[n] = '\0';
    return sign * strtod(clean, NULL);
}

/* String#split with the q separator, reduced to the two fields the reference
 * reads: the media range (fields[0]) and the optional q text (fields[1]).
 * Ruby drops trailing empty fields, so a present-but-empty fields[1] counts
 * only when something follows it. */
static bool cf_split_accept_item(cf_span item, cf_span *params, cf_span *qtext,
                                 bool *has_q) {
    size_t qstart, qend;
    if (!cf_find_q_separator(item, &qstart, &qend)) {
        *params = cf_span_trim(item);
        *qtext = (cf_span){NULL, 0};
        *has_q = false;
        return params->len != 0;
    }
    *params = cf_span_trim((cf_span){item.ptr, qstart});
    cf_span rest = {item.ptr + qend, item.len - qend};
    size_t s2start, s2end;
    if (cf_find_q_separator(rest, &s2start, &s2end)) {
        *qtext = (cf_span){rest.ptr, s2start};
        *has_q = s2start > 0 || rest.len > s2end;
    } else {
        *qtext = rest;
        *has_q = rest.len != 0;
    }
    return params->len != 0;
}

/* ACCEPT_HEADER_REGEXP scanning: quoted strings may contain commas. */
struct cf_accept_items {
    cf_span *items;
    size_t count, cap;
};

static cf_err cf_accept_items_push(struct cf_accept_items *out, cf_span item) {
    if (out->count == out->cap) {
        size_t cap = out->cap == 0 ? 8 : out->cap * 2;
        cf_span *grown = realloc(out->items, cap * sizeof *grown);
        if (grown == NULL) return CF_NOMEM;
        out->items = grown;
        out->cap = cap;
    }
    out->items[out->count++] = item;
    return CF_OK;
}

static cf_err cf_scan_accept_items(cf_span header,
                                   struct cf_accept_items *out) {
    out->items = NULL;
    out->count = 0;
    out->cap = 0;
    size_t i = 0;
    while (i < header.len) {
        unsigned char c = header.ptr[i];
        if (c == ',' || cf_is_ascii_space(c) || c == '"') {
            i++;
            continue;
        }
        size_t start = i;
        i++;
        while (i < header.len && header.ptr[i] != ',') {
            if (header.ptr[i] == '"') {
                size_t close = 0;
                bool found = false;
                for (size_t k = i + 1; k < header.len; k++) {
                    if (header.ptr[k] == '"') {
                        close = k;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    i = header.len;
                    break;
                }
                i = close + 1;
            } else {
                i++;
            }
        }
        cf_err rc =
            cf_accept_items_push(out, (cf_span){header.ptr + start, i - start});
        if (rc != CF_OK) {
            free(out->items);
            memset(out, 0, sizeof *out);
            return rc;
        }
    }
    return CF_OK;
}

static void cf_sort_xml(struct cf_accept_list *list) {
    size_t text_xml = SIZE_MAX, app_xml = SIZE_MAX;
    for (size_t i = 0; i < list->len; i++) {
        if (cf_span_eq_lit(list->items[i].name, "text/xml") &&
            text_xml == SIZE_MAX) {
            text_xml = i;
        }
        if (cf_span_eq_lit(list->items[i].name, "application/xml") &&
            app_xml == SIZE_MAX) {
            app_xml = i;
        }
    }
    if (text_xml != SIZE_MAX && app_xml != SIZE_MAX) {
        list->items[app_xml].q = fmax(list->items[app_xml].q,
                                      list->items[text_xml].q);
        if (app_xml > text_xml) {
            struct cf_accept_item tmp = list->items[app_xml];
            list->items[app_xml] = list->items[text_xml];
            list->items[text_xml] = tmp;
            size_t swap = app_xml;
            app_xml = text_xml;
            text_xml = swap;
        }
        for (size_t k = text_xml; k + 1 < list->len; k++) {
            list->items[k] = list->items[k + 1];
        }
        list->len--;
        if (app_xml > text_xml) app_xml--;
    } else if (text_xml != SIZE_MAX) {
        list->items[text_xml].name =
            (cf_span){(const unsigned char *)"application/xml", 15};
        /* app_xml stays SIZE_MAX: the reference does not run the +xml pass
         * when only text/xml was present. */
    }
    if (app_xml != SIZE_MAX) {
        double app_q = list->items[app_xml].q;
        for (size_t idx = app_xml + 1; idx < list->len; idx++) {
            if (list->items[idx].q < app_q) break;
            cf_span name = list->items[idx].name;
            const char *suffix = "+xml";
            size_t sn = strlen(suffix);
            bool ends = name.len >= sn &&
                        memcmp(name.ptr + name.len - sn, suffix, sn) == 0;
            if (ends) {
                struct cf_accept_item tmp = list->items[app_xml];
                list->items[app_xml] = list->items[idx];
                list->items[idx] = tmp;
                app_xml = idx;
            }
        }
    }
}

/* Mime::Type.parse(accept), restricted like MimeNegotiation#formats. The
 * result is an owned array of borrowed descriptors; *out stays NULL on
 * failure. */
static cf_err cf_parse_accept(cf_span header, const cf_format ***out,
                              size_t *out_count) {
    *out = NULL;
    *out_count = 0;
    const cf_format **formats = NULL;
    size_t formats_len = 0, formats_cap = 0;

    bool has_comma = false;
    for (size_t i = 0; i < header.len; i++) {
        if (header.ptr[i] == ',') {
            has_comma = true;
            break;
        }
    }

    if (!has_comma) {
        cf_span h = header;
        size_t qstart, qend;
        if (cf_find_q_separator(header, &qstart, &qend)) {
            h = (cf_span){header.ptr, qstart};
        }
        h = cf_span_trim(h);
        if (h.len == 0) {
            *out = NULL;
            *out_count = 0;
            return CF_OK;
        }
        const cf_format *expanded[CF_MIME_COUNT_MAX];
        cf_span prefix;
        if (cf_star_prefix(h, &prefix)) {
            size_t expanded_count = cf_expand_star(prefix, expanded,
                                                   CF_MIME_COUNT_MAX);
            if (expanded_count > 0) {
                formats = malloc(expanded_count * sizeof *formats);
                if (formats == NULL) return CF_NOMEM;
                memcpy(formats, expanded, expanded_count * sizeof *formats);
                *out = formats;
                *out_count = expanded_count;
                return CF_OK;
            }
        }
        const cf_format *fmt = NULL;
        cf_err rc = cf_lookup_mime(h, &fmt);
        if (rc != CF_OK) return rc;
        if (fmt != NULL) {
            formats = malloc(sizeof *formats);
            if (formats == NULL) return CF_NOMEM;
            formats[0] = fmt;
            *out = formats;
            *out_count = 1;
        }
        return CF_OK;
    }

    struct cf_accept_items items;
    cf_err rc = cf_scan_accept_items(header, &items);
    if (rc != CF_OK) return rc;
    struct cf_accept_list list;
    memset(&list, 0, sizeof list);
    size_t index = 0;
    for (size_t i = 0; i < items.count && rc == CF_OK; i++) {
        cf_span params, qtext;
        bool has_q;
        if (!cf_split_accept_item(items.items[i], &params, &qtext, &has_q)) {
            continue;
        }
        cf_span prefix;
        cf_span names[CF_MIME_COUNT_MAX];
        size_t name_count = 0;
        if (cf_star_prefix(params, &prefix)) {
            for (size_t k = 0; k < CF_REGISTERED_COUNT &&
                               name_count < CF_MIME_COUNT_MAX;
                 k++) {
                if (cf_mime_matches(&cf_registered[k], prefix)) {
                    const char *s = cf_registered[k].fmt->string;
                    names[name_count++] =
                        (cf_span){(const unsigned char *)s, strlen(s)};
                }
            }
        } else {
            names[0] = params;
            name_count = 1;
        }
        for (size_t k = 0; k < name_count && rc == CF_OK; k++) {
            double q;
            if (has_q) {
                q = cf_ruby_to_f(qtext);
            } else if (cf_span_eq_lit(names[k], "*/*")) {
                q = 0.0;
            } else {
                q = 1.0;
            }
            rc = cf_accept_push(&list, index, names[k], trunc(q * 100.0));
            index++;
        }
    }
    free(items.items);
    if (rc != CF_OK) {
        free(list.items);
        return rc;
    }
    /* sort_by: q descending, then the original index (Ruby's sort_by with an
     * explicit index tiebreak). */
    for (size_t i = 1; i < list.len; i++) {
        struct cf_accept_item key = list.items[i];
        size_t j = i;
        while (j > 0 && (list.items[j - 1].q < key.q ||
                         (list.items[j - 1].q == key.q &&
                          list.items[j - 1].index > key.index))) {
            list.items[j] = list.items[j - 1];
            j--;
        }
        list.items[j] = key;
    }
    cf_sort_xml(&list);

    for (size_t i = 0; i < list.len; i++) {
        const cf_format *fmt = NULL;
        rc = cf_lookup_mime(list.items[i].name, &fmt);
        if (rc != CF_OK) break;
        if (fmt == NULL) continue;
        bool dup = false;
        for (size_t k = 0; k < formats_len; k++) {
            if (cf_format_is(formats[k], fmt)) {
                dup = true;
                break;
            }
        }
        if (dup) continue;
        if (formats_len == formats_cap) {
            size_t cap = formats_cap == 0 ? 8 : formats_cap * 2;
            const cf_format **grown = realloc(formats, cap * sizeof *grown);
            if (grown == NULL) {
                rc = CF_NOMEM;
                break;
            }
            formats = grown;
            formats_cap = cap;
        }
        formats[formats_len++] = fmt;
    }
    free(list.items);
    if (rc != CF_OK) {
        free(formats);
        return rc;
    }
    *out = formats;
    *out_count = formats_len;
    return CF_OK;
}

static cf_err cf_content_mime_type(cf_span content_type, bool present,
                                   const cf_format **out) {
    *out = NULL;
    if (!present) return CF_OK;
    cf_span base = content_type;
    for (size_t i = 0; i < content_type.len; i++) {
        unsigned char c = content_type.ptr[i];
        if (c == ',' || c == ';') {
            base = (cf_span){content_type.ptr, i};
            break;
        }
    }
    base = cf_span_trim(base);
    if (base.len == 0) return CF_OK;
    /* to_ascii_lowercase */
    unsigned char *lower = malloc(base.len);
    if (lower == NULL) return CF_NOMEM;
    for (size_t i = 0; i < base.len; i++) {
        unsigned char c = base.ptr[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (unsigned char)(c + ('a' - 'A')) : c;
    }
    cf_err rc = cf_lookup_mime((cf_span){lower, base.len}, out);
    free(lower);
    return rc;
}

/* `compacted contains needle`, where the compacted string is Accept with all
 * ASCII whitespace removed (String#split(char::is_whitespace).collect()). */
static bool cf_compact_contains(cf_span s, const char *needle) {
    size_t k = 0;
    for (size_t i = 0; i < s.len; i++) {
        if (cf_is_ascii_space(s.ptr[i])) continue;
        if (s.ptr[i] == (unsigned char)needle[k]) {
            k++;
            if (needle[k] == '\0') return true;
        } else {
            k = s.ptr[i] == (unsigned char)needle[0] ? 1 : 0;
        }
    }
    return false;
}

/* format.rs should_apply_vary_header. */
static bool cf_should_apply_vary(bool has_format_param, bool has_accept,
                                 cf_span accept, bool has_content_type,
                                 cf_span content_type, bool xhr) {
    if (has_format_param) return false;
    /* Only touch `accept` when the header is present: an absent header must
     * never dereference the caller's (uninitialized) span (A00 verify §8). */
    bool present = has_accept && cf_span_trim(accept).len > 0;
    bool ct_present = has_content_type && content_type.len > 0;
    if (xhr && (present || ct_present)) return true;
    if (!present) return false;
    /* BROWSER_LIKE_ACCEPTS = /,\s*\*\/\*|\*\/\*\s*,/ */
    bool browser_like = cf_compact_contains(accept, ",*/*") ||
                        cf_compact_contains(accept, "*/*,");
    return !browser_like;
}

/* ------------------------------------------------------- private context */

struct cf_ctx_state {
    cf_params *params; /* owned merged body/query/path tree */
    struct cf_cookie_jar jar;
    struct cf_flash_map flash;
    /* cached negotiation */
    bool formats_done;
    cf_err formats_rc;
    const cf_format **formats;
    size_t formats_count;
    bool rendered_set;
    const cf_format *rendered;
    unsigned error_status; /* 0 = none */
};

void cf_ctx_set_error_status(cf_ctx *ctx, unsigned status) {
    if (ctx == NULL || ctx->private_state == NULL) return;
    struct cf_ctx_state *st = ctx->private_state;
    st->error_status = status;
}

static struct cf_ctx_state *cf_ctx_state(const cf_ctx *ctx) {
    return ctx != NULL ? ctx->private_state : NULL;
}

const cf_params *cf_ctx_params(const cf_ctx *ctx) {
    struct cf_ctx_state *st = cf_ctx_state(ctx);
    return st != NULL ? st->params : NULL;
}

const cf_param *cf_ctx_param(const cf_ctx *ctx, cf_span name) {
    return cf_param_get(cf_ctx_params(ctx), name);
}

static bool cf_ctx_format_param(const cf_ctx *ctx, cf_span *out) {
    struct cf_ctx_state *st = cf_ctx_state(ctx);
    if (st == NULL || st->params == NULL) return false;
    const cf_param *param =
        cf_param_get(st->params, (cf_span){(const unsigned char *)"format", 6});
    if (param == NULL) return false;
    cf_span value;
    if (cf_param_string(param, &value) != CF_OK) return false;
    *out = value;
    return true;
}

static bool cf_ctx_xhr(const cf_ctx *ctx) {
    cf_span value;
    if (!cf_request_header(ctx->request, "x-requested-with", &value)) {
        return false;
    }
    /* to_ascii_lowercase contains "xmlhttprequest" */
    static const char *needle = "xmlhttprequest";
    size_t n = strlen(needle);
    if (value.len < n) return false;
    for (size_t i = 0; i + n <= value.len; i++) {
        bool match = true;
        for (size_t k = 0; k < n; k++) {
            unsigned char c = value.ptr[i + k];
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c != (unsigned char)needle[k]) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

/* request.formats (format.rs formats()). */
static cf_err cf_ctx_compute_formats(cf_ctx *ctx) {
    struct cf_ctx_state *st = cf_ctx_state(ctx);
    if (st == NULL) return CF_INVALID;
    if (st->formats_done) return st->formats_rc;

    cf_span format_param;
    bool has_format_param = cf_ctx_format_param(ctx, &format_param);
    if (has_format_param) {
        const cf_format *fmt = cf_lookup_by_extension(format_param);
        st->formats_done = true;
        st->formats_rc = CF_OK;
        if (fmt != NULL) {
            st->formats = malloc(sizeof *st->formats);
            if (st->formats == NULL) {
                st->formats_rc = CF_NOMEM;
                return CF_NOMEM;
            }
            st->formats[0] = fmt;
            st->formats_count = 1;
        }
        return CF_OK;
    }

    /* {NULL, 0} on a miss: cf_request_header leaves *out untouched. */
    cf_span accept = {NULL, 0};
    bool has_accept = cf_request_header(ctx->request, "accept", &accept);
    cf_span content_type = {NULL, 0};
    bool has_content_type =
        cf_request_header(ctx->request, "content-type", &content_type);
    bool xhr = cf_ctx_xhr(ctx);

    bool valid_accept = cf_should_apply_vary(
        false, has_accept, accept, has_content_type, content_type, xhr);
    if (valid_accept) {
        cf_span trimmed = cf_span_trim(accept);
        cf_err rc;
        if (trimmed.len == 0) {
            const cf_format *ct = NULL;
            rc = cf_content_mime_type(content_type, has_content_type, &ct);
            if (rc == CF_OK && ct != NULL) {
                st->formats = malloc(sizeof *st->formats);
                if (st->formats == NULL) {
                    rc = CF_NOMEM;
                } else {
                    st->formats[0] = ct;
                    st->formats_count = 1;
                }
            }
        } else {
            rc = cf_parse_accept(accept, &st->formats, &st->formats_count);
        }
        st->formats_done = true;
        st->formats_rc = rc;
        if (rc != CF_OK && rc != CF_INVALID) return rc;
        if (rc == CF_INVALID) {
            /* InvalidMimeType maps to 406 (00-contracts.md); remember the
             * explicit status so dispatch does not fall back to 400. */
            cf_ctx_set_error_status(ctx, 406);
            return CF_INVALID;
        }
        return CF_OK;
    }

    /* format_from_path_extension */
    cf_span path = ctx->request->path;
    const cf_format *from_ext = NULL;
    for (size_t i = path.len; i > 0; i--) {
        if (path.ptr[i - 1] != '.') continue;
        cf_span ext = {path.ptr + i, path.len - i};
        bool ok = ext.len > 0;
        for (size_t k = 0; k < ext.len && ok; k++) {
            unsigned char c = ext.ptr[k];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_')) {
                ok = false;
            }
        }
        if (ok) from_ext = cf_lookup_by_extension(ext);
        break;
    }
    st->formats_done = true;
    st->formats_rc = CF_OK;
    const cf_format *fallback = from_ext != NULL
                                    ? from_ext
                                    : (xhr ? &cf_format_js : &cf_format_html);
    st->formats = malloc(sizeof *st->formats);
    if (st->formats == NULL) {
        st->formats_rc = CF_NOMEM;
        return CF_NOMEM;
    }
    st->formats[0] = fallback;
    st->formats_count = 1;
    return CF_OK;
}

cf_err cf_ctx_formats(cf_ctx *ctx, const cf_format ***out, size_t *count) {
    if (out == NULL || count == NULL) return CF_INVALID;
    *out = NULL;
    *count = 0;
    if (ctx == NULL || ctx->private_state == NULL) return CF_INVALID;
    cf_err rc = cf_ctx_compute_formats(ctx);
    if (rc != CF_OK) return rc;
    struct cf_ctx_state *st = ctx->private_state;
    *out = st->formats;
    *count = st->formats_count;
    return CF_OK;
}

const cf_format *cf_ctx_format(cf_ctx *ctx) {
    const cf_format **list = NULL;
    size_t count = 0;
    if (cf_ctx_formats(ctx, &list, &count) != CF_OK || count == 0) return NULL;
    return list[0];
}

static const cf_format *cf_negotiate(const cf_format *const *formats,
                                     size_t formats_count,
                                     const cf_format *const *order,
                                     size_t order_count) {
    for (size_t i = 0; i < formats_count; i++) {
        const cf_format *priority = formats[i];
        if (cf_format_is(priority, &cf_format_all)) {
            return order_count > 0 ? order[0] : NULL;
        }
        for (size_t k = 0; k < order_count; k++) {
            if (cf_format_is(order[k], priority)) return priority;
        }
    }
    for (size_t k = 0; k < order_count; k++) {
        if (cf_format_is(order[k], &cf_format_all)) {
            return formats_count > 0 ? formats[0] : NULL;
        }
    }
    return NULL;
}

cf_err cf_ctx_respond_to(cf_ctx *ctx, const cf_format *const *offered,
                         size_t count, const cf_format **out) {
    if (out == NULL) return CF_INVALID;
    *out = NULL;
    const cf_format **formats = NULL;
    size_t formats_count = 0;
    cf_err rc = cf_ctx_formats(ctx, &formats, &formats_count);
    if (rc != CF_OK) return rc;
    const cf_format *chosen =
        cf_negotiate(formats, formats_count, offered, count);
    if (chosen == NULL) {
        cf_ctx_set_error_status(ctx, 406);
        return CF_NOT_FOUND;
    }
    if (cf_format_is(chosen, &cf_format_all)) {
        chosen = count > 0 ? offered[0] : &cf_format_html;
    }
    struct cf_ctx_state *st = cf_ctx_state(ctx);
    if (st != NULL) {
        st->rendered_set = true;
        st->rendered = chosen;
    }
    *out = chosen;
    return CF_OK;
}

const cf_format *cf_ctx_rendered_format(cf_ctx *ctx) {
    struct cf_ctx_state *st = cf_ctx_state(ctx);
    if (st != NULL && st->rendered_set) return st->rendered;
    const cf_format **list = NULL;
    size_t count = 0;
    if (cf_ctx_formats(ctx, &list, &count) == CF_OK && count > 0 &&
        !cf_format_is(list[0], &cf_format_all)) {
        return list[0];
    }
    return &cf_format_html;
}

bool cf_ctx_vary_accept(const cf_ctx *ctx) {
    if (ctx == NULL || ctx->request == NULL) return false;
    cf_span format_param;
    bool has_format_param = cf_ctx_format_param(ctx, &format_param);
    (void)format_param;
    /* {NULL, 0} on a miss: cf_request_header leaves *out untouched. */
    cf_span accept = {NULL, 0};
    bool has_accept = cf_request_header(ctx->request, "accept", &accept);
    cf_span content_type = {NULL, 0};
    bool has_content_type =
        cf_request_header(ctx->request, "content-type", &content_type);
    return cf_should_apply_vary(has_format_param, has_accept, accept,
                                has_content_type, content_type,
                                cf_ctx_xhr(ctx));
}

/* ------------------------------------------------------- context lifecycle */

cf_err cf_ctx_create(cf_ctx *ctx, cf_app *app, cf_db *reader,
                     const cf_request *request, cf_response *response) {
    if (ctx == NULL || app == NULL || request == NULL || response == NULL) {
        return CF_INVALID;
    }
    memset(ctx, 0, sizeof *ctx);
    ctx->app = app;
    ctx->reader = reader;
    ctx->request = request;
    ctx->response = response;

    struct cf_ctx_state *st = calloc(1, sizeof *st);
    if (st == NULL) return CF_NOMEM;
    ctx->private_state = st;

    for (size_t i = 0; i < request->header_count; i++) {
        if (!cf_span_ieq_lit(request->headers[i].name, "cookie")) continue;
        cf_err rc = cf_jar_parse_cookie_header(&st->jar,
                                               request->headers[i].value);
        if (rc != CF_OK) return rc;
    }

    cf_err rc = cf_params_parse(request, &st->params);
    if (rc != CF_OK) return rc;

    cf_route_match match;
    memset(&match, 0, sizeof match);
    rc = cf_route_match_request(request, &match);
    if (rc != CF_OK) return rc;
    ctx->route = match;
    if (ctx->route.path_params != NULL) {
        rc = cf_params_merge(st->params, ctx->route.path_params);
        if (rc != CF_OK) return rc;
    }
    return CF_OK;
}

void cf_ctx_destroy(cf_ctx *ctx) {
    if (ctx == NULL) return;
    struct cf_ctx_state *st = ctx->private_state;
    if (st != NULL) {
        cf_params_destroy(st->params);
        cf_cookie_jar_dispose(&st->jar);
        cf_flash_dispose(&st->flash);
        free(st->formats);
        free(st);
        ctx->private_state = NULL;
    }
    cf_route_match_dispose(&ctx->route);
    memset(ctx, 0, sizeof *ctx);
}

/* --------------------------------------------------------- cookie/flash */

cf_err cf_ctx_cookie_set(cf_ctx *ctx, cf_span name, cf_span value,
                         const cf_cookie_options *options) {
    struct cf_ctx_state *st = cf_ctx_state(ctx);
    if (st == NULL || name.len == 0) return CF_INVALID;
    int64_t now = cf_now_us(ctx->app);
    return cf_cookie_jar_set(&st->jar, name, value, options, now);
}

cf_err cf_ctx_cookie_delete(cf_ctx *ctx, cf_span name,
                            const cf_cookie_options *options) {
    struct cf_ctx_state *st = cf_ctx_state(ctx);
    if (st == NULL || name.len == 0) return CF_INVALID;
    return cf_cookie_jar_delete(&st->jar, name, options);
}

cf_err cf_ctx_cookie_get(const cf_ctx *ctx, cf_span name, cf_span *out) {
    struct cf_ctx_state *st = cf_ctx_state(ctx);
    if (st == NULL || out == NULL) return CF_INVALID;
    size_t idx = cf_cookie_current_find(&st->jar, name);
    if (idx == SIZE_MAX) return CF_NOT_FOUND;
    *out = (cf_span){(const unsigned char *)st->jar.current[idx].value,
                     st->jar.current[idx].value_len};
    return CF_OK;
}

cf_err cf_ctx_flash_set(cf_ctx *ctx, cf_span key, cf_span value) {
    struct cf_ctx_state *st = cf_ctx_state(ctx);
    if (st == NULL || key.len == 0) return CF_INVALID;
    size_t idx = cf_flash_find(&st->flash, key);
    if (idx != SIZE_MAX) {
        char *copy = cf_span_dup(value);
        if (copy == NULL) return CF_NOMEM;
        free(st->flash.entries[idx].value);
        st->flash.entries[idx].value = copy;
        st->flash.entries[idx].value_len = value.len;
        return CF_OK;
    }
    if (st->flash.len == st->flash.cap) {
        size_t cap = st->flash.cap == 0 ? 4 : st->flash.cap * 2;
        struct cf_flash_entry *grown =
            realloc(st->flash.entries, cap * sizeof *grown);
        if (grown == NULL) return CF_NOMEM;
        st->flash.entries = grown;
        st->flash.cap = cap;
    }
    char *kcopy = cf_span_dup(key);
    char *vcopy = cf_span_dup(value);
    if (kcopy == NULL || vcopy == NULL) {
        free(kcopy);
        free(vcopy);
        return CF_NOMEM;
    }
    st->flash.entries[st->flash.len++] =
        (struct cf_flash_entry){kcopy, key.len, vcopy, value.len};
    return CF_OK;
}

cf_err cf_ctx_flash_get(const cf_ctx *ctx, cf_span key, cf_span *out) {
    struct cf_ctx_state *st = cf_ctx_state(ctx);
    if (st == NULL || out == NULL) return CF_INVALID;
    size_t idx = cf_flash_find(&st->flash, key);
    if (idx == SIZE_MAX) return CF_NOT_FOUND;
    *out = (cf_span){(const unsigned char *)st->flash.entries[idx].value,
                     st->flash.entries[idx].value_len};
    return CF_OK;
}

cf_err cf_finish_cookies(cf_ctx *ctx) {
    struct cf_ctx_state *st = cf_ctx_state(ctx);
    if (st == NULL || ctx->response == NULL) return CF_INVALID;
    bool ssl = ctx->request->tls;
    bool onion = cf_host_is_onion(ctx->request);

    for (size_t i = 0; i < st->jar.sets_len; i++) {
        struct cf_pending_set *s = &st->jar.sets[i];
        if (s->secure && !ssl && !onion) continue;
        cf_builder b = {0};
        cf_err rc = cf_cookie_build_set(&b, s);
        if (rc == CF_OK) {
            rc = cf_response_header(
                ctx->response,
                (cf_span){(const unsigned char *)"Set-Cookie", 10},
                (cf_span){b.ptr, b.len});
        }
        cf_builder_dispose(&b);
        if (rc != CF_OK) return rc;
    }
    for (size_t i = 0; i < st->jar.deletes_len; i++) {
        cf_builder b = {0};
        cf_err rc = cf_cookie_build_delete(&b, &st->jar.deletes[i]);
        if (rc == CF_OK) {
            rc = cf_response_header(
                ctx->response,
                (cf_span){(const unsigned char *)"Set-Cookie", 10},
                (cf_span){b.ptr, b.len});
        }
        cf_builder_dispose(&b);
        if (rc != CF_OK) return rc;
    }
    /* Queued headers are now owned by the response; a second finish writes
     * nothing. */
    for (size_t i = 0; i < st->jar.sets_len; i++) {
        cf_pending_set_clear(&st->jar.sets[i]);
    }
    free(st->jar.sets);
    st->jar.sets = NULL;
    st->jar.sets_len = st->jar.sets_cap = 0;
    for (size_t i = 0; i < st->jar.deletes_len; i++) {
        cf_pending_delete_clear(&st->jar.deletes[i]);
    }
    free(st->jar.deletes);
    st->jar.deletes = NULL;
    st->jar.deletes_len = st->jar.deletes_cap = 0;
    return CF_OK;
}

/* ------------------------------------------------------------ dispatch */

static unsigned cf_generic_status(cf_err err) {
    switch (err) {
    case CF_NOT_FOUND:
        return 404;
    case CF_FORBIDDEN:
        return 403;
    case CF_INVALID:
    case CF_LIMIT:
        return 400; /* malformed HTTP/params (00-contracts.md) */
    case CF_BUSY:
        return 503;
    case CF_NOMEM:
    case CF_IO:
    case CF_DB:
    case CF_INTERNAL:
    default:
        return 500;
    }
}

static cf_err cf_finish_mapped(cf_ctx *ctx, cf_err err) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    unsigned status = 0;
    struct cf_ctx_state *st = cf_ctx_state(ctx);
    if (st != NULL && st->error_status != 0) status = st->error_status;
    if (status == 0) status = cf_generic_status(err);
    if (status >= 500) {
        /* Sanitized log: operation name and route id only. */
        fprintf(stderr, "campfire: dispatch: %s (route=%u)\n",
                cf_err_name(err), ctx->route.id);
    }
    cf_response_dispose(ctx->response);
    cf_response_init(ctx->response);
    ctx->response->status = status;
    return CF_OK;
}

cf_err cf_dispatch(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_action_fn action = cf_route_action(ctx->route.id);
    if (action == NULL) {
        /* Matched row without a bound action: the reference's public 404,
         * not an empty body (00-contracts.md error translation). */
        return cf_action_reference_action_not_found(ctx);
    }
    cf_err rc = action(ctx);
    if (rc != CF_OK) return cf_finish_mapped(ctx, rc);
    rc = cf_finish_cookies(ctx);
    if (rc != CF_OK) return cf_finish_mapped(ctx, rc);
    return CF_OK;
}

cf_err cf_ctx_process(cf_app *app, cf_db *reader, const cf_request *request,
                      cf_response *response) {
    if (response == NULL) return CF_INVALID;
    cf_response_init(response);
    if (app == NULL || request == NULL) return CF_INVALID;

    cf_ctx ctx;
    cf_err rc = cf_ctx_create(&ctx, app, reader, request, response);
    if (rc != CF_OK) {
        if (rc == CF_NOT_FOUND) {
            /* Unmatched route: the reference's public 404 (the same handler
             * the 40 reference_error rows bind), not an empty body. */
            rc = cf_action_reference_action_not_found(&ctx);
            cf_ctx_destroy(&ctx);
            if (rc != CF_OK) cf_response_dispose(response);
            return rc;
        }
        if (rc == CF_INVALID || rc == CF_LIMIT) {
            /* Malformed params / bad path capture -> 400. */
            cf_err mapped = cf_finish_mapped(&ctx, rc);
            cf_ctx_destroy(&ctx);
            return mapped;
        }
        cf_ctx_destroy(&ctx);
        return rc;
    }
    rc = cf_dispatch(&ctx);
    cf_ctx_destroy(&ctx);
    if (rc != CF_OK) {
        cf_response_dispose(response);
        return rc;
    }
    return CF_OK;
}
