/* tests/models/ban_test.c — D01 model family "ban".
 *
 * Oracle: tmp/rust-ref/crates/db/src/models/ban.rs (Ban::validate/mod tests,
 * Ban::banned, Ban::for_user, Ban::create) and the pinned fixture test
 * crates/db/src/tests/user_test.rs (ban_creates_bans_from_session_ips_...,
 * ban_rejects_private_session_ips), which exercises Ban::banned/for_user.
 * The C fixture importer does not exist yet, so rows are seeded with fixed
 * raw SQL (schema.sql columns); timestamps are the reference SQL text form.
 *
 * Reads run on a write-mode scratch connection (D01 db-core); mutations run
 * through the public cf_write path (D02 writer, opaque cf_tx) on a cf_app
 * whose DATABASE_PATH is that same scratch file; the writer must be started
 * with cf_writer_start before cf_write admits anything.
 *
 * Build (plain; add -fsanitize=address,undefined -fno-sanitize-recover=all
 * -fno-omit-frame-pointer -g for the sanitize run):
 *   clang -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra
 *         -Werror -pthread -Isrc -Itests
 *         -Ivendor/src/sqlite/sqlite-amalgamation-3530400
 *         tests/models/ban_test.c src/models/ban.c src/core/alloc.c
 *         src/core/buffer.c src/core/clock.c src/core/error.c
 *         src/core/random.c src/config.c src/app.c src/db/schema.c
 *         src/db/reader.c src/db/statements.c src/db/writer.c
 *         vendor/build/sqlite-clang/libsqlite3.a -lm
 *         -o build/d01-model-ban/plain/test_ban
 */
#include "cf_test.h"

#include "app.h"
#include "config.h"
#include "core/testclock.h"
#include "db/db_testutil.h"
#include "db/writer.h" /* cf_writer_start/stop: the D02 writer bootstrap */
#include "models/ban.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CF_BAN_TEST_SECRET_HEX \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

/* 2023-11-14 22:13:20.123456 UTC, the pinned reference clock test value. */
#define CF_BAN_TEST_NOW_US INT64_C(1700000000123456)

static cf_str S(const char *text) {
    return (cf_str){(char *)text, strlen(text)};
}

static int raw_exec(cf_db *db, const char *sql) {
    return sqlite3_exec(cf_db_handle(db), sql, NULL, NULL, NULL);
}

/* Rows used by every read case.  users.id 1 and 2 exist for the bans FK. */
static void seed_fixture(cf_db_scratch *scratch) {
    CF_REQUIRE(scratch->db != NULL);
    CF_REQUIRE(raw_exec(scratch->db,
                        "INSERT INTO users (id, name, created_at, updated_at) "
                        "VALUES (1, 'u1', '2023-11-14 22:13:20.123456', "
                        "'2023-11-14 22:13:20.123456')") == SQLITE_OK);
    CF_REQUIRE(raw_exec(scratch->db,
                        "INSERT INTO users (id, name, created_at, updated_at) "
                        "VALUES (2, 'u2', '2023-11-14 22:13:20.123456', "
                        "'2023-11-14 22:13:20.123456')") == SQLITE_OK);
    CF_REQUIRE(raw_exec(scratch->db,
                        "INSERT INTO bans (created_at, ip_address, updated_at, "
                        "user_id) VALUES ('2023-11-14 22:13:20.123456', "
                        "'8.8.8.8', '2024-01-02 03:04:05', 1)") == SQLITE_OK);
    CF_REQUIRE(raw_exec(scratch->db,
                        "INSERT INTO bans (created_at, ip_address, updated_at, "
                        "user_id) VALUES ('2023-12-31 23:59:59.000001', "
                        "'1.1.1.1', '2023-12-31 23:59:59.000001', 1)") == SQLITE_OK);
    CF_REQUIRE(raw_exec(scratch->db,
                        "INSERT INTO bans (created_at, ip_address, updated_at, "
                        "user_id) VALUES ('2024-01-02 03:04:05', '', "
                        "'2024-01-02 03:04:05', 2)") == SQLITE_OK);
}

/* ---- cf_ban_validate ---------------------------------------------------- */

