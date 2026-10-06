/* tests/cable/test_cable_broadcasts.c — the exact Turbo payload source
 * (channels/broadcasts.rs): stream names, DOM targets, attributes and the
 * delivered Active Support JSON frames, through real C01 connections. The
 * HTML comes from the reference tests' FakePartials; the production cases
 * render through A02's presenters and renderers (cf_broadcast_partials_
 * views), proving no duplicated markup, and pin the per-recipient rule:
 * direct-room creates render each membership's own `_direct` anchor (the
 * member set and unread class differ per recipient) while shared-room
 * broadcasts render once and deliver identical bytes to every member. */
#include "cable_channels_testutil.h"

#include "models/boost.h"
#include "models/message.h"
#include "app/support/route_double.h"
#include "models/room.h"
#include "views.h"

#include <sqlite3.h>

/* ---- helpers ---------------------------------------------------------------- */

static void subscribe_turbo(chan_fixture *f, chan_conn *c,
                            const cf_span *parts, size_t count,
                            char *identifier_out, size_t cap) {
    char signed_name[512];
    chan_signed_stream(f, parts, count, signed_name, sizeof signed_name);
    snprintf(identifier_out, cap,
             "{\"channel\":\"Turbo::StreamsChannel\","
             "\"signed_stream_name\":\"%s\"}",
             signed_name);
    CF_REQUIRE(chan_subscribe(c, identifier_out));
    CF_CHECK(chan_confirm(c, identifier_out));
}

static void expect_turbo(chan_conn *c, const char *identifier, const char *tag,
                         int timeout_ms) {
    char payload[4096];
    chan_json_string(payload, sizeof payload, tag);
    char expected[10240];
    chan_delivery_expected(expected, sizeof expected, identifier, payload);
    CF_CHECK(chan_expect(c, expected, timeout_ms));
}

/* The delivery frame whose message contains `needle` (raw, escaped JSON). */
static bool chan_expect_contains(chan_conn *c, const char *identifier,
                                 const char *needle, int timeout_ms) {
    char got[16384];
    if (!chan_next(c, got, sizeof got, timeout_ms)) {
        fprintf(stderr, "  no frame containing: %s\n", needle);
        return false;
    }
    char quoted[4096];
    chan_json_string(quoted, sizeof quoted, identifier);
    char prefix[8192];
    snprintf(prefix, sizeof prefix, "{\"identifier\":%s,\"message\":", quoted);
    if (strncmp(got, prefix, strlen(prefix)) != 0 ||
        strstr(got, needle) == NULL) {
        fprintf(stderr,
                "  frame mismatch\n  got: %s\n  wanted identifier %s and %s\n",
                got, identifier, needle);
        return false;
    }
    return true;
}

static cf_room load_room(chan_fixture *f, int64_t room_id) {
    cf_room room = {0};
    bool found = false;
    CF_REQUIRE(cf_room_find_by_id(f->scratch.db, room_id, &found, &room) ==
               CF_OK);
    CF_REQUIRE(found);
    return room;
}

static cf_message load_message(chan_fixture *f, int64_t message_id) {
    cf_message message = {0};
    CF_REQUIRE(cf_message_find(f->scratch.db, message_id, &message) == CF_OK);
    return message;
}

static cf_boost load_boost(chan_fixture *f, int64_t boost_id) {
    cf_boost boost = {0};
    CF_REQUIRE(cf_boost_find(f->scratch.db, boost_id, &boost) == CF_OK);
    return boost;
}

static cf_membership load_membership(chan_fixture *f, int64_t id) {
    cf_membership membership = {0};
    CF_REQUIRE(cf_membership_find(f->scratch.db, id, &membership) == CF_OK);
    return membership;
}

/* A byte substring of the builder's content (empty needles match). */
static bool builder_has(const cf_builder *builder, const char *needle) {
    size_t n = strlen(needle);
    if (builder->len < n) return false;
    for (size_t i = 0; i + n <= builder->len; i++) {
        if (n == 0 || memcmp(builder->ptr + i, needle, n) == 0) return true;
    }
    return false;
}

/* The room-messages stream parts for a room: [gid param, "messages"]. */
static void room_messages_parts(const cf_room *room, cf_str *gid,
                                cf_span parts[2]) {
    CF_REQUIRE(cf_cable_room_gid_param(room, gid) == CF_OK);
    parts[0] = (cf_span){(const unsigned char *)gid->ptr, gid->len};
    parts[1] = (cf_span){(const unsigned char *)"messages", 8};
}

static void subscribe_room_messages(chan_fixture *f, chan_conn *c,
                                    const cf_room *room,
                                    char *identifier_out, size_t cap) {
    char signed_name[512];
    cf_str gid = {0};
    cf_span parts[2];
    room_messages_parts(room, &gid, parts);
    chan_signed_stream(f, parts, 2, signed_name, sizeof signed_name);
    cf_str_dispose(&gid);
    snprintf(identifier_out, cap,
             "{\"channel\":\"RoomMessagesChannel\","
             "\"signed_stream_name\":\"%s\"}",
             signed_name);
    CF_REQUIRE(chan_subscribe(c, identifier_out));
    CF_CHECK(chan_confirm(c, identifier_out));
}

