/* H02: request parameters and method override (01-foundation-http.md "H02").
 *
 * Executable specification: tmp/rust-ref/crates/kit/src/params.rs (port of
 * ActionDispatch::ParamBuilder / Rack::QueryParser), kit/src/body.rs (content
 * type dispatch, deep munge, multipart), kit/src/adapter.rs::method_override,
 * and the pinned vector corpus tests/fixtures/crates/kit/tests/
 * params_vectors.json (2,755 Rails-generated cases).
 *
 * C differences fixed by 01-foundation-http.md, taken over the Rust source:
 *  - parameter depth 32 (Rust: 100) and 4096 total parameter nodes;
 *  - multipart is bounded to 100 non-file fields and 16 files;
 *  - uploads are rejected with CF_INVALID until S01 lands (partial multipart:
 *    structure and non-file fields only; see the H02 evidence).
 *
 * Error codes: malformed shapes, encodings, type conflicts and strict-JSON
 * failures are CF_INVALID; the fixed bounds above are CF_LIMIT. Both are
 * "malformed HTTP/params" (400) per 00-contracts.md.
 *
 * The tree is one arena per cf_params: every node, key and string is bump
 * allocated and released by cf_params_destroy. Bounds are checked before the
 * arena grows (params_new_node). No pointer escapes cf_params' lifetime
 * except the borrowed const cf_param* accessor results the contract defines. */
#define CF_HTTP_PARAMS_INTERNALS 1
#include "http/params.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "yyjson.h"

#define CF_PARAM_DEPTH_LIMIT 32u
#define CF_PARAM_NODE_LIMIT 4096u
#define CF_MULTIPART_FIELD_LIMIT 100u
#define CF_MULTIPART_FILE_LIMIT 16u
#define CF_ARENA_BLOCK 8192u

struct cf_params_block {
    struct cf_params_block *next;
    size_t cap, used;
    unsigned char data[];
};

/* ------------------------------------------------------------------ spans */

static bool span_eq(cf_span a, cf_span b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.ptr, b.ptr, a.len) == 0);
}

static bool span_eq_cstr(cf_span a, const char *b) {
    return span_eq(a, (cf_span){(const unsigned char *)b, strlen(b)});
}

static bool span_eq_ci(cf_span a, cf_span b) {
    if (a.len != b.len) return false;
    for (size_t i = 0; i < a.len; i++) {
        unsigned char x = a.ptr[i], y = b.ptr[i];
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return false;
    }
    return true;
}

static bool span_eq_cstr_ci(cf_span a, const char *b) {
    return span_eq_ci(a, (cf_span){(const unsigned char *)b, strlen(b)});
}

static bool span_is_ascii_visible(cf_span s) {
    for (size_t i = 0; i < s.len; i++) {
        if (s.ptr[i] < 0x20 || s.ptr[i] > 0x7E) return false;
    }
    return true;
}

