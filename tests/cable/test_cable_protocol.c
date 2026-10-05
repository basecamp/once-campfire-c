/* tests/cable/test_cable_protocol.c — C01 protocol.c vectors against
 * tmp/rust-ref/crates/cable/src/protocol.rs (ActionCable::INTERNAL). */
#include "cable/cable.h"
#include "cf_test.h"

#include <string.h>

static void check_frame(cf_err rc, const cf_str *frame, const char *expected) {
    CF_REQUIRE(rc == CF_OK);
    CF_REQUIRE(frame->ptr != NULL);
    CF_CHECK(frame->len == strlen(expected));
    CF_CHECK(memcmp(frame->ptr, expected, frame->len) == 0);
}

CF_TEST(welcome_ping_and_disconnect_frames) {
    cf_str frame = {0};

    CF_REQUIRE(cf_cable_welcome(&frame) == CF_OK);
    check_frame(CF_OK, &frame, "{\"type\":\"welcome\"}");
    cf_str_dispose(&frame);

    CF_REQUIRE(cf_cable_ping(1759640000, &frame) == CF_OK);
    check_frame(CF_OK, &frame, "{\"type\":\"ping\",\"message\":1759640000}");
    cf_str_dispose(&frame);

    CF_REQUIRE(cf_cable_ping(-1, &frame) == CF_OK);
    check_frame(CF_OK, &frame, "{\"type\":\"ping\",\"message\":-1}");
    cf_str_dispose(&frame);

    /* Connection::Base#close without a reason. */
    CF_REQUIRE(cf_cable_disconnect_frame(CF_CABLE_REASON_NONE, (cf_span){NULL, 0},
                                         &frame) == CF_OK);
    check_frame(CF_OK, &frame,
                "{\"type\":\"disconnect\",\"reason\":null,\"reconnect\":true}");
    cf_str_dispose(&frame);

    /* respond_to_invalid_request: unauthorized, no reconnect. */
    CF_REQUIRE(cf_cable_disconnect_frame(CF_CABLE_REASON_UNAUTHORIZED,
                                         (cf_span){(const unsigned char *)"false", 5},
                                         &frame) == CF_OK);
    check_frame(CF_OK, &frame,
                "{\"type\":\"disconnect\",\"reason\":\"unauthorized\","
                "\"reconnect\":false}");
    cf_str_dispose(&frame);

    /* A remote disconnect carries the raw reconnect value. */
    CF_REQUIRE(cf_cable_disconnect_frame(
                   CF_CABLE_REASON_REMOTE,
                   (cf_span){(const unsigned char *)"{\"x\":1}", 7},
                   &frame) == CF_OK);
    check_frame(CF_OK, &frame,
                "{\"type\":\"disconnect\",\"reason\":\"remote\","
                "\"reconnect\":{\"x\":1}}");
    cf_str_dispose(&frame);

    CF_REQUIRE(cf_cable_disconnect_frame(CF_CABLE_REASON_SERVER_RESTART,
                                         (cf_span){NULL, 0}, &frame) == CF_OK);
    check_frame(CF_OK, &frame,
                "{\"type\":\"disconnect\",\"reason\":\"server_restart\","
                "\"reconnect\":true}");
    cf_str_dispose(&frame);

    CF_CHECK(strcmp(cf_cable_reason_name(CF_CABLE_REASON_INVALID_REQUEST),
                    "invalid_request") == 0);
    CF_CHECK(strcmp(cf_cable_reason_name(CF_CABLE_REASON_REMOTE), "remote") ==
             0);
}