/* ---- Message::Broadcasts ------------------------------------------------------ */

CF_TEST(message_broadcasts) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    cf_broadcast_partials partials;
    chan_fake_partials(&partials);

    chan_conn kevin;
    CF_REQUIRE(chan_connect(&f, CHAN_KEVIN, &kevin));
    cf_room designers = load_room(&f, CHAN_DESIGNERS);
    char messages_id[1024];
    subscribe_room_messages(&f, &kevin, &designers, messages_id,
                            sizeof messages_id);
    char unreads[64];
    chan_channel_identifier(unreads, sizeof unreads, "UnreadRoomsChannel");
    CF_REQUIRE(chan_subscribe(&kevin, unreads));
    CF_CHECK(chan_confirm(&kevin, unreads));

    /* message.broadcast_create: append the message, then the members' unread
     * streams. */
    cf_message second = load_message(&f, 101);
    CF_REQUIRE(cf_broadcast_message_create(f.scratch.db, f.cable, &designers,
                                           &second, &partials) == CF_OK);
    expect_turbo(&kevin, messages_id,
                 "<turbo-stream action=\"append\" "
                 "target=\"messages_rooms_closed_10\"><template>"
                 "<div id=\"message_0002\">message 101</div>"
                 "</template></turbo-stream>",
                 3000);
    {
        char expected[10240];
        chan_delivery_expected(expected, sizeof expected, unreads,
                               "{\"roomId\":10}");
        CF_CHECK(chan_expect(&kevin, expected, 3000));
    }

    /* messages#update: replace the presentation, keeping the scroll. */
    cf_broadcast_message_replace(f.cable, &designers, &second, &partials);
    expect_turbo(&kevin, messages_id,
                 "<turbo-stream maintain_scroll=\"true\" action=\"replace\" "
                 "target=\"presentation_message_0002\"><template>"
                 "<div>presentation 101 & more</div>"
                 "</template></turbo-stream>",
                 3000);

    /* MessagesController#destroy. */
    CF_REQUIRE(cf_broadcast_message_remove(f.cable, &designers, &second) ==
               CF_OK);
    expect_turbo(&kevin, messages_id,
                 "<turbo-stream action=\"remove\" "
                 "target=\"message_0002\"></turbo-stream>",
                 3000);
    CF_CHECK(chan_silent(&kevin, 300));

    cf_message_dispose(&second);
    cf_room_dispose(&designers);
    chan_disconnect(&kevin);
    chan_fixture_close(&f);
}

CF_TEST(boost_broadcasts) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    cf_broadcast_partials partials;
    chan_fake_partials(&partials);

    chan_conn kevin;
    CF_REQUIRE(chan_connect(&f, CHAN_KEVIN, &kevin));
    cf_room designers = load_room(&f, CHAN_DESIGNERS);
    char messages_id[1024];
    subscribe_room_messages(&f, &kevin, &designers, messages_id,
                            sizeof messages_id);

    cf_message first = load_message(&f, 100);
    cf_boost boost = load_boost(&f, 200);
    CF_REQUIRE(cf_broadcast_boost_create(f.cable, &designers, &first, &boost,
                                         &partials) == CF_OK);
    expect_turbo(&kevin, messages_id,
                 "<turbo-stream maintain_scroll=\"true\" action=\"append\" "
                 "target=\"boosts_message_0001\"><template>"
                 "<div>boost 200</div></template></turbo-stream>",
                 3000);

    CF_REQUIRE(cf_broadcast_boost_remove(f.cable, &designers, &boost) == CF_OK);
    expect_turbo(&kevin, messages_id,
                 "<turbo-stream action=\"remove\" "
                 "target=\"boost_200\"></turbo-stream>",
                 3000);

    cf_message_dispose(&first);
    cf_boost_dispose(&boost);
    cf_room_dispose(&designers);
    chan_disconnect(&kevin);
    chan_fixture_close(&f);
}

/* ---- room list broadcasts ------------------------------------------------------ */

