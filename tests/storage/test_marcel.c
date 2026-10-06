/* tests/storage/test_marcel.c — Marcel identification (S02 unfurl half).
 *
 * Reads the pinned marcel vectors from tests/fixtures/vectors/storage.json
 * (tmp/rust-ref/vectors/storage.json, produced against marcel 1.1.0):
 * `data_hex` rows are compared exactly; rows whose bytes live in a reference
 * fixture file (.keep, alpha-centuri.mov, black_hole.jpg, earth.png, moon.jpg,
 * pixel.bmp) are counted and reported BLOCKED when the fixture binary is not
 * present under tests/fixtures/vectors/storage/ — never skipped silently and
 * never passed without bytes. Synthetic bytes for the table's own magic
 * signatures (PNG/JPEG/PDF/GIF/ZIP/DOCX) cover the magic path beyond the
 * hex rows, and extension/declared-type/most-specific/parent-graph cases pin
 * the lookup semantics.
 */
#include "cf_test.h"

#include "storage/marcel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yyjson.h"

static cf_span span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

static char *read_file(const char *path, size_t *out_len) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long len = ftell(file);
    if (len < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *data = malloc((size_t)len + 1);
    if (data == NULL) {
        fclose(file);
        return NULL;
    }
    size_t got = fread(data, 1, (size_t)len, file);
    fclose(file);
    if (got != (size_t)len) {
        free(data);
        return NULL;
    }
    data[len] = '\0';
    *out_len = (size_t)len;
    return data;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool decode_hex(cf_span text, unsigned char **out, size_t *out_len) {
    unsigned char *data = malloc(text.len / 2 + 1);
    if (data == NULL) return false;
    size_t at = 0;
    for (size_t i = 0; i + 1 < text.len; i += 2) {
        int hi = hex_digit((char)text.ptr[i]);
        int lo = hex_digit((char)text.ptr[i + 1]);
        if (hi < 0 || lo < 0) {
            free(data);
            return false;
        }
        data[at++] = (unsigned char)((hi << 4) | lo);
    }
    *out = data;
    *out_len = at;
    return true;
}

static cf_err identify(cf_span data, cf_span name, cf_span declared,
                       bool has_declared, char *out, size_t cap) {
    cf_builder builder = {0};
    cf_err rc = cf_marcel_identify(data, name, declared, has_declared,
                                   &builder);
    if (rc == CF_OK) {
        if (builder.len + 1 > cap) {
            rc = CF_INVALID;
        } else {
            memcpy(out, builder.ptr != NULL ? (const char *)builder.ptr : "",
                   builder.len);
            out[builder.len] = '\0';
        }
    }
    cf_builder_dispose(&builder);
    return rc;
}

CF_TEST(marcel_vectors) {
    const char *path = "tests/fixtures/vectors/storage.json";
    size_t len = 0;
    char *text = read_file(path, &len);
    if (text == NULL) {
        /* The fixture is mandatory for this test; report the missing path
         * loudly rather than passing with zero cases. */
        fprintf(stderr, "  missing fixture: %s\n", path);
    }
    CF_REQUIRE(text != NULL);

    yyjson_doc *doc = yyjson_read(text, len, 0);
    CF_REQUIRE(doc != NULL);
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *vectors = yyjson_obj_get(root, "marcel");
    CF_REQUIRE(vectors != NULL && yyjson_is_arr(vectors));

    size_t compared = 0, blocked = 0, index = 0;
    yyjson_val *vector;
    yyjson_arr_iter iter = yyjson_arr_iter_with(vectors);
    while ((vector = yyjson_arr_iter_next(&iter)) != NULL) {
        yyjson_val *jname = yyjson_obj_get(vector, "name");
        yyjson_val *jhex = yyjson_obj_get(vector, "data_hex");
        yyjson_val *jfixture = yyjson_obj_get(vector, "fixture");
        yyjson_val *jdeclared = yyjson_obj_get(vector, "declared_type");
        yyjson_val *jexpect = yyjson_obj_get(vector, "content_type");
        CF_REQUIRE(jname != NULL && yyjson_is_str(jname));
        CF_REQUIRE(jexpect != NULL && yyjson_is_str(jexpect));
        cf_span name = {(const unsigned char *)yyjson_get_str(jname),
                        yyjson_get_len(jname)};
        cf_span expect = {(const unsigned char *)yyjson_get_str(jexpect),
                          yyjson_get_len(jexpect)};
        bool has_declared =
            jdeclared != NULL && yyjson_is_str(jdeclared) &&
            yyjson_get_len(jdeclared) > 0;
        cf_span declared = {NULL, 0};
        if (has_declared) {
            declared = (cf_span){(const unsigned char *)yyjson_get_str(jdeclared),
                                 yyjson_get_len(jdeclared)};
        }
        unsigned char *data = NULL;
        size_t data_len = 0;
        bool have_data = false;
        if (jhex != NULL && yyjson_is_str(jhex)) {
            cf_span hex = {(const unsigned char *)yyjson_get_str(jhex),
                           yyjson_get_len(jhex)};
            CF_REQUIRE(decode_hex(hex, &data, &data_len));
            have_data = true;
        } else if (jfixture != NULL && yyjson_is_str(jfixture)) {
            char fixture_path[512];
            snprintf(fixture_path, sizeof fixture_path,
                     "tests/fixtures/vectors/storage/%s",
                     yyjson_get_str(jfixture));
            size_t file_len = 0;
            char *file = read_file(fixture_path, &file_len);
            if (file != NULL) {
                data = (unsigned char *)file;
                data_len = file_len;
                have_data = true;
            }
        }
        if (!have_data) {
            blocked++;
            fprintf(stderr,
                    "  BLOCKED marcel vector %zu (%s): fixture bytes absent\n",
                    index, (const char *)name.ptr);
            index++;
            continue;
        }
        char got[128];
        CF_REQUIRE(identify((cf_span){data, data_len}, name, declared,
                            has_declared, got, sizeof got) == CF_OK);
        if (strlen(got) != expect.len ||
            memcmp(got, expect.ptr, expect.len) != 0) {
            fprintf(stderr, "  vector %zu (%.*s): got %s want %.*s\n", index,
                    (int)name.len, name.ptr, got, (int)expect.len,
                    (const char *)expect.ptr);
        }
        CF_CHECK(strlen(got) == expect.len &&
                 memcmp(got, expect.ptr, expect.len) == 0);
        compared++;
        free(data);
        index++;
    }
    yyjson_doc_free(doc);
    free(text);
    fprintf(stderr, "marcel vectors: compared=%zu blocked=%zu\n", compared,
            blocked);
    /* The data_hex rows are the runnable half; the fixture-file rows are
     * BLOCKED (fixture binaries are not in the repository fixtures). */
    CF_CHECK(compared >= 15);
}

CF_TEST(marcel_magic_synthetic_and_helpers) {
    char out[128];
    /* Pinned magic signatures (tables.rs MAGIC rows), no fixture needed. */
    static const struct {
        const char *bytes;
        size_t len;
        const char *name;
        const char *want;
    } cases[] = {
        {"\x89PNG\r\n\x1a\n", 8, "x", "image/png"},
        {"\xff\xd8\xff\xe0", 4, "x", "image/jpeg"},
        {"GIF89a", 6, "x", "image/gif"},
        {"%PDF-1.4", 8, "x", "application/pdf"},
        {"PK\x03\x04rest", 8, "archive.zip", "application/zip"},
        {"PK\x03\x04rest", 8, "report.docx",
         "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        cf_span data = {(const unsigned char *)cases[i].bytes, cases[i].len};
        CF_REQUIRE(identify(data, span(cases[i].name), (cf_span){NULL, 0},
                            false, out, sizeof out) == CF_OK);
        if (strcmp(out, cases[i].want) != 0) {
            fprintf(stderr, "  magic %zu: got %s want %s\n", i, out,
                    cases[i].want);
        }
        CF_CHECK(strcmp(out, cases[i].want) == 0);
    }
    /* Extension + declared rules. */
    CF_REQUIRE(identify((cf_span){NULL, 0}, span("renamed.txt"),
                        (cf_span){NULL, 0}, false, out, sizeof out) == CF_OK);
    CF_CHECK(strcmp(out, "text/plain") == 0);
    CF_REQUIRE(identify((cf_span){NULL, 0}, span(".keep"), (cf_span){NULL, 0},
                        false, out, sizeof out) == CF_OK);
    CF_CHECK(strcmp(out, "application/octet-stream") == 0);
    CF_REQUIRE(identify((cf_span){NULL, 0}, span("x"), span("Text/Plain; q=1"),
                        true, out, sizeof out) == CF_OK);
    CF_CHECK(strcmp(out, "text/plain") == 0);
    CF_REQUIRE(identify((cf_span){NULL, 0}, span("x"),
                        span("application/octet-stream"), true, out,
                        sizeof out) == CF_OK);
    CF_CHECK(strcmp(out, "application/octet-stream") == 0);
    /* UPPER.JPG: extension lookup is case-insensitive. */
    CF_REQUIRE(identify((cf_span){NULL, 0}, span("UPPER.JPG"),
                        (cf_span){NULL, 0}, false, out, sizeof out) == CF_OK);
    CF_CHECK(strcmp(out, "image/jpeg") == 0);
    /* Declared type must lose to magic; a filename type must lose to both. */
    CF_REQUIRE(identify(
                   (cf_span){(const unsigned char *)"\x89PNG\r\n\x1a\n", 8},
                   span("x.txt"), span("image/gif"), true, out,
                   sizeof out) == CF_OK);
    CF_CHECK(strcmp(out, "image/png") == 0);
    /* docx over zip comes from the parent graph, not just the table. */
    CF_CHECK(strcmp(cf_marcel_first_extension(span("image/png")), "png") == 0);
    CF_CHECK(cf_marcel_first_extension(span("application/x-nope")) == NULL);
    cf_builder ext = {0};
    bool found = false;
    CF_REQUIRE(cf_marcel_for_extension(span(".JPG"), &found, &ext) == CF_OK);
    CF_REQUIRE(found);
    CF_CHECK(ext.len == 10 && memcmp(ext.ptr, "image/jpeg", 10) == 0);
    cf_builder_dispose(&ext);
    CF_CHECK(cf_marcel_magic_prefix_len() == 65555);
    CF_CHECK(cf_marcel_extname(span("a/b.c")).len == 2);
    CF_CHECK(cf_marcel_extname(span(".bashrc")).len == 0);
    CF_CHECK(cf_marcel_extname(span("a.b.")).len == 1);
}

CF_TEST_MAIN()
