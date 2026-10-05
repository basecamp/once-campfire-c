/* Reader connections, read transactions, error mapping and datetime text
 * (D01 db-core).
 *
 * One cf_db wraps one SQLite connection.  cf_db_open applies exactly the
 * per-connection settings of docs/devel/implementation/02-data-auth.md (WAL,
 * synchronous=NORMAL, foreign_keys=ON, locking_mode=NORMAL, mmap_size=0,
 * busy_timeout 1000 ms, 1000-page autocheckpoint) and then calls
 * cf_db_schema_ensure.  A reader connection belongs to the thread that opened
 * it; cf_read_begin/cf_read_end reject any other thread.
 *
 * Datetime text follows the pinned reference (tmp/rust-ref/crates/db/src/
 * time.rs, Rails datetime(6) quoting): UTC "YYYY-MM-DD HH:MM:SS" plus
 * ".ffffff" only when the microseconds are non-zero.
 */
#include "db/db_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- errors and messages ------------------------------------------------- */

static _Thread_local char cf_db_error_buf[CF_DB_ERROR_CAP];

const char *cf_db_last_error(void) {
    return cf_db_error_buf;
}

cf_err cf_db_failf(cf_err rc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cf_db_error_buf, sizeof cf_db_error_buf, fmt, ap);
    va_end(ap);
    return rc;
}

cf_err cf_db_err(int sqlite_rc) {
    switch (sqlite_rc & 0xff) {
    case SQLITE_OK:
        return CF_OK;
    case SQLITE_NOMEM:
        return CF_NOMEM;
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
        return CF_BUSY;
    case SQLITE_CONSTRAINT:
        return CF_INVALID;
    case SQLITE_IOERR:
    case SQLITE_CANTOPEN:
        return CF_IO;
    case SQLITE_MISUSE:
        return CF_INTERNAL;
    default:
        return CF_DB;
    }
}

/* --- connection settings ------------------------------------------------- */

/* Run one PRAGMA and read back its first column as text when it returns a
 * row.  Reports `what` plus SQLite's message on failure. */
static cf_err pragma_text(sqlite3 *handle, const char *sql, char *out,
                          size_t cap, const char *what) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        cf_err err = cf_db_err(rc);
        return cf_db_failf(err, "%s: %s", what, sqlite3_errmsg(handle));
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        cf_err err = cf_db_err(rc);
        cf_db_failf(err, "%s: %s", what, sqlite3_errmsg(handle));
        sqlite3_finalize(stmt);
        return err;
    }
    out[0] = '\0';
    if (rc == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        int len = sqlite3_column_bytes(stmt, 0);
        if (text == NULL || len < 0 || (size_t)len >= cap) {
            sqlite3_finalize(stmt);
            return cf_db_failf(CF_DB, "%s: unexpected result", what);
        }
        memcpy(out, text, (size_t)len);
        out[len] = '\0';
    }
    sqlite3_finalize(stmt);
    return CF_OK;
}

static cf_err configure_connection(sqlite3 *handle, bool read_only) {
    if (sqlite3_busy_timeout(handle, CF_DB_BUSY_TIMEOUT_MS) != SQLITE_OK) {
        return cf_db_failf(CF_NOMEM, "cannot set busy_timeout=%d ms",
                           CF_DB_BUSY_TIMEOUT_MS);
    }
    if (sqlite3_wal_autocheckpoint(handle, CF_DB_AUTOCHECKPOINT_PAGES) !=
        SQLITE_OK) {
        return cf_db_failf(CF_DB, "cannot set %d-page autocheckpoint",
                           CF_DB_AUTOCHECKPOINT_PAGES);
    }

    char mode[16];
    cf_err err = pragma_text(handle, "PRAGMA journal_mode=WAL", mode,
                             sizeof mode, "journal_mode=WAL");
    if (err != CF_OK) return err;
    if (strcmp(mode, "wal") != 0) {
        return cf_db_failf(CF_DB,
                           "journal_mode=WAL was not activated (got \"%s\")",
                           mode);
    }

    static const struct {
        const char *sql;
        const char *what;
    } settings[] = {
        {"PRAGMA synchronous=NORMAL", "synchronous=NORMAL"},
        {"PRAGMA foreign_keys=ON", "foreign_keys=ON"},
        {"PRAGMA locking_mode=NORMAL", "locking_mode=NORMAL"},
        {"PRAGMA mmap_size=0", "mmap_size=0"},
    };
    for (size_t i = 0; i < sizeof settings / sizeof settings[0]; i++) {
        char ignored[16];
        err = pragma_text(handle, settings[i].sql, ignored, sizeof ignored,
                          settings[i].what);
        if (err != CF_OK) return err;
    }

    if (read_only) {
        char ignored[16];
        err = pragma_text(handle, "PRAGMA query_only=ON", ignored,
                          sizeof ignored, "query_only=ON");
        if (err != CF_OK) return err;
    }
    return CF_OK;
}

