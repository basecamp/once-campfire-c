/* K01a Accept-Encoding selection tests (06-cache-performance.md
 * "Representation and HTTP behavior"; CACHE-05's selection half).
 *
 * Oracle: the pinned reference's app-level Rack::Deflater port,
 * tmp/rust-ref crates/kit/src/deflater.rs (pin
 * 64f86353021145b63849fb1cd93adeb08f3b8dbb), functions
 * parse_accept_encoding (lines 138-158) and select_best_encoding
 * (lines 160-190, tests 280-309). Every row below is either one of the
 * pinned reference's own test expectations (marked "pin test") or derived
 * from those functions under the rules documented in src/encoding.h.
 *
 * The one deliberate divergence from the wired reference is disclosed in
 * src/encoding.h: the front layer (front/compression.rs) would also answer
 * zstd and case-fold coding names; this module mirrors the app layer that
 * owns identity/gzip and the 406. Rows that would differ on the wire because
 * of the front layer are marked "app-layer oracle". */
#include "cf.h"
#include "encoding.h"

#include "cf_test.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static cf_span S(const char *s) {
    return (cf_span){(const unsigned char *)s, strlen(s)};
}

static void expect_enc(const char *header, cf_encoding want) {
    cf_encoding got = cf_encoding_select(S(header));
    if (got != want) {
        fprintf(stderr, "  Accept-Encoding: \"%s\" -> %d, want %d\n",
                header, (int)got, (int)want);
    }
    CF_CHECK(got == want);
}

CF_TEST(encoding_absent_and_empty_select_identity) {
    /* deflater.rs:45: absent -> None -> unwrap_or("") -> no tokens. */
    CF_CHECK(cf_encoding_select((cf_span){NULL, 0}) == CF_ENC_IDENTITY);
    CF_CHECK(cf_encoding_select(S("")) == CF_ENC_IDENTITY);
    CF_CHECK(cf_encoding_select(S("   ")) == CF_ENC_IDENTITY);
    CF_CHECK(cf_encoding_select(S(",")) == CF_ENC_IDENTITY);
    CF_CHECK(cf_encoding_select(S(", ,\t,")) == CF_ENC_IDENTITY);
}

CF_TEST(encoding_pin_test_vectors) {
    /* deflater.rs:280-290, in order. */
    expect_enc("gzip, deflate, br", CF_ENC_GZIP);         /* pin test */
    expect_enc("", CF_ENC_IDENTITY);                      /* pin test */
    expect_enc("br", CF_ENC_IDENTITY);                    /* pin test */
    expect_enc("gzip;q=0", CF_ENC_IDENTITY);              /* pin test */
    expect_enc("identity;q=0.5, gzip;q=0.1", CF_ENC_IDENTITY); /* pin test */
    expect_enc("*", CF_ENC_GZIP);                         /* pin test */
    expect_enc("identity;q=0, *;q=0", CF_ENC_UNACCEPTABLE); /* pin test */
    expect_enc("gzip;q=0, identity;q=0", CF_ENC_UNACCEPTABLE); /* pin test */
    /* deflater.rs:292-309, the q-value table. */
    expect_enc("identity;q=", CF_ENC_IDENTITY);           /* pin test */
    expect_enc("identity;q=0", CF_ENC_UNACCEPTABLE);      /* pin test */
    expect_enc("gzip;q=", CF_ENC_GZIP);                   /* pin test */
    expect_enc("gzip;q=abc", CF_ENC_GZIP);                /* pin test */
    expect_enc("gzip;Q=0.5, identity;q=0.9", CF_ENC_GZIP); /* pin test */
    expect_enc("gzip;q=.5", CF_ENC_GZIP);                 /* pin test */
    expect_enc("gzip;q=.", CF_ENC_IDENTITY);              /* pin test */
    expect_enc("gzip;q=0..5, identity;q=0.1", CF_ENC_IDENTITY); /* pin test */
    expect_enc("gzip;q=0.5.1, identity;q=0.6", CF_ENC_IDENTITY); /* pin test */
}

