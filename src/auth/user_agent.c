/* src/auth/user_agent.c — port of the useragent gem (0.16.11) surface the
 * platform layer needs.  Reference:
 * tmp/rust-ref/crates/campfire/src/concerns/user_agent.rs (itself a port of
 * the gem, checked there against tmp/rust-ref/vectors/campfire_user_agents.json,
 * copied to tests/fixtures/ua/).
 *
 * The port keeps every gem quirk the reference documents: UserAgent.parse's
 * product scanner (quote backtracking, `,gzip(gfe)`), the browser class table
 * and its per-class browser/version/platform/os answers, UserAgent::Version's
 * incomplete ordering, Ruby's ASCII-only \s and \d, and the `try_*` methods
 * distinguishing "absent" from "the gem raised".  Where the gem raises the
 * result is CF_UA_RAISED and platform.c maps it to false / "" exactly as the
 * Rust `to_view` does.
 *
 * Unicode: the application only ever compares case-insensitively against
 * lowercase ASCII needles, so case folding is streaming and per codepoint.
 * Rust's char::to_lowercase maps a few non-ASCII codepoints to ASCII text, and
 * the corpus pins two of them (U+212A KELVIN SIGN -> k, U+0130 LATIN CAPITAL
 * LETTER I WITH DOT ABOVE -> "i" + U+0307); both are folded exactly.  Latin-1
 * upper/lowercase pairs fold too.  Codepoints outside that subset stand for
 * themselves: for every sequence in the pinned corpus this is
 * indistinguishable from Rust's full table (the differing codepoints never
 * fold into an ASCII needle), and non-ASCII input only reaches the parse
 * through a caller that bypasses the kit's readability gate (the corpus and
 * attack probes do; production does not: HeaderValue::to_str admits only
 * visible ASCII and HTAB).
 *
 * All spans borrow the User-Agent string or a literal; only the agent's
 * product/comment arrays are owned.
 */
#include "user_agent.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- helpers */

static cf_span ua_lit(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

static bool ua_span_eq_lit(cf_span a, const char *b) {
    size_t n = strlen(b);
    return a.len == n && (n == 0 || memcmp(a.ptr, b, n) == 0);
}

static bool ua_span_eq_span(cf_span a, cf_span b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.ptr, b.ptr, a.len) == 0);
}

static bool ua_span_contains_lit(cf_span hay, const char *needle) {
    size_t n = strlen(needle);
    if (n == 0) return true;
    if (hay.len < n) return false;
    for (size_t i = 0; i + n <= hay.len; i++) {
        if (memcmp(hay.ptr + i, needle, n) == 0) return true;
    }
    return false;
}

static bool ua_starts_with_lit(cf_span s, const char *prefix) {
    size_t n = strlen(prefix);
    return s.len >= n && memcmp(s.ptr, prefix, n) == 0;
}

/* Ruby's `\s` (user_agent.rs is_ruby_space): ASCII only. */
static bool ua_ruby_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
           c == '\r';
}

/* ruby_compat::strip: leading/trailing NUL and ASCII whitespace. */
static cf_span ua_ruby_strip(cf_span s) {
    if (s.len == 0) return s; /* keep a possibly-NULL span untouched */
    size_t start = 0, end = s.len;
    while (start < end &&
           (s.ptr[start] == '\0' || ua_ruby_space(s.ptr[start]))) {
        start++;
    }
    while (end > start &&
           (s.ptr[end - 1] == '\0' || ua_ruby_space(s.ptr[end - 1]))) {
        end--;
    }
    return (cf_span){s.ptr + start, end - start};
}

static bool ua_ascii_digit(unsigned char c) { return c >= '0' && c <= '9'; }

