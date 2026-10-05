/* F00 probe: SQLite 3.53.4 amalgamation built with
 * -O2 -DSQLITE_THREADSAFE=1 -DSQLITE_ENABLE_FTS5.
 * Exercises: libversion, full PRAGMA compile_options list, WAL on a temp file,
 * foreign_keys=ON, user_version set/read, FTS5 virtual table round trip,
 * busy_timeout 1000. Any failed assertion fails the probe (exit 1). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sqlite3.h>

static int failures = 0;

static void fail(const char *what) {
    failures++;
    fprintf(stderr, "PROBE FAIL: %s\n", what);
}

static int exec_sql(sqlite3 *db, const char *sql) {
    char *errmsg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite3_exec rc=%d err=%s sql=\"%s\"\n", rc,
                errmsg ? errmsg : "(none)", sql);
        sqlite3_free(errmsg);
        fail(sql);
        return 0;
    }
    return 1;
}

static int query_int64(sqlite3 *db, const char *sql, sqlite3_int64 *out) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "prepare rc=%d err=%s sql=\"%s\"\n", rc, sqlite3_errmsg(db), sql);
        fail(sql);
        return 0;
    }
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "step rc=%d err=%s sql=\"%s\"\n", rc, sqlite3_errmsg(db), sql);
        sqlite3_finalize(st);
        fail(sql);
        return 0;
    }
    *out = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return 1;
}

static char *query_text(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    static char buf[256];
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "prepare rc=%d err=%s sql=\"%s\"\n", rc, sqlite3_errmsg(db), sql);
        fail(sql);
        return NULL;
    }
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "step rc=%d err=%s sql=\"%s\"\n", rc, sqlite3_errmsg(db), sql);
        sqlite3_finalize(st);
        fail(sql);
        return NULL;
    }
    const unsigned char *t = sqlite3_column_text(st, 0);
    snprintf(buf, sizeof buf, "%s", t ? (const char *)t : "(null)");
    sqlite3_finalize(st);
    return buf;
}

static void print_compile_options(sqlite3 *db, int *has_fts5, int *has_threadsafe1) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, "PRAGMA compile_options", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        fail("prepare PRAGMA compile_options");
        return;
    }
    int i = 0;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const unsigned char *opt = sqlite3_column_text(st, 0);
        printf("  compile_options[%d] = %s\n", i++, opt ? (const char *)opt : "(null)");
        if (opt) {
            if (strcmp((const char *)opt, "ENABLE_FTS5") == 0) *has_fts5 = 1;
            if (strcmp((const char *)opt, "THREADSAFE=1") == 0) *has_threadsafe1 = 1;
        }
    }
    if (rc != SQLITE_DONE) fail("step PRAGMA compile_options");
    sqlite3_finalize(st);
    printf("compile_options count=%d\n", i);
}

int main(void) {
    printf("sqlite3_libversion() = %s\n", sqlite3_libversion());
    printf("sqlite3_sourceid()   = %s\n", sqlite3_sourceid());
    printf("sqlite3_threadsafe() = %d (expect 1)\n", sqlite3_threadsafe());
    if (sqlite3_threadsafe() != 1) fail("sqlite3_threadsafe() != 1");
    if (strcmp(sqlite3_libversion(), "3.53.4") != 0) fail("sqlite3_libversion() != 3.53.4");

    char path[128];
    snprintf(path, sizeof path, "/tmp/cf-sqlite-probe-%ld.db", (long)getpid());
    unlink(path);

    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "open rc=%d err=%s\n", rc, db ? sqlite3_errmsg(db) : "(no db)");
        return 1;
    }
    printf("opened temp database: %s\n", path);

    int has_fts5 = 0, has_threadsafe1 = 0;
    print_compile_options(db, &has_fts5, &has_threadsafe1);
    printf("compile option ENABLE_FTS5 present=%d (expect 1)\n", has_fts5);
    printf("compile option THREADSAFE=1 present=%d (expect 1)\n", has_threadsafe1);
    if (!has_fts5) fail("ENABLE_FTS5 not in PRAGMA compile_options");
    if (!has_threadsafe1) fail("THREADSAFE=1 not in PRAGMA compile_options");

    char *jm = query_text(db, "PRAGMA journal_mode=WAL");
    printf("PRAGMA journal_mode=WAL -> %s (expect wal)\n", jm ? jm : "(error)");
    if (!jm || strcmp(jm, "wal") != 0) fail("journal_mode WAL not activated");

    if (exec_sql(db, "PRAGMA foreign_keys=ON")) {
        sqlite3_int64 fk = -1;
        if (query_int64(db, "PRAGMA foreign_keys", &fk))
            printf("PRAGMA foreign_keys -> %lld (expect 1)\n", (long long)fk);
        if (fk != 1) fail("foreign_keys not ON");
    }

    if (exec_sql(db, "PRAGMA user_version=42")) {
        sqlite3_int64 uv = -1;
        if (query_int64(db, "PRAGMA user_version", &uv))
            printf("PRAGMA user_version -> %lld (expect 42)\n", (long long)uv);
        if (uv != 42) fail("user_version round trip failed");
    }

    if (exec_sql(db, "CREATE VIRTUAL TABLE fts_probe USING fts5(body)")) {
        int ok = exec_sql(db, "INSERT INTO fts_probe(body) VALUES ('hello campfire'), ('goodbye world')");
        printf("FTS5 create+insert ok=%d\n", ok);
    }
    sqlite3_stmt *st = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT rowid, body FROM fts_probe WHERE fts_probe MATCH 'campfire'", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "fts5 prepare rc=%d err=%s\n", rc, sqlite3_errmsg(db));
        fail("fts5 match prepare");
    } else {
        rc = sqlite3_step(st);
        if (rc != SQLITE_ROW) {
            fprintf(stderr, "fts5 step rc=%d err=%s\n", rc, sqlite3_errmsg(db));
            fail("fts5 MATCH returned no row");
        } else {
            printf("FTS5 MATCH 'campfire' -> rowid=%lld body=%s\n",
                   (long long)sqlite3_column_int64(st, 0),
                   (const char *)sqlite3_column_text(st, 1));
            sqlite3_finalize(st);
        }
    }

    sqlite3_busy_timeout(db, 1000);
    sqlite3_int64 bt = -1;
    if (query_int64(db, "PRAGMA busy_timeout", &bt))
        printf("sqlite3_busy_timeout(1000) then PRAGMA busy_timeout -> %lld (expect 1000)\n",
               (long long)bt);
    if (bt != 1000) fail("busy_timeout not 1000");

    sqlite3_close(db);
    unlink(path);
    char aux[160];
    snprintf(aux, sizeof aux, "%s-wal", path); unlink(aux);
    snprintf(aux, sizeof aux, "%s-shm", path); unlink(aux);

    printf("probe_result=%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
