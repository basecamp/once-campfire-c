/* Prepared-statement cache, binding copy semantics, row copy before reset,
 * and SQLite -> cf_err mapping (D01 db-core).
 *
 * Acceptance: 07-verification.md DB-01/DB-03 support; 02-data-auth.md D01
 * statement rules ("reset and clear bindings on every exit", "prefer
 * SQLITE_TRANSIENT", "copy row data before resetting statements", "dynamic
 * SQL is forbidden").
 */
#include "cf_test.h"

#include "db/db_testutil.h"

enum {
    STMT_SELECT_ONE,
    STMT_SELECT_BOUND,
    STMT_INSERT_SCRATCH,
    STMT_SELECT_SCRATCH,
    STMT_BAD_SQL,
    TEST_STMT_COUNT,
};

static const cf_stmt_def stmt_defs[TEST_STMT_COUNT] = {
    [STMT_SELECT_ONE] = {"SELECT 1"},
    [STMT_SELECT_BOUND] = {"SELECT ?1"},
    [STMT_INSERT_SCRATCH] = {"INSERT INTO scratch (v, n) VALUES (?1, ?2)"},
    [STMT_SELECT_SCRATCH] = {"SELECT v, n FROM scratch ORDER BY rowid"},
    [STMT_BAD_SQL] = {"SELECT FROM WHERE"},
};
static const cf_stmt_set stmt_set = {stmt_defs, TEST_STMT_COUNT};

static cf_span sp(const char *text) {
    cf_span span = {(const unsigned char *)text, strlen(text)};
    return span;
}

static bool scratch_with_table(cf_db_scratch *scratch) {
    if (!cf_db_scratch_open(scratch)) return false;
    return cf_db_test_exec(cf_db_handle(scratch->db),
                           "CREATE TABLE scratch (v TEXT, n INTEGER)") ==
           SQLITE_OK;
}

CF_TEST(statements_cache_is_per_connection_and_reused) {
    cf_db_scratch scratch;
    CF_REQUIRE(scratch_with_table(&scratch));

    sqlite3_stmt *first = NULL;
    sqlite3_stmt *second = NULL;
    CF_REQUIRE(cf_db_stmt(scratch.db, &stmt_set, STMT_SELECT_ONE, &first) ==
               CF_OK);
    CF_REQUIRE(cf_db_stmt(scratch.db, &stmt_set, STMT_SELECT_ONE, &second) ==
               CF_OK);
    CF_CHECK(first != NULL && first == second); /* prepared once per cache */
    CF_CHECK(sqlite3_step(first) == SQLITE_ROW);
    CF_CHECK(sqlite3_column_int64(first, 0) == 1);
    cf_db_stmt_done(first);

    /* Another connection has its own cache. */
    cf_db *other = NULL;
    CF_REQUIRE(cf_db_open(scratch.path, false, &other) == CF_OK);
    sqlite3_stmt *other_stmt = NULL;
    CF_REQUIRE(cf_db_stmt(other, &stmt_set, STMT_SELECT_ONE, &other_stmt) ==
               CF_OK);
    CF_CHECK(other_stmt != first);

    /* Clearing finalizes; a later get prepares a fresh, usable statement. */
    cf_db_stmt_cache_clear(scratch.db);
    sqlite3_stmt *fresh = NULL;
    CF_REQUIRE(cf_db_stmt(scratch.db, &stmt_set, STMT_SELECT_ONE, &fresh) ==
               CF_OK);
    CF_CHECK(fresh != NULL);
    CF_CHECK(sqlite3_step(fresh) == SQLITE_ROW);
    cf_db_stmt_done(fresh);

    cf_db_close(other);
    cf_db_scratch_close(&scratch);
}

