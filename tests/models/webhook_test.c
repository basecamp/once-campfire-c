/* D01 model family "webhook" tests.
 *
 * Reference oracle: tmp/rust-ref/crates/db/src/models/webhook.rs (5
 * functions) and its fixture coverage in
 * tests/fixtures/crates/db/src/tests/user_test.rs:
 *  - webhook_payload: bender's webhook, message "first" by Jason in
 *    "Designers", body "First post!" and the two route-helper paths;
 *  - webhook_payload_escapes_html_entities: room name NULL, body
 *    "<p>Tom & Jerry</p>" -> html "<p>Tom & Jerry</p>";
 *  - create_bot_with_webhook: find_by_user missing after the webhook is
 *    removed.
 * Fixture rows below are the Rails FixtureSet shapes (ids are
 * crc32(label) % (2^30 - 1) as fixtures.rs::identify):
 *   users jason=149087659, bender=394959859, david=127326141;
 *   rooms designers=654632876; messages first=309456473.
 *
 * Payload coverage note: every payload case here is body-less, covering
 * payload's Option::None html branch, the null room name branch and the
 * attachment-filename plain-text branch (message.rs plain_text_body never
 * consults rich text when body_html is None). R02's pipeline now exists
 * (src/richtext.h), so wh_rich_text() threads the production singleton
 * cf_tx_rich_text(NULL) — the same value the app wires — even though these
 * body-less cases never reach it. The stored-body oracle case remains
 * deferred in docs/devel/evidence/D01-model-webhook.md.
 *
 * Build (plain; intended full link once the sibling model files land):
 *   clang -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra
 *         -Werror -pthread -Isrc -Itests
 *         -Ivendor/src/sqlite/sqlite-amalgamation-3530400
 *         src/models/webhook.c src/models/types.c src/models/message.c
 *         src/models/room.c src/models/user.c src/models/rich_text_record.c
 *         src/models/active_storage.c src/models/boost.c src/models/sound.c
 *         tests/models/webhook_test.c
 *         src/core/alloc.c src/core/buffer.c src/core/clock.c
 *         src/core/error.c src/core/random.c src/config.c src/app.c
 *         src/db/schema.c src/db/reader.c src/db/statements.c src/db/writer.c
 *         src/views/escape.c
 *         vendor/build/sqlite-clang/libsqlite3.a -lm
 *         -o build/d01-model-webhook/plain/test_webhook
 */
#include "models/webhook.h"

#include "models/message.h"

#include "app.h"
#include "cf.h"
#include "config.h"
#include "core/testclock.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "db/writer.h" /* cf_writer_start/stop: the D02 writer bootstrap */
#include "richtext.h"
#include "cf_test.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WH_HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define WH_ORIGIN "http://127.0.0.1:32123"

/* Fixed instants: "2026-01-02 03:04:05.678901", "2026-03-04 05:06:07.000001". */
#define WH_T1_US INT64_C(1767323045678901)
#define WH_T2_US INT64_C(1772600767000001)
#define WH_T1_TEXT "2026-01-02 03:04:05.678901"
#define WH_T2_TEXT "2026-03-04 05:06:07.000001"

/* Fixture ids (fixtures.rs::identify). */
#define WH_ID_JASON INT64_C(149087659)
#define WH_ID_BENDER INT64_C(394959859)
#define WH_ID_DAVID INT64_C(127326141)
#define WH_ID_DESIGNERS INT64_C(654632876)
#define WH_ID_FIRST INT64_C(309456473)

/* Borrowed cf_str over a literal. */
static cf_str lit(const char *text) {
    cf_str value = {(char *)text, strlen(text)};
    return value;
}

static bool str_is(cf_str value, const char *expected) {
    size_t len = strlen(expected);
    return value.len == len && value.ptr != NULL &&
           memcmp(value.ptr, expected, len) == 0 && value.ptr[len] == '\0';
}

static bool opt_is(const cf_optional_str *value, const char *expected) {
    return value->present && str_is(value->value, expected);
}

static bool opt_absent(const cf_optional_str *value) {
    return !value->present && value->value.ptr == NULL && value->value.len == 0;
}

static bool webhook_empty(const cf_webhook *webhook) {
    return webhook->id == 0 && webhook->user_id == 0 &&
           opt_absent(&webhook->url) && webhook->created_at == 0 &&
           webhook->updated_at == 0;
}

static bool webhook_is(const cf_webhook *webhook, int64_t id, int64_t user_id,
                       const char *url, int64_t created_at,
                       int64_t updated_at) {
    bool url_ok = url == NULL ? opt_absent(&webhook->url)
                              : opt_is(&webhook->url, url);
    return webhook->id == id && webhook->user_id == user_id && url_ok &&
           webhook->created_at == created_at &&
           webhook->updated_at == updated_at;
}

/* --- scratch database + app (D02 writer) --------------------------------- */

typedef struct {
    char *dir;
    char path[CF_DB_TEST_PATH_CAP];
    cf_app *app;
} wh_world;

