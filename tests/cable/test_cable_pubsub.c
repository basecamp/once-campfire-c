/* tests/cable/test_cable_pubsub.c — C02 local subscription maps, cross-loop
 * fan-out, the per-loop pending bounds and their dropped-broadcast counters,
 * and the raw internal channel. Drives loops with a counting sink (the
 * socket path's exact frames are in test_cable_channels.c). */
#include "cable_channels_testutil.h"

#include <stdlib.h>

typedef struct {
    size_t calls;
} count_sink;

static cf_err count_sink_fn(void *user, cf_cable_frame *frame) {
    (void)frame;
    ((count_sink *)user)->calls++;
    return CF_OK;
}

/* A sink that always refuses, like a socket whose pending cap was reached. */
static cf_err refusing_sink_fn(void *user, cf_cable_frame *frame) {
    (void)user;
    (void)frame;
    return CF_BUSY;
}

static bool feed_subscribe(cf_cable_loop *loop, const char *channel,
                           int64_t room_id) {
    char identifier[256];
    char cmd[512];
    chan_room_identifier(identifier, sizeof identifier, channel, room_id);
    char quoted[512];
    chan_json_string(quoted, sizeof quoted, identifier);
    snprintf(cmd, sizeof cmd, "{\"command\":\"subscribe\",\"identifier\":%s}",
             quoted);
    return cf_cable_test_feed(loop, (cf_span){(const unsigned char *)cmd,
                                              strlen(cmd)}) == CF_OK;
}

static bool publish(cf_cable *cable, const char *stream, const char *payload) {
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

static bool publish_rc(cf_cable *cable, const char *stream, const char *payload,
                       cf_err *out) {
    cf_buf *buf = NULL;
    if (cf_buf_copy((cf_span){(const unsigned char *)payload,
                              strlen(payload)},
                    &buf) != CF_OK) {
        return false;
    }
    *out = cf_cable_publish(
        cable, (cf_span){(const unsigned char *)stream, strlen(stream)}, buf);
    cf_buf_release(buf);
    return true;
}

CF_TEST(fanout_reaches_matching_local_subscribers_only) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));

    count_sink jz = {0}, kevin = {0}, bender = {0};
    cf_cable_loop *jz_loop = NULL, *kevin_loop = NULL, *bender_loop = NULL;
    CF_REQUIRE(cf_cable_test_attach(f.cable, CHAN_JZ, "JZ", count_sink_fn, &jz,
                                    &jz_loop) == CF_OK);
    CF_REQUIRE(cf_cable_test_attach(f.cable, CHAN_KEVIN, "Kevin",
                                    count_sink_fn, &kevin,
                                    &kevin_loop) == CF_OK);
    CF_REQUIRE(cf_cable_test_attach(f.cable, CHAN_BENDER, "Bender",
                                    count_sink_fn, &bender,
                                    &bender_loop) == CF_OK);

    /* JZ and Kevin are designers members; Bender is not. */
    CF_REQUIRE(feed_subscribe(jz_loop, "RoomChannel", CHAN_DESIGNERS));
    CF_REQUIRE(feed_subscribe(kevin_loop, "RoomChannel", CHAN_DESIGNERS));
    CF_REQUIRE(feed_subscribe(bender_loop, "RoomChannel", CHAN_DESIGNERS));
    jz.calls = kevin.calls = bender.calls = 0;

    char stream[256];
    {
        cf_room room = {0};
        bool found = false;
        CF_REQUIRE(cf_room_find_by_id(f.scratch.db, CHAN_DESIGNERS, &found,
                                      &room) == CF_OK);
        CF_REQUIRE(found);
        cf_str gid = {0};
        CF_REQUIRE(cf_cable_room_gid_param(&room, &gid) == CF_OK);
        snprintf(stream, sizeof stream, "room:%.*s", (int)gid.len, gid.ptr);
        cf_str_dispose(&gid);
        cf_room_dispose(&room);
    }
    CF_REQUIRE(publish(f.cable, stream, "{\"n\":1}"));
    CF_CHECK(jz.calls == 1);
    CF_CHECK(kevin.calls == 1);
    CF_CHECK(bender.calls == 0);

    /* A stream nobody reads reaches nobody. */
    CF_REQUIRE(publish(f.cable, "room:not-a-gid", "{\"n\":2}"));
    CF_CHECK(jz.calls == 1 && kevin.calls == 1 && bender.calls == 0);

    /* The user's own unread stream reaches only that user. */
    CF_REQUIRE(publish(f.cable, "user_2_unreads", "{\"roomId\":10}"));
    CF_CHECK(jz.calls == 1 && kevin.calls == 1 && bender.calls == 0);
    cf_cable_stats stats;
    cf_cable_stats_get(f.cable, &stats);
    CF_CHECK(stats.loops == 3);
    CF_CHECK(stats.published == 3);
    CF_CHECK(stats.delivered == 2);
    CF_CHECK(stats.dropped == 0);

    cf_cable_test_detach(jz_loop);
    cf_cable_test_detach(kevin_loop);
    cf_cable_test_detach(bender_loop);
    chan_fixture_close(&f);
}

