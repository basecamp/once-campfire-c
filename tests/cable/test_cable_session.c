/* tests/cable/test_cable_session.c — C01 handshake authentication with A01:
 * ApplicationCable::Connection's SessionAuthenticator (signed session_token
 * cookie -> Session::find_by_token -> User::find_by_id), plus the same
 * callback driving the server hook's post-upgrade auth gate. */
#include "cable_testutil.h"

#include "auth.h"
#include "db/db_testutil.h"

#define CAB_SECRET "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

typedef struct {
    cf_db *reader;
} session_fixture;

static session_fixture g_fixture;

static cf_cable_session_auth g_session_auth;

static bool seed_session_db(cf_db_scratch *scratch) {
    if (cf_db_test_exec(cf_db_handle(scratch->db),
                        "INSERT INTO users "
                        "(bio,bot_token,created_at,email_address,name,"
                        "password_digest,role,status,updated_at) VALUES "
                        "(NULL,NULL,'2026-01-02 03:04:05','u@example.com',"
                        "'Cable Tester','x',0,0,'2026-01-02 03:04:05')") !=
        SQLITE_OK) {
        return false;
    }
    if (cf_db_test_exec(cf_db_handle(scratch->db),
                        "INSERT INTO sessions "
                        "(created_at,last_active_at,token,updated_at,user_id) "
                        "VALUES ('2026-01-02 03:04:05','2026-01-02 03:04:05',"
                        "'session-token-1',"
                        "'2026-01-02 03:04:05',1)") != SQLITE_OK) {
        return false;
    }
    return true;
}

static cf_str make_cookie(const char *token, bool url_escape) {
    cf_str signed_value = {0};
    CF_REQUIRE(cf_auth_signed_cookie_generate(
                   (cf_span){(const unsigned char *)CAB_SECRET,
                             sizeof CAB_SECRET - 1},
                   (cf_span){(const unsigned char *)"session_token", 13},
                   (cf_span){(const unsigned char *)token, strlen(token)},
                   false, 0, &signed_value) == CF_OK);
    if (!url_escape) return signed_value;
    /* Percent-encode '=' the way a browser does. */
    cf_builder escaped = {0};
    for (size_t i = 0; i < signed_value.len; i++) {
        unsigned char c = (unsigned char)signed_value.ptr[i];
        if (c == '=') {
            CF_REQUIRE(cf_builder_append(
                           &escaped,
                           (cf_span){(const unsigned char *)"%3D", 3}) ==
                       CF_OK);
        } else {
            CF_REQUIRE(cf_builder_append(&escaped,
                                         (cf_span){&c, 1}) == CF_OK);
        }
    }
    char *text = malloc(escaped.len + 1);
    CF_REQUIRE(text != NULL);
    memcpy(text, escaped.ptr, escaped.len);
    text[escaped.len] = '\0';
    size_t len = escaped.len;
    cf_builder_dispose(&escaped);
    cf_str_dispose(&signed_value);
    return (cf_str){text, len};
}

static bool authenticate_with_cookie(const char *cookie_value,
                                     bool include_cookie, bool *ok,
                                     int64_t *user_id) {
    cf_header headers[2];
    size_t n = 0;
    char cookie_line[2048];
    if (include_cookie) {
        snprintf(cookie_line, sizeof cookie_line, "session_token=%s",
                 cookie_value);
        headers[n++] = (cf_header){
            (cf_span){(const unsigned char *)"cookie", 6},
            (cf_span){(const unsigned char *)cookie_line,
                      strlen(cookie_line)}};
    }
    cf_cable_request request;
    memset(&request, 0, sizeof request);
    request.method = CF_GET;
    request.target = (cf_span){(const unsigned char *)"/cable", 6};
    request.path = request.target;
    request.headers = headers;
    request.header_count = n;
    g_session_auth.reader = g_fixture.reader;
    g_session_auth.secret_key_base = (cf_span){
        (const unsigned char *)CAB_SECRET, sizeof CAB_SECRET - 1};
    bool authenticated = false;
    int64_t id = 0;
    cf_err rc = cf_cable_session_authenticate(&g_session_auth, &request,
                                              &authenticated, &id);
    if (rc != CF_OK) return false;
    *ok = authenticated;
    *user_id = id;
    return true;
}

CF_TEST(session_authenticator_resolves_the_signed_cookie) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    CF_REQUIRE(seed_session_db(&scratch));
    CF_REQUIRE(cf_db_open(scratch.path, true, &g_fixture.reader) == CF_OK);

    cf_str cookie = make_cookie("session-token-1", false);
    bool ok = false;
    int64_t user_id = 0;
    CF_REQUIRE(authenticate_with_cookie(cookie.ptr, true, &ok, &user_id));
    CF_CHECK(ok == true);
    CF_CHECK(user_id == 1);
    cf_str_dispose(&cookie);

    /* URL-escaped wire form. */
    cookie = make_cookie("session-token-1", true);
    CF_REQUIRE(authenticate_with_cookie(cookie.ptr, true, &ok, &user_id));
    CF_CHECK(ok == true);
    CF_CHECK(user_id == 1);
    cf_str_dispose(&cookie);

    /* No cookie, tampered signature, unknown token: not authenticated. */
    CF_REQUIRE(authenticate_with_cookie("", false, &ok, &user_id));
    CF_CHECK(ok == false);

    cookie = make_cookie("session-token-1", false);
    if (cookie.len > 3) cookie.ptr[cookie.len - 1] = 'x';
    CF_REQUIRE(authenticate_with_cookie(cookie.ptr, true, &ok, &user_id));
    CF_CHECK(ok == false);
    cf_str_dispose(&cookie);

    cookie = make_cookie("session-token-404", false);
    CF_REQUIRE(authenticate_with_cookie(cookie.ptr, true, &ok, &user_id));
    CF_CHECK(ok == false);
    cf_str_dispose(&cookie);

    cf_db_close(g_fixture.reader);
    g_fixture.reader = NULL;
    cf_db_scratch_close(&scratch);
}

CF_TEST_MAIN()
