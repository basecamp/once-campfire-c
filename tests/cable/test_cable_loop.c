/* tests/cable/test_cable_loop.c — C01 live path: a real H01 loopback HTTP
 * loop with cf_cable_server_upgrade installed as the upgrade hook. The /cable
 * mount answers before admission (404 for invalid upgrades, 101 + welcome for
 * valid ones, with pipelined frames handed off); other paths still reach the
 * admit hook. */
#include "cable_testutil.h"

#include "http/http.h"

#include <arpa/inet.h>
#include <netinet/in.h>

struct loop_fixture {
    int listen_fd;
    unsigned port;
    char origin[64];
    cf_http_loop *loop;
    pthread_t thread;
    cf_cable_server *server;
    int admitted;
};

static cf_err fixture_admit(void *user, cf_http_task *task) {
    struct loop_fixture *f = user;
    f->admitted++;
    cf_response resp;
    cf_response_init(&resp);
    resp.status = 200;
    cf_buf *body = NULL;
    if (cf_buf_copy((cf_span){(const unsigned char *)"ok", 2}, &body) !=
        CF_OK) {
        return CF_NOMEM;
    }
    cf_err rc = cf_response_body(&resp, body);
    cf_buf_release(body);
    if (rc == CF_OK) {
        rc = cf_response_header(
            &resp, (cf_span){(const unsigned char *)"Content-Type", 12},
            (cf_span){(const unsigned char *)"text/plain", 10});
    }
    if (rc == CF_OK) rc = cf_http_task_submit(task, &resp);
    cf_response_dispose(&resp);
    return rc == CF_OK ? CF_OK : CF_BUSY;
}

static cf_err fixture_auth(void *user, const cf_cable_request *request,
                           bool *authenticated, int64_t *user_id) {
    (void)user;
    *authenticated = false;
    *user_id = 0;
    for (size_t i = 0; i < request->header_count; i++) {
        if (strncasecmp((const char *)request->headers[i].name.ptr, "cookie",
                        6) != 0 ||
            request->headers[i].name.len != 6) {
            continue;
        }
        cf_span v = request->headers[i].value;
        const char *needle = "session_token=";
        size_t needle_len = strlen(needle);
        if (v.len < needle_len) continue;
        for (size_t p = 0; p + needle_len <= v.len; p++) {
            if (memcmp(v.ptr + p, needle, needle_len) != 0) continue;
            const unsigned char *d = v.ptr + p + needle_len;
            size_t digits = 0;
            int64_t id = 0;
            while (digits < 4 && d + digits < v.ptr + v.len &&
                   d[digits] >= '0' && d[digits] <= '9') {
                id = id * 10 + (d[digits] - '0');
                digits++;
            }
            if (digits != 0 && (id == 1 || id == 2)) {
                *authenticated = true;
                *user_id = id;
                return CF_OK;
            }
        }
    }
    return CF_OK;
}

static void *fixture_loop_main(void *arg) {
    struct loop_fixture *f = arg;
    (void)cf_http_loop_run(f->loop);
    return NULL;
}

static bool fixture_start(struct loop_fixture *f) {
    memset(f, 0, sizeof *f);
    f->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (f->listen_fd < 0) return false;
    int one = 1;
    setsockopt(f->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(f->listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(f->listen_fd, 32) != 0) {
        return false;
    }
    socklen_t len = sizeof addr;
    if (getsockname(f->listen_fd, (struct sockaddr *)&addr, &len) != 0) {
        return false;
    }
    f->port = ntohs(addr.sin_port);
    snprintf(f->origin, sizeof f->origin, "http://127.0.0.1:%u", f->port);

    cf_cable_server_config server_config;
    cf_cable_server_config_default(&server_config);
    server_config.assume_ssl = false;
    server_config.allow_same_origin_as_host = true;
    server_config.hooks.authenticate = fixture_auth;
    server_config.hooks.on_text = ct_on_text_echo;
    if (cf_cable_server_create(&server_config, &f->server) != CF_OK) {
        return false;
    }

    cf_http_loop_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.listen_fd = f->listen_fd;
    cfg.loop_index = 0;
    cfg.public_origin = f->origin;
    cfg.connections_per_loop = 16;
    cfg.admit = fixture_admit;
    cfg.admit_user = f;
    cfg.upgrade = cf_cable_server_upgrade;
    cfg.upgrade_user = f->server;
    if (cf_http_loop_create(&cfg, &f->loop) != CF_OK) return false;
    if (pthread_create(&f->thread, NULL, fixture_loop_main, f) != 0) {
        return false;
    }
    return true;
}

static void fixture_stop(struct loop_fixture *f) {
    if (f->loop != NULL) {
        cf_http_loop_stop(f->loop);
        pthread_join(f->thread, NULL);
        cf_http_loop_destroy(f->loop);
        f->loop = NULL;
    }
    if (f->server != NULL) {
        cf_cable_server_destroy(f->server);
        f->server = NULL;
    }
    if (f->listen_fd >= 0) close(f->listen_fd);
}

static int fixture_connect(const struct loop_fixture *f) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)f->port);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Read one whole HTTP response (headers + Content-Length body). */
static bool read_http_response(int fd, int *status, char *body,
                               size_t body_cap, size_t *body_len) {
    char head[2048];
    ssize_t n = ct_read_http_head(fd, head, sizeof head, 3000);
    if (n <= 0) return false;
    *status = atoi(head + 9);
    const char *cl = strcasestr(head, "Content-Length:");
    size_t length = cl != NULL ? (size_t)strtoul(cl + 15, NULL, 10) : 0;
    if (length + 1 > body_cap) return false;
    if (length != 0 && !ct_read_exact(fd, body, length, 3000)) return false;
    body[length] = '\0';
    *body_len = length;
    return true;
}

