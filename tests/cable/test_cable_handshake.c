/* tests/cable/test_cable_handshake.c — C01 handshake vectors through the
 * cf_cable_server_upgrade hook (the /cable front mount, server.rs call):
 * method/Upgrade/Connection checks, version 13, key validation, the reference
 * Origin policy (D-C07: proxy headers ignored), subprotocol negotiation,
 * permessage-deflate negotiation, the 101 response and post-upgrade auth
 * (unauthorized disconnect + close 1000). */
#include "cable_testutil.h"

#include "auth.h"
#include "db/db_testutil.h"

#define HDR(name, value)                                                   \
    ((cf_header){                                                          \
        (cf_span){(const unsigned char *)(name), sizeof(name) - 1},        \
        (cf_span){(const unsigned char *)(value), sizeof(value) - 1}})

#define RFC_KEY "dGhlIHNhbXBsZSBub25jZQ=="
#define RFC_ACCEPT "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="

typedef struct {
    bool authenticated;
    int64_t user_id;
    int auth_calls;
} handshake_auth_state;

static handshake_auth_state g_auth;
static ct_capture g_msg;

static cf_err handshake_auth(void *user, cf_cable_socket *socket,
                             const cf_cable_request *request,
                             bool *authenticated, int64_t *user_id) {
    (void)user;
    (void)socket;
    (void)request;
    *authenticated = g_auth.authenticated;
    *user_id = g_auth.user_id;
    g_auth.auth_calls++;
    return CF_OK;
}

static cf_cable_server *make_server(bool assume_ssl, bool allow_same_origin,
                                    bool disable_forgery,
                                    const char *const *allowed,
                                    size_t allowed_len,
                                    cf_cable_on_text_fn on_text) {
    cf_cable_server_config config;
    cf_cable_server_config_default(&config);
    config.assume_ssl = assume_ssl;
    config.allow_same_origin_as_host = allow_same_origin;
    config.disable_request_forgery_protection = disable_forgery;
    config.allowed_request_origins = allowed;
    config.allowed_request_origins_len = allowed_len;
    config.hooks.authenticate = handshake_auth;
    config.hooks.on_text = on_text;
    config.hooks.on_text_user = &g_msg;
    cf_cable_server *server = NULL;
    CF_REQUIRE(cf_cable_server_create(&config, &server) == CF_OK);
    return server;
}

static cf_http_upgrade_result hook_call_full(
    cf_cable_server *server, const char *method, const char *path,
    cf_header *headers, size_t n, int fd, const unsigned char *pending,
    size_t pending_len, cf_builder *reply_out,
    cf_http_upgrade_taken_fn *taken_out, void **taken_user_out) {
    cf_http_upgrade_request req;
    memset(&req, 0, sizeof req);
    req.fd = fd;
    req.method = strcmp(method, "GET") == 0 ? CF_GET : CF_POST;
    req.target = (cf_span){(const unsigned char *)path, strlen(path)};
    req.path = req.target;
    req.peer_ip = (cf_span){(const unsigned char *)"127.0.0.1", 9};
    req.headers = headers;
    req.header_count = n;
    req.pending = pending;
    req.pending_len = pending_len;
    cf_http_upgrade_result result = cf_cable_server_upgrade(server, &req);
    *reply_out = req.reply;
    if (taken_out != NULL) *taken_out = req.taken;
    if (taken_user_out != NULL) *taken_user_out = req.taken_user;
    return result;
}

static cf_http_upgrade_result hook_call(cf_cable_server *server,
                                        const char *method, const char *path,
                                        cf_header *headers, size_t n, int fd,
                                        cf_builder *reply_out) {
    return hook_call_full(server, method, path, headers, n, fd, NULL, 0,
                          reply_out, NULL, NULL);
}

static void expect_404(cf_cable_server *server, cf_header *headers, size_t n,
                       const char *label) {
    cf_builder reply = {0};
    cf_http_upgrade_result r =
        hook_call(server, "GET", "/cable", headers, n, -1, &reply);
    if (r != CF_HTTP_UPGRADE_REPLY) {
        fprintf(stderr, "    %s: result %d\n", label, (int)r);
    }
    CF_CHECK(r == CF_HTTP_UPGRADE_REPLY);
    CF_REQUIRE(reply.len != 0);
    CF_CHECK(memmem(reply.ptr, reply.len, "HTTP/1.1 404 Not Found", 22) !=
             NULL);
    CF_CHECK(memmem(reply.ptr, reply.len,
                    "Content-Type: text/plain; charset=utf-8", 38) != NULL);
    CF_CHECK(memmem(reply.ptr, reply.len, "\r\n\r\nPage not found", 18) !=
             NULL);
    cf_builder_dispose(&reply);
}

/* Run one accepted upgrade the way the loop does: the hook returns TAKEN,
 * the caller detaches (nothing to do here) and then invokes the stored
 * continuation that starts the connection thread. */
static bool take_connection(cf_cable_server *server, cf_header *headers,
                            size_t n, int fds[2]) {
    if (!ct_socketpair(fds)) return false;
    cf_builder reply = {0};
    cf_http_upgrade_taken_fn taken = NULL;
    void *taken_user = NULL;
    cf_http_upgrade_result r = hook_call_full(server, "GET", "/cable", headers,
                                              n, fds[0], NULL, 0, &reply,
                                              &taken, &taken_user);
    cf_builder_dispose(&reply);
    if (r != CF_HTTP_UPGRADE_TAKEN || taken == NULL) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (taken(taken_user, NULL) != CF_OK) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    return true;
}

static void expect_101(int fd, const char *must_contain) {
    char head[1024];
    ssize_t n = ct_read_http_head(fd, head, sizeof head, 2000);
    CF_REQUIRE(n > 0);
    CF_CHECK(strstr(head, "HTTP/1.1 101 Switching Protocols") != NULL);
    CF_CHECK(strstr(head, "sec-websocket-accept: " RFC_ACCEPT) != NULL);
    if (must_contain != NULL) {
        CF_CHECK(strstr(head, must_contain) != NULL);
    }
}

static void expect_text_frame_labeled(int fd, const char *expect,
                                      const char *label) {
    unsigned opcode = 0;
    bool rsv1 = false;
    unsigned char payload[1024];
    ssize_t n = ct_read_server_frame(fd, &opcode, &rsv1, payload,
                                     sizeof payload, 3000);
    if (n < 0) fprintf(stderr, "    %s: no frame\n", label);
    CF_REQUIRE(n >= 0);
    if (opcode != 0x1 || (size_t)n != strlen(expect)) {
        fprintf(stderr, "    %s: n=%zd opcode=%u expect_len=%zu\n", label, n,
                opcode, strlen(expect));
    }
    CF_CHECK(opcode == 0x1);
    CF_CHECK((size_t)n == strlen(expect));
    if ((size_t)n == strlen(expect)) {
        CF_CHECK(memcmp(payload, expect, (size_t)n) == 0);
    }
}


