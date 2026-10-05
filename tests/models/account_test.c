/* D01 model family "account" tests.
 *
 * Reference oracles (pinned; hashes in contracts/reference-files.json):
 *  - tmp/rust-ref/crates/db/src/models/account.rs (the implemented source,
 *    15 functions);
 *  - tests/fixtures/crates/db/src/tests/account_test.rs (settings round trip,
 *    NULL settings untouched by name updates, unknown keys rejected, join
 *    code shape 4-4-4, singleton guard, reset_join_code);
 *  - tests/fixtures/crates/db/src/tests/columns_test.rs (field/column
 *    layout, id 6001, settings {"n":6005});
 *  - tests/fixtures/crates/db/src/tests/differential_test.rs (the settings
 *    write is its own transaction).
 *
 * Mutations go through the public cf_write path (00-contracts.md: cf_tx is
 * opaque; only D02's writer creates one) on a scratch database created by
 * cf_db_open (fresh schema, D-C01).  A fixed test clock makes created_at /
 * updated_at deterministic and a fixed RNG byte makes join codes
 * deterministic; there are no sleeps.
 *
 * Build (plain; add -fsanitize=address,undefined -fno-sanitize-recover=all
 * -fno-omit-frame-pointer -g for the sanitize run).  The binary needs D02's
 * writer; src/models/types.c does not own the shared text disposers yet, so
 * this test frees borrowed text through its own helpers and never calls
 * cf_str_dispose (see docs/devel/evidence/D01-model-account.md):
 *   clang -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra
 *         -Werror -pthread -Isrc -Itests
 *         -Ivendor/src/sqlite/sqlite-amalgamation-3530400
 *         -Ivendor/src/yyjson/src
 *         tests/models/account_test.c src/models/account.c
 *         src/core/{alloc,buffer,clock,error,random}.c
 *         src/config.c src/app.c
 *         src/db/{schema,reader,statements,writer}.c
 *         vendor/build/sqlite-clang/libsqlite3.a
 *         vendor/build/yyjson-clang/libyyjson.a -lm
 *         -o build/d01-account/plain/test_account
 */
#include "models/account.h"

#include "app.h"
#include "cf.h"
#include "cf_test.h"
#include "config.h"
#include "core/testclock.h"
#include "core/testrandom.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "db/writer.h" /* cf_writer_start/stop: the D02 writer bootstrap */

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIXED_NOW INT64_C(1790000000123456)   /* 2026-09-21 14:13:20.123456 */
#define FIXED_LATER INT64_C(1790000001123457) /* +1 s +1 us */
#define RAW_FRAC_TIME_US INT64_C(1767323045000006) /* 2026-01-02 03:04:05.000006 */
#define RAW_ZERO_TIME_US INT64_C(1767323045000000) /* 2026-01-02 03:04:05 */

#define DEFAULT_SETTINGS \
    "{\"restrict_room_creation_to_administrators\":false}"
#define SETTINGS_TRUE \
    "{\"restrict_room_creation_to_administrators\":true}"
#define SETTINGS_FALSE \
    "{\"restrict_room_creation_to_administrators\":false}"
#define SETTINGS_NULL \
    "{\"restrict_room_creation_to_administrators\":null}"
#define SETTINGS_KEY "restrict_room_creation_to_administrators"

#define ACCOUNT_ORIGIN "http://127.0.0.1:32123"
#define ACCOUNT_HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

/* --- scratch database + app (D02 writer) --------------------------------- */

typedef struct {
    char *dir;
    char path[CF_DB_TEST_PATH_CAP];
    cf_db *db; /* scratch connection: raw setup and reads */
    cf_app *app;
} account_env;

static void env_open(account_env *env) {
    memset(env, 0, sizeof *env);
    env->dir = cf_db_test_dir();
    CF_REQUIRE(env->dir != NULL);
    cf_db_test_path(env->path, sizeof env->path, env->dir);

    /* Fresh schema (D-C01) so the writer's own connection opens an existing
     * version-1 database regardless of D02's lazy-open strategy. */
    CF_REQUIRE(cf_db_open(env->path, false, &env->db) == CF_OK);

    cf_config_entry entries[3] = {
        {"PUBLIC_ORIGIN", ACCOUNT_ORIGIN},
        {"SECRET_KEY_BASE", ACCOUNT_HEX64},
        {"DATABASE_PATH", env->path},
    };
    cf_config *config = NULL;
    CF_REQUIRE(cf_config_parse(entries, 3, NULL, &config) == CF_OK);
    CF_REQUIRE(cf_app_create(config, &env->app) == CF_OK);
    /* D02: cf_write returns CF_INTERNAL until the writer thread is started. */
    CF_REQUIRE(cf_writer_start(env->app, config) == CF_OK);
}

static void env_close(account_env *env) {
    cf_writer_stop(env->app); /* no cf_write caller is active here */
    cf_app_destroy(env->app);
    env->app = NULL;
    cf_db_close(env->db);
    env->db = NULL;
    if (env->dir != NULL) {
        cf_db_test_cleanup(env->dir);
        free(env->dir);
        env->dir = NULL;
    }
}

static int exec_sql(account_env *env, const char *sql) {
    return sqlite3_exec(cf_db_handle(env->db), sql, NULL, NULL, NULL);
}

/* Replace the singleton row with a raw one; settings_sql is a SQL literal
 * (NULL, or a single-quoted JSON string). */
static void set_raw_row(account_env *env, const char *settings_sql) {
    char sql[1024];
    snprintf(sql, sizeof sql,
             "DELETE FROM \"accounts\"; "
             "INSERT INTO \"accounts\" (\"name\", \"join_code\", "
             "\"custom_styles\", \"settings\", \"singleton_guard\", "
             "\"created_at\", \"updated_at\") VALUES "
             "('Raw', 'raw-code', NULL, %s, 0, "
             "'2026-01-02 03:04:05.000006', '2026-01-02 03:04:05')",
             settings_sql);
    CF_REQUIRE(exec_sql(env, sql) == SQLITE_OK);
}

