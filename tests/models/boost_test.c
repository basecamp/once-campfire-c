/* tests/models/boost_test.c — D01 model family "boost" tests.
 *
 * Oracle: tmp/rust-ref/crates/db/src/models/boost.rs plus the pinned reference
 * tests that cover this model:
 *  - tests/fixtures/crates/db/src/tests/columns_test.rs (`at(n)`, the
 *    for_message_ordered and per-column expectations),
 *  - tests/fixtures/crates/db/src/tests/callbacks_test.rs
 *    (boosting_touches_the_message_and_room_and_reindexes,
 *    boosts_are_ordered_by_creation).
 * The C fixture rows below are a minimal deterministic transcription of the
 * reference fixture shape (user/room/message/boost), not the Ruby/Toml
 * fixture corpus, which has no runnable C artifact.
 *
 * Mutations run through the landed D02 writer (cf_app + cf_writer_start +
 * cf_write), so boost.c sees the real cf_tx contract.
 *
 * Build (plain; add -fsanitize=address,undefined -fno-sanitize-recover=all
 * -fno-omit-frame-pointer -g for the sanitize run).  src/models/message.c is
 * not landed yet in the shared checkout, so the local verification link adds
 * a message scaffold (see the evidence file); the production link substitutes
 * src/models/message.c for it:
 *   clang -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra
 *         -Werror -pthread -Isrc -Itests -Ivendor/src/sqlite/sqlite-amalgamation-3530400
 *         src/models/boost.c src/models/message.c src/core/alloc.c
 *         src/core/buffer.c src/core/clock.c src/core/error.c src/core/random.c
 *         src/db/schema.c src/db/reader.c src/db/statements.c src/db/writer.c
 *         src/app.c src/config.c tests/models/boost_test.c
 *         vendor/build/sqlite-clang/libsqlite3.a -lm
 *         -o build/d01-boost/plain/test_boost
 */
#include "cf_test.h"

#include "app.h"
#include "cf.h"
#include "config.h"
#include "core/testclock.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "db/writer.h"
#include "models/boost.h"
#include "models/message.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- fixture: scratch db + app + started writer ---------------------------
 * Mirrors tests/db/test_writer.c.  The scratch connection is this test's own
 * connection: it seeds fixture rows before a write and reads committed rows
 * after cf_write returns; mutations go through the writer thread only. */

typedef struct {
    cf_db_scratch scratch;
    cf_app *app;
    bool writer_started;
} boost_fixture;

static cf_config *boost_config(const char *db_path) {
    static const char hex64[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    cf_config_entry entries[4] = {
        {"PUBLIC_ORIGIN", "http://127.0.0.1:3999"},
        {"SECRET_KEY_BASE", hex64},
        {"DATABASE_PATH", db_path},
        {"CF_WRITER_QUEUE", "4"},
    };
    cf_config *config = NULL;
    if (cf_config_parse(entries, 4, NULL, &config) != CF_OK) return NULL;
    return config;
}

static bool fixture_open(boost_fixture *fixture) {
    memset(fixture, 0, sizeof *fixture);
    if (!cf_db_scratch_open(&fixture->scratch)) return false;
    cf_config *config = boost_config(fixture->scratch.path);
    if (config == NULL) return false;
    if (cf_app_create(config, &fixture->app) != CF_OK) return false;
    if (cf_writer_start(fixture->app, config) != CF_OK) return false;
    fixture->writer_started = true;
    return true;
}

static void fixture_close(boost_fixture *fixture) {
    if (fixture->writer_started) cf_writer_stop(fixture->app);
    if (fixture->app != NULL) cf_app_destroy(fixture->app); /* frees config */
    cf_db_scratch_close(&fixture->scratch);
}

/* --- deterministic timestamps ---------------------------------------------
 * Same spacing as the reference columns_test.rs oracle:
 * at(n) = 1_790_000_000_000_000 + n * 1_000_123 microseconds. */
static int64_t at_us(int64_t n) {
    return INT64_C(1790000000000000) + n * INT64_C(1000123);
}

/* --- raw fixture helpers (test-side SQL, independent of the model) -------- */

static bool exec_sql(cf_db *db, const char *sql) {
    return cf_db_test_exec(cf_db_handle(db), sql) == SQLITE_OK;
}

static int64_t query_scalar_i64(cf_db *db, const char *sql, int64_t arg,
                                bool *ok) {
    *ok = false;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL) !=
        SQLITE_OK) {
        return 0;
    }
    if (arg != INT64_MIN &&
        sqlite3_bind_int64(stmt, 1, arg) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return 0;
    }
    int64_t value = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        value = sqlite3_column_int64(stmt, 0);
        *ok = true;
    }
    sqlite3_finalize(stmt);
    return value;
}

