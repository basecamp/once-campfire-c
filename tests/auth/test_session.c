/* tests/auth/test_session.c — A01 acceptance AUTH-02 (login matrix), AUTH-04
 * (activity refresh only when due; unchanged cookies are not rewritten),
 * AUTH-05 (logout/ban/deactivation/membership removal deny access) and
 * AUTH-06 (wrong-user room scoping), plus the return-to/flash session state.
 */
#include "cf_test.h"

#include "app.h"
#include "auth.h"
#include "auth/internal.h"
#include "context.h"
#include "core/testclock.h"
#include "db/writer.h"
#include "http/http_internal.h"
#include "models/membership.h"
#include "models/user.h"

#include "support/auth_env.h"
#include "../app/support/test_request.h"

#include <stdio.h>
#include <string.h>

#define T_NOW_US INT64_C(1767272400000000) /* 2026-01-01T12:00:00Z */

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* Rack form-encodes cookie values on the wire; A00 unescapes on the way in, so
 * the test header must escape the characters a base64 value can contain.
 * cookie_pair_append appends "name=value" to an existing header buffer. */
static void cookie_pair_append(char *out, size_t cap, const char *name,
                               cf_span value) {
    size_t at = strlen(out);
    if (at != 0) at += (size_t)snprintf(out + at, cap - at, "; ");
    at += (size_t)snprintf(out + at, cap - at, "%s=", name);
    for (size_t i = 0; i < value.len && at + 4 < cap; i++) {
        unsigned char c = value.ptr[i];
        if (c == '+' || c == '%') {
            at += (size_t)snprintf(out + at, cap - at, "%%%02X", c);
        } else {
            out[at++] = (char)c;
        }
    }
    out[at] = '\0';
}

static void cookie_header(char *out, size_t cap, const char *name,
                          cf_span value) {
    out[0] = '\0';
    cookie_pair_append(out, cap, name, value);
}

static void make_ctx(auth_env *env, cf_request *req, cf_response *resp,
                     cf_ctx *ctx) {
    cf_err rc = auth_env_ctx(env, req, resp, ctx);
    if (rc != CF_OK) printf("    ctx create failed: rc=%d path=%.*s\n", rc,
                            (int)req->path.len, req->path.ptr);
    CF_REQUIRE(rc == CF_OK);
}

static cf_err login_ctx(auth_env *env, cf_ctx *ctx, cf_response *resp,
                        cf_request *req, int64_t user_id) {
    cf_test_req_init(req);
    req->method = CF_POST;
    req->original_method = CF_POST;
    req->path = SP("/session");
    make_ctx(env, req, resp, ctx);
    cf_user user;
    bool found = false;
    CF_REQUIRE(cf_user_find_by_id(env->reader, user_id, &found, &user) == CF_OK);
    CF_REQUIRE(found);
    cf_session session;
    cf_err rc = cf_auth_start_new_session_for(ctx, &user, &session);
    if (rc == CF_OK) cf_session_dispose(&session);
    cf_user_dispose(&user);
    return rc;
}

/* The cookie value the request jar currently holds, or {NULL,0}. */
static cf_str current_cookie(cf_ctx *ctx, const char *name) {
    cf_span wire;
    if (cf_ctx_cookie_get(ctx, SP(name), &wire) != CF_OK) return (cf_str){0};
    cf_str copy = {0};
    CF_REQUIRE(auth_str_dup(wire, &copy) == CF_OK);
    return copy;
}

/* The signed session_token cookie the request jar currently holds. */
static cf_str current_token_cookie(cf_ctx *ctx) {
    return current_cookie(ctx, "session_token");
}

/* Serialize the finished response (status 200 when the test action left it
 * unset) and return the head bytes, so a test can assert the exact
 * Set-Cookie lines A00 flushes from the jar. */
static cf_str response_head(cf_response *resp, cf_request *req) {
    if (resp->status == 0) resp->status = 200;
    cf_http_serialized ser = {0};
    CF_REQUIRE(cf_http_response_serialize(resp, req, &ser) == CF_OK);
    CF_REQUIRE(ser.headers != NULL);
    cf_str head = {0};
    CF_REQUIRE(auth_str_dup(cf_buf_span(ser.headers), &head) == CF_OK);
    cf_buf_release(ser.headers);
    return head;
}

static bool str_has(const cf_str *text, const char *needle) {
    return text->ptr != NULL && strstr(text->ptr, needle) != NULL;
}

static size_t str_count(const cf_str *text, const char *needle) {
    if (text->ptr == NULL) return 0;
    size_t count = 0;
    const char *at = text->ptr;
    size_t len = strlen(needle);
    while ((at = strstr(at, needle)) != NULL) {
        count++;
        at += len;
    }
    return count;
}

/* A request carrying an already-built Cookie header. */
static void ctx_with_cookie_header(auth_env *env, const char *header,
                                   cf_request *req, cf_response *resp,
                                   cf_ctx *ctx) {
    cf_test_req_init(req);
    req->path = SP("/rooms/1");
    CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(header)) == CF_OK);
    make_ctx(env, req, resp, ctx);
}