static cf_err first_into(account_env *env, cf_account *out) {
    bool found = false;
    cf_err err = cf_account_first(env->db, &found, out);
    if (err != CF_OK) return err;
    return found ? CF_OK : CF_NOT_FOUND;
}

/* Refresh `out` from the database (the reference test's reload_from). */
static cf_err reload_first(account_env *env, cf_account *out) {
    cf_account fresh = {0};
    cf_err err = first_into(env, &fresh);
    if (err != CF_OK) return err;
    cf_account_dispose(out);
    *out = fresh;
    return CF_OK;
}

/* --- mutation callbacks (D02 cf_write) ----------------------------------- */

struct create_op {
    cf_str name;
    cf_err result;
    cf_account account;
    char message[160];
};

static cf_err create_cb(cf_tx *tx, void *arg) {
    struct create_op *op = arg;
    op->result = cf_account_create(tx, op->name, &op->account);
    snprintf(op->message, sizeof op->message, "%s", cf_db_last_error());
    return op->result;
}

static cf_err run_create(account_env *env, const char *name, cf_account *out,
                         char *message, size_t cap) {
    struct create_op op = {0};
    op.name = (cf_str){(char *)name, strlen(name)};
    cf_err err = cf_write(env->app, create_cb, &op);
    if (err == CF_OK) {
        *out = op.account;
    } else {
        cf_account_dispose(&op.account);
    }
    if (message != NULL) snprintf(message, cap, "%s", op.message);
    return err;
}

struct update_op {
    cf_account *account;
    cf_optional_str name;
    const cf_optional_str *custom_styles;
    const cf_account_setting *settings;
    size_t settings_len;
    cf_err result;
    char message[160];
};

static cf_err update_cb(cf_tx *tx, void *arg) {
    struct update_op *op = arg;
    op->result = cf_account_update(tx, op->account, op->name,
                                   op->custom_styles, op->settings,
                                   op->settings_len);
    snprintf(op->message, sizeof op->message, "%s", cf_db_last_error());
    return op->result;
}

static cf_err run_update(account_env *env, cf_account *account,
                         cf_optional_str name,
                         const cf_optional_str *custom_styles,
                         const cf_account_setting *settings,
                         size_t settings_len, char *message, size_t cap) {
    struct update_op op = {0};
    op.account = account;
    op.name = name;
    op.custom_styles = custom_styles;
    op.settings = settings;
    op.settings_len = settings_len;
    cf_err err = cf_write(env->app, update_cb, &op);
    if (message != NULL) snprintf(message, cap, "%s", op.message);
    return err;
}

struct reset_op {
    cf_account *account;
    cf_err result;
};

static cf_err reset_cb(cf_tx *tx, void *arg) {
    struct reset_op *op = arg;
    op->result = cf_account_reset_join_code(tx, op->account);
    return op->result;
}

static cf_err run_reset(account_env *env, cf_account *account) {
    struct reset_op op = {.account = account};
    return cf_write(env->app, reset_cb, &op);
}

/* --- test-local disposal (cf_str_dispose and friends are another D01
 * subpacket's translation unit and are not linked here) ------------------- */

static void free_str(cf_str *value) {
    free(value->ptr);
    value->ptr = NULL;
    value->len = 0;
}

static void free_errors(cf_model_errors *errors) {
    for (size_t i = 0; i < errors->len; i++) {
        free(errors->items[i].field.ptr);
        free(errors->items[i].message.ptr);
    }
    free(errors->items);
    errors->items = NULL;
    errors->len = 0;
    errors->cap = 0;
}

/* --- small helpers ------------------------------------------------------- */

static bool str_is(cf_str value, const char *literal) {
    size_t len = strlen(literal);
    if (value.len != len) return false;
    if (len == 0) return true;
    return value.ptr != NULL && memcmp(value.ptr, literal, len) == 0;
}

static cf_optional_str opt_none(void) {
    return (cf_optional_str){false, {NULL, 0}};
}

static cf_optional_str opt_some(const char *text) {
    cf_optional_str value = {true, {(char *)text, strlen(text)}};
    return value;
}

/* --- create / first / find / count --------------------------------------- */

CF_TEST(create_first_find_count_roundtrip) {
    account_env env;
    env_open(&env);
    cf_test_clock_set_fixed_us(FIXED_NOW);
    cf_test_random_fill(0);

    cf_account account = {0};
    CF_REQUIRE(run_create(&env, "Chat", &account, NULL, 0) == CF_OK);
    CF_CHECK(account.id > 0);
    CF_CHECK(str_is(account.name, "Chat"));
    CF_CHECK(str_is(account.join_code, "AAAA-AAAA-AAAA"));
    CF_CHECK(account.custom_styles.present == false);
    CF_REQUIRE(account.settings_json.present);
    CF_CHECK(str_is(account.settings_json.value, DEFAULT_SETTINGS));
    CF_CHECK(account.singleton_guard == 0);
    CF_CHECK(account.created_at == FIXED_NOW);
    CF_CHECK(account.updated_at == FIXED_NOW);

    cf_account first = {0};
    bool found = false;
    CF_REQUIRE(cf_account_first(env.db, &found, &first) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(first.id == account.id);
    CF_CHECK(str_is(first.name, "Chat"));
    CF_CHECK(str_is(first.join_code, "AAAA-AAAA-AAAA"));
    CF_CHECK(first.created_at == FIXED_NOW);
    CF_CHECK(first.updated_at == FIXED_NOW);
    cf_account_dispose(&first);

    cf_account by_id = {0};
    CF_REQUIRE(cf_account_find(env.db, account.id, &by_id) == CF_OK);
    CF_CHECK(by_id.id == account.id);
    CF_CHECK(str_is(by_id.name, "Chat"));
    CF_REQUIRE(by_id.settings_json.present);
    CF_CHECK(str_is(by_id.settings_json.value, DEFAULT_SETTINGS));
    cf_account_dispose(&by_id);

    int64_t count = -1;
    CF_REQUIRE(cf_account_count(env.db, &count) == CF_OK);
    CF_CHECK(count == 1);

    /* Stored datetimes are the reference UTC SQL text: six fraction digits
     * when the microseconds are non-zero. */
    sqlite3_stmt *raw = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(env.db),
                                  "SELECT \"created_at\", \"updated_at\" FROM "
                                  "\"accounts\"",
                                  -1, &raw, NULL) == SQLITE_OK);
    CF_REQUIRE(sqlite3_step(raw) == SQLITE_ROW);
    const char *created = (const char *)sqlite3_column_text(raw, 0);
    const char *updated = (const char *)sqlite3_column_text(raw, 1);
    CF_CHECK(created != NULL &&
             strcmp(created, "2026-09-21 14:13:20.123456") == 0);
    CF_CHECK(updated != NULL &&
             strcmp(updated, "2026-09-21 14:13:20.123456") == 0);
    sqlite3_finalize(raw);

    cf_account_dispose(&account);
    cf_test_random_clear();
    cf_test_clock_clear();
    env_close(&env);
}