CF_TEST(room_list_broadcasts) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    cf_broadcast_partials partials;
    chan_fake_partials(&partials);

    chan_conn jz;
    CF_REQUIRE(chan_connect(&f, CHAN_JZ, &jz));
    cf_span rooms_part[1] = {{(const unsigned char *)"rooms", 5}};
    char everyone[1024];
    subscribe_turbo(&f, &jz, rooms_part, 1, everyone, sizeof everyone);

    cf_str own_gid = {0};
    CF_REQUIRE(cf_cable_user_gid_param(CHAN_JZ, &own_gid) == CF_OK);
    cf_span own_parts[2] = {
        {(const unsigned char *)own_gid.ptr, own_gid.len},
        {(const unsigned char *)"rooms", 5},
    };
    char own[1024];
    subscribe_turbo(&f, &jz, own_parts, 2, own, sizeof own);
    cf_str_dispose(&own_gid);

    cf_room hq = load_room(&f, CHAN_DESIGNERS);

    /* Rooms::OpensController#create: prepend to everyone's shared_rooms. */
    CF_REQUIRE(cf_broadcast_open_room_create(f.cable, &hq, &partials) ==
               CF_OK);
    expect_turbo(&jz, everyone,
                 "<turbo-stream action=\"prepend\" target=\"shared_rooms\">"
                 "<template><li>shared 10</li></template></turbo-stream>",
                 3000);

    /* Rooms::OpensController#update passes the room as an open room. */
    cf_room as_open = hq;
    as_open.room_type = CF_ROOM_OPEN;
    CF_REQUIRE(cf_broadcast_open_room_update(f.cable, &as_open, &partials) ==
               CF_OK);
    expect_turbo(&jz, everyone,
                 "<turbo-stream action=\"replace\" "
                 "target=\"list_rooms_open_10\"><template>"
                 "<li>shared 10</li></template></turbo-stream>",
                 3000);

    /* RoomsController#destroy. */
    CF_REQUIRE(cf_broadcast_room_remove(f.cable, &hq) == CF_OK);
    expect_turbo(&jz, everyone,
                 "<turbo-stream action=\"remove\" "
                 "target=\"list_rooms_closed_10\"></turbo-stream>",
                 3000);

    /* Closed rooms go to each member's own stream. */
    cf_room watercooler = load_room(&f, CHAN_WATERCOOLER);
    CF_REQUIRE(cf_broadcast_closed_room_create(f.scratch.db, f.cable,
                                               &watercooler,
                                               &partials) == CF_OK);
    expect_turbo(&jz, own,
                 "<turbo-stream action=\"prepend\" target=\"shared_rooms\">"
                 "<template><li>shared 11</li></template></turbo-stream>",
                 3000);
    CF_REQUIRE(cf_broadcast_closed_room_update(f.scratch.db, f.cable, &hq,
                                              &partials) == CF_OK);
    expect_turbo(&jz, own,
                 "<turbo-stream action=\"replace\" "
                 "target=\"list_rooms_closed_10\"><template>"
                 "<li>shared 10</li></template></turbo-stream>",
                 3000);
    CF_CHECK(chan_silent(&jz, 300));

    cf_room_dispose(&watercooler);
    cf_room_dispose(&hq);
    chan_disconnect(&jz);
    chan_fixture_close(&f);
}

CF_TEST(direct_room_broadcasts) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    cf_broadcast_partials partials;
    chan_fake_partials(&partials);

    chan_conn kevin;
    CF_REQUIRE(chan_connect(&f, CHAN_KEVIN, &kevin));
    cf_str own_gid = {0};
    CF_REQUIRE(cf_cable_user_gid_param(CHAN_KEVIN, &own_gid) == CF_OK);
    cf_span own_parts[2] = {
        {(const unsigned char *)own_gid.ptr, own_gid.len},
        {(const unsigned char *)"rooms", 5},
    };
    char own[1024];
    subscribe_turbo(&f, &kevin, own_parts, 2, own, sizeof own);
    cf_str_dispose(&own_gid);

    cf_room direct = load_room(&f, CHAN_DIRECT);
    CF_REQUIRE(cf_broadcast_direct_room_create(f.scratch.db, f.cable, &direct,
                                               &partials) == CF_OK);
    /* Kevin's membership is 25 (Bender's 26 goes to Bender's stream). */
    expect_turbo(&kevin, own,
                 "<turbo-stream action=\"prepend\" target=\"direct_rooms\">"
                 "<template><li>direct 25</li></template></turbo-stream>",
                 3000);
    CF_CHECK(chan_silent(&kevin, 300));

    cf_room_dispose(&direct);
    chan_disconnect(&kevin);
    chan_fixture_close(&f);
}

