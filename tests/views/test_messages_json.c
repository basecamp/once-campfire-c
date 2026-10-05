/* V-F bot JSON views: field-order-exact serialization cases.
 *
 * Reference oracles:
 *  - tmp/rust-ref/crates/views/src/messages/json.rs (Jbuilder field order:
 *    `messages/_message.json`, `messages/by_bots/{index,show}.json`,
 *    `messages/boosts/_boost.json`, `messages/boosts/by_bots/show.json`).
 *  - tmp/rails-ref/app/views/messages/{_message,by_bots/index,by_bots/show,
 *    boosts/_boost,boosts/by_bots/show}.json.jbuilder and
 *    app/views/users/_user.json.jbuilder (no h() call: raw columns into the
 *    JSON encoder).
 *  - tmp/rust-ref/crates/views/src/messages/support.rs json_time
 *    (`2026-09-26T12:26:46.848Z`: milliseconds, truncated).
 *  - tmp/rust-ref/crates/rails_compat/src/json.rs (ActiveSupport
 *    escape_html_entities: `<>&` as lowercase-hex \u escapes, `/` and
 *    non-ASCII untouched, U+2028/U+2029 not escaped).
 *  - 03-application.md A00 ("URL helpers ... use PUBLIC_ORIGIN") and D-C07
 *    for the absolute-URL rule.
 *
 * The avatar_url tokens below are deterministic for this fixture (fixed
 * SECRET_KEY_BASE and fixed user updated_at values); they pin the exact
 * bytes including the signed token.  Cases that construct rows in memory
 * (epoch/negative timestamps, gone creator, invalid UTF-8) exercise the
 * public functions' time formatting and failure contracts, not the database.
 *
 * This binary contains its own CF_TEST_MAIN(): it links the application
 * library the way the other unit test binaries do (Makefile generic
 * $(TESTS_DIR)/% rule plus src/views/messages_json.c, which the integrator
 * wires into APP_LIB_SRCS/VIEWS lists).
 */
#include "cf_test.h"

#include "app.h"
#include "config.h"
#include "context.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "models/boost.h"
#include "models/message.h"
#include "models/room.h"
#include "models/user.h"
#include "richtext.h"
#include "views.h"

/* V-F shared serializers (shim signatures in
 * src/actions/messages/by_bots.c and src/actions/messages/boosts/by_bots.c,
 * exact).  Declared here until the integrator lands them in views.h. */
cf_err cf_views_message_json(cf_ctx *, const cf_message *, cf_builder *out);
cf_err cf_views_boost_json(cf_ctx *, const cf_boost *, const cf_message *,
                           cf_builder *out);
cf_err cf_views_absolute_url(cf_ctx *, cf_span path, cf_builder *out);

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define ORIGIN "http://campfire.test"

/* Byte-exact expectations captured from the reference semantics (field
 * order per the Jbuilder templates; json_time truncation; ActiveSupport
 * \u escaping with no h() pass). */