static void wh_world_open(wh_world *world) {
    memset(world, 0, sizeof *world);
    world->dir = cf_db_test_dir();
    CF_REQUIRE(world->dir != NULL);
    cf_db_test_path(world->path, sizeof world->path, world->dir);

    /* Fresh schema (D-C01) so the writer opens an existing version-1 db. */
    cf_db *db = NULL;
    CF_REQUIRE(cf_db_open(world->path, false, &db) == CF_OK);
    cf_db_close(db);

    cf_config_entry entries[3] = {
        {"PUBLIC_ORIGIN", WH_ORIGIN},
        {"SECRET_KEY_BASE", WH_HEX64},
        {"DATABASE_PATH", world->path},
    };
    cf_config *config = NULL;
    CF_REQUIRE(cf_config_parse(entries, 3, NULL, &config) == CF_OK);
    CF_REQUIRE(cf_app_create(config, &world->app) == CF_OK);
    /* D02: cf_write returns CF_INTERNAL until the writer thread is started.
     * config is borrowed for the call; the app owns it afterwards. */
    CF_REQUIRE(cf_writer_start(world->app, config) == CF_OK);
}

static void wh_world_dispose(wh_world *world) {
    cf_writer_stop(world->app);
    cf_app_destroy(world->app);
    world->app = NULL;
    if (world->dir != NULL) {
        cf_db_test_cleanup(world->dir);
        free(world->dir);
        world->dir = NULL;
    }
}

static cf_db *wh_open(const wh_world *world, bool read_only) {
    cf_db *db = NULL;
    CF_REQUIRE(cf_db_open(world->path, read_only, &db) == CF_OK);
    return db;
}

static void wh_exec(cf_db *db, const char *sql) {
    char *error = NULL;
    int rc = sqlite3_exec(cf_db_handle(db), sql, NULL, NULL, &error);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "webhook_test: exec failed: %s\n  %s\n",
                error != NULL ? error : "?", sql);
    }
    sqlite3_free(error);
    CF_REQUIRE(rc == SQLITE_OK);
}

/* Reference fixture rows (webhooks.yml, users.yml, rooms.yml, messages.yml). */
static void wh_insert_core_fixtures(cf_db *db) {
    wh_exec(db,
            "INSERT INTO users (id, name, email_address, role, status, "
            "created_at, updated_at) VALUES (" "149087659" ", 'Jason', "
            "'jason@37signals.com', 1, 0, '" WH_T1_TEXT "', '" WH_T1_TEXT
            "')");
    wh_exec(db,
            "INSERT INTO users (id, name, role, status, bot_token, created_at, "
            "updated_at) VALUES (" "394959859" ", 'Bender Bot', 2, 0, "
            "'bendertoken', '" WH_T1_TEXT "', '" WH_T1_TEXT "')");
    wh_exec(db,
            "INSERT INTO users (id, name, role, status, created_at, "
            "updated_at) VALUES (" "127326141" ", 'David', 1, 0, '" WH_T1_TEXT
            "', '" WH_T1_TEXT "')");
    wh_exec(db,
            "INSERT INTO rooms (id, name, type, creator_id, created_at, "
            "updated_at) VALUES (" "654632876" ", 'Designers', "
            "'Rooms::Closed', " "127326141" ", '" WH_T1_TEXT "', '" WH_T1_TEXT
            "')");
    wh_exec(db,
            "INSERT INTO messages (id, client_message_id, created_at, "
            "creator_id, room_id, updated_at) VALUES (" "309456473"
            ", '0001', '" WH_T1_TEXT "', " "149087659" ", " "654632876" ", '"
            WH_T1_TEXT "')");
}

static void wh_insert_webhook(cf_db *db, int64_t id, const char *url_sql,
                              int64_t user_id) {
    char sql[256];
    snprintf(sql, sizeof sql,
             "INSERT INTO webhooks (id, created_at, updated_at, url, user_id) "
             "VALUES (%lld, '%s', '%s', %s, %lld)",
             (long long)id, WH_T1_TEXT, WH_T1_TEXT, url_sql,
             (long long)user_id);
    wh_exec(db, sql);
}

/* message.rs plain_text_body consults rich text only when a body row exists;
 * every payload case below is body-less, so the pipeline is never reached.
 * The production R02 singleton stands in for Tx::rich_text. */
static const cf_richtext *wh_rich_text(void) { return cf_tx_rich_text(NULL); }

/* --- find_by_user (read path, cf_db) ------------------------------------- */

CF_TEST(find_by_user_absent_returns_empty_result) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));

    bool found = true;
    cf_webhook out;
    memset(&out, 0xA5, sizeof out); /* the callee must leave it empty */
    CF_CHECK(cf_webhook_find_by_user(scratch.db, 12345, &found, &out) == CF_OK);
    CF_CHECK(!found);
    CF_CHECK(webhook_empty(&out));

    cf_db_scratch_close(&scratch);
}

