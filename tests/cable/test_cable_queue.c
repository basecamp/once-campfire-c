/* tests/cable/test_cable_queue.c — C01 bounded output: the 4 MiB pending cap,
 * the 30 s stalled-write close (both counted separately), "never count a
 * queued frame as delivered", and the reference protocol ping cadence on an
 * idle healthy connection. */
#include "cable_testutil.h"

static char g_flood_text[64 * 1024];
static size_t g_flood_len;
static int g_flood_count;

static cf_err flood_on_text(void *user, cf_cable_socket *socket,
                            cf_span text) {
    (void)user;
    (void)text;
    for (int i = 0; i < g_flood_count; i++) {
        cf_err rc = cf_cable_socket_send_text(
            socket, (cf_span){(const unsigned char *)g_flood_text,
                              g_flood_len});
        if (rc != CF_OK) return rc;
    }
    return CF_OK;
}

static cf_err queue_auth(void *user, cf_cable_socket *socket,
                         const cf_cable_request *request,
                         bool *authenticated, int64_t *user_id) {
    (void)socket;
    (void)user;
    (void)request;
    *authenticated = true;
    *user_id = 1;
    return CF_OK;
}

static cf_cable_hooks queue_hooks(void) {
    cf_cable_hooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.authenticate = queue_auth;
    hooks.on_text = flood_on_text;
    return hooks;
}

static void read_welcome(ct_run *run) {
    char buf[512];
    CF_REQUIRE(ct_read_until(run->peer_fd, buf, sizeof buf, "\"welcome\"",
                             2000) > 0);
}

CF_TEST(pending_cap_closes_and_counts_queue_cap_separately) {
    g_flood_len = sizeof g_flood_text;
    memset(g_flood_text, 'q', g_flood_len);
    g_flood_count = 1;
    cf_cable_hooks hooks = queue_hooks();
    cf_cable_limits limits;
    cf_cable_limits_default(&limits);
    limits.max_pending_bytes = 32 * 1024;
    limits.beat_interval_ms = 60000; /* stay out of the way */
    ct_run run;
    CF_REQUIRE(ct_run_start(&run, &hooks, &limits, false));
    read_welcome(&run);

    CF_REQUIRE(ct_send_client_frame(run.peer_fd, 0x1, true, false,
                                    (const unsigned char *)"go", 2));
    /* The frame is larger than the cap: the connection closes without it. */
    ct_run_join(&run);
    close(run.fd);
    run.fd = -1;
    struct pollfd pf = {run.peer_fd, POLLIN | POLLHUP, 0};
    int pr = poll(&pf, 1, 3000);
    CF_CHECK(pr > 0 && (pf.revents & POLLHUP) != 0);
    CF_CHECK(run.stats.queue_cap_closes == 1);
    CF_CHECK(run.stats.write_timeout_closes == 0);
    /* Only the welcome frame was ever queued and delivered. */
    CF_CHECK(run.stats.frames_queued == 1);
    CF_CHECK(run.stats.frames_sent == 1);
    ct_run_finish(&run);
}

static char g_big_frames[4][200 * 1024];

static cf_err flush_four(void *user, cf_cable_socket *socket, cf_span text) {
    (void)user;
    (void)text;
    for (int i = 0; i < 4; i++) {
        cf_err rc = cf_cable_socket_send_text(
            socket, (cf_span){(const unsigned char *)g_big_frames[i],
                              sizeof g_big_frames[i]});
        if (rc != CF_OK) return rc;
    }
    return CF_OK;
}

CF_TEST(stalled_writer_closes_by_timeout_and_queued_is_not_delivered) {
    memset(g_big_frames, 's', sizeof g_big_frames);
    cf_cable_hooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.authenticate = queue_auth;
    hooks.on_text = flush_four;

    cf_cable_limits limits;
    cf_cable_limits_default(&limits);
    limits.write_timeout_ms = 200;
    limits.beat_interval_ms = 60000;
    ct_run run;
    CF_REQUIRE(ct_run_start(&run, &hooks, &limits, false));
    read_welcome(&run);
    CF_REQUIRE(ct_send_client_frame(run.peer_fd, 0x1, true, false,
                                    (const unsigned char *)"go", 2));

    /* Never read: the writer stalls and the connection must close itself. */
    ct_run_join(&run);
    close(run.fd);
    run.fd = -1;
    struct pollfd pf = {run.peer_fd, POLLIN | POLLHUP, 0};
    int pr = poll(&pf, 1, 5000);
    CF_CHECK(pr > 0 && (pf.revents & POLLHUP) != 0);
    CF_CHECK(run.stats.write_timeout_closes == 1);
    CF_CHECK(run.stats.queue_cap_closes == 0);
    /* Welcome + the four 200 KiB frames were queued; the stalled kernel
     * buffer means at least one was never delivered. */
    CF_CHECK(run.stats.frames_queued == 5);
    CF_CHECK(run.stats.frames_sent > 0);
    CF_CHECK(run.stats.frames_sent < 5);
    CF_CHECK(run.stats.bytes_sent < 4 * 200 * 1024);
    ct_run_finish(&run);
}

CF_TEST(idle_connection_keeps_the_reference_ping_cadence) {
    cf_cable_hooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.authenticate = queue_auth;
    cf_cable_limits limits;
    cf_cable_limits_default(&limits);
    limits.beat_interval_ms = 150;
    ct_run run;
    CF_REQUIRE(ct_run_start(&run, &hooks, &limits, false));
    read_welcome(&run);

    for (int i = 0; i < 2; i++) {
        unsigned opcode = 0;
        bool rsv1 = false;
        unsigned char payload[256];
        int64_t started = ct_now_ms();
        ssize_t n = ct_read_server_frame(run.peer_fd, &opcode, &rsv1, payload,
                                         sizeof payload, 2000);
        CF_REQUIRE(n > 0);
        CF_CHECK(opcode == 0x1); /* a text frame, not a control ping */
        CF_CHECK(ct_now_ms() - started <= 1800);
        if (n > 0) {
            payload[n < (ssize_t)sizeof payload ? (size_t)n
                                                : sizeof payload - 1] = '\0';
            CF_CHECK(strncmp((char *)payload, "{\"type\":\"ping\",\"message\":",
                             25) == 0);
            long long stamp = atoll((char *)payload + 25);
            int64_t now = (int64_t)time(NULL);
            CF_CHECK(stamp >= now - 5 && stamp <= now + 5);
        }
    }
    /* An idle connection that received pings is still usable. */
    CF_REQUIRE(ct_send_client_frame(run.peer_fd, 0x8, true, false, NULL, 0));
    unsigned opcode = 0;
    bool rsv1 = false;
    unsigned char payload[8];
    CF_CHECK(ct_read_server_frame(run.peer_fd, &opcode, &rsv1, payload,
                                  sizeof payload, 2000) >= 0);
    CF_CHECK(opcode == 0x8);
    ct_run_join(&run);
    CF_CHECK(run.stats.pings_sent >= 2);
    CF_CHECK(run.stats.close_frames_received == 1);
    CF_CHECK(run.stats.close_frames_sent == 1);
    ct_run_finish(&run);
}

CF_TEST_MAIN()