static void expect_close_frame(int fd, uint16_t code) {
    unsigned opcode = 0;
    bool rsv1 = false;
    unsigned char payload[64];
    ssize_t n = ct_read_server_frame(fd, &opcode, &rsv1, payload,
                                     sizeof payload, 3000);
    CF_REQUIRE(n >= 0);
    CF_CHECK(opcode == 0x8);
    if (n == 2) {
        CF_CHECK(((payload[0] << 8) | payload[1]) == code);
    }
}

static cf_header valid_headers[] = {
    HDR("host", "127.0.0.1:32123"),
    HDR("origin", "http://127.0.0.1:32123"),
    HDR("upgrade", "websocket"),
    HDR("connection", "Upgrade"),
    HDR("sec-websocket-version", "13"),
    HDR("sec-websocket-key", RFC_KEY),
};

CF_TEST(rejects_invalid_upgrades_with_the_reference_404) {
    g_auth.authenticated = true;
    cf_cable_server *server = make_server(false, true, false, NULL, 0, NULL);

    /* not the mount */
    {
        cf_builder reply = {0};
        cf_http_upgrade_result r =
            hook_call(server, "GET", "/other", valid_headers, 6, -1, &reply);
        CF_CHECK(r == CF_HTTP_UPGRADE_PASS);
        CF_CHECK(reply.len == 0);
        cf_builder_dispose(&reply);
    }
    /* an ordinary GET on /cable (no upgrade headers) */
    {
        cf_header h[] = {HDR("host", "127.0.0.1:32123"),
                         HDR("origin", "http://127.0.0.1:32123")};
        expect_404(server, h, 2, "plain GET");
    }
    /* POST with otherwise valid upgrade headers */
    {
        cf_builder reply = {0};
        cf_http_upgrade_result r = hook_call(server, "POST", "/cable",
                                             valid_headers, 6, -1, &reply);
        CF_CHECK(r == CF_HTTP_UPGRADE_REPLY);
        CF_CHECK(memmem(reply.ptr, reply.len, "404 Not Found", 13) != NULL);
        cf_builder_dispose(&reply);
    }
    /* missing Upgrade header */
    {
        cf_header h[] = {HDR("host", "127.0.0.1:32123"),
                         HDR("origin", "http://127.0.0.1:32123"),
                         HDR("connection", "Upgrade"),
                         HDR("sec-websocket-version", "13"),
                         HDR("sec-websocket-key", RFC_KEY)};
        expect_404(server, h, 5, "no upgrade header");
    }
    /* Connection without the upgrade token */
    {
        cf_header h[] = {HDR("host", "127.0.0.1:32123"),
                         HDR("origin", "http://127.0.0.1:32123"),
                         HDR("upgrade", "websocket"),
                         HDR("connection", "keep-alive"),
                         HDR("sec-websocket-version", "13"),
                         HDR("sec-websocket-key", RFC_KEY)};
        expect_404(server, h, 6, "connection token");
    }
    /* version 8 */
    {
        cf_header h[] = {HDR("host", "127.0.0.1:32123"),
                         HDR("origin", "http://127.0.0.1:32123"),
                         HDR("upgrade", "websocket"),
                         HDR("connection", "Upgrade"),
                         HDR("sec-websocket-version", "8"),
                         HDR("sec-websocket-key", RFC_KEY)};
        expect_404(server, h, 6, "version 8");
    }
    /* missing key */
    {
        cf_header h[] = {HDR("host", "127.0.0.1:32123"),
                         HDR("origin", "http://127.0.0.1:32123"),
                         HDR("upgrade", "websocket"),
                         HDR("connection", "Upgrade"),
                         HDR("sec-websocket-version", "13")};
        expect_404(server, h, 5, "missing key");
    }
    /* a key that decodes to fewer than 16 bytes */
    {
        cf_header h[] = {HDR("host", "127.0.0.1:32123"),
                         HDR("origin", "http://127.0.0.1:32123"),
                         HDR("upgrade", "websocket"),
                         HDR("connection", "Upgrade"),
                         HDR("sec-websocket-version", "13"),
                         HDR("sec-websocket-key", "QUJD")};
        expect_404(server, h, 6, "short key");
    }
    /* a key that is not canonical base64 */
    {
        cf_header h[] = {HDR("host", "127.0.0.1:32123"),
                         HDR("origin", "http://127.0.0.1:32123"),
                         HDR("upgrade", "websocket"),
                         HDR("connection", "Upgrade"),
                         HDR("sec-websocket-version", "13"),
                         HDR("sec-websocket-key", "!!!not-base64!!!")};
        expect_404(server, h, 6, "invalid base64");
    }
    cf_cable_server_destroy(server);
}