/* A second request carrying the session_token value from `raw`. */
static void ctx_with_token(auth_env *env, cf_span raw, cf_request *req,
                           cf_response *resp, cf_ctx *ctx) {
    char header[1024];
    cookie_header(header, sizeof header, "session_token", raw);
    ctx_with_cookie_header(env, header, req, resp, ctx);
}

CF_TEST(login_matrix) {
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    cf_str digest = {0};
    CF_REQUIRE(cf_auth_password_digest(CF_STR_LIT("secret123456"), 4, &digest) ==
               CF_OK);
    int64_t active = auth_seed_user(&env, "Active", "a@example.com",
                                    digest.ptr, 0, 0);
    int64_t deactivated = auth_seed_user(&env, "Gone", "gone@example.com",
                                         digest.ptr, 0, 1);
    int64_t banned = auth_seed_user(&env, "Bad", "bad@example.com", digest.ptr,
                                    0, 2);
    int64_t nodigest = auth_seed_user(&env, "NoPw", "nopw@example.com", NULL,
                                      0, 0);
    CF_REQUIRE(active && deactivated && banned && nodigest);

    cf_request req;
    cf_response resp;
    cf_ctx ctx;
    cf_test_req_init(&req);
    req.method = CF_POST;
    req.original_method = CF_POST;
    req.path = SP("/session");
    make_ctx(&env, &req, &resp, &ctx);

    bool ok = false;
    cf_user user;
    CF_REQUIRE(cf_auth_authenticate_by(&ctx, SP("a@example.com"),
                                       SP("secret123456"), &ok,
                                       &user) == CF_OK);
    CF_CHECK(ok && user.id == active);
    if (ok) cf_user_dispose(&user);

    ok = true;
    CF_REQUIRE(cf_auth_authenticate_by(&ctx, SP("a@example.com"), SP("wrong"),
                                       &ok, &user) == CF_OK);
    CF_CHECK(!ok);

    ok = true;
    CF_REQUIRE(cf_auth_authenticate_by(&ctx, SP("unknown@example.com"),
                                       SP("secret123456"), &ok,
                                       &user) == CF_OK);
    CF_CHECK(!ok); /* unknown user: the dummy verification still ran */

    ok = true;
    CF_REQUIRE(cf_auth_authenticate_by(&ctx, SP("a@example.com"), SP(""), &ok,
                                       &user) == CF_OK);
    CF_CHECK(!ok); /* blank password: no lookup, no verification */

    ok = true;
    CF_REQUIRE(cf_auth_authenticate_by(&ctx, SP("gone@example.com"),
                                       SP("secret123456"), &ok,
                                       &user) == CF_OK);
    CF_CHECK(!ok); /* deactivated users are not found by the active lookup */

    ok = true;
    CF_REQUIRE(cf_auth_authenticate_by(&ctx, SP("bad@example.com"),
                                       SP("secret123456"), &ok,
                                       &user) == CF_OK);
    CF_CHECK(!ok);

    ok = true;
    CF_REQUIRE(cf_auth_authenticate_by(&ctx, SP("nopw@example.com"),
                                       SP("secret123456"), &ok,
                                       &user) == CF_OK);
    CF_CHECK(!ok); /* no digest never verifies */

    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);
    cf_str_dispose(&digest);
    auth_env_close(&env);
}

CF_TEST(login_sets_signed_session_token_and_authenticates) {
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    cf_test_clock_set_fixed_us(T_NOW_US);
    int64_t id = auth_seed_user(&env, "Active", "a@example.com", NULL, 1, 0);
    CF_REQUIRE(id != 0);
    cf_request req;
    cf_response resp;
    cf_ctx ctx;
    CF_REQUIRE(login_ctx(&env, &ctx, &resp, &req, id) == CF_OK);
    CF_CHECK(ctx.identity.kind == CF_AUTH_SESSION);
    CF_CHECK(ctx.identity.user_id == id);
    CF_CHECK(ctx.identity.session_id != 0);
    CF_CHECK(ctx.identity.role == CF_ROLE_ADMINISTRATOR);
    CF_CHECK(!ctx.identity.activity_due);

    cf_str wire = current_token_cookie(&ctx);
    cf_str token = {0};
    bool found = false;
    CF_REQUIRE(cf_auth_signed_cookie_verify(
                   SP(AUTH_VEC_SECRET_KEY_BASE), SP("session_token"),
                   (cf_span){(const unsigned char *)wire.ptr, wire.len},
                   T_NOW_US, &token, &found) == CF_OK);
    CF_CHECK(found);
    /* The cookie carries the token of a session row owned by the user. */
    cf_session row;
    bool session_found = false;
    CF_REQUIRE(cf_session_find_by_token(env.reader, token, &session_found,
                                        &row) == CF_OK);
    CF_CHECK(session_found);
    if (session_found) {
        CF_CHECK(row.user_id == id);
        cf_session_dispose(&row);
    }
    cf_str_dispose(&token);

    /* The second request resigns the token back to the same session row and
     * does not refresh activity (the session is fresh), so no Set-Cookie is
     * emitted for it. */
    cf_str cookie = current_token_cookie(&ctx);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);

    cf_request req2;
    cf_response resp2;
    cf_ctx ctx2;
    ctx_with_token(&env, (cf_span){(const unsigned char *)cookie.ptr,
                                   cookie.len},
                   &req2, &resp2, &ctx2);
    CF_REQUIRE(cf_authenticate(&ctx2) == CF_OK);
    CF_CHECK(!cf_auth_halted(&ctx2));
    CF_CHECK(ctx2.identity.kind == CF_AUTH_SESSION);
    CF_CHECK(ctx2.identity.user_id == id);
    CF_CHECK(!ctx2.identity.activity_due);
    CF_REQUIRE(cf_finish_cookies(&ctx2) == CF_OK);
    CF_CHECK(resp2.headers == NULL); /* unchanged: no cookie update */
    cf_ctx_destroy(&ctx2);
    cf_response_dispose(&resp2);
    cf_str_dispose(&token);
    cf_str_dispose(&wire);
    cf_str_dispose(&cookie);
    cf_test_clock_clear();
    auth_env_close(&env);
}

