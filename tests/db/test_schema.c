/* DB-01: fresh schema executes in a temp dir; user_version; expected tables
 * and indexes present (including messages(room_id, created_at)); FTS5 virtual
 * table exists and is usable; foreign keys are enforced; version-mismatch and
 * missing-table opens fail.
 *
 * Acceptance: 07-verification.md DB-01; 02-data-auth.md D01.
 */
#include "cf_test.h"

#include "db/db_testutil.h"

static const char *const contract_tables[] = {
    "accounts",       "action_text_rich_texts",
    "active_storage_attachments", "active_storage_blobs",
    "active_storage_variant_records", "bans",
    "boosts",         "memberships",
    "message_search_index", "messages",
    "push_subscriptions", "rooms",
    "searches",       "sessions",
    "users",          "webhooks",
};
#define CONTRACT_TABLE_COUNT \
    (sizeof contract_tables / sizeof contract_tables[0])

static const char *const contract_indexes[] = {
    "index_accounts_on_singleton_guard",
    "index_action_text_rich_texts_uniqueness",
    "index_active_storage_attachments_on_blob_id",
    "index_active_storage_attachments_uniqueness",
    "index_active_storage_blobs_on_key",
    "index_active_storage_variant_records_uniqueness",
    "index_bans_on_ip_address",
    "index_bans_on_user_id",
    "index_boosts_on_booster_id",
    "index_boosts_on_message_id",
    "index_memberships_on_room_id",
    "index_memberships_on_room_id_and_created_at",
    "index_memberships_on_room_id_and_user_id",
    "index_memberships_on_user_id",
    "index_messages_on_creator_id",
    "index_messages_on_room_id",
    "index_messages_on_room_id_and_created_at",
    "index_push_subscriptions_on_user_id",
    "index_searches_on_user_id",
    "index_sessions_on_token",
    "index_sessions_on_user_id",
    "index_users_on_bot_token",
    "index_users_on_email_address",
    "index_webhooks_on_user_id",
    "idx_on_endpoint_p256dh_key_auth_key_7553014576",
};
#define CONTRACT_INDEX_COUNT \
    (sizeof contract_indexes / sizeof contract_indexes[0])

CF_TEST(schema_fresh_creates_tables_indexes_version_fts_and_fks) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    sqlite3 *handle = cf_db_handle(scratch.db);
    CF_REQUIRE(handle != NULL);

    bool ok = false;
    CF_CHECK(cf_db_test_i64(handle, "PRAGMA user_version", &ok) == 1 && ok);

    for (size_t i = 0; i < CONTRACT_TABLE_COUNT; i++) {
        if (!cf_db_test_has_object(handle, "table", contract_tables[i])) {
            CF_CHECK(!"missing contract table");
            printf("    missing table: %s\n", contract_tables[i]);
        }
    }
    for (size_t i = 0; i < CONTRACT_INDEX_COUNT; i++) {
        if (!cf_db_test_has_object(handle, "index", contract_indexes[i])) {
            CF_CHECK(!"missing contract index");
            printf("    missing index: %s\n", contract_indexes[i]);
        }
    }
    /* 15 CREATE TABLE + 1 virtual table + 5 FTS5 shadow tables. */
    CF_CHECK(cf_db_test_object_count(handle, "table") == 21);
    /* 25 CREATE INDEX; unique-constraint autoindexes are sqlite_autoindex_*. */
    CF_CHECK(cf_db_test_object_count(handle, "index") == 25);

    /* The paging index is the one D01 highlights: messages(room_id,
     * created_at).  Its columns are part of the created object. */
    bool index_ok = false;
    char index_sql[512];
    int64_t index_rows = cf_db_test_i64(
        handle,
        "SELECT count(*) FROM pragma_index_info("
        "'index_messages_on_room_id_and_created_at')",
        &index_ok);
    CF_CHECK(index_ok && index_rows == 2);
    CF_CHECK(cf_db_test_text(handle,
                             "SELECT sql FROM sqlite_master WHERE name="
                             "'index_messages_on_room_id_and_created_at'",
                             index_sql, sizeof index_sql) != NULL);
    CF_CHECK(strstr(index_sql, "\"room_id\"") != NULL &&
             strstr(index_sql, "\"created_at\"") != NULL);

    /* FTS5 virtual table is usable: insert rowid 1 and match with the porter
     * tokenizer. */
    CF_REQUIRE(cf_db_test_exec(
                   handle,
                   "INSERT INTO message_search_index(rowid, body) "
                   "VALUES (1, 'running dogs')") == SQLITE_OK);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(
                   handle,
                   "SELECT rowid FROM message_search_index "
                   "WHERE message_search_index MATCH 'run'",
                   -1, &stmt, NULL) == SQLITE_OK);
    CF_CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    CF_CHECK(sqlite3_column_int64(stmt, 0) == 1);
    sqlite3_finalize(stmt);

    /* Foreign keys are ON and enforced. */
    CF_CHECK(cf_db_test_i64(handle, "PRAGMA foreign_keys", &ok) == 1 && ok);
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

    CF_REQUIRE(sqlite3_prepare_v2(
                   handle,
                   "INSERT INTO messages (client_message_id, created_at, "
                   "creator_id, room_id, updated_at) VALUES ('c2', "
                   "'2026-01-01 00:00:00', 1, 999, '2026-01-01 00:00:00')",
                   -1, &stmt, NULL) == SQLITE_OK);
    int rc = sqlite3_step(stmt);
    CF_CHECK((rc & 0xff) == SQLITE_CONSTRAINT);
    CF_CHECK(rc == SQLITE_CONSTRAINT_FOREIGNKEY ||
             sqlite3_extended_errcode(handle) ==
                 SQLITE_CONSTRAINT_FOREIGNKEY);
    CF_CHECK(cf_db_err(rc) == CF_INVALID);
    sqlite3_finalize(stmt);

    cf_db_scratch_close(&scratch);
}