/* Strict UTF-8 (RFC 3629): rejects overlongs, surrogates and > U+10FFFF. */
static bool utf8_valid(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        if (c < 0x80) {
            i++;
        } else if (c >= 0xC2 && c <= 0xDF) {
            if (i + 1 >= n || (s[i + 1] & 0xC0) != 0x80) return false;
            i += 2;
        } else if (c == 0xE0) {
            if (i + 2 >= n || s[i + 1] < 0xA0 || s[i + 1] > 0xBF || (s[i + 2] & 0xC0) != 0x80) return false;
            i += 3;
        } else if ((c >= 0xE1 && c <= 0xEC) || (c >= 0xEE && c <= 0xEF)) {
            if (i + 2 >= n || (s[i + 1] & 0xC0) != 0x80 || (s[i + 2] & 0xC0) != 0x80) return false;
            i += 3;
        } else if (c == 0xED) {
            if (i + 2 >= n || s[i + 1] < 0x80 || s[i + 1] > 0x9F || (s[i + 2] & 0xC0) != 0x80) return false;
            i += 3;
        } else if (c == 0xF0) {
            if (i + 3 >= n || s[i + 1] < 0x90 || s[i + 1] > 0xBF ||
                (s[i + 2] & 0xC0) != 0x80 || (s[i + 3] & 0xC0) != 0x80) {
                return false;
            }
            i += 4;
        } else if (c >= 0xF1 && c <= 0xF3) {
            if (i + 3 >= n || (s[i + 1] & 0xC0) != 0x80 || (s[i + 2] & 0xC0) != 0x80 ||
                (s[i + 3] & 0xC0) != 0x80) {
                return false;
            }
            i += 4;
        } else if (c == 0xF4) {
            if (i + 3 >= n || s[i + 1] < 0x80 || s[i + 1] > 0x8F ||
                (s[i + 2] & 0xC0) != 0x80 || (s[i + 3] & 0xC0) != 0x80) {
                return false;
            }
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ arena */

static void *arena_alloc(cf_params *p, size_t size) {
    if (size == 0) return NULL;
    size_t aligned = (size + 7u) & ~(size_t)7u;
    if (aligned < size) return NULL; /* overflow */
    struct cf_params_block *b = p->blocks;
    if (b != NULL && b->cap - b->used >= aligned) {
        void *out = b->data + b->used;
        b->used += aligned;
        return out;
    }
    size_t cap = aligned > CF_ARENA_BLOCK ? aligned : CF_ARENA_BLOCK;
    if (cap > SIZE_MAX - sizeof(struct cf_params_block)) return NULL;
    struct cf_params_block *nb = malloc(sizeof(struct cf_params_block) + cap);
    if (nb == NULL) return NULL;
    nb->next = p->blocks;
    nb->cap = cap;
    nb->used = aligned;
    p->blocks = nb;
    return nb->data;
}

static cf_err params_new_node(cf_params *p, cf_param_kind kind, cf_param **out) {
    if (p->node_count >= CF_PARAM_NODE_LIMIT) return CF_LIMIT;
    cf_param *n = arena_alloc(p, sizeof(*n));
    if (n == NULL) return CF_NOMEM;
    memset(n, 0, sizeof(*n));
    n->kind = kind;
    p->node_count++;
    *out = n;
    return CF_OK;
}

/* --------------------------------------------------- ordered object/array */

static size_t object_find(const cf_param *obj, cf_span key) {
    for (size_t i = 0; i < obj->u.object.len; i++) {
        if (span_eq(obj->u.object.entries[i].key, key)) return i;
    }
    return obj->u.object.len;
}

static cf_err object_insert(cf_params *p, cf_param *obj, cf_span key, cf_param *value) {
    size_t i = object_find(obj, key);
    if (i < obj->u.object.len) { /* Ruby Hash#[]=: replace, keep position */
        obj->u.object.entries[i].value = value;
        return CF_OK;
    }
    if (obj->u.object.len == obj->u.object.cap) {
        size_t ncap = obj->u.object.cap ? obj->u.object.cap * 2 : 4;
        if (ncap > SIZE_MAX / sizeof(struct cf_param_entry)) return CF_LIMIT;
        struct cf_param_entry *entries = arena_alloc(p, ncap * sizeof(*entries));
        if (entries == NULL) return CF_NOMEM;
        if (obj->u.object.len > 0) {
            memcpy(entries, obj->u.object.entries, obj->u.object.len * sizeof(*entries));
        }
        obj->u.object.entries = entries;
        obj->u.object.cap = ncap;
    }
    unsigned char *copy = NULL;
    if (key.len > 0) {
        copy = arena_alloc(p, key.len);
        if (copy == NULL) return CF_NOMEM;
        memcpy(copy, key.ptr, key.len);
    }
    obj->u.object.entries[obj->u.object.len].key = (cf_span){copy, key.len};
    obj->u.object.entries[obj->u.object.len].value = value;
    obj->u.object.len++;
    return CF_OK;
}

static cf_err array_push(cf_params *p, cf_param *arr, cf_param *item) {
    if (arr->u.array.len == arr->u.array.cap) {
        size_t ncap = arr->u.array.cap ? arr->u.array.cap * 2 : 4;
        if (ncap > SIZE_MAX / sizeof(cf_param *)) return CF_LIMIT;
        cf_param **items = arena_alloc(p, ncap * sizeof(*items));
        if (items == NULL) return CF_NOMEM;
        if (arr->u.array.len > 0) {
            memcpy(items, arr->u.array.items, arr->u.array.len * sizeof(*items));
        }
        arr->u.array.items = items;
        arr->u.array.cap = ncap;
    }
    arr->u.array.items[arr->u.array.len++] = item;
    return CF_OK;
}

static cf_err param_string_copy(cf_params *p, const unsigned char *bytes, size_t len, cf_param **out) {
    cf_param *n;
    cf_err err = params_new_node(p, CF_PARAM_STRING, &n);
    if (err != CF_OK) return err;
    unsigned char *copy = NULL;
    if (len > 0) {
        copy = arena_alloc(p, len);
        if (copy == NULL) return CF_NOMEM;
        memcpy(copy, bytes, len);
    }
    n->u.string = (cf_span){copy, len};
    *out = n;
    return CF_OK;
}

/* ------------------------------------------------------- number analysis */

static bool ascii_digit(unsigned char c) { return c >= '0' && c <= '9'; }

/* Exact integrality of a JSON number lexeme (yyjson raw text), without any
 * double conversion except the finiteness check. Numbers written as
 * fractions/exponents count only when their exact value is an integer that
 * fits int64: 1e2 -> 100, "100.00" -> 100, 1.5 and 2^63 -> not usable.
 * Malformed syntax cannot occur for yyjson output; returns CF_INVALID then. */
static cf_err number_analyze(const unsigned char *s, size_t n, bool *integral, int64_t *value) {
    *integral = false;
    *value = 0;
    /* Strict double conversion, as the default yyjson reader performs: an
     * out-of-range magnitude (e.g. 1e999) is rejected, not truncated. */
    char *text = malloc(n + 1);
    if (text == NULL) return CF_NOMEM;
    memcpy(text, s, n);
    text[n] = '\0';
    double approx = strtod(text, NULL);
    free(text);
    if (!isfinite(approx)) return CF_INVALID;

    size_t i = 0;
    bool neg = false;
    if (i < n && s[i] == '-') { neg = true; i++; }
    size_t int_start = i;
    if (i < n && s[i] == '0') {
        i++;
    } else if (i < n && s[i] >= '1' && s[i] <= '9') {
        while (i < n && ascii_digit(s[i])) i++;
    } else {
        return CF_INVALID;
    }
    size_t int_len = i - int_start;
    size_t frac_start = i;
    size_t frac_len = 0;
    if (i < n && s[i] == '.') {
        i++;
        frac_start = i;
        while (i < n && ascii_digit(s[i])) i++;
        frac_len = i - frac_start;
        if (frac_len == 0) return CF_INVALID;
    }
    long exp = 0;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        bool eneg = false;
        if (i < n && (s[i] == '+' || s[i] == '-')) { eneg = s[i] == '-'; i++; }
        size_t es = i;
        long e = 0;
        while (i < n && ascii_digit(s[i])) {
            if (e < 1000000) e = e * 10 + (s[i] - '0');
            i++;
        }
        if (i == es) return CF_INVALID;
        exp = eneg ? -e : e;
    }
    if (i != n) return CF_INVALID;

    size_t total = int_len + frac_len;
    /* digit j of the significand: int digits then fraction digits */
    #define DIGIT_AT(j) ((j) < int_len ? s[int_start + (j)] - '0' : s[frac_start + (j) - int_len] - '0')
    size_t leading = 0;
    while (leading < total && DIGIT_AT(leading) == 0) leading++;
    if (leading == total) { /* zero, whatever the exponent */
        *integral = true;
        *value = 0;
        return CF_OK;
    }

    long shift = exp - (long)frac_len;
    size_t used = total; /* digits that form the integer value */
    if (shift < 0) {
        size_t drop = (size_t)(-shift);
        if (drop >= total) return CF_OK; /* strictly between -1 and 1 */
        for (size_t j = total - drop; j < total; j++) {
            if (DIGIT_AT(j) != 0) return CF_OK; /* fractional part */
        }
        used = total - drop;
    }

    uint64_t limit = neg ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    uint64_t acc = 0;
    for (size_t j = leading; j < used; j++) {
        uint64_t d = (uint64_t)DIGIT_AT(j);
        if (acc > (limit - d) / 10u) return CF_OK; /* out of int64 range */
        acc = acc * 10u + d;
    }
    if (shift > 0) {
        for (long e2 = 0; e2 < shift; e2++) {
            if (acc > limit / 10u) return CF_OK;
            acc *= 10u;
        }
    }
    #undef DIGIT_AT
    *integral = true;
    if (neg) {
        *value = acc == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)acc;
    } else {
        *value = (int64_t)acc;
    }
    return CF_OK;
}

static cf_err param_number_copy(cf_params *p, const unsigned char *raw, size_t len, cf_param **out) {
    bool integral;
    int64_t value;
    cf_err err = number_analyze(raw, len, &integral, &value);
    if (err != CF_OK) return err;
    cf_param *n;
    err = params_new_node(p, CF_PARAM_NUMBER, &n);
    if (err != CF_OK) return err;
    n->u.number.integral = integral;
    n->u.number.value = value;
    *out = n;
    return CF_OK;
}

/* ------------------------------------------- Rails ParamBuilder store port */