static const char *WANT_msg100 = "{\"id\":100,\"created_at\":\"2026-09-26T13:01:00.000Z\",\"body\":{\"plain_text\":\"Hello world\",\"html\":\"\\u003cdiv class=\\\"lexxy-content\\\"\\u003e\\n  \\u003cdiv\\u003eHello \\u003cstrong\\u003eworld\\u003c/strong\\u003e\\u003c/div\\u003e\\n\\u003c/div\\u003e\\n\"},\"creator\":{\"id\":1,\"name\":\"David\",\"role\":\"administrator\",\"avatar_url\":\"http://campfire.test/users/eyJfcmFpbHMiOnsiZGF0YSI6MSwicHVyIjoidXNlci9hdmF0YXIifX0--135fa084cce0ec78e1a8d00abe9737ed982bfe63fc7fa9f2b0d636b05b550483/avatar?v=20260926130020\"},\"room\":{\"id\":10},\"url\":\"http://campfire.test/rooms/10/messages/100\"}";
static const char *WANT_msg102 = "{\"id\":102,\"created_at\":\"2026-09-26T12:26:46.848Z\",\"body\":{\"plain_text\":\"frac\",\"html\":\"\\u003cdiv class=\\\"lexxy-content\\\"\\u003e\\n  frac\\n\\u003c/div\\u003e\\n\"},\"creator\":{\"id\":2,\"name\":\"JZ\",\"role\":\"member\",\"avatar_url\":\"http://campfire.test/users/eyJfcmFpbHMiOnsiZGF0YSI6MiwicHVyIjoidXNlci9hdmF0YXIifX0--6612e0473d1902a58177f87a967e645347d07280c9806207b7807ed69ca05f02/avatar?v=20260926130020\"},\"room\":{\"id\":10},\"url\":\"http://campfire.test/rooms/10/messages/102\"}";
static const char *WANT_msg105 = "{\"id\":105,\"created_at\":\"2026-09-26T13:05:00.000Z\",\"body\":{\"plain_text\":\"\",\"html\":\"\"},\"creator\":{\"id\":3,\"name\":\"\\u003cHelper\\u003e \\u0026 \\\"co\\\"\",\"role\":\"bot\",\"avatar_url\":\"http://campfire.test/users/eyJfcmFpbHMiOnsiZGF0YSI6MywicHVyIjoidXNlci9hdmF0YXIifX0--7a774bc875f5dbdad26e12c0221447bc9bad883df95c7a0131ea622d6326e0a7/avatar?v=20260926130020\"},\"room\":{\"id\":10},\"url\":\"http://campfire.test/rooms/10/messages/105\"}";
static const char *WANT_msg106 = "{\"id\":106,\"created_at\":\"2026-09-26T13:06:00.000Z\",\"body\":{\"plain_text\":\"moon.jpg\",\"html\":\"\"},\"creator\":{\"id\":1,\"name\":\"David\",\"role\":\"administrator\",\"avatar_url\":\"http://campfire.test/users/eyJfcmFpbHMiOnsiZGF0YSI6MSwicHVyIjoidXNlci9hdmF0YXIifX0--135fa084cce0ec78e1a8d00abe9737ed982bfe63fc7fa9f2b0d636b05b550483/avatar?v=20260926130020\"},\"room\":{\"id\":10},\"url\":\"http://campfire.test/rooms/10/messages/106\"}";
static const char *WANT_msg108 = "{\"id\":108,\"created_at\":\"1970-01-01T00:00:00.000Z\",\"body\":{\"plain_text\":\"\",\"html\":\"\"},\"creator\":{\"id\":3,\"name\":\"\\u003cHelper\\u003e \\u0026 \\\"co\\\"\",\"role\":\"bot\",\"avatar_url\":\"http://campfire.test/users/eyJfcmFpbHMiOnsiZGF0YSI6MywicHVyIjoidXNlci9hdmF0YXIifX0--7a774bc875f5dbdad26e12c0221447bc9bad883df95c7a0131ea622d6326e0a7/avatar?v=20260926130020\"},\"room\":{\"id\":10},\"url\":\"http://campfire.test/rooms/10/messages/108\"}";
static const char *WANT_msg109 = "{\"id\":109,\"created_at\":\"1969-12-31T23:59:59.500Z\",\"body\":{\"plain_text\":\"\",\"html\":\"\"},\"creator\":{\"id\":3,\"name\":\"\\u003cHelper\\u003e \\u0026 \\\"co\\\"\",\"role\":\"bot\",\"avatar_url\":\"http://campfire.test/users/eyJfcmFpbHMiOnsiZGF0YSI6MywicHVyIjoidXNlci9hdmF0YXIifX0--7a774bc875f5dbdad26e12c0221447bc9bad883df95c7a0131ea622d6326e0a7/avatar?v=20260926130020\"},\"room\":{\"id\":10},\"url\":\"http://campfire.test/rooms/10/messages/109\"}";
static const char *WANT_boost30 = "{\"id\":30,\"content\":\"\xF0\x9F\x8E\x89\",\"created_at\":\"2026-09-26T13:03:00.000Z\",\"booster\":{\"id\":2,\"name\":\"JZ\",\"role\":\"member\",\"avatar_url\":\"http://campfire.test/users/eyJfcmFpbHMiOnsiZGF0YSI6MiwicHVyIjoidXNlci9hdmF0YXIifX0--6612e0473d1902a58177f87a967e645347d07280c9806207b7807ed69ca05f02/avatar?v=20260926130020\"},\"message\":{\"id\":100,\"url\":\"http://campfire.test/rooms/10/messages/100\"}}";
static const char *WANT_boost31 = "{\"id\":31,\"content\":\"a\\u003cb\\u003e\\u0026\\\"c\\\"\",\"created_at\":\"2026-09-26T13:04:00.500Z\",\"booster\":{\"id\":3,\"name\":\"\\u003cHelper\\u003e \\u0026 \\\"co\\\"\",\"role\":\"bot\",\"avatar_url\":\"http://campfire.test/users/eyJfcmFpbHMiOnsiZGF0YSI6MywicHVyIjoidXNlci9hdmF0YXIifX0--7a774bc875f5dbdad26e12c0221447bc9bad883df95c7a0131ea622d6326e0a7/avatar?v=20260926130020\"},\"message\":{\"id\":100,\"url\":\"http://campfire.test/rooms/10/messages/100\"}}";
static const char *WANT_abs = "http://campfire.test/rooms/1/messages/2";