CF_TEST(origin_policy_is_the_reference_policy) {
    g_auth.authenticated = true;
    /* Same origin, no SSL assumed: http://host matches. */
    {
        cf_cable_server *server =
            make_server(false, true, false, NULL, 0, NULL);
        int fds[2];
        CF_REQUIRE(take_connection(server, valid_headers, 6, fds));
        expect_101(fds[1], NULL);
        close(fds[1]);
        cf_cable_server_destroy(server);
    }
    /* https origin when SSL is not assumed, evil origin, absent origin. */
    {
        cf_cable_server *server =
            make_server(false, true, false, NULL, 0, NULL);
        cf_header https[] = {HDR("host", "127.0.0.1:32123"),
                             HDR("origin", "https://127.0.0.1:32123"),
                             HDR("upgrade", "websocket"),
                             HDR("connection", "Upgrade"),
                             HDR("sec-websocket-version", "13"),
                             HDR("sec-websocket-key", RFC_KEY)};
        expect_404(server, https, 6, "https origin");
        cf_header evil[] = {HDR("host", "127.0.0.1:32123"),
                            HDR("origin", "http://evil.example"),
                            HDR("upgrade", "websocket"),
                            HDR("connection", "Upgrade"),
                            HDR("sec-websocket-version", "13"),
                            HDR("sec-websocket-key", RFC_KEY)};
        expect_404(server, evil, 6, "evil origin");
        cf_header none[] = {HDR("host", "127.0.0.1:32123"),
                            HDR("upgrade", "websocket"),
                            HDR("connection", "Upgrade"),
                            HDR("sec-websocket-version", "13"),
                            HDR("sec-websocket-key", RFC_KEY)};
        expect_404(server, none, 5, "no origin");
        cf_cable_server_destroy(server);
    }
    /* assume_ssl: the same-origin rule compares against https://host. */
    {
        cf_cable_server *server =
            make_server(true, true, false, NULL, 0, NULL);
        cf_header http[] = {HDR("host", "127.0.0.1:32123"),
                            HDR("origin", "http://127.0.0.1:32123"),
                            HDR("upgrade", "websocket"),
                            HDR("connection", "Upgrade"),
                            HDR("sec-websocket-version", "13"),
                            HDR("sec-websocket-key", RFC_KEY)};
        expect_404(server, http, 6, "http origin with assume_ssl");
        cf_header https[] = {HDR("host", "127.0.0.1:32123"),
                             HDR("origin", "https://127.0.0.1:32123"),
                             HDR("upgrade", "websocket"),
                             HDR("connection", "Upgrade"),
                             HDR("sec-websocket-version", "13"),
                             HDR("sec-websocket-key", RFC_KEY)};
        int fds[2];
        CF_REQUIRE(take_connection(server, https, 6, fds));
        expect_101(fds[1], NULL);
        close(fds[1]);
        cf_cable_server_destroy(server);
    }
    /* An explicitly allowed origin passes even cross-origin. */
    {
        static const char *allowed[] = {"http://evil.example"};
        cf_cable_server *server = make_server(false, true, false, allowed, 1,
                                              NULL);
        cf_header evil[] = {HDR("host", "127.0.0.1:32123"),
                            HDR("origin", "http://evil.example"),
                            HDR("upgrade", "websocket"),
                            HDR("connection", "Upgrade"),
                            HDR("sec-websocket-version", "13"),
                            HDR("sec-websocket-key", RFC_KEY)};
        int fds[2];
        CF_REQUIRE(take_connection(server, evil, 6, fds));
        expect_101(fds[1], NULL);
        close(fds[1]);
        cf_cable_server_destroy(server);
    }
    /* D-C07: X-Forwarded-* never changes the listener's scheme. The
     * reference would treat this https origin as same-origin through
     * X-Forwarded-Proto; the C port ignores the header (no SSL listener). */
    {
        cf_cable_server *server =
            make_server(false, true, false, NULL, 0, NULL);
        cf_header https[] = {HDR("host", "127.0.0.1:32123"),
                             HDR("origin", "https://127.0.0.1:32123"),
                             HDR("x-forwarded-proto", "https"),
                             HDR("upgrade", "websocket"),
                             HDR("connection", "Upgrade"),
                             HDR("sec-websocket-version", "13"),
                             HDR("sec-websocket-key", RFC_KEY)};
        expect_404(server, https, 7, "x-forwarded-proto ignored");
        cf_cable_server_destroy(server);
    }
    /* An explicitly disabled forgery check accepts a missing Origin. */
    {
        cf_cable_server *server = make_server(false, true, true, NULL, 0,
                                              NULL);
        cf_header none[] = {HDR("host", "127.0.0.1:32123"),
                            HDR("upgrade", "websocket"),
                            HDR("connection", "Upgrade"),
                            HDR("sec-websocket-version", "13"),
                            HDR("sec-websocket-key", RFC_KEY)};
        int fds[2];
        CF_REQUIRE(take_connection(server, none, 5, fds));
        expect_101(fds[1], NULL);
        close(fds[1]);
        cf_cable_server_destroy(server);
    }
}

CF_TEST(negotiates_the_actioncable_subprotocol_and_deflate) {
    g_auth.authenticated = true;
    cf_cable_server *server = make_server(false, true, false, NULL, 0, NULL);

    /* The client's order decides; both listed protocols are supported. */
    {
        cf_header h[] = {HDR("host", "127.0.0.1:32123"),
                         HDR("origin", "http://127.0.0.1:32123"),
                         HDR("upgrade", "websocket"),
                         HDR("connection", "Upgrade"),
                         HDR("sec-websocket-version", "13"),
                         HDR("sec-websocket-key", RFC_KEY),
                         HDR("sec-websocket-protocol",
                             "foo, actioncable-unsupported, "
                             "actioncable-v1-json")};
        int fds[2];
        CF_REQUIRE(take_connection(server, h, 7, fds));
        expect_101(fds[1], "sec-websocket-protocol: actioncable-unsupported");
        close(fds[1]);
    }
    {
        cf_header h[] = {HDR("host", "127.0.0.1:32123"),
                         HDR("origin", "http://127.0.0.1:32123"),
                         HDR("upgrade", "websocket"),
                         HDR("connection", "Upgrade"),
                         HDR("sec-websocket-version", "13"),
                         HDR("sec-websocket-key", RFC_KEY),
                         HDR("sec-websocket-protocol",
                             "actioncable-v1-json, actioncable-unsupported")};
        int fds[2];
        CF_REQUIRE(take_connection(server, h, 7, fds));
        expect_101(fds[1], "sec-websocket-protocol: actioncable-v1-json");
        close(fds[1]);
    }
    {
        cf_header h[] = {HDR("host", "127.0.0.1:32123"),
                         HDR("origin", "http://127.0.0.1:32123"),
                         HDR("upgrade", "websocket"),
                         HDR("connection", "Upgrade"),
                         HDR("sec-websocket-version", "13"),
                         HDR("sec-websocket-key", RFC_KEY),
                         HDR("sec-websocket-protocol", "foo")};
        int fds[2];
        CF_REQUIRE(take_connection(server, h, 7, fds));
        char head[512];
        CF_REQUIRE(ct_read_http_head(fds[1], head, sizeof head, 2000) > 0);
        CF_CHECK(strstr(head, "sec-websocket-protocol") == NULL);
        close(fds[1]);
    }
    /* permessage-deflate offers we can answer. */
    {
        cf_header h[] = {
            HDR("host", "127.0.0.1:32123"),
            HDR("origin", "http://127.0.0.1:32123"),
            HDR("upgrade", "websocket"),
            HDR("connection", "Upgrade"),
            HDR("sec-websocket-version", "13"),
            HDR("sec-websocket-key", RFC_KEY),
            HDR("sec-websocket-extensions",
                "permessage-deflate; client_max_window_bits")};
        int fds[2];
        CF_REQUIRE(take_connection(server, h, 7, fds));
        expect_101(fds[1], "sec-websocket-extensions: " CF_CABLE_DEFLATE_RESPONSE);
        close(fds[1]);
    }
    {
        cf_header h[] = {
            HDR("host", "127.0.0.1:32123"),
            HDR("origin", "http://127.0.0.1:32123"),
            HDR("upgrade", "websocket"),
            HDR("connection", "Upgrade"),
            HDR("sec-websocket-version", "13"),
            HDR("sec-websocket-key", RFC_KEY),
            HDR("sec-websocket-extensions",
                "x-webkit-deflate-frame, permessage-deflate")};
        int fds[2];
        CF_REQUIRE(take_connection(server, h, 7, fds));
        expect_101(fds[1], "sec-websocket-extensions");
        close(fds[1]);
    }
    /* Unsuitable offers leave compression off. */
    {
        cf_header h[] = {
            HDR("host", "127.0.0.1:32123"),
            HDR("origin", "http://127.0.0.1:32123"),
            HDR("upgrade", "websocket"),
            HDR("connection", "Upgrade"),
            HDR("sec-websocket-version", "13"),
            HDR("sec-websocket-key", RFC_KEY),
            HDR("sec-websocket-extensions",
                "permessage-deflate; server_max_window_bits=10")};
        int fds[2];
        CF_REQUIRE(take_connection(server, h, 7, fds));
        char head[512];
        CF_REQUIRE(ct_read_http_head(fds[1], head, sizeof head, 2000) > 0);
        CF_CHECK(strstr(head, "sec-websocket-extensions") == NULL);
        close(fds[1]);
    }
    {
        cf_header h[] = {
            HDR("host", "127.0.0.1:32123"),
            HDR("origin", "http://127.0.0.1:32123"),
            HDR("upgrade", "websocket"),
            HDR("connection", "Upgrade"),
            HDR("sec-websocket-version", "13"),
            HDR("sec-websocket-key", RFC_KEY),
            HDR("sec-websocket-extensions", "permessage-deflate; unknown")};
        int fds[2];
        CF_REQUIRE(take_connection(server, h, 7, fds));
        char head[512];
        CF_REQUIRE(ct_read_http_head(fds[1], head, sizeof head, 2000) > 0);
        CF_CHECK(strstr(head, "sec-websocket-extensions") == NULL);
        close(fds[1]);
    }
    cf_cable_server_destroy(server);
}