CF_TEST(schema_connection_settings_match_contract) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    sqlite3 *handle = cf_db_handle(scratch.db);
    CF_REQUIRE(handle != NULL);

    char text[64];
    bool ok = false;
    CF_CHECK(cf_db_test_text(handle, "PRAGMA journal_mode", text,
                             sizeof text) != NULL &&
             strcmp(text, "wal") == 0);
    CF_CHECK(cf_db_test_i64(handle, "PRAGMA synchronous", &ok) == 1 && ok);
    CF_CHECK(cf_db_test_i64(handle, "PRAGMA foreign_keys", &ok) == 1 && ok);
    CF_CHECK(cf_db_test_text(handle, "PRAGMA locking_mode", text,
                             sizeof text) != NULL &&
             strcmp(text, "normal") == 0);
    CF_CHECK(cf_db_test_i64(handle, "PRAGMA mmap_size", &ok) == 0 && ok);
    CF_CHECK(cf_db_test_i64(handle, "PRAGMA busy_timeout", &ok) == 1000 && ok);
    CF_CHECK(cf_db_test_i64(handle, "PRAGMA wal_autocheckpoint", &ok) ==
                 1000 &&
             ok);

    cf_db_scratch_close(&scratch);
}

CF_TEST(schema_reopen_validates_and_readers_are_query_only) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    cf_db_close(scratch.db); /* keep the file; reopen it read-only */
    scratch.db = NULL;

    cf_db *reader = NULL;
    CF_REQUIRE(cf_db_open(scratch.path, true, &reader) == CF_OK);
    CF_REQUIRE(reader != NULL);
    CF_CHECK(cf_db_thread_ok(reader));
    sqlite3 *handle = cf_db_handle(reader);
    bool ok = false;
    CF_CHECK(cf_db_test_i64(handle, "PRAGMA query_only", &ok) == 1 && ok);
    CF_CHECK(cf_db_test_i64(handle, "PRAGMA user_version", &ok) == 1 && ok);
    CF_CHECK(cf_db_test_object_count(handle, "table") == 21);
    /* A read-only connection refuses writes but still serves reads. */
    CF_CHECK(cf_db_test_exec(
                 handle,
                 "INSERT INTO users (created_at, name, updated_at) VALUES "
                 "('2026-01-01 00:00:00', 'X', '2026-01-01 00:00:00')") ==
             SQLITE_READONLY);
    CF_CHECK(cf_read_begin(reader) == CF_OK);
    CF_CHECK(cf_db_in_read(reader));
    CF_CHECK(cf_db_test_i64(handle, "SELECT count(*) FROM users", &ok) == 0 &&
             ok);
    CF_CHECK(cf_read_end(reader) == CF_OK);
    CF_CHECK(!cf_db_in_read(reader));
    cf_db_close(reader);
    cf_db_scratch_close(&scratch); /* db already NULL, removes the directory */
}

