/* tests/models/search_test.c — D01 model family "search" tests.
 *
 * Source: tmp/rust-ref/crates/db/src/models/search.rs (5 inventoried
 * functions).  Oracle: tests/fixtures/crates/db/src/tests/callbacks_test.rs
 * `recording_searches_keeps_the_ten_most_recent` (the ported reference test:
 * after twelve Search::record calls only the ten most recent remain, the
 * first listed is the last recorded, a stale fixture query is gone, and
 * re-recording an older query touches it back to the front), plus the pinned
 * source SQL and the fresh-schema contract.
 *
 * cf_tx is created only by D02's writer, so every mutation goes through the
 * public cf_write path into a scratch DATABASE_PATH (the same shape as
 * tests/models/first_run_test.c); committed rows are read through a separate
 * read-only cf_db.  The reference TestDb loads fixture rows from
 * reference/test/fixtures, which F02 did not copy into this checkout; the
 * cases below build the same starting state with deterministic raw rows (one
 * stale "pizza" search) and the F01 fixed test clock, so no sleeps and no
 * fixture dependency.  Raw SQLite handles in this file are test-only setup
 * and inspection, never the application surface under test.
 *
 * Every implemented function has an ordinary path and an edge/failure path:
 *   cf_search_ordered_for_user   order by updated_at DESC, user isolation,
 *                                empty result, parsed datetime, NULL out/db
 *   cf_search_count              zero/after-insert, NULL out/db
 *   cf_search_count_for_user     per-user split, unknown user, NULL out/db
 *   cf_search_record             create, exact (user, query) find + touch,
 *                                trim to the ten most recent, empty and
 *                                literal-metacharacter queries, FK rollback,
 *                                invalid span, NULL tx/out
 *   cf_search_destroy_all_for_user  user isolation, idempotent, unknown user,
 *                                NULL tx
 *   dispose helpers              NULL/empty/partially built state, leak-free
 *
 * Build (plain; add -fsanitize=address,undefined -fno-sanitize-recover=all
 * -fno-omit-frame-pointer -g for the sanitize run):
 *   clang -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra
 *         -Werror -pthread -Isrc -Itests
 *         -Ivendor/src/sqlite/sqlite-amalgamation-3530400
 *         tests/models/search_test.c src/models/search.c
 *         src/db/{schema,reader,statements,writer}.c
 *         src/core/{alloc,buffer,clock}.c src/config.c src/app.c
 *         vendor/build/sqlite-clang/libsqlite3.a -lm
 *         -o build/d01-search/test_search
 */
#include "cf_test.h"

#include "app.h"
#include "cf.h"
#include "config.h"
#include "core/testclock.h"
#include "db/db_internal.h"
#include "db/writer.h"
#include "models/search.h"

#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The shared release helpers (cf_str_dispose / cf_optional_str_dispose) are
 * defined once in src/models/types.c. */

/* --- deterministic clock and borrowed literals ----------------------------- */

/* 2026-01-01 00:00:00 UTC, whole seconds (stored text has no fraction). */
#define T_BASE 1767225600000000LL
#define T_SEC 1000000LL
#define SEARCH_SECRET_HEX \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static cf_str text(const char *bytes) {
    cf_str value = {(char *)bytes, strlen(bytes)};
    return value;
}

static void check_str(cf_str value, const char *expected) {
    size_t len = strlen(expected);
    if (value.len != len || value.ptr == NULL ||
        (len != 0 && memcmp(value.ptr, expected, len) != 0) ||
        value.ptr[value.len] != '\0') {
        CF_CHECK(!"text mismatch");
        printf("    got \"%.*s\" (len %zu), expected \"%s\"\n",
               (int)value.len, value.ptr != NULL ? value.ptr : "", value.len,
               expected);
    }
}

static void check_empty_record(const cf_search *search) {
    CF_CHECK(search->id == 0);
    CF_CHECK(search->user_id == 0);
    CF_CHECK(search->created_at == 0);
    CF_CHECK(search->updated_at == 0);
    CF_CHECK(search->query.ptr == NULL);
    CF_CHECK(search->query.len == 0);
}