CF_TEST(find_by_user_rejects_missing_outputs_and_database) {
    bool found = false;
    cf_webhook out;
    memset(&out, 0, sizeof out);
    CF_CHECK(cf_webhook_find_by_user(NULL, 1, &found, &out) == CF_INVALID);
    CF_CHECK(!found);
    CF_CHECK(webhook_empty(&out));
    CF_CHECK(cf_webhook_find_by_user(NULL, 1, NULL, &out) == CF_INVALID);
    CF_CHECK(cf_webhook_find_by_user(NULL, 1, &found, NULL) == CF_INVALID);

    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    found = true;
    CF_CHECK(cf_webhook_find_by_user(scratch.db, 1, &found, NULL) ==
             CF_INVALID);
    found = true;
    CF_CHECK(cf_webhook_find_by_user(scratch.db, 1, NULL, &out) == CF_INVALID);
    cf_db_scratch_close(&scratch);
}

CF_TEST(find_by_user_reads_all_columns_and_stops_at_one_row) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    wh_insert_core_fixtures(scratch.db);
    /* Two rows for the same user: the reference SQL is LIMIT 1 with no
     * ORDER BY, so either is valid but exactly one is returned. */
    wh_insert_webhook(scratch.db, 9001, "'http://example.com/bender'",
                      WH_ID_BENDER);
    wh_insert_webhook(scratch.db, 9002, "NULL", WH_ID_BENDER);
    wh_insert_webhook(scratch.db, 9003, "'http://example.com/david'",
                      WH_ID_DAVID);

    bool found = false;
    cf_webhook out;
    memset(&out, 0, sizeof out);
    CF_CHECK(cf_webhook_find_by_user(scratch.db, WH_ID_BENDER, &found, &out) ==
             CF_OK);
    CF_CHECK(found);
    if (out.id == 9001) {
        CF_CHECK(webhook_is(&out, 9001, WH_ID_BENDER,
                            "http://example.com/bender", WH_T1_US, WH_T1_US));
    } else {
        CF_CHECK(webhook_is(&out, 9002, WH_ID_BENDER, NULL, WH_T1_US,
                            WH_T1_US));
    }
    cf_webhook_dispose(&out);

    /* The other user's row is not returned for this user_id. */
    found = false;
    memset(&out, 0, sizeof out);
    CF_CHECK(cf_webhook_find_by_user(scratch.db, WH_ID_JASON, &found, &out) ==
             CF_OK);
    CF_CHECK(!found);
    CF_CHECK(webhook_empty(&out));

    cf_db_scratch_close(&scratch);
}

CF_TEST(find_by_user_distinguishes_null_and_empty_url) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    wh_insert_core_fixtures(scratch.db);
    wh_insert_webhook(scratch.db, 9101, "NULL", WH_ID_DAVID);
    wh_insert_webhook(scratch.db, 9102, "''", WH_ID_BENDER);

    bool found = false;
    cf_webhook out;
    memset(&out, 0, sizeof out);
    CF_CHECK(cf_webhook_find_by_user(scratch.db, WH_ID_DAVID, &found, &out) ==
             CF_OK);
    CF_CHECK(found);
    CF_CHECK(webhook_is(&out, 9101, WH_ID_DAVID, NULL, WH_T1_US, WH_T1_US));
    cf_webhook_dispose(&out);

    found = false;
    memset(&out, 0, sizeof out);
    CF_CHECK(cf_webhook_find_by_user(scratch.db, WH_ID_BENDER, &found, &out) ==
             CF_OK);
    CF_CHECK(found);
    CF_CHECK(out.url.present);
    CF_CHECK(out.url.value.ptr != NULL && out.url.value.len == 0);
    CF_CHECK(out.created_at == WH_T1_US && out.updated_at == WH_T1_US);
    cf_webhook_dispose(&out);

    cf_db_scratch_close(&scratch);
}

/* --- create (mutation path, cf_tx via D02 cf_write) ---------------------- */

typedef struct {
    cf_err rc;
    cf_webhook record;
    int64_t user_id;
    cf_optional_str url; /* borrowed for the call */
} wh_create_arg;

static cf_err wh_write_create(cf_tx *tx, void *arg) {
    wh_create_arg *a = arg;
    a->rc = cf_webhook_create(tx, a->user_id, a->url, &a->record);
    return a->rc;
}

CF_TEST(create_returns_record_and_stores_reference_datetime_text) {
    wh_world world;
    wh_world_open(&world);
    cf_db *setup = wh_open(&world, false);
    wh_insert_core_fixtures(setup);
    cf_db_close(setup);

    wh_create_arg arg;
    memset(&arg, 0, sizeof arg);
    arg.user_id = WH_ID_BENDER;
    arg.url.present = true;
    arg.url.value = lit("http://example.com/bender");

    cf_test_clock_set_fixed_us(WH_T1_US);
    CF_CHECK(cf_write(world.app, wh_write_create, &arg) == CF_OK);
    cf_test_clock_clear();

    CF_CHECK(arg.rc == CF_OK);
    CF_CHECK(webhook_is(&arg.record, 1, WH_ID_BENDER,
                        "http://example.com/bender", WH_T1_US, WH_T1_US));
    cf_webhook_dispose(&arg.record);

    cf_db *reader = wh_open(&world, true);
    sqlite3 *handle = cf_db_handle(reader);
    char text[256];
    const char *row = cf_db_test_text(
        handle,
        "SELECT url || '|' || created_at || '|' || updated_at || '|' || "
        "user_id FROM webhooks WHERE id = 1",
        text, sizeof text);
    CF_CHECK(row != NULL &&
             strcmp(row, "http://example.com/bender|" WH_T1_TEXT "|" WH_T1_TEXT
                         "|394959859") == 0);

    bool found = false;
    cf_webhook got;
    memset(&got, 0, sizeof got);
    CF_CHECK(cf_webhook_find_by_user(reader, WH_ID_BENDER, &found, &got) ==
             CF_OK);
    CF_CHECK(found);
    CF_CHECK(webhook_is(&got, 1, WH_ID_BENDER, "http://example.com/bender",
                        WH_T1_US, WH_T1_US));
    cf_webhook_dispose(&got);
    cf_db_close(reader);
    wh_world_dispose(&world);
}