static void expect_error(const char *ip, const char *message) {
    cf_model_errors errors;
    cf_err rc = cf_ban_validate(S(ip), &errors);
    CF_CHECK(rc == CF_OK);
    CF_CHECK(errors.len == 1);
    if (errors.len == 1) {
        CF_CHECK(errors.items[0].field.len == strlen("ip_address"));
        CF_CHECK(errors.items[0].field.ptr != NULL &&
                 memcmp(errors.items[0].field.ptr, "ip_address",
                        strlen("ip_address")) == 0);
        CF_CHECK(errors.items[0].message.len == strlen(message));
        CF_CHECK(errors.items[0].message.ptr != NULL &&
                 memcmp(errors.items[0].message.ptr, message,
                        strlen(message)) == 0);
    }
    cf_model_errors_dispose(&errors);
}

static void expect_valid(const char *ip) {
    cf_model_errors errors;
    CF_CHECK(cf_ban_validate(S(ip), &errors) == CF_OK);
    CF_CHECK(errors.len == 0);
    cf_model_errors_dispose(&errors);
}

CF_TEST(validate_accepts_public_addresses) {
    expect_valid("8.8.8.8");
    expect_valid("2001:4860:4860::8888");
    expect_valid("[::ffff:8.8.4.4]");       /* bracketed, public */
    expect_valid("8.8.8.8/+24");            /* Rust u32 prefix parse takes '+' */
    expect_valid("11.0.0.1/8");             /* 11/8 is outside the checked ranges */
    expect_valid("8.8.8.8/0");              /* masked to 0.0.0.0, not checked */
    expect_valid("2001:db8::1/32");
}

CF_TEST(validate_rejects_internal_addresses) {
    static const char *const internal[] = {
        "127.0.0.1",       "10.1.2.3",         "172.16.0.1",
        "192.168.1.1",     "169.254.169.254",  "::1",
        "fc00::1",         "fd12::1",          "fe80::1",
        "::ffff:10.0.0.1", "::ffff:127.0.0.1",
    };
    for (size_t i = 0; i < sizeof internal / sizeof internal[0]; i++) {
        expect_error(internal[i], "cannot be a private or internal IP address");
    }
    /* Prefix masking and the mapped-0xffff quirk (bits 32..47 only). */
    expect_error("10.0.0.1/8", "cannot be a private or internal IP address");
    expect_error("fc00::1/8", "cannot be a private or internal IP address");
    expect_error("::ffff:169.254.1.1",
                 "cannot be a private or internal IP address");
    expect_error("1:2:3:4:5:ffff:10.0.0.1",
                 "cannot be a private or internal IP address");
}

CF_TEST(validate_rejects_unparsable_addresses) {
    static const char *const garbage[] = {
        "not an ip", "8.8.8.8/33", "1.2.3.256", "01.2.3.4", "[::1",
        "1:2:3:4:5:6:7:8:9", "1:2:3:4:5:6:7:8:", "1::2::3", "::g", "[]",
        "fe80::1%eth0",
    };
    for (size_t i = 0; i < sizeof garbage / sizeof garbage[0]; i++) {
        expect_error(garbage[i], "is not a valid IP address");
    }
    expect_error("", "is not a valid IP address");
    CF_CHECK(cf_ban_validate(S("8.8.8.8"), NULL) == CF_INVALID);
    CF_CHECK(cf_ban_validate((cf_str){NULL, 5}, NULL) == CF_INVALID);
    /* A span without bytes is unparsable, not a crash. */
    cf_model_errors errors;
    CF_CHECK(cf_ban_validate((cf_str){NULL, 5}, &errors) == CF_OK);
    CF_CHECK(errors.len == 1);
    cf_model_errors_dispose(&errors);
}

/* ---- cf_ban_banned ------------------------------------------------------ */