CF_TEST(accepted_connection_welcomes_or_rejects_unauthenticated) {
    /* Authorized: 101 + welcome, then pipelined input is consumed. */
    g_auth.authenticated = true;
    g_auth.user_id = 42;
    g_auth.auth_calls = 0;
    memset(&g_msg, 0, sizeof g_msg);
    cf_cable_server *server = make_server(false, true, false, NULL, 0, NULL);
    {
        int fds[2];
        CF_REQUIRE(take_connection(server, valid_headers, 6, fds));
        expect_101(fds[1], NULL);
        expect_text_frame_labeled(fds[1], "{\"type\":\"welcome\"}", "welcome-auth");
        /* Wait until the server counts the live connection. */
        for (int i = 0; i < 100 && cf_cable_server_connections(server) != 1;
             i++) {
            usleep(10 * 1000);
        }
        CF_CHECK(cf_cable_server_connections(server) == 1);
        close(fds[1]);
    }
    cf_cable_server_destroy(server);
    CF_CHECK(g_auth.auth_calls == 1);

    /* Unauthorized: 101 then the disconnect frame, then close 1000. */
    g_auth.authenticated = false;
    g_auth.user_id = 0;
    server = make_server(false, true, false, NULL, 0, NULL);
    {
        int fds[2];
        CF_REQUIRE(take_connection(server, valid_headers, 6, fds));
        expect_101(fds[1], NULL);
        expect_text_frame_labeled(
            fds[1],
            "{\"type\":\"disconnect\",\"reason\":\"unauthorized\","
            "\"reconnect\":false}",
            "unauthorized");
        expect_close_frame(fds[1], 1000);
        close(fds[1]);
        for (int i = 0;
             i < 200 && cf_cable_server_connections(server) != 0; i++) {
            usleep(10 * 1000);
        }
        cf_cable_socket_stats stats;
        cf_cable_server_stats(server, &stats);
        CF_CHECK(stats.unauthorized_closes == 1);
        CF_CHECK(stats.protocol_closes == 0);
        CF_CHECK(stats.queue_cap_closes == 0);
    }
    cf_cable_server_destroy(server);

    /* Pipelined frames: bytes received after the handshake reach the reader. */
    g_auth.authenticated = true;
    memset(&g_msg, 0, sizeof g_msg);
    unsigned char pending[64];
    const char *early = "early";
    size_t pending_len = ct_client_frame(pending, 0x1, true, false,
                                         (const unsigned char *)early, 5);
    server = make_server(false, true, false, NULL, 0, ct_on_text_capture);
    {
        int fds[2];
        CF_REQUIRE(ct_socketpair(fds));
        cf_http_upgrade_request req;
        memset(&req, 0, sizeof req);
        req.fd = fds[0];
        req.method = CF_GET;
        req.target = (cf_span){(const unsigned char *)"/cable", 6};
        req.path = req.target;
        req.peer_ip = (cf_span){(const unsigned char *)"127.0.0.1", 9};
        req.headers = valid_headers;
        req.header_count = 6;
        req.pending = pending;
        req.pending_len = pending_len;
        cf_http_upgrade_result r = cf_cable_server_upgrade(server, &req);
        CF_CHECK(r == CF_HTTP_UPGRADE_TAKEN);
        if (r == CF_HTTP_UPGRADE_TAKEN) {
            CF_REQUIRE(req.taken != NULL);
            CF_CHECK(req.taken(req.taken_user, NULL) == CF_OK);
            expect_101(fds[1], NULL);
            expect_text_frame_labeled(fds[1], "{\"type\":\"welcome\"}", "welcome-pending");
            /* The pipelined text arrives through on_text; echo it back so the
             * client can observe the handoff.  The default hooks above do not
             * echo, so only receipt is checked. */
            for (int i = 0; i < 100 && g_msg.text_count == 0; i++) {
                usleep(10 * 1000);
            }
            CF_CHECK(g_msg.text_count == 1);
            if (g_msg.text_count == 1) {
                CF_CHECK(strcmp(g_msg.text[0], early) == 0);
            }
        }
        cf_builder_dispose(&req.reply);
        close(fds[1]);
    }
    cf_cable_server_destroy(server);
}

/* ---- header-readability gate (the cable crate's to_str sites) -------------- */
/* The pin reads the handshake headers through http::HeaderValue::to_str
 * (http 1.5.0 value.rs:558-560: HTAB or 0x20..=0x7E is readable), so a value
 * carrying any other byte reads as ABSENT.  Every expectation below is the
 * extracted-pin oracle's output (docs/devel/evidence/cable-handshake-gate.md).
 * Single-get sites (Upgrade, Version, Key, Origin, Host) consult the first
 * value only; list sites (Connection, Protocol, Extensions, Cookie) drop an
 * unreadable value whole and still read later duplicates. */

