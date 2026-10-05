/* tests/auth/test_platform.c — A01 completion: the User-Agent platform layer
 * against the pinned differential corpus.
 *
 * The corpus (tests/fixtures/ua/campfire_user_agents.json, byte-identical to
 * tmp/rust-ref/vectors/campfire_user_agents.json) was produced by running the
 * reference Ruby through the useragent gem / platform_agent; the pinned Rust
 * port is checked against it in user_agent.rs/platform.rs.  This test asserts
 * that cf_platform_parse answers every `application_platform` field of every
 * case, plus `blocked`, `bot` and `version`, with the same "to_view" mapping
 * the Rust uses: a field the gem raised on answers false (booleans) or ""
 * (texts).
 *
 * It also feeds the parser every corpus prefix and a deterministic hostile
 * byte soup: the parser must answer without crashing on any input.
 */
#include "cf_test.h"

#include "auth/platform.h"
#include "auth/user_agent.h"
#include "cf.h"

#include <yyjson.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CORPUS_PATH "tests/fixtures/ua/campfire_user_agents.json"

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

static cf_span json_span(yyjson_val *value) {
    if (!yyjson_is_str(value)) return SP("");
    const char *text = yyjson_get_str(value);
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* to_view: a bool field answers false when it is null or an error marker. */
static bool json_flag(yyjson_val *object, const char *key) {
    yyjson_val *value = yyjson_obj_get(object, key);
    return yyjson_is_bool(value) ? yyjson_get_bool(value) : false;
}

static const char *json_text(yyjson_val *object, const char *key) {
    yyjson_val *value = yyjson_obj_get(object, key);
    return yyjson_is_str(value) ? yyjson_get_str(value) : "";
}

static bool span_eq_text(cf_span span, const char *text) {
    size_t n = strlen(text);
    return span.len == n &&
           (n == 0 || memcmp(span.ptr, text, n) == 0);
}

/* One corpus case: every application_platform field, blocked, bot, version. */
static void check_case(yyjson_val *case_obj, size_t *failures,
                       size_t *cases) {
    const char *ua = yyjson_is_str(yyjson_obj_get(case_obj, "ua"))
                         ? yyjson_get_str(yyjson_obj_get(case_obj, "ua"))
                         : "";
    cf_span user_agent = SP(ua);
    cf_platform p;
    CF_REQUIRE(cf_platform_parse(user_agent, &p) == CF_OK);
    (*cases)++;

    yyjson_val *ap = yyjson_obj_get(case_obj, "application_platform");
    CF_REQUIRE(ap != NULL);
    struct {
        const char *name;
        bool actual;
    } flags[] = {
        {"ios", p.ios},
        {"android", p.android},
        {"mac", p.mac},
        {"chrome", p.chrome},
        {"firefox", p.firefox},
        {"safari", p.safari},
        {"edge", p.edge},
        {"apple_messages", p.apple_messages},
        {"mobile", p.mobile},
        {"desktop", p.desktop},
        {"windows", p.windows},
    };
    for (size_t i = 0; i < sizeof flags / sizeof flags[0]; i++) {
        if (flags[i].actual != json_flag(ap, flags[i].name)) {
            printf("    %s: %s expected %d got %d\n", flags[i].name, ua,
                   json_flag(ap, flags[i].name), flags[i].actual);
            (*failures)++;
        }
    }
    if (!span_eq_text(p.browser, json_text(ap, "browser"))) {
        printf("    browser: %s expected \"%s\" got \"%.*s\"\n", ua,
               json_text(ap, "browser"), (int)p.browser.len,
               (const char *)p.browser.ptr);
        (*failures)++;
    }
    if (!span_eq_text(p.operating_system, json_text(ap, "operating_system"))) {
        printf("    operating_system: %s expected \"%s\" got \"%.*s\"\n", ua,
               json_text(ap, "operating_system"),
               (int)p.operating_system.len,
               (const char *)p.operating_system.ptr);
        (*failures)++;
    }
    /* Version: the gem's version.to_s, "" where the gem raised or was nil. */
    if (!span_eq_text(p.browser_version, json_text(case_obj, "version"))) {
        printf("    version: %s expected \"%s\" got \"%.*s\"\n", ua,
               json_text(case_obj, "version"), (int)p.browser_version.len,
               (const char *)p.browser_version.ptr);
        (*failures)++;
    }
    if (p.bot != json_flag(case_obj, "bot")) {
        printf("    bot: %s expected %d got %d\n", ua,
               json_flag(case_obj, "bot"), p.bot);
        (*failures)++;
    }
    bool blocked = yyjson_is_true(yyjson_obj_get(case_obj, "blocked"));
    if (cf_platform_blocked(&p) != blocked) {
        printf("    blocked: %s expected %d got %d\n", ua, blocked,
               cf_platform_blocked(&p));
        (*failures)++;
    }
}

CF_TEST(corpus_application_platform_matches_the_reference) {
    yyjson_doc *doc = yyjson_read_file(CORPUS_PATH, 0, NULL, NULL);
    CF_REQUIRE(doc != NULL);
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *cases = yyjson_obj_get(root, "user_agents");
    CF_REQUIRE(cases != NULL);
    CF_REQUIRE(yyjson_is_arr(cases));

    size_t failures = 0, checked = 0;
    size_t count = yyjson_arr_size(cases);
    for (size_t i = 0; i < count; i++) {
        check_case(yyjson_arr_get(cases, i), &failures, &checked);
    }
    printf("    corpus: %zu application_platform cases, %zu field mismatches\n",
           checked, failures);
    CF_CHECK(checked == 385);
    CF_CHECK(failures == 0);
    yyjson_doc_free(doc);
}

/* Every corpus UA (and each of its prefixes) parses without crashing; the
 * same for a deterministic hostile byte soup. */
CF_TEST(parser_survives_prefixes_and_hostile_input) {
    yyjson_doc *doc = yyjson_read_file(CORPUS_PATH, 0, NULL, NULL);
    CF_REQUIRE(doc != NULL);
    yyjson_val *cases = yyjson_obj_get(yyjson_doc_get_root(doc), "user_agents");
    CF_REQUIRE(cases != NULL);
    size_t count = yyjson_arr_size(cases);
    size_t parses = 0;
    for (size_t i = 0; i < count; i++) {
        cf_span ua = json_span(yyjson_obj_get(yyjson_arr_get(cases, i), "ua"));
        for (size_t n = 0; n <= ua.len; n++) {
            cf_platform p;
            cf_err rc =
                cf_platform_parse((cf_span){ua.ptr, n}, &p);
            CF_REQUIRE(rc == CF_OK);
            (void)cf_platform_blocked(&p);
            parses++;
        }
    }
    yyjson_doc_free(doc);

    /* Deterministic xorshift soup plus the shapes the scanner special-cases. */
    static const char *const SHAPES[] = {
        "", " ", "\t\r\n", "\0", "\"", "'", "\"\"\"", "'''''", "/", "////",
        "Mozilla/5.0 (", "Mozilla/5.0 (unterminated", ",",
        ",gzip(gfe)", "a,b", "a/", "a//", "(", ")", "()", "(; )", "; ",
        "Podcast Addict - Dalvik", "Podcast Addict - Dalvik/2.1.0 (a)",
        "Opera/9.80 (Opera Mini/", "AppleWebKit/", "CrOS ", "CrOS a ",
        "Intel Mac OS X", "CPU iPhone OS  like Mac OS X", "Windows NT ",
        "Windows Phone OS ", "\xff\xfe\xfd", "\xc3", "\xed\xa0\x80",
        "\xf0\x9f\xa6\x8a/1.0", "K", "\xc4\xb0",
    };
    for (size_t i = 0; i < sizeof SHAPES / sizeof SHAPES[0]; i++) {
        cf_platform p;
        CF_REQUIRE(cf_platform_parse(SP(SHAPES[i]), &p) == CF_OK);
        (void)cf_platform_blocked(&p);
        parses++;
    }
    {
        /* The degenerate NULL span (an absent header). */
        cf_platform p;
        CF_REQUIRE(cf_platform_parse((cf_span){NULL, 0}, &p) == CF_OK);
        (void)cf_platform_blocked(&p);
        parses++;
    }
    uint32_t state = 0x12345678u;
    char soup[4096];
    for (size_t round = 0; round < 256; round++) {
        size_t len = 1 + (round * 37) % sizeof soup;
        for (size_t i = 0; i < len; i++) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            soup[i] = (char)(state >> 11);
        }
        cf_platform p;
        CF_REQUIRE(cf_platform_parse((cf_span){(const unsigned char *)soup, len},
                                     &p) == CF_OK);
        (void)cf_platform_blocked(&p);
        parses++;
    }
    printf("    hostile inputs: %zu parses without a crash\n", parses);
    CF_CHECK(parses > 385);
}