static int64_t query_column_i64(cf_db *db, const char *sql, int64_t arg) {
    bool ok = false;
    int64_t value = query_scalar_i64(db, sql, arg, &ok);
    CF_REQUIRE(ok);
    return value;
}

static int64_t boost_count(cf_db *db) {
    return query_column_i64(db, "SELECT COUNT(*) FROM \"boosts\"", INT64_MIN);
}

static int64_t query_time_us(cf_db *db, const char *sql, int64_t arg) {
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL) ==
               SQLITE_OK);
    CF_REQUIRE(sqlite3_bind_int64(stmt, 1, arg) == SQLITE_OK);
    CF_REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    int64_t us = 0;
    cf_err rc = cf_db_time_from_text(cf_stmt_column_text(stmt, 0), &us);
    CF_REQUIRE(sqlite3_finalize(stmt) == SQLITE_OK);
    CF_REQUIRE(rc == CF_OK);
    return us;
}

static int64_t message_updated_us(cf_db *db, int64_t message_id) {
    return query_time_us(
        db, "SELECT \"updated_at\" FROM \"messages\" WHERE \"id\" = ?",
        message_id);
}

static int64_t room_updated_us(cf_db *db, int64_t room_id) {
    return query_time_us(
        db, "SELECT \"updated_at\" FROM \"rooms\" WHERE \"id\" = ?", room_id);
}

static bool query_text_is(cf_db *db, const char *sql, int64_t arg,
                          const char *expected) {
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL) ==
               SQLITE_OK);
    CF_REQUIRE(sqlite3_bind_int64(stmt, 1, arg) == SQLITE_OK);
    bool equal = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        int len = sqlite3_column_bytes(stmt, 0);
        size_t want = strlen(expected);
        equal = text != NULL && len >= 0 && (size_t)len == want &&
                memcmp(text, expected, want) == 0;
    }
    CF_REQUIRE(sqlite3_finalize(stmt) == SQLITE_OK);
    return equal;
}

static void insert_user(cf_db *db, int64_t id, int64_t ts) {
    char text[CF_DB_TIME_TEXT_CAP];
    CF_REQUIRE(cf_db_time_to_text(ts, text) == CF_OK);
    char sql[256];
    snprintf(sql, sizeof sql,
             "INSERT INTO \"users\" (\"id\", \"name\", \"created_at\", "
             "\"updated_at\") VALUES (%lld, 'User', '%s', '%s')",
             (long long)id, text, text);
    CF_REQUIRE(exec_sql(db, sql));
}

static void insert_room(cf_db *db, int64_t id, int64_t creator_id,
                        int64_t ts) {
    char text[CF_DB_TIME_TEXT_CAP];
    CF_REQUIRE(cf_db_time_to_text(ts, text) == CF_OK);
    char sql[256];
    snprintf(sql, sizeof sql,
             "INSERT INTO \"rooms\" (\"id\", \"created_at\", \"creator_id\", "
             "\"type\", \"updated_at\") VALUES (%lld, '%s', %lld, "
             "'Rooms::Open', '%s')",
             (long long)id, text, (long long)creator_id, text);
    CF_REQUIRE(exec_sql(db, sql));
}

