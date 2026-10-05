/* tests/cable/test_cable_channels.c — C02 per-channel behavior over real C01
 * WebSocket connections: the eight registered channel names, subscribe
 * validation/rejection, identifier echo, action payloads and the exact
 * delivery frames. Mirrors the reference's channels_test.rs over a scratch
 * fixtures database. */
#include "cable_channels_testutil.h"

#include "models/message.h"
#include "models/room.h"

#include <sqlite3.h>

/* ---- helpers --------------------------------------------------------------- */

typedef struct {
    bool connected;
    int64_t connections;
    bool unread;
} chan_membership_state;

static chan_membership_state chan_membership_state_of(cf_db *db,
                                                      int64_t room_id,
                                                      int64_t user_id) {
    chan_membership_state state = {false, 0, false};
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(cf_db_handle(db),
                           "SELECT connections, connected_at, unread_at "
                           "FROM memberships WHERE room_id=?1 AND user_id=?2",
                           -1, &stmt, NULL) != SQLITE_OK) {
        return state;
    }
    sqlite3_bind_int64(stmt, 1, room_id);
    sqlite3_bind_int64(stmt, 2, user_id);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        state.connections = sqlite3_column_int64(stmt, 0);
        state.connected = sqlite3_column_type(stmt, 1) != SQLITE_NULL;
        state.unread = sqlite3_column_type(stmt, 2) != SQLITE_NULL;
    }
    sqlite3_finalize(stmt);
    return state;
}

/* Poll the membership until `predicate` holds (the server applies the
 * unsubscribe effect asynchronously on the loop's owner thread). */
static bool chan_wait_membership(cf_db *db, int64_t room_id, int64_t user_id,
                                 bool want_connected, int64_t want_connections,
                                 int timeout_ms) {
    int64_t deadline = ct_now_ms() + timeout_ms;
    for (;;) {
        chan_membership_state state =
            chan_membership_state_of(db, room_id, user_id);
        if (state.connected == want_connected &&
            state.connections == want_connections) {
            return true;
        }
        if (ct_now_ms() >= deadline) return false;
        usleep(20000);
    }
}

static cf_room chan_find_room(chan_fixture *f, int64_t room_id) {
    cf_room room = {0};
    bool found = false;
    CF_REQUIRE(cf_room_find_by_id(f->scratch.db, room_id, &found, &room) ==
               CF_OK);
    CF_REQUIRE(found);
    return room;
}

static cf_message chan_find_message(chan_fixture *f, int64_t message_id) {
    cf_message message = {0};
    CF_REQUIRE(cf_message_find(f->scratch.db, message_id, &message) == CF_OK);
    return message;
}

static bool chan_publish_payload(cf_cable *cable, const char *stream,
                                 const char *payload) {
    cf_buf *buf = NULL;
    if (cf_buf_copy((cf_span){(const unsigned char *)payload,
                              strlen(payload)},
                    &buf) != CF_OK) {
        return false;
    }
    cf_err rc = cf_cable_publish(
        cable, (cf_span){(const unsigned char *)stream, strlen(stream)}, buf);
    cf_buf_release(buf);
    return rc == CF_OK;
}

/* A room-messages identifier with a signed stream name. */
static void chan_room_messages_identifier(chan_fixture *f, int64_t room_id,
                                          char *out, size_t cap) {
    cf_room room = chan_find_room(f, room_id);
    cf_str gid = {0};
    CF_REQUIRE(cf_cable_room_gid_param(&room, &gid) == CF_OK);
    cf_span parts[2] = {
        {(const unsigned char *)gid.ptr, gid.len},
        {(const unsigned char *)"messages", 8},
    };
    char signed_name[512];
    chan_signed_stream(f, parts, 2, signed_name, sizeof signed_name);
    snprintf(out, cap,
             "{\"channel\":\"RoomMessagesChannel\","
             "\"signed_stream_name\":\"%s\"}",
             signed_name);
    cf_str_dispose(&gid);
    cf_room_dispose(&room);
}

/* ---- ApplicationCable::Channel and HeartbeatChannel ------------------------- */

CF_TEST(heartbeat_and_base_channels_confirm) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    chan_conn c;
    CF_REQUIRE(chan_connect(&f, CHAN_JZ, &c));

    char id[128];
    chan_channel_identifier(id, sizeof id, "HeartbeatChannel");
    CF_REQUIRE(chan_subscribe(&c, id));
    CF_CHECK(chan_confirm(&c, id));

    chan_channel_identifier(id, sizeof id, "ApplicationCable::Channel");
    CF_REQUIRE(chan_subscribe(&c, id));
    CF_CHECK(chan_confirm(&c, id));

    /* EmptyChannel performs nothing. */
    CF_REQUIRE(chan_perform(&c, id, "{\"action\":\"start\"}"));
    CF_CHECK(chan_silent(&c, 300));

    chan_disconnect(&c);
    chan_fixture_close(&f);
}

