/* tests/integrations/test_push.c — Web Push vectors and policy (I02).
 *
 * Pinned sources (read-only):
 *   tmp/rust-ref/crates/campfire/src/integrations/web_push.rs
 *   tmp/rust-ref/crates/campfire/src/integrations/web_push/{encryption,vapid,pool}.rs
 *   tmp/rust-ref/crates/campfire/src/integrations/web_push/tests.rs
 *   tmp/rust-ref/crates/campfire/src/integrations/testdata/web_push_expected.json
 * The RFC 8291 section-5 vector comes from encryption.rs's test module.
 *
 * Deterministic fixtures (injected keys/salts) come first; the loopback
 * delivery case at the end proves the wire bytes without touching I01's
 * files (its exchange is stubbed locally in this file). Subscription
 * records use the real D01 model for endpoint gating; recipient payload
 * selection itself (pushes_for/badge) stays in the model, covered by its
 * own tests — the seam is documented in the cases below.
 *
 * LINK FALLBACKS: push.c calls the model's resolved_endpoint_ip (read-only
 * header type plus two model objects). Every other model/db symbol below is
 * a weak stand-in that aborts when called: these paths never run here, and
 * the real definitions win in a full application link.
 */
#include "cf_test.h"

#include "core/testclock.h"
#include "core/testrandom.h"
#include "db/db_internal.h"
#include "integrations/push.h"
#include "models/membership.h"
#include "models/message.h"
#include "models/room.h"
#include "models/user.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/obj_mac.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ================= weak fallbacks for unlinked siblings ================= */

__attribute__((weak)) cf_err cf_db_err(int sqlite_rc) {
    (void)sqlite_rc;
    abort();
}

__attribute__((weak)) cf_err cf_db_failf(cf_err rc, const char *fmt, ...) {
    (void)fmt;
    abort();
    return rc;
}

__attribute__((weak)) cf_err cf_db_stmt(cf_db *db, const cf_stmt_set *set, size_t id,
                                        sqlite3_stmt **stmt) {
    (void)db;
    (void)set;
    (void)id;
    (void)stmt;
    abort();
}

__attribute__((weak)) void cf_db_stmt_done(sqlite3_stmt *stmt) {
    (void)stmt;
    abort();
}

__attribute__((weak)) cf_err cf_db_time_to_text(int64_t us, char out[CF_DB_TIME_TEXT_CAP]) {
    (void)us;
    (void)out;
    abort();
}

__attribute__((weak)) cf_err cf_db_time_from_text(cf_span text, int64_t *out_us) {
    (void)text;
    (void)out_us;
    abort();
}

__attribute__((weak)) cf_err cf_stmt_bind_i64(sqlite3_stmt *stmt, int index, int64_t value) {
    (void)stmt;
    (void)index;
    (void)value;
    abort();
}

__attribute__((weak)) cf_err cf_stmt_bind_text(sqlite3_stmt *stmt, int index, cf_span text) {
    (void)stmt;
    (void)index;
    (void)text;
    abort();
}

__attribute__((weak)) cf_err cf_stmt_bind_opt_text(sqlite3_stmt *stmt, int index, bool present,
                                                   cf_span text) {
    (void)stmt;
    (void)index;
    (void)present;
    (void)text;
    abort();
}

__attribute__((weak)) bool cf_stmt_column_is_null(sqlite3_stmt *stmt, int column) {
    (void)stmt;
    (void)column;
    abort();
}

__attribute__((weak)) int64_t cf_stmt_column_i64(sqlite3_stmt *stmt, int column) {
    (void)stmt;
    (void)column;
    abort();
}

__attribute__((weak)) cf_span cf_stmt_column_text(sqlite3_stmt *stmt, int column) {
    (void)stmt;
    (void)column;
    abort();
}

__attribute__((weak)) int sqlite3_step(sqlite3_stmt *stmt) {
    (void)stmt;
    abort();
}

__attribute__((weak)) int64_t cf_now_us(const cf_app *app) {
    (void)app;
    abort();
}

__attribute__((weak)) cf_db *cf_tx_db(cf_tx *tx) {
    (void)tx;
    abort();
}

__attribute__((weak)) int64_t cf_membership_connection_cutoff(int64_t now_us) {
    (void)now_us;
    abort();
}

__attribute__((weak)) cf_err cf_membership_unread_count(cf_db *db, int64_t user_id,
                                                        int64_t *out) {
    (void)db;
    (void)user_id;
    (void)out;
    abort();
}

__attribute__((weak)) cf_err cf_message_creator(cf_db *db, const cf_message *message,
                                                cf_user *out) {
    (void)db;
    (void)message;
    (void)out;
    abort();
}

__attribute__((weak)) cf_err cf_message_plain_text_body(cf_db *db, const cf_message *message,
                                                       const cf_richtext *rich_text,
                                                       cf_str *out) {
    (void)db;
    (void)message;
    (void)rich_text;
    (void)out;
    abort();
}

__attribute__((weak)) cf_err cf_message_mentionees(cf_db *db, const cf_message *message,
                                                  const cf_richtext *rich_text,
                                                  cf_user_vector *out) {
    (void)db;
    (void)message;
    (void)rich_text;
    (void)out;
    abort();
}

__attribute__((weak)) void cf_room_dispose(cf_room *room) {
    (void)room;
    abort();
}

__attribute__((weak)) cf_err cf_room_find(cf_db *db, int64_t id, cf_room *out) {
    (void)db;
    (void)id;
    (void)out;
    abort();
}

__attribute__((weak)) bool cf_room_direct(const cf_room *room) {
    (void)room;
    abort();
}

__attribute__((weak)) void cf_user_dispose(cf_user *user) {
    (void)user;
    abort();
}

__attribute__((weak)) void cf_user_vector_dispose(cf_user_vector *vector) {
    (void)vector;
    abort();
}

/* ================= small test helpers =================================== */

/* Decode pinned urlsafe Base64; aborts the case on failure. */
static unsigned char *must_decode(const char *b64, size_t *len_out) {
    unsigned char *bytes = NULL;
    size_t len = 0;
    CF_REQUIRE(cf_push_b64u_decode(b64, &bytes, &len) == CF_OK);
    CF_REQUIRE(bytes != NULL);
    if (len_out != NULL) *len_out = len;
    return bytes;
}

static char *must_encode(const unsigned char *bytes, size_t len) {
    char *text = NULL;
    CF_REQUIRE(cf_push_b64u_encode(bytes, len, &text) == CF_OK);
    CF_REQUIRE(text != NULL);
    return text;
}

/* HMAC-SHA256 + HKDF-Expand mirrors for the test-side decrypt path
 * (independent of push.c's static helpers). */
static void test_hmac(const unsigned char *key, size_t key_len,
                       const unsigned char *data, size_t data_len,
                       unsigned char out[32]) {
    unsigned int len = 0;
    CF_REQUIRE(HMAC(EVP_sha256(), key, (int)key_len, data, data_len, out, &len) != NULL);
    CF_REQUIRE(len == 32);
}

static void test_hkdf(const unsigned char *salt, size_t salt_len,
                       const unsigned char *ikm, size_t ikm_len,
                       const unsigned char *info, size_t info_len,
                       unsigned char *okm, size_t okm_len) {
    unsigned char prk[32];
    unsigned char previous[32];
    size_t previous_len = 0;
    size_t done = 0;
    unsigned char counter = 1;
    test_hmac(salt, salt_len, ikm, ikm_len, prk);
    while (done < okm_len) {
        unsigned char block[32 + 256];
        CF_REQUIRE(previous_len + info_len + 1 <= sizeof block);
        if (previous_len != 0) memcpy(block, previous, previous_len);
        if (info_len != 0) memcpy(block + previous_len, info, info_len);
        block[previous_len + info_len] = counter;
        test_hmac(prk, sizeof prk, block, previous_len + info_len + 1, previous);
        {
            size_t take = okm_len - done < 32 ? okm_len - done : 32;
            memcpy(okm + done, previous, take);
            done += take;
        }
        previous_len = 32;
        counter++;
    }
}

/* What a user agent does with the message (RFC 8291 section 3.4), for the
 * tests: mirrors `encryption::decrypt` via EVP derive/decrypt. */
