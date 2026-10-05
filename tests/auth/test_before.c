/* tests/auth/test_before.c — A01 acceptance AUTH-03 (safe/unsafe method x
 * HTTP/HTTPS x Origin x Fetch-Site), the banned-IP 429, the auth policies and
 * bot restrictions of the before-action chain, and room authorization.
 *
 * The expectation table follows kit/src/ctx.rs verify_authenticity_token and
 * concerns.rs reject_banned_ip/deny_bots, as specified by 02-data-auth.md
 * ("A01"): GET/HEAD pass; Origin null or mismatched rejects; same-origin/
 * same-site accepted; cross-site/none/invalid rejected; a missing Sec-Fetch-
 * Site is accepted only on plain HTTP with SSL disabled; HTTPS without the
 * header is 422; the Origin comparison stays even for same-site.
 */
#include "cf_test.h"

#include "app.h"
#include "auth.h"
#include "context.h"
#include "db/writer.h"

#include "support/auth_env.h"
#include "../app/support/test_request.h"

#include <string.h>

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* One CSRF check for an already-built request. */
static unsigned csrf_status(auth_env *env, cf_request *req) {
    cf_response resp;
    cf_ctx ctx;
    CF_REQUIRE(auth_env_ctx(env, req, &resp, &ctx) == CF_OK);
    cf_err rc = cf_check_csrf(&ctx, false);
    unsigned status = resp.status;
    CF_CHECK(rc == CF_OK);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);
    return status;
}

static unsigned csrf_case(auth_env *env, cf_method method, bool tls,
                          const char *origin, const char *fetch_site) {
    cf_request req;
    cf_test_req_init(&req);
    req.method = method;
    req.original_method = method;
    req.tls = tls;
    req.path = SP("/session");
    if (origin != NULL) {
        CF_REQUIRE(cf_test_req_header(&req, SP("Origin"), SP(origin)) == CF_OK);
    }
    if (fetch_site != NULL) {
        CF_REQUIRE(cf_test_req_header(&req, SP("Sec-Fetch-Site"),
                                      SP(fetch_site)) == CF_OK);
    }
    return csrf_status(env, &req);
}

static unsigned csrf_host_case(auth_env *env, const char *host,
                               const char *origin, const char *fetch_site) {
    cf_request req;
    cf_test_req_init(&req);
    req.method = CF_POST;
    req.original_method = CF_POST;
    req.path = SP("/session");
    CF_REQUIRE(cf_test_req_header(&req, SP("Host"), SP(host)) == CF_OK);
    if (origin != NULL) {
        CF_REQUIRE(cf_test_req_header(&req, SP("Origin"), SP(origin)) == CF_OK);
    }
    if (fetch_site != NULL) {
        CF_REQUIRE(cf_test_req_header(&req, SP("Sec-Fetch-Site"),
                                      SP(fetch_site)) == CF_OK);
    }
    return csrf_status(env, &req);
}

