/* D01 model family "rich_text_record" tests.
 *
 * Reference oracle: tmp/rust-ref/crates/db/src/models/rich_text_record.rs
 * (4 functions) and its fixture coverage:
 *  - tests/fixtures/crates/db/src/tests/columns_test.rs
 *    users_sessions_accounts_boosts_and_rich_texts_read_each_column_into_its_own_field
 *    (find_for reads every column through the unique key; fixture-shaped row
 *    id 8001 / name "body" / record_type "Message" / record_id 8005);
 *  - the schema's UNIQUE index index_action_text_rich_texts_uniqueness
 *    (record_type, record_id, name) is the create failure path.
 * Datetime constants below are the epoch microseconds of the matching UTC
 * text, cross-checked against cf_db_time_to_text (D01 db-core).
 *
 * Mutation cases run through D02's cf_write on a scratch database created by
 * cf_db_open (fresh schema, D-C01); a fixed test clock makes created_at /
 * updated_at deterministic (no sleeps).
 *
 * Build (plain):
 *   clang -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra
 *         -Werror -pthread -Isrc -Itests
 *         -Ivendor/src/sqlite/sqlite-amalgamation-3530400
 *         src/models/rich_text_record.c src/models/types.c
 *         tests/models/rich_text_record_test.c
 *         src/core/alloc.c src/core/buffer.c src/core/clock.c
 *         src/core/error.c src/core/random.c src/config.c src/app.c
 *         src/db/schema.c src/db/reader.c src/db/statements.c
 *         src/db/writer.c
 *         vendor/build/sqlite-clang/libsqlite3.a
 *         vendor/build/yyjson-clang/libyyjson.a -lm
 *         -o build/d01-model-rich_text_record/plain/test_rich_text_record
 */
#include "models/rich_text_record.h"

#include "app.h"
#include "cf.h"
#include "config.h"
#include "core/testclock.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "db/writer.h"
#include "cf_test.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RTR_HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define RTR_ORIGIN "http://127.0.0.1:32123"

/* Fixed instants: "2026-01-02 03:04:05.678901", "2026-03-04 05:06:07.000001",
 * and whole-second "2026-01-02 03:04:05" (no fractional text). */
#define RTR_NOW1_US INT64_C(1767323045678901)
#define RTR_NOW2_US INT64_C(1772600767000001)
#define RTR_WHOLE_US INT64_C(1767323045000000)
#define RTR_NOW1_TEXT "2026-01-02 03:04:05.678901"
#define RTR_NOW2_TEXT "2026-03-04 05:06:07.000001"
#define RTR_WHOLE_TEXT "2026-01-02 03:04:05"

/* Borrowed cf_str over a literal, for calls in this file. */
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

static bool record_empty(const cf_rich_text_record *record) {
    return record->id == 0 && record->name.ptr == NULL &&
           record->name.len == 0 && opt_absent(&record->body) &&
           record->record_type.ptr == NULL && record->record_type.len == 0 &&
           record->record_id == 0 && record->created_at == 0 &&
           record->updated_at == 0;
}

static bool record_is(const cf_rich_text_record *r, int64_t id, cf_str name,
                      const char *body, cf_str record_type, int64_t record_id,
                      int64_t created_at, int64_t updated_at) {
    return r->id == id && str_is(r->name, name.ptr) &&
           opt_is(&r->body, body) &&
           str_is(r->record_type, record_type.ptr) &&
           r->record_id == record_id && r->created_at == created_at &&
           r->updated_at == updated_at;
}

static void exec_or_abort(sqlite3 *handle, const char *sql) {
    CF_REQUIRE(cf_db_test_exec(handle, sql) == SQLITE_OK);
}

static int64_t count_rows(sqlite3 *handle) {
    bool ok = false;
    int64_t count = cf_db_test_i64(
        handle, "SELECT count(*) FROM action_text_rich_texts", &ok);
    CF_CHECK(ok);
    return count;
}

/* --- scratch database + app (D02 writer) --------------------------------- */