CF_TEST(activity_refreshes_only_when_due) {
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    cf_test_clock_set_fixed_us(T_NOW_US);
    int64_t id = auth_seed_user(&env, "Active", "a@example.com", NULL, 0, 0);
    CF_REQUIRE(id != 0);
    cf_request req;
    cf_response resp;
    cf_ctx ctx;
    CF_REQUIRE(login_ctx(&env, &ctx, &resp, &req, id) == CF_OK);
    cf_str wire = current_token_cookie(&ctx);
    /* Also store an unchanged encrypted session cookie; the refresh must not
     * touch it. */
    CF_REQUIRE(cf_auth_session_write(&ctx, SP("k"), SP("\"v\"")) == CF_OK);
    cf_str session_wire = current_cookie(&ctx, "_campfire_session");
    CF_REQUIRE(session_wire.ptr != NULL);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);

    char header[4096];
    cookie_header(header, sizeof header, "session_token",
                  (cf_span){(const unsigned char *)wire.ptr, wire.len});
    cookie_pair_append(header, sizeof header, "_campfire_session",
                       (cf_span){(const unsigned char *)session_wire.ptr,
                                 session_wire.len});

    /* Exactly one hour later: not due (strictly older than an hour). */
    cf_test_clock_set_fixed_us(T_NOW_US + CF_SESSION_ACTIVITY_REFRESH_RATE_US);
    cf_request req2;
    cf_response resp2;
    cf_ctx ctx2;
    ctx_with_cookie_header(&env, header, &req2, &resp2, &ctx2);
    CF_REQUIRE(cf_authenticate(&ctx2) == CF_OK);
    CF_CHECK(!cf_auth_halted(&ctx2));
    CF_CHECK(!ctx2.identity.activity_due);
    CF_REQUIRE(cf_finish_cookies(&ctx2) == CF_OK);
    CF_CHECK(resp2.headers == NULL);
    cf_span unchanged;
    CF_REQUIRE(cf_ctx_cookie_get(&ctx2, SP("_campfire_session"), &unchanged) ==
               CF_OK);
    CF_CHECK(unchanged.len == session_wire.len &&
             memcmp(unchanged.ptr, session_wire.ptr, session_wire.len) == 0);
    cf_ctx_destroy(&ctx2);
    cf_response_dispose(&resp2);

    /* One microsecond past the hour: due; the row is refreshed and the cookie
     * re-signed. */
    int64_t due_at = T_NOW_US + CF_SESSION_ACTIVITY_REFRESH_RATE_US + 1;
    cf_test_clock_set_fixed_us(due_at);
    cf_request req3;
    cf_response resp3;
    cf_ctx ctx3;
    ctx_with_cookie_header(&env, header, &req3, &resp3, &ctx3);
    CF_REQUIRE(cf_authenticate(&ctx3) == CF_OK);
    CF_CHECK(!cf_auth_halted(&ctx3));
    CF_CHECK(ctx3.identity.activity_due);
    CF_REQUIRE(cf_finish_cookies(&ctx3) == CF_OK);
    CF_CHECK(resp3.headers != NULL);
    cf_span refreshed;
    CF_REQUIRE(cf_ctx_cookie_get(&ctx3, SP("session_token"), &refreshed) ==
               CF_OK);
    /* The re-signed cookie differs from the old one (same token, new expiry). */
    CF_CHECK(refreshed.len != wire.len ||
             memcmp(refreshed.ptr, wire.ptr, wire.len) != 0);
    /* The refresh touches only session_token: the unchanged
     * _campfire_session state is not re-set and its bytes are untouched. */
    cf_str head = response_head(&resp3, &req3);
    CF_CHECK(str_has(&head, "Set-Cookie: session_token="));
    CF_CHECK(str_count(&head, "Set-Cookie") == 1);
    CF_CHECK(strstr(head.ptr, "_campfire_session") == NULL);
    cf_str_dispose(&head);
    CF_REQUIRE(cf_ctx_cookie_get(&ctx3, SP("_campfire_session"), &unchanged) ==
               CF_OK);
    CF_CHECK(unchanged.len == session_wire.len &&
             memcmp(unchanged.ptr, session_wire.ptr, session_wire.len) == 0);

    char sql[128];
    char text[64];
    snprintf(sql, sizeof sql, "SELECT last_active_at FROM sessions WHERE id=%lld",
             (long long)ctx3.identity.session_id);
    const char *stored = cf_db_test_text(cf_db_handle(env.scratch.db), sql,
                                         text, sizeof text);
    CF_CHECK(stored != NULL);
    if (stored != NULL) {
        CF_CHECK(strcmp(stored, "2026-01-01 14:00:00.000001") == 0);
    }
    cf_ctx_destroy(&ctx3);
    cf_response_dispose(&resp3);
    cf_str_dispose(&session_wire);
    cf_str_dispose(&wire);
    cf_test_clock_clear();
    auth_env_close(&env);
}