CF_TEST(confirmation_rejection_and_message_frames) {
    cf_str frame = {0};
    const char *identifier = "{\"channel\":\"RoomChannel\"}";
    cf_span id = {(const unsigned char *)identifier, strlen(identifier)};

    CF_REQUIRE(cf_cable_confirm_subscription(id, &frame) == CF_OK);
    check_frame(CF_OK, &frame,
                "{\"identifier\":\"{\\\"channel\\\":\\\"RoomChannel\\\"}\","
                "\"type\":\"confirm_subscription\"}");
    cf_str_dispose(&frame);

    CF_REQUIRE(cf_cable_reject_subscription(id, &frame) == CF_OK);
    check_frame(CF_OK, &frame,
                "{\"identifier\":\"{\\\"channel\\\":\\\"RoomChannel\\\"}\","
                "\"type\":\"reject_subscription\"}");
    cf_str_dispose(&frame);

    /* The identifier is ActiveSupport::JSON encoded: <, > and & escape. */
    const char *odd = "a<b>c&d";
    CF_REQUIRE(cf_cable_confirm_subscription(
                   (cf_span){(const unsigned char *)odd, strlen(odd)},
                   &frame) == CF_OK);
    check_frame(CF_OK, &frame,
                "{\"identifier\":\"a\\u003cb\\u003ec\\u0026d\","
                "\"type\":\"confirm_subscription\"}");
    cf_str_dispose(&frame);

    /* An empty identifier still encodes as a JSON string. */
    CF_REQUIRE(cf_cable_confirm_subscription((cf_span){NULL, 0}, &frame) ==
               CF_OK);
    check_frame(CF_OK, &frame,
                "{\"identifier\":\"\",\"type\":\"confirm_subscription\"}");
    cf_str_dispose(&frame);

    /* The message frame inserts both already-encoded JSON parts verbatim. */
    CF_REQUIRE(cf_cable_message_frame(
                   id,
                   (cf_span){(const unsigned char *)"{\"roomId\":1}", 12},
                   &frame) == CF_OK);
    check_frame(CF_OK, &frame,
                "{\"identifier\":{\"channel\":\"RoomChannel\"},"
                "\"message\":{\"roomId\":1}}");
    cf_str_dispose(&frame);

    const char *escaped = "\"\\u003cb\\u003e\\u0026\\u003c/b\\u003e\"";
    CF_REQUIRE(cf_cable_message_frame(
                   id,
                   (cf_span){(const unsigned char *)escaped,
                             strlen(escaped)},
                   &frame) == CF_OK);
    check_frame(CF_OK, &frame,
                "{\"identifier\":{\"channel\":\"RoomChannel\"},"
                "\"message\":\"\\u003cb\\u003e\\u0026\\u003c/b\\u003e\"}");
    cf_str_dispose(&frame);
}

CF_TEST(protocol_constants_match_the_reference) {
    CF_CHECK(strcmp(CF_CABLE_SUBPROTOCOL_V1, "actioncable-v1-json") == 0);
    CF_CHECK(strcmp(CF_CABLE_SUBPROTOCOL_UNSUPPORTED,
                    "actioncable-unsupported") == 0);
    CF_CHECK(strcmp(CF_CABLE_DEFAULT_MOUNT, "/cable") == 0);
    CF_CHECK(CF_CABLE_MAX_MESSAGE == ((size_t)1 << 20));
    CF_CHECK(CF_CABLE_MIN_COMPRESSED == 256);
    CF_CHECK(CF_CABLE_MAX_PENDING_BYTES == ((size_t)4 << 20));
    CF_CHECK(CF_CABLE_WRITE_STALL_MS == 30000);
    CF_CHECK(CF_CABLE_CLOSE_TIMEOUT_MS == 5000);
    CF_CHECK(CF_CABLE_BEAT_INTERVAL_MS == 3000);
    CF_CHECK(strcmp(CF_CABLE_DEFLATE_RESPONSE,
                    "permessage-deflate; server_no_context_takeover; "
                    "client_no_context_takeover") == 0);
    CF_CHECK(CF_CABLE_PROTOCOL_ERROR == 1002);
    CF_CHECK(CF_CABLE_UNACCEPTABLE == 1003);
    CF_CHECK(CF_CABLE_ENCODING_ERROR == 1007);
    CF_CHECK(CF_CABLE_TOO_LARGE == 1009);
}

CF_TEST_MAIN()