static void expect_gate_outcome(cf_cable_server *server, cf_header *h, size_t n,
                                bool expect_101, const char *must,
                                const char *must_not, const char *label) {
    if (!expect_101) {
        expect_404(server, h, n, label);
        return;
    }
    int fds[2];
    CF_REQUIRE(ct_socketpair(fds));
    cf_builder reply = {0};
    cf_http_upgrade_taken_fn taken = NULL;
    void *taken_user = NULL;
    cf_http_upgrade_result r =
        hook_call_full(server, "GET", "/cable", h, n, fds[0], NULL, 0, &reply,
                       &taken, &taken_user);
    cf_builder_dispose(&reply);
    if (r != CF_HTTP_UPGRADE_TAKEN || taken == NULL) {
        fprintf(stderr, "    %s: result %d taken=%p\n", label, (int)r,
                (void *)taken);
    }
    CF_REQUIRE(r == CF_HTTP_UPGRADE_TAKEN);
    CF_REQUIRE(taken != NULL);
    CF_REQUIRE(taken(taken_user, NULL) == CF_OK);
    char head[1024];
    CF_REQUIRE(ct_read_http_head(fds[1], head, sizeof head, 2000) > 0);
    if (strstr(head, "HTTP/1.1 101 Switching Protocols") == NULL) {
        fprintf(stderr, "    %s: head=%s\n", label, head);
    }
    CF_CHECK(strstr(head, "HTTP/1.1 101 Switching Protocols") != NULL);
    if (must != NULL && strstr(head, must) == NULL) {
        fprintf(stderr, "    %s: missing '%s'; head=%s\n", label, must, head);
    }
    if (must != NULL) CF_CHECK(strstr(head, must) != NULL);
    if (must_not != NULL && strstr(head, must_not) != NULL) {
        fprintf(stderr, "    %s: unexpected '%s'\n", label, must_not);
    }
    if (must_not != NULL) CF_CHECK(strstr(head, must_not) == NULL);
    close(fds[1]);
}

/* Copy the six base headers, replacing `name`'s value with the raw bytes;
 * a header the base does not carry (Protocol, Extensions) is appended. */
static size_t gate_headers(cf_header *out, const char *name,
                           const unsigned char *value, size_t value_len) {
    size_t hl = strlen(name);
    size_t n = 0;
    bool replaced = false;
    for (size_t i = 0; i < 6; i++) {
        if (valid_headers[i].name.len == hl &&
            memcmp(valid_headers[i].name.ptr, name, hl) == 0) {
            out[n++] = (cf_header){valid_headers[i].name,
                                   (cf_span){value, value_len}};
            replaced = true;
        } else {
            out[n++] = valid_headers[i];
        }
    }
    if (!replaced) {
        out[n++] = (cf_header){
            (cf_span){(const unsigned char *)name, hl},
            (cf_span){value, value_len}};
    }
    return n;
}

static size_t gate_headers_dup(cf_header *out, const char *name,
                               const unsigned char *first, size_t first_len,
                               const unsigned char *second,
                               size_t second_len) {
    size_t n = gate_headers(out, name, first, first_len);
    out[n++] = (cf_header){
        (cf_span){(const unsigned char *)name, strlen(name)},
        (cf_span){second, second_len}};
    return n;
}

/* Every byte class to_str rejects on top of H01's admitted set: obs-text
 * (valid UTF-8 and not), NUL, DEL and the C0 controls.  H01 answers 400 for
 * the control/DEL classes on the wire; at this unit level all of them must
 * take the absent-value path. */
static void gate_unreadable_classes(cf_cable_server *server, const char *name,
                                    const char *valid, size_t valid_len,
                                    bool absent_101,
                                    const char *absent_must_not) {
    static const struct {
        const char *label;
        unsigned char bytes[2];
        size_t len;
    } classes[] = {
        {"obs-text-utf8", {0xc3, 0xa9}, 2},   /* e-acute */
        {"obs-text-invalid-utf8", {0xff, 0x00}, 1},
        {"nul", {0x00, 0x00}, 1},
        {"del", {0x7f, 0x00}, 1},
        {"c0", {0x01, 0x00}, 1},
        {"cr", {0x0d, 0x00}, 1},
        {"lf", {0x0a, 0x00}, 1},
    };
    unsigned char value[128];
    for (size_t i = 0; i < sizeof classes / sizeof classes[0]; i++) {
        size_t len = valid_len + classes[i].len;
        CF_REQUIRE(len <= sizeof value);
        memcpy(value, valid, valid_len);
        memcpy(value + valid_len, classes[i].bytes, classes[i].len);
        char label[160];
        snprintf(label, sizeof label, "%s/%s", name, classes[i].label);
        cf_header h[8];
        size_t n = gate_headers(h, name, value, len);
        expect_gate_outcome(server, h, n, absent_101, NULL, absent_must_not,
                            label);
    }
}

typedef struct {
    const char *name;
    const char *valid;
    bool list_site;      /* get_all: an unreadable value does not shadow a later one */
    bool absent_101;     /* 101 when the header reads absent (list sites) */
    const char *absent_must_not;
    const char *valid_must; /* required substring on the readable control */
    const char *htab;       /* readable HTAB variant: outcome follows the value */
    bool htab_101;
    const char *htab_must;
} gate_row;