static cf_config *make_config(void) {
    cf_config_entry entries[2] = {
        {"PUBLIC_ORIGIN", ORIGIN},
        {"SECRET_KEY_BASE", HEX64},
    };
    cf_config *config = NULL;
    CF_REQUIRE(cf_config_parse(entries, 2, NULL, &config) == CF_OK);
    return config;
}

static cf_app *make_app(void) {
    cf_config *config = make_config();
    cf_app *app = NULL;
    CF_REQUIRE(cf_app_create(config, &app) == CF_OK);
    return app;
}

static void exec_sql(cf_db *db, const char *sql) {
    char *message = NULL;
    int rc = sqlite3_exec(cf_db_handle(db), sql, NULL, NULL, &message);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  sql failed: %s\n  %s\n",
                message != NULL ? message : "?", sql);
        sqlite3_free(message);
    }
    CF_REQUIRE(rc == SQLITE_OK);
}

typedef struct {
    cf_app *app;
    cf_response response;
    cf_request request;
    cf_ctx ctx;
} fixture;

static void fixture_init(fixture *f, cf_db *db) {
    memset(f, 0, sizeof *f);
    f->app = make_app();
    f->request.method = CF_GET;
    f->request.path = (cf_span){(const unsigned char *)"/rooms/10", 9};
    f->request.target = f->request.path;
    cf_response_init(&f->response);
    CF_REQUIRE(cf_ctx_create(&f->ctx, f->app, db, &f->request,
                             &f->response) == CF_OK);
}

static void fixture_dispose(fixture *f) {
    cf_ctx_destroy(&f->ctx);
    cf_response_dispose(&f->response);
    cf_app_destroy(f->app);
}

/* Room 10 (HQ) with David (1, administrator), JZ (2, member) and a bot (3,
 * `<Helper> & "co"`); text message 100, fractional-time message 102, bodiless
 * message 105, attachment message 106, and boosts 30/31. */
