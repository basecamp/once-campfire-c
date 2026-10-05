/* tests/cable/cable_channels_testutil.h — C02 test fixture: a scratch
 * database with the reference channel fixtures, the application and the
 * cable, plus one real C01 WebSocket connection per fake client.
 *
 * Test-only; never part of the application. Clients speak the exact client
 * frame format through cable_testutil.h and read server frames back. */
#ifndef CABLE_CHANNELS_TESTUTIL_H
#define CABLE_CHANNELS_TESTUTIL_H

#include "cable_testutil.h"

#include "app.h"
#include "auth.h"
#include "cable/channels.h"
#include "config.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "richtext.h"

#define CHAN_SECRET \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

/* Fixture ids (the reference DB fixtures' labels). */
#define CHAN_JZ 1
#define CHAN_KEVIN 2
#define CHAN_DAVID 3
#define CHAN_BENDER 4
#define CHAN_DESIGNERS 10    /* closed; JZ, Kevin, David */
#define CHAN_WATERCOOLER 11  /* closed; JZ, David */
#define CHAN_DIRECT 12       /* direct; Kevin, Bender */

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    cf_cable *cable;
    cf_cable_hooks hooks;
    int session_seq;
} chan_fixture;

static inline bool chan_exec(cf_db *db, const char *sql) {
    return cf_db_test_exec(cf_db_handle(db), sql) == SQLITE_OK;
}

static inline bool chan_seed(cf_db *db) {
    return chan_exec(db,
                     "INSERT INTO users (id, created_at, email_address, name, "
                     "role, status, updated_at) VALUES "
                     "(1,'2026-01-01 00:00:00','jz@example.com','JZ',0,0,"
                     "'2026-01-01 00:00:00'),"
                     "(2,'2026-01-01 00:00:00','kevin@example.com','Kevin',0,0,"
                     "'2026-01-01 00:00:00'),"
                     "(3,'2026-01-01 00:00:00','david@example.com','David',0,0,"
                     "'2026-01-01 00:00:00'),"
                     "(4,'2026-01-01 00:00:00','bender@example.com','Bender',0,0,"
                     "'2026-01-01 00:00:00')") &&
           chan_exec(db,
                     "INSERT INTO rooms (id, created_at, creator_id, name, "
                     "type, updated_at) VALUES "
                     "(10,'2026-01-01 00:00:00',1,'Designers',"
                     "'Rooms::Closed','2026-01-01 00:00:00'),"
                     "(11,'2026-01-01 00:00:00',1,'Watercooler',"
                     "'Rooms::Closed','2026-01-01 00:00:00'),"
                     "(12,'2026-01-01 00:00:00',2,NULL,"
                     "'Rooms::Direct','2026-01-01 00:00:00')") &&
           chan_exec(db,
                     "INSERT INTO memberships (id, created_at, room_id, "
                     "updated_at, user_id) VALUES "
                     "(20,'2026-01-01 00:00:00',10,'2026-01-01 00:00:00',1),"
                     "(21,'2026-01-01 00:00:00',10,'2026-01-01 00:00:00',2),"
                     "(22,'2026-01-01 00:00:00',10,'2026-01-01 00:00:00',3),"
                     "(23,'2026-01-01 00:00:00',11,'2026-01-01 00:00:00',1),"
                     "(24,'2026-01-01 00:00:00',11,'2026-01-01 00:00:00',3),"
                     "(25,'2026-01-01 00:00:00',12,'2026-01-01 00:00:00',2),"
                     "(26,'2026-01-01 00:00:00',12,'2026-01-01 00:00:00',4)") &&
           chan_exec(db,
                     "INSERT INTO messages (id, client_message_id, created_at, "
                     "creator_id, room_id, updated_at) VALUES "
                     "(100,'0001','2026-01-01 00:00:01',1,10,"
                     "'2026-01-01 00:00:01'),"
                     "(101,'0002','2026-01-01 00:00:02',2,10,"
                     "'2026-01-01 00:00:02')") &&
           chan_exec(db,
                     "INSERT INTO boosts (id, created_at, message_id, "
                     "booster_id, content, updated_at) VALUES "
                     "(200,'2026-01-01 00:00:03',100,2,'🎉',"
                     "'2026-01-01 00:00:03')") &&
           chan_exec(db,
                     "INSERT INTO action_text_rich_texts (id, body, created_at, "
                     "name, record_id, record_type, updated_at) VALUES "
                     "(1, '<div>Hello <strong>world</strong></div>', "
                     "'2026-01-01 00:00:01', 'body', 100, 'Message', "
                     "'2026-01-01 00:00:01')");
}