static int test_decrypt(const unsigned char *body, size_t body_len,
                        const unsigned char receiver_priv[32],
                        const unsigned char *auth, size_t auth_len,
                        uint32_t *record_size_out, unsigned char **plain_out,
                        size_t *plain_len_out) {
    static const unsigned char info_prefix[] = "WebPush: info";
    static const unsigned char cek_info[] = "Content-Encoding: aes128gcm";
    static const unsigned char nonce_info[] = "Content-Encoding: nonce";
    uint32_t record_size = 0;
    size_t id_len = 0;
    const unsigned char *server_pub = NULL;
    const unsigned char *ciphertext = NULL;
    size_t ciphertext_len = 0;
    unsigned char receiver_pub[65];
    unsigned char secret[32];
    unsigned char prk[32];
    unsigned char key[16];
    unsigned char nonce[12];
    unsigned char *info = NULL;
    unsigned char *plain = NULL;
    EVP_CIPHER_CTX *gcm = NULL;
    EC_GROUP *group = NULL;
    EC_POINT *point = NULL;
    EC_POINT *peer_point = NULL;
    EC_POINT *shared_point = NULL;
    BIGNUM *priv_bn = NULL;
    BIGNUM *x = NULL;
    int step = 0;
    int final_step = 0;
    int rc = -1;
    if (body_len < 21) return -1;
    record_size = ((uint32_t)body[16] << 24) | ((uint32_t)body[17] << 16) |
        ((uint32_t)body[18] << 8) | (uint32_t)body[19];
    id_len = body[20];
    if (21 + id_len > body_len || id_len != 65) return -1;
    server_pub = body + 21;
    ciphertext = body + 21 + id_len;
    ciphertext_len = body_len - 21 - id_len;
    /* ECDH through EC point math (big-endian scalars throughout). */
    group = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
    point = EC_POINT_new(group);
    peer_point = EC_POINT_new(group);
    shared_point = EC_POINT_new(group);
    priv_bn = BN_bin2bn(receiver_priv, 32, NULL);
    x = BN_new();
    if (group == NULL || point == NULL || peer_point == NULL || shared_point == NULL ||
        priv_bn == NULL || x == NULL) {
        goto done;
    }
    {
        BN_CTX *ctx = BN_CTX_new();
        size_t oct = 0;
        if (ctx == NULL) goto done;
        if (EC_POINT_mul(group, point, priv_bn, NULL, NULL, ctx) != 1) {
            BN_CTX_free(ctx);
            goto done;
        }
        oct = EC_POINT_point2oct(group, point, POINT_CONVERSION_UNCOMPRESSED,
                                 receiver_pub, sizeof receiver_pub, ctx);
        if (oct != sizeof receiver_pub) {
            BN_CTX_free(ctx);
            goto done;
        }
        if (EC_POINT_oct2point(group, peer_point, server_pub, id_len, ctx) != 1) {
            BN_CTX_free(ctx);
            goto done;
        }
        if (EC_POINT_mul(group, shared_point, NULL, peer_point, priv_bn, ctx) != 1) {
            BN_CTX_free(ctx);
            goto done;
        }
        if (EC_POINT_get_affine_coordinates(group, shared_point, x, NULL, ctx) != 1) {
            BN_CTX_free(ctx);
            goto done;
        }
        BN_CTX_free(ctx);
        if (BN_bn2binpad(x, secret, sizeof secret) != (int)sizeof secret) goto done;
    }
    {
        size_t info_len = sizeof(info_prefix) + sizeof receiver_pub + id_len;
        info = malloc(info_len);
        if (info == NULL) goto done;
        memcpy(info, info_prefix, sizeof(info_prefix) - 1);
        info[sizeof(info_prefix) - 1] = 0;
        memcpy(info + sizeof info_prefix, receiver_pub, sizeof receiver_pub);
        memcpy(info + sizeof info_prefix + sizeof receiver_pub, server_pub, id_len);
        test_hkdf(auth, auth_len, secret, sizeof secret, info, info_len, prk, sizeof prk);
        test_hkdf(body, 16, prk, sizeof prk, cek_info, sizeof(cek_info), key, sizeof key);
        test_hkdf(body, 16, prk, sizeof prk, nonce_info, sizeof(nonce_info), nonce,
                  sizeof nonce);
    }
    plain = malloc(ciphertext_len + 1);
    if (plain == NULL) goto done;
    gcm = EVP_CIPHER_CTX_new();
    if (gcm == NULL) goto done;
    if (EVP_DecryptInit_ex(gcm, EVP_aes_128_gcm(), NULL, NULL, NULL) != 1) goto done;
    if (EVP_CIPHER_CTX_ctrl(gcm, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1) goto done;
    if (EVP_DecryptInit_ex(gcm, NULL, NULL, key, nonce) != 1) goto done;
    if (EVP_DecryptUpdate(gcm, plain, &step, ciphertext, (int)(ciphertext_len - 16)) != 1) {
        goto done;
    }
    if (EVP_CIPHER_CTX_ctrl(gcm, EVP_CTRL_GCM_SET_TAG, 16,
                            (void *)(ciphertext + ciphertext_len - 16)) != 1) {
        goto done;
    }
    if (EVP_DecryptFinal_ex(gcm, plain + step, &final_step) != 1) goto done;
    *record_size_out = record_size;
    *plain_out = plain;
    *plain_len_out = (size_t)step + (size_t)final_step;
    plain = NULL;
    rc = 0;
done:
    EVP_CIPHER_CTX_free(gcm);
    EC_GROUP_free(group);
    EC_POINT_free(point);
    EC_POINT_free(peer_point);
    EC_POINT_free(shared_point);
    BN_free(priv_bn);
    BN_free(x);
    free(info);
    free(plain);
    return rc;
}

/* Verify a raw (r||s) ES256 signature over `input` with the b64 public key. */
static bool test_verify_sig(const char *pub_b64, const char *input, const char *sig_b64) {
    unsigned char *pub = NULL;
    size_t pub_len = 0;
    unsigned char *sig = NULL;
    size_t sig_len = 0;
    EVP_PKEY *key = NULL;
    EVP_PKEY_CTX *ctx = NULL;
    EVP_MD_CTX *mctx = NULL;
    ECDSA_SIG *parsed = NULL;
    unsigned char *der = NULL;
    int der_len = 0;
    bool ok = false;
    OSSL_PARAM params[3];
    if (cf_push_b64u_decode(pub_b64, &pub, &pub_len) != CF_OK) goto done;
    if (cf_push_b64u_decode(sig_b64, &sig, &sig_len) != CF_OK) goto done;
    if (sig_len != 64) goto done;
    {
        BIGNUM *r = BN_bin2bn(sig, 32, NULL);
        BIGNUM *s = BN_bin2bn(sig + 32, 32, NULL);
        parsed = ECDSA_SIG_new();
        if (r == NULL || s == NULL || parsed == NULL) {
            BN_free(r);
            BN_free(s);
            goto done;
        }
        if (ECDSA_SIG_set0(parsed, r, s) != 1) {
            BN_free(r);
            BN_free(s);
            goto done;
        }
        der_len = i2d_ECDSA_SIG(parsed, NULL);
        if (der_len <= 0) goto done;
        der = malloc((size_t)der_len);
        if (der == NULL) goto done;
        {
            unsigned char *cursor = der;
            if (i2d_ECDSA_SIG(parsed, &cursor) != der_len) goto done;
        }
    }
    ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
    if (ctx == NULL) goto done;
    params[0] = OSSL_PARAM_construct_utf8_string("group", "P-256", 0);
    params[1] = OSSL_PARAM_construct_octet_string("pub", pub, pub_len);
    params[2] = OSSL_PARAM_construct_end();
    if (EVP_PKEY_fromdata_init(ctx) <= 0) goto done;
    if (EVP_PKEY_fromdata(ctx, &key, EVP_PKEY_PUBLIC_KEY, params) <= 0) goto done;
    mctx = EVP_MD_CTX_new();
    if (mctx == NULL) goto done;
    if (EVP_DigestVerifyInit(mctx, NULL, EVP_sha256(), NULL, key) != 1) goto done;
    ok = EVP_DigestVerify(mctx, der, (size_t)der_len, (const unsigned char *)input,
                          strlen(input)) == 1;
done:
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(key);
    EVP_MD_CTX_free(mctx);
    ECDSA_SIG_free(parsed);
    free(pub);
    free(sig);
    free(der);
    return ok;
}

/* ================= pinned vectors =========================================
 * From encryption.rs tests (RFC 8291 section 5), web_push_expected.json and
 * web_push/tests.rs. Comments name the exact source of each constant.
 */

static const char *rfc_plaintext_b64 = "V2hlbiBJIGdyb3cgdXAsIEkgd2FudCB0byBiZSBhIHdhdGVybWVsb24";
static const char *rfc_as_private = "yfWPiYE-n46HLnH0KqZOF1fJJU3MYrct3AELtAQ-oRw";
static const char *rfc_ua_public = "BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4";
static const char *rfc_ua_private = "q1dXpw3UpT5VOmu_cf_v6ih07Aems3njxI-JWgLcM94";
static const char *rfc_salt = "DGv6ra1nlYgDCS1FRnbzlw";
static const char *rfc_auth = "BTBZMqHH6r4Tts7J_aSIgg";
static const char *rfc_message =
    "DGv6ra1nlYgDCS1FRnbzlwAAEABBBP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A_"
    "yl95bQpu6cVPTpK4Mqgkf1CXztLVBSt2Ks3oZwbuwXPXLWyouBWLVWGNWQexSgSxsj_Qulcy4a-fN";

static const char *gem_receiver_private = "Y1H_BKvKi4Ht2cE4HZ8AgD0nn8chE3zhgBtEODTn4I4=";
static const char *gem_p256dh =
    "BDaRUn_gvMok9AhEo1soNNypgpMDfe2K0hz6RMEIxxULWKfVUqodLAxtX1mrUMC4PGgk1RkZJTut7vJ2RMrxowo=";
static const char *gem_auth = "BCuqPYqblNIL9NEJ4WT1Yw==";
static const char *gem_message =
    "{\"title\":\"Designers <&> \\\"quotes\\\" \xc3\xa9 \xf0\x9f\x98\x80\",\"options\":{\"body\":\"Kevin: "
    "line\\nbreak\\ttab \xe2\x80\xa8 \\u001f / \\\\ \",\"icon\":\"/account/logo\",\"data\":{\"path\":\"/rooms/1\","
    "\"badge\":3}}}";
static const char *gem_ciphertext =
    "aDE1uz32M2tQ2YYPv8YaFgAAALhBBFV0pWPjdGKwrY1oz2-NmLXT5SR0UqYQ03iA6_A8Y31sv1ypadCShWimwnH_k6x4pD3AEDyG2_dw_"
    "9St5Q6lcifhHkDdS_zMiD9jb3UQ2Fhzj8ruG1JMPaT6hhcbqokvY9mqIgQLqcTIWnDqjzYHt8Cw9l-pQzTsWan6M6njekeeDAdf09ietcPM2AU4kSJh59AD3sCJfxW8s_"
    "fSQYGUbbw7TitE1FfklTZ9fGxwME3KBDqqhIoCJr4W9F22sk22eMtmQwMN90xo2nP0Sb59Yurptd3_erpzfKknE5iAYpQeID4IF1qXYQ6O-"
    "G0jg8scfCtzxtaXqlT4";
static const int64_t gem_now = 1700000000;
static const char *gem_authorization_k =
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8cTriz_qYBVicY02_VxTQ";
static const char *gem_jwt_header = "eyJ0eXAiOiJKV1QiLCJhbGciOiJFUzI1NiJ9";
static const char *gem_jwt_payload =
    "eyJhdWQiOiJodHRwczovL2ZjbS5nb29nbGVhcGlzLmNvbSIsImV4cCI6MTcwMDA0MzIwMCwic3ViIjoibWFpbHRvOnN1cHBvcnRAMzdzaWduYWxzLmNvbSJ9";

/* VAPID test pair from web_push/tests.rs (parity/.env.reference). */
static const char *vapid_public =
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8cTriz_qYBVicY02_VxTQ=";
static const char *vapid_private = "qfXLHghuG1rSHZUVo9SscNRI-0EIHRbIrfeGCqbAwak=";
static const char *reference_subject = "mailto:support@37signals.com";

/* ================= crypto vector cases ================================== */

CF_TEST(push_rfc8291_encrypt_vector) {
    /* encryption.rs `matches_the_rfc_8291_test_vector`: deterministic encrypt
     * with the RFC server key/salt and the RFC layout (record size 4096,
     * padding [0x02]) must equal MESSAGE byte for byte. */
    size_t plain_len = 0;
    size_t priv_len = 0;
    size_t salt_len = 0;
    unsigned char *plain = must_decode(rfc_plaintext_b64, &plain_len);
    unsigned char *priv = must_decode(rfc_as_private, &priv_len);
    unsigned char *salt = must_decode(rfc_salt, &salt_len);
    unsigned char server_priv[32];
    unsigned char salt16[16];
    unsigned char *body = NULL;
    size_t body_len = 0;
    char *encoded = NULL;
    static const unsigned char padding[] = {2};
    CF_REQUIRE(priv_len == 32);
    CF_REQUIRE(salt_len == 16);
    memcpy(server_priv, priv, 32);
    memcpy(salt16, salt, 16);
    CF_REQUIRE(cf_push_encrypt_with(plain, plain_len, rfc_ua_public, rfc_auth, server_priv,
                                    salt16, 4096, padding, sizeof padding, &body,
                                    &body_len, NULL) == CF_OK);
    encoded = must_encode(body, body_len);
    CF_CHECK(strcmp(encoded, rfc_message) == 0);
    free(plain);
    free(priv);
    free(salt);
    free(body);
    free(encoded);
}

CF_TEST(push_rfc8291_decrypt_vector) {
    /* The RFC MESSAGE decrypts with the UA key/auth to (4096, plaintext). */
    size_t cipher_len = 0;
    size_t priv_len = 0;
    size_t auth_len = 0;
    unsigned char *cipher = must_decode(rfc_message, &cipher_len);
    unsigned char *priv = must_decode(rfc_ua_private, &priv_len);
    unsigned char *auth = must_decode(rfc_auth, &auth_len);
    unsigned char receiver_priv[32];
    uint32_t record_size = 0;
    unsigned char *plain = NULL;
    size_t out_len = 0;
    unsigned char *expected = NULL;
    size_t expected_len = 0;
    CF_REQUIRE(priv_len == 32);
    memcpy(receiver_priv, priv, 32);
    CF_REQUIRE(test_decrypt(cipher, cipher_len, receiver_priv, auth, auth_len, &record_size,
                             &plain, &out_len) == 0);
    CF_CHECK(record_size == 4096);
    expected = must_decode(rfc_plaintext_b64, &expected_len);
    CF_REQUIRE(out_len == expected_len + 1);
    CF_CHECK(memcmp(plain, expected, expected_len) == 0);
    CF_CHECK(plain[expected_len] == 2);
    free(cipher);
    free(priv);
    free(auth);
    free(plain);
    free(expected);
}

CF_TEST(push_gem_ciphertext_decrypts) {
    /* encryption.rs `decrypts_what_the_gem_encrypted`: the pinned gem
     * ciphertext opens with the receiver key, gem framing intact. */
    size_t cipher_len = 0;
    size_t priv_len = 0;
    size_t auth_len = 0;
    unsigned char *cipher = must_decode(gem_ciphertext, &cipher_len);
    unsigned char *priv = must_decode(gem_receiver_private, &priv_len);
    unsigned char *auth = must_decode(gem_auth, &auth_len);
    unsigned char receiver_priv[32];
    uint32_t record_size = 0;
    unsigned char *plain = NULL;
    size_t out_len = 0;
    size_t message_len = strlen(gem_message);
    CF_REQUIRE(priv_len == 32);
    memcpy(receiver_priv, priv, 32);
    CF_REQUIRE(test_decrypt(cipher, cipher_len, receiver_priv, auth, auth_len, &record_size,
                             &plain, &out_len) == 0);
    CF_CHECK(record_size == (uint32_t)(cipher_len - 86));
    CF_REQUIRE(out_len == message_len + 2);
    CF_CHECK(memcmp(plain, gem_message, message_len) == 0);
    CF_CHECK(plain[message_len] == 2);
    CF_CHECK(plain[message_len + 1] == 0);
    free(cipher);
    free(priv);
    free(auth);
    free(plain);
}

CF_TEST(push_gem_framing_round_trip) {
    /* encryption.rs `round_trips_with_the_gems_framing` over our API:
     * cf_push_encrypt with fresh entropy opens to message + 02 00. */
    EC_GROUP *group = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
    EC_POINT *point = EC_POINT_new(group);
    BIGNUM *priv_bn = BN_new();
    unsigned char receiver_priv[32];
    unsigned char receiver_pub[65];
    unsigned char auth[16];
    char *p256dh = NULL;
    char *auth_b64 = NULL;
    unsigned char *body = NULL;
    size_t body_len = 0;
    uint32_t record_size = 0;
    unsigned char *plain = NULL;
    size_t out_len = 0;
    static const char input[] = "{\"title\":\"hi\"}";
    CF_REQUIRE(group != NULL && point != NULL && priv_bn != NULL);
    CF_REQUIRE(BN_rand_range(priv_bn, EC_GROUP_get0_order(group)) == 1);
    CF_REQUIRE(!BN_is_zero(priv_bn));
    CF_REQUIRE(BN_bn2binpad(priv_bn, receiver_priv, 32) == 32);
    {
        BN_CTX *ctx = BN_CTX_new();
        CF_REQUIRE(ctx != NULL);
        CF_REQUIRE(EC_POINT_mul(group, point, priv_bn, NULL, NULL, ctx) == 1);
        CF_REQUIRE(EC_POINT_point2oct(group, point, POINT_CONVERSION_UNCOMPRESSED,
                                      receiver_pub, sizeof receiver_pub,
                                      ctx) == sizeof receiver_pub);
        BN_CTX_free(ctx);
    }
    CF_REQUIRE(cf_random_bytes(auth, sizeof auth) == CF_OK);
    p256dh = must_encode(receiver_pub, sizeof receiver_pub);
    auth_b64 = must_encode(auth, sizeof auth);
    CF_REQUIRE(cf_push_encrypt((const unsigned char *)input, strlen(input), p256dh, auth_b64,
                               &body, &body_len, NULL) == CF_OK);
    CF_REQUIRE(test_decrypt(body, body_len, receiver_priv, auth, sizeof auth, &record_size,
                             &plain, &out_len) == 0);
    CF_CHECK(out_len == strlen(input) + 2);
    CF_CHECK(memcmp(plain, input, strlen(input)) == 0);
    CF_CHECK(plain[strlen(input)] == 2 && plain[strlen(input) + 1] == 0);
    CF_CHECK(record_size == (uint32_t)(body_len - 86));
    EC_GROUP_free(group);
    EC_POINT_free(point);
    BN_free(priv_bn);
    free(p256dh);
    free(auth_b64);
    free(body);
    free(plain);
}

CF_TEST(push_encrypt_rejects) {
    /* encryption.rs `rejects_what_the_gem_rejects` over our API, with the
     * reference refusal messages. */
    EC_GROUP *group = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
    BIGNUM *priv_bn = BN_new();
    unsigned char receiver_priv[32];
    unsigned char receiver_pub[65];
    char *ok_key = NULL;
    unsigned char *body = NULL;
    size_t body_len = 0;
    cf_push_crypt_error detail = CF_PUSH_CRYPT_OK;
    static const unsigned char one = 'm';
    CF_REQUIRE(group != NULL && priv_bn != NULL);
    CF_REQUIRE(BN_rand_range(priv_bn, EC_GROUP_get0_order(group)) == 1);
    CF_REQUIRE(!BN_is_zero(priv_bn));
    CF_REQUIRE(BN_bn2binpad(priv_bn, receiver_priv, 32) == 32);
    {
        BN_CTX *ctx = BN_CTX_new();
        EC_POINT *point = EC_POINT_new(group);
        CF_REQUIRE(ctx != NULL && point != NULL);
        CF_REQUIRE(EC_POINT_mul(group, point, priv_bn, NULL, NULL, ctx) == 1);
        CF_REQUIRE(EC_POINT_point2oct(group, point, POINT_CONVERSION_UNCOMPRESSED,
                                      receiver_pub, sizeof receiver_pub,
                                      ctx) == sizeof receiver_pub);
        EC_POINT_free(point);
        BN_CTX_free(ctx);
    }
    ok_key = must_encode(receiver_pub, sizeof receiver_pub);
    CF_CHECK(cf_push_encrypt(&one, 1, NULL, "YXV0aA", &body, &body_len, &detail) == CF_INVALID);
    CF_CHECK(detail == CF_PUSH_CRYPT_ARGUMENT);
    CF_CHECK(cf_push_encrypt(&one, 1, ok_key, "", &body, &body_len, &detail) == CF_INVALID);
    CF_CHECK(detail == CF_PUSH_CRYPT_ARGUMENT);
    CF_CHECK(cf_push_encrypt(&one, 1, "not base64!", "YXV0aA", &body, &body_len,
                             &detail) == CF_INVALID);
    CF_CHECK(detail == CF_PUSH_CRYPT_ARGUMENT);
    CF_CHECK(cf_push_encrypt(&one, 1, "dGVzdF9rZXk", "YXV0aA", &body, &body_len,
                             &detail) == CF_INVALID);
    CF_CHECK(detail == CF_PUSH_CRYPT_INVALID_KEY);
    CF_CHECK(body == NULL);
    EC_GROUP_free(group);
    BN_free(priv_bn);
    free(ok_key);
}

CF_TEST(push_encrypt_rejects_oversize) {
    EC_GROUP *group = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
    BIGNUM *priv_bn = BN_new();
    unsigned char receiver_priv[32];
    unsigned char receiver_pub[65];
    char *ok_key = NULL;
    unsigned char *big = malloc(4100);
    unsigned char *body = NULL;
    size_t body_len = 0;
    cf_push_crypt_error detail = CF_PUSH_CRYPT_OK;
    CF_REQUIRE(big != NULL && group != NULL && priv_bn != NULL);
    memset(big, 'x', 4100);
    CF_REQUIRE(BN_rand_range(priv_bn, EC_GROUP_get0_order(group)) == 1);
    CF_REQUIRE(!BN_is_zero(priv_bn));
    CF_REQUIRE(BN_bn2binpad(priv_bn, receiver_priv, 32) == 32);
    {
        BN_CTX *ctx = BN_CTX_new();
        EC_POINT *point = EC_POINT_new(group);
        CF_REQUIRE(ctx != NULL && point != NULL);
        CF_REQUIRE(EC_POINT_mul(group, point, priv_bn, NULL, NULL, ctx) == 1);
        CF_REQUIRE(EC_POINT_point2oct(group, point, POINT_CONVERSION_UNCOMPRESSED,
                                      receiver_pub, sizeof receiver_pub,
                                      ctx) == sizeof receiver_pub);
        EC_POINT_free(point);
        BN_CTX_free(ctx);
    }
    ok_key = must_encode(receiver_pub, sizeof receiver_pub);
    CF_CHECK(cf_push_encrypt(big, 4100, ok_key, "YXV0aA", &body, &body_len, &detail) ==
             CF_INVALID);
    CF_CHECK(detail == CF_PUSH_CRYPT_ARGUMENT);
    CF_CHECK(body == NULL);
    EC_GROUP_free(group);
    BN_free(priv_bn);
    free(big);
    free(ok_key);
}

CF_TEST(push_b64u_flavors) {
    /* encoding.rs `urlsafe_decode_is_lenient_like_ruby`. */
    unsigned char *a = NULL;
    unsigned char *b = NULL;
    size_t alen = 0;
    size_t blen = 0;
    unsigned char *junk = NULL;
    size_t junk_len = 0;
    CF_CHECK(cf_push_b64u_decode("QQ", &a, &alen) == CF_OK);
    CF_REQUIRE(alen == 1 && a != NULL);
    CF_CHECK(a[0] == 'A');
    CF_CHECK(cf_push_b64u_decode("QQ==", &b, &blen) == CF_OK);
    CF_REQUIRE(blen == 1 && b != NULL);
    CF_CHECK(b[0] == 'A');
    CF_CHECK(cf_push_b64u_decode("QQ=", &junk, &junk_len) == CF_INVALID);
    CF_CHECK(cf_push_b64u_decode("QR", &junk, &junk_len) == CF_INVALID);
    CF_CHECK(junk == NULL);
    {
        unsigned char *std = NULL;
        unsigned char *url = NULL;
        size_t slen = 0;
        size_t ulen = 0;
        CF_CHECK(cf_push_b64u_decode("a+b/", &std, &slen) == CF_OK);
        CF_CHECK(cf_push_b64u_decode("a-b_", &url, &ulen) == CF_OK);
        CF_REQUIRE(slen == ulen);
        CF_CHECK(memcmp(std, url, slen) == 0);
        free(std);
        free(url);
    }
    {
        char *unpadded = NULL;
        static const unsigned char raw[] = {0xfb, 0xff};
        CF_CHECK(cf_push_b64u_encode(raw, sizeof raw, &unpadded) == CF_OK);
        CF_CHECK(strcmp(unpadded, "-_8") == 0);
        free(unpadded);
    }
    free(a);
    free(b);
}

/* ================= payload and headers ================================== */

CF_TEST(push_message_json_exact) {
    /* web_push/tests.rs `encodes_the_message_like_json_generate`: the
     * expected.json message pins plain-JSON escaping (raw <&>/slashes/
     * non-ASCII, short escapes, lowercase \u001f). */
    char *json = NULL;
    CF_REQUIRE(cf_push_notification_json("Designers <&> \"quotes\" \xc3\xa9 \xf0\x9f\x98\x80",
                                         "Kevin: line\nbreak\ttab \xe2\x80\xa8 \x1f / \\ ",
                                         "/rooms/1", 3, &json) == CF_OK);
    CF_CHECK(strcmp(json, gem_message) == 0);
    free(json);
}

CF_TEST(push_json_escapes_controls_and_keeps_utf8) {
    char *json = NULL;
    /* serde_json leaves DEL (0x7f) and non-ASCII raw; only C0 controls
     * escape (short escapes where they exist, \u00xx otherwise). */
    CF_REQUIRE(cf_push_notification_json("t", "a\x01" "b\x7f" "c\xf0\x9f\x98\x80", "/p", -5,
                                         &json) == CF_OK);
    CF_CHECK(strstr(json, "\\u0001") != NULL);
    CF_CHECK(strstr(json, "b\x7f") != NULL);
    CF_CHECK(strstr(json, "\xf0\x9f\x98\x80") != NULL);
    CF_CHECK(strstr(json, "\"badge\":-5") != NULL);
    free(json);
}

CF_TEST(push_vapid_header_like_gem) {
    /* web_push/tests.rs `signs_the_vapid_header_like_the_gem` at now. */
    cf_push_vapid vapid;
    cf_push_vapid_error detail = CF_PUSH_VAPID_OK;
    char *authorization = NULL;
    memset(&vapid, 0, sizeof vapid);
    CF_REQUIRE(cf_push_vapid_init(reference_subject, vapid_public, vapid_private, &vapid,
                                  &detail) == CF_OK);
    CF_REQUIRE(cf_push_vapid_authorization(&vapid, "https://fcm.googleapis.com", gem_now,
                                           &authorization) == CF_OK);
    {
        const char *t = authorization;
        const char *k = NULL;
        CF_REQUIRE(strncmp(t, "vapid t=", 8) == 0);
        t += 8;
        k = strstr(t, ",k=");
        CF_REQUIRE(k != NULL);
        CF_CHECK(strcmp(k + 3, gem_authorization_k) == 0);
        {
            char *jwt = malloc((size_t)(k - t) + 1);
            char *dot1 = NULL;
            char *dot2 = NULL;
            CF_REQUIRE(jwt != NULL);
            memcpy(jwt, t, (size_t)(k - t));
            jwt[k - t] = '\0';
            dot1 = strchr(jwt, '.');
            CF_REQUIRE(dot1 != NULL);
            *dot1 = '\0';
            dot2 = strchr(dot1 + 1, '.');
            CF_REQUIRE(dot2 != NULL);
            *dot2 = '\0';
            CF_CHECK(strcmp(jwt, gem_jwt_header) == 0);
            CF_CHECK(strcmp(dot1 + 1, gem_jwt_payload) == 0);
            {
                char signing_input[1024];
                snprintf(signing_input, sizeof signing_input, "%s.%s", jwt, dot1 + 1);
                CF_CHECK(test_verify_sig(vapid_public, signing_input, dot2 + 1));
            }
            free(jwt);
        }
    }
    cf_push_vapid_dispose(&vapid);
    free(authorization);
}

CF_TEST(push_vapid_configured_subject) {
    cf_push_vapid vapid;
    char *authorization = NULL;
    memset(&vapid, 0, sizeof vapid);
    CF_REQUIRE(cf_push_vapid_init("mailto:ops@example.com", vapid_public, vapid_private,
                                  &vapid, NULL) == CF_OK);
    CF_REQUIRE(cf_push_vapid_authorization(&vapid, "https://fcm.googleapis.com", 0,
                                           &authorization) == CF_OK);
    {
        const char *t = authorization + strlen("vapid t=");
        const char *comma = strchr(t, ',');
        char *jwt = NULL;
        char *dot1 = NULL;
        char *payload = NULL;
        unsigned char *claims = NULL;
        size_t claims_len = 0;
        CF_REQUIRE(comma != NULL);
        jwt = malloc((size_t)(comma - t) + 1);
        CF_REQUIRE(jwt != NULL);
        memcpy(jwt, t, (size_t)(comma - t));
        jwt[comma - t] = '\0';
        dot1 = strchr(jwt, '.');
        CF_REQUIRE(dot1 != NULL);
        /* Decode the payload segment (up to the second dot). */
        {
            char *dot2 = strchr(dot1 + 1, '.');
            CF_REQUIRE(dot2 != NULL);
            *dot2 = '\0';
            CF_REQUIRE(cf_push_b64u_decode(dot1 + 1, &claims, &claims_len) == CF_OK);
            payload = malloc(claims_len + 1);
            CF_REQUIRE(payload != NULL);
            memcpy(payload, claims, claims_len);
            payload[claims_len] = '\0';
        }
        CF_CHECK(strstr(payload, "\"sub\":\"mailto:ops@example.com\"") != NULL);
        CF_CHECK(strstr(payload, "\"exp\":43200") != NULL);
        free(jwt);
        free(claims);
        free(payload);
    }
    cf_push_vapid_dispose(&vapid);
    free(authorization);
}

CF_TEST(push_vapid_rejects_bad_keys) {
    /* web_push/tests.rs `rejects_bad_vapid_keys_up_front`. */
    EC_GROUP *group = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
    BIGNUM *priv_bn = BN_new();
    unsigned char other_priv[32];
    unsigned char other_pub[65];
    char *other_public = NULL;
    cf_push_vapid vapid;
    cf_push_vapid_error detail = CF_PUSH_VAPID_OK;
    char unpadded_public[256];
    char unpadded_private[128];
    CF_REQUIRE(group != NULL && priv_bn != NULL);
    CF_REQUIRE(BN_rand_range(priv_bn, EC_GROUP_get0_order(group)) == 1);
    CF_REQUIRE(!BN_is_zero(priv_bn));
    CF_REQUIRE(BN_bn2binpad(priv_bn, other_priv, 32) == 32);
    {
        BN_CTX *ctx = BN_CTX_new();
        EC_POINT *point = EC_POINT_new(group);
        CF_REQUIRE(ctx != NULL && point != NULL);
        CF_REQUIRE(EC_POINT_mul(group, point, priv_bn, NULL, NULL, ctx) == 1);
        CF_REQUIRE(EC_POINT_point2oct(group, point, POINT_CONVERSION_UNCOMPRESSED,
                                      other_pub, sizeof other_pub, ctx) == sizeof other_pub);
        EC_POINT_free(point);
        BN_CTX_free(ctx);
    }
    other_public = must_encode(other_pub, sizeof other_pub);
    memset(&vapid, 0, sizeof vapid);
    CF_CHECK(cf_push_vapid_init(reference_subject, "", vapid_private, &vapid, &detail) ==
             CF_INVALID);
    CF_CHECK(detail == CF_PUSH_VAPID_INVALID_PUBLIC_KEY);
    CF_CHECK(cf_push_vapid_init(reference_subject, "dGVzdF9rZXk", vapid_private, &vapid,
                                &detail) == CF_INVALID);
    CF_CHECK(detail == CF_PUSH_VAPID_INVALID_PUBLIC_KEY);
    CF_CHECK(cf_push_vapid_init(reference_subject, vapid_public, "not base64!", &vapid,
                                &detail) == CF_INVALID);
    CF_CHECK(detail == CF_PUSH_VAPID_INVALID_PRIVATE_KEY);
    CF_CHECK(cf_push_vapid_init(reference_subject, vapid_public, "", &vapid,
                                &detail) == CF_INVALID);
    CF_CHECK(detail == CF_PUSH_VAPID_INVALID_PRIVATE_KEY);
    {
        /* 33 decoded bytes are too many for a P-256 scalar. */
        unsigned char long_priv[33];
        char *long_b64 = NULL;
        memset(long_priv, 1, sizeof long_priv);
        CF_REQUIRE(cf_push_b64u_encode(long_priv, sizeof long_priv, &long_b64) == CF_OK);
        CF_CHECK(cf_push_vapid_init(reference_subject, vapid_public, long_b64, &vapid,
                                    &detail) == CF_INVALID);
        CF_CHECK(detail == CF_PUSH_VAPID_INVALID_PRIVATE_KEY);
        free(long_b64);
    }
    {
        char *bad_priv = must_encode((const unsigned char *)"\xff\xff\xff\xff\xff\xff\xff\xff"
                                    "\xff\xff\xff\xff\xff\xff\xff\xff"
                                    "\xff\xff\xff\xff\xff\xff\xff\xff"
                                    "\xff\xff\xff\xff\xff\xff\xff\xff",
                                    32);
        CF_CHECK(cf_push_vapid_init(reference_subject, vapid_public, bad_priv, &vapid,
                                    &detail) == CF_INVALID);
        CF_CHECK(detail == CF_PUSH_VAPID_INVALID_PRIVATE_KEY);
        free(bad_priv);
    }
    CF_CHECK(cf_push_vapid_init(reference_subject, other_public, vapid_private, &vapid,
                                &detail) == CF_INVALID);
    CF_CHECK(detail == CF_PUSH_VAPID_MISMATCHED);
    /* Unpadded equivalents of the good pair parse identically. */
    snprintf(unpadded_public, sizeof unpadded_public, "%s", vapid_public);
    unpadded_public[strcspn(unpadded_public, "=")] = '\0';
    snprintf(unpadded_private, sizeof unpadded_private, "%s", vapid_private);
    unpadded_private[strcspn(unpadded_private, "=")] = '\0';
    CF_CHECK(cf_push_vapid_init(reference_subject, unpadded_public, unpadded_private,
                                &vapid, &detail) == CF_OK);
    CF_CHECK(detail == CF_PUSH_VAPID_OK);
    cf_push_vapid_dispose(&vapid);
    /* Missing configuration disables push explicitly. */
    CF_CHECK(cf_push_vapid_from_config(NULL, &vapid, &detail) == CF_INVALID);
    {
        struct cf_config empty;
        memset(&empty, 0, sizeof empty);
        CF_CHECK(cf_push_vapid_from_config(&empty, &vapid, &detail) == CF_INVALID);
        CF_CHECK(detail == CF_PUSH_VAPID_MISSING);
    }
    EC_GROUP_free(group);
    BN_free(priv_bn);
    free(other_public);
}

CF_TEST(push_headers_match_gem) {
    /* `delivers_to_the_pinned_address_with_the_gems_headers`: the five
     * fixed headers plus Authorization, in order. */
    cf_push_vapid vapid;
    cf_push_headers headers;
    memset(&vapid, 0, sizeof vapid);
    memset(&headers, 0, sizeof headers);
    CF_REQUIRE(cf_push_vapid_init(reference_subject, vapid_public, vapid_private, &vapid,
                                  NULL) == CF_OK);
    CF_REQUIRE(cf_push_build_headers(&vapid, "https://fcm.googleapis.com", gem_now, 270,
                                     &headers) == CF_OK);
    CF_REQUIRE(headers.len == 6);
    CF_CHECK(strcmp(headers.items[0].name, "Content-Type") == 0);
    CF_CHECK(strcmp(headers.items[0].value, "application/octet-stream") == 0);
    CF_CHECK(strcmp(headers.items[1].name, "Ttl") == 0);
    CF_CHECK(strcmp(headers.items[1].value, "2419200") == 0);
    CF_CHECK(strcmp(headers.items[2].name, "Urgency") == 0);
    CF_CHECK(strcmp(headers.items[2].value, "high") == 0);
    CF_CHECK(strcmp(headers.items[3].name, "Content-Encoding") == 0);
    CF_CHECK(strcmp(headers.items[3].value, "aes128gcm") == 0);
    CF_CHECK(strcmp(headers.items[4].name, "Content-Length") == 0);
    CF_CHECK(strcmp(headers.items[4].value, "270") == 0);
    CF_CHECK(strcmp(headers.items[5].name, "Authorization") == 0);
    CF_CHECK(strncmp(headers.items[5].value, "vapid t=", 8) == 0);
    CF_CHECK(strstr(headers.items[5].value, ",k=") != NULL);
    {
        char prefix[128];
        snprintf(prefix, sizeof prefix, "vapid t=%s.", gem_jwt_header);
        CF_CHECK(strncmp(headers.items[5].value, prefix, strlen(prefix)) == 0);
    }
    cf_push_headers_dispose(&headers);
    cf_push_vapid_dispose(&vapid);
}

/* ================= response policy ====================================== */

static void check_classification(unsigned status, const char *reason, const char *class_name,
                                 bool invalidates) {
    cf_push_delivery delivery;
    memset(&delivery, 0, sizeof delivery);
    CF_REQUIRE(cf_push_classify_response(status, reason, "push.example", &delivery) == CF_OK);
    CF_CHECK(strcmp(delivery.class_name, class_name) == 0);
    CF_CHECK(cf_push_delivery_invalidates(&delivery) == invalidates);
    CF_CHECK(delivery.status == status);
    cf_push_delivery_dispose(&delivery);
}

CF_TEST(push_response_matrix) {
    /* web_push/tests.rs `raises_what_the_gem_raises`. */
    check_classification(410, "Gone", "WebPush::ExpiredSubscription", true);
    check_classification(404, "Not Found", "WebPush::InvalidSubscription", true);
    check_classification(403, "Forbidden", "WebPush::Unauthorized", false);
    check_classification(400, "UnauthorizedRegistration", "WebPush::Unauthorized", false);
    check_classification(400, "Bad Request", "WebPush::ResponseError", false);
    check_classification(413, "Payload Too Large", "WebPush::PayloadTooLarge", false);
    check_classification(429, "Too Many Requests", "WebPush::TooManyRequests", false);
    check_classification(503, "Service Unavailable", "WebPush::PushServiceError", false);
    check_classification(302, "Found", "WebPush::ResponseError", false);
    check_classification(400, NULL, "WebPush::ResponseError", false);
    {
        cf_push_delivery delivery;
        memset(&delivery, 0, sizeof delivery);
        CF_REQUIRE(cf_push_classify_response(201, "Created", "push.example", &delivery) ==
                   CF_OK);
        CF_CHECK(delivery.kind == CF_PUSH_DELIVERY_OK);
        CF_CHECK(delivery.delivered && !delivery.skipped);
        CF_CHECK(!cf_push_delivery_invalidates(&delivery));
        cf_push_delivery_dispose(&delivery);
    }
}

/* ================= subscription fixtures and gating ===================== */

static cf_push_subscription make_subscription(int64_t id, int64_t user_id, const char *endpoint,
                                              const char *p256dh, const char *auth) {
    cf_push_subscription sub;
    memset(&sub, 0, sizeof sub);
    sub.id = id;
    sub.user_id = user_id;
    if (endpoint != NULL) {
        sub.endpoint.present = true;
        sub.endpoint.value.ptr = strdup(endpoint);
        sub.endpoint.value.len = strlen(endpoint);
    }
    if (p256dh != NULL) {
        sub.p256dh_key.present = true;
        sub.p256dh_key.value.ptr = strdup(p256dh);
        sub.p256dh_key.value.len = strlen(p256dh);
    }
    if (auth != NULL) {
        sub.auth_key.present = true;
        sub.auth_key.value.ptr = strdup(auth);
        sub.auth_key.value.len = strlen(auth);
    }
    return sub;
}

/* Fake PrivateNetworkGuard: only fcm.googleapis.com resolves, publicly. */
static cf_optional_str fake_resolve(void *arg, cf_str host) {
    cf_optional_str out;
    const char *ip = (const char *)arg;
    memset(&out, 0, sizeof out);
    if (host.len == strlen("fcm.googleapis.com") &&
        memcmp(host.ptr, "fcm.googleapis.com", host.len) == 0 && ip != NULL) {
        out.present = true;
        out.value.ptr = strdup(ip);
        out.value.len = strlen(ip);
    }
    return out;
}

/* Canned exchange: records the request head and answers on script. */
typedef struct {
    int called;
    unsigned status;
    char reason[128];
    cf_push_transport_error transport;
    char *host;
    char *target;
    unsigned port;
    size_t header_count;
    char *first_authorization;
    size_t body_len;
} canned_exchange_state;

static cf_err canned_exchange(void *ctx, const cf_push_request *request, unsigned *out_status,
                              char *reason_buf, size_t reason_cap,
                              cf_push_transport_error *transport_err) {
    canned_exchange_state *state = ctx;
    if (state == NULL || request == NULL || out_status == NULL || transport_err == NULL) {
        return CF_INVALID;
    }
    state->called++;
    free(state->host);
    free(state->target);
    free(state->first_authorization);
    state->host = strdup(request->host);
    state->target = strdup(request->target);
    state->port = request->port;
    state->header_count = request->headers.len;
    state->body_len = request->body_len;
    state->first_authorization = NULL;
    for (size_t i = 0; i < request->headers.len; i++) {
        if (strcmp(request->headers.items[i].name, "Authorization") == 0) {
            state->first_authorization = strdup(request->headers.items[i].value);
        }
    }
    *out_status = state->status;
    if (reason_buf != NULL && reason_cap != 0) {
        snprintf(reason_buf, reason_cap, "%s", state->reason);
    }
    *transport_err = state->transport;
    return CF_OK;
}

static cf_push_vapid *must_vapid(cf_push_vapid *vapid) {
    memset(vapid, 0, sizeof *vapid);
    CF_REQUIRE(cf_push_vapid_init(reference_subject, vapid_public, vapid_private, vapid,
                                  NULL) == CF_OK);
    return vapid;
}

CF_TEST(push_skips_undeliverable_endpoints) {
    /* web_push/tests.rs `skips_endpoints_it_may_not_deliver_to`: gating
     * runs through the real model resolver; the exchange never fires. */
    static const char *skipped[] = {
        "https://fcm.googleapis.com:22/fcm/send/abc",
        "http://fcm.googleapis.com/fcm/send/abc",
        "https://evilfcm.googleapis.com.attacker.example/webhook",
        "https://attacker.example.com/collect",
    };
    cf_push_vapid vapid;
    canned_exchange_state canned;
    must_vapid(&vapid);
    memset(&canned, 0, sizeof canned);
    canned.status = 201;
    for (size_t i = 0; i < sizeof(skipped) / sizeof(skipped[0]); i++) {
        cf_push_subscription sub =
            make_subscription(1, 1, skipped[i], gem_p256dh, gem_auth);
        cf_push_delivery delivery;
        memset(&delivery, 0, sizeof delivery);
        CF_REQUIRE(cf_push_deliver(&sub, "t", "b", "/", 0, &vapid, fake_resolve,
                                   (void *)"142.250.185.206", canned_exchange,
                                   &canned, gem_now, &delivery) == CF_OK);
        CF_CHECK(delivery.skipped);
        CF_CHECK(!delivery.delivered);
        CF_CHECK(!cf_push_delivery_invalidates(&delivery));
        cf_push_delivery_dispose(&delivery);
        cf_push_subscription_dispose(&sub);
    }
    /* Unresolvable hosts skip too (NULL guard answer). */
    {
        cf_push_subscription sub = make_subscription(1, 1, "https://fcm.googleapis.com/x",
                                                    gem_p256dh, gem_auth);
        cf_push_delivery delivery;
        memset(&delivery, 0, sizeof delivery);
        CF_REQUIRE(cf_push_deliver(&sub, "t", "b", "/", 0, &vapid, fake_resolve, NULL,
                                   canned_exchange, &canned, gem_now,
                                   &delivery) == CF_OK);
        CF_CHECK(delivery.skipped);
        cf_push_delivery_dispose(&delivery);
        cf_push_subscription_dispose(&sub);
    }
    CF_CHECK(canned.called == 0);
    cf_push_vapid_dispose(&vapid);
    free(canned.host);
    free(canned.target);
    free(canned.first_authorization);
}

CF_TEST(push_delivers_through_canned_exchange) {
    cf_push_vapid vapid;
    canned_exchange_state canned;
    cf_push_subscription sub =
        make_subscription(7, 42, "https://fcm.googleapis.com/fcm/send/abc", gem_p256dh, gem_auth);
    cf_push_delivery delivery;
    must_vapid(&vapid);
    memset(&canned, 0, sizeof canned);
    memset(&delivery, 0, sizeof delivery);
    canned.status = 201;
    snprintf(canned.reason, sizeof canned.reason, "Created");
    canned.transport = CF_PUSH_TRANSPORT_OK;
    CF_REQUIRE(cf_push_deliver(&sub, "t", "b", "/", 0, &vapid, fake_resolve,
                               (void *)"142.250.185.206", canned_exchange, &canned,
                               gem_now, &delivery) == CF_OK);
    CF_CHECK(delivery.delivered && !delivery.skipped);
    CF_CHECK(!cf_push_delivery_invalidates(&delivery));
    CF_CHECK(delivery.status == 201);
    CF_CHECK(canned.called == 1);
    CF_CHECK(strcmp(canned.host, "fcm.googleapis.com") == 0);
    CF_CHECK(strcmp(canned.target, "/fcm/send/abc") == 0);
    CF_CHECK(canned.port == 443);
    CF_CHECK(canned.header_count == 6);
    CF_REQUIRE(canned.first_authorization != NULL);
    CF_CHECK(strncmp(canned.first_authorization, "vapid t=", 8) == 0);
    CF_CHECK(canned.body_len > 86);
    cf_push_delivery_dispose(&delivery);
    cf_push_subscription_dispose(&sub);
    cf_push_vapid_dispose(&vapid);
    free(canned.host);
    free(canned.target);
    free(canned.first_authorization);
}

CF_TEST(push_only_own_faults_invalidate) {
    /* web_push/tests.rs `only_the_subscriptions_own_faults_invalidate_it`. */
    cf_push_vapid vapid;
    canned_exchange_state canned;
    must_vapid(&vapid);
    memset(&canned, 0, sizeof canned);
    canned.status = 201;
    canned.transport = CF_PUSH_TRANSPORT_OK;
    /* A key that is not a curve point can never be delivered to. */
    {
        cf_push_subscription bad = make_subscription(1, 1, "https://fcm.googleapis.com/x",
                                                    "dGVzdF9rZXk", "dGVzdF9hdXRo");
        cf_push_delivery delivery;
        memset(&delivery, 0, sizeof delivery);
        CF_REQUIRE(cf_push_deliver(&bad, "t", "b", "/", 0, &vapid, fake_resolve,
                                   (void *)"142.250.185.206", canned_exchange, &canned,
                                   gem_now, &delivery) == CF_OK);
        CF_CHECK(strcmp(delivery.class_name, "OpenSSL::PKey::EC::Point::Error") == 0);
        CF_CHECK(cf_push_delivery_invalidates(&delivery));
        cf_push_delivery_dispose(&delivery);
        cf_push_subscription_dispose(&bad);
    }
    /* A TLS failure may be our fault: preserved. */
    {
        cf_push_subscription sub = make_subscription(1, 1, "https://fcm.googleapis.com/x",
                                                    gem_p256dh, gem_auth);
        cf_push_delivery delivery;
        memset(&delivery, 0, sizeof delivery);
        canned.transport = CF_PUSH_TRANSPORT_TLS;
        CF_REQUIRE(cf_push_deliver(&sub, "t", "b", "/", 0, &vapid, fake_resolve,
                                   (void *)"142.250.185.206", canned_exchange, &canned,
                                   gem_now, &delivery) == CF_OK);
        CF_CHECK(strcmp(delivery.class_name, "OpenSSL::SSL::SSLError") == 0);
        CF_CHECK(!cf_push_delivery_invalidates(&delivery));
        cf_push_delivery_dispose(&delivery);
        canned.transport = CF_PUSH_TRANSPORT_OPEN_TIMEOUT;
        memset(&delivery, 0, sizeof delivery);
        CF_REQUIRE(cf_push_deliver(&sub, "t", "b", "/", 0, &vapid, fake_resolve,
                                   (void *)"142.250.185.206", canned_exchange, &canned,
                                   gem_now, &delivery) == CF_OK);
        CF_CHECK(strcmp(delivery.class_name, "Net::OpenTimeout") == 0);
        CF_CHECK(!cf_push_delivery_invalidates(&delivery));
        cf_push_delivery_dispose(&delivery);
        cf_push_subscription_dispose(&sub);
    }
    /* Blank keys are an argument problem: preserved. */
    {
        cf_push_subscription blank =
            make_subscription(1, 1, "https://fcm.googleapis.com/x", "", "dGVzdF9hdXRo");
        cf_push_delivery delivery;
        memset(&delivery, 0, sizeof delivery);
        canned.transport = CF_PUSH_TRANSPORT_OK;
        CF_REQUIRE(cf_push_deliver(&blank, "t", "b", "/", 0, &vapid, fake_resolve,
                                   (void *)"142.250.185.206", canned_exchange, &canned,
                                   gem_now, &delivery) == CF_OK);
        CF_CHECK(strcmp(delivery.class_name, "ArgumentError") == 0);
        CF_CHECK(!cf_push_delivery_invalidates(&delivery));
        cf_push_delivery_dispose(&delivery);
        cf_push_subscription_dispose(&blank);
    }
    /* Gone statuses destroy; other statuses preserve. */
    {
        static const struct {
            unsigned status;
            const char *reason;
            bool gone;
        } rows[] = {
            {410, "Gone", true}, {404, "Not Found", true}, {403, "Forbidden", false},
        };
        for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
            cf_push_subscription sub = make_subscription(1, 1, "https://fcm.googleapis.com/x",
                                                        gem_p256dh, gem_auth);
            cf_push_delivery delivery;
            memset(&delivery, 0, sizeof delivery);
            canned.status = rows[i].status;
            snprintf(canned.reason, sizeof canned.reason, "%s", rows[i].reason);
            CF_REQUIRE(cf_push_deliver(&sub, "t", "b", "/", 0, &vapid, fake_resolve,
                                       (void *)"142.250.185.206", canned_exchange, &canned,
                                       gem_now, &delivery) == CF_OK);
            CF_CHECK(cf_push_delivery_invalidates(&delivery) == rows[i].gone);
            cf_push_delivery_dispose(&delivery);
            cf_push_subscription_dispose(&sub);
        }
    }
    cf_push_vapid_dispose(&vapid);
    free(canned.host);
    free(canned.target);
    free(canned.first_authorization);
}

