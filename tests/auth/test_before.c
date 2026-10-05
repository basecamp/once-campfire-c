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
#include "auth/platform.h"
#include "context.h"
#include "db/writer.h"
#include "http/http_internal.h"
#include "routes.h"
#include "views.h"

#include "support/auth_env.h"
#include "../app/support/test_request.h"

#include <string.h>

#ifndef CF_APP_VERSION
#define CF_APP_VERSION "0"
#endif

/* The route double provides no endpoint table; src/routes.c owns the real
 * cf_route_by_id and before.c references it weakly.  This test binary defines
 * its own weak lookup so the messages-own-layout rule is observable. */
static cf_route before_test_route;

const cf_route *cf_route_by_id(uint32_t id) __attribute__((weak));
__attribute__((weak)) const cf_route *cf_route_by_id(uint32_t id) {
    return id != 0 && id == before_test_route.id ? &before_test_route : NULL;
}

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
    /* The base URL for the Origin comparison comes from the Host header
     * (PUBLIC_ORIGIN's host:port here, so a matching Origin can be formed). */
    CF_REQUIRE(cf_test_req_header(&req, SP("Host"),
                                  SP("127.0.0.1:32123")) == CF_OK);
    if (origin != NULL) {
        CF_REQUIRE(cf_test_req_header(&req, SP("Origin"), SP(origin)) == CF_OK);
    }
    if (fetch_site != NULL) {
        CF_REQUIRE(cf_test_req_header(&req, SP("Sec-Fetch-Site"),
                                      SP(fetch_site)) == CF_OK);
    }
    return csrf_status(env, &req);
}

/* Same, for an HTTP/1.0-style request with no Host header at all: the pin's
 * base_url is scheme + "localhost" then (request.rs:160-166), not
 * PUBLIC_ORIGIN. */
