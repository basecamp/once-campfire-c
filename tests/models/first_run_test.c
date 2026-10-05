/* tests/models/first_run_test.c — D01 model family "first_run" tests.
 *
 * Reference oracles (pinned; hashes in reference-files.json):
 *  - tmp/rust-ref/crates/db/src/models/first_run.rs (the implemented source);
 *  - tests/fixtures/crates/db/src/tests/first_run_test.rs, the port of
 *    tmp/rails-ref/test/models/first_run_test.rb: the first user is an
 *    administrator, has exactly one room named "All Talk", that room is open,
 *    and the stored digest makes the reference sign-in path possible.
 * The reference test deletes every seeded row first (fresh()); a scratch
 * database created by the writer is the same starting state, so no fixture
 * loader is needed here.
 *
 * cf_tx is opaque and only the D02 writer creates one, so every mutation goes
 * through the public cf_write path (00-contracts.md: "cf_write accepts a
 * callback and argument, waits on its completion, and returns its result").
 * The app is configured with a scratch DATABASE_PATH, so the writer creates
 * and uses a fresh version-1 database; committed rows are then read through a
 * separate read-only cf_db. PasswordDigest belongs to A01: as in the C model
 * boundary, the digest arrives already hashed and is stored verbatim.
 *
 * Build (plain; add -fsanitize=address,undefined -fno-sanitize-recover=all
 * -fno-omit-frame-pointer -g for the sanitize run). --gc-sections is required
 * until A01/R02 land: message.c and user.c contain not-yet-implemented
 * boundaries (cf_tx_rich_text / cf_richtext_* for R02, cf_password_verify for
 * A01) in functions the first_run path never calls; the weak stand-ins above
 * cover the unowned types.h helpers. Exact command used (all 15 model units,
 * D02 writer, core, config/app, escape for cf_json_string):
 *   clang -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra
 *         -Werror -pthread -ffunction-sections -fdata-sections -Isrc -Itests
 *         -Ivendor/src/sqlite/sqlite-amalgamation-3530400
 *         -Ivendor/src/yyjson/src
 *         tests/models/first_run_test.c plus every src/models model unit
 *         src/core/{alloc,buffer,clock,error,random}.c src/db/{schema,reader,
 *         statements,writer}.c src/config.c src/app.c src/views/escape.c
 *         vendor/build/sqlite-clang/libsqlite3.a
 *         vendor/build/yyjson-clang/libyyjson.a
 *         -Wl,--gc-sections -lm -o build/d01-first-run/test_first_run
 */
#include "cf_test.h"

#include "app.h"
#include "cf.h"
#include "config.h"
#include "db/db_internal.h"
#include "db/writer.h" /* cf_writer_start/stop: the D02 writer bootstrap */
#include "models/first_run.h"
#include "models/user.h" /* concrete cf_user record and its dispose */

#include <pthread.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The shared release helpers come from src/models/types.c; the A01
 * cf_password_verify boundary comes from tests/models/support/password.c
 * (first_run never authenticates, so the verifier is not exercised here). */

#define FR_SECRET_HEX \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define FR_USER_NAME "User"
#define FR_USER_EMAIL "user@example.com"
/* Stands in for PasswordDigest::create("secret123456", 4) from the reference
 * test: first_run stores the A01-produced digest verbatim. */
#define FR_PASSWORD_DIGEST \
    "$2a$04$0123456789012345678901234567890123456789012345678901"

/* ---- scratch app + reader ------------------------------------------------- */

typedef struct {
    char *dir;
    char path[512];
    cf_app *app;
} fr_env;

static void fr_env_close(fr_env *env);

static bool fr_env_open(fr_env *env) {
    memset(env, 0, sizeof *env);
    char template[] = "/tmp/cf_first_run_XXXXXX";
    char *dir = mkdtemp(template);
    if (dir == NULL) {
        return false;
    }
    env->dir = strdup(dir);
    if (env->dir == NULL) {
        return false;
    }
    snprintf(env->path, sizeof env->path, "%s/test.sqlite3", dir);

    cf_config_entry entries[] = {
        {"PUBLIC_ORIGIN", "http://127.0.0.1:32123"},
        {"SECRET_KEY_BASE", FR_SECRET_HEX},
        {"DATABASE_PATH", env->path},
    };
    cf_config *config = NULL;
    if (cf_config_parse(entries, sizeof entries / sizeof entries[0], NULL,
                        &config) != CF_OK) {
        fr_env_close(env);
        return false;
    }
    if (cf_app_create(config, &env->app) != CF_OK) {
        cf_config_destroy(config);
        fr_env_close(env);
        return false;
    }
    /* D02: nothing may call cf_write before the writer is started. The writer
     * creates/validates the scratch database at DATABASE_PATH. */
    if (cf_writer_start(env->app, config) != CF_OK) {
        fr_env_close(env);
        return false;
    }
    return true;
}