/* --- scratch app, writer and reader --------------------------------------- */

typedef struct {
    char *dir;
    char path[512];
    cf_app *app;
    cf_db *reader;   /* read-only; committed rows only */
    sqlite3 *raw;    /* test-only setup/inspection on the same file */
} search_env;

static void search_env_close(search_env *env);

static bool search_env_open(search_env *env) {
    memset(env, 0, sizeof *env);
    char template[] = "/tmp/cf_search_XXXXXX";
    char *dir = mkdtemp(template);
    if (dir == NULL) return false;
    env->dir = strdup(dir);
    if (env->dir == NULL) {
        rmdir(dir);
        return false;
    }
    snprintf(env->path, sizeof env->path, "%s/test.sqlite3", dir);

    cf_config_entry entries[] = {
        {"PUBLIC_ORIGIN", "http://127.0.0.1:32123"},
        {"SECRET_KEY_BASE", SEARCH_SECRET_HEX},
        {"DATABASE_PATH", env->path},
    };
    cf_config *config = NULL;
    if (cf_config_parse(entries, sizeof entries / sizeof entries[0], NULL,
                        &config) != CF_OK) {
        search_env_close(env);
        return false;
    }
    if (cf_app_create(config, &env->app) != CF_OK) {
        cf_config_destroy(config);
        search_env_close(env);
        return false;
    }
    /* cf_app_create consumed config: cf_app_destroy releases it. */
    cf_err rc = cf_writer_start(env->app, config);
    if (rc != CF_OK) {
        search_env_close(env);
        return false;
    }
    if (cf_db_open(env->path, true, &env->reader) != CF_OK) {
        search_env_close(env);
        return false;
    }
    if (sqlite3_open_v2(env->path, &env->raw, SQLITE_OPEN_READWRITE, NULL) !=
        SQLITE_OK) {
        search_env_close(env);
        return false;
    }
    sqlite3_busy_timeout(env->raw, 1000);
    return true;
}

static void search_env_close(search_env *env) {
    if (env->raw != NULL) {
        sqlite3_close(env->raw);
        env->raw = NULL;
    }
    cf_db_close(env->reader);
    env->reader = NULL;
    if (env->app != NULL) {
        cf_writer_stop(env->app);
        cf_app_destroy(env->app);
        env->app = NULL;
    }
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

/* --- mutation wrappers (public cf_write path) ------------------------------ */

struct record_call {
    int64_t user_id;
    cf_str query; /* borrowed for the call */
    cf_search out;
    cf_err rc;
};

static cf_err record_cb(cf_tx *tx, void *arg) {
    struct record_call *call = arg;
    call->rc = cf_search_record(tx, call->user_id, call->query, &call->out);
    return call->rc;
}

/* On failure the model contract leaves *out empty; dispose defensively so a
 * leaked value surfaces as the failed emptiness check rather than as a test
 * leak. */
static cf_err record_run(search_env *env, int64_t user_id, cf_str query,
                         cf_search *out) {
    struct record_call call;
    memset(&call, 0, sizeof call);
    call.user_id = user_id;
    call.query = query;
    cf_err rc = cf_write(env->app, record_cb, &call);
    if (rc == CF_OK) {
        *out = call.out; /* ownership moves to the caller */
        return CF_OK;
    }
    memset(out, 0, sizeof *out);
    check_empty_record(&call.out);
    cf_search_dispose(&call.out);
    return rc;
}

struct destroy_call {
    int64_t user_id;
    cf_err rc;
};

static cf_err destroy_cb(cf_tx *tx, void *arg) {
    struct destroy_call *call = arg;
    call->rc = cf_search_destroy_all_for_user(tx, call->user_id);
    return call->rc;
}

static cf_err destroy_run(search_env *env, int64_t user_id) {
    struct destroy_call call = {user_id, CF_OK};
    return cf_write(env->app, destroy_cb, &call);
}

/* --- raw SQL setup and inspection (test-only) ------------------------------ */

static int64_t insert_user(search_env *env, const char *name) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        env->raw,
        "INSERT INTO users (name, created_at, updated_at) "
        "VALUES (?1, '2026-01-01 00:00:00', '2026-01-01 00:00:00')",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    int64_t id = -1;
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        id = sqlite3_last_insert_rowid(env->raw);
    }
    sqlite3_finalize(stmt);
    return id;
}