/* Result of store_nested, mirroring params.rs `Stored`. */
enum stored_kind { STORED_PARAMS, STORED_ARRAY, STORED_NIL };
struct stored_result {
    enum stored_kind kind;
    cf_param *value; /* object / array / NULL node */
};

static cf_err stored_as_param(cf_params *p, struct stored_result stored, cf_param *fallback,
                              cf_param **out) {
    if (stored.kind == STORED_PARAMS) {
        *out = fallback;
        return CF_OK;
    }
    if (stored.kind == STORED_ARRAY) {
        *out = stored.value;
        return CF_OK;
    }
    return params_new_node(p, CF_PARAM_NULL, out);
}

/* params[k] ||= [] followed by the Array type check (params.rs array_slot). */
static cf_err array_slot(cf_params *p, cf_param *obj, cf_span key, cf_param **out) {
    size_t idx = object_find(obj, key);
    if (idx == obj->u.object.len || obj->u.object.entries[idx].value->kind == CF_PARAM_NULL) {
        cf_param *arr;
        cf_err err = params_new_node(p, CF_PARAM_ARRAY, &arr);
        if (err != CF_OK) return err;
        err = object_insert(p, obj, key, arr);
        if (err != CF_OK) return err;
        *out = arr;
        return CF_OK;
    }
    cf_param *cur = obj->u.object.entries[idx].value;
    if (cur->kind != CF_PARAM_ARRAY) return CF_INVALID; /* expected Array */
    *out = cur;
    return CF_OK;
}

/* Ruby's `key.split(/[\[\]]+/)` reachability test (params.rs params_hash_has_key). */
static bool params_hash_has_key(const cf_param *hash, cf_span key) {
    for (size_t i = 0; i + 1 < key.len; i++) {
        if (key.ptr[i] == '[' && key.ptr[i + 1] == ']') return false;
    }
    const cf_param *current = hash;
    size_t i = 0;
    while (i < key.len) {
        while (i < key.len && (key.ptr[i] == '[' || key.ptr[i] == ']')) i++;
        size_t start = i;
        while (i < key.len && key.ptr[i] != '[' && key.ptr[i] != ']') i++;
        if (i == start) continue;
        if (current == NULL) return false;
        cf_span part = {key.ptr + start, i - start};
        size_t idx = object_find(current, part);
        if (idx == current->u.object.len) return false;
        const cf_param *v = current->u.object.entries[idx].value;
        current = v->kind == CF_PARAM_OBJECT ? v : NULL;
    }
    return true;
}

static size_t find_byte(cf_span s, unsigned char byte, size_t from) {
    for (size_t i = from; i < s.len; i++) {
        if (s.ptr[i] == byte) return i;
    }
    return s.len; /* not found sentinel: s.len is never a valid bracket position here */
}

/* params.rs store_nested_param, depth renamed to the C fixed limit. */
static cf_err store_nested(cf_params *p, cf_param *obj, cf_span name, cf_param *v,
                           size_t depth, struct stored_result *out) {
    if (depth >= CF_PARAM_DEPTH_LIMIT) return CF_LIMIT;

    cf_span k;
    size_t after_off;
    if (depth == 0) {
        size_t start = find_byte(name, '[', 1);
        if (start < name.len) {
            k = (cf_span){name.ptr, start};
            after_off = start;
        } else {
            k = name;
            after_off = name.len;
        }
    } else if (name.len >= 2 && name.ptr[0] == '[' && name.ptr[1] == ']') {
        k = (cf_span){name.ptr, 2}; /* literal "[]" */
        after_off = 2;
    } else if (name.len > 0 && name.ptr[0] == '[') {
        size_t end = find_byte(name, ']', 1);
        if (end < name.len) {
            k = (cf_span){name.ptr + 1, end - 1};
            after_off = end + 1;
        } else {
            k = name;
            after_off = name.len;
        }
    } else {
        k = name;
        after_off = name.len;
    }

    if (k.len == 0) { /* empty key: nil, discarded by the caller's conversion */
        cf_param *nil_node;
        cf_err err = params_new_node(p, CF_PARAM_NULL, &nil_node);
        if (err != CF_OK) return err;
        out->kind = STORED_NIL;
        out->value = nil_node;
        return CF_OK;
    }

    cf_span after = {name.ptr + after_off, name.len - after_off};
    if (after.len == 0) {
        if (k.len == 2 && k.ptr[0] == '[' && k.ptr[1] == ']' && depth != 0) {
            cf_param *arr;
            cf_err err = params_new_node(p, CF_PARAM_ARRAY, &arr);
            if (err != CF_OK) return err;
            if (v->kind != CF_PARAM_NULL) {
                err = array_push(p, arr, v);
                if (err != CF_OK) return err;
            }
            out->kind = STORED_ARRAY;
            out->value = arr;
            return CF_OK;
        }
        cf_err err = object_insert(p, obj, k, v);
        if (err != CF_OK) return err;
        out->kind = STORED_PARAMS;
        out->value = obj;
        return CF_OK;
    }

    if (after.len == 1 && after.ptr[0] == '[') {
        cf_err err = object_insert(p, obj, name, v);
        if (err != CF_OK) return err;
        out->kind = STORED_PARAMS;
        out->value = obj;
        return CF_OK;
    }

    if (after.len == 2 && after.ptr[0] == '[' && after.ptr[1] == ']') {
        cf_param *arr;
        cf_err err = array_slot(p, obj, k, &arr);
        if (err != CF_OK) return err;
        if (v->kind != CF_PARAM_NULL) {
            err = array_push(p, arr, v);
            if (err != CF_OK) return err;
        }
        out->kind = STORED_PARAMS;
        out->value = obj;
        return CF_OK;
    }

    if (after.len >= 2 && after.ptr[0] == '[' && after.ptr[1] == ']') {
        cf_span nested = {after.ptr + 2, after.len - 2};
        /* Recognize x[][y]: a single clean key inside brackets. */
        cf_span child_key = nested;
        if (nested.len >= 2 && nested.ptr[0] == '[' && nested.ptr[nested.len - 1] == ']') {
            cf_span inner = {nested.ptr + 1, nested.len - 2};
            bool clean = inner.len > 0;
            for (size_t i = 0; clean && i < inner.len; i++) {
                if (inner.ptr[i] == '[' || inner.ptr[i] == ']') clean = false;
            }
            if (clean) child_key = inner;
        }
        cf_param *arr;
        cf_err err = array_slot(p, obj, k, &arr);
        if (err != CF_OK) return err;
        cf_param *last = arr->u.array.len > 0 ? arr->u.array.items[arr->u.array.len - 1] : NULL;
        if (last != NULL && last->kind == CF_PARAM_OBJECT && !params_hash_has_key(last, child_key)) {
            err = store_nested(p, last, child_key, v, depth + 1, &(struct stored_result){0});
            if (err != CF_OK) return err;
        } else {
            cf_param *child;
            err = params_new_node(p, CF_PARAM_OBJECT, &child);
            if (err != CF_OK) return err;
            struct stored_result stored;
            err = store_nested(p, child, child_key, v, depth + 1, &stored);
            if (err != CF_OK) return err;
            cf_param *item;
            err = stored_as_param(p, stored, child, &item);
            if (err != CF_OK) return err;
            err = array_push(p, arr, item);
            if (err != CF_OK) return err;
        }
        out->kind = STORED_PARAMS;
        out->value = obj;
        return CF_OK;
    }

    {
        size_t idx = object_find(obj, k);
        if (idx == obj->u.object.len || obj->u.object.entries[idx].value->kind == CF_PARAM_NULL) {
            cf_param *child;
            cf_err err = params_new_node(p, CF_PARAM_OBJECT, &child);
            if (err != CF_OK) return err;
            struct stored_result stored;
            err = store_nested(p, child, after, v, depth + 1, &stored);
            if (err != CF_OK) return err;
            cf_param *item;
            err = stored_as_param(p, stored, child, &item);
            if (err != CF_OK) return err;
            err = object_insert(p, obj, k, item);
            if (err != CF_OK) return err;
        } else if (obj->u.object.entries[idx].value->kind == CF_PARAM_OBJECT) {
            struct stored_result stored;
            cf_err err = store_nested(p, obj->u.object.entries[idx].value, after, v, depth + 1, &stored);
            if (err != CF_OK) return err;
            if (stored.kind != STORED_PARAMS) {
                cf_param *item;
                err = stored_as_param(p, stored, obj->u.object.entries[idx].value, &item);
                if (err != CF_OK) return err;
                obj->u.object.entries[idx].value = item;
            }
        } else {
            return CF_INVALID; /* expected Hash */
        }
    }
    out->kind = STORED_PARAMS;
    out->value = obj;
    return CF_OK;
}