static void fr_env_close(fr_env *env) {
    cf_writer_stop(env->app); /* no cf_write caller is active here */
    cf_app_destroy(env->app);
    env->app = NULL;
    if (env->dir != NULL) {
        remove(env->path);
        char sidecar[600];
        snprintf(sidecar, sizeof sidecar, "%s-wal", env->path);
        remove(sidecar);
        snprintf(sidecar, sizeof sidecar, "%s-shm", env->path);
        remove(sidecar);
        rmdir(env->dir);
        free(env->dir);
        env->dir = NULL;
    }
}

static cf_db *fr_env_reader(const fr_env *env) {
    cf_db *db = NULL;
    if (cf_db_open(env->path, true, &db) != CF_OK) {
        return NULL;
    }
    return db;
}

/* ---- raw row inspection (copied-before-reset discipline not needed:
 * statements are finalized after each single-row read) ---------------------- */

static int64_t fr_scalar_i64(sqlite3 *handle, const char *sql, bool *ok) {
    *ok = false;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(handle, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    int64_t value = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *ok = true;
        value = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return value;
}

static bool fr_i64_is(sqlite3 *handle, const char *sql, int64_t want) {
    bool ok = false;
    int64_t got = fr_scalar_i64(handle, sql, &ok);
    return ok && got == want;
}

static bool fr_text_is(sqlite3 *handle, const char *sql, const char *want) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(handle, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }
    bool match = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        match = text != NULL && strcmp((const char *)text, want) == 0;
    }
    sqlite3_finalize(stmt);
    return match;
}

static bool fr_is_null(sqlite3 *handle, const char *sql) {
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

static int64_t fr_count(sqlite3 *handle, const char *table) {
    char sql[128];
    snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM \"%s\"", table);
    bool ok = false;
    int64_t count = fr_scalar_i64(handle, sql, &ok);
    return ok ? count : -1;
}

static bool fr_columns_equal(sqlite3 *handle, const char *left_sql,
                             const char *right_sql) {
    bool left_ok = false, right_ok = false;
    int64_t left = fr_scalar_i64(handle, left_sql, &left_ok);
    int64_t right = fr_scalar_i64(handle, right_sql, &right_ok);
    return left_ok && right_ok && left == right;
}

static bool fr_str_is(const cf_str *value, const char *want) {
    size_t want_len = strlen(want);
    return value->ptr != NULL && value->len == want_len &&
           memcmp(value->ptr, want, want_len) == 0 &&
           value->ptr[value->len] == '\0';
}

/* ---- the mutation under test ---------------------------------------------- */

struct fr_attempt {
    cf_err callback_result;
    cf_user user;
};

static cf_err fr_create_cb(cf_tx *tx, void *arg) {
    struct fr_attempt *attempt = arg;
    attempt->callback_result = cf_first_run_create(
        tx, CF_STR_LIT(FR_USER_NAME), CF_STR_LIT(FR_USER_EMAIL),
        CF_STR_LIT(FR_PASSWORD_DIGEST), &attempt->user);
    return attempt->callback_result;
}

static cf_err fr_run(cf_app *app, struct fr_attempt *attempt) {
    memset(attempt, 0, sizeof *attempt);
    return cf_write(app, fr_create_cb, attempt);
}

/* The committed state of exactly one setup: singleton account, one
 * administrator, one open "All Talk" room owned by that user, and exactly one
 * membership joining them (Room::for_user(admin) has one room). */