/* ---- RoomChannel ------------------------------------------------------------ */

CF_TEST(room_channel_streams_for_member_rooms_only) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    chan_conn c;
    CF_REQUIRE(chan_connect(&f, CHAN_KEVIN, &c));

    char member[128];
    chan_room_identifier(member, sizeof member, "RoomChannel", CHAN_DESIGNERS);
    CF_REQUIRE(chan_subscribe(&c, member));
    CF_CHECK(chan_confirm(&c, member));

    /* Not a member, unknown room, missing room_id. */
    char other[128];
    chan_room_identifier(other, sizeof other, "RoomChannel", CHAN_WATERCOOLER);
    CF_REQUIRE(chan_subscribe(&c, other));
    CF_CHECK(chan_reject(&c, other));
    chan_room_identifier(other, sizeof other, "RoomChannel", -1);
    CF_REQUIRE(chan_subscribe(&c, other));
    CF_CHECK(chan_reject(&c, other));
    snprintf(other, sizeof other, "{\"channel\":\"RoomChannel\"}");
    CF_REQUIRE(chan_subscribe(&c, other));
    CF_CHECK(chan_reject(&c, other));

    /* Params are cast like Active Record casts an id. */
    char by_string[128];
    snprintf(by_string, sizeof by_string,
             "{\"channel\":\"RoomChannel\",\"room_id\":\"%d\"}",
             CHAN_DESIGNERS);
    CF_REQUIRE(chan_subscribe(&c, by_string));
    CF_CHECK(chan_confirm(&c, by_string));

    /* Both subscriptions read the room's stream ("room:<gid param>"). */
    char stream[256];
    chan_room_stream(&f, "RoomChannel", CHAN_DESIGNERS, stream,
                     sizeof stream);
    CF_REQUIRE(chan_publish_payload(f.cable, stream, "{\"hello\":1}"));

    char expected_member[10240];
    char expected_string[10240];
    chan_delivery_expected(expected_member, sizeof expected_member, member,
                           "{\"hello\":1}");
    chan_delivery_expected(expected_string, sizeof expected_string, by_string,
                           "{\"hello\":1}");
    char got[10240];
    CF_REQUIRE(chan_next(&c, got, sizeof got, 3000));
    if (strcmp(got, expected_member) == 0) {
        CF_CHECK(chan_expect(&c, expected_string, 3000));
    } else {
        CF_CHECK(strcmp(got, expected_string) == 0);
        CF_CHECK(chan_expect(&c, expected_member, 3000));
    }
    CF_CHECK(chan_silent(&c, 300));

    chan_disconnect(&c);
    chan_fixture_close(&f);
}

/* ---- PresenceChannel --------------------------------------------------------- */

CF_TEST(presence_marks_membership_and_reads) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    chan_conn c;
    CF_REQUIRE(chan_connect(&f, CHAN_DAVID, &c));

    char reads[64];
    chan_channel_identifier(reads, sizeof reads, "ReadRoomsChannel");
    CF_REQUIRE(chan_subscribe(&c, reads));
    CF_CHECK(chan_confirm(&c, reads));

    /* The member's unread marker is cleared by `present`. */
    CF_REQUIRE(chan_exec(f.scratch.db,
                         "UPDATE memberships SET unread_at='2024-01-01 "
                         "00:00:00' WHERE id=22"));
    chan_membership_state before =
        chan_membership_state_of(f.scratch.db, CHAN_DESIGNERS, CHAN_DAVID);
    CF_REQUIRE(before.unread);

    char presence[128];
    chan_room_identifier(presence, sizeof presence, "PresenceChannel",
                         CHAN_DESIGNERS);
    CF_REQUIRE(chan_subscribe(&c, presence));
    CF_CHECK(chan_confirm(&c, presence));
    /* The confirmation precedes the read notification it caused. */
    char expected[10240];
    chan_delivery_expected(expected, sizeof expected, reads, "{\"room_id\":10}");
    CF_CHECK(chan_expect(&c, expected, 3000));

    chan_membership_state after =
        chan_membership_state_of(f.scratch.db, CHAN_DESIGNERS, CHAN_DAVID);
    CF_CHECK(after.connected);
    CF_CHECK(after.connections == 1);
    CF_CHECK(!after.unread);

    /* Unsubscribing marks the membership disconnected. */
    CF_REQUIRE(chan_unsubscribe(&c, presence));
    CF_CHECK(chan_wait_membership(f.scratch.db, CHAN_DESIGNERS, CHAN_DAVID,
                                  false, 0, 3000));
    CF_CHECK(chan_silent(&c, 300));

    chan_disconnect(&c);
    chan_fixture_close(&f);
}

