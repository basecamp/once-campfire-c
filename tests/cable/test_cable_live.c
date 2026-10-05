/* tests/cable/test_cable_live.c — C02 production wiring over a real socket:
 * an H01 loopback HTTP loop with cf_cable_server_upgrade installed exactly as
 * main.c installs it, the cable's own authenticate/on_text hooks, a real
 * signed session cookie, a real Action Cable handshake, subscribe/confirm and
 * one broadcast round-trip. */
#include "cable_channels_testutil.h"

#include "http/http.h"

#include <arpa/inet.h>
#include <netinet/in.h>

typedef struct {
    chan_fixture app;
    int listen_fd;
    unsigned port;
    char origin[64];
    cf_http_loop *loop;
    pthread_t thread;
    cf_cable_server *server;
} live_fixture;

static cf_err live_admit(void *user, cf_http_task *task) {
    (void)user;
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
        rc = cf_http_task_submit(task, &resp);
    }
    cf_response_dispose(&resp);
    return rc == CF_OK ? CF_OK : CF_BUSY;
}

static void *live_loop_main(void *arg) {
    live_fixture *f = arg;
    (void)cf_http_loop_run(f->loop);
    return NULL;
}

static bool live_start(live_fixture *f) {
    memset(f, 0, sizeof *f);
    if (!chan_fixture_open(&f->app)) return false;
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

    /* The exact main.c wiring: the C01 server with this cable's hooks, and
     * the loop's upgrade seam pointing at the server. */
    cf_cable_server_config server_config;
    cf_cable_server_config_default(&server_config);
    server_config.assume_ssl = false;
    cf_cable_server_hooks(f->app.cable, &server_config.hooks);
    if (cf_cable_server_create(&server_config, &f->server) != CF_OK) {
        return false;
    }

    cf_http_loop_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.listen_fd = f->listen_fd;
    cfg.loop_index = 0;
    cfg.public_origin = f->origin;
    cfg.connections_per_loop = 16;
    cfg.admit = live_admit;
    cfg.upgrade = cf_cable_server_upgrade;
    cfg.upgrade_user = f->server;
    if (cf_http_loop_create(&cfg, &f->loop) != CF_OK) return false;
    if (pthread_create(&f->thread, NULL, live_loop_main, f) != 0) return false;
    return true;
}

static void live_stop(live_fixture *f) {
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
    chan_fixture_close(&f->app);
}

static int live_connect(const live_fixture *f) {
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

static size_t live_upgrade_request(char *buf, size_t cap, live_fixture *f,
                                   const char *cookie) {
    return (size_t)snprintf(
        buf, cap,
        "GET /cable HTTP/1.1\r\n"
        "Host: 127.0.0.1:%u\r\n"
        "Origin: http://127.0.0.1:%u\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Protocol: actioncable-v1-json\r\n"
        "Cookie: %s\r\n"
        "\r\n",
        f->port, f->port, cookie);
}

/* Read one server text frame, skipping pings. */
static bool live_next_text(int fd, char *buf, size_t cap, int timeout_ms) {
    int64_t deadline = ct_now_ms() + timeout_ms;
    for (;;) {
        int left = (int)(deadline - ct_now_ms());
        if (left <= 0) return false;
        unsigned opcode = 0;
        bool rsv1 = false;
        ssize_t n = ct_read_server_frame(fd, &opcode, &rsv1,
                                         (unsigned char *)buf, cap - 1, left);
        if (n < 0 || opcode != 0x1) return false;
        buf[n] = '\0';
        if (strncmp(buf, "{\"type\":\"ping\"", 14) == 0) continue;
        return true;
    }
}

static bool live_expect(int fd, const char *expected, int timeout_ms) {
    char got[16384];
    if (!live_next_text(fd, got, sizeof got, timeout_ms)) {
        fprintf(stderr, "  no frame; expected: %s\n", expected);
        return false;
    }
    if (strcmp(got, expected) != 0) {
        fprintf(stderr, "  frame mismatch\n  got:      %s\n  expected: %s\n",
                got, expected);
        return false;
    }
    return true;
}

CF_TEST(live_cable_handshake_subscribe_and_broadcast) {
    live_fixture f;
    CF_REQUIRE(live_start(&f));

    char cookie[1024];
    chan_cookie(&f.app, CHAN_KEVIN, cookie, sizeof cookie);
    int fd = live_connect(&f);
    CF_REQUIRE(fd >= 0);
    char request[4096];
    size_t n = live_upgrade_request(request, sizeof request, &f, cookie);
    CF_REQUIRE(ct_send_all(fd, request, n));
    char head[2048];
    CF_REQUIRE(ct_read_http_head(fd, head, sizeof head, 3000) > 0);
    CF_CHECK(strstr(head, "101 Switching Protocols") != NULL);
    CF_CHECK(strstr(head, "actioncable-v1-json") != NULL);
    CF_CHECK(live_expect(fd, "{\"type\":\"welcome\"}", 3000));

    /* A real subscribe: session -> channel -> confirmation. */
    char identifier[128];
    chan_room_identifier(identifier, sizeof identifier, "RoomChannel",
                         CHAN_DESIGNERS);
    char quoted[256];
    chan_json_string(quoted, sizeof quoted, identifier);
    char command[512];
    snprintf(command, sizeof command,
             "{\"command\":\"subscribe\",\"identifier\":%s}", quoted);
    CF_REQUIRE(ct_send_client_frame(fd, 0x1, true, false,
                                    (const unsigned char *)command,
                                    strlen(command)));
    char expected[1024];
    chan_confirm_expected(expected, sizeof expected, identifier);
    CF_CHECK(live_expect(fd, expected, 3000));

    /* A broadcast published from another thread reaches the real socket. */
    char stream[256];
    chan_room_stream(&f.app, "RoomChannel", CHAN_DESIGNERS, stream,
                     sizeof stream);
    cf_buf *payload = NULL;
    CF_REQUIRE(cf_buf_copy((cf_span){(const unsigned char *)"{\"live\":1}", 10},
                           &payload) == CF_OK);
    CF_REQUIRE(cf_cable_publish(
                   f.app.cable,
                   (cf_span){(const unsigned char *)stream, strlen(stream)},
                   payload) == CF_OK);
    cf_buf_release(payload);
    char delivery[1024];
    chan_delivery_expected(delivery, sizeof delivery, identifier,
                           "{\"live\":1}");
    CF_CHECK(live_expect(fd, delivery, 3000));

    /* A second channel on the same connection. */
    char heartbeat[64];
    chan_channel_identifier(heartbeat, sizeof heartbeat, "HeartbeatChannel");
    chan_json_string(quoted, sizeof quoted, heartbeat);
    snprintf(command, sizeof command,
             "{\"command\":\"subscribe\",\"identifier\":%s}", quoted);
    CF_REQUIRE(ct_send_client_frame(fd, 0x1, true, false,
                                    (const unsigned char *)command,
                                    strlen(command)));
    chan_confirm_expected(expected, sizeof expected, heartbeat);
    CF_CHECK(live_expect(fd, expected, 3000));

    /* Close handshake, then a clean stop. */
    CF_REQUIRE(ct_send_client_frame(fd, 0x8, true, false, NULL, 0));
    unsigned opcode = 0;
    bool rsv1 = false;
    unsigned char frame[256];
    ssize_t m = ct_read_server_frame(fd, &opcode, &rsv1, frame, sizeof frame,
                                     3000);
    CF_CHECK(m >= 0 && opcode == 0x8);
    close(fd);
    /* PresenceChannel was not involved, so the connection score is 0. */
    live_stop(&f);
}

CF_TEST_MAIN()