static void fr_check_single_setup(sqlite3 *handle) {
    CF_CHECK(fr_count(handle, "accounts") == 1);
    CF_CHECK(fr_text_is(handle, "SELECT name FROM accounts", "Campfire"));
    CF_CHECK(fr_i64_is(handle, "SELECT singleton_guard FROM accounts", 0));

    CF_CHECK(fr_count(handle, "users") == 1);
    CF_CHECK(fr_text_is(handle, "SELECT name FROM users", FR_USER_NAME));
    CF_CHECK(fr_text_is(handle, "SELECT email_address FROM users",
                        FR_USER_EMAIL));
    CF_CHECK(fr_text_is(handle, "SELECT password_digest FROM users",
                        FR_PASSWORD_DIGEST));
    CF_CHECK(fr_i64_is(handle, "SELECT role FROM users",
                       CF_ROLE_ADMINISTRATOR));
    CF_CHECK(fr_i64_is(handle, "SELECT status FROM users", CF_STATUS_ACTIVE));
    CF_CHECK(fr_is_null(handle, "SELECT bio FROM users"));
    CF_CHECK(fr_is_null(handle, "SELECT bot_token FROM users"));

    CF_CHECK(fr_count(handle, "rooms") == 1);
    CF_CHECK(fr_text_is(handle, "SELECT name FROM rooms",
                        CF_FIRST_RUN_FIRST_ROOM_NAME));
    CF_CHECK(fr_text_is(handle, "SELECT type FROM rooms", "Rooms::Open"));
    CF_CHECK(fr_columns_equal(handle, "SELECT creator_id FROM rooms",
                              "SELECT id FROM users"));

    CF_CHECK(fr_count(handle, "memberships") == 1);
    CF_CHECK(fr_columns_equal(handle, "SELECT room_id FROM memberships",
                              "SELECT id FROM rooms"));
    CF_CHECK(fr_columns_equal(handle, "SELECT user_id FROM memberships",
                              "SELECT id FROM users"));
    /* RoomType::Open::default_involvement() is "mentions". */
    CF_CHECK(fr_text_is(handle, "SELECT involvement FROM memberships",
                        "mentions"));
    CF_CHECK(fr_i64_is(handle, "SELECT connections FROM memberships", 0));
    CF_CHECK(fr_is_null(handle, "SELECT unread_at FROM memberships"));
    CF_CHECK(fr_is_null(handle, "SELECT connected_at FROM memberships"));
}

/* ---- cases ---------------------------------------------------------------- */

CF_TEST(creates_singleton_account_administrator_room_and_membership) {
    fr_env env;
    CF_REQUIRE(fr_env_open(&env));

    struct fr_attempt attempt;
    CF_REQUIRE(fr_run(env.app, &attempt) == CF_OK);
    CF_REQUIRE(attempt.callback_result == CF_OK);

    /* The returned record is the administrator User::create stored. */
    const cf_user *admin = &attempt.user;
    CF_CHECK(admin->id > 0);
    CF_CHECK(fr_str_is(&admin->name, FR_USER_NAME));
    CF_CHECK(admin->email_address.present &&
             fr_str_is(&admin->email_address.value, FR_USER_EMAIL));
    CF_CHECK(admin->password_digest.present &&
             fr_str_is(&admin->password_digest.value, FR_PASSWORD_DIGEST));
    CF_CHECK(admin->role == CF_ROLE_ADMINISTRATOR);
    CF_CHECK(admin->status == CF_STATUS_ACTIVE);
    CF_CHECK(!admin->bio.present);
    CF_CHECK(!admin->bot_token.present);
    CF_CHECK(admin->created_at > 0);
    CF_CHECK(admin->updated_at > 0);

    cf_db *reader = fr_env_reader(&env);
    CF_REQUIRE(reader != NULL);
    sqlite3 *handle = cf_db_handle(reader);
    CF_REQUIRE(handle != NULL);

    fr_check_single_setup(handle);
    bool ok = false;
    CF_CHECK(fr_scalar_i64(handle, "SELECT id FROM users", &ok) == admin->id &&
             ok);

    cf_db_close(reader);
    cf_user_dispose(&attempt.user);
    fr_env_close(&env);
}

CF_TEST(second_setup_fails_singleton_and_leaves_first_run_intact) {
    fr_env env;
    CF_REQUIRE(fr_env_open(&env));

    struct fr_attempt first;
    CF_REQUIRE(fr_run(env.app, &first) == CF_OK);
    CF_REQUIRE(first.callback_result == CF_OK);
    cf_user_dispose(&first.user);

    /* The account's unique singleton_guard rejects a second setup; the
     * callback fails, D02 rolls the transaction back, and out stays empty. */
    struct fr_attempt second;
    cf_err write_err = fr_run(env.app, &second);
    CF_CHECK(write_err == CF_INVALID);
    CF_CHECK(second.callback_result == CF_INVALID);
    CF_CHECK(second.user.id == 0);
    CF_CHECK(second.user.name.ptr == NULL && second.user.name.len == 0);
    CF_CHECK(!second.user.email_address.present);
    CF_CHECK(!second.user.password_digest.present);

    cf_db *reader = fr_env_reader(&env);
    CF_REQUIRE(reader != NULL);
    sqlite3 *handle = cf_db_handle(reader);
    CF_REQUIRE(handle != NULL);
    fr_check_single_setup(handle);

    cf_db_close(reader);
    fr_env_close(&env);
}