static unsigned csrf_no_host_case(auth_env *env, bool tls, const char *origin,
                                  const char *fetch_site) {
    cf_request req;
    cf_test_req_init(&req);
    req.method = CF_POST;
    req.original_method = CF_POST;
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

/* One CSRF check for raw header bytes (NULL bytes pointer = header absent). */
static unsigned csrf_bytes_case(auth_env *env, const unsigned char *origin,
                                size_t origin_len, bool has_origin,
                                const unsigned char *site, size_t site_len,
                                bool has_site) {
    cf_request req;
    cf_test_req_init(&req);
    req.method = CF_POST;
    req.original_method = CF_POST;
    req.path = SP("/session");
    CF_REQUIRE(cf_test_req_header(&req, SP("Host"),
                                  SP("127.0.0.1:32123")) == CF_OK);
    if (has_origin) {
        CF_REQUIRE(cf_test_req_header(
                       &req, SP("Origin"),
                       (cf_span){origin, origin_len}) == CF_OK);
    }
    if (has_site) {
        CF_REQUIRE(cf_test_req_header(
                       &req, SP("Sec-Fetch-Site"),
                       (cf_span){site, site_len}) == CF_OK);
    }
    return csrf_status(env, &req);
}

/* The ruled header-read gate: kit `request.header(name)` is
 * `HeaderMap::get(name).and_then(|v| v.to_str().ok())`, and http 1.5.0's
 * to_str admits only HTAB and visible ASCII (0x20..=0x7E). Every other byte
 * -- obs-text (which H01 admits), DEL, other controls, NUL -- makes Origin
 * and Sec-Fetch-Site read as *absent*, so `verify_authenticity_token` takes
 * its `None` branches: the origin check passes and a missing Sec-Fetch-Site
 * is accepted on plain HTTP with DISABLE_SSL. The controls show the outcome
 * is the gate, not the comparison: the same bytes with one extra obs-text
 * byte flip 422 -> accepted. */
static const char csrf_long_origin[] =
    "http://127.0.0.1:32123aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char csrf_long_site[] =
    "same-originaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
CF_TEST(csrf_unreadable_origin_and_fetch_site_read_as_absent) {
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));

    struct {
        const char *label;
        const unsigned char *bytes;
        size_t len;
        bool present;
        unsigned expected;
    } origins[] = {
        {"absent", NULL, 0, false, 0},
        {"exact match", (const unsigned char *)"http://127.0.0.1:32123",
         sizeof "http://127.0.0.1:32123" - 1, true, 0},
        {"null", (const unsigned char *)"null", sizeof "null" - 1, true, 422},
        {"mismatch", (const unsigned char *)"http://evil.test",
         sizeof "http://evil.test" - 1, true, 422},
        {"empty value", (const unsigned char *)"", 0, true, 422},
        {"obs-text UTF-8",
         (const unsigned char *)"http://127.0.0.1:32123\xc3\xa9",
         sizeof "http://127.0.0.1:32123\xc3\xa9" - 1, true, 0},
        {"invalid UTF-8", (const unsigned char *)"http://127.0.0.1:32123\xff",
         sizeof "http://127.0.0.1:32123\xff" - 1, true, 0},
        {"DEL", (const unsigned char *)"http://127.0.0.1:32123\x7f",
         sizeof "http://127.0.0.1:32123\x7f" - 1, true, 0},
        {"control", (const unsigned char *)"http://127.0.0.1:32123\x01",
         sizeof "http://127.0.0.1:32123\x01" - 1, true, 0},
        {"NUL", (const unsigned char *)"http://127.0.0.1:32123\0",
         sizeof "http://127.0.0.1:32123\0" - 1, true, 0},
        {"HTAB-padded match",
         (const unsigned char *)"\thttp://127.0.0.1:32123\t",
         sizeof "\thttp://127.0.0.1:32123\t" - 1, true, 422},
        {"long readable", (const unsigned char *)csrf_long_origin,
         sizeof csrf_long_origin - 1, true, 422},
    };
    for (size_t i = 0; i < sizeof origins / sizeof origins[0]; i++) {
        unsigned status = csrf_bytes_case(
            &env, origins[i].bytes, origins[i].len, origins[i].present,
            (const unsigned char *)"same-origin", sizeof "same-origin" - 1,
            true);
        if (status != origins[i].expected) {
            printf("    origin %s: expected %u got %u\n", origins[i].label,
                   origins[i].expected, status);
            CF_CHECK(status == origins[i].expected);
        }
    }

    struct {
        const char *label;
        const unsigned char *bytes;
        size_t len;
        bool present;
        unsigned expected;
    } sites[] = {
        {"absent", NULL, 0, false, 0},
        {"same-origin", (const unsigned char *)"same-origin",
         sizeof "same-origin" - 1, true, 0},
        {"same-site", (const unsigned char *)"same-site",
         sizeof "same-site" - 1, true, 0},
        {"cross-site", (const unsigned char *)"cross-site",
         sizeof "cross-site" - 1, true, 422},
        {"none", (const unsigned char *)"none", sizeof "none" - 1, true, 422},
        {"banana", (const unsigned char *)"banana", sizeof "banana" - 1, true,
         422},
        {"empty value", (const unsigned char *)"", 0, true, 422},
        {"obs-text UTF-8", (const unsigned char *)"same-origin\xc3\xa9",
         sizeof "same-origin\xc3\xa9" - 1, true, 0},
        {"invalid UTF-8", (const unsigned char *)"same-origin\xff",
         sizeof "same-origin\xff" - 1, true, 0},
        {"DEL", (const unsigned char *)"same-origin\x7f",
         sizeof "same-origin\x7f" - 1, true, 0},
        {"control", (const unsigned char *)"same-origin\x01",
         sizeof "same-origin\x01" - 1, true, 0},
        {"NUL", (const unsigned char *)"same-origin\0",
         sizeof "same-origin\0" - 1, true, 0},
        {"HTAB prefix", (const unsigned char *)"\tsame-origin",
         sizeof "\tsame-origin" - 1, true, 422},
        {"long readable", (const unsigned char *)csrf_long_site,
         sizeof csrf_long_site - 1, true, 422},
    };
    for (size_t i = 0; i < sizeof sites / sizeof sites[0]; i++) {
        unsigned status = csrf_bytes_case(
            &env, (const unsigned char *)"http://127.0.0.1:32123",
            sizeof "http://127.0.0.1:32123" - 1, true, sites[i].bytes,
            sites[i].len, sites[i].present);
        if (status != sites[i].expected) {
            printf("    sec-fetch-site %s: expected %u got %u\n",
                   sites[i].label, sites[i].expected, status);
            CF_CHECK(status == sites[i].expected);
        }
    }
    auth_env_close(&env);

    /* HTTPS (force_ssl): the absent branch is 422, so an unreadable value
     * must not slip past it either. */
    auth_env tls_env;
    CF_REQUIRE(auth_env_open_opts(&tls_env, true));
    CF_CHECK(csrf_bytes_case(&tls_env, NULL, 0, false, NULL, 0, false) == 422);
    CF_CHECK(csrf_bytes_case(&tls_env, NULL, 0, false,
                             (const unsigned char *)"same-origin",
                             sizeof "same-origin" - 1, true) == 0);
    CF_CHECK(csrf_bytes_case(&tls_env, NULL, 0, false,
                             (const unsigned char *)"same-origin\xc3\xa9",
                             sizeof "same-origin\xc3\xa9" - 1, true) == 422);
    CF_CHECK(csrf_bytes_case(&tls_env, NULL, 0, false,
                             (const unsigned char *)"same-origin\x7f",
                             sizeof "same-origin\x7f" - 1, true) == 422);
    auth_env_close(&tls_env);
}