static void seed(cf_db *db) {
    exec_sql(db,
             "INSERT INTO accounts (id, created_at, custom_styles, join_code, "
             "name, settings, singleton_guard, updated_at) VALUES "
             "(1, '2026-09-26 13:00:20', NULL, 'CRMu-l8Ge-KB9B', "
             "'37signals', NULL, 0, '2026-09-26 13:00:20')");
    exec_sql(db,
             "INSERT INTO users (id, created_at, email_address, name, role, "
             "status, updated_at) VALUES "
             "(1, '2026-09-26 13:00:20', 'david@37signals.com', "
             "'David', 1, 0, '2026-09-26 13:00:20')");
    exec_sql(db,
             "INSERT INTO users (id, created_at, email_address, name, role, "
             "status, updated_at) VALUES "
             "(2, '2026-09-26 13:00:20', 'jz@37signals.com', 'JZ', 0, "
             "0, '2026-09-26 13:00:20')");
    exec_sql(db,
             "INSERT INTO users (id, created_at, email_address, name, role, "
             "status, updated_at) VALUES "
             "(3, '2026-09-26 13:00:20', 'bot@example.com', "
             "'<Helper> & \"co\"', 2, 0, '2026-09-26 13:00:20')");
    exec_sql(db,
             "INSERT INTO rooms (id, created_at, creator_id, name, type, "
             "updated_at) VALUES (10, '2026-09-26 13:00:20', 1, 'HQ', "
             "'Rooms::Closed', '2026-09-26 13:00:20')");
    exec_sql(db,
             "INSERT INTO messages (id, client_message_id, created_at, "
             "creator_id, room_id, updated_at) VALUES "
             "(100, 'm1', '2026-09-26 13:01:00', 1, 10, "
             "'2026-09-26 13:01:00')");
    exec_sql(db,
             "INSERT INTO action_text_rich_texts (id, body, created_at, name, "
             "record_id, record_type, updated_at) VALUES "
             "(1, '<div>Hello <strong>world</strong></div>', "
             "'2026-09-26 13:01:00', 'body', 100, 'Message', "
             "'2026-09-26 13:01:00')");
    /* Sub-millisecond precision: json_time truncates to milliseconds. */
    exec_sql(db,
             "INSERT INTO messages (id, client_message_id, created_at, "
             "creator_id, room_id, updated_at) VALUES "
             "(102, 'm-frac', '2026-09-26 12:26:46.848999', 2, 10, "
             "'2026-09-26 12:26:46.848999')");
    exec_sql(db,
             "INSERT INTO action_text_rich_texts (id, body, created_at, name, "
             "record_id, record_type, updated_at) VALUES "
             "(5, 'frac', '2026-09-26 12:26:46', 'body', 102, 'Message', "
             "'2026-09-26 12:26:46')");
    /* No rich text row: the reference's unwrap_or_default rescue. */
    exec_sql(db,
             "INSERT INTO messages (id, client_message_id, created_at, "
             "creator_id, room_id, updated_at) VALUES "
             "(105, 'm-nobody', '2026-09-26 13:05:00', 3, 10, "
             "'2026-09-26 13:05:00')");
    /* Attachment arm: blob attached, no stored body text. */
    exec_sql(db,
             "INSERT INTO messages (id, client_message_id, created_at, "
             "creator_id, room_id, updated_at) VALUES "
             "(106, 'm-att', '2026-09-26 13:06:00', 1, 10, "
             "'2026-09-26 13:06:00')");
    exec_sql(db,
             "INSERT INTO active_storage_blobs (id, byte_size, checksum, "
             "content_type, created_at, filename, key, metadata, service_name) "
             "VALUES (1, 10, NULL, 'image/jpeg', '2026-09-26 "
             "13:00:20', 'moon.jpg', 'key1', NULL, 'local')");
    exec_sql(db,
             "INSERT INTO active_storage_attachments (id, blob_id, "
             "created_at, name, record_id, record_type) VALUES "
             "(1, 1, '2026-09-26 13:00:20', 'attachment', 106, "
             "'Message')");
    exec_sql(db,
             "INSERT INTO boosts (id, content, created_at, message_id, "
             "booster_id, updated_at) VALUES "
             "(30, '\xF0\x9F\x8E\x89', '2026-09-26 13:03:00', 100, 2, "
             "'2026-09-26 13:03:00')");
    exec_sql(db,
             "INSERT INTO boosts (id, content, created_at, message_id, "
             "booster_id, updated_at) VALUES "
             "(31, 'a<b>&\"c\"', '2026-09-26 13:04:00.5', 100, 3, "
             "'2026-09-26 13:04:00')");
}

static bool span_contains(const cf_builder *b, const char *needle) {
    size_t n = strlen(needle);
    if (n == 0) return true;
    if (b->ptr == NULL || b->len < n) return false;
    for (size_t i = 0; i + n <= b->len; i++) {
        if (memcmp(b->ptr + i, needle, n) == 0) return true;
    }
    return false;
}

static void expect_exact(cf_builder *got, const char *want,
                         const char *label) {
    size_t want_len = strlen(want);
    if (got->len == want_len && memcmp(got->ptr, want, want_len) == 0) return;
    fprintf(stderr, "    %s: got %zu byte(s):\n%.*s\n", label, got->len,
            (int)got->len, got->ptr ? (const char *)got->ptr : "");
    fprintf(stderr, "    %s: want %zu byte(s):\n%s\n", label, want_len, want);
    CF_CHECK(0 && "JSON bytes differ");
}

static void render_message(fixture *f, cf_db *db, int64_t id, cf_builder *out,
                           const char *want) {
    cf_message row = {0};
    CF_REQUIRE(cf_message_find(db, id, &row) == CF_OK);
    CF_REQUIRE(cf_views_message_json(&f->ctx, &row, out) == CF_OK);
    expect_exact(out, want, "message json");
    cf_message_dispose(&row);
}

CF_TEST(message_json_text_field_order_exact) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed(scratch.db);
    cf_richtext_configure((cf_span){(const unsigned char *)HEX64, 64});
    fixture f;
    fixture_init(&f, scratch.db);

    cf_builder out = {0};
    render_message(&f, scratch.db, 100, &out, WANT_msg100);
    cf_builder_dispose(&out);
    fixture_dispose(&f);
    cf_db_scratch_close(&scratch);
}

