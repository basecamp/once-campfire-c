/* tests/storage/test_active_storage.c — S02 unit tests (STORE-02/03 helper
 * level): signed blob ids, disk keys/tokens and variation keys (round-trip,
 * wrong-purpose, tampered, expired, malformed; byte-exact generation against
 * the A01 app-verifier vectors and storage.json crypto/format vectors);
 * filename sanitize/disposition/escaping; Rack-exact byte ranges; proxy and
 * disk serve planning; direct-upload checks; attachment states; latch-based
 * concurrent variant first-wins; purge recheck; disk PUT checksum; S03
 * fail-loud.
 *
 * Provenance (read-only, never regenerated):
 *   tests/auth/vectors.h (AUTH_VEC_SECRET_KEY_BASE, AUTH_VEC_APP_GEN/VERIFY)
 *   tmp/rust-ref/vectors/storage.json (variations, filenames, verifier paths)
 *   tmp/rust-ref/crates/ruby/src/rack.rs (RACK_RANGES rows)
 *
 * Build (direct cc; proposed integrator patch adds
 * tests/storage/test_active_storage.c to the Makefile UNIT_TEST_SRCS list):
 *   V=/home/msaraiva/dev/mark/Proj/Misc/once-campfire-c/vendor
 *   clang -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra \
 *         -Werror -pthread -O1 -g -Isrc -Itests -I$V/src/yyjson/src \
 *         -I$V/build/openssl-clang/install/include \
 *         tests/storage/test_active_storage.c src/storage/active_storage.c \
 *         src/storage/files.c src/auth/tokens.c src/auth/crypto.c \
 *         src/auth/json.c src/auth/message.c \
 *         src/core/alloc.c src/core/buffer.c src/core/clock.c \
 *         src/core/error.c src/core/random.c src/models/types.c \
 *         $V/src/yyjson/src/yyjson.c \
 *         $V/build/openssl-clang/install/lib/libcrypto.a -ldl \
 *         -o /tmp/opencode/test_active_storage
 */
#include "cf_test.h"

#include "storage/active_storage.h"
#include "storage/storage.h"

#include "auth.h"
#include "auth/vectors.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static cf_span S(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

static bool str_is(cf_str value, const char *want) {
    size_t len = strlen(want);
    return value.ptr != NULL && value.len == len &&
           memcmp(value.ptr, want, len) == 0;
}

static bool builder_is(cf_builder *b, const char *want) {
    size_t len = strlen(want);
    return b->ptr != NULL && b->len == len && memcmp(b->ptr, want, len) == 0;
}

static cf_span secret(void) {
    return S(AUTH_VEC_SECRET_KEY_BASE);
}

static bool span_eq(cf_span a, cf_span b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.ptr, b.ptr, a.len) == 0);
}

/* Fixed UTC microseconds (see vectors.h ISO strings). */
#define NOW_2026_01_01_12_00 INT64_C(1767268800000000)
#define NOW_2026_01_01_12_04 INT64_C(1767269040000000)
#define NOW_2026_01_01_12_05 INT64_C(1767269100000000)
#define VEC_EXPIRY_12_05 INT64_C(1767269100000000)
#define NOW_2026_09_26_12_00 INT64_C(1790424000000000)

/* ---- signed blob ids ------------------------------------------------------ */

CF_TEST(blob_id_generate_matches_reference) {
    /* AUTH_VEC_APP_GEN rows 0-1: {"ActiveStorage", id, "blob_id", no expiry}. */
    for (size_t i = 0; i < 2; i++) {
        const auth_vec_app_gen *vec = &AUTH_VEC_APP_GEN[i];
        int64_t id = 0;
        CF_REQUIRE(cf_active_integer_cast(S(vec->data_json), &id));
        cf_str generated = {0};
        CF_REQUIRE(cf_active_blob_sign(secret(), id, false, 0, &generated) == CF_OK);
        if (!str_is(generated, vec->message)) {
            printf("    row %zu: got %s want %s\n", i, generated.ptr, vec->message);
            CF_CHECK(false);
        }
        cf_str_dispose(&generated);
    }
}

CF_TEST(blob_id_verify_matrix) {
    /* Valid tokens verify; wrong purpose, tampering, garbage and a token
     * decoded-without-verifying never authorize. */
    const char *good =
        "eyJfcmFpbHMiOnsiZGF0YSI6NDIsInB1ciI6ImJsb2JfaWQifX0=--c252dc1b14dd0e8cd599eb40203378880bafed1f";
    int64_t id = 0;
    bool found = false;
    CF_REQUIRE(cf_active_blob_verify(secret(), S(good), NOW_2026_01_01_12_00,
                                     &id, &found) == CF_OK);
    CF_CHECK(found && id == 42);
    /* Wrong purpose. */
    CF_REQUIRE(cf_active_blob_verify(secret(), S(good), NOW_2026_01_01_12_00,
                                     &id, &found) == CF_OK);
    (void)id;
    {
        /* Re-verify the blob_id token under the blob_key purpose through the
         * disk-key helper: must not be found. */
        cf_active_disk_key key = {0};
        bool key_found = true;
        CF_REQUIRE(cf_active_disk_key_verify(secret(), S(good),
                                             NOW_2026_01_01_12_00, &key,
                                             &key_found) == CF_OK);
        CF_CHECK(!key_found);
        cf_active_disk_key_dispose(&key);
    }
    /* Tampered digest. */
    const char *tampered =
        "eyJfcmFpbHMiOnsiZGF0YSI6NDIsInB1ciI6ImJsb2JfaWQifX0=--c252dc1b14dd0e8cd599eb40203378880bafed10";
    CF_REQUIRE(cf_active_blob_verify(secret(), S(tampered),
                                     NOW_2026_01_01_12_00, &id,
                                     &found) == CF_OK);
    CF_CHECK(!found);
    /* Garbage and truncated input. */
    CF_REQUIRE(cf_active_blob_verify(secret(), S("garbage"),
                                     NOW_2026_01_01_12_00, &id,
                                     &found) == CF_OK);
    CF_CHECK(!found);
    CF_REQUIRE(cf_active_blob_verify(secret(), S("eyJfcmFpbHMiOnsiZGF0YSI6NDI"),
                                     NOW_2026_01_01_12_00, &id,
                                     &found) == CF_OK);
    CF_CHECK(!found);
    /* A valid signature for a non-id payload is not an id. */
    const char *str_payload =
        "eyJfcmFpbHMiOnsiZGF0YSI6IjAxMjNhYmMiLCJwdXIiOiJ2YXJpYXRpb24ifX0=--a7fae304e60ae716c5ff86c403cbdf5caa286719";
    CF_REQUIRE(cf_active_blob_verify(secret(), S(str_payload),
                                     NOW_2026_01_01_12_00, &id,
                                     &found) == CF_OK);
    CF_CHECK(!found);
    /* NULL input is absent, not a crash. */
    CF_REQUIRE(cf_active_blob_verify(secret(), (cf_span){NULL, 0},
                                     NOW_2026_01_01_12_00, &id,
                                     &found) == CF_OK);
    CF_CHECK(!found);
}

CF_TEST(blob_id_expiry_enforced) {
    /* Signed with an expiry: valid before, rejected at/after. */
    cf_str token = {0};
    CF_REQUIRE(cf_active_blob_sign(secret(), 7, true, VEC_EXPIRY_12_05,
                                   &token) == CF_OK);
    int64_t id = 0;
    bool found = false;
    CF_REQUIRE(cf_active_blob_verify(secret(),
                                     (cf_span){(unsigned char *)token.ptr, token.len},
                                     NOW_2026_01_01_12_04, &id,
                                     &found) == CF_OK);
    CF_CHECK(found && id == 7);
    CF_REQUIRE(cf_active_blob_verify(secret(),
                                     (cf_span){(unsigned char *)token.ptr, token.len},
                                     NOW_2026_01_01_12_05, &id,
                                     &found) == CF_OK);
    CF_CHECK(!found);
    cf_str_dispose(&token);
}

/* ---- disk keys and upload tokens ------------------------------------------- */

CF_TEST(disk_key_generate_matches_reference) {
    /* AUTH_VEC_APP_GEN row 2: exact blob_key envelope bytes. */
    const auth_vec_app_gen *vec = &AUTH_VEC_APP_GEN[2];
    CF_REQUIRE(strcmp(vec->purpose, "blob_key") == 0);
    cf_str generated = {0};
    CF_REQUIRE(cf_active_disk_key_sign(
                   secret(), S("xtapjjcjiudrlk3tmwyjgpuobabd"),
                   S("inline; filename=\"a&b.png\"; filename*=UTF-8''a%26b.png"),
                   S("image/png"), true, S("local"), true, VEC_EXPIRY_12_05,
                   &generated) == CF_OK);
    /* The reference data_json uses \u0026 for &; our escaper must too. */
    if (!str_is(generated, vec->message)) {
        printf("    got  %s\n    want %s\n", generated.ptr, vec->message);
        CF_CHECK(false);
    }
    cf_str_dispose(&generated);
}

CF_TEST(disk_key_verify_matrix) {
    const char *good =
        "eyJfcmFpbHMiOnsiZGF0YSI6eyJrZXkiOiJ4dGFwampjaml1ZHJsazN0bXd5amdwdW9iYWJkIiwiZGlzcG9zaXRpb24iOiJpbmxpbmU7IGZpbGVuYW1lPVwiYVx1MDAyNmIucG5nXCI7IGZpbGVuYW1lKj1VVEYtOCcnYSUyNmIucG5nIiwiY29udGVudF90eXBlIjoiaW1hZ2UvcG5nIiwic2VydmljZV9uYW1lIjoibG9jYWwifSwiZXhwIjoiMjAyNi0wMS0wMVQxMjowNTowMC4wMDBaIiwicHVyIjoiYmxvYl9rZXkifX0=--764195902735b3201f5cf6129ff3b5089150e3e1";
    cf_active_disk_key key = {0};
    bool found = false;
    CF_REQUIRE(cf_active_disk_key_verify(secret(), S(good),
                                         NOW_2026_01_01_12_04, &key,
                                         &found) == CF_OK);
    CF_REQUIRE(found);
    CF_CHECK(str_is(key.key, "xtapjjcjiudrlk3tmwyjgpuobabd"));
    CF_CHECK(str_is(key.disposition,
                    "inline; filename=\"a&b.png\"; filename*=UTF-8''a%26b.png"));
    CF_CHECK(key.content_type.present &&
             str_is(key.content_type.value, "image/png"));
    CF_CHECK(str_is(key.service_name, "local"));
    cf_active_disk_key_dispose(&key);
    /* Expired at the envelope time. */
    CF_REQUIRE(cf_active_disk_key_verify(secret(), S(good),
                                         NOW_2026_01_01_12_05, &key,
                                         &found) == CF_OK);
    CF_CHECK(!found);
    cf_active_disk_key_dispose(&key);
    /* A blob_token is not a blob_key. */
    const char *token =
        "eyJfcmFpbHMiOnsiZGF0YSI6eyJrZXkiOiJrIiwiY29udGVudF90eXBlIjoidGV4dC9wbGFpbiIsImNvbnRlbnRfbGVuZ3RoIjoxMiwiY2hlY2tzdW0iOiJhYmM9PSIsInNlcnZpY2VfbmFtZSI6ImxvY2FsIn0sImV4cCI6IjIwMjYtMDEtMDFUMTI6MDU6MDAuMDAwWiIsInB1ciI6ImJsb2JfdG9rZW4ifX0=--fc9c359d9a82afb6ad1f7cbb5c2056c05eb12081";
    CF_REQUIRE(cf_active_disk_key_verify(secret(), S(token),
                                         NOW_2026_01_01_12_04, &key,
                                         &found) == CF_OK);
    CF_CHECK(!found);
    cf_active_disk_key_dispose(&key);
}