CF_TEST(parse_rejects_null_and_blank_answers_nothing) {
    CF_CHECK(cf_platform_parse(SP(""), NULL) == CF_INVALID);
    CF_CHECK(!cf_platform_blocked(NULL));
}

/* ------------------------------------------------- A01 r2: the kit's gate */

/* The predicate the pinned `HeaderValue::to_str` applies (http 1.5.0
 * value.rs, is_visible_ascii): HTAB or 0x20..=0x7E.  Every byte >= 0x80
 * fails, valid UTF-8 included. */
static bool to_str_readable(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = p[i];
        if (c != '\t' && (c < 0x20 || c > 0x7E)) return false;
    }
    return true;
}

/* The gate differential: every single byte, the verifier's H01-reachable
 * mismatch vectors (obs-text), and every corpus UA.  The verifier measured 25
 * mismatches against the old UTF-8-validity gate (its ua_probe on cases.bin);
 * this table pins the same class. */
CF_TEST(header_readable_matches_header_value_to_str) {
    size_t failures = 0, readable_singles = 0;
    for (unsigned b = 0; b < 256; b++) {
        unsigned char c = (unsigned char)b;
        bool expected = to_str_readable(&c, 1);
        bool actual = cf_ua_header_readable((cf_span){&c, 1});
        if (actual != expected) {
            printf("    byte %02x: expected %d got %d\n", b, expected, actual);
            failures++;
        }
        if (expected) readable_singles++;
    }
    CF_CHECK(readable_singles == 96); /* HTAB + 0x20..0x7E */

    static const char *const rejected[] = {
        /* obs-text: valid UTF-8 is not enough (the verifier's unicode set) */
        "\xc3\xa9", "\xc2\xa0", "\xe2\x84\xaa", "\xc4\xb0", "\xe1\xba\x9e",
        "\xf0\x9f\xa6\x8a/1.0",
        "\xe6\xb5\x8f\xe8\xa7\x88\xe5\x99\xa8/1.0 Chrome/140.0",
        "Mozilla/5.0 (Macintosh) Chrome/119.0 \xc3\xa9",
        "\xc2\xa0\xc2\xa0", "x\xc2\xa0", "\xc2\xa0x",
        "\xc3\x9c" "n\xc3\xaf" "c\xc3\xb8" "d\xc3\xa9",
        /* invalid UTF-8, obs-text to H01 */
        "\xff\xfe", "\xed\xa0\x80", "\xc3",
        /* controls H01 already rejects; the predicate is exact anyway */
        "\x7f", "\x01", "\r", "\n",
    };
    for (size_t i = 0; i < sizeof rejected / sizeof rejected[0]; i++) {
        cf_span s = SP(rejected[i]);
        if (cf_ua_header_readable(s)) {
            printf("    rejected[%zu]: expected unreadable\n", i);
            failures++;
        }
    }
    static const char *const accepted[] = {
        "", "curl/8.4.0", "\t", " \t ", "~",
        ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
         "(KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36"),
    };
    for (size_t i = 0; i < sizeof accepted / sizeof accepted[0]; i++) {
        if (!cf_ua_header_readable(SP(accepted[i]))) {
            printf("    accepted[%zu]: expected readable\n", i);
            failures++;
        }
    }

    yyjson_doc *doc = yyjson_read_file(CORPUS_PATH, 0, NULL, NULL);
    CF_REQUIRE(doc != NULL);
    yyjson_val *cases = yyjson_obj_get(yyjson_doc_get_root(doc), "user_agents");
    CF_REQUIRE(cases != NULL);
    size_t count = yyjson_arr_size(cases), unreadable = 0, obs_text = 0;
    for (size_t i = 0; i < count; i++) {
        /* yyjson_get_len, not strlen: one corpus UA is a lone NUL byte, which
         * the span still carries (H01 rejects it before any gate). */
        yyjson_val *ua_val = yyjson_obj_get(yyjson_arr_get(cases, i), "ua");
        bool is_str = yyjson_is_str(ua_val);
        cf_span ua = {(const unsigned char *)(is_str ? yyjson_get_str(ua_val)
                                                     : ""),
                      is_str ? yyjson_get_len(ua_val) : 0};
        bool expected = to_str_readable(ua.ptr, ua.len);
        if (!expected) {
            unreadable++;
            bool has_obs_text = false;
            for (size_t k = 0; k < ua.len; k++) {
                if (ua.ptr[k] >= 0x80) has_obs_text = true;
            }
            obs_text += has_obs_text ? 1 : 0;
        }
        if (cf_ua_header_readable(ua) != expected) {
            printf("    corpus[%zu]: expected readable=%d\n", i, expected);
            failures++;
        }
    }
    yyjson_doc_free(doc);
    printf("    gate: %zu cases, %zu unreadable (%zu obs-text, %zu controls), "
           "%zu mismatches\n",
           count, unreadable, obs_text, unreadable - obs_text, failures);
    CF_CHECK(count == 385);
    /* Indices 252-254, 341-343, 361 and 373-381, 383-384: obs-text for the
     * verifier's 25-mismatch class, controls (H01 already rejects VT/CR/LF/
     * NUL) for the rest; 382 is pure ASCII. */
    CF_CHECK(unreadable == 18);
    CF_CHECK(obs_text == 14);
    CF_CHECK(failures == 0);
}

