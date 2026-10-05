/* DB-03: concurrent readers and serialized writes do not share statements;
 * a reader connection belongs to one thread; the 1000 ms busy timeout is
 * bounded and readers still progress under WAL while a raw write transaction
 * is held.
 *
 * Contention is forced deterministically by a raw SQLite connection holding
 * BEGIN IMMEDIATE; no timing race is assumed and no sleep is used.
 *
 * Acceptance: 07-verification.md DB-03; 02-data-auth.md D01.
 */
#include "cf_test.h"

#include "db/db_testutil.h"

#include <pthread.h>

enum {
    STMT_COUNT_MESSAGES,
    STMT_COUNT_ROOMS,
    STMT_INSERT_ROOM,
    TEST_STMT_COUNT,
};

static const cf_stmt_def reader_defs[TEST_STMT_COUNT] = {
    [STMT_COUNT_MESSAGES] = {"SELECT count(*) FROM messages"},
    [STMT_COUNT_ROOMS] = {"SELECT count(*) FROM rooms"},
    [STMT_INSERT_ROOM] = {
        "INSERT INTO rooms (created_at, creator_id, name, type, updated_at) "
        "VALUES (?1, ?2, ?3, ?4, ?5)"},
};
static const cf_stmt_set reader_set = {reader_defs, TEST_STMT_COUNT};

static cf_span sp(const char *text) {
    cf_span span = {(const unsigned char *)text, strlen(text)};
    return span;
}

/* --- two reader threads on their own connections ------------------------- */

typedef struct {
    const char *path;
    pthread_barrier_t *barrier;
    cf_err open_err;
    cf_err begin_err;
    cf_err step_err;
    cf_err end_err;
    int64_t message_count;
    sqlite3_stmt *stmt;
} reader_run;

static void *reader_thread_main(void *arg) {
    reader_run *run = arg;
    cf_db *db = NULL;
    run->open_err = cf_db_open(run->path, true, &db);
    if (run->open_err != CF_OK) return NULL;

    run->begin_err = cf_db_stmt(db, &reader_set, STMT_COUNT_MESSAGES, &run->stmt);

    /* Both read transactions are open before either steps: the reads
     * genuinely overlap under WAL. */
    pthread_barrier_wait(run->barrier);

    if (run->begin_err == CF_OK) run->begin_err = cf_read_begin(db);
    if (run->begin_err == CF_OK) {
        sqlite3_stmt *again = NULL;
        cf_err err = cf_db_stmt(db, &reader_set, STMT_COUNT_MESSAGES, &again);
        if (err != CF_OK) {
            run->step_err = err;
        } else {
            if (again != run->stmt) run->step_err = CF_INTERNAL;
            int rc = sqlite3_step(again);
            if (rc == SQLITE_ROW) {
                run->message_count = sqlite3_column_int64(again, 0);
            } else {
                run->step_err = cf_db_err(rc);
            }
            cf_db_stmt_done(again);
        }
    }
    if (run->begin_err == CF_OK) run->end_err = cf_read_end(db);
    cf_db_close(db);
    return NULL;
}