CF_TEST(involvement_change_broadcasts) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    cf_broadcast_partials partials;
    chan_fake_partials(&partials);

    chan_conn kevin;
    CF_REQUIRE(chan_connect(&f, CHAN_KEVIN, &kevin));
    cf_str own_gid = {0};
    CF_REQUIRE(cf_cable_user_gid_param(CHAN_KEVIN, &own_gid) == CF_OK);
    cf_span own_parts[2] = {
        {(const unsigned char *)own_gid.ptr, own_gid.len},
        {(const unsigned char *)"rooms", 5},
    };
    char own[1024];
    subscribe_turbo(&f, &kevin, own_parts, 2, own, sizeof own);
    cf_str_dispose(&own_gid);

    cf_room designers = load_room(&f, CHAN_DESIGNERS);
    cf_membership membership = load_membership(&f, 21); /* designers/kevin */

    /* Direct rooms never change the list. */
    cf_room direct = load_room(&f, CHAN_DIRECT);
    CF_REQUIRE(cf_broadcast_involvement_change(
                   f.cable, &direct, &membership, true,
                   CF_INVOLVEMENT_MENTIONS, &partials) == CF_OK);
    CF_CHECK(chan_silent(&kevin, 300));
    cf_room_dispose(&direct);

    /* Becoming invisible removes the room from the list. */
    membership.involvement.present = true;
    membership.involvement.value = CF_INVOLVEMENT_INVISIBLE;
    CF_REQUIRE(cf_broadcast_involvement_change(
                   f.cable, &designers, &membership, true,
                   CF_INVOLVEMENT_MENTIONS, &partials) == CF_OK);
    expect_turbo(&kevin, own,
                 "<turbo-stream action=\"remove\" "
                 "target=\"list_rooms_closed_10\"></turbo-stream>",
                 3000);

    /* Coming back from invisible prepends it again. */
    membership.involvement.value = CF_INVOLVEMENT_EVERYTHING;
    CF_REQUIRE(cf_broadcast_involvement_change(
                   f.cable, &designers, &membership, true,
                   CF_INVOLVEMENT_INVISIBLE, &partials) == CF_OK);
    expect_turbo(&kevin, own,
                 "<turbo-stream action=\"prepend\" target=\"shared_rooms\">"
                 "<template><li>shared 10</li></template></turbo-stream>",
                 3000);

    /* Any other change does not touch the list. */
    CF_REQUIRE(cf_broadcast_involvement_change(
                   f.cable, &designers, &membership, true,
                   CF_INVOLVEMENT_MENTIONS, &partials) == CF_OK);
    CF_CHECK(chan_silent(&kevin, 300));

    /* nil.inquiry: the reference raises after the update saved. */
    CF_CHECK(cf_broadcast_involvement_change(
                 f.cable, &designers, &membership, false,
                 CF_INVOLVEMENT_MENTIONS, &partials) == CF_INVALID);

    cf_membership_dispose(&membership);
    cf_room_dispose(&designers);
    chan_disconnect(&kevin);
    chan_fixture_close(&f);
}

/* ---- production partials ------------------------------------------------------- */

static cf_err test_asset_path(void *user, cf_span logical, cf_builder *out) {
    (void)user;
    cf_err rc = cf_builder_append(
        out, (cf_span){(const unsigned char *)"/assets/", 8});
    if (rc == CF_OK) rc = cf_builder_append(out, logical);
    return rc;
}

CF_TEST(production_partials_render_through_a02) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));

    /* A request context for the presenters and a view context for the
     * renderers, exactly as a controller would supply them. */
    cf_test_routes_reset();
    CF_REQUIRE(cf_test_routes_add("GET", "/rooms/:id", 1, NULL) == CF_OK);
    cf_response response;
    cf_response_init(&response);
    cf_request request;
    memset(&request, 0, sizeof request);
    request.method = CF_GET;
    request.path = (cf_span){(const unsigned char *)"/rooms/10", 9};
    request.target = request.path;
    cf_ctx ctx;
    CF_REQUIRE(cf_ctx_create(&ctx, f.app, f.scratch.db, &request, &response) ==
               CF_OK);
    ctx.identity.kind = CF_AUTH_SESSION;
    ctx.identity.user_id = CHAN_KEVIN;
    cf_view_ctx view;
    memset(&view, 0, sizeof view);
    view.asset_path = test_asset_path;
    cf_broadcast_views views = {.ctx = &ctx, .view = &view};
    cf_broadcast_partials partials;
    cf_broadcast_partials_views(&partials, &views);

    chan_conn kevin;
    CF_REQUIRE(chan_connect(&f, CHAN_KEVIN, &kevin));
    cf_room designers = load_room(&f, CHAN_DESIGNERS);
    char messages_id[1024];
    subscribe_room_messages(&f, &kevin, &designers, messages_id,
                            sizeof messages_id);

    cf_message first = load_message(&f, 100);
    CF_REQUIRE(cf_broadcast_message_create(f.scratch.db, f.cable, &designers,
                                           &first, &partials) == CF_OK);
    /* The delivered HTML is A02's messages/_message fragment: the message
     * container id and class, not duplicated markup. */
    CF_CHECK(chan_expect_contains(&kevin, messages_id,
                                  "id=\\\"message_0001\\\"", 3000));

    cf_message_dispose(&first);
    cf_room_dispose(&designers);
    chan_disconnect(&kevin);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&response);
    chan_fixture_close(&f);
}

/* ---- production partials: per-recipient rendering ------------------------------- */

/* A request/view context pair for the production partials (the same shape a
 * controller supplies), plus the partials it installs. */
typedef struct {
    cf_ctx ctx;
    cf_response response;
    cf_view_ctx view;
    cf_broadcast_views views;
    cf_broadcast_partials partials;
} prod_partials;