CF_TEST(presence_counts_connections_and_refreshes) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    chan_conn first, second;
    CF_REQUIRE(chan_connect(&f, CHAN_DAVID, &first));
    CF_REQUIRE(chan_connect(&f, CHAN_DAVID, &second));

    char presence[128];
    chan_room_identifier(presence, sizeof presence, "PresenceChannel",
                         CHAN_DESIGNERS);
    CF_REQUIRE(chan_subscribe(&first, presence));
    CF_CHECK(chan_confirm(&first, presence));
    CF_REQUIRE(chan_subscribe(&second, presence));
    CF_CHECK(chan_confirm(&second, presence));
    chan_membership_state state =
        chan_membership_state_of(f.scratch.db, CHAN_DESIGNERS, CHAN_DAVID);
    CF_CHECK(state.connections == 2);

    /* `refresh` on a live connection is a no-op. */
    CF_REQUIRE(chan_perform(&first, presence, "{\"action\":\"refresh\"}"));
    CF_CHECK(chan_silent(&first, 200));
    CF_CHECK(chan_wait_membership(f.scratch.db, CHAN_DESIGNERS, CHAN_DAVID,
                                  true, 2, 1000));

    /* Each connection leaving decrements the counter; the last clears the
     * connected state (Membership::Connectable). */
    CF_REQUIRE(chan_unsubscribe(&second, presence));
    CF_CHECK(chan_wait_membership(f.scratch.db, CHAN_DESIGNERS, CHAN_DAVID,
                                  true, 1, 3000));
    CF_REQUIRE(chan_unsubscribe(&first, presence));
    CF_CHECK(chan_wait_membership(f.scratch.db, CHAN_DESIGNERS, CHAN_DAVID,
                                  false, 0, 3000));

    chan_disconnect(&first);
    chan_disconnect(&second);
    chan_fixture_close(&f);
}

CF_TEST(presence_rejects_rooms_the_user_is_not_in) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    chan_conn c;
    CF_REQUIRE(chan_connect(&f, CHAN_KEVIN, &c));

    char presence[128];
    chan_room_identifier(presence, sizeof presence, "PresenceChannel",
                         CHAN_WATERCOOLER);
    CF_REQUIRE(chan_subscribe(&c, presence));
    CF_CHECK(chan_reject(&c, presence));
    chan_room_identifier(presence, sizeof presence, "PresenceChannel", -1);
    CF_REQUIRE(chan_subscribe(&c, presence));
    CF_CHECK(chan_reject(&c, presence));
    /* A rejected subscription must not touch another user's membership. */
    chan_membership_state state =
        chan_membership_state_of(f.scratch.db, CHAN_DESIGNERS, CHAN_JZ);
    CF_CHECK(!state.connected);
    CF_CHECK(chan_silent(&c, 300));

    chan_disconnect(&c);
    chan_fixture_close(&f);
}

/* ---- TypingNotificationsChannel ---------------------------------------------- */

CF_TEST(typing_notifications_start_and_stop) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    chan_conn typist, reader;
    CF_REQUIRE(chan_connect(&f, CHAN_JZ, &typist));
    CF_REQUIRE(chan_connect(&f, CHAN_KEVIN, &reader));

    char typing[128];
    chan_room_identifier(typing, sizeof typing, "TypingNotificationsChannel",
                         CHAN_DESIGNERS);
    CF_REQUIRE(chan_subscribe(&typist, typing));
    CF_CHECK(chan_confirm(&typist, typing));
    CF_REQUIRE(chan_subscribe(&reader, typing));
    CF_CHECK(chan_confirm(&reader, typing));

    CF_REQUIRE(chan_perform(&typist, typing, "{\"action\":\"start\"}"));
    char expected[10240];
    chan_delivery_expected(expected, sizeof expected, typing,
                           "{\"action\":\"start\",\"user\":{\"id\":1,"
                           "\"name\":\"JZ\"}}");
    CF_CHECK(chan_expect(&reader, expected, 3000));
    CF_CHECK(chan_expect(&typist, expected, 3000));

    CF_REQUIRE(chan_perform(&typist, typing, "{\"action\":\"stop\"}"));
    chan_delivery_expected(expected, sizeof expected, typing,
                           "{\"action\":\"stop\",\"user\":{\"id\":1,"
                           "\"name\":\"JZ\"}}");
    CF_CHECK(chan_expect(&reader, expected, 3000));

    /* Not an action: nothing is sent. */
    CF_REQUIRE(chan_perform(&typist, typing, "{\"action\":\"dance\"}"));
    CF_CHECK(chan_silent(&reader, 300));

    chan_disconnect(&typist);
    chan_disconnect(&reader);
    chan_fixture_close(&f);
}