/* --------------------------------- A01 r2: Version#<=> over the corpus */

/* The corpus comparisons differential (all 352): cmp sign, lt, eq (Rust's
 * PartialEq is string equality), plus boundary vectors around the missing-
 * segment fill.  Expected values confirmed against the pinned Rust (the
 * verifier's ua-rust-harness fed the same pairs). */
CF_TEST(version_comparisons_match_the_pin) {
    yyjson_doc *doc = yyjson_read_file(CORPUS_PATH, 0, NULL, NULL);
    CF_REQUIRE(doc != NULL);
    yyjson_val *comparisons =
        yyjson_obj_get(yyjson_doc_get_root(doc), "comparisons");
    CF_REQUIRE(comparisons != NULL);
    size_t count = yyjson_arr_size(comparisons), failures = 0;
    for (size_t i = 0; i < count; i++) {
        yyjson_val *c = yyjson_arr_get(comparisons, i);
        const char *a = yyjson_get_str(yyjson_obj_get(c, "a"));
        const char *b = yyjson_get_str(yyjson_obj_get(c, "b"));
        long expected = (long)yyjson_get_int(yyjson_obj_get(c, "cmp"));
        int actual = cf_ua_version_cmp_lit(a, b);
        bool lt = yyjson_is_true(yyjson_obj_get(c, "lt"));
        bool eq = yyjson_is_true(yyjson_obj_get(c, "eq"));
        if ((actual < 0 ? -1 : actual > 0 ? 1 : 0) != expected ||
            (actual < 0) != lt || (strcmp(a, b) == 0) != eq) {
            printf("    comparisons[%zu] \"%s\" vs \"%s\": expected cmp=%ld "
                   "lt=%d eq=%d got cmp=%d lt=%d eq=%d\n",
                   i, a, b, expected, lt, eq, actual, actual < 0,
                   strcmp(a, b) == 0);
            failures++;
        }
    }
    yyjson_doc_free(doc);
    printf("    comparisons: %zu checked, %zu mismatches\n", count, failures);
    CF_CHECK(count == 352);
    CF_CHECK(failures == 0);
}