/* Host joins the gate through `request.base_url`: kit request.rs's `host()`
 * reads `request.header("host")`, so an unreadable Host reads as absent and
 * base_url falls back exactly like a missing Host: scheme + "localhost"
 * (request.rs:160-166; see csrf_absent_host_uses_the_pin_localhost_fallback).
 * The origin comparison then sees the fallback, not the raw bytes. */
CF_TEST(csrf_unreadable_host_reads_as_absent_for_base_url) {
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    const unsigned char *own = (const unsigned char *)"http://127.0.0.1:32123";
    const size_t own_len = sizeof "http://127.0.0.1:32123" - 1;

    struct {
        const char *label;
        const unsigned char *host;
        size_t len;
        const unsigned char *origin;
        size_t origin_len;
        unsigned expected;
    } hosts[] = {
        {"exact", (const unsigned char *)"127.0.0.1:32123",
         sizeof "127.0.0.1:32123" - 1, own, own_len, 0},
        {"mismatch", (const unsigned char *)"other.test",
         sizeof "other.test" - 1, own, own_len, 422},
        /* Unreadable: absent -> the pin's "localhost" base URL, so an Origin
         * naming PUBLIC_ORIGIN no longer matches (before the localhost
         * fallback it did). */
        {"obs-text", (const unsigned char *)"127.0.0.1:32123\xc3\xa9",
         sizeof "127.0.0.1:32123\xc3\xa9" - 1,
         (const unsigned char *)"http://localhost",
         sizeof "http://localhost" - 1, 0},
        {"obs-text vs PUBLIC_ORIGIN",
         (const unsigned char *)"127.0.0.1:32123\xc3\xa9",
         sizeof "127.0.0.1:32123\xc3\xa9" - 1, own, own_len, 422},
        {"DEL", (const unsigned char *)"127.0.0.1:32123\x7f",
         sizeof "127.0.0.1:32123\x7f" - 1,
         (const unsigned char *)"http://localhost",
         sizeof "http://localhost" - 1, 0},
        {"NUL", (const unsigned char *)"127.0.0.1:32123\0",
         sizeof "127.0.0.1:32123\0" - 1,
         (const unsigned char *)"http://localhost",
         sizeof "http://localhost" - 1, 0},
    };
    for (size_t i = 0; i < sizeof hosts / sizeof hosts[0]; i++) {
        cf_request req;
        cf_test_req_init(&req);
        req.method = CF_POST;
        req.original_method = CF_POST;
        req.path = SP("/session");
        CF_REQUIRE(cf_test_req_header(
                       &req, SP("Host"),
                       (cf_span){hosts[i].host, hosts[i].len}) == CF_OK);
        CF_REQUIRE(cf_test_req_header(
                       &req, SP("Origin"),
                       (cf_span){hosts[i].origin, hosts[i].origin_len}) ==
                   CF_OK);
        CF_REQUIRE(cf_test_req_header(&req, SP("Sec-Fetch-Site"),
                                      SP("same-origin")) == CF_OK);
        unsigned status = csrf_status(&env, &req);
        if (status != hosts[i].expected) {
            printf("    host %s: expected %u got %u\n", hosts[i].label,
                   hosts[i].expected, status);
            CF_CHECK(status == hosts[i].expected);
        }
    }
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

/* HTTP/1.0 requests may omit Host (H01 admits minor==0 without one).  The
 * pin's host() then falls back header("host") -> uri.authority() (always
 * None: H01 admits origin-form targets only) -> "localhost", so base_url is
 * `http://localhost` (port = the scheme default, dropped) and an Origin
 * naming PUBLIC_ORIGIN does not match.  This run used to answer 401 (the
 * PUBLIC_ORIGIN fallback matched and the check passed); the faithful answer
 * is 422. */
CF_TEST(csrf_absent_host_uses_the_pin_localhost_fallback) {
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    /* PUBLIC_ORIGIN's Origin no longer matches the localhost base URL. */
    CF_CHECK(csrf_no_host_case(&env, false, "http://127.0.0.1:32123",
                               "same-origin") == 422);
    /* The pin's fallback itself matches. */
    CF_CHECK(csrf_no_host_case(&env, false, "http://localhost",
                               "same-origin") == 0);
    /* No Origin at all still passes on plain HTTP with SSL disabled. */
    CF_CHECK(csrf_no_host_case(&env, false, NULL, NULL) == 0);
    /* A localhost:80 Origin is not the portless localhost base URL. */
    CF_CHECK(csrf_no_host_case(&env, false, "http://localhost:80",
                               "same-origin") == 422);
    /* HTTPS: absent Host -> https://localhost, and the missing
     * Sec-Fetch-Site is 422 regardless. */
    auth_env tls_env;
    CF_REQUIRE(auth_env_open_opts(&tls_env, true));
    CF_CHECK(csrf_no_host_case(&tls_env, true, "https://localhost",
                               "same-origin") == 0);
    CF_CHECK(csrf_no_host_case(&tls_env, true, "http://127.0.0.1:32123",
                               "same-origin") == 422);
    auth_env_close(&tls_env);
    auth_env_close(&env);
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

/* ------------------------------------------------------------------ A01
 * completion: version headers, allow_browser and the stored platform. */

static bool before_span_has(cf_span hay, const char *needle) {
    size_t n = strlen(needle);
    if (n == 0) return true;
    if (hay.len < n) return false;
    for (size_t i = 0; i + n <= hay.len; i++) {
        if (memcmp(hay.ptr + i, needle, n) == 0) return true;
    }
    return false;
}

static bool builder_has(const cf_builder *b, const char *needle) {
    return b != NULL && before_span_has((cf_span){b->ptr, b->len}, needle);
}

/* The A02 incompatible-browser render resolves the browser logos through the
 * pinned asset manifest. */
static bool before_views_ready(void) {
    static bool ready;
    if (ready) return true;
    if (cf_views_assets_configure("tests/fixtures/assets") != CF_OK) {
        return false;
    }
    ready = true;
    return true;
}

/* Observe one response header through H01's serializer (cf_headers is
 * opaque): the line must start a line in the serialized header block. */
static bool before_header_line(const cf_response *resp, const cf_request *req,
                               const char *line) {
    cf_http_serialized out;
    if (cf_http_response_serialize(resp, req, &out) != CF_OK) return false;
    cf_span headers = cf_buf_span(out.headers);
    size_t n = strlen(line);
    bool found = false;
    for (size_t i = 0; i + n <= headers.len && !found; i++) {
        if (i != 0 && headers.ptr[i - 1] != '\n') continue;
        if (memcmp(headers.ptr + i, line, n) == 0) found = true;
    }
    if (out.headers != NULL) cf_buf_release(out.headers);
    return found;
}

static cf_span before_body(const cf_response *resp) {
    if (resp == NULL || resp->body_kind != CF_BODY_BUFFER ||
        resp->body == NULL) {
        return SP("");
    }
    return cf_buf_span(resp->body);
}

static void before_ua_request(cf_request *req, const char *path,
                              const char *ua) {
    cf_test_req_init(req);
    req->path = SP(path);
    if (ua != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("User-Agent"), SP(ua)) == CF_OK);
    }
}

