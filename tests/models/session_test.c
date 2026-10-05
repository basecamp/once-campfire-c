/* tests/models/session_test.c — D01 model family "session" tests.
 *
 * Reference oracles (pinned; hashes in reference-files.json):
 *  - tmp/rust-ref/crates/db/src/models/session.rs (the implemented source,
 *    8 functions);
 *  - tests/fixtures/crates/db/src/tests/callbacks_test.rs
 *    (session_start_and_resume: 24-char token, refresh only after an hour,
 *    a resume inside the hour writes nothing;
 *    fixture_session_resumes_because_it_is_two_hours_old);
 *  - tests/fixtures/crates/db/src/tests/columns_test.rs
 *    (Session::find_by_token reads each column: id 5001, token "token 5003",
 *    ip "10.0.50.4", agent "agent 5005");
 *  - tests/fixtures/crates/db/src/tests/fixtures_test.rs (the david_safari
 *    session is whole-second and two hours old; the C checkout does not carry
 *    the reference fixture database, so the equivalent rows are constructed
 *    explicitly, as tests/models/first_run_test.c does).
 *
 * Mutations go through the public cf_write path (00-contracts.md: cf_tx is
 * opaque; only D02's writer creates one) on a scratch database created by
 * cf_db_open (fresh schema, D-C01). A fixed test clock makes start/resume
 * timestamps deterministic and a fixed RNG byte makes the token
 * deterministic; there are no sleeps.
 *
 * Build (plain; add -fsanitize=address,undefined -fno-sanitize-recover=all
 * -fno-omit-frame-pointer -g for the sanitize run). The binary needs D02's
 * writer and defines weak fallbacks for the shared model runtime
 * (src/models/types.c is not in the checkout yet); see
 * docs/devel/evidence/D01-model-session.md:
 *   clang -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra
 *         -Werror -pthread -Isrc -Itests
 *         -Ivendor/src/sqlite/sqlite-amalgamation-3530400
 *         tests/models/session_test.c src/models/session.c
 *         src/core/alloc.c src/core/buffer.c src/core/clock.c
 *         src/core/error.c src/core/random.c src/config.c src/app.c
 *         src/db/schema.c src/db/reader.c src/db/statements.c
 *         src/db/writer.c
 *         vendor/build/sqlite-clang/libsqlite3.a -lm
 *         -o build/d01-model-session/plain/test_session
 */
#include "models/session.h"

#include "app.h"
#include "cf.h"
#include "config.h"
#include "core/testclock.h"
#include "core/testrandom.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "db/writer.h" /* cf_writer_start/stop: the D02 writer bootstrap */
#include "cf_test.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The shared release helpers (cf_str_dispose / cf_optional_str_dispose) are
 * defined once in src/models/types.c. */

#define SESSION_HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define SESSION_ORIGIN "http://127.0.0.1:32123"

/* Fixed instants (epoch microseconds and their reference SQL text):
 * "2026-01-02 03:04:05.678901", "2026-03-04 05:06:07.000001" and the
 * whole-second "2026-01-02 03:04:05". */
#define S_WHOLE_US INT64_C(1767323045000000)
#define S_WHOLE_TEXT "2026-01-02 03:04:05"
#define S_NOW1_US INT64_C(1767323045678901)
#define S_NOW1_TEXT "2026-01-02 03:04:05.678901"
#define S_NOW2_US INT64_C(1772600767000001)
#define S_NOW2_TEXT "2026-03-04 05:06:07.000001"

/* Fixture-shaped values from columns_test.rs. */
#define S_FIX_ID INT64_C(5001)
#define S_FIX_USER INT64_C(4001)
#define S_FIX_TOKEN "token 5003"
#define S_FIX_IP "10.0.50.4"
#define S_FIX_AGENT "agent 5005"

/* --- small helpers -------------------------------------------------------- */

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

static bool session_empty(const cf_session *session) {
    return session->id == 0 && session->user_id == 0 &&
           session->token.ptr == NULL && session->token.len == 0 &&
           opt_absent(&session->ip_address) &&
           opt_absent(&session->user_agent) &&
           session->last_active_at == 0 && session->created_at == 0 &&
           session->updated_at == 0;
}

static cf_optional_str opt_lit(const char *text) {
    cf_optional_str value = {.present = true,
                             .value = {(char *)text, strlen(text)}};
    return value;
}

