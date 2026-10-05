/* K01a cf_gzip tests (06-cache-performance.md:84; c-port-arch.md
 * "Compression and SQLite").
 *
 * The exact vectors were produced by the pinned zlib-ng 2.3.3 archive this
 * build links (vendor/build/zlib-ng-clang/libz.a, DEPS.json entry zlib-ng)
 * running the same code, then embedded here; a fixed input must always
 * produce these bytes. Header bytes are also checked structurally against
 * the pinned reference's GzBuilder::new().mtime(0).operating_system(3)
 * (tmp/rust-ref crates/kit/src/deflater.rs:216-218): FLG 0, MTIME 0, XFL 0
 * (level 6), OS 3. Every vector is round-tripped through zlib inflate, so a
 * wrong stream cannot pass by matching bytes alone. */
#include "cf.h"

#include "cf_test.h"
#include "core/alloc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

static cf_span S(const char *s) {
    return (cf_span){(const unsigned char *)s, strlen(s)};
}

static void hex_of(const unsigned char *p, size_t n, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[p[i] >> 4];
        out[2 * i + 1] = digits[p[i] & 0xF];
    }
    out[2 * n] = '\0';
}

/* Compresses, checks the exact member bytes, checks the pinned header
 * fields, and inflates back to the input. */
static void check_vector(const unsigned char *in, size_t n, const char *want_hex) {
    cf_buf *g = NULL;
    CF_REQUIRE(cf_gzip((cf_span){in, n}, &g) == CF_OK);
    CF_REQUIRE(g != NULL);
    cf_span gs = cf_buf_span(g);

    CF_REQUIRE(gs.len >= 20);
    CF_CHECK(gs.ptr[0] == 0x1F && gs.ptr[1] == 0x8B && gs.ptr[2] == 0x08);
    CF_CHECK(gs.ptr[3] == 0x00); /* FLG: no FTEXT/FHCRC/FEXTRA/FNAME/FCOMMENT */
    CF_CHECK(gs.ptr[4] == 0 && gs.ptr[5] == 0 && gs.ptr[6] == 0 &&
             gs.ptr[7] == 0); /* MTIME 0 */
    CF_CHECK(gs.ptr[8] == 0x00); /* XFL: level 6 */
    CF_CHECK(gs.ptr[9] == 0x03); /* OS: Unix, as GzBuilder::operating_system(3) */
    uint32_t isize = (uint32_t)gs.ptr[gs.len - 4] |
                     ((uint32_t)gs.ptr[gs.len - 3] << 8) |
                     ((uint32_t)gs.ptr[gs.len - 2] << 16) |
                     ((uint32_t)gs.ptr[gs.len - 1] << 24);
    CF_CHECK(isize == (uint32_t)n);

    if (gs.len < 4096) {
        char hex[8192];
        hex_of(gs.ptr, gs.len, hex);
        if (strcmp(hex, want_hex) != 0) {
            fprintf(stderr, "  gzip(%zu) = %s\n  want %s\n", n, hex, want_hex);
        }
        CF_CHECK(strcmp(hex, want_hex) == 0);
    }

    /* Round trip through zlib inflate (gzip wrapper). */
    z_stream z;
    memset(&z, 0, sizeof z);
    CF_REQUIRE(inflateInit2(&z, 15 + 16) == Z_OK);
    cf_builder out = {0};
    unsigned char chunk[8192];
    z.next_in = (Bytef *)(uintptr_t)gs.ptr;
    z.avail_in = (uint32_t)gs.len;
    for (;;) {
        z.next_out = chunk;
        z.avail_out = (uint32_t)sizeof chunk;
        int zr = inflate(&z, Z_NO_FLUSH);
        size_t produced = sizeof chunk - z.avail_out;
        if (produced != 0) {
            CF_REQUIRE(cf_builder_append(&out, (cf_span){chunk, produced}) ==
                       CF_OK);
        }
        if (zr == Z_STREAM_END) break;
        CF_REQUIRE(zr == Z_OK);
        CF_REQUIRE(produced != 0); /* inflate must progress */
    }
    CF_CHECK(z.avail_in == 0);
    CF_CHECK(out.len == n && (n == 0 || memcmp(out.ptr, in, n) == 0));
    cf_builder_dispose(&out);
    inflateEnd(&z);
    cf_buf_release(g);
}

CF_TEST(gzip_empty_member) {
    check_vector((const unsigned char *)"", 0,
                 "1f8b080000000000000303000000000000000000");
    cf_buf *g = NULL;
    CF_REQUIRE(cf_gzip((cf_span){NULL, 0}, &g) == CF_OK); /* null+0 is empty */
    CF_CHECK(cf_buf_span(g).len == 20);
    cf_buf_release(g);
}

CF_TEST(gzip_exact_vector_small) {
    check_vector((const unsigned char *)"hello campfire\n", 15,
                 "1f8b0800000000000003cb48cdc9c957484ecc2d48cb2c4ae50200"
                 "c385e0000f000000");
}

CF_TEST(gzip_exact_vector_repeating_1k) {
    unsigned char in[1024];
    for (int i = 0; i < 64; i++) memcpy(in + i * 16, "hello campfire! ", 16);
    check_vector(in, sizeof in,
                 "1f8b0800000000000003cb48cdc9c957484ecc2d48cb2c4a5554c818"
                 "e58f86c7687a481c29f90100daf034fa00040000");
}