static inline bool chan_fixture_open(chan_fixture *f) {
    memset(f, 0, sizeof *f);
    if (!cf_db_scratch_open(&f->scratch)) return false;
    if (!chan_seed(f->scratch.db)) return false;
    cf_config_entry entries[3] = {
        {"PUBLIC_ORIGIN", "http://campfire.test"},
        {"SECRET_KEY_BASE", CHAN_SECRET},
        {"DATABASE_PATH", f->scratch.path},
    };
    if (cf_config_parse(entries, 3, NULL, &f->config) != CF_OK) return false;
    cf_richtext_configure(
        (cf_span){(const unsigned char *)CHAN_SECRET, sizeof CHAN_SECRET - 1});
    if (cf_app_create(f->config, &f->app) != CF_OK) {
        f->config = NULL;
        return false;
    }
    f->config = NULL; /* owned by the app */
    if (cf_app_start(f->app) != CF_OK) return false;
    cf_cable_config cable_config;
    memset(&cable_config, 0, sizeof cable_config);
    cable_config.app = f->app;
    cable_config.database_path = f->scratch.path;
    if (cf_cable_create(&cable_config, &f->cable) != CF_OK) return false;
    cf_cable_server_hooks(f->cable, &f->hooks);
    return true;
}

static inline void chan_fixture_close(chan_fixture *f) {
    if (f->cable != NULL) cf_cable_destroy(f->cable);
    if (f->app != NULL) {
        cf_app_stop(f->app);
        cf_app_destroy(f->app);
    }
    f->cable = NULL;
    f->app = NULL;
    if (f->scratch.db != NULL) cf_db_scratch_close(&f->scratch);
}

/* A signed session_token cookie for `user_id` (A01). Percent-encodes the
 * signed value as a browser would. */
static inline void chan_cookie(chan_fixture *f, int64_t user_id, char *out,
                               size_t cap) {
    char token[64];
    snprintf(token, sizeof token, "session-%lld-%d", (long long)user_id,
             f->session_seq++);
    char sql[256];
    snprintf(sql, sizeof sql,
             "INSERT INTO sessions (created_at,last_active_at,token,"
             "updated_at,user_id) VALUES "
             "('2026-01-01 00:00:00','2026-01-01 00:00:00','%s',"
             "'2026-01-01 00:00:00',%lld)",
             token, (long long)user_id);
    CF_REQUIRE(chan_exec(f->scratch.db, sql));
    cf_str signed_value = {0};
    CF_REQUIRE(cf_auth_signed_cookie_generate(
                   (cf_span){(const unsigned char *)CHAN_SECRET,
                             sizeof CHAN_SECRET - 1},
                   (cf_span){(const unsigned char *)"session_token", 13},
                   (cf_span){(const unsigned char *)token, strlen(token)},
                   false, 0, &signed_value) == CF_OK);
    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, cap - pos, "session_token=");
    for (size_t i = 0; i < signed_value.len && pos + 4 < cap; i++) {
        unsigned char c = (unsigned char)signed_value.ptr[i];
        if (c == '+') {
            pos += (size_t)snprintf(out + pos, cap - pos, "%%2B");
        } else if (c == '/') {
            pos += (size_t)snprintf(out + pos, cap - pos, "%%2F");
        } else if (c == '=') {
            pos += (size_t)snprintf(out + pos, cap - pos, "%%3D");
        } else {
            out[pos++] = (char)c;
        }
    }
    out[pos < cap ? pos : cap - 1] = '\0';
    cf_str_dispose(&signed_value);
}

typedef struct {
    ct_run run;
    cf_cable_request request;
    cf_header headers[1];
    char cookie[1024];
} chan_conn;