CF_TEST(statements_bindings_copy_and_clear_on_done) {
    cf_db_scratch scratch;
    CF_REQUIRE(scratch_with_table(&scratch));

    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(cf_db_stmt(scratch.db, &stmt_set, STMT_SELECT_BOUND, &stmt) ==
               CF_OK);

    CF_REQUIRE(cf_stmt_bind_i64(stmt, 1, 42) == CF_OK);
    CF_REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    CF_CHECK(cf_stmt_column_i64(stmt, 0) == 42);
    cf_db_stmt_done(stmt);

    /* cf_db_stmt_done cleared the binding: the parameter is NULL now. */
    CF_REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    CF_CHECK(cf_stmt_column_is_null(stmt, 0));
    cf_db_stmt_done(stmt);

    /* Text is copied at bind time, not referenced (no SQLITE_STATIC). */
    char buffer[16] = "alpha";
    CF_REQUIRE(cf_stmt_bind_text(stmt, 1, sp(buffer)) == CF_OK);
    memset(buffer, 'x', strlen(buffer));
    CF_REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    cf_span text = cf_stmt_column_text(stmt, 0);
    CF_CHECK(text.len == 5 && memcmp(text.ptr, "alpha", 5) == 0);
    cf_db_stmt_done(stmt);

    /* Empty text is distinct from NULL. */
    CF_REQUIRE(cf_stmt_bind_text(stmt, 1, (cf_span){NULL, 0}) == CF_OK);
    CF_REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    CF_CHECK(!cf_stmt_column_is_null(stmt, 0));
    CF_CHECK(cf_stmt_column_text(stmt, 0).len == 0);
    cf_db_stmt_done(stmt);

    CF_REQUIRE(cf_stmt_bind_null(stmt, 1) == CF_OK);
    CF_REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    CF_CHECK(cf_stmt_column_is_null(stmt, 0));
    cf_db_stmt_done(stmt);

    /* Optional helpers: absent binds NULL, present binds the value. */
    CF_REQUIRE(cf_stmt_bind_opt_i64(
                   stmt, 1, (cf_optional_i64){.present = false, .value = 7}) ==
               CF_OK);
    CF_REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    CF_CHECK(cf_stmt_column_is_null(stmt, 0));
    cf_db_stmt_done(stmt);
    CF_REQUIRE(cf_stmt_bind_opt_i64(
                   stmt, 1, (cf_optional_i64){.present = true, .value = 7}) ==
               CF_OK);
    CF_REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    CF_CHECK(cf_stmt_column_i64(stmt, 0) == 7);
    cf_db_stmt_done(stmt);

    CF_REQUIRE(cf_stmt_bind_opt_text(stmt, 1, false, sp("skip")) == CF_OK);
    CF_REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    CF_CHECK(cf_stmt_column_is_null(stmt, 0));
    cf_db_stmt_done(stmt);
    CF_REQUIRE(cf_stmt_bind_opt_text(stmt, 1, true, sp("kept")) == CF_OK);
    CF_REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    CF_CHECK(cf_stmt_column_text(stmt, 0).len == 4);
    cf_db_stmt_done(stmt);

    cf_db_scratch_close(&scratch);
}

CF_TEST(statements_row_copy_survives_reset) {
    cf_db_scratch scratch;
    CF_REQUIRE(scratch_with_table(&scratch));

    sqlite3_stmt *insert = NULL;
    CF_REQUIRE(cf_db_stmt(scratch.db, &stmt_set, STMT_INSERT_SCRATCH,
                          &insert) == CF_OK);
    CF_REQUIRE(cf_stmt_bind_text(insert, 1, sp("persist")) == CF_OK);
    CF_REQUIRE(cf_stmt_bind_i64(insert, 2, 7) == CF_OK);
    CF_REQUIRE(sqlite3_step(insert) == SQLITE_DONE);
    cf_db_stmt_done(insert);
    CF_REQUIRE(cf_stmt_bind_opt_text(insert, 1, false, sp("")) == CF_OK);
    CF_REQUIRE(cf_stmt_bind_i64(insert, 2, 8) == CF_OK);
    CF_REQUIRE(sqlite3_step(insert) == SQLITE_DONE);
    cf_db_stmt_done(insert);

    sqlite3_stmt *select = NULL;
    CF_REQUIRE(cf_db_stmt(scratch.db, &stmt_set, STMT_SELECT_SCRATCH,
                          &select) == CF_OK);
    CF_REQUIRE(sqlite3_step(select) == SQLITE_ROW);
    cf_buf *copied = NULL;
    CF_REQUIRE(cf_stmt_column_copy_text(select, 0, &copied) == CF_OK);
    CF_REQUIRE(copied != NULL);
    CF_CHECK(cf_stmt_column_i64(select, 1) == 7);
    cf_span before = cf_buf_span(copied);
    CF_CHECK(before.len == 7 && memcmp(before.ptr, "persist", 7) == 0);

    /* Reset the statement; the copied bytes must remain intact. */
    cf_db_stmt_done(select);
    cf_span after = cf_buf_span(copied);
    CF_CHECK(after.len == before.len &&
             memcmp(after.ptr, before.ptr, before.len) == 0);
    cf_buf_release(copied);

    /* NULL renders as an absent copy, not an empty string.  The reset above
     * restarted the statement, so advance through the first row again. */
    CF_REQUIRE(sqlite3_step(select) == SQLITE_ROW);
    CF_REQUIRE(sqlite3_step(select) == SQLITE_ROW);
    CF_CHECK(cf_stmt_column_is_null(select, 0));
    cf_buf *absent = NULL;
    CF_REQUIRE(cf_stmt_column_copy_text(select, 0, &absent) == CF_OK);
    CF_CHECK(absent == NULL);
    CF_CHECK(cf_stmt_column_i64(select, 1) == 8);
    cf_db_stmt_done(select);

    cf_db_scratch_close(&scratch);
}