static cf_err before_run(auth_env *env, cf_request *req, cf_response *resp,
                         cf_ctx *ctx) {
    cf_err rc = auth_env_ctx(env, req, resp, ctx);
    if (rc != CF_OK) return rc;
    cf_before policy = {CF_AUTH_SKIPPED, false, false};
    return cf_before_actions(ctx, policy);
}

/* The blocked-browser page's own marker. */
#define INCOMPATIBLE_MARKER "Upgrade to a supported web browser"
#define BLOCKED_UA_CHROME100                                                 \
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "          \
    "(KHTML, like Gecko) Chrome/100.0.0.0 Safari/537.36"
#define BLOCKED_UA_SAFARI171                                                 \
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "  \
    "(KHTML, like Gecko) Version/17.1 Safari/605.1.15"

/* Step 1: X-Version always from the build define; X-Rev only when a nonempty
 * CF_GIT_REVISION was defined. */
CF_TEST(version_headers_x_version_and_conditional_x_rev) {
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    cf_request req;
    cf_test_req_init(&req);
    cf_response resp;
    cf_ctx ctx;
    CF_REQUIRE(before_run(&env, &req, &resp, &ctx) == CF_OK);
    CF_CHECK(!cf_auth_halted(&ctx));
    /* The action that follows would set the status; the version headers were
     * written by step 1 and are already queued. */
    resp.status = 200;
    CF_CHECK(before_header_line(&resp, &req, "X-Version: " CF_APP_VERSION));
#ifdef CF_GIT_REVISION
    CF_CHECK(before_header_line(&resp, &req, "X-Rev: " CF_GIT_REVISION));
#else
    CF_CHECK(!before_header_line(&resp, &req, "X-Rev: "));
#endif
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);
    auth_env_close(&env);
}

/* Step 7 matrix: the five guarded browsers below their minimum (200 HTML),
 * newer versions, an unknown browser and a bot-exempt outdated Chrome. */
