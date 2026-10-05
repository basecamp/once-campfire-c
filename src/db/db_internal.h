/* Database-core internals: schema, reader, statements (D01 db-core).
 *
 * This is the db module's shared surface, included by model modules and by
 * D02's writer.c.  It is not part of the frozen application contract in
 * src/cf.h; cross-module declarations belong to the integrator.
 *
 * Rules carried from docs/devel/implementation/02-data-auth.md:
 *  - exactly one prepared-statement cache per connection, keyed by a fixed
 *    enum in the owning module; SQL is fixed, source-selected text only;
 *  - every statement use ends with cf_db_stmt_done (sqlite3_reset plus
 *    sqlite3_clear_bindings);
 *  - bindings copy their bytes (SQLITE_TRANSIENT), never SQLITE_STATIC;
 *  - row data is copied before the statement is reset;
 *  - a reader connection belongs to the thread that opened it: cf_read_begin
 *    and cf_read_end fail with CF_INTERNAL when called from another thread;
 *  - no read transaction may span cf_write: end the read transaction and
 *    reset statements before calling cf_write (00-contracts.md).
 */
#ifndef CF_DB_INTERNAL_H
#define CF_DB_INTERNAL_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sqlite3.h>

#include "cf.h"

/* Fresh-schema version recorded in PRAGMA user_version. */
#define CF_DB_USER_VERSION 1

/* Per-connection settings from 02-data-auth.md. */
#define CF_DB_BUSY_TIMEOUT_MS 1000
#define CF_DB_AUTOCHECKPOINT_PAGES 1000

/* A cf_db_error() message never exceeds this (including the NUL). */
#define CF_DB_ERROR_CAP 512

/* Longest cf_db_time_to_text output plus its NUL:
 * "YYYY-MM-DD HH:MM:SS.ffffff". */
#define CF_DB_TIME_TEXT_CAP 27

/* Statements prepared per connection.  A fixed enum in the owning module
 * indexes the module's cf_stmt_set; the cache is per connection. */
#define CF_DB_STMT_CACHE_CAPACITY 256

/* One fixed, source-selected SQL text. */
typedef struct {
    const char *sql;
} cf_stmt_def;

/* A module's fixed statement table; its local enum values (0..count-1) are
 * the cache keys.  Modules own their table; no SQL is assembled at runtime. */
typedef struct {
    const cf_stmt_def *defs;
    size_t count;
} cf_stmt_set;

typedef struct {
    const cf_stmt_set *set; /* NULL marks a free slot */
    size_t id;
    sqlite3_stmt *stmt;
} cf_stmt_slot;

struct cf_db {
    sqlite3 *handle;
    char *path;       /* copy of the opening path, for error messages */
    pthread_t owner;  /* thread that opened the connection */
    bool read_only;
    bool in_read;     /* cf_read_begin without cf_read_end */
    size_t stmt_count;
    cf_stmt_slot stmts[CF_DB_STMT_CACHE_CAPACITY];
};

/* --- errors and messages (reader.c) ------------------------------------ */

/* Map a SQLite result code to cf_err.  SQLITE_BUSY/LOCKED -> CF_BUSY,
 * SQLITE_CONSTRAINT -> CF_INVALID, SQLITE_NOMEM -> CF_NOMEM,
 * SQLITE_IOERR/CANTOPEN -> CF_IO, SQLITE_MISUSE -> CF_INTERNAL, other
 * failures -> CF_DB.  SQLITE_OK maps to CF_OK. */
cf_err cf_db_err(int sqlite_rc);

/* Message of the most recent failed cf_db_* call on this thread ("" if none).
 * The returned pointer is thread-local and valid until the next failed call. */
const char *cf_db_last_error(void);

/* Record a message for the current thread and return rc, so callers can
 * `return cf_db_failf(...)`. */
