/* tests/models/test_touch.c — packet V-G shared record-touch tests.
 *
 * Exercises cf_touch_user through the public cf_write path (D02 writer,
 * opaque cf_tx) on a cf_app whose DATABASE_PATH is a scratch file, mirroring
 * tests/models/ban_test.c. The fixed test clock pins `now`.
 *
 * Build (integrator wiring; mirrors the sibling model tests):
 *   clang -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra
 *         -Werror -pthread -Isrc -Itests
 *         -Ivendor/src/sqlite/sqlite-amalgamation-3530400
 *         tests/models/test_touch.c src/models/[all model translation units]
 *         src/models/touch.c src/db/{schema,reader,statements,writer}.c
 *         src/core/{alloc,buffer,clock,error,random}.c src/config.c
 *         src/app.c vendor/build/sqlite-clang/libsqlite3.a -lm
 *         -o build/touch/plain/test_touch
 */
#include "cf_test.h"

#include "app.h"
#include "config.h"
#include "core/testclock.h"
#include "db/db_testutil.h"
#include "db/writer.h"
#include "models/touch.h"
#include "models/user.h"

#include <sqlite3.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CF_TOUCH_TEST_SECRET_HEX \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

/* 2023-11-14 22:13:20.123456 UTC, the pinned reference clock test value. */
#define CF_TOUCH_TEST_NOW_US INT64_C(1700000000123456)
#define CF_TOUCH_TEST_SEED_TEXT "2024-01-02 03:04:05"

static int raw_exec(cf_db *db, const char *sql) {
    return sqlite3_exec(cf_db_handle(db), sql, NULL, NULL, NULL);
}

static void seed_fixture(cf_db_scratch *scratch) {
    CF_REQUIRE(scratch->db != NULL);
    CF_REQUIRE(raw_exec(scratch->db,
                        "INSERT INTO users (id, name, created_at, updated_at) "
                        "VALUES (1, 'u1', '" CF_TOUCH_TEST_SEED_TEXT "', '"
                        CF_TOUCH_TEST_SEED_TEXT "')") == SQLITE_OK);
}

static cf_app *make_app(const char *database_path) {
    cf_config_entry entries[4] = {
        {"PUBLIC_ORIGIN", "http://127.0.0.1:32123"},
        {"SECRET_KEY_BASE", CF_TOUCH_TEST_SECRET_HEX},
        {"PORT", "32123"},
        {"DATABASE_PATH", database_path},
    };
    cf_config *config = NULL;
    CF_REQUIRE(cf_config_parse(entries, 4, NULL, &config) == CF_OK);
    cf_app *app = NULL;
    CF_REQUIRE(cf_app_create(config, &app) == CF_OK);
    CF_REQUIRE(app != NULL);
    CF_REQUIRE(cf_writer_start(app, config) == CF_OK);
    return app;
}

static void dispose_app(cf_app *app) {
    cf_writer_stop(app);
    cf_app_destroy(app);
}

struct touch_call {
    cf_user user;
    cf_err rc;
};

static cf_err touch_call_fn(cf_tx *tx, void *arg) {
    struct touch_call *call = arg;
    call->rc = cf_touch_user(tx, &call->user);
    return call->rc;
}

static cf_err touch_null_user_fn(cf_tx *tx, void *arg) {
    cf_err *rc = arg;
    *rc = cf_touch_user(tx, NULL);
    return *rc;
}

struct touch_id_call {
    int64_t user_id;
    cf_err rc;
};

static cf_err touch_id_call_fn(cf_tx *tx, void *arg) {
    struct touch_id_call *call = arg;
    call->rc = cf_touch_user_id(tx, call->user_id);
    return call->rc;
}

/* Read back the stored updated_at text for one user id. */
static void read_updated_text(cf_db *db, int64_t id, char *out, size_t cap) {
    char sql[128];
    snprintf(sql, sizeof sql,
             "SELECT updated_at FROM users WHERE id = %" PRId64, id);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL) ==
               SQLITE_OK);
    CF_REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    const unsigned char *text = sqlite3_column_text(stmt, 0);
    CF_REQUIRE(text != NULL);
    size_t len = strlen((const char *)text);
    CF_REQUIRE(len < cap);
    memcpy(out, text, len + 1);
    CF_REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
}

