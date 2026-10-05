/* tests/auth/test_crypto.c — A01 unit tests for the current-format crypto
 * layer: key derivation, signed/encrypted cookie vectors, tamper/wrong-key
 * negatives and the bcrypt password vectors.
 *
 * Oracle: tests/fixtures/vectors/rails_compat.json via the generated
 * tests/auth/vectors.h (current-format selection only).  Build (plain; add
 * -fsanitize=address,undefined for the sanitize run):
 *   clang -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra
 *         -Werror -pthread -Isrc -Itests
 *         -Ivendor/src/yyjson/src -Ivendor/src/sqlite/sqlite-amalgamation-3530400
 *         -Ivendor/build/libxcrypt-clang -Ivendor/build/openssl-clang/install/include
 *         tests/auth/test_crypto.c src/auth/{crypto,json,message,tokens,password}.c
 *         src/core/{alloc,buffer,clock,error,random}.c
 *         vendor/build/yyjson-clang/libyyjson.a
 *         vendor/build/openssl-clang/install/lib/libcrypto.a
 *         vendor/build/libxcrypt-clang/.libs/libcrypt.a -lm -ldl
 *         -o build/a01/test_crypto
 */
#include "cf_test.h"

#include "auth.h"
#include "auth/internal.h"

#include "vectors.h"

#include <stdio.h>
#include <string.h>

#include <yyjson.h>