/* -------------------------------------------------- urlencoded decoding */

static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* URI.decode_www_form_component: '+' is space, %XX is one byte, a bad '%' is
 * an error. The decoded copy lives in the params arena. */
static cf_err decode_component(cf_params *p, const unsigned char *s, size_t n,
                               cf_span *out) {
    unsigned char *buf = NULL;
    if (n > 0) {
        buf = arena_alloc(p, n);
        if (buf == NULL) return CF_NOMEM;
    }
    size_t w = 0;
    for (size_t i = 0; i < n;) {
        unsigned char c = s[i];
        if (c == '+') {
            buf[w++] = ' ';
            i++;
        } else if (c == '%') {
            if (i + 2 >= n) return CF_INVALID;
            int hi = hex_value(s[i + 1]);
            int lo = hex_value(s[i + 2]);
            if (hi < 0 || lo < 0) return CF_INVALID;
            buf[w++] = (unsigned char)((hi << 4) | lo);
            i += 3;
        } else {
            buf[w++] = c;
            i++;
        }
    }
    *out = (cf_span){buf, w};
    return CF_OK;
}

/* params.rs top_level_key: the part before the first '[' at index >= 1. */
static size_t top_level_key_len(cf_span name) {
    for (size_t i = 1; i < name.len; i++) {
        if (name.ptr[i] == '[') return i;
    }
    return name.len;
}

/* Rails from_pairs for one decoded pair; shared by urlencoded bodies, query
 * strings and multipart text fields. */
static cf_err store_pair(cf_params *p, cf_span key, bool has_value, cf_span value) {
    if (!utf8_valid(key.ptr, key.len)) return CF_INVALID;
    if (top_level_key_len(key) == 0) return CF_OK; /* skip empty top-level key */
    cf_param *v;
    cf_err err;
    if (!has_value) {
        err = params_new_node(p, CF_PARAM_NULL, &v);
    } else {
        if (!utf8_valid(value.ptr, value.len)) return CF_INVALID;
        err = param_string_copy(p, value.ptr, value.len, &v);
    }
    if (err != CF_OK) return err;
    struct stored_result stored;
    return store_nested(p, &p->root, key, v, 0, &stored);
}

/* `body.rs::form_pairs` / `params.rs::query_pairs`. Form bodies drop one
 * trailing NUL and must be UTF-8; query strings are decoded per component. */
static cf_err parse_urlencoded(cf_params *p, cf_span input, bool form_body) {
    if (input.len == 0) return CF_OK;
    if (form_body) {
        if (input.len > 0 && input.ptr[input.len - 1] == '\0') input.len--;
        if (!utf8_valid(input.ptr, input.len)) return CF_INVALID;
    }
    size_t pos = 0;
    bool first = true;
    while (pos <= input.len) {
        size_t end = pos;
        while (end < input.len && input.ptr[end] != '&') end++;
        const unsigned char *part = input.ptr + pos;
        size_t plen = end - pos;
        if (!first) {
            while (plen > 0 && part[0] == ' ') {
                part++;
                plen--;
            }
        }
        first = false;
        if (plen > 0) {
            size_t eq = 0;
            while (eq < plen && part[eq] != '=') eq++;
            cf_span key;
            cf_err err = decode_component(p, part, eq, &key);
            if (err != CF_OK) return err;
            if (eq < plen) {
                cf_span value;
                err = decode_component(p, part + eq + 1, plen - eq - 1, &value);
                if (err != CF_OK) return err;
                err = store_pair(p, key, true, value);
            } else {
                err = store_pair(p, key, false, (cf_span){NULL, 0});
            }
            if (err != CF_OK) return err;
        }
        if (end == input.len) break;
        pos = end + 1;
    }
    return CF_OK;
}

/* ------------------------------------------------------------- JSON body */