static void insert_message(cf_db *db, int64_t id, int64_t room_id,
                           int64_t creator_id, int64_t ts) {
    char text[CF_DB_TIME_TEXT_CAP];
    CF_REQUIRE(cf_db_time_to_text(ts, text) == CF_OK);
    char sql[256];
    snprintf(sql, sizeof sql,
             "INSERT INTO \"messages\" (\"id\", \"client_message_id\", "
             "\"created_at\", \"creator_id\", \"room_id\", \"updated_at\") "
             "VALUES (%lld, 'cm-%lld', '%s', %lld, %lld, '%s')",
             (long long)id, (long long)id, text, (long long)creator_id,
             (long long)room_id, text);
    CF_REQUIRE(exec_sql(db, sql));
}

static void insert_boost_row(cf_db *db, int64_t id, int64_t message_id,
                             int64_t booster_id, const char *content,
                             int64_t created_us, int64_t updated_us) {
    char created[CF_DB_TIME_TEXT_CAP];
    char updated[CF_DB_TIME_TEXT_CAP];
    CF_REQUIRE(cf_db_time_to_text(created_us, created) == CF_OK);
    CF_REQUIRE(cf_db_time_to_text(updated_us, updated) == CF_OK);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(
                   cf_db_handle(db),
                   "INSERT INTO \"boosts\" (\"id\", \"message_id\", "
                   "\"booster_id\", \"content\", \"created_at\", "
                   "\"updated_at\") VALUES (?, ?, ?, ?, ?, ?)",
                   -1, &stmt, NULL) == SQLITE_OK);
    CF_REQUIRE(sqlite3_bind_int64(stmt, 1, id) == SQLITE_OK);
    CF_REQUIRE(sqlite3_bind_int64(stmt, 2, message_id) == SQLITE_OK);
    CF_REQUIRE(sqlite3_bind_int64(stmt, 3, booster_id) == SQLITE_OK);
    CF_REQUIRE(sqlite3_bind_text(stmt, 4, content, -1, SQLITE_TRANSIENT) ==
               SQLITE_OK);
    CF_REQUIRE(sqlite3_bind_text(stmt, 5, created, -1, SQLITE_TRANSIENT) ==
               SQLITE_OK);
    CF_REQUIRE(sqlite3_bind_text(stmt, 6, updated, -1, SQLITE_TRANSIENT) ==
               SQLITE_OK);
    CF_REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
    CF_REQUIRE(sqlite3_finalize(stmt) == SQLITE_OK);
}

/* user 1, room 1, messages 7001 and 7002; used by the read tests. */
static void seed_messages(cf_db *db) {
    insert_user(db, 1, at_us(0));
    insert_room(db, 1, 1, at_us(0));
    insert_message(db, 7001, 1, 1, at_us(0));
    insert_message(db, 7002, 1, 1, at_us(0));
}

/* The transaction callbacks; cf_write_fn has no closures. */
typedef struct {
    int64_t message_id;
    int64_t booster_id;
    cf_str content;
    cf_boost *out;
} create_args;

static cf_err create_fn(cf_tx *tx, void *arg) {
    create_args *args = arg;
    return cf_boost_create(tx, args->message_id, args->booster_id,
                           args->content, args->out);
}

typedef struct {
    const cf_boost *boost;
} boost_args;

static cf_err destroy_fn(cf_tx *tx, void *arg) {
    return cf_boost_destroy(tx, ((boost_args *)arg)->boost);
}

static cf_err delete_row_fn(cf_tx *tx, void *arg) {
    return cf_boost_delete_row(tx, ((boost_args *)arg)->boost);
}

static bool vector_has_id(const cf_boost_vector *vector, int64_t id) {
    for (size_t i = 0; i < vector->len; i++) {
        if (vector->items[i].id == id) return true;
    }
    return false;
}

/* --- reads ---------------------------------------------------------------- */