typedef struct {
    char *dir;
    char path[CF_DB_TEST_PATH_CAP];
    cf_app *app;
} rtr_world;

static void rtr_world_open(rtr_world *world) {
    memset(world, 0, sizeof *world);
    world->dir = cf_db_test_dir();
    CF_REQUIRE(world->dir != NULL);
    cf_db_test_path(world->path, sizeof world->path, world->dir);

    /* Fresh schema (D-C01) so the writer's own connection opens an existing
     * version-1 database regardless of D02's lazy-open strategy. */
    cf_db *db = NULL;
    CF_REQUIRE(cf_db_open(world->path, false, &db) == CF_OK);
    cf_db_close(db);

    cf_config_entry entries[3] = {
        {"PUBLIC_ORIGIN", RTR_ORIGIN},
        {"SECRET_KEY_BASE", RTR_HEX64},
        {"DATABASE_PATH", world->path},
    };
    cf_config *config = NULL;
    CF_REQUIRE(cf_config_parse(entries, 3, NULL, &config) == CF_OK);
    CF_REQUIRE(cf_app_create(config, &world->app) == CF_OK);
    /* D02: cf_write returns CF_INTERNAL until the writer thread is started.
     * config is borrowed for the call; the app still owns it. */
    CF_REQUIRE(cf_writer_start(world->app, config) == CF_OK);
}

static void rtr_world_dispose(rtr_world *world) {
    cf_writer_stop(world->app);
    cf_app_destroy(world->app);
    world->app = NULL;
    if (world->dir != NULL) {
        cf_db_test_cleanup(world->dir);
        free(world->dir);
        world->dir = NULL;
    }
}

static cf_db *rtr_reader(const rtr_world *world) {
    cf_db *db = NULL;
    CF_REQUIRE(cf_db_open(world->path, true, &db) == CF_OK);
    return db;
}

/* --- find_for (read path, cf_db) ----------------------------------------- */

CF_TEST(find_for_reports_absent_row_as_empty_result) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    CF_REQUIRE(scratch.db != NULL);

    bool found = true;
    cf_rich_text_record out;
    memset(&out, 0xA5, sizeof out); /* the callee must leave it empty */
    CF_CHECK(cf_rich_text_record_find_for(scratch.db, lit("Message"), 1,
                                          lit("body"), &found,
                                          &out) == CF_OK);
    CF_CHECK(!found);
    CF_CHECK(record_empty(&out));
    /* The failed-lookup message channel is not used for Option::None. */

    cf_db_scratch_close(&scratch);
}

CF_TEST(find_for_rejects_missing_outputs_and_database) {
    cf_rich_text_record out;
    memset(&out, 0, sizeof out);
    bool found = false;

    CF_CHECK(cf_rich_text_record_find_for(NULL, lit("Message"), 1, lit("body"),
                                          &found, &out) == CF_INVALID);
    CF_CHECK(!found);
    CF_CHECK(record_empty(&out));

    found = false;
    CF_CHECK(cf_rich_text_record_find_for(NULL, lit("Message"), 1, lit("body"),
                                          NULL, &out) == CF_INVALID);
    CF_CHECK(!found);
    CF_CHECK(record_empty(&out));

    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    found = true;
    CF_CHECK(cf_rich_text_record_find_for(scratch.db, lit("Message"), 1,
                                          lit("body"), &found,
                                          NULL) == CF_INVALID);
    CF_CHECK(!found);
    found = true;
    CF_CHECK(cf_rich_text_record_find_for(scratch.db, lit("Message"), 1,
                                          lit("body"), NULL,
                                          &out) == CF_INVALID);
    /* A NULL found pointer cannot be cleared; the record output is. */
    CF_CHECK(found);
    CF_CHECK(record_empty(&out));
    cf_db_scratch_close(&scratch);
}