static bool prod_partials_open(prod_partials *p, chan_fixture *f) {
    memset(p, 0, sizeof *p);
    cf_test_routes_reset();
    if (cf_test_routes_add("GET", "/rooms/:id", 1, NULL) != CF_OK) {
        return false;
    }
    cf_response_init(&p->response);
    cf_request request;
    memset(&request, 0, sizeof request);
    request.method = CF_GET;
    request.path = (cf_span){(const unsigned char *)"/rooms/10", 9};
    request.target = request.path;
    if (cf_ctx_create(&p->ctx, f->app, f->scratch.db, &request, &p->response) !=
        CF_OK) {
        return false;
    }
    p->ctx.identity.kind = CF_AUTH_SESSION;
    p->ctx.identity.user_id = CHAN_KEVIN;
    p->view.asset_path = test_asset_path;
    p->views.ctx = &p->ctx;
    p->views.view = &p->view;
    cf_broadcast_partials_views(&p->partials, &p->views);
    return true;
}

static void prod_partials_close(prod_partials *p) {
    cf_ctx_destroy(&p->ctx);
    cf_response_dispose(&p->response);
}

/* Subscribe `c` to its own `[user_gid, "rooms"]` stream. */
static void subscribe_own_rooms(chan_fixture *f, chan_conn *c, int64_t user_id,
                                char *identifier_out, size_t cap) {
    cf_str gid = {0};
    CF_REQUIRE(cf_cable_user_gid_param(user_id, &gid) == CF_OK);
    cf_span parts[2] = {
        {(const unsigned char *)gid.ptr, gid.len},
        {(const unsigned char *)"rooms", 5},
    };
    subscribe_turbo(f, c, parts, 2, identifier_out, cap);
    cf_str_dispose(&gid);
}

/* The exact frame a Turbo broadcast of `html` produces: byte parity between
 * the renderer's output and what the pipeline delivers.  The message is
 * quoted with the production cf_json_string (Active Support escaping, so
 * newlines inside the partial become \n, exactly as turbo_publish writes
 * them). */
static bool expect_turbo_html(chan_conn *c, const char *identifier,
                              const char *action, const char *target,
                              cf_span html, int timeout_ms) {
    size_t need = html.len + 256;
    char *tag = malloc(need);
    if (tag == NULL) return false;
    int n = snprintf(tag, need,
                     "<turbo-stream action=\"%s\" target=\"%s\">"
                     "<template>%.*s</template></turbo-stream>",
                     action, target, (int)html.len,
                     (const char *)html.ptr);
    bool ok = n >= 0 && (size_t)n < need;
    if (!ok) {
        free(tag);
        return false;
    }
    cf_builder quoted = {0};
    ok = cf_json_string(
             &quoted,
             (cf_span){(const unsigned char *)tag, (size_t)n}) == CF_OK;
    free(tag);
    char payload[65536];
    char expected[131072];
    if (ok && quoted.len + 1 <= sizeof payload) {
        memcpy(payload, quoted.ptr, quoted.len);
        payload[quoted.len] = '\0';
        chan_delivery_expected(expected, sizeof expected, identifier, payload);
    } else {
        ok = false;
    }
    cf_builder_dispose(&quoted);
    return ok && chan_expect(c, expected, timeout_ms);
}

/* `unread_at` is a real column; the fixture seeds every membership read so a
 * per-membership unread state is one UPDATE away. */
static void mark_unread(chan_fixture *f, int64_t membership_id, bool unread) {
    char sql[160];
    if (unread) {
        snprintf(sql, sizeof sql,
                 "UPDATE memberships SET unread_at='2026-01-02 00:00:00' "
                 "WHERE id=%lld",
                 (long long)membership_id);
    } else {
        snprintf(sql, sizeof sql,
                 "UPDATE memberships SET unread_at=NULL WHERE id=%lld",
                 (long long)membership_id);
    }
    CF_REQUIRE(chan_exec(f->scratch.db, sql));
}

/* The direct-room broadcast is per recipient: each membership's own unread
 * state is rendered (the reference's presenter.sidebar_direct per
 * membership), and each frame is byte-identical to the renderer's output for
 * that membership.  Drop counters do not move. */