CF_TEST(allow_browser_blocks_outdated_browsers_and_keeps_platform) {
    CF_REQUIRE(before_views_ready());
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    struct {
        const char *ua;
        bool blocked;
    } cases[] = {
        {BLOCKED_UA_SAFARI171, true},
        {"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
         "(KHTML, like Gecko) Version/17.2 Safari/605.1.15",
         false},
        {BLOCKED_UA_CHROME100, true},
        {"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
         "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
         false},
        {"Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:120.0) "
         "Gecko/20100101 Firefox/120.0",
         true},
        {"Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:121.0) "
         "Gecko/20100101 Firefox/121.0",
         false},
        {"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
         "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36 "
         "OPR/103.0.0.0",
         true},
        {"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
         "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36 "
         "OPR/104.0.0.0",
         false},
        {"Mozilla/4.0 (compatible; MSIE 6.0; Windows NT 5.1)", true},
        /* IE is `false` in VERSIONS: every version is blocked. */
        {"Mozilla/5.0 (Windows NT 6.3; Trident/7.0; rv:11.0) like Gecko", true},
        {"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
         "(KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36",
         false},
        /* Chrome-Lighthouse is a bot: below the minimum but exempt. */
        {"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
         "(KHTML, like Gecko) Chrome/100.0.0.0 Safari/537.36 "
         "Chrome-Lighthouse",
         false},
        {"curl/8.4.0", false},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        cf_request req;
        before_ua_request(&req, "/", cases[i].ua);
        cf_response resp;
        cf_ctx ctx;
        CF_REQUIRE(before_run(&env, &req, &resp, &ctx) == CF_OK);
        const cf_platform *platform = cf_ctx_platform(&ctx);
        if (cases[i].blocked) {
            if (resp.status != 200) {
                printf("    case %zu: expected 200 got %u\n", i, resp.status);
                CF_CHECK(resp.status == 200);
            }
            CF_CHECK(cf_platform_blocked(platform));
            CF_CHECK(before_header_line(
                &resp, &req, "Content-Type: text/html; charset=utf-8"));
            CF_CHECK(before_span_has(before_body(&resp),
                                     INCOMPATIBLE_MARKER));
        } else {
            CF_CHECK(!cf_auth_halted(&ctx));
            CF_CHECK(resp.status == 0);
            CF_REQUIRE(platform != NULL);
            CF_CHECK(!cf_platform_blocked(platform));
        }
        CF_REQUIRE(platform != NULL);
        cf_ctx_destroy(&ctx);
        cf_response_dispose(&resp);
    }
    auth_env_close(&env);
}

/* The kit gate: no header, a blank value, or a value HeaderValue::to_str
 * rejects is never *checked* -- but the layout still gets the reference
 * `platform` helper's parse (the readable value, else ""), never zeros.
 *
 * to_str -> is_visible_ascii (http 1.5.0): obs-text (>= 0x80) is unreadable
 * even when it is valid UTF-8, so the reference parses "" for every value
 * here below the whitespace-only row.  The last row is the verifier's
 * smallest reproduction: Safari/16 reads as a blocked browser if the gate
 * admits the trailing " \xc3\xa9", and must instead be Mozilla/normal. */
