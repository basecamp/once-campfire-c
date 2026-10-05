/* D01 model tests: active_storage (Blob, Attachment).
 *
 * Source: tmp/rust-ref/crates/db/src/models/active_storage.rs.
 * Oracle: the pinned reference tests that exercise these functions
 * (tests/fixtures/crates/db/src/tests/message_test.rs uses Blob::create with
 * key "abc123" / filename "moon.jpg" / service "local" and checks the row as
 * stored: assigned id/created_at from tx.now()).
 *
 * Every implemented function has an ordinary path and an edge/failure path:
 *   cf_blob_find           raw rows incl. NULL vs present optionals + reader
 *                          connection + CF_NOT_FOUND + copy-before-reset
 *   cf_blob_create         assigned id/clock, stored datetime text,
 *                          NULL/empty optionals, duplicate-key CF_INVALID
 *   cf_attachment_find_for found and all three not-found dimensions
 *   cf_attachment_create   assigned id/clock, FK failure, unique failure
 *   cf_attachment_delete   removes exactly one row, idempotent on absent id
 *   cf_attachment_blob     blob match, dangling blob_id CF_NOT_FOUND
 *   dispose helpers        NULL/empty-safe, leak-free under ASan
 *
 * Mutations run through D02's real cf_write (BEGIN IMMEDIATE on the writer
 * connection, like the rich_text_record/boost model tests); reads run on a
 * query_only reader connection.  Deterministic time uses the F01 test clock
 * (src/core/testclock.h), the same injection the reference TestDb clock uses
 * (`t.clock.travel_to`): no sleeps.
 *
 * Build (plain):
 *   clang -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra
 *         -Werror -pthread -Isrc -Itests
 *         -Ivendor/src/sqlite/sqlite-amalgamation-3530400 -Ivendor/src/yyjson/src
 *         tests/models/active_storage_test.c src/models/active_storage.c
 *         src/core/alloc.c src/core/buffer.c src/core/clock.c
 *         src/core/error.c src/core/random.c src/config.c src/app.c
 *         src/db/schema.c src/db/reader.c src/db/statements.c src/db/writer.c
 *         vendor/build/sqlite-clang/libsqlite3.a
 *         vendor/build/yyjson-clang/libyyjson.a -lm
 *         -o build/d01-models/plain/active_storage_test
 */
#include "cf_test.h"

#include "app.h"
#include "config.h"
#include "core/testclock.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "db/writer.h"
#include "models/active_storage.h"

#include <stdlib.h>
#include <string.h>

/* The shared release helpers (cf_str_dispose / cf_optional_str_dispose) are
 * defined once in src/models/types.c. */

/* --- borrowed literal views for inputs (owned only for the call) ---------- */

static cf_str text(const char *bytes) {
    cf_str value = {(char *)bytes, strlen(bytes)};
    return value;
}