CF_TEST(create_absent_url_stores_null) {
    wh_world world;
    wh_world_open(&world);
    cf_db *setup = wh_open(&world, false);
    wh_insert_core_fixtures(setup);
    cf_db_close(setup);

    wh_create_arg arg;
    memset(&arg, 0, sizeof arg);
    arg.user_id = WH_ID_BENDER;
    /* no url */

    cf_test_clock_set_fixed_us(WH_T2_US);
    CF_CHECK(cf_write(world.app, wh_write_create, &arg) == CF_OK);
    cf_test_clock_clear();

    CF_CHECK(arg.rc == CF_OK);
    CF_CHECK(webhook_is(&arg.record, 1, WH_ID_BENDER, NULL, WH_T2_US,
                        WH_T2_US));
    cf_webhook_dispose(&arg.record);

    cf_db *reader = wh_open(&world, true);
    bool ok = false;
    CF_CHECK(cf_db_test_i64(cf_db_handle(reader),
                            "SELECT count(*) FROM webhooks WHERE id = 1 AND "
                            "url IS NULL AND created_at = '" WH_T2_TEXT
                            "' AND updated_at = '" WH_T2_TEXT "'",
                            &ok) == 1);
    CF_CHECK(ok);
    cf_db_close(reader);
    wh_world_dispose(&world);
}

CF_TEST(create_empty_url_stores_empty_text_not_null) {
    wh_world world;
    wh_world_open(&world);
    cf_db *setup = wh_open(&world, false);
    wh_insert_core_fixtures(setup);
    cf_db_close(setup);

    wh_create_arg arg;
    memset(&arg, 0, sizeof arg);
    arg.user_id = WH_ID_BENDER;
    arg.url.present = true; /* empty borrowed span */
    arg.url.value = (cf_str){NULL, 0};

    cf_test_clock_set_fixed_us(WH_T1_US);
    CF_CHECK(cf_write(world.app, wh_write_create, &arg) == CF_OK);
    cf_test_clock_clear();

    CF_CHECK(arg.rc == CF_OK);
    CF_CHECK(arg.record.url.present);
    CF_CHECK(arg.record.url.value.ptr != NULL &&
             arg.record.url.value.len == 0);
    cf_webhook_dispose(&arg.record);

    cf_db *reader = wh_open(&world, true);
    bool ok = false;
    CF_CHECK(cf_db_test_i64(cf_db_handle(reader),
                            "SELECT count(*) FROM webhooks WHERE id = 1 AND "
                            "url IS NOT NULL AND url = ''",
                            &ok) == 1);
    CF_CHECK(ok);
    cf_db_close(reader);
    wh_world_dispose(&world);
}

CF_TEST(create_missing_user_is_invalid_and_leaves_output_empty) {
    wh_world world;
    wh_world_open(&world);
    cf_db *setup = wh_open(&world, false);
    wh_insert_core_fixtures(setup);
    cf_db_close(setup);

    wh_create_arg arg;
    memset(&arg, 0, sizeof arg);
    arg.user_id = 424242; /* no such user: FK violation */
    CF_CHECK(cf_write(world.app, wh_write_create, &arg) == CF_INVALID);
    CF_CHECK(arg.rc == CF_INVALID);
    CF_CHECK(webhook_empty(&arg.record));

    cf_db *reader = wh_open(&world, true);
    bool ok = false;
    CF_CHECK(cf_db_test_i64(cf_db_handle(reader),
                            "SELECT count(*) FROM webhooks", &ok) == 0);
    CF_CHECK(ok);
    cf_db_close(reader);
    wh_world_dispose(&world);
}

CF_TEST(create_rejects_missing_output_or_transaction) {
    cf_optional_str url = {0};
    cf_webhook out;
    memset(&out, 0xA5, sizeof out);
    CF_CHECK(cf_webhook_create(NULL, 1, url, &out) == CF_INVALID);
    CF_CHECK(webhook_empty(&out));
    CF_CHECK(cf_webhook_create(NULL, 1, url, NULL) == CF_INVALID);
}

/* --- update_url ---------------------------------------------------------- */

typedef struct {
    cf_err rc;
    cf_webhook record;
    cf_str url; /* borrowed for the call */
} wh_update_arg;