CF_TEST(disk_token_round_trip_and_accept) {
    cf_str encoded = {0};
    CF_REQUIRE(cf_active_disk_token_sign(secret(), S("abcdefghij1234567890abcd"),
                                         S("image/png"), true, 42, S("abc=="),
                                         S("local"), VEC_EXPIRY_12_05,
                                         &encoded) == CF_OK);
    cf_active_disk_token token = {0};
    bool found = false;
    CF_REQUIRE(cf_active_disk_token_verify(
                   secret(),
                   (cf_span){(unsigned char *)encoded.ptr, encoded.len},
                   NOW_2026_01_01_12_04, &token, &found) == CF_OK);
    CF_REQUIRE(found);
    CF_CHECK(str_is(token.key, "abcdefghij1234567890abcd"));
    CF_CHECK(token.content_length == 42);
    CF_CHECK(str_is(token.checksum, "abc=="));
    /* Content type compares case-insensitively; length must match exactly. */
    CF_CHECK(cf_active_disk_token_acceptable(&token, S("image/png"), 42, true));
    CF_CHECK(cf_active_disk_token_acceptable(&token, S("IMAGE/PNG"), 42, true));
    CF_CHECK(cf_active_disk_token_acceptable(&token,
                                             S("image/png; charset=x"), 42,
                                             true));
    CF_CHECK(!cf_active_disk_token_acceptable(&token, S("image/jpeg"), 42, true));
    CF_CHECK(!cf_active_disk_token_acceptable(&token, S("image/png"), 43, true));
    CF_CHECK(!cf_active_disk_token_acceptable(&token, S("image/png"), 42, false));
    cf_active_disk_token_dispose(&token);
    cf_str_dispose(&encoded);
    /* Null content type: absent matches absent only. */
    CF_REQUIRE(cf_active_disk_token_sign(secret(), S("k2"), S(""), false, 1,
                                         S("c=="), S("local"), VEC_EXPIRY_12_05,
                                         &encoded) == CF_OK);
    CF_REQUIRE(cf_active_disk_token_verify(
                   secret(),
                   (cf_span){(unsigned char *)encoded.ptr, encoded.len},
                   NOW_2026_01_01_12_04, &token, &found) == CF_OK);
    CF_REQUIRE(found);
    CF_CHECK(!token.content_type.present);
    CF_CHECK(cf_active_disk_token_acceptable(&token, (cf_span){NULL, 0}, 1, true));
    CF_CHECK(!cf_active_disk_token_acceptable(&token, S("text/plain"), 1, true));
    cf_active_disk_token_dispose(&token);
    cf_str_dispose(&encoded);
}

/* ---- variations: storage.json vectors --------------------------------------- */

static cf_active_vval *mk_sym(const char *text) {
    cf_active_vval *v = NULL;
    CF_REQUIRE(cf_active_vsym(S(text), &v) == CF_OK);
    return v;
}

static cf_active_vval *mk_str(const char *text) {
    cf_active_vval *v = NULL;
    CF_REQUIRE(cf_active_vstr(S(text), &v) == CF_OK);
    return v;
}

static cf_active_vval *mk_int(int64_t n) {
    cf_active_vval *v = NULL;
    CF_REQUIRE(cf_active_vint(n, &v) == CF_OK);
    return v;
}

static cf_active_vval *mk_resize(int64_t w, int64_t h) {
    cf_active_vval *arr = NULL;
    CF_REQUIRE(cf_active_varr(&arr) == CF_OK);
    cf_active_vval *a = mk_int(w);
    cf_active_vval *b = mk_int(h);
    CF_REQUIRE(cf_active_varr_push(arr, a) == CF_OK);
    CF_REQUIRE(cf_active_varr_push(arr, b) == CF_OK);
    return arr;
}

static void entries_put(cf_active_ventries *e, const char *key,
                        cf_active_vval *v) {
    CF_REQUIRE(cf_active_ventries_push(e, S(key), v) == CF_OK);
}

static void check_str(cf_str value, const char *want, const char *label) {
    if (!str_is(value, want)) {
        printf("    %s: got %s want %s\n", label,
               value.ptr == NULL ? "(null)" : value.ptr, want);
        CF_CHECK(false);
    }
}

/* One storage.json variations row: typed entries digest to `digest`, sign to
 * `key`, and the key decodes to string-valued entries digesting to
 * `decoded_digest`. is_sym selects symbol (typed) vs string values. */
static void check_variation_row(cf_active_ventries *typed, const char *digest,
                                 const char *key, const char *decoded_digest,
                                 const char *label) {
    cf_str got_digest = {0};
    CF_REQUIRE(cf_active_variation_digest(typed, &got_digest) == CF_OK);
    check_str(got_digest, digest, label);
    cf_str_dispose(&got_digest);
    cf_str signed_key = {0};
    CF_REQUIRE(cf_active_variation_sign(secret(), typed, &signed_key) == CF_OK);
    check_str(signed_key, key, label);
    cf_active_ventries decoded = {0};
    bool found = false;
    CF_REQUIRE(cf_active_variation_verify(
                   secret(),
                   (cf_span){(unsigned char *)signed_key.ptr, signed_key.len},
                   NOW_2026_09_26_12_00, &decoded, &found) == CF_OK);
    CF_REQUIRE(found);
    CF_REQUIRE(decoded.len == typed->len);
    /* Decoded values are strings, never symbols; digests must match the
     * decoded_digest column (equal to digest for string-typed rows). */
    for (size_t i = 0; i < decoded.len; i++) {
        CF_CHECK(decoded.items[i].key_len == typed->items[i].key_len &&
                 memcmp(decoded.items[i].key, typed->items[i].key,
                        decoded.items[i].key_len) == 0);
        if (typed->items[i].value->kind == CF_ACTIVE_V_SYM) {
            CF_CHECK(decoded.items[i].value->kind == CF_ACTIVE_V_STR);
            CF_CHECK(decoded.items[i].value->len ==
                     typed->items[i].value->len);
        } else {
            CF_CHECK(cf_active_vval_equal(decoded.items[i].value,
                                          typed->items[i].value));
        }
    }
    CF_REQUIRE(cf_active_variation_digest(&decoded, &got_digest) == CF_OK);
    check_str(got_digest, decoded_digest, label);
    cf_str_dispose(&got_digest);
    cf_active_ventries_dispose(&decoded);
    cf_str_dispose(&signed_key);
}