CF_TEST(find_returns_row_and_not_found) {
    boost_fixture fixture;
    CF_REQUIRE(fixture_open(&fixture));
    cf_db *db = fixture.scratch.db;
    seed_messages(db);
    insert_boost_row(db, 7001, 7001, 1, "Hello", at_us(1), at_us(2));
    insert_boost_row(db, 7002, 7001, 1, "\xf0\x9f\x91\x8d boost", at_us(3),
                     at_us(4));
    insert_boost_row(db, 7003, 7002, 1, "", at_us(5), at_us(5));

    cf_boost boost = {0};
    CF_REQUIRE(cf_boost_find(db, 7001, &boost) == CF_OK);
    CF_CHECK(boost.id == 7001);
    CF_CHECK(boost.message_id == 7001);
    CF_CHECK(boost.booster_id == 1);
    CF_CHECK(boost.content.ptr != NULL && boost.content.len == 5);
    CF_CHECK(boost.content.ptr != NULL &&
             strcmp(boost.content.ptr, "Hello") == 0);
    CF_CHECK(boost.content.ptr != NULL &&
             boost.content.ptr[boost.content.len] == '\0');
    CF_CHECK(boost.created_at == at_us(1));
    CF_CHECK(boost.updated_at == at_us(2));
    cf_boost_dispose(&boost);

    /* UTF-8 bytes round-trip exactly (byte length, not code points). */
    CF_REQUIRE(cf_boost_find(db, 7002, &boost) == CF_OK);
    const char *emoji = "\xf0\x9f\x91\x8d boost";
    CF_CHECK(boost.content.len == strlen(emoji));
    CF_CHECK(boost.content.ptr != NULL &&
             memcmp(boost.content.ptr, emoji, strlen(emoji)) == 0);
    cf_boost_dispose(&boost);

    /* Empty content is copied empty text, not absent. */
    CF_REQUIRE(cf_boost_find(db, 7003, &boost) == CF_OK);
    CF_CHECK(boost.content.ptr != NULL);
    CF_CHECK(boost.content.len == 0);
    CF_CHECK(boost.content.ptr != NULL && boost.content.ptr[0] == '\0');
    cf_boost_dispose(&boost);

    /* Missing id is CF_NOT_FOUND with an empty record. */
    cf_boost absent = {0};
    CF_CHECK(cf_boost_find(db, 9999, &absent) == CF_NOT_FOUND);
    CF_CHECK(absent.id == 0 && absent.content.ptr == NULL &&
             absent.content.len == 0);

    CF_CHECK(cf_boost_find(db, 7001, NULL) == CF_INVALID);

    fixture_close(&fixture);
}

CF_TEST(for_message_returns_matching_rows) {
    boost_fixture fixture;
    CF_REQUIRE(fixture_open(&fixture));
    cf_db *db = fixture.scratch.db;
    seed_messages(db);
    insert_boost_row(db, 11, 7001, 1, "one", at_us(10), at_us(10));
    insert_boost_row(db, 12, 7001, 2, "two", at_us(30), at_us(30));
    insert_boost_row(db, 13, 7001, 3, "three", at_us(20), at_us(20));
    insert_boost_row(db, 14, 7002, 1, "other", at_us(40), at_us(40));

    cf_boost_vector boosts = {0};
    CF_REQUIRE(cf_boost_for_message(db, 7001, &boosts) == CF_OK);
    CF_CHECK(boosts.len == 3);
    CF_CHECK(boosts.items != NULL && boosts.cap >= boosts.len);
    CF_CHECK(vector_has_id(&boosts, 11));
    CF_CHECK(vector_has_id(&boosts, 12));
    CF_CHECK(vector_has_id(&boosts, 13));
    CF_CHECK(!vector_has_id(&boosts, 14));
    for (size_t i = 0; i < boosts.len; i++) {
        CF_CHECK(boosts.items[i].message_id == 7001);
        CF_CHECK(boosts.items[i].content.ptr != NULL);
    }
    cf_boost_vector_dispose(&boosts);

    CF_REQUIRE(cf_boost_for_message(db, 7002, &boosts) == CF_OK);
    CF_CHECK(boosts.len == 1);
    CF_CHECK(vector_has_id(&boosts, 14));
    cf_boost_vector_dispose(&boosts);

    /* No rows is success with an empty vector, not CF_NOT_FOUND. */
    CF_REQUIRE(cf_boost_for_message(db, 7999, &boosts) == CF_OK);
    CF_CHECK(boosts.len == 0 && boosts.items == NULL && boosts.cap == 0);

    CF_CHECK(cf_boost_for_message(NULL, 7001, &boosts) == CF_INVALID);
    CF_CHECK(boosts.len == 0 && boosts.items == NULL);
    CF_CHECK(cf_boost_for_message(db, 7001, NULL) == CF_INVALID);

    fixture_close(&fixture);
}