CF_TEST(push_missing_vapid_is_explicit) {
    /* Lack of configured VAPID keys disables push with an explicit error,
     * never a silent drop. */
    cf_push_subscription sub = make_subscription(1, 1, "https://fcm.googleapis.com/x",
                                                gem_p256dh, gem_auth);
    canned_exchange_state canned;
    cf_push_delivery delivery;
    memset(&canned, 0, sizeof canned);
    memset(&delivery, 0, sizeof delivery);
    CF_REQUIRE(cf_push_deliver(&sub, "t", "b", "/", 0, NULL, fake_resolve,
                               (void *)"142.250.185.206", canned_exchange, &canned,
                               gem_now, &delivery) == CF_OK);
    CF_CHECK(delivery.kind == CF_PUSH_DELIVERY_ARGUMENT);
    CF_CHECK(!delivery.delivered && !delivery.skipped);
    CF_CHECK(!cf_push_delivery_invalidates(&delivery));
    CF_CHECK(strstr(delivery.detail, "VAPID_PUBLIC_KEY") != NULL);
    CF_CHECK(canned.called == 0);
    cf_push_delivery_dispose(&delivery);
    cf_push_subscription_dispose(&sub);
    free(canned.host);
    free(canned.target);
    free(canned.first_authorization);
}