CF_TEST(encoding_quality_ordering) {
    expect_enc("gzip", CF_ENC_GZIP);
    expect_enc("identity", CF_ENC_IDENTITY);
    /* A listed positive gzip beats the implicit identity, whatever its q
     * (deflater.rs:181 appends identity after sorting). */
    expect_enc("gzip;q=0.001", CF_ENC_GZIP);
    expect_enc("gzip;q=0.5, identity", CF_ENC_IDENTITY);
    expect_enc("gzip;q=0.5, identity;q=0.4", CF_ENC_GZIP);
    /* Equal q: available preference (gzip) wins, not client order. */
    expect_enc("gzip;q=1.0, identity;q=1.0", CF_ENC_GZIP);
    expect_enc("br, gzip", CF_ENC_GZIP);
    expect_enc("identity, gzip", CF_ENC_GZIP);
    expect_enc("gzip, identity;q=0.5", CF_ENC_GZIP);
    /* No clamping: q outside [0,1] is used as parsed. */
    expect_enc("gzip;q=2, identity;q=3", CF_ENC_IDENTITY);
    expect_enc("gzip;q=2, identity;q=1", CF_ENC_GZIP);
    /* Zero spellings are exactly zero. */
    expect_enc("gzip;q=0.0", CF_ENC_IDENTITY);
    expect_enc("gzip;q=00", CF_ENC_IDENTITY);
    expect_enc("gzip;q=0.000", CF_ENC_IDENTITY);
    expect_enc("identity;q=0.0, gzip", CF_ENC_GZIP);
    expect_enc("identity;q=0.9, gzip;q=0.9", CF_ENC_GZIP);
    /* Ruby to_f stops at a second dot and ignores a trailing dot. */
    expect_enc("gzip;q=1.", CF_ENC_GZIP);
    expect_enc("gzip;q=..", CF_ENC_IDENTITY);
    expect_enc("gzip;q=0.9., identity;q=0.8", CF_ENC_GZIP);
    expect_enc("gzip;q=, identity;q=0.5", CF_ENC_GZIP);
}

CF_TEST(encoding_wildcard) {
    expect_enc("*", CF_ENC_GZIP);
    expect_enc("*;q=0", CF_ENC_UNACCEPTABLE);
    expect_enc("*;q=0.5", CF_ENC_GZIP);
    /* The wildcard never covers an explicitly listed coding, even q=0. */
    expect_enc("gzip;q=0, *;q=1", CF_ENC_IDENTITY);
    expect_enc("*;q=0, gzip", CF_ENC_GZIP);
    expect_enc("*, identity;q=0", CF_ENC_GZIP);
    expect_enc("gzip, *", CF_ENC_GZIP);
    expect_enc("gzip, *;q=0", CF_ENC_GZIP);
    expect_enc("*;q=0.5, identity;q=0.4", CF_ENC_GZIP);
    expect_enc("identity;q=0.4, *;q=0.5", CF_ENC_GZIP);
    /* The wildcard excludes the explicitly listed gzip even though a
     * covering wildcard would outrank it. */
    expect_enc("gzip;q=0.4, *;q=0.9", CF_ENC_IDENTITY);
    /* Only the first '*' expands; a later q=0 wildcard has no effect. */
    expect_enc("*, *;q=0", CF_ENC_GZIP);
    expect_enc("*, identity;q=0.5", CF_ENC_GZIP);
}

CF_TEST(encoding_duplicates_and_reject_any_zero) {
    /* reject! removes a name when ANY expanded entry has q == 0.0. */
    expect_enc("gzip;q=0, gzip;q=1", CF_ENC_IDENTITY);
    expect_enc("gzip;q=1, gzip;q=0", CF_ENC_IDENTITY);
    expect_enc("identity;q=1, identity;q=0", CF_ENC_UNACCEPTABLE);
    expect_enc("identity;q=0.5, identity;q=0.6, gzip;q=0.1", CF_ENC_IDENTITY);
    expect_enc("gzip;q=1, gzip;q=0.5, identity;q=0.7", CF_ENC_GZIP);
}

CF_TEST(encoding_uppercase_is_not_the_available_token) {
    /* deflater.rs:155 keeps the coding's case; the available names are
     * lowercase, so an uppercase client token falls back to identity.
     * (App-layer oracle: the reference's front layer would compress.) */
    expect_enc("GZIP", CF_ENC_IDENTITY);
    expect_enc("GZip", CF_ENC_IDENTITY);
    expect_enc("GZIP;q=1, gzip;q=0.5", CF_ENC_GZIP);
    expect_enc("GZIP;q=0, gzip;q=0.5", CF_ENC_GZIP);
    expect_enc("IDENTITY;q=0", CF_ENC_IDENTITY);
    /* "Identity" is an unknown token, not identity; gzip is forbidden, so
     * only the implicit identity remains. */
    expect_enc("Identity;q=0, gzip;q=0", CF_ENC_IDENTITY);
}