CF_TEST(logout_terminates_session_and_disconnects) {
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    cf_test_clock_set_fixed_us(T_NOW_US);
    int64_t id = auth_seed_user(&env, "Active", "a@example.com", NULL, 0, 0);
    CF_REQUIRE(id != 0);
    cf_request req;
    cf_response resp;
    cf_ctx ctx;
    CF_REQUIRE(login_ctx(&env, &ctx, &resp, &req, id) == CF_OK);
    cf_str wire = current_token_cookie(&ctx);
    int64_t session_id = ctx.identity.session_id;

    CF_REQUIRE(cf_auth_terminate_current_session(&ctx) == CF_OK);
    CF_CHECK(ctx.identity.kind == CF_AUTH_NONE);
    CF_CHECK(cf_ctx_cookie_get(&ctx, SP("session_token"), &(cf_span){0}) ==
             CF_NOT_FOUND);
    bool ok = false;
    char sql[128];
    snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM sessions WHERE id=%lld",
             (long long)session_id);
    CF_CHECK(cf_db_test_i64(cf_db_handle(env.scratch.db), sql, &ok) == 0);
    CF_CHECK(ok);
    /* DISCONNECT_USER(reconnect=true) through the writer. */
    bool saw_disconnect = false;
    for (size_t i = 0; i < env.event_count; i++) {
        if (env.events[i].kind == CF_EVENT_DISCONNECT_USER &&
            env.events[i].user_id == id && env.events[i].reconnect) {
            saw_disconnect = true;
        }
    }
    CF_CHECK(saw_disconnect);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);

    /* The old cookie no longer authenticates: 302 to sign in. */
    cf_request req2;
    cf_response resp2;
    cf_ctx ctx2;
    ctx_with_token(&env, (cf_span){(const unsigned char *)wire.ptr, wire.len},
                   &req2, &resp2, &ctx2);
    CF_REQUIRE(cf_authenticate(&ctx2) == CF_OK);
    CF_CHECK(cf_auth_halted(&ctx2));
    CF_CHECK(resp2.status == 302);
    CF_CHECK(ctx2.identity.kind == CF_AUTH_NONE);
    cf_ctx_destroy(&ctx2);
    cf_response_dispose(&resp2);
    cf_str_dispose(&wire);
    cf_test_clock_clear();
    auth_env_close(&env);
}

typedef struct {
    int64_t user_id;
    bool ban;
} account_change_arg;

static cf_err account_change_cb(cf_tx *tx, void *user) {
    account_change_arg *arg = user;
    cf_user u;
    cf_err rc = cf_user_find(cf_tx_db(tx), arg->user_id, &u);
    if (rc != CF_OK) return rc;
    rc = arg->ban ? cf_user_ban(tx, &u) : cf_user_deactivate(tx, &u);
    cf_user_dispose(&u);
    return rc;
}