CF_TEST(push_recipients_sort_by_id) {
    /* pool.queue delivers in id order (`find_each`): the helper sorts the
     * recipient vectors before delivery. Ownership itself (user scoping via
     * find_for_user_by_keys, exclusion of the author, involvement filters)
     * lives in the model's pushes_for reads, covered by the D01 suite. */
    cf_push_subscription items[4];
    cf_push_subscription_vector vector;
    memset(items, 0, sizeof items);
    items[0].id = 9;
    items[1].id = 3;
    items[2].id = 12;
    items[3].id = 3 - 2; /* 1 */
    vector.items = items;
    vector.len = 4;
    vector.cap = 4;
    cf_push_sort_by_id(&vector);
    CF_CHECK(items[0].id == 1);
    CF_CHECK(items[1].id == 3);
    CF_CHECK(items[2].id == 9);
    CF_CHECK(items[3].id == 12);
}

CF_TEST(push_payload_caps_are_model_owned) {
    /* The 3 KiB body / 256-byte title caps with valid-UTF-8 ellipsis live
     * in the model's `payload_for` (truncate_json_string), covered by the
     * D01 push_subscription suite; push sends the payload it is given.
     * This case pins the shared constants both sides rely on. */
    CF_CHECK(CF_PUSH_SUBSCRIPTION_MAX_PAYLOAD_TITLE_BYTES == 256);
    CF_CHECK(CF_PUSH_SUBSCRIPTION_MAX_PAYLOAD_BODY_BYTES == 3072);
    CF_CHECK(CF_PUSH_SUBSCRIPTION_MAX_PAYLOAD_TITLE_BYTES ==
             (size_t)256); /* json-escaped bytes, not chars */
}