CF_TEST(typing_notifications_share_the_room_stream_however_spelled) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    chan_conn typist, reader;
    CF_REQUIRE(chan_connect(&f, CHAN_JZ, &typist));
    CF_REQUIRE(chan_connect(&f, CHAN_KEVIN, &reader));

    char prefixed[128], plain[128];
    chan_room_identifier(prefixed, sizeof prefixed,
                         "::TypingNotificationsChannel", CHAN_DESIGNERS);
    chan_room_identifier(plain, sizeof plain, "TypingNotificationsChannel",
                         CHAN_DESIGNERS);
    CF_REQUIRE(chan_subscribe(&typist, prefixed));
    CF_CHECK(chan_confirm(&typist, prefixed));
    CF_REQUIRE(chan_subscribe(&reader, plain));
    CF_CHECK(chan_confirm(&reader, plain));

    CF_REQUIRE(chan_perform(&typist, prefixed, "{\"action\":\"start\"}"));
    char expected[10240];
    chan_delivery_expected(expected, sizeof expected, plain,
                           "{\"action\":\"start\",\"user\":{\"id\":1,"
                           "\"name\":\"JZ\"}}");
    CF_CHECK(chan_expect(&reader, expected, 3000));
    chan_delivery_expected(expected, sizeof expected, prefixed,
                           "{\"action\":\"start\",\"user\":{\"id\":1,"
                           "\"name\":\"JZ\"}}");
    CF_CHECK(chan_expect(&typist, expected, 3000));

    chan_disconnect(&typist);
    chan_disconnect(&reader);
    chan_fixture_close(&f);
}

/* ---- ReadRoomsChannel / UnreadRoomsChannel ----------------------------------- */

CF_TEST(unread_rooms_streams_only_the_subscribers_own_stream) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    chan_conn outsider, member;
    CF_REQUIRE(chan_connect(&f, CHAN_JZ, &outsider));
    CF_REQUIRE(chan_connect(&f, CHAN_BENDER, &member));

    char unreads[64];
    chan_channel_identifier(unreads, sizeof unreads, "UnreadRoomsChannel");
    CF_REQUIRE(chan_subscribe(&outsider, unreads));
    CF_CHECK(chan_confirm(&outsider, unreads));
    CF_REQUIRE(chan_subscribe(&member, unreads));
    CF_CHECK(chan_confirm(&member, unreads));

    /* A message in the direct room tells its members' unread streams. */
    cf_room direct = chan_find_room(&f, CHAN_DIRECT);
    cf_message message = chan_find_message(&f, 100);
    cf_broadcast_partials partials;
    chan_fake_partials(&partials);
    CF_REQUIRE(cf_broadcast_message_create(f.scratch.db, f.cable, &direct,
                                           &message, &partials) == CF_OK);
    char expected[10240];
    chan_delivery_expected(expected, sizeof expected, unreads,
                           "{\"roomId\":12}");
    CF_CHECK(chan_expect(&member, expected, 3000));
    CF_CHECK(chan_silent(&outsider, 300));
    cf_room_dispose(&direct);
    cf_message_dispose(&message);

    chan_disconnect(&outsider);
    chan_disconnect(&member);
    chan_fixture_close(&f);
}

CF_TEST(read_rooms_streams_the_users_reads) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    chan_conn c;
    CF_REQUIRE(chan_connect(&f, CHAN_JZ, &c));

    char reads[64];
    chan_channel_identifier(reads, sizeof reads, "ReadRoomsChannel");
    CF_REQUIRE(chan_subscribe(&c, reads));
    CF_CHECK(chan_confirm(&c, reads));

    CF_REQUIRE(cf_broadcast_read_room(f.cable, CHAN_JZ, 7) == CF_OK);
    CF_REQUIRE(cf_broadcast_read_room(f.cable, CHAN_DAVID, 8) == CF_OK);
    char expected[10240];
    chan_delivery_expected(expected, sizeof expected, reads, "{\"room_id\":7}");
    CF_CHECK(chan_expect(&c, expected, 3000));
    CF_CHECK(chan_silent(&c, 300));

    chan_disconnect(&c);
    chan_fixture_close(&f);
}

