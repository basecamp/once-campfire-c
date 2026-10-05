/* tests/cable/test_cable_frames.c — C01 framing vectors over socketpairs:
 * the reference socket.rs test vectors (masking, variable lengths,
 * fragmentation, control interleave, UTF-8, size caps, close codes). */
#include "cable_testutil.h"

typedef struct {
    unsigned char *ptr;
    size_t len, cap;
} fs_buf;

static void fs_append(fs_buf *b, const void *data, size_t len) {
    if (b->len + len > b->cap) {
        size_t cap = b->cap == 0 ? 256 : b->cap;
        while (cap < b->len + len) cap *= 2;
        b->ptr = realloc(b->ptr, cap);
        b->cap = cap;
    }
    memcpy(b->ptr + b->len, data, len);
    b->len += len;
}

static void fs_frame(fs_buf *b, unsigned opcode, bool fin, bool rsv1,
                     const unsigned char *payload, size_t len) {
    unsigned char *frame = malloc(len + 16);
    size_t n = ct_client_frame(frame, opcode, fin, rsv1, payload, len);
    fs_append(b, frame, n);
    free(frame);
}

static void fs_free(fs_buf *b) {
    free(b->ptr);
    b->ptr = NULL;
    b->len = b->cap = 0;
}

static ct_capture g_cap;

static void reset_capture(bool authenticated) {
    memset(&g_cap, 0, sizeof g_cap);
    g_cap.authenticated = authenticated;
    g_cap.user_id = 7;
}

static cf_cable_hooks capture_hooks(void) {
    cf_cable_hooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.authenticate = ct_auth_capture;
    hooks.authenticate_user = &g_cap;
    hooks.on_text = ct_on_text_capture;
    hooks.on_text_user = &g_cap;
    return hooks;
}

static void read_welcome_and_close(ct_run *run) {
    char buf[512];
    CF_REQUIRE(ct_read_until(run->peer_fd, buf, sizeof buf, "\"welcome\"",
                             2000) > 0);
}

/* Drive one raw byte sequence and expect the server's close code. With
 * `send_may_fail` the peer is allowed to stop draining -- a deadline or EPIPE
 * from ct_send_all is not a failure, because a server that rejects a frame
 * from its header (the size-cap vectors) stops reading, and how much of the
 * payload the socketpair happens to accept is a transport detail. The close
 * frame is still readable either way. */
static void expect_protocol_close(const char *label, const unsigned char *bytes,
                                  size_t len, bool deflate, uint16_t expected,
                                  bool send_may_fail) {
    reset_capture(true);
    cf_cable_hooks hooks = capture_hooks();
    ct_run run;
    CF_REQUIRE(ct_run_start(&run, &hooks, NULL, deflate));
    read_welcome_and_close(&run);
    if (!ct_send_all(run.peer_fd, bytes, len)) {
        if (!send_may_fail) {
            fprintf(stderr, "    %s: send failed\n", label);
            CF_CHECK(false);
            ct_run_close(&run);
            ct_run_join(&run);
            ct_run_finish(&run);
            return;
        }
    }
    unsigned opcode = 0;
    bool rsv1 = false;
    unsigned char payload[300];
    ssize_t n = ct_read_server_frame(run.peer_fd, &opcode, &rsv1, payload,
                                     sizeof payload, 3000);
    if (n != 2 || opcode != 0x8) {
        fprintf(stderr, "    %s: n=%zd opcode=%u\n", label, n, opcode);
    }
    CF_CHECK(n == 2);
    CF_CHECK(opcode == 0x8);
    if (n == 2) {
        uint16_t code = (uint16_t)((payload[0] << 8) | payload[1]);
        if (code != expected) {
            fprintf(stderr, "    %s: close code %u != %u\n", label, code,
                    expected);
        }
        CF_CHECK(code == expected);
    }
    ct_run_close(&run);
    ct_run_join(&run);
    ct_run_finish(&run);
}