CF_TEST(push_test_body_is_uuid) {
    char body[37];
    CF_REQUIRE(cf_push_test_body(body) == CF_OK);
    CF_CHECK(strlen(body) == 36);
    CF_CHECK(body[8] == '-' && body[13] == '-' && body[18] == '-' && body[23] == '-');
    CF_CHECK(body[14] == '4');
    CF_CHECK(body[19] == '8' || body[19] == '9' || body[19] == 'a' || body[19] == 'b');
    for (size_t i = 0; i < 36; i++) {
        if (body[i] == '-') continue;
        CF_CHECK((body[i] >= '0' && body[i] <= '9') || (body[i] >= 'a' && body[i] <= 'f'));
    }
}

CF_TEST(push_logs_stay_sanitized) {
    /* No outcome line may carry endpoint or key material verbatim. */
    cf_push_vapid vapid;
    canned_exchange_state canned;
    char line[512];
    must_vapid(&vapid);
    memset(&canned, 0, sizeof canned);
    canned.status = 410;
    snprintf(canned.reason, sizeof canned.reason, "Gone");
    canned.transport = CF_PUSH_TRANSPORT_OK;
    {
        cf_push_subscription sub = make_subscription(1, 1, "https://fcm.googleapis.com/fcm/secret",
                                                    gem_p256dh, gem_auth);
        cf_push_delivery delivery;
        memset(&delivery, 0, sizeof delivery);
        CF_REQUIRE(cf_push_deliver(&sub, "t", "b", "/", 0, &vapid, fake_resolve,
                                   (void *)"142.250.185.206", canned_exchange, &canned,
                                   gem_now, &delivery) == CF_OK);
        cf_push_format_log(&delivery, line, sizeof line);
        CF_CHECK(strstr(line, "fcm/secret") == NULL);
        CF_CHECK(strstr(line, gem_p256dh) == NULL);
        CF_CHECK(strstr(line, "fcm.googleapis.com") != NULL);
        CF_CHECK(strstr(line, "410") != NULL);
        cf_push_delivery_dispose(&delivery);
        cf_push_subscription_dispose(&sub);
    }
    {
        cf_push_subscription bad = make_subscription(1, 1, "https://fcm.googleapis.com/secret",
                                                    "dGVzdF9rZXk", "dGVzdF9hdXRo");
        cf_push_delivery delivery;
        memset(&delivery, 0, sizeof delivery);
        CF_REQUIRE(cf_push_deliver(&bad, "t", "b", "/", 0, &vapid, fake_resolve,
                                   (void *)"142.250.185.206", canned_exchange, &canned,
                                   gem_now, &delivery) == CF_OK);
        cf_push_format_log(&delivery, line, sizeof line);
        CF_CHECK(strstr(line, "secret") == NULL);
        CF_CHECK(strstr(line, "dGVzdF9rZXk") == NULL);
        cf_push_delivery_dispose(&delivery);
        cf_push_subscription_dispose(&bad);
    }
    cf_push_vapid_dispose(&vapid);
    free(canned.host);
    free(canned.target);
    free(canned.first_authorization);
}