CF_TEST(for_message_ordered_sorts_by_created_at) {
    boost_fixture fixture;
    CF_REQUIRE(fixture_open(&fixture));
    cf_db *db = fixture.scratch.db;
    seed_messages(db);
    insert_boost_row(db, 21, 7001, 1, "late", at_us(30), at_us(30));
    insert_boost_row(db, 22, 7001, 2, "early", at_us(10), at_us(10));
    insert_boost_row(db, 23, 7001, 3, "middle", at_us(20), at_us(20));
    insert_boost_row(db, 24, 7002, 1, "other", at_us(5), at_us(5));

    cf_boost_vector boosts = {0};
    CF_REQUIRE(cf_boost_for_message_ordered(db, 7001, &boosts) == CF_OK);
    CF_REQUIRE(boosts.len == 3);
    CF_CHECK(boosts.items[0].id == 22 && boosts.items[0].created_at == at_us(10));
    CF_CHECK(boosts.items[1].id == 23 && boosts.items[1].created_at == at_us(20));
    CF_CHECK(boosts.items[2].id == 21 && boosts.items[2].created_at == at_us(30));
    for (size_t i = 1; i < boosts.len; i++) {
        CF_CHECK(boosts.items[i - 1].created_at <= boosts.items[i].created_at);
    }
    cf_boost_vector_dispose(&boosts);

    CF_REQUIRE(cf_boost_for_message_ordered(db, 7999, &boosts) == CF_OK);
    CF_CHECK(boosts.len == 0 && boosts.items == NULL);

    CF_CHECK(cf_boost_for_message_ordered(NULL, 7001, &boosts) == CF_INVALID);
    CF_CHECK(cf_boost_for_message_ordered(db, 7001, NULL) == CF_INVALID);

    fixture_close(&fixture);
}

CF_TEST(find_by_message_and_booster_scopes_all_three) {
    boost_fixture fixture;
    CF_REQUIRE(fixture_open(&fixture));
    cf_db *db = fixture.scratch.db;
    seed_messages(db);
    insert_boost_row(db, 31, 7001, 1, "mine", at_us(10), at_us(10));
    insert_boost_row(db, 32, 7002, 2, "theirs", at_us(11), at_us(11));

    cf_boost boost = {0};
    CF_REQUIRE(cf_boost_find_by_message_and_booster(db, 7001, 31, 1, &boost) ==
               CF_OK);
    CF_CHECK(boost.id == 31 && boost.message_id == 7001 &&
             boost.booster_id == 1);
    CF_CHECK(boost.content.ptr != NULL &&
             strcmp(boost.content.ptr, "mine") == 0);
    cf_boost_dispose(&boost);

    /* Every wrong leg of the AND is absent. */
    CF_CHECK(cf_boost_find_by_message_and_booster(db, 7001, 31, 2, &boost) ==
             CF_NOT_FOUND);
    CF_CHECK(boost.id == 0 && boost.content.ptr == NULL);
    CF_CHECK(cf_boost_find_by_message_and_booster(db, 7002, 31, 1, &boost) ==
             CF_NOT_FOUND);
    CF_CHECK(boost.id == 0 && boost.content.ptr == NULL);
    CF_CHECK(cf_boost_find_by_message_and_booster(db, 7001, 32, 2, &boost) ==
             CF_NOT_FOUND);
    CF_CHECK(boost.id == 0 && boost.content.ptr == NULL);
    CF_CHECK(cf_boost_find_by_message_and_booster(db, 7001, 99, 1, &boost) ==
             CF_NOT_FOUND);
    CF_CHECK(boost.id == 0 && boost.content.ptr == NULL);

    /* The message leg is real scope: id 32 exists, but not in message 7001. */
    CF_REQUIRE(cf_boost_find_by_message_and_booster(db, 7002, 32, 2, &boost) ==
               CF_OK);
    CF_CHECK(boost.message_id == 7002 && boost.booster_id == 2);
    cf_boost_dispose(&boost);

    CF_CHECK(cf_boost_find_by_message_and_booster(db, 7001, 31, 1, NULL) ==
             CF_INVALID);

    fixture_close(&fixture);
}