CF_TEST(csrf_method_http_origin_fetchsite_matrix) {
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    const char *own = "http://127.0.0.1:32123";

    struct {
        cf_method method;
        bool tls;
        const char *origin;
        const char *fetch_site;
        unsigned expected;
    } cases[] = {
        {CF_GET, false, NULL, NULL, 0},
        {CF_HEAD, false, NULL, NULL, 0},
        {CF_POST, false, NULL, NULL, 0},
        {CF_POST, false, NULL, "same-origin", 0},
        {CF_POST, false, NULL, "same-site", 0},
        {CF_POST, false, NULL, "cross-site", 422},
        {CF_POST, false, NULL, "none", 422},
        {CF_POST, false, NULL, "banana", 422},
        {CF_POST, false, own, NULL, 0},
        {CF_POST, true, NULL, NULL, 422},
        {CF_POST, true, NULL, "same-origin", 0},
        /* HTTPS without Sec-Fetch-Site is 422 even when Origin matches. */
        {CF_POST, true, own, NULL, 422},
        {CF_POST, true, "null", "same-origin", 422},
        {CF_POST, true, "http://evil.test", "same-site", 422},
        {CF_POST, true, own, "cross-site", 422},
        {CF_OPTIONS, false, NULL, NULL, 0},
        {CF_PUT, false, NULL, NULL, 0},
        {CF_DELETE, true, NULL, "same-site", 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        unsigned status = csrf_case(&env, cases[i].method, cases[i].tls,
                                    cases[i].origin, cases[i].fetch_site);
        if (status != cases[i].expected) {
            printf("    case %zu: expected %u got %u\n", i,
                   cases[i].expected, status);
            CF_CHECK(status == cases[i].expected);
        }
    }
    auth_env_close(&env);
}

CF_TEST(csrf_origin_compares_against_request_base_url) {
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    /* The base URL uses the Host header and omits a default port, like the
     * pinned request.base_url. */
    CF_CHECK(csrf_host_case(&env, "127.0.0.1:32123",
                            "http://127.0.0.1:32123", "same-origin") == 0);
    CF_CHECK(csrf_host_case(&env, "127.0.0.1:32123", "http://127.0.0.1",
                            "same-origin") == 422);
    CF_CHECK(csrf_host_case(&env, "127.0.0.1", "http://127.0.0.1",
                            "same-origin") == 0);
    CF_CHECK(csrf_host_case(&env, "127.0.0.1", "http://127.0.0.1:80",
                            "same-origin") == 422);
    auth_env_close(&env);
}

CF_TEST(csrf_force_ssl_plain_http_missing_header_is_422) {
    auth_env env;
    CF_REQUIRE(auth_env_open_opts(&env, true));
    CF_CHECK(csrf_case(&env, CF_POST, false, NULL, NULL) == 422);
    CF_CHECK(csrf_case(&env, CF_POST, false, NULL, "same-origin") == 0);
    CF_CHECK(csrf_case(&env, CF_GET, false, NULL, NULL) == 0);
    auth_env_close(&env);
}

static unsigned before_status(auth_env *env, cf_request *req, cf_before policy) {
    cf_response resp;
    cf_ctx ctx;
    CF_REQUIRE(auth_env_ctx(env, req, &resp, &ctx) == CF_OK);
    cf_err rc = cf_before_actions(&ctx, policy);
    unsigned status = resp.status;
    CF_CHECK(rc == CF_OK);
    /* Keep the context alive through cookie assertions where needed; here the
     * status alone is the observation. */
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);
    return status;
}

CF_TEST(banned_ip_returns_429_on_unsafe_methods) {
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    int64_t user = auth_seed_user(&env, "Banned", "ban@example.com", "$2a$04$",
                                  0, 0);
    CF_REQUIRE(user != 0);
    CF_REQUIRE(auth_exec(&env,
        "INSERT INTO bans (created_at,ip_address,updated_at,user_id) VALUES "
        "('2026-01-02 03:04:05','203.0.113.9','2026-01-02 03:04:05',1)"));

    cf_before skipped = {CF_AUTH_SKIPPED, false, false};
    cf_request req;
    cf_test_req_init(&req);
    req.method = CF_POST;
    req.original_method = CF_POST;
    req.peer_ip = SP("203.0.113.9");
    req.path = SP("/session");
    CF_CHECK(before_status(&env, &req, skipped) == 429);

    cf_test_req_init(&req);
    req.peer_ip = SP("203.0.113.9");
    req.path = SP("/rooms");
    CF_CHECK(before_status(&env, &req, skipped) == 0);

    cf_test_req_init(&req);
    req.method = CF_DELETE;
    req.original_method = CF_DELETE;
    req.peer_ip = SP("203.0.113.9");
    req.path = SP("/rooms/1");
    CF_CHECK(before_status(&env, &req, skipped) == 429);

    cf_test_req_init(&req);
    req.method = CF_POST;
    req.original_method = CF_POST;
    req.peer_ip = SP("198.51.100.7");
    req.path = SP("/session");
    CF_CHECK(before_status(&env, &req, skipped) == 0);
    auth_env_close(&env);
}

CF_TEST(required_auth_redirects_to_sign_in_and_remembers_return_to) {
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    cf_request req;
    cf_test_req_init(&req);
    req.path = SP("/rooms/7");
    req.query = SP("page=2");
    cf_response resp;
    cf_ctx ctx;
    CF_REQUIRE(auth_env_ctx(&env, &req, &resp, &ctx) == CF_OK);
    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    CF_REQUIRE(cf_before_actions(&ctx, policy) == CF_OK);
    CF_CHECK(cf_auth_halted(&ctx));
    CF_CHECK(resp.status == 302);
    /* The return-to value is stored in the encrypted session cookie, which the
     * redirect's response carries. */
    cf_span session_cookie;
    CF_CHECK(cf_ctx_cookie_get(&ctx, SP("_campfire_session"),
                               &session_cookie) == CF_OK);
    CF_CHECK(cf_finish_cookies(&ctx) == CF_OK);
    CF_CHECK(resp.headers != NULL);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);
    auth_env_close(&env);
}