CF_TEST(allow_browser_skips_absent_blank_and_unreadable_user_agents) {
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    struct {
        const char *value;
        const char *browser; /* Platform#browser of the empty/value parse */
    } cases[] = {
        {NULL, "Mozilla"},         /* absent -> parse("") */
        {"", "Mozilla"},           /* empty value -> parse("") */
        {" \t ", "Mozilla"},       /* whitespace-only -> gem default UA */
        {"\xc2\xa0", "Mozilla"},   /* NBSP: obs-text, unreadable -> parse("") */
        {"\xc3\xa9", "Mozilla"},   /* é: valid UTF-8, still unreadable */
        {"\xe6\xb5\x8f\xe8\xa7\x88\xe5\x99\xa8/1.0 Chrome/140.0",
         "Mozilla"},               /* CJK + Chrome/140: unreadable, not a UA */
        {"\xff\xfe", "Mozilla"},   /* unreadable -> parse("") */
        {"\xff", "Mozilla"},
        {"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
         "(KHTML, like Gecko) Version/16.0 Safari/605.1.15 \xc3\xa9",
         "Mozilla"}, /* verifier's smallest repro: unreadable -> no block */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        cf_request req;
        before_ua_request(&req, "/", cases[i].value);
        cf_response resp;
        cf_ctx ctx;
        CF_REQUIRE(before_run(&env, &req, &resp, &ctx) == CF_OK);
        CF_CHECK(!cf_auth_halted(&ctx));
        CF_CHECK(resp.status == 0);
        const cf_platform *platform = cf_ctx_platform(&ctx);
        CF_REQUIRE(platform != NULL);
        CF_CHECK(!cf_platform_blocked(platform));
        CF_CHECK(platform->desktop && !platform->mobile);
        CF_CHECK(!platform->ios && !platform->android && !platform->mac &&
                 !platform->windows && !platform->chrome &&
                 !platform->firefox && !platform->safari && !platform->edge &&
                 !platform->apple_messages && !platform->bot);
        CF_CHECK(platform->operating_system.len == 0);
        size_t n = strlen(cases[i].browser);
        if (platform->browser.len != n ||
            memcmp(platform->browser.ptr, cases[i].browser, n) != 0) {
            printf("    case %zu: browser expected \"%s\" got \"%.*s\"\n", i,
                   cases[i].browser, (int)platform->browser.len,
                   (const char *)platform->browser.ptr);
            CF_CHECK(0);
        }
        cf_ctx_destroy(&ctx);
        cf_response_dispose(&resp);
    }
    /* Readable, present values are stored even when they do not block.  Only
     * visible ASCII (and HTAB) is readable; the corpus' non-ASCII agents are
     * reached only by a direct parse, never through the kit. */
    cf_request req;
    before_ua_request(&req, "/", "curl/8.4.0");
    cf_response resp;
    cf_ctx ctx;
    CF_REQUIRE(before_run(&env, &req, &resp, &ctx) == CF_OK);
    const cf_platform *platform = cf_ctx_platform(&ctx);
    CF_REQUIRE(platform != NULL);
    CF_CHECK(!cf_platform_blocked(platform));
    CF_CHECK(platform->browser.len == 4 &&
             memcmp(platform->browser.ptr, "curl", 4) == 0);
    CF_CHECK(platform->desktop);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);
    auth_env_close(&env);
}

/* page/frame/own-layout matrix: a Turbo-Frame request gets turbo-rails' frame
 * layout unless the matched route's endpoint is messages# or
 * messages/by_bots# (those controllers own the application layout). */
CF_TEST(incompatible_browser_page_frame_and_own_layout_matrix) {
    CF_REQUIRE(before_views_ready());
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));

    struct {
        const char *endpoint;
        bool turbo_frame;
        bool expect_page;
    } cases[] = {
        {NULL, false, true},                          /* plain page */
        {NULL, true, false},                          /* frame request */
        {"messages#index", true, true},               /* own layout */
        {"messages/by_bots#index", true, true},       /* own layout */
        {"messages/boosts/by_bots#create", true, false},
        {"rooms#show", true, false},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        memset(&before_test_route, 0, sizeof before_test_route);
        before_test_route.id = 7;
        before_test_route.endpoint = cases[i].endpoint;
        cf_request req;
        before_ua_request(&req, "/", BLOCKED_UA_CHROME100);
        if (cases[i].turbo_frame) {
            CF_REQUIRE(cf_test_req_header(&req, SP("Turbo-Frame"),
                                          SP("room_1")) == CF_OK);
        }
        cf_response resp;
        cf_ctx ctx;
        CF_REQUIRE(before_run(&env, &req, &resp, &ctx) == CF_OK);
        if (resp.status != 200) {
            printf("    case %zu: expected 200 got %u\n", i, resp.status);
            CF_CHECK(resp.status == 200);
        }
        bool page = before_span_has(before_body(&resp), "<!DOCTYPE html>");
        if (page != cases[i].expect_page) {
            printf("    case %zu (%s): expected %s layout\n", i,
                   cases[i].endpoint != NULL ? cases[i].endpoint : "-",
                   cases[i].expect_page ? "page" : "frame");
            CF_CHECK(page == cases[i].expect_page);
        }
        CF_CHECK(before_span_has(before_body(&resp), INCOMPATIBLE_MARKER));
        cf_ctx_destroy(&ctx);
        cf_response_dispose(&resp);
    }
    memset(&before_test_route, 0, sizeof before_test_route);
    auth_env_close(&env);
}

/* An explicit render answers HTML whatever the request format: a blocked
 * browser gets the page for a webmanifest path or `Accept: application/json`,
 * never a 406. */
CF_TEST(incompatible_browser_answers_html_for_any_request_format) {
    CF_REQUIRE(before_views_ready());
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    static const char *const paths[] = {"/", "/webmanifest.json"};
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        cf_request req;
        before_ua_request(&req, paths[i], BLOCKED_UA_CHROME100);
        CF_REQUIRE(cf_test_req_header(&req, SP("Accept"),
                                      SP("application/json")) == CF_OK);
        cf_response resp;
        cf_ctx ctx;
        CF_REQUIRE(before_run(&env, &req, &resp, &ctx) == CF_OK);
        CF_CHECK(resp.status == 200);
        CF_CHECK(before_header_line(
            &resp, &req, "Content-Type: text/html; charset=utf-8"));
        CF_CHECK(before_span_has(before_body(&resp), INCOMPATIBLE_MARKER));
        cf_ctx_destroy(&ctx);
        cf_response_dispose(&resp);
    }
    auth_env_close(&env);
}