/* --- mutations ------------------------------------------------------------ */

CF_TEST(create_inserts_row_and_touches_message_and_room) {
    boost_fixture fixture;
    CF_REQUIRE(fixture_open(&fixture));
    cf_db *db = fixture.scratch.db;
    seed_messages(db);

    /* Known instant with non-zero microseconds: the stored text must use the
     * reference form with six fraction digits. */
    const int64_t now = INT64_C(1790000000123456);
    cf_test_clock_set_fixed_us(now);

    char mutable_content[] = "wow";
    cf_boost boost = {0};
    create_args args = { 7001, 1, { mutable_content, 3 }, &boost };
    CF_REQUIRE(cf_write(fixture.app, create_fn, &args) == CF_OK);

    CF_CHECK(boost.id > 0);
    CF_CHECK(boost.message_id == 7001);
    CF_CHECK(boost.booster_id == 1);
    CF_CHECK(boost.content.len == 3 && boost.content.ptr != NULL);
    CF_CHECK(boost.content.ptr != NULL &&
             strcmp(boost.content.ptr, "wow") == 0);
    CF_CHECK(boost.created_at == now);
    CF_CHECK(boost.updated_at == now);
    /* The record owns a copy, not the caller's buffer. */
    CF_CHECK(boost.content.ptr != mutable_content);
    mutable_content[0] = 'X';
    CF_CHECK(strcmp(boost.content.ptr, "wow") == 0);

    CF_CHECK(boost_count(db) == 1);
    CF_CHECK(query_text_is(db, "SELECT \"content\" FROM \"boosts\" WHERE \"id\" = ?",
                           boost.id, "wow"));
    CF_CHECK(query_text_is(db, "SELECT \"created_at\" FROM \"boosts\" WHERE \"id\" = ?",
                           boost.id, "2026-09-21 14:13:20.123456"));
    CF_CHECK(query_text_is(db, "SELECT \"updated_at\" FROM \"boosts\" WHERE \"id\" = ?",
                           boost.id, "2026-09-21 14:13:20.123456"));
    CF_CHECK(query_column_i64(
                  db, "SELECT \"message_id\" FROM \"boosts\" WHERE \"id\" = ?",
                  boost.id) == 7001);
    CF_CHECK(query_column_i64(
                  db, "SELECT \"booster_id\" FROM \"boosts\" WHERE \"id\" = ?",
                  boost.id) == 1);
    /* Touch: the message, and through it the room, moved to tx.now(). */
    CF_CHECK(message_updated_us(db, 7001) == now);
    CF_CHECK(room_updated_us(db, 1) == now);
    cf_boost_dispose(&boost);

    cf_test_clock_clear();
    fixture_close(&fixture);
}

CF_TEST(create_missing_message_fails_and_leaves_out_empty) {
    boost_fixture fixture;
    CF_REQUIRE(fixture_open(&fixture));
    cf_db *db = fixture.scratch.db;
    seed_messages(db);
    cf_test_clock_set_fixed_us(INT64_C(1790000000123456));

    cf_boost boost = {0};
    create_args args = { 9999, 1, CF_STR_LIT("orphan"), &boost };
    /* boosts.message_id references messages(id): CF_INVALID from the FK. */
    CF_CHECK(cf_write(fixture.app, create_fn, &args) == CF_INVALID);
    CF_CHECK(boost.id == 0 && boost.content.ptr == NULL && boost.content.len == 0);
    CF_CHECK(boost_count(db) == 0);
    /* The failed insert never reached the touch. */
    CF_CHECK(message_updated_us(db, 7001) == at_us(0));
    CF_CHECK(room_updated_us(db, 1) == at_us(0));

    /* Invalid arguments refuse before any SQL. */
    CF_CHECK(cf_write(fixture.app, create_fn,
                      &(create_args){ 7001, 1, { NULL, 3 }, &boost }) ==
             CF_INVALID);
    CF_CHECK(boost.content.ptr == NULL);
    create_args no_out = { 7001, 1, CF_STR_LIT("x"), NULL };
    CF_CHECK(cf_write(fixture.app, create_fn, &no_out) == CF_INVALID);
    CF_CHECK(boost_count(db) == 0);

    cf_test_clock_clear();
    fixture_close(&fixture);
}

