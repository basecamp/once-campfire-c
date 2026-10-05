/* R01 / VIEW-01 tests: context-specific escaping primitives.
 *
 * Reference oracles (tmp/rust-ref):
 *  - crates/ruby/src/erb.rs tests: html_escape `<&>"'x é` and CGI/url vectors.
 *  - crates/ruby/src/uri.rs test: cgi_escape/url_encode of `a*~ b-._+é`.
 *  - crates/rails_compat/src/json.rs tests: JSON gem + ActiveSupport
 *    escape_html_entities byte vectors (lowercase \u hex, 0x7f raw,
 *    U+2028 not escaped).
 *  - Source-as-spec: every '&' is escaped, including entity-looking input
 *    (the golden editor markup in
 *    tests/fixtures/crates/views/tests/golden/b/messages_edit_text.json
 *    contains `&amp;amp;`), so `&amp;` as literal text becomes `&amp;amp;`.
 *    The no-double-escape rule is architectural: sanitized HTML goes through
 *    cf_html_trusted and is appended verbatim.
 *
 * Build (plain and sanitize), from the repository root:
 *   clang -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra
 *         -Werror -pthread -Isrc -Itests tests/views/test_escape.c
 *         src/views/escape.c src/core/buffer.c src/core/alloc.c
 *         -o build/r01/plain/test_escape
 *   ... plus -fsanitize=address,undefined -fno-sanitize-recover=all
 *         -fno-omit-frame-pointer -g for the sanitize binary.
 */
#include "cf.h"

#include "core/alloc.h"

#include "cf_test.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- helpers --------------------------------------------------------------- */

static cf_span span_of(const char *s) {
    return (cf_span){(const unsigned char *)s, strlen(s)};
}

static void expect_bytes(const cf_builder *b, const void *want, size_t want_len,
                         const char *label) {
    const unsigned char *w = want;
    if (b->len == want_len && (want_len == 0 || memcmp(b->ptr, w, want_len) == 0)) {
        return;
    }
    fprintf(stderr, "    %s: got %zu byte(s):", label, b->len);
    for (size_t i = 0; i < b->len; i++) fprintf(stderr, " %02x", b->ptr[i]);
    fprintf(stderr, "\n    %s: want %zu byte(s):", label, want_len);
    for (size_t i = 0; i < want_len; i++) fprintf(stderr, " %02x", w[i]);
    fprintf(stderr, "\n");
    CF_CHECK(0 && "output bytes differ");
}