CF_TEST(variation_vectors_digest_and_key) {
    /* {format: :webp, resize_to_limit: [512, 512]} */
    {
        cf_active_ventries e = {0};
        entries_put(&e, "format", mk_sym("webp"));
        entries_put(&e, "resize_to_limit", mk_resize(512, 512));
        check_variation_row(&e, "6gwfjNKv9eUy9jNUtEZvQFLU0hQ=",
                            "eyJfcmFpbHMiOnsiZGF0YSI6eyJmb3JtYXQiOiJ3ZWJwIiwicmVzaXplX3RvX2xpbWl0IjpbNTEyLDUxMl19LCJwdXIiOiJ2YXJpYXRpb24ifX0=--fdaf7fb16189b7c943c4e2dcf1589b6be23516de",
                            "3xm5qtUwCk3YQHQ55FXsVFKT908=", "webp-512-sym");
        cf_active_ventries_dispose(&e);
    }
    /* {format: :png, resize_to_limit: [512, 512]} */
    {
        cf_active_ventries e = {0};
        entries_put(&e, "format", mk_sym("png"));
        entries_put(&e, "resize_to_limit", mk_resize(512, 512));
        check_variation_row(&e, "ksXvpLsa7BuCVyHOwmQOKedbIbM=",
                            "eyJfcmFpbHMiOnsiZGF0YSI6eyJmb3JtYXQiOiJwbmciLCJyZXNpemVfdG9fbGltaXQiOls1MTIsNTEyXX0sInB1ciI6InZhcmlhdGlvbiJ9fQ==--112bd901bdf14f562e7c8aec3550e9fb70c7c0f8",
                            "1IHlyI9hGC89qU5BLFobIUWcNlY=", "png-512-sym");
        cf_active_ventries_dispose(&e);
    }
    /* {format: :png, resize_to_limit: [192, 192]} */
    {
        cf_active_ventries e = {0};
        entries_put(&e, "format", mk_sym("png"));
        entries_put(&e, "resize_to_limit", mk_resize(192, 192));
        check_variation_row(&e, "EXATZfM7OVVC86CDh85TOsa8ZSw=",
                            "eyJfcmFpbHMiOnsiZGF0YSI6eyJmb3JtYXQiOiJwbmciLCJyZXNpemVfdG9fbGltaXQiOlsxOTIsMTkyXX0sInB1ciI6InZhcmlhdGlvbiJ9fQ==--4598198319d89f0bc3abcd7daa7746e22411703b",
                            "mbzpjsoDLqwSpa2bWYg2RV9IXB8=", "png-192-sym");
        cf_active_ventries_dispose(&e);
    }
    /* {format: "jpg", resize_to_limit: [1200, 800]} (message thumb) */
    {
        cf_active_ventries e = {0};
        entries_put(&e, "format", mk_str("jpg"));
        entries_put(&e, "resize_to_limit", mk_resize(1200, 800));
        check_variation_row(&e, "IBhrLAIapu+NCId+2Kz6EqUWRKY=",
                            "eyJfcmFpbHMiOnsiZGF0YSI6eyJmb3JtYXQiOiJqcGciLCJyZXNpemVfdG9fbGltaXQiOlsxMjAwLDgwMF19LCJwdXIiOiJ2YXJpYXRpb24ifX0=--28426ca1e33b0fea71b8b10b7f52a844de5886cf",
                            "IBhrLAIapu+NCId+2Kz6EqUWRKY=", "thumb-str");
        cf_active_ventries_dispose(&e);
    }
    /* {format: "png", resize_to_limit: [1200, 800]} */
    {
        cf_active_ventries e = {0};
        entries_put(&e, "format", mk_str("png"));
        entries_put(&e, "resize_to_limit", mk_resize(1200, 800));
        check_variation_row(&e, "aFrmZ9PxOH5XKtnRmopvClCmS1g=",
                            "eyJfcmFpbHMiOnsiZGF0YSI6eyJmb3JtYXQiOiJwbmciLCJyZXNpemVfdG9fbGltaXQiOlsxMjAwLDgwMF19LCJwdXIiOiJ2YXJpYXRpb24ifX0=--d85268fba402efde8dbac959a2b2775d27e6b9af",
                            "aFrmZ9PxOH5XKtnRmopvClCmS1g=", "png-1200-str");
        cf_active_ventries_dispose(&e);
    }
    /* {format: "gif", resize_to_limit: [1200, 800]} */
    {
        cf_active_ventries e = {0};
        entries_put(&e, "format", mk_str("gif"));
        entries_put(&e, "resize_to_limit", mk_resize(1200, 800));
        check_variation_row(&e, "QhvxvYWo0o2axWU0l3EW8VQDYbM=",
                            "eyJfcmFpbHMiOnsiZGF0YSI6eyJmb3JtYXQiOiJnaWYiLCJyZXNpemVfdG9fbGltaXQiOlsxMjAwLDgwMF19LCJwdXIiOiJ2YXJpYXRpb24ifX0=--e91a330f8d562a89c9d04eda3f699d5911d66747",
                            "QhvxvYWo0o2axWU0l3EW8VQDYbM=", "gif-1200-str");
        cf_active_ventries_dispose(&e);
    }
    /* {format: :png, resize_to_limit: [1200, 800]}: same key as the string
     * row, different digest. */
    {
        cf_active_ventries e = {0};
        entries_put(&e, "format", mk_sym("png"));
        entries_put(&e, "resize_to_limit", mk_resize(1200, 800));
        check_variation_row(&e, "E5GdheK2KKwpmZTUH1L2aXvHokU=",
                            "eyJfcmFpbHMiOnsiZGF0YSI6eyJmb3JtYXQiOiJwbmciLCJyZXNpemVfdG9fbGltaXQiOlsxMjAwLDgwMF19LCJwdXIiOiJ2YXJpYXRpb24ifX0=--d85268fba402efde8dbac959a2b2775d27e6b9af",
                            "aFrmZ9PxOH5XKtnRmopvClCmS1g=", "png-1200-sym");
        cf_active_ventries_dispose(&e);
    }
    /* {format: :webp} (video preview) */
    {
        cf_active_ventries e = {0};
        entries_put(&e, "format", mk_sym("webp"));
        check_variation_row(&e, "IrCln/Mml8kDmpMni9j4/WNB7Uo=",
                            "eyJfcmFpbHMiOnsiZGF0YSI6eyJmb3JtYXQiOiJ3ZWJwIn0sInB1ciI6InZhcmlhdGlvbiJ9fQ==--b8634e00ce24592a11bf9c5caf4675d890fa5f45",
                            "xOmba/xbWDCWCW1OFccttgK9Cr4=", "preview-sym");
        cf_active_ventries_dispose(&e);
    }
    /* {format: :webp, resize_to_limit: [1200, 800]} */
    {
        cf_active_ventries e = {0};
        entries_put(&e, "format", mk_sym("webp"));
        entries_put(&e, "resize_to_limit", mk_resize(1200, 800));
        check_variation_row(&e, "WRV0qldsxKR5cZNFjQ/qrXUXKAs=",
                            "eyJfcmFpbHMiOnsiZGF0YSI6eyJmb3JtYXQiOiJ3ZWJwIiwicmVzaXplX3RvX2xpbWl0IjpbMTIwMCw4MDBdfSwicHVyIjoidmFyaWF0aW9uIn19--132e54230de6ddc9c41e0118730416f2e3d3127f",
                            "u0TSbUypNLiezYYkiqbZ8jAyVv0=", "webp-1200-sym");
        cf_active_ventries_dispose(&e);
    }
    /* {format: "webp", resize_to_limit: [1200, 800]}: same key, str digest. */
    {
        cf_active_ventries e = {0};
        entries_put(&e, "format", mk_str("webp"));
        entries_put(&e, "resize_to_limit", mk_resize(1200, 800));
        check_variation_row(&e, "u0TSbUypNLiezYYkiqbZ8jAyVv0=",
                            "eyJfcmFpbHMiOnsiZGF0YSI6eyJmb3JtYXQiOiJ3ZWJwIiwicmVzaXplX3RvX2xpbWl0IjpbMTIwMCw4MDBdfSwicHVyIjoidmFyaWF0aW9uIn19--132e54230de6ddc9c41e0118730416f2e3d3127f",
                            "u0TSbUypNLiezYYkiqbZ8jAyVv0=", "webp-1200-str");
        cf_active_ventries_dispose(&e);
    }
    /* Nested hash, nil, bool and negative int. */
    {
        cf_active_ventries e = {0};
        cf_active_vval *pair = NULL;
        CF_REQUIRE(cf_active_varr(&pair) == CF_OK);
        CF_REQUIRE(cf_active_varr_push(pair, mk_int(100)) == CF_OK);
        cf_active_vval *nil = NULL;
        CF_REQUIRE(cf_active_vnil(&nil) == CF_OK);
        CF_REQUIRE(cf_active_varr_push(pair, nil) == CF_OK);
        entries_put(&e, "resize_to_limit", pair);
        cf_active_vval *saver = NULL;
        CF_REQUIRE(cf_active_vhash(&saver) == CF_OK);
        CF_REQUIRE(cf_active_vhash_set(saver, S("quality"), mk_int(80)) == CF_OK);
        cf_active_vval *strip = NULL;
        CF_REQUIRE(cf_active_vbool(true, &strip) == CF_OK);
        CF_REQUIRE(cf_active_vhash_set(saver, S("strip"), strip) == CF_OK);
        entries_put(&e, "saver", saver);
        entries_put(&e, "rotate", mk_int(-90));
        entries_put(&e, "format", mk_str("jpeg"));
        check_variation_row(&e, "k87huoFjx1rwqYykr9U8ItY3+SY=",
                            "eyJfcmFpbHMiOnsiZGF0YSI6eyJyZXNpemVfdG9fbGltaXQiOlsxMDAsbnVsbF0sInNhdmVyIjp7InF1YWxpdHkiOjgwLCJzdHJpcCI6dHJ1ZX0sInJvdGF0ZSI6LTkwLCJmb3JtYXQiOiJqcGVnIn0sInB1ciI6InZhcmlhdGlvbiJ9fQ==--4ebf4320d92c445a51cf7c5267c3cd654b1375ec",
                            "k87huoFjx1rwqYykr9U8ItY3+SY=", "nested-str");
        cf_active_ventries_dispose(&e);
    }
    /* Bignum marshal integers (2^30 overflows the inline form). */
    {
        static const int64_t nums[] = {0, 1, 121, 122, 123, 255, 256, 65535,
                                       65536, 16777216, 1073741823, 1073741824,
                                       -1, -123, -124, -256, -257, -65536, -65537};
        cf_active_ventries e = {0};
        cf_active_vval *arr = NULL;
        CF_REQUIRE(cf_active_varr(&arr) == CF_OK);
        for (size_t i = 0; i < sizeof nums / sizeof nums[0]; i++) {
            CF_REQUIRE(cf_active_varr_push(arr, mk_int(nums[i])) == CF_OK);
        }
        entries_put(&e, "resize_to_limit", arr);
        entries_put(&e, "format", mk_sym("png"));
        check_variation_row(&e, "ux+iVOj7EuVut3cVa1xmBkI5U/k=",
                            "eyJfcmFpbHMiOnsiZGF0YSI6eyJyZXNpemVfdG9fbGltaXQiOlswLDEsMTIxLDEyMiwxMjMsMjU1LDI1Niw2NTUzNSw2NTUzNiwxNjc3NzIxNiwxMDczNzQxODIzLDEwNzM3NDE4MjQsLTEsLTEyMywtMTI0LC0yNTYsLTI1NywtNjU1MzYsLTY1NTM3XSwiZm9ybWF0IjoicG5nIn0sInB1ciI6InZhcmlhdGlvbiJ9fQ==--9263d44032a345a51611e1338de871e184fa77b4",
                            "ATYfiU5fLc9l4PxfYYVz/WB5qjs=", "bignum-sym");
        cf_active_ventries_dispose(&e);
    }
}

CF_TEST(variation_decode_rejects) {
    /* Wrong purpose is not found; garbage is not found. */
    const char *blob_id =
        "eyJfcmFpbHMiOnsiZGF0YSI6NDIsInB1ciI6ImJsb2JfaWQifX0=--c252dc1b14dd0e8cd599eb40203378880bafed1f";
    cf_active_ventries out = {0};
    bool found = true;
    CF_REQUIRE(cf_active_variation_verify(secret(), S(blob_id),
                                          NOW_2026_09_26_12_00, &out,
                                          &found) == CF_OK);
    CF_CHECK(!found);
    cf_active_ventries_dispose(&out);
    CF_REQUIRE(cf_active_variation_verify(secret(), S("garbage"),
                                          NOW_2026_09_26_12_00, &out,
                                          &found) == CF_OK);
    CF_CHECK(!found);
    cf_active_ventries_dispose(&out);
}

CF_TEST(variation_default_to) {
    /* variation_for shape: defaults first, overridden in place. */
    cf_active_ventries defaults = {0}, variation = {0}, merged = {0};
    entries_put(&defaults, "format", mk_str("png"));
    entries_put(&variation, "resize_to_limit", mk_resize(1200, 800));
    entries_put(&variation, "format", mk_str("jpg"));
    CF_REQUIRE(cf_active_variation_default(&defaults, &variation, &merged) == CF_OK);
    CF_REQUIRE(merged.len == 2);
    CF_CHECK(merged.items[0].key_len == 6 &&
             memcmp(merged.items[0].key, "format", 6) == 0);
    CF_CHECK(merged.items[0].value->kind == CF_ACTIVE_V_STR &&
             merged.items[0].value->len == 3);
    CF_CHECK(merged.items[1].key_len == 15);
    cf_active_ventries_dispose(&merged);
    cf_active_ventries_dispose(&defaults);
    cf_active_ventries_dispose(&variation);
}

/* ---- filenames, dispositions, escaping --------------------------------------- */

static size_t unhex(const char *hex, unsigned char *out, size_t cap) {
    size_t n = strlen(hex) / 2;
    CF_REQUIRE(n <= cap);
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        CF_REQUIRE(sscanf(hex + 2 * i, "%2x", &v) == 1);
        out[i] = (unsigned char)v;
    }
    return n;
}

typedef struct {
    const char *input_hex;
    const char *sanitized;
    const char *inline_disp;
    const char *attachment_disp;
    const char *escaped_path;
} filename_row;

