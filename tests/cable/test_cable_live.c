/* tests/cable/test_cable_live.c — C02 production wiring over a real socket:
 * an H01 loopback HTTP loop with cf_cable_server_upgrade installed exactly as
 * main.c installs it, the cable's own authenticate/on_text hooks, a real
 * signed session cookie, a real Action Cable handshake, subscribe/confirm and
 * one broadcast round-trip.
 *
 * P12-02 acceptance: the upgrade keeps its HTTP connection-slot reservation
 * for its whole lifetime; one bounded reactor thread per HTTP loop services
 * every upgraded socket (never one thread/reader per connection); releases
 * happen exactly once across normal closes, resets, failed upgrades and
 * broadcast/idle load. */
#include "cable_channels_testutil.h"

#include "cable/revocation.h"
#include "db/writer.h"
#include "http/http.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <stdatomic.h>

#define LIVE_MAX_LOOPS 2

/* Cap for every positive wait in this file. These assertions are about
 * eventual state, never a latency budget: under machine oversubscription
 * (several concurrent test instances on one machine) a correct server can be
 * delayed for many seconds. The waits below poll (or read in poll slices)
 * until this cap, so a genuinely stuck condition still fails — just later.
 * The cap stays finite, and it comfortably exceeds the one production
 * latency this file depends on: the ~5 s server-side close timeout. */
#define LIVE_EVENTUAL_MS 20000

typedef struct {
    chan_fixture app;
    int listen_fd;
    unsigned port;
    char origin[64];
    size_t loop_count;
    size_t connections_per_loop;
    int listen_sndbuf; /* nonzero: small kernel send buffer for the P12-02b
                        * aggregate output-budget case */
    cf_http_loop *loops[LIVE_MAX_LOOPS];
    pthread_t threads[LIVE_MAX_LOOPS];
    size_t started;
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
    cf_http_loop *loop = arg;
    (void)cf_http_loop_run(loop);
    return NULL;
}

static bool live_spawn_loop(live_fixture *f, size_t i) {
    if (pthread_create(&f->threads[i], NULL, live_loop_main, f->loops[i]) !=
        0) {
        return false;
    }
    f->started++;
    return true;
}

/* Create `loops` loops with per-loop connection budgets `caps`, spawning
 * threads for the first `spawn_now` only; the rest are spawned later with
 * live_start_remaining. Seeding a loop that runs alone before the others
 * start is what makes the two-loop accept distribution deterministic (see
 * live_open_across_two_loops). */