static cf_err json_value_to_param(cf_params *p, yyjson_val *v, size_t depth, cf_param **out) {
    if (depth >= CF_PARAM_DEPTH_LIMIT) return CF_LIMIT;
    if (yyjson_is_null(v)) return params_new_node(p, CF_PARAM_NULL, out);
    if (yyjson_is_bool(v)) {
        cf_err err = params_new_node(p, CF_PARAM_BOOL, out);
        if (err != CF_OK) return err;
        (*out)->u.boolean = yyjson_get_bool(v);
        return CF_OK;
    }
    if (yyjson_is_str(v)) {
        size_t len = yyjson_get_len(v);
        const char *str = yyjson_get_str(v);
        return param_string_copy(p, (const unsigned char *)str, len, out);
    }
    if (yyjson_is_raw(v)) { /* numbers, read with YYJSON_READ_NUMBER_AS_RAW */
        const char *raw = yyjson_get_raw(v);
        return param_number_copy(p, (const unsigned char *)raw, strlen(raw), out);
    }
    if (yyjson_is_arr(v)) {
        cf_param *arr;
        cf_err err = params_new_node(p, CF_PARAM_ARRAY, &arr);
        if (err != CF_OK) return err;
        yyjson_arr_iter it = yyjson_arr_iter_with(v);
        yyjson_val *item;
        while ((item = yyjson_arr_iter_next(&it)) != NULL) {
            if (yyjson_is_null(item)) continue; /* NoNilParamEncoder deep munge */
            cf_param *converted;
            err = json_value_to_param(p, item, depth + 1, &converted);
            if (err != CF_OK) return err;
            err = array_push(p, arr, converted);
            if (err != CF_OK) return err;
        }
        *out = arr;
        return CF_OK;
    }
    if (yyjson_is_obj(v)) {
        cf_param *obj;
        cf_err err = params_new_node(p, CF_PARAM_OBJECT, &obj);
        if (err != CF_OK) return err;
        yyjson_obj_iter it = yyjson_obj_iter_with(v);
        yyjson_val *key;
        while ((key = yyjson_obj_iter_next(&it)) != NULL) {
            yyjson_val *item = yyjson_obj_iter_get_val(key);
            cf_param *converted;
            err = json_value_to_param(p, item, depth + 1, &converted);
            if (err != CF_OK) return err;
            cf_span k = {(const unsigned char *)yyjson_get_str(key), yyjson_get_len(key)};
            err = object_insert(p, obj, k, converted);
            if (err != CF_OK) return err;
        }
        *out = obj;
        return CF_OK;
    }
    return CF_INVALID;
}

/* params.rs from_json_body: a non-object document is wrapped as `_json`. */
static cf_err parse_json_body(cf_params *p, cf_span body) {
    yyjson_doc *doc = yyjson_read((const char *)body.ptr, body.len, YYJSON_READ_NUMBER_AS_RAW);
    if (doc == NULL) return CF_INVALID;
    yyjson_val *root = yyjson_doc_get_root(doc);
    cf_err err = CF_OK;
    if (root != NULL) {
        if (yyjson_is_obj(root)) {
            yyjson_obj_iter it = yyjson_obj_iter_with(root);
            yyjson_val *key;
            while (err == CF_OK && (key = yyjson_obj_iter_next(&it)) != NULL) {
                yyjson_val *item = yyjson_obj_iter_get_val(key);
                cf_param *converted;
                /* 0 containers above a top-level member, as in store_nested */
                err = json_value_to_param(p, item, 0, &converted);
                if (err != CF_OK) break;
                cf_span k = {(const unsigned char *)yyjson_get_str(key), yyjson_get_len(key)};
                err = object_insert(p, &p->root, k, converted);
            }
        } else {
            cf_param *converted;
            err = json_value_to_param(p, root, 0, &converted);
            if (err == CF_OK) {
                static const unsigned char wrap[] = "_json";
                err = object_insert(p, &p->root, (cf_span){wrap, 5}, converted);
            }
        }
    }
    yyjson_doc_free(doc);
    return err;
}

/* ------------------------------------------------------------- multipart */

static const unsigned char *find_seq(const unsigned char *hay, size_t hlen,
                                     const unsigned char *needle, size_t nlen) {
    if (nlen == 0 || nlen > hlen) return NULL;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (memcmp(hay + i, needle, nlen) == 0) return hay + i;
    }
    return NULL;
}

static const unsigned char *find_delim_line(const unsigned char *b, size_t len,
                                            const unsigned char *delim, size_t dlen,
                                            size_t from) {
    for (size_t i = from; i + dlen <= len; i++) {
        if ((i == 0 || b[i - 1] == '\n') && memcmp(b + i, delim, dlen) == 0) return b + i;
    }
    return NULL;
}

/* `boundary` parameter of a Content-Type value (case-insensitive name,
 * quoted or token value). Returns false when absent/unusable. */
static bool content_type_boundary(cf_span ct, cf_span *out) {
    size_t i = 0;
    while (i < ct.len && ct.ptr[i] != ';') i++;
    while (i < ct.len) {
        i++; /* ';' */
        while (i < ct.len && (ct.ptr[i] == ' ' || ct.ptr[i] == '\t')) i++;
        size_t ns = i;
        while (i < ct.len && ct.ptr[i] != '=' && ct.ptr[i] != ';') i++;
        size_t ne = i;
        while (ne > ns && (ct.ptr[ne - 1] == ' ' || ct.ptr[ne - 1] == '\t')) ne--;
        cf_span pname = {ct.ptr + ns, ne - ns};
        if (i >= ct.len || ct.ptr[i] != '=') {
            while (i < ct.len && ct.ptr[i] != ';') i++;
            continue;
        }
        i++;
        while (i < ct.len && (ct.ptr[i] == ' ' || ct.ptr[i] == '\t')) i++;
        cf_span val;
        if (i < ct.len && ct.ptr[i] == '"') {
            i++;
            size_t vs = i;
            while (i < ct.len && ct.ptr[i] != '"') {
                if (ct.ptr[i] == '\\' && i + 1 < ct.len) i++;
                i++;
            }
            if (i >= ct.len) return false;
            val = (cf_span){ct.ptr + vs, i - vs};
            i++;
        } else {
            size_t vs = i;
            while (i < ct.len && ct.ptr[i] != ';') i++;
            size_t ve = i;
            while (ve > vs && (ct.ptr[ve - 1] == ' ' || ct.ptr[ve - 1] == '\t')) ve--;
            val = (cf_span){ct.ptr + vs, ve - vs};
        }
        if (span_eq_cstr_ci(pname, "boundary")) {
            if (val.len == 0 || val.len > 70) return false;
            for (size_t j = 0; j < val.len; j++) {
                unsigned char c = val.ptr[j];
                if (c < 0x20 || c > 0x7E || c == '"') return false;
            }
            *out = val;
            return true;
        }
    }
    return false;
}

/* Content-Disposition parameters needed here: the field name and whether a
 * filename part was present (its value is not used; uploads are rejected). */
struct part_disposition {
    cf_span name;
    bool has_name;
    bool has_filename;
    bool filename_empty;
};

/* Unescape a quoted parameter into the arena (Rack keeps the backslash only
 * for filename escapes; those values are not used here). */
static cf_err unescape_quoted(cf_params *p, cf_span raw, cf_span *out) {
    unsigned char *buf = arena_alloc(p, raw.len > 0 ? raw.len : 1);
    if (buf == NULL) return CF_NOMEM;
    size_t w = 0;
    for (size_t i = 0; i < raw.len; i++) {
        if (raw.ptr[i] == '\\' && i + 1 < raw.len) {
            i++;
            buf[w++] = raw.ptr[i];
        } else {
            buf[w++] = raw.ptr[i];
        }
    }
    *out = (cf_span){buf, w};
    return CF_OK;
}

