/* tests/auth/test_tokens.c — A01 unit tests for the signed-ID family:
 * ActiveRecord::SignedId, app verifiers (Active Storage), GlobalID /
 * SignedGlobalID and Turbo stream names, against the pinned current-format
 * vectors. */
#include "cf_test.h"

#include "auth.h"
#include "auth/internal.h"

#include "vectors.h"

#include <stdio.h>
#include <string.h>

static cf_span S(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

static int64_t vec_now_us(const char *iso) {
    int64_t ns = 0;
    CF_REQUIRE(auth_iso8601_parse_ns(S(iso), &ns) == CF_OK);
    return ns / 1000;
}

static bool str_is(cf_str value, const char *want) {
    size_t len = strlen(want);
    return value.ptr != NULL && value.len == len &&
           memcmp(value.ptr, want, len) == 0;
}

CF_TEST(global_id_vectors) {
    for (size_t i = 0; i < AUTH_VEC_GIDS_LEN; i++) {
        const auth_vec_gid *vec = &AUTH_VEC_GIDS[i];
        cf_str app = {0}, model = {0}, id = {0};
        bool found = false;
        CF_REQUIRE(cf_auth_global_id_parse(S(vec->gid), &app, &model, &id,
                                           &found) == CF_OK);
        CF_CHECK(found);
        CF_CHECK(str_is(model, vec->model_name));
        CF_CHECK(str_is(id, vec->id));
        cf_str param = {0};
        CF_REQUIRE(cf_auth_global_id_param(S(vec->gid), &param) == CF_OK);
        CF_CHECK(str_is(param, vec->param));
        cf_str_dispose(&app);
        cf_str_dispose(&model);
        cf_str_dispose(&id);
        cf_str_dispose(&param);
    }
    cf_str app = {0}, model = {0}, id = {0};
    bool found = true;
    CF_CHECK(cf_auth_global_id_parse(S("gid://campfire/User"), &app, &model,
                                     &id, &found) == CF_OK);
    CF_CHECK(!found);
    cf_str_dispose(&app);
    cf_str_dispose(&model);
    cf_str_dispose(&id);
}

CF_TEST(signed_id_generate_vectors) {
    for (size_t i = 0; i < AUTH_VEC_SID_GEN_LEN; i++) {
        const auth_vec_sid_gen *vec = &AUTH_VEC_SID_GEN[i];
        bool has_expiry = vec->expires_at != NULL;
        int64_t expires_us = has_expiry ? vec_now_us(vec->expires_at) : 0;
        cf_str generated = {0};
        CF_REQUIRE(cf_auth_signed_id_generate(
                       S(AUTH_VEC_SECRET_KEY_BASE), S(vec->model), vec->id,
                       vec->purpose ? S(vec->purpose) : (cf_span){NULL, 0},
                       vec->purpose != NULL, has_expiry, expires_us,
                       &generated) == CF_OK);
        if (!str_is(generated, vec->signed_id)) {
            printf("    case %zu model=%s id=%lld: got %s\n", i, vec->model,
                   (long long)vec->id, generated.ptr);
            CF_CHECK(false);
        }
        cf_str_dispose(&generated);
    }
}

CF_TEST(signed_id_verify_vectors) {
    for (size_t i = 0; i < AUTH_VEC_SID_VERIFY_LEN; i++) {
        const auth_vec_sid_verify *vec = &AUTH_VEC_SID_VERIFY[i];
        cf_optional_i64 id = {0};
        bool found = false;
        CF_REQUIRE(cf_auth_signed_id_verify(
                       S(AUTH_VEC_SECRET_KEY_BASE), S(vec->model),
                       S(vec->signed_id),
                       vec->purpose ? S(vec->purpose) : (cf_span){NULL, 0},
                       vec->purpose != NULL, vec_now_us(vec->now), &id,
                       &found) == CF_OK);
        if (vec->has_expected) {
            if (!found) {
                printf("    %s: expected %lld\n", vec->case_name,
                       (long long)vec->expected);
                CF_CHECK(found);
            } else {
                CF_CHECK(id.value == vec->expected);
            }
        } else {
            CF_CHECK(!found);
        }
    }
}

CF_TEST(sgid_generate_vectors) {
    for (size_t i = 0; i < AUTH_VEC_SGID_GEN_LEN; i++) {
        const auth_vec_sgid_gen *vec = &AUTH_VEC_SGID_GEN[i];
        cf_str generated = {0};
        if (vec->data != NULL && strstr(vec->data, "?expires_in") != NULL) {
            CF_REQUIRE(cf_auth_sgid_generate_attachable(
                           S(AUTH_VEC_SECRET_KEY_BASE), S(vec->gid),
                           &generated) == CF_OK);
        } else {
            bool has_expiry = vec->expires_at != NULL;
            int64_t expires_us =
                has_expiry ? vec_now_us(vec->expires_at) : 0;
            CF_REQUIRE(cf_auth_sgid_generate(
                           S(AUTH_VEC_SECRET_KEY_BASE), S(vec->gid),
                           S(vec->purpose), has_expiry, expires_us,
                           &generated) == CF_OK);
        }
        if (!str_is(generated, vec->sgid)) {
            printf("    case %zu gid=%s: got %s\n", i, vec->gid,
                   generated.ptr);
            CF_CHECK(false);
        }
        cf_str_dispose(&generated);
    }
}

CF_TEST(sgid_verify_vectors) {
    for (size_t i = 0; i < AUTH_VEC_SGID_VERIFY_LEN; i++) {
        const auth_vec_sgid_verify *vec = &AUTH_VEC_SGID_VERIFY[i];
        cf_str gid = {0};
        bool found = false;
        CF_REQUIRE(cf_auth_sgid_locate(
                       S(AUTH_VEC_SECRET_KEY_BASE), S(vec->sgid),
                       S(vec->purpose), vec_now_us(vec->now), &gid,
                       &found) == CF_OK);
        if (vec->has_expected) {
            if (!found) {
                printf("    %s: expected %s\n", vec->case_name,
                       vec->expected_gid);
                CF_CHECK(found);
            } else {
                CF_CHECK(str_is(gid, vec->expected_gid));
            }
        } else {
            CF_CHECK(!found);
        }
        cf_str_dispose(&gid);
    }
}

CF_TEST(turbo_generate_vectors) {
    for (size_t i = 0; i < AUTH_VEC_TURBO_GEN_LEN; i++) {
        const auth_vec_turbo_gen *vec = &AUTH_VEC_TURBO_GEN[i];
        cf_span parts[8];
        for (size_t k = 0; k < vec->parts_len; k++) parts[k] = S(vec->parts[k]);
        cf_str signed_name = {0};
        CF_REQUIRE(cf_auth_turbo_signed_stream_name(
                       S(AUTH_VEC_SECRET_KEY_BASE), parts, vec->parts_len,
                       &signed_name) == CF_OK);
        CF_CHECK(str_is(signed_name, vec->signed_name));
        cf_str_dispose(&signed_name);
    }
}

CF_TEST(turbo_verify_vectors) {
    for (size_t i = 0; i < AUTH_VEC_TURBO_VERIFY_LEN; i++) {
        const auth_vec_turbo_verify *vec = &AUTH_VEC_TURBO_VERIFY[i];
        cf_str name = {0};
        bool found = false;
        CF_REQUIRE(cf_auth_turbo_verified_stream_name(
                       S(AUTH_VEC_SECRET_KEY_BASE), S(vec->signed_name), &name,
                       &found) == CF_OK);
        if (vec->has_expected) {
            if (!found) {
                printf("    %s: expected %s\n", vec->case_name, vec->expected);
                CF_CHECK(found);
            } else {
                CF_CHECK(str_is(name, vec->expected));
            }
        } else {
            CF_CHECK(!found);
        }
        cf_str_dispose(&name);
    }
}

CF_TEST(app_verifier_generate_vectors) {
    for (size_t i = 0; i < AUTH_VEC_APP_GEN_LEN; i++) {
        const auth_vec_app_gen *vec = &AUTH_VEC_APP_GEN[i];
        bool has_expiry = vec->expires_at != NULL;
        int64_t expires_us = has_expiry ? vec_now_us(vec->expires_at) : 0;
        cf_str message = {0};
        CF_REQUIRE(cf_auth_app_verifier_generate(
                       S(AUTH_VEC_SECRET_KEY_BASE), S(vec->name),
                       S(vec->data_json),
                       vec->purpose ? S(vec->purpose) : (cf_span){NULL, 0},
                       vec->purpose != NULL, has_expiry, expires_us,
                       &message) == CF_OK);
        if (!str_is(message, vec->message)) {
            printf("    case %zu name=%s: got %s\n", i, vec->name,
                   message.ptr);
            CF_CHECK(false);
        }
        cf_str_dispose(&message);
    }
}

CF_TEST(app_verifier_verify_vectors) {
    for (size_t i = 0; i < AUTH_VEC_APP_VERIFY_LEN; i++) {
        const auth_vec_app_verify *vec = &AUTH_VEC_APP_VERIFY[i];
        cf_str data = {0};
        bool found = false;
        CF_REQUIRE(cf_auth_app_verifier_verify(
                       S(AUTH_VEC_SECRET_KEY_BASE), S(vec->name),
                       S(vec->message),
                       vec->purpose ? S(vec->purpose) : (cf_span){NULL, 0},
                       vec->purpose != NULL, vec_now_us(vec->now), &data,
                       &found) == CF_OK);
        if (vec->has_expected) {
            if (!found) {
                printf("    %s: expected %s\n", vec->case_name,
                       vec->expected_json);
                CF_CHECK(found);
            } else {
                CF_CHECK(str_is(data, vec->expected_json));
            }
        } else {
            CF_CHECK(!found);
        }
        cf_str_dispose(&data);
    }
}

/* A valid HMAC over a payload the reference's Base64.urlsafe_decode64 rejects
 * is InvalidSignature there (message_verifier.rs read_message: the decode
 * error becomes `.ok_or(Error::InvalidSignature)`), i.e. unauthenticated:
 * found=false with CF_OK, like every other malformed input (auth.h). */
CF_TEST(verifier_non_base64_payload_reads_unauthenticated) {
    unsigned char key[64];
    CF_REQUIRE(auth_pbkdf2_sha256(S(AUTH_VEC_SECRET_KEY_BASE),
                                  S("ActiveStorage"), sizeof key,
                                  key) == CF_OK);
    /* Rejected by urlsafe_decode: invalid characters, partial padding,
     * non-zero trailing bits, and a lone-padded quad. */
    const char *payloads[] = {"not base64!", "QQ=", "QR", "%25", "="};
    for (size_t i = 0; i < sizeof payloads / sizeof payloads[0]; i++) {
        cf_span encoded = S(payloads[i]);
        cf_str hex = {0};
        CF_REQUIRE(auth_hmac_hex(CF_AUTH_DIGEST_SHA1,
                                 (cf_span){key, sizeof key}, encoded,
                                 &hex) == CF_OK);
        cf_builder message = {0};
        CF_REQUIRE(cf_builder_append(&message, encoded) == CF_OK);
        CF_REQUIRE(cf_builder_append(&message, S("--")) == CF_OK);
        CF_REQUIRE(cf_builder_append(
                       &message,
                       (cf_span){(const unsigned char *)hex.ptr, hex.len}) ==
                   CF_OK);
        cf_str data = {0};
        bool found = true;
        cf_err rc = cf_auth_app_verifier_verify(
            S(AUTH_VEC_SECRET_KEY_BASE), S("ActiveStorage"),
            (cf_span){message.ptr, message.len}, (cf_span){NULL, 0}, false,
            INT64_C(1767272400000000), &data, &found);
        if (rc != CF_OK || found) {
            printf("    payload %s: rc=%s found=%d\n", payloads[i],
                   cf_err_name(rc), (int)found);
        }
        CF_CHECK(rc == CF_OK);
        CF_CHECK(!found);
        cf_str_dispose(&data);
        cf_builder_dispose(&message);
        cf_str_dispose(&hex);
    }
}

CF_TEST_MAIN()