/* --- open / close -------------------------------------------------------- */

cf_err cf_db_open(const char *path, bool read_only, cf_db **out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "no output pointer");
    *out = NULL;
    if (path == NULL || path[0] == '\0') {
        return cf_db_failf(CF_INVALID, "database path is empty");
    }

    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX;
    if (!read_only) flags |= SQLITE_OPEN_CREATE;

    sqlite3 *handle = NULL;
    int rc = sqlite3_open_v2(path, &handle, flags, NULL);
    if (rc != SQLITE_OK) {
        cf_err err = cf_db_err(rc);
        cf_db_failf(err, "cannot open database \"%s\": %s", path,
                    handle != NULL ? sqlite3_errmsg(handle)
                                   : "out of memory");
        if (handle != NULL) sqlite3_close(handle);
        return err;
    }

    cf_db *db = calloc(1, sizeof *db);
    if (db == NULL) {
        sqlite3_close(handle);
        return cf_db_failf(CF_NOMEM, "cannot allocate connection for \"%s\"",
                           path);
    }
    db->handle = handle;
    db->owner = pthread_self();
    db->read_only = read_only;
    db->path = strdup(path);
    if (db->path == NULL) {
        sqlite3_close(handle);
        free(db);
        return cf_db_failf(CF_NOMEM, "cannot copy database path \"%s\"", path);
    }

    cf_err err = configure_connection(handle, read_only);
    if (err == CF_OK) err = cf_db_schema_ensure(db);
    if (err != CF_OK) {
        cf_db_close(db);
        return err;
    }

    *out = db;
    return CF_OK;
}

void cf_db_close(cf_db *db) {
    if (db == NULL) return;
    if (!cf_db_thread_ok(db)) {
        cf_db_failf(CF_INTERNAL,
                    "database \"%s\" closed from a different thread",
                    db->path);
    }
    if (db->in_read) {
        sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL);
        db->in_read = false;
    }
    cf_db_stmt_cache_clear(db);
    sqlite3_close(db->handle);
    free(db->path);
    free(db);
}

sqlite3 *cf_db_handle(cf_db *db) {
    return db != NULL ? db->handle : NULL;
}

bool cf_db_thread_ok(const cf_db *db) {
    if (db == NULL) return false;
    /* Reader connections belong to their opening thread.  D02 serializes the
     * single writer connection, so any one thread may use it. */
    if (!db->read_only) return true;
    return pthread_equal(db->owner, pthread_self());
}

bool cf_db_in_read(const cf_db *db) {
    return db != NULL && db->in_read;
}

/* --- read transactions --------------------------------------------------- */

cf_err cf_read_begin(cf_db *db) {
    if (db == NULL) return cf_db_failf(CF_INVALID, "no database");
    if (!cf_db_thread_ok(db)) {
        return cf_db_failf(CF_INTERNAL,
                           "read transaction begun from a non-owning thread");
    }
    if (db->in_read) {
        return cf_db_failf(CF_INTERNAL, "read transaction already open");
    }
    int rc = sqlite3_exec(db->handle, "BEGIN", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        cf_err err = cf_db_err(rc);
        return cf_db_failf(err, "BEGIN: %s", sqlite3_errmsg(db->handle));
    }
    db->in_read = true;
    return CF_OK;
}