static void account_state_denies_access(bool ban) {
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    cf_test_clock_set_fixed_us(T_NOW_US);
    int64_t id = auth_seed_user(&env, "Active", "a@example.com", NULL, 0, 0);
    CF_REQUIRE(id != 0);
    cf_request req;
    cf_response resp;
    cf_ctx ctx;
    CF_REQUIRE(login_ctx(&env, &ctx, &resp, &req, id) == CF_OK);
    cf_str wire = current_token_cookie(&ctx);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);
    /* The session must carry a bannable (public) IP. */
    CF_REQUIRE(auth_exec(&env,
        "UPDATE sessions SET ip_address='203.0.113.9' WHERE user_id=1"));

    /* user.ban / user.deactivate delete sessions and emit
     * DISCONNECT_USER(reconnect=false). */
    account_change_arg arg = {.user_id = id, .ban = ban};
    cf_err change_rc = cf_write(env.app, account_change_cb, &arg);
    if (change_rc != CF_OK) {
        printf("    %s failed: %s (%s)\n", ban ? "ban" : "deactivate",
               cf_err_name(change_rc), cf_db_last_error());
    }
    CF_REQUIRE(change_rc == CF_OK);
    bool ok = false;
    char sql[128];
    snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM sessions WHERE user_id=%lld",
             (long long)id);
    CF_CHECK(cf_db_test_i64(cf_db_handle(env.scratch.db), sql, &ok) == 0);
    CF_CHECK(ok);
    snprintf(sql, sizeof sql, "SELECT status FROM users WHERE id=%lld",
             (long long)id);
    CF_CHECK(cf_db_test_i64(cf_db_handle(env.scratch.db), sql, &ok) ==
             (ban ? 2 : 1));
    bool saw_disconnect = false;
    for (size_t i = 0; i < env.event_count; i++) {
        if (env.events[i].kind == CF_EVENT_DISCONNECT_USER &&
            env.events[i].user_id == id && !env.events[i].reconnect) {
            saw_disconnect = true;
        }
    }
    CF_CHECK(saw_disconnect);

    /* The old cookie no longer authenticates. */
    cf_request req2;
    cf_response resp2;
    cf_ctx ctx2;
    ctx_with_token(&env, (cf_span){(const unsigned char *)wire.ptr, wire.len},
                   &req2, &resp2, &ctx2);
    CF_REQUIRE(cf_authenticate(&ctx2) == CF_OK);
    CF_CHECK(cf_auth_halted(&ctx2));
    CF_CHECK(resp2.status == 302);
    cf_ctx_destroy(&ctx2);
    cf_response_dispose(&resp2);
    cf_str_dispose(&wire);
    cf_test_clock_clear();
    auth_env_close(&env);
}

CF_TEST(ban_denies_access) { account_state_denies_access(true); }

CF_TEST(deactivation_denies_access) { account_state_denies_access(false); }

CF_TEST(post_authenticating_url_stays_under_public_origin) {
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    cf_test_clock_set_fixed_us(T_NOW_US);
    int64_t id = auth_seed_user(&env, "Active", "a@example.com", NULL, 0, 0);
    CF_REQUIRE(id != 0);
    cf_request req;
    cf_response resp;
    cf_ctx ctx;
    cf_test_req_init(&req);
    req.path = SP("/rooms/7");
    req.query = SP("page=2");
    make_ctx(&env, &req, &resp, &ctx);
    CF_REQUIRE(cf_authenticate(&ctx) == CF_OK);
    CF_CHECK(cf_auth_halted(&ctx));
    CF_CHECK(resp.status == 302);

    cf_user user;
    bool found = false;
    CF_REQUIRE(cf_user_find_by_id(env.reader, id, &found, &user) == CF_OK);
    CF_REQUIRE(found);
    cf_session session;
    CF_REQUIRE(cf_auth_start_new_session_for(&ctx, &user, &session) == CF_OK);
    cf_session_dispose(&session);
    cf_user_dispose(&user);

    cf_str url = {0};
    CF_REQUIRE(cf_auth_post_authenticating_url(&ctx, &url) == CF_OK);
    CF_CHECK(auth_str_is(url, "http://127.0.0.1:32123/rooms/7?page=2"));
    cf_str_dispose(&url);
    /* Consuming the return-to leaves nothing but session_id: commit deletes
     * the cookie instead of re-writing it. */
    cf_span gone;
    CF_CHECK(cf_ctx_cookie_get(&ctx, SP("_campfire_session"), &gone) ==
             CF_NOT_FOUND);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);

    /* A stored URL outside PUBLIC_ORIGIN falls back to the root. */
    cf_test_req_init(&req);
    req.path = SP("/");
    make_ctx(&env, &req, &resp, &ctx);
    CF_REQUIRE(cf_auth_session_write(
                   &ctx, SP("return_to_after_authenticating"),
                   SP("\"https://evil.test/x\"")) == CF_OK);
    url = (cf_str){0};
    CF_REQUIRE(cf_auth_post_authenticating_url(&ctx, &url) == CF_OK);
    CF_CHECK(auth_str_is(url, "http://127.0.0.1:32123/"));
    cf_str_dispose(&url);
    CF_CHECK(cf_ctx_cookie_get(&ctx, SP("_campfire_session"), &gone) ==
             CF_NOT_FOUND);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);
    cf_test_clock_clear();
    auth_env_close(&env);
}

CF_TEST(session_cookie_overflow_is_an_error) {
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    cf_test_clock_set_fixed_us(T_NOW_US);
    cf_request req;
    cf_response resp;
    cf_ctx ctx;
    cf_test_req_init(&req);
    make_ctx(&env, &req, &resp, &ctx);
    /* A value that pushes the encrypted cookie past 4096 bytes (name
     * included) is CookieOverflow: unrescued, a 500. */
    cf_builder value = {0};
    CF_REQUIRE(cf_builder_append(&value, SP("\"")) == CF_OK);
    for (int i = 0; i < 8000; i++) {
        CF_REQUIRE(cf_builder_append(&value, SP("x")) == CF_OK);
    }
    CF_REQUIRE(cf_builder_append(&value, SP("\"")) == CF_OK);
    CF_CHECK(cf_auth_session_write(&ctx, SP("huge"),
                                   (cf_span){value.ptr, value.len}) ==
             CF_INTERNAL);
    cf_builder_dispose(&value);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);
    cf_test_clock_clear();
    auth_env_close(&env);
}