static void exec_or_abort(sqlite3 *handle, const char *sql) {
    CF_REQUIRE(cf_db_test_exec(handle, sql) == SQLITE_OK);
}

static int64_t count_sessions(sqlite3 *handle) {
    bool ok = false;
    int64_t count =
        cf_db_test_i64(handle, "SELECT count(*) FROM sessions", &ok);
    CF_CHECK(ok);
    return count;
}

static bool sql_text_is(sqlite3 *handle, const char *sql, const char *want) {
    char buffer[512];
    return cf_db_test_text(handle, sql, buffer, sizeof buffer) != NULL &&
           strcmp(buffer, want) == 0;
}

static bool sql_is_null(sqlite3 *handle, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(handle, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }
    bool is_null = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        is_null = sqlite3_column_type(stmt, 0) == SQLITE_NULL;
    }
    sqlite3_finalize(stmt);
    return is_null;
}

/* Insert a fixture-shaped user; sessions.user_id has a foreign key. */
static void insert_user_row(cf_db *db, int64_t id, const char *name) {
    char sql[256];
    snprintf(sql, sizeof sql,
             "INSERT INTO users (id, name, created_at, updated_at) "
             "VALUES (%lld, '%s', '" S_WHOLE_TEXT "', '" S_WHOLE_TEXT "')",
             (long long)id, name);
    exec_or_abort(cf_db_handle(db), sql);
}

/* Insert a session row with the reference storage text; NULL strings insert
 * SQL NULL (the optional columns). */
static void insert_session_row(cf_db *db, int64_t id, int64_t user_id,
                               const char *token, const char *ip_or_null,
                               const char *agent_or_null,
                               const char *last_text, const char *created_text,
                               const char *updated_text) {
    char ip_clause[64];
    char agent_clause[64];
    if (ip_or_null != NULL) {
        snprintf(ip_clause, sizeof ip_clause, "'%s'", ip_or_null);
    } else {
        snprintf(ip_clause, sizeof ip_clause, "NULL");
    }
    if (agent_or_null != NULL) {
        snprintf(agent_clause, sizeof agent_clause, "'%s'", agent_or_null);
    } else {
        snprintf(agent_clause, sizeof agent_clause, "NULL");
    }
    char sql[768];
    snprintf(sql, sizeof sql,
             "INSERT INTO sessions (id, user_id, token, ip_address, "
             "user_agent, last_active_at, created_at, updated_at) "
             "VALUES (%lld, %lld, '%s', %s, %s, '%s', '%s', '%s')",
             (long long)id, (long long)user_id, token, ip_clause, agent_clause,
             last_text, created_text, updated_text);
    exec_or_abort(cf_db_handle(db), sql);
}

/* --- scratch database + app (D02 writer) ---------------------------------- */

typedef struct {
    char *dir;
    char path[CF_DB_TEST_PATH_CAP];
    cf_app *app;
} session_world;

static void session_world_open(session_world *world) {
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
        {"PUBLIC_ORIGIN", SESSION_ORIGIN},
        {"SECRET_KEY_BASE", SESSION_HEX64},
        {"DATABASE_PATH", world->path},
    };
    cf_config *config = NULL;
    CF_REQUIRE(cf_config_parse(entries, 3, NULL, &config) == CF_OK);
    CF_REQUIRE(cf_app_create(config, &world->app) == CF_OK);
    /* D02: cf_write returns CF_INTERNAL until the writer thread is started. */
    CF_REQUIRE(cf_writer_start(world->app, config) == CF_OK);
}

static void session_world_dispose(session_world *world) {
    cf_writer_stop(world->app); /* no cf_write caller is active here */
    cf_app_destroy(world->app);
    world->app = NULL;
    if (world->dir != NULL) {
        cf_db_test_cleanup(world->dir);
        free(world->dir);
        world->dir = NULL;
    }
}

static cf_db *session_world_db(const session_world *world, bool read_only) {
    cf_db *db = NULL;
    CF_REQUIRE(cf_db_open(world->path, read_only, &db) == CF_OK);
    return db;
}

/* --- mutation callbacks ---------------------------------------------------- */

struct start_op {
    int64_t user_id;
    cf_optional_str user_agent;
    cf_optional_str ip_address;
    cf_err result;
    cf_session session;
};