static const filename_row FILENAME_ROWS[] = {
    {"6d6f6f6e2e6a7067", "moon.jpg", "inline; filename=\"moon.jpg\"; filename*=UTF-8''moon.jpg",
     "attachment; filename=\"moon.jpg\"; filename*=UTF-8''moon.jpg", "moon.jpg"},
    {"2020737061636564206e616d65202e706e6720", "spaced name .png",
     "inline; filename=\"spaced name .png\"; filename*=UTF-8''spaced%20name%20.png",
     "attachment; filename=\"spaced name .png\"; filename*=UTF-8''spaced%20name%20.png",
     "spaced%20name%20.png"},
    {"612f625c633a643b657c66256724683c693e6a3f6b2a6c226d096e0d6f0a2e747874",
     "a-b-c-d-e-f-g-h-i-j-k-l-m-n-o-.txt",
     "inline; filename=\"a-b-c-d-e-f-g-h-i-j-k-l-m-n-o-.txt\"; filename*=UTF-8''a-b-c-d-e-f-g-h-i-j-k-l-m-n-o-.txt",
     "attachment; filename=\"a-b-c-d-e-f-g-h-i-j-k-l-m-n-o-.txt\"; filename*=UTF-8''a-b-c-d-e-f-g-h-i-j-k-l-m-n-o-.txt",
     "a-b-c-d-e-f-g-h-i-j-k-l-m-n-o-.txt"},
    {"e280ae6576696c2e657865", "-evil.exe", "inline; filename=\"-evil.exe\"; filename*=UTF-8''-evil.exe",
     "attachment; filename=\"-evil.exe\"; filename*=UTF-8''-evil.exe", "-evil.exe"},
    {"72c3a973756dc3a92e706466", "r\xc3\xa9sum\xc3\xa9.pdf",
     "inline; filename=\"resume.pdf\"; filename*=UTF-8''r%C3%A9sum%C3%A9.pdf",
     "attachment; filename=\"resume.pdf\"; filename*=UTF-8''r%C3%A9sum%C3%A9.pdf",
     "r%C3%A9sum%C3%A9.pdf"},
    {"2e626173687263", ".bashrc", "inline; filename=\".bashrc\"; filename*=UTF-8''.bashrc",
     "attachment; filename=\".bashrc\"; filename*=UTF-8''.bashrc", ".bashrc"},
    {"617263686976652e7461722e677a", "archive.tar.gz",
     "inline; filename=\"archive.tar.gz\"; filename*=UTF-8''archive.tar.gz",
     "attachment; filename=\"archive.tar.gz\"; filename*=UTF-8''archive.tar.gz", "archive.tar.gz"},
    {"6e6f657874", "noext", "inline; filename=\"noext\"; filename*=UTF-8''noext",
     "attachment; filename=\"noext\"; filename*=UTF-8''noext", "noext"},
    {"747261696c696e672e", "trailing.", "inline; filename=\"trailing.\"; filename*=UTF-8''trailing.",
     "attachment; filename=\"trailing.\"; filename*=UTF-8''trailing.", "trailing."},
    {"2e2e", "..", "inline; filename=\"..\"; filename*=UTF-8''..",
     "attachment; filename=\"..\"; filename*=UTF-8''..", ".."},
    {"e697a5e69cace8aa9e2e706e67", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.png",
     "inline; filename=\"%3F%3F%3F.png\"; filename*=UTF-8''%E6%97%A5%E6%9C%AC%E8%AA%9E.png",
     "attachment; filename=\"%3F%3F%3F.png\"; filename*=UTF-8''%E6%97%A5%E6%9C%AC%E8%AA%9E.png",
     "%E6%97%A5%E6%9C%AC%E8%AA%9E.png"},
    {"656d6f6a6920f09f98802e676966", "emoji \xf0\x9f\x98\x80.gif",
     "inline; filename=\"emoji %3F.gif\"; filename*=UTF-8''emoji%20%F0%9F%98%80.gif",
     "attachment; filename=\"emoji %3F.gif\"; filename*=UTF-8''emoji%20%F0%9F%98%80.gif",
     "emoji%20%F0%9F%98%80.gif"},
    {"cea96d65676120c39f2e747874", "\xce\xa9mega \xc3\x9f.txt",
     "inline; filename=\"%3Fmega ss.txt\"; filename*=UTF-8''%CE%A9mega%20%C3%9F.txt",
     "attachment; filename=\"%3Fmega ss.txt\"; filename*=UTF-8''%CE%A9mega%20%C3%9F.txt",
     "%CE%A9mega%20%C3%9F.txt"},
    {"776974682771756f746520262028706172656e73292023312b7e2e747874",
     "with'quote & (parens) #1+~.txt",
     "inline; filename=\"with%27quote %26 %28parens%29 #1+~.txt\"; filename*=UTF-8''with%27quote%20&%20%28parens%29%20#1+~.txt",
     "attachment; filename=\"with%27quote %26 %28parens%29 #1+~.txt\"; filename*=UTF-8''with%27quote%20&%20%28parens%29%20#1+~.txt",
     "with'quote%20&%20(parens)%20%231+~.txt"},
    {"c3867468657220c593757672652e6a7067", "\xc3\x86ther \xc5\x93uvre.jpg",
     "inline; filename=\"AEther oeuvre.jpg\"; filename*=UTF-8''%C3%86ther%20%C5%93uvre.jpg",
     "attachment; filename=\"AEther oeuvre.jpg\"; filename*=UTF-8''%C3%86ther%20%C5%93uvre.jpg",
     "%C3%86ther%20%C5%93uvre.jpg"},
    {"626164ff627974652e6a7067", "bad\xef\xbf\xbd" "byte.jpg",
     "inline; filename=\"bad%3Fbyte.jpg\"; filename*=UTF-8''bad%EF%BF%BDbyte.jpg",
     "attachment; filename=\"bad%3Fbyte.jpg\"; filename*=UTF-8''bad%EF%BF%BDbyte.jpg",
     "bad%EF%BF%BDbyte.jpg"},
};

CF_TEST(filenames_and_dispositions_match_vectors) {
    for (size_t i = 0; i < sizeof FILENAME_ROWS / sizeof FILENAME_ROWS[0]; i++) {
        const filename_row *row = &FILENAME_ROWS[i];
        unsigned char input[256];
        size_t n = unhex(row->input_hex, input, sizeof input);
        cf_builder sanitized = {0};
        CF_REQUIRE(cf_active_filename_sanitize((cf_span){input, n}, &sanitized) == CF_OK);
        if (!builder_is(&sanitized, row->sanitized)) {
            printf("    row %zu sanitize: got %.*s want %s\n", i, (int)sanitized.len,
                   sanitized.ptr, row->sanitized);
            CF_CHECK(false);
        }
        cf_span san = {sanitized.ptr, sanitized.len};
        cf_builder inline_disp = {0}, attach_disp = {0}, escaped = {0};
        CF_REQUIRE(cf_active_content_disposition(S("inline"), san, &inline_disp) == CF_OK);
        CF_REQUIRE(cf_active_content_disposition(S("attachment"), san, &attach_disp) == CF_OK);
        CF_REQUIRE(cf_active_escape_path(san, &escaped) == CF_OK);
        if (!builder_is(&inline_disp, row->inline_disp)) {
            printf("    row %zu inline: got %.*s\n                 want %s\n", i,
                   (int)inline_disp.len, inline_disp.ptr, row->inline_disp);
            CF_CHECK(false);
        }
        if (!builder_is(&attach_disp, row->attachment_disp)) {
            printf("    row %zu attach: got %.*s\n                 want %s\n", i,
                   (int)attach_disp.len, attach_disp.ptr, row->attachment_disp);
            CF_CHECK(false);
        }
        if (!builder_is(&escaped, row->escaped_path)) {
            printf("    row %zu escape: got %.*s want %s\n", i, (int)escaped.len,
                   escaped.ptr, row->escaped_path);
            CF_CHECK(false);
        }
        /* A non-attachment disposition serves inline. */
        cf_builder dl = {0};
        CF_REQUIRE(cf_active_content_disposition(S("download"), san, &dl) == CF_OK);
        CF_CHECK(builder_is(&dl, row->inline_disp));
        cf_builder_dispose(&sanitized);
        cf_builder_dispose(&inline_disp);
        cf_builder_dispose(&attach_disp);
        cf_builder_dispose(&escaped);
        cf_builder_dispose(&dl);
    }
}

CF_TEST(content_type_tables) {
    CF_CHECK(cf_active_content_type_variable(S("image/png")));
    CF_CHECK(cf_active_content_type_variable(S("image/avif")));
    CF_CHECK(!cf_active_content_type_variable(S("image/bmp")));
    CF_CHECK(!cf_active_content_type_variable(S("video/mp4")));
    CF_CHECK(cf_active_content_type_web_image(S("image/webp")));
    CF_CHECK(!cf_active_content_type_web_image(S("image/avif")));
    CF_CHECK(cf_active_content_type_allowed_inline(S("application/pdf")));
    CF_CHECK(!cf_active_content_type_allowed_inline(S("text/html")));
    CF_CHECK(cf_active_content_type_serve_as_binary(S("text/html")));
    CF_CHECK(cf_active_content_type_serve_as_binary(S("image/svg+xml")));
    CF_CHECK(!cf_active_content_type_serve_as_binary(S("image/png")));
    CF_CHECK(span_eq(cf_active_content_type_for_serving(S("text/html")),
                        S("application/octet-stream")));
    CF_CHECK(span_eq(cf_active_content_type_for_serving(S("image/png")),
                        S("image/png")));
    CF_CHECK(cf_active_forced_disposition(S("text/html")) != NULL &&
             strcmp(cf_active_forced_disposition(S("text/html")), "attachment") == 0);
    CF_CHECK(cf_active_forced_disposition(S("application/pdf")) == NULL);
    CF_CHECK(cf_active_forced_disposition(S("video/mp4")) != NULL);
}

/* ---- byte ranges: RACK_RANGES rows ------------------------------------------ */

typedef struct {
    const char *header; /* NULL = absent */
    uint64_t size;
    cf_active_range_outcome outcome;
    cf_active_range ranges[4];
    size_t n;
} range_row;

static const range_row RANGE_ROWS[] = {
    {"bytes=0-4", 10, CF_ACTIVE_RANGE_PART, {{0, 4}}, 1},
    {"bytes=5-", 10, CF_ACTIVE_RANGE_PART, {{5, 9}}, 1},
    {"bytes=-3", 10, CF_ACTIVE_RANGE_PART, {{7, 9}}, 1},
    {"bytes=-30", 10, CF_ACTIVE_RANGE_PART, {{0, 9}}, 1},
    {"bytes=0-99999999999999999999", 10, CF_ACTIVE_RANGE_PART, {{0, 9}}, 1},
    {"bytes=99999999999999999999-", 10, CF_ACTIVE_RANGE_UNSAT, {{0, 0}}, 0},
    {"bytes=-99999999999999999999", 10, CF_ACTIVE_RANGE_PART, {{0, 9}}, 1},
    {"bytes=0-1,", 10, CF_ACTIVE_RANGE_PART, {{0, 1}}, 1},
    {"bytes= -5", 10, CF_ACTIVE_RANGE_PART, {{0, 5}}, 1},
    {"bytes=0-1, 3-4", 10, CF_ACTIVE_RANGE_PART, {{0, 1}, {3, 4}}, 2},
    {"bytes=0-1,\t 3-4", 10, CF_ACTIVE_RANGE_PART, {{0, 1}, {3, 4}}, 2},
    {"bytes=3-5,1-2", 10, CF_ACTIVE_RANGE_PART, {{3, 5}, {1, 2}}, 2},
    {"bytes=+1-2", 10, CF_ACTIVE_RANGE_PART, {{1, 2}}, 1},
    {"bytes=1-+2", 10, CF_ACTIVE_RANGE_PART, {{1, 2}}, 1},
    {"bytes=1_0-2_0", 100, CF_ACTIVE_RANGE_PART, {{10, 20}}, 1},
    {"bytes=0d5-0d9", 100, CF_ACTIVE_RANGE_PART, {{5, 9}}, 1},
    {"bytes=\xc2\xa0" "1-2", 10, CF_ACTIVE_RANGE_PART, {{0, 2}}, 1},
    {"bytes=a-b", 10, CF_ACTIVE_RANGE_PART, {{0, 0}}, 1},
    {"bytes=0-0x5", 10, CF_ACTIVE_RANGE_PART, {{0, 0}}, 1},
    {"bytes=0-1 ", 10, CF_ACTIVE_RANGE_PART, {{0, 1}}, 1},
    {"bytes=0 -1", 10, CF_ACTIVE_RANGE_PART, {{0, 1}}, 1},
    {"bytes=1-2-3", 10, CF_ACTIVE_RANGE_PART, {{1, 2}}, 1},
    {"bytes=9-9", 10, CF_ACTIVE_RANGE_PART, {{9, 9}}, 1},
    {"bytes=10-", 10, CF_ACTIVE_RANGE_UNSAT, {{0, 0}}, 0},
    {"bytes=10-12", 10, CF_ACTIVE_RANGE_UNSAT, {{0, 0}}, 0},
    {"bytes=-0", 10, CF_ACTIVE_RANGE_UNSAT, {{0, 0}}, 0},
    {"bytes=--5", 10, CF_ACTIVE_RANGE_UNSAT, {{0, 0}}, 0},
    {"bytes=0-4,5-9,0-0", 10, CF_ACTIVE_RANGE_UNSAT, {{0, 0}}, 0},
    {"bytes=;bytes=2-3", 10, CF_ACTIVE_RANGE_PART, {{2, 3}}, 1},
    {"bytes=0-1;bytes=2-3", 10, CF_ACTIVE_RANGE_PART, {{0, 1}}, 1},
    {"xbytes=0-1", 10, CF_ACTIVE_RANGE_PART, {{0, 1}}, 1},
    {"bytes=;0-1", 10, CF_ACTIVE_RANGE_INVALID, {{0, 0}}, 0},
    {"bytes=", 10, CF_ACTIVE_RANGE_INVALID, {{0, 0}}, 0},
    {"bytes=-", 10, CF_ACTIVE_RANGE_INVALID, {{0, 0}}, 0},
    {"bytes=5", 10, CF_ACTIVE_RANGE_INVALID, {{0, 0}}, 0},
    {"bytes=1-0", 10, CF_ACTIVE_RANGE_INVALID, {{0, 0}}, 0},
    {"bytes=0-1,,2-3", 10, CF_ACTIVE_RANGE_INVALID, {{0, 0}}, 0},
    {"items=0-1", 10, CF_ACTIVE_RANGE_INVALID, {{0, 0}}, 0},
    {"bytes=0-4", 0, CF_ACTIVE_RANGE_FULL, {{0, 0}}, 0},
    {NULL, 10, CF_ACTIVE_RANGE_FULL, {{0, 0}}, 0},
    {"", 10, CF_ACTIVE_RANGE_FULL, {{0, 0}}, 0},
    {"   ", 10, CF_ACTIVE_RANGE_FULL, {{0, 0}}, 0},
};