/* Raw searches insert for setup that does not go through record(): ids and
 * both datetime texts are chosen by the caller. */
static int64_t insert_search(search_env *env, int64_t user_id,
                             const char *query, const char *created_at,
                             const char *updated_at) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        env->raw,
        "INSERT INTO searches (created_at, query, updated_at, user_id) "
        "VALUES (?1, ?2, ?3, ?4)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, created_at, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, query, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, updated_at, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, user_id);
    int64_t id = -1;
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        id = sqlite3_last_insert_rowid(env->raw);
    }
    sqlite3_finalize(stmt);
    return id;
}

static int64_t raw_count(const search_env *env, const char *where_clause) {
    char sql[160];
    snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM searches%s", where_clause);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(env->raw, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    int64_t count = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

static bool raw_bound_text_is(const search_env *env, const char *sql,
                              int64_t id, const char *want) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(env->raw, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, id);
    bool match = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *bytes = sqlite3_column_text(stmt, 0);
        match = bytes != NULL && strcmp((const char *)bytes, want) == 0;
    }
    sqlite3_finalize(stmt);
    return match;
}

/* --- tests ----------------------------------------------------------------- */

CF_TEST(count_and_ordered_empty_database) {
    search_env env;
    CF_REQUIRE(search_env_open(&env));
    int64_t user = insert_user(&env, "empty-user");
    CF_REQUIRE(user > 0);

    int64_t count = -1;
    CF_REQUIRE(cf_search_count(env.reader, &count) == CF_OK);
    CF_CHECK(count == 0);
    CF_REQUIRE(cf_search_count_for_user(env.reader, user, &count) == CF_OK);
    CF_CHECK(count == 0);
    CF_REQUIRE(cf_search_count_for_user(env.reader, 999999, &count) == CF_OK);
    CF_CHECK(count == 0);

    cf_search_vector searches;
    CF_REQUIRE(cf_search_ordered_for_user(env.reader, user, &searches) ==
               CF_OK);
    CF_CHECK(searches.len == 0 && searches.items == NULL && searches.cap == 0);
    cf_search_vector_dispose(&searches); /* zero state is a no-op */
    cf_search_vector_dispose(NULL);

    CF_CHECK(cf_search_count(env.reader, NULL) == CF_INVALID);
    CF_CHECK(cf_search_count_for_user(env.reader, user, NULL) == CF_INVALID);
    CF_CHECK(cf_search_ordered_for_user(env.reader, user, NULL) == CF_INVALID);
    CF_CHECK(cf_search_count(NULL, &count) == CF_INVALID);
    CF_CHECK(cf_search_count_for_user(NULL, user, &count) == CF_INVALID);
    CF_CHECK(cf_search_ordered_for_user(NULL, user, &searches) == CF_INVALID);
    CF_CHECK(searches.len == 0);

    search_env_close(&env);
}