/* Scan one header value for `name=` / `filename=` / `filename*=` parameters,
 * Rack style. Unquoted values are trimmed; quoted values are unescaped into
 * the arena. `name` and `filename` are borrowed from the caller's header
 * scan buffer, not yet copied. */
static cf_err scan_disposition(cf_params *p, cf_span value, struct part_disposition *out) {
    size_t i = 0;
    while (i < value.len && value.ptr[i] != ';') i++;
    while (i < value.len) {
        i++;
        while (i < value.len && (value.ptr[i] == ' ' || value.ptr[i] == '\t')) i++;
        size_t ns = i;
        while (i < value.len && value.ptr[i] != '=' && value.ptr[i] != ';') i++;
        size_t ne = i;
        while (ne > ns && (value.ptr[ne - 1] == ' ' || value.ptr[ne - 1] == '\t')) ne--;
        cf_span pname = {value.ptr + ns, ne - ns};
        if (i >= value.len || value.ptr[i] != '=') {
            while (i < value.len && value.ptr[i] != ';') i++;
            continue;
        }
        i++;
        while (i < value.len && (value.ptr[i] == ' ' || value.ptr[i] == '\t')) i++;
        cf_span pval;
        if (i < value.len && value.ptr[i] == '"') {
            i++;
            size_t vs = i;
            while (i < value.len && value.ptr[i] != '"') {
                if (value.ptr[i] == '\\' && i + 1 < value.len) i++;
                i++;
            }
            if (i >= value.len) {
                while (i < value.len && value.ptr[i] != ';') i++;
                continue;
            }
            cf_span raw = {value.ptr + vs, i - vs};
            i++;
            cf_err err = unescape_quoted(p, raw, &pval);
            if (err != CF_OK) return err;
        } else {
            size_t vs = i;
            while (i < value.len && value.ptr[i] != ';') i++;
            size_t ve = i;
            while (ve > vs && (value.ptr[ve - 1] == ' ' || value.ptr[ve - 1] == '\t')) ve--;
            pval = (cf_span){value.ptr + vs, ve - vs};
        }
        if (span_eq_cstr_ci(pname, "name")) {
            out->name = pval;
            out->has_name = true;
        } else if (span_eq_cstr_ci(pname, "filename")) {
            out->has_filename = true;
            out->filename_empty = pval.len == 0;
        } else if (span_eq_cstr_ci(pname, "filename*")) {
            out->has_filename = true;
            out->filename_empty = pval.len == 0;
        }
    }
    return CF_OK;
}

/* Headers of one part: values are spans inside the request body. */
static cf_err scan_part_headers(cf_params *p, cf_span headers, struct part_disposition *out) {
    size_t pos = 0;
    while (pos < headers.len) {
        size_t end = pos;
        while (end < headers.len && headers.ptr[end] != '\n') end++;
        size_t line_end = end;
        if (line_end > pos && headers.ptr[line_end - 1] == '\r') line_end--;
        cf_span line = {headers.ptr + pos, line_end - pos};
        size_t colon = 0;
        while (colon < line.len && line.ptr[colon] != ':') colon++;
        if (colon < line.len) {
            cf_span name = {line.ptr, colon};
            cf_span value = {line.ptr + colon + 1, line.len - colon - 1};
            while (value.len > 0 && (value.ptr[0] == ' ' || value.ptr[0] == '\t')) {
                value.ptr++;
                value.len--;
            }
            if (span_eq_cstr_ci(name, "content-disposition")) {
                cf_err err = scan_disposition(p, value, out);
                if (err != CF_OK) return err;
            } else if (!out->has_name && span_eq_cstr_ci(name, "content-id")) {
                out->name = value;
                out->has_name = value.len > 0;
            }
        }
        if (end == headers.len) break;
        pos = end + 1;
    }
    return CF_OK;
}

static cf_err parse_multipart(cf_params *p, cf_span body, cf_span boundary) {
    unsigned char delim[72 + 2];
    if (boundary.len + 2 > sizeof(delim)) return CF_INVALID;
    delim[0] = '-';
    delim[1] = '-';
    memcpy(delim + 2, boundary.ptr, boundary.len);
    size_t dlen = boundary.len + 2;
    const unsigned char *b = body.ptr;
    size_t len = body.len;
    const unsigned char *cur = find_delim_line(b, len, delim, dlen, 0);
    if (cur == NULL) return CF_INVALID;
    size_t fields = 0;
    size_t files = 0;
    for (;;) {
        size_t pos = (size_t)(cur - b) + dlen;
        if (pos + 1 < len && b[pos] == '-' && b[pos + 1] == '-') break; /* closing */
        while (pos < len && (b[pos] == ' ' || b[pos] == '\t')) pos++;
        if (pos + 1 < len && b[pos] == '\r' && b[pos + 1] == '\n') {
            pos += 2;
        } else if (pos < len && b[pos] == '\n') {
            pos++;
        } else {
            return CF_INVALID;
        }
        const unsigned char *next = find_delim_line(b, len, delim, dlen, pos);
        if (next == NULL) return CF_INVALID; /* no closing delimiter */
        size_t pe = (size_t)(next - b);
        if (pe >= 2 && b[pe - 2] == '\r' && b[pe - 1] == '\n') {
            pe -= 2;
        } else if (pe >= 1 && b[pe - 1] == '\n') {
            pe -= 1;
        }
        const unsigned char *sep = find_seq(b + pos, pe - pos, (const unsigned char *)"\r\n\r\n", 4);
        size_t header_len, body_off;
        if (sep != NULL) {
            header_len = (size_t)(sep - (b + pos));
            body_off = header_len + 4;
        } else {
            sep = find_seq(b + pos, pe - pos, (const unsigned char *)"\n\n", 2);
            if (sep == NULL) return CF_INVALID;
            header_len = (size_t)(sep - (b + pos));
            body_off = header_len + 2;
        }
        cf_span headers = {b + pos, header_len};
        cf_span value = {b + pos + body_off, pe - pos - body_off};
        struct part_disposition part;
        memset(&part, 0, sizeof(part));
        cf_err err;
        /* Whitespace-only headers are handled by scan_part_headers. */
        if (headers.len > 0) {
            err = scan_part_headers(p, headers, &part);
            if (err != CF_OK) return err;
        }
        if (part.has_filename) {
            /* A blank filename means no file was selected: Rack drops the part. */
            if (!part.filename_empty) files++;
            cur = next;
            continue;
        }
        fields++;
        if (fields > CF_MULTIPART_FIELD_LIMIT) return CF_LIMIT;
        if (!part.has_name || part.name.len == 0) return CF_INVALID;
        if (!utf8_valid(value.ptr, value.len)) return CF_INVALID;
        err = store_pair(p, part.name, true, value);
        if (err != CF_OK) return err;
        cur = next;
    }
    /* Bounds before behavior: the file cap is checked first, then H02's
     * deliberate rejection of upload staging until S01 lands. */
    if (files > CF_MULTIPART_FILE_LIMIT) return CF_LIMIT;
    if (files > 0) return CF_INVALID;
    return CF_OK;
}