CF_TEST(gzip_exact_vector_all_byte_values) {
    unsigned char in[256];
    for (int i = 0; i < 256; i++) in[i] = (unsigned char)i;
    check_vector(in, sizeof in,
                 "1f8b0800000000000003010001fffe000102030405060708090a0b0c0d"
                 "0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a"
                 "2b2c2d2e2f303132333435363738393a3b3c3d3e3f4041424344454647"
                 "48494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f6061626364"
                 "65666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f8081"
                 "82838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e"
                 "9fa0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babb"
                 "bcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8"
                 "d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5"
                 "f6f7f8f9fafbfcfdfeff738c052900010000");
}

/* Smallest input found where zlib-ng level 5 and level 6 differ (134 vs 135
 * bytes, different bytes), so this exact vector pins the level-6 choice, not
 * just gzip validity. Generated by the 11-element loop below. */
CF_TEST(gzip_exact_vector_pins_level_6) {
    unsigned char in[4096];
    size_t p = 0;
    for (int k = 1; k <= 11; k++) {
        int repeats = k % 13;
        p += (size_t)snprintf((char *)in + p, sizeof in - p,
                              "<div id=\"message_%d\" class=\"message\">", k);
        for (int t = 0; t < repeats; t++) {
            memcpy(in + p, "hello there ", 12);
            p += 12;
        }
        memcpy(in + p, "</div>\n", 7);
        p += 7;
    }
    CF_REQUIRE(p == 1267); /* the vector below was produced for exactly this */
    check_vector(in, p,
                 "1f8b0800000000000003b349c92c53c84cb155ca4d2d2e4e4c4f8d37"
                 "545248ce492c2e868b28d965a4e6e4e42b9464a416a52ad8e8a76496"
                 "d971d9a06b33c2af8d18238c89378218e34cc8338e18a34d29379a18"
                 "6bcca86b0d31569ad3ce4a62acb7a08ff5c438c592fe4e21c6598606"
                 "03ebae0c62dc48a00c1960f702008f7594dbf3040000");
}

/* Larger than the 16 KiB drain chunk, with structure so matches run across
 * chunk boundaries; round-trip and determinism, not a byte vector. */
CF_TEST(gzip_roundtrip_and_determinism_across_chunks) {
    size_t n = 300000;
    unsigned char *in = malloc(n);
    CF_REQUIRE(in != NULL);
    uint32_t state = 0xC0FFEE01u;
    for (size_t i = 0; i < n; i++) {
        state = state * 1664525u + 1013904223u;
        in[i] = (unsigned char)((i % 37 == 0) ? (state >> 24)
                                              : "campfire "[i % 9]);
    }
    check_vector(in, n, ""); /* hex skipped above 4096 bytes */
    free(in);

    /* Same input, two calls, identical bytes; different input differs. */
    const unsigned char a[] = "determinism is a representation property";
    const unsigned char b[] = "determinism is a representation propertZ";
    cf_buf *g1 = NULL, *g2 = NULL, *g3 = NULL;
    CF_REQUIRE(cf_gzip((cf_span){a, sizeof a - 1}, &g1) == CF_OK);
    CF_REQUIRE(cf_gzip((cf_span){a, sizeof a - 1}, &g2) == CF_OK);
    CF_REQUIRE(cf_gzip((cf_span){b, sizeof b - 1}, &g3) == CF_OK);
    cf_span s1 = cf_buf_span(g1), s2 = cf_buf_span(g2), s3 = cf_buf_span(g3);
    CF_CHECK(s1.len == s2.len && memcmp(s1.ptr, s2.ptr, s1.len) == 0);
    CF_CHECK(s3.len != s1.len || memcmp(s3.ptr, s1.ptr, s1.len) != 0);
    cf_buf_release(g1);
    cf_buf_release(g2);
    cf_buf_release(g3);
}

CF_TEST(gzip_argument_errors_clear_out) {
    cf_buf *out = (cf_buf *)(uintptr_t)0x1;
    CF_CHECK(cf_gzip(S("x"), NULL) == CF_INVALID);
    CF_CHECK(cf_gzip((cf_span){NULL, 3}, &out) == CF_INVALID);
    CF_CHECK(out == NULL);
    out = (cf_buf *)(uintptr_t)0x1;
    CF_CHECK(cf_gzip((cf_span){NULL, 0}, &out) == CF_OK);
    CF_CHECK(out != NULL);
    cf_buf_release(out);
}

static void *failing_alloc(size_t size) {
    (void)size;
    return NULL;
}

static void *failing_realloc(void *ptr, size_t size) {
    (void)ptr;
    (void)size;
    return NULL;
}

static void failing_free(void *ptr) { free(ptr); }

CF_TEST(gzip_reports_nomem_without_touching_out) {
    cf_core_set_allocator(failing_alloc, failing_realloc, failing_free);
    cf_buf *out = (cf_buf *)(uintptr_t)0x1;
    cf_err rc = cf_gzip(S("hello campfire"), &out);
    cf_core_reset_allocator();
    CF_CHECK(rc == CF_NOMEM);
    CF_CHECK(out == NULL);

    /* The same call succeeds once allocation is restored. */
    out = NULL;
    CF_REQUIRE(cf_gzip(S("hello campfire"), &out) == CF_OK);
    CF_CHECK(cf_buf_span(out).len > 20);
    cf_buf_release(out);
}

CF_TEST_MAIN()