/* Port of recording_searches_keeps_the_ten_most_recent. */
CF_TEST(record_creates_touches_and_trims_to_ten) {
    search_env env;
    CF_REQUIRE(search_env_open(&env));
    int64_t user = insert_user(&env, "david");
    int64_t other = insert_user(&env, "other");
    CF_REQUIRE(user > 0 && other > 0);

    /* A stale saved search, like the fixture row the reference test outlives. */
    CF_REQUIRE(insert_search(&env, user, "pizza", "2025-12-31 00:00:00",
                             "2025-12-31 00:00:00") > 0);

    int64_t ids[12];
    for (int n = 0; n < 12; n++) {
        char query[32];
        snprintf(query, sizeof query, "query %d", n);
        int64_t now = T_BASE + (int64_t)(n + 1) * T_SEC;
        cf_test_clock_set_fixed_us(now);

        cf_search search;
        memset(&search, 0, sizeof search);
        CF_REQUIRE(record_run(&env, user, text(query), &search) == CF_OK);
        CF_CHECK(search.id > 0);
        CF_CHECK(search.user_id == user);
        check_str(search.query, query);
        CF_CHECK(search.created_at == now); /* fresh insert: both stamps now */
        CF_CHECK(search.updated_at == now);
        ids[n] = search.id;
        cf_search_dispose(&search);
    }

    /* The 12th creation trims to ten before its touch. */
    int64_t count = -1;
    CF_REQUIRE(cf_search_count_for_user(env.reader, user, &count) == CF_OK);
    CF_CHECK(count == 10);
    CF_REQUIRE(cf_search_count(env.reader, &count) == CF_OK);
    CF_CHECK(count == 10);
    CF_CHECK(raw_count(&env, "") == 10);
    CF_CHECK(raw_count(&env, " WHERE query = 'pizza'") == 0);

    cf_search_vector searches;
    CF_REQUIRE(cf_search_ordered_for_user(env.reader, user, &searches) ==
               CF_OK);
    CF_REQUIRE(searches.len == 10);
    CF_CHECK(searches.items[0].id == ids[11]);
    CF_CHECK(searches.items[9].id == ids[2]);
    for (size_t i = 0; i < searches.len; i++) {
        char query[32];
        snprintf(query, sizeof query, "query %d", 11 - (int)i);
        check_str(searches.items[i].query, query);
        CF_CHECK(searches.items[i].user_id == user);
        if (i != 0) {
            CF_CHECK(searches.items[i - 1].updated_at >
                      searches.items[i].updated_at);
        }
    }

    /* Whole-second clocks store without a fraction (reference datetime(6)).
     * ids[2] is the oldest surviving row: ids[0] and ids[1] were trimmed. */
    CF_CHECK(raw_bound_text_is(&env,
                               "SELECT created_at FROM searches WHERE id = ?",
                               ids[2], "2026-01-01 00:00:03"));
    CF_CHECK(raw_bound_text_is(&env,
                               "SELECT updated_at FROM searches WHERE id = ?",
                               ids[2], "2026-01-01 00:00:03"));

    /* Re-recording an existing query touches it back to the front; the row
     * keeps its identity and created_at, the count does not grow. */
    int64_t touch_now = T_BASE + 100 * T_SEC + 123456; /* non-zero fraction */
    cf_test_clock_set_fixed_us(touch_now);
    cf_search touched;
    memset(&touched, 0, sizeof touched);
    CF_REQUIRE(record_run(&env, user, text("query 5"), &touched) == CF_OK);
    CF_CHECK(touched.id == ids[5]);
    CF_CHECK(touched.user_id == user);
    check_str(touched.query, "query 5");
    CF_CHECK(touched.created_at == T_BASE + 6 * T_SEC);
    CF_CHECK(touched.updated_at == touch_now);

    CF_REQUIRE(cf_search_count_for_user(env.reader, user, &count) == CF_OK);
    CF_CHECK(count == 10);
    CF_CHECK(raw_bound_text_is(&env,
                               "SELECT updated_at FROM searches WHERE id = ?",
                               ids[5], "2026-01-01 00:01:40.123456"));

    cf_search_vector_dispose(&searches);
    CF_REQUIRE(cf_search_ordered_for_user(env.reader, user, &searches) ==
               CF_OK);
    CF_REQUIRE(searches.len == 10);
    CF_CHECK(searches.items[0].id == ids[5]);
    check_str(searches.items[0].query, "query 5");
    CF_CHECK(searches.items[0].updated_at == touch_now);
    cf_search_vector_dispose(&searches);

    /* The other user's row (none recorded) stays untouched. */
    CF_REQUIRE(cf_search_count_for_user(env.reader, other, &count) == CF_OK);
    CF_CHECK(count == 0);

    cf_search_dispose(&touched);
    cf_test_clock_clear();
    search_env_close(&env);
}

