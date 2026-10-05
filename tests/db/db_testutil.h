/* Test-only helpers for the D01 db-core tests.
 *
 * Not production code and not part of any application API.  Provides a
 * temporary-directory scratch database and small raw-SQLite queries used to
 * inspect what the db module created.
 */
#ifndef CF_DB_TESTUTIL_H
#define CF_DB_TESTUTIL_H

#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cf.h"
#include "db/db_internal.h"

#define CF_DB_TEST_FILE "test.db"
#define CF_DB_TEST_PATH_CAP 512

static inline char *cf_db_test_dir(void) {
    char template[] = "/tmp/cf_db_test_XXXXXX";
    char *dir = mkdtemp(template);
    if (dir == NULL) return NULL;
    return strdup(dir);
}

static inline const char *cf_db_test_path(char *buf, size_t cap,
                                          const char *dir) {
    snprintf(buf, cap, "%s/%s", dir, CF_DB_TEST_FILE);
    return buf;
}

/* Remove one named database (plus WAL sidecars) from a scratch directory. */
static inline void cf_db_test_cleanup_named(const char *dir,
                                            const char *name) {
    if (dir == NULL) return;
    char path[CF_DB_TEST_PATH_CAP];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    remove(path);
    snprintf(path, sizeof path, "%s/%s-wal", dir, name);
    remove(path);
    snprintf(path, sizeof path, "%s/%s-shm", dir, name);
    remove(path);
}

static inline void cf_db_test_cleanup(const char *dir) {
    if (dir == NULL) return;
    cf_db_test_cleanup_named(dir, CF_DB_TEST_FILE);
    rmdir(dir);
}

/* A scratch directory with a version-1 database open in write mode. */
typedef struct {
    char *dir;
    char path[CF_DB_TEST_PATH_CAP];
    cf_db *db;
} cf_db_scratch;

static inline bool cf_db_scratch_open(cf_db_scratch *scratch) {
    memset(scratch, 0, sizeof *scratch);
    scratch->dir = cf_db_test_dir();
    if (scratch->dir == NULL) return false;
    cf_db_test_path(scratch->path, sizeof scratch->path, scratch->dir);
    if (cf_db_open(scratch->path, false, &scratch->db) != CF_OK) {
        return false;
    }
    return true;
}

static inline void cf_db_scratch_close(cf_db_scratch *scratch) {
    cf_db_close(scratch->db);
    scratch->db = NULL;
    cf_db_test_cleanup(scratch->dir);
    free(scratch->dir);
    scratch->dir = NULL;
}

/* Raw exec; the caller checks the returned SQLite code. */
static inline int cf_db_test_exec(sqlite3 *handle, const char *sql) {
    return sqlite3_exec(handle, sql, NULL, NULL, NULL);
}

/* One-row, one-column integer query; sets *ok to false on failure. */
static inline int64_t cf_db_test_i64(sqlite3 *handle, const char *sql,
                                     bool *ok) {
    *ok = false;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(handle, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *ok = true;
        int64_t value = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return value;
    }
    sqlite3_finalize(stmt);
    return 0;
}

/* One-row, one-column text query copied into buf; NULL on failure. */
static inline const char *cf_db_test_text(sqlite3 *handle, const char *sql,
                                          char *buf, size_t cap) {
    buf[0] = '\0';
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(handle, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return NULL;
    }
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return NULL;
    }
    const unsigned char *text = sqlite3_column_text(stmt, 0);
    if (text == NULL || (size_t)sqlite3_column_bytes(stmt, 0) >= cap) {
        sqlite3_finalize(stmt);
        return NULL;
    }
    snprintf(buf, cap, "%s", (const char *)text);
    sqlite3_finalize(stmt);
    return buf;
}

/* True when sqlite_master holds a `type` object with this exact name. */
static inline bool cf_db_test_has_object(sqlite3 *handle, const char *type,
                                         const char *name) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(
            handle, "SELECT 1 FROM sqlite_master WHERE type=?1 AND name=?2",
            -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return found;
}

/* Count schema objects of one type, excluding SQLite's internal names. */
static inline int64_t cf_db_test_object_count(sqlite3 *handle,
                                              const char *type) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(
            handle,
            "SELECT count(*) FROM sqlite_master WHERE type=?1 AND "
            "name NOT LIKE 'sqlite_%'",
            -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(stmt, 1, type, -1, SQLITE_TRANSIENT);
    int64_t count = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

#endif /* CF_DB_TESTUTIL_H */