static void expect_protocol_close_labeled(const char *label,
                                          const unsigned char *bytes,
                                          size_t len, bool deflate,
                                          uint16_t expected) {
    expect_protocol_close(label, bytes, len, deflate, expected, false);
}

static void expect_unmasked_raw(const unsigned char *bytes, size_t len,
                                uint16_t expected) {
    reset_capture(true);
    cf_cable_hooks hooks = capture_hooks();
    ct_run run;
    CF_REQUIRE(ct_run_start(&run, &hooks, NULL, false));
    read_welcome_and_close(&run);
    CF_REQUIRE(ct_send_all(run.peer_fd, bytes, len));
    unsigned opcode = 0;
    bool rsv1 = false;
    unsigned char payload[300];
    ssize_t n = ct_read_server_frame(run.peer_fd, &opcode, &rsv1, payload,
                                     sizeof payload, 3000);
    CF_CHECK(n == 2);
    CF_CHECK(opcode == 0x8);
    if (n == 2) {
        CF_CHECK(((payload[0] << 8) | payload[1]) == expected);
    }
    ct_run_close(&run);
    ct_run_join(&run);
    ct_run_finish(&run);
}

CF_TEST(reads_text_fragments_compressed_messages_and_control_frames) {
    reset_capture(true);
    cf_cable_hooks hooks = capture_hooks();
    ct_run run;
    CF_REQUIRE(ct_run_start(&run, &hooks, NULL, true));
    read_welcome_and_close(&run);

    const char *big = "compressed compressed compressed ";
    char long_text[3200];
    size_t pos = 0;
    while (pos + strlen(big) < sizeof long_text - 1) {
        memcpy(long_text + pos, big, strlen(big));
        pos += strlen(big);
    }
    long_text[pos] = '\0';
    unsigned char deflated[4096];
    size_t deflated_len = 0;
    CF_REQUIRE(ct_deflate_raw((const unsigned char *)long_text, pos, deflated,
                              sizeof deflated, &deflated_len));

    fs_buf bytes = {0};
    const char *subscribe = "{\"command\":\"subscribe\"}";
    fs_frame(&bytes, 0x1, true, false,
             (const unsigned char *)subscribe, strlen(subscribe));
    fs_frame(&bytes, 0x1, false, false, (const unsigned char *)"hel", 3);
    fs_frame(&bytes, 0x9, true, false, (const unsigned char *)"p", 1);
    fs_frame(&bytes, 0x0, true, false,
             (const unsigned char *)"lo \xE2\x98\x83", 6);
    fs_frame(&bytes, 0x1, true, true, deflated, deflated_len);
    unsigned char close_payload[16];
    close_payload[0] = 0x03;
    close_payload[1] = 0xE9; /* 1001 */
    memcpy(close_payload + 2, "going away", 10);
    fs_frame(&bytes, 0x8, true, false, close_payload, 12);
    CF_REQUIRE(ct_send_all(run.peer_fd, bytes.ptr, bytes.len));
    fs_free(&bytes);

    unsigned opcode = 0;
    bool rsv1 = false;
    unsigned char payload[512];
    ssize_t n = ct_read_server_frame(run.peer_fd, &opcode, &rsv1, payload,
                                     sizeof payload, 3000);
    CF_REQUIRE(n == 1);
    CF_CHECK(opcode == 0xA); /* pong */
    CF_CHECK(payload[0] == 'p');

    n = ct_read_server_frame(run.peer_fd, &opcode, &rsv1, payload,
                             sizeof payload, 3000);
    CF_REQUIRE(n == 2);
    CF_CHECK(opcode == 0x8);
    CF_CHECK(((payload[0] << 8) | payload[1]) == 1001);

    CF_CHECK(g_cap.text_count == 3);
    if (g_cap.text_count == 3) {
        CF_CHECK(strcmp(g_cap.text[0], subscribe) == 0);
        CF_CHECK(strcmp(g_cap.text[1], "hello \xE2\x98\x83") == 0);
        CF_CHECK(g_cap.text_len[2] == pos);
        /* The capture keeps the first 255 bytes; compare that prefix. */
        CF_CHECK(memcmp(g_cap.text[2], long_text, 255) == 0);
    }
    ct_run_close(&run);
    ct_run_join(&run);
    ct_run_finish(&run);
}