CF_TEST(record_matches_exact_user_and_query) {
    search_env env;
    CF_REQUIRE(search_env_open(&env));
    int64_t user = insert_user(&env, "a");
    int64_t other = insert_user(&env, "b");
    CF_REQUIRE(user > 0 && other > 0);

    /* The same text for another user is a different saved search. */
    cf_test_clock_set_fixed_us(T_BASE);
    cf_search first;
    memset(&first, 0, sizeof first);
    CF_REQUIRE(record_run(&env, user, text("x"), &first) == CF_OK);
    cf_test_clock_set_fixed_us(T_BASE + T_SEC);
    cf_search second;
    memset(&second, 0, sizeof second);
    CF_REQUIRE(record_run(&env, other, text("x"), &second) == CF_OK);
    CF_CHECK(second.id != first.id);
    CF_CHECK(second.user_id == other);

    cf_test_clock_set_fixed_us(T_BASE + 2 * T_SEC);
    cf_search again;
    memset(&again, 0, sizeof again);
    CF_REQUIRE(record_run(&env, user, text("x"), &again) == CF_OK);
    CF_CHECK(again.id == first.id); /* find, not create */
    CF_CHECK(again.user_id == user);
    CF_CHECK(again.created_at == T_BASE);        /* untouched */
    CF_CHECK(again.updated_at == T_BASE + 2 * T_SEC);

    int64_t count = -1;
    CF_REQUIRE(cf_search_count_for_user(env.reader, user, &count) == CF_OK);
    CF_CHECK(count == 1);
    CF_REQUIRE(cf_search_count_for_user(env.reader, other, &count) == CF_OK);
    CF_CHECK(count == 1);

    /* The predicate is `query = ?`, not LIKE: metacharacters are literal. */
    cf_test_clock_set_fixed_us(T_BASE + 3 * T_SEC);
    cf_search percent;
    memset(&percent, 0, sizeof percent);
    CF_REQUIRE(record_run(&env, user, text("%"), &percent) == CF_OK);
    CF_CHECK(percent.id != first.id);

    cf_test_clock_set_fixed_us(T_BASE + 4 * T_SEC);
    cf_search underscore;
    memset(&underscore, 0, sizeof underscore);
    CF_REQUIRE(record_run(&env, user, text("_"), &underscore) == CF_OK);
    CF_CHECK(underscore.id != percent.id);

    cf_test_clock_set_fixed_us(T_BASE + 5 * T_SEC);
    cf_search percent_again;
    memset(&percent_again, 0, sizeof percent_again);
    CF_REQUIRE(record_run(&env, user, text("%"), &percent_again) == CF_OK);
    CF_CHECK(percent_again.id == percent.id); /* literal match, no wildcard */
    CF_CHECK(percent_again.updated_at == T_BASE + 5 * T_SEC);

    CF_REQUIRE(cf_search_count_for_user(env.reader, user, &count) == CF_OK);
    CF_CHECK(count == 3);

    cf_search_dispose(&first);
    cf_search_dispose(&second);
    cf_search_dispose(&again);
    cf_search_dispose(&percent);
    cf_search_dispose(&underscore);
    cf_search_dispose(&percent_again);
    cf_test_clock_clear();
    search_env_close(&env);
}

CF_TEST(record_empty_query_keeps_one_row) {
    search_env env;
    CF_REQUIRE(search_env_open(&env));
    int64_t user = insert_user(&env, "empty-query");
    CF_REQUIRE(user > 0);

    cf_test_clock_set_fixed_us(T_BASE);
    cf_search empty;
    memset(&empty, 0, sizeof empty);
    CF_REQUIRE(record_run(&env, user, text(""), &empty) == CF_OK);
    CF_CHECK(empty.id > 0);
    CF_CHECK(empty.query.ptr != NULL); /* present, not NULL: an owned "" */
    check_str(empty.query, "");

    cf_test_clock_set_fixed_us(T_BASE + T_SEC);
    cf_search repeat;
    memset(&repeat, 0, sizeof repeat);
    CF_REQUIRE(record_run(&env, user, text(""), &repeat) == CF_OK);
    CF_CHECK(repeat.id == empty.id);
    CF_CHECK(repeat.created_at == T_BASE);
    CF_CHECK(repeat.updated_at == T_BASE + T_SEC);

    int64_t count = -1;
    CF_REQUIRE(cf_search_count_for_user(env.reader, user, &count) == CF_OK);
    CF_CHECK(count == 1);

    cf_search_dispose(&empty);
    cf_search_dispose(&repeat);
    cf_test_clock_clear();
    search_env_close(&env);
}