static cf_span S(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

static int64_t vec_now_us(const char *iso) {
    int64_t ns = 0;
    CF_REQUIRE(auth_iso8601_parse_ns(S(iso), &ns) == CF_OK);
    return ns / 1000;
}

static bool json_equal(const char *left, const char *right) {
    yyjson_read_err err;
    yyjson_doc *a = yyjson_read_opts((char *)left, strlen(left), 0, NULL, &err);
    yyjson_doc *b =
        yyjson_read_opts((char *)right, strlen(right), 0, NULL, &err);
    bool equal = a != NULL && b != NULL &&
                 yyjson_equals(yyjson_doc_get_root(a), yyjson_doc_get_root(b));
    if (a != NULL) yyjson_doc_free(a);
    if (b != NULL) yyjson_doc_free(b);
    return equal;
}

static bool str_is(cf_str value, const char *want) {
    size_t len = strlen(want);
    return value.ptr != NULL && value.len == len &&
           memcmp(value.ptr, want, len) == 0;
}

CF_TEST(key_derivation_vectors) {
    for (size_t i = 0; i < AUTH_VEC_KEYS_LEN; i++) {
        const auth_vec_key *vec = &AUTH_VEC_KEYS[i];
        unsigned char key[64];
        CF_REQUIRE(auth_pbkdf2_sha256(S(AUTH_VEC_SECRET_KEY_BASE),
                                      S(vec->salt), vec->length,
                                      key) == CF_OK);
        cf_str hex = {0};
        CF_REQUIRE(auth_hex_encode((cf_span){key, vec->length}, &hex) == CF_OK);
        CF_CHECK(str_is(hex, vec->key_hex));
        cf_str_dispose(&hex);
    }
}

CF_TEST(base64_flavors) {
    cf_str strict = {0};
    CF_REQUIRE(auth_base64_encode(S("a b"), CF_AUTH_ENCODING_STRICT,
                                  &strict) == CF_OK);
    CF_CHECK(str_is(strict, "YSBi"));
    cf_str url = {0};
    CF_REQUIRE(auth_base64_encode(S("\xfb\xff"), CF_AUTH_ENCODING_URLSAFE,
                                  &url) == CF_OK);
    CF_CHECK(str_is(url, "-_8"));
    cf_str padded = {0};
    CF_REQUIRE(auth_base64_encode(S("\xfb\xff"), CF_AUTH_ENCODING_URLSAFE_PADDED,
                                  &padded) == CF_OK);
    CF_CHECK(str_is(padded, "-_8="));
    cf_buf *decoded = NULL;
    CF_REQUIRE(auth_base64_strict_decode(S("YSBi"), &decoded) == CF_OK);
    CF_CHECK(cf_buf_span(decoded).len == 3);
    CF_CHECK(memcmp(cf_buf_span(decoded).ptr, "a b", 3) == 0);
    cf_buf_release(decoded);
    /* urlsafe_decode is lenient about padding and alphabet; strict is not. */
    decoded = NULL;
    CF_CHECK(auth_base64_urlsafe_decode(S("QQ"), &decoded) == CF_OK);
    CF_CHECK(decoded != NULL && cf_buf_span(decoded).len == 1);
    if (decoded != NULL) cf_buf_release(decoded);
    decoded = NULL;
    CF_CHECK(auth_base64_strict_decode(S("QQ"), &decoded) != CF_OK);
    CF_CHECK(decoded == NULL);
    decoded = NULL;
    CF_CHECK(auth_base64_strict_decode(S("QR=="), &decoded) != CF_OK);
    cf_str_dispose(&strict);
    cf_str_dispose(&url);
    cf_str_dispose(&padded);
}

CF_TEST(signed_cookie_generate_vectors) {
    for (size_t i = 0; i < AUTH_VEC_SIGNED_GEN_LEN; i++) {
        const auth_vec_signed_gen *vec = &AUTH_VEC_SIGNED_GEN[i];
        bool has_expiry = vec->expires_at != NULL;
        int64_t expires_us = has_expiry ? vec_now_us(vec->expires_at) : 0;
        cf_str raw = {0};
        CF_REQUIRE(cf_auth_signed_cookie_generate(
                       S(AUTH_VEC_SECRET_KEY_BASE), S(vec->name), S(vec->value),
                       has_expiry, expires_us, &raw) == CF_OK);
        CF_CHECK(str_is(raw, vec->raw));
        /* The generated value verifies back to the input. */
        cf_str value = {0};
        bool found = false;
        CF_REQUIRE(cf_auth_signed_cookie_verify(
                       S(AUTH_VEC_SECRET_KEY_BASE), S(vec->name),
                       (cf_span){(const unsigned char *)raw.ptr, raw.len},
                       vec_now_us(AUTH_VEC_NOW) + 0, &value, &found) == CF_OK);
        CF_CHECK(found);
        if (found) {
            CF_CHECK(str_is(value, vec->value));
        }
        cf_str_dispose(&value);
        cf_str_dispose(&raw);
    }
}

CF_TEST(signed_cookie_verify_vectors) {
    for (size_t i = 0; i < AUTH_VEC_SIGNED_VERIFY_LEN; i++) {
        const auth_vec_cookie_verify *vec = &AUTH_VEC_SIGNED_VERIFY[i];
        cf_str value = {0};
        bool found = false;
        CF_REQUIRE(cf_auth_signed_cookie_verify(
                       S(AUTH_VEC_SECRET_KEY_BASE), S(vec->name), S(vec->raw),
                       vec_now_us(vec->now), &value, &found) == CF_OK);
        if (vec->has_expected) {
            if (!found) {
                CF_CHECK(found);
                printf("    %s: expected %s\n", vec->case_name, vec->expected);
            } else {
                CF_CHECK(str_is(value, vec->expected));
            }
        } else {
            CF_CHECK(!found);
        }
        cf_str_dispose(&value);
    }
}

CF_TEST(signed_cookie_rejects_wrong_secret) {
    cf_str raw = {0};
    CF_REQUIRE(cf_auth_signed_cookie_generate(
                   S(AUTH_VEC_SECRET_KEY_BASE), S("session_token"),
                   S("e5XWBGGnEJqiHpVCZzbj6bra"), true,
                   INT64_C(2400000000000000), &raw) == CF_OK);
    static const char other_secret[] =
        "0000000000000000000000000000000000000000000000000000000000000000"
        "rotatedffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
    cf_str value = {0};
    bool found = false;
    CF_REQUIRE(cf_auth_signed_cookie_verify(
                   S(other_secret), S("session_token"),
                   (cf_span){(const unsigned char *)raw.ptr, raw.len},
                   vec_now_us(AUTH_VEC_NOW), &value, &found) == CF_OK);
    CF_CHECK(!found);
    cf_str_dispose(&value);
    cf_str_dispose(&raw);
}

CF_TEST(encrypted_cookie_round_trips) {
    for (size_t i = 0; i < AUTH_VEC_ENCRYPTED_GEN_LEN; i++) {
        const auth_vec_encrypted_gen *vec = &AUTH_VEC_ENCRYPTED_GEN[i];
        bool has_expiry = vec->expires_at != NULL;
        int64_t expires_us = has_expiry ? vec_now_us(vec->expires_at) : 0;
        cf_str raw = {0};
        CF_REQUIRE(cf_auth_cookie_encrypt(
                       S(AUTH_VEC_SECRET_KEY_BASE), S(vec->name),
                       S(vec->value_json), has_expiry, expires_us,
                       &raw) == CF_OK);
        cf_str decoded = {0};
        bool found = false;
        CF_REQUIRE(cf_auth_cookie_decrypt(
                       S(AUTH_VEC_SECRET_KEY_BASE), S(vec->name),
                       (cf_span){(const unsigned char *)raw.ptr, raw.len},
                       vec_now_us(AUTH_VEC_NOW), &decoded, &found) == CF_OK);
        CF_CHECK(found);
        if (found) CF_CHECK(json_equal(decoded.ptr, vec->value_json));
        cf_str_dispose(&decoded);
        cf_str_dispose(&raw);
    }
}

CF_TEST(encrypted_cookie_verify_vectors) {
    for (size_t i = 0; i < AUTH_VEC_ENCRYPTED_VERIFY_LEN; i++) {
        const auth_vec_encrypted_verify *vec = &AUTH_VEC_ENCRYPTED_VERIFY[i];
        cf_str decoded = {0};
        bool found = false;
        CF_REQUIRE(cf_auth_cookie_decrypt(
                       S(AUTH_VEC_SECRET_KEY_BASE), S(vec->name), S(vec->raw),
                       vec_now_us(vec->now), &decoded, &found) == CF_OK);
        if (vec->has_expected) {
            if (!found) {
                CF_CHECK(found);
                printf("    %s: expected %s\n", vec->case_name,
                       vec->expected_json);
            } else {
                CF_CHECK(json_equal(decoded.ptr, vec->expected_json));
            }
        } else {
            CF_CHECK(!found);
        }
        cf_str_dispose(&decoded);
    }
}

CF_TEST(password_vectors) {
    for (size_t i = 0; i < AUTH_VEC_PASSWORD_CHECKS_LEN; i++) {
        const auth_vec_password_check *vec = &AUTH_VEC_PASSWORD_CHECKS[i];
        bool ok = cf_password_verify(
            (cf_str){(char *)vec->password, strlen(vec->password)},
            (cf_str){(char *)vec->digest, strlen(vec->digest)});
        if (ok != vec->expected) {
            printf("    password %zu: expected %d got %d\n", i,
                   (int)vec->expected, (int)ok);
            CF_CHECK(ok == vec->expected);
        }
    }
    bool seeded = cf_password_verify(
        CF_STR_LIT("secret123456"),
        (cf_str){(char *)AUTH_VEC_SEEDED_DIGEST,
                 strlen(AUTH_VEC_SEEDED_DIGEST)});
    CF_CHECK(seeded);
    CF_CHECK(!cf_password_verify(CF_STR_LIT("anything"), CF_STR_LIT("not a digest")));
    CF_CHECK(!cf_password_verify(CF_STR_LIT("anything"), CF_STR_LIT("")));

    cf_str digest = {0};
    CF_REQUIRE(cf_auth_password_digest(CF_STR_LIT("pässwörd ☃"), 4, &digest) ==
               CF_OK);
    CF_CHECK(strncmp(digest.ptr, "$2a$04$", 7) == 0);
    CF_CHECK(cf_password_verify(CF_STR_LIT("pässwörd ☃"), digest));
    cf_str_dispose(&digest);
    CF_CHECK(cf_auth_password_digest(CF_STR_LIT("x"), 3, &digest) == CF_INVALID);
    CF_CHECK(cf_auth_password_digest(CF_STR_LIT("x"), 32, &digest) == CF_INVALID);
}

CF_TEST_MAIN()