CF_TEST(performing_subscribed_streams_twice_like_ruby) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    chan_conn c;
    CF_REQUIRE(chan_connect(&f, CHAN_BENDER, &c));

    char unreads[64];
    chan_channel_identifier(unreads, sizeof unreads, "UnreadRoomsChannel");
    CF_REQUIRE(chan_subscribe(&c, unreads));
    CF_CHECK(chan_confirm(&c, unreads));
    CF_REQUIRE(chan_perform(&c, unreads, "{\"action\":\"subscribed\"}"));
    CF_CHECK(chan_silent(&c, 200));

    cf_room direct = chan_find_room(&f, CHAN_DIRECT);
    CF_REQUIRE(cf_broadcast_unread_room(f.scratch.db, f.cable, &direct) ==
               CF_OK);
    char expected[10240];
    chan_delivery_expected(expected, sizeof expected, unreads,
                           "{\"roomId\":12}");
    CF_CHECK(chan_expect(&c, expected, 3000));
    CF_CHECK(chan_expect(&c, expected, 3000));
    cf_room_dispose(&direct);

    chan_disconnect(&c);
    chan_fixture_close(&f);
}

/* ---- RoomMessagesChannel ------------------------------------------------------ */

CF_TEST(room_messages_member_confirms_and_receives) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    chan_conn c;
    CF_REQUIRE(chan_connect(&f, CHAN_KEVIN, &c));

    char identifier[1024];
    chan_room_messages_identifier(&f, CHAN_DESIGNERS, identifier,
                                  sizeof identifier);
    CF_REQUIRE(chan_subscribe(&c, identifier));
    CF_CHECK(chan_confirm(&c, identifier));

    cf_room room = chan_find_room(&f, CHAN_DESIGNERS);
    cf_message message = chan_find_message(&f, 100);
    CF_REQUIRE(cf_broadcast_message_remove(f.cable, &room, &message) == CF_OK);
    char expected[10240];
    chan_delivery_expected(
        expected, sizeof expected, identifier,
        "\"\\u003cturbo-stream action=\\\"remove\\\" "
        "target=\\\"message_0001\\\"\\u003e\\u003c/turbo-stream\\u003e\"");
    CF_CHECK(chan_expect(&c, expected, 3000));
    cf_room_dispose(&room);
    cf_message_dispose(&message);

    chan_disconnect(&c);
    chan_fixture_close(&f);
}