/* DB-04: two concurrent setup requests yield one account, administrator and
 * original room. The writer serializes the two transactions; the second one
 * fails on the singleton guard and rolls back, nothing is duplicated. */
typedef struct {
    cf_app *app;
    pthread_barrier_t *barrier;
    struct fr_attempt attempt;
    cf_err write_err;
} fr_setup_thread;

static void *fr_setup_thread_main(void *arg) {
    fr_setup_thread *run = arg;
    pthread_barrier_wait(run->barrier);
    run->write_err = cf_write(run->app, fr_create_cb, &run->attempt);
    return NULL;
}

CF_TEST(concurrent_setups_yield_one_setup_db04) {
    fr_env env;
    CF_REQUIRE(fr_env_open(&env));

    pthread_barrier_t barrier;
    CF_REQUIRE(pthread_barrier_init(&barrier, NULL, 2) == 0);

    fr_setup_thread runs[2];
    memset(runs, 0, sizeof runs);
    pthread_t threads[2];
    for (size_t i = 0; i < 2; i++) {
        runs[i].app = env.app;
        runs[i].barrier = &barrier;
        CF_REQUIRE(pthread_create(&threads[i], NULL, fr_setup_thread_main,
                                  &runs[i]) == 0);
    }
    for (size_t i = 0; i < 2; i++) {
        CF_REQUIRE(pthread_join(threads[i], NULL) == 0);
    }
    CF_REQUIRE(pthread_barrier_destroy(&barrier) == 0);

    int succeeded = 0;
    for (size_t i = 0; i < 2; i++) {
        CF_CHECK(runs[i].attempt.callback_result == CF_OK ||
                 runs[i].attempt.callback_result == CF_INVALID);
        if (runs[i].write_err == CF_OK) {
            CF_CHECK(runs[i].attempt.callback_result == CF_OK);
            succeeded++;
        } else {
            CF_CHECK(runs[i].attempt.callback_result == CF_INVALID);
            CF_CHECK(runs[i].write_err == CF_INVALID);
        }
    }
    CF_CHECK(succeeded == 1);

    cf_db *reader = fr_env_reader(&env);
    CF_REQUIRE(reader != NULL);
    sqlite3 *handle = cf_db_handle(reader);
    CF_REQUIRE(handle != NULL);
    fr_check_single_setup(handle);

    cf_db_close(reader);
    for (size_t i = 0; i < 2; i++) {
        if (runs[i].attempt.callback_result == CF_OK) {
            cf_user_dispose(&runs[i].attempt.user);
        }
    }
    fr_env_close(&env);
}

/* Invalid C arguments and an empty out pointer: CF_INVALID, and because the
 * callback fails the whole transaction (account included) rolls back. */
static cf_err fr_null_out_cb(cf_tx *tx, void *arg) {
    cf_err *result = arg;
    *result = cf_first_run_create(tx, CF_STR_LIT(FR_USER_NAME),
                                  CF_STR_LIT(FR_USER_EMAIL),
                                  CF_STR_LIT(FR_PASSWORD_DIGEST), NULL);
    return *result;
}

CF_TEST(missing_tx_or_out_is_invalid_and_rolls_back) {
    cf_user direct = {0};
    CF_CHECK(cf_first_run_create(NULL, CF_STR_LIT(FR_USER_NAME),
                                 CF_STR_LIT(FR_USER_EMAIL),
                                 CF_STR_LIT(FR_PASSWORD_DIGEST),
                                 &direct) == CF_INVALID);
    CF_CHECK(direct.id == 0 && direct.name.ptr == NULL);

    fr_env env;
    CF_REQUIRE(fr_env_open(&env));

    cf_err callback_result = CF_OK;
    CF_CHECK(cf_write(env.app, fr_null_out_cb, &callback_result) == CF_INVALID);
    CF_CHECK(callback_result == CF_INVALID);

    cf_db *reader = fr_env_reader(&env);
    CF_REQUIRE(reader != NULL);
    sqlite3 *handle = cf_db_handle(reader);
    CF_REQUIRE(handle != NULL);
    CF_CHECK(fr_count(handle, "accounts") == 0);
    CF_CHECK(fr_count(handle, "users") == 0);
    CF_CHECK(fr_count(handle, "rooms") == 0);
    CF_CHECK(fr_count(handle, "memberships") == 0);

    cf_db_close(reader);
    fr_env_close(&env);
}

CF_TEST_MAIN()