CF_TEST(production_partials_direct_room_is_per_recipient) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    /* Kevin's direct membership (25) unread; Bender's (26) read. */
    mark_unread(&f, 25, true);

    chan_conn kevin, bender;
    CF_REQUIRE(chan_connect(&f, CHAN_KEVIN, &kevin));
    CF_REQUIRE(chan_connect(&f, CHAN_BENDER, &bender));
    char kevin_stream[1024], bender_stream[1024];
    subscribe_own_rooms(&f, &kevin, CHAN_KEVIN, kevin_stream,
                        sizeof kevin_stream);
    subscribe_own_rooms(&f, &bender, CHAN_BENDER, bender_stream,
                        sizeof bender_stream);

    prod_partials p;
    CF_REQUIRE(prod_partials_open(&p, &f));

    cf_room direct = load_room(&f, CHAN_DIRECT);
    cf_membership m25 = load_membership(&f, 25);
    cf_membership m26 = load_membership(&f, 26);
    CF_REQUIRE(m25.unread_at.present);
    CF_CHECK(!m26.unread_at.present);

    /* The expected bytes: the renderer called directly for each membership
     * (unread true for Kevin, false for Bender), with the broadcast pipeline
     * wrapping them unchanged. */
    cf_builder html25 = {0}, html26 = {0};
    CF_REQUIRE(cf_view_rooms_direct_room_partial(&p.views, &m25, &html25) ==
               CF_OK);
    CF_REQUIRE(cf_view_rooms_direct_room_partial(&p.views, &m26, &html26) ==
               CF_OK);
    CF_CHECK(builder_has(&html25, "class=\"direct unread\""));
    CF_CHECK(!builder_has(&html25, "class=\"direct\""));
    CF_CHECK(builder_has(&html26, "class=\"direct\""));
    CF_CHECK(!builder_has(&html26, "class=\"direct unread\""));
    CF_CHECK(builder_has(&html25, "Bender")); /* Kevin's other member */
    CF_CHECK(builder_has(&html26, "Kevin"));
    CF_CHECK(memcmp(html25.ptr, html26.ptr,
                    html25.len < html26.len ? html25.len : html26.len) != 0);

    cf_cable_stats before;
    cf_cable_stats_get(f.cable, &before);
    CF_REQUIRE(cf_broadcast_direct_room_create(f.scratch.db, f.cable, &direct,
                                               &p.partials) == CF_OK);
    CF_CHECK(expect_turbo_html(&kevin, kevin_stream, "prepend", "direct_rooms",
                               (cf_span){html25.ptr, html25.len}, 3000));
    CF_CHECK(expect_turbo_html(&bender, bender_stream, "prepend", "direct_rooms",
                               (cf_span){html26.ptr, html26.len}, 3000));
    /* One frame per recipient, nothing crossed over. */
    CF_CHECK(chan_silent(&kevin, 200));
    CF_CHECK(chan_silent(&bender, 200));

    cf_cable_stats after;
    cf_cable_stats_get(f.cable, &after);
    CF_CHECK(after.published == before.published + 2);
    CF_CHECK(after.delivered == before.delivered + 2);
    CF_CHECK(after.dropped == before.dropped);

    cf_builder_dispose(&html25);
    cf_builder_dispose(&html26);
    cf_membership_dispose(&m25);
    cf_membership_dispose(&m26);
    cf_room_dispose(&direct);
    prod_partials_close(&p);
    chan_disconnect(&kevin);
    chan_disconnect(&bender);
    chan_fixture_close(&f);
}

/* The shared-room broadcasts are rendered once: two recipients on the same
 * stream (and two members on their own streams) receive identical bytes, and
 * the renderer's unread=false mapping holds even when their memberships are
 * unread. */
CF_TEST(production_partials_shared_room_is_shared_bytes) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    /* Every designers member unread: the shared partial must not reflect it. */
    mark_unread(&f, 20, true);
    mark_unread(&f, 21, true);
    mark_unread(&f, 22, true);

    chan_conn jz, kevin;
    CF_REQUIRE(chan_connect(&f, CHAN_JZ, &jz));
    CF_REQUIRE(chan_connect(&f, CHAN_KEVIN, &kevin));
    cf_span rooms_part[1] = {{(const unsigned char *)"rooms", 5}};
    char everyone_jz[1024], everyone_kevin[1024];
    subscribe_turbo(&f, &jz, rooms_part, 1, everyone_jz, sizeof everyone_jz);
    subscribe_turbo(&f, &kevin, rooms_part, 1, everyone_kevin,
                    sizeof everyone_kevin);
    char own_jz[1024], own_kevin[1024];
    subscribe_own_rooms(&f, &jz, CHAN_JZ, own_jz, sizeof own_jz);
    subscribe_own_rooms(&f, &kevin, CHAN_KEVIN, own_kevin, sizeof own_kevin);

    prod_partials p;
    CF_REQUIRE(prod_partials_open(&p, &f));

    cf_room designers = load_room(&f, CHAN_DESIGNERS);

    /* opens#update: one render, `:rooms`, list target. */
    cf_room as_open = designers;
    as_open.room_type = CF_ROOM_OPEN;
    cf_builder shared = {0};
    CF_REQUIRE(cf_view_rooms_shared_room_partial(NULL, &as_open, &shared) ==
               CF_OK);
    CF_CHECK(!builder_has(&shared, " unread\""));
    CF_CHECK(builder_has(
        &shared, "class=\"align-center gap room btn txt-nowrap\""));

    cf_cable_stats before;
    cf_cable_stats_get(f.cable, &before);
    CF_REQUIRE(cf_broadcast_open_room_update(f.cable, &as_open, &p.partials) ==
               CF_OK);
    CF_CHECK(expect_turbo_html(&jz, everyone_jz, "replace",
                               "list_rooms_open_10",
                               (cf_span){shared.ptr, shared.len}, 3000));
    CF_CHECK(expect_turbo_html(&kevin, everyone_kevin, "replace",
                               "list_rooms_open_10",
                               (cf_span){shared.ptr, shared.len}, 3000));
    cf_cable_stats after;
    cf_cable_stats_get(f.cable, &after);
    CF_CHECK(after.published == before.published + 1);
    CF_CHECK(after.delivered == before.delivered + 2);
    CF_CHECK(after.dropped == before.dropped);

    /* closeds#create: the same bytes on each member's own stream, even
     * though every membership is unread. */
    cf_builder closed = {0};
    CF_REQUIRE(cf_view_rooms_shared_room_partial(NULL, &designers, &closed) ==
               CF_OK);
    CF_CHECK(!builder_has(&closed, " unread\""));
    CF_CHECK(builder_has(&closed, "id=\"list_rooms_closed_10\""));
    CF_CHECK(builder_has(&closed, ">Designers</span>"));

    cf_cable_stats_get(f.cable, &before);
    CF_REQUIRE(cf_broadcast_closed_room_create(f.scratch.db, f.cable,
                                               &designers, &p.partials) ==
               CF_OK);
    CF_CHECK(expect_turbo_html(&jz, own_jz, "prepend", "shared_rooms",
                               (cf_span){closed.ptr, closed.len}, 3000));
    CF_CHECK(expect_turbo_html(&kevin, own_kevin, "prepend", "shared_rooms",
                               (cf_span){closed.ptr, closed.len}, 3000));
    cf_cable_stats_get(f.cable, &after);
    /* Three members published (David has no connection), two delivered. */
    CF_CHECK(after.published == before.published + 3);
    CF_CHECK(after.delivered == before.delivered + 2);
    CF_CHECK(after.dropped == before.dropped);

    cf_builder_dispose(&shared);
    cf_builder_dispose(&closed);
    cf_room_dispose(&designers);
    prod_partials_close(&p);
    chan_disconnect(&jz);
    chan_disconnect(&kevin);
    chan_fixture_close(&f);
}