/* The Apple Messages link preview claims to be both bots; the view's title
 * branch reads apple_messages off the platform the before-action stored. */
CF_TEST(incompatible_browser_apple_messages_title_branch) {
    CF_REQUIRE(before_views_ready());
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    cf_request req;
    before_ua_request(
        &req, "/",
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_11_1) "
        "AppleWebKit/601.2.4 (KHTML, like Gecko) Version/9.0.1 "
        "Safari/601.2.4 facebookexternalhit/1.1 Facebot Twitterbot/1.0");
    cf_response resp;
    cf_ctx ctx;
    CF_REQUIRE(before_run(&env, &req, &resp, &ctx) == CF_OK);
    const cf_platform *platform = cf_ctx_platform(&ctx);
    CF_REQUIRE(platform != NULL);
    CF_CHECK(platform->apple_messages);
    CF_CHECK(resp.status == 200);
    CF_CHECK(before_span_has(before_body(&resp), "<title>Campfire</title>"));
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);

    /* The ordinary blocked page keeps the default title. */
    cf_request plain;
    before_ua_request(&plain, "/", BLOCKED_UA_SAFARI171);
    CF_REQUIRE(before_run(&env, &plain, &resp, &ctx) == CF_OK);
    CF_CHECK(before_span_has(before_body(&resp),
                             "<title>Unsupported browser</title>"));
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);
    auth_env_close(&env);
}

/* With a real User-Agent the stored platform reaches the layout: the rooms
 * bell renders its Chrome/Windows notification help from it. */
CF_TEST(platform_facts_reach_the_layout_with_a_real_user_agent) {
    CF_REQUIRE(before_views_ready());
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    cf_request req;
    before_ua_request(&req, "/",
                      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                      "AppleWebKit/537.36 (KHTML, like Gecko) "
                      "Chrome/140.0.0.0 Safari/537.36");
    cf_response resp;
    cf_ctx ctx;
    CF_REQUIRE(before_run(&env, &req, &resp, &ctx) == CF_OK);
    const cf_platform *platform = cf_ctx_platform(&ctx);
    CF_REQUIRE(platform != NULL);
    CF_CHECK(platform->chrome && platform->windows && platform->desktop);
    CF_CHECK(!platform->mobile);
    CF_CHECK(platform->browser.len == 6 &&
             memcmp(platform->browser.ptr, "Chrome", 6) == 0);
    CF_CHECK(platform->operating_system.len == 7 &&
             memcmp(platform->operating_system.ptr, "Windows", 7) == 0);

    cf_view_layout_model layout = {0};
    CF_REQUIRE(cf_presenter_layout_load(&ctx, cf_ctx_platform(&ctx),
                                        &layout) == CF_OK);
    CF_CHECK(layout.platform.chrome && layout.platform.windows);
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, &ctx, &layout);
    cf_view_room room = {0};
    room.id = 5;
    room.kind = CF_ROOM_OPEN;
    room.display_name = (cf_str){(char *)"HQ", 2};
    cf_builder out = {0};
    CF_REQUIRE(cf_view_room_bell(&view_ctx, &room, &out) == CF_OK);
    CF_CHECK(builder_has(&out, "Check your Chrome settings"));
    CF_CHECK(builder_has(&out, "System &gt; Notification"));
    cf_builder_dispose(&out);
    cf_view_layout_model_dispose(&layout);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);
    auth_env_close(&env);
}

/* Render the rooms bell through the layout presenter with the context's
 * stored platform (before_views_ready must have run). */
static bool before_bell_html(cf_ctx *ctx, cf_builder *out) {
    cf_view_layout_model layout = {0};
    if (cf_presenter_layout_load(ctx, cf_ctx_platform(ctx), &layout) != CF_OK) {
        return false;
    }
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, &layout);
    cf_view_room room = {0};
    room.id = 5;
    room.kind = CF_ROOM_OPEN;
    room.display_name = (cf_str){(char *)"HQ", 2};
    cf_err rc = cf_view_room_bell(&view_ctx, &room, out);
    cf_view_layout_model_dispose(&layout);
    return rc == CF_OK;
}

/* The reference `platform` helper always hands the layout a parse: an absent
 * (or blank) header yields `ApplicationPlatform.new("")` -- browser "Mozilla",
 * desktop, no other fact -- not zeroed facts. */