CF_TEST(record_failure_paths) {
    search_env env;
    CF_REQUIRE(search_env_open(&env));
    int64_t user = insert_user(&env, "failures");
    CF_REQUIRE(user > 0);

    cf_test_clock_set_fixed_us(T_BASE);

    cf_search out;
    memset(&out, 0, sizeof out);
    CF_CHECK(cf_search_record(NULL, user, text("q"), &out) == CF_INVALID);
    check_empty_record(&out);
    /* A failed call never leaves a value behind. */
    CF_CHECK(cf_search_record(NULL, user, text("q"), NULL) == CF_INVALID);

    /* Invalid borrowed span (bytes without a pointer): the callback fails,
     * the writer rolls back, nothing is stored. */
    cf_str bad = {NULL, 5};
    CF_CHECK(record_run(&env, user, bad, &out) == CF_INVALID);
    check_empty_record(&out);

    /* searches.user_id references users: an unknown user is a constraint
     * failure (CF_INVALID) that rolls back without a row. */
    CF_CHECK(record_run(&env, 999999, text("q"), &out) == CF_INVALID);
    check_empty_record(&out);
    CF_CHECK(raw_count(&env, "") == 0);

    /* The writer and its statement cache stay usable afterwards. */
    cf_search valid;
    memset(&valid, 0, sizeof valid);
    CF_REQUIRE(record_run(&env, user, text("q"), &valid) == CF_OK);
    CF_CHECK(valid.id > 0);
    CF_CHECK(raw_count(&env, "") == 1);
    cf_search_dispose(&valid);

    cf_test_clock_clear();
    search_env_close(&env);
}

CF_TEST(destroy_all_for_user_is_scoped_and_idempotent) {
    search_env env;
    CF_REQUIRE(search_env_open(&env));
    int64_t user = insert_user(&env, "doomed");
    int64_t other = insert_user(&env, "kept");
    CF_REQUIRE(user > 0 && other > 0);

    cf_test_clock_set_fixed_us(T_BASE);
    for (int n = 0; n < 3; n++) {
        char query[16];
        snprintf(query, sizeof query, "doomed %d", n);
        cf_search search;
        memset(&search, 0, sizeof search);
        CF_REQUIRE(record_run(&env, user, text(query), &search) == CF_OK);
        cf_search_dispose(&search);
    }
    for (int n = 0; n < 2; n++) {
        char query[16];
        snprintf(query, sizeof query, "kept %d", n);
        cf_search search;
        memset(&search, 0, sizeof search);
        CF_REQUIRE(record_run(&env, other, text(query), &search) == CF_OK);
        cf_search_dispose(&search);
    }
    CF_CHECK(raw_count(&env, "") == 5);

    CF_REQUIRE(destroy_run(&env, user) == CF_OK);
    CF_CHECK(raw_count(&env, "") == 2);
    int64_t count = -1;
    CF_REQUIRE(cf_search_count_for_user(env.reader, user, &count) == CF_OK);
    CF_CHECK(count == 0);
    CF_REQUIRE(cf_search_count_for_user(env.reader, other, &count) == CF_OK);
    CF_CHECK(count == 2);

    cf_search_vector searches;
    CF_REQUIRE(cf_search_ordered_for_user(env.reader, user, &searches) ==
               CF_OK);
    CF_CHECK(searches.len == 0);
    cf_search_vector_dispose(&searches);

    /* destroy_all is a delete-all: repeating it and deleting an unknown
     * user's searches are both OK (the source ignores affected counts). */
    CF_CHECK(destroy_run(&env, user) == CF_OK);
    CF_CHECK(destroy_run(&env, 999999) == CF_OK);
    CF_CHECK(cf_search_destroy_all_for_user(NULL, user) == CF_INVALID);
    CF_CHECK(raw_count(&env, "") == 2);

    cf_test_clock_clear();
    search_env_close(&env);
}