cf_err cf_db_failf(cf_err rc, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* --- connection (reader.c) --------------------------------------------- */

/* Open `path` and apply the 02-data-auth.md per-connection settings.  A
 * fresh database is created transactionally with user_version=1 when
 * read_only is false; an existing database is validated (user_version==1 and
 * every required table) and otherwise fails with CF_DB and a clear message.
 * read_only connections never create a database and set query_only.  On
 * success *out owns the connection; on failure *out stays NULL. */
cf_err cf_db_open(const char *path, bool read_only, cf_db **out);

/* Finalize the statement cache, end any open read transaction and close the
 * handle.  Accepts NULL; call from the opening thread. */
void cf_db_close(cf_db *db);

/* Borrowed handle for D02's writer transaction code; use only on the thread
 * that currently owns the connection (readers: the opening thread; writer:
 * whichever single thread D02 serializes it on). */
sqlite3 *cf_db_handle(cf_db *db);

/* True when the calling thread may use the connection: the opening thread
 * for a reader (02: "a reader belongs to one thread"); a writer connection is
 * serialized by D02 and may be used by one thread at a time. */
bool cf_db_thread_ok(const cf_db *db);

/* --- schema (schema.c) -------------------------------------------------- */

/* Create the fresh schema (one BEGIN IMMEDIATE transaction containing the
 * exact contracts/schema.sql text and PRAGMA user_version=1), or validate an
 * existing database: user_version==1 and all required tables present.  Any
 * other state fails with CF_DB and a message in cf_db_last_error(). */
cf_err cf_db_schema_ensure(cf_db *db);

/* --- read transactions (reader.c) --------------------------------------- */

/* BEGIN a deferred read transaction / COMMIT it.  A read transaction must
 * not span cf_write: end it (and reset statements) before calling the
 * writer.  Fails CF_INTERNAL when called from a non-owning thread or when
 * the begin/end pairing is wrong. */
cf_err cf_read_begin(cf_db *db);
cf_err cf_read_end(cf_db *db);
bool cf_db_in_read(const cf_db *db);

/* --- datetime text <-> UTC microseconds (reader.c) ---------------------- */

/* Format/parse the reference UTC SQL text: "YYYY-MM-DD HH:MM:SS" plus
 * ".ffffff" when the microseconds are non-zero, exactly as the pinned
 * Rust Timestamp::to_db (Rails datetime(6) quoting) writes it.  Parsing also
 * accepts a 'T' separator, a trailing 'Z' or ' UTC', and fewer fraction
 * digits (right-padded to microseconds).  out must hold
 * CF_DB_TIME_TEXT_CAP bytes. */
cf_err cf_db_time_to_text(int64_t us, char out[CF_DB_TIME_TEXT_CAP]);
cf_err cf_db_time_from_text(cf_span text, int64_t *out_us);

/* --- build info (reader.c) ---------------------------------------------- */

/* sqlite3_libversion() of the linked amalgamation. */
const char *cf_db_libversion(void);

/* Owned copy of the connection's PRAGMA compile_options rows, one per line.
 * ENABLE_FTS5 and THREADSAFE=1 are visible in the tested build. */
cf_err cf_db_compile_options(cf_db *db, cf_buf **out);

/* --- statements (statements.c) ------------------------------------------ */

/* Prepared statement `id` of `set` on this connection, prepared once and
 * reused.  The statement stays owned by the connection's cache: after use
 * call cf_db_stmt_done (reset + clear bindings) and never finalize it
 * yourself.  Fails CF_INTERNAL for an unknown id, CF_LIMIT when the cache is
 * full, CF_DB for invalid SQL. */
cf_err cf_db_stmt(cf_db *db, const cf_stmt_set *set, size_t id,
                  sqlite3_stmt **out);

/* sqlite3_reset + sqlite3_clear_bindings; accepts NULL. */
void cf_db_stmt_done(sqlite3_stmt *stmt);

/* Finalize every cached statement (connection close / shutdown). */
void cf_db_stmt_cache_clear(cf_db *db);

/* Bind helpers.  Text is copied (SQLITE_TRANSIENT) so the span may die at
 * any time after the call.  bind_text with an empty span binds empty text,
 * not NULL; use bind_null / bind_opt_* for absent values. */
cf_err cf_stmt_bind_i64(sqlite3_stmt *stmt, int index, int64_t value);
cf_err cf_stmt_bind_null(sqlite3_stmt *stmt, int index);
cf_err cf_stmt_bind_text(sqlite3_stmt *stmt, int index, cf_span text);
cf_err cf_stmt_bind_opt_i64(sqlite3_stmt *stmt, int index, cf_optional_i64 value);
cf_err cf_stmt_bind_opt_text(sqlite3_stmt *stmt, int index, bool present,
                             cf_span text);

/* Column readers for the current row.  is_null distinguishes SQL NULL from
 * empty; the text span borrows SQLite's buffer until the next step/reset, so
 * copy it before cf_db_stmt_done (cf_stmt_column_copy_text does). */
bool cf_stmt_column_is_null(sqlite3_stmt *stmt, int column);
int64_t cf_stmt_column_i64(sqlite3_stmt *stmt, int column);
cf_span cf_stmt_column_text(sqlite3_stmt *stmt, int column);

/* Copy the column into an owned buffer: NULL column leaves *out NULL (absent)
 * while an empty string yields a zero-length buffer (empty).  The copy
 * survives reset/finalize. */
cf_err cf_stmt_column_copy_text(sqlite3_stmt *stmt, int column, cf_buf **out);

#endif /* CF_DB_INTERNAL_H */