CF_TEST(touch_bumps_updated_at_in_row_and_record) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_fixture(&scratch);
    cf_app *app = make_app(scratch.path);

    bool found = false;
    cf_user user;
    memset(&user, 0, sizeof user);
    CF_REQUIRE(cf_user_find_by_id(scratch.db, 1, &found, &user) == CF_OK);
    CF_REQUIRE(found);

    char expected[CF_DB_TIME_TEXT_CAP];
    CF_REQUIRE(cf_db_time_to_text(CF_TOUCH_TEST_NOW_US, expected) == CF_OK);
    CF_REQUIRE(strcmp(expected, CF_TOUCH_TEST_SEED_TEXT) != 0);

    cf_test_clock_set_fixed_us(CF_TOUCH_TEST_NOW_US);
    struct touch_call call;
    memset(&call, 0, sizeof call);
    call.user = user;
    memset(&user, 0, sizeof user); /* call owns the record now */
    cf_err write_rc = cf_write(app, touch_call_fn, &call);
    cf_test_clock_clear();

    CF_CHECK(write_rc == CF_OK);
    CF_REQUIRE(call.rc == CF_OK);
    /* In-memory record takes the same `now`. */
    CF_CHECK(call.user.updated_at == CF_TOUCH_TEST_NOW_US);
    /* The row carries the exact reference SQL text form of that value. */
    char stored[CF_DB_TIME_TEXT_CAP];
    read_updated_text(scratch.db, 1, stored, sizeof stored);
    CF_CHECK(strcmp(stored, expected) == 0);
    cf_user_dispose(&call.user);

    cf_db_scratch_close(&scratch);
    dispose_app(app);
}

CF_TEST(touch_rejects_null_arguments) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_fixture(&scratch);
    cf_app *app = make_app(scratch.path);

    struct touch_call call;
    memset(&call, 0, sizeof call);
    call.user.id = 1;
    /* NULL transaction: call directly, outside cf_write. */
    CF_CHECK(cf_touch_user(NULL, &call.user) == CF_INVALID);
    CF_CHECK(cf_touch_user(NULL, NULL) == CF_INVALID);
    /* NULL user through the writer path. */
    cf_err null_rc = CF_OK;
    CF_CHECK(cf_write(app, touch_null_user_fn, &null_rc) == CF_INVALID);
    CF_CHECK(null_rc == CF_INVALID);

    cf_db_scratch_close(&scratch);
    dispose_app(app);
}

CF_TEST(touch_absent_id_changes_no_row) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_fixture(&scratch);
    cf_app *app = make_app(scratch.path);

    /* Like the sibling touches, no-row-matched is still success. */
    cf_user ghost;
    memset(&ghost, 0, sizeof ghost);
    ghost.id = 424242;

    cf_test_clock_set_fixed_us(CF_TOUCH_TEST_NOW_US);
    struct touch_call call;
    memset(&call, 0, sizeof call);
    call.user = ghost;
    cf_err write_rc = cf_write(app, touch_call_fn, &call);
    cf_test_clock_clear();

    CF_CHECK(write_rc == CF_OK);
    CF_CHECK(call.rc == CF_OK);
    CF_CHECK(call.user.updated_at == CF_TOUCH_TEST_NOW_US);
    bool found = true;
    cf_user missing;
    memset(&missing, 0, sizeof missing);
    CF_CHECK(cf_user_find_by_id(scratch.db, 424242, &found, &missing) ==
             CF_OK);
    CF_CHECK(!found);

    cf_db_scratch_close(&scratch);
    dispose_app(app);
}

CF_TEST(touch_id_bumps_row_without_record) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_fixture(&scratch);
    cf_app *app = make_app(scratch.path);

    char expected[CF_DB_TIME_TEXT_CAP];
    CF_REQUIRE(cf_db_time_to_text(CF_TOUCH_TEST_NOW_US, expected) == CF_OK);

    /* The avatars_destroy-shaped call: id only, no record in hand. */
    cf_test_clock_set_fixed_us(CF_TOUCH_TEST_NOW_US);
    struct touch_id_call call;
    memset(&call, 0, sizeof call);
    call.user_id = 1;
    cf_err write_rc = cf_write(app, touch_id_call_fn, &call);
    cf_test_clock_clear();

    CF_CHECK(write_rc == CF_OK);
    CF_CHECK(call.rc == CF_OK);
    char stored[CF_DB_TIME_TEXT_CAP];
    read_updated_text(scratch.db, 1, stored, sizeof stored);
    CF_CHECK(strcmp(stored, expected) == 0);

    /* NULL transaction is rejected without touching the clock. */
    CF_CHECK(cf_touch_user_id(NULL, 1) == CF_INVALID);

    cf_db_scratch_close(&scratch);
    dispose_app(app);
}

CF_TEST_MAIN()