static bool ua_ascii_alpha(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static bool ua_ascii_alnum(unsigned char c) {
    return ua_ascii_alpha(c) || ua_ascii_digit(c);
}

static unsigned char ua_ascii_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

/* Decode one UTF-8 codepoint; an invalid byte stands for itself.  Returns the
 * bytes consumed (at least 1 when n > 0). */
static size_t ua_decode(const unsigned char *p, size_t n, uint32_t *out) {
    unsigned char c = p[0];
    if (c < 0x80) {
        *out = c;
        return 1;
    }
    size_t need;
    uint32_t cp;
    if (c >= 0xc2 && c <= 0xdf) {
        need = 1;
        cp = c & 0x1f;
    } else if (c >= 0xe0 && c <= 0xef) {
        need = 2;
        cp = c & 0x0f;
    } else if (c >= 0xf0 && c <= 0xf4) {
        need = 3;
        cp = c & 0x07;
    } else {
        *out = c;
        return 1;
    }
    if (need >= n) {
        *out = c;
        return 1;
    }
    for (size_t i = 1; i <= need; i++) {
        if ((p[i] & 0xc0) != 0x80) {
            *out = c;
            return 1;
        }
        cp = (cp << 6) | (p[i] & 0x3f);
    }
    if ((need == 2 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) ||
        (need == 3 && (cp < 0x10000 || cp > 0x10ffff))) {
        *out = c;
        return 1;
    }
    *out = cp;
    return need + 1;
}

static size_t ua_encode(uint32_t cp, unsigned char out[4]) {
    if (cp < 0x80) {
        out[0] = (unsigned char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (unsigned char)(0xc0 | (cp >> 6));
        out[1] = (unsigned char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (unsigned char)(0xe0 | (cp >> 12));
        out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (unsigned char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (unsigned char)(0xf0 | (cp >> 18));
    out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (unsigned char)(0x80 | (cp & 0x3f));
    return 4;
}

/* One codepoint's lowercase bytes (Rust char::to_lowercase for the ported
 * subset; everything else stands for itself). */
static size_t ua_fold_seq(const unsigned char *p, size_t n, uint32_t cp,
                          unsigned char out[4]) {
    if (cp < 0x80) {
        out[0] = ua_ascii_lower((unsigned char)cp);
        return 1;
    }
    if (cp == 0x212a) { /* KELVIN SIGN -> k */
        out[0] = 'k';
        return 1;
    }
    if (cp == 0x130) { /* LATIN CAPITAL I WITH DOT ABOVE -> i + U+0307 */
        out[0] = 'i';
        out[1] = 0xcc;
        out[2] = 0x87;
        return 3;
    }
    if ((cp >= 0xc0 && cp <= 0xd6) || (cp >= 0xd8 && cp <= 0xde)) {
        out[0] = (unsigned char)(cp + 0x20);
        return 1;
    }
    if (cp == 0x178) { /* Ÿ -> ÿ */
        return ua_encode(0xff, out);
    }
    if (n > 4) n = 4;
    memcpy(out, p, n);
    return n;
}

/* Streaming lowercase cursor over a span. */
typedef struct {
    cf_span s;
    size_t pos;
    unsigned char buf[4];
    size_t buf_len, buf_pos;
} ua_fold_cursor;

static bool ua_fold_next(ua_fold_cursor *c, unsigned char *out) {
    while (c->buf_pos >= c->buf_len) {
        if (c->pos >= c->s.len) return false;
        uint32_t cp;
        size_t used = ua_decode(c->s.ptr + c->pos, c->s.len - c->pos, &cp);
        c->buf_len = ua_fold_seq(c->s.ptr + c->pos, used, cp, c->buf);
        c->buf_pos = 0;
        c->pos += used;
    }
    *out = c->buf[c->buf_pos++];
    return true;
}

/* `a.downcase == b.downcase` over two folded streams. */
static bool ua_fold_eq_span(cf_span a, cf_span b) {
    ua_fold_cursor x = {a, 0, {0}, 0, 0};
    ua_fold_cursor y = {b, 0, {0}, 0, 0};
    unsigned char bx, by;
    for (;;) {
        bool has_x = ua_fold_next(&x, &bx);
        bool has_y = ua_fold_next(&y, &by);
        if (!has_x || !has_y) return has_x == has_y;
        if (bx != by) return false;
    }
}

static bool ua_fold_eq_lit(cf_span haystack, const char *literal) {
    return ua_fold_eq_span(haystack, ua_lit(literal));
}

bool cf_ua_fold_eq(cf_span haystack, const char *literal) {
    return ua_fold_eq_lit(haystack, literal);
}

/* `haystack.downcase.include?(needle)` for a lowercase ASCII needle. */
bool cf_ua_fold_contains(cf_span haystack, const char *needle) {
    size_t needle_len = strlen(needle);
    if (needle_len == 0) return true;
    for (size_t start = 0; start < haystack.len;) {
        ua_fold_cursor c = {haystack, start, {0}, 0, 0};
        size_t k = 0;
        unsigned char byte;
        while (k < needle_len && ua_fold_next(&c, &byte)) {
            if (byte != (unsigned char)needle[k]) break;
            k++;
        }
        if (k == needle_len) return true;
        uint32_t cp;
        start += ua_decode(haystack.ptr + start, haystack.len - start, &cp);
    }
    return false;
}

/* ActiveSupport `present?` for one codepoint (Unicode White_Space). */
static bool ua_cp_whitespace(uint32_t cp) {
    return (cp >= 0x09 && cp <= 0x0d) || cp == 0x20 || cp == 0x85 ||
           cp == 0xa0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) ||
           cp == 0x2028 || cp == 0x2029 || cp == 0x202f || cp == 0x205f ||
           cp == 0x3000;
}

bool cf_ua_present(cf_span text) {
    for (size_t i = 0; i < text.len;) {
        uint32_t cp;
        size_t used = ua_decode(text.ptr + i, text.len - i, &cp);
        if (!ua_cp_whitespace(cp)) return true;
        i += used;
    }
    return false;
}

bool cf_ua_header_readable(cf_span text) {
    /* HeaderValue::to_str -> is_visible_ascii (http 1.5.0 value.rs): a byte is
     * readable when it is HTAB or 0x20..=0x7E.  Every byte >= 0x80 fails,
     * valid UTF-8 included: obs-text (which H01 admits) is never readable, so
     * a header carrying one answers the reference's `None`.  The other
     * controls cannot reach here (H01 rejects them), but the predicate is
     * exact for any caller. */
    for (size_t i = 0; i < text.len; i++) {
        unsigned char c = text.ptr[i];
        if (c != '\t' && (c < 0x20 || c > 0x7E)) return false;
    }
    return true;
}

/* ------------------------------------------------------------- versions */

typedef struct {
    bool is_int;
    cf_span digits; /* leading zeros stripped */
    cf_span text;   /* string segments */
} ua_segment;

/* scan_sequences, up to `max` leading segments. */
static size_t ua_scan_segments(cf_span s, ua_segment *out, size_t max) {
    size_t count = 0, i = 0;
    while (i < s.len && count < max) {
        unsigned char c = s.ptr[i];
        if (ua_ascii_digit(c)) {
            size_t start = i;
            while (i < s.len && ua_ascii_digit(s.ptr[i])) i++;
            cf_span digits = {s.ptr + start, i - start};
            while (digits.len > 1 && digits.ptr[0] == '0') {
                digits.ptr++;
                digits.len--;
            }
            out[count++] = (ua_segment){true, digits, {NULL, 0}};
        } else if (ua_ascii_alpha(c)) {
            size_t end = i + 1;
            while (end < s.len &&
                   (ua_ascii_alnum(s.ptr[end]) || s.ptr[end] == '-')) {
                end++;
            }
            if (end == s.len) {
                out[count++] =
                    (ua_segment){false, {NULL, 0}, {s.ptr + i, s.len - i}};
                i = end;
            } else {
                i++;
            }
        } else {
            i++;
        }
    }
    return count;
}

static cf_ua_version ua_version_new(cf_span s) {
    cf_ua_version v;
    v.text = s;
    v.blank = true;
    for (size_t i = 0; i < s.len; i++) {
        if (!ua_ruby_space(s.ptr[i])) {
            v.blank = false;
            break;
        }
    }
    size_t digits = 0;
    while (digits < s.len && ua_ascii_digit(s.ptr[digits])) digits++;
    v.comparable = !v.blank && digits > 0 &&
                   (digits == s.len || s.ptr[digits] == '.');
    return v;
}

/* Version#to_a, truncated to the six segments the comparison reads. */
static size_t ua_version_segments(cf_ua_version v, ua_segment out[6]) {
    if (v.blank) return 0;
    if (!v.comparable) {
        out[0] = (ua_segment){false, {NULL, 0}, v.text};
        return 1;
    }
    return ua_scan_segments(v.text, out, 6);
}

/* Version#<=> compared with `Less`; the gem's ordering is not total. */
static int ua_version_cmp(cf_ua_version a, cf_ua_version b) {
    if (!a.comparable) {
        return ua_span_eq_span(a.text, b.text) ? 0 : -1;
    }
    ua_segment sa[6], sb[6];
    size_t na = ua_version_segments(a, sa), nb = ua_version_segments(b, sb);
    /* The Rust fills a missing segment with `Segment::Int("0")`: digits "0",
     * length 1 (scan_sequences/`Segment::int` never yield a zero-length Int).
     * A shorter length here would order a real `0` segment above a missing
     * one, so `1` and `1.0` would not compare equal. */
    static const unsigned char UA_ZERO_DIGIT = '0';
    static const ua_segment ZERO = {true, {&UA_ZERO_DIGIT, 1}, {NULL, 0}};
    for (size_t i = 0; i < 6; i++) {
        const ua_segment *x = i < na ? &sa[i] : &ZERO;
        const ua_segment *y = i < nb ? &sb[i] : &ZERO;
        if (!x->is_int && y->is_int) return -1;
        if (x->is_int && !y->is_int) return 1;
        if (x->is_int) {
            if (x->digits.len != y->digits.len) {
                return x->digits.len < y->digits.len ? -1 : 1;
            }
            /* Both spans are nonempty (scan_sequences keeps "0"). */
            int c = memcmp(x->digits.ptr, y->digits.ptr, x->digits.len);
            if (c != 0) return c < 0 ? -1 : 1;
        } else {
            size_t n = x->text.len < y->text.len ? x->text.len : y->text.len;
            int c = n == 0 ? 0 : memcmp(x->text.ptr, y->text.ptr, n);
            if (c != 0) return c < 0 ? -1 : 1;
            if (x->text.len != y->text.len) {
                return x->text.len < y->text.len ? -1 : 1;
            }
        }
    }
    return 0;
}

bool cf_ua_version_lt(cf_ua_version a, cf_ua_version b) {
    return ua_version_cmp(a, b) < 0;
}

bool cf_ua_version_lt_lit(cf_ua_version a, const char *text) {
    return ua_version_cmp(a, ua_version_new(ua_lit(text))) < 0;
}

int cf_ua_version_cmp_lit(const char *a, const char *b) {
    return ua_version_cmp(ua_version_new(ua_lit(a)), ua_version_new(ua_lit(b)));
}

/* ------------------------------------------------------- product scanner */

struct ua_build_product {
    cf_span product;
    cf_span version;
    cf_span comment_raw;
    bool has_comment;
};

/* UserAgent::MATCHER at the start of s; *consumed is the match length. */
static bool ua_match_product(cf_span s, size_t *consumed,
                             struct ua_build_product *out) {
    size_t quotes = 0;
    while (quotes < s.len &&
           (s.ptr[quotes] == '\'' || s.ptr[quotes] == '"')) {
        quotes++;
    }
    size_t start;
    if (quotes < s.len && s.ptr[quotes] != '/' &&
        !ua_ruby_space(s.ptr[quotes])) {
        start = quotes;
    } else if (quotes > 0) {
        start = quotes - 1;
    } else {
        return false;
    }

    size_t i = start + 1;
    while (i < s.len && s.ptr[i] != '/' && !ua_ruby_space(s.ptr[i])) i++;
    cf_span product = {s.ptr + start, i - start};

    if (i < s.len && s.ptr[i] == '/') i++;
    size_t version_start = i;
    while (i < s.len && !ua_ruby_space(s.ptr[i]) && s.ptr[i] != ',') i++;
    cf_span version = {s.ptr + version_start, i - version_start};

    cf_span comment = {NULL, 0};
    bool has_comment = false;
    if (i < s.len && ua_ruby_space(s.ptr[i]) && i + 1 < s.len &&
        s.ptr[i + 1] == '(') {
        for (size_t close = i + 2; close < s.len; close++) {
            if (s.ptr[close] == ')') {
                comment = (cf_span){s.ptr + i + 2, close - (i + 2)};
                has_comment = true;
                i = close + 1;
                break;
            }
        }
    } else if (s.len - i >= 10 && memcmp(s.ptr + i, ",gzip(gfe)", 10) == 0) {
        i += 10;
    }

    out->product = product;
    out->version = version;
    out->comment_raw = comment;
    out->has_comment = has_comment;
    *consumed = i;
    return true;
}

/* ruby_split(s, "; "), returning the length after trailing empty fields are
 * dropped.  Writes at most `cap` fields when `out` is set. */
static size_t ua_split_semicolon(cf_span s, cf_span *out, size_t cap) {
    if (s.len == 0) {
        /* Ruby "".split("; ") is [] (the lone empty field drops). */
        return 0;
    }
    size_t count = 0, kept = 0, start = 0, i = 0;
    while (i <= s.len) {
        bool at = i == s.len ||
                  (s.ptr[i] == ';' && i + 1 < s.len && s.ptr[i + 1] == ' ');
        if (!at) {
            i++;
            continue;
        }
        cf_span field = {s.ptr + start, i - start};
        if (out != NULL && count < cap) out[count] = field;
        count++;
        /* Empty interior fields are kept; trailing empties drop (Ruby). */
        if (field.len != 0) kept = count;
        i += 2;
        start = i;
    }
    return kept;
}

static cf_err ua_product_set_comments(cf_ua_product *product,
                                      const struct ua_build_product *built) {
    if (!built->has_comment) {
        product->comments = NULL;
        product->comment_count = 0;
        return CF_OK;
    }
    size_t count = ua_split_semicolon(built->comment_raw, NULL, 0);
    cf_span *fields = NULL;
    if (count > 0) {
        fields = malloc(count * sizeof *fields);
        if (fields == NULL) return CF_NOMEM;
        ua_split_semicolon(built->comment_raw, fields, count);
    }
    product->comments = fields;
    product->comment_count = count;
    return CF_OK;
}

/* Set after the product list exists (ua_detect_kind reads it). */
static void ua_set_kind(cf_ua_agent *agent);

cf_err cf_ua_parse(cf_span user_agent, cf_ua_agent *out) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    static const char DEFAULT_UA[] = "Mozilla/4.0 (compatible)";
    cf_span rest;
    if (ua_ruby_strip(user_agent).len == 0) {
        rest = ua_lit(DEFAULT_UA);
    } else {
        rest = user_agent;
    }

    while (rest.len > 0) {
        size_t consumed = 0;
        struct ua_build_product built;
        if (!ua_match_product(rest, &consumed, &built)) break;
        if (out->count == out->cap) {
            size_t cap = out->cap == 0 ? 8 : out->cap * 2;
            cf_ua_product *grown =
                realloc(out->products, cap * sizeof *grown);
            if (grown == NULL) return CF_NOMEM;
            out->products = grown;
            out->cap = cap;
        }
        cf_ua_product *product = &out->products[out->count];
        memset(product, 0, sizeof *product);
        product->product = built.product;
        product->version = built.version;
        product->comment_raw = built.comment_raw;
        product->has_comment = built.has_comment;
        if (ua_product_set_comments(product, &built) != CF_OK) {
            return CF_NOMEM;
        }
        out->count++;
        cf_span tail = {rest.ptr + consumed, rest.len - consumed};
        rest = ua_ruby_strip(tail);
    }
    ua_set_kind(out);
    return CF_OK;
}

void cf_ua_dispose(cf_ua_agent *agent) {
    if (agent == NULL) return;
    for (size_t i = 0; i < agent->count; i++) {
        free(agent->products[i].comments);
    }
    free(agent->products);
    memset(agent, 0, sizeof *agent);
}

/* --------------------------------------------------------------- kinds */

typedef enum {
    UA_KIND_BASE = 0,
    UA_KIND_EDGE,
    UA_KIND_IE,
    UA_KIND_OPERA,
    UA_KIND_WECHAT,
    UA_KIND_VIVALDI,
    UA_KIND_CHROME,
    UA_KIND_ITUNES,
    UA_KIND_PLAYSTATION,
    UA_KIND_PODCAST,
    UA_KIND_WEBKIT,
    UA_KIND_GECKO,
    UA_KIND_WMP,
    UA_KIND_APPLECOREMEDIA,
    UA_KIND_LIBAVFORMAT
} ua_kind;

static cf_span ua_comment_at(const cf_ua_product *p, size_t index) {
    if (!p->has_comment || index >= p->comment_count) {
        return (cf_span){NULL, 0};
    }
    return p->comments[index];
}

static cf_span ua_first_comment(const cf_ua_product *p) {
    return ua_comment_at(p, 0);
}

static bool ua_has_comments(const cf_ua_product *p) {
    return p->has_comment && p->comment_count > 0;
}

static const cf_ua_product *ua_detect_product_span(const cf_ua_agent *a,
                                                   cf_span name) {
    for (size_t i = 0; i < a->count; i++) {
        if (ua_fold_eq_span(a->products[i].product, name)) {
            return &a->products[i];
        }
    }
    return NULL;
}

static const cf_ua_product *ua_detect_product(const cf_ua_agent *a,
                                              const char *name) {
    return ua_detect_product_span(a, ua_lit(name));
}

static bool ua_any_product(const cf_ua_agent *a, const char *name) {
    for (size_t i = 0; i < a->count; i++) {
        if (ua_span_eq_lit(a->products[i].product, name)) return true;
    }
    return false;
}

/* The IE browser class reads the "; "-joined comment.  Joining the ruby_split
 * fields reproduces the raw comment except for a dropped trailing separator,
 * which can neither create nor remove an MSIE/rv: or Trident...rv: match, so
 * the raw comment is searched directly. */

/* joined_comment[/(MSIE\s|rv:)([\d\.]+)/, 2]. */
static cf_span ua_ie_version(cf_span comment) {
    for (size_t i = 0; i < comment.len; i++) {
        const unsigned char *tail = NULL;
        size_t tail_len = 0;
        if (comment.len - i >= 4 && memcmp(comment.ptr + i, "MSIE", 4) == 0 &&
            i + 4 < comment.len && ua_ruby_space(comment.ptr[i + 4])) {
            tail = comment.ptr + i + 5;
            tail_len = comment.len - (i + 5);
        } else if (comment.len - i >= 3 &&
                   memcmp(comment.ptr + i, "rv:", 3) == 0) {
            tail = comment.ptr + i + 3;
            tail_len = comment.len - (i + 3);
        }
        if (tail == NULL) continue;
        size_t digits = 0;
        while (digits < tail_len &&
               (ua_ascii_digit(tail[digits]) || tail[digits] == '.')) {
            digits++;
        }
        if (digits > 0) return (cf_span){tail, digits};
    }
    return (cf_span){NULL, 0};
}

/* joined_comment =~ /Trident.+rv:/ (same line, "rv:" not at position 0). */
static bool ua_trident_rv(cf_span comment) {
    for (size_t i = 0; i + 7 <= comment.len; i++) {
        if (memcmp(comment.ptr + i, "Trident", 7) != 0) continue;
        cf_span rest = {comment.ptr + i + 7, comment.len - (i + 7)};
        size_t line = 0;
        while (line < rest.len && rest.ptr[line] != '\n') line++;
        cf_span one_line = {rest.ptr, line};
        for (size_t j = 1; j + 3 <= one_line.len; j++) {
            if (memcmp(one_line.ptr + j, "rv:", 3) == 0) return true;
        }
    }
    return false;
}

static bool ua_comment_contains(const cf_ua_product *p, size_t index,
                                const char *needle) {
    return ua_span_contains_lit(ua_comment_at(p, index), needle);
}

/* /\A(AppleWebKit)\/([\d\.]+)/i on a comment (the version capture). */
static cf_span ua_webkit_comment_version(cf_span comment) {
    size_t chars = 0, bytes = 0;
    while (bytes < comment.len && chars < 11) {
        uint32_t cp;
        size_t used = ua_decode(comment.ptr + bytes, comment.len - bytes, &cp);
        bytes += used;
        chars++;
    }
    /* `name` is the first 11 chars, or the whole comment when shorter. */
    size_t name_len = bytes == comment.len ? comment.len : bytes;
    if (chars != 11) return (cf_span){NULL, 0};
    cf_span name = {comment.ptr, name_len};
    if (!ua_fold_eq_lit(name, "applewebkit")) return (cf_span){NULL, 0};
    if (name_len >= comment.len || comment.ptr[name_len] != '/') {
        return (cf_span){NULL, 0};
    }
    cf_span tail = {comment.ptr + name_len + 1, comment.len - name_len - 1};
    size_t digits = 0;
    while (digits < tail.len &&
           (ua_ascii_digit(tail.ptr[digits]) || tail.ptr[digits] == '.')) {
        digits++;
    }
    if (digits == 0) return (cf_span){NULL, 0};
    return (cf_span){tail.ptr, digits};
}

static cf_span ua_find_webkit_build(const cf_ua_agent *a) {
    const cf_ua_product *p = ua_detect_product(a, "applewebkit");
    if (p != NULL) return p->version;
    for (size_t i = 0; i < a->count; i++) {
        if (!a->products[i].has_comment) continue;
        for (size_t k = 0; k < a->products[i].comment_count; k++) {
            cf_span v =
                ua_webkit_comment_version(a->products[i].comments[k]);
            if (v.ptr != NULL) return v;
        }
    }
    return (cf_span){NULL, 0};
}

static const char *ua_webkit_build_version(cf_span build) {
    static const struct {
        const char *build;
        const char *version;
    } TABLE[] = {
        {"85.7", "1.0"},       {"85.8.5", "1.0.3"},  {"85.8.2", "1.0.3"},
        {"124", "1.2"},        {"125.2", "1.2.2"},   {"125.4", "1.2.3"},
        {"125.5.5", "1.2.4"},  {"125.5.6", "1.2.4"}, {"125.5.7", "1.2.4"},
        {"312.1.1", "1.3"},    {"312.1", "1.3"},     {"312.5", "1.3.1"},
        {"312.5.1", "1.3.1"},  {"312.5.2", "1.3.1"}, {"312.8", "1.3.2"},
        {"312.8.1", "1.3.2"},  {"412", "2.0"},       {"412.6", "2.0"},
        {"412.6.2", "2.0"},    {"412.7", "2.0.1"},   {"416.11", "2.0.2"},
        {"416.12", "2.0.2"},   {"417.9", "2.0.3"},   {"418", "2.0.3"},
        {"418.8", "2.0.4"},    {"418.9", "2.0.4"},   {"418.9.1", "2.0.4"},
        {"419", "2.0.4"},      {"425.13", "2.2"},    {"534.52.7", "5.1.2"},
    };
    for (size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++) {
        if (ua_span_eq_lit(build, TABLE[i].build)) return TABLE[i].version;
    }
    return NULL;
}

static bool ua_kind_extends(ua_kind kind, const cf_ua_agent *a) {
    const cf_ua_product *first = a->count > 0 ? &a->products[0] : NULL;
    const cf_ua_product *last =
        a->count > 0 ? &a->products[a->count - 1] : NULL;
    cf_span first_version =
        first != NULL ? first->version : (cf_span){NULL, 0};
    switch (kind) {
    case UA_KIND_BASE:
        return true;
    case UA_KIND_EDGE:
        return last != NULL && ua_span_eq_lit(last->product, "Edge");
    case UA_KIND_IE:
        return first != NULL && first->has_comment &&
               ((ua_comment_at(first, 1).ptr != NULL &&
                 ua_comment_contains(first, 1, "MSIE")) ||
                ua_trident_rv(first->comment_raw));
    case UA_KIND_OPERA:
        return (first != NULL && ua_span_eq_lit(first->product, "Opera")) ||
               (last != NULL && ua_span_eq_lit(last->product, "OPR"));
    case UA_KIND_WECHAT:
        for (size_t i = 0; i < a->count; i++) {
            if (cf_ua_fold_contains(a->products[i].product,
                                    "micromessenger")) {
                return true;
            }
        }
        return false;
    case UA_KIND_VIVALDI:
        return ua_any_product(a, "Vivaldi");
    case UA_KIND_CHROME:
        return ua_any_product(a, "Chrome") || ua_any_product(a, "CriOS");
    case UA_KIND_ITUNES:
        return ua_any_product(a, "iTunes");
    case UA_KIND_PLAYSTATION:
        return first != NULL &&
               (ua_comment_contains(first, 0, "PLAYSTATION 3") ||
                ua_comment_contains(first, 0, "PlayStation Vita") ||
                ua_comment_contains(first, 0, "PlayStation 4"));
    case UA_KIND_PODCAST:
        return a->count >= 3 &&
               ua_span_eq_lit(a->products[0].product, "Podcast") &&
               ua_span_eq_lit(a->products[1].product, "Addict") &&
               ua_span_eq_lit(a->products[2].product, "-");
    case UA_KIND_WEBKIT:
        for (size_t i = 0; i < a->count; i++) {
            if (ua_fold_eq_lit(a->products[i].product, "applewebkit")) {
                return true;
            }
            if (a->products[i].has_comment) {
                for (size_t k = 0; k < a->products[i].comment_count; k++) {
                    if (ua_webkit_comment_version(a->products[i].comments[k])
                            .ptr != NULL) {
                        return true;
                    }
                }
            }
        }
        return false;
    case UA_KIND_GECKO:
        return first != NULL && ua_span_eq_lit(first->product, "Mozilla");
    case UA_KIND_WMP: {
        static const char *const NAMES[] = {"NSPlayer", "Windows-Media-Player",
                                            "WMFSDK"};
        bool named = false;
        for (size_t n = 0; n < 3; n++) {
            if (ua_any_product(a, NAMES[n])) named = true;
        }
        if (!named) return false;
        return !(ua_span_eq_lit(first_version, "4.1.0.3856") ||
                 ua_span_eq_lit(first_version, "7.10.0.3059") ||
                 ua_span_eq_lit(first_version, "7.0.0.1956"));
    }
    case UA_KIND_APPLECOREMEDIA:
        return ua_any_product(a, "AppleCoreMedia");
    case UA_KIND_LIBAVFORMAT:
        return ua_any_product(a, "Lavf") ||
               (ua_any_product(a, "NSPlayer") &&
                ua_span_eq_lit(first_version, "4.1.0.3856"));
    }
    return false;
}

/* UserAgent::Browsers::ALL, in detection order. */
static ua_kind ua_detect_kind(const cf_ua_agent *a) {
    static const ua_kind ORDER[] = {
        UA_KIND_EDGE,    UA_KIND_IE,      UA_KIND_OPERA, UA_KIND_WECHAT,
        UA_KIND_VIVALDI, UA_KIND_CHROME,  UA_KIND_ITUNES,
        UA_KIND_PLAYSTATION, UA_KIND_PODCAST, UA_KIND_WEBKIT,
        UA_KIND_GECKO,   UA_KIND_WMP,     UA_KIND_APPLECOREMEDIA,
        UA_KIND_LIBAVFORMAT,
    };
    for (size_t i = 0; i < sizeof ORDER / sizeof ORDER[0]; i++) {
        if (ua_kind_extends(ORDER[i], a)) return ORDER[i];
    }
    return UA_KIND_BASE;
}

static void ua_set_kind(cf_ua_agent *agent) {
    agent->kind = (int)ua_detect_kind(agent);
}

/* ------------------------------------------------------ os regex helpers */

/* /CrOS\s([^\s]+)\s(\d+(\.\d+)*)/ capture 2. */
static cf_span ua_chrome_os_version(cf_span os) {
    for (size_t i = 0; i + 4 <= os.len; i++) {
        if (memcmp(os.ptr + i, "CrOS", 4) != 0) continue;
        size_t j = i + 4;
        if (j >= os.len || !ua_ruby_space(os.ptr[j])) continue;
        j++;
        size_t word = 0;
        while (j + word < os.len && !ua_ruby_space(os.ptr[j + word])) word++;
        if (word == 0) continue;
        j += word;
        if (j >= os.len || !ua_ruby_space(os.ptr[j])) continue;
        j++;
        size_t start = j;
        size_t end = start;
        while (end < os.len && ua_ascii_digit(os.ptr[end])) end++;
        if (end == start) continue;
        while (end + 1 < os.len && os.ptr[end] == '.' &&
               ua_ascii_digit(os.ptr[end + 1])) {
            end++;
            while (end < os.len && ua_ascii_digit(os.ptr[end])) end++;
        }
        return (cf_span){os.ptr + start, end - start};
    }
    return (cf_span){NULL, 0};
}

/* /(?:Intel|PPC) Mac OS X\s*([0-9_\.]+)?/: the leftmost occurrence wins even
 * without digits. */
static bool ua_mac_os_x_version(cf_span os, bool *has_version,
                                cf_span *version) {
    for (size_t i = 0; i < os.len; i++) {
        cf_span rest = {os.ptr + i, os.len - i};
        static const char *const PREFIXES[] = {"Intel Mac OS X",
                                               "PPC Mac OS X"};
        bool matched = false;
        for (size_t p = 0; p < 2; p++) {
            if (ua_starts_with_lit(rest, PREFIXES[p])) {
                rest.ptr += strlen(PREFIXES[p]);
                rest.len -= strlen(PREFIXES[p]);
                matched = true;
                break;
            }
        }
        if (!matched) continue;
        while (rest.len > 0 && ua_ruby_space(rest.ptr[0])) {
            rest.ptr++;
            rest.len--;
        }
        size_t digits = 0;
        while (digits < rest.len &&
               (ua_ascii_digit(rest.ptr[digits]) ||
                rest.ptr[digits] == '_' || rest.ptr[digits] == '.')) {
            digits++;
        }
        *has_version = digits > 0;
        *version = (cf_span){rest.ptr, digits};
        return true;
    }
    return false;
}

/* IOS_VERSION_REGEX = /CPU (?:iPhone |iPod )?OS ([\d_]+) like Mac OS X/. */
static cf_span ua_ios_version(cf_span os) {
    for (size_t i = 0; i + 4 <= os.len; i++) {
        if (memcmp(os.ptr + i, "CPU ", 4) != 0) continue;
        cf_span rest = {os.ptr + i + 4, os.len - i - 4};
        static const char *const DEVICES[] = {"iPhone ", "iPod "};
        for (size_t d = 0; d < 3; d++) {
            cf_span r = rest;
            if (d < 2) {
                if (!ua_starts_with_lit(r, DEVICES[d])) continue;
                r.ptr += strlen(DEVICES[d]);
                r.len -= strlen(DEVICES[d]);
            }
            if (!ua_starts_with_lit(r, "OS ")) continue;
            r.ptr += 3;
            r.len -= 3;
            size_t digits = 0;
            while (digits < r.len &&
                   (ua_ascii_digit(r.ptr[digits]) || r.ptr[digits] == '_')) {
                digits++;
            }
            if (digits == 0) continue;
            if (!ua_starts_with_lit(
                    (cf_span){r.ptr + digits, r.len - digits},
                    " like Mac OS X")) {
                continue;
            }
            return (cf_span){r.ptr, digits};
        }
    }
    return (cf_span){NULL, 0};
}

/* /Windows NT [\d\.]+|Windows Phone (OS )?[\d\.]+/: the matched text. */
static cf_span ua_windows_os(cf_span s) {
    for (size_t i = 0; i < s.len; i++) {
        cf_span rest = {s.ptr + i, s.len - i};
        cf_span tail = {NULL, 0};
        if (ua_starts_with_lit(rest, "Windows NT ")) {
            tail = (cf_span){rest.ptr + 11, rest.len - 11};
        } else if (ua_starts_with_lit(rest, "Windows Phone ")) {
            cf_span after = {rest.ptr + 14, rest.len - 14};
            if (ua_starts_with_lit(after, "OS ")) {
                cf_span candidate = {after.ptr + 3, after.len - 3};
                size_t n = 0;
                while (n < candidate.len &&
                       (ua_ascii_digit(candidate.ptr[n]) ||
                        candidate.ptr[n] == '.')) {
                    n++;
                }
                tail = n > 0 ? candidate : after;
            } else {
                tail = after;
            }
        } else {
            continue;
        }
        size_t digits = 0;
        while (digits < tail.len &&
               (ua_ascii_digit(tail.ptr[digits]) || tail.ptr[digits] == '.')) {
            digits++;
        }
        if (digits == 0) continue;
        return (cf_span){rest.ptr, rest.len - tail.len + digits};
    }
    return (cf_span){NULL, 0};
}

/* <prefix>([class]+) at its leftmost match; the class is ASCII digits plus
 * optionally '.' (both capture_after call sites). */
static cf_span ua_capture_after(cf_span s, const char *prefix, bool dots) {
    size_t plen = strlen(prefix);
    for (size_t i = 0; i + plen <= s.len; i++) {
        if (memcmp(s.ptr + i, prefix, plen) != 0) continue;
        size_t len = 0;
        while (i + plen + len < s.len &&
               (ua_ascii_digit(s.ptr[i + plen + len]) ||
                (dots && s.ptr[i + plen + len] == '.'))) {
            len++;
        }
        if (len > 0) return (cf_span){s.ptr + i + plen, len};
    }
    return (cf_span){NULL, 0};
}

/* ---------------------------------------------------------- scratch buf */

static void ua_buf_reset(cf_ua_buf *b) {
    b->len = 0;
    b->overflow = false;
}

static void ua_buf_put(cf_ua_buf *b, cf_span s) {
    if (b->overflow) return;
    if (b->ptr == NULL || b->len + s.len > b->cap) {
        b->overflow = true;
        return;
    }
    if (s.len > 0) memcpy(b->ptr + b->len, s.ptr, s.len);
    b->len += s.len;
}

static void ua_buf_put_lit(cf_ua_buf *b, const char *s) {
    ua_buf_put(b, ua_lit(s));
}

static void ua_buf_put_replaced(cf_ua_buf *b, cf_span s, unsigned char from,
                                unsigned char to) {
    for (size_t i = 0; i < s.len && !b->overflow; i++) {
        unsigned char c = s.ptr[i] == from ? to : s.ptr[i];
        ua_buf_put(b, (cf_span){&c, 1});
    }
}

/* --------------------------------------------------------- normalize_os */

static cf_span ua_normalize_os(cf_span os, cf_ua_buf *scratch,
                               bool *borrowed) {
    static const struct {
        const char *from;
        const char *to;
    } WINDOWS[] = {
        {"Windows NT 10.0", "Windows 10"},
        {"Windows NT 6.3", "Windows 8.1"},
        {"Windows NT 6.2", "Windows 8"},
        {"Windows NT 6.1", "Windows 7"},
        {"Windows NT 6.0", "Windows Vista"},
        {"Windows NT 5.2", "Windows XP x64 Edition"},
        {"Windows NT 5.1", "Windows XP"},
        {"Windows NT 5.01", "Windows 2000, Service Pack 1 (SP1)"},
        {"Windows NT 5.0", "Windows 2000"},
        {"Windows NT 4.0", "Windows NT 4.0"},
        {"Windows 98", "Windows 98"},
        {"Windows 95", "Windows 95"},
        {"Windows CE", "Windows CE"},
    };
    for (size_t i = 0; i < sizeof WINDOWS / sizeof WINDOWS[0]; i++) {
        if (ua_span_eq_lit(os, WINDOWS[i].from)) {
            *borrowed = true;
            return ua_lit(WINDOWS[i].to);
        }
    }
    bool has_version = false;
    cf_span version = {NULL, 0};
    if (ua_mac_os_x_version(os, &has_version, &version)) {
        ua_buf_reset(scratch);
        ua_buf_put_lit(scratch, "OS X");
        if (has_version) {
            ua_buf_put_lit(scratch, " ");
            ua_buf_put_replaced(scratch, version, '_', '.');
        }
        if (!scratch->overflow) {
            *borrowed = false;
            return (cf_span){(const unsigned char *)scratch->ptr,
                             scratch->len};
        }
        *borrowed = true;
        return os;
    }
    version = ua_ios_version(os);
    if (version.ptr != NULL) {
        ua_buf_reset(scratch);
        ua_buf_put_lit(scratch, "iOS ");
        ua_buf_put_replaced(scratch, version, '_', '.');
        if (!scratch->overflow) {
            *borrowed = false;
            return (cf_span){(const unsigned char *)scratch->ptr,
                             scratch->len};
        }
        *borrowed = true;
        return os;
    }
    version = ua_chrome_os_version(os);
    if (version.ptr != NULL) {
        ua_buf_reset(scratch);
        ua_buf_put_lit(scratch, "ChromeOS ");
        ua_buf_put(scratch, version);
        if (!scratch->overflow) {
            *borrowed = false;
            return (cf_span){(const unsigned char *)scratch->ptr,
                             scratch->len};
        }
        *borrowed = true;
        return os;
    }
    *borrowed = true;
    return os;
}

/* ------------------------------------------------------------- queries */

static const cf_ua_product *ua_application(const cf_ua_agent *a) {
    switch ((ua_kind)a->kind) {
    case UA_KIND_CHROME:
    case UA_KIND_VIVALDI:
    case UA_KIND_WEBKIT:
    case UA_KIND_ITUNES:
    case UA_KIND_APPLECOREMEDIA: {
        for (size_t i = 0; i < a->count; i++) {
            if (ua_has_comments(&a->products[i])) return &a->products[i];
        }
        return NULL;
    }
    default:
        return a->count > 0 ? &a->products[0] : NULL;
    }
}

static cf_span ua_application_comment_raw(const cf_ua_agent *a) {
    const cf_ua_product *p = ua_application(a);
    if (p == NULL || !p->has_comment) return (cf_span){NULL, 0};
    return p->comment_raw;
}

static cf_span ua_webkit_platform(const cf_ua_agent *a) {
    const cf_ua_product *p = ua_application(a);
    if (p == NULL || !p->has_comment) return (cf_span){NULL, 0};
    cf_span first = ua_first_comment(p);
    if (first.len != 0 && ua_span_contains_lit(first, "Windows")) {
        return ua_lit("Windows");
    }
    if (ua_span_eq_lit(first, "BB10")) return ua_lit("BlackBerry");
    for (size_t i = 0; i < p->comment_count; i++) {
        if (ua_span_contains_lit(p->comments[i], "Android")) {
            return ua_lit("Android");
        }
    }
    return first;
}

static cf_span ua_webkit_os(const cf_ua_agent *a, cf_ua_buf *scratch,
                            bool *borrowed) {
    *borrowed = true;
    const cf_ua_product *p = ua_application(a);
    if (p == NULL || !p->has_comment) return (cf_span){NULL, 0};
    cf_span at0 = ua_comment_at(p, 0);
    cf_span at1 = ua_comment_at(p, 1);
    cf_span at2 = ua_comment_at(p, 2);
    cf_span raw;
    if (at0.len != 0 && ua_span_contains_lit(at0, "Windows NT")) {
        raw = at0;
    } else if (at2.ptr == NULL ||
               (at1.len != 0 && ua_span_contains_lit(at1, "Android"))) {
        raw = at1;
    } else {
        raw = (cf_span){NULL, 0};
        for (size_t i = 0; i < p->comment_count; i++) {
            if (ua_ios_version(p->comments[i]).ptr != NULL) {
                raw = p->comments[i];
                break;
            }
        }
        if (raw.ptr == NULL) raw = at2;
    }
    if (raw.ptr == NULL) return (cf_span){NULL, 0};
    return ua_normalize_os(raw, scratch, borrowed);
}

static cf_span ua_chrome_os(const cf_ua_agent *a, cf_ua_buf *scratch,
                            bool *borrowed) {
    *borrowed = true;
    const cf_ua_product *p = ua_application(a);
    if (p == NULL || !p->has_comment) return (cf_span){NULL, 0};
    cf_span at0 = ua_comment_at(p, 0);
    cf_span at1 = ua_comment_at(p, 1);
    cf_span at2 = ua_comment_at(p, 2);
    cf_span pick;
    if (at0.len != 0 && ua_span_contains_lit(at0, "Windows NT")) {
        pick = at0;
    } else if (at2.ptr == NULL ||
               (at1.len != 0 && ua_span_contains_lit(at1, "Android"))) {
        pick = at1;
    } else {
        pick = at2;
    }
    if (pick.ptr == NULL) return (cf_span){NULL, 0};
    return ua_normalize_os(pick, scratch, borrowed);
}

static cf_span ua_gecko_browser(const cf_ua_agent *a) {
    static const char *const NAMES[] = {"PaleMoon", "Firefox", "Camino",
                                        "Iceweasel", "Seamonkey"};
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
        if (ua_detect_product(a, NAMES[i]) != NULL) return ua_lit(NAMES[i]);
    }
    if (a->count > 0) return a->products[0].product;
    return (cf_span){NULL, 0};
}

static cf_span ua_gecko_os(const cf_ua_agent *a, cf_ua_buf *scratch,
                           bool *borrowed) {
    *borrowed = true;
    const cf_ua_product *p = ua_application(a);
    if (p == NULL || !p->has_comment) return (cf_span){NULL, 0};
    cf_span first = ua_first_comment(p);
    cf_span second = ua_comment_at(p, 1);
    size_t index;
    if (ua_span_eq_lit(second, "U")) {
        index = 2;
    } else if (ua_starts_with_lit(first, "Windows ") ||
               ua_starts_with_lit(first, "Android")) {
        index = 0;
    } else if (ua_span_eq_lit(first, "Mobile")) {
        return (cf_span){NULL, 0};
    } else {
        index = 1;
    }
    cf_span raw = ua_comment_at(p, index);
    if (raw.ptr == NULL) return (cf_span){NULL, 0};
    return ua_normalize_os(raw, scratch, borrowed);
}

static cf_span ua_itunes_os(const cf_ua_agent *a, cf_ua_buf *scratch,
                            bool *borrowed) {
    *borrowed = true;
    const cf_ua_product *p = ua_application(a);
    cf_span first =
        p != NULL && p->has_comment ? ua_first_comment(p) : (cf_span){NULL, 0};
    bool windows = first.len != 0 && ua_span_contains_lit(first, "Windows");
    if (!windows) return ua_webkit_os(a, scratch, borrowed);
    /* itunes_full_os appends ")" to a cut "(Build NNNN"; the appended byte
     * cannot create a Windows-name match, so the raw field is tested. */
    cf_span full_os = p != NULL ? ua_comment_at(p, 1) : (cf_span){NULL, 0};
    const char *name = "Windows";
    if (full_os.len != 0) {
        if (ua_span_contains_lit(full_os, "Windows 8.1")) {
            name = "Windows 8.1";
        } else if (ua_span_contains_lit(full_os, "Windows 8")) {
            name = "Windows 8";
        } else if (ua_span_contains_lit(full_os, "Windows 7")) {
            name = "Windows 7";
        } else if (ua_span_contains_lit(full_os, "Windows Vista")) {
            name = "Windows Vista";
        } else if (ua_span_contains_lit(full_os, "Windows XP")) {
            name = "Windows XP";
        }
    }
    return ua_lit(name);
}

static bool ua_playstation_browser(const cf_ua_agent *a, cf_span *out) {
    const cf_ua_product *p = ua_application(a);
    cf_span first = p != NULL ? ua_comment_at(p, 0) : (cf_span){NULL, 0};
    if (first.len == 0) return false;
    if (ua_span_contains_lit(first, "PLAYSTATION 3")) {
        *out = ua_lit("PS3 Internet Browser");
        return true;
    }
    if (a->count > 0 &&
        ua_span_eq_lit(a->products[a->count - 1].product, "Silk")) {
        *out = ua_lit("Silk");
        return true;
    }
    if (ua_span_contains_lit(first, "PlayStation 4")) {
        *out = ua_lit("PS4 Internet Browser");
        return true;
    }
    return false;
}

/* playstation_os: the comments joined with " ". */
static cf_span ua_playstation_os(const cf_ua_agent *a, cf_ua_buf *scratch,
                                 bool *borrowed) {
    *borrowed = true;
    const cf_ua_product *p = ua_application(a);
    if (p == NULL || !p->has_comment) return (cf_span){NULL, 0};
    ua_buf_reset(scratch);
    for (size_t i = 0; i < p->comment_count && !scratch->overflow; i++) {
        if (i > 0) ua_buf_put(scratch, ua_lit(" "));
        ua_buf_put(scratch, p->comments[i]);
    }
    if (scratch->overflow) {
        /* Hostile input only (a comment longer than the scratch): the raw
         * comment text keeps the answer deterministic. */
        return p->comment_raw;
    }
    *borrowed = false;
    return (cf_span){(const unsigned char *)scratch->ptr, scratch->len};
}

/* ruby_split(s, separator).last() (trailing empty fields dropped). */
static cf_span ua_split_last(cf_span s, cf_span separator) {
    cf_span last = {s.ptr, 0}; /* "" when every field is empty */
    size_t start = 0, i = 0;
    while (i <= s.len) {
        bool at = separator.len > 0 && i + separator.len <= s.len &&
                  memcmp(s.ptr + i, separator.ptr, separator.len) == 0;
        if (at || i == s.len) {
            cf_span field = {s.ptr + start, i - start};
            if (field.len != 0) last = field;
            if (at) {
                i += separator.len;
                start = i;
                continue;
            }
        }
        i++;
    }
    return last;
}

static cf_ua_version_result ua_playstation_version(const cf_ua_agent *a,
                                                   cf_ua_buf *scratch) {
    cf_ua_version_result r;
    memset(&r, 0, sizeof r);
    bool borrowed = true;
    cf_span os = ua_playstation_os(a, scratch, &borrowed);
    if (os.ptr == NULL) return r;
    cf_span browser;
    if (ua_playstation_browser(a, &browser) &&
        ua_span_eq_lit(browser, "Silk")) {
        if (a->count == 0) return r;
        r.version = ua_version_new(a->products[a->count - 1].version);
        r.found = true;
        r.borrowed = true;
        return r;
    }
    cf_span marker = {NULL, 0};
    if (ua_span_contains_lit(os, "PLAYSTATION 3")) {
        marker = ua_lit("PLAYSTATION 3 ");
    } else if (ua_span_contains_lit(os, "PlayStation 4")) {
        marker = ua_lit("PlayStation 4 ");
    } else if (ua_span_contains_lit(os, "PlayStation Vita")) {
        marker = ua_lit("PlayStation Vita ");
    } else {
        return r;
    }
    r.version = ua_version_new(ua_split_last(os, marker));
    r.found = true;
    r.borrowed = borrowed;
    return r;
}

static cf_span ua_podcast_addict_os(const cf_ua_agent *a, bool *raised) {
    *raised = false;
    if (a->count <= 3) return (cf_span){NULL, 0};
    const cf_ua_product *device = &a->products[3];
    if (!ua_span_eq_lit(device->product, "Dalvik") &&
        !ua_span_eq_lit(device->product, "Mozilla")) {
        return (cf_span){NULL, 0};
    }
    if (!device->has_comment) {
        *raised = true;
        return (cf_span){NULL, 0};
    }
    if (device->comment_count > 3) return device->comments[2];
    if (device->comment_count == 3) return ua_lit("Android");
    return (cf_span){NULL, 0};
}

/* windows_media_player_os; *raised is the gem's ArgumentError/NoMethodError. */
static bool ua_windows_media_player_os(const cf_ua_agent *a, bool *raised,
                                       cf_span *out) {
    *raised = false;
    const cf_ua_product *p = ua_application(a);
    if (p == NULL) {
        *raised = true;
        return false;
    }
    ua_segment segs[6];
    size_t n = ua_scan_segments(p->version, segs, 6);
    if (n == 0 || !segs[0].is_int) {
        *raised = true;
        return false;
    }
    uint64_t values[6];
    for (size_t i = 0; i < 6; i++) values[i] = UINT64_MAX;
    for (size_t i = 0; i < n; i++) {
        if (!segs[i].is_int) continue;
        uint64_t value = 0;
        for (size_t k = 0; k < segs[i].digits.len; k++) {
            uint64_t digit = (uint64_t)(segs[i].digits.ptr[k] - '0');
            if (value > (UINT64_MAX - digit) / 10) {
                value = UINT64_MAX;
                break;
            }
            value = value * 10 + digit;
        }
        values[i] = value;
    }
    uint64_t major = values[0], part3 = values[3], part2 = values[2];
    const char *name = "Windows";
    if (major <= 4) {
        if (part3 == 3564 || part3 == 3925) {
            name = "Windows 98";
        } else if (part3 == 3857) {
            name = "Windows 9x";
        } else if (part3 == 3936) {
            name = "Windows XP";
        } else if (part3 == 3938) {
            name = "Windows 2000";
        }
    } else if (major == 7) {
        if (part3 == 3055) name = "Windows 98";
    } else if (major == 8) {
        name = "Windows XP";
    } else if (major == 9 || major == 10) {
        if (part3 == 2980) {
            name = "Windows 98/2000";
        } else if (part3 == 3268 || part3 == 3367 || part3 == 3270) {
            name = "Windows 2000";
        } else if (part3 == 3802 || part3 == 4503) {
            name = "Windows XP";
        }
    } else if (major == 11 || major == 12) {
        if (part2 == 9841 || part2 == 9858 || part2 == 9860 ||
            part2 == 9879) {
            name = "Windows 10";
        } else if (part2 == 9651) {
            name = "Windows Phone 8.1";
        } else if (part2 == 9600) {
            name = "Windows 8.1";
        } else if (part2 == 9200) {
            name = "Windows 8";
        } else if (part2 == 7600 || part2 == 7601) {
            name = "Windows 7";
        } else if (part2 >= 6000 && part2 <= 6002) {
            name = "Windows Vista";
        } else if (part2 == 5721) {
            name = "Windows XP";
        }
    }
    *out = ua_lit(name);
    return true;
}

cf_ua_result cf_ua_browser(const cf_ua_agent *agent, cf_span *out) {
    if (agent == NULL || out == NULL) return CF_UA_ABSENT;
    switch ((ua_kind)agent->kind) {
    case UA_KIND_BASE:
        if (agent->count == 0) return CF_UA_ABSENT;
        *out = agent->products[0].product;
        return CF_UA_SOME;
    case UA_KIND_EDGE:
        *out = ua_lit("Edge");
        return CF_UA_SOME;
    case UA_KIND_IE:
        *out = ua_lit("Internet Explorer");
        return CF_UA_SOME;
    case UA_KIND_OPERA:
        *out = ua_lit("Opera");
        return CF_UA_SOME;
    case UA_KIND_WECHAT:
        *out = ua_lit("Wechat Browser");
        return CF_UA_SOME;
    case UA_KIND_VIVALDI:
        *out = ua_lit("Vivaldi");
        return CF_UA_SOME;
    case UA_KIND_CHROME:
        *out = ua_detect_product(agent, "Iron") != NULL ? ua_lit("Iron")
                                                        : ua_lit("Chrome");
        return CF_UA_SOME;
    case UA_KIND_ITUNES:
        *out = ua_lit("iTunes");
        return CF_UA_SOME;
    case UA_KIND_PLAYSTATION:
        return ua_playstation_browser(agent, out) ? CF_UA_SOME : CF_UA_ABSENT;
    case UA_KIND_PODCAST:
        *out = ua_lit("Podcast Addict");
        return CF_UA_SOME;
    case UA_KIND_WEBKIT: {
        char buffer[256];
        cf_ua_buf scratch = {buffer, 0, sizeof buffer, false};
        bool borrowed = true;
        cf_span os = ua_webkit_os(agent, &scratch, &borrowed);
        if (ua_span_contains_lit(os, "Android")) {
            *out = ua_lit("Android");
        } else if (ua_span_eq_lit(ua_webkit_platform(agent), "BlackBerry")) {
            *out = ua_lit("BlackBerry");
        } else {
            *out = ua_lit("Safari");
        }
        return CF_UA_SOME;
    }
    case UA_KIND_GECKO: {
        cf_span name = ua_gecko_browser(agent);
        if (name.ptr == NULL) return CF_UA_ABSENT;
        *out = name;
        return CF_UA_SOME;
    }
    case UA_KIND_WMP:
        *out = ua_lit("Windows Media Player");
        return CF_UA_SOME;
    case UA_KIND_APPLECOREMEDIA:
        *out = ua_lit("AppleCoreMedia");
        return CF_UA_SOME;
    case UA_KIND_LIBAVFORMAT:
        *out = ua_lit("libavformat");
        return CF_UA_SOME;
    }
    return CF_UA_ABSENT;
}

static cf_ua_version_result ua_webkit_version(const cf_ua_agent *a,
                                              cf_ua_buf *scratch) {
    cf_ua_version_result r;
    memset(&r, 0, sizeof r);
    const cf_ua_product *version_product = ua_detect_product(a, "Version");
    if (version_product != NULL) {
        r.version = ua_version_new(version_product->version);
        r.found = true;
        r.borrowed = true;
        return r;
    }
    bool borrowed = true;
    cf_span os = ua_webkit_os(a, scratch, &borrowed);
    cf_span ios = ua_capture_after(os, "iOS ", true);
    cf_span browser;
    if (ios.ptr != NULL && cf_ua_browser(a, &browser) == CF_UA_SOME &&
        ua_span_eq_lit(browser, "Safari")) {
        /* Version::new(ios.replace('_', ".")): the normalizer already
         * replaced underscores.  The capture points into the caller's
         * scratch when the os was synthesized. */
        r.version = ua_version_new(ios);
        r.found = true;
        r.borrowed = borrowed;
        return r;
    }
    cf_span build = ua_find_webkit_build(a);
    const char *mapped =
        build.ptr != NULL ? ua_webkit_build_version(build) : NULL;
    r.version = ua_version_new(mapped != NULL ? ua_lit(mapped)
                                              : (cf_span){NULL, 0});
    r.found = true;
    r.borrowed = true;
    return r;
}

cf_ua_version_result cf_ua_version_of(const cf_ua_agent *agent,
                                      cf_ua_buf *scratch) {
    cf_ua_version_result r;
    memset(&r, 0, sizeof r);
    if (agent == NULL) return r;
    switch ((ua_kind)agent->kind) {
    case UA_KIND_BASE:
    case UA_KIND_WMP:
    case UA_KIND_APPLECOREMEDIA: {
        const cf_ua_product *p = ua_application(agent);
        if (p == NULL) return r;
        r.version = ua_version_new(p->version);
        r.found = true;
        r.borrowed = true;
        return r;
    }
    case UA_KIND_EDGE:
    case UA_KIND_VIVALDI: {
        if (agent->count == 0) return r;
        r.version =
            ua_version_new(agent->products[agent->count - 1].version);
        r.found = true;
        r.borrowed = true;
        return r;
    }
    case UA_KIND_IE: {
        cf_span joined = ua_application_comment_raw(agent);
        r.version = ua_version_new(ua_ie_version(joined));
        r.found = true;
        r.borrowed = true;
        return r;
    }
    case UA_KIND_OPERA: {
        /* Opera#version: Opera Mini, then Version, then OPR, then base.
         * opera_mini() reads the first product's *joined* comment (the raw
         * text; joining only normalizes "; " separators). */
        cf_span joined =
            agent->count > 0 ? agent->products[0].comment_raw
                             : (cf_span){NULL, 0};
        bool mini = ua_span_contains_lit(joined, "Opera Mini");
        if (mini) {
            /* application_comment(): the first product's comments. */
            const cf_ua_product *p = ua_application(agent);
            cf_span capture = {NULL, 0};
            if (p != NULL && p->has_comment) {
                for (size_t k = 0; k < p->comment_count; k++) {
                    if (ua_span_contains_lit(p->comments[k], "Opera Mini")) {
                        capture = ua_capture_after(
                            p->comments[k], "Opera Mini/", true);
                        break;
                    }
                }
            }
            r.version = ua_version_new(capture);
            r.found = true;
            r.borrowed = true;
            return r;
        }
        const cf_ua_product *p = ua_detect_product(agent, "Version");
        if (p == NULL) p = ua_detect_product(agent, "OPR");
        if (p == NULL) p = ua_application(agent);
        if (p == NULL) return r;
        r.version = ua_version_new(p->version);
        r.found = true;
        r.borrowed = true;
        return r;
    }
    case UA_KIND_WECHAT: {
        const cf_ua_product *p = ua_detect_product(agent, "MicroMessenger");
        if (p == NULL) {
            r.raised = true;
            return r;
        }
        r.version = ua_version_new(p->version);
        r.found = true;
        r.borrowed = true;
        return r;
    }
    case UA_KIND_CHROME: {
        const cf_ua_product *p = ua_detect_product(agent, "CriOs");
        if (p == NULL) p = ua_detect_product(agent, "chrome");
        if (p == NULL) {
            r.raised = true;
            return r;
        }
        r.version = ua_version_new(p->version);
        r.found = true;
        r.borrowed = true;
        return r;
    }
    case UA_KIND_ITUNES: {
        const cf_ua_product *p = ua_detect_product(agent, "iTunes");
        if (p == NULL) {
            r.raised = true;
            return r;
        }
        r.version = ua_version_new(p->version);
        r.found = true;
        r.borrowed = true;
        return r;
    }
    case UA_KIND_PLAYSTATION:
        return ua_playstation_version(agent, scratch);
    case UA_KIND_PODCAST:
        return r;
    case UA_KIND_WEBKIT:
        return ua_webkit_version(agent, scratch);
    case UA_KIND_GECKO: {
        const cf_ua_product *found =
            ua_detect_product_span(agent, ua_gecko_browser(agent));
        if (found == NULL) {
            r.raised = true;
            return r;
        }
        r.version = ua_version_new(found->version);
        if (r.version.blank) {
            const cf_ua_product *base = ua_application(agent);
            if (base == NULL) return r;
            r.version = ua_version_new(base->version);
        }
        r.found = true;
        r.borrowed = true;
        return r;
    }
    case UA_KIND_LIBAVFORMAT: {
        if (ua_detect_product(agent, "NSPlayer") != NULL) return r;
        const cf_ua_product *p = ua_application(agent);
        if (p == NULL) return r;
        r.version = ua_version_new(p->version);
        r.found = true;
        r.borrowed = true;
        return r;
    }
    }
    return r;
}

cf_ua_result cf_ua_platform(const cf_ua_agent *agent, cf_span *out) {
    if (agent == NULL || out == NULL) return CF_UA_ABSENT;
    switch ((ua_kind)agent->kind) {
    case UA_KIND_BASE:
    case UA_KIND_LIBAVFORMAT:
        return CF_UA_ABSENT;
    case UA_KIND_EDGE:
    case UA_KIND_IE:
    case UA_KIND_WMP:
        *out = ua_lit("Windows");
        return CF_UA_SOME;
    case UA_KIND_OPERA:
    case UA_KIND_APPLECOREMEDIA: {
        const cf_ua_product *p = ua_application(agent);
        if (p == NULL || !p->has_comment) return CF_UA_ABSENT;
        cf_span first = ua_comment_at(p, 0);
        if (first.ptr == NULL) return CF_UA_ABSENT;
        if (first.len != 0 && ua_span_contains_lit(first, "Windows")) {
            *out = ua_lit("Windows");
        } else {
            *out = first;
        }
        return CF_UA_SOME;
    }
    case UA_KIND_WECHAT: {
        const cf_ua_product *p = ua_application(agent);
        if (p == NULL || !p->has_comment) return CF_UA_ABSENT;
        cf_span first = ua_comment_at(p, 0);
        if (first.ptr == NULL) return CF_UA_ABSENT;
        if (first.len != 0 && ua_span_contains_lit(first, "iPhone")) {
            *out = ua_lit("iPhone");
        } else {
            bool android = false;
            for (size_t i = 0; i < p->comment_count; i++) {
                if (ua_span_contains_lit(p->comments[i], "Android")) {
                    android = true;
                    break;
                }
            }
            *out = android ? ua_lit("Android") : first;
        }
        return CF_UA_SOME;
    }
    case UA_KIND_CHROME:
    case UA_KIND_VIVALDI: {
        const cf_ua_product *p = ua_application(agent);
        if (p == NULL || !p->has_comment) return CF_UA_ABSENT;
        cf_span first = ua_comment_at(p, 0);
        if (first.len != 0 && ua_span_contains_lit(first, "Windows")) {
            *out = ua_lit("Windows");
            return CF_UA_SOME;
        }
        for (size_t i = 0; i < p->comment_count; i++) {
            if (ua_span_contains_lit(p->comments[i], "CrOS")) {
                *out = ua_lit("ChromeOS");
                return CF_UA_SOME;
            }
        }
        for (size_t i = 0; i < p->comment_count; i++) {
            if (ua_span_contains_lit(p->comments[i], "Android")) {
                *out = ua_lit("Android");
                return CF_UA_SOME;
            }
        }
        if (first.ptr == NULL) return CF_UA_ABSENT;
        *out = first;
        return CF_UA_SOME;
    }
    case UA_KIND_WEBKIT:
    case UA_KIND_ITUNES: {
        cf_span platform = ua_webkit_platform(agent);
        if (platform.ptr == NULL) return CF_UA_ABSENT;
        *out = platform;
        return CF_UA_SOME;
    }
    case UA_KIND_PLAYSTATION: {
        char buffer[512];
        cf_ua_buf scratch = {buffer, 0, sizeof buffer, false};
        bool borrowed = true;
        cf_span os = ua_playstation_os(agent, &scratch, &borrowed);
        if (os.ptr == NULL) return CF_UA_ABSENT;
        if (ua_span_contains_lit(os, "PLAYSTATION 3")) {
            *out = ua_lit("PlayStation 3");
        } else if (ua_span_contains_lit(os, "PlayStation 4")) {
            *out = ua_lit("PlayStation 4");
        } else if (ua_span_contains_lit(os, "PlayStation Vita")) {
            *out = ua_lit("PlayStation Vita");
        } else {
            return CF_UA_ABSENT;
        }
        return CF_UA_SOME;
    }
    case UA_KIND_PODCAST: {
        bool raised = false;
        cf_span os = ua_podcast_addict_os(agent, &raised);
        if (raised) return CF_UA_RAISED;
        if (os.ptr == NULL) return CF_UA_RAISED; /* Ok(None).ok_or(Raised) */
        if (ua_span_contains_lit(os, "Android")) {
            *out = ua_lit("Android");
            return CF_UA_SOME;
        }
        return CF_UA_ABSENT;
    }
    case UA_KIND_GECKO: {
        const cf_ua_product *p = ua_application(agent);
        if (p == NULL || !p->has_comment) return CF_UA_ABSENT;
        cf_span first = ua_comment_at(p, 0);
        if (ua_span_eq_lit(first, "compatible") ||
            ua_span_eq_lit(first, "Mobile")) {
            return CF_UA_ABSENT;
        }
        if (first.ptr == NULL) return CF_UA_ABSENT;
        if (ua_starts_with_lit(first, "Windows ")) {
            *out = ua_lit("Windows");
        } else {
            *out = first;
        }
        return CF_UA_SOME;
    }
    }
    return CF_UA_ABSENT;
}

cf_ua_result cf_ua_os(const cf_ua_agent *agent, cf_ua_buf *scratch,
                      cf_span *out, bool *borrowed_out) {
    if (agent == NULL || out == NULL) return CF_UA_ABSENT;
    bool borrowed = true;
    if (borrowed_out != NULL) *borrowed_out = true;
    cf_ua_result result = CF_UA_SOME;
    switch ((ua_kind)agent->kind) {
    case UA_KIND_BASE:
    case UA_KIND_LIBAVFORMAT:
        result = CF_UA_ABSENT;
        break;
    case UA_KIND_EDGE: {
        bool found = false;
        for (size_t i = 0; i < agent->count && !found; i++) {
            const cf_ua_product *p = &agent->products[i];
            if (!p->has_comment) continue;
            for (size_t k = 0; k < p->comment_count; k++) {
                cf_span os = ua_windows_os(p->comments[k]);
                if (os.ptr != NULL) {
                    *out = ua_normalize_os(os, scratch, &borrowed);
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            *out = ua_normalize_os((cf_span){NULL, 0}, scratch, &borrowed);
        }
        break;
    }
    case UA_KIND_IE: {
        cf_span joined = ua_application_comment_raw(agent);
        *out = ua_normalize_os(ua_windows_os(joined), scratch, &borrowed);
        break;
    }
    case UA_KIND_OPERA: {
        const cf_ua_product *p = ua_application(agent);
        if (p == NULL || !p->has_comment) {
            result = CF_UA_ABSENT;
            break;
        }
        cf_span first = ua_comment_at(p, 0);
        if (first.len != 0 && ua_span_contains_lit(first, "Windows")) {
            *out = ua_normalize_os(first, scratch, &borrowed);
            break;
        }
        cf_span second = ua_comment_at(p, 1);
        if (second.ptr == NULL) {
            result = CF_UA_ABSENT;
            break;
        }
        *out = second;
        break;
    }
    case UA_KIND_WECHAT:
    case UA_KIND_CHROME:
    case UA_KIND_VIVALDI:
    case UA_KIND_APPLECOREMEDIA: {
        cf_span os = ua_chrome_os(agent, scratch, &borrowed);
        if (os.ptr == NULL) {
            result = CF_UA_ABSENT;
            break;
        }
        *out = os;
        break;
    }
    case UA_KIND_WEBKIT: {
        cf_span os = ua_webkit_os(agent, scratch, &borrowed);
        if (os.ptr == NULL) {
            result = CF_UA_ABSENT;
            break;
        }
        *out = os;
        break;
    }
    case UA_KIND_ITUNES: {
        cf_span os = ua_itunes_os(agent, scratch, &borrowed);
        if (os.ptr == NULL) {
            result = CF_UA_ABSENT;
            break;
        }
        *out = os;
        break;
    }
    case UA_KIND_PLAYSTATION: {
        cf_span os = ua_playstation_os(agent, scratch, &borrowed);
        if (os.ptr == NULL) {
            result = CF_UA_ABSENT;
            break;
        }
        *out = os;
        break;
    }
    case UA_KIND_PODCAST: {
        bool raised = false;
        cf_span os = ua_podcast_addict_os(agent, &raised);
        if (raised) {
            result = CF_UA_RAISED;
        } else if (os.ptr == NULL) {
            result = CF_UA_ABSENT;
        } else {
            *out = os;
        }
        break;
    }
    case UA_KIND_GECKO: {
        cf_span os = ua_gecko_os(agent, scratch, &borrowed);
        if (os.ptr == NULL) {
            result = CF_UA_ABSENT;
            break;
        }
        *out = os;
        break;
    }
    case UA_KIND_WMP: {
        bool raised = false;
        cf_span os = {NULL, 0};
        if (!ua_windows_media_player_os(agent, &raised, &os)) {
            result = raised ? CF_UA_RAISED : CF_UA_ABSENT;
            break;
        }
        *out = os;
        break;
    }
    default:
        result = CF_UA_ABSENT;
        break;
    }
    if (result == CF_UA_SOME && borrowed_out != NULL) {
        *borrowed_out = borrowed;
    }
    return result;
}

bool cf_ua_is_bot(const cf_ua_agent *agent) {
    if (agent == NULL) return true;
    const cf_ua_product *application = ua_application(agent);
    if (application == NULL) return true;
    for (size_t i = 0; i < agent->count; i++) {
        const cf_ua_product *p = &agent->products[i];
        if (!p->has_comment) continue;
        for (size_t k = 0; k < p->comment_count; k++) {
            if (cf_ua_fold_contains(p->comments[k], "bot")) return true;
        }
    }
    if (ua_detect_product(agent, "Chrome-Lighthouse") != NULL) return true;
    return ua_span_contains_lit(application->product, "bot");
}