static bool live_start_cfg_phased(live_fixture *f, size_t loops,
                                  const size_t *caps, size_t spawn_now,
                                  int listen_sndbuf, size_t output_bytes) {
    if (loops == 0 || loops > LIVE_MAX_LOOPS) return false;
    if (spawn_now > loops) spawn_now = loops;
    memset(f, 0, sizeof *f);
    f->listen_fd = -1;
    f->loop_count = loops;
    f->connections_per_loop = caps[0];
    f->listen_sndbuf = listen_sndbuf;
    if (!chan_fixture_open(&f->app)) return false;
    f->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (f->listen_fd < 0) return false;
    int one = 1;
    setsockopt(f->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (listen_sndbuf > 0) {
        /* Accepted sockets inherit SO_SNDBUF on Linux: a small send buffer
         * keeps queued wire bytes charged to the reactor's aggregate output
         * budget instead of being absorbed by the kernel. */
        (void)setsockopt(f->listen_fd, SOL_SOCKET, SO_SNDBUF, &listen_sndbuf,
                         sizeof listen_sndbuf);
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(f->listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(f->listen_fd, 64) != 0) {
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
    server_config.loops = loops;
    /* The documented per-reactor floor keeps this at 8 MiB per loop whatever
     * smaller total is configured; the fixture passes the configured total. */
    if (output_bytes != 0) server_config.output_bytes = output_bytes;
    cf_cable_server_hooks(f->app.cable, &server_config.hooks);
    if (cf_cable_server_create(&server_config, &f->server) != CF_OK) {
        return false;
    }

    for (size_t i = 0; i < loops; i++) {
        cf_http_loop_config cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.listen_fd = f->listen_fd;
        cfg.loop_index = (uint32_t)i;
        cfg.public_origin = f->origin;
        cfg.connections_per_loop = caps[i];
        cfg.admit = live_admit;
        cfg.upgrade = cf_cable_server_upgrade;
        cfg.upgrade_user = f->server;
        if (cf_http_loop_create(&cfg, &f->loops[i]) != CF_OK) return false;
        if (i < spawn_now && !live_spawn_loop(f, i)) return false;
    }
    return true;
}

static bool live_start_remaining(live_fixture *f) {
    for (size_t i = f->started; i < f->loop_count; i++) {
        if (!live_spawn_loop(f, i)) return false;
    }
    return true;
}

static bool live_start_cfg_opts(live_fixture *f, size_t loops,
                                size_t connections_per_loop, int listen_sndbuf,
                                size_t output_bytes) {
    if (loops == 0 || loops > LIVE_MAX_LOOPS) return false;
    size_t caps[LIVE_MAX_LOOPS];
    for (size_t i = 0; i < loops; i++) caps[i] = connections_per_loop;
    return live_start_cfg_phased(f, loops, caps, loops, listen_sndbuf,
                                 output_bytes);
}

static bool live_start_cfg(live_fixture *f, size_t loops,
                           size_t connections_per_loop) {
    return live_start_cfg_opts(f, loops, connections_per_loop, 0, 0);
}

static bool live_start(live_fixture *f) {
    return live_start_cfg(f, 1, 16);
}

static void live_stop(live_fixture *f) {
    /* Stop the loops first, then stop the cable server (which shuts the
     * upgraded sockets down and releases their lifetime reservations, letting
     * the loops drain), join the loops, and only then free the server: no
     * loop thread may still be inside the upgrade hook while it is freed. */
    for (size_t i = 0; i < f->started; i++) {
        cf_http_loop_stop(f->loops[i]);
    }
    if (f->server != NULL) cf_cable_server_stop(f->server);
    for (size_t i = 0; i < f->started; i++) {
        pthread_join(f->threads[i], NULL);
    }
    if (f->server != NULL) {
        cf_cable_server_destroy(f->server);
        f->server = NULL;
    }
    for (size_t i = 0; i < f->loop_count; i++) {
        if (f->loops[i] != NULL) cf_http_loop_destroy(f->loops[i]);
        f->loops[i] = NULL;
    }
    f->started = 0;
    f->loop_count = 0;
    if (f->listen_fd >= 0) close(f->listen_fd);
    f->listen_fd = -1;
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

static bool live_wait_connections(const live_fixture *f, size_t want,
                                  int timeout_ms) {
    int64_t deadline = ct_now_ms() + timeout_ms;
    while (ct_now_ms() < deadline) {
        if (cf_cable_server_connections(f->server) == want) return true;
        struct timespec ts = {0, 2 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    return cf_cable_server_connections(f->server) == want;
}

/* Connect and complete one real authenticated upgrade; false (with the fd
 * closed) on any failure. */
static bool live_upgrade_ok(live_fixture *f, const char *cookie, int *out_fd) {
    int fd = live_connect(f);
    if (fd < 0) return false;
    char request[4096];
    size_t n = live_upgrade_request(request, sizeof request, f, cookie);
    if (!ct_send_all(fd, request, n)) {
        close(fd);
        return false;
    }
    char head[2048];
    if (ct_read_http_head(fd, head, sizeof head, LIVE_EVENTUAL_MS) <= 0 ||
        strstr(head, "101 Switching Protocols") == NULL) {
        close(fd);
        return false;
    }
    if (!live_expect(fd, "{\"type\":\"welcome\"}", LIVE_EVENTUAL_MS)) {
        close(fd);
        return false;
    }
    *out_fd = fd;
    return true;
}

/* Open `want` live upgraded sockets with both owner reactors established
 * deterministically. The fixture must be started with
 * live_start_cfg_phased(..., 1, ...) and a one-connection budget for loop 0:
 * the seed upgrade is then accepted by the only running loop, and every
 * later connection that succeeds can only be served by the loop started
 * afterwards, because loop 0's single connection slot stays held by the seed
 * for its lifetime (accepted connections at cap are closed at accept). The
 * retries are bounded by `deadline`, so a reactor that genuinely cannot be
 * created fails the caller's assertions instead of hanging. */
/* The two phases of live_open_across_two_loops: seed the only running loop,
 * then open the rest after the other loop(s) start. Callers that need a
 * thread-count baseline with both owner loops running use these directly. */
static bool live_seed_first_loop(live_fixture *f, const char *cookie,
                                 int *out_fd) {
    return live_upgrade_ok(f, cookie, out_fd);
}

static bool live_open_rest(live_fixture *f, const char *cookie, int *fds,
                           size_t want, size_t *opened, int64_t deadline) {
    while (*opened < want) {
        int fd = -1;
        if (live_upgrade_ok(f, cookie, &fd)) {
            fds[*opened] = fd;
            (*opened)++;
            continue;
        }
        if (ct_now_ms() >= deadline) return false;
        struct timespec ts = {0, 2 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    return true;
}

static bool live_open_across_two_loops(live_fixture *f, const char *cookie,
                                       int *fds, size_t want, size_t *opened,
                                       int64_t deadline) {
    if (*opened >= want) return true;
    if (!live_seed_first_loop(f, cookie, &fds[*opened])) return false;
    (*opened)++;
    if (!live_start_remaining(f)) return false;
    return live_open_rest(f, cookie, fds, want, opened, deadline);
}

/* The client-side close handshake, then close. */
static void live_close_ws(int fd) {
    (void)ct_send_client_frame(fd, 0x8, true, false, NULL, 0);
    unsigned opcode = 0;
    bool rsv1 = false;
    unsigned char frame[64];
    (void)ct_read_server_frame(fd, &opcode, &rsv1, frame, sizeof frame, 2000);
    close(fd);
}

/* Force an RST after an upgrade (the reset release path). */
static void live_reset_ws(int fd) {
    struct linger lin = {1, 0};
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lin, sizeof lin);
    close(fd);
}

/* Wait for the reference heartbeat ping (a text frame; the server sends it
 * as the reference JSON envelope, not as a WebSocket control frame). */
static bool live_expect_ping(int fd, int timeout_ms) {
    int64_t deadline = ct_now_ms() + timeout_ms;
    for (;;) {
        int left = (int)(deadline - ct_now_ms());
        if (left <= 0) return false;
        unsigned opcode = 0;
        bool rsv1 = false;
        unsigned char payload[512];
        ssize_t n = ct_read_server_frame(fd, &opcode, &rsv1, payload,
                                         sizeof payload - 1, left);
        if (n < 0) return false;
        payload[n] = '\0';
        if (opcode == 0x1 &&
            strncmp((char *)payload, "{\"type\":\"ping\"", 14) == 0) {
            return true;
        }
    }
}

/* How many threads this process has (the P12-02 measurement). */
static int live_thread_count(void) {
    DIR *dir = opendir("/proc/self/task");
    if (dir == NULL) return -1;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] != '.') count++;
    }
    closedir(dir);
    return count;
}

/* A plain HTTP request on a connection the loop should have closed at accept
 * (it must not become an upgrade or a response). */
static bool live_expect_refused(const live_fixture *f) {
    int fd = live_connect(f);
    if (fd < 0) return false;
    char request[512];
    int n = snprintf(request, sizeof request,
                     "GET /up HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n",
                     f->port);
    (void)ct_send_all(fd, request, (size_t)n);
    char head[512];
    ssize_t got = ct_read_http_head(fd, head, sizeof head, 2000);
    bool refused = got <= 0 || strstr(head, "HTTP/1.1 101") == NULL;
    close(fd);
    return refused;
}

/* An invalid /cable request (no upgrade headers) must be answered 404 once
 * the loop admits the connection. Under load the just-released upgrade can
 * leave the loop's admission momentarily full, so the new connection is
 * refused at accept and arrives as a reset; retry within `deadline` so the
 * eventual 404 is asserted, while a server that never answers it still fails
 * at the cap. */
static bool live_expect_invalid_404(const live_fixture *f, int64_t deadline) {
    for (;;) {
        int fd = live_connect(f);
        if (fd >= 0) {
            char request[256];
            int n = snprintf(request, sizeof request,
                             "GET /cable HTTP/1.1\r\n"
                             "Host: 127.0.0.1:%u\r\n"
                             "Connection: Upgrade\r\n"
                             "Upgrade: websocket\r\n"
                             "\r\n",
                             f->port);
            if (ct_send_all(fd, request, (size_t)n)) {
                char reply[1024];
                int64_t left = deadline - ct_now_ms();
                if (left > 0 &&
                    ct_read_until(fd, reply, sizeof reply, "404",
                                  (int)left) > 0) {
                    close(fd);
                    return true;
                }
            }
            close(fd);
        }
        if (ct_now_ms() >= deadline) return false;
        struct timespec ts = {0, 2 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
}

CF_TEST(live_cable_handshake_subscribe_and_broadcast) {
    live_fixture f;
    CF_REQUIRE(live_start(&f));

    char cookie[1024];
    chan_cookie(&f.app, CHAN_KEVIN, cookie, sizeof cookie);
    int fd = -1;
    CF_REQUIRE(live_upgrade_ok(&f, cookie, &fd));

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
    CF_CHECK(live_expect(fd, expected, LIVE_EVENTUAL_MS));

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
    CF_CHECK(live_expect(fd, delivery, LIVE_EVENTUAL_MS));

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
    CF_CHECK(live_expect(fd, expected, LIVE_EVENTUAL_MS));

    /* Close handshake, then a clean stop. */
    live_close_ws(fd);
    CF_CHECK(live_wait_connections(&f, 0, LIVE_EVENTUAL_MS));
    live_stop(&f);
}

/* P12-02 acceptance (a): with a one-connection loop budget, one live upgraded
 * socket prevents every new admission until it closes, and each ending
 * releases the reservation exactly once. Repeated across normal closes,
 * resets, refused upgrades and post-upgrade authentication failures. */
CF_TEST(single_slot_admission_releases_exactly_once) {
    live_fixture f;
    CF_REQUIRE(live_start_cfg(&f, 1, 1));

    char cookie[1024];
    chan_cookie(&f.app, CHAN_JZ, cookie, sizeof cookie);

    for (int cycle = 0; cycle < 3; cycle++) {
        int a = -1;
        CF_REQUIRE(live_upgrade_ok(&f, cookie, &a));
        CF_CHECK(live_wait_connections(&f, 1, LIVE_EVENTUAL_MS));

        /* Every further admission is refused while the upgrade lives: an
         * ordinary request and a second, valid upgrade both get no
         * response. */
        CF_CHECK(live_expect_refused(&f));
        int b = live_connect(&f);
        CF_REQUIRE(b >= 0);
        char request[4096];
        size_t n = live_upgrade_request(request, sizeof request, &f, cookie);
        CF_REQUIRE(ct_send_all(b, request, n));
        char head[512];
        ssize_t got = ct_read_http_head(b, head, sizeof head, 2000);
        CF_CHECK(got <= 0 || strstr(head, "101 Switching Protocols") == NULL);
        close(b);
        CF_CHECK(cf_cable_server_connections(f.server) == 1);

        if (cycle == 0) {
            /* The normal close releases once; the next cycle must fit. */
            live_close_ws(a);
        } else if (cycle == 1) {
            /* A reset (RST) releases through the same single path. */
            live_reset_ws(a);
        } else {
            live_close_ws(a);
        }
        CF_CHECK(live_wait_connections(&f, 0, LIVE_EVENTUAL_MS));

        /* A failed upgrade never consumes the freed slot: an invalid /cable
         * request is answered 404 (retried within the cap, since the freed
         * slot may not have returned to the loop yet) and the next real
         * upgrade still fits. */
        CF_CHECK(live_expect_invalid_404(&f,
                                         ct_now_ms() + LIVE_EVENTUAL_MS));
        CF_CHECK(live_wait_connections(&f, 0, LIVE_EVENTUAL_MS));
    }

    /* A fully authenticated-looking upgrade with a bad cookie fails after
     * the 101; its socket still releases its reservation exactly once. */
    int bad_auth = -1;
    int fd = live_connect(&f);
    CF_REQUIRE(fd >= 0);
    char bad_cookie[64];
    snprintf(bad_cookie, sizeof bad_cookie, "session_token=bogus");
    char request[4096];
    size_t n = live_upgrade_request(request, sizeof request, &f, bad_cookie);
    CF_REQUIRE(ct_send_all(fd, request, n));
    char head[2048];
    CF_REQUIRE(ct_read_http_head(fd, head, sizeof head, LIVE_EVENTUAL_MS) > 0);
    CF_CHECK(strstr(head, "101 Switching Protocols") != NULL);
    CF_CHECK(live_wait_connections(&f, 1, LIVE_EVENTUAL_MS));
    /* The unauthorized disconnect frame, then the close frame. */
    char got[512];
    CF_CHECK(live_next_text(fd, got, sizeof got, LIVE_EVENTUAL_MS));
    unsigned opcode = 0;
    bool rsv1 = false;
    unsigned char frame[64];
    ssize_t m = ct_read_server_frame(fd, &opcode, &rsv1, frame, sizeof frame,
                                     LIVE_EVENTUAL_MS);
    CF_CHECK(m >= 0 && opcode == 0x8);
    close(fd);
    CF_CHECK(live_wait_connections(&f, 0, LIVE_EVENTUAL_MS));

    CF_REQUIRE(live_upgrade_ok(&f, cookie, &bad_auth));
    CF_CHECK(live_wait_connections(&f, 1, LIVE_EVENTUAL_MS));
    live_close_ws(bad_auth);
    CF_CHECK(live_wait_connections(&f, 0, LIVE_EVENTUAL_MS));

    /* The close timeout also releases exactly once: a peer that never answers
     * the server's close frame keeps the slot until the 5 s deadline, then
     * the connection finishes and the next upgrade fits. */
    int silent = live_connect(&f);
    CF_REQUIRE(silent >= 0);
    CF_REQUIRE(ct_send_all(silent, request,
                           live_upgrade_request(request, sizeof request, &f,
                                                bad_cookie)));
    CF_REQUIRE(ct_read_http_head(silent, head, sizeof head, LIVE_EVENTUAL_MS) >
               0);
    CF_CHECK(strstr(head, "101 Switching Protocols") != NULL);
    CF_CHECK(live_wait_connections(&f, 1, LIVE_EVENTUAL_MS));
    CF_CHECK(live_expect_refused(&f)); /* still admitted while closing */
    /* The 5 s close timeout still releases well inside the eventual cap. */
    CF_CHECK(live_wait_connections(&f, 0, LIVE_EVENTUAL_MS));
    close(silent);
    CF_REQUIRE(live_upgrade_ok(&f, cookie, &bad_auth));
    CF_CHECK(live_wait_connections(&f, 1, LIVE_EVENTUAL_MS));
    live_close_ws(bad_auth);
    CF_CHECK(live_wait_connections(&f, 0, LIVE_EVENTUAL_MS));
    live_stop(&f);
}

/* P12-02 acceptance (b) and (d): many upgraded sockets add exactly one
 * reactor thread for their loop, and idle connected sockets keep the thread
 * and connection counts flat across the ping cadence. */
CF_TEST(threads_do_not_grow_per_socket_or_by_idling) {
    live_fixture f;
    CF_REQUIRE(live_start_cfg(&f, 1, 16));

    char cookie[1024];
    chan_cookie(&f.app, CHAN_JZ, cookie, sizeof cookie);
    int before = live_thread_count();
    CF_REQUIRE(before > 0);

    enum { LIVE_SOCKETS = 8 };
    int fds[LIVE_SOCKETS];
    for (int i = 0; i < LIVE_SOCKETS; i++) {
        fds[i] = -1;
        CF_REQUIRE(live_upgrade_ok(&f, cookie, &fds[i]));
    }
    CF_CHECK(live_wait_connections(&f, LIVE_SOCKETS, LIVE_EVENTUAL_MS));
    int after = live_thread_count();
    CF_REQUIRE(after > 0);
    CF_CHECK(after - before <= 1);
    CF_CHECK(cf_cable_server_reactors(f.server) == 1);

    /* Idle across more than one 3 s ping beat: no growth, all still live,
     * and the reference heartbeat still arrives. */
    struct timespec idle = {3, 400 * 1000 * 1000};
    nanosleep(&idle, NULL);
    CF_CHECK(cf_cable_server_connections(f.server) == LIVE_SOCKETS);
    int later = live_thread_count();
    /* Bounded by the one reactor thread, whatever the runtime (Fil-C's GC
     * may add a helper thread); idle sockets add nothing per socket. */
    CF_CHECK(later - before <= 1);
    CF_CHECK(live_expect_ping(fds[0], LIVE_EVENTUAL_MS));

    for (int i = 0; i < LIVE_SOCKETS; i++) {
        live_close_ws(fds[i]);
    }
    CF_CHECK(live_wait_connections(&f, 0, LIVE_EVENTUAL_MS));
    CF_CHECK(cf_cable_server_reactors(f.server) == 1);
    live_stop(&f);
}

/* P12-02 acceptance (c): with two HTTP loops, upgraded sockets are serviced
 * by at most two reactor threads and cross-loop broadcast delivery still
 * reaches sockets on both loops. */
CF_TEST(two_loops_bound_threads_and_deliver_cross_loop) {
    live_fixture f;
    /* Deterministic spread: loop 0 starts alone with a one-connection budget
     * and is seeded with one live upgrade, then loop 1 starts; loop 0 can
     * never accept again, so every remaining socket lands on loop 1 (see
     * live_open_across_two_loops). No burst lottery, no one-sided outcome. */
    static const size_t loop_caps[2] = {1, 64};
    CF_REQUIRE(live_start_cfg_phased(&f, 2, loop_caps, 1, 0, 0));

    char cookie[1024];
    chan_cookie(&f.app, CHAN_JZ, cookie, sizeof cookie);

    enum { LIVE_CROSS_SOCKETS = 32 };
    int fds[LIVE_CROSS_SOCKETS];
    size_t opened = 0;
    CF_REQUIRE(live_seed_first_loop(&f, cookie, &fds[opened]));
    opened++;
    CF_REQUIRE(live_start_remaining(&f));
    /* Both owner loops now run and loop 0's reactor is already counted in the
     * baseline, so only loop 1's reactor thread may be new once the rest
     * open: one bounded reactor thread per loop, never one per socket. */
    int before = live_thread_count();
    CF_REQUIRE(before > 0);
    CF_CHECK(live_open_rest(&f, cookie, fds, LIVE_CROSS_SOCKETS, &opened,
                            ct_now_ms() + LIVE_EVENTUAL_MS));
    CF_CHECK(opened == LIVE_CROSS_SOCKETS);
    CF_CHECK(cf_cable_server_reactors(f.server) == 2);
    CF_CHECK(live_wait_connections(&f, opened, LIVE_EVENTUAL_MS));
    int after = live_thread_count();
    CF_REQUIRE(after > 0);
    CF_CHECK(after - before <= 1);

    /* Both loops deliver the same broadcast: subscribe every socket to the
     * shared room stream and publish once. */
    char identifier[128];
    chan_room_identifier(identifier, sizeof identifier, "RoomChannel",
                         CHAN_DESIGNERS);
    char quoted[256];
    chan_json_string(quoted, sizeof quoted, identifier);
    char command[512];
    snprintf(command, sizeof command,
             "{\"command\":\"subscribe\",\"identifier\":%s}", quoted);
    char expected[1024];
    chan_confirm_expected(expected, sizeof expected, identifier);
    for (size_t i = 0; i < opened; i++) {
        CF_REQUIRE(ct_send_client_frame(fds[i], 0x1, true, false,
                                        (const unsigned char *)command,
                                        strlen(command)));
        CF_REQUIRE(live_expect(fds[i], expected, LIVE_EVENTUAL_MS));
    }
    char stream[256];
    chan_room_stream(&f.app, "RoomChannel", CHAN_DESIGNERS, stream,
                     sizeof stream);
    cf_buf *payload = NULL;
    CF_REQUIRE(cf_buf_copy(
                   (cf_span){(const unsigned char *)"{\"cross\":1}", 11},
                   &payload) == CF_OK);
    CF_REQUIRE(cf_cable_publish(
                   f.app.cable,
                   (cf_span){(const unsigned char *)stream, strlen(stream)},
                   payload) == CF_OK);
    cf_buf_release(payload);
    char delivery[1024];
    chan_delivery_expected(delivery, sizeof delivery, identifier,
                           "{\"cross\":1}");
    for (size_t i = 0; i < opened; i++) {
        CF_CHECK(live_expect(fds[i], delivery, LIVE_EVENTUAL_MS));
    }

    for (size_t i = 0; i < opened; i++) {
        live_close_ws(fds[i]);
    }
    CF_CHECK(live_wait_connections(&f, 0, LIVE_EVENTUAL_MS));
    live_stop(&f);
}

/* P12-02b acceptance (b): upgrade authentication and subscribe
 * validation/model effects execute on the app's bounded request-worker pool,
 * never on the reactor, and the reactor holds no DB reader. */
CF_TEST(model_work_runs_on_pool_workers_not_the_reactor) {
    live_fixture f;
    CF_REQUIRE(live_start(&f));
    cf_cable_test_reset_work_identity(f.app.cable);

    char cookie[1024];
    chan_cookie(&f.app, CHAN_KEVIN, cookie, sizeof cookie);
    int fd = -1;
    CF_REQUIRE(live_upgrade_ok(&f, cookie, &fd));

    cf_cable_work_identity auth;
    cf_cable_test_work_identity(f.app.cable, false, &auth);
    CF_CHECK(auth.ran);
    CF_CHECK(auth.on_worker);                 /* ran with a worker's reader */
    CF_CHECK(auth.thread != auth.owner_thread); /* not the reactor thread */
    CF_CHECK(auth.thread != (uintptr_t)pthread_self());
    CF_CHECK(!auth.owner_has_reader);         /* no DB reader on the reactor */

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
    CF_CHECK(live_expect(fd, expected, LIVE_EVENTUAL_MS));

    cf_cable_work_identity sub;
    cf_cable_test_work_identity(f.app.cable, true, &sub);
    CF_CHECK(sub.ran);
    CF_CHECK(sub.on_worker);
    CF_CHECK(sub.thread != sub.owner_thread);
    CF_CHECK(!sub.owner_has_reader);

    /* One control slot per owner loop, whatever the connection count. */
    CF_CHECK(cf_cable_server_reactors(f.server) == 1);
    CF_CHECK(cf_cable_test_revocation_loop_slots(f.app.cable) == 1);

    live_close_ws(fd);
    CF_CHECK(live_wait_connections(&f, 0, LIVE_EVENTUAL_MS));
    live_stop(&f);
}

/* P12-02b acceptance (c): one revocation control slot per owner loop revokes
 * every connection of the user in one barrier. 24 live sockets across two
 * HTTP loops (one shared slot each) all receive the exact disconnect frame
 * and release their admission. */
static int64_t live_revoke_user_id;
static bool live_revoke_reconnect;

static cf_err live_revoke_write(cf_tx *tx, void *arg) {
    (void)arg;
    return cf_tx_event(tx,
                       (cf_event){.kind = CF_EVENT_DISCONNECT_USER,
                                  .user_id = live_revoke_user_id,
                                  .reconnect = live_revoke_reconnect});
}

CF_TEST(one_control_slot_per_loop_revokes_24_sockets) {
    live_fixture f;
    /* Deterministic spread: loop 0 starts alone with a one-connection budget
     * and is seeded with the first live socket, so it can never accept
     * another connection; loop 1 then starts and serves the remaining 23
     * (see live_open_across_two_loops). Both reactors are established before
     * the assertions below, which stay strict. */
    static const size_t loop_caps[2] = {1, 64};
    CF_REQUIRE(live_start_cfg_phased(&f, 2, loop_caps, 1, 0, 0));
    CF_REQUIRE(cf_writer_set_control_handler(f.app.app,
                                             cf_cable_revocation_handler,
                                             f.app.cable) == CF_OK);

    char cookie[1024];
    chan_cookie(&f.app, CHAN_JZ, cookie, sizeof cookie);
    char identifier[128];
    chan_room_identifier(identifier, sizeof identifier, "RoomChannel",
                         CHAN_DESIGNERS);
    char quoted[256];
    chan_json_string(quoted, sizeof quoted, identifier);
    char command[512];
    snprintf(command, sizeof command,
             "{\"command\":\"subscribe\",\"identifier\":%s}", quoted);
    char expected[1024];
    chan_confirm_expected(expected, sizeof expected, identifier);

    enum { REVOKE_SOCKETS = 24 };
    int fds[REVOKE_SOCKETS];
    size_t opened = 0;
    CF_CHECK(live_open_across_two_loops(&f, cookie, fds, REVOKE_SOCKETS,
                                        &opened,
                                        ct_now_ms() + LIVE_EVENTUAL_MS));
    CF_CHECK(opened == REVOKE_SOCKETS);
    CF_CHECK(cf_cable_server_reactors(f.server) == 2);
    CF_CHECK(cf_cable_test_revocation_loop_slots(f.app.cable) == 2);
    CF_CHECK(live_wait_connections(&f, opened, LIVE_EVENTUAL_MS));

    for (size_t i = 0; i < opened; i++) {
        CF_REQUIRE(ct_send_client_frame(fds[i], 0x1, true, false,
                                        (const unsigned char *)command,
                                        strlen(command)));
    }
    for (size_t i = 0; i < opened; i++) {
        CF_REQUIRE(live_expect(fds[i], expected, LIVE_EVENTUAL_MS));
    }

    /* One committed logout: the writer barrier returns only after both loops
     * acknowledged their single shared control slot. */
    live_revoke_user_id = CHAN_JZ;
    live_revoke_reconnect = true;
    CF_CHECK(cf_write(f.app.app, live_revoke_write, NULL) == CF_OK);

    for (size_t i = 0; i < opened; i++) {
        CF_CHECK(live_expect(
            fds[i],
            "{\"type\":\"disconnect\",\"reason\":\"remote\",\"reconnect\":true}",
            LIVE_EVENTUAL_MS));
        unsigned opcode = 0;
        bool rsv1 = false;
        unsigned char frame[64];
        ssize_t n = ct_read_server_frame(fds[i], &opcode, &rsv1, frame,
                                         sizeof frame, LIVE_EVENTUAL_MS);
        CF_CHECK(n >= 0 && opcode == 0x8);
        close(fds[i]);
    }
    CF_CHECK(live_wait_connections(&f, 0, LIVE_EVENTUAL_MS));
    /* Both owner loops are still there with their one slot each (they are
     * joined and unregistered by server stop). */
    CF_CHECK(cf_cable_test_revocation_loop_slots(f.app.cable) == 2);
    live_stop(&f);
}

/* P12-02b acceptance (d): the aggregate OUTPUT budget covers the HTTP->Cable
 * transition. Three subscribed sockets whose kernel send buffers are small
 * hold ~3 MiB of queued wire bytes each; the 8 MiB per-reactor floor trips
 * and closes exactly the socket whose next frame no longer fits, while the
 * per-socket 4 MiB pending cap is never the cause and the reactor stays
 * healthy. */
CF_TEST(aggregate_output_budget_closes_across_http_to_cable) {
    live_fixture f;
    CF_REQUIRE(live_start_cfg_opts(&f, 1, 16, 16384, (size_t)8 << 20));

    char cookie[1024];
    chan_cookie(&f.app, CHAN_JZ, cookie, sizeof cookie);
    enum { BUDGET_SOCKETS = 3 };
    int fds[BUDGET_SOCKETS];
    for (int i = 0; i < BUDGET_SOCKETS; i++) {
        fds[i] = -1;
        CF_REQUIRE(live_upgrade_ok(&f, cookie, &fds[i]));
    }

    char identifier[128];
    chan_room_identifier(identifier, sizeof identifier, "RoomChannel",
                         CHAN_DESIGNERS);
    char quoted[256];
    chan_json_string(quoted, sizeof quoted, identifier);
    char command[512];
    snprintf(command, sizeof command,
             "{\"command\":\"subscribe\",\"identifier\":%s}", quoted);
    char expected[1024];
    chan_confirm_expected(expected, sizeof expected, identifier);
    for (int i = 0; i < BUDGET_SOCKETS; i++) {
        CF_REQUIRE(ct_send_client_frame(fds[i], 0x1, true, false,
                                        (const unsigned char *)command,
                                        strlen(command)));
        CF_REQUIRE(live_expect(fds[i], expected, LIVE_EVENTUAL_MS));
    }
    /* Stop reading now: the peers keep their receive windows closed. */

    size_t blob_len = (size_t)1 << 20; /* one 1 MiB JSON string per frame */
    char *blob = malloc(blob_len);
    CF_REQUIRE(blob != NULL);
    memset(blob, 'a', blob_len);
    blob[0] = '"';
    blob[blob_len - 1] = '"';
    cf_buf *payload = NULL;
    CF_REQUIRE(cf_buf_copy((cf_span){(const unsigned char *)blob, blob_len},
                           &payload) == CF_OK);
    free(blob);
    char stream[256];
    chan_room_stream(&f.app, "RoomChannel", CHAN_DESIGNERS, stream,
                     sizeof stream);
    cf_span stream_span = {(const unsigned char *)stream, strlen(stream)};
    for (int round = 0; round < 3; round++) {
        (void)cf_cable_publish(f.app.cable, stream_span, payload);
    }
    cf_buf_release(payload);

    /* The over-budget close is observed by the reactor on its next pass (the
     * enqueue wakes it): wait for the finish before reading the counters. */
    int64_t deadline = ct_now_ms() + LIVE_EVENTUAL_MS;
    cf_cable_socket_stats stats;
    memset(&stats, 0, sizeof stats);
    while (ct_now_ms() < deadline) {
        cf_cable_server_stats(f.server, &stats);
        if (stats.over_budget_closes >= 1) break;
        struct timespec ts = {0, 2 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    CF_CHECK(stats.over_budget_closes >= 1); /* aggregate, not per socket */
    CF_CHECK(stats.queue_cap_closes == 0);
    CF_CHECK(cf_cable_server_connections(f.server) < BUDGET_SOCKETS);

    /* The reactor is healthy: a fresh upgrade is admitted and subscribes. */
    int fresh = -1;
    CF_REQUIRE(live_upgrade_ok(&f, cookie, &fresh));
    CF_REQUIRE(ct_send_client_frame(fresh, 0x1, true, false,
                                    (const unsigned char *)command,
                                    strlen(command)));
    CF_CHECK(live_expect(fresh, expected, LIVE_EVENTUAL_MS));
    live_close_ws(fresh);

    for (int i = 0; i < BUDGET_SOCKETS; i++) {
        if (fds[i] >= 0) close(fds[i]);
    }
    live_stop(&f);
}

CF_TEST_MAIN()