CF_TEST(pending_command_bound_drops_and_counts) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    count_sink sink = {0};
    cf_cable_loop *loop = NULL;
    CF_REQUIRE(cf_cable_test_attach(f.cable, CHAN_JZ, "JZ", count_sink_fn,
                                    &sink, &loop) == CF_OK);
    CF_REQUIRE(feed_subscribe(loop, "RoomChannel", CHAN_DESIGNERS));
    sink.calls = 0;

    char stream[256];
    {
        cf_room room = {0};
        bool found = false;
        CF_REQUIRE(cf_room_find_by_id(f.scratch.db, CHAN_DESIGNERS, &found,
                                      &room) == CF_OK);
        CF_REQUIRE(found);
        cf_str gid = {0};
        CF_REQUIRE(cf_cable_room_gid_param(&room, &gid) == CF_OK);
        snprintf(stream, sizeof stream, "room:%.*s", (int)gid.len, gid.ptr);
        cf_str_dispose(&gid);
        cf_room_dispose(&room);
    }

    /* Hold automatic delivery so the bounded queue accumulates. */
    cf_cable_test_hold_delivery(loop, true);
    cf_err rc = CF_OK;
    for (size_t i = 0; i < CF_CABLE_LOOP_MAX_COMMANDS; i++) {
        CF_REQUIRE(publish_rc(f.cable, stream, "{\"n\":1}", &rc));
        CF_REQUIRE(rc == CF_OK);
    }
    CF_CHECK(cf_cable_loop_pending(loop) == CF_CABLE_LOOP_MAX_COMMANDS);
    /* The next one overflows: dropped, counted and reported as CF_BUSY. */
    CF_REQUIRE(publish_rc(f.cable, stream, "{\"n\":1}", &rc));
    CF_CHECK(rc == CF_BUSY);
    CF_CHECK(cf_cable_loop_pending(loop) == CF_CABLE_LOOP_MAX_COMMANDS);
    CF_CHECK(cf_cable_loop_dropped(loop) == 1);
    cf_cable_stats stats;
    cf_cable_stats_get(f.cable, &stats);
    CF_CHECK(stats.dropped == 1);
    CF_CHECK(sink.calls == 0);

    /* Releasing delivers exactly the queued commands, once each. */
    cf_cable_test_hold_delivery(loop, false);
    CF_CHECK(cf_cable_loop_pending(loop) == 0);
    CF_CHECK(sink.calls == CF_CABLE_LOOP_MAX_COMMANDS);
    cf_cable_stats_get(f.cable, &stats);
    CF_CHECK(stats.delivered == CF_CABLE_LOOP_MAX_COMMANDS);

    cf_cable_test_detach(loop);
    chan_fixture_close(&f);
}

CF_TEST(pending_byte_bound_drops_and_counts) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));

    /* A cable whose per-loop byte bound is small enough to reach quickly. */
    cf_cable_config config;
    memset(&config, 0, sizeof config);
    config.app = f.app;
    config.database_path = f.scratch.path;
    config.max_pending_bytes = 4096;
    cf_cable *small = NULL;
    CF_REQUIRE(cf_cable_create(&config, &small) == CF_OK);

    count_sink sink = {0};
    cf_cable_loop *loop = NULL;
    CF_REQUIRE(cf_cable_test_attach(small, CHAN_JZ, "JZ", count_sink_fn, &sink,
                                    &loop) == CF_OK);
    CF_REQUIRE(feed_subscribe(loop, "RoomChannel", CHAN_DESIGNERS));
    sink.calls = 0;

    char stream[256];
    {
        cf_room room = {0};
        bool found = false;
        CF_REQUIRE(cf_room_find_by_id(f.scratch.db, CHAN_DESIGNERS, &found,
                                      &room) == CF_OK);
        CF_REQUIRE(found);
        cf_str gid = {0};
        CF_REQUIRE(cf_cable_room_gid_param(&room, &gid) == CF_OK);
        snprintf(stream, sizeof stream, "room:%.*s", (int)gid.len, gid.ptr);
        cf_str_dispose(&gid);
        cf_room_dispose(&room);
    }
    char payload[1100];
    memset(payload, 'x', sizeof payload - 2);
    payload[0] = '{';
    payload[1] = '"';
    payload[sizeof payload - 2] = '}';
    payload[sizeof payload - 1] = '\0';

    cf_cable_test_hold_delivery(loop, true);
    size_t accepted = 0;
    cf_err rc = CF_OK;
    for (size_t i = 0; i < 8; i++) {
        CF_REQUIRE(publish_rc(small, stream, payload, &rc));
        if (rc == CF_OK) {
            accepted++;
        } else {
            CF_CHECK(rc == CF_BUSY);
            break;
        }
    }
    CF_CHECK(accepted != 0);
    CF_CHECK(accepted < 8);
    CF_CHECK(cf_cable_loop_dropped(loop) >= 1);
    CF_CHECK(cf_cable_loop_pending(loop) == accepted);
    cf_cable_test_hold_delivery(loop, false);
    CF_CHECK(sink.calls == accepted);

    cf_cable_test_detach(loop);
    cf_cable_destroy(small);
    chan_fixture_close(&f);
}