CF_TEST(message_json_time_truncates_submillis) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed(scratch.db);
    /* .848999 renders .848: truncation, never rounding (support.rs). */
    cf_message row = {0};
    CF_REQUIRE(cf_message_find(scratch.db, 102, &row) == CF_OK);
    CF_CHECK(row.created_at == 1790425606848999ll);
    cf_richtext_configure((cf_span){(const unsigned char *)HEX64, 64});
    fixture f;
    fixture_init(&f, scratch.db);

    cf_builder out = {0};
    CF_REQUIRE(cf_views_message_json(&f.ctx, &row, &out) == CF_OK);
    expect_exact(&out, WANT_msg102, "message json");
    cf_builder_dispose(&out);
    cf_message_dispose(&row);
    fixture_dispose(&f);
    cf_db_scratch_close(&scratch);
}

CF_TEST(message_json_epoch_and_negative_time_shapes) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed(scratch.db);
    cf_richtext_configure((cf_span){(const unsigned char *)HEX64, 64});
    fixture f;
    fixture_init(&f, scratch.db);

    cf_message base = {0};
    CF_REQUIRE(cf_message_find(scratch.db, 105, &base) == CF_OK);
    cf_message epoch = base;
    epoch.id = 108;
    epoch.created_at = 0;
    cf_builder out = {0};
    CF_REQUIRE(cf_views_message_json(&f.ctx, &epoch, &out) == CF_OK);
    expect_exact(&out, WANT_msg108, "message json");
    cf_builder_dispose(&out);

    /* -0.5 s floors to the previous second with a .500 fraction. */
    cf_message neg = base;
    neg.id = 109;
    neg.created_at = -500000;
    CF_REQUIRE(cf_views_message_json(&f.ctx, &neg, &out) == CF_OK);
    expect_exact(&out, WANT_msg109, "message json");
    cf_builder_dispose(&out);

    cf_message_dispose(&base);
    fixture_dispose(&f);
    cf_db_scratch_close(&scratch);
}

CF_TEST(message_json_missing_body_is_empty) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed(scratch.db);
    cf_richtext_configure((cf_span){(const unsigned char *)HEX64, 64});
    fixture f;
    fixture_init(&f, scratch.db);

    cf_builder out = {0};
    render_message(&f, scratch.db, 105, &out, WANT_msg105);
    /* The bot name is JSON-escaped, never h()-escaped: no entity remains. */
    CF_CHECK(!span_contains(&out, "&lt;"));
    CF_CHECK(!span_contains(&out, "&amp;"));
    CF_CHECK(span_contains(&out, "\\u003cHelper\\u003e"));
    cf_builder_dispose(&out);
    fixture_dispose(&f);
    cf_db_scratch_close(&scratch);
}

CF_TEST(message_json_attachment_arm) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed(scratch.db);
    cf_richtext_configure((cf_span){(const unsigned char *)HEX64, 64});
    fixture f;
    fixture_init(&f, scratch.db);

    /* The attachment arm keeps the Jbuilder shape: plain_text names the
     * file, html renders empty through the unwrap_or_default rescue. */
    cf_builder out = {0};
    render_message(&f, scratch.db, 106, &out, WANT_msg106);
    cf_builder_dispose(&out);
    fixture_dispose(&f);
    cf_db_scratch_close(&scratch);
}

static void render_boost(fixture *f, cf_db *db, int64_t boost_id,
                         int64_t message_id, cf_builder *out,
                         const char *want) {
    cf_boost boost = {0};
    CF_REQUIRE(cf_boost_find(db, boost_id, &boost) == CF_OK);
    cf_message message = {0};
    CF_REQUIRE(cf_message_find(db, message_id, &message) == CF_OK);
    CF_REQUIRE(cf_views_boost_json(&f->ctx, &boost, &message, out) == CF_OK);
    expect_exact(out, want, "boost json");
    cf_boost_dispose(&boost);
    cf_message_dispose(&message);
}

CF_TEST(boost_json_field_order_exact) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed(scratch.db);
    cf_richtext_configure((cf_span){(const unsigned char *)HEX64, 64});
    fixture f;
    fixture_init(&f, scratch.db);

    cf_builder out = {0};
    render_boost(&f, scratch.db, 30, 100, &out, WANT_boost30);
    /* Non-ASCII content passes through raw (no \u escape, no escaping of
     * U+2028/U+2029-style payloads by the writer). */
    CF_CHECK(span_contains(&out, "\xF0\x9F\x8E\x89"));
    cf_builder_dispose(&out);
    fixture_dispose(&f);
    cf_db_scratch_close(&scratch);
}

