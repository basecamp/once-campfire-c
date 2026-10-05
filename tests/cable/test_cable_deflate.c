/* tests/cable/test_cable_deflate.c — C01 permessage-deflate vectors:
 * RFC 7692 raw-deflate framing, no context takeover, compressed and
 * uncompressed variants kept separate (socket.rs deflate / Frame::deflated). */
#include "cable_testutil.h"

static const char *g_send_texts[4];
static size_t g_send_lens[4];
static int g_send_count;

static cf_err send_on_text(void *user, cf_cable_socket *socket,
                           cf_span text) {
    (void)user;
    (void)text;
    for (int i = 0; i < g_send_count; i++) {
        cf_err rc = cf_cable_socket_send_text(
            socket, (cf_span){(const unsigned char *)g_send_texts[i],
                              g_send_lens[i]});
        if (rc != CF_OK) return rc;
    }
    return CF_OK;
}

static cf_err auth_ok(void *user, const cf_cable_request *request,
                      bool *authenticated, int64_t *user_id) {
    (void)user;
    (void)request;
    *authenticated = true;
    *user_id = 1;
    return CF_OK;
}

static cf_cable_hooks hooks_with(cf_cable_on_text_fn on_text) {
    cf_cable_hooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.authenticate = auth_ok;
    hooks.on_text = on_text;
    return hooks;
}

static void read_welcome(ct_run *run) {
    char buf[512];
    CF_REQUIRE(ct_read_until(run->peer_fd, buf, sizeof buf, "\"welcome\"",
                             2000) > 0);
}

/* A client text message: raw-deflate it as RFC 7692 framing does. */
static void send_compressed_text(ct_run *run, const char *text, size_t len) {
    unsigned char *deflated = malloc(len + 64);
    size_t deflated_len = 0;
    CF_REQUIRE(ct_deflate_raw((const unsigned char *)text, len, deflated,
                              len + 64, &deflated_len));
    CF_REQUIRE(ct_send_client_frame(run->peer_fd, 0x1, true, true, deflated,
                                    deflated_len));
    free(deflated);
}

CF_TEST(inflates_client_messages_with_rfc7692_framing) {
    static ct_capture cap;
    memset(&cap, 0, sizeof cap);
    cf_cable_hooks hooks = hooks_with(ct_on_text_capture);
    hooks.on_text_user = &cap;
    ct_run run;
    CF_REQUIRE(ct_run_start(&run, &hooks, NULL, true));
    read_welcome(&run);

    char small[64];
    memset(small, 'a', 40);
    small[40] = '\0';
    send_compressed_text(&run, small, 40);

    /* 300 bytes so the wire stream really is deflated. */
    char big[512];
    for (int i = 0; i < 300; i++) big[i] = (char)('a' + (i % 26));
    big[300] = '\0';
    send_compressed_text(&run, big, 300);

    /* Close to finish, then check both messages arrived intact. */
    CF_REQUIRE(ct_send_client_frame(run.peer_fd, 0x8, true, false, NULL, 0));
    unsigned opcode = 0;
    bool rsv1 = false;
    unsigned char payload[8];
    CF_CHECK(ct_read_server_frame(run.peer_fd, &opcode, &rsv1, payload,
                                  sizeof payload, 3000) >= 0);
    CF_CHECK(opcode == 0x8);
    CF_CHECK(cap.text_count == 2);
    if (cap.text_count == 2) {
        CF_CHECK(strcmp(cap.text[0], small) == 0);
        CF_CHECK(cap.text_len[1] == 300);
        CF_CHECK(memcmp(cap.text[1], big, 255) == 0);
    }
    ct_run_close(&run);
    ct_run_join(&run);
    ct_run_finish(&run);
}

CF_TEST(server_compresses_only_when_worth_it) {
    static const char small_text[] = "{\"type\":\"ping\",\"message\":1}";
    char big_text[700];
    memset(big_text, 'x', sizeof big_text);
    big_text[sizeof big_text - 1] = '\0';
    g_send_texts[0] = small_text;
    g_send_lens[0] = sizeof small_text - 1;
    g_send_texts[1] = big_text;
    g_send_lens[1] = sizeof big_text - 1;
    g_send_count = 2;

    for (int deflate = 0; deflate <= 1; deflate++) {
        cf_cable_hooks hooks = hooks_with(send_on_text);
        ct_run run;
        CF_REQUIRE(ct_run_start(&run, &hooks, NULL, deflate != 0));
        read_welcome(&run);
        CF_REQUIRE(ct_send_client_frame(run.peer_fd, 0x1, true, false,
                                        (const unsigned char *)"go", 2));

        unsigned opcode = 0;
        bool rsv1 = false;
        unsigned char payload[4096];
        ssize_t n = ct_read_server_frame(run.peer_fd, &opcode, &rsv1, payload,
                                         sizeof payload, 3000);
        CF_REQUIRE(n == (ssize_t)(sizeof small_text - 1));
        CF_CHECK(opcode == 0x1 || opcode == 0x2);
        CF_CHECK(rsv1 == false);
        CF_CHECK(memcmp(payload, small_text, sizeof small_text - 1) == 0);

        n = ct_read_server_frame(run.peer_fd, &opcode, &rsv1, payload,
                                 sizeof payload, 3000);
        CF_REQUIRE(n >= 0);
        if (deflate != 0) {
            CF_CHECK(rsv1 == true);
            unsigned char inflated[2048];
            size_t inflated_len = 0;
            CF_REQUIRE(ct_inflate_raw(payload, (size_t)n, inflated,
                                      sizeof inflated, &inflated_len));
            CF_CHECK(inflated_len == sizeof big_text - 1);
            CF_CHECK(memcmp(inflated, big_text, sizeof big_text - 1) == 0);
        } else {
            CF_CHECK(rsv1 == false);
            CF_CHECK((size_t)n == sizeof big_text - 1);
            CF_CHECK(memcmp(payload, big_text, sizeof big_text - 1) == 0);
        }
        ct_run_close(&run);
        ct_run_join(&run);
        ct_run_finish(&run);
    }
}