CF_TEST(banned_reports_stored_addresses) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_fixture(&scratch);

    bool found = true;
    CF_CHECK(cf_ban_banned(scratch.db, S("8.8.8.8"), &found) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(cf_ban_banned(scratch.db, S("1.1.1.1"), &found) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(cf_ban_banned(scratch.db, S(""), &found) == CF_OK);
    CF_CHECK(found); /* exact-match on the stored empty string */
    CF_CHECK(cf_ban_banned(scratch.db, S("8.8.4.4"), &found) == CF_OK);
    CF_CHECK(!found);
    CF_CHECK(cf_ban_banned(scratch.db, S("::1"), &found) == CF_OK);
    CF_CHECK(!found);
    /* Invalid arguments report CF_INVALID and leave *out empty. */
    found = false; /* the caller keeps out empty on failure */
    CF_CHECK(cf_ban_banned(NULL, S("8.8.8.8"), &found) == CF_INVALID);
    CF_CHECK(!found);
    CF_CHECK(cf_ban_banned(scratch.db, S("8.8.8.8"), NULL) == CF_INVALID);
    CF_CHECK(cf_ban_banned(scratch.db, (cf_str){NULL, 5}, &found) == CF_INVALID);
    CF_CHECK(!found);

    cf_db_scratch_close(&scratch);
}

/* ---- cf_ban_for_user ---------------------------------------------------- */

CF_TEST(for_user_returns_copied_rows_and_empty_vector) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_fixture(&scratch);

    cf_ban_vector rows;
    CF_REQUIRE(cf_ban_for_user(scratch.db, 1, &rows) == CF_OK);
    CF_REQUIRE(rows.len == 2);
    CF_CHECK(rows.items[0].id == 1);
    CF_CHECK(rows.items[0].user_id == 1);
    CF_CHECK(rows.items[0].ip_address.ptr != NULL &&
             rows.items[0].ip_address.len == 7 &&
             memcmp(rows.items[0].ip_address.ptr, "8.8.8.8", 7) == 0);
    CF_CHECK(rows.items[0].created_at == INT64_C(1700000000123456));
    CF_CHECK(rows.items[0].updated_at == INT64_C(1704164645000000));
    CF_CHECK(rows.items[1].id == 2);
    CF_CHECK(rows.items[1].created_at == INT64_C(1704067199000001));
    CF_CHECK(rows.items[1].ip_address.ptr != NULL &&
             memcmp(rows.items[1].ip_address.ptr, "1.1.1.1", 7) == 0);
    /* Values above are read after the statement was reset (copy-before-reset). */
    cf_ban_vector_dispose(&rows);
    CF_CHECK(rows.items == NULL && rows.len == 0 && rows.cap == 0);

    /* Stored empty string is an empty (not absent) cf_str. */
    CF_REQUIRE(cf_ban_for_user(scratch.db, 2, &rows) == CF_OK);
    CF_REQUIRE(rows.len == 1);
    CF_CHECK(rows.items[0].ip_address.ptr == NULL);
    CF_CHECK(rows.items[0].ip_address.len == 0);
    cf_ban_vector_dispose(&rows);

    /* No rows: found-style empty vector, not an error. */
    CF_REQUIRE(cf_ban_for_user(scratch.db, 3, &rows) == CF_OK);
    CF_CHECK(rows.items == NULL && rows.len == 0);
    cf_ban_vector_dispose(&rows);

    CF_CHECK(cf_ban_for_user(NULL, 1, &rows) == CF_INVALID);
    CF_CHECK(cf_ban_for_user(scratch.db, 1, NULL) == CF_INVALID);

    cf_db_scratch_close(&scratch);
}

/* ---- cf_ban_create (D02 cf_write) --------------------------------------- */

#define CF_BAN_TEST_BANS_COUNT_SQL "SELECT count(*) FROM bans"

static cf_app *make_app(const char *database_path) {
    cf_config_entry entries[4] = {
        {"PUBLIC_ORIGIN", "http://127.0.0.1:32123"},
        {"SECRET_KEY_BASE", CF_BAN_TEST_SECRET_HEX},
        {"PORT", "32123"},
        {"DATABASE_PATH", database_path},
    };
    cf_config *config = NULL;
    CF_REQUIRE(cf_config_parse(entries, 4, NULL, &config) == CF_OK);
    cf_app *app = NULL;
    CF_REQUIRE(cf_app_create(config, &app) == CF_OK);
    CF_REQUIRE(app != NULL);
    /* D02: cf_write returns CF_INTERNAL until the writer thread is started. */
    CF_REQUIRE(cf_writer_start(app, config) == CF_OK);
    return app;
}

static void dispose_app(cf_app *app) {
    cf_writer_stop(app); /* no cf_write caller is active here */
    cf_app_destroy(app);
}