CF_TEST(byte_ranges_match_rack) {
    for (size_t i = 0; i < sizeof RANGE_ROWS / sizeof RANGE_ROWS[0]; i++) {
        const range_row *row = &RANGE_ROWS[i];
        cf_active_range *ranges = NULL;
        size_t n = 0;
        cf_active_range_outcome outcome = CF_ACTIVE_RANGE_FULL;
        cf_span header = row->header == NULL ? (cf_span){NULL, 0} : S(row->header);
        CF_REQUIRE(cf_active_range_parse(header, row->header != NULL, row->size,
                                         &ranges, &n, &outcome) == CF_OK);
        if (outcome != row->outcome || n != row->n) {
            printf("    row %zu %s/%llu: outcome %d/%zu, want %d/%zu\n", i,
                   row->header == NULL ? "(absent)" : row->header,
                   (unsigned long long)row->size, (int)outcome, n,
                   (int)row->outcome, row->n);
            CF_CHECK(false);
        }
        for (size_t k = 0; k < n && k < row->n; k++) {
            if (ranges[k].start != row->ranges[k].start ||
                ranges[k].end != row->ranges[k].end) {
                printf("    row %zu range %zu: got %llu-%llu want %llu-%llu\n",
                       i, k, (unsigned long long)ranges[k].start,
                       (unsigned long long)ranges[k].end,
                       (unsigned long long)row->ranges[k].start,
                       (unsigned long long)row->ranges[k].end);
                CF_CHECK(false);
            }
        }
        cf_active_ranges_dispose(ranges);
    }
    /* 99 commas parse (unsatisfiable here), 100 do not. */
    {
        char big[16 + 100 * 4 + 1];
        strcpy(big, "bytes=0-1,");
        for (int i = 0; i < 98; i++) strcat(big, "0-0,");
        cf_active_range *ranges = (cf_active_range *)0x1;
        size_t n = 0;
        cf_active_range_outcome outcome = CF_ACTIVE_RANGE_FULL;
        CF_REQUIRE(cf_active_range_parse(S(big), true, 10, &ranges, &n,
                                         &outcome) == CF_OK);
        CF_CHECK(outcome == CF_ACTIVE_RANGE_UNSAT && ranges == NULL);
        strcat(big, "0-0,");
        ranges = (cf_active_range *)0x1;
        CF_REQUIRE(cf_active_range_parse(S(big), true, 10, &ranges, &n,
                                         &outcome) == CF_OK);
        CF_CHECK(outcome == CF_ACTIVE_RANGE_INVALID && ranges == NULL);
    }
}

/* ---- proxy serve planning -------------------------------------------------- */

static void check_plan_str(cf_str *field, const char *want, const char *label) {
    if (!str_is(*field, want)) {
        printf("    %s: got %s want %s\n", label,
               field->ptr == NULL ? "(null)" : field->ptr, want);
        CF_CHECK(false);
    }
}

CF_TEST(proxy_serve_matrix) {
    /* Missing file is 404. */
    {
        cf_active_serve plan = {0};
        CF_REQUIRE(cf_active_proxy_serve(false, false, S(""), 100, S("image/png"),
                                         S("moon.jpg"), S(""), false, S(""),
                                         false, &plan) == CF_OK);
        CF_CHECK(plan.status == 404 && !plan.has_body);
        cf_active_serve_dispose(&plan);
    }
    /* Full 200 with Accept-Ranges and inline disposition. */
    {
        cf_active_serve plan = {0};
        CF_REQUIRE(cf_active_proxy_serve(true, false, S(""), 100, S("image/png"),
                                         S("moon.jpg"), S(""), false, S(""),
                                         false, &plan) == CF_OK);
        CF_CHECK(plan.status == 200);
        CF_CHECK(plan.content_length == 100 && plan.has_body);
        CF_CHECK(plan.accept_ranges && !plan.has_single);
        check_plan_str(&plan.content_type, "image/png", "proxy ct");
        check_plan_str(&plan.content_disposition,
                       "inline; filename=\"moon.jpg\"; filename*=UTF-8''moon.jpg",
                       "proxy disp");
        CF_CHECK(plan.content_range.ptr == NULL);
        cf_active_serve_dispose(&plan);
    }
    /* Forced attachment + binary content type. */
    {
        cf_active_serve plan = {0};
        CF_REQUIRE(cf_active_proxy_serve(true, false, S(""), 10, S("text/html"),
                                         S("x.html"), S("inline"), true, S(""),
                                         false, &plan) == CF_OK);
        CF_CHECK(plan.status == 200);
        check_plan_str(&plan.content_type, "application/octet-stream", "forced ct");
        check_plan_str(&plan.content_disposition,
                       "attachment; filename=\"x.html\"; filename*=UTF-8''x.html",
                       "forced disp");
        cf_active_serve_dispose(&plan);
    }
    /* Single range 206. */
    {
        cf_active_serve plan = {0};
        CF_REQUIRE(cf_active_proxy_serve(true, true, S("bytes=10-19"), 100,
                                         S("image/png"), S("moon.jpg"), S(""),
                                         false, S(""), false, &plan) == CF_OK);
        CF_CHECK(plan.status == 206);
        CF_CHECK(plan.has_single && plan.single.start == 10 && plan.single.end == 19);
        CF_CHECK(plan.content_length == 10 && plan.has_body);
        check_plan_str(&plan.content_range, "bytes 10-19/100", "range");
        CF_CHECK(plan.accept_ranges);
        cf_active_serve_dispose(&plan);
    }
    /* Unsatisfiable and malformed ranges are 416 on the proxy. */
    {
        cf_active_serve plan = {0};
        CF_REQUIRE(cf_active_proxy_serve(true, true, S("bytes=200-300"), 100,
                                         S("image/png"), S("a.png"), S(""), false,
                                         S(""), false, &plan) == CF_OK);
        CF_CHECK(plan.status == 416 && !plan.has_body);
        cf_active_serve_dispose(&plan);
        CF_REQUIRE(cf_active_proxy_serve(true, true, S("items=0-1"), 100,
                                         S("image/png"), S("a.png"), S(""), false,
                                         S(""), false, &plan) == CF_OK);
        CF_CHECK(plan.status == 416 && !plan.has_body);
        cf_active_serve_dispose(&plan);
    }
    /* Multipart 206: fixed test boundary, exact framing and length. */
    {
        cf_active_serve plan = {0};
        const char *boundary = "0123456789abcdef0123456789abcdef";
        CF_REQUIRE(cf_active_proxy_serve(true, true, S("bytes=0-1, 3-4"), 10,
                                         S("image/png"), S("a.png"), S(""), false,
                                         S(boundary), true, &plan) == CF_OK);
        CF_REQUIRE(plan.status == 206);
        CF_REQUIRE(plan.n_ranges == 2 && plan.ranges != NULL);
        check_plan_str(&plan.content_type,
                       "multipart/byteranges; boundary=0123456789abcdef0123456789abcdef",
                       "multi ct");
        CF_CHECK(plan.content_range.ptr == NULL);
        cf_builder body = {0};
        for (size_t i = 0; i < plan.n_ranges; i++) {
            CF_REQUIRE(cf_active_proxy_part_heading(S(boundary), S("image/png"),
                                                    plan.ranges[i].start,
                                                    plan.ranges[i].end, 10,
                                                    &body) == CF_OK);
            /* File bytes would stream here; count the slice length. */
            for (uint64_t k = plan.ranges[i].start; k <= plan.ranges[i].end; k++) {
                unsigned char dot = '.';
                CF_REQUIRE(cf_builder_append(&body, (cf_span){&dot, 1}) == CF_OK);
            }
        }
        CF_REQUIRE(cf_active_proxy_part_trailer(S(boundary), &body) == CF_OK);
        CF_CHECK(body.len == (size_t)plan.content_length);
        const char *want_head =
            "\r\n--0123456789abcdef0123456789abcdef\r\nContent-Type: image/png\r\n"
            "Content-Range: bytes 0-1/10\r\n\r\n";
        CF_CHECK(body.len >= strlen(want_head) &&
                 memcmp(body.ptr, want_head, strlen(want_head)) == 0);
        const char *want_tail = "\r\n--0123456789abcdef0123456789abcdef--\r\n";
        CF_CHECK(body.len >= strlen(want_tail) &&
                 memcmp(body.ptr + body.len - strlen(want_tail), want_tail,
                        strlen(want_tail)) == 0);
        cf_builder_dispose(&body);
        cf_active_serve_dispose(&plan);
    }
    /* Empty file serves 200 with no body. */
    {
        cf_active_serve plan = {0};
        CF_REQUIRE(cf_active_proxy_serve(true, false, S(""), 0, S("image/png"),
                                         S("e.png"), S(""), false, S(""), false,
                                         &plan) == CF_OK);
        CF_CHECK(plan.status == 200 && !plan.has_body && plan.content_length == 0);
        cf_active_serve_dispose(&plan);
    }
}

/* ---- disk serve planning ---------------------------------------------------- */