static inline bool chan_connect(chan_fixture *f, int64_t user_id,
                                chan_conn *c) {
    memset(c, 0, sizeof *c);
    chan_cookie(f, user_id, c->cookie, sizeof c->cookie);
    c->headers[0].name = (cf_span){(const unsigned char *)"Cookie", 6};
    c->headers[0].value =
        (cf_span){(const unsigned char *)c->cookie, strlen(c->cookie)};
    c->request.method = CF_GET;
    c->request.headers = c->headers;
    c->request.header_count = 1;
    if (!ct_run_start_with_request(&c->run, &f->hooks, NULL, false,
                                   &c->request)) {
        return false;
    }
    unsigned opcode = 0;
    bool rsv1 = false;
    unsigned char payload[256];
    ssize_t n = ct_read_server_frame(c->run.peer_fd, &opcode, &rsv1, payload,
                                     sizeof payload, 3000);
    if (n != 18 || memcmp(payload, "{\"type\":\"welcome\"}", 18) != 0) {
        return false;
    }
    return true;
}

/* The next non-ping text frame; false on timeout/close/error. */
static inline bool chan_next(chan_conn *c, char *buf, size_t cap,
                             int timeout_ms) {
    int64_t deadline = ct_now_ms() + timeout_ms;
    for (;;) {
        int left = (int)(deadline - ct_now_ms());
        if (left <= 0) return false;
        unsigned opcode = 0;
        bool rsv1 = false;
        ssize_t n = ct_read_server_frame(c->run.peer_fd, &opcode, &rsv1,
                                         (unsigned char *)buf, cap - 1, left);
        if (n < 0) return false;
        buf[n] = '\0';
        if (opcode != 0x1) return false;
        if (strncmp(buf, "{\"type\":\"ping\"", 14) == 0) continue;
        return true;
    }
}