CF_TEST(encoding_malformed_parameters) {
    /* Parameters after the first ';': q= must be the start of the remainder
     * (deflater.rs:148-153), and only the [0-9.] run is read. */
    expect_enc("gzip;q=0.5;level=9, identity;q=0.6", CF_ENC_IDENTITY);
    expect_enc("gzip;level=9;q=0.5, identity;q=0.9", CF_ENC_GZIP);
    expect_enc("gzip; q=0.5, identity", CF_ENC_IDENTITY);
    expect_enc("gzip; q = 0.5, identity;q=0.9", CF_ENC_GZIP);
    expect_enc("gzip;q=0.5 , identity;q=0.6", CF_ENC_IDENTITY);
    /* An empty coding name is just an unknown token. */
    expect_enc(";q=0, gzip;q=0.5, identity;q=0.4", CF_ENC_GZIP);
    expect_enc(";q=0, identity;q=0.4", CF_ENC_IDENTITY);
}

CF_TEST(encoding_whitespace_trim) {
    expect_enc(" gzip ", CF_ENC_GZIP);
    expect_enc("\tgzip\t,\tidentity", CF_ENC_GZIP);
    expect_enc("gzip ;q=0", CF_ENC_IDENTITY);
    expect_enc("gzip\t;q=0.5 ,\tidentity;q=0.4", CF_ENC_GZIP);
}

CF_TEST(encoding_take_16_tokens) {
    /* deflater.rs:162 takes the first 16 parsed entries; later tokens are
     * invisible, including a later q=0 or wildcard. */
    expect_enc("br,br,br,br,br,br,br,br,br,br,br,br,br,br,br,br,identity;q=0",
               CF_ENC_IDENTITY);
    expect_enc("br,br,br,br,br,br,br,br,br,br,br,br,br,br,br,br,gzip",
               CF_ENC_IDENTITY);
    expect_enc("br,br,br,br,br,br,br,br,br,br,br,br,br,br,br,br,*",
               CF_ENC_IDENTITY);
    /* The 16th token still counts: identity;q=0 there forbids identity. */
    expect_enc("br,br,br,br,br,br,br,br,br,br,br,br,br,br,br,identity;q=0",
               CF_ENC_UNACCEPTABLE);
    /* ... and the 16th token can be selected. */
    expect_enc("br,br,br,br,br,br,br,br,br,br,br,br,br,br,br,gzip;q=0.5",
               CF_ENC_GZIP);
}

CF_TEST(encoding_readability_gate) {
    /* to_str rejects every byte outside HTAB/0x20..0x7E; an unreadable value
     * is treated as absent (identity), and the whole value is refused, not
     * just the offending token (deflater.rs:45 and_then(to_str).ok()). */
    static const unsigned char obs[] = {0xFF, ',', 'g', 'z', 'i', 'p'};
    static const unsigned char obs_tail[] = {'g', 'z', 'i', 'p', ',', 0x80};
    static const unsigned char utf8[] = {'g', 'z', 'i', 'p', ',', ' ', 0xC3, 0xA9};
    static const unsigned char del[] = {'g', 'z', 'i', 'p', 0x7F};
    static const unsigned char nul[] = {'g', 'z', 'i', 'p', 0x00};
    CF_CHECK(cf_encoding_select((cf_span){obs, sizeof obs}) == CF_ENC_IDENTITY);
    CF_CHECK(cf_encoding_select((cf_span){obs_tail, sizeof obs_tail}) ==
             CF_ENC_IDENTITY);
    CF_CHECK(cf_encoding_select((cf_span){utf8, sizeof utf8}) == CF_ENC_IDENTITY);
    CF_CHECK(cf_encoding_select((cf_span){del, sizeof del}) == CF_ENC_IDENTITY);
    CF_CHECK(cf_encoding_select((cf_span){nul, sizeof nul}) == CF_ENC_IDENTITY);
    /* HTAB is readable. */
    CF_CHECK(cf_encoding_select(S("gzip\t,\tidentity")) == CF_ENC_GZIP);
}

CF_TEST(encoding_names) {
    CF_CHECK(strcmp(cf_encoding_name(CF_ENC_GZIP), "gzip") == 0);
    CF_CHECK(strcmp(cf_encoding_name(CF_ENC_IDENTITY), "identity") == 0);
    CF_CHECK(cf_encoding_name(CF_ENC_UNACCEPTABLE) == NULL);
}

CF_TEST_MAIN()