CF_TEST(session_cookie_state_round_trip) {
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    cf_test_clock_set_fixed_us(T_NOW_US);
    cf_request req;
    cf_response resp;
    cf_ctx ctx;
    cf_test_req_init(&req);
    make_ctx(&env, &req, &resp, &ctx);
    CF_REQUIRE(cf_auth_session_write(&ctx, SP("return_to_after_authenticating"),
                                     SP("\"/rooms/1\"")) == CF_OK);
    cf_str stored = {0};
    bool found = false;
    CF_REQUIRE(cf_auth_session_read(&ctx, SP("return_to_after_authenticating"),
                                    &stored, &found) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(auth_str_is(stored, "\"/rooms/1\""));
    cf_str_dispose(&stored);
    cf_span wire;
    CF_REQUIRE(cf_ctx_cookie_get(&ctx, SP("_campfire_session"), &wire) == CF_OK);

    /* A second request with the same cookie sees the value. */
    char header[2048];
    cookie_header(header, sizeof header, "_campfire_session", wire);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);
    cf_request req2;
    cf_response resp2;
    cf_ctx ctx2;
    cf_test_req_init(&req2);
    req2.path = SP("/");
    CF_REQUIRE(cf_test_req_header(&req2, SP("Cookie"), SP(header)) == CF_OK);
    make_ctx(&env, &req2, &resp2, &ctx2);
    stored = (cf_str){0};
    CF_REQUIRE(cf_auth_session_read(&ctx2, SP("return_to_after_authenticating"),
                                    &stored, &found) == CF_OK);
    CF_CHECK(found && auth_str_is(stored, "\"/rooms/1\""));
    cf_str_dispose(&stored);
    cf_ctx_destroy(&ctx2);
    cf_response_dispose(&resp2);
    cf_test_clock_clear();
    auth_env_close(&env);
}

/* Session::insert marks the session changed only when the stored value
 * differs; an identical write must neither re-encrypt nor re-set the cookie
 * (kit session.rs insert/commit). */
CF_TEST(identical_session_write_does_not_rewrite_cookie) {
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    cf_test_clock_set_fixed_us(T_NOW_US);

    cf_request req0;
    cf_response resp0;
    cf_ctx ctx0;
    cf_test_req_init(&req0);
    make_ctx(&env, &req0, &resp0, &ctx0);
    CF_REQUIRE(cf_auth_session_write(&ctx0, SP("k"), SP("\"v\"")) == CF_OK);
    cf_str wire = current_cookie(&ctx0, "_campfire_session");
    CF_REQUIRE(wire.ptr != NULL);
    cf_ctx_destroy(&ctx0);
    cf_response_dispose(&resp0);

    /* Request with the cookie: the identical write changes nothing. */
    char header[2048];
    cookie_header(header, sizeof header, "_campfire_session",
                  (cf_span){(const unsigned char *)wire.ptr, wire.len});
    cf_request req;
    cf_response resp;
    cf_ctx ctx;
    cf_test_req_init(&req);
    req.path = SP("/");
    CF_REQUIRE(cf_test_req_header(&req, SP("Cookie"), SP(header)) == CF_OK);
    make_ctx(&env, &req, &resp, &ctx);
    CF_REQUIRE(cf_auth_session_write(&ctx, SP("k"), SP("\"v\"")) == CF_OK);
    cf_str wire2 = current_cookie(&ctx, "_campfire_session");
    CF_CHECK(wire2.len == wire.len &&
             memcmp(wire2.ptr, wire.ptr, wire.len) == 0);
    cf_str_dispose(&wire2);
    /* serde_json Value equality: an escaped spelling of the same string is
     * also unchanged (and the stored representation is kept). */
    CF_REQUIRE(cf_auth_session_write(&ctx, SP("k"), SP("\"\\u0076\"")) == CF_OK);
    wire2 = current_cookie(&ctx, "_campfire_session");
    CF_CHECK(wire2.len == wire.len &&
             memcmp(wire2.ptr, wire.ptr, wire.len) == 0);
    cf_str_dispose(&wire2);
    CF_REQUIRE(cf_finish_cookies(&ctx) == CF_OK);
    cf_str head = response_head(&resp, &req);
    CF_CHECK(!str_has(&head, "Set-Cookie"));
    cf_str_dispose(&head);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);

    /* A changed write does re-encrypt and re-set the cookie. */
    cf_request req2;
    cf_response resp2;
    cf_ctx ctx2;
    cf_test_req_init(&req2);
    req2.path = SP("/");
    CF_REQUIRE(cf_test_req_header(&req2, SP("Cookie"), SP(header)) == CF_OK);
    make_ctx(&env, &req2, &resp2, &ctx2);
    CF_REQUIRE(cf_auth_session_write(&ctx2, SP("k"), SP("\"w\"")) == CF_OK);
    wire2 = current_cookie(&ctx2, "_campfire_session");
    CF_CHECK(wire2.ptr != NULL &&
             (wire2.len != wire.len ||
              memcmp(wire2.ptr, wire.ptr, wire.len) != 0));
    cf_str_dispose(&wire2);
    CF_REQUIRE(cf_finish_cookies(&ctx2) == CF_OK);
    head = response_head(&resp2, &req2);
    CF_CHECK(str_has(&head, "Set-Cookie: _campfire_session="));
    cf_str_dispose(&head);
    cf_ctx_destroy(&ctx2);
    cf_response_dispose(&resp2);

    cf_str_dispose(&wire);
    cf_test_clock_clear();
    auth_env_close(&env);
}