cf_err cf_read_end(cf_db *db) {
    if (db == NULL) return cf_db_failf(CF_INVALID, "no database");
    if (!cf_db_thread_ok(db)) {
        return cf_db_failf(CF_INTERNAL,
                           "read transaction ended from a non-owning thread");
    }
    if (!db->in_read) {
        return cf_db_failf(CF_INTERNAL, "no read transaction is open");
    }
    int rc = sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    db->in_read = false;
    if (rc != SQLITE_OK) {
        cf_err err = cf_db_err(rc);
        sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL);
        return cf_db_failf(err, "COMMIT read transaction: %s",
                           sqlite3_errmsg(db->handle));
    }
    return CF_OK;
}

/* --- datetime text <-> UTC microseconds ---------------------------------- */

static void civil_from_days(int64_t days, int *year, unsigned *month,
                            unsigned *day) {
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp + (mp < 10 ? 3 : (unsigned)-9);
    *year = (int)(y + (m <= 2));
    *month = m;
    *day = d;
}

static int64_t days_from_civil(int year, unsigned month, unsigned day) {
    int y = year - (month <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned mp = month > 2 ? month - 3 : month + 9;
    unsigned doy = (153 * mp + 2) / 5 + day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

cf_err cf_db_time_to_text(int64_t us, char out[CF_DB_TIME_TEXT_CAP]) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "no output buffer");
    int64_t seconds = us / 1000000;
    int64_t micros = us % 1000000;
    if (micros < 0) {
        micros += 1000000;
        seconds -= 1;
    }
    int64_t days = seconds / 86400;
    int64_t rem = seconds % 86400;
    if (rem < 0) {
        rem += 86400;
        days -= 1;
    }
    int year;
    unsigned month, day;
    civil_from_days(days, &year, &month, &day);
    if (year < 0 || year > 9999) {
        return cf_db_failf(CF_INVALID,
                           "datetime %lld is outside the stored year range",
                           (long long)us);
    }
    int hour = (int)(rem / 3600);
    int minute = (int)((rem % 3600) / 60);
    int second = (int)(rem % 60);
    int n = snprintf(out, CF_DB_TIME_TEXT_CAP, "%04d-%02u-%02u %02d:%02d:%02d",
                     year, month, day, hour, minute, second);
    if (n < 0 || n >= CF_DB_TIME_TEXT_CAP) {
        return cf_db_failf(CF_INTERNAL, "datetime format overflow");
    }
    if (micros != 0) {
        snprintf(out + n, (size_t)(CF_DB_TIME_TEXT_CAP - n), ".%06d",
                 (int)micros);
    }
    return CF_OK;
}

static bool parse_digits(const char *text, size_t len, int *out) {
    if (len == 0) return false;
    int value = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + (text[i] - '0');
    }
    *out = value;
    return true;
}

static bool parse_date_valid(int year, int month, int day) {
    static const int days_in_month[12] = {31, 28, 31, 30, 31, 30,
                                          31, 31, 30, 31, 30, 31};
    if (year < 0 || year > 9999 || month < 1 || month > 12 || day < 1) {
        return false;
    }
    int last = days_in_month[month - 1];
    if (month == 2 &&
        ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
        last = 29;
    }
    return day <= last;
}

