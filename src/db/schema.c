/* Fresh-schema creation and existing-database validation (D01 db-core).
 *
 * docs/devel/implementation/02-data-auth.md D01: contracts/schema.sql is the
 * complete fresh schema; a missing database is created transactionally with
 * PRAGMA user_version=1, and opening an existing database requires
 * user_version==1 and the required tables.  There are no migrations, no
 * rollback tooling and no Rails upgrade path.
 *
 * schema_sql.h is generated from the contract by tests/db/gen_schema_sql.py;
 * CF_DB_SCHEMA_SQL is the contract text byte for byte, including its final
 * `PRAGMA user_version = 1;`.
 */
#include "db/db_internal.h"
#include "db/schema_sql.h"

#include <stdint.h>
#include <string.h>

/* Every table the version 1 contract creates, message_search_index included
 * (it is a virtual table and appears in sqlite_master as type='table'). */
static const char *const cf_required_tables[] = {
    "accounts",
    "action_text_rich_texts",
    "active_storage_attachments",
    "active_storage_blobs",
    "active_storage_variant_records",
    "bans",
    "boosts",
    "memberships",
    "message_search_index",
    "messages",
    "push_subscriptions",
    "rooms",
    "searches",
    "sessions",
    "users",
    "webhooks",
};

#define CF_REQUIRED_TABLE_COUNT \
    (sizeof cf_required_tables / sizeof cf_required_tables[0])

/* Run one SQL statement, reporting failure with SQLite's message.  `what`
 * names the failing step, not the SQL text. */
static cf_err exec_step(sqlite3 *handle, const char *sql, const char *what) {
    char *message = NULL;
    int rc = sqlite3_exec(handle, sql, NULL, NULL, &message);
    if (rc == SQLITE_OK) return CF_OK;
    cf_err err = cf_db_err(rc);
    cf_db_failf(err, "%s: %s", what,
                message != NULL ? message : sqlite3_errmsg(handle));
    sqlite3_free(message);
    return err;
}

/* Run a one-row, one-column integer query. */
static cf_err query_i64(sqlite3 *handle, const char *sql, const char *what,
                        int64_t *out) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return cf_db_failf(cf_db_err(rc), "%s: %s", what,
                           sqlite3_errmsg(handle));
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        cf_err err = rc == SQLITE_DONE ? CF_DB : cf_db_err(rc);
        cf_db_failf(err, "%s: %s", what, sqlite3_errmsg(handle));
        sqlite3_finalize(stmt);
        return err;
    }
    *out = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return CF_OK;
}

static cf_err count_schema_objects(sqlite3 *handle, int64_t *out) {
    return query_i64(handle,
                     "SELECT count(*) FROM sqlite_master "
                     "WHERE name NOT LIKE 'sqlite_%'",
                     "count schema objects", out);
}

static cf_err require_table(sqlite3 *handle, const char *table) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        handle,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1", -1,
        &stmt, NULL);
    if (rc != SQLITE_OK) {
        return cf_db_failf(cf_db_err(rc), "schema check: %s",
                           sqlite3_errmsg(handle));
    }
    rc = sqlite3_bind_text(stmt, 1, table, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    bool found = rc == SQLITE_ROW;
    if (!found && rc != SQLITE_DONE) {
        cf_err err = cf_db_err(rc);
        cf_db_failf(err, "schema check for table \"%s\": %s", table,
                    sqlite3_errmsg(handle));
        sqlite3_finalize(stmt);
        return err;
    }
    sqlite3_finalize(stmt);
    if (!found) {
        return cf_db_failf(
            CF_DB, "database is missing required table \"%s\" "
                   "(incomplete version %d schema)",
            table, CF_DB_USER_VERSION);
    }
    return CF_OK;
}

static cf_err validate_existing(cf_db *db) {
    int64_t user_version = 0;
    cf_err err = query_i64(db->handle, "PRAGMA user_version",
                           "read user_version", &user_version);
    if (err != CF_OK) return err;
    if (user_version != CF_DB_USER_VERSION) {
        return cf_db_failf(
            CF_DB,
            "database \"%s\" has user_version=%lld; this build requires "
            "version %d and cannot migrate or upgrade an existing file",
            db->path, (long long)user_version, CF_DB_USER_VERSION);
    }
    for (size_t i = 0; i < CF_REQUIRED_TABLE_COUNT; i++) {
        err = require_table(db->handle, cf_required_tables[i]);
        if (err != CF_OK) return err;
    }
    return CF_OK;
}

#define CF_REFRESH_INDEX_SQL \
    "CREATE INDEX IF NOT EXISTS index_messages_on_room_id_and_updated_at " \
    "ON messages(room_id,updated_at)"

static cf_err create_fresh(cf_db *db) {
    cf_err err = exec_step(db->handle, "BEGIN IMMEDIATE", "BEGIN IMMEDIATE");
    if (err != CF_OK) return err;

    err = exec_step(db->handle, CF_DB_SCHEMA_SQL, "execute schema DDL");
    if (err == CF_OK) err = exec_step(db->handle, CF_REFRESH_INDEX_SQL, "ensure refresh index");
    if (err == CF_OK) {
        int64_t user_version = 0;
        err = query_i64(db->handle, "PRAGMA user_version",
                        "verify user_version after schema creation",
                        &user_version);
        if (err == CF_OK && user_version != CF_DB_USER_VERSION) {
            err = cf_db_failf(CF_DB,
                              "schema creation left user_version=%lld, "
                              "expected %d",
                              (long long)user_version, CF_DB_USER_VERSION);
        }
    }
    if (err == CF_OK) {
        err = exec_step(db->handle, "COMMIT", "COMMIT schema creation");
        if (err == CF_OK) return CF_OK;
    }

    sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL);
    return err;
}

cf_err cf_db_schema_ensure(cf_db *db) {
    if (db == NULL) return cf_db_failf(CF_INVALID, "no database");
    if (!cf_db_thread_ok(db)) {
        return cf_db_failf(CF_INTERNAL,
                           "schema check from a non-owning thread");
    }

    int64_t user_version = 0;
    cf_err err = query_i64(db->handle, "PRAGMA user_version",
                           "read user_version", &user_version);
    if (err != CF_OK) return err;

    int64_t objects = 0;
    err = count_schema_objects(db->handle, &objects);
    if (err != CF_OK) return err;

    if (objects == 0) {
        if (user_version != 0) {
            return cf_db_failf(
                CF_DB,
                "database \"%s\" has no schema objects but user_version=%lld; "
                "expected a fresh file or version %d",
                db->path, (long long)user_version, CF_DB_USER_VERSION);
        }
        if (db->read_only) {
            return cf_db_failf(
                CF_DB,
                "database \"%s\" is empty and this connection is read-only; "
                "open it for writing to create the version %d schema",
                db->path, CF_DB_USER_VERSION);
        }
        return create_fresh(db);
    }

    err = validate_existing(db);
    if (err == CF_OK && !db->read_only) {
        err = exec_step(db->handle,
                       CF_REFRESH_INDEX_SQL, "ensure refresh index");
    }
    return err;
}