CF_TEST(reader_two_connections_concurrent_under_wal) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    sqlite3 *handle = cf_db_handle(scratch.db);
    CF_REQUIRE(handle != NULL);
    CF_REQUIRE(cf_db_test_exec(
                   handle,
                   "INSERT INTO users (id, created_at, name, updated_at) "
                   "VALUES (1, '2026-01-01 00:00:00', 'A', "
                   "'2026-01-01 00:00:00')") == SQLITE_OK);
    CF_REQUIRE(cf_db_test_exec(
                   handle,
                   "INSERT INTO rooms (id, created_at, creator_id, name, type, "
                   "updated_at) VALUES (1, '2026-01-01 00:00:00', 1, 'R', "
                   "'open', '2026-01-01 00:00:00')") == SQLITE_OK);
    CF_REQUIRE(cf_db_test_exec(
                   handle,
                   "INSERT INTO messages (client_message_id, created_at, "
                   "creator_id, room_id, updated_at) VALUES ('c1', "
                   "'2026-01-01 00:00:00', 1, 1, '2026-01-01 00:00:00')") ==
               SQLITE_OK);

    pthread_barrier_t barrier;
    CF_REQUIRE(pthread_barrier_init(&barrier, NULL, 2) == 0);
    reader_run runs[2];
    memset(runs, 0, sizeof runs);
    runs[0].path = scratch.path;
    runs[0].barrier = &barrier;
    runs[1].path = scratch.path;
    runs[1].barrier = &barrier;

    pthread_t threads[2];
    CF_REQUIRE(pthread_create(&threads[0], NULL, reader_thread_main,
                              &runs[0]) == 0);
    CF_REQUIRE(pthread_create(&threads[1], NULL, reader_thread_main,
                              &runs[1]) == 0);
    CF_REQUIRE(pthread_join(threads[0], NULL) == 0);
    CF_REQUIRE(pthread_join(threads[1], NULL) == 0);
    CF_REQUIRE(pthread_barrier_destroy(&barrier) == 0);

    for (size_t i = 0; i < 2; i++) {
        CF_CHECK(runs[i].open_err == CF_OK);
        CF_CHECK(runs[i].begin_err == CF_OK);
        CF_CHECK(runs[i].step_err == CF_OK);
        CF_CHECK(runs[i].end_err == CF_OK);
        CF_CHECK(runs[i].message_count == 1);
        CF_CHECK(runs[i].stmt != NULL);
    }
    /* One cache per connection: the two readers never share a statement. */
    CF_CHECK(runs[0].stmt != runs[1].stmt);

    cf_db_scratch_close(&scratch);
}

/* --- a reader belongs to one thread -------------------------------------- */

typedef struct {
    cf_db *db;
    cf_err begin_err;
    char message[CF_DB_ERROR_CAP];
} foreign_use;

static void *foreign_thread_main(void *arg) {
    foreign_use *use = arg;
    use->begin_err = cf_read_begin(use->db);
    /* cf_db_last_error() is thread-local: copy it for the case thread. */
    snprintf(use->message, sizeof use->message, "%s", cf_db_last_error());
    return NULL;
}

CF_TEST(reader_connection_rejects_foreign_thread) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));

    cf_db *reader = NULL;
    CF_REQUIRE(cf_db_open(scratch.path, true, &reader) == CF_OK);
    CF_REQUIRE(reader != NULL);
    CF_CHECK(cf_db_thread_ok(reader));

    foreign_use use = {reader, CF_OK, ""};
    pthread_t thread;
    CF_REQUIRE(pthread_create(&thread, NULL, foreign_thread_main, &use) == 0);
    CF_REQUIRE(pthread_join(thread, NULL) == 0);
    CF_CHECK(use.begin_err == CF_INTERNAL);
    CF_CHECK(strstr(use.message, "thread") != NULL);

    /* The owning thread still works after the foreign attempt. */
    CF_CHECK(cf_read_begin(reader) == CF_OK);
    CF_CHECK(cf_db_in_read(reader));
    CF_CHECK(cf_read_end(reader) == CF_OK);

    cf_db_close(reader);
    cf_db_scratch_close(&scratch);
}

/* --- bounded busy timeout with a held raw write transaction -------------- */