CF_TEST(disk_serve_matrix) {
    char mtime[30];
    CF_REQUIRE(cf_active_httpdate(784111777, mtime) == CF_OK);
    CF_CHECK(strcmp(mtime, "Sun, 06 Nov 1994 08:49:37 GMT") == 0);
    CF_REQUIRE(cf_active_httpdate(0, mtime) == CF_OK);
    CF_CHECK(strcmp(mtime, "Thu, 01 Jan 1970 00:00:00 GMT") == 0);
    /* OPTIONS. */
    {
        cf_active_serve plan = {0};
        CF_REQUIRE(cf_active_disk_serve(S("OPTIONS"), false, S(""), false, S(""),
                                        true, 10, S(mtime), S(""), S(""),
                                        &plan) == CF_OK);
        CF_CHECK(plan.status == 200 && !plan.has_body);
        check_plan_str(&plan.allow, "GET, HEAD, OPTIONS", "allow");
        cf_active_serve_dispose(&plan);
    }
    /* Conditional GET hit. */
    {
        cf_active_serve plan = {0};
        CF_REQUIRE(cf_active_disk_serve(S("GET"), false, S(""), true, S(mtime),
                                        true, 10, S(mtime), S("image/png"),
                                        S("inline"), &plan) == CF_OK);
        CF_CHECK(plan.status == 304 && !plan.has_body);
        check_plan_str(&plan.content_type, "image/png", "304 ct");
        check_plan_str(&plan.content_disposition, "inline", "304 disp");
        check_plan_str(&plan.cache_control, "max-age=3600, public", "304 cc");
        CF_CHECK(plan.last_modified.ptr == NULL);
        cf_active_serve_dispose(&plan);
    }
    /* Plain 200 with cache headers and defaults. */
    {
        cf_active_serve plan = {0};
        CF_REQUIRE(cf_active_disk_serve(S("GET"), false, S(""), true,
                                        S("Tue, 01 Jan 1980 00:00:00 GMT"), true,
                                        10, S(mtime), S(""), S(""), &plan) == CF_OK);
        CF_CHECK(plan.status == 200 && plan.has_body && plan.content_length == 10);
        check_plan_str(&plan.content_type, "application/octet-stream", "disk ct");
        check_plan_str(&plan.content_disposition, "attachment", "disk disp");
        check_plan_str(&plan.last_modified, mtime, "disk lm");
        check_plan_str(&plan.cache_control, "max-age=3600, public", "disk cc");
        CF_CHECK(!plan.accept_ranges);
        cf_active_serve_dispose(&plan);
    }
    /* HEAD has headers but no body. */
    {
        cf_active_serve plan = {0};
        CF_REQUIRE(cf_active_disk_serve(S("HEAD"), false, S(""), false, S(""),
                                        true, 10, S(mtime), S("text/plain"),
                                        S("inline"), &plan) == CF_OK);
        CF_CHECK(plan.status == 200 && !plan.has_body && plan.content_length == 10);
        cf_active_serve_dispose(&plan);
    }
    /* Single range 206; malformed ranges fall back to 200 here. */
    {
        cf_active_serve plan = {0};
        CF_REQUIRE(cf_active_disk_serve(S("GET"), true, S("bytes=2-5"), false,
                                        S(""), true, 10, S(mtime),
                                        S("text/plain"), S("inline"),
                                        &plan) == CF_OK);
        CF_CHECK(plan.status == 206 && plan.has_single);
        CF_CHECK(plan.single.start == 2 && plan.single.end == 5);
        check_plan_str(&plan.content_range, "bytes 2-5/10", "disk range");
        cf_active_serve_dispose(&plan);
        CF_REQUIRE(cf_active_disk_serve(S("GET"), true, S("items=0-1"), false,
                                        S(""), true, 10, S(mtime),
                                        S("text/plain"), S("inline"),
                                        &plan) == CF_OK);
        CF_CHECK(plan.status == 200 && !plan.has_single);
        cf_active_serve_dispose(&plan);
    }
    /* Unsatisfiable: 416 keeps the stamped content headers (serve_file
     * overwrites the file server's text/plain) plus the range suffix. */
    {
        cf_active_serve plan = {0};
        CF_REQUIRE(cf_active_disk_serve(S("GET"), true, S("bytes=10-"), false,
                                        S(""), true, 10, S(mtime),
                                        S("image/png"), S("inline"),
                                        &plan) == CF_OK);
        CF_CHECK(plan.status == 416);
        check_plan_str(&plan.content_type, "image/png", "416 ct");
        check_plan_str(&plan.content_disposition, "inline", "416 disp");
        check_plan_str(&plan.content_range, "bytes */10", "416 range");
        check_plan_str(&plan.cache_control, "max-age=3600, public", "416 cc");
        CF_CHECK(plan.last_modified.ptr == NULL);
        CF_CHECK(plan.content_length == strlen(CF_ACTIVE_UNSATISFIABLE_MESSAGE));
        cf_active_serve_dispose(&plan);
    }
    /* Multipart disk 206 uses the fixed boundary and lowercase part names. */
    {
        cf_active_serve plan = {0};
        CF_REQUIRE(cf_active_disk_serve(S("GET"), true, S("bytes=0-1, 3-4"),
                                        false, S(""), true, 10, S(mtime),
                                        S("text/plain"), S("inline"),
                                        &plan) == CF_OK);
        CF_REQUIRE(plan.status == 206 && plan.n_ranges == 2);
        check_plan_str(&plan.content_type,
                       "multipart/byteranges; boundary=AaB03x", "disk multi");
        check_plan_str(&plan.boundary, "AaB03x", "disk boundary");
        cf_builder body = {0};
        for (size_t i = 0; i < plan.n_ranges; i++) {
            CF_REQUIRE(cf_active_disk_part_heading(plan.ranges[i].start,
                                                   plan.ranges[i].end, 10,
                                                   &body) == CF_OK);
            for (uint64_t k = plan.ranges[i].start; k <= plan.ranges[i].end; k++) {
                unsigned char dot = '.';
                CF_REQUIRE(cf_builder_append(&body, (cf_span){&dot, 1}) == CF_OK);
            }
        }
        CF_REQUIRE(cf_active_disk_part_trailer(&body) == CF_OK);
        CF_CHECK(body.len == (size_t)plan.content_length);
        const char *want_head =
            "\r\n--AaB03x\r\ncontent-type: text/plain\r\ncontent-range: bytes 0-1/10\r\n\r\n";
        CF_CHECK(body.len >= strlen(want_head) &&
                 memcmp(body.ptr, want_head, strlen(want_head)) == 0);
        cf_builder_dispose(&body);
        cf_active_serve_dispose(&plan);
    }
    /* Missing file is 404. */
    {
        cf_active_serve plan = {0};
        CF_REQUIRE(cf_active_disk_serve(S("GET"), false, S(""), false, S(""),
                                        false, 0, S(mtime), S(""), S(""),
                                        &plan) == CF_OK);
        CF_CHECK(plan.status == 404);
        cf_active_serve_dispose(&plan);
    }
}

/* ---- redirect and service URLs: storage.json vectors -------------------------- */

CF_TEST(redirect_and_service_urls_match_vectors) {
    /* messages[0] moon.jpg blob: key 1ykrknnn9r4youv0f71wrntkzilx. */
    const char *signed_id =
        "eyJfcmFpbHMiOnsiZGF0YSI6MSwicHVyIjoiYmxvYl9pZCJ9fQ==--c71a3525fa5389ac29b174e9ac015974d7de3cec";
    int64_t id = 0;
    bool found = false;
    CF_REQUIRE(cf_active_blob_verify(secret(), S(signed_id),
                                     NOW_2026_09_26_12_00, &id, &found) == CF_OK);
    CF_CHECK(found && id == 1);
    /* rails_blob_path / rails_blob_download_path shapes. */
    cf_builder path = {0};
    CF_REQUIRE(cf_active_blob_redirect_path(secret(), 1, S("moon.jpg"), S(""),
                                            false, &path) == CF_OK);
    if (!builder_is(&path,
                    "/rails/active_storage/blobs/redirect/eyJfcmFpbHMiOnsiZGF0YSI6MSwicHVyIjoiYmxvYl9pZCJ9fQ==--c71a3525fa5389ac29b174e9ac015974d7de3cec/moon.jpg")) {
        printf("    redirect: got %.*s\n", (int)path.len, path.ptr);
        CF_CHECK(false);
    }
    cf_builder_dispose(&path);
    CF_REQUIRE(cf_active_blob_redirect_path(secret(), 1, S("moon.jpg"),
                                            S("attachment"), true, &path) == CF_OK);
    if (!builder_is(&path,
                    "/rails/active_storage/blobs/redirect/eyJfcmFpbHMiOnsiZGF0YSI6MSwicHVyIjoiYmxvYl9pZCJ9fQ==--c71a3525fa5389ac29b174e9ac015974d7de3cec/moon.jpg?disposition=attachment")) {
        printf("    download: got %.*s\n", (int)path.len, path.ptr);
        CF_CHECK(false);
    }
    cf_builder_dispose(&path);
    /* service_url / service_url_attachment: no-expiry disk paths. */
    {
        cf_builder url = {0};
        CF_REQUIRE(cf_active_content_disposition(S("inline"), S("moon.jpg"),
                                                 &url) == CF_OK);
        cf_builder_dispose(&url);
        cf_builder disp = {0};
        CF_REQUIRE(cf_active_content_disposition(S("inline"), S("moon.jpg"),
                                                 &disp) == CF_OK);
        /* Recreate blob.url(disposition:) with a hand-signed unexpiring key
         * payload and compare against the vector's disk path. */
        cf_str encoded = {0};
        CF_REQUIRE(cf_active_disk_key_sign(
                       secret(), S("1ykrknnn9r4youv0f71wrntkzilx"),
                       (cf_span){disp.ptr, disp.len}, S("image/jpeg"), true,
                       S("local"), false, 0, &encoded) == CF_OK);
        cf_builder full = {0};
        CF_REQUIRE(cf_builder_append(&full, S("/rails/active_storage/disk/")) == CF_OK);
        CF_REQUIRE(cf_active_escape_segment(
                       (cf_span){(unsigned char *)encoded.ptr, encoded.len},
                       &full) == CF_OK);
        CF_REQUIRE(cf_builder_append(&full, S("/moon.jpg")) == CF_OK);
        if (!builder_is(&full,
                        "/rails/active_storage/disk/eyJfcmFpbHMiOnsiZGF0YSI6eyJrZXkiOiIxeWtya25ubjlyNHlvdXYwZjcxd3JudGt6aWx4IiwiZGlzcG9zaXRpb24iOiJpbmxpbmU7IGZpbGVuYW1lPVwibW9vbi5qcGdcIjsgZmlsZW5hbWUqPVVURi04Jydtb29uLmpwZyIsImNvbnRlbnRfdHlwZSI6ImltYWdlL2pwZWciLCJzZXJ2aWNlX25hbWUiOiJsb2NhbCJ9LCJwdXIiOiJibG9iX2tleSJ9fQ==--af0dcb0fd8a245c6d4cc0f432a18b7f1fc002647/moon.jpg")) {
            printf("    service: got %.*s\n", (int)full.len, full.ptr);
            CF_CHECK(false);
        }
        cf_builder_dispose(&full);
        cf_str_dispose(&encoded);
        cf_builder_dispose(&disp);
    }
    /* service_url_path helper with expiry (redirect Location building). */
    {
        cf_builder url = {0};
        CF_REQUIRE(cf_active_service_url_path(
                       secret(), S("1ykrknnn9r4youv0f71wrntkzilx"),
                       VEC_EXPIRY_12_05, S("moon.jpg"), S("image/jpeg"), true,
                       S("inline"), S("local"), &url) == CF_OK);
        CF_CHECK(url.len > sizeof("/rails/active_storage/disk/") &&
                 memcmp(url.ptr, "/rails/active_storage/disk/",
                        sizeof("/rails/active_storage/disk/") - 1) == 0);
        CF_CHECK(url.len > 9 && memcmp(url.ptr + url.len - 9, "/moon.jpg", 9) == 0);
        /* The Location verifies under blob_key before expiry. */
        const char *prefix = "/rails/active_storage/disk/";
        size_t pre = strlen(prefix);
        CF_REQUIRE(url.len > pre + 10);
        size_t slash = url.len;
        while (slash > pre && url.ptr[slash - 1] != '/') slash--;
        CF_REQUIRE(slash > pre);
        cf_span seg = {url.ptr + pre, slash - pre - 1};
        cf_active_disk_key key = {0};
        CF_REQUIRE(cf_active_disk_key_verify(secret(), seg,
                                             NOW_2026_01_01_12_04, &key,
                                             &found) == CF_OK);
        CF_CHECK(found);
        CF_CHECK(str_is(key.key, "1ykrknnn9r4youv0f71wrntkzilx"));
        cf_active_disk_key_dispose(&key);
        cf_builder_dispose(&url);
    }
    /* Thumb representation redirect path shape (signed blob + variation). */
    {
        cf_active_ventries thumb = {0};
        entries_put(&thumb, "format", mk_str("jpg"));
        entries_put(&thumb, "resize_to_limit", mk_resize(1200, 800));
        cf_str vkey = {0};
        CF_REQUIRE(cf_active_variation_sign(secret(), &thumb, &vkey) == CF_OK);
        check_str(vkey,
                  "eyJfcmFpbHMiOnsiZGF0YSI6eyJmb3JtYXQiOiJqcGciLCJyZXNpemVfdG9fbGltaXQiOlsxMjAwLDgwMF19LCJwdXIiOiJ2YXJpYXRpb24ifX0=--28426ca1e33b0fea71b8b10b7f52a844de5886cf",
                  "thumb key");
        cf_str_dispose(&vkey);
        cf_active_ventries_dispose(&thumb);
    }
}