static size_t build_upgrade_request(char *buf, size_t cap,
                                    const struct loop_fixture *f,
                                    const char *origin, bool cookie) {
    return (size_t)snprintf(
        buf, cap,
        "GET /cable HTTP/1.1\r\n"
        "Host: 127.0.0.1:%u\r\n"
        "Origin: %s\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Protocol: actioncable-v1-json, "
        "actioncable-unsupported\r\n"
        "Sec-WebSocket-Extensions: permessage-deflate; "
        "client_max_window_bits\r\n"
        "%s"
        "\r\n",
        f->port, origin, cookie ? "Cookie: session_token=1\r\n" : "");
}

CF_TEST(live_loop_mounts_cable_before_routing) {
    struct loop_fixture f;
    CF_REQUIRE(fixture_start(&f));
    char request[2048];

    /* A plain GET on /cable stays an ordinary 404 (no admission). */
    {
        int fd = fixture_connect(&f);
        CF_REQUIRE(fd >= 0);
        int n = snprintf(request, sizeof request,
                         "GET /cable HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
                         "Origin: http://127.0.0.1:%u\r\n"
                         "Connection: close\r\n\r\n",
                         f.port, f.port);
        CF_REQUIRE(ct_send_all(fd, request, (size_t)n));
        int status = 0;
        char body[64];
        size_t body_len = 0;
        CF_REQUIRE(read_http_response(fd, &status, body, sizeof body,
                                      &body_len));
        CF_CHECK(status == 404);
        CF_CHECK(body_len == 14);
        CF_CHECK(memcmp(body, "Page not found", 14) == 0);
        close(fd);
    }
    /* A cross-origin upgrade is the same 404. */
    {
        int fd = fixture_connect(&f);
        CF_REQUIRE(fd >= 0);
        size_t n = build_upgrade_request(request, sizeof request, &f,
                                         "http://evil.example", true);
        CF_REQUIRE(ct_send_all(fd, request, n));
        int status = 0;
        char body[64];
        size_t body_len = 0;
        CF_REQUIRE(read_http_response(fd, &status, body, sizeof body,
                                      &body_len));
        CF_CHECK(status == 404);
        CF_CHECK(memcmp(body, "Page not found", 14) == 0);
        close(fd);
    }
    /* Other paths still reach the admit hook. */
    {
        int fd = fixture_connect(&f);
        CF_REQUIRE(fd >= 0);
        int n = snprintf(request, sizeof request,
                         "GET /other HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
                         "Connection: close\r\n\r\n",
                         f.port);
        CF_REQUIRE(ct_send_all(fd, request, (size_t)n));
        int status = 0;
        char body[64];
        size_t body_len = 0;
        CF_REQUIRE(read_http_response(fd, &status, body, sizeof body,
                                      &body_len));
        CF_CHECK(status == 200);
        CF_CHECK(body_len == 2 && memcmp(body, "ok", 2) == 0);
        close(fd);
    }
    /* A valid upgrade: 101 + welcome, echo, close handshake. */
    {
        int fd = fixture_connect(&f);
        CF_REQUIRE(fd >= 0);
        size_t n = build_upgrade_request(request, sizeof request, &f,
                                         f.origin, true);
        CF_REQUIRE(ct_send_all(fd, request, n));
        char head[1024];
        CF_REQUIRE(ct_read_http_head(fd, head, sizeof head, 3000) > 0);
        CF_CHECK(strstr(head, "101 Switching Protocols") != NULL);
        CF_CHECK(strstr(head, "sec-websocket-protocol: actioncable-v1-json") !=
                 NULL);
        CF_CHECK(strstr(head, "sec-websocket-extensions") != NULL);
        unsigned opcode = 0;
        bool rsv1 = false;
        unsigned char payload[512];
        ssize_t m = ct_read_server_frame(fd, &opcode, &rsv1, payload,
                                         sizeof payload, 3000);
        CF_REQUIRE(m >= 0);
        CF_CHECK(m == 18 && memcmp(payload, "{\"type\":\"welcome\"}", 18) == 0);

        CF_REQUIRE(ct_send_client_frame(fd, 0x1, true, false,
                                        (const unsigned char *)"hello", 5));
        m = ct_read_server_frame(fd, &opcode, &rsv1, payload, sizeof payload,
                                 3000);
        CF_REQUIRE(m == 5);
        CF_CHECK(memcmp(payload, "hello", 5) == 0);

        CF_REQUIRE(ct_send_client_frame(fd, 0x8, true, false, NULL, 0));
        m = ct_read_server_frame(fd, &opcode, &rsv1, payload, sizeof payload,
                                 3000);
        CF_CHECK(m == 0);
        CF_CHECK(opcode == 0x8);
        close(fd);
    }
    /* Pipelined frames: the same request plus a frame in one write; the
     * bytes after the head are handed to the cable socket and processed. */
    {
        int fd = fixture_connect(&f);
        CF_REQUIRE(fd >= 0);
        size_t n = build_upgrade_request(request, sizeof request, &f,
                                         f.origin, true);
        unsigned char frame[64];
        const char *early = "early";
        size_t frame_len = ct_client_frame(frame, 0x1, true, false,
                                           (const unsigned char *)early, 5);
        CF_REQUIRE(n + frame_len <= sizeof request);
        memcpy(request + n, frame, frame_len);
        CF_REQUIRE(ct_send_all(fd, request, n + frame_len));
        char head[1024];
        CF_REQUIRE(ct_read_http_head(fd, head, sizeof head, 3000) > 0);
        unsigned opcode = 0;
        bool rsv1 = false;
        unsigned char payload[512];
        ssize_t m = ct_read_server_frame(fd, &opcode, &rsv1, payload,
                                         sizeof payload, 3000);
        CF_REQUIRE(m == 18);
        CF_CHECK(memcmp(payload, "{\"type\":\"welcome\"}", 18) == 0);
        m = ct_read_server_frame(fd, &opcode, &rsv1, payload, sizeof payload,
                                 3000);
        CF_REQUIRE(m == 5);
        CF_CHECK(memcmp(payload, early, 5) == 0);
        CF_REQUIRE(ct_send_client_frame(fd, 0x8, true, false, NULL, 0));
        m = ct_read_server_frame(fd, &opcode, &rsv1, payload, sizeof payload,
                                 3000);
        CF_CHECK(opcode == 0x8);
        close(fd);
    }
    /* No session cookie: the upgrade succeeds, then the connection is told
     * not to reconnect and closed. */
    {
        int fd = fixture_connect(&f);
        CF_REQUIRE(fd >= 0);
        size_t n = build_upgrade_request(request, sizeof request, &f,
                                         f.origin, false);
        CF_REQUIRE(ct_send_all(fd, request, n));
        char head[1024];
        CF_REQUIRE(ct_read_http_head(fd, head, sizeof head, 3000) > 0);
        CF_CHECK(strstr(head, "101 Switching Protocols") != NULL);
        unsigned opcode = 0;
        bool rsv1 = false;
        unsigned char payload[512];
        ssize_t m = ct_read_server_frame(fd, &opcode, &rsv1, payload,
                                         sizeof payload, 3000);
        CF_REQUIRE(m > 0);
        CF_CHECK(memmem(payload, (size_t)m, "unauthorized", 12) != NULL);
        m = ct_read_server_frame(fd, &opcode, &rsv1, payload, sizeof payload,
                                 3000);
        CF_CHECK(m >= 0 && opcode == 0x8);
        close(fd);
    }

    CF_CHECK(f.admitted == 1); /* only /other reached the admit hook */
    fixture_stop(&f);
}

CF_TEST_MAIN()