/* ================= loopback delivery ===================================== */

typedef struct {
    int port;
    int ready;
    int connections;
    unsigned want_status;
    char want_reason[64];
    char method[16];
    char target[256];
    char content_type[64];
    char ttl[32];
    char urgency[32];
    char encoding[32];
    char authorization[2048];
    long content_length;
    unsigned char *body;
    size_t body_len;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} loopback_server;

static void server_store_header(loopback_server *server, const char *name, const char *value) {
    if (strcasecmp(name, "Content-Type") == 0) {
        snprintf(server->content_type, sizeof server->content_type, "%s", value);
    } else if (strcasecmp(name, "Ttl") == 0) {
        snprintf(server->ttl, sizeof server->ttl, "%s", value);
    } else if (strcasecmp(name, "Urgency") == 0) {
        snprintf(server->urgency, sizeof server->urgency, "%s", value);
    } else if (strcasecmp(name, "Content-Encoding") == 0) {
        snprintf(server->encoding, sizeof server->encoding, "%s", value);
    } else if (strcasecmp(name, "Authorization") == 0) {
        snprintf(server->authorization, sizeof server->authorization, "%s", value);
    } else if (strcasecmp(name, "Content-Length") == 0) {
        server->content_length = atol(value);
    }
}

static ssize_t server_read_line(int fd, char *buf, size_t cap) {
    size_t at = 0;
    while (at + 1 < cap) {
        char c = 0;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return at == 0 ? n : (ssize_t)at;
        if (c == '\n') break;
        if (c != '\r') buf[at++] = c;
    }
    buf[at] = '\0';
    return (ssize_t)at;
}