/* ---- direct uploads ------------------------------------------------------------ */

CF_TEST(direct_upload_check_matrix) {
    int64_t size = -1;
    CF_CHECK(cf_active_direct_upload_check(S("42"), true, true, true, &size) ==
             CF_ACTIVE_UPLOAD_OK);
    CF_CHECK(size == 42);
    CF_CHECK(cf_active_direct_upload_check(S("16777216"), true, true, true,
                                           &size) == CF_ACTIVE_UPLOAD_OK);
    CF_CHECK(cf_active_direct_upload_check(S("16777217"), true, true, true,
                                           &size) == CF_ACTIVE_UPLOAD_TOO_LARGE);
    CF_CHECK(cf_active_direct_upload_check(S("-1"), true, true, true, &size) ==
             CF_ACTIVE_UPLOAD_TOO_LARGE);
    CF_CHECK(cf_active_direct_upload_check(S("abc"), true, true, true, &size) ==
             CF_ACTIVE_UPLOAD_UNPROCESSABLE);
    CF_CHECK(cf_active_direct_upload_check(S(""), true, true, true, &size) ==
             CF_ACTIVE_UPLOAD_UNPROCESSABLE);
    CF_CHECK(cf_active_direct_upload_check(S("12"), false, true, true, &size) ==
             CF_ACTIVE_UPLOAD_UNPROCESSABLE);
    CF_CHECK(cf_active_direct_upload_check(S("12"), true, false, true, &size) ==
             CF_ACTIVE_UPLOAD_UNPROCESSABLE);
    CF_CHECK(cf_active_direct_upload_check(S("12"), true, true, false, &size) ==
             CF_ACTIVE_UPLOAD_UNPROCESSABLE);
    /* Active Storage JS sends byte_size as a number: numbers render as text. */
    CF_CHECK(cf_active_direct_upload_check(S("0"), true, true, true, &size) ==
             CF_ACTIVE_UPLOAD_OK);
    CF_CHECK(size == 0);
    /* integer_cast edges: must start like a number and fit in i64. */
    CF_CHECK(cf_active_direct_upload_check(S(" +12x"), true, true, true,
                                           &size) == CF_ACTIVE_UPLOAD_OK);
    CF_CHECK(size == 12);
    CF_CHECK(cf_active_direct_upload_check(S("99999999999999999999"), true, true,
                                           true,
                                           &size) ==
             CF_ACTIVE_UPLOAD_UNPROCESSABLE);
}

CF_TEST(direct_upload_json_shape) {
    cf_builder body = {0};
    CF_REQUIRE(cf_active_direct_upload_json(
                   7, S("key28charsxxxxxxxxxxxxxxxxxx"), S("a&b.png"),
                   S("image/png"), true, S("{\"identified\":true}"), S("local"),
                   42, S("abc=="), true, S("2026-03-02T16:00:00.123Z"),
                   S("signed-id"), S("http://h/disk/token"), S("image/png"),
                   true, &body) == CF_OK);
    /* ActiveSupport escaping applies to the JSON body too: & -> \u0026. */
    const char *flat = "{\"id\":7,\"key\":\"key28charsxxxxxxxxxxxxxxxxxx\","
                       "\"filename\":\"a\\u0026b.png\",\"content_type\":\"image/png\","
                       "\"metadata\":{\"identified\":true},\"service_name\":\"local\","
                       "\"byte_size\":42,\"checksum\":\"abc==\","
                       "\"created_at\":\"2026-03-02T16:00:00.123Z\","
                       "\"signed_id\":\"signed-id\","
                       "\"direct_upload\":{\"url\":\"http://h/disk/token\","
                       "\"headers\":{\"Content-Type\":\"image/png\"}}}";
    if (!builder_is(&body, flat)) {
        printf("    body: got %.*s\n", (int)body.len, body.ptr);
        CF_CHECK(false);
    }
    cf_builder_dispose(&body);
    /* Null content type, checksum and upload type. */
    CF_REQUIRE(cf_active_direct_upload_json(
                   1, S("k"), S("f"), S(""), false, S("{}"), S("local"), 0,
                   S(""), false, S("t"), S("s"), S("u"), S(""), false,
                   &body) == CF_OK);
    if (!builder_is(&body,
                    "{\"id\":1,\"key\":\"k\",\"filename\":\"f\","
                    "\"content_type\":null,\"metadata\":{},"
                    "\"service_name\":\"local\",\"byte_size\":0,"
                    "\"checksum\":null,\"created_at\":\"t\",\"signed_id\":\"s\","
                    "\"direct_upload\":{\"url\":\"u\","
                    "\"headers\":{\"Content-Type\":null}}}")) {
        printf("    nulls: got %.*s\n", (int)body.len, body.ptr);
        CF_CHECK(false);
    }
    cf_builder_dispose(&body);
}

CF_TEST(json_time_vectors) {
    struct {
        const char *in;
        const char *want;
    } rows[] = {
        {"2026-03-02 16:00:00.123456", "2026-03-02T16:00:00.123Z"},
        {"2026-03-02 16:00:00", "2026-03-02T16:00:00.000Z"},
        {"2026-03-02 16:00:00.1", "2026-03-02T16:00:00.100Z"},
        {"garbage", "garbage"},
        {"2026-03-02", "2026-03-02"},
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        cf_builder out = {0};
        CF_REQUIRE(cf_active_json_time(S(rows[i].in), &out) == CF_OK);
        if (!builder_is(&out, rows[i].want)) {
            printf("    row %zu: got %.*s want %s\n", i, (int)out.len, out.ptr,
                   rows[i].want);
            CF_CHECK(false);
        }
        cf_builder_dispose(&out);
    }
}

/* ---- attachments ----------------------------------------------------------------- */

CF_TEST(attachment_state_machine) {
    cf_active_attach_kind kind = CF_ACTIVE_ATTACH_INVALID;
    int64_t blob_id = -1;
    /* Absent means unchanged. */
    CF_REQUIRE(cf_active_attach_classify(secret(), CF_ACTIVE_SHAPE_ABSENT, S(""),
                                         NOW_2026_01_01_12_00, &kind,
                                         &blob_id) == CF_OK);
    CF_CHECK(kind == CF_ACTIVE_ATTACH_UNCHANGED);
    /* Permitted null/"" delete. */
    CF_REQUIRE(cf_active_attach_classify(secret(), CF_ACTIVE_SHAPE_NULL, S(""),
                                         NOW_2026_01_01_12_00, &kind,
                                         &blob_id) == CF_OK);
    CF_CHECK(kind == CF_ACTIVE_ATTACH_DELETE);
    CF_REQUIRE(cf_active_attach_classify(secret(), CF_ACTIVE_SHAPE_EMPTY, S(""),
                                         NOW_2026_01_01_12_00, &kind,
                                         &blob_id) == CF_OK);
    CF_CHECK(kind == CF_ACTIVE_ATTACH_DELETE);
    /* Uploads stage next. */
    CF_REQUIRE(cf_active_attach_classify(secret(), CF_ACTIVE_SHAPE_UPLOAD, S(""),
                                         NOW_2026_01_01_12_00, &kind,
                                         &blob_id) == CF_OK);
    CF_CHECK(kind == CF_ACTIVE_ATTACH_UPLOAD);
    /* A verified signed blob id attaches the existing blob. */
    const char *signed_id =
        "eyJfcmFpbHMiOnsiZGF0YSI6NDIsInB1ciI6ImJsb2JfaWQifX0=--c252dc1b14dd0e8cd599eb40203378880bafed1f";
    CF_REQUIRE(cf_active_attach_classify(secret(), CF_ACTIVE_SHAPE_STRING,
                                         S(signed_id), NOW_2026_01_01_12_00,
                                         &kind, &blob_id) == CF_OK);
    CF_CHECK(kind == CF_ACTIVE_ATTACH_SIGNED && blob_id == 42);
    /* Anything else is invalid (never authorized without verifying). */
    CF_REQUIRE(cf_active_attach_classify(secret(), CF_ACTIVE_SHAPE_STRING,
                                         S("not-a-token"), NOW_2026_01_01_12_00,
                                         &kind, &blob_id) == CF_OK);
    CF_CHECK(kind == CF_ACTIVE_ATTACH_INVALID);
    CF_REQUIRE(cf_active_attach_classify(secret(), CF_ACTIVE_SHAPE_STRING,
                                         S("42"), NOW_2026_01_01_12_00, &kind,
                                         &blob_id) == CF_OK);
    CF_CHECK(kind == CF_ACTIVE_ATTACH_INVALID);
    /* A blob_key token is not a blob reference. */
    const char *disk_key =
        "eyJfcmFpbHMiOnsiZGF0YSI6eyJrZXkiOiJ4dGFwampjaml1ZHJsazN0bXd5amdwdW9iYWJkIiwiZGlzcG9zaXRpb24iOiJpbmxpbmU7IGZpbGVuYW1lPVwiYVx1MDAyNmIucG5nXCI7IGZpbGVuYW1lKj1VVEYtOCcnYSUyNmIucG5nIiwiY29udGVudF90eXBlIjoiaW1hZ2UvcG5nIiwic2VydmljZV9uYW1lIjoibG9jYWwifSwiZXhwIjoiMjAyNi0wMS0wMVQxMjowNTowMC4wMDBaIiwicHVyIjoiYmxvYl9rZXkifX0=--764195902735b3201f5cf6129ff3b5089150e3e1";
    CF_REQUIRE(cf_active_attach_classify(secret(), CF_ACTIVE_SHAPE_STRING,
                                         S(disk_key), NOW_2026_01_01_12_04,
                                         &kind, &blob_id) == CF_OK);
    CF_CHECK(kind == CF_ACTIVE_ATTACH_INVALID);
}