static cf_err start_cb(cf_tx *tx, void *arg) {
    struct start_op *op = arg;
    op->result =
        cf_session_start(tx, op->user_id, op->user_agent, op->ip_address,
                         &op->session);
    return op->result;
}

static cf_err run_start(cf_app *app, struct start_op *op) {
    return cf_write(app, start_cb, op);
}

struct resume_op {
    cf_session *session;
    cf_optional_str user_agent;
    cf_optional_str ip_address;
    cf_err result;
};

static cf_err resume_cb(cf_tx *tx, void *arg) {
    struct resume_op *op = arg;
    op->result = cf_session_resume(tx, op->session, op->user_agent,
                                   op->ip_address);
    return op->result;
}

static cf_err run_resume(cf_app *app, struct resume_op *op) {
    return cf_write(app, resume_cb, op);
}

struct destroy_op {
    const cf_session *session;
    cf_err result;
};

static cf_err destroy_cb(cf_tx *tx, void *arg) {
    struct destroy_op *op = arg;
    op->result = cf_session_destroy(tx, op->session);
    return op->result;
}

static cf_err run_destroy(cf_app *app, struct destroy_op *op) {
    return cf_write(app, destroy_cb, op);
}

/* --- find ----------------------------------------------------------------- */

CF_TEST(session_find_reads_every_column_and_reports_absent_row) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    insert_user_row(scratch.db, S_FIX_USER, "User 4002");
    insert_session_row(scratch.db, S_FIX_ID, S_FIX_USER, S_FIX_TOKEN, S_FIX_IP,
                       S_FIX_AGENT, S_NOW1_TEXT, S_WHOLE_TEXT, S_NOW2_TEXT);
    /* Same user, optional columns NULL. */
    insert_session_row(scratch.db, S_FIX_ID + 1, S_FIX_USER, "token 5004",
                       NULL, NULL, S_WHOLE_TEXT, S_WHOLE_TEXT, S_WHOLE_TEXT);

    cf_session out;
    memset(&out, 0xA5, sizeof out); /* the callee must reset it */
    CF_REQUIRE(cf_session_find(scratch.db, S_FIX_ID, &out) == CF_OK);
    CF_CHECK(out.id == S_FIX_ID);
    CF_CHECK(out.user_id == S_FIX_USER);
    CF_CHECK(str_is(out.token, S_FIX_TOKEN));
    CF_CHECK(opt_is(&out.ip_address, S_FIX_IP));
    CF_CHECK(opt_is(&out.user_agent, S_FIX_AGENT));
    CF_CHECK(out.last_active_at == S_NOW1_US);
    CF_CHECK(out.created_at == S_WHOLE_US);
    CF_CHECK(out.updated_at == S_NOW2_US);
    cf_session_dispose(&out);
    CF_CHECK(session_empty(&out)); /* dispose resets */

    /* NULL optionals stay absent, not empty. */
    memset(&out, 0xA5, sizeof out);
    CF_REQUIRE(cf_session_find(scratch.db, S_FIX_ID + 1, &out) == CF_OK);
    CF_CHECK(opt_absent(&out.ip_address));
    CF_CHECK(opt_absent(&out.user_agent));
    cf_session_dispose(&out);

    /* A missing row is CF_NOT_FOUND with an empty record. */
    memset(&out, 0xA5, sizeof out);
    CF_CHECK(cf_session_find(scratch.db, 999999, &out) == CF_NOT_FOUND);
    CF_CHECK(session_empty(&out));

    /* Argument errors. */
    CF_CHECK(cf_session_find(scratch.db, S_FIX_ID, NULL) == CF_INVALID);
    CF_CHECK(cf_session_find(NULL, S_FIX_ID, &out) == CF_INVALID);

    cf_db_scratch_close(&scratch);
}

/* --- find_by_token --------------------------------------------------------- */