static cf_optional_str optional(const char *bytes) {
    cf_optional_str value = {true, text(bytes)};
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

static void check_absent(cf_optional_str value) {
    CF_CHECK(!value.present);
    CF_CHECK(value.value.ptr == NULL);
    CF_CHECK(value.value.len == 0);
}

static void check_present(cf_optional_str value, const char *expected) {
    CF_CHECK(value.present);
    if (value.present) check_str(value.value, expected);
}

static bool blob_matches(const cf_blob *a, const cf_blob *b) {
    return a->id == b->id && a->byte_size == b->byte_size &&
           a->created_at == b->created_at && a->key.len == b->key.len &&
           memcmp(a->key.ptr, b->key.ptr, a->key.len) == 0 &&
           a->filename.len == b->filename.len &&
           memcmp(a->filename.ptr, b->filename.ptr, a->filename.len) == 0 &&
           a->service_name.len == b->service_name.len &&
           memcmp(a->service_name.ptr, b->service_name.ptr,
                  a->service_name.len) == 0 &&
           a->content_type.present == b->content_type.present &&
           a->metadata.present == b->metadata.present &&
           a->checksum.present == b->checksum.present &&
           (!a->content_type.present ||
            (a->content_type.value.len == b->content_type.value.len &&
             memcmp(a->content_type.value.ptr, b->content_type.value.ptr,
                    a->content_type.value.len) == 0)) &&
           (!a->metadata.present ||
            (a->metadata.value.len == b->metadata.value.len &&
             memcmp(a->metadata.value.ptr, b->metadata.value.ptr,
                    a->metadata.value.len) == 0)) &&
           (!a->checksum.present ||
            (a->checksum.value.len == b->checksum.value.len &&
             memcmp(a->checksum.value.ptr, b->checksum.value.ptr,
                    a->checksum.value.len) == 0));
}

/* --- scratch database + app with D02's writer ----------------------------- */

#define AS_SECRET_HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define AS_ORIGIN "http://127.0.0.1:32123"

typedef struct {
    char *dir;
    char path[CF_DB_TEST_PATH_CAP];
    cf_app *app;
} as_world;

static void as_world_open(as_world *world) {
    memset(world, 0, sizeof *world);
    world->dir = cf_db_test_dir();
    CF_REQUIRE(world->dir != NULL);
    cf_db_test_path(world->path, sizeof world->path, world->dir);

    /* Fresh schema (D-C01) so the writer opens an existing version-1 file. */
    cf_db *db = NULL;
    CF_REQUIRE(cf_db_open(world->path, false, &db) == CF_OK);
    cf_db_close(db);

    cf_config_entry entries[3] = {
        {"PUBLIC_ORIGIN", AS_ORIGIN},
        {"SECRET_KEY_BASE", AS_SECRET_HEX64},
        {"DATABASE_PATH", world->path},
    };
    cf_config *config = NULL;
    CF_REQUIRE(cf_config_parse(entries, 3, NULL, &config) == CF_OK);
    CF_REQUIRE(cf_app_create(config, &world->app) == CF_OK);
    CF_REQUIRE(cf_writer_start(world->app, config) == CF_OK);
}

static void as_world_dispose(as_world *world) {
    cf_writer_stop(world->app);
    cf_app_destroy(world->app);
    world->app = NULL;
    if (world->dir != NULL) {
        cf_db_test_cleanup(world->dir);
        free(world->dir);
        world->dir = NULL;
    }
}

/* Reads run on a query_only connection owned by the test thread. */
static cf_db *as_reader(const as_world *world) {
    cf_db *db = NULL;
    CF_REQUIRE(cf_db_open(world->path, true, &db) == CF_OK);
    return db;
}

/* A second writer connection, used only to seed raw rows and to run raw
 * assertions; no read transaction is ever open across cf_write. */
static cf_db *as_scratch_writer(const as_world *world) {
    cf_db *db = NULL;
    CF_REQUIRE(cf_db_open(world->path, false, &db) == CF_OK);
    return db;
}

static int64_t count_rows(cf_db *db, const char *sql) {
    bool ok = false;
    int64_t count = cf_db_test_i64(cf_db_handle(db), sql, &ok);
    CF_CHECK(ok);
    return ok ? count : -1;
}

/* --- mutation callbacks (cf_write runs them on the writer thread) --------- */

typedef struct {
    const cf_blob *input;
    cf_blob *out;
} blob_write_arg;

static cf_err write_blob(cf_tx *tx, void *arg) {
    const blob_write_arg *write = arg;
    return cf_blob_create(tx, write->input, write->out);
}

typedef struct {
    cf_str record_type;
    int64_t record_id;
    cf_str name;
    int64_t blob_id;
    cf_attachment *out;
} attachment_write_arg;

static cf_err write_attachment(cf_tx *tx, void *arg) {
    const attachment_write_arg *write = arg;
    return cf_attachment_create(tx, write->record_type, write->record_id,
                                write->name, write->blob_id, write->out);
}

typedef struct {
    const cf_attachment *attachment;
} attachment_delete_arg;

static cf_err write_delete_attachment(cf_tx *tx, void *arg) {
    const attachment_delete_arg *write = arg;
    return cf_attachment_delete(tx, write->attachment);
}

/* --- cf_blob_create / cf_blob_find ---------------------------------------- */

CF_TEST(blob_create_assigns_id_and_clock_time) {
    as_world world;
    as_world_open(&world);

    /* The pinned reference test truncates nanoseconds; a fixed microsecond
     * clock is the exact reference `tx.now()` value. */
    cf_test_clock_set_fixed_us(1700000000123456LL);

    /* Fixture-shaped input from message_test.rs: key abc123, filename
     * moon.jpg, service local; id/created_at are ignored and assigned. */
    cf_blob input;
    memset(&input, 0, sizeof input);
    input.id = 777;
    input.created_at = 1;
    input.key = text("abc123");
    input.filename = text("moon.jpg");
    input.service_name = text("local");
    input.byte_size = 4096;
    input.content_type = optional("image/jpeg");
    input.metadata = optional("{\"identified\":true,\"width\":800}");
    input.checksum = optional("md5:deadbeef");

    cf_blob created;
    memset(&created, 0, sizeof created);
    blob_write_arg write = {&input, &created};
    CF_REQUIRE(cf_write(world.app, write_blob, &write) == CF_OK);

    CF_CHECK(created.id > 0);
    CF_CHECK(created.id != 777);
    CF_CHECK(created.created_at == 1700000000123456LL);
    CF_CHECK(created.byte_size == 4096);
    check_str(created.key, "abc123");
    check_str(created.filename, "moon.jpg");
    check_str(created.service_name, "local");
    check_present(created.content_type, "image/jpeg");
    check_present(created.metadata, "{\"identified\":true,\"width\":800}");
    check_present(created.checksum, "md5:deadbeef");

    /* The returned record is the row as stored (reference
     * create_returns_the_row_as_stored): find must reproduce it. */
    cf_db *reader = as_reader(&world);
    cf_blob found;
    memset(&found, 0, sizeof found);
    CF_REQUIRE(cf_blob_find(reader, created.id, &found) == CF_OK);
    CF_CHECK(blob_matches(&created, &found));

    /* Datetime storage representation: six fraction digits only when the
     * microseconds are non-zero. */
    char stored[64];
    CF_REQUIRE(cf_db_test_text(cf_db_handle(reader),
                               "SELECT created_at FROM active_storage_blobs "
                               "LIMIT 1",
                               stored, sizeof stored) != NULL);
    CF_CHECK(strcmp(stored, "2023-11-14 22:13:20.123456") == 0);

    CF_CHECK(count_rows(reader, "SELECT count(*) FROM active_storage_blobs") ==
             1);

    cf_blob_dispose(&created);
    cf_blob_dispose(&found);
    cf_db_close(reader);
    cf_test_clock_clear();
    as_world_dispose(&world);
}

CF_TEST(blob_create_null_optionals_and_zero_fraction) {
    as_world world;
    as_world_open(&world);
    cf_test_clock_set_fixed_us(1900000000000000LL);

    cf_blob input;
    memset(&input, 0, sizeof input);
    input.key = text("nulls");
    input.filename = text("blob.bin");
    input.service_name = text("local");
    input.byte_size = 1;
    /* content_type, checksum and metadata stay absent/NULL. */

    cf_blob created;
    memset(&created, 0, sizeof created);
    blob_write_arg write = {&input, &created};
    CF_REQUIRE(cf_write(world.app, write_blob, &write) == CF_OK);
    CF_CHECK(created.id > 0);
    CF_CHECK(created.created_at == 1900000000000000LL);
    check_absent(created.content_type);
    check_absent(created.checksum);
    check_absent(created.metadata);

    cf_db *reader = as_reader(&world);
    cf_blob found;
    memset(&found, 0, sizeof found);
    CF_REQUIRE(cf_blob_find(reader, created.id, &found) == CF_OK);
    check_absent(found.content_type);
    check_absent(found.checksum);
    check_absent(found.metadata);

    /* Zero microseconds: no ".000000" suffix in the stored text. */
    char stored[64];
    CF_REQUIRE(cf_db_test_text(
                   cf_db_handle(reader),
                   "SELECT created_at FROM active_storage_blobs WHERE key = "
                   "'nulls'",
                   stored, sizeof stored) != NULL);
    CF_CHECK(strcmp(stored, "2030-03-17 17:46:40") == 0);

    CF_CHECK(count_rows(reader,
                        "SELECT count(*) FROM active_storage_blobs WHERE "
                        "content_type IS NULL AND checksum IS NULL AND "
                        "metadata IS NULL") == 1);

    /* An empty present string is stored as empty text, not NULL: absent and
     * empty must stay distinguishable through find. */
    cf_blob empty_input;
    memset(&empty_input, 0, sizeof empty_input);
    empty_input.key = text("empty-meta");
    empty_input.filename = text("blob.bin");
    empty_input.service_name = text("local");
    empty_input.byte_size = 2;
    empty_input.metadata = optional("");
    cf_blob empty;
    memset(&empty, 0, sizeof empty);
    blob_write_arg empty_write = {&empty_input, &empty};
    CF_REQUIRE(cf_write(world.app, write_blob, &empty_write) == CF_OK);

    cf_blob empty_found;
    memset(&empty_found, 0, sizeof empty_found);
    CF_REQUIRE(cf_blob_find(reader, empty.id, &empty_found) == CF_OK);
    check_present(empty_found.metadata, "");
    CF_CHECK(empty_found.metadata.value.ptr != NULL);
    CF_CHECK(empty_found.metadata.value.len == 0);
    check_absent(empty_found.content_type);
    CF_CHECK(count_rows(reader,
                        "SELECT count(*) FROM active_storage_blobs WHERE "
                        "metadata = ''") == 1);

    cf_blob_dispose(&created);
    cf_blob_dispose(&found);
    cf_blob_dispose(&empty);
    cf_blob_dispose(&empty_found);
    cf_db_close(reader);
    cf_test_clock_clear();
    as_world_dispose(&world);
}

CF_TEST(blob_create_duplicate_key_is_invalid) {
    as_world world;
    as_world_open(&world);

    cf_blob input;
    memset(&input, 0, sizeof input);
    input.key = text("dup-key");
    input.filename = text("one.bin");
    input.service_name = text("local");

    cf_blob first;
    memset(&first, 0, sizeof first);
    blob_write_arg first_write = {&input, &first};
    CF_REQUIRE(cf_write(world.app, write_blob, &first_write) == CF_OK);

    input.filename = text("two.bin");
    input.byte_size = 9;
    cf_blob second;
    memset(&second, 0, sizeof second);
    blob_write_arg second_write = {&input, &second};
    /* The unique index index_active_storage_blobs_on_key maps to CF_INVALID
     * (D01 db-core error mapping); the writer rolls the failed insert back. */
    CF_CHECK(cf_write(world.app, write_blob, &second_write) == CF_INVALID);
    CF_CHECK(second.id == 0);
    CF_CHECK(second.key.ptr == NULL && second.filename.ptr == NULL);

    cf_db *reader = as_reader(&world);
    CF_CHECK(count_rows(reader, "SELECT count(*) FROM active_storage_blobs") ==
             1);

    cf_blob_dispose(&first);
    cf_db_close(reader);
    cf_test_clock_clear();
    as_world_dispose(&world);
}

CF_TEST(blob_find_reads_raw_rows_and_not_found) {
    as_world world;
    as_world_open(&world);

    /* Raw rows (not produced by create): NULL vs present optionals, fraction
     * and no-fraction datetime text, long filename. */
    cf_db *seed = as_scratch_writer(&world);
    CF_REQUIRE(cf_db_test_exec(
                   cf_db_handle(seed),
                   "INSERT INTO active_storage_blobs (id, byte_size, checksum, "
                   "content_type, created_at, filename, key, metadata, "
                   "service_name) VALUES "
                   "(7, 1234, NULL, NULL, '2023-11-14 22:13:20.123456', "
                   "'report final.pdf', 'key-seven', NULL, 'local'),"
                   "(8, 55, 'sha256:abc', 'text/plain', "
                   "'2026-01-02 03:04:05', 'note.txt', 'key-eight', '{}', "
                   "'s3')") == SQLITE_OK);
    cf_db_close(seed);

    cf_db *reader = as_reader(&world);
    cf_blob first;
    memset(&first, 0, sizeof first);
    CF_REQUIRE(cf_blob_find(reader, 7, &first) == CF_OK);
    CF_CHECK(first.id == 7);
    CF_CHECK(first.byte_size == 1234);
    CF_CHECK(first.created_at == 1700000000123456LL);
    check_str(first.key, "key-seven");
    check_str(first.filename, "report final.pdf");
    check_str(first.service_name, "local");
    check_absent(first.checksum);
    check_absent(first.content_type);
    check_absent(first.metadata);

    cf_blob second;
    memset(&second, 0, sizeof second);
    CF_REQUIRE(cf_blob_find(reader, 8, &second) == CF_OK);
    CF_CHECK(second.id == 8);
    CF_CHECK(second.byte_size == 55);
    CF_CHECK(second.created_at == 1767323045000000LL);
    check_present(second.checksum, "sha256:abc");
    check_present(second.content_type, "text/plain");
    check_present(second.metadata, "{}");

    /* Copy-before-reset: the first record's bytes must survive the second
     * statement use (ASan catches a dangling copy). */
    check_str(first.key, "key-seven");
    check_str(first.filename, "report final.pdf");

    /* Absent row: CF_NOT_FOUND with an empty record, not an error. */
    cf_blob missing;
    memset(&missing, 0, sizeof missing);
    CF_CHECK(cf_blob_find(reader, 999999, &missing) == CF_NOT_FOUND);
    CF_CHECK(missing.id == 0);
    CF_CHECK(missing.key.ptr == NULL && missing.key.len == 0);
    CF_CHECK(missing.filename.ptr == NULL);

    cf_blob_dispose(&first);
    cf_blob_dispose(&second);
    cf_db_close(reader);
    as_world_dispose(&world);
}

/* --- cf_attachment_create / cf_attachment_find_for ------------------------ */

CF_TEST(attachment_create_and_find_for) {
    as_world world;
    as_world_open(&world);
    cf_test_clock_set_fixed_us(1700000000123456LL);

    cf_blob blob;
    memset(&blob, 0, sizeof blob);
    blob.key = text("attachment-key");
    blob.filename = text("moon.jpg");
    blob.service_name = text("local");
    cf_blob stored;
    memset(&stored, 0, sizeof stored);
    blob_write_arg blob_write = {&blob, &stored};
    CF_REQUIRE(cf_write(world.app, write_blob, &blob_write) == CF_OK);

    cf_attachment created;
    memset(&created, 0, sizeof created);
    attachment_write_arg write = {CF_STR_LIT("Message"), 42,
                                  CF_STR_LIT("attachment"), stored.id,
                                  &created};
    CF_REQUIRE(cf_write(world.app, write_attachment, &write) == CF_OK);
    CF_CHECK(created.id > 0);
    CF_CHECK(created.record_id == 42);
    CF_CHECK(created.blob_id == stored.id);
    CF_CHECK(created.created_at == 1700000000123456LL);
    check_str(created.name, "attachment");
    check_str(created.record_type, "Message");

    cf_db *reader = as_reader(&world);
    bool found = false;
    cf_attachment read;
    memset(&read, 0, sizeof read);
    CF_REQUIRE(cf_attachment_find_for(reader, CF_STR_LIT("Message"), 42,
                                      CF_STR_LIT("attachment"), &found,
                                      &read) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(read.id == created.id);
    CF_CHECK(read.record_id == created.record_id);
    CF_CHECK(read.blob_id == created.blob_id);
    CF_CHECK(read.created_at == created.created_at);
    check_str(read.name, "attachment");
    check_str(read.record_type, "Message");
    cf_attachment_dispose(&read);

    /* None of the three predicate dimensions may match loosely. */
    const struct {
        const char *record_type;
        int64_t record_id;
        const char *name;
    } misses[] = {
        {"Room", 42, "attachment"},
        {"Message", 43, "attachment"},
        {"Message", 42, "avatar"},
    };
    for (size_t i = 0; i < sizeof misses / sizeof misses[0]; i++) {
        found = true;
        cf_attachment miss;
        memset(&miss, 0, sizeof miss);
        CF_REQUIRE(cf_attachment_find_for(reader, text(misses[i].record_type),
                                          misses[i].record_id,
                                          text(misses[i].name), &found,
                                          &miss) == CF_OK);
        CF_CHECK(!found);
        CF_CHECK(miss.id == 0);
        CF_CHECK(miss.name.ptr == NULL && miss.record_type.ptr == NULL);
    }

    cf_attachment_dispose(&created);
    cf_blob_dispose(&stored);
    cf_db_close(reader);
    cf_test_clock_clear();
    as_world_dispose(&world);
}

CF_TEST(attachment_create_requires_existing_blob) {
    as_world world;
    as_world_open(&world);

    cf_attachment created;
    memset(&created, 0, sizeof created);
    attachment_write_arg write = {CF_STR_LIT("Message"), 1,
                                  CF_STR_LIT("attachment"), 987654, &created};
    /* foreign_keys=ON: a blob_id without a blob row fails the FK and maps to
     * CF_INVALID (D01 db-core error mapping). */
    CF_CHECK(cf_write(world.app, write_attachment, &write) == CF_INVALID);
    CF_CHECK(created.id == 0);
    CF_CHECK(created.name.ptr == NULL && created.record_type.ptr == NULL);

    cf_db *reader = as_reader(&world);
    CF_CHECK(count_rows(reader,
                        "SELECT count(*) FROM active_storage_attachments") ==
             0);

    cf_db_close(reader);
    cf_test_clock_clear();
    as_world_dispose(&world);
}

CF_TEST(attachment_create_duplicate_is_invalid) {
    as_world world;
    as_world_open(&world);

    cf_blob blob;
    memset(&blob, 0, sizeof blob);
    blob.key = text("dup-attachment-key");
    blob.filename = text("file.bin");
    blob.service_name = text("local");
    cf_blob stored;
    memset(&stored, 0, sizeof stored);
    blob_write_arg blob_write = {&blob, &stored};
    CF_REQUIRE(cf_write(world.app, write_blob, &blob_write) == CF_OK);

    cf_attachment first;
    memset(&first, 0, sizeof first);
    attachment_write_arg first_write = {CF_STR_LIT("Message"), 5,
                                        CF_STR_LIT("attachment"), stored.id,
                                        &first};
    CF_REQUIRE(cf_write(world.app, write_attachment, &first_write) == CF_OK);

    /* Unique (record_type, record_id, name, blob_id) violation. */
    cf_attachment duplicate;
    memset(&duplicate, 0, sizeof duplicate);
    attachment_write_arg duplicate_write = {
        CF_STR_LIT("Message"), 5, CF_STR_LIT("attachment"), stored.id,
        &duplicate};
    CF_CHECK(cf_write(world.app, write_attachment, &duplicate_write) ==
             CF_INVALID);
    CF_CHECK(duplicate.id == 0);

    /* A different name is a different attachment. */
    cf_attachment other;
    memset(&other, 0, sizeof other);
    attachment_write_arg other_write = {CF_STR_LIT("Message"), 5,
                                        CF_STR_LIT("avatar"), stored.id,
                                        &other};
    CF_CHECK(cf_write(world.app, write_attachment, &other_write) == CF_OK);

    cf_db *reader = as_reader(&world);
    CF_CHECK(count_rows(reader,
                        "SELECT count(*) FROM active_storage_attachments") ==
             2);

    cf_attachment_dispose(&first);
    cf_attachment_dispose(&other);
    cf_blob_dispose(&stored);
    cf_db_close(reader);
    cf_test_clock_clear();
    as_world_dispose(&world);
}

/* --- cf_attachment_delete -------------------------------------------------- */

CF_TEST(attachment_delete_removes_row_and_is_idempotent) {
    as_world world;
    as_world_open(&world);

    cf_blob blob;
    memset(&blob, 0, sizeof blob);
    blob.key = text("delete-key");
    blob.filename = text("file.bin");
    blob.service_name = text("local");
    cf_blob stored;
    memset(&stored, 0, sizeof stored);
    blob_write_arg blob_write = {&blob, &stored};
    CF_REQUIRE(cf_write(world.app, write_blob, &blob_write) == CF_OK);

    cf_attachment one;
    memset(&one, 0, sizeof one);
    attachment_write_arg one_write = {CF_STR_LIT("Message"), 9,
                                      CF_STR_LIT("one"), stored.id, &one};
    CF_REQUIRE(cf_write(world.app, write_attachment, &one_write) == CF_OK);
    cf_attachment two;
    memset(&two, 0, sizeof two);
    attachment_write_arg two_write = {CF_STR_LIT("Message"), 9,
                                      CF_STR_LIT("two"), stored.id, &two};
    CF_REQUIRE(cf_write(world.app, write_attachment, &two_write) == CF_OK);

    attachment_delete_arg delete_one = {&one};
    CF_REQUIRE(cf_write(world.app, write_delete_attachment, &delete_one) ==
               CF_OK);

    cf_db *reader = as_reader(&world);
    bool found = true;
    cf_attachment gone;
    memset(&gone, 0, sizeof gone);
    CF_CHECK(cf_attachment_find_for(reader, CF_STR_LIT("Message"), 9,
                                    CF_STR_LIT("one"), &found,
                                    &gone) == CF_OK);
    CF_CHECK(!found);
    found = false;
    cf_attachment survivor;
    memset(&survivor, 0, sizeof survivor);
    CF_CHECK(cf_attachment_find_for(reader, CF_STR_LIT("Message"), 9,
                                    CF_STR_LIT("two"), &found,
                                    &survivor) == CF_OK);
    CF_CHECK(found);
    check_str(survivor.name, "two");
    cf_attachment_dispose(&survivor);

    CF_CHECK(count_rows(reader,
                        "SELECT count(*) FROM active_storage_attachments") ==
             1);

    /* execute semantics: deleting an absent id affects 0 rows and is still
     * OK (the reference does not check the affected count). */
    CF_CHECK(cf_write(world.app, write_delete_attachment, &delete_one) ==
             CF_OK);
    cf_attachment never;
    memset(&never, 0, sizeof never);
    never.id = 999999;
    attachment_delete_arg delete_never = {&never};
    CF_CHECK(cf_write(world.app, write_delete_attachment, &delete_never) ==
             CF_OK);
    CF_CHECK(count_rows(reader,
                        "SELECT count(*) FROM active_storage_attachments") ==
             1);

    cf_attachment_dispose(&one);
    cf_attachment_dispose(&two);
    cf_blob_dispose(&stored);
    cf_db_close(reader);
    cf_test_clock_clear();
    as_world_dispose(&world);
}

/* --- cf_attachment_blob ---------------------------------------------------- */

CF_TEST(attachment_blob_matches_and_missing_is_not_found) {
    as_world world;
    as_world_open(&world);

    cf_blob blob;
    memset(&blob, 0, sizeof blob);
    blob.key = text("blob-key");
    blob.filename = text("image.png");
    blob.service_name = text("local");
    blob.byte_size = 88;
    blob.content_type = optional("image/png");
    cf_blob stored;
    memset(&stored, 0, sizeof stored);
    blob_write_arg blob_write = {&blob, &stored};
    CF_REQUIRE(cf_write(world.app, write_blob, &blob_write) == CF_OK);

    cf_attachment attachment;
    memset(&attachment, 0, sizeof attachment);
    attachment_write_arg write = {CF_STR_LIT("Message"), 3,
                                  CF_STR_LIT("attachment"), stored.id,
                                  &attachment};
    CF_REQUIRE(cf_write(world.app, write_attachment, &write) == CF_OK);

    cf_db *reader = as_reader(&world);
    cf_blob via_attachment;
    memset(&via_attachment, 0, sizeof via_attachment);
    CF_REQUIRE(cf_attachment_blob(reader, &attachment, &via_attachment) ==
               CF_OK);
    CF_CHECK(blob_matches(&stored, &via_attachment));

    /* A dangling blob_id follows Blob::find: CF_NOT_FOUND, empty record. */
    cf_attachment dangling;
    memset(&dangling, 0, sizeof dangling);
    dangling.blob_id = 424242;
    cf_blob missing;
    memset(&missing, 0, sizeof missing);
    CF_CHECK(cf_attachment_blob(reader, &dangling, &missing) == CF_NOT_FOUND);
    CF_CHECK(missing.id == 0 && missing.key.ptr == NULL);

    /* Argument guard. */
    CF_CHECK(cf_attachment_blob(reader, NULL, &missing) == CF_INVALID);

    cf_blob_dispose(&stored);
    cf_blob_dispose(&via_attachment);
    cf_attachment_dispose(&attachment);
    cf_db_close(reader);
    cf_test_clock_clear();
    as_world_dispose(&world);
}

/* --- disposal -------------------------------------------------------------- */

CF_TEST(dispose_paths_are_empty_safe) {
    cf_str_dispose(NULL);
    cf_optional_str_dispose(NULL);
    cf_blob_dispose(NULL);
    cf_attachment_dispose(NULL);
    cf_blob_vector_dispose(NULL);
    cf_attachment_vector_dispose(NULL);

    cf_blob empty;
    memset(&empty, 0, sizeof empty);
    cf_blob_dispose(&empty);
    CF_CHECK(empty.key.ptr == NULL && empty.filename.ptr == NULL);
    cf_blob_dispose(&empty); /* double dispose of an empty object is a no-op */

    /* A populated record releases every owned string and resets. */
    cf_blob blob;
    memset(&blob, 0, sizeof blob);
    blob.id = 5;
    blob.byte_size = 10;
    blob.created_at = 123;
    blob.key = text("owned-key");
    blob.filename = text("owned.bin");
    blob.service_name = text("owned-service");
    blob.content_type = optional("text/plain");
    blob.metadata = optional("{}");
    blob.checksum = optional("sum");
    /* Deep-copy the borrowed views so dispose really frees heap bytes. */
    struct {
        cf_str *field;
    } strings[] = {{&blob.key},
                    {&blob.filename},
                    {&blob.service_name},
                    {&blob.content_type.value},
                    {&blob.metadata.value},
                    {&blob.checksum.value}};
    for (size_t i = 0; i < sizeof strings / sizeof strings[0]; i++) {
        char *copy = malloc(strings[i].field->len + 1);
        CF_REQUIRE(copy != NULL);
        memcpy(copy, strings[i].field->ptr, strings[i].field->len);
        copy[strings[i].field->len] = '\0';
        strings[i].field->ptr = copy;
    }

    cf_blob_vector vector;
    memset(&vector, 0, sizeof vector);
    vector.items = malloc(2 * sizeof(cf_blob));
    CF_REQUIRE(vector.items != NULL);
    vector.len = 2;
    vector.cap = 2;
    for (int i = 0; i < 2; i++) {
        memset(&vector.items[i], 0, sizeof vector.items[i]);
        vector.items[i].key = text("vector-key");
        char *copy = malloc(vector.items[i].key.len + 1);
        CF_REQUIRE(copy != NULL);
        memcpy(copy, vector.items[i].key.ptr, vector.items[i].key.len);
        copy[vector.items[i].key.len] = '\0';
        vector.items[i].key.ptr = copy;
    }

    cf_attachment attachment;
    memset(&attachment, 0, sizeof attachment);
    attachment.id = 1;
    attachment.name = text("owned-name");
    attachment.record_type = text("Message");
    char *name_copy = malloc(attachment.name.len + 1);
    CF_REQUIRE(name_copy != NULL);
    memcpy(name_copy, attachment.name.ptr, attachment.name.len);
    name_copy[attachment.name.len] = '\0';
    attachment.name.ptr = name_copy;
    char *type_copy = malloc(attachment.record_type.len + 1);
    CF_REQUIRE(type_copy != NULL);
    memcpy(type_copy, attachment.record_type.ptr, attachment.record_type.len);
    type_copy[attachment.record_type.len] = '\0';
    attachment.record_type.ptr = type_copy;

    cf_attachment_vector attachment_vector;
    memset(&attachment_vector, 0, sizeof attachment_vector);
    attachment_vector.items = malloc(sizeof(cf_attachment));
    CF_REQUIRE(attachment_vector.items != NULL);
    attachment_vector.len = 1;
    attachment_vector.cap = 1;
    memset(&attachment_vector.items[0], 0, sizeof attachment_vector.items[0]);
    attachment_vector.items[0].name = text("vec");
    char *vec_copy = malloc(3 + 1);
    CF_REQUIRE(vec_copy != NULL);
    memcpy(vec_copy, "vec", 3);
    vec_copy[3] = '\0';
    attachment_vector.items[0].name.ptr = vec_copy;

    cf_blob_dispose(&blob);
    CF_CHECK(blob.key.ptr == NULL && blob.key.len == 0);
    CF_CHECK(blob.filename.ptr == NULL && blob.service_name.ptr == NULL);
    CF_CHECK(!blob.content_type.present && !blob.metadata.present &&
             !blob.checksum.present);
    CF_CHECK(blob.id == 0 && blob.byte_size == 0 && blob.created_at == 0);

    cf_blob_vector_dispose(&vector);
    CF_CHECK(vector.items == NULL && vector.len == 0 && vector.cap == 0);

    cf_attachment_dispose(&attachment);
    CF_CHECK(attachment.name.ptr == NULL && attachment.record_type.ptr == NULL);
    CF_CHECK(attachment.id == 0);

    cf_attachment_vector_dispose(&attachment_vector);
    CF_CHECK(attachment_vector.items == NULL && attachment_vector.len == 0);
}

CF_TEST_MAIN()