CF_TEST(variable_length_frames_125_4096_70000) {
    reset_capture(true);
    cf_cable_hooks hooks = capture_hooks();
    ct_run run;
    CF_REQUIRE(ct_run_start(&run, &hooks, NULL, false));
    read_welcome_and_close(&run);

    size_t lengths[3] = {125, 4096, 70000};
    fs_buf bytes = {0};
    for (size_t i = 0; i < 3; i++) {
        unsigned char *text = malloc(lengths[i]);
        memset(text, (int)('a' + i), lengths[i]);
        fs_frame(&bytes, 0x1, true, false, text, lengths[i]);
        free(text);
    }
    CF_REQUIRE(ct_send_all(run.peer_fd, bytes.ptr, bytes.len));
    fs_free(&bytes);
    /* Give the server thread time to process, then close. */
    CF_REQUIRE(ct_send_client_frame(run.peer_fd, 0x8, true, false, NULL, 0));
    unsigned opcode = 0;
    bool rsv1 = false;
    unsigned char payload[8];
    CF_REQUIRE(ct_read_server_frame(run.peer_fd, &opcode, &rsv1, payload,
                                    sizeof payload, 3000) >= 0);
    CF_CHECK(opcode == 0x8);
    CF_CHECK(g_cap.text_count == 3);
    if (g_cap.text_count == 3) {
        CF_CHECK(g_cap.text_len[0] == 125);
        CF_CHECK(g_cap.text_len[1] == 4096);
        CF_CHECK(g_cap.text_len[2] == 70000);
        CF_CHECK(g_cap.text[2][0] == 'c');
        CF_CHECK(g_cap.text[2][254] == 'c');
    }
    ct_run_close(&run);
    ct_run_join(&run);
    ct_run_finish(&run);
}