/* ---- output cap (CF_VIEWS_MAX_OUTPUT, views.h) --------------------------------- */

/* A partial that emits `bytes` bytes of 'x': the synthetic oversized content
 * for the 8 MiB broadcast output cap. */
static cf_err cap_filler_presentation(void *user, const cf_message *message,
                                      cf_builder *out) {
    (void)message;
    size_t bytes = *(const size_t *)user;
    unsigned char *filler = malloc(bytes != 0 ? bytes : 1);
    if (filler == NULL) return CF_NOMEM;
    memset(filler, 'x', bytes);
    cf_err rc = cf_builder_append(out, (cf_span){filler, bytes});
    free(filler);
    return rc;
}

/* The cap is exact on the stream tag builder: at the cap the tag lands on it
 * byte-for-byte, one byte over is CF_LIMIT with the builder left as it was,
 * and attribute values count R01's escaped expansion. */
CF_TEST(action_tag_output_cap_is_exact) {
    static const char action[] = "replace";
    static const char target[] = "presentation_message_0001";
    cf_span action_span = {(const unsigned char *)action, sizeof action - 1};
    cf_span target_span = {(const unsigned char *)target, sizeof target - 1};

    /* The fixed markup overhead, measured through the real call. */
    cf_builder tag = {0};
    CF_REQUIRE(cf_broadcast_action_tag(&tag, action_span, true, target_span,
                                       true, (cf_span){NULL, 0}, true) == CF_OK);
    size_t overhead = tag.len;
    cf_builder_dispose(&tag);
    CF_REQUIRE(overhead < CF_VIEWS_MAX_OUTPUT);

    size_t fit = CF_VIEWS_MAX_OUTPUT - overhead; /* reaches the cap exactly */
    size_t over_entry = (size_t)CF_VIEWS_MAX_OUTPUT + 1;
    unsigned char *filler = malloc(over_entry);
    CF_REQUIRE(filler != NULL);
    memset(filler, 'x', over_entry);

    CF_REQUIRE(cf_broadcast_action_tag(&tag, action_span, true, target_span,
                                       true, (cf_span){filler, fit}, true) ==
               CF_OK);
    CF_CHECK(tag.len == CF_VIEWS_MAX_OUTPUT);
    cf_builder_dispose(&tag);

    /* One byte more: CF_LIMIT and the entry bytes (none) are kept. */
    CF_CHECK(cf_broadcast_action_tag(&tag, action_span, true, target_span, true,
                                     (cf_span){filler, fit + 1}, true) ==
             CF_LIMIT);
    CF_CHECK(tag.len == 0);
    cf_builder_dispose(&tag);

    /* A builder already at (or over) the cap takes no further bytes and is
     * left byte-identical: no unsigned underflow in the room arithmetic. */
    CF_REQUIRE(cf_broadcast_action_tag(&tag, action_span, true, target_span,
                                       true, (cf_span){filler, fit}, true) ==
               CF_OK);
    CF_CHECK(tag.len == CF_VIEWS_MAX_OUTPUT);
    CF_CHECK(cf_broadcast_action_tag(&tag, action_span, true, target_span, true,
                                     (cf_span){(const unsigned char *)"x", 1},
                                     true) == CF_LIMIT);
    CF_CHECK(tag.len == CF_VIEWS_MAX_OUTPUT);
    cf_builder_dispose(&tag);
    CF_REQUIRE(cf_builder_append(&tag, (cf_span){filler, over_entry}) == CF_OK);
    CF_CHECK(tag.len == over_entry);
    CF_CHECK(cf_broadcast_action_tag(&tag, action_span, true, target_span, true,
                                     (cf_span){(const unsigned char *)"x", 1},
                                     true) == CF_LIMIT);
    CF_CHECK(tag.len == over_entry);
    cf_builder_dispose(&tag);
    free(filler);

    /* Attribute values are counted escaped: each '&' occupies 5 bytes
     * (&amp;), so one more '&' crosses an exactly full builder. */
    size_t k = 1000;
    unsigned char *amps = malloc(k + 1);
    CF_REQUIRE(amps != NULL);
    memset(amps, '&', k + 1);
    cf_span amps_k = {amps, k};
    cf_span amps_k1 = {amps, k + 1};

    CF_REQUIRE(cf_broadcast_action_tag(&tag, amps_k, false, (cf_span){NULL, 0},
                                       true, (cf_span){NULL, 0}, false) == CF_OK);
    size_t amps_len = tag.len;
    cf_builder_dispose(&tag);
    CF_REQUIRE(cf_broadcast_action_tag(&tag, amps_k1, false, (cf_span){NULL, 0},
                                       true, (cf_span){NULL, 0}, false) == CF_OK);
    CF_CHECK(tag.len == amps_len + 5);
    cf_builder_dispose(&tag);

    size_t room = CF_VIEWS_MAX_OUTPUT - amps_len;
    unsigned char *pad = malloc(room);
    CF_REQUIRE(pad != NULL);
    memset(pad, 'x', room);
    CF_REQUIRE(cf_broadcast_action_tag(&tag, amps_k, false, (cf_span){NULL, 0},
                                       true, (cf_span){pad, room}, false) == CF_OK);
    CF_CHECK(tag.len == CF_VIEWS_MAX_OUTPUT);
    cf_builder_dispose(&tag);
    CF_CHECK(cf_broadcast_action_tag(&tag, amps_k1, false, (cf_span){NULL, 0},
                                     true, (cf_span){pad, room}, false) ==
             CF_LIMIT);
    CF_CHECK(tag.len == 0);
    cf_builder_dispose(&tag);
    free(pad);
    free(amps);
}