CF_TEST(reader_busy_timeout_bounded_writers_wait_readers_progress) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    sqlite3 *handle = cf_db_handle(scratch.db);
    CF_REQUIRE(cf_db_test_exec(
                   handle,
                   "INSERT INTO rooms (id, created_at, creator_id, name, type, "
                   "updated_at) VALUES (1, '2026-01-01 00:00:00', 1, 'R', "
                   "'open', '2026-01-01 00:00:00')") == SQLITE_OK);

    /* A raw connection holds the write lock for the whole measurement. */
    sqlite3 *raw = NULL;
    CF_REQUIRE(sqlite3_open_v2(
                   scratch.path, &raw,
                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                   NULL) == SQLITE_OK);
    CF_REQUIRE(cf_db_test_exec(raw, "BEGIN IMMEDIATE") == SQLITE_OK);
    CF_REQUIRE(cf_db_test_exec(
                   raw,
                   "INSERT INTO rooms (id, created_at, creator_id, name, type, "
                   "updated_at) VALUES (2, '2026-01-01 00:00:00', 1, 'Held', "
                   "'open', '2026-01-01 00:00:00')") == SQLITE_OK);

    /* Readers keep working: WAL does not block readers on a writer. */
    cf_db *reader = NULL;
    CF_REQUIRE(cf_db_open(scratch.path, true, &reader) == CF_OK);
    CF_REQUIRE(cf_read_begin(reader) == CF_OK);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(cf_db_stmt(reader, &reader_set, STMT_COUNT_ROOMS, &stmt) ==
               CF_OK);
    CF_REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    CF_CHECK(sqlite3_column_int64(stmt, 0) == 1); /* uncommitted row absent */
    cf_db_stmt_done(stmt);
    CF_REQUIRE(cf_read_end(reader) == CF_OK);
    cf_db_close(reader);

    /* The writer waits, bounded by busy_timeout, then reports CF_BUSY. */
    cf_db *writer = NULL;
    CF_REQUIRE(cf_db_open(scratch.path, false, &writer) == CF_OK);
    bool ok = false;
    CF_CHECK(cf_db_test_i64(cf_db_handle(writer), "PRAGMA busy_timeout", &ok) ==
                 1000 &&
             ok);
    CF_REQUIRE(cf_db_stmt(writer, &reader_set, STMT_INSERT_ROOM, &stmt) ==
               CF_OK);
    CF_REQUIRE(cf_stmt_bind_text(stmt, 1, sp("2026-01-01 00:00:00")) ==
               CF_OK);
    CF_REQUIRE(cf_stmt_bind_i64(stmt, 2, 1) == CF_OK);
    CF_REQUIRE(cf_stmt_bind_text(stmt, 3, sp("Blocked")) == CF_OK);
    CF_REQUIRE(cf_stmt_bind_text(stmt, 4, sp("open")) == CF_OK);
    CF_REQUIRE(cf_stmt_bind_text(stmt, 5, sp("2026-01-01 00:00:00")) ==
               CF_OK);

    uint64_t started = cf_monotonic_ms();
    int rc = sqlite3_step(stmt);
    uint64_t elapsed = cf_monotonic_ms() - started;
    printf("    held-lock step returned SQLITE_BUSY after %llu ms\n",
           (unsigned long long)elapsed);
    CF_CHECK(rc == SQLITE_BUSY);
    CF_CHECK(cf_db_err(rc) == CF_BUSY);
    CF_CHECK(elapsed >= 900);  /* it really waited for the busy timeout */
    CF_CHECK(elapsed <= 3000); /* ... and stayed bounded */
    cf_db_stmt_done(stmt);

    /* Releasing the lock lets the same connection write. */
    CF_REQUIRE(cf_db_test_exec(raw, "ROLLBACK") == SQLITE_OK);
    sqlite3_close(raw);

    CF_REQUIRE(cf_db_stmt(writer, &reader_set, STMT_INSERT_ROOM, &stmt) ==
               CF_OK);
    CF_REQUIRE(cf_stmt_bind_text(stmt, 1, sp("2026-01-01 00:00:00")) ==
               CF_OK);
    CF_REQUIRE(cf_stmt_bind_i64(stmt, 2, 1) == CF_OK);
    CF_REQUIRE(cf_stmt_bind_text(stmt, 3, sp("After")) == CF_OK);
    CF_REQUIRE(cf_stmt_bind_text(stmt, 4, sp("open")) == CF_OK);
    CF_REQUIRE(cf_stmt_bind_text(stmt, 5, sp("2026-01-01 00:00:00")) ==
               CF_OK);
    CF_CHECK(sqlite3_step(stmt) == SQLITE_DONE);
    cf_db_stmt_done(stmt);

    /* Lifecycle errors are explicit, not silent. */
    CF_CHECK(cf_read_end(writer) == CF_INTERNAL);

    cf_db_close(writer);
    cf_db_scratch_close(&scratch);
}

CF_TEST_MAIN()