/* ----------------------------------------------------------- body dispatch */

struct content_scope {
    bool present; /* a non-empty Content-Type header value (Rust's filter) */
    cf_span ct;   /* the header value when present */
    cf_span media; /* lowercase-comparable base before ';'/',' when nonempty */
    bool has_media;
};

static void content_scope_of(const cf_request *req, struct content_scope *out) {
    memset(out, 0, sizeof(*out));
    cf_span ct = {NULL, 0};
    bool found = false;
    for (size_t i = 0; i < req->header_count && i < 100; i++) {
        if (span_eq_cstr_ci(req->headers[i].name, "content-type")) {
            ct = req->headers[i].value;
            found = true;
            break;
        }
    }
    if (!found || !span_is_ascii_visible(ct)) return; /* absent / unusable */
    if (ct.len == 0) return;
    out->present = true;
    out->ct = ct;
    size_t end = 0;
    while (end < ct.len && ct.ptr[end] != ';' && ct.ptr[end] != ',') end++;
    size_t start = 0;
    while (start < end && (ct.ptr[start] == ' ' || ct.ptr[start] == '\t')) start++;
    while (end > start && (ct.ptr[end - 1] == ' ' || ct.ptr[end - 1] == '\t')) end--;
    if (end > start) {
        out->media = (cf_span){ct.ptr + start, end - start};
        out->has_media = true;
    }
}

static bool media_is_json(cf_span media) {
    return span_eq_cstr_ci(media, "application/json") ||
           span_eq_cstr_ci(media, "text/x-json") ||
           span_eq_cstr_ci(media, "application/jsonrequest") ||
           span_eq_cstr_ci(media, "application/problem+json");
}

static bool media_is_multipart(cf_span media) {
    return span_eq_cstr_ci(media, "multipart/form-data") ||
           span_eq_cstr_ci(media, "multipart/related") ||
           span_eq_cstr_ci(media, "multipart/mixed");
}

static cf_err parse_body_into(cf_params *p, const cf_request *req) {
    struct content_scope scope;
    content_scope_of(req, &scope);
    bool is_multipart = scope.has_media && media_is_multipart(scope.media);
    if (is_multipart) {
        cf_span boundary;
        if (content_type_boundary(scope.ct, &boundary)) {
            return parse_multipart(p, req->body, boundary);
        }
        /* body.rs falls through to the form path when no boundary parses */
    }
    if (!is_multipart && scope.has_media && media_is_json(scope.media) && req->body.len > 0) {
        return parse_json_body(p, req->body);
    }
    if ((scope.has_media && span_eq_cstr_ci(scope.media, "application/x-www-form-urlencoded")) ||
        (!scope.present && req->original_method == CF_POST) || is_multipart) {
        return parse_urlencoded(p, req->body, true);
    }
    return CF_OK;
}

/* ------------------------------------------------------------ public API */

cf_err cf_params_parse(const cf_request *req, cf_params **out) {
    if (out == NULL) return CF_INVALID;
    *out = NULL;
    if (req == NULL) return CF_INVALID;
    cf_params *p = malloc(sizeof(*p));
    if (p == NULL) return CF_NOMEM;
    memset(p, 0, sizeof(*p));
    p->root.kind = CF_PARAM_OBJECT;
    p->node_count = 0; /* the root object is not a parameter value */
    cf_err err = parse_body_into(p, req);
    if (err == CF_OK && req->query.len > 0) {
        /* Ctx::new merges independent ParamMaps: the query string is parsed on
         * its own and then merged over the body top-level by top-level
         * (ParamMap::merge = Hash#merge!), so a query value replaces the whole
         * body value. Nesting within one source still happens while parsing
         * that source; the two trees are never merged recursively. */
        cf_params *q = malloc(sizeof(*q));
        if (q == NULL) {
            cf_params_destroy(p);
            return CF_NOMEM;
        }
        memset(q, 0, sizeof(*q));
        q->root.kind = CF_PARAM_OBJECT;
        q->node_count = 0;
        err = parse_urlencoded(q, req->query, false);
        if (err == CF_OK) err = cf_params_merge(p, q);
        cf_params_destroy(q);
    }
    if (err != CF_OK) {
        cf_params_destroy(p);
        return err;
    }
    *out = p;
    return CF_OK;
}

void cf_params_destroy(cf_params *params) {
    if (params == NULL) return;
    struct cf_params_block *b = params->blocks;
    while (b != NULL) {
        struct cf_params_block *next = b->next;
        free(b);
        b = next;
    }
    free(params);
}

/* Deep copy used by cf_params_merge. */
static cf_err param_copy(cf_params *dst, const cf_param *src, size_t depth, cf_param **out) {
    if (depth >= CF_PARAM_DEPTH_LIMIT) return CF_LIMIT;
    cf_err err;
    switch (src->kind) {
    case CF_PARAM_NULL:
        return params_new_node(dst, CF_PARAM_NULL, out);
    case CF_PARAM_BOOL: {
        err = params_new_node(dst, CF_PARAM_BOOL, out);
        if (err != CF_OK) return err;
        (*out)->u.boolean = src->u.boolean;
        return CF_OK;
    }
    case CF_PARAM_STRING:
        return param_string_copy(dst, src->u.string.ptr, src->u.string.len, out);
    case CF_PARAM_NUMBER: {
        err = params_new_node(dst, CF_PARAM_NUMBER, out);
        if (err != CF_OK) return err;
        (*out)->u.number = src->u.number;
        return CF_OK;
    }
    case CF_PARAM_ARRAY: {
        cf_param *arr;
        err = params_new_node(dst, CF_PARAM_ARRAY, &arr);
        if (err != CF_OK) return err;
        for (size_t i = 0; i < src->u.array.len; i++) {
            cf_param *item;
            err = param_copy(dst, src->u.array.items[i], depth + 1, &item);
            if (err != CF_OK) return err;
            err = array_push(dst, arr, item);
            if (err != CF_OK) return err;
        }
        *out = arr;
        return CF_OK;
    }
    case CF_PARAM_OBJECT: {
        cf_param *obj;
        err = params_new_node(dst, CF_PARAM_OBJECT, &obj);
        if (err != CF_OK) return err;
        for (size_t i = 0; i < src->u.object.len; i++) {
            cf_param *value;
            err = param_copy(dst, src->u.object.entries[i].value, depth + 1, &value);
            if (err != CF_OK) return err;
            err = object_insert(dst, obj, src->u.object.entries[i].key, value);
            if (err != CF_OK) return err;
        }
        *out = obj;
        return CF_OK;
    }
    case CF_PARAM_UPLOAD:
        return CF_INVALID; /* no upload nodes exist until S01 */
    }
    return CF_INTERNAL;
}