/* An over-cap broadcast fails cleanly with CF_LIMIT at the builder: no frame
 * reaches the loop, the rejected build is never counted as published or
 * delivered (a build drop is surfaced to the caller, the same post-commit
 * failure policy as CF_BUSY), and under-cap broadcasts on the same connection
 * keep working. */
CF_TEST(over_cap_broadcast_is_dropped_without_publishing) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));

    chan_conn kevin;
    CF_REQUIRE(chan_connect(&f, CHAN_KEVIN, &kevin));
    cf_room designers = load_room(&f, CHAN_DESIGNERS);
    char messages_id[1024];
    subscribe_room_messages(&f, &kevin, &designers, messages_id,
                            sizeof messages_id);

    cf_cable_stats before;
    cf_cable_stats_get(f.cable, &before);

    size_t oversized = (size_t)CF_VIEWS_MAX_OUTPUT + 1;
    cf_broadcast_partials partials;
    memset(&partials, 0, sizeof partials);
    partials.message_presentation = cap_filler_presentation;
    partials.user = &oversized;

    cf_message first = load_message(&f, 100);
    CF_REQUIRE(cf_broadcast_message_replace(f.cable, &designers, &first,
                                            &partials) == CF_LIMIT);

    /* Dropped at the builder: nothing on the wire, no accounting drift. */
    CF_CHECK(chan_silent(&kevin, 300));
    cf_cable_stats after;
    cf_cable_stats_get(f.cable, &after);
    CF_CHECK(after.published == before.published);
    CF_CHECK(after.delivered == before.delivered);
    CF_CHECK(after.dropped == before.dropped);

    /* The same connection's under-cap path is unaffected. */
    cf_broadcast_partials fake;
    chan_fake_partials(&fake);
    CF_REQUIRE(cf_broadcast_message_replace(f.cable, &designers, &first, &fake) ==
               CF_OK);
    expect_turbo(&kevin, messages_id,
                 "<turbo-stream maintain_scroll=\"true\" action=\"replace\" "
                 "target=\"presentation_message_0001\"><template>"
                 "<div>presentation 100 & more</div>"
                 "</template></turbo-stream>",
                 3000);
    cf_cable_stats_get(f.cable, &after);
    CF_CHECK(after.published == before.published + 1);
    CF_CHECK(after.delivered == before.delivered + 1);
    CF_CHECK(after.dropped == before.dropped);

    cf_message_dispose(&first);
    cf_room_dispose(&designers);
    chan_disconnect(&kevin);
    chan_fixture_close(&f);
}

CF_TEST_MAIN()