/* commit(): a session left with nothing but session_id is deleted, never
 * re-written (kit session.rs commit). */
CF_TEST(session_reduced_to_session_id_deletes_cookie) {
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    cf_test_clock_set_fixed_us(T_NOW_US);
    cf_request req;
    cf_response resp;
    cf_ctx ctx;
    cf_test_req_init(&req);
    make_ctx(&env, &req, &resp, &ctx);
    CF_REQUIRE(cf_auth_session_write(&ctx, SP("k"), SP("\"v\"")) == CF_OK);
    cf_span wire;
    CF_REQUIRE(cf_ctx_cookie_get(&ctx, SP("_campfire_session"), &wire) == CF_OK);

    /* Removing a key that is not there is not a change (Session::remove). */
    CF_REQUIRE(cf_auth_session_remove(&ctx, SP("missing")) == CF_OK);
    CF_CHECK(cf_ctx_cookie_get(&ctx, SP("_campfire_session"), &wire) == CF_OK);

    /* Removing the only key leaves session_id: the cookie is deleted. */
    CF_REQUIRE(cf_auth_session_remove(&ctx, SP("k")) == CF_OK);
    CF_CHECK(cf_ctx_cookie_get(&ctx, SP("_campfire_session"), &wire) ==
             CF_NOT_FOUND);
    CF_REQUIRE(cf_finish_cookies(&ctx) == CF_OK);
    cf_str head = response_head(&resp, &req);
    CF_CHECK(str_has(&head,
                     "Set-Cookie: _campfire_session=; path=/; max-age=0; "
                     "expires=Thu, 01 Jan 1970 00:00:00 GMT; samesite=lax"));
    cf_str_dispose(&head);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);
    cf_test_clock_clear();
    auth_env_close(&env);
}

/* Logout: reset_session + commit deletes _campfire_session (Reference
 * concerns.rs terminate_current_session / session.rs reset+commit) and the
 * deletion header matches the reference bytes (cookies.rs
 * delete_cookie_header). */
CF_TEST(logout_deletes_session_cookie_with_reference_bytes) {
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    cf_test_clock_set_fixed_us(T_NOW_US);
    int64_t id = auth_seed_user(&env, "S", "s@example.com", NULL, 0, 0);
    CF_REQUIRE(id != 0);

    /* A request that stores something in the encrypted session cookie. */
    cf_request req0;
    cf_response resp0;
    cf_ctx ctx0;
    cf_test_req_init(&req0);
    make_ctx(&env, &req0, &resp0, &ctx0);
    CF_REQUIRE(cf_auth_session_write(&ctx0, SP("return_to_after_authenticating"),
                                     SP("\"/rooms/1\"")) == CF_OK);
    cf_str session_wire = current_cookie(&ctx0, "_campfire_session");
    CF_REQUIRE(session_wire.ptr != NULL);
    cf_ctx_destroy(&ctx0);
    cf_response_dispose(&resp0);

    cf_request req1;
    cf_response resp1;
    cf_ctx ctx1;
    CF_REQUIRE(login_ctx(&env, &ctx1, &resp1, &req1, id) == CF_OK);
    cf_str token = current_cookie(&ctx1, "session_token");
    CF_REQUIRE(token.ptr != NULL);
    cf_ctx_destroy(&ctx1);
    cf_response_dispose(&resp1);

    /* A logout request carrying both cookies. */
    char header[4096];
    cookie_header(header, sizeof header, "session_token",
                  (cf_span){(const unsigned char *)token.ptr, token.len});
    size_t at = strlen(header);
    at += (size_t)snprintf(header + at, sizeof header - at,
                           "; _campfire_session=");
    for (size_t i = 0; i < session_wire.len && at + 4 < sizeof header; i++) {
        unsigned char c = (unsigned char)session_wire.ptr[i];
        if (c == '+' || c == '%') {
            at += (size_t)snprintf(header + at, sizeof header - at, "%%%02X", c);
        } else {
            header[at++] = (char)c;
        }
    }
    header[at] = '\0';

    cf_request req2;
    cf_response resp2;
    cf_ctx ctx2;
    cf_test_req_init(&req2);
    req2.path = SP("/session");
    CF_REQUIRE(cf_test_req_header(&req2, SP("Cookie"), SP(header)) == CF_OK);
    make_ctx(&env, &req2, &resp2, &ctx2);
    CF_REQUIRE(cf_authenticate(&ctx2) == CF_OK);
    CF_CHECK(!cf_auth_halted(&ctx2));
    CF_CHECK(ctx2.identity.kind == CF_AUTH_SESSION);
    CF_REQUIRE(cf_auth_terminate_current_session(&ctx2) == CF_OK);
    cf_span gone;
    CF_CHECK(cf_ctx_cookie_get(&ctx2, SP("_campfire_session"), &gone) ==
             CF_NOT_FOUND);
    CF_CHECK(cf_ctx_cookie_get(&ctx2, SP("session_token"), &gone) ==
             CF_NOT_FOUND);
    CF_REQUIRE(cf_finish_cookies(&ctx2) == CF_OK);
    cf_str head = response_head(&resp2, &req2);
    const char *session_delete =
        "Set-Cookie: _campfire_session=; path=/; max-age=0; "
        "expires=Thu, 01 Jan 1970 00:00:00 GMT; samesite=lax";
    const char *token_delete =
        "Set-Cookie: session_token=; path=/; max-age=0; "
        "expires=Thu, 01 Jan 1970 00:00:00 GMT; samesite=lax";
    CF_CHECK(str_has(&head, session_delete));
    CF_CHECK(str_has(&head, token_delete));
    if (str_has(&head, session_delete) && str_has(&head, token_delete)) {
        /* reset_session's delete is queued before cookies.delete("session_token"). */
        CF_CHECK(strstr(head.ptr, session_delete) <
                 strstr(head.ptr, token_delete));
    }
    /* Exactly the two deletions: no re-encrypted _campfire_session set. */
    CF_CHECK(str_count(&head, "Set-Cookie") == 2);
    cf_str_dispose(&head);

    /* The old token no longer authenticates. */
    cf_ctx_destroy(&ctx2);
    cf_response_dispose(&resp2);
    cf_request req3;
    cf_response resp3;
    cf_ctx ctx3;
    ctx_with_token(&env,
                   (cf_span){(const unsigned char *)token.ptr, token.len},
                   &req3, &resp3, &ctx3);
    CF_REQUIRE(cf_authenticate(&ctx3) == CF_OK);
    CF_CHECK(cf_auth_halted(&ctx3));
    CF_CHECK(resp3.status == 302);
    cf_ctx_destroy(&ctx3);
    cf_response_dispose(&resp3);
    cf_str_dispose(&session_wire);
    cf_str_dispose(&token);
    cf_test_clock_clear();
    auth_env_close(&env);
}