static cf_err wh_write_update(cf_tx *tx, void *arg) {
    wh_update_arg *a = arg;
    a->rc = cf_webhook_update_url(tx, &a->record, a->url);
    return a->rc;
}

CF_TEST(update_url_changes_url_and_updated_at_only) {
    wh_world world;
    wh_world_open(&world);
    cf_db *setup = wh_open(&world, false);
    wh_insert_core_fixtures(setup);
    cf_db_close(setup);

    wh_create_arg create;
    memset(&create, 0, sizeof create);
    create.user_id = WH_ID_BENDER;
    create.url.present = true;
    create.url.value = lit("http://old.example.com");
    cf_test_clock_set_fixed_us(WH_T1_US);
    CF_CHECK(cf_write(world.app, wh_write_create, &create) == CF_OK);
    cf_test_clock_clear();
    CF_CHECK(create.rc == CF_OK);

    wh_update_arg update;
    memset(&update, 0, sizeof update);
    update.record = create.record; /* keep created_at from the insert */
    create.record = (cf_webhook){0}; /* ownership moved to update.record */
    update.url = lit("http://new.example.com");
    cf_test_clock_set_fixed_us(WH_T2_US);
    CF_CHECK(cf_write(world.app, wh_write_update, &update) == CF_OK);
    cf_test_clock_clear();

    CF_CHECK(update.rc == CF_OK);
    CF_CHECK(webhook_is(&update.record, 1, WH_ID_BENDER,
                        "http://new.example.com", WH_T1_US, WH_T2_US));
    cf_webhook_dispose(&update.record);
    cf_webhook_dispose(&create.record);

    cf_db *reader = wh_open(&world, true);
    char text[256];
    const char *row = cf_db_test_text(
        cf_db_handle(reader),
        "SELECT url || '|' || created_at || '|' || updated_at FROM webhooks "
        "WHERE id = 1",
        text, sizeof text);
    CF_CHECK(row != NULL && strcmp(row, "http://new.example.com|" WH_T1_TEXT
                                        "|" WH_T2_TEXT) == 0);
    cf_db_close(reader);
    wh_world_dispose(&world);
}

CF_TEST(update_url_unchanged_is_a_noop_without_touch) {
    wh_world world;
    wh_world_open(&world);
    cf_db *setup = wh_open(&world, false);
    wh_insert_core_fixtures(setup);
    cf_db_close(setup);

    wh_create_arg create;
    memset(&create, 0, sizeof create);
    create.user_id = WH_ID_BENDER;
    create.url.present = true;
    create.url.value = lit("http://same.example.com");
    cf_test_clock_set_fixed_us(WH_T1_US);
    CF_CHECK(cf_write(world.app, wh_write_create, &create) == CF_OK);
    cf_test_clock_clear();
    CF_CHECK(create.rc == CF_OK);

    /* The clock advances; an unchanged update must not read it or touch the
     * row (the reference returns before `tx.now()`). */
    wh_update_arg update;
    memset(&update, 0, sizeof update);
    update.record = create.record;
    create.record = (cf_webhook){0}; /* ownership moved to update.record */
    update.url = lit("http://same.example.com");
    cf_test_clock_set_fixed_us(WH_T2_US);
    CF_CHECK(cf_write(world.app, wh_write_update, &update) == CF_OK);
    cf_test_clock_clear();

    CF_CHECK(update.rc == CF_OK);
    CF_CHECK(webhook_is(&update.record, 1, WH_ID_BENDER,
                        "http://same.example.com", WH_T1_US, WH_T1_US));
    cf_webhook_dispose(&update.record);
    cf_webhook_dispose(&create.record);

    cf_db *reader = wh_open(&world, true);
    bool ok = false;
    CF_CHECK(cf_db_test_i64(cf_db_handle(reader),
                            "SELECT count(*) FROM webhooks WHERE id = 1 AND "
                            "url = 'http://same.example.com' AND updated_at = '"
                            WH_T1_TEXT "'",
                            &ok) == 1);
    CF_CHECK(ok);
    cf_db_close(reader);
    wh_world_dispose(&world);
}

CF_TEST(update_url_sets_value_when_previously_absent) {
    wh_world world;
    wh_world_open(&world);
    cf_db *setup = wh_open(&world, false);
    wh_insert_core_fixtures(setup);
    cf_db_close(setup);

    wh_create_arg create;
    memset(&create, 0, sizeof create);
    create.user_id = WH_ID_BENDER; /* url None */
    cf_test_clock_set_fixed_us(WH_T1_US);
    CF_CHECK(cf_write(world.app, wh_write_create, &create) == CF_OK);
    cf_test_clock_clear();
    CF_CHECK(create.rc == CF_OK);
    CF_CHECK(opt_absent(&create.record.url));

    wh_update_arg update;
    memset(&update, 0, sizeof update);
    update.record = create.record;
    create.record = (cf_webhook){0}; /* ownership moved to update.record */
    update.url = lit("http://added.example.com");
    cf_test_clock_set_fixed_us(WH_T2_US);
    CF_CHECK(cf_write(world.app, wh_write_update, &update) == CF_OK);
    cf_test_clock_clear();

    CF_CHECK(update.rc == CF_OK);
    CF_CHECK(webhook_is(&update.record, 1, WH_ID_BENDER,
                        "http://added.example.com", WH_T1_US, WH_T2_US));
    cf_webhook_dispose(&update.record);
    cf_webhook_dispose(&create.record);
    wh_world_dispose(&world);
}

