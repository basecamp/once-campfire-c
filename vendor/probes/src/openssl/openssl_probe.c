/*
 * F00 clang probe for OpenSSL 4.0.3 (static, no-asm, no-shared build).
 *
 * Required observations from docs/devel/implementation/01-foundation-http.md:
 *   - EVP HMAC-SHA256 known-answer vector
 *   - PBKDF2-HMAC-SHA256 known answers
 *   - RAND_bytes
 * Exits 0 only when every check passes.
 */
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>

#include <stdio.h>
#include <string.h>

static int failures;

static int check_hex(const char *label, const unsigned char *got, size_t got_len,
                     const char *want_hex)
{
    char got_hex[2 * 64 + 1];
    size_t want_len = strlen(want_hex) / 2;
    size_t i;

    if (got_len > 64) {
        printf("FAIL %s: digest too long (%zu bytes)\n", label, got_len);
        return 1;
    }
    for (i = 0; i < got_len; i++)
        snprintf(got_hex + 2 * i, 3, "%02x", got[i]);
    got_hex[2 * got_len] = '\0';
    if (got_len != want_len || strcmp(got_hex, want_hex) != 0) {
        printf("FAIL %s: got %s want %s\n", label, got_hex, want_hex);
        return 1;
    }
    printf("PASS %s: %s\n", label, got_hex);
    return 0;
}

static int hmac_sha256(const unsigned char *key, size_t key_len,
                       const unsigned char *data, size_t data_len,
                       unsigned char *out, size_t *out_len)
{
    EVP_MAC *mac;
    EVP_MAC_CTX *ctx;
    OSSL_PARAM params[2];
    int ok = 0;

    mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
    if (mac == NULL)
        return 1;
    ctx = EVP_MAC_CTX_new(mac);
    if (ctx == NULL) {
        EVP_MAC_free(mac);
        return 1;
    }
    params[0] = OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, "SHA256", 0);
    params[1] = OSSL_PARAM_construct_end();
    if (EVP_MAC_init(ctx, key, key_len, params) == 1 &&
        EVP_MAC_update(ctx, data, data_len) == 1 &&
        EVP_MAC_final(ctx, out, out_len, 64) == 1)
        ok = 1;
    EVP_MAC_CTX_free(ctx);
    EVP_MAC_free(mac);
    return ok ? 0 : 1;
}

static int pbkdf2_sha256(const char *pass, const char *salt, unsigned int iter,
                         unsigned char *out, size_t out_len)
{
    EVP_KDF *kdf;
    EVP_KDF_CTX *ctx;
    OSSL_PARAM params[5];
    unsigned int iterations = iter;
    int ok = 0;

    kdf = EVP_KDF_fetch(NULL, "PBKDF2", NULL);
    if (kdf == NULL)
        return 1;
    ctx = EVP_KDF_CTX_new(kdf);
    if (ctx == NULL) {
        EVP_KDF_free(kdf);
        return 1;
    }
    params[0] = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, "SHA256", 0);
    params[1] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD,
                                                  (void *)pass, strlen(pass));
    params[2] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,
                                                  (void *)salt, strlen(salt));
    params[3] = OSSL_PARAM_construct_uint(OSSL_KDF_PARAM_ITER, &iterations);
    params[4] = OSSL_PARAM_construct_end();
    if (EVP_KDF_derive(ctx, out, out_len, params) == 1)
        ok = 1;
    EVP_KDF_CTX_free(ctx);
    EVP_KDF_free(kdf);
    return ok ? 0 : 1;
}

int main(void)
{
    unsigned char hmac_out[64];
    size_t hmac_len = 0;
    unsigned char kdf_out[32];
    unsigned char rnd1[32];
    unsigned char rnd2[32];
    size_t i;
    int all_zero = 1;

    printf("OpenSSL_version(OPENSSL_VERSION) = %s\n", OpenSSL_version(OPENSSL_VERSION));
    printf("compile-time OPENSSL_VERSION_TEXT = %s\n", OPENSSL_VERSION_TEXT);

    /* RFC 4231 test case 2. */
    failures += hmac_sha256((const unsigned char *)"Jefe", 4,
                            (const unsigned char *)"what do ya want for nothing?", 28,
                            hmac_out, &hmac_len);
    failures += check_hex("hmac-sha256-rfc4231-tc2", hmac_out, hmac_len,
                          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    /* PBKDF2-HMAC-SHA256 vectors (verified independently with Python hashlib). */
    if (pbkdf2_sha256("password", "salt", 1, kdf_out, sizeof kdf_out) != 0) {
        printf("FAIL pbkdf2-sha256-c1: EVP_KDF_derive failed\n");
        failures++;
    } else {
        failures += check_hex("pbkdf2-sha256-c1", kdf_out, sizeof kdf_out,
                              "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    }
    if (pbkdf2_sha256("password", "salt", 2, kdf_out, sizeof kdf_out) != 0) {
        printf("FAIL pbkdf2-sha256-c2: EVP_KDF_derive failed\n");
        failures++;
    } else {
        failures += check_hex("pbkdf2-sha256-c2", kdf_out, sizeof kdf_out,
                              "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
    }

    if (RAND_status() != 1) {
        printf("FAIL rand-status: RAND_status returned %d\n", RAND_status());
        failures++;
    } else {
        printf("PASS rand-status: RAND_status() == 1\n");
    }
    if (RAND_bytes(rnd1, sizeof rnd1) != 1 || RAND_bytes(rnd2, sizeof rnd2) != 1) {
        printf("FAIL rand-bytes: RAND_bytes failed\n");
        failures++;
    } else if (memcmp(rnd1, rnd2, sizeof rnd1) == 0) {
        printf("FAIL rand-bytes: two RAND_bytes outputs identical\n");
        failures++;
    } else {
        for (i = 0; i < sizeof rnd1; i++) {
            if (rnd1[i] != 0)
                all_zero = 0;
        }
        if (all_zero) {
            printf("FAIL rand-bytes: output all zero\n");
            failures++;
        } else {
            printf("PASS rand-bytes: 2x32 bytes, distinct, non-zero sample %02x%02x%02x%02x\n",
                   rnd1[0], rnd1[1], rnd1[2], rnd1[3]);
        }
    }

    if (failures == 0) {
        printf("OPENSSL_PROBE: ALL CHECKS PASSED\n");
        return 0;
    }
    printf("OPENSSL_PROBE: %d CHECK(S) FAILED\n", failures);
    return 1;
}