CF_TEST(destroy_deletes_and_touches_message_and_room) {
    boost_fixture fixture;
    CF_REQUIRE(fixture_open(&fixture));
    cf_db *db = fixture.scratch.db;
    seed_messages(db);
    insert_boost_row(db, 7001, 7001, 1, "gone", at_us(1), at_us(1));

    cf_boost boost = {0};
    CF_REQUIRE(cf_boost_find(db, 7001, &boost) == CF_OK);

    const int64_t now = at_us(100);
    cf_test_clock_set_fixed_us(now);
    boost_args args = { &boost };
    CF_REQUIRE(cf_write(fixture.app, destroy_fn, &args) == CF_OK);
    CF_CHECK(boost_count(db) == 0);
    CF_CHECK(message_updated_us(db, 7001) == now);
    CF_CHECK(room_updated_us(db, 1) == now);

    /* delete_row ignores the affected-row count (execute_cached), so
     * destroying the same boost again still touches its message. */
    const int64_t later = at_us(200);
    cf_test_clock_set_fixed_us(later);
    CF_REQUIRE(cf_write(fixture.app, destroy_fn, &args) == CF_OK);
    CF_CHECK(boost_count(db) == 0);
    CF_CHECK(message_updated_us(db, 7001) == later);
    CF_CHECK(room_updated_us(db, 1) == later);

    CF_CHECK(cf_write(fixture.app, destroy_fn, &(boost_args){ NULL }) ==
             CF_INVALID);

    cf_boost_dispose(&boost);
    cf_test_clock_clear();
    fixture_close(&fixture);
}

CF_TEST(delete_row_deletes_without_touch) {
    boost_fixture fixture;
    CF_REQUIRE(fixture_open(&fixture));
    cf_db *db = fixture.scratch.db;
    seed_messages(db);
    insert_boost_row(db, 7001, 7001, 1, "row", at_us(1), at_us(1));
    insert_boost_row(db, 7002, 7002, 1, "other", at_us(2), at_us(2));

    cf_boost boost = {0};
    CF_REQUIRE(cf_boost_find(db, 7001, &boost) == CF_OK);
    cf_test_clock_set_fixed_us(at_us(300));
    boost_args args = { &boost };
    CF_REQUIRE(cf_write(fixture.app, delete_row_fn, &args) == CF_OK);
    CF_CHECK(boost_count(db) == 1); /* only 7002 remains */
    /* The delete alone: no message and no room touch. */
    CF_CHECK(message_updated_us(db, 7001) == at_us(0));
    CF_CHECK(room_updated_us(db, 1) == at_us(0));

    /* A missing row is not an error and still does not touch. */
    CF_REQUIRE(cf_write(fixture.app, delete_row_fn, &args) == CF_OK);
    CF_CHECK(boost_count(db) == 1);
    CF_CHECK(message_updated_us(db, 7001) == at_us(0));

    CF_CHECK(cf_write(fixture.app, delete_row_fn, &(boost_args){ NULL }) ==
             CF_INVALID);
    CF_CHECK(cf_boost_delete_row(NULL, &boost) == CF_INVALID);

    cf_boost_dispose(&boost);
    cf_test_clock_clear();
    fixture_close(&fixture);
}

/* --- failure and disposal edges ------------------------------------------- */