CF_TEST(update_url_of_missing_row_matches_reference) {
    wh_world world;
    wh_world_open(&world);

    wh_update_arg update;
    memset(&update, 0, sizeof update);
    update.record.id = 987654321; /* no such row */
    update.url = lit("http://ghost.example.com");
    cf_test_clock_set_fixed_us(WH_T1_US);
    CF_CHECK(cf_write(world.app, wh_write_update, &update) == CF_OK);
    cf_test_clock_clear();

    /* The reference ignores execute's affected-row count and still updates
     * the in-memory record. */
    CF_CHECK(update.rc == CF_OK);
    CF_CHECK(opt_is(&update.record.url, "http://ghost.example.com"));
    CF_CHECK(update.record.updated_at == WH_T1_US);
    cf_webhook_dispose(&update.record);

    cf_db *reader = wh_open(&world, true);
    bool ok = false;
    CF_CHECK(cf_db_test_i64(cf_db_handle(reader),
                            "SELECT count(*) FROM webhooks", &ok) == 0);
    CF_CHECK(ok);
    cf_db_close(reader);
    wh_world_dispose(&world);
}

CF_TEST(update_url_rejects_missing_record_or_transaction) {
    cf_webhook record;
    memset(&record, 0, sizeof record);
    CF_CHECK(cf_webhook_update_url(NULL, &record, lit("x")) == CF_INVALID);
    CF_CHECK(opt_absent(&record.url));
    CF_CHECK(cf_webhook_update_url(NULL, NULL, lit("x")) == CF_INVALID);
    cf_webhook_dispose(&record);
}

/* --- destroy ------------------------------------------------------------- */

typedef struct {
    cf_err rc;
    cf_webhook record;
} wh_destroy_arg;

static cf_err wh_write_destroy(cf_tx *tx, void *arg) {
    wh_destroy_arg *a = arg;
    a->rc = cf_webhook_destroy(tx, &a->record);
    return a->rc;
}

CF_TEST(destroy_removes_row_and_stale_destroy_succeeds) {
    wh_world world;
    wh_world_open(&world);
    cf_db *setup = wh_open(&world, false);
    wh_insert_core_fixtures(setup);
    wh_insert_webhook(setup, 9001, "'http://example.com/bender'",
                      WH_ID_BENDER);
    cf_db_close(setup);

    wh_destroy_arg arg;
    memset(&arg, 0, sizeof arg);
    arg.record.id = 9001;
    cf_test_clock_set_fixed_us(WH_T1_US);
    CF_CHECK(cf_write(world.app, wh_write_destroy, &arg) == CF_OK);
    cf_test_clock_clear();
    CF_CHECK(arg.rc == CF_OK);

    cf_db *reader = wh_open(&world, true);
    bool found = true;
    cf_webhook got;
    memset(&got, 0, sizeof got);
    CF_CHECK(cf_webhook_find_by_user(reader, WH_ID_BENDER, &found, &got) ==
             CF_OK);
    CF_CHECK(!found);
    CF_CHECK(webhook_empty(&got));
    bool ok = false;
    CF_CHECK(cf_db_test_i64(cf_db_handle(reader), "SELECT count(*) FROM webhooks",
                            &ok) == 0);
    CF_CHECK(ok);
    cf_db_close(reader);

    /* The reference ignores the affected count: deleting a stale id returns
     * Ok and changes nothing. */
    arg.record.id = 987654321;
    CF_CHECK(cf_write(world.app, wh_write_destroy, &arg) == CF_OK);
    CF_CHECK(arg.rc == CF_OK);
    wh_world_dispose(&world);
}

CF_TEST(destroy_rejects_missing_record_or_transaction) {
    cf_webhook record;
    memset(&record, 0, sizeof record);
    CF_CHECK(cf_webhook_destroy(NULL, &record) == CF_INVALID);
    CF_CHECK(cf_webhook_destroy(NULL, NULL) == CF_INVALID);
}

/* --- payload ------------------------------------------------------------- */