/* The reference rejects each of these with websocket-driver's code. */
CF_TEST(rejects_protocol_errors) {
    unsigned char buf[8192];
    size_t max = CF_CABLE_MAX_MESSAGE;

    /* unmasked */
    {
        unsigned char raw[] = {0x81, 0x02, 'h', 'i'};
        expect_unmasked_raw(raw, sizeof raw, 1003);
    }
    /* unmasked, with a reserved opcode: the opcode check comes first */
    {
        unsigned char raw[] = {0x83, 0x01, 'x'};
        expect_unmasked_raw(raw, sizeof raw, 1002);
    }
    /* compressed, not negotiated */
    {
        unsigned char deflated[64];
        size_t n = 0;
        CF_REQUIRE(ct_deflate_raw((const unsigned char *)"hi", 2, deflated,
                                  sizeof deflated, &n));
        fs_buf b = {0};
        fs_frame(&b, 0x1, true, true, deflated, n);
        expect_protocol_close_labeled("compressed not negotiated", b.ptr, b.len, false, 1002);
        fs_free(&b);
    }
    /* compressed continuation */
    {
        fs_buf b = {0};
        fs_frame(&b, 0x1, false, true, (const unsigned char *)"", 0);
        fs_frame(&b, 0x0, true, true, (const unsigned char *)"", 0);
        expect_protocol_close_labeled("compressed continuation", b.ptr, b.len, true, 1002);
        fs_free(&b);
    }
    /* compressed ping */
    {
        fs_buf b = {0};
        fs_frame(&b, 0x9, true, true, (const unsigned char *)"x", 1);
        expect_protocol_close_labeled("compressed ping", b.ptr, b.len, true, 1002);
        fs_free(&b);
    }
    /* invalid deflate data */
    {
        unsigned char junk[8];
        memset(junk, 0xFF, sizeof junk);
        fs_buf b = {0};
        fs_frame(&b, 0x1, true, true, junk, sizeof junk);
        expect_protocol_close_labeled("invalid deflate data", b.ptr, b.len, true, 1002);
        fs_free(&b);
    }
    /* not UTF-8 */
    {
        unsigned char raw[] = {0xFF, 0xFE};
        fs_buf b = {0};
        fs_frame(&b, 0x1, true, false, raw, sizeof raw);
        expect_protocol_close_labeled("not UTF-8", b.ptr, b.len, false, 1007);
        fs_free(&b);
    }
    /* invalid UTF-8 split across fragments */
    {
        fs_buf b = {0};
        fs_frame(&b, 0x1, false, false, (const unsigned char *)"\xC3", 1);
        fs_frame(&b, 0x0, true, false, (const unsigned char *)"\x28", 1);
        expect_protocol_close_labeled("invalid UTF-8 split", b.ptr, b.len, false, 1007);
        fs_free(&b);
    }
    /* nothing to continue */
    {
        fs_buf b = {0};
        fs_frame(&b, 0x0, true, false, (const unsigned char *)"x", 1);
        expect_protocol_close_labeled("nothing to continue", b.ptr, b.len, false, 1002);
        fs_free(&b);
    }
    /* a new message mid-message */
    {
        fs_buf b = {0};
        fs_frame(&b, 0x1, false, false, (const unsigned char *)"x", 1);
        fs_frame(&b, 0x1, true, false, (const unsigned char *)"y", 1);
        expect_protocol_close_labeled("new message mid-message", b.ptr, b.len, false, 1002);
        fs_free(&b);
    }
    /* fragmented control frame */
    {
        fs_buf b = {0};
        fs_frame(&b, 0x9, false, false, (const unsigned char *)"x", 1);
        expect_protocol_close_labeled("fragmented control", b.ptr, b.len, false, 1002);
        fs_free(&b);
    }
    /* long control frame */
    {
        unsigned char payload[126];
        memset(payload, 0, sizeof payload);
        fs_buf b = {0};
        fs_frame(&b, 0x9, true, false, payload, sizeof payload);
        expect_protocol_close_labeled("long control", b.ptr, b.len, false, 1002);
        fs_free(&b);
    }
    /* reserved opcode */
    {
        fs_buf b = {0};
        fs_frame(&b, 0x3, true, false, (const unsigned char *)"x", 1);
        expect_protocol_close_labeled("reserved opcode", b.ptr, b.len, false, 1002);
        fs_free(&b);
    }
    /* reserved RSV2/RSV3 */
    {
        unsigned char raw[] = {0x91, 0x81, 0x12, 0x34, 0x56, 0x78, 'x' ^ 0x12};
        expect_unmasked_raw(raw, sizeof raw, 1002);
    }
    /* too large (full frame) */
    {
        fs_buf b = {0};
        unsigned char *text = malloc(max + 1);
        memset(text, 'a', max + 1);
        fs_frame(&b, 0x1, true, false, text, max + 1);
        free(text);
        expect_protocol_close("too large full", b.ptr, b.len, false, 1009, true);
        fs_free(&b);
    }
    /* too large in fragments: header only, refused before its payload */
    {
        size_t half = max / 2 + 1;
        fs_buf b = {0};
        unsigned char *text = malloc(half);
        memset(text, 'a', half);
        fs_frame(&b, 0x1, false, false, text, half);
        free(text);
        unsigned char head[14];
        /* continuation, fin, masked, 64-bit length = half, plus mask */
        head[0] = 0x80;
        head[1] = 0x80 | 127;
        for (int i = 0; i < 8; i++) {
            head[2 + i] = (unsigned char)((uint64_t)half >> (56 - 8 * i));
        }
        memcpy(head + 10, CT_MASK, 4);
        fs_append(&b, head, sizeof head);
        expect_protocol_close_labeled("too large fragments", b.ptr, b.len, false, 1009);
        fs_free(&b);
    }
    /* too large once inflated */
    {
        unsigned char *text = malloc(max + 1);
        memset(text, 'a', max + 1);
        unsigned char *deflated = malloc(max + 1);
        size_t deflated_len = 0;
        CF_REQUIRE(ct_deflate_raw(text, max + 1, deflated, max + 1,
                                  &deflated_len));
        fs_buf b = {0};
        fs_frame(&b, 0x1, true, true, deflated, deflated_len);
        free(text);
        free(deflated);
        expect_protocol_close_labeled("too large inflated", b.ptr, b.len, true, 1009);
        fs_free(&b);
    }
    /* one-byte close */
    {
        unsigned char payload[] = {0x03};
        fs_buf b = {0};
        fs_frame(&b, 0x8, true, false, payload, 1);
        expect_protocol_close_labeled("one-byte close", b.ptr, b.len, false, 1002);
        fs_free(&b);
    }
    /* reserved close code 1005 */
    {
        unsigned char payload[] = {0x03, 0xED};
        fs_buf b = {0};
        fs_frame(&b, 0x8, true, false, payload, 2);
        expect_protocol_close_labeled("reserved close code", b.ptr, b.len, false, 1002);
        fs_free(&b);
    }
    /* close reason not UTF-8 */
    {
        unsigned char payload[] = {0x03, 0xE8, 0xFF};
        fs_buf b = {0};
        fs_frame(&b, 0x8, true, false, payload, sizeof payload);
        expect_protocol_close_labeled("close reason not UTF-8", b.ptr, b.len, false, 1002);
        fs_free(&b);
    }
    (void)buf;
}