CF_TEST(authorize_room_scopes_to_membership) {
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    int64_t user = auth_seed_user(&env, "Member", "m@example.com", NULL, 0, 0);
    int64_t other = auth_seed_user(&env, "Other", "o@example.com", NULL, 0, 0);
    CF_REQUIRE(user != 0 && other != 0);
    auth_seed_room_membership(&env, 1001, user, "Rooms::Open");
    auth_seed_room_membership(&env, 1002, other, "Rooms::Open");

    CF_CHECK(cf_authorize_room(env.reader, user, 1001) == CF_OK);
    /* another user's room: denied */
    CF_CHECK(cf_authorize_room(env.reader, user, 1002) == CF_NOT_FOUND);
    /* a room with no membership at all: denied */
    CF_CHECK(cf_authorize_room(env.reader, user, 9999) == CF_NOT_FOUND);
    auth_env_close(&env);
}

CF_TEST(bot_authentication_and_csrf_exemption) {
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    int64_t bot = auth_seed_bot(&env, "Bot", "tok1234567890abcdef");
    CF_REQUIRE(bot != 0);

    char valid_key[64];
    snprintf(valid_key, sizeof valid_key, "%lld-tok1234567890abcdef",
             (long long)bot);

    /* Valid bot + deny_bots=false + cross-site CSRF: the bot exempts itself
     * from forgery protection, so the chain completes. */
    cf_request req;
    cf_test_req_init(&req);
    req.method = CF_POST;
    req.original_method = CF_POST;
    req.path = SP("/rooms/1/messages");
    char body[80];
    snprintf(body, sizeof body, "bot_key=%s", valid_key);
    req.body = SP(body);
    CF_REQUIRE(cf_test_req_header(
                   &req, SP("Content-Type"),
                   SP("application/x-www-form-urlencoded")) == CF_OK);
    CF_REQUIRE(cf_test_req_header(&req, SP("Origin"), SP("http://evil.test")) ==
               CF_OK);
    CF_REQUIRE(cf_test_req_header(&req, SP("Sec-Fetch-Site"),
                                  SP("cross-site")) == CF_OK);
    cf_response resp;
    cf_ctx ctx;
    CF_REQUIRE(auth_env_ctx(&env, &req, &resp, &ctx) == CF_OK);
    cf_before allow_bots = {CF_AUTH_REQUIRED, false, true};
    CF_REQUIRE(cf_before_actions(&ctx, allow_bots) == CF_OK);
    CF_CHECK(!cf_auth_halted(&ctx));
    CF_CHECK(ctx.identity.kind == CF_AUTH_BOT);
    CF_CHECK(ctx.identity.user_id == bot);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);

    /* deny_bots is a 403 for the same authenticated bot. */
    cf_before deny = {CF_AUTH_REQUIRED, true, true};
    CF_REQUIRE(auth_env_ctx(&env, &req, &resp, &ctx) == CF_OK);
    CF_REQUIRE(cf_before_actions(&ctx, deny) == CF_OK);
    CF_CHECK(cf_auth_halted(&ctx));
    CF_CHECK(resp.status == 403);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);

    /* An unknown bot key falls through to the sign-in redirect (302). */
    cf_request bad;
    cf_test_req_init(&bad);
    bad.method = CF_POST;
    bad.original_method = CF_POST;
    bad.path = SP("/rooms/1/messages");
    bad.body = SP("bot_key=999-not-a-token");
    CF_REQUIRE(cf_test_req_header(
                   &bad, SP("Content-Type"),
                   SP("application/x-www-form-urlencoded")) == CF_OK);
    CF_REQUIRE(auth_env_ctx(&env, &bad, &resp, &ctx) == CF_OK);
    CF_REQUIRE(cf_before_actions(&ctx, allow_bots) == CF_OK);
    CF_CHECK(cf_auth_halted(&ctx));
    CF_CHECK(resp.status == 302);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);
    auth_env_close(&env);
}

CF_TEST_MAIN()
