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

/* The Content-Type read follows http 1.5.0's HeaderValue::to_str (kit
 * body.rs:59 / adapter.rs:277 use `headers.get(CONTENT_TYPE)
 * .and_then(|v| v.to_str().ok())`), which admits HTAB as well as visible
 * ASCII: a trailing/leading tab is readable and `media_type`'s trim removes
 * it.  span_is_ascii_visible above stays the stricter probe for the method
 * override, whose pin path upper-cases without trimming. */
static bool span_is_visible_or_htab(cf_span s) {
    for (size_t i = 0; i < s.len; i++) {
        unsigned char c = s.ptr[i];
        if (c != '\t' && (c < 0x20 || c > 0x7E)) return false;
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

/* Copy `bytes` into the params arena; an empty span stays {NULL, 0}. */
static cf_err arena_span_copy(cf_params *p, const unsigned char *bytes, size_t len,
                              cf_span *out) {
    unsigned char *copy = NULL;
    if (len > 0) {
        copy = arena_alloc(p, len);
        if (copy == NULL) return CF_NOMEM;
        memcpy(copy, bytes, len);
    }
    *out = (cf_span){copy, len};
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

/* ------------------------------------------------- number text (to_s) --- */

/* serde_json 1.0.151's POW10 table (de.rs, `static POW10: [f64; 309]`):
 * 10^k for k in 0..=308, each the correctly rounded double of the literal
 * `1eNNN`. A JSON number's reference `Param::to_s` text is serde_json's
 * `Number` Display: integers via itoa, floats via zmij 1.0.23's shortest
 * round-trip formatter. serde_json parses floats by multiplying/dividing
 * this table (its non-float_roundtrip path), which can differ from a
 * correctly rounded strtod by one ulp (e.g. 9007199254740993.0), so the
 * C side reproduces that arithmetic exactly instead of calling strtod on
 * the lexeme. Generated from the pinned source; hex floats are exact. */
static const double k_json_pow10[309] = {
    0x1.0000000000000p+0, 0x1.4000000000000p+3, 0x1.9000000000000p+6, 0x1.f400000000000p+9,
    0x1.3880000000000p+13, 0x1.86a0000000000p+16, 0x1.e848000000000p+19, 0x1.312d000000000p+23,
    0x1.7d78400000000p+26, 0x1.dcd6500000000p+29, 0x1.2a05f20000000p+33, 0x1.74876e8000000p+36,
    0x1.d1a94a2000000p+39, 0x1.2309ce5400000p+43, 0x1.6bcc41e900000p+46, 0x1.c6bf526340000p+49,
    0x1.1c37937e08000p+53, 0x1.6345785d8a000p+56, 0x1.bc16d674ec800p+59, 0x1.158e460913d00p+63,
    0x1.5af1d78b58c40p+66, 0x1.b1ae4d6e2ef50p+69, 0x1.0f0cf064dd592p+73, 0x1.52d02c7e14af6p+76,
    0x1.a784379d99db4p+79, 0x1.08b2a2c280291p+83, 0x1.4adf4b7320335p+86, 0x1.9d971e4fe8402p+89,
    0x1.027e72f1f1281p+93, 0x1.431e0fae6d721p+96, 0x1.93e5939a08ceap+99, 0x1.f8def8808b024p+102,
    0x1.3b8b5b5056e17p+106, 0x1.8a6e32246c99cp+109, 0x1.ed09bead87c03p+112, 0x1.3426172c74d82p+116,
    0x1.812f9cf7920e3p+119, 0x1.e17b84357691bp+122, 0x1.2ced32a16a1b1p+126, 0x1.78287f49c4a1dp+129,
    0x1.d6329f1c35ca5p+132, 0x1.25dfa371a19e7p+136, 0x1.6f578c4e0a061p+139, 0x1.cb2d6f618c879p+142,
    0x1.1efc659cf7d4cp+146, 0x1.66bb7f0435c9ep+149, 0x1.c06a5ec5433c6p+152, 0x1.18427b3b4a05cp+156,
    0x1.5e531a0a1c873p+159, 0x1.b5e7e08ca3a8fp+162, 0x1.11b0ec57e649ap+166, 0x1.561d276ddfdc0p+169,
    0x1.aba4714957d30p+172, 0x1.0b46c6cdd6e3ep+176, 0x1.4e1878814c9cep+179, 0x1.a19e96a19fc41p+182,
    0x1.05031e2503da9p+186, 0x1.4643e5ae44d13p+189, 0x1.97d4df19d6057p+192, 0x1.fdca16e04b86dp+195,
    0x1.3e9e4e4c2f344p+199, 0x1.8e45e1df3b015p+202, 0x1.f1d75a5709c1bp+205, 0x1.3726987666191p+209,
    0x1.84f03e93ff9f5p+212, 0x1.e62c4e38ff872p+215, 0x1.2fdbb0e39fb47p+219, 0x1.7bd29d1c87a19p+222,
    0x1.dac74463a989fp+225, 0x1.28bc8abe49f64p+229, 0x1.72ebad6ddc73dp+232, 0x1.cfa698c95390cp+235,
    0x1.21c81f7dd43a7p+239, 0x1.6a3a275d49491p+242, 0x1.c4c8b1349b9b5p+245, 0x1.1afd6ec0e1411p+249,
    0x1.61bcca7119916p+252, 0x1.ba2bfd0d5ff5bp+255, 0x1.145b7e285bf99p+259, 0x1.59725db272f7fp+262,
    0x1.afcef51f0fb5fp+265, 0x1.0de1593369d1bp+269, 0x1.5159af8044462p+272, 0x1.a5b01b605557bp+275,
    0x1.078e111c3556dp+279, 0x1.4971956342ac8p+282, 0x1.9bcdfabc1357ap+285, 0x1.0160bcb58c16cp+289,
    0x1.41b8ebe2ef1c7p+292, 0x1.922726dbaae39p+295, 0x1.f6b0f092959c7p+298, 0x1.3a2e965b9d81dp+302,
    0x1.88ba3bf284e24p+305, 0x1.eae8caef261adp+308, 0x1.32d17ed577d0cp+312, 0x1.7f85de8ad5c4fp+315,
    0x1.df67562d8b363p+318, 0x1.2ba095dc7701ep+322, 0x1.7688bb5394c25p+325, 0x1.d42aea2879f2ep+328,
    0x1.249ad2594c37dp+332, 0x1.6dc186ef9f45cp+335, 0x1.c931e8ab87173p+338, 0x1.1dbf316b346e8p+342,
    0x1.652efdc6018a2p+345, 0x1.be7abd3781ecap+348, 0x1.170cb642b133fp+352, 0x1.5ccfe3d35d80ep+355,
    0x1.b403dcc834e12p+358, 0x1.108269fd210cbp+362, 0x1.54a3047c694fep+365, 0x1.a9cbc59b83a3dp+368,
    0x1.0a1f5b8132466p+372, 0x1.4ca732617ed80p+375, 0x1.9fd0fef9de8e0p+378, 0x1.03e29f5c2b18cp+382,
    0x1.44db473335defp+385, 0x1.961219000356bp+388, 0x1.fb969f40042c5p+391, 0x1.3d3e2388029bbp+395,
    0x1.8c8dac6a0342ap+398, 0x1.efb1178484135p+401, 0x1.35ceaeb2d28c1p+405, 0x1.83425a5f872f1p+408,
    0x1.e412f0f768fadp+411, 0x1.2e8bd69aa19ccp+415, 0x1.7a2ecc414a03fp+418, 0x1.d8ba7f519c84fp+421,
    0x1.27748f9301d32p+425, 0x1.7151b377c247ep+428, 0x1.cda62055b2d9ep+431, 0x1.2087d4358fc82p+435,
    0x1.68a9c942f3ba3p+438, 0x1.c2d43b93b0a8cp+441, 0x1.19c4a53c4e697p+445, 0x1.6035ce8b6203dp+448,
    0x1.b843422e3a84dp+451, 0x1.132a095ce4930p+455, 0x1.57f48bb41db7cp+458, 0x1.adf1aea12525bp+461,
    0x1.0cb70d24b7379p+465, 0x1.4fe4d06de5057p+468, 0x1.a3de04895e46dp+471, 0x1.066ac2d5daec4p+475,
    0x1.4805738b51a75p+478, 0x1.9a06d06e26112p+481, 0x1.00444244d7cabp+485, 0x1.405552d60dbd6p+488,
    0x1.906aa78b912ccp+491, 0x1.f485516e7577fp+494, 0x1.38d352e5096afp+498, 0x1.8708279e4bc5bp+501,
    0x1.e8ca3185deb72p+504, 0x1.317e5ef3ab327p+508, 0x1.7dddf6b095ff1p+511, 0x1.dd55745cbb7edp+514,
    0x1.2a5568b9f52f4p+518, 0x1.74eac2e8727b1p+521, 0x1.d22573a28f19dp+524, 0x1.2357684599702p+528,
    0x1.6c2d4256ffcc3p+531, 0x1.c73892ecbfbf4p+534, 0x1.1c835bd3f7d78p+538, 0x1.63a432c8f5cd6p+541,
    0x1.bc8d3f7b3340cp+544, 0x1.15d847ad00087p+548, 0x1.5b4e5998400a9p+551, 0x1.b221effe500d4p+554,
    0x1.0f5535fef2084p+558, 0x1.532a837eae8a5p+561, 0x1.a7f5245e5a2cfp+564, 0x1.08f936baf85c1p+568,
    0x1.4b378469b6732p+571, 0x1.9e056584240fep+574, 0x1.02c35f729689fp+578, 0x1.4374374f3c2c6p+581,
    0x1.945145230b378p+584, 0x1.f965966bce056p+587, 0x1.3bdf7e0360c36p+591, 0x1.8ad75d8438f43p+594,
    0x1.ed8d34e547314p+597, 0x1.3478410f4c7ecp+601, 0x1.819651531f9e8p+604, 0x1.e1fbe5a7e7861p+607,
    0x1.2d3d6f88f0b3dp+611, 0x1.788ccb6b2ce0cp+614, 0x1.d6affe45f818fp+617, 0x1.262dfeebbb0f9p+621,
    0x1.6fb97ea6a9d38p+624, 0x1.cba7de5054486p+627, 0x1.1f48eaf234ad4p+631, 0x1.671b25aec1d89p+634,
    0x1.c0e1ef1a724ebp+637, 0x1.188d357087713p+641, 0x1.5eb082cca94d7p+644, 0x1.b65ca37fd3a0dp+647,
    0x1.11f9e62fe4448p+651, 0x1.56785fbbdd55ap+654, 0x1.ac1677aad4ab1p+657, 0x1.0b8e0acac4eafp+661,
    0x1.4e718d7d7625ap+664, 0x1.a20df0dcd3af1p+667, 0x1.0548b68a044d6p+671, 0x1.469ae42c8560cp+674,
    0x1.98419d37a6b8fp+677, 0x1.fe52048590673p+680, 0x1.3ef342d37a408p+684, 0x1.8eb0138858d0ap+687,
    0x1.f25c186a6f04cp+690, 0x1.37798f4285630p+694, 0x1.8557f31326bbbp+697, 0x1.e6adefd7f06aap+700,
    0x1.302cb5e6f642ap+704, 0x1.7c37e360b3d35p+707, 0x1.db45dc38e0c82p+710, 0x1.290ba9a38c7d1p+714,
    0x1.734e940c6f9c6p+717, 0x1.d022390f8b837p+720, 0x1.221563a9b7323p+724, 0x1.6a9abc9424febp+727,
    0x1.c5416bb92e3e6p+730, 0x1.1b48e353bce70p+734, 0x1.621b1c28ac20cp+737, 0x1.baa1e332d728fp+740,
    0x1.14a52dffc6799p+744, 0x1.59ce797fb817fp+747, 0x1.b04217dfa61dfp+750, 0x1.0e294eebc7d2cp+754,
    0x1.51b3a2a6b9c76p+757, 0x1.a6208b5068394p+760, 0x1.07d457124123dp+764, 0x1.49c96cd6d16ccp+767,
    0x1.9c3bc80c85c7fp+770, 0x1.01a55d07d39cfp+774, 0x1.420eb449c8843p+777, 0x1.9292615c3aa54p+780,
    0x1.f736f9b3494e9p+783, 0x1.3a825c100dd11p+787, 0x1.8922f31411456p+790, 0x1.eb6bafd91596bp+793,
    0x1.33234de7ad7e3p+797, 0x1.7fec216198ddcp+800, 0x1.dfe729b9ff153p+803, 0x1.2bf07a143f6d4p+807,
    0x1.76ec98994f489p+810, 0x1.d4a7bebfa31abp+813, 0x1.24e8d737c5f0bp+817, 0x1.6e230d05b76cdp+820,
    0x1.c9abd04725481p+823, 0x1.1e0b622c774d0p+827, 0x1.658e3ab795204p+830, 0x1.bef1c9657a686p+833,
    0x1.17571ddf6c814p+837, 0x1.5d2ce55747a18p+840, 0x1.b4781ead1989ep+843, 0x1.10cb132c2ff63p+847,
    0x1.54fdd7f73bf3cp+850, 0x1.aa3d4df50af0bp+853, 0x1.0a6650b926d67p+857, 0x1.4cffe4e7708c0p+860,
    0x1.a03fde214caf1p+863, 0x1.0427ead4cfed6p+867, 0x1.4531e58a03e8cp+870, 0x1.967e5eec84e2fp+873,
    0x1.fc1df6a7a61bbp+876, 0x1.3d92ba28c7d15p+880, 0x1.8cf768b2f9c5ap+883, 0x1.f03542dfb8370p+886,
    0x1.362149cbd3226p+890, 0x1.83a99c3ec7eb0p+893, 0x1.e494034e79e5cp+896, 0x1.2edc82110c2f9p+900,
    0x1.7a93a2954f3b8p+903, 0x1.d9388b3aa30a5p+906, 0x1.27c35704a5e67p+910, 0x1.71b42cc5cf601p+913,
    0x1.ce2137f743382p+916, 0x1.20d4c2fa8a031p+920, 0x1.6909f3b92c83dp+923, 0x1.c34c70a777a4dp+926,
    0x1.1a0fc668aac70p+930, 0x1.6093b802d578cp+933, 0x1.b8b8a6038ad6fp+936, 0x1.137367c236c65p+940,
    0x1.585041b2c477fp+943, 0x1.ae64521f7595ep+946, 0x1.0cfeb353a97dbp+950, 0x1.503e602893dd2p+953,
    0x1.a44df832b8d46p+956, 0x1.06b0bb1fb384cp+960, 0x1.485ce9e7a065fp+963, 0x1.9a742461887f6p+966,
    0x1.008896bcf54fap+970, 0x1.40aabc6c32a38p+973, 0x1.90d56b873f4c7p+976, 0x1.f50ac6690f1f8p+979,
    0x1.3926bc01a973bp+983, 0x1.87706b0213d0ap+986, 0x1.e94c85c298c4cp+989, 0x1.31cfd3999f7b0p+993,
    0x1.7e43c8800759cp+996, 0x1.ddd4baa009303p+999, 0x1.2aa4f4a405be2p+1003, 0x1.754e31cd072dap+1006,
    0x1.d2a1be4048f90p+1009, 0x1.23a516e82d9bap+1013, 0x1.6c8e5ca239029p+1016, 0x1.c7b1f3cac7433p+1019,
    0x1.1ccf385ebc8a0p+1023,
};

enum json_number_kind { JSON_NUMBER_U64, JSON_NUMBER_I64, JSON_NUMBER_F64 };

struct json_number_value {
    enum json_number_kind kind;
    uint64_t u; /* JSON_NUMBER_U64 */
    int64_t i;  /* JSON_NUMBER_I64 */
    double f;   /* JSON_NUMBER_F64 */
};

static bool u64_mul10_add_overflows(uint64_t value, unsigned digit) {
    return value > UINT64_MAX / 10u ||
           (value == UINT64_MAX / 10u && digit > UINT64_MAX % 10u);
}

/* serde_json 1.0.151 de.rs parse_integer/parse_decimal/parse_exponent and
 * f64_from_parts (the not(float_roundtrip) path the workspace builds), for a
 * number lexeme yyjson already validated. CF_INVALID is serde_json's
 * NumberOutOfRange parse error, which fails the whole JSON body there too. */
static cf_err json_number_parse(const unsigned char *s, size_t n,
                                struct json_number_value *out) {
    size_t at = 0;
    bool positive = true;
    if (s[at] == '-') {
        positive = false;
        at++;
    }
    uint64_t sig = 0;
    int64_t exp_before = 0; /* long-integer digit count / fraction exponent */
    bool is_float = false;

    if (s[at] == '0') {
        at++; /* the single leading zero */
    } else {
        sig = (uint64_t)(s[at] - '0');
        at++;
        bool overflowed = false;
        while (at < n && ascii_digit(s[at])) {
            unsigned d = (unsigned)(s[at] - '0');
            if (u64_mul10_add_overflows(sig, d)) {
                overflowed = true;
                break;
            }
            sig = sig * 10u + (uint64_t)d;
            at++;
        }
        if (overflowed) {
            /* parse_long_integer: further integer digits only move the
             * exponent; the value becomes a float. */
            while (at < n && ascii_digit(s[at])) {
                exp_before++;
                at++;
            }
            is_float = true;
        }
    }
    if (at < n && s[at] == '.') {
        is_float = true;
        at++;
        int64_t after = 0;
        while (at < n && ascii_digit(s[at])) {
            unsigned d = (unsigned)(s[at] - '0');
            if (u64_mul10_add_overflows(sig, d)) {
                /* parse_decimal_overflow: the rest of the fraction cannot
                 * change the u64 significand or the exponent. */
                while (at < n && ascii_digit(s[at])) at++;
                break;
            }
            sig = sig * 10u + (uint64_t)d;
            after--;
            at++;
        }
        exp_before += after;
    }
    bool has_exp = false;
    bool positive_exp = true;
    int64_t exp = 0;
    if (at < n && (s[at] == 'e' || s[at] == 'E')) {
        has_exp = true;
        is_float = true;
        at++;
        if (s[at] == '+') {
            at++;
        } else if (s[at] == '-') {
            positive_exp = false;
            at++;
        }
        exp = (int64_t)(s[at] - '0');
        at++;
        while (at < n && ascii_digit(s[at])) {
            unsigned d = (unsigned)(s[at] - '0');
            if (exp > (INT32_MAX - (int64_t)d) / 10) {
                /* parse_exponent_overflow: the exponent alone is out of
                 * range. A nonzero significand with a positive exponent is a
                 * parse error; everything else is +/-0.0. */
                if (sig != 0 && positive_exp) return CF_INVALID;
                out->kind = JSON_NUMBER_F64;
                out->f = positive ? 0.0 : -0.0;
                return CF_OK;
            }
            exp = exp * 10 + (int64_t)d;
            at++;
        }
    }

    if (!is_float) {
        if (positive) {
            out->kind = JSON_NUMBER_U64;
            out->u = sig;
            return CF_OK;
        }
        /* de.rs parse_number: neg = (significand as i64).wrapping_neg();
         * a non-negative result (a zero significand, or one past 2^63)
         * becomes a float. */
        uint64_t wrapped = 0u - sig;
        int64_t neg;
        memcpy(&neg, &wrapped, sizeof neg);
        if (neg >= 0) {
            out->kind = JSON_NUMBER_F64;
            out->f = -(double)sig;
            return CF_OK;
        }
        out->kind = JSON_NUMBER_I64;
        out->i = neg;
        return CF_OK;
    }

    int64_t final_exp = exp_before;
    if (has_exp) {
        final_exp = positive_exp ? final_exp + exp : final_exp - exp;
    }
    double value = (double)sig;
    int64_t e = final_exp;
    for (;;) {
        if (e >= -308 && e <= 308) {
            if (e >= 0) {
                value *= k_json_pow10[e];
                if (isinf(value)) return CF_INVALID;
            } else {
                value /= k_json_pow10[-e];
            }
            break;
        }
        if (value == 0.0) break;
        if (e >= 0) return CF_INVALID;
        value /= 1e308;
        e += 308;
    }
    if (!positive) value = -value;
    out->kind = JSON_NUMBER_F64;
    out->f = value;
    return CF_OK;
}

/* zmij 1.0.23's shortest round-trip float rendering (Buffer::format_finite),
 * the formatter serde_json 1.0.151's Number Display uses: decimal notation
 * when the decimal exponent of the leading digit is in -5..=15, otherwise
 * scientific with an explicit sign and no zero padding. The shortest
 * significand is found by printing at increasing precision until the decimal
 * reads back exactly (the nearest p+1-digit decimal round-trips whenever any
 * does), then laid out with zmij's fixed/scientific rules. */
static bool number_format_f64(double value, char *out, size_t *out_len) {
    if (value == 0.0) {
        size_t w = 0;
        if (signbit(value)) out[w++] = '-';
        out[w++] = '0';
        out[w++] = '.';
        out[w++] = '0';
        *out_len = w;
        return true;
    }
    char printed[64];
    int precision = -1;
    for (int p = 0; p <= 17; p++) {
        snprintf(printed, sizeof printed, "%.*e", p, value);
        if (strtod(printed, NULL) == value) {
            precision = p;
            break;
        }
    }
    if (precision < 0) return false;

    char digits[32];
    size_t nd = 0;
    size_t at = 0;
    bool negative = printed[0] == '-';
    if (negative) at++;
    digits[nd++] = printed[at++];
    if (printed[at] == '.') {
        at++;
        while (printed[at] != 'e' && printed[at] != 'E') {
            digits[nd++] = printed[at++];
        }
    }
    int dec_exp = atoi(printed + at + 1);

    size_t w = 0;
    if (negative) out[w++] = '-';
    if (dec_exp >= -5 && dec_exp <= 15) {
        if ((int)nd - 1 <= dec_exp) { /* 1234e7 -> 12340000000.0 */
            memcpy(out + w, digits, nd);
            w += nd;
            for (int k = 0; k < dec_exp + 1 - (int)nd; k++) out[w++] = '0';
            out[w++] = '.';
            out[w++] = '0';
        } else if (dec_exp >= 0) { /* 1234e-2 -> 12.34 */
            size_t head = (size_t)dec_exp + 1;
            memcpy(out + w, digits, head);
            w += head;
            out[w++] = '.';
            memcpy(out + w, digits + head, nd - head);
            w += nd - head;
        } else { /* 1234e-6 -> 0.001234 */
            out[w++] = '0';
            out[w++] = '.';
            for (int k = 0; k < -dec_exp - 1; k++) out[w++] = '0';
            memcpy(out + w, digits, nd);
            w += nd;
        }
    } else { /* 1234e30 -> 1.234e+33 */
        out[w++] = digits[0];
        if (nd > 1) {
            out[w++] = '.';
            memcpy(out + w, digits + 1, nd - 1);
            w += nd - 1;
        }
        out[w++] = 'e';
        if (dec_exp >= 0) {
            out[w++] = '+';
        } else {
            out[w++] = '-';
            dec_exp = -dec_exp;
        }
        char expbuf[8];
        int en = snprintf(expbuf, sizeof expbuf, "%d", dec_exp);
        memcpy(out + w, expbuf, (size_t)en);
        w += (size_t)en;
    }
    *out_len = w;
    return true;
}

/* `Param::to_s` number text: itoa for PosInt/NegInt, zmij for Float. */
static cf_err json_number_text(const struct json_number_value *value, char *buf,
                               size_t cap, size_t *len) {
    switch (value->kind) {
    case JSON_NUMBER_U64: {
        int n = snprintf(buf, cap, "%llu", (unsigned long long)value->u);
        if (n < 0 || (size_t)n >= cap) return CF_INTERNAL;
        *len = (size_t)n;
        return CF_OK;
    }
    case JSON_NUMBER_I64: {
        int n = snprintf(buf, cap, "%lld", (long long)value->i);
        if (n < 0 || (size_t)n >= cap) return CF_INTERNAL;
        *len = (size_t)n;
        return CF_OK;
    }
    case JSON_NUMBER_F64:
        if (!number_format_f64(value->f, buf, len)) return CF_INTERNAL;
        return CF_OK;
    }
    return CF_INTERNAL;
}

static cf_err param_number_copy(cf_params *p, const unsigned char *raw, size_t len, cf_param **out) {
    bool integral;
    int64_t value;
    cf_err err = number_analyze(raw, len, &integral, &value);
    if (err != CF_OK) return err;
    struct json_number_value parsed;
    err = json_number_parse(raw, len, &parsed);
    if (err != CF_OK) return err;
    char text[64];
    size_t text_len = 0;
    err = json_number_text(&parsed, text, sizeof text, &text_len);
    if (err != CF_OK) return err;
    cf_param *n;
    err = params_new_node(p, CF_PARAM_NUMBER, &n);
    if (err != CF_OK) return err;
    n->u.number.integral = integral;
    n->u.number.value = value;
    err = arena_span_copy(p, raw, len, &n->u.number.raw);
    if (err != CF_OK) return err;
    err = arena_span_copy(p, (const unsigned char *)text, text_len,
                          &n->u.number.text);
    if (err != CF_OK) return err;
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
        if (pe < pos) return CF_INVALID; /* malformed delimiter ordering */
        /* Strip only the line break that introduces the next delimiter: it
         * ends this part's body and lies inside [pos, pe). The bytes before
         * `pos` belong to the previous delimiter line, so an empty malformed
         * part between adjacent boundaries must not consume them (P1A-01:
         * the former absolute `pe >= 2` check underflowed `pe - pos`). */
        if (pe - pos >= 2 && b[pe - 2] == '\r' && b[pe - 1] == '\n') {
            pe -= 2;
        } else if (pe - pos >= 1 && b[pe - 1] == '\n') {
            pe -= 1;
        }
        size_t part_len = pe - pos;
        const unsigned char *sep = find_seq(b + pos, part_len, (const unsigned char *)"\r\n\r\n", 4);
        size_t header_len, body_off;
        if (sep != NULL) {
            header_len = (size_t)(sep - (b + pos));
            body_off = header_len + 4;
        } else {
            sep = find_seq(b + pos, part_len, (const unsigned char *)"\n\n", 2);
            if (sep == NULL) return CF_INVALID; /* no header/body separator */
            header_len = (size_t)(sep - (b + pos));
            body_off = header_len + 2;
        }
        if (header_len > part_len || body_off > part_len) return CF_INVALID;
        cf_span headers = {b + pos, header_len};
        cf_span value = {b + pos + body_off, part_len - body_off};
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
    if (!found || !span_is_visible_or_htab(ct)) return; /* absent / unusable */
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
        (*out)->u.number.integral = src->u.number.integral;
        (*out)->u.number.value = src->u.number.value;
        err = arena_span_copy(dst, src->u.number.raw.ptr, src->u.number.raw.len,
                              &(*out)->u.number.raw);
        if (err != CF_OK) return err;
        return arena_span_copy(dst, src->u.number.text.ptr,
                               src->u.number.text.len, &(*out)->u.number.text);
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

cf_err cf_param_to_s(const cf_param *param, cf_span *out) {
    if (out == NULL) return CF_INVALID;
    *out = (cf_span){NULL, 0};
    if (param == NULL) return CF_NOT_FOUND;
    static const unsigned char k_true[] = "true";
    static const unsigned char k_false[] = "false";
    switch (param->kind) {
    case CF_PARAM_NULL:
        return CF_OK; /* NilClass#to_s is "" */
    case CF_PARAM_BOOL:
        *out = param->u.boolean ? (cf_span){k_true, 4} : (cf_span){k_false, 5};
        return CF_OK;
    case CF_PARAM_NUMBER:
        *out = param->u.number.text;
        return CF_OK;
    case CF_PARAM_STRING:
        *out = param->u.string;
        return CF_OK;
    default:
        return CF_NOT_FOUND; /* Array/Hash/Upload: the reference's None */
    }
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