struct ban_create_call {
    int64_t user_id;
    const char *ip;
    cf_err rc;
    cf_ban record;
};

static cf_err ban_create_call(cf_tx *tx, void *arg) {
    struct ban_create_call *call = arg;
    call->rc = cf_ban_create(tx, call->user_id, S(call->ip), &call->record);
    return call->rc;
}

static void init_call(struct ban_create_call *call, int64_t user_id,
                      const char *ip) {
    memset(call, 0, sizeof *call);
    call->user_id = user_id;
    call->ip = ip;
}

CF_TEST(create_inserts_record_with_fixed_clock) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_fixture(&scratch);
    cf_app *app = make_app(scratch.path);

    cf_test_clock_set_fixed_us(CF_BAN_TEST_NOW_US);
    struct ban_create_call call;
    init_call(&call, 2, "9.9.9.9");
    cf_err write_rc = cf_write(app, ban_create_call, &call);
    cf_test_clock_clear();

    CF_CHECK(write_rc == CF_OK); /* committed */
    CF_REQUIRE(call.rc == CF_OK);
    CF_CHECK(call.record.id > 3);
    CF_CHECK(call.record.user_id == 2);
    CF_CHECK(call.record.ip_address.ptr != NULL);
    CF_CHECK(call.record.ip_address.len == 7);
    CF_CHECK(call.record.ip_address.ptr != NULL &&
             memcmp(call.record.ip_address.ptr, "9.9.9.9", 7) == 0);
    CF_CHECK(call.record.created_at == CF_BAN_TEST_NOW_US);
    CF_CHECK(call.record.updated_at == CF_BAN_TEST_NOW_US);
    cf_ban_dispose(&call.record);

    sqlite3 *handle = cf_db_handle(scratch.db);
    bool ok = false;
    CF_CHECK(cf_db_test_i64(handle, CF_BAN_TEST_BANS_COUNT_SQL, &ok) == 4);
    CF_CHECK(ok);
    char buf[128];
    CF_CHECK(cf_db_test_text(handle,
                             "SELECT ip_address FROM bans WHERE id = 4", buf,
                             sizeof buf) != NULL);
    CF_CHECK(strcmp(buf, "9.9.9.9") == 0);
    CF_CHECK(cf_db_test_text(
                 handle, "SELECT created_at FROM bans WHERE id = 4", buf,
                 sizeof buf) != NULL);
    CF_CHECK(strcmp(buf, "2023-11-14 22:13:20.123456") == 0); /* SQL text form */

    /* Committed row is visible to the reader connection. */
    bool found = false;
    CF_CHECK(cf_ban_banned(scratch.db, S("9.9.9.9"), &found) == CF_OK);
    CF_CHECK(found);

    dispose_app(app);
    cf_db_scratch_close(&scratch);
}

CF_TEST(create_rejects_invalid_addresses_without_inserting) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_fixture(&scratch);
    cf_app *app = make_app(scratch.path);

    static const char *const rejected[] = {"192.168.1.1", "::1", "nope", ""};
    for (size_t i = 0; i < sizeof rejected / sizeof rejected[0]; i++) {
        struct ban_create_call call;
        init_call(&call, 2, rejected[i]);
        cf_err write_rc = cf_write(app, ban_create_call, &call);
        CF_CHECK(write_rc == CF_INVALID);
        CF_CHECK(call.rc == CF_INVALID);
        CF_CHECK(call.record.id == 0);
        CF_CHECK(call.record.ip_address.ptr == NULL);
        CF_CHECK(call.record.ip_address.len == 0);
        cf_ban_dispose(&call.record);
    }
    bool ok = false;
    CF_CHECK(cf_db_test_i64(cf_db_handle(scratch.db), CF_BAN_TEST_BANS_COUNT_SQL,
                            &ok) == 3);
    CF_CHECK(ok);

    /* Direct invalid-argument guard: no transaction involved. */
    cf_ban record = {0};
    CF_CHECK(cf_ban_create(NULL, 2, S("8.8.8.8"), &record) == CF_INVALID);
    CF_CHECK(record.id == 0 && record.ip_address.ptr == NULL);

    dispose_app(app);
    cf_db_scratch_close(&scratch);
}

CF_TEST_MAIN()