CF_TEST(schema_version_mismatch_fails_with_clear_message) {
    char *dir = cf_db_test_dir();
    CF_REQUIRE(dir != NULL);
    char path[CF_DB_TEST_PATH_CAP];
    cf_db_test_path(path, sizeof path, dir);

    /* A version-1 database bumped to version 2 must be rejected. */
    cf_db *db = NULL;
    CF_REQUIRE(cf_db_open(path, false, &db) == CF_OK);
    cf_db_close(db);

    sqlite3 *raw = NULL;
    CF_REQUIRE(sqlite3_open_v2(path, &raw,
                               SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                               NULL) == SQLITE_OK);
    CF_REQUIRE(cf_db_test_exec(raw, "PRAGMA user_version=2") == SQLITE_OK);
    sqlite3_close(raw);

    db = NULL;
    CF_CHECK(cf_db_open(path, false, &db) == CF_DB);
    CF_CHECK(db == NULL);
    CF_CHECK(strstr(cf_db_last_error(), "user_version") != NULL);

    /* An empty-but-versioned file is neither fresh nor version 1. */
    char path2[CF_DB_TEST_PATH_CAP];
    snprintf(path2, sizeof path2, "%s/other.db", dir);
    raw = NULL;
    CF_REQUIRE(sqlite3_open_v2(path2, &raw,
                               SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                               NULL) == SQLITE_OK);
    CF_REQUIRE(cf_db_test_exec(raw, "PRAGMA user_version=7") == SQLITE_OK);
    sqlite3_close(raw);
    db = NULL;
    CF_CHECK(cf_db_open(path2, false, &db) == CF_DB);
    CF_CHECK(db == NULL);
    CF_CHECK(strstr(cf_db_last_error(), "user_version") != NULL);

    cf_db_test_cleanup_named(dir, "other.db");
    cf_db_test_cleanup(dir);
    free(dir);
}

CF_TEST(schema_missing_table_fails) {
    char *dir = cf_db_test_dir();
    CF_REQUIRE(dir != NULL);
    char path[CF_DB_TEST_PATH_CAP];
    cf_db_test_path(path, sizeof path, dir);

    cf_db *db = NULL;
    CF_REQUIRE(cf_db_open(path, false, &db) == CF_OK);
    sqlite3 *handle = cf_db_handle(db);
    CF_REQUIRE(cf_db_test_exec(handle, "DROP TABLE webhooks") == SQLITE_OK);
    cf_db_close(db);

    db = NULL;
    CF_CHECK(cf_db_open(path, false, &db) == CF_DB);
    CF_CHECK(db == NULL);
    CF_CHECK(strstr(cf_db_last_error(), "webhooks") != NULL);

    /* Readers validate the same way. */
    db = NULL;
    CF_CHECK(cf_db_open(path, true, &db) == CF_DB);
    CF_CHECK(db == NULL);
    CF_CHECK(strstr(cf_db_last_error(), "webhooks") != NULL);

    cf_db_test_cleanup(dir);
    free(dir);
}

CF_TEST(schema_empty_read_only_fails_clearly) {
    char *dir = cf_db_test_dir();
    CF_REQUIRE(dir != NULL);
    char path[CF_DB_TEST_PATH_CAP];
    cf_db_test_path(path, sizeof path, dir);

    sqlite3 *raw = NULL;
    CF_REQUIRE(sqlite3_open_v2(path, &raw,
                               SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                               NULL) == SQLITE_OK);
    sqlite3_close(raw);

    cf_db *db = NULL;
    CF_CHECK(cf_db_open(path, true, &db) == CF_DB);
    CF_CHECK(db == NULL);
    CF_CHECK(strstr(cf_db_last_error(), "empty") != NULL);

    cf_db_test_cleanup(dir);
    free(dir);
}

CF_TEST(schema_not_a_database_fails) {
    char *dir = cf_db_test_dir();
    CF_REQUIRE(dir != NULL);
    char path[CF_DB_TEST_PATH_CAP];
    cf_db_test_path(path, sizeof path, dir);

    FILE *f = fopen(path, "wb");
    CF_REQUIRE(f != NULL);
    CF_REQUIRE(fwrite("this is not a sqlite database", 1, 29, f) == 29);
    CF_REQUIRE(fclose(f) == 0);

    cf_db *db = NULL;
    CF_CHECK(cf_db_open(path, false, &db) != CF_OK);
    CF_CHECK(db == NULL);
    CF_CHECK(cf_db_last_error()[0] != '\0');

    cf_db_test_cleanup(dir);
    free(dir);
}

CF_TEST_MAIN()