#define EXPECT_RENDER(fn, in, want)                                         \
    do {                                                                    \
        cf_builder b_ = {0};                                                \
        cf_err rc_ = (fn)(&b_, (in));                                       \
        CF_CHECK(rc_ == CF_OK);                                             \
        if (rc_ == CF_OK) expect_bytes(&b_, (want), sizeof(want) - 1, #fn); \
        cf_builder_dispose(&b_);                                            \
    } while (0)

/* Calls fn with a nonempty builder and expects CF_INVALID with the builder
 * content preserved. */
static void expect_rejected_with_content(cf_err (*fn)(cf_builder *, cf_span),
                                         cf_span in) {
    cf_builder b = {0};
    cf_err rc = cf_builder_append(&b, span_of("keep"));
    CF_REQUIRE(rc == CF_OK);
    CF_CHECK(fn(&b, in) == CF_INVALID);
    expect_bytes(&b, "keep", 4, "after CF_INVALID");
    cf_builder_dispose(&b);
}

/* --- failing allocator for builder-failure paths ---------------------------- */

static int64_t alloc_calls;
static int64_t fail_at = -1;

static void *test_alloc(size_t size) {
    if (alloc_calls++ == fail_at) return NULL;
    return malloc(size);
}

static void *test_realloc(void *ptr, size_t size) {
    if (alloc_calls++ == fail_at) return NULL;
    return realloc(ptr, size);
}

static void test_free(void *ptr) { free(ptr); }

static void fail_allocations_at(int64_t ordinal) {
    alloc_calls = 0;
    fail_at = ordinal;
    cf_core_set_allocator(test_alloc, test_realloc, test_free);
}

static void fail_allocations_off(void) {
    fail_at = -1;
    cf_core_reset_allocator();
}

/* --- HTML text / attribute --------------------------------------------------- */

CF_TEST(html_text_escapes_text_context_only) {
    EXPECT_RENDER(cf_html_text, span_of("&"), "&amp;");
    EXPECT_RENDER(cf_html_text, span_of("<"), "&lt;");
    EXPECT_RENDER(cf_html_text, span_of(">"), "&gt;");
    /* Quotes are inert in text content. */
    EXPECT_RENDER(cf_html_text, span_of("\""), "\"");
    EXPECT_RENDER(cf_html_text, span_of("'"), "'");
}

CF_TEST(html_attr_escapes_all_five) {
    EXPECT_RENDER(cf_html_attr, span_of("&"), "&amp;");
    EXPECT_RENDER(cf_html_attr, span_of("<"), "&lt;");
    EXPECT_RENDER(cf_html_attr, span_of(">"), "&gt;");
    EXPECT_RENDER(cf_html_attr, span_of("\""), "&quot;");
    EXPECT_RENDER(cf_html_attr, span_of("'"), "&#39;");
}

CF_TEST(html_matches_erb_reference_vector) {
    /* crates/ruby/src/erb.rs test escapes_like_erb_util. */
    const char *in = "<&>\"'x \xC3\xA9";
    EXPECT_RENDER(cf_html_attr, span_of(in), "&lt;&amp;&gt;&quot;&#39;x \xC3\xA9");
    EXPECT_RENDER(cf_html_text, span_of(in), "&lt;&amp;&gt;\"'x \xC3\xA9");
}

CF_TEST(html_text_is_single_pass) {
    /* One '&' escapes exactly once; the inserted entity is never scanned
     * again. Already-sanitized HTML is not re-escaped: it goes through
     * cf_html_trusted (see trusted tests). */
    EXPECT_RENDER(cf_html_text, span_of("a & b"), "a &amp; b");
    EXPECT_RENDER(cf_html_attr, span_of("x=\"a&b\""), "x=&quot;a&amp;b&quot;");
}

CF_TEST(html_text_entity_looking_input_matches_reference) {
    /* ERB::Util.html_escape escapes every '&' in one pass, so text that
     * spells an entity becomes doubled (`&amp;amp;`). This matches the pinned
     * golden editor markup, which contains `&amp;amp;`; treating such input
     * as pre-escaped would be escape_once semantics the reference does not
     * have. */
    EXPECT_RENDER(cf_html_text, span_of("&amp;"), "&amp;amp;");
    EXPECT_RENDER(cf_html_attr, span_of("&lt;"), "&amp;lt;");
}

CF_TEST(html_unicode_passthrough) {
    /* é = C3 A9, U+1F600 = F0 9F 98 80. Escaping only touches ASCII bytes. */
    EXPECT_RENDER(cf_html_text, span_of("<é😀&>"), "&lt;é😀&amp;&gt;");
    EXPECT_RENDER(cf_html_attr, span_of("\"é😀'"), "&quot;é😀&#39;");
    /* Raw U+00A0 stays raw: ERB leaves it alone; the richtext DOM serializer
     * (R02) is the place that writes &nbsp;. */
    EXPECT_RENDER(cf_html_text, span_of("a\xC2\xA0" "b"), "a\xC2\xA0" "b");
}

CF_TEST(html_controls_and_newlines_pass_through) {
    /* ERB escapes the five characters only; newlines, tabs and C0 controls
     * are copied byte for byte. */
    const char *in = "a\nb\tc\rd\x01\x7F";
    EXPECT_RENDER(cf_html_text, span_of(in), "a\nb\tc\rd\x01\x7F");
    EXPECT_RENDER(cf_html_attr, span_of(in), "a\nb\tc\rd\x01\x7F");
}

CF_TEST(html_nul_is_span_data) {
    /* cf_span carries an explicit length and the reference escaper is
     * byte-wise, so an embedded NUL is data, not a string terminator: it is
     * copied through (HTML) rather than rejected or truncated. NUL rejection
     * belongs to the request-parse boundary, not to output escaping. */
    static const unsigned char in[] = {'a', 0x00, 'b', '<'};
    cf_builder b = {0};
    CF_REQUIRE(cf_html_text(&b, (cf_span){in, sizeof in}) == CF_OK);
    expect_bytes(&b, (const unsigned char *)"a\0b&lt;", 7, "html_text NUL");
    cf_builder_dispose(&b);

    CF_REQUIRE(cf_html_attr(&b, (cf_span){in, sizeof in}) == CF_OK);
    expect_bytes(&b, (const unsigned char *)"a\0b&lt;", 7, "html_attr NUL");
    cf_builder_dispose(&b);
}

/* --- URL component ------------------------------------------------------------ */

CF_TEST(url_component_matches_reference_vector) {
    /* crates/ruby/src/uri.rs test escapes_like_ruby (url_encode). */
    EXPECT_RENDER(cf_url_component, span_of("a*~ b-._+\xC3\xA9"),
                  "a%2A~%20b-._%2B%C3%A9");
}

CF_TEST(url_component_encode_set) {
    EXPECT_RENDER(cf_url_component, span_of("AZaz09-._~"), "AZaz09-._~");
    EXPECT_RENDER(cf_url_component, span_of(":/?#[]@!$&'()*+,;="),
                  "%3A%2F%3F%23%5B%5D%40%21%24%26%27%28%29%2A%2B%2C%3B%3D");
}

CF_TEST(url_component_space_and_plus) {
    /* Component spelling: space is %20 and a literal '+' is %2B. The '+' for
     * space form belongs to CGI.escape query values, not this encoder. */
    EXPECT_RENDER(cf_url_component, span_of("a b"), "a%20b");
    EXPECT_RENDER(cf_url_component, span_of("+"), "%2B");
    EXPECT_RENDER(cf_url_component, span_of("a+b c"), "a%2Bb%20c");
}

CF_TEST(url_component_unicode) {
    /* Multibyte and astral code points are percent-encoded byte-wise. */
    EXPECT_RENDER(cf_url_component, span_of("é😀"), "%C3%A9%F0%9F%98%80");
}

CF_TEST(url_component_nul) {
    static const unsigned char in[] = {'a', 0x00, 'b'};
    cf_builder b = {0};
    CF_REQUIRE(cf_url_component(&b, (cf_span){in, sizeof in}) == CF_OK);
    expect_bytes(&b, "a%00b", 5, "url NUL");
    cf_builder_dispose(&b);
}

/* --- JSON string -------------------------------------------------------------- */

CF_TEST(json_string_matches_reference_mixed) {
    /* crates/rails_compat/src/json.rs
     * escapes_html_entities_but_not_separators_or_slashes, string part:
     * <a href="x">&' U+2028 é \n \t 0x01 0x7f / </a> */
    const char *in = "<a href=\"x\">&'\xE2\x80\xA8\xC3\xA9\n\t\x01\x7F/</a>";
    EXPECT_RENDER(cf_json_string, span_of(in),
                  "\"\\u003ca href=\\\"x\\\"\\u003e\\u0026'"
                  "\xE2\x80\xA8\xC3\xA9\\n\\t\\u0001\x7F/\\u003c/a\\u003e\"");
}

static size_t json_control_expected(unsigned char c, char *out) {
    static const char hex[] = "0123456789abcdef";
    switch (c) {
    case '\b': out[0] = '\\'; out[1] = 'b'; return 2;
    case '\t': out[0] = '\\'; out[1] = 't'; return 2;
    case '\n': out[0] = '\\'; out[1] = 'n'; return 2;
    case '\f': out[0] = '\\'; out[1] = 'f'; return 2;
    case '\r': out[0] = '\\'; out[1] = 'r'; return 2;
    default:
        out[0] = '\\'; out[1] = 'u'; out[2] = '0'; out[3] = '0';
        out[4] = hex[c >> 4];
        out[5] = hex[c & 0xf];
        return 6;
    }
}

CF_TEST(json_string_escapes_c0_controls) {
    /* Every C0 control, exactly as the json gem writes it: named short
     * escapes where the gem has them, otherwise lowercase \u00xx. */
    for (unsigned c = 0; c < 0x20; c++) {
        unsigned char in = (unsigned char)c;
        char body[6];
        size_t body_len = json_control_expected(in, body);
        char want[9] = {'"'};
        memcpy(want + 1, body, body_len);
        want[1 + body_len] = '"';

        cf_builder b = {0};
        CF_REQUIRE(cf_json_string(&b, (cf_span){&in, 1}) == CF_OK);
        expect_bytes(&b, want, body_len + 2, "json C0 control");
        cf_builder_dispose(&b);
    }
    /* 0x7f is not a C0 control and stays raw. */
    const unsigned char del = 0x7F;
    EXPECT_RENDER(cf_json_string, ((cf_span){&del, 1}), "\"\x7F\"");
}

CF_TEST(json_string_html_sensitive_and_quotes) {
    /* ActiveSupport::JSON escape_html_entities plus string escaping; '/'
     * untouched. */
    EXPECT_RENDER(cf_json_string, span_of("<>&\"\\/"),
                  "\"\\u003c\\u003e\\u0026\\\"\\\\/\"");
    EXPECT_RENDER(cf_json_string, span_of(""), "\"\"");
}

CF_TEST(json_string_unicode_passthrough) {
    /* Valid UTF-8 is copied raw, including astral, U+2028/U+2029 and the
     * U+0080/U+FFFF/U+10FFFF boundary sequences. */
    EXPECT_RENDER(cf_json_string, span_of("é😀\xE2\x80\xA8\xE2\x80\xA9"),
                  "\"é😀\xE2\x80\xA8\xE2\x80\xA9\"");
    EXPECT_RENDER(cf_json_string, span_of("\xC2\x80\xEF\xBF\xBF\xF4\x8F\xBF\xBF"),
                  "\"\xC2\x80\xEF\xBF\xBF\xF4\x8F\xBF\xBF\"");
}

CF_TEST(json_string_embedded_nul) {
    /* The json gem escapes an embedded NUL as \u0000. */
    static const unsigned char in[] = {'a', 0x00, 'b'};
    cf_builder b = {0};
    CF_REQUIRE(cf_json_string(&b, (cf_span){in, sizeof in}) == CF_OK);
    expect_bytes(&b, "\"a\\u0000b\"", 10, "json NUL");
    cf_builder_dispose(&b);
}

CF_TEST(json_string_rejects_invalid_utf8) {
    /* The Rust reference only ever receives &str (valid UTF-8), so the C
     * boundary rejects what it cannot represent instead of emitting invalid
     * JSON. The builder content is preserved. */
    static const struct {
        const char *s;
        size_t len;
    } bad[] = {
        {"\x80", 1},             /* lone continuation           */
        {"\xC3\x28", 2},         /* bad continuation            */
        {"\xC0\xAF", 2},         /* overlong 2-byte             */
        {"\xE0\x80\xAF", 3},     /* overlong 3-byte             */
        {"\xED\xA0\x80", 3},     /* UTF-16 surrogate U+D800     */
        {"\xE2\x82", 2},         /* truncated 3-byte            */
        {"\xF0\x9F\x98", 3},     /* truncated 4-byte            */
        {"\xF4\x90\x80\x80", 4}, /* above U+10FFFF              */
        {"\xF5\x80\x80\x80", 4}, /* invalid lead                */
        {"ok\xFF\xFE", 4},       /* invalid bytes after ASCII   */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        cf_span in = {(const unsigned char *)bad[i].s, bad[i].len};
        expect_rejected_with_content(cf_json_string, in);
    }
}

CF_TEST(escapes_reject_null_arguments) {
    CF_CHECK(cf_html_text(NULL, span_of("x")) == CF_INVALID);
    CF_CHECK(cf_html_attr(NULL, span_of("x")) == CF_INVALID);
    CF_CHECK(cf_json_string(NULL, span_of("x")) == CF_INVALID);
    CF_CHECK(cf_url_component(NULL, span_of("x")) == CF_INVALID);

    for (int which = 0; which < 4; which++) {
        cf_builder b = {0};
        cf_builder_append(&b, span_of("keep"));
        cf_err rc;
        switch (which) {
        case 0: rc = cf_html_text(&b, (cf_span){NULL, 3}); break;
        case 1: rc = cf_html_attr(&b, (cf_span){NULL, 3}); break;
        case 2: rc = cf_json_string(&b, (cf_span){NULL, 3}); break;
        default: rc = cf_url_component(&b, (cf_span){NULL, 3}); break;
        }
        CF_CHECK(rc == CF_INVALID);
        expect_bytes(&b, "keep", 4, "after NULL span");
        cf_builder_dispose(&b);
    }

    /* Empty NULL span is legal and appends nothing (JSON emits ""). */
    EXPECT_RENDER(cf_html_text, ((cf_span){NULL, 0}), "");
    EXPECT_RENDER(cf_url_component, ((cf_span){NULL, 0}), "");
    EXPECT_RENDER(cf_json_string, ((cf_span){NULL, 0}), "\"\"");
}

/* --- trusted HTML -------------------------------------------------------------- */

CF_TEST(html_trusted_appends_verbatim) {
    cf_buf *bytes = NULL;
    CF_REQUIRE(cf_buf_copy(span_of("&amp;<b>é</b>"), &bytes) == CF_OK);
    cf_safe_html safe = {bytes};

    cf_builder b = {0};
    CF_REQUIRE(cf_builder_append(&b, span_of("pre|")) == CF_OK);
    CF_CHECK(cf_html_trusted(&b, &safe) == CF_OK);
    /* Sanitized bytes are inserted as-is; escaping them would double escape
     * already-sanitized HTML. */
    expect_bytes(&b, "pre|&amp;<b>é</b>", 18, "trusted append");
    cf_builder_dispose(&b);
    cf_safe_html_dispose(&safe);
}

CF_TEST(html_trusted_argument_errors) {
    cf_safe_html empty = {NULL};
    cf_builder b = {0};
    CF_CHECK(cf_html_trusted(NULL, &empty) == CF_INVALID);
    CF_CHECK(cf_html_trusted(&b, NULL) == CF_INVALID);
    CF_CHECK(b.len == 0);
    /* Empty trusted HTML appends nothing but is not an error. */
    CF_CHECK(cf_html_trusted(&b, &empty) == CF_OK);
    CF_CHECK(b.len == 0);
    cf_builder_dispose(&b);
}

CF_TEST(safe_html_dispose_resets) {
    cf_safe_html_dispose(NULL); /* accepted */
    cf_buf *bytes = NULL;
    CF_REQUIRE(cf_buf_copy(span_of("x"), &bytes) == CF_OK);
    cf_safe_html safe = {bytes};
    cf_safe_html_dispose(&safe);
    CF_CHECK(safe.bytes == NULL);
    cf_safe_html_dispose(&safe); /* second dispose is a no-op */
    CF_CHECK(safe.bytes == NULL);
}

/* --- builder-failure atomicity --------------------------------------------------- */

/* Renders via fn into a builder holding `prefix`, with the nth allocation
 * (0-based) failing; expects CF_NOMEM and unchanged content. */
static void expect_alloc_failure(cf_err (*fn)(cf_builder *, cf_span),
                                 const char *prefix, cf_span in,
                                 int64_t fail_ordinal) {
    cf_builder b = {0};
    if (prefix[0] != '\0') CF_REQUIRE(cf_builder_append(&b, span_of(prefix)) == CF_OK);
    size_t before = b.len;
    unsigned char snapshot[16];
    CF_REQUIRE(before <= sizeof snapshot);
    if (before != 0) memcpy(snapshot, b.ptr, before);

    fail_allocations_at(fail_ordinal);
    cf_err rc = fn(&b, in);
    fail_allocations_off();
    CF_CHECK(rc == CF_NOMEM);
    CF_CHECK(b.len == before);
    if (before != 0) {
        CF_CHECK(memcmp(b.ptr, snapshot, before) == 0);
    }
    cf_builder_dispose(&b);
}

CF_TEST(builder_failure_html_is_atomic) {
    /* 200 ampersands expand to 1000 bytes: forces several growths, so both
     * the first-allocation and a mid-render allocation can be failed. */
    static char many_amp[201];
    memset(many_amp, '&', 200);
    many_amp[200] = '\0';
    cf_span in = span_of(many_amp);

    expect_alloc_failure(cf_html_text, "", in, 0);
    expect_alloc_failure(cf_html_text, "", in, 1);
    expect_alloc_failure(cf_html_attr, "keep", in, 0);
    expect_alloc_failure(cf_html_attr, "keep", in, 1);

    /* The builder stays usable after the failed call. */
    cf_builder b = {0};
    fail_allocations_at(0);
    CF_CHECK(cf_html_text(&b, in) == CF_NOMEM);
    fail_allocations_off();
    CF_CHECK(cf_html_text(&b, span_of("&")) == CF_OK);
    expect_bytes(&b, "&amp;", 5, "retry after NOMEM");
    cf_builder_dispose(&b);
}

CF_TEST(builder_failure_url_is_atomic) {
    static char many_bad[201];
    memset(many_bad, ' ', 200);
    many_bad[200] = '\0';
    cf_span in = span_of(many_bad);

    expect_alloc_failure(cf_url_component, "", in, 0);
    expect_alloc_failure(cf_url_component, "", in, 1);
    expect_alloc_failure(cf_url_component, "keep", in, 1);
}

CF_TEST(builder_failure_json_is_atomic) {
    static char many_lt[201];
    memset(many_lt, '<', 200);
    many_lt[200] = '\0';
    cf_span in = span_of(many_lt);

    expect_alloc_failure(cf_json_string, "", in, 0);
    expect_alloc_failure(cf_json_string, "", in, 1);
    expect_alloc_failure(cf_json_string, "keep", in, 0);
    expect_alloc_failure(cf_json_string, "keep", in, 1);
}

CF_TEST(builder_failure_trusted_is_atomic) {
    static char big[501];
    memset(big, 'x', 500);
    big[500] = '\0';
    cf_buf *bytes = NULL;
    CF_REQUIRE(cf_buf_copy(span_of(big), &bytes) == CF_OK);
    cf_safe_html safe = {bytes};

    cf_builder b = {0};
    CF_REQUIRE(cf_builder_append(&b, span_of("keep")) == CF_OK);

    fail_allocations_at(0);
    cf_err rc = cf_html_trusted(&b, &safe);
    fail_allocations_off();
    CF_CHECK(rc == CF_NOMEM);
    expect_bytes(&b, "keep", 4, "after trusted NOMEM");

    CF_CHECK(cf_html_trusted(&b, &safe) == CF_OK);
    CF_CHECK(b.len == 504);
    cf_builder_dispose(&b);
    cf_safe_html_dispose(&safe);
}

CF_TEST_MAIN()