CF_TEST(session_find_by_token_found_absent_and_errors) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    insert_user_row(scratch.db, S_FIX_USER, "User 4002");
    insert_session_row(scratch.db, S_FIX_ID, S_FIX_USER, S_FIX_TOKEN, S_FIX_IP,
                       S_FIX_AGENT, S_NOW1_TEXT, S_WHOLE_TEXT, S_NOW2_TEXT);

    bool found = false;
    cf_session out;
    memset(&out, 0xA5, sizeof out);
    CF_REQUIRE(cf_session_find_by_token(scratch.db, CF_STR_LIT(S_FIX_TOKEN),
                                        &found, &out) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(out.id == S_FIX_ID && str_is(out.token, S_FIX_TOKEN));
    cf_session_dispose(&out);

    /* No match is Option::None, not an error, and the record stays empty. */
    found = true;
    memset(&out, 0xA5, sizeof out);
    CF_CHECK(cf_session_find_by_token(scratch.db, CF_STR_LIT("no such token"),
                                      &found, &out) == CF_OK);
    CF_CHECK(!found);
    CF_CHECK(session_empty(&out));

    /* The empty token matches nothing (the column is NOT NULL). */
    found = true;
    CF_CHECK(cf_session_find_by_token(scratch.db, (cf_str){NULL, 0}, &found,
                                      &out) == CF_OK);
    CF_CHECK(!found);
    CF_CHECK(session_empty(&out));

    CF_CHECK(cf_session_find_by_token(scratch.db, CF_STR_LIT(S_FIX_TOKEN),
                                      NULL, &out) == CF_INVALID);
    CF_CHECK(cf_session_find_by_token(scratch.db, CF_STR_LIT(S_FIX_TOKEN),
                                      &found, NULL) == CF_INVALID);
    CF_CHECK(cf_session_find_by_token(NULL, CF_STR_LIT(S_FIX_TOKEN), &found,
                                      &out) == CF_INVALID);

    cf_db_scratch_close(&scratch);
}

/* --- for_user / count_for_user --------------------------------------------- */

CF_TEST(session_for_user_and_count_scope_by_user) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    insert_user_row(scratch.db, S_FIX_USER, "User 4002");
    insert_user_row(scratch.db, S_FIX_USER + 1, "User 4003");
    /* The fixture session plus a second one, and one for another user. */
    insert_session_row(scratch.db, S_FIX_ID, S_FIX_USER, S_FIX_TOKEN, S_FIX_IP,
                       S_FIX_AGENT, S_NOW1_TEXT, S_WHOLE_TEXT, S_NOW2_TEXT);
    insert_session_row(scratch.db, S_FIX_ID + 1, S_FIX_USER, "token 5004",
                       NULL, NULL, S_WHOLE_TEXT, S_WHOLE_TEXT, S_WHOLE_TEXT);
    insert_session_row(scratch.db, S_FIX_ID + 2, S_FIX_USER + 1, "token 5005",
                       NULL, NULL, S_WHOLE_TEXT, S_WHOLE_TEXT, S_WHOLE_TEXT);

    cf_session_vector sessions;
    CF_REQUIRE(cf_session_for_user(scratch.db, S_FIX_USER, &sessions) == CF_OK);
    CF_REQUIRE(sessions.len == 2);
    bool saw_5001 = false;
    bool saw_5002 = false;
    for (size_t i = 0; i < sessions.len; i++) {
        CF_CHECK(sessions.items[i].user_id == S_FIX_USER);
        if (sessions.items[i].id == S_FIX_ID &&
            str_is(sessions.items[i].token, S_FIX_TOKEN)) {
            saw_5001 = true;
        }
        if (sessions.items[i].id == S_FIX_ID + 1 &&
            str_is(sessions.items[i].token, "token 5004")) {
            saw_5002 = true;
        }
    }
    CF_CHECK(saw_5001 && saw_5002);
    cf_session_vector_dispose(&sessions);
    CF_CHECK(sessions.items == NULL && sessions.len == 0);

    /* A user with no sessions yields an empty vector, not an error. */
    CF_REQUIRE(cf_session_for_user(scratch.db, 999999, &sessions) == CF_OK);
    CF_CHECK(sessions.len == 0 && sessions.items == NULL);

    int64_t count = -1;
    CF_REQUIRE(cf_session_count_for_user(scratch.db, S_FIX_USER, &count) ==
               CF_OK);
    CF_CHECK(count == 2);
    CF_REQUIRE(cf_session_count_for_user(scratch.db, S_FIX_USER + 1, &count) ==
               CF_OK);
    CF_CHECK(count == 1);
    CF_REQUIRE(cf_session_count_for_user(scratch.db, 999999, &count) == CF_OK);
    CF_CHECK(count == 0);

    CF_CHECK(cf_session_for_user(scratch.db, S_FIX_USER, NULL) == CF_INVALID);
    CF_CHECK(cf_session_count_for_user(scratch.db, S_FIX_USER, NULL) ==
             CF_INVALID);
    CF_CHECK(cf_session_for_user(NULL, S_FIX_USER, &sessions) == CF_INVALID);
    cf_session_vector_dispose(NULL); /* destructors accept NULL */
    cf_session_dispose(NULL);

    cf_db_scratch_close(&scratch);
}