/* An orphan continuation is rejected where the reference rejects it: after
 * its payload has been read (socket.rs Reader::next), so the header's mask
 * and length checks decide the close code first. */
CF_TEST(orphan_continuation_close_code_order) {
    /* Unmasked: head[1] & 0x80 -> 1003 (not the orphan 1002). */
    {
        unsigned char raw[] = {0x80, 0x01, 'x'};
        expect_unmasked_raw(raw, sizeof raw, 1003);
    }
    /* Over-cap announced length: the MAX_MESSAGE check -> 1009. The mask bit
     * is set but the reference fails at the length check before reading the
     * mask, so only the 2-byte head and the 8-byte length go on the wire. */
    {
        unsigned char head[10];
        head[0] = 0x80; /* fin, continuation */
        head[1] = 0x80 | 127;
        memset(head + 2, 0xFF, 8);
        expect_unmasked_raw(head, sizeof head, 1009);
    }
    /* Masked and within limits: the payload is read and dropped, then 1002. */
    {
        fs_buf b = {0};
        fs_frame(&b, 0x0, true, false, (const unsigned char *)"x", 1);
        expect_protocol_close_labeled("orphan continuation", b.ptr, b.len,
                                      false, 1002);
        fs_free(&b);
    }
}

CF_TEST(one_mebibyte_message_is_accepted_whole) {
    reset_capture(true);
    cf_cable_hooks hooks = capture_hooks();
    ct_run run;
    CF_REQUIRE(ct_run_start(&run, &hooks, NULL, false));
    read_welcome_and_close(&run);
    size_t max = CF_CABLE_MAX_MESSAGE;
    unsigned char *text = malloc(max);
    CF_REQUIRE(text != NULL);
    memset(text, 'z', max);
    CF_REQUIRE(ct_send_client_frame(run.peer_fd, 0x1, true, false, text, max));
    free(text);
    /* Processed in order; follow with a close so the run finishes. */
    CF_REQUIRE(ct_send_client_frame(run.peer_fd, 0x8, true, false, NULL, 0));
    unsigned opcode = 0;
    bool rsv1 = false;
    unsigned char payload[8];
    CF_CHECK(ct_read_server_frame(run.peer_fd, &opcode, &rsv1, payload,
                                  sizeof payload, 5000) >= 0);
    CF_CHECK(opcode == 0x8);
    CF_CHECK(g_cap.text_count == 1);
    if (g_cap.text_count == 1) CF_CHECK(g_cap.text_len[0] == max);
    ct_run_close(&run);
    ct_run_join(&run);
    ct_run_finish(&run);
}

CF_TEST_MAIN()