CF_TEST(attachment_allowlist) {
    CF_CHECK(cf_active_attach_allowed(S("User"), S("avatar")));
    CF_CHECK(cf_active_attach_allowed(S("Account"), S("logo")));
    CF_CHECK(cf_active_attach_allowed(S("Message"), S("attachment")));
    CF_CHECK(cf_active_attach_allowed(S("ActiveStorage::VariantRecord"), S("image")));
    CF_CHECK(cf_active_attach_allowed(S("ActiveStorage::Blob"), S("preview_image")));
    CF_CHECK(!cf_active_attach_allowed(S("User"), S("logo")));
    CF_CHECK(!cf_active_attach_allowed(S("Account"), S("avatar")));
    CF_CHECK(!cf_active_attach_allowed(S("Message"), S("avatar")));
    CF_CHECK(!cf_active_attach_allowed(S("Room"), S("attachment")));
    CF_CHECK(!cf_active_attach_allowed(S("user"), S("avatar")));
    CF_CHECK(!cf_active_attach_allowed(S(""), S("")));
}

/* ---- concurrent variant first-wins (STORE-03, latch-based) ---------------------- */

typedef struct {
    pthread_mutex_t lock;
    pthread_barrier_t gate; /* both racers insert at the same instant */
    bool taken;
    int winners;
    int serial;
} race_registry;

static cf_err race_try_insert(void *ctx, cf_span digest, bool *inserted) {
    race_registry *reg = ctx;
    (void)digest;
    /* Rendezvous first so both threads genuinely race the record. */
    int w = pthread_barrier_wait(&reg->gate);
    if (w != 0 && w != PTHREAD_BARRIER_SERIAL_THREAD) return CF_IO;
    pthread_mutex_lock(&reg->lock);
    if (!reg->taken) {
        reg->taken = true;
        reg->winners++;
        *inserted = true;
    } else {
        *inserted = false;
    }
    reg->serial++;
    pthread_mutex_unlock(&reg->lock);
    return CF_OK;
}

typedef struct {
    race_registry *reg;
    cf_storage *storage;
    char key[CF_STORAGE_KEY_LENGTH + 1];
    bool won;
    cf_err rc;
} racer_arg;

static const char RACE_DIGEST[] = "IBhrLAIapu+NCId+2Kz6EqUWRKY=";

static void *race_thread(void *arg) {
    racer_arg *racer = arg;
    /* Each racer stages its own file (the transform output). */
    cf_storage_upload *upload = NULL;
    racer->rc = cf_storage_upload_begin(racer->storage, &upload);
    if (racer->rc == CF_OK)
        racer->rc = cf_storage_upload_write(upload, S("variant-bytes"));
    if (racer->rc == CF_OK) racer->rc = cf_storage_upload_move(upload, S(racer->key));
    if (racer->rc == CF_OK) {
        bool won = false;
        racer->rc = cf_active_variant_claim(racer->reg, S(RACE_DIGEST),
                                            race_try_insert, &won);
        if (racer->rc == CF_OK) {
            racer->won = won;
            if (won) racer->rc = cf_storage_upload_commit(upload);
        }
    }
    /* The loser drops only its own file; the winner keeps its record file. */
    cf_storage_upload_dispose(upload);
    return NULL;
}

static void remove_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d != NULL) {
            struct dirent *entry;
            while ((entry = readdir(d)) != NULL) {
                if (strcmp(entry->d_name, ".") == 0 ||
                    strcmp(entry->d_name, "..") == 0)
                    continue;
                char child[4096];
                snprintf(child, sizeof child, "%s/%s", path, entry->d_name);
                remove_tree(child);
            }
            closedir(d);
        }
        (void)rmdir(path);
    } else {
        (void)unlink(path);
    }
}

static bool make_root(char *out, size_t cap) {
    snprintf(out, cap, "/tmp/cf-s02-race-XXXXXX");
    return mkdtemp(out) != NULL;
}

CF_TEST(concurrent_variant_keeps_first_record) {
    char root[256];
    CF_REQUIRE(make_root(root, sizeof root));
    cf_storage *storage = NULL;
    CF_REQUIRE(cf_storage_open(root, &storage) == CF_OK);
    race_registry reg;
    memset(&reg, 0, sizeof reg);
    CF_REQUIRE(pthread_mutex_init(&reg.lock, NULL) == 0);
    CF_REQUIRE(pthread_barrier_init(&reg.gate, NULL, 2) == 0);
    racer_arg a = {&reg, storage, "aaaaaaaaaaaaaaaaaaaaaaaaaaaa", false, CF_IO};
    racer_arg b = {&reg, storage, "bbbbbbbbbbbbbbbbbbbbbbbbbbbb", false, CF_IO};
    pthread_t ta, tb;
    CF_REQUIRE(pthread_create(&ta, NULL, race_thread, &a) == 0);
    CF_REQUIRE(pthread_create(&tb, NULL, race_thread, &b) == 0);
    CF_REQUIRE(pthread_join(ta, NULL) == 0);
    CF_REQUIRE(pthread_join(tb, NULL) == 0);
    CF_CHECK(a.rc == CF_OK && b.rc == CF_OK);
    /* Exactly one record won, from exactly two attempts. */
    CF_CHECK(reg.winners == 1 && reg.serial == 2);
    CF_CHECK(a.won != b.won);
    /* The winner's file is intact; the loser's file is gone. */
    const char *winner_key = a.won ? a.key : b.key;
    const char *loser_key = a.won ? b.key : a.key;
    int fd = -1;
    uint64_t size = 0;
    CF_CHECK(cf_storage_open_read(storage, S(winner_key), &fd, &size) == CF_OK);
    CF_CHECK(size == strlen("variant-bytes"));
    if (fd >= 0) close(fd);
    CF_CHECK(cf_storage_open_read(storage, S(loser_key), &fd, &size) == CF_NOT_FOUND);
    pthread_barrier_destroy(&reg.gate);
    pthread_mutex_destroy(&reg.lock);
    cf_storage_close(storage);
    remove_tree(root);
}

/* ---- purge recheck (STORE-03) ------------------------------------------------- */

CF_TEST(purge_gate_and_files) {
    /* Referenced blobs refuse (no-op success); unreferenced proceed. */
    CF_CHECK(!cf_active_purge_proceed(1));
    CF_CHECK(!cf_active_purge_proceed(7));
    CF_CHECK(cf_active_purge_proceed(0));
    char root[256];
    CF_REQUIRE(make_root(root, sizeof root));
    cf_storage *storage = NULL;
    CF_REQUIRE(cf_storage_open(root, &storage) == CF_OK);
    /* Unreferenced file is deleted. */
    cf_storage_upload *upload = NULL;
    CF_REQUIRE(cf_storage_upload_begin(storage, &upload) == CF_OK);
    CF_REQUIRE(cf_storage_upload_write(upload, S("blob-bytes")) == CF_OK);
    CF_REQUIRE(cf_storage_upload_move(upload, S("cccccccccccccccccccccccccccc")) == CF_OK);
    CF_REQUIRE(cf_storage_upload_commit(upload) == CF_OK);
    cf_storage_upload_dispose(upload);
    CF_REQUIRE(cf_active_purge_files(storage, S("cccccccccccccccccccccccccccc"),
                                     false) == CF_OK);
    int fd = -1;
    uint64_t size = 0;
    CF_CHECK(cf_storage_open_read(storage, S("cccccccccccccccccccccccccccc"), &fd,
                                  &size) == CF_NOT_FOUND);
    /* Missing file during purge is success. */
    CF_REQUIRE(cf_active_purge_files(storage, S("dddddddddddddddddddddddddddd"),
                                     true) == CF_OK);
    /* A directory smuggled at the key path cannot be removed by unlink:
     * permission/I-O failure is CF_IO for the job to record as failed. */
    char dirpath[512];
    snprintf(dirpath, sizeof dirpath, "%s/%c%c/%c%c/%s", root, 'e', 'e', 'e',
             'e', "eeeeeeeeeeeeeeeeeeeeeeeeeeee");
    char parent[512];
    snprintf(parent, sizeof parent, "%s/ee", root);
    CF_REQUIRE(mkdir(parent, 0700) == 0 || errno == EEXIST);
    snprintf(parent, sizeof parent, "%s/ee/ee", root);
    CF_REQUIRE(mkdir(parent, 0700) == 0 || errno == EEXIST);
    CF_REQUIRE(mkdir(dirpath, 0700) == 0);
    CF_CHECK(cf_active_purge_files(storage, S("eeeeeeeeeeeeeeeeeeeeeeeeeeee"),
                                   false) == CF_IO);
    CF_CHECK(cf_active_purge_files(storage, S("not-a-valid-key!!__________"),
                                   false) == CF_INVALID);
    CF_CHECK(cf_active_purge_files(NULL, S("cccccccccccccccccccccccccccc"),
                                   false) == CF_INVALID);
    cf_storage_close(storage);
    remove_tree(root);
}

/* ---- disk PUT checksum enforcement ---------------------------------------------- */

CF_TEST(disk_upload_verifies_checksum) {
    char root[256];
    snprintf(root, sizeof root, "/tmp/cf-s02-put-XXXXXX");
    CF_REQUIRE(mkdtemp(root) != NULL);
    cf_storage *storage = NULL;
    CF_REQUIRE(cf_storage_open(root, &storage) == CF_OK);
    /* MD5("bytes") base64 is 1B2M2Y8AsgTpgAmY7PhCfg== (reference key.rs). */
    const char *contents = "bytes";
    /* Compute the expected checksum through the staging boundary itself to
     * avoid hardcoding: stage, read checksum, then PUT fresh. */
    cf_storage_upload *probe = NULL;
    CF_REQUIRE(cf_storage_upload_begin(storage, &probe) == CF_OK);
    CF_REQUIRE(cf_storage_upload_write(probe, S(contents)) == CF_OK);
    cf_builder sum = {0};
    CF_REQUIRE(cf_storage_upload_checksum(probe, &sum) == CF_OK);
    char expect[64];
    CF_REQUIRE(sum.len < sizeof expect);
    memcpy(expect, sum.ptr, sum.len);
    expect[sum.len] = '\0';
    cf_storage_upload_dispose(probe);
    cf_builder_dispose(&sum);
    CF_REQUIRE(cf_active_disk_upload(storage, S("ffffffffffffffffffffffffffff"),
                                    S(contents), S(expect)) == CF_OK);
    int fd = -1;
    uint64_t size = 0;
    CF_REQUIRE(cf_storage_open_read(storage, S("ffffffffffffffffffffffffffff"),
                                    &fd, &size) == CF_OK);
    CF_CHECK(size == strlen(contents));
    if (fd >= 0) close(fd);
    /* Mismatch stores nothing and reports invalid (the action's 422). */
    CF_CHECK(cf_active_disk_upload(storage, S("gggggggggggggggggggggggggggg"),
                                   S(contents), S("AAAAAAAAAAAAAAAAAAAAAA==")) ==
             CF_INVALID);
    CF_CHECK(cf_storage_open_read(storage, S("gggggggggggggggggggggggggggg"), &fd,
                                  &size) == CF_NOT_FOUND);
    cf_storage_close(storage);
    remove_tree(root);
}

/* ---- S03 boundary ------------------------------------------------------------------ */

CF_TEST(representation_needs_s03) {
    /* Media completion is S03: fail loudly, never approximate. */
    CF_CHECK(cf_active_representation_process(S("image/png"), true) == CF_INTERNAL);
    CF_CHECK(cf_active_representation_process(S("video/mp4"), true) == CF_INTERNAL);
    CF_CHECK(cf_active_representation_process(S("image/png"), false) == CF_INTERNAL);
}

CF_TEST_MAIN()