CF_TEST(payload_no_body_builds_reference_json) {
    wh_world world;
    wh_world_open(&world);
    cf_db *setup = wh_open(&world, false);
    wh_insert_core_fixtures(setup);
    wh_insert_webhook(setup, 9001, "'http://example.com/bender'",
                      WH_ID_BENDER);
    cf_db_close(setup);

    cf_db *reader = wh_open(&world, true);
    bool found = false;
    cf_webhook hook;
    memset(&hook, 0, sizeof hook);
    CF_CHECK(cf_webhook_find_by_user(reader, WH_ID_BENDER, &found, &hook) ==
             CF_OK);
    CF_REQUIRE(found);
    cf_message message;
    memset(&message, 0, sizeof message);
    CF_CHECK(cf_message_find(reader, WH_ID_FIRST, &message) == CF_OK);

    cf_str out = {0};
    CF_CHECK(cf_webhook_payload(reader, &hook, wh_rich_text(),
                                &message, lit("/rooms/1/bot/key/messages"),
                                lit("/rooms/1/@2"), &out) == CF_OK);
    CF_CHECK(str_is(out,
                    "{\"user\":{\"id\":149087659,\"name\":\"Jason\"},"
                    "\"room\":{\"id\":654632876,\"name\":\"Designers\","
                    "\"path\":\"/rooms/1/bot/key/messages\"},"
                    "\"message\":{\"id\":309456473,\"body\":{\"html\":null,"
                    "\"plain\":\"\"},\"path\":\"/rooms/1/@2\"}}"));
    cf_str_dispose(&out);

    cf_message_dispose(&message);
    cf_webhook_dispose(&hook);
    cf_db_close(reader);
    wh_world_dispose(&world);
}

CF_TEST(payload_null_room_name_serializes_null) {
    wh_world world;
    wh_world_open(&world);
    cf_db *setup = wh_open(&world, false);
    wh_insert_core_fixtures(setup);
    wh_insert_webhook(setup, 9001, "'http://example.com/bender'",
                      WH_ID_BENDER);
    wh_exec(setup, "UPDATE rooms SET name = NULL WHERE id = 654632876");
    cf_db_close(setup);

    cf_db *reader = wh_open(&world, true);
    bool found = false;
    cf_webhook hook;
    memset(&hook, 0, sizeof hook);
    CF_CHECK(cf_webhook_find_by_user(reader, WH_ID_BENDER, &found, &hook) ==
             CF_OK);
    CF_REQUIRE(found);
    cf_message message;
    memset(&message, 0, sizeof message);
    CF_CHECK(cf_message_find(reader, WH_ID_FIRST, &message) == CF_OK);

    cf_str out = {0};
    CF_CHECK(cf_webhook_payload(reader, &hook, wh_rich_text(),
                                &message, lit("/rooms/1/bot/key/messages"),
                                lit("/rooms/1/@2"), &out) == CF_OK);
    CF_CHECK(str_is(out,
                    "{\"user\":{\"id\":149087659,\"name\":\"Jason\"},"
                    "\"room\":{\"id\":654632876,\"name\":null,"
                    "\"path\":\"/rooms/1/bot/key/messages\"},"
                    "\"message\":{\"id\":309456473,\"body\":{\"html\":null,"
                    "\"plain\":\"\"},\"path\":\"/rooms/1/@2\"}}"));
    cf_str_dispose(&out);

    cf_message_dispose(&message);
    cf_webhook_dispose(&hook);
    cf_db_close(reader);
    wh_world_dispose(&world);
}

/* No stored body, so plain_text_body falls back to the attachment filename
 * (message.rs); the recipient mention "@Bender Bot" is removed and the
 * surrounding Unicode whitespace trimmed, and the JSON escapes <, > and &. */
CF_TEST(payload_plain_from_attachment_removes_mention_trims_and_escapes) {
    wh_world world;
    wh_world_open(&world);
    cf_db *setup = wh_open(&world, false);
    wh_insert_core_fixtures(setup);
    wh_insert_webhook(setup, 9001, "'http://example.com/bender'",
                      WH_ID_BENDER);
    wh_exec(setup,
            "INSERT INTO active_storage_blobs (id, byte_size, filename, key, "
            "service_name, created_at) VALUES (7001, 9, "
            "'@Bender Bot \xc2\xa0<Tom & Jerry>', 'blob-key-7001', 'test', '"
            WH_T1_TEXT "')");
    wh_exec(setup,
            "INSERT INTO active_storage_attachments (id, blob_id, name, "
            "record_id, record_type, created_at) VALUES (7002, 7001, "
            "'attachment', 309456473, 'Message', '" WH_T1_TEXT "')");
    cf_db_close(setup);

    cf_db *reader = wh_open(&world, true);
    bool found = false;
    cf_webhook hook;
    memset(&hook, 0, sizeof hook);
    CF_CHECK(cf_webhook_find_by_user(reader, WH_ID_BENDER, &found, &hook) ==
             CF_OK);
    CF_REQUIRE(found);
    cf_message message;
    memset(&message, 0, sizeof message);
    CF_CHECK(cf_message_find(reader, WH_ID_FIRST, &message) == CF_OK);

    cf_str out = {0};
    CF_CHECK(cf_webhook_payload(reader, &hook, wh_rich_text(),
                                &message, lit("/rooms/1/bot/key/messages"),
                                lit("/rooms/1/@2"), &out) == CF_OK);
    CF_CHECK(str_is(out,
                    "{\"user\":{\"id\":149087659,\"name\":\"Jason\"},"
                    "\"room\":{\"id\":654632876,\"name\":\"Designers\","
                    "\"path\":\"/rooms/1/bot/key/messages\"},"
                    "\"message\":{\"id\":309456473,\"body\":{\"html\":null,"
                    "\"plain\":\"\\u003cTom \\u0026 Jerry\\u003e\"},"
                    "\"path\":\"/rooms/1/@2\"}}"));
    cf_str_dispose(&out);

    cf_message_dispose(&message);
    cf_webhook_dispose(&hook);
    cf_db_close(reader);
    wh_world_dispose(&world);
}