static void *loopback_thread(void *arg) {
    loopback_server *server = arg;
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof addr;
    int connections = 0;
    if (listener < 0) return NULL;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listener, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(listener);
        return NULL;
    }
    if (listen(listener, 4) != 0) {
        close(listener);
        return NULL;
    }
    if (getsockname(listener, (struct sockaddr *)&addr, &addr_len) != 0) {
        close(listener);
        return NULL;
    }
    pthread_mutex_lock(&server->lock);
    server->port = ntohs(addr.sin_port);
    server->ready = 1;
    pthread_cond_signal(&server->cond);
    pthread_mutex_unlock(&server->lock);
    while (connections < 2) {
        int fd = accept(listener, NULL, NULL);
        char line[4096];
        if (fd < 0) break;
        if (server_read_line(fd, line, sizeof line) <= 0) {
            close(fd);
            break;
        }
        pthread_mutex_lock(&server->lock);
        sscanf(line, "%15s %255s", server->method, server->target);
        server->content_length = -1;
        while (server_read_line(fd, line, sizeof line) > 0) {
            char *colon = strchr(line, ':');
            if (colon != NULL) {
                *colon = '\0';
                const char *value = colon + 1;
                while (*value == ' ' || *value == '\t') value++;
                server_store_header(server, line, value);
            }
        }
        free(server->body);
        server->body = NULL;
        server->body_len = 0;
        if (server->content_length > 0 && server->content_length < 100000) {
            server->body = malloc((size_t)server->content_length);
            if (server->body != NULL) {
                size_t got = 0;
                while (got < (size_t)server->content_length) {
                    ssize_t n = recv(fd, server->body + got,
                                     (size_t)server->content_length - got, 0);
                    if (n <= 0) break;
                    got += (size_t)n;
                }
                server->body_len = got;
            }
        }
        {
            char response[256];
            snprintf(response, sizeof response, "HTTP/1.1 %u %s\r\nContent-Length: 0\r\n"
                                                "Connection: close\r\n\r\n",
                     server->want_status, server->want_reason);
            send(fd, response, strlen(response), 0);
        }
        server->connections++;
        connections++;
        pthread_mutex_unlock(&server->lock);
        close(fd);
    }
    close(listener);
    return NULL;
}