CF_TEST(room_message_streams_are_rejected_for_everyone_else) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));

    /* Kevin is only in designers and the direct room. */
    chan_conn kevin;
    CF_REQUIRE(chan_connect(&f, CHAN_KEVIN, &kevin));

    /* An unsigned stream name, a missing name, a signed non-message stream,
     * a stale room type, a user GID in place of a room. */
    cf_room designers = chan_find_room(&f, CHAN_DESIGNERS);
    cf_str gid = {0};
    CF_REQUIRE(cf_cable_room_gid_param(&designers, &gid) == CF_OK);
    char identifier[1024];
    snprintf(identifier, sizeof identifier,
             "{\"channel\":\"RoomMessagesChannel\","
             "\"signed_stream_name\":\"%.*s:messages\"}",
             (int)gid.len, gid.ptr);
    CF_REQUIRE(chan_subscribe(&kevin, identifier));
    CF_CHECK(chan_reject(&kevin, identifier));

    snprintf(identifier, sizeof identifier,
             "{\"channel\":\"RoomMessagesChannel\"}");
    CF_REQUIRE(chan_subscribe(&kevin, identifier));
    CF_CHECK(chan_reject(&kevin, identifier));

    {
        cf_span parts[1] = {{(const unsigned char *)"rooms", 5}};
        char signed_name[512];
        chan_signed_stream(&f, parts, 1, signed_name, sizeof signed_name);
        snprintf(identifier, sizeof identifier,
                 "{\"channel\":\"RoomMessagesChannel\","
                 "\"signed_stream_name\":\"%s\"}",
                 signed_name);
        CF_REQUIRE(chan_subscribe(&kevin, identifier));
        CF_CHECK(chan_reject(&kevin, identifier));
    }
    {
        /* A room whose type changed since the name was signed. */
        cf_room stale = designers;
        stale.room_type = CF_ROOM_OPEN;
        cf_str stale_gid = {0};
        CF_REQUIRE(cf_cable_room_gid_param(&stale, &stale_gid) == CF_OK);
        cf_span parts[2] = {
            {(const unsigned char *)stale_gid.ptr, stale_gid.len},
            {(const unsigned char *)"messages", 8},
        };
        char signed_name[512];
        chan_signed_stream(&f, parts, 2, signed_name, sizeof signed_name);
        snprintf(identifier, sizeof identifier,
                 "{\"channel\":\"RoomMessagesChannel\","
                 "\"signed_stream_name\":\"%s\"}",
                 signed_name);
        CF_REQUIRE(chan_subscribe(&kevin, identifier));
        CF_CHECK(chan_reject(&kevin, identifier));
        cf_str_dispose(&stale_gid);
    }
    {
        cf_str user_gid = {0};
        CF_REQUIRE(cf_cable_user_gid_param(CHAN_KEVIN, &user_gid) == CF_OK);
        cf_span parts[2] = {
            {(const unsigned char *)user_gid.ptr, user_gid.len},
            {(const unsigned char *)"messages", 8},
        };
        char signed_name[512];
        chan_signed_stream(&f, parts, 2, signed_name, sizeof signed_name);
        snprintf(identifier, sizeof identifier,
                 "{\"channel\":\"RoomMessagesChannel\","
                 "\"signed_stream_name\":\"%s\"}",
                 signed_name);
        CF_REQUIRE(chan_subscribe(&kevin, identifier));
        CF_CHECK(chan_reject(&kevin, identifier));
        cf_str_dispose(&user_gid);
    }
    cf_str_dispose(&gid);
    cf_room_dispose(&designers);

    /* A user who was never a member. */
    chan_conn bender;
    CF_REQUIRE(chan_connect(&f, CHAN_BENDER, &bender));
    char member_identifier[1024];
    chan_room_messages_identifier(&f, CHAN_DESIGNERS, member_identifier,
                                  sizeof member_identifier);
    CF_REQUIRE(chan_subscribe(&bender, member_identifier));
    CF_CHECK(chan_reject(&bender, member_identifier));
    CF_CHECK(chan_silent(&kevin, 300));

    chan_disconnect(&kevin);
    chan_disconnect(&bender);
    chan_fixture_close(&f);
}

/* ---- Turbo::StreamsChannel with RoomStreamsAreAuthorized ---------------------- */

CF_TEST(turbo_streams_refuses_room_messages_but_serves_the_room_list) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    chan_conn c;
    CF_REQUIRE(chan_connect(&f, CHAN_JZ, &c));

    /* A validly signed room-message stream is rejected by the guard. */
    char identifier[1024];
    {
        char messages_identifier[1024];
        (void)messages_identifier;
        cf_room room = chan_find_room(&f, CHAN_DESIGNERS);
        cf_str gid = {0};
        CF_REQUIRE(cf_cable_room_gid_param(&room, &gid) == CF_OK);
        cf_span parts[2] = {
            {(const unsigned char *)gid.ptr, gid.len},
            {(const unsigned char *)"messages", 8},
        };
        char signed_name[512];
        chan_signed_stream(&f, parts, 2, signed_name, sizeof signed_name);
        snprintf(identifier, sizeof identifier,
                 "{\"channel\":\"Turbo::StreamsChannel\","
                 "\"signed_stream_name\":\"%s\"}",
                 signed_name);
        cf_str_dispose(&gid);
        cf_room_dispose(&room);
    }
    CF_REQUIRE(chan_subscribe(&c, identifier));
    CF_CHECK(chan_reject(&c, identifier));

    snprintf(identifier, sizeof identifier,
             "{\"channel\":\"Turbo::StreamsChannel\","
             "\"signed_stream_name\":\"forged--0000\"}");
    CF_REQUIRE(chan_subscribe(&c, identifier));
    CF_CHECK(chan_reject(&c, identifier));

    snprintf(identifier, sizeof identifier,
             "{\"channel\":\"Turbo::StreamsChannel\"}");
    CF_REQUIRE(chan_subscribe(&c, identifier));
    CF_CHECK(chan_reject(&c, identifier));

    /* The room list is served. */
    {
        cf_span parts[1] = {{(const unsigned char *)"rooms", 5}};
        char signed_name[512];
        chan_signed_stream(&f, parts, 1, signed_name, sizeof signed_name);
        snprintf(identifier, sizeof identifier,
                 "{\"channel\":\"Turbo::StreamsChannel\","
                 "\"signed_stream_name\":\"%s\"}",
                 signed_name);
    }
    CF_REQUIRE(chan_subscribe(&c, identifier));
    CF_CHECK(chan_confirm(&c, identifier));

    cf_room designers = chan_find_room(&f, CHAN_DESIGNERS);
    CF_REQUIRE(cf_broadcast_room_remove(f.cable, &designers) == CF_OK);
    char expected[10240];
    chan_delivery_expected(
        expected, sizeof expected, identifier,
        "\"\\u003cturbo-stream action=\\\"remove\\\" "
        "target=\\\"list_rooms_closed_10\\\"\\u003e\\u003c/turbo-stream\\u003e\"");
    CF_CHECK(chan_expect(&c, expected, 3000));
    cf_room_dispose(&designers);

    chan_disconnect(&c);
    chan_fixture_close(&f);
}