/* Each message is a complete deflate stream: there is no context takeover,
 * and a frame's compressed/uncompressed variants stay separate per socket. */
static cf_cable_frame *g_frames[2];
static int g_frame_count;

static cf_err send_frames_on_text(void *user, cf_cable_socket *socket,
                                  cf_span text) {
    (void)user;
    (void)text;
    for (int i = 0; i < g_frame_count; i++) {
        cf_err rc = cf_cable_socket_send_frame(socket, g_frames[i]);
        if (rc != CF_OK) return rc;
    }
    return CF_OK;
}

CF_TEST(no_context_takeover_and_variants_kept_separate) {
    char first[600];
    char second[600];
    for (size_t i = 0; i < sizeof first; i++) {
        first[i] = (char)('a' + (i % 4));
        second[i] = (char)('q' + (i % 3));
    }
    first[sizeof first - 1] = '\0';
    second[sizeof second - 1] = '\0';
    const size_t first_len = sizeof first - 1;
    const size_t second_len = sizeof second - 1;

    CF_REQUIRE(cf_cable_frame_create(
                   (cf_span){(const unsigned char *)first, first_len},
                   &g_frames[0]) == CF_OK);
    CF_REQUIRE(cf_cable_frame_create(
                   (cf_span){(const unsigned char *)second, second_len},
                   &g_frames[1]) == CF_OK);
    g_frame_count = 2;

    cf_cable_hooks hooks = hooks_with(send_frames_on_text);
    ct_run run;
    CF_REQUIRE(ct_run_start(&run, &hooks, NULL, true));
    read_welcome(&run);
    CF_REQUIRE(ct_send_client_frame(run.peer_fd, 0x1, true, false,
                                    (const unsigned char *)"go", 2));

    for (int i = 0; i < 2; i++) {
        unsigned opcode = 0;
        bool rsv1 = false;
        unsigned char payload[4096];
        ssize_t n = ct_read_server_frame(run.peer_fd, &opcode, &rsv1, payload,
                                         sizeof payload, 3000);
        CF_REQUIRE(n > 0);
        CF_CHECK(rsv1 == true);
        /* Inflating each frame alone succeeds only without context
         * takeover. */
        static unsigned char inflated[2048];
        size_t inflated_len = 0;
        CF_REQUIRE(ct_inflate_raw(payload, (size_t)n, inflated,
                                  sizeof inflated, &inflated_len));
        const char *expect = i == 0 ? first : second;
        size_t expect_len = i == 0 ? first_len : second_len;
        CF_CHECK(inflated_len == expect_len);
        CF_CHECK(memcmp(inflated, expect, expect_len) == 0);
    }

    ct_run_close(&run);
    ct_run_join(&run);
    ct_run_finish(&run);

    /* The same frame on a socket without negotiation stays uncompressed. */
    ct_run plain;
    cf_cable_hooks plain_hooks = hooks_with(send_frames_on_text);
    g_frame_count = 1;
    CF_REQUIRE(ct_run_start(&plain, &plain_hooks, NULL, false));
    read_welcome(&plain);
    CF_REQUIRE(ct_send_client_frame(plain.peer_fd, 0x1, true, false,
                                    (const unsigned char *)"go", 2));
    unsigned opcode = 0;
    bool rsv1 = false;
    unsigned char payload[1024];
    ssize_t n = ct_read_server_frame(plain.peer_fd, &opcode, &rsv1, payload,
                                     sizeof payload, 3000);
    CF_CHECK(n == (ssize_t)first_len);
    CF_CHECK(rsv1 == false);
    if (n == (ssize_t)first_len) {
        CF_CHECK(memcmp(payload, first, first_len) == 0);
    }
    ct_run_close(&plain);
    ct_run_join(&plain);
    ct_run_finish(&plain);

    cf_cable_frame_release(g_frames[0]);
    cf_cable_frame_release(g_frames[1]);
    g_frames[0] = g_frames[1] = NULL;
}

CF_TEST_MAIN()
