/* tests/cable/test_cable_handshake.c — C01 handshake vectors through the
 * cf_cable_server_upgrade hook (the /cable front mount, server.rs call):
 * method/Upgrade/Connection checks, version 13, key validation, the reference
 * Origin policy (D-C07: proxy headers ignored), subprotocol negotiation,
 * permessage-deflate negotiation, the 101 response and post-upgrade auth
 * (unauthorized disconnect + close 1000). */
#include "cable_testutil.h"

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

static cf_err handshake_auth(void *user, const cf_cable_request *request,
                             bool *authenticated, int64_t *user_id) {
    (void)user;
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
    if (taken(taken_user) != CF_OK) {
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
            CF_CHECK(req.taken(req.taken_user) == CF_OK);
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

CF_TEST_MAIN()