/* --- needs_resume ---------------------------------------------------------- */

CF_TEST(session_needs_resume_is_strictly_older_than_one_hour) {
    cf_session session;
    memset(&session, 0, sizeof session);
    session.last_active_at = S_NOW1_US;

    CF_CHECK(!cf_session_needs_resume(&session, S_NOW1_US));
    CF_CHECK(!cf_session_needs_resume(
        &session, S_NOW1_US + CF_SESSION_ACTIVITY_REFRESH_RATE_US));
    CF_CHECK(!cf_session_needs_resume(
        &session, S_NOW1_US + CF_SESSION_ACTIVITY_REFRESH_RATE_US - 1));
    CF_CHECK(cf_session_needs_resume(
        &session, S_NOW1_US + CF_SESSION_ACTIVITY_REFRESH_RATE_US + 1));
    /* The david_safari fixture shape: two hours old. */
    CF_CHECK(cf_session_needs_resume(&session, S_NOW1_US + 2 * INT64_C(3600000000)));
    CF_CHECK(!cf_session_needs_resume(NULL, S_NOW1_US));
}

/* --- start ----------------------------------------------------------------- */

CF_TEST(session_start_generates_base58_token_and_reference_datetimes) {
    session_world world;
    session_world_open(&world);
    cf_db *db = session_world_db(&world, false);
    insert_user_row(db, S_FIX_USER, "User 4002");
    cf_db_close(db);

    cf_test_clock_set_fixed_us(S_NOW1_US);
    cf_test_random_fill(0x10); /* alphabet[16] == 'H' */

    struct start_op op;
    memset(&op, 0, sizeof op);
    op.user_id = S_FIX_USER;
    op.user_agent = opt_lit("ua");
    op.ip_address = opt_lit("1.2.3.4");
    CF_REQUIRE(run_start(world.app, &op) == CF_OK);
    CF_REQUIRE(op.result == CF_OK);

    /* callbacks_test.rs: a 24-character token over the base58 alphabet. */
    CF_CHECK(op.session.token.len == 24);
    CF_CHECK(str_is(op.session.token, "HHHHHHHHHHHHHHHHHHHHHHHH"));
    for (size_t i = 0; i < op.session.token.len; i++) {
        char c = op.session.token.ptr[i];
        bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9');
        CF_CHECK(alnum && c != '0' && c != 'O' && c != 'I' && c != 'l');
    }
    CF_CHECK(op.session.user_id == S_FIX_USER);
    CF_CHECK(op.session.last_active_at == S_NOW1_US);
    CF_CHECK(op.session.created_at == S_NOW1_US);
    CF_CHECK(op.session.updated_at == S_NOW1_US);
    CF_CHECK(opt_is(&op.session.user_agent, "ua"));
    CF_CHECK(opt_is(&op.session.ip_address, "1.2.3.4"));

    /* The stored row uses the reference UTC text, fraction only when set. */
    db = session_world_db(&world, true);
    sqlite3 *handle = cf_db_handle(db);
    CF_CHECK(count_sessions(handle) == 1);
    CF_CHECK(sql_text_is(handle, "SELECT token FROM sessions WHERE id = 1",
                         "HHHHHHHHHHHHHHHHHHHHHHHH"));
    CF_CHECK(sql_text_is(handle,
                         "SELECT last_active_at FROM sessions WHERE id = 1",
                         S_NOW1_TEXT));
    CF_CHECK(sql_text_is(handle,
                         "SELECT created_at FROM sessions WHERE id = 1",
                         S_NOW1_TEXT));
    CF_CHECK(sql_text_is(handle, "SELECT updated_at FROM sessions WHERE id = 1",
                         S_NOW1_TEXT));
    CF_CHECK(sql_text_is(handle,
                         "SELECT user_agent FROM sessions WHERE id = 1", "ua"));
    CF_CHECK(sql_text_is(handle,
                         "SELECT ip_address FROM sessions WHERE id = 1",
                         "1.2.3.4"));

    /* find_by_token returns the same record (columns_test.rs). */
    bool found = false;
    cf_session reloaded;
    CF_REQUIRE(cf_session_find_by_token(db, op.session.token, &found,
                                        &reloaded) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(reloaded.id == op.session.id &&
             reloaded.last_active_at == op.session.last_active_at &&
             opt_is(&reloaded.user_agent, "ua"));
    cf_session_dispose(&reloaded);

    /* start with absent optionals binds SQL NULL.  A fresh fill byte keeps
     * the second token distinct: the token column is UNIQUE. */
    cf_test_random_fill(0x11);
    struct start_op bare;
    memset(&bare, 0, sizeof bare);
    bare.user_id = S_FIX_USER;
    CF_REQUIRE(run_start(world.app, &bare) == CF_OK);
    CF_CHECK(opt_absent(&bare.session.ip_address));
    CF_CHECK(opt_absent(&bare.session.user_agent));
    CF_CHECK(count_sessions(handle) == 2);
    CF_CHECK(sql_is_null(handle, "SELECT ip_address FROM sessions WHERE id = 2"));
    CF_CHECK(sql_is_null(handle,
                         "SELECT user_agent FROM sessions WHERE id = 2"));

    cf_db_close(db);
    cf_session_dispose(&op.session);
    cf_session_dispose(&bare.session);
    cf_test_random_clear();
    cf_test_clock_clear();
    session_world_dispose(&world);
}

CF_TEST(session_start_fails_without_entropy_and_inserts_nothing) {
    session_world world;
    session_world_open(&world);
    cf_db *db = session_world_db(&world, false);
    insert_user_row(db, S_FIX_USER, "User 4002");
    cf_db_close(db);

    cf_test_clock_set_fixed_us(S_NOW1_US);
    cf_test_random_fail();

    struct start_op op;
    memset(&op, 0, sizeof op);
    op.user_id = S_FIX_USER;
    CF_CHECK(run_start(world.app, &op) == CF_IO);
    CF_CHECK(op.result == CF_IO);
    CF_CHECK(session_empty(&op.session));

    db = session_world_db(&world, true);
    CF_CHECK(count_sessions(cf_db_handle(db)) == 0); /* rolled back */
    cf_db_close(db);

    cf_test_random_clear();
    cf_test_clock_clear();
    session_world_dispose(&world);
}

CF_TEST(session_start_rejects_a_stuck_entropy_source) {
    session_world world;
    session_world_open(&world);
    cf_db *db = session_world_db(&world, false);
    insert_user_row(db, S_FIX_USER, "User 4002");
    cf_db_close(db);

    cf_test_clock_set_fixed_us(S_NOW1_US);
    /* Every byte is >= 232, so no uniform base58 draw can be accepted; the
     * generator must fail instead of spinning. */
    cf_test_random_fill(0xFF);

    struct start_op op;
    memset(&op, 0, sizeof op);
    op.user_id = S_FIX_USER;
    CF_CHECK(run_start(world.app, &op) == CF_INTERNAL);
    CF_CHECK(op.result == CF_INTERNAL);
    CF_CHECK(session_empty(&op.session));

    db = session_world_db(&world, true);
    CF_CHECK(count_sessions(cf_db_handle(db)) == 0);
    cf_db_close(db);

    cf_test_random_clear();
    cf_test_clock_clear();
    session_world_dispose(&world);
}

CF_TEST(session_start_rejects_unknown_user) {
    session_world world;
    session_world_open(&world);
    cf_test_clock_set_fixed_us(S_NOW1_US);
    cf_test_random_fill(0x10);

    struct start_op op;
    memset(&op, 0, sizeof op);
    op.user_id = 999999; /* no users row: the FK fails */
    CF_CHECK(run_start(world.app, &op) == CF_INVALID);
    CF_CHECK(op.result == CF_INVALID);
    CF_CHECK(session_empty(&op.session));

    cf_db *db = session_world_db(&world, true);
    CF_CHECK(count_sessions(cf_db_handle(db)) == 0);
    cf_db_close(db);

    cf_test_random_clear();
    cf_test_clock_clear();
    session_world_dispose(&world);
}

/* --- resume ---------------------------------------------------------------- */

CF_TEST(session_resume_within_the_hour_writes_nothing) {
    session_world world;
    session_world_open(&world);
    cf_db *db = session_world_db(&world, false);
    insert_user_row(db, S_FIX_USER, "User 4002");
    insert_session_row(db, S_FIX_ID, S_FIX_USER, S_FIX_TOKEN, S_FIX_IP,
                       S_FIX_AGENT, S_NOW1_TEXT, S_WHOLE_TEXT, S_NOW1_TEXT);
    cf_db_close(db);

    /* Resume at the same instant, then exactly one hour later: the reference
     * refreshes only when the activity is strictly more than an hour old. */
    cf_test_clock_set_fixed_us(S_NOW1_US);
    cf_session session;
    db = session_world_db(&world, true);
    CF_REQUIRE(cf_session_find(db, S_FIX_ID, &session) == CF_OK);
    cf_db_close(db);

    struct resume_op op;
    memset(&op, 0, sizeof op);
    op.session = &session;
    op.user_agent = opt_lit("ua2");
    op.ip_address = opt_lit("9.9.9.9");
    CF_REQUIRE(run_resume(world.app, &op) == CF_OK);
    CF_CHECK(opt_is(&session.user_agent, S_FIX_AGENT));
    CF_CHECK(opt_is(&session.ip_address, S_FIX_IP));

    cf_test_clock_set_fixed_us(S_NOW1_US + CF_SESSION_ACTIVITY_REFRESH_RATE_US);
    CF_REQUIRE(run_resume(world.app, &op) == CF_OK);
    CF_CHECK(opt_is(&session.user_agent, S_FIX_AGENT));

    db = session_world_db(&world, true);
    sqlite3 *handle = cf_db_handle(db);
    CF_CHECK(sql_text_is(handle,
                         "SELECT user_agent FROM sessions WHERE id = 5001",
                         S_FIX_AGENT));
    CF_CHECK(sql_text_is(handle,
                         "SELECT ip_address FROM sessions WHERE id = 5001",
                         S_FIX_IP));
    CF_CHECK(sql_text_is(handle,
                         "SELECT last_active_at FROM sessions WHERE id = 5001",
                         S_NOW1_TEXT));
    cf_db_close(db);

    cf_session_dispose(&session);
    cf_test_clock_clear();
    session_world_dispose(&world);
}

CF_TEST(session_resume_after_an_hour_refreshes_agent_ip_and_activity) {
    session_world world;
    session_world_open(&world);
    cf_db *db = session_world_db(&world, false);
    insert_user_row(db, S_FIX_USER, "User 4002");
    insert_session_row(db, S_FIX_ID, S_FIX_USER, S_FIX_TOKEN, S_FIX_IP,
                       S_FIX_AGENT, S_WHOLE_TEXT, S_WHOLE_TEXT, S_WHOLE_TEXT);
    cf_db_close(db);

    cf_session session;
    db = session_world_db(&world, true);
    CF_REQUIRE(cf_session_find(db, S_FIX_ID, &session) == CF_OK);
    cf_db_close(db);

    int64_t refreshed_at = S_WHOLE_US + CF_SESSION_ACTIVITY_REFRESH_RATE_US + 1;
    cf_test_clock_set_fixed_us(refreshed_at);

    struct resume_op op;
    memset(&op, 0, sizeof op);
    op.session = &session;
    op.user_agent = opt_lit("ua2");
    op.ip_address = opt_lit("9.9.9.9");
    CF_REQUIRE(run_resume(world.app, &op) == CF_OK);

    /* In-memory record refreshed before the UPDATE executed. */
    CF_CHECK(opt_is(&session.user_agent, "ua2"));
    CF_CHECK(opt_is(&session.ip_address, "9.9.9.9"));
    CF_CHECK(session.last_active_at == refreshed_at);
    CF_CHECK(session.updated_at == refreshed_at);

    /* Committed row carries the same values and reference text. */
    db = session_world_db(&world, true);
    cf_session reloaded;
    CF_REQUIRE(cf_session_find(db, S_FIX_ID, &reloaded) == CF_OK);
    CF_CHECK(opt_is(&reloaded.user_agent, "ua2"));
    CF_CHECK(opt_is(&reloaded.ip_address, "9.9.9.9"));
    CF_CHECK(reloaded.last_active_at == refreshed_at);
    CF_CHECK(reloaded.last_active_at > S_WHOLE_US);
    CF_CHECK(reloaded.updated_at == refreshed_at);
    cf_session_dispose(&reloaded);
    sqlite3 *handle = cf_db_handle(db);
    CF_CHECK(sql_text_is(handle,
                         "SELECT last_active_at FROM sessions WHERE id = 5001",
                         "2026-01-02 04:04:05.000001"));
    cf_db_close(db);

    /* A later resume with absent optionals clears them (Rails assigns nil). */
    int64_t cleared_at = refreshed_at + CF_SESSION_ACTIVITY_REFRESH_RATE_US + 1;
    cf_test_clock_set_fixed_us(cleared_at);
    memset(&op, 0, sizeof op);
    op.session = &session;
    CF_REQUIRE(run_resume(world.app, &op) == CF_OK);
    CF_CHECK(opt_absent(&session.user_agent));
    CF_CHECK(opt_absent(&session.ip_address));

    db = session_world_db(&world, true);
    CF_CHECK(sql_is_null(cf_db_handle(db),
                         "SELECT user_agent FROM sessions WHERE id = 5001"));
    CF_CHECK(sql_is_null(cf_db_handle(db),
                         "SELECT ip_address FROM sessions WHERE id = 5001"));
    cf_db_close(db);

    cf_session_dispose(&session);
    cf_test_clock_clear();
    session_world_dispose(&world);
}

CF_TEST(session_mutations_reject_missing_arguments) {
    cf_session session;
    memset(&session, 0, sizeof session);
    CF_CHECK(cf_session_resume(NULL, &session, (cf_optional_str){0},
                               (cf_optional_str){0}) == CF_INVALID);
    CF_CHECK(cf_session_resume((cf_tx *)&session, NULL, (cf_optional_str){0},
                               (cf_optional_str){0}) == CF_INVALID);

    cf_session out;
    memset(&out, 0xA5, sizeof out);
    CF_CHECK(cf_session_start(NULL, 1, (cf_optional_str){0},
                              (cf_optional_str){0}, &out) == CF_INVALID);
    CF_CHECK(session_empty(&out));
    CF_CHECK(cf_session_start((cf_tx *)&session, 1, (cf_optional_str){0},
                              (cf_optional_str){0}, NULL) == CF_INVALID);
}

/* --- destroy --------------------------------------------------------------- */

CF_TEST(session_destroy_deletes_the_row_immediately) {
    session_world world;
    session_world_open(&world);
    cf_db *db = session_world_db(&world, false);
    insert_user_row(db, S_FIX_USER, "User 4002");
    cf_db_close(db);

    cf_test_clock_set_fixed_us(S_NOW1_US);
    cf_test_random_fill(0x10);
    struct start_op op;
    memset(&op, 0, sizeof op);
    op.user_id = S_FIX_USER;
    CF_REQUIRE(run_start(world.app, &op) == CF_OK);
    int64_t session_id = op.session.id;

    struct destroy_op destroy;
    memset(&destroy, 0, sizeof destroy);
    destroy.session = &op.session;
    CF_REQUIRE(run_destroy(world.app, &destroy) == CF_OK);
    CF_CHECK(destroy.result == CF_OK);

    /* Deletion affects authentication immediately: no row, no token. */
    db = session_world_db(&world, true);
    cf_session missing;
    memset(&missing, 0xA5, sizeof missing);
    CF_CHECK(cf_session_find(db, session_id, &missing) == CF_NOT_FOUND);
    CF_CHECK(session_empty(&missing));
    bool found = true;
    CF_CHECK(cf_session_find_by_token(db, op.session.token, &found,
                                      &missing) == CF_OK);
    CF_CHECK(!found);
    int64_t count = -1;
    CF_CHECK(cf_session_count_for_user(db, S_FIX_USER, &count) == CF_OK);
    CF_CHECK(count == 0);
    cf_db_close(db);

    /* The reference ignores the affected-row count: deleting again is OK. */
    CF_REQUIRE(run_destroy(world.app, &destroy) == CF_OK);
    CF_CHECK(destroy.result == CF_OK);

    cf_session_dispose(&op.session);
    cf_test_random_clear();
    cf_test_clock_clear();
    session_world_dispose(&world);
}

CF_TEST(session_destroy_rejects_missing_arguments) {
    cf_session session;
    memset(&session, 0, sizeof session);
    CF_CHECK(cf_session_destroy(NULL, &session) == CF_INVALID);
    CF_CHECK(cf_session_destroy((cf_tx *)&session, NULL) == CF_INVALID);
}

CF_TEST_MAIN();