CF_TEST(handshake_headers_use_the_pinned_to_str_byte_gate) {
    g_auth.authenticated = true;
    cf_cable_server *server = make_server(false, true, false, NULL, 0, NULL);
    static const gate_row rows[] = {
        {"upgrade", "websocket", false, false, NULL, NULL, "websock\ttet",
         false, NULL},
        {"connection", "Upgrade", true, false, NULL, NULL, "Upgrade\t", true,
         NULL},
        {"sec-websocket-version", "13", false, false, NULL, NULL, "1\t3",
         false, NULL},
        {"sec-websocket-key", RFC_KEY, false, false, NULL, NULL,
         "dGhlIHNhbXBsZSBub25jZ\tQ==", false, NULL},
        {"sec-websocket-protocol", "actioncable-v1-json", true, true,
         "sec-websocket-protocol",
         "sec-websocket-protocol: actioncable-v1-json",
         "actioncable-v1-json\t", true,
         "sec-websocket-protocol: actioncable-v1-json"},
        {"sec-websocket-extensions", "permessage-deflate", true, true,
         "sec-websocket-extensions",
         "sec-websocket-extensions: permessage-deflate",
         "permessage-deflate\t", true,
         "sec-websocket-extensions: permessage-deflate"},
        {"origin", "http://127.0.0.1:32123", false, false, NULL, NULL,
         "http://127.0.0.1:32123\t", false, NULL},
        {"host", "127.0.0.1:32123", false, false, NULL, NULL,
         "127.0.0.1:32123\t", false, NULL},
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        const gate_row *row = &rows[i];
        size_t valid_len = strlen(row->valid);
        size_t htab_len = strlen(row->htab);
        cf_header h[8];
        char label[192];
        size_t n;

        /* readable control: the normal handshake is unchanged */
        n = gate_headers(h, row->name, (const unsigned char *)row->valid,
                         valid_len);
        snprintf(label, sizeof label, "%s/control-readable", row->name);
        expect_gate_outcome(server, h, n, true, row->valid_must, NULL, label);

        /* every unreadable byte class reads as absent */
        gate_unreadable_classes(server, row->name, row->valid, valid_len,
                                row->absent_101, row->absent_must_not);

        /* space-only is readable but never a valid value */
        n = gate_headers(h, row->name, (const unsigned char *)"   ", 3);
        snprintf(label, sizeof label, "%s/space-only", row->name);
        expect_gate_outcome(server, h, n, row->absent_101, NULL,
                            row->absent_must_not, label);

        /* HTAB is readable: the outcome follows the value's semantics */
        n = gate_headers(h, row->name, (const unsigned char *)row->htab,
                         htab_len);
        snprintf(label, sizeof label, "%s/htab", row->name);
        expect_gate_outcome(server, h, n, row->htab_101, row->htab_must, NULL,
                            label);

        /* duplicates: a single-get read takes the first value; a list read
         * drops the unreadable value and still sees the readable one */
        unsigned char bad[128];
        size_t bad_len = valid_len + 2;
        CF_REQUIRE(bad_len <= sizeof bad);
        memcpy(bad, row->valid, valid_len);
        bad[valid_len] = 0xc3;
        bad[valid_len + 1] = 0xa9;
        n = gate_headers_dup(h, row->name, bad, bad_len,
                             (const unsigned char *)row->valid, valid_len);
        snprintf(label, sizeof label, "%s/unreadable-then-readable",
                 row->name);
        expect_gate_outcome(server, h, n, row->list_site,
                            row->list_site ? row->valid_must : NULL,
                            row->list_site ? NULL : row->absent_must_not,
                            label);
        n = gate_headers_dup(h, row->name,
                             (const unsigned char *)row->valid, valid_len, bad,
                             bad_len);
        snprintf(label, sizeof label, "%s/readable-then-unreadable",
                 row->name);
        expect_gate_outcome(server, h, n, true, row->valid_must, NULL, label);
    }
    cf_cable_server_destroy(server);
}

/* The verifier's two wire reproductions and the mixed-token shapes. */
CF_TEST(handshake_gate_reproduces_the_verifier_wire_cases) {
    g_auth.authenticated = true;
    cf_cable_server *server = make_server(false, true, false, NULL, 0, NULL);
    static const struct {
        const char *name;
        const char *value;
        bool expect_101;
        const char *must_not;
        const char *label;
    } cases[] = {
        {"sec-websocket-protocol", "x\xc3\xa9, actioncable-v1-json", true,
         "sec-websocket-protocol", "protocol x<obs>, actioncable (verifier)"},
        {"sec-websocket-protocol", "actioncable-v1-json, x\xc3\xa9", true,
         "sec-websocket-protocol", "protocol actioncable, x<obs> (verifier)"},
        {"connection", "upgrade, x\xc3\xa9", false, NULL,
         "connection upgrade, x<obs> (verifier)"},
        {"connection", "x\xc3\xa9, upgrade", false, NULL,
         "connection x<obs>, upgrade (verifier)"},
        {"sec-websocket-extensions", "x\xc3\xa9, permessage-deflate", true,
         "sec-websocket-extensions",
         "extensions x<obs> header dropped whole"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        cf_header h[8];
        size_t n = gate_headers(h, cases[i].name,
                                (const unsigned char *)cases[i].value,
                                strlen(cases[i].value));
        expect_gate_outcome(server, h, n, cases[i].expect_101, NULL,
                            cases[i].must_not, cases[i].label);
    }
    /* A list-site duplicate with the supported token only in the second
     * header still negotiates. */
    {
        cf_header h[8];
        size_t n = gate_headers_dup(h, "sec-websocket-protocol",
                                    (const unsigned char *)"\xc3\xa9", 2,
                                    (const unsigned char *)"actioncable-v1-json",
                                    strlen("actioncable-v1-json"));
        expect_gate_outcome(server, h, n, true,
                            "sec-websocket-protocol: actioncable-v1-json", NULL,
                            "protocol unreadable header then readable");
    }
    {
        cf_header h[8];
        size_t n = gate_headers_dup(h, "connection",
                                    (const unsigned char *)"keep-alive, \xc3\xa9",
                                    strlen("keep-alive, \xc3\xa9"),
                                    (const unsigned char *)"Upgrade",
                                    strlen("Upgrade"));
        expect_gate_outcome(server, h, n, true, NULL, NULL,
                            "connection unreadable header then readable");
    }
    cf_cable_server_destroy(server);
}

/* ---- Cookie readability (ApplicationCable session authentication) --------- */

#define GATE_CAB_SECRET \
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

typedef struct {
    cf_db *reader;
} gate_session_fixture;

static gate_session_fixture g_gate_session;

static void gate_seed_session_db(cf_db_scratch *scratch) {
    CF_REQUIRE(cf_db_test_exec(cf_db_handle(scratch->db),
                               "INSERT INTO users "
                               "(bio,bot_token,created_at,email_address,name,"
                               "password_digest,role,status,updated_at) VALUES "
                               "(NULL,NULL,'2026-01-02 03:04:05',"
                               "'u@example.com','Cable Tester','x',0,0,"
                               "'2026-01-02 03:04:05')") == SQLITE_OK);
    CF_REQUIRE(cf_db_test_exec(cf_db_handle(scratch->db),
                               "INSERT INTO sessions "
                               "(created_at,last_active_at,token,updated_at,"
                               "user_id) VALUES ('2026-01-02 03:04:05',"
                               "'2026-01-02 03:04:05','session-token-1',"
                               "'2026-01-02 03:04:05',1)") == SQLITE_OK);
}

static cf_str gate_signed_cookie(const char *token) {
    cf_str cookie = {0};
    CF_REQUIRE(cf_auth_signed_cookie_generate(
                   (cf_span){(const unsigned char *)GATE_CAB_SECRET,
                             sizeof GATE_CAB_SECRET - 1},
                   (cf_span){(const unsigned char *)"session_token", 13},
                   (cf_span){(const unsigned char *)token, strlen(token)},
                   false, 0, &cookie) == CF_OK);
    return cookie;
}

static void gate_cookie_auth(cf_header *headers, size_t n, bool *ok,
                             int64_t *user_id) {
    cf_cable_session_auth auth;
    memset(&auth, 0, sizeof auth);
    auth.reader = g_gate_session.reader;
    auth.secret_key_base = (cf_span){(const unsigned char *)GATE_CAB_SECRET,
                                     sizeof GATE_CAB_SECRET - 1};
    cf_cable_request request;
    memset(&request, 0, sizeof request);
    request.method = CF_GET;
    request.target = (cf_span){(const unsigned char *)"/cable", 6};
    request.path = request.target;
    request.headers = headers;
    request.header_count = n;
    bool authenticated = false;
    int64_t id = 0;
    CF_REQUIRE(cf_cable_session_authenticate(&auth, &request, &authenticated,
                                             &id) == CF_OK);
    *ok = authenticated;
    *user_id = id;
}

#define GATE_COOKIE_HEADER(value, len)                                     \
    ((cf_header){                                                          \
        (cf_span){(const unsigned char *)"cookie", 6},                     \
        (cf_span){(const unsigned char *)(value), (len)}})

CF_TEST(cable_cookie_header_uses_the_pinned_to_str_byte_gate) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    gate_seed_session_db(&scratch);
    CF_REQUIRE(cf_db_open(scratch.path, true, &g_gate_session.reader) == CF_OK);

    cf_str cookie = gate_signed_cookie("session-token-1");
    char line[2048];
    int line_len = snprintf(line, sizeof line, "session_token=%s", cookie.ptr);
    CF_REQUIRE(line_len > 0 && (size_t)line_len < sizeof line);
    size_t base_len = (size_t)line_len;

    bool ok = false;
    int64_t user_id = 0;

    /* readable control: the signed cookie authenticates as before */
    {
        cf_header h[4] = {GATE_COOKIE_HEADER(line, base_len)};
        gate_cookie_auth(h, 1, &ok, &user_id);
        CF_CHECK(ok == true);
        CF_CHECK(user_id == 1);
    }

    /* an unreadable pair anywhere in the value drops the header whole:
     * a neutralized gate parses the second pair and authenticates. */
    {
        unsigned char value[2048];
        int k = snprintf((char *)value, sizeof value, "x=\xc3\xa9; %s", line);
        CF_REQUIRE(k > 0 && (size_t)k < sizeof value);
        cf_header h[4] = {GATE_COOKIE_HEADER(value, (size_t)k)};
        gate_cookie_auth(h, 1, &ok, &user_id);
        CF_CHECK(ok == false);
    }
    {
        unsigned char value[2048];
        int k = snprintf((char *)value, sizeof value, "%s; x=\xc3\xa9", line);
        CF_REQUIRE(k > 0 && (size_t)k < sizeof value);
        cf_header h[4] = {GATE_COOKIE_HEADER(value, (size_t)k)};
        gate_cookie_auth(h, 1, &ok, &user_id);
        CF_CHECK(ok == false);
    }

    /* every unreadable byte class appended to a readable cookie */
    {
        static const struct {
            const char *label;
            unsigned char bytes[2];
            size_t len;
        } classes[] = {
            {"obs-text-utf8", {0xc3, 0xa9}, 2},
            {"obs-text-invalid-utf8", {0xff, 0x00}, 1},
            {"nul", {0x00, 0x00}, 1},
            {"del", {0x7f, 0x00}, 1},
            {"c0", {0x01, 0x00}, 1},
            {"cr", {0x0d, 0x00}, 1},
            {"lf", {0x0a, 0x00}, 1},
        };
        for (size_t i = 0; i < sizeof classes / sizeof classes[0]; i++) {
            unsigned char value[2048];
            memcpy(value, line, base_len);
            memcpy(value + base_len, classes[i].bytes, classes[i].len);
            cf_header h[4] = {
                GATE_COOKIE_HEADER(value, base_len + classes[i].len)};
            gate_cookie_auth(h, 1, &ok, &user_id);
            if (ok) {
                fprintf(stderr, "    cookie/%s: authenticated\n",
                        classes[i].label);
            }
            CF_CHECK(ok == false);
        }
    }

    /* get_all semantics: an unreadable header is skipped and the readable
     * duplicate on either side still authenticates. */
    {
        cf_header h[4] = {
            GATE_COOKIE_HEADER(line, base_len),
            GATE_COOKIE_HEADER("\xc3\xa9", 2),
        };
        gate_cookie_auth(h, 2, &ok, &user_id);
        CF_CHECK(ok == true);
        CF_CHECK(user_id == 1);
    }
    {
        cf_header h[4] = {
            GATE_COOKIE_HEADER("\xc3\xa9", 2),
            GATE_COOKIE_HEADER(line, base_len),
        };
        gate_cookie_auth(h, 2, &ok, &user_id);
        CF_CHECK(ok == true);
        CF_CHECK(user_id == 1);
    }

    cf_str_dispose(&cookie);
    cf_db_close(g_gate_session.reader);
    g_gate_session.reader = NULL;
    cf_db_scratch_close(&scratch);
}