CF_TEST(first_orders_by_id_and_count_multiple) {
    account_env env;
    env_open(&env);
    CF_REQUIRE(exec_sql(&env,
                        "INSERT INTO \"accounts\" (\"id\", \"name\", "
                        "\"join_code\", \"singleton_guard\", \"created_at\", "
                        "\"updated_at\") VALUES "
                        "(7, 'seven', 'c-seven', 1, '2026-01-02 03:04:05', "
                        "'2026-01-02 03:04:05'), "
                        "(3, 'three', 'c-three', 0, '2026-01-02 03:04:05', "
                        "'2026-01-02 03:04:05')") == SQLITE_OK);

    cf_account first = {0};
    bool found = false;
    CF_REQUIRE(cf_account_first(env.db, &found, &first) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(first.id == 3);
    CF_CHECK(str_is(first.name, "three"));
    cf_account_dispose(&first);

    int64_t count = -1;
    CF_REQUIRE(cf_account_count(env.db, &count) == CF_OK);
    CF_CHECK(count == 2);

    env_close(&env);
}

CF_TEST(create_second_account_is_invalid_and_random_failure) {
    account_env env;
    env_open(&env);
    cf_test_random_fill(7);

    cf_account first = {0};
    CF_REQUIRE(run_create(&env, "One", &first, NULL, 0) == CF_OK);

    cf_account second = {0};
    char message[160] = {0};
    CF_CHECK(run_create(&env, "Two", &second, message, sizeof message) ==
             CF_INVALID);
    CF_CHECK(second.id == 0 && second.name.ptr == NULL);
    CF_CHECK(strstr(message, "UNIQUE") != NULL);

    int64_t count = -1;
    CF_REQUIRE(cf_account_count(env.db, &count) == CF_OK);
    CF_CHECK(count == 1);

    /* The randomness source failing fails create before any insert. */
    cf_test_random_fail();
    cf_account failed = {0};
    CF_CHECK(run_create(&env, "Three", &failed, NULL, 0) == CF_IO);
    CF_CHECK(failed.id == 0);
    CF_REQUIRE(cf_account_count(env.db, &count) == CF_OK);
    CF_CHECK(count == 1);

    cf_test_random_clear();
    cf_account_dispose(&first);
    env_close(&env);
}

CF_TEST(first_and_count_empty_database) {
    account_env env;
    env_open(&env);

    cf_account absent = {0};
    bool found = true;
    CF_REQUIRE(cf_account_first(env.db, &found, &absent) == CF_OK);
    CF_CHECK(found == false);
    CF_CHECK(absent.id == 0 && absent.name.ptr == NULL &&
             absent.join_code.ptr == NULL &&
             absent.settings_json.present == false);
    cf_account_dispose(&absent);

    int64_t count = -1;
    CF_REQUIRE(cf_account_count(env.db, &count) == CF_OK);
    CF_CHECK(count == 0);

    env_close(&env);
}

CF_TEST(find_missing_row_is_not_found) {
    account_env env;
    env_open(&env);

    cf_account missing = {0};
    CF_CHECK(cf_account_find(env.db, 42, &missing) == CF_NOT_FOUND);
    CF_CHECK(missing.id == 0 && missing.name.ptr == NULL);
    CF_CHECK(strcmp(cf_db_last_error(), "Couldn't find Account") == 0);
    cf_account_dispose(&missing);

    env_close(&env);
}

/* --- Account::settings / AccountSettings --------------------------------- */

CF_TEST(settings_of_null_invalid_and_raw_columns) {
    account_env env;
    env_open(&env);

    /* A NULL settings column keeps NULL distinct from the merged default. */
    set_raw_row(&env, "NULL");
    cf_account account = {0};
    CF_REQUIRE(first_into(&env, &account) == CF_OK);
    CF_CHECK(account.settings_json.present == false);
    CF_CHECK(account.custom_styles.present == false);
    CF_CHECK(account.created_at == RAW_FRAC_TIME_US);
    CF_CHECK(account.updated_at == RAW_ZERO_TIME_US);

    cf_account_settings settings = {0};
    CF_REQUIRE(cf_account_settings_of(&account, &settings) == CF_OK);
    CF_CHECK(str_is(settings.json, DEFAULT_SETTINGS));
    CF_CHECK(!cf_account_settings_restrict_room_creation_to_administrators(
        &settings));
    cf_account_settings_dispose(&settings);
    cf_account_dispose(&account);

    /* Unknown members survive, in parse order, with the default appended. */
    set_raw_row(&env, "'{\"n\":6005}'");
    CF_REQUIRE(first_into(&env, &account) == CF_OK);
    CF_REQUIRE(cf_account_settings_of(&account, &settings) == CF_OK);
    CF_CHECK(str_is(settings.json,
                    "{\"n\":6005,\"restrict_room_creation_to_administrators\":false}"));
    cf_str value = {0};
    CF_CHECK(cf_account_settings_get(&settings, CF_STR_LIT("n"), &value));
    CF_CHECK(str_is(value, "6005"));
    free_str(&value);
    CF_CHECK(
        !cf_account_settings_get(&settings, CF_STR_LIT("missing"), &value));
    CF_CHECK(value.ptr == NULL && value.len == 0);
    cf_account_settings_dispose(&settings);
    cf_account_dispose(&account);

    /* true / null / non-blank string / blank string values. */
    struct {
        const char *json_sql;
        bool restricted;
        const char *to_json;
    } cases[] = {
        {"'{\"restrict_room_creation_to_administrators\":true}'", true,
         SETTINGS_TRUE},
        {"'{\"restrict_room_creation_to_administrators\":null}'", false,
         SETTINGS_NULL},
        {"'{\"restrict_room_creation_to_administrators\":\"false\"}'", true,
         "{\"restrict_room_creation_to_administrators\":\"false\"}"},
        {"'{\"restrict_room_creation_to_administrators\":\"\"}'", false,
         "{\"restrict_room_creation_to_administrators\":\"\"}"},
        {"'{\"restrict_room_creation_to_administrators\":\"  \"}'", false,
         "{\"restrict_room_creation_to_administrators\":\"  \"}"},
        {"'{\"restrict_room_creation_to_administrators\":\" x \"}'", true,
         "{\"restrict_room_creation_to_administrators\":\" x \"}"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        set_raw_row(&env, cases[i].json_sql);
        CF_REQUIRE(first_into(&env, &account) == CF_OK);
        CF_REQUIRE(cf_account_settings_of(&account, &settings) == CF_OK);
        CF_CHECK(cf_account_settings_restrict_room_creation_to_administrators(
                     &settings) == cases[i].restricted);
        cf_str json = {0};
        CF_REQUIRE(cf_account_settings_to_json(&settings, &json) == CF_OK);
        CF_CHECK(str_is(json, cases[i].to_json));
        free_str(&json);
        cf_account_settings_dispose(&settings);
        cf_account_dispose(&account);
    }

    /* Object values serialize back as JSON text through get. */
    set_raw_row(&env, "'{\"a\":{\"x\":1},\"b\":[true,false]}'");
    CF_REQUIRE(first_into(&env, &account) == CF_OK);
    CF_REQUIRE(cf_account_settings_of(&account, &settings) == CF_OK);
    CF_CHECK(cf_account_settings_get(&settings, CF_STR_LIT("a"), &value));
    CF_CHECK(str_is(value, "{\"x\":1}"));
    free_str(&value);
    CF_CHECK(cf_account_settings_get(&settings, CF_STR_LIT("b"), &value));
    CF_CHECK(str_is(value, "[true,false]"));
    free_str(&value);
    cf_account_settings_dispose(&settings);
    cf_account_dispose(&account);

    /* Duplicate keys collapse to the first position with the last value. */
    set_raw_row(&env,
                "'{\"restrict_room_creation_to_administrators\":true,"
                "\"restrict_room_creation_to_administrators\":false}'");
    CF_REQUIRE(first_into(&env, &account) == CF_OK);
    CF_REQUIRE(cf_account_settings_of(&account, &settings) == CF_OK);
    CF_CHECK(!cf_account_settings_restrict_room_creation_to_administrators(
        &settings));
    cf_str merged = {0};
    CF_REQUIRE(cf_account_settings_to_json(&settings, &merged) == CF_OK);
    CF_CHECK(str_is(merged, SETTINGS_FALSE));
    free_str(&merged);
    cf_account_settings_dispose(&settings);
    cf_account_dispose(&account);

    /* Invalid JSON and non-object roots fall back to the merged default. */
    const char *invalid[] = {"'not json'", "'[1,2,3]'", "''", "'7'"};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        set_raw_row(&env, invalid[i]);
        CF_REQUIRE(first_into(&env, &account) == CF_OK);
        CF_REQUIRE(cf_account_settings_of(&account, &settings) == CF_OK);
        CF_CHECK(str_is(settings.json, DEFAULT_SETTINGS));
        cf_account_settings_dispose(&settings);
        cf_account_dispose(&account);
    }

    env_close(&env);
}

CF_TEST(settings_set_get_to_json) {
    account_env env;
    env_open(&env);
    cf_test_clock_set_fixed_us(FIXED_NOW);
    cf_test_random_fill(0);

    cf_account account = {0};
    CF_REQUIRE(run_create(&env, "Chat", &account, NULL, 0) == CF_OK);
    cf_account_settings settings = {0};
    CF_REQUIRE(cf_account_settings_of(&account, &settings) == CF_OK);

    CF_REQUIRE(cf_account_settings_set_restrict_room_creation_to_administrators(
                   &settings, CF_STR_LIT("true")) == CF_OK);
    CF_CHECK(
        cf_account_settings_restrict_room_creation_to_administrators(&settings));
    cf_str json = {0};
    CF_REQUIRE(cf_account_settings_to_json(&settings, &json) == CF_OK);
    CF_CHECK(str_is(json, SETTINGS_TRUE));
    free_str(&json);

    cf_str value = {0};
    CF_REQUIRE(
        cf_account_settings_get(&settings, CF_STR_LIT(SETTINGS_KEY), &value));
    CF_CHECK(str_is(value, "true"));
    free_str(&value);

    CF_REQUIRE(cf_account_settings_set_restrict_room_creation_to_administrators(
                   &settings, CF_STR_LIT("false")) == CF_OK);
    CF_CHECK(!cf_account_settings_restrict_room_creation_to_administrators(
        &settings));
    CF_REQUIRE(cf_account_settings_to_json(&settings, &json) == CF_OK);
    CF_CHECK(str_is(json, SETTINGS_FALSE));
    free_str(&json);

    /* Blank casts to None -> JSON null, and present() reads false. */
    CF_REQUIRE(cf_account_settings_set_restrict_room_creation_to_administrators(
                   &settings, CF_STR_LIT("")) == CF_OK);
    CF_CHECK(!cf_account_settings_restrict_room_creation_to_administrators(
        &settings));
    CF_REQUIRE(
        cf_account_settings_get(&settings, CF_STR_LIT(SETTINGS_KEY), &value));
    CF_CHECK(str_is(value, "null"));
    free_str(&value);
    CF_REQUIRE(cf_account_settings_to_json(&settings, &json) == CF_OK);
    CF_CHECK(str_is(json, SETTINGS_NULL));
    free_str(&json);

    /* "off" is a FALSE_VALUE, "on" is not. */
    CF_REQUIRE(cf_account_settings_set_restrict_room_creation_to_administrators(
                   &settings, CF_STR_LIT("off")) == CF_OK);
    CF_CHECK(!cf_account_settings_restrict_room_creation_to_administrators(
        &settings));
    CF_REQUIRE(cf_account_settings_set_restrict_room_creation_to_administrators(
                   &settings, CF_STR_LIT("on")) == CF_OK);
    CF_CHECK(
        cf_account_settings_restrict_room_creation_to_administrators(&settings));

    /* Zero-state input behaves as an empty object, not an error. */
    cf_account_settings zero = {0};
    CF_REQUIRE(cf_account_settings_to_json(&zero, &json) == CF_OK);
    CF_CHECK(str_is(json, "{}"));
    free_str(&json);
    CF_REQUIRE(cf_account_settings_set_restrict_room_creation_to_administrators(
                   &zero, CF_STR_LIT("1")) == CF_OK);
    CF_CHECK(
        cf_account_settings_restrict_room_creation_to_administrators(&zero));
    cf_account_settings_dispose(&zero);

    cf_account_settings_dispose(&settings);
    cf_account_dispose(&account);
    cf_test_random_clear();
    cf_test_clock_clear();
    env_close(&env);
}

CF_TEST(cast_boolean_values) {
    static const struct {
        const char *text;
        bool blank;
        bool value;
    } cases[] = {
        {"", true, false},       {"0", false, false},
        {"f", false, false},     {"F", false, false},
        {"false", false, false}, {"FALSE", false, false},
        {"off", false, false},   {"OFF", false, false},
        {"1", false, true},      {"true", false, true},
        {"t", false, true},      {"yes", false, true},
        {" false", false, true}, {"FALSE ", false, true},
        {"oFF", false, true},    {"on", false, true},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        bool out = true; /* sentinel: blank must leave it untouched */
        bool present =
            cf_account_cast_boolean(opt_some(cases[i].text).value, &out);
        CF_CHECK(present == !cases[i].blank);
        if (present) {
            CF_CHECK(out == cases[i].value);
        } else {
            CF_CHECK(out == true);
        }
    }
    bool out = false;
    CF_CHECK(cf_account_cast_boolean((cf_str){NULL, 0}, &out) == false);
    CF_CHECK(out == false);
    CF_CHECK(cf_account_cast_boolean((cf_str){NULL, 3}, &out) == false);
}

CF_TEST(settings_assign_ordinary_and_unknown_key) {
    account_env env;
    env_open(&env);
    cf_test_clock_set_fixed_us(FIXED_NOW);
    cf_test_random_fill(0);

    cf_account account = {0};
    CF_REQUIRE(run_create(&env, "Chat", &account, NULL, 0) == CF_OK);
    cf_account_settings settings = {0};
    CF_REQUIRE(cf_account_settings_of(&account, &settings) == CF_OK);

    cf_account_setting ordinary[] = {
        {CF_STR_LIT(SETTINGS_KEY), CF_STR_LIT("true")}};
    cf_model_errors errors = {0};
    CF_REQUIRE(cf_account_settings_assign(&settings, ordinary, 1, &errors) ==
               CF_OK);
    CF_CHECK(errors.len == 0);
    CF_CHECK(
        cf_account_settings_restrict_room_creation_to_administrators(&settings));
    cf_str json = {0};
    CF_REQUIRE(cf_account_settings_to_json(&settings, &json) == CF_OK);
    CF_CHECK(str_is(json, SETTINGS_TRUE));
    free_str(&json);

    /* Prior assignments stay applied when a later key is unknown (the
     * source's &mut assign), and out_errors carries its message. */
    cf_account_setting mixed[] = {
        {CF_STR_LIT(SETTINGS_KEY), CF_STR_LIT("off")},
        {CF_STR_LIT("nope"), CF_STR_LIT("1")}};
    CF_CHECK(cf_account_settings_assign(&settings, mixed, 2, &errors) ==
             CF_INVALID);
    CF_REQUIRE(errors.len == 1);
    CF_CHECK(str_is(errors.items[0].field, "nope"));
    CF_CHECK(str_is(errors.items[0].message,
                    "undefined method 'nope=' for account settings"));
    CF_CHECK(!cf_account_settings_restrict_room_creation_to_administrators(
        &settings));
    CF_REQUIRE(cf_account_settings_to_json(&settings, &json) == CF_OK);
    CF_CHECK(str_is(json, SETTINGS_FALSE));
    free_str(&json);
    free_errors(&errors);

    cf_account_setting unknown[] = {{CF_STR_LIT("other"), CF_STR_LIT("x")}};
    CF_CHECK(cf_account_settings_assign(&settings, unknown, 1, &errors) ==
             CF_INVALID);
    CF_REQUIRE(errors.len == 1);
    CF_CHECK(str_is(errors.items[0].message,
                    "undefined method 'other=' for account settings"));
    CF_CHECK(strcmp(cf_db_last_error(),
                    "undefined method 'other=' for account settings") == 0);
    free_errors(&errors);

    /* out_errors may be NULL; the failure and its message still surface. */
    CF_CHECK(cf_account_settings_assign(&settings, unknown, 1, NULL) ==
             CF_INVALID);
    CF_CHECK(strcmp(cf_db_last_error(),
                    "undefined method 'other=' for account settings") == 0);

    cf_account_settings_dispose(&settings);
    cf_account_dispose(&account);
    cf_test_random_clear();
    cf_test_clock_clear();
    env_close(&env);
}

/* --- update / reset_join_code / reload ----------------------------------- */

CF_TEST(update_name_styles_and_no_touch) {
    account_env env;
    env_open(&env);
    cf_test_clock_set_fixed_us(FIXED_NOW);
    cf_test_random_fill(0);

    cf_account account = {0};
    CF_REQUIRE(run_create(&env, "Chat", &account, NULL, 0) == CF_OK);

    cf_test_clock_set_fixed_us(FIXED_LATER);
    CF_REQUIRE(run_update(&env, &account, opt_some("X"), NULL, NULL, 0, NULL,
                          0) == CF_OK);
    CF_CHECK(str_is(account.name, "X"));
    CF_CHECK(account.updated_at == FIXED_LATER);

    cf_account stored = {0};
    CF_REQUIRE(first_into(&env, &stored) == CF_OK);
    CF_CHECK(str_is(stored.name, "X"));
    CF_CHECK(stored.updated_at == FIXED_LATER);
    CF_REQUIRE(stored.settings_json.present);
    CF_CHECK(str_is(stored.settings_json.value, DEFAULT_SETTINGS));
    cf_account_dispose(&stored);

    /* Same name: no write, no touch. */
    cf_test_clock_set_fixed_us(FIXED_LATER + 5);
    CF_REQUIRE(run_update(&env, &account, opt_some("X"), NULL, NULL, 0, NULL,
                          0) == CF_OK);
    CF_CHECK(account.updated_at == FIXED_LATER);

    /* custom_styles Some(Some(text)), then Some(None) -> SQL NULL. */
    cf_optional_str styles = opt_some("/* css */");
    CF_REQUIRE(run_update(&env, &account, opt_none(), &styles, NULL, 0, NULL,
                          0) == CF_OK);
    CF_CHECK(account.custom_styles.present &&
             str_is(account.custom_styles.value, "/* css */"));
    CF_REQUIRE(reload_first(&env, &account) == CF_OK);
    CF_CHECK(account.custom_styles.present &&
             str_is(account.custom_styles.value, "/* css */"));

    /* Re-setting the identical styles is not a change: no write, no touch. */
    int64_t after_styles = account.updated_at;
    CF_REQUIRE(run_update(&env, &account, opt_none(), &styles, NULL, 0, NULL,
                          0) == CF_OK);
    CF_CHECK(account.updated_at == after_styles);

    cf_optional_str clear = opt_none();
    CF_REQUIRE(run_update(&env, &account, opt_none(), &clear, NULL, 0, NULL,
                          0) == CF_OK);
    CF_CHECK(account.custom_styles.present == false);
    CF_REQUIRE(reload_first(&env, &account) == CF_OK);
    CF_CHECK(account.custom_styles.present == false);

    /* Empty text is present-but-empty, distinct from SQL NULL. */
    cf_optional_str empty = {true, {NULL, 0}};
    CF_REQUIRE(run_update(&env, &account, opt_none(), &empty, NULL, 0, NULL,
                          0) == CF_OK);
    CF_CHECK(account.custom_styles.present &&
             account.custom_styles.value.len == 0);
    CF_REQUIRE(reload_first(&env, &account) == CF_OK);
    CF_CHECK(account.custom_styles.present &&
             account.custom_styles.value.len == 0);

    cf_account_dispose(&account);
    cf_test_random_clear();
    cf_test_clock_clear();
    env_close(&env);
}

CF_TEST(update_settings_from_null_column) {
    account_env env;
    env_open(&env);
    cf_test_clock_set_fixed_us(FIXED_NOW);
    cf_test_random_fill(0);

    cf_account account = {0};
    CF_REQUIRE(run_create(&env, "Chat", &account, NULL, 0) == CF_OK);
    set_raw_row(&env, "NULL");
    CF_REQUIRE(reload_first(&env, &account) == CF_OK);
    CF_CHECK(account.settings_json.present == false);

    /* update!(name:) on a NULL settings column leaves settings alone. */
    CF_REQUIRE(run_update(&env, &account, opt_some("X"), NULL, NULL, 0, NULL,
                          0) == CF_OK);
    CF_REQUIRE(reload_first(&env, &account) == CF_OK);
    CF_CHECK(str_is(account.name, "X"));
    CF_CHECK(account.settings_json.present == false);

    cf_account_setting values[] = {
        {CF_STR_LIT(SETTINGS_KEY), CF_STR_LIT("true")}};
    CF_REQUIRE(run_update(&env, &account, opt_none(), NULL, values, 1, NULL,
                          0) == CF_OK);
    CF_REQUIRE(account.settings_json.present);
    CF_CHECK(str_is(account.settings_json.value, SETTINGS_TRUE));
    CF_REQUIRE(reload_first(&env, &account) == CF_OK);
    CF_REQUIRE(account.settings_json.present);
    CF_CHECK(str_is(account.settings_json.value, SETTINGS_TRUE));
    cf_account_settings settings = {0};
    CF_REQUIRE(cf_account_settings_of(&account, &settings) == CF_OK);
    CF_CHECK(
        cf_account_settings_restrict_room_creation_to_administrators(&settings));
    cf_account_settings_dispose(&settings);

    cf_account_setting values_false[] = {
        {CF_STR_LIT(SETTINGS_KEY), CF_STR_LIT("false")}};
    CF_REQUIRE(run_update(&env, &account, opt_none(), NULL, values_false, 1,
                          NULL, 0) == CF_OK);
    CF_REQUIRE(reload_first(&env, &account) == CF_OK);
    CF_REQUIRE(account.settings_json.present);
    CF_CHECK(str_is(account.settings_json.value, SETTINGS_FALSE));
    CF_REQUIRE(cf_account_settings_of(&account, &settings) == CF_OK);
    CF_CHECK(!cf_account_settings_restrict_room_creation_to_administrators(
        &settings));
    cf_account_settings_dispose(&settings);

    cf_account_dispose(&account);
    cf_test_random_clear();
    cf_test_clock_clear();
    env_close(&env);
}

CF_TEST(update_settings_equal_value_no_touch) {
    account_env env;
    env_open(&env);
    set_raw_row(&env, "'" SETTINGS_TRUE "'");

    cf_account account = {0};
    CF_REQUIRE(first_into(&env, &account) == CF_OK);
    CF_CHECK(account.updated_at == RAW_ZERO_TIME_US);

    cf_test_clock_set_fixed_us(FIXED_LATER);
    cf_account_setting values[] = {
        {CF_STR_LIT(SETTINGS_KEY), CF_STR_LIT("true")}};
    CF_REQUIRE(run_update(&env, &account, opt_none(), NULL, values, 1, NULL,
                          0) == CF_OK);
    CF_REQUIRE(account.settings_json.present);
    CF_CHECK(str_is(account.settings_json.value, SETTINGS_TRUE));
    CF_CHECK(account.updated_at == RAW_ZERO_TIME_US);

    cf_account stored = {0};
    CF_REQUIRE(first_into(&env, &stored) == CF_OK);
    CF_CHECK(stored.updated_at == RAW_ZERO_TIME_US);
    CF_REQUIRE(stored.settings_json.present);
    CF_CHECK(str_is(stored.settings_json.value, SETTINGS_TRUE));
    cf_account_dispose(&stored);

    cf_account_dispose(&account);
    cf_test_clock_clear();
    env_close(&env);
}

CF_TEST(update_rejected_settings_write_nothing) {
    account_env env;
    env_open(&env);
    cf_test_clock_set_fixed_us(FIXED_NOW);
    cf_test_random_fill(0);

    cf_account account = {0};
    CF_REQUIRE(run_create(&env, "Chat", &account, NULL, 0) == CF_OK);

    cf_test_clock_set_fixed_us(FIXED_LATER);
    cf_account_setting bad[] = {{CF_STR_LIT("nope"), CF_STR_LIT("1")}};
    char message[160] = {0};
    CF_CHECK(run_update(&env, &account, opt_some("X"), NULL, bad, 1, message,
                        sizeof message) == CF_INVALID);
    /* The source mutates self.name before assign fails; the row is untouched. */
    CF_CHECK(str_is(account.name, "X"));
    CF_CHECK(strcmp(message,
                    "undefined method 'nope=' for account settings") == 0);

    cf_account stored = {0};
    CF_REQUIRE(first_into(&env, &stored) == CF_OK);
    CF_CHECK(str_is(stored.name, "Chat"));
    CF_CHECK(stored.updated_at == FIXED_NOW);
    CF_REQUIRE(stored.settings_json.present);
    CF_CHECK(str_is(stored.settings_json.value, DEFAULT_SETTINGS));
    cf_account_dispose(&stored);

    cf_account_dispose(&account);
    cf_test_random_clear();
    cf_test_clock_clear();
    env_close(&env);
}

CF_TEST(update_combined_changes_single_statement) {
    account_env env;
    env_open(&env);
    cf_test_clock_set_fixed_us(FIXED_NOW);
    cf_test_random_fill(0);

    cf_account account = {0};
    CF_REQUIRE(run_create(&env, "Chat", &account, NULL, 0) == CF_OK);

    /* name + custom_styles + settings in one call (mask 7). */
    cf_test_clock_set_fixed_us(FIXED_LATER);
    cf_optional_str styles = opt_some("s");
    cf_account_setting values[] = {
        {CF_STR_LIT(SETTINGS_KEY), CF_STR_LIT("true")}};
    CF_REQUIRE(run_update(&env, &account, opt_some("Y"), &styles, values, 1,
                          NULL, 0) == CF_OK);
    CF_CHECK(str_is(account.name, "Y"));
    CF_CHECK(account.custom_styles.present &&
             str_is(account.custom_styles.value, "s"));
    CF_REQUIRE(account.settings_json.present);
    CF_CHECK(str_is(account.settings_json.value, SETTINGS_TRUE));
    CF_CHECK(account.updated_at == FIXED_LATER);

    cf_account stored = {0};
    CF_REQUIRE(first_into(&env, &stored) == CF_OK);
    CF_CHECK(str_is(stored.name, "Y"));
    CF_CHECK(stored.custom_styles.present &&
             str_is(stored.custom_styles.value, "s"));
    CF_REQUIRE(stored.settings_json.present);
    CF_CHECK(str_is(stored.settings_json.value, SETTINGS_TRUE));
    CF_CHECK(stored.updated_at == FIXED_LATER);
    cf_account_dispose(&stored);

    /* name + custom_styles NULL (mask 3). */
    cf_optional_str clear = opt_none();
    CF_REQUIRE(run_update(&env, &account, opt_some("Z"), &clear, NULL, 0, NULL,
                          0) == CF_OK);
    CF_REQUIRE(reload_first(&env, &account) == CF_OK);
    CF_CHECK(str_is(account.name, "Z"));
    CF_CHECK(account.custom_styles.present == false);

    /* name + settings (mask 5). */
    cf_account_setting values_false[] = {
        {CF_STR_LIT(SETTINGS_KEY), CF_STR_LIT("false")}};
    CF_REQUIRE(run_update(&env, &account, opt_some("W"), NULL, values_false, 1,
                          NULL, 0) == CF_OK);
    CF_REQUIRE(reload_first(&env, &account) == CF_OK);
    CF_CHECK(str_is(account.name, "W"));
    CF_REQUIRE(account.settings_json.present);
    CF_CHECK(str_is(account.settings_json.value, SETTINGS_FALSE));

    /* settings only (mask 4). */
    CF_REQUIRE(run_update(&env, &account, opt_none(), NULL, values, 1, NULL,
                          0) == CF_OK);
    CF_REQUIRE(reload_first(&env, &account) == CF_OK);
    CF_CHECK(str_is(account.settings_json.value, SETTINGS_TRUE));
    CF_CHECK(str_is(account.name, "W"));

    cf_account_dispose(&account);
    cf_test_random_clear();
    cf_test_clock_clear();
    env_close(&env);
}

CF_TEST(reset_join_code_updates_row_and_record) {
    account_env env;
    env_open(&env);
    cf_test_clock_set_fixed_us(FIXED_NOW);
    cf_test_random_fill(0);

    cf_account account = {0};
    CF_REQUIRE(run_create(&env, "Chat", &account, NULL, 0) == CF_OK);
    CF_CHECK(str_is(account.join_code, "AAAA-AAAA-AAAA"));

    cf_test_clock_set_fixed_us(FIXED_LATER);
    cf_test_random_fill(1);
    CF_REQUIRE(run_reset(&env, &account) == CF_OK);
    CF_CHECK(str_is(account.join_code, "BBBB-BBBB-BBBB"));
    CF_CHECK(account.updated_at == FIXED_LATER);
    CF_CHECK(account.created_at == FIXED_NOW);

    cf_account stored = {0};
    CF_REQUIRE(first_into(&env, &stored) == CF_OK);
    CF_CHECK(str_is(stored.join_code, "BBBB-BBBB-BBBB"));
    CF_CHECK(stored.updated_at == FIXED_LATER);
    cf_account_dispose(&stored);

    cf_account_dispose(&account);
    cf_test_random_clear();
    cf_test_clock_clear();
    env_close(&env);
}

CF_TEST(reload_reflects_database_and_deleted_row) {
    account_env env;
    env_open(&env);
    cf_test_clock_set_fixed_us(FIXED_NOW);
    cf_test_random_fill(0);

    cf_account account = {0};
    CF_REQUIRE(run_create(&env, "Chat", &account, NULL, 0) == CF_OK);
    int64_t id = account.id;

    char sql[128];
    snprintf(sql, sizeof sql,
             "UPDATE \"accounts\" SET \"name\" = 'Db' WHERE \"id\" = %lld",
             (long long)id);
    CF_REQUIRE(exec_sql(&env, sql) == SQLITE_OK);
    CF_REQUIRE(cf_account_reload(env.db, &account) == CF_OK);
    CF_CHECK(account.id == id);
    CF_CHECK(str_is(account.name, "Db"));

    CF_REQUIRE(exec_sql(&env, "DELETE FROM \"accounts\"") == SQLITE_OK);
    CF_CHECK(cf_account_reload(env.db, &account) == CF_NOT_FOUND);
    CF_CHECK(account.id == id);
    CF_CHECK(str_is(account.name, "Db"));

    cf_account_dispose(&account);
    cf_test_random_clear();
    cf_test_clock_clear();
    env_close(&env);
}

/* --- generate_join_code -------------------------------------------------- */

CF_TEST(generate_join_code_shape_and_failures) {
    cf_str code = {0};

    cf_test_random_fill(0);
    CF_REQUIRE(cf_account_generate_join_code(&code) == CF_OK);
    CF_CHECK(str_is(code, "AAAA-AAAA-AAAA"));
    CF_CHECK(code.len == 14);
    free_str(&code);

    cf_test_random_fill(247); /* largest accepted byte: 247 % 62 == 61 */
    CF_REQUIRE(cf_account_generate_join_code(&code) == CF_OK);
    CF_CHECK(str_is(code, "9999-9999-9999"));
    free_str(&code);

    cf_test_random_fill(248); /* first rejected byte -> bounded fallback */
    CF_REQUIRE(cf_account_generate_join_code(&code) == CF_OK);
    CF_CHECK(str_is(code, "AAAA-AAAA-AAAA"));
    free_str(&code);

    cf_test_random_fill(255); /* 255 % 62 == 7 -> 'H' */
    CF_REQUIRE(cf_account_generate_join_code(&code) == CF_OK);
    CF_CHECK(str_is(code, "HHHH-HHHH-HHHH"));
    free_str(&code);

    cf_test_random_fail();
    CF_CHECK(cf_account_generate_join_code(&code) == CF_IO);
    CF_CHECK(code.ptr == NULL && code.len == 0);
    cf_test_random_clear();

    /* Shape with the real entropy source. */
    CF_REQUIRE(cf_account_generate_join_code(&code) == CF_OK);
    CF_REQUIRE(code.len == 14);
    CF_CHECK(code.ptr[4] == '-' && code.ptr[9] == '-');
    for (size_t i = 0; i < code.len; i++) {
        char c = code.ptr[i];
        if (i == 4 || i == 9) continue;
        CF_CHECK((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9'));
    }
    free_str(&code);
}

/* --- disposal edges ------------------------------------------------------ */

CF_TEST(dispose_zero_and_partial_states) {
    cf_account_dispose(NULL);
    cf_account zero = {0};
    cf_account_dispose(&zero);
    cf_account_dispose(&zero); /* idempotent after reset */

    cf_account_settings_dispose(NULL);
    cf_account_settings empty_settings = {0};
    cf_account_settings_dispose(&empty_settings);

    cf_account_vector_dispose(NULL);
    cf_account_vector empty_vector = {0};
    cf_account_vector_dispose(&empty_vector);

    /* A populated vector owns its copied elements. */
    cf_account_vector vector = {0};
    vector.items = malloc(2 * sizeof *vector.items);
    CF_REQUIRE(vector.items != NULL);
    memset(vector.items, 0, 2 * sizeof *vector.items);
    vector.len = 2;
    vector.cap = 2;
    vector.items[0].id = 1;
    vector.items[0].name.ptr = strdup("one");
    vector.items[0].name.len = 3;
    vector.items[1].id = 2;
    vector.items[1].settings_json.present = true;
    vector.items[1].settings_json.value.ptr = strdup(DEFAULT_SETTINGS);
    vector.items[1].settings_json.value.len = strlen(DEFAULT_SETTINGS);
    cf_account_vector_dispose(&vector);
    cf_account_vector_dispose(&vector);

    /* A settings value built by the model disposes safely. */
    cf_account_settings settings = {0};
    settings.json.ptr = strdup(SETTINGS_TRUE);
    settings.json.len = strlen(SETTINGS_TRUE);
    cf_account_settings_dispose(&settings);
    cf_account_settings_dispose(&settings);
}

CF_TEST_MAIN()