/* Test-only socket exchange: dials the loopback server regardless of the
 * request host (the pinned-IP dial itself belongs to I01's helper). */
static int loopback_port;

static cf_err loopback_exchange(void *ctx, const cf_push_request *request, unsigned *out_status,
                                char *reason_buf, size_t reason_cap,
                                cf_push_transport_error *transport_err) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    char head[8192];
    size_t at = 0;
    char line[256];
    unsigned status = 0;
    char reason[128];
    (void)ctx;
    if (fd < 0 || request == NULL || out_status == NULL || transport_err == NULL) {
        if (fd >= 0) close(fd);
        return CF_INVALID;
    }
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)loopback_port);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        *transport_err = CF_PUSH_TRANSPORT_IO;
        return CF_OK;
    }
    at += (size_t)snprintf(head + at, sizeof head - at, "POST %s HTTP/1.1\r\nHost: %s\r\n",
                           request->target, request->host);
    for (size_t i = 0; i < request->headers.len && at + 256 < sizeof head; i++) {
        at += (size_t)snprintf(head + at, sizeof head - at, "%s: %s\r\n",
                               request->headers.items[i].name,
                               request->headers.items[i].value);
    }
    at += (size_t)snprintf(head + at, sizeof head - at, "Connection: close\r\n\r\n");
    if (send(fd, head, at, 0) != (ssize_t)at ||
        send(fd, request->body, request->body_len, 0) != (ssize_t)request->body_len) {
        close(fd);
        *transport_err = CF_PUSH_TRANSPORT_IO;
        return CF_OK;
    }
    shutdown(fd, SHUT_WR);
    if (server_read_line(fd, line, sizeof line) <= 0) {
        close(fd);
        *transport_err = CF_PUSH_TRANSPORT_READ_TIMEOUT;
        return CF_OK;
    }
    if (sscanf(line, "HTTP/1.1 %u %127[^\n]", &status, reason) < 1) {
        close(fd);
        *transport_err = CF_PUSH_TRANSPORT_IO;
        return CF_OK;
    }
    if (sscanf(line, "HTTP/1.1 %u %127[^\n]", &status, reason) < 2) reason[0] = '\0';
    /* Drain the rest so the server's close is clean. */
    while (server_read_line(fd, line, sizeof line) > 0) {
    }
    close(fd);
    *out_status = status;
    if (reason_buf != NULL && reason_cap != 0) snprintf(reason_buf, reason_cap, "%s", reason);
    *transport_err = CF_PUSH_TRANSPORT_OK;
    return CF_OK;
}

CF_TEST(push_loopback_delivery) {
    /* Local-server delivery without I01's files: the real request bytes
     * cross a real socket; the pinned receiver key opens the body. */
    loopback_server server;
    pthread_t thread;
    cf_push_vapid vapid;
    cf_push_subscription sub;
    cf_push_delivery delivery;
    uint32_t record_size = 0;
    unsigned char *plain = NULL;
    size_t plain_len = 0;
    unsigned char *priv = NULL;
    unsigned char *auth = NULL;
    size_t priv_len = 0;
    size_t auth_len = 0;
    unsigned char receiver_priv[32];
    memset(&server, 0, sizeof server);
    pthread_mutex_init(&server.lock, NULL);
    pthread_cond_init(&server.cond, NULL);
    server.want_status = 201;
    snprintf(server.want_reason, sizeof server.want_reason, "Created");
    CF_REQUIRE(pthread_create(&thread, NULL, loopback_thread, &server) == 0);
    pthread_mutex_lock(&server.lock);
    while (!server.ready) pthread_cond_wait(&server.cond, &server.lock);
    loopback_port = server.port;
    pthread_mutex_unlock(&server.lock);
    CF_REQUIRE(server.port != 0);

    must_vapid(&vapid);
    sub = make_subscription(7, 42, "https://fcm.googleapis.com/fcm/send/abc", gem_p256dh,
                            gem_auth);
    memset(&delivery, 0, sizeof delivery);
    CF_REQUIRE(cf_push_deliver(&sub, "t", "b", "/", 0, &vapid, fake_resolve,
                               (void *)"127.0.0.1", loopback_exchange, NULL, gem_now,
                               &delivery) == CF_OK);
    CF_CHECK(delivery.delivered && !delivery.skipped);
    CF_CHECK(delivery.status == 201);
    /* The server saw the gem's request shape. */
    pthread_mutex_lock(&server.lock);
    CF_CHECK(strcmp(server.method, "POST") == 0);
    CF_CHECK(strcmp(server.target, "/fcm/send/abc") == 0);
    CF_CHECK(strcmp(server.content_type, "application/octet-stream") == 0);
    CF_CHECK(strcmp(server.ttl, "2419200") == 0);
    CF_CHECK(strcmp(server.urgency, "high") == 0);
    CF_CHECK(strcmp(server.encoding, "aes128gcm") == 0);
    CF_CHECK(strncmp(server.authorization, "vapid t=", 8) == 0);
    CF_REQUIRE(server.body != NULL && server.body_len > 86);
    CF_CHECK(server.body_len == (size_t)server.content_length);
    pthread_mutex_unlock(&server.lock);
    /* The pinned receiver opens the delivered body. */
    priv = must_decode(gem_receiver_private, &priv_len);
    auth = must_decode(gem_auth, &auth_len);
    CF_REQUIRE(priv_len == 32);
    memcpy(receiver_priv, priv, 32);
    pthread_mutex_lock(&server.lock);
    CF_REQUIRE(test_decrypt(server.body, server.body_len, receiver_priv, auth, auth_len,
                             &record_size, &plain, &plain_len) == 0);
    pthread_mutex_unlock(&server.lock);
    CF_CHECK(record_size + 86 == (uint32_t)server.body_len);
    {
        char *json = NULL;
        CF_REQUIRE(cf_push_notification_json("t", "b", "/", 0, &json) == CF_OK);
        CF_REQUIRE(plain_len == strlen(json) + 2);
        CF_CHECK(memcmp(plain, json, strlen(json)) == 0);
        CF_CHECK(plain[strlen(json)] == 2 && plain[strlen(json) + 1] == 0);
        free(json);
    }
    /* A 410 from the same server destroys the subscription. */
    pthread_mutex_lock(&server.lock);
    server.want_status = 410;
    snprintf(server.want_reason, sizeof server.want_reason, "Gone");
    pthread_mutex_unlock(&server.lock);
    cf_push_delivery_dispose(&delivery);
    memset(&delivery, 0, sizeof delivery);
    CF_REQUIRE(cf_push_deliver(&sub, "t", "b", "/", 0, &vapid, fake_resolve,
                               (void *)"127.0.0.1", loopback_exchange, NULL, gem_now,
                               &delivery) == CF_OK);
    CF_CHECK(!delivery.delivered && !delivery.skipped);
    CF_CHECK(strcmp(delivery.class_name, "WebPush::ExpiredSubscription") == 0);
    CF_CHECK(cf_push_delivery_invalidates(&delivery));
    pthread_join(thread, NULL);
    cf_push_delivery_dispose(&delivery);
    cf_push_subscription_dispose(&sub);
    cf_push_vapid_dispose(&vapid);
    free(priv);
    free(auth);
    free(plain);
    free(server.body);
    pthread_mutex_destroy(&server.lock);
    pthread_cond_destroy(&server.cond);
}

CF_TEST_MAIN()