cf_err cf_db_time_from_text(cf_span text, int64_t *out_us) {
    if (out_us == NULL) return cf_db_failf(CF_INVALID, "no output pointer");
    *out_us = 0;
    if (text.ptr == NULL) {
        return cf_db_failf(CF_INVALID, "datetime text is absent");
    }

    const char *begin = (const char *)text.ptr;
    size_t len = text.len;
    while (len != 0 && (begin[0] == ' ' || begin[0] == '\t' ||
                        begin[0] == '\n' || begin[0] == '\r')) {
        begin++;
        len--;
    }
    while (len != 0 && (begin[len - 1] == ' ' || begin[len - 1] == '\t' ||
                        begin[len - 1] == '\n' || begin[len - 1] == '\r')) {
        len--;
    }
    if (len >= 4 && memcmp(begin + len - 4, " UTC", 4) == 0) {
        len -= 4;
    } else if (len >= 1 && begin[len - 1] == 'Z') {
        len -= 1;
    }

    if (len < 19) {
        return cf_db_failf(CF_INVALID, "invalid datetime text");
    }
    if (begin[4] != '-' || begin[7] != '-' || begin[13] != ':' ||
        begin[16] != ':' || (begin[10] != ' ' && begin[10] != 'T')) {
        return cf_db_failf(CF_INVALID, "invalid datetime text separator");
    }

    int year, month, day, hour, minute, second;
    if (!parse_digits(begin, 4, &year) ||
        !parse_digits(begin + 5, 2, &month) ||
        !parse_digits(begin + 8, 2, &day) ||
        !parse_digits(begin + 11, 2, &hour) ||
        !parse_digits(begin + 14, 2, &minute) ||
        !parse_digits(begin + 17, 2, &second)) {
        return cf_db_failf(CF_INVALID, "invalid datetime digit");
    }
    if (!parse_date_valid(year, month, day) || hour > 23 || minute > 59 ||
        second > 59) {
        return cf_db_failf(CF_INVALID, "datetime field out of range");
    }

    int micros = 0;
    if (len > 19) {
        if (begin[19] != '.') {
            return cf_db_failf(CF_INVALID, "invalid datetime fraction");
        }
        size_t digits = len - 20;
        if (digits == 0) {
            return cf_db_failf(CF_INVALID, "empty datetime fraction");
        }
        int value = 0;
        for (size_t i = 0; i < digits; i++) {
            char c = begin[20 + i];
            if (c < '0' || c > '9') {
                return cf_db_failf(CF_INVALID,
                                   "invalid datetime fraction digit");
            }
            if (i < 6) value = value * 10 + (c - '0');
        }
        for (size_t i = digits < 6 ? digits : 6; i < 6; i++) {
            value *= 10;
        }
        micros = value;
    }

    int64_t seconds = days_from_civil(year, (unsigned)month, (unsigned)day) *
                          86400 +
                      hour * 3600 + minute * 60 + second;
    *out_us = seconds * 1000000 + micros;
    return CF_OK;
}

/* --- build info ---------------------------------------------------------- */

const char *cf_db_libversion(void) {
    return sqlite3_libversion();
}

cf_err cf_db_compile_options(cf_db *db, cf_buf **out) {
    if (out == NULL) return cf_db_failf(CF_INVALID, "no output pointer");
    *out = NULL;
    if (db == NULL) return cf_db_failf(CF_INVALID, "no database");
    if (!cf_db_thread_ok(db)) {
        return cf_db_failf(CF_INTERNAL,
                           "compile options read from a non-owning thread");
    }

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, "PRAGMA compile_options", -1, &stmt,
                                NULL);
    if (rc != SQLITE_OK) {
        cf_err err = cf_db_err(rc);
        return cf_db_failf(err, "PRAGMA compile_options: %s",
                           sqlite3_errmsg(db->handle));
    }

    cf_builder builder = {0};
    cf_err err = CF_OK;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        int len = sqlite3_column_bytes(stmt, 0);
        cf_span row = {text, (size_t)len};
        err = cf_builder_append(&builder, row);
        if (err == CF_OK) {
            static const unsigned char newline = '\n';
            cf_span nl = {&newline, 1};
            err = cf_builder_append(&builder, nl);
        }
        if (err != CF_OK) break;
    }
    if (err == CF_OK && rc != SQLITE_DONE) {
        err = cf_db_failf(cf_db_err(rc), "PRAGMA compile_options: %s",
                          sqlite3_errmsg(db->handle));
    }
    sqlite3_finalize(stmt);
    if (err != CF_OK) {
        cf_builder_dispose(&builder);
        return err;
    }
    return cf_builder_freeze(&builder, out);
}
