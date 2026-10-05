/* tests/auth/test_password.c — A01's password service at the model boundary:
 * cf_user_authenticate / cf_user_authenticated (src/models/user.c) via
 * cf_password_verify, the pinned 72-byte bcrypt truncation, and the injected
 * test cost (config injects 4; never an environment name).
 */
#include "cf_test.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "models/user.h"

#include "support/auth_env.h"
#include "vectors.h"

#include <string.h>

#define LONG_72 \
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define LONG_79 LONG_72 "ignored"
#define LONG_71 \
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

CF_TEST(config_injects_the_test_bcrypt_cost) {
    cf_config_entry entries[] = {
        {"PUBLIC_ORIGIN", "http://127.0.0.1:32123"},
        {"SECRET_KEY_BASE", AUTH_VEC_SECRET_KEY_BASE},
    };
    cf_config *config = NULL;
    CF_REQUIRE(cf_config_parse(entries, 2, NULL, &config) == CF_OK);
    CF_CHECK(config->bcrypt_cost == CF_BCRYPT_COST);
    cf_config_test_set_bcrypt_cost(config, 4);
    CF_CHECK(config->bcrypt_cost == 4);
    cf_config_test_set_bcrypt_cost(config, 3);
    CF_CHECK(config->bcrypt_cost == 4); /* outside 4..31 is ignored */
    cf_config_destroy(config);
}

CF_TEST(user_authenticate_honors_the_72_byte_limit) {
    cf_user user;
    memset(&user, 0, sizeof user);
    /* The pinned digest of 72 'a's. */
    static const char digest[] =
        "$2a$12$TZrwCKzycdLOZ2HeWOMW1OYHE8nrAWHoOY05FWLQ6N38GSUKn4DMW";
    user.password_digest = (cf_optional_str){
        .present = true,
        .value = {.ptr = (char *)digest, .len = sizeof digest - 1}};

    bool ok = false;
    CF_REQUIRE(cf_user_authenticate(&user, CF_STR_LIT(LONG_72), &ok) == CF_OK);
    CF_CHECK(ok);
    ok = false;
    CF_REQUIRE(cf_user_authenticate(&user, CF_STR_LIT(LONG_79), &ok) == CF_OK);
    CF_CHECK(ok); /* bytes beyond 72 are ignored */
    ok = true;
    CF_REQUIRE(cf_user_authenticate(&user, CF_STR_LIT(LONG_71), &ok) == CF_OK);
    CF_CHECK(!ok);
    ok = true;
    CF_REQUIRE(cf_user_authenticate(&user, CF_STR_LIT("anything"), &ok) == CF_OK);
    CF_CHECK(!ok);

    /* Absent or empty digests never verify. */
    cf_user none;
    memset(&none, 0, sizeof none);
    ok = true;
    CF_REQUIRE(cf_user_authenticate(&none, CF_STR_LIT("x"), &ok) == CF_OK);
    CF_CHECK(!ok);
    none.password_digest = (cf_optional_str){.present = true, .value = {NULL, 0}};
    ok = true;
    CF_REQUIRE(cf_user_authenticate(&none, CF_STR_LIT("x"), &ok) == CF_OK);
    CF_CHECK(!ok);
}

CF_TEST(authenticated_runs_the_dummy_for_an_unknown_user) {
    bool ok = true;
    /* candidate NULL: the constant dummy verification runs, result false. */
    CF_REQUIRE(cf_user_authenticated(NULL, CF_STR_LIT("secret123456"), &ok) ==
               CF_OK);
    CF_CHECK(!ok);
    /* Empty password: no lookup, no verification. */
    ok = true;
    CF_REQUIRE(cf_user_authenticated(NULL, CF_STR_LIT(""), &ok) == CF_OK);
    CF_CHECK(!ok);

    /* A real candidate with the pinned seeded digest verifies. */
    cf_user user;
    memset(&user, 0, sizeof user);
    static const char digest[] =
        "$2a$12$kDVPfUd.VKsV9HzG8RHRFu0fyaW7gije1Krza2.A0J5S8XEcOQT4S";
    user.password_digest = (cf_optional_str){
        .present = true,
        .value = {.ptr = (char *)digest, .len = sizeof digest - 1}};
    ok = false;
    CF_REQUIRE(cf_user_authenticated(&user, CF_STR_LIT("secret123456"), &ok) ==
               CF_OK);
    CF_CHECK(ok);
}

CF_TEST(digest_creation_and_round_trip_at_cost_4) {
    cf_str digest = {0};
    CF_REQUIRE(cf_auth_password_digest(CF_STR_LIT("secret123456"), 4, &digest) ==
               CF_OK);
    CF_CHECK(digest.len == 60);
    CF_CHECK(strncmp(digest.ptr, "$2a$04$", 7) == 0);
    CF_CHECK(cf_password_verify(CF_STR_LIT("secret123456"), digest));
    CF_CHECK(!cf_password_verify(CF_STR_LIT("wrong"), digest));
    cf_str_dispose(&digest);
    CF_CHECK(cf_auth_password_digest(CF_STR_LIT("x"),
                                     CF_AUTH_BCRYPT_MIN_COST - 1,
                                     &digest) == CF_INVALID);
    CF_CHECK(cf_auth_password_digest(CF_STR_LIT("x"),
                                     CF_AUTH_BCRYPT_MAX_COST + 1,
                                     &digest) == CF_INVALID);
}

CF_TEST_MAIN()