typedef struct {
    int64_t room_id;
    int64_t user_id;
} membership_destroy_arg;

static cf_err membership_destroy_cb(cf_tx *tx, void *user) {
    membership_destroy_arg *arg = user;
    cf_membership membership;
    bool found = false;
    cf_err rc = cf_membership_find_by_room_and_user(
        cf_tx_db(tx), arg->room_id, arg->user_id, &found, &membership);
    if (rc != CF_OK) return rc;
    if (!found) return CF_NOT_FOUND;
    rc = cf_membership_destroy(tx, &membership);
    cf_membership_dispose(&membership);
    return rc;
}

CF_TEST(membership_removal_disconnects_and_denies_room) {
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    int64_t user = auth_seed_user(&env, "Member", "m@example.com", NULL, 0, 0);
    CF_REQUIRE(user != 0);
    auth_seed_room_membership(&env, 2001, user, "Rooms::Open");
    CF_CHECK(cf_authorize_room(env.reader, user, 2001) == CF_OK);

    membership_destroy_arg arg = {.room_id = 2001, .user_id = user};
    CF_REQUIRE(cf_write(env.app, membership_destroy_cb, &arg) == CF_OK);
    CF_CHECK(cf_authorize_room(env.reader, user, 2001) == CF_NOT_FOUND);
    bool saw_disconnect = false;
    for (size_t i = 0; i < env.event_count; i++) {
        if (env.events[i].kind == CF_EVENT_DISCONNECT_USER &&
            env.events[i].user_id == user && env.events[i].reconnect) {
            saw_disconnect = true;
        }
    }
    CF_CHECK(saw_disconnect);
    auth_env_close(&env);
}

CF_TEST(wrong_user_room_scope_is_denied) {
    auth_env env;
    CF_REQUIRE(auth_env_open(&env));
    int64_t a = auth_seed_user(&env, "A", "a@example.com", NULL, 0, 0);
    int64_t b = auth_seed_user(&env, "B", "b@example.com", NULL, 0, 0);
    CF_REQUIRE(a != 0 && b != 0);
    auth_seed_room_membership(&env, 3001, a, "Rooms::Open");
    auth_seed_room_membership(&env, 3002, b, "Rooms::Closed");
    CF_CHECK(cf_authorize_room(env.reader, a, 3001) == CF_OK);
    CF_CHECK(cf_authorize_room(env.reader, b, 3002) == CF_OK);
    CF_CHECK(cf_authorize_room(env.reader, a, 3002) == CF_NOT_FOUND);
    CF_CHECK(cf_authorize_room(env.reader, b, 3001) == CF_NOT_FOUND);
    CF_CHECK(cf_authorize_room(env.reader, a, 3999) == CF_NOT_FOUND);
    auth_env_close(&env);
}

CF_TEST_MAIN()