CF_TEST(find_for_reads_every_column_and_matches_all_three_keys) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    sqlite3 *handle = cf_db_handle(scratch.db);
    /* Fixture-shaped row from columns_test.rs (id 8001, name "body",
     * record_type "Message", record_id 8005). */
    exec_or_abort(
        handle,
        "INSERT INTO action_text_rich_texts "
        "(id, name, body, record_type, record_id, created_at, updated_at) "
        "VALUES (8001, 'body', '<p>8003</p>', 'Message', 8005, "
        "'" RTR_NOW1_TEXT "', '" RTR_NOW2_TEXT "')");

    bool found = false;
    cf_rich_text_record out;
    memset(&out, 0, sizeof out);
    CF_CHECK(cf_rich_text_record_find_for(scratch.db, lit("Message"), 8005,
                                          lit("body"), &found,
                                          &out) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(record_is(&out, 8001, lit("body"), "<p>8003</p>", lit("Message"),
                       8005, RTR_NOW1_US, RTR_NOW2_US));
    /* owned copies, not SQLite views: strings are NUL-terminated */
    CF_CHECK(out.name.ptr[out.name.len] == '\0');
    CF_CHECK(out.body.value.ptr[out.body.value.len] == '\0');
    CF_CHECK(out.record_type.ptr[out.record_type.len] == '\0');
    cf_rich_text_record_dispose(&out);

    /* Each WHERE term is required: a wrong name, record_type or record_id
     * all miss. */
    found = true;
    memset(&out, 0, sizeof out);
    CF_CHECK(cf_rich_text_record_find_for(scratch.db, lit("Message"), 8005,
                                          lit("title"), &found,
                                          &out) == CF_OK);
    CF_CHECK(!found);
    CF_CHECK(record_empty(&out));

    found = true;
    memset(&out, 0, sizeof out);
    CF_CHECK(cf_rich_text_record_find_for(scratch.db, lit("Room"), 8005,
                                          lit("body"), &found,
                                          &out) == CF_OK);
    CF_CHECK(!found);
    CF_CHECK(record_empty(&out));

    found = true;
    memset(&out, 0, sizeof out);
    CF_CHECK(cf_rich_text_record_find_for(scratch.db, lit("Message"), 8004,
                                          lit("body"), &found,
                                          &out) == CF_OK);
    CF_CHECK(!found);
    CF_CHECK(record_empty(&out));

    cf_db_scratch_close(&scratch);
}

CF_TEST(find_for_keeps_null_body_absent_and_empty_body_present) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    sqlite3 *handle = cf_db_handle(scratch.db);
    exec_or_abort(handle,
                  "INSERT INTO action_text_rich_texts "
                  "(id, name, body, record_type, record_id, created_at, "
                  "updated_at) VALUES (9001, 'body', NULL, 'Message', 9001, "
                  "'" RTR_WHOLE_TEXT "', '" RTR_WHOLE_TEXT "')");
    exec_or_abort(handle,
                  "INSERT INTO action_text_rich_texts "
                  "(id, name, body, record_type, record_id, created_at, "
                  "updated_at) VALUES (9002, 'body', '', 'Message', 9002, "
                  "'" RTR_WHOLE_TEXT "', '" RTR_WHOLE_TEXT "')");

    bool found = false;
    cf_rich_text_record out;
    memset(&out, 0, sizeof out);
    CF_CHECK(cf_rich_text_record_find_for(scratch.db, lit("Message"), 9001,
                                          lit("body"), &found,
                                          &out) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(opt_absent(&out.body));
    CF_CHECK(out.created_at == RTR_WHOLE_US); /* no-fraction text parses */
    cf_rich_text_record_dispose(&out);

    found = false;
    memset(&out, 0, sizeof out);
    CF_CHECK(cf_rich_text_record_find_for(scratch.db, lit("Message"), 9002,
                                          lit("body"), &found,
                                          &out) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(out.body.present);
    CF_CHECK(out.body.value.ptr != NULL && out.body.value.len == 0);
    cf_rich_text_record_dispose(&out);

    cf_db_scratch_close(&scratch);
}

CF_TEST(find_for_matches_record_type_and_name_exactly) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    sqlite3 *handle = cf_db_handle(scratch.db);
    exec_or_abort(handle,
                  "INSERT INTO action_text_rich_texts "
                  "(id, name, body, record_type, record_id, created_at, "
                  "updated_at) VALUES (1, 'body', 'x', 'Message', 5, '"
                  RTR_WHOLE_TEXT "', '" RTR_WHOLE_TEXT "')");

    bool found = false;
    cf_rich_text_record out;
    memset(&out, 0, sizeof out);
    /* A prefix of the stored type is not the stored type. */
    CF_CHECK(cf_rich_text_record_find_for(scratch.db, lit("Messag"), 5,
                                          lit("body"), &found,
                                          &out) == CF_OK);
    CF_CHECK(!found);
    CF_CHECK(record_empty(&out));
    cf_db_scratch_close(&scratch);
}

