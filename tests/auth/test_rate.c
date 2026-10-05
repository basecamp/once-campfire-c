/* tests/auth/test_rate.c — A01 acceptance AUTH-02's rate-window contract:
 * 10 attempts per fixed 180 s per-IP window, attempt 11 limited, a bounded
 * 4096-IP map with expired entries removed and new IPs rejected while full.
 */
#include "cf_test.h"

#include "app.h"
#include "auth.h"
#include "auth/internal.h"
#include "context.h"
#include "core/testclock.h"

#include "support/auth_env.h"
#include "../app/support/test_request.h"

#include <stdio.h>
#include <string.h>

#define RATE_T0 INT64_C(1767272400000000) /* 2026-01-01T12:00:00Z */

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* One request from `ip`; returns whether the limiter rejected it. */
static bool try_ip(auth_env *env, const char *ip, cf_err *out_rc) {
    cf_request req;
    cf_test_req_init(&req);
    req.path = SP("/session");
    req.peer_ip = SP(ip);
    cf_response resp;
    cf_ctx ctx;
    cf_err rc = auth_env_ctx(env, &req, &resp, &ctx);
    if (rc != CF_OK) {
        *out_rc = rc;
        return false;
    }
    bool limited = false;
    rc = cf_auth_sign_in_rate_limit(&ctx, &limited);
    *out_rc = rc;
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);
    return limited;
}

CF_TEST(fixed_window_ten_attempts_per_ip) {
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    cf_test_clock_set_fixed_us(RATE_T0);
    cf_auth_rate_limit_test_reset();
    cf_err rc = CF_OK;
    for (int attempt = 1; attempt <= 10; attempt++) {
        bool limited = try_ip(&env, "198.51.100.1", &rc);
        CF_REQUIRE(rc == CF_OK);
        if (limited) printf("    attempt %d unexpectedly limited\n", attempt);
        CF_CHECK(!limited);
    }
    CF_CHECK(try_ip(&env, "198.51.100.1", &rc)); /* attempt 11 */
    CF_REQUIRE(rc == CF_OK);
    /* A different IP has its own window. */
    CF_CHECK(!try_ip(&env, "198.51.100.2", &rc));
    /* The window does not slide: one microsecond before it expires the entry
     * still counts; at expires_at it is removed (the Rust `expires_at > now`
     * retain) and a fresh window starts. */
    cf_test_clock_set_fixed_us(RATE_T0 + CF_AUTH_SIGN_IN_WINDOW_US - 1);
    CF_CHECK(try_ip(&env, "198.51.100.1", &rc)); /* still inside */
    cf_test_clock_set_fixed_us(RATE_T0 + CF_AUTH_SIGN_IN_WINDOW_US);
    CF_CHECK(!try_ip(&env, "198.51.100.1", &rc)); /* fresh window */
    cf_auth_rate_limit_test_reset();
    cf_test_clock_clear();
    auth_env_close(&env);
}

CF_TEST(map_cap_rejects_new_ips_without_eviction) {
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    cf_test_clock_set_fixed_us(RATE_T0);
    cf_auth_rate_limit_test_reset();
    cf_err rc = CF_OK;
    char ip[32];
    for (int i = 0; i < CF_AUTH_SIGN_IN_MAP_CAP; i++) {
        snprintf(ip, sizeof ip, "10.%d.%d.%d", (i >> 16) & 0xff,
                 (i >> 8) & 0xff, i & 0xff);
        if (try_ip(&env, ip, &rc)) {
            printf("    unexpectedly limited at entry %d\n", i);
            CF_CHECK(false);
            break;
        }
        if (rc != CF_OK) break;
    }
    /* A new IP while the live map is full: rejected (no eviction). */
    CF_CHECK(try_ip(&env, "203.0.113.250", &rc));
    CF_REQUIRE(rc == CF_OK);
    /* Existing entries keep counting: 10 more attempts on one of them cross
     * the limit even though the map is full. */
    snprintf(ip, sizeof ip, "10.0.0.0");
    for (int i = 0; i < 9; i++) {
        CF_CHECK(!try_ip(&env, ip, &rc));
        CF_REQUIRE(rc == CF_OK);
    }
    CF_CHECK(try_ip(&env, ip, &rc));
    /* Expiry frees room: a new IP is accepted once the window has passed. */
    cf_test_clock_set_fixed_us(RATE_T0 + CF_AUTH_SIGN_IN_WINDOW_US + 1);
    CF_CHECK(!try_ip(&env, "203.0.113.250", &rc));
    cf_auth_rate_limit_test_reset();
    cf_test_clock_clear();
    auth_env_close(&env);
}

CF_TEST_MAIN()