CF_TEST(statements_error_mapping_and_failed_prepare) {
    CF_CHECK(cf_db_err(SQLITE_OK) == CF_OK);
    CF_CHECK(cf_db_err(SQLITE_NOMEM) == CF_NOMEM);
    CF_CHECK(cf_db_err(SQLITE_BUSY) == CF_BUSY);
    CF_CHECK(cf_db_err(SQLITE_LOCKED) == CF_BUSY);
    CF_CHECK(cf_db_err(SQLITE_CONSTRAINT) == CF_INVALID);
    CF_CHECK(cf_db_err(SQLITE_CONSTRAINT_UNIQUE) == CF_INVALID);
    CF_CHECK(cf_db_err(SQLITE_IOERR) == CF_IO);
    CF_CHECK(cf_db_err(SQLITE_CANTOPEN) == CF_IO);
    CF_CHECK(cf_db_err(SQLITE_MISUSE) == CF_INTERNAL);
    CF_CHECK(cf_db_err(SQLITE_CORRUPT) == CF_DB);
    CF_CHECK(cf_db_err(SQLITE_NOTADB) == CF_DB);

    cf_db_scratch scratch;
    CF_REQUIRE(scratch_with_table(&scratch));
    sqlite3 *handle = cf_db_handle(scratch.db);
    CF_REQUIRE(cf_db_test_exec(
                   handle, "CREATE UNIQUE INDEX idx_scratch_n ON scratch(n)") ==
               SQLITE_OK);

    sqlite3_stmt *insert = NULL;
    CF_REQUIRE(cf_db_stmt(scratch.db, &stmt_set, STMT_INSERT_SCRATCH,
                          &insert) == CF_OK);
    CF_REQUIRE(cf_stmt_bind_opt_text(insert, 1, true, sp("a")) == CF_OK);
    CF_REQUIRE(cf_stmt_bind_i64(insert, 2, 1) == CF_OK);
    CF_REQUIRE(sqlite3_step(insert) == SQLITE_DONE);
    cf_db_stmt_done(insert);

    CF_REQUIRE(cf_stmt_bind_opt_text(insert, 1, true, sp("b")) == CF_OK);
    CF_REQUIRE(cf_stmt_bind_i64(insert, 2, 1) == CF_OK);
    int rc = sqlite3_step(insert);
    CF_CHECK(rc == SQLITE_CONSTRAINT);
    CF_CHECK(cf_db_err(rc) == CF_INVALID);
    cf_db_stmt_done(insert);

    /* Invalid fixed SQL fails the get with a message; the cache stays usable
     * and dynamic SQL is never involved. */
    sqlite3_stmt *bad = NULL;
    CF_CHECK(cf_db_stmt(scratch.db, &stmt_set, STMT_BAD_SQL, &bad) == CF_DB);
    CF_CHECK(bad == NULL);
    CF_CHECK(cf_db_last_error()[0] != '\0');
    CF_CHECK(cf_db_stmt(scratch.db, &stmt_set, STMT_SELECT_ONE, &bad) ==
             CF_OK);
    CF_CHECK(sqlite3_step(bad) == SQLITE_ROW);
    cf_db_stmt_done(bad);

    /* Unknown ids and missing sets are internal errors, not empty successes. */
    CF_CHECK(cf_db_stmt(scratch.db, &stmt_set, TEST_STMT_COUNT, &bad) ==
             CF_INTERNAL);
    CF_CHECK(cf_db_stmt(scratch.db, NULL, 0, &bad) == CF_INVALID);

    cf_db_scratch_close(&scratch);
}

CF_TEST(statements_build_info_shows_fts5_and_threadsafe) {
    cf_db_scratch scratch;
    CF_REQUIRE(scratch_with_table(&scratch));

    printf("    sqlite3_libversion() = %s\n", cf_db_libversion());
    CF_CHECK(strcmp(cf_db_libversion(), sqlite3_libversion()) == 0);
    CF_CHECK(strlen(cf_db_libversion()) != 0);
    CF_CHECK(sqlite3_threadsafe() == 1);

    cf_buf *options = NULL;
    CF_REQUIRE(cf_db_compile_options(scratch.db, &options) == CF_OK);
    CF_REQUIRE(options != NULL);
    cf_span text = cf_buf_span(options);
    CF_REQUIRE(text.ptr != NULL && text.len != 0);
    printf("    compile_options:\n%.*s", (int)text.len, text.ptr);
    CF_CHECK(memmem(text.ptr, text.len, "ENABLE_FTS5\n", 12) != NULL);
    CF_CHECK(memmem(text.ptr, text.len, "THREADSAFE=1\n", 13) != NULL);
    cf_buf_release(options);

    cf_db_scratch_close(&scratch);
}

CF_TEST_MAIN()