CF_TEST(payload_missing_room_or_recipient_is_not_found) {
    wh_world world;
    wh_world_open(&world);
    cf_db *setup = wh_open(&world, false);
    wh_insert_core_fixtures(setup);
    wh_insert_webhook(setup, 9001, "'http://example.com/bender'",
                      WH_ID_BENDER);
    cf_db_close(setup);

    cf_db *reader = wh_open(&world, true);
    bool found = false;
    cf_webhook hook;
    memset(&hook, 0, sizeof hook);
    CF_CHECK(cf_webhook_find_by_user(reader, WH_ID_BENDER, &found, &hook) ==
             CF_OK);
    CF_REQUIRE(found);

    /* message.creator() succeeds (Jason exists); Room::find then misses. */
    cf_message message;
    memset(&message, 0, sizeof message);
    message.id = WH_ID_FIRST;
    message.room_id = 424242;
    message.creator_id = WH_ID_JASON;
    cf_str out = {0};
    CF_CHECK(cf_webhook_payload(reader, &hook, wh_rich_text(),
                                &message, lit("/rooms/1/bot/key/messages"),
                                lit("/rooms/1/@2"), &out) == CF_NOT_FOUND);
    CF_CHECK(out.ptr == NULL && out.len == 0);

    /* The webhook's user_id names nobody: User::find misses after the room. */
    cf_message real;
    memset(&real, 0, sizeof real);
    CF_CHECK(cf_message_find(reader, WH_ID_FIRST, &real) == CF_OK);
    cf_webhook ghost;
    memset(&ghost, 0, sizeof ghost);
    ghost.id = 1;
    ghost.user_id = 424242;
    out = (cf_str){0};
    CF_CHECK(cf_webhook_payload(reader, &ghost, wh_rich_text(),
                                &real, lit("/rooms/1/bot/key/messages"),
                                lit("/rooms/1/@2"), &out) == CF_NOT_FOUND);
    CF_CHECK(out.ptr == NULL && out.len == 0);

    cf_message_dispose(&real);
    cf_webhook_dispose(&hook);
    cf_db_close(reader);
    wh_world_dispose(&world);
}

CF_TEST(payload_rejects_missing_arguments) {
    wh_world world;
    wh_world_open(&world);
    cf_db *setup = wh_open(&world, false);
    wh_insert_core_fixtures(setup);
    cf_db_close(setup);

    cf_db *reader = wh_open(&world, true);
    cf_webhook hook;
    memset(&hook, 0, sizeof hook);
    hook.id = 1;
    hook.user_id = WH_ID_BENDER;
    cf_message message;
    memset(&message, 0, sizeof message);
    message.id = WH_ID_FIRST;
    message.room_id = WH_ID_DESIGNERS;
    message.creator_id = WH_ID_JASON;

    cf_str out = (cf_str){NULL, 0};
    CF_CHECK(cf_webhook_payload(NULL, &hook, wh_rich_text(),
                                &message, lit("/r"), lit("/m"), &out) ==
             CF_INVALID);
    CF_CHECK(out.ptr == NULL && out.len == 0);
    CF_CHECK(cf_webhook_payload(reader, NULL, wh_rich_text(),
                                &message, lit("/r"), lit("/m"), &out) ==
             CF_INVALID);
    CF_CHECK(cf_webhook_payload(reader, &hook, NULL, &message, lit("/r"),
                                lit("/m"), &out) == CF_INVALID);
    CF_CHECK(cf_webhook_payload(reader, &hook, wh_rich_text(), NULL,
                                lit("/r"), lit("/m"), &out) == CF_INVALID);
    CF_CHECK(cf_webhook_payload(reader, &hook, wh_rich_text(),
                                &message, lit("/r"), lit("/m"), NULL) ==
             CF_INVALID);

    cf_db_close(reader);
    wh_world_dispose(&world);
}

/* --- record/vector disposal ----------------------------------------------- */

CF_TEST(dispose_helpers_accept_null_and_reset) {
    cf_webhook_dispose(NULL);
    cf_webhook_vector_dispose(NULL);

    cf_webhook_vector vector;
    memset(&vector, 0, sizeof vector);
    vector.items = calloc(2, sizeof *vector.items);
    CF_REQUIRE(vector.items != NULL);
    vector.len = 2;
    vector.cap = 2;
    vector.items[0].url.present = true;
    vector.items[0].url.value.ptr = malloc(4);
    CF_REQUIRE(vector.items[0].url.value.ptr != NULL);
    memcpy(vector.items[0].url.value.ptr, "url", 3);
    vector.items[0].url.value.len = 3;

    cf_webhook_vector_dispose(&vector);
    CF_CHECK(vector.items == NULL && vector.len == 0 && vector.cap == 0);
}

CF_TEST_MAIN()