cf_err cf_params_merge(cf_params *target, const cf_params *source) {
    if (target == NULL || source == NULL) return CF_INVALID;
    for (size_t i = 0; i < source->root.u.object.len; i++) {
        const struct cf_param_entry *entry = &source->root.u.object.entries[i];
        cf_param *copy;
        /* Depth 0 at the entry value, matching store_nested's convention at
         * the root: every tree cf_params_parse accepted (up to 31 bracket
         * levels) is copied under the same depth-32 bound. */
        cf_err err = param_copy(target, entry->value, 0, &copy);
        if (err != CF_OK) return err;
        err = object_insert(target, &target->root, entry->key, copy);
        if (err != CF_OK) return err;
    }
    return CF_OK;
}

/* ------------------------------------------------------------- accessors */

const cf_param *cf_param_get(const cf_params *params, cf_span name) {
    if (params == NULL) return NULL;
    size_t idx = object_find(&params->root, name);
    if (idx == params->root.u.object.len) return NULL;
    return params->root.u.object.entries[idx].value;
}

cf_param_kind cf_param_type(const cf_param *param) {
    if (param == NULL) return CF_PARAM_NULL;
    return param->kind;
}

cf_err cf_param_string(const cf_param *param, cf_span *out) {
    if (out == NULL) return CF_INVALID;
    *out = (cf_span){NULL, 0};
    if (param == NULL) return CF_NOT_FOUND;
    if (param->kind != CF_PARAM_STRING) return CF_INVALID;
    *out = param->u.string;
    return CF_OK;
}

size_t cf_param_count(const cf_param *param) {
    if (param == NULL) return 0;
    if (param->kind == CF_PARAM_ARRAY) return param->u.array.len;
    if (param->kind == CF_PARAM_OBJECT) return param->u.object.len;
    return 0;
}

const cf_param *cf_param_at(const cf_param *param, size_t index) {
    if (param == NULL || param->kind != CF_PARAM_ARRAY) return NULL;
    if (index >= param->u.array.len) return NULL;
    return param->u.array.items[index];
}

const cf_param *cf_param_field(const cf_param *param, cf_span name) {
    if (param == NULL || param->kind != CF_PARAM_OBJECT) return NULL;
    size_t idx = object_find(param, name);
    if (idx == param->u.object.len) return NULL;
    return param->u.object.entries[idx].value;
}

cf_err cf_param_i64(const cf_param *param, cf_optional_i64 *out) {
    if (out == NULL) return CF_INVALID;
    out->present = false;
    out->value = 0;
    if (param == NULL) return CF_NOT_FOUND;
    if (param->kind == CF_PARAM_NULL) return CF_OK;
    if (param->kind != CF_PARAM_NUMBER || !param->u.number.integral) return CF_INVALID;
    out->present = true;
    out->value = param->u.number.value;
    return CF_OK;
}

cf_err cf_param_bool(const cf_param *param, bool *present, bool *out) {
    if (present == NULL || out == NULL) return CF_INVALID;
    *present = false;
    *out = false;
    if (param == NULL) return CF_NOT_FOUND;
    if (param->kind == CF_PARAM_NULL) return CF_OK;
    if (param->kind != CF_PARAM_BOOL) return CF_INVALID;
    *present = true;
    *out = param->u.boolean;
    return CF_OK;
}

/* -------------------------------------------------------- method override */

/* adapter.rs OVERRIDABLE_METHODS, in its fixed order. LINK/UNLINK have no
 * cf_method value and become CF_OTHER. */
struct override_name {
    const char *name;
    cf_method method;
};

static const struct override_name k_overridable[] = {
    {"GET", CF_GET},       {"HEAD", CF_HEAD},     {"PUT", CF_PUT},
    {"POST", CF_POST},     {"DELETE", CF_DELETE}, {"OPTIONS", CF_OPTIONS},
    {"PATCH", CF_PATCH},   {"LINK", CF_OTHER},    {"UNLINK", CF_OTHER},
};

static void ascii_uppercase(unsigned char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] >= 'a' && s[i] <= 'z') s[i] -= 'a' - 'A';
    }
}

static cf_err apply_override(cf_span candidate, cf_method *out) {
    unsigned char buf[32];
    if (candidate.len > sizeof(buf)) return CF_OK; /* no method token is this long */
    memcpy(buf, candidate.ptr, candidate.len);
    ascii_uppercase(buf, candidate.len);
    for (size_t i = 0; i < sizeof(k_overridable) / sizeof(k_overridable[0]); i++) {
        if (span_eq_cstr((cf_span){buf, candidate.len}, k_overridable[i].name)) {
            *out = k_overridable[i].method;
            return CF_OK;
        }
    }
    return CF_OK;
}

cf_err cf_effective_method(const cf_request *req, cf_method *out) {
    if (req == NULL || out == NULL) return CF_INVALID;
    *out = req->method;
    if (req->method != CF_POST) return CF_OK;

    struct content_scope scope;
    content_scope_of(req, &scope);
    bool form_data = !scope.has_media ||
                     span_eq_cstr_ci(scope.media, "application/x-www-form-urlencoded") ||
                     media_is_multipart(scope.media);

    bool have_candidate = false;
    if (form_data) {
        cf_params *body = malloc(sizeof(*body));
        if (body == NULL) return CF_NOMEM;
        memset(body, 0, sizeof(*body));
        body->root.kind = CF_PARAM_OBJECT;
        body->node_count = 0;
        cf_err err = parse_body_into(body, req);
        if (err != CF_OK) {
            cf_params_destroy(body);
            return err;
        }
        static const unsigned char method_key[] = "_method";
        const cf_param *field = cf_param_get(body, (cf_span){method_key, 7});
        unsigned char buf[32];
        size_t len = 0;
        if (field != NULL && field->kind == CF_PARAM_STRING) {
            have_candidate = true;
            if (field->u.string.len <= sizeof(buf)) {
                if (field->u.string.len > 0) {
                    memcpy(buf, field->u.string.ptr, field->u.string.len);
                }
                len = field->u.string.len;
            } else {
                len = SIZE_MAX; /* present but unusable; header is not consulted */
            }
        }
        cf_params_destroy(body);
        if (have_candidate) {
            if (len == SIZE_MAX) return CF_OK;
            return apply_override((cf_span){buf, len}, out);
        }
    }

    for (size_t i = 0; i < req->header_count && i < 100; i++) {
        if (span_eq_cstr_ci(req->headers[i].name, "x-http-method-override")) {
            cf_span v = req->headers[i].value;
            if (span_is_ascii_visible(v)) {
                return apply_override(v, out);
            }
            return CF_OK;
        }
    }
    return CF_OK;
}