CF_TEST(read_failures_leave_out_empty) {
    boost_fixture fixture;
    CF_REQUIRE(fixture_open(&fixture));
    cf_db *db = fixture.scratch.db;
    seed_messages(db);
    insert_boost_row(db, 8001, 7001, 1, "good", at_us(1), at_us(1));
    /* A corrupt datetime text: the reference row decode fails the read. */
    CF_REQUIRE(exec_sql(db, "INSERT INTO \"boosts\" (\"id\", \"message_id\", "
                            "\"booster_id\", \"content\", \"created_at\", "
                            "\"updated_at\") VALUES (8002, 7001, 1, 'bad', "
                            "'not-a-datetime', '2026-09-21 14:13:20')"));

    cf_boost boost = {0};
    CF_CHECK(cf_boost_find(db, 8002, &boost) == CF_INVALID);
    CF_CHECK(boost.id == 0 && boost.content.ptr == NULL);

    cf_boost_vector boosts = {0};
    CF_CHECK(cf_boost_for_message(db, 7001, &boosts) == CF_INVALID);
    CF_CHECK(boosts.len == 0 && boosts.items == NULL && boosts.cap == 0);

    /* NULL database handles are CF_INVALID, and the out values stay empty. */
    CF_CHECK(cf_boost_find(NULL, 8001, &boost) == CF_INVALID);
    CF_CHECK(boost.id == 0 && boost.content.ptr == NULL);
    CF_CHECK(cf_boost_for_message(NULL, 7001, &boosts) == CF_INVALID);
    CF_CHECK(boosts.items == NULL && boosts.len == 0);
    CF_CHECK(cf_boost_find_by_message_and_booster(NULL, 7001, 8001, 1,
                                                  &boost) == CF_INVALID);

    /* A row that is not the malformed one still reads fine. */
    CF_REQUIRE(cf_boost_find(db, 8001, &boost) == CF_OK);
    cf_boost_dispose(&boost);

    fixture_close(&fixture);
}

CF_TEST(dispose_paths_and_argument_errors) {
    cf_boost zero = {0};
    cf_boost_dispose(&zero);
    cf_boost_dispose(&zero); /* zero state is a no-op */
    cf_boost_dispose(NULL);

    cf_boost_vector vector = {0};
    cf_boost_vector_dispose(&vector);
    CF_CHECK(vector.items == NULL && vector.len == 0 && vector.cap == 0);
    cf_boost_vector_dispose(NULL);

    /* A partially filled vector frees exactly its owned elements. */
    cf_boost_vector partial = {0};
    partial.items = malloc(2 * sizeof *partial.items);
    CF_REQUIRE(partial.items != NULL);
    memset(partial.items, 0, 2 * sizeof *partial.items);
    partial.items[0].content.ptr = malloc(6);
    CF_REQUIRE(partial.items[0].content.ptr != NULL);
    memcpy(partial.items[0].content.ptr, "hello", 6);
    partial.items[0].content.len = 5;
    partial.items[1].content.ptr = malloc(1);
    CF_REQUIRE(partial.items[1].content.ptr != NULL);
    partial.items[1].content.ptr[0] = '\0';
    partial.len = 2;
    partial.cap = 2;
    cf_boost_vector_dispose(&partial);
    CF_CHECK(partial.items == NULL && partial.len == 0 && partial.cap == 0);

    /* Mutation argument errors. */
    boost_fixture fixture;
    CF_REQUIRE(fixture_open(&fixture));
    cf_db *db = fixture.scratch.db;
    seed_messages(db);
    cf_test_clock_set_fixed_us(at_us(1));

    cf_boost out = {0};
    CF_CHECK(cf_write(fixture.app, create_fn,
                      &(create_args){ 7001, 1, CF_STR_LIT("x"), NULL }) ==
             CF_INVALID);
    CF_CHECK(cf_write(fixture.app, create_fn,
                      &(create_args){ 7001, 1, { NULL, 0 }, &out }) ==
             CF_OK); /* empty span is empty text, not an error */
    CF_CHECK(out.content.ptr != NULL && out.content.len == 0);
    cf_boost_dispose(&out);
    CF_CHECK(boost_count(db) == 1);

    CF_CHECK(cf_boost_create(NULL, 7001, 1, CF_STR_LIT("x"), &out) ==
             CF_INVALID);
    CF_CHECK(out.content.ptr == NULL);
    CF_CHECK(cf_boost_delete_row(NULL, NULL) == CF_INVALID);

    cf_test_clock_clear();
    fixture_close(&fixture);
}

CF_TEST_MAIN()
