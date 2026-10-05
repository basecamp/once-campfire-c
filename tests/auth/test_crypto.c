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

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include <openssl/evp.h>
#include <yyjson.h>

static cf_span S(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* The derivation oracle used by the memo tests: OpenSSL directly, bypassing
 * auth_pbkdf2_sha256 and its cache entirely. */
static void direct_pbkdf2(cf_span password, cf_span salt, size_t length,
                          unsigned char *out) {
    CF_REQUIRE(length <= 64);
    CF_REQUIRE(PKCS5_PBKDF2_HMAC((const char *)password.ptr, (int)password.len,
                                 salt.ptr, (int)salt.len, 1000, EVP_sha256(),
                                 (int)length, out) == 1);
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

/* --- memoized key derivation ----------------------------------------------
 * auth_pbkdf2_sha256 memoizes derived keys per full secret + salt + length
 * (the pin's ActiveSupport::CachingKeyGenerator).  The tests below never
 * inspect the cache directly: every result must equal the direct OpenSSL
 * derivation, whatever the cache did, and distinct secrets must never expose
 * each other's keys. */

enum {
    CHURN_THREADS = 4,
    CHURN_SECRETS = 3,
    CHURN_SALTS = 2,
    CHURN_ROUNDS = 2
};

static const char *const churn_secrets[CHURN_SECRETS] = {
    "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff",
    "ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100",
    "0123456789abcdeffedcba98765432100123456789abcdeffedcba9876543210",
};
static const char *const churn_salts[CHURN_SALTS] = {
    "cache-churn/one",
    "cache-churn/two",
};

typedef struct {
    unsigned char expected[CHURN_SECRETS][CHURN_SALTS][64];
    atomic_int bad;
} churn_state;

static void *churn_worker(void *arg) {
    churn_state *state = arg;
    for (int round = 0; round < CHURN_ROUNDS; round++) {
        for (int s = 0; s < CHURN_SECRETS; s++) {
            for (int t = 0; t < CHURN_SALTS; t++) {
                unsigned char got[64];
                cf_err rc = auth_pbkdf2_sha256(S(churn_secrets[s]),
                                               S(churn_salts[t]), 64, got);
                if (rc != CF_OK ||
                    memcmp(got, state->expected[s][t], 64) != 0) {
                    atomic_store(&state->bad, 1);
                }
            }
        }
    }
    return NULL;
}

/* All threads race to derive the same fresh (secret, salt) set, so the
 * lookup/derive/insert path runs concurrently on first sight.  Runs under the
 * TSan build (auth binaries are part of tsan-impl) with no data races and no
 * wrong bytes. */
CF_TEST(key_cache_thread_churn) {
    churn_state state;
    memset(&state, 0, sizeof state);
    atomic_init(&state.bad, 0);
    for (int s = 0; s < CHURN_SECRETS; s++) {
        for (int t = 0; t < CHURN_SALTS; t++) {
            direct_pbkdf2(S(churn_secrets[s]), S(churn_salts[t]), 64,
                          state.expected[s][t]);
        }
    }
    pthread_t threads[CHURN_THREADS];
    for (int i = 0; i < CHURN_THREADS; i++) {
        CF_REQUIRE(pthread_create(&threads[i], NULL, churn_worker, &state) ==
                   0);
    }
    for (int i = 0; i < CHURN_THREADS; i++) {
        CF_REQUIRE(pthread_join(threads[i], NULL) == 0);
    }
    CF_CHECK(atomic_load(&state.bad) == 0);
}

CF_TEST(key_cache_secrets_stay_isolated) {
    /* Both 64 hex characters, sharing a 63-character prefix: only a full
     * length+bytes comparison can tell them apart. */
    static const char secret_a[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    static const char secret_b[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde0";
    static const char *const salts[] = {
        "signed cookie",
        "authenticated encrypted cookie",
        "active_record/signed_id",
        "signed_global_ids",
        "turbo/signed_stream_verifier_key",
        "ActiveStorage",
    };
    static const size_t lengths[] = {64, 32};
    for (size_t s = 0; s < sizeof salts / sizeof salts[0]; s++) {
        for (size_t l = 0; l < sizeof lengths / sizeof lengths[0]; l++) {
            unsigned char want_a[64], want_b[64], got[64];
            direct_pbkdf2(S(secret_a), S(salts[s]), lengths[l], want_a);
            direct_pbkdf2(S(secret_b), S(salts[s]), lengths[l], want_b);
            CF_CHECK(memcmp(want_a, want_b, lengths[l]) != 0);
            CF_REQUIRE(auth_pbkdf2_sha256(S(secret_a), S(salts[s]),
                                          lengths[l], got) == CF_OK);
            CF_CHECK(memcmp(got, want_a, lengths[l]) == 0);
            /* Interleaved: secret_b's call must not return secret_a's entry. */
            CF_REQUIRE(auth_pbkdf2_sha256(S(secret_b), S(salts[s]),
                                          lengths[l], got) == CF_OK);
            CF_CHECK(memcmp(got, want_b, lengths[l]) == 0);
            /* And back to secret_a, whose entry is still live alongside. */
            CF_REQUIRE(auth_pbkdf2_sha256(S(secret_a), S(salts[s]),
                                          lengths[l], got) == CF_OK);
            CF_CHECK(memcmp(got, want_a, lengths[l]) == 0);
        }
    }
    /* secret_a is a prefix of this secret: the length is part of the key. */
    static const char secret_extended[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdefbeef";
    unsigned char want[64], got[64];
    direct_pbkdf2(S(secret_extended), S("signed cookie"), 64, want);
    CF_REQUIRE(auth_pbkdf2_sha256(S(secret_extended), S("signed cookie"), 64,
                                  got) == CF_OK);
    CF_CHECK(memcmp(got, want, 64) == 0);
}

CF_TEST(key_cache_capacity_overflow_derives_correctly) {
    /* More distinct secrets than the bounded cache holds: past capacity the
     * fallback is a direct derivation, never another secret's entry, and
     * oversized keying material is never truncated into a cache key. */
    for (int i = 0; i < 40; i++) {
        char secret[65];
        snprintf(secret, sizeof secret, "%016x%016x%016x%016x", (unsigned)i,
                 (unsigned)i ^ 0x55555555u, (unsigned)i * 2654435761u,
                 (unsigned)~i);
        CF_REQUIRE(strlen(secret) == 64);
        unsigned char want[64], got[64];
        direct_pbkdf2(S(secret), S("signed cookie"), 64, want);
        CF_REQUIRE(auth_pbkdf2_sha256(S(secret), S("signed cookie"), 64, got) ==
                   CF_OK);
        CF_CHECK(memcmp(got, want, 64) == 0);
    }
    /* Oversized secret (beyond the cache's storage cap): still byte-identical,
     * twice in a row (derived, not cached). */
    char big[600];
    memset(big, 'a', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    unsigned char want[32], got[32];
    direct_pbkdf2(S(big), S("oversized"), 32, want);
    for (int i = 0; i < 2; i++) {
        CF_REQUIRE(auth_pbkdf2_sha256(S(big), S("oversized"), 32, got) ==
                   CF_OK);
        CF_CHECK(memcmp(got, want, 32) == 0);
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