/* ---- dispatch edge cases ------------------------------------------------------- */

CF_TEST(duplicate_unsubscribe_malformed_and_unknown_are_ignored) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    chan_conn c;
    CF_REQUIRE(chan_connect(&f, CHAN_JZ, &c));

    char unreads[64];
    chan_channel_identifier(unreads, sizeof unreads, "UnreadRoomsChannel");
    CF_REQUIRE(chan_subscribe(&c, unreads));
    CF_CHECK(chan_confirm(&c, unreads));
    /* A repeated identifier is ignored without a reply. */
    CF_REQUIRE(chan_subscribe(&c, unreads));
    CF_CHECK(chan_silent(&c, 300));

    /* Unsubscribing an unknown identifier is silent. */
    CF_REQUIRE(chan_unsubscribe(&c, "{\"channel\":\"UnreadRoomsChannel\"}"));
    CF_CHECK(chan_silent(&c, 300));
    CF_REQUIRE(chan_unsubscribe(&c, unreads));
    CF_CHECK(chan_silent(&c, 300));

    /* Malformed identifiers, an unknown channel and unknown commands. */
    CF_REQUIRE(chan_send_identifier_command(&c, "subscribe", "not json"));
    CF_CHECK(chan_silent(&c, 300));
    CF_REQUIRE(chan_send_text(
        &c, "{\"command\":\"subscribe\",\"identifier\":5}"));
    CF_CHECK(chan_silent(&c, 300));
    CF_REQUIRE(chan_send_text(&c, "{\"command\":\"subscribe\"}"));
    CF_CHECK(chan_silent(&c, 300));
    CF_REQUIRE(chan_subscribe(&c, "{\"channel\":\"NopeChannel\"}"));
    CF_CHECK(chan_silent(&c, 300));
    CF_REQUIRE(chan_send_text(&c, "{\"command\":\"explode\"}"));
    CF_CHECK(chan_silent(&c, 300));
    CF_REQUIRE(chan_send_text(&c, "not json at all"));
    CF_CHECK(chan_silent(&c, 300));
    /* A message command for an unknown subscription is silent. */
    CF_REQUIRE(chan_perform(&c, "{\"channel\":\"UnreadRoomsChannel\"}",
                            "{\"action\":\"subscribed\"}"));
    CF_CHECK(chan_silent(&c, 300));
    /* Malformed perform data. */
    CF_REQUIRE(chan_perform(&c, unreads, "not json"));
    CF_CHECK(chan_silent(&c, 300));

    chan_disconnect(&c);
    chan_fixture_close(&f);
}

CF_TEST(identifiers_are_echoed_byte_for_byte) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    chan_conn c;
    CF_REQUIRE(chan_connect(&f, CHAN_JZ, &c));

    /* Unusual spelling, extra params and whitespace survive unchanged in the
     * confirmation and in every delivery frame. */
    const char *identifier =
        "{ \"channel\" : \"UnreadRoomsChannel\", \"extra\" : [1, 2] }";
    CF_REQUIRE(chan_subscribe(&c, identifier));
    CF_CHECK(chan_confirm(&c, identifier));

    cf_room designers = chan_find_room(&f, CHAN_DESIGNERS);
    CF_REQUIRE(cf_broadcast_unread_room(f.scratch.db, f.cable, &designers) ==
               CF_OK);
    char expected[10240];
    chan_delivery_expected(expected, sizeof expected, identifier,
                           "{\"roomId\":10}");
    CF_CHECK(chan_expect(&c, expected, 3000));
    cf_room_dispose(&designers);

    chan_disconnect(&c);
    chan_fixture_close(&f);
}