CF_TEST(absent_and_blank_user_agents_get_the_empty_parse_facts) {
    CF_REQUIRE(before_views_ready());
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));

    /* (a) no User-Agent header. */
    cf_request req;
    cf_test_req_init(&req);
    cf_response resp;
    cf_ctx ctx;
    CF_REQUIRE(before_run(&env, &req, &resp, &ctx) == CF_OK);
    const cf_platform *platform = cf_ctx_platform(&ctx);
    CF_REQUIRE(platform != NULL);
    CF_CHECK(platform->browser.len == 7 &&
             memcmp(platform->browser.ptr, "Mozilla", 7) == 0);
    CF_CHECK(platform->desktop && !platform->mobile);
    CF_CHECK(!platform->ios && !platform->android && !platform->mac &&
             !platform->windows && !platform->chrome && !platform->firefox &&
             !platform->safari && !platform->edge &&
             !platform->apple_messages && !platform->bot);
    CF_CHECK(platform->operating_system.len == 0);
    CF_CHECK(!cf_platform_blocked(platform));
    cf_builder absent_bell = {0};
    CF_REQUIRE(before_bell_html(&ctx, &absent_bell));
    CF_CHECK(builder_has(&absent_bell, "Check your Mozilla settings"));
    cf_view_layout_model layout = {0};
    CF_REQUIRE(cf_presenter_layout_load(&ctx, cf_ctx_platform(&ctx), &layout) ==
               CF_OK);
    CF_CHECK(layout.platform.desktop && !layout.platform.mobile);
    CF_CHECK(layout.platform.browser.len == 7 &&
             memcmp(layout.platform.browser.ptr, "Mozilla", 7) == 0);
    cf_view_layout_model_dispose(&layout);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);

    /* (b) a present but blank header: never checked, same parse -> the same
     * bell output. */
    cf_request blank;
    before_ua_request(&blank, "/", " \t ");
    CF_REQUIRE(before_run(&env, &blank, &resp, &ctx) == CF_OK);
    CF_CHECK(!cf_auth_halted(&ctx));
    CF_CHECK(resp.status == 0);
    platform = cf_ctx_platform(&ctx);
    CF_REQUIRE(platform != NULL);
    CF_CHECK(platform->browser.len == 7 &&
             memcmp(platform->browser.ptr, "Mozilla", 7) == 0);
    CF_CHECK(platform->desktop && !platform->mobile);
    cf_builder blank_bell = {0};
    CF_REQUIRE(before_bell_html(&ctx, &blank_bell));
    CF_CHECK(absent_bell.len == blank_bell.len &&
             memcmp(absent_bell.ptr, blank_bell.ptr, blank_bell.len) == 0);
    cf_builder_dispose(&blank_bell);
    cf_builder_dispose(&absent_bell);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);
    auth_env_close(&env);
}

/* The stored platform owns no strings, so the parser's synthesized text
 * (normalize_os' "ChromeOS ..."/"iOS ..."/"OS X ...") must be rebased onto
 * the stored copy: it has to stay readable after allow_browser's local value
 * is gone, all the way into the layout and the bell. */
CF_TEST(synthesized_platform_text_survives_the_store) {
    CF_REQUIRE(before_views_ready());
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    static const char *const ua =
        "Mozilla/5.0 (X11; CrOS x86_64 14541.0.0) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36";
    cf_request req;
    before_ua_request(&req, "/", ua);
    cf_response resp;
    cf_ctx ctx;
    CF_REQUIRE(before_run(&env, &req, &resp, &ctx) == CF_OK);
    const cf_platform *platform = cf_ctx_platform(&ctx);
    CF_REQUIRE(platform != NULL);
    CF_CHECK(!cf_platform_blocked(platform));
    CF_CHECK(platform->chrome && platform->desktop);
    CF_CHECK(platform->operating_system.len == 18 &&
             memcmp(platform->operating_system.ptr, "ChromeOS 14541.0.0",
                    18) == 0);

    cf_view_layout_model layout = {0};
    CF_REQUIRE(cf_presenter_layout_load(&ctx, platform, &layout) == CF_OK);
    CF_CHECK(layout.platform.operating_system.len == 18 &&
             memcmp(layout.platform.operating_system.ptr,
                    "ChromeOS 14541.0.0", 18) == 0);
    cf_view_layout_model_dispose(&layout);

    cf_builder bell = {0};
    CF_REQUIRE(before_bell_html(&ctx, &bell));
    CF_CHECK(builder_has(&bell, "Check your ChromeOS 14541.0.0 settings"));
    cf_builder_dispose(&bell);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);
    auth_env_close(&env);
}

CF_TEST_MAIN()