/* The pin keeps the FIRST occurrence of a cookie name: connection.rs:25-26
 * feeds every readable Cookie header, in header order, to
 * CookieJar::from_headers (cookies.rs:148-159), whose seen set spans headers,
 * and parse_cookie_header's seen guard (cookies.rs:279-281) keeps the first
 * pair within one header.  A later duplicate never overrides, so an invalid
 * first occurrence rejects even when a valid value follows.  Every case whose
 * first occurrence decides flips back under a last-wins loop. */
CF_TEST(cable_cookie_duplicate_precedence_is_first_wins) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    gate_seed_session_db(&scratch);
    CF_REQUIRE(cf_db_open(scratch.path, true, &g_gate_session.reader) == CF_OK);

    cf_str cookie = gate_signed_cookie("session-token-1");
    char valid[2048];
    int n = snprintf(valid, sizeof valid, "session_token=%s", cookie.ptr);
    CF_REQUIRE(n > 0 && (size_t)n < sizeof valid);
    size_t valid_len = (size_t)n;

    bool ok = false;
    int64_t user_id = 0;

    /* control: a single cookie still authenticates */
    {
        cf_header h[4] = {GATE_COOKIE_HEADER(valid, valid_len)};
        gate_cookie_auth(h, 1, &ok, &user_id);
        CF_CHECK(ok == true);
        CF_CHECK(user_id == 1);
    }

    /* one header, valid first: the valid pair decides (last-wins would
     * let the trailing garbage reject). */
    {
        char value[4096];
        n = snprintf(value, sizeof value, "%s; session_token=garbage", valid);
        CF_REQUIRE(n > 0 && (size_t)n < sizeof value);
        cf_header h[4] = {GATE_COOKIE_HEADER(value, (size_t)n)};
        gate_cookie_auth(h, 1, &ok, &user_id);
        CF_CHECK(ok == true);
        CF_CHECK(user_id == 1);
    }

    /* one header, garbage first: the invalid pair decides and blocks the
     * trailing valid pair (last-wins would authenticate). */
    {
        char value[4096];
        n = snprintf(value, sizeof value, "session_token=garbage; %s", valid);
        CF_REQUIRE(n > 0 && (size_t)n < sizeof value);
        cf_header h[4] = {GATE_COOKIE_HEADER(value, (size_t)n)};
        gate_cookie_auth(h, 1, &ok, &user_id);
        CF_CHECK(ok == false);
        CF_CHECK(user_id == 0);
    }

    /* two Cookie headers, valid then garbage: the first header decides. */
    {
        cf_header h[4] = {
            GATE_COOKIE_HEADER(valid, valid_len),
            GATE_COOKIE_HEADER("session_token=garbage",
                               sizeof "session_token=garbage" - 1),
        };
        gate_cookie_auth(h, 2, &ok, &user_id);
        CF_CHECK(ok == true);
        CF_CHECK(user_id == 1);
    }

    /* two Cookie headers, garbage then valid: the first (invalid) header
     * decides and the later valid cookie never overrides it. */
    {
        cf_header h[4] = {
            GATE_COOKIE_HEADER("session_token=garbage",
                               sizeof "session_token=garbage" - 1),
            GATE_COOKIE_HEADER(valid, valid_len),
        };
        gate_cookie_auth(h, 2, &ok, &user_id);
        CF_CHECK(ok == false);
        CF_CHECK(user_id == 0);
    }

    /* a valid cookie split across an unreadable first header and a readable
     * second: the unreadable value is dropped whole, so the readable header
     * is the first occurrence and authenticates. */
    {
        cf_header h[4] = {
            GATE_COOKIE_HEADER("\xc3\xa9", 2),
            GATE_COOKIE_HEADER(valid, valid_len),
        };
        gate_cookie_auth(h, 2, &ok, &user_id);
        CF_CHECK(ok == true);
        CF_CHECK(user_id == 1);
    }

    cf_str_dispose(&cookie);
    cf_db_close(g_gate_session.reader);
    g_gate_session.reader = NULL;
    cf_db_scratch_close(&scratch);
}