/* A `subscribed` that raises (here: the rooms table is gone) leaves the
 * connection up and the subscription registered, with no reply. */
CF_TEST(failed_subscribe_leaves_the_connection_up) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    chan_conn c;
    CF_REQUIRE(chan_connect(&f, CHAN_JZ, &c));

    CF_REQUIRE(chan_exec(f.scratch.db, "ALTER TABLE rooms RENAME TO rooms_x"));
    char typing[128];
    chan_room_identifier(typing, sizeof typing, "TypingNotificationsChannel",
                         CHAN_DESIGNERS);
    CF_REQUIRE(chan_subscribe(&c, typing));
    CF_CHECK(chan_silent(&c, 300));
    CF_REQUIRE(chan_exec(f.scratch.db, "ALTER TABLE rooms_x RENAME TO rooms"));

    /* Performing on the failed subscription logs and keeps the connection. */
    CF_REQUIRE(chan_perform(&c, typing, "{\"action\":\"start\"}"));
    CF_CHECK(chan_silent(&c, 300));
    char heartbeat[64];
    chan_channel_identifier(heartbeat, sizeof heartbeat, "HeartbeatChannel");
    CF_REQUIRE(chan_subscribe(&c, heartbeat));
    CF_CHECK(chan_confirm(&c, heartbeat));

    chan_disconnect(&c);
    chan_fixture_close(&f);
}

/* connection.rs perform_action / Rails: only an all-whitespace action becomes
 * "receive"; every other string is dispatched verbatim, never trimmed. */
CF_TEST(padded_action_names_are_dispatched_verbatim) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    chan_conn typist, reader;
    CF_REQUIRE(chan_connect(&f, CHAN_JZ, &typist));
    CF_REQUIRE(chan_connect(&f, CHAN_KEVIN, &reader));

    char typing[128];
    chan_room_identifier(typing, sizeof typing, "TypingNotificationsChannel",
                         CHAN_DESIGNERS);
    CF_REQUIRE(chan_subscribe(&typist, typing));
    CF_CHECK(chan_confirm(&typist, typing));
    CF_REQUIRE(chan_subscribe(&reader, typing));
    CF_CHECK(chan_confirm(&reader, typing));

    /* A padded name is not "start" and performs nothing. */
    CF_REQUIRE(chan_perform(&typist, typing, "{\"action\":\" start \"}"));
    CF_CHECK(chan_silent(&reader, 300));
    CF_REQUIRE(chan_perform(&typist, typing, "{\"action\":\"start \"}"));
    CF_CHECK(chan_silent(&reader, 300));
    CF_REQUIRE(chan_perform(&typist, typing, "{\"action\":\" start\"}"));
    CF_CHECK(chan_silent(&reader, 300));

    /* The exact name still broadcasts, with the payload verbatim. */
    CF_REQUIRE(chan_perform(&typist, typing, "{\"action\":\"start\"}"));
    char expected[10240];
    chan_delivery_expected(expected, sizeof expected, typing,
                           "{\"action\":\"start\",\"user\":{\"id\":1,"
                           "\"name\":\"JZ\"}}");
    CF_CHECK(chan_expect(&reader, expected, 3000));

    chan_disconnect(&typist);
    chan_disconnect(&reader);
    chan_fixture_close(&f);
}

/* The exact naming.rs channel_name/broadcasting_for mapping, including the
 * namespaced classes no stream uses today. */
CF_TEST(broadcasting_names_match_naming_rs) {
    static const struct {
        const char *class_name;
        const char *expected;
    } cases[] = {
        {"RoomChannel", "room:part"},
        {"TypingNotificationsChannel", "typing_notifications:part"},
        {"Turbo::StreamsChannel", "turbo:streams:part"},
        {"ApplicationCable::Channel", "application_cable::part"},
        {"HTMLChannel", "html:part"},
        {"HTMLParserChannel", "html_parser:part"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        cf_str out = {0};
        CF_REQUIRE(cf_cable_test_broadcasting_for(
                       cases[i].class_name,
                       (cf_span){(const unsigned char *)"part", 4}, &out) ==
                   CF_OK);
        if (strcmp(out.ptr, cases[i].expected) != 0) {
            fprintf(stderr, "  %s -> %s (expected %s)\n", cases[i].class_name,
                    out.ptr, cases[i].expected);
        }
        CF_CHECK(strcmp(out.ptr, cases[i].expected) == 0);
        cf_str_dispose(&out);
    }
}

CF_TEST_MAIN()