CF_TEST(version_ordering_boundaries) {
    static const struct {
        const char *a, *b;
        int cmp;
    } cases[] = {
        {"1", "1.0", 0},          /* missing segment is Int("0") */
        {"1.0", "1", 0},
        {"1.0.0", "1", 0},
        {"1", "1.0.0", 0},
        {"1.0", "1.0.1", -1},
        {"1.0.1", "1.0", 1},
        {"1.", "1.0", 0},         /* trailing dot: nothing after it */
        {"1.", "1.0b", 1},
        {"0", "", 0},             /* the blank side has no segments, so the
                                     real 0 fills the missing one: <=> says
                                     equal even though the strings differ */
        {"", "0", -1},
        {"", "", 0},
        {"0.0", "0", 0},
        {"000", "0", 0},
        {"017.0", "17", 0},
        {"1.0.0.0.0.0.1", "1", 0}, /* only the first six segments count */
        {"1.2.3.4.5.6.7", "1.2.3.4.5.6", 0},
        {"17.2.0", "17.2", 0},
        {"120.0.0.0", "120", 0},
        {"1", "1.0b", 1},
        {"1.0b", "1", -1},
        {"", "1.0", -1},
        {"1.0", "", 1},
        {"17.10", "17.2", 1},     /* digit-length ordering, not numeric */
        {"1.2", "1.10", -1},
    };
    size_t failures = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int actual = cf_ua_version_cmp_lit(cases[i].a, cases[i].b);
        actual = actual < 0 ? -1 : actual > 0 ? 1 : 0;
        if (actual != cases[i].cmp) {
            printf("    boundary[%zu] \"%s\" vs \"%s\": expected %d got %d\n",
                   i, cases[i].a, cases[i].b, cases[i].cmp, actual);
            failures++;
        }
        /* No reverse-symmetry check: the gem's ordering is not total, so
         * cmp(a,b) == 0 does not imply cmp(b,a) == 0 ("0" vs "" is the
         * corpus' own example). */
    }
    printf("    boundaries: %zu checked, %zu mismatches\n",
           sizeof cases / sizeof cases[0], failures);
    CF_CHECK(failures == 0);
}

CF_TEST_MAIN()