static inline bool chan_expect(chan_conn *c, const char *expected,
                               int timeout_ms) {
    char got[8192];
    if (!chan_next(c, got, sizeof got, timeout_ms)) {
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

/* Nothing arrives (pings aside) within the window. */
static inline bool chan_silent(chan_conn *c, int timeout_ms) {
    char got[8192];
    if (chan_next(c, got, sizeof got, timeout_ms)) {
        fprintf(stderr, "  unexpected frame: %s\n", got);
        return false;
    }
    return true;
}

/* JSON-escape as Active Support does (what cf_json_string writes). */
static inline void chan_json_string(char *out, size_t cap, const char *text) {
    size_t pos = 0;
    if (pos + 1 < cap) out[pos++] = '"';
    for (const unsigned char *p = (const unsigned char *)text;
         *p != '\0' && pos + 7 < cap; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            out[pos++] = '\\';
            out[pos++] = (char)c;
        } else if (c == '<') {
            pos += (size_t)snprintf(out + pos, cap - pos, "\\u003c");
        } else if (c == '>') {
            pos += (size_t)snprintf(out + pos, cap - pos, "\\u003e");
        } else if (c == '&') {
            pos += (size_t)snprintf(out + pos, cap - pos, "\\u0026");
        } else {
            out[pos++] = (char)c;
        }
    }
    if (pos + 1 < cap) out[pos++] = '"';
    out[pos < cap ? pos : cap - 1] = '\0';
}

static inline bool chan_send_text(chan_conn *c, const char *text) {
    return ct_send_client_frame(c->run.peer_fd, 0x1, true, false,
                                (const unsigned char *)text, strlen(text));
}

/* {"command":"<command>","identifier":<raw JSON>} */
static inline bool chan_send_identifier_command(chan_conn *c,
                                                const char *command,
                                                const char *identifier) {
    char quoted[8192];
    chan_json_string(quoted, sizeof quoted, identifier);
    char cmd[9216];
    snprintf(cmd, sizeof cmd,
             "{\"command\":\"%s\",\"identifier\":%s}", command, quoted);
    return chan_send_text(c, cmd);
}

static inline bool chan_subscribe(chan_conn *c, const char *identifier) {
    return chan_send_identifier_command(c, "subscribe", identifier);
}

static inline bool chan_unsubscribe(chan_conn *c, const char *identifier) {
    return chan_send_identifier_command(c, "unsubscribe", identifier);
}

/* {"command":"message","identifier":<raw>,"data":"<data JSON>"} */
static inline bool chan_perform(chan_conn *c, const char *identifier,
                                const char *data_json) {
    char quoted[8192];
    chan_json_string(quoted, sizeof quoted, identifier);
    char data_quoted[8192];
    chan_json_string(data_quoted, sizeof data_quoted, data_json);
    char cmd[18432];
    snprintf(cmd, sizeof cmd,
             "{\"command\":\"message\",\"identifier\":%s,\"data\":%s}", quoted,
             data_quoted);
    return chan_send_text(c, cmd);
}

static inline void chan_confirm_expected(char *out, size_t cap,
                                         const char *identifier) {
    char quoted[8192];
    chan_json_string(quoted, sizeof quoted, identifier);
    snprintf(out, cap, "{\"identifier\":%s,\"type\":\"confirm_subscription\"}",
             quoted);
}

static inline void chan_reject_expected(char *out, size_t cap,
                                        const char *identifier) {
    char quoted[8192];
    chan_json_string(quoted, sizeof quoted, identifier);
    snprintf(out, cap, "{\"identifier\":%s,\"type\":\"reject_subscription\"}",
             quoted);
}

/* The delivery frame a broadcast payload arrives in. */
static inline void chan_delivery_expected(char *out, size_t cap,
                                          const char *identifier,
                                          const char *encoded_message) {
    char quoted[8192];
    chan_json_string(quoted, sizeof quoted, identifier);
    snprintf(out, cap, "{\"identifier\":%s,\"message\":%s}", quoted,
             encoded_message);
}

static inline bool chan_confirm(chan_conn *c, const char *identifier) {
    char expected[10240];
    chan_confirm_expected(expected, sizeof expected, identifier);
    return chan_expect(c, expected, 3000);
}

static inline bool chan_reject(chan_conn *c, const char *identifier) {
    char expected[10240];
    chan_reject_expected(expected, sizeof expected, identifier);
    return chan_expect(c, expected, 3000);
}

/* Close the connection's WebSocket and join the owner thread (its exit
 * detaches the loop, running unsubscribe effects). */
static inline void chan_disconnect(chan_conn *c) {
    if (c->run.peer_fd < 0) return;
    (void)ct_send_client_frame(c->run.peer_fd, 0x8, true, false, NULL, 0);
    unsigned opcode = 0;
    bool rsv1 = false;
    unsigned char payload[256];
    for (int i = 0; i < 4; i++) {
        ssize_t n = ct_read_server_frame(c->run.peer_fd, &opcode, &rsv1,
                                         payload, sizeof payload, 1000);
        if (n < 0 || opcode == 0x8) break;
    }
    ct_run_join(&c->run);
    close(c->run.fd);
    close(c->run.peer_fd);
    c->run.fd = -1;
    c->run.peer_fd = -1;
}

/* ---- stand-in partials (the reference tests' FakePartials) ------------------ */

static inline cf_err chan_fake_message(void *user, const cf_message *message,
                                       cf_builder *out) {
    (void)user;
    char text[320];
    int n = snprintf(text, sizeof text,
                     "<div id=\"message_%s\">message %lld</div>",
                     message->client_message_id.ptr, (long long)message->id);
    if (n < 0) return CF_INTERNAL;
    return cf_builder_append(
        out, (cf_span){(const unsigned char *)text, (size_t)n});
}

static inline cf_err chan_fake_presentation(void *user,
                                            const cf_message *message,
                                            cf_builder *out) {
    (void)user;
    char text[160];
    int n = snprintf(text, sizeof text,
                     "<div>presentation %lld & more</div>",
                     (long long)message->id);
    if (n < 0) return CF_INTERNAL;
    return cf_builder_append(
        out, (cf_span){(const unsigned char *)text, (size_t)n});
}

static inline cf_err chan_fake_boost(void *user, const cf_boost *boost,
                                     cf_builder *out) {
    (void)user;
    char text[96];
    int n = snprintf(text, sizeof text, "<div>boost %lld</div>",
                     (long long)boost->id);
    if (n < 0) return CF_INTERNAL;
    return cf_builder_append(
        out, (cf_span){(const unsigned char *)text, (size_t)n});
}

static inline cf_err chan_fake_shared_room(void *user, const cf_room *room,
                                           cf_builder *out) {
    (void)user;
    char text[96];
    int n = snprintf(text, sizeof text, "<li>shared %lld</li>",
                     (long long)room->id);
    if (n < 0) return CF_INTERNAL;
    return cf_builder_append(
        out, (cf_span){(const unsigned char *)text, (size_t)n});
}

static inline cf_err chan_fake_direct_room(void *user,
                                           const cf_membership *membership,
                                           cf_builder *out) {
    (void)user;
    char text[96];
    int n = snprintf(text, sizeof text, "<li>direct %lld</li>",
                     (long long)membership->id);
    if (n < 0) return CF_INTERNAL;
    return cf_builder_append(
        out, (cf_span){(const unsigned char *)text, (size_t)n});
}

static inline void chan_fake_partials(cf_broadcast_partials *out) {
    memset(out, 0, sizeof *out);
    out->message = chan_fake_message;
    out->message_presentation = chan_fake_presentation;
    out->boost = chan_fake_boost;
    out->shared_room = chan_fake_shared_room;
    out->direct_room = chan_fake_direct_room;
}

/* Sign a stream name the way the room page does (A01 turbo verifier). */
static inline void chan_signed_stream(const chan_fixture *f, const cf_span *parts,
                                      size_t count, char *out, size_t cap) {
    (void)f;
    cf_str signed_name = {0};
    CF_REQUIRE(cf_auth_turbo_signed_stream_name(
                   (cf_span){(const unsigned char *)CHAN_SECRET,
                             sizeof CHAN_SECRET - 1},
                   parts, count, &signed_name) == CF_OK);
    snprintf(out, cap, "%.*s", (int)signed_name.len, signed_name.ptr);
    cf_str_dispose(&signed_name);
}

/* Identifier builders. */
static inline void chan_room_identifier(char *out, size_t cap,
                                        const char *channel, int64_t room_id) {
    snprintf(out, cap, "{\"channel\":\"%s\",\"room_id\":%lld}", channel,
             (long long)room_id);
}

static inline void chan_channel_identifier(char *out, size_t cap,
                                           const char *channel) {
    snprintf(out, cap, "{\"channel\":\"%s\"}", channel);
}

/* The stream name a room channel streams from: "<channel name>:<gid param>"
 * (broadcasting_for). */
static inline void chan_room_stream(chan_fixture *f, const char *channel,
                                    int64_t room_id, char *out, size_t cap) {
    cf_room room = {0};
    bool found = false;
    CF_REQUIRE(cf_room_find_by_id(f->scratch.db, room_id, &found, &room) ==
               CF_OK);
    CF_REQUIRE(found);
    cf_str gid = {0};
    CF_REQUIRE(cf_cable_room_gid_param(&room, &gid) == CF_OK);
    cf_builder name = {0};
    const char *suffix = "Channel";
    size_t channel_len = strlen(channel);
    if (channel_len > 7 &&
        strcmp(channel + channel_len - 7, suffix) == 0) {
        channel_len -= 7;
    }
    for (size_t i = 0; i < channel_len; i++) {
        char c = channel[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        CF_REQUIRE(cf_builder_append(&name, (cf_span){(const unsigned char *)&c,
                                                      1}) == CF_OK);
    }
    CF_REQUIRE(cf_builder_append(
                   &name, (cf_span){(const unsigned char *)":", 1}) == CF_OK);
    CF_REQUIRE(cf_builder_append(
                   &name,
                   (cf_span){(const unsigned char *)gid.ptr, gid.len}) == CF_OK);
    snprintf(out, cap, "%.*s", (int)name.len, (const char *)name.ptr);
    cf_builder_dispose(&name);
    cf_str_dispose(&gid);
    cf_room_dispose(&room);
}

#endif /* CABLE_CHANNELS_TESTUTIL_H */