/* --- create (mutation path, cf_tx via D02 cf_write) ---------------------- */

typedef struct {
    cf_err rc;
    cf_rich_text_record record;
} rtr_create_arg;

static cf_err rtr_write_create(cf_tx *tx, void *arg) {
    rtr_create_arg *a = arg;
    a->rc = cf_rich_text_record_create(tx, lit("Message"), 42, lit("body"),
                                       lit("<p>hello</p>"), &a->record);
    return a->rc;
}

CF_TEST(create_returns_generated_record_and_stores_reference_datetime_text) {
    rtr_world world;
    rtr_world_open(&world);

    rtr_create_arg arg;
    memset(&arg, 0, sizeof arg);
    cf_test_clock_set_fixed_us(RTR_NOW1_US);
    CF_CHECK(cf_write(world.app, rtr_write_create, &arg) == CF_OK);
    cf_test_clock_clear();

    CF_CHECK(arg.rc == CF_OK);
    CF_CHECK(arg.record.id == 1); /* fresh AUTOINCREMENT database */
    CF_CHECK(record_is(&arg.record, 1, lit("body"), "<p>hello</p>",
                       lit("Message"), 42, RTR_NOW1_US, RTR_NOW1_US));
    cf_rich_text_record_dispose(&arg.record);

    /* Committed state read on a separate reader. */
    cf_db *reader = rtr_reader(&world);
    sqlite3 *handle = cf_db_handle(reader);
    CF_CHECK(count_rows(handle) == 1);

    char text[128];
    const char *created = cf_db_test_text(
        handle, "SELECT created_at FROM action_text_rich_texts WHERE id = 1",
        text, sizeof text);
    CF_CHECK(created != NULL && strcmp(created, RTR_NOW1_TEXT) == 0);
    const char *updated = cf_db_test_text(
        handle, "SELECT updated_at FROM action_text_rich_texts WHERE id = 1",
        text, sizeof text);
    CF_CHECK(updated != NULL && strcmp(updated, RTR_NOW1_TEXT) == 0);
    const char *body = cf_db_test_text(
        handle, "SELECT body FROM action_text_rich_texts WHERE id = 1", text,
        sizeof text);
    CF_CHECK(body != NULL && strcmp(body, "<p>hello</p>") == 0);
    const char *type = cf_db_test_text(
        handle,
        "SELECT record_type || ':' || name || ':' || record_id "
        "FROM action_text_rich_texts WHERE id = 1",
        text, sizeof text);
    CF_CHECK(type != NULL && strcmp(type, "Message:body:42") == 0);

    bool found = false;
    cf_rich_text_record got;
    memset(&got, 0, sizeof got);
    CF_CHECK(cf_rich_text_record_find_for(reader, lit("Message"), 42,
                                          lit("body"), &found,
                                          &got) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(record_is(&got, 1, lit("body"), "<p>hello</p>", lit("Message"),
                       42, RTR_NOW1_US, RTR_NOW1_US));
    cf_rich_text_record_dispose(&got);
    cf_db_close(reader);

    rtr_world_dispose(&world);
}

CF_TEST(create_whole_second_omits_fractional_datetime_text) {
    rtr_world world;
    rtr_world_open(&world);

    rtr_create_arg arg;
    memset(&arg, 0, sizeof arg);
    cf_test_clock_set_fixed_us(RTR_WHOLE_US);
    CF_CHECK(cf_write(world.app, rtr_write_create, &arg) == CF_OK);
    cf_test_clock_clear();

    CF_CHECK(arg.rc == CF_OK);
    CF_CHECK(arg.record.created_at == RTR_WHOLE_US);
    CF_CHECK(arg.record.updated_at == RTR_WHOLE_US);
    cf_rich_text_record_dispose(&arg.record);

    cf_db *reader = rtr_reader(&world);
    sqlite3 *handle = cf_db_handle(reader);
    char text[128];
    const char *created = cf_db_test_text(
        handle, "SELECT created_at FROM action_text_rich_texts WHERE id = 1",
        text, sizeof text);
    CF_CHECK(created != NULL && strcmp(created, RTR_WHOLE_TEXT) == 0);
    cf_db_close(reader);
    rtr_world_dispose(&world);
}

CF_TEST(create_rejects_missing_output_or_transaction) {
    cf_rich_text_record out;
    memset(&out, 0x5A, sizeof out);
    CF_CHECK(cf_rich_text_record_create(NULL, lit("Message"), 1, lit("body"),
                                        lit("x"), &out) == CF_INVALID);
    CF_CHECK(record_empty(&out));
    CF_CHECK(cf_rich_text_record_create(NULL, lit("Message"), 1, lit("body"),
                                        lit("x"), NULL) == CF_INVALID);
}

typedef struct {
    cf_err first;
    cf_err duplicate;
    cf_err other_name;
} rtr_duplicate_arg;

static cf_err rtr_write_duplicate(cf_tx *tx, void *arg) {
    rtr_duplicate_arg *a = arg;
    cf_rich_text_record first, duplicate, other_name;
    memset(&first, 0, sizeof first);
    memset(&duplicate, 0, sizeof duplicate);
    memset(&other_name, 0, sizeof other_name);
    a->first = cf_rich_text_record_create(tx, lit("Message"), 7, lit("body"),
                                          lit("one"), &first);
    a->duplicate = cf_rich_text_record_create(tx, lit("Message"), 7,
                                              lit("body"), lit("two"),
                                              &duplicate);
    /* Same record and name, different key: the index allows it. */
    a->other_name = cf_rich_text_record_create(tx, lit("Message"), 7,
                                               lit("title"), lit("three"),
                                               &other_name);
    cf_rich_text_record_dispose(&first);
    cf_rich_text_record_dispose(&duplicate);
    cf_rich_text_record_dispose(&other_name);
    /* The duplicate failure is the case under test, not a failed callback:
     * commit the two valid rows so the uniqueness outcome is visible. */
    if (a->first != CF_OK) return a->first;
    if (a->other_name != CF_OK) return a->other_name;
    return CF_OK;
}

CF_TEST(create_duplicate_unique_key_is_invalid_and_keeps_existing_row) {
    rtr_world world;
    rtr_world_open(&world);

    rtr_duplicate_arg arg;
    memset(&arg, 0, sizeof arg);
    cf_test_clock_set_fixed_us(RTR_NOW1_US);
    CF_CHECK(cf_write(world.app, rtr_write_duplicate, &arg) == CF_OK);
    cf_test_clock_clear();

    CF_CHECK(arg.first == CF_OK);
    CF_CHECK(arg.duplicate == CF_INVALID); /* SQLITE_CONSTRAINT -> CF_INVALID */
    CF_CHECK(arg.other_name == CF_OK);

    cf_db *reader = rtr_reader(&world);
    sqlite3 *handle = cf_db_handle(reader);
    CF_CHECK(count_rows(handle) == 2);

    bool found = false;
    cf_rich_text_record got;
    memset(&got, 0, sizeof got);
    CF_CHECK(cf_rich_text_record_find_for(reader, lit("Message"), 7,
                                          lit("body"), &found,
                                          &got) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(opt_is(&got.body, "one")); /* the duplicate did not replace it */
    cf_rich_text_record_dispose(&got);
    memset(&got, 0, sizeof got);
    CF_CHECK(cf_rich_text_record_find_for(reader, lit("Message"), 7,
                                          lit("title"), &found,
                                          &got) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(opt_is(&got.body, "three"));
    cf_rich_text_record_dispose(&got);
    cf_db_close(reader);
    rtr_world_dispose(&world);
}

/* --- update_body --------------------------------------------------------- */

typedef struct {
    cf_err create_rc;
    int64_t id;
    cf_err find_rc;
    bool found;
    cf_err update_rc;
    cf_rich_text_record record;
} rtr_update_arg;

static cf_err rtr_write_create_for_update(cf_tx *tx, void *arg) {
    rtr_update_arg *a = arg;
    cf_rich_text_record created;
    memset(&created, 0, sizeof created);
    a->create_rc = cf_rich_text_record_create(tx, lit("Message"), 99,
                                              lit("body"), lit("before"),
                                              &created);
    a->id = created.id;
    cf_rich_text_record_dispose(&created);
    return a->create_rc;
}

static cf_err rtr_write_update(cf_tx *tx, void *arg) {
    rtr_update_arg *a = arg;
    a->find_rc = cf_rich_text_record_find_for(cf_tx_db(tx), lit("Message"), 99,
                                              lit("body"), &a->found,
                                              &a->record);
    if (a->find_rc != CF_OK || !a->found) {
        return a->find_rc != CF_OK ? a->find_rc : CF_NOT_FOUND;
    }
    a->update_rc =
        cf_rich_text_record_update_body(tx, &a->record, lit("<p>after</p>"));
    return a->update_rc;
}

static cf_err rtr_write_update_empty(cf_tx *tx, void *arg) {
    (void)arg;
    cf_rich_text_record record;
    memset(&record, 0, sizeof record);
    bool found = false;
    cf_err rc = cf_rich_text_record_find_for(cf_tx_db(tx), lit("Message"), 99,
                                             lit("body"), &found, &record);
    if (rc != CF_OK || !found) {
        cf_rich_text_record_dispose(&record);
        return rc != CF_OK ? rc : CF_NOT_FOUND;
    }
    rc = cf_rich_text_record_update_body(tx, &record, lit(""));
    cf_rich_text_record_dispose(&record);
    return rc;
}

CF_TEST(update_body_changes_body_and_updated_at_only) {
    rtr_world world;
    rtr_world_open(&world);
    rtr_update_arg arg;
    memset(&arg, 0, sizeof arg);

    cf_test_clock_set_fixed_us(RTR_NOW1_US);
    CF_CHECK(cf_write(world.app, rtr_write_create_for_update, &arg) == CF_OK);
    CF_CHECK(arg.create_rc == CF_OK);
    cf_test_clock_clear();

    /* The record is read and updated inside the same write transaction. */
    cf_test_clock_set_fixed_us(RTR_NOW2_US);
    CF_CHECK(cf_write(world.app, rtr_write_update, &arg) == CF_OK);
    cf_test_clock_clear();
    CF_CHECK(arg.find_rc == CF_OK);
    CF_CHECK(arg.found);
    CF_CHECK(arg.update_rc == CF_OK);
    CF_CHECK(arg.record.id == arg.id);
    CF_CHECK(str_is(arg.record.name, "body"));
    CF_CHECK(str_is(arg.record.record_type, "Message"));
    CF_CHECK(arg.record.record_id == 99);
    CF_CHECK(opt_is(&arg.record.body, "<p>after</p>"));
    CF_CHECK(arg.record.created_at == RTR_NOW1_US); /* untouched */
    CF_CHECK(arg.record.updated_at == RTR_NOW2_US);
    cf_rich_text_record_dispose(&arg.record);

    cf_db *reader = rtr_reader(&world);
    sqlite3 *handle = cf_db_handle(reader);
    char text[160];
    const char *row = cf_db_test_text(
        handle,
        "SELECT body || '|' || created_at || '|' || updated_at "
        "FROM action_text_rich_texts WHERE id = 1",
        text, sizeof text);
    CF_CHECK(row != NULL &&
             strcmp(row, "<p>after</p>|" RTR_NOW1_TEXT "|" RTR_NOW2_TEXT) ==
                 0);
    cf_db_close(reader);

    /* Ordinary edge: an empty body is stored as empty text, present. */
    cf_test_clock_set_fixed_us(RTR_WHOLE_US);
    CF_CHECK(cf_write(world.app, rtr_write_update_empty, NULL) == CF_OK);
    cf_test_clock_clear();

    reader = rtr_reader(&world);
    handle = cf_db_handle(reader);
    bool ok = false;
    CF_CHECK(cf_db_test_i64(handle,
                            "SELECT count(*) FROM action_text_rich_texts "
                            "WHERE id = 1 AND body IS NOT NULL AND body = '' "
                            "AND updated_at = '" RTR_WHOLE_TEXT "'",
                            &ok) == 1);
    CF_CHECK(ok);
    cf_db_close(reader);
    rtr_world_dispose(&world);
}

typedef struct {
    cf_err rc;
    cf_rich_text_record record;
} rtr_stale_arg;

static cf_err rtr_write_stale_update(cf_tx *tx, void *arg) {
    rtr_stale_arg *a = arg;
    a->record.id = 987654321; /* no such row */
    a->rc = cf_rich_text_record_update_body(tx, &a->record, lit("ghost"));
    return a->rc;
}

CF_TEST(update_body_of_missing_row_matches_reference) {
    rtr_world world;
    rtr_world_open(&world);

    rtr_stale_arg arg;
    memset(&arg, 0, sizeof arg);
    cf_test_clock_set_fixed_us(RTR_NOW1_US);
    CF_CHECK(cf_write(world.app, rtr_write_stale_update, &arg) == CF_OK);
    cf_test_clock_clear();

    /* Reference update_body ignores execute's affected-row count and still
     * updates the in-memory record. */
    CF_CHECK(arg.rc == CF_OK);
    CF_CHECK(opt_is(&arg.record.body, "ghost"));
    CF_CHECK(arg.record.updated_at == RTR_NOW1_US);
    cf_rich_text_record_dispose(&arg.record);

    cf_db *reader = rtr_reader(&world);
    CF_CHECK(count_rows(cf_db_handle(reader)) == 0);
    cf_db_close(reader);
    rtr_world_dispose(&world);
}

CF_TEST(update_body_rejects_missing_record_or_transaction) {
    cf_rich_text_record record;
    memset(&record, 0, sizeof record);
    CF_CHECK(cf_rich_text_record_update_body(NULL, &record,
                                             lit("x")) == CF_INVALID);
    CF_CHECK(opt_absent(&record.body));
    CF_CHECK(cf_rich_text_record_update_body(NULL, NULL, lit("x")) ==
             CF_INVALID);
    cf_rich_text_record_dispose(&record);
}

/* --- delete -------------------------------------------------------------- */

typedef struct {
    cf_err create_rc;
    int64_t id;
    cf_err find_rc;
    bool found;
    cf_err delete_rc;
    cf_rich_text_record record;
} rtr_delete_arg;

static cf_err rtr_write_delete(cf_tx *tx, void *arg) {
    rtr_delete_arg *a = arg;
    if (a->id == 0) {
        cf_rich_text_record created;
        memset(&created, 0, sizeof created);
        a->create_rc = cf_rich_text_record_create(tx, lit("Message"), 123,
                                                  lit("body"), lit("gone"),
                                                  &created);
        a->id = created.id;
        cf_rich_text_record_dispose(&created);
        return a->create_rc;
    }
    if (!a->found) {
        a->find_rc = cf_rich_text_record_find_for(cf_tx_db(tx), lit("Message"),
                                                  123, lit("body"), &a->found,
                                                  &a->record);
        if (a->find_rc != CF_OK || !a->found) {
            return a->find_rc != CF_OK ? a->find_rc : CF_NOT_FOUND;
        }
    }
    a->delete_rc = cf_rich_text_record_delete(tx, &a->record);
    return a->delete_rc;
}

static cf_err rtr_write_stale_delete(cf_tx *tx, void *arg) {
    rtr_delete_arg *a = arg;
    a->delete_rc = cf_rich_text_record_delete(tx, &a->record);
    return a->delete_rc;
}

CF_TEST(delete_removes_row_and_stale_delete_succeeds) {
    rtr_world world;
    rtr_world_open(&world);

    rtr_delete_arg arg;
    memset(&arg, 0, sizeof arg);
    cf_test_clock_set_fixed_us(RTR_NOW1_US);
    CF_CHECK(cf_write(world.app, rtr_write_delete, &arg) == CF_OK);
    cf_test_clock_clear();
    CF_CHECK(arg.create_rc == CF_OK);
    CF_CHECK(arg.id == 1);

    CF_CHECK(cf_write(world.app, rtr_write_delete, &arg) == CF_OK);
    CF_CHECK(arg.find_rc == CF_OK);
    CF_CHECK(arg.found);
    CF_CHECK(arg.delete_rc == CF_OK);
    cf_rich_text_record_dispose(&arg.record);

    cf_db *reader = rtr_reader(&world);
    bool found = true;
    cf_rich_text_record got;
    memset(&got, 0, sizeof got);
    CF_CHECK(cf_rich_text_record_find_for(reader, lit("Message"), 123,
                                          lit("body"), &found,
                                          &got) == CF_OK);
    CF_CHECK(!found);
    CF_CHECK(count_rows(cf_db_handle(reader)) == 0);
    cf_db_close(reader);

    /* Reference delete ignores the affected count: deleting the stale record
     * again returns Ok and changes nothing. */
    arg.record.id = arg.id; /* dispose zeroed it; only the id is needed */
    CF_CHECK(cf_write(world.app, rtr_write_stale_delete, &arg) == CF_OK);
    CF_CHECK(arg.delete_rc == CF_OK);
    reader = rtr_reader(&world);
    CF_CHECK(count_rows(cf_db_handle(reader)) == 0);
    cf_db_close(reader);

    rtr_world_dispose(&world);
}

CF_TEST(delete_rejects_missing_record_or_transaction) {
    cf_rich_text_record record;
    memset(&record, 0, sizeof record);
    CF_CHECK(cf_rich_text_record_delete(NULL, &record) == CF_INVALID);
    CF_CHECK(cf_rich_text_record_delete(NULL, NULL) == CF_INVALID);
}

/* --- record/vector disposal ---------------------------------------------- */

CF_TEST(dispose_helpers_accept_null_and_reset) {
    cf_rich_text_record_dispose(NULL);
    cf_rich_text_record_vector_dispose(NULL);

    cf_rich_text_record_vector vector;
    memset(&vector, 0, sizeof vector);
    vector.items = calloc(2, sizeof *vector.items);
    CF_REQUIRE(vector.items != NULL);
    vector.len = 2;
    vector.cap = 2;
    /* Element zero owns allocations; dispose must reach and release them
     * (ASan leak checking is the observable). */
    vector.items[0].name.ptr = malloc(6);
    CF_REQUIRE(vector.items[0].name.ptr != NULL);
    memcpy(vector.items[0].name.ptr, "body", 5);
    vector.items[0].name.len = 4; /* deliberately shorter than the NUL body */
    vector.items[0].body.present = true;
    vector.items[0].body.value.ptr = malloc(2);
    CF_REQUIRE(vector.items[0].body.value.ptr != NULL);
    vector.items[0].body.value.ptr[0] = 'x';
    vector.items[0].body.value.ptr[1] = '\0';
    vector.items[0].body.value.len = 1;
    vector.items[1].record_type.ptr = malloc(8);
    CF_REQUIRE(vector.items[1].record_type.ptr != NULL);
    memcpy(vector.items[1].record_type.ptr, "Message", 8);
    vector.items[1].record_type.len = 7;

    cf_rich_text_record_vector_dispose(&vector);
    CF_CHECK(vector.items == NULL);
    CF_CHECK(vector.len == 0);
    CF_CHECK(vector.cap == 0);
}

CF_TEST_MAIN()