static void cookie_auth_case(const char *label, const char *v1,
                             const char *v2, bool want_ok) {
    cf_header h[4];
    size_t n = 0;
    if (v1 != NULL) h[n++] = GATE_COOKIE_HEADER(v1, strlen(v1));
    if (v2 != NULL) h[n++] = GATE_COOKIE_HEADER(v2, strlen(v2));
    bool ok = false;
    int64_t user_id = 0;
    gate_cookie_auth(h, n, &ok, &user_id);
    if (ok != want_ok || (want_ok ? user_id != 1 : user_id != 0)) {
        fprintf(stderr, "    %s: ok=%d user_id=%lld, want ok=%d\n", label,
                (int)ok, (long long)user_id, (int)want_ok);
        CF_CHECK(0);
        return;
    }
    CF_CHECK(1);
}

/* parse_cookie_header byte-exactness (cookies.rs:269-289): the first part is
 * never trimmed, later parts lose only leading 0x20, a part with no '=' is an
 * occurrence with an empty value, the name and value keep their bytes around
 * the first '=', and every case is the pin's outcome. */
CF_TEST(cable_cookie_parsing_matches_the_pin) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    gate_seed_session_db(&scratch);
    CF_REQUIRE(cf_db_open(scratch.path, true, &g_gate_session.reader) == CF_OK);

    cf_str cookie = gate_signed_cookie("session-token-1");
    char base[2048];
    int n = snprintf(base, sizeof base, "session_token=%s", cookie.ptr);
    CF_REQUIRE(n > 0 && (size_t)n < sizeof base);
    char buf[4096];

    /* bare name: an occurrence with an empty value blocks a later duplicate */
    snprintf(buf, sizeof buf, "session_token; %s", base);
    cookie_auth_case("bare first, valid later in one header", buf, NULL,
                     false);
    snprintf(buf, sizeof buf, "%s; session_token", base);
    cookie_auth_case("valid first, bare later in one header", buf, NULL, true);
    cookie_auth_case("bare header then valid header", "session_token", base,
                     false);
    cookie_auth_case("valid header then bare header", base, "session_token",
                     true);

    /* name bytes: only a later part loses leading spaces, and a space before
     * '=' is part of the name */
    snprintf(buf, sizeof buf, "session_token =%s", cookie.ptr);
    cookie_auth_case("name-space cookie is not session_token", buf, NULL,
                     false);
    snprintf(buf, sizeof buf, "session_token =garbage; %s", base);
    cookie_auth_case("name-space pair skipped, later valid wins", buf, NULL,
                     true);
    cookie_auth_case("name-space header skipped, later valid header wins",
                     "session_token =garbage", base, true);
    cookie_auth_case("valid header wins over later name-space header", base,
                     "session_token =garbage", true);
    snprintf(buf, sizeof buf, " %s", base);
    cookie_auth_case("leading space on the first part is kept", buf, NULL,
                     false);
    snprintf(buf, sizeof buf, " %s; session_token=garbage", base);
    cookie_auth_case("leading-space first part skipped, garbage decides", buf,
                     NULL, false);
    snprintf(buf, sizeof buf, " %s", base);
    cookie_auth_case("leading-space header skipped, later garbage header", buf,
                     "session_token=garbage", false);
    snprintf(buf, sizeof buf, "x=1; %s", base);
    cookie_auth_case("later part trims leading spaces", buf, NULL, true);
    snprintf(buf, sizeof buf, "x=1;  %s", base);
    cookie_auth_case("later part trims two leading spaces", buf, NULL, true);

    /* value bytes: no trim around '=' */
    snprintf(buf, sizeof buf, "session_token= %s", cookie.ptr);
    cookie_auth_case("value keeps the leading space", buf, NULL, false);
    cookie_auth_case("two headers, leading-space value first", buf, base,
                     false);
    cookie_auth_case("two headers, leading-space value second", base, buf,
                     true);
    snprintf(buf, sizeof buf, "%s ; session_token=garbage", base);
    cookie_auth_case("value keeps the trailing space", buf, NULL, false);
    snprintf(buf, sizeof buf, "%s ", base);
    cookie_auth_case("two headers, trailing-space value first", buf,
                     "session_token=garbage", false);

    /* HTAB is readable and, like every byte that is not a part-leading 0x20,
     * is kept */
    snprintf(buf, sizeof buf, "session_token=\t%s", cookie.ptr);
    cookie_auth_case("value keeps the leading HTAB", buf, NULL, false);
    snprintf(buf, sizeof buf, "session_token\t=%s", cookie.ptr);
    cookie_auth_case("name keeps the trailing HTAB", buf, NULL, false);
    snprintf(buf, sizeof buf, "%s\t", base);
    cookie_auth_case("value keeps the trailing HTAB", buf, NULL, false);

    cf_str_dispose(&cookie);
    cf_db_close(g_gate_session.reader);
    g_gate_session.reader = NULL;
    cf_db_scratch_close(&scratch);
}

CF_TEST_MAIN()