CF_TEST(refusing_sink_drops_are_counted) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    cf_cable_loop *loop = NULL;
    CF_REQUIRE(cf_cable_test_attach(f.cable, CHAN_JZ, "JZ", refusing_sink_fn,
                                    NULL, &loop) == CF_OK);
    CF_REQUIRE(feed_subscribe(loop, "RoomChannel", CHAN_DESIGNERS));

    char stream[256];
    {
        cf_room room = {0};
        bool found = false;
        CF_REQUIRE(cf_room_find_by_id(f.scratch.db, CHAN_DESIGNERS, &found,
                                      &room) == CF_OK);
        CF_REQUIRE(found);
        cf_str gid = {0};
        CF_REQUIRE(cf_cable_room_gid_param(&room, &gid) == CF_OK);
        snprintf(stream, sizeof stream, "room:%.*s", (int)gid.len, gid.ptr);
        cf_str_dispose(&gid);
        cf_room_dispose(&room);
    }
    /* The subscriber's sink refuses (the transport is closing): the frame is
     * dropped and counted, never retried. */
    cf_err rc = CF_OK;
    CF_REQUIRE(publish_rc(f.cable, stream, "{\"n\":9}", &rc));
    CF_CHECK(rc == CF_OK || rc == CF_BUSY);
    CF_CHECK(cf_cable_loop_dropped(loop) >= 1);
    CF_CHECK(cf_cable_loop_pending(loop) == 0);

    cf_cable_test_detach(loop);
    chan_fixture_close(&f);
}

CF_TEST(internal_channel_receives_raw_payloads) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    count_sink jz = {0}, kevin = {0};
    cf_cable_loop *jz_loop = NULL, *kevin_loop = NULL;
    CF_REQUIRE(cf_cable_test_attach(f.cable, CHAN_JZ, "JZ", count_sink_fn, &jz,
                                    &jz_loop) == CF_OK);
    CF_REQUIRE(cf_cable_test_attach(f.cable, CHAN_KEVIN, "Kevin",
                                    count_sink_fn, &kevin,
                                    &kevin_loop) == CF_OK);

    cf_str gid = {0};
    CF_REQUIRE(cf_cable_user_gid_param(CHAN_JZ, &gid) == CF_OK);
    char stream[256];
    snprintf(stream, sizeof stream, "action_cable/%.*s", (int)gid.len,
             gid.ptr);
    CF_REQUIRE(publish(f.cable, stream,
                       "{\"type\":\"disconnect\",\"reconnect\":true}"));
    CF_CHECK(jz.calls == 1);
    CF_CHECK(kevin.calls == 0);

    cf_str_dispose(&gid);
    cf_cable_test_detach(jz_loop);
    cf_cable_test_detach(kevin_loop);
    chan_fixture_close(&f);
}

CF_TEST(detached_loop_stops_receiving) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    count_sink sink = {0};
    cf_cable_loop *loop = NULL;
    CF_REQUIRE(cf_cable_test_attach(f.cable, CHAN_JZ, "JZ", count_sink_fn,
                                    &sink, &loop) == CF_OK);
    CF_REQUIRE(feed_subscribe(loop, "RoomChannel", CHAN_DESIGNERS));
    sink.calls = 0;

    char stream[256];
    {
        cf_room room = {0};
        bool found = false;
        CF_REQUIRE(cf_room_find_by_id(f.scratch.db, CHAN_DESIGNERS, &found,
                                      &room) == CF_OK);
        CF_REQUIRE(found);
        cf_str gid = {0};
        CF_REQUIRE(cf_cable_room_gid_param(&room, &gid) == CF_OK);
        snprintf(stream, sizeof stream, "room:%.*s", (int)gid.len, gid.ptr);
        cf_str_dispose(&gid);
        cf_room_dispose(&room);
    }
    CF_REQUIRE(publish(f.cable, stream, "{\"n\":1}"));
    CF_CHECK(sink.calls == 1);
    cf_cable_test_detach(loop);

    /* The detached loop is gone from the registry. */
    CF_REQUIRE(publish(f.cable, stream, "{\"n\":2}"));
    cf_cable_stats stats;
    cf_cable_stats_get(f.cable, &stats);
    CF_CHECK(stats.loops == 0);
    CF_CHECK(stats.delivered == 1);
    chan_fixture_close(&f);
}

CF_TEST_MAIN()