CF_TEST(boost_json_content_and_name_escaping) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed(scratch.db);
    cf_richtext_configure((cf_span){(const unsigned char *)HEX64, 64});
    fixture f;
    fixture_init(&f, scratch.db);

    cf_builder out = {0};
    render_boost(&f, scratch.db, 31, 100, &out, WANT_boost31);
    /* Half-second precision and the escaped content/name spellings. */
    CF_CHECK(span_contains(&out, "13:04:00.500Z"));
    CF_CHECK(span_contains(&out, "a\\u003cb\\u003e\\u0026\\\"c\\\""));
    cf_builder_dispose(&out);
    fixture_dispose(&f);
    cf_db_scratch_close(&scratch);
}

CF_TEST(absolute_url_uses_public_origin) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed(scratch.db);
    fixture f;
    fixture_init(&f, scratch.db);

    cf_builder out = {0};
    CF_REQUIRE(cf_views_absolute_url(
                   &f.ctx,
                   (cf_span){(const unsigned char *)"/rooms/1/messages/2", 19},
                   &out) == CF_OK);
    expect_exact(&out, WANT_abs, "absolute url");
    cf_builder_dispose(&out);
    /* Every payload URL in the message/boost documents shares the rule. */
    CF_CHECK(strncmp(WANT_msg100, "{\"id\":100", 9) == 0);
    CF_CHECK(strstr(WANT_msg100, "\"url\":\"http://campfire.test/rooms/") !=
             NULL);
    CF_CHECK(strstr(WANT_boost30,
                    "\"url\":\"http://campfire.test/rooms/") != NULL);
    fixture_dispose(&f);
    cf_db_scratch_close(&scratch);
}

CF_TEST(failures_leave_builder_unchanged) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed(scratch.db);
    cf_richtext_configure((cf_span){(const unsigned char *)HEX64, 64});
    fixture f;
    fixture_init(&f, scratch.db);

    cf_message row = {0};
    CF_REQUIRE(cf_message_find(scratch.db, 106, &row) == CF_OK);

    /* A gone creator (unreachable through FK writes) propagates NOT_FOUND
     * and leaves the caller's bytes alone, like the reference's `?`. */
    cf_message orphan = row;
    orphan.id = 107;
    orphan.creator_id = 999;
    cf_builder out = {0};
    CF_REQUIRE(cf_builder_append(&out, (cf_span){
        (const unsigned char *)"SENTINEL", 8}) == CF_OK);
    CF_CHECK(cf_views_message_json(&f.ctx, &orphan, &out) == CF_NOT_FOUND);
    CF_CHECK(out.len == 8);
    CF_CHECK(memcmp(out.ptr, "SENTINEL", 8) == 0);
    cf_builder_dispose(&out);

    /* Invalid UTF-8 never becomes invalid JSON. */
    cf_boost bad = {0};
    bad.id = 77;
    bad.message_id = 100;
    bad.booster_id = 2;
    bad.content.ptr = (char *)"\xff\xfe";
    bad.content.len = 2;
    CF_REQUIRE(cf_builder_append(&out, (cf_span){
        (const unsigned char *)"KEEP", 4}) == CF_OK);
    CF_CHECK(cf_views_boost_json(&f.ctx, &bad, &row, &out) == CF_INVALID);
    CF_CHECK(out.len == 4);
    CF_CHECK(memcmp(out.ptr, "KEEP", 4) == 0);
    cf_builder_dispose(&out);

    /* Contract gates. */
    CF_CHECK(cf_views_message_json(NULL, &row, &out) == CF_INVALID);
    CF_CHECK(cf_views_message_json(&f.ctx, NULL, &out) == CF_INVALID);
    CF_CHECK(cf_views_message_json(&f.ctx, &row, NULL) == CF_INVALID);
    CF_CHECK(cf_views_boost_json(NULL, &bad, &row, &out) == CF_INVALID);
    CF_CHECK(cf_views_absolute_url(NULL, (cf_span){NULL, 0}, &out) ==
             CF_INVALID);

    cf_message_dispose(&row);
    fixture_dispose(&f);
    cf_db_scratch_close(&scratch);
}

CF_TEST_MAIN()