CF_TEST(ordered_for_user_orders_by_updated_at_and_isolates_users) {
    search_env env;
    CF_REQUIRE(search_env_open(&env));
    int64_t user = insert_user(&env, "ordering");
    int64_t other = insert_user(&env, "ordering-other");
    CF_REQUIRE(user > 0 && other > 0);

    /* updated_at decides the order; created_at and id do not. */
    CF_REQUIRE(insert_search(&env, user, "middle", "2026-01-04 00:00:00",
                             "2026-01-02 00:00:00") > 0);
    CF_REQUIRE(insert_search(&env, user, "newest", "2026-01-02 00:00:00",
                             "2026-01-03 00:00:00") > 0);
    CF_REQUIRE(insert_search(&env, user, "oldest", "2026-01-05 00:00:00",
                             "2026-01-01 00:00:00") > 0);
    CF_REQUIRE(insert_search(&env, other, "other", "2026-01-06 00:00:00",
                             "2026-01-06 00:00:00") > 0);

    cf_search_vector searches;
    CF_REQUIRE(cf_search_ordered_for_user(env.reader, user, &searches) ==
               CF_OK);
    CF_REQUIRE(searches.len == 3);
    check_str(searches.items[0].query, "newest");
    check_str(searches.items[1].query, "middle");
    check_str(searches.items[2].query, "oldest");
    for (size_t i = 0; i < searches.len; i++) {
        CF_CHECK(searches.items[i].user_id == user);
    }
    /* Datetime text is parsed to UTC microseconds, not kept as bytes. */
    CF_CHECK(searches.items[0].updated_at == T_BASE + 2 * 86400 * T_SEC);
    CF_CHECK(searches.items[0].created_at == T_BASE + 86400 * T_SEC);
    cf_search_vector_dispose(&searches);

    CF_REQUIRE(cf_search_ordered_for_user(env.reader, other, &searches) ==
               CF_OK);
    CF_REQUIRE(searches.len == 1);
    check_str(searches.items[0].query, "other");
    cf_search_vector_dispose(&searches);

    CF_REQUIRE(cf_search_ordered_for_user(env.reader, 999999, &searches) ==
               CF_OK);
    CF_CHECK(searches.len == 0);
    cf_search_vector_dispose(&searches);

    search_env_close(&env);
}

CF_TEST(dispose_helpers_are_null_safe_and_reset) {
    cf_search_dispose(NULL);
    cf_search_vector_dispose(NULL);

    cf_search empty;
    memset(&empty, 0, sizeof empty);
    cf_search_dispose(&empty); /* zero state */
    check_empty_record(&empty);

    /* Partially built vector: disposal walks owned rows, then frees the
     * array and resets the header. */
    cf_search_vector vector;
    memset(&vector, 0, sizeof vector);
    vector.items = malloc(2 * sizeof *vector.items);
    CF_REQUIRE(vector.items != NULL);
    memset(vector.items, 0, 2 * sizeof *vector.items);
    vector.len = 1;
    vector.cap = 2;
    vector.items[0].id = 7;
    vector.items[0].user_id = 8;
    vector.items[0].query.len = 5;
    vector.items[0].query.ptr = malloc(6);
    CF_REQUIRE(vector.items[0].query.ptr != NULL);
    memcpy(vector.items[0].query.ptr, "owned", 6);

    cf_search_vector_dispose(&vector);
    CF_CHECK(vector.items == NULL && vector.len == 0 && vector.cap == 0);
}

CF_TEST_MAIN()
