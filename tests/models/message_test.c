/* tests/models/message_test.c — D01 model family "message" tests.
 *
 * Oracle: tmp/rust-ref/crates/db/src/tests/message_test.rs (pinned) plus the
 * model SQL in models/message.rs; deterministic rows are seeded through the
 * fresh schema (the reference YAML fixture set is not part of this checkout),
 * and mutation cases go through the public writer contract cf_write (D02).
 *
 * Rich text: the reference tests run against testing.rs BasicRichText, the
 * test-support stand-in for the RichText trait.  Its committed C translation
 * lives in tests/models/support/richtext.c (a test-only double; R02's real
 * pipeline replaces it at link time in a later phase).  The stand-in ignores
 * its pipeline argument, so test_rich_text() returns an opaque instance.
 *
 * Mutation cases run through D02 (cf_writer_start/cf_write/cf_writer_stop) and
 * the sibling model .c files; all 21 cases pass plain, ASan/UBSan and Fil-C in
 * the group link (see docs/devel/evidence/D01-model-message.md).  The group
 * link still needs the proposed D02/R02 rich-text accessor and A01's password
 * verifier; until those land a scratch shim can supply them (evidence).
 */
#include "cf_test.h"

#include "app.h"
#include "cf.h"
#include "config.h"
#include "core/testclock.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "db/writer.h"
#include "models/active_storage.h"
#include "models/boost.h"
#include "models/message.h"
#include "models/rich_text_record.h"
#include "models/room.h"
#include "models/sound.h"
#include "models/user.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

/* 2026-01-21T08:53:20Z; ordering offsets are added in microseconds. */
#define T0 INT64_C(1769000000000000)
#define SEC (INT64_C(1000000))

static const unsigned char cf_test_richtext_standin;

static const cf_richtext *test_rich_text(void) {
    return (const cf_richtext *)&cf_test_richtext_standin;
}

static cf_str lit(const char *text) {
    cf_str value = {(char *)text, strlen(text)};
    return value;
}

/* The BasicRichText-shaped pipeline double lives in
 * tests/models/support/richtext.c (test support, not production code);
 * the linked double defines cf_richtext_to_plain_text and
 * cf_richtext_mentioned_user_ids. */

/* --- deterministic seeding ------------------------------------------------ */

typedef struct {
    sqlite3_stmt *stmt;
} seed_stmt;

static void seed_prepare(cf_db *db, seed_stmt *seed, const char *sql) {
    seed->stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &seed->stmt,
                                  NULL) == SQLITE_OK);
}

static void seed_done(seed_stmt *seed) { sqlite3_finalize(seed->stmt); }

static void seed_i64(seed_stmt *seed, int index, int64_t value) {
    CF_REQUIRE(sqlite3_bind_int64(seed->stmt, index, value) == SQLITE_OK);
}

static void seed_text(seed_stmt *seed, int index, const char *text) {
    CF_REQUIRE(sqlite3_bind_text(seed->stmt, index, text, -1,
                                 SQLITE_TRANSIENT) == SQLITE_OK);
}

static void seed_null(seed_stmt *seed, int index) {
    CF_REQUIRE(sqlite3_bind_null(seed->stmt, index) == SQLITE_OK);
}

static void seed_time(seed_stmt *seed, int index, int64_t us) {
    char text[CF_DB_TIME_TEXT_CAP];
    CF_REQUIRE(cf_db_time_to_text(us, text) == CF_OK);
    seed_text(seed, index, text);
}

static void seed_step(seed_stmt *seed) {
    CF_REQUIRE(sqlite3_step(seed->stmt) == SQLITE_DONE);
    seed_done(seed);
}

static void seed_user(cf_db *db, int64_t id, const char *name, int role,
                      int status, int64_t created) {
    seed_stmt s;
    seed_prepare(db, &s,
                 "INSERT INTO \"users\" (\"id\",\"name\",\"role\",\"status\","
                 "\"created_at\",\"updated_at\") VALUES (?,?,?,?,?,?)");
    seed_i64(&s, 1, id);
    seed_text(&s, 2, name);
    seed_i64(&s, 3, role);
    seed_i64(&s, 4, status);
    seed_time(&s, 5, created);
    seed_time(&s, 6, created);
    seed_step(&s);
}

static void seed_room(cf_db *db, int64_t id, int64_t creator, const char *type,
                      int64_t created) {
    seed_stmt s;
    seed_prepare(db, &s,
                 "INSERT INTO \"rooms\" (\"id\",\"created_at\",\"creator_id\","
                 "\"name\",\"type\",\"updated_at\") VALUES (?,?,?,?,?,?)");
    seed_i64(&s, 1, id);
    seed_time(&s, 2, created);
    seed_i64(&s, 3, creator);
    seed_null(&s, 4);
    seed_text(&s, 5, type);
    seed_time(&s, 6, created);
    seed_step(&s);
}

static void seed_membership(cf_db *db, int64_t id, int64_t room_id,
                            int64_t user_id, const char *involvement,
                            int64_t created) {
    seed_stmt s;
    seed_prepare(db, &s,
                 "INSERT INTO \"memberships\" (\"id\",\"created_at\","
                 "\"involvement\",\"room_id\",\"updated_at\",\"user_id\") "
                 "VALUES (?,?,?,?,?,?)");
    seed_i64(&s, 1, id);
    seed_time(&s, 2, created);
    seed_text(&s, 3, involvement);
    seed_i64(&s, 4, room_id);
    seed_time(&s, 5, created);
    seed_i64(&s, 6, user_id);
    seed_step(&s);
}

static void seed_message(cf_db *db, int64_t id, int64_t room_id,
                         int64_t creator_id, const char *client_message_id,
                         int64_t created, int64_t updated) {
    seed_stmt s;
    seed_prepare(db, &s,
                 "INSERT INTO \"messages\" (\"id\",\"room_id\",\"creator_id\","
                 "\"client_message_id\",\"created_at\",\"updated_at\") "
                 "VALUES (?,?,?,?,?,?)");
    seed_i64(&s, 1, id);
    seed_i64(&s, 2, room_id);
    seed_i64(&s, 3, creator_id);
    seed_text(&s, 4, client_message_id);
    seed_time(&s, 5, created);
    seed_time(&s, 6, updated);
    seed_step(&s);
}

static void seed_rich_text(cf_db *db, int64_t id, int64_t record_id,
                           const char *body_or_null, int64_t created) {
    seed_stmt s;
    seed_prepare(db, &s,
                 "INSERT INTO \"action_text_rich_texts\" (\"id\",\"body\","
                 "\"created_at\",\"name\",\"record_id\",\"record_type\","
                 "\"updated_at\") VALUES (?,?,?,?,?,?,?)");
    seed_i64(&s, 1, id);
    if (body_or_null != NULL) {
        seed_text(&s, 2, body_or_null);
    } else {
        seed_null(&s, 2);
    }
    seed_time(&s, 3, created);
    seed_text(&s, 4, "body");
    seed_i64(&s, 5, record_id);
    seed_text(&s, 6, "Message");
    seed_time(&s, 7, created);
    seed_step(&s);
}

static void seed_blob(cf_db *db, int64_t id, const char *key,
                      const char *filename, int64_t created) {
    seed_stmt s;
    seed_prepare(db, &s,
                 "INSERT INTO \"active_storage_blobs\" (\"id\",\"byte_size\","
                 "\"checksum\",\"content_type\",\"created_at\",\"filename\","
                 "\"key\",\"metadata\",\"service_name\") "
                 "VALUES (?,?,?,?,?,?,?,?,?)");
    seed_i64(&s, 1, id);
    seed_i64(&s, 2, 1);
    seed_null(&s, 3);
    seed_text(&s, 4, "image/jpeg");
    seed_time(&s, 5, created);
    seed_text(&s, 6, filename);
    seed_text(&s, 7, key);
    seed_null(&s, 8);
    seed_text(&s, 9, "local");
    seed_step(&s);
}

static void seed_attachment(cf_db *db, int64_t id, int64_t record_id,
                            int64_t blob_id, int64_t created) {
    seed_stmt s;
    seed_prepare(db, &s,
                 "INSERT INTO \"active_storage_attachments\" (\"id\","
                 "\"blob_id\",\"created_at\",\"name\",\"record_id\","
                 "\"record_type\") VALUES (?,?,?,?,?,?)");
    seed_i64(&s, 1, id);
    seed_i64(&s, 2, blob_id);
    seed_time(&s, 3, created);
    seed_text(&s, 4, "attachment");
    seed_i64(&s, 5, record_id);
    seed_text(&s, 6, "Message");
    seed_step(&s);
}

static void seed_boost(cf_db *db, int64_t id, int64_t message_id,
                       int64_t booster_id, const char *content,
                       int64_t created) {
    seed_stmt s;
    seed_prepare(db, &s,
                 "INSERT INTO \"boosts\" (\"id\",\"booster_id\",\"content\","
                 "\"created_at\",\"message_id\",\"updated_at\") "
                 "VALUES (?,?,?,?,?,?)");
    seed_i64(&s, 1, id);
    seed_i64(&s, 2, booster_id);
    seed_text(&s, 3, content);
    seed_time(&s, 4, created);
    seed_i64(&s, 5, message_id);
    seed_time(&s, 6, created);
    seed_step(&s);
}

static void seed_fts(cf_db *db, int64_t rowid, const char *body) {
    seed_stmt s;
    seed_prepare(db, &s,
                 "INSERT INTO message_search_index(rowid, body) VALUES (?,?)");
    seed_i64(&s, 1, rowid);
    seed_text(&s, 2, body);
    seed_step(&s);
}

/* The standard fixture: two users, two open rooms, memberships, messages. */
static void seed_base(cf_db *db) {
    seed_user(db, 1, "david", 0, 0, T0);
    seed_user(db, 2, "jason", 0, 0, T0);
    seed_user(db, 3, "kevin", 0, 0, T0); /* not a member of room 10 */
    seed_room(db, 10, 1, "Rooms::Open", T0);
    seed_room(db, 20, 1, "Rooms::Open", T0);
    seed_membership(db, 100, 10, 1, "everything", T0);
    seed_membership(db, 101, 10, 2, "mentions", T0);
    seed_membership(db, 102, 20, 1, "everything", T0);
}

static int64_t *message_ids(const cf_message_vector *vector, size_t *len) {
    int64_t *ids = malloc(vector->len * sizeof *ids);
    CF_REQUIRE(ids != NULL || vector->len == 0);
    for (size_t i = 0; i < vector->len; i++) ids[i] = vector->items[i].id;
    *len = vector->len;
    return ids;
}

static int compare_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static bool ids_equal_sorted(int64_t *ids, size_t len, const int64_t *want,
                             size_t want_len) {
    qsort(ids, len, sizeof *ids, compare_i64);
    if (len != want_len) return false;
    for (size_t i = 0; i < len; i++) {
        if (ids[i] != want[i]) return false;
    }
    return true;
}

/* --- reader cases --------------------------------------------------------- */

CF_TEST(content_type_name_all_values) {
    CF_CHECK(strcmp(cf_content_type_name(CF_CONTENT_TYPE_ATTACHMENT),
                    "attachment") == 0);
    CF_CHECK(strcmp(cf_content_type_name(CF_CONTENT_TYPE_SOUND), "sound") ==
             0);
    CF_CHECK(strcmp(cf_content_type_name(CF_CONTENT_TYPE_TEXT), "text") == 0);
    CF_CHECK(cf_content_type_name((cf_content_type)7) == NULL);
}

CF_TEST(sound_in_matches_reference_rule) {
    const cf_sound *sound = NULL;

    CF_CHECK(cf_message_sound_in(lit("/play bell"), &sound));
    CF_REQUIRE(sound != NULL);
    CF_CHECK(strcmp(sound->name, "bell") == 0);

    CF_CHECK(cf_message_sound_in(lit("/play 56k"), &sound));
    CF_REQUIRE(sound != NULL);
    CF_CHECK(strcmp(sound->name, "56k") == 0);

    sound = NULL;
    CF_CHECK(!cf_message_sound_in(lit("/play nosuchsound"), &sound));
    CF_CHECK(sound == NULL);
    CF_CHECK(!cf_message_sound_in(lit("/play "), &sound));
    CF_CHECK(!cf_message_sound_in(lit("/play bell "), &sound));
    CF_CHECK(!cf_message_sound_in(lit("/play bell\n"), &sound));
    CF_CHECK(!cf_message_sound_in(lit("x/play bell"), &sound));
    CF_CHECK(!cf_message_sound_in(lit("/play"), &sound));
    CF_CHECK(!cf_message_sound_in(lit("/play be_ll"), &sound));
    CF_CHECK(!cf_message_sound_in(lit(""), &sound));
}

CF_TEST(find_by_id_find_last_count) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    cf_db *db = scratch.db;
    seed_base(db);
    seed_message(db, 1, 10, 1, "c1", T0, T0);
    seed_message(db, 2, 10, 2, "c2", T0 + SEC, T0 + SEC);
    seed_message(db, 3, 20, 1, "c3", T0 + 2 * SEC, T0 + 2 * SEC);

    bool found = false;
    cf_message message;
    CF_REQUIRE(cf_message_find_by_id(db, 2, &found, &message) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(message.id == 2 && message.room_id == 10 &&
             message.creator_id == 2);
    CF_CHECK(message.created_at == T0 + SEC &&
             message.updated_at == T0 + SEC);
    CF_CHECK(strcmp(message.client_message_id.ptr, "c2") == 0);
    cf_message_dispose(&message);

    found = true;
    CF_CHECK(cf_message_find_by_id(db, 99, &found, &message) == CF_OK);
    CF_CHECK(!found);
    CF_CHECK(message.id == 0 && message.client_message_id.ptr == NULL);

    CF_CHECK(cf_message_find(db, 1, &message) == CF_OK);
    CF_CHECK(message.id == 1);
    cf_message_dispose(&message);
    CF_CHECK(cf_message_find(db, 99, &message) == CF_NOT_FOUND);
    CF_CHECK(message.client_message_id.ptr == NULL);

    CF_CHECK(cf_message_last(db, &found, &message) == CF_OK);
    CF_CHECK(found && message.id == 3);
    cf_message_dispose(&message);

    int64_t count = 0;
    CF_CHECK(cf_message_count(db, &count) == CF_OK);
    CF_CHECK(count == 3);
    CF_CHECK(cf_message_count(NULL, &count) == CF_INVALID);

    cf_message_vector by_creator;
    CF_REQUIRE(cf_message_by_creator(db, 1, &by_creator) == CF_OK);
    size_t len = 0;
    int64_t *ids = message_ids(&by_creator, &len);
    const int64_t want[] = {1, 3};
    CF_CHECK(ids_equal_sorted(ids, len, want, 2));
    free(ids);
    cf_message_vector_dispose(&by_creator);

    CF_REQUIRE(cf_message_by_creator(db, 42, &by_creator) == CF_OK);
    CF_CHECK(by_creator.len == 0);
    cf_message_vector_dispose(&by_creator);

    cf_db_scratch_close(&scratch);
}

CF_TEST(for_room_find_in_room_count) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    cf_db *db = scratch.db;
    seed_base(db);
    seed_message(db, 1, 10, 1, "c1", T0, T0);
    seed_message(db, 2, 10, 2, "c2", T0 + SEC, T0 + SEC);
    seed_message(db, 3, 20, 1, "c3", T0 + 2 * SEC, T0 + 2 * SEC);

    cf_message_vector messages;
    CF_REQUIRE(cf_message_for_room(db, 10, &messages) == CF_OK);
    size_t len = 0;
    int64_t *ids = message_ids(&messages, &len);
    const int64_t want[] = {1, 2};
    CF_CHECK(ids_equal_sorted(ids, len, want, 2));
    free(ids);
    cf_message_vector_dispose(&messages);

    cf_message message;
    CF_CHECK(cf_message_find_in_room(db, 10, 2, &message) == CF_OK);
    CF_CHECK(message.id == 2 && message.room_id == 10);
    cf_message_dispose(&message);
    CF_CHECK(cf_message_find_in_room(db, 20, 2, &message) == CF_NOT_FOUND);
    CF_CHECK(message.client_message_id.ptr == NULL);

    int64_t count = 0;
    CF_CHECK(cf_message_count_in_room(db, 10, &count) == CF_OK);
    CF_CHECK(count == 2);
    CF_CHECK(cf_message_count_in_room(db, 99, &count) == CF_OK);
    CF_CHECK(count == 0);

    cf_db_scratch_close(&scratch);
}

CF_TEST(find_reachable_uses_memberships) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    cf_db *db = scratch.db;
    seed_base(db);
    seed_message(db, 1, 10, 1, "c1", T0, T0);

    cf_message message;
    CF_CHECK(cf_message_find_reachable(db, 2, 1, &message) == CF_OK);
    CF_CHECK(message.id == 1);
    cf_message_dispose(&message);
    CF_CHECK(cf_message_find_reachable(db, 3, 1, &message) == CF_NOT_FOUND);
    CF_CHECK(cf_message_find_reachable(db, 2, 99, &message) == CF_NOT_FOUND);

    cf_db_scratch_close(&scratch);
}

CF_TEST(pagination_before_after_around) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    cf_db *db = scratch.db;
    seed_base(db);
    for (int i = 0; i < 10; i++) {
        seed_message(db, i + 1, 10, 1, "c", T0 + i * SEC, T0 + i * SEC);
    }

    cf_message_vector all;
    CF_REQUIRE(cf_message_last_page(db, 10, &all) == CF_OK);
    CF_REQUIRE(all.len == 10);
    for (size_t i = 0; i < all.len; i++) {
        CF_CHECK(all.items[i].id == (int64_t)i + 1);
    }

    cf_message_vector first;
    CF_REQUIRE(cf_message_first_page(db, 10, &first) == CF_OK);
    CF_REQUIRE(first.len == 10);
    for (size_t i = 0; i < first.len; i++) {
        CF_CHECK(first.items[i].id == (int64_t)i + 1);
    }
    cf_message_vector_dispose(&first);

    cf_message middle = all.items[5];
    cf_message_vector before;
    CF_REQUIRE(cf_message_page_before(db, 10, &middle, &before) == CF_OK);
    CF_REQUIRE(before.len == 5);
    CF_CHECK(before.items[0].id == 1 && before.items[4].id == 5);
    cf_message_vector_dispose(&before);

    cf_message_vector after;
    CF_REQUIRE(cf_message_page_after(db, 10, &middle, &after) == CF_OK);
    CF_REQUIRE(after.len == 4);
    CF_CHECK(after.items[0].id == 7 && after.items[3].id == 10);
    cf_message_vector_dispose(&after);

    cf_message_vector around;
    CF_REQUIRE(cf_message_page_around(db, 10, &middle, &around) == CF_OK);
    CF_REQUIRE(around.len == 10);
    for (size_t i = 0; i < around.len; i++) {
        CF_CHECK(around.items[i].id == (int64_t)i + 1);
    }
    cf_message_vector_dispose(&around);

    bool exists = false;
    CF_CHECK(cf_message_exists_before(db, 10, &middle, &exists) == CF_OK);
    CF_CHECK(exists);
    CF_CHECK(cf_message_exists_after(db, 10, &middle, &exists) == CF_OK);
    CF_CHECK(exists);
    CF_CHECK(cf_message_exists_after(db, 10, &all.items[9], &exists) == CF_OK);
    CF_CHECK(!exists);
    CF_CHECK(cf_message_exists_before(db, 10, &all.items[0], &exists) == CF_OK);
    CF_CHECK(!exists);

    CF_CHECK(cf_message_paged(db, 10, &exists) == CF_OK);
    CF_CHECK(!exists);
    CF_CHECK(cf_message_paged(db, 20, &exists) == CF_OK);
    CF_CHECK(!exists);
    cf_message_vector_dispose(&all);

    cf_db_scratch_close(&scratch);
}

CF_TEST(pagination_page_size_and_equal_timestamps) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    cf_db *db = scratch.db;
    seed_base(db);
    /* Room 10: PAGE_SIZE + 1 rows; room 20: two rows sharing created_at. */
    for (int i = 0; i < CF_MESSAGE_PAGE_SIZE + 1; i++) {
        seed_message(db, i + 1, 10, 1, "c", T0 + i * SEC, T0 + i * SEC);
    }
    seed_message(db, 1000, 20, 1, "d1", T0, T0);
    seed_message(db, 1001, 20, 1, "d2", T0, T0);

    bool paged = false;
    CF_CHECK(cf_message_paged(db, 10, &paged) == CF_OK);
    CF_CHECK(paged);
    CF_CHECK(cf_message_paged(db, 20, &paged) == CF_OK);
    CF_CHECK(!paged);

    cf_message_vector last;
    CF_REQUIRE(cf_message_last_page(db, 10, &last) == CF_OK);
    CF_REQUIRE(last.len == CF_MESSAGE_PAGE_SIZE);
    /* Newest 40, oldest first: id 2..41. */
    CF_CHECK(last.items[0].id == 2);
    CF_CHECK(last.items[CF_MESSAGE_PAGE_SIZE - 1].id == 41);
    cf_message_vector_dispose(&last);

    cf_message_vector first;
    CF_REQUIRE(cf_message_first_page(db, 10, &first) == CF_OK);
    CF_REQUIRE(first.len == CF_MESSAGE_PAGE_SIZE);
    CF_CHECK(first.items[0].id == 1);
    CF_CHECK(first.items[CF_MESSAGE_PAGE_SIZE - 1].id == 40);
    cf_message_vector_dispose(&first);

    cf_message_vector ties;
    CF_REQUIRE(cf_message_last_page(db, 20, &ties) == CF_OK);
    CF_REQUIRE(ties.len == 2);
    size_t len = 0;
    int64_t *ids = message_ids(&ties, &len);
    const int64_t want[] = {1000, 1001};
    CF_CHECK(ids_equal_sorted(ids, len, want, 2));
    free(ids);
    cf_message_vector_dispose(&ties);

    cf_db_scratch_close(&scratch);
}

CF_TEST(page_created_and_updated_since) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    cf_db *db = scratch.db;
    seed_base(db);
    seed_message(db, 1, 10, 1, "c1", T0, T0);
    seed_message(db, 2, 10, 1, "c2", T0 + SEC, T0 + 10 * SEC);
    seed_message(db, 3, 10, 1, "c3", T0 + 2 * SEC, T0 + 20 * SEC);
    seed_message(db, 4, 20, 1, "c4", T0 + 3 * SEC, T0 + 30 * SEC);

    cf_message_vector messages;
    CF_REQUIRE(cf_message_page_created_since(db, 10, T0, &messages) == CF_OK);
    CF_REQUIRE(messages.len == 2);
    CF_CHECK(messages.items[0].id == 2 && messages.items[1].id == 3);
    cf_message_vector_dispose(&messages);

    CF_REQUIRE(cf_message_page_updated_since(db, 10, T0, NULL, 0, &messages) ==
               CF_OK);
    CF_REQUIRE(messages.len == 2);
    /* ORDER BY created_at DESC, reversed -> oldest first. */
    CF_CHECK(messages.items[0].id == 2 && messages.items[1].id == 3);
    cf_message_vector_dispose(&messages);

    const int64_t exclude[] = {2};
    CF_REQUIRE(cf_message_page_updated_since(db, 10, T0, exclude, 1,
                                             &messages) == CF_OK);
    CF_REQUIRE(messages.len == 1);
    CF_CHECK(messages.items[0].id == 3);
    cf_message_vector_dispose(&messages);

    const int64_t exclude_all[] = {2, 3};
    CF_REQUIRE(cf_message_page_updated_since(db, 10, T0, exclude_all, 2,
                                             &messages) == CF_OK);
    CF_CHECK(messages.len == 0);
    cf_message_vector_dispose(&messages);

    CF_REQUIRE(cf_message_page_updated_since(db, 10, T0 + 25 * SEC, NULL, 0,
                                             &messages) == CF_OK);
    CF_CHECK(messages.len == 0);
    cf_message_vector_dispose(&messages);

    cf_db_scratch_close(&scratch);
}

CF_TEST(search_in_room_and_reachable) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    cf_db *db = scratch.db;
    seed_base(db);
    seed_message(db, 1, 10, 1, "c1", T0, T0);
    seed_message(db, 2, 10, 1, "c2", T0 + SEC, T0 + SEC);
    seed_message(db, 3, 20, 1, "c3", T0 + 2 * SEC, T0 + 2 * SEC);
    seed_fts(db, 1, "Do NOT feed the eel OR the shark");
    seed_fts(db, 2, "My hovercraft is full of eels");
    seed_fts(db, 3, "eel in another room");

    cf_message_vector messages;
    CF_REQUIRE(cf_message_search_in_room(db, 10, lit("eel"), &messages) ==
               CF_OK);
    CF_REQUIRE(messages.len == 2);
    CF_CHECK(messages.items[0].id == 1 && messages.items[1].id == 2);
    cf_message_vector_dispose(&messages);

    /* Words are quoted, so query syntax is never interpreted. */
    CF_REQUIRE(cf_message_search_in_room(db, 10, lit("NOT eel"), &messages) ==
               CF_OK);
    CF_REQUIRE(messages.len == 1);
    CF_CHECK(messages.items[0].id == 1);
    cf_message_vector_dispose(&messages);

    CF_REQUIRE(cf_message_search_in_room(db, 10, lit("eel dolphin"),
                                         &messages) == CF_OK);
    CF_CHECK(messages.len == 0);
    cf_message_vector_dispose(&messages);

    /* A NUL separates terms too. */
    cf_str nul_query = {(char *)"eel\0shark", sizeof("eel\0shark") - 1};
    CF_REQUIRE(cf_message_search_in_room(db, 10, nul_query, &messages) ==
               CF_OK);
    CF_REQUIRE(messages.len == 1);
    CF_CHECK(messages.items[0].id == 1);
    cf_message_vector_dispose(&messages);

    /* The reference corpus: every one of these is a successful search
     * ("a\0b" and "\0" carry their NUL bytes). */
    static const struct {
        const char *text;
        size_t len;
    } queries[] = {
        {"NOT", 3},     {"OR", 2},       {"AND", 3},      {"NEAR", 4},
        {"eel NOT", 7}, {"NOT eel", 7},  {"shark OR", 8}, {"\"", 1},
        {"*", 1},       {"", 0},         {"a\0b", 3},     {"\0", 1},
    };
    for (size_t i = 0; i < sizeof queries / sizeof queries[0]; i++) {
        cf_str q = {(char *)queries[i].text, queries[i].len};
        CF_CHECK(cf_message_search_in_room(db, 10, q, &messages) == CF_OK);
        cf_message_vector_dispose(&messages);
    }

    CF_CHECK(cf_message_search_in_room(db, 10, lit(""), &messages) == CF_OK);
    CF_CHECK(messages.len == 0);
    cf_message_vector_dispose(&messages);

    /* Reachable: only rooms the user belongs to; newest 100, oldest first. */
    CF_REQUIRE(cf_message_search_reachable(db, 2, lit("eel"), &messages) ==
               CF_OK);
    CF_REQUIRE(messages.len == 2);
    CF_CHECK(messages.items[0].id == 1 && messages.items[1].id == 2);
    cf_message_vector_dispose(&messages);
    CF_REQUIRE(cf_message_search_reachable(db, 3, lit("eel"), &messages) ==
               CF_OK);
    CF_CHECK(messages.len == 0);
    cf_message_vector_dispose(&messages);

    cf_db_scratch_close(&scratch);
}

CF_TEST(mentionees_in_room_joins_memberships) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    cf_db *db = scratch.db;
    seed_base(db);

    const int64_t ids[] = {1, 3, 2, 2};
    cf_user_vector users;
    CF_REQUIRE(cf_message_mentionees_in_room(db, 10, ids, 4, &users) == CF_OK);
    CF_CHECK(users.len == 2);
    CF_CHECK(users.items[0].id == 1 && users.items[1].id == 2);
    CF_CHECK(strcmp(users.items[0].name.ptr, "david") == 0);
    cf_user_vector_dispose(&users);

    const int64_t kevin_only[] = {3};
    CF_REQUIRE(cf_message_mentionees_in_room(db, 10, kevin_only, 1, &users) ==
               CF_OK);
    CF_CHECK(users.len == 0);
    cf_user_vector_dispose(&users);

    CF_REQUIRE(cf_message_mentionees_in_room(db, 10, NULL, 0, &users) ==
               CF_OK);
    CF_CHECK(users.len == 0);
    cf_user_vector_dispose(&users);

    cf_db_scratch_close(&scratch);
}

CF_TEST(body_body_html_and_attachment) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    cf_db *db = scratch.db;
    seed_base(db);
    seed_message(db, 1, 10, 1, "c1", T0, T0);
    seed_message(db, 2, 10, 1, "c2", T0 + SEC, T0 + SEC);
    seed_message(db, 3, 10, 1, "c3", T0 + 2 * SEC, T0 + 2 * SEC);
    seed_rich_text(db, 10, 1, "<p>Hello <strong>there</strong></p>", T0);
    seed_rich_text(db, 11, 2, NULL, T0);
    seed_blob(db, 20, "blob-key", "moon.jpg", T0);
    seed_attachment(db, 30, 3, 20, T0);

    cf_message message;
    CF_REQUIRE(cf_message_find(db, 1, &message) == CF_OK);
    bool found = false;
    cf_rich_text_record record;
    CF_REQUIRE(cf_message_body(db, &message, &found, &record) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(record.record_id == 1 && record.id == 10);
    CF_CHECK(strcmp(record.record_type.ptr, "Message") == 0);
    CF_CHECK(strcmp(record.name.ptr, "body") == 0);
    CF_CHECK(record.body.present &&
             strcmp(record.body.value.ptr, "<p>Hello <strong>there</strong></p>") ==
                 0);
    cf_rich_text_record_dispose(&record);

    cf_str html = {0};
    CF_REQUIRE(cf_message_body_html(db, &message, &found, &html) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(html.len == strlen("<p>Hello <strong>there</strong></p>"));
    cf_str_dispose(&html);

    cf_attachment attachment;
    cf_blob blob;
    CF_REQUIRE(cf_message_attachment(db, &message, &found, &attachment,
                                     &blob) == CF_OK);
    CF_CHECK(!found && attachment.id == 0 && blob.id == 0);
    cf_message_dispose(&message);

    /* Record with a NULL body: body found, body_html absent. */
    CF_REQUIRE(cf_message_find(db, 2, &message) == CF_OK);
    CF_REQUIRE(cf_message_body(db, &message, &found, &record) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(!record.body.present);
    cf_rich_text_record_dispose(&record);
    CF_REQUIRE(cf_message_body_html(db, &message, &found, &html) == CF_OK);
    CF_CHECK(!found && html.ptr == NULL);
    cf_message_dispose(&message);

    /* No record at all. */
    CF_REQUIRE(cf_message_find(db, 3, &message) == CF_OK);
    CF_REQUIRE(cf_message_body(db, &message, &found, &record) == CF_OK);
    CF_CHECK(!found);
    CF_REQUIRE(cf_message_attachment(db, &message, &found, &attachment,
                                     &blob) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(attachment.blob_id == 20 && attachment.record_id == 3);
    CF_CHECK(strcmp(blob.filename.ptr, "moon.jpg") == 0);
    CF_CHECK(strcmp(blob.key.ptr, "blob-key") == 0);
    cf_blob_dispose(&blob);
    cf_attachment_dispose(&attachment);
    cf_message_dispose(&message);

    cf_db_scratch_close(&scratch);
}

CF_TEST(plain_text_body_and_content_type) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    cf_db *db = scratch.db;
    seed_base(db);
    seed_message(db, 1, 10, 1, "c1", T0, T0);
    seed_message(db, 2, 10, 1, "c2", T0 + SEC, T0 + SEC);
    seed_message(db, 3, 10, 1, "c3", T0 + 2 * SEC, T0 + 2 * SEC);
    seed_message(db, 4, 10, 1, "c4", T0 + 3 * SEC, T0 + 3 * SEC);
    seed_message(db, 5, 10, 1, "c5", T0 + 4 * SEC, T0 + 4 * SEC);
    seed_rich_text(db, 10, 1,
                   "<span>My hovercraft is full of eels</span>", T0);
    seed_rich_text(db, 11, 2, "&nbsp;", T0); /* whitespace-only body */
    seed_rich_text(db, 12, 4, "/play trombone", T0);
    seed_rich_text(db, 13, 5, "/play nosuchsound", T0);
    seed_blob(db, 20, "moon-key", "moon.jpg", T0);
    seed_attachment(db, 30, 2, 20, T0); /* blank body + attachment */
    seed_attachment(db, 31, 3, 20, T0); /* no body at all + attachment */

    cf_message message;
    cf_str plain = {0};
    cf_content_type content_type = CF_CONTENT_TYPE_TEXT;

    CF_REQUIRE(cf_message_find(db, 1, &message) == CF_OK);
    CF_REQUIRE(cf_message_plain_text_body(db, &message, test_rich_text(),
                                          &plain) == CF_OK);
    CF_CHECK(strcmp(plain.ptr, "My hovercraft is full of eels") == 0);
    cf_str_dispose(&plain);
    CF_REQUIRE(cf_message_content_type(db, &message, test_rich_text(),
                                       &content_type) == CF_OK);
    CF_CHECK(content_type == CF_CONTENT_TYPE_TEXT);
    cf_message_dispose(&message);

    /* Whitespace-only body falls back to the attachment filename. */
    CF_REQUIRE(cf_message_find(db, 2, &message) == CF_OK);
    CF_REQUIRE(cf_message_plain_text_body(db, &message, test_rich_text(),
                                          &plain) == CF_OK);
    CF_CHECK(strcmp(plain.ptr, "moon.jpg") == 0);
    cf_str_dispose(&plain);
    CF_REQUIRE(cf_message_content_type(db, &message, test_rich_text(),
                                       &content_type) == CF_OK);
    CF_CHECK(content_type == CF_CONTENT_TYPE_ATTACHMENT);
    cf_message_dispose(&message);

    /* No body: filename, attachment content type. */
    CF_REQUIRE(cf_message_find(db, 3, &message) == CF_OK);
    CF_REQUIRE(cf_message_plain_text_body(db, &message, test_rich_text(),
                                          &plain) == CF_OK);
    CF_CHECK(strcmp(plain.ptr, "moon.jpg") == 0);
    cf_str_dispose(&plain);
    CF_REQUIRE(cf_message_content_type(db, &message, test_rich_text(),
                                       &content_type) == CF_OK);
    CF_CHECK(content_type == CF_CONTENT_TYPE_ATTACHMENT);
    cf_message_dispose(&message);

    /* Sound bodies. */
    CF_REQUIRE(cf_message_find(db, 4, &message) == CF_OK);
    const cf_sound *sound = NULL;
    bool sound_found = false;
    CF_REQUIRE(cf_message_sound(db, &message, test_rich_text(), &sound_found,
                                &sound) == CF_OK);
    CF_CHECK(sound_found && sound != NULL &&
             strcmp(sound->name, "trombone") == 0);
    CF_REQUIRE(cf_message_content_type(db, &message, test_rich_text(),
                                       &content_type) == CF_OK);
    CF_CHECK(content_type == CF_CONTENT_TYPE_SOUND);
    cf_message_dispose(&message);

    CF_REQUIRE(cf_message_find(db, 5, &message) == CF_OK);
    CF_REQUIRE(cf_message_sound(db, &message, test_rich_text(), &sound_found,
                                &sound) == CF_OK);
    CF_CHECK(!sound_found && sound == NULL);
    CF_REQUIRE(cf_message_content_type(db, &message, test_rich_text(),
                                       &content_type) == CF_OK);
    CF_CHECK(content_type == CF_CONTENT_TYPE_TEXT);
    cf_message_dispose(&message);

    cf_db_scratch_close(&scratch);
}

CF_TEST(mentionees_from_message_body) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    cf_db *db = scratch.db;
    seed_base(db);
    seed_message(db, 1, 10, 1, "c1", T0, T0);
    seed_rich_text(db, 10, 1, "<p>Hello everyone</p>", T0);

    cf_message message;
    CF_REQUIRE(cf_message_find(db, 1, &message) == CF_OK);
    cf_user_vector users;
    CF_REQUIRE(cf_message_mentionees(db, &message, test_rich_text(), &users) ==
               CF_OK);
    /* The pipeline reports no mentions for this body in both the stand-in
     * and the reference test-support; the room join itself is covered
     * directly by mentionees_in_room_joins_memberships. */
    CF_CHECK(users.len == 0);
    cf_user_vector_dispose(&users);
    cf_message_dispose(&message);

    /* Without a body record there are no mentions. */
    seed_message(db, 2, 10, 1, "c2", T0 + SEC, T0 + SEC);
    CF_REQUIRE(cf_message_find(db, 2, &message) == CF_OK);
    CF_REQUIRE(cf_message_mentionees(db, &message, test_rich_text(), &users) ==
               CF_OK);
    CF_CHECK(users.len == 0);
    cf_user_vector_dispose(&users);
    cf_message_dispose(&message);

    cf_db_scratch_close(&scratch);
}

CF_TEST(room_creator_boosts) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    cf_db *db = scratch.db;
    seed_base(db);
    seed_message(db, 1, 10, 2, "c1", T0, T0);
    seed_message(db, 2, 10, 1, "c2", T0 + SEC, T0 + SEC);
    seed_boost(db, 50, 1, 1, "++", T0 + 10 * SEC);
    seed_boost(db, 51, 1, 2, "++", T0 + 5 * SEC);
    seed_boost(db, 52, 2, 1, "++", T0);

    cf_message message;
    CF_REQUIRE(cf_message_find(db, 1, &message) == CF_OK);

    cf_room room;
    CF_REQUIRE(cf_message_room(db, &message, &room) == CF_OK);
    CF_CHECK(room.id == 10);
    cf_room_dispose(&room);

    cf_user creator;
    CF_REQUIRE(cf_message_creator(db, &message, &creator) == CF_OK);
    CF_CHECK(creator.id == 2 && strcmp(creator.name.ptr, "jason") == 0);
    cf_user_dispose(&creator);

    cf_boost_vector boosts;
    CF_REQUIRE(cf_message_boosts(db, &message, &boosts) == CF_OK);
    CF_REQUIRE(boosts.len == 2);
    CF_CHECK(boosts.items[0].id == 51 && boosts.items[1].id == 50);
    cf_boost_vector_dispose(&boosts);
    cf_message_dispose(&message);

    CF_CHECK(cf_message_room(db, NULL, &room) == CF_INVALID);
    CF_CHECK(cf_message_creator(db, NULL, &creator) == CF_INVALID);
    CF_CHECK(cf_message_boosts(db, NULL, &boosts) == CF_INVALID);

    /* The schema's foreign keys make a missing room/creator unreachable, so
     * the missing-reference path is only the NULL-message guard above. */

    cf_db_scratch_close(&scratch);
}

CF_TEST(reload_reflects_stored_row) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    cf_db *db = scratch.db;
    seed_base(db);
    seed_message(db, 1, 10, 1, "before", T0, T0);

    cf_message message;
    CF_REQUIRE(cf_message_find(db, 1, &message) == CF_OK);
    seed_stmt s;
    seed_prepare(db, &s,
                 "UPDATE \"messages\" SET \"client_message_id\" = ?, "
                 "\"updated_at\" = ? WHERE \"id\" = ?");
    seed_text(&s, 1, "after");
    seed_time(&s, 2, T0 + SEC);
    seed_i64(&s, 3, 1);
    seed_step(&s);

    CF_REQUIRE(cf_message_reload(db, &message) == CF_OK);
    CF_CHECK(strcmp(message.client_message_id.ptr, "after") == 0);
    CF_CHECK(message.updated_at == T0 + SEC);

    seed_prepare(db, &s, "DELETE FROM \"messages\" WHERE \"id\" = ?");
    seed_i64(&s, 1, 1);
    seed_step(&s);
    CF_CHECK(cf_message_reload(db, &message) == CF_NOT_FOUND);
    CF_CHECK(strcmp(message.client_message_id.ptr, "after") == 0);
    cf_message_dispose(&message);

    cf_db_scratch_close(&scratch);
}

CF_TEST(dispose_paths_are_empty_safe) {
    cf_message empty;
    memset(&empty, 0, sizeof empty);
    cf_message_dispose(&empty);
    cf_message_dispose(NULL);
    CF_CHECK(empty.client_message_id.ptr == NULL);

    cf_message_vector vector = {0};
    cf_message_vector_dispose(&vector);
    cf_message_vector_dispose(NULL);
    CF_CHECK(vector.items == NULL && vector.len == 0);

    /* Owned vector: dispose releases each copied row (ASan checks the rest). */
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_base(scratch.db);
    seed_message(scratch.db, 1, 10, 1, "c1", T0, T0);
    seed_message(scratch.db, 2, 10, 1, "c2", T0 + SEC, T0 + SEC);
    cf_message_vector found;
    CF_REQUIRE(cf_message_for_room(scratch.db, 10, &found) == CF_OK);
    CF_CHECK(found.len == 2);
    cf_message_vector_dispose(&found);
    CF_CHECK(found.items == NULL && found.len == 0 && found.cap == 0);
    cf_db_scratch_close(&scratch);
}

CF_TEST(missing_database_is_an_error_not_a_skip) {
    cf_message message;
    bool found = false;
    CF_CHECK(cf_message_find_by_id(NULL, 1, &found, &message) == CF_INVALID);
    CF_CHECK(!found && message.client_message_id.ptr == NULL);
    CF_CHECK(cf_message_last(NULL, &found, &message) == CF_INVALID);
    int64_t count = 0;
    CF_CHECK(cf_message_count(NULL, &count) == CF_INVALID);
    CF_CHECK(cf_message_find(NULL, 1, &message) == CF_INVALID);
    cf_message_vector vector;
    CF_CHECK(cf_message_for_room(NULL, 1, &vector) == CF_INVALID);
    CF_CHECK(cf_message_search_in_room(NULL, 1, lit("x"), &vector) ==
             CF_INVALID);
    CF_CHECK(cf_message_paged(NULL, 1, &found) == CF_INVALID);
    cf_user_vector users;
    const int64_t someone[] = {1};
    CF_CHECK(cf_message_mentionees_in_room(NULL, 1, someone, 1, &users) ==
             CF_INVALID);
}

/* --- writer cases (D02) --------------------------------------------------- */

/* App with the writer started (D02 bootstrap) over the seeded database. */
static void make_app(cf_app **app, const char *path) {
    const cf_config_entry entries[] = {
        {"PUBLIC_ORIGIN", "http://127.0.0.1:32123"},
        {"SECRET_KEY_BASE", HEX64},
        {"DATABASE_PATH", path},
    };
    cf_config *config = NULL;
    CF_REQUIRE(cf_config_parse(entries, 3, NULL, &config) == CF_OK);
    *app = NULL;
    CF_REQUIRE(cf_app_create(config, app) == CF_OK);
    CF_REQUIRE(cf_writer_start(*app, config) == CF_OK);
}

struct create_call {
    cf_new_message attributes;
    cf_message out;
};

static cf_err call_create(cf_tx *tx, void *arg) {
    struct create_call *call = arg;
    return cf_message_create(tx, &call->attributes, &call->out);
}

static cf_err write_create(cf_app *app, cf_new_message attributes,
                           cf_message *out) {
    struct create_call call;
    memset(&call, 0, sizeof call);
    call.attributes = attributes;
    cf_err rc = cf_write(app, call_create, &call);
    if (rc == CF_OK && call.out.id == 0) rc = CF_DB;
    if (rc != CF_OK) {
        cf_message_dispose(&call.out);
        memset(out, 0, sizeof *out);
        return rc;
    }
    *out = call.out;
    return CF_OK;
}

struct update_call {
    cf_message message;
    cf_str body;
};

static cf_err call_update_body(cf_tx *tx, void *arg) {
    struct update_call *call = arg;
    return cf_message_update_body(tx, &call->message, call->body);
}

static cf_err call_touch(cf_tx *tx, void *arg) {
    return cf_message_touch(tx, (cf_message *)arg);
}

struct replace_call {
    cf_message message;
    cf_optional_i64 blob_id;
};

static cf_err call_replace_attachment(cf_tx *tx, void *arg) {
    struct replace_call *call = arg;
    return cf_message_replace_attachment(tx, &call->message, call->blob_id);
}

static cf_err call_destroy(cf_tx *tx, void *arg) {
    return cf_message_destroy(tx, (const cf_message *)arg);
}

static cf_message clone_message(const cf_message *source) {
    cf_message copy;
    memset(&copy, 0, sizeof copy);
    copy.id = source->id;
    copy.room_id = source->room_id;
    copy.creator_id = source->creator_id;
    copy.created_at = source->created_at;
    copy.updated_at = source->updated_at;
    copy.client_message_id.len = source->client_message_id.len;
    copy.client_message_id.ptr = malloc(source->client_message_id.len + 1);
    CF_REQUIRE(copy.client_message_id.ptr != NULL);
    memcpy(copy.client_message_id.ptr, source->client_message_id.ptr,
           source->client_message_id.len);
    copy.client_message_id.ptr[source->client_message_id.len] = '\0';
    return copy;
}

CF_TEST(writer_create_returns_the_row_as_stored) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_base(scratch.db);
    seed_blob(scratch.db, 40, "attachment-key", "moon.jpg", T0);

    cf_app *app = NULL;
    make_app(&app, scratch.path);

    /* The reference's create_returns_the_row_as_stored cases, with the body
     * text the stand-in can index. */
    static const struct {
        const char *body;      /* NULL = no body assigned */
        bool has_blob;
        const char *client_id; /* NULL = defaults to a UUID */
        const char *term;      /* term the FTS row must match */
        bool expect_match;
    } cases[] = {
        {"<p>Hello <strong>alpha</strong></p>", false, "text", "alpha", true},
        {"", false, "empty body", "zulu", false},
        {NULL, true, "attachment", "moon", true},
        {"With a beta picture", true, "both", "beta", true},
        {NULL, false, NULL, "zulu", false},
    };

    cf_test_clock_set_fixed_us(T0 + 100 * SEC);
    for (size_t n = 0; n < sizeof cases / sizeof cases[0]; n++) {
        cf_new_message attributes;
        memset(&attributes, 0, sizeof attributes);
        attributes.room_id = 10;
        attributes.creator_id = 1;
        attributes.body.present = cases[n].body != NULL;
        if (cases[n].body != NULL) {
            attributes.body.value = lit(cases[n].body);
        }
        attributes.attachment_blob_id =
            (cf_optional_i64){cases[n].has_blob, 40};
        attributes.client_message_id.present = cases[n].client_id != NULL;
        if (cases[n].client_id != NULL) {
            attributes.client_message_id.value = lit(cases[n].client_id);
        }

        cf_message created;
        CF_REQUIRE(write_create(app, attributes, &created) == CF_OK);
        CF_CHECK(created.room_id == 10 && created.creator_id == 1);
        CF_CHECK(created.created_at == T0 + 100 * SEC);
        if (cases[n].client_id == NULL) {
            CF_CHECK(created.client_message_id.len == 36);
            CF_CHECK(created.client_message_id.ptr[8] == '-' &&
                     created.client_message_id.ptr[13] == '-' &&
                     created.client_message_id.ptr[18] == '-' &&
                     created.client_message_id.ptr[23] == '-');
        } else {
            CF_CHECK(strcmp(created.client_message_id.ptr,
                            cases[n].client_id) == 0);
        }

        /* The returned record is the row as stored, at the microseconds the
         * columns keep. */
        cf_message stored;
        CF_REQUIRE(cf_message_find(scratch.db, created.id, &stored) == CF_OK);
        CF_CHECK(stored.room_id == created.room_id);
        CF_CHECK(stored.creator_id == created.creator_id);
        CF_CHECK(stored.created_at == created.created_at);
        CF_CHECK(stored.updated_at == created.updated_at);
        CF_CHECK(stored.client_message_id.len ==
                 created.client_message_id.len);
        CF_CHECK(memcmp(stored.client_message_id.ptr,
                        created.client_message_id.ptr,
                        created.client_message_id.len) == 0);
        cf_message_dispose(&stored);

        /* The attachment row exists when a blob was assigned. */
        cf_attachment attachment;
        cf_blob blob;
        bool found = false;
        CF_REQUIRE(cf_message_attachment(scratch.db, &created, &found,
                                         &attachment, &blob) == CF_OK);
        CF_CHECK(found == cases[n].has_blob);
        if (found) {
            CF_CHECK(attachment.blob_id == 40);
            cf_blob_dispose(&blob);
            cf_attachment_dispose(&attachment);
        }

        /* FTS rowid matches the message id; the indexed text is the plain
         * text of the body, or the attachment filename, or "". */
        cf_message_vector matches;
        CF_REQUIRE(cf_message_search_in_room(scratch.db, 10,
                                             lit(cases[n].term),
                                             &matches) == CF_OK);
        CF_CHECK(matches.len == (cases[n].expect_match ? 1 : 0));
        if (matches.len == 1) {
            CF_CHECK(matches.items[0].id == created.id);
        }
        cf_message_vector_dispose(&matches);
        cf_message_dispose(&created);
    }

    int64_t count = 0;
    CF_REQUIRE(cf_message_count(scratch.db, &count) == CF_OK);
    CF_CHECK(count == 5);

    cf_test_clock_clear();
    cf_writer_stop(app);
    cf_app_destroy(app);
    cf_db_scratch_close(&scratch);
}

CF_TEST(writer_update_body_reindexes_and_touches) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_base(scratch.db);
    cf_app *app = NULL;
    make_app(&app, scratch.path);

    cf_test_clock_set_fixed_us(T0 + 100 * SEC);
    cf_new_message attributes;
    memset(&attributes, 0, sizeof attributes);
    attributes.room_id = 10;
    attributes.creator_id = 1;
    attributes.client_message_id.present = true;
    attributes.client_message_id.value = lit("earth");
    attributes.body.present = true;
    attributes.body.value = lit("My hovercraft is full of eels");
    cf_message message;
    CF_REQUIRE(write_create(app, attributes, &message) == CF_OK);

    cf_message_vector found;
    CF_REQUIRE(cf_message_search_in_room(scratch.db, 10, lit("eel"), &found) ==
               CF_OK);
    CF_REQUIRE(found.len == 1);
    CF_CHECK(found.items[0].id == message.id);
    cf_message_vector_dispose(&found);

    cf_test_clock_set_fixed_us(T0 + 200 * SEC);
    struct update_call update;
    memset(&update, 0, sizeof update);
    update.message = clone_message(&message);
    update.body = lit("My hovercraft is full of sharks");
    CF_REQUIRE(cf_write(app, call_update_body, &update) == CF_OK);
    CF_CHECK(update.message.updated_at == T0 + 200 * SEC);
    cf_message_dispose(&update.message);

    cf_message reloaded;
    CF_REQUIRE(cf_message_find(scratch.db, message.id, &reloaded) == CF_OK);
    CF_CHECK(reloaded.updated_at == T0 + 200 * SEC);
    cf_message_dispose(&message);
    message = reloaded; /* the stored row, for the no-op check below */

    CF_REQUIRE(cf_message_search_in_room(scratch.db, 10, lit("sharks"),
                                         &found) == CF_OK);
    CF_REQUIRE(found.len == 1);
    CF_CHECK(found.items[0].id == message.id);
    cf_message_vector_dispose(&found);
    CF_REQUIRE(cf_message_search_in_room(scratch.db, 10, lit("eels"), &found) ==
               CF_OK);
    CF_CHECK(found.len == 0);
    cf_message_vector_dispose(&found);

    /* An identical body is a no-op: no touch, updated_at unchanged. */
    cf_test_clock_set_fixed_us(T0 + 300 * SEC);
    memset(&update, 0, sizeof update);
    update.message = clone_message(&message);
    update.body = lit("My hovercraft is full of sharks");
    CF_REQUIRE(cf_write(app, call_update_body, &update) == CF_OK);
    CF_CHECK(update.message.updated_at == T0 + 200 * SEC);
    cf_message_dispose(&update.message);

    /* A message without a body gets one (create path), reindexing "". */
    memset(&attributes, 0, sizeof attributes);
    attributes.room_id = 10;
    attributes.creator_id = 1;
    attributes.client_message_id.present = true;
    attributes.client_message_id.value = lit("nobody");
    cf_message bodyless;
    CF_REQUIRE(write_create(app, attributes, &bodyless) == CF_OK);
    cf_test_clock_set_fixed_us(T0 + 400 * SEC);
    memset(&update, 0, sizeof update);
    update.message = clone_message(&bodyless);
    update.body = lit("now with text");
    CF_REQUIRE(cf_write(app, call_update_body, &update) == CF_OK);
    cf_message_dispose(&update.message);
    CF_REQUIRE(cf_message_search_in_room(scratch.db, 10, lit("text"), &found) ==
               CF_OK);
    CF_REQUIRE(found.len == 1);
    CF_CHECK(found.items[0].id == bodyless.id);
    cf_message_vector_dispose(&found);
    cf_message_dispose(&bodyless);
    cf_message_dispose(&message);

    cf_test_clock_clear();
    cf_writer_stop(app);
    cf_app_destroy(app);
    cf_db_scratch_close(&scratch);
}

CF_TEST(writer_touch_and_replace_attachment) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_base(scratch.db);
    seed_blob(scratch.db, 40, "blob-a", "a.jpg", T0);
    seed_blob(scratch.db, 41, "blob-b", "b.jpg", T0);
    cf_app *app = NULL;
    make_app(&app, scratch.path);

    cf_test_clock_set_fixed_us(T0 + 100 * SEC);
    cf_new_message attributes;
    memset(&attributes, 0, sizeof attributes);
    attributes.room_id = 10;
    attributes.creator_id = 1;
    attributes.client_message_id.present = true;
    attributes.client_message_id.value = lit("att");
    cf_message message;
    CF_REQUIRE(write_create(app, attributes, &message) == CF_OK);

    /* touch: message and room updated_at both move. */
    int64_t message_id = message.id;
    cf_test_clock_set_fixed_us(T0 + 200 * SEC);
    struct update_call touch = {0};
    touch.message = clone_message(&message);
    CF_REQUIRE(cf_write(app, call_touch, &touch) == CF_OK);
    CF_CHECK(touch.message.updated_at == T0 + 200 * SEC);
    cf_message_dispose(&touch.message);
    cf_message_dispose(&message);
    CF_REQUIRE(cf_message_find(scratch.db, message_id, &message) == CF_OK);
    CF_CHECK(message.updated_at == T0 + 200 * SEC);

    /* replace_attachment(Some): attach, then swap. */
    struct replace_call replace;
    memset(&replace, 0, sizeof replace);
    replace.message = clone_message(&message);
    replace.blob_id = (cf_optional_i64){true, 40};
    CF_REQUIRE(cf_write(app, call_replace_attachment, &replace) == CF_OK);
    cf_message_dispose(&replace.message);
    cf_attachment attachment;
    cf_blob blob;
    bool found = false;
    CF_REQUIRE(cf_message_attachment(scratch.db, &message, &found, &attachment,
                                     &blob) == CF_OK);
    CF_REQUIRE(found);
    CF_CHECK(attachment.blob_id == 40);
    cf_blob_dispose(&blob);
    cf_attachment_dispose(&attachment);

    memset(&replace, 0, sizeof replace);
    replace.message = clone_message(&message);
    replace.blob_id = (cf_optional_i64){true, 41};
    CF_REQUIRE(cf_write(app, call_replace_attachment, &replace) == CF_OK);
    cf_message_dispose(&replace.message);
    CF_REQUIRE(cf_message_attachment(scratch.db, &message, &found, &attachment,
                                     &blob) == CF_OK);
    CF_REQUIRE(found);
    CF_CHECK(attachment.blob_id == 41);
    cf_blob_dispose(&blob);
    cf_attachment_dispose(&attachment);
    bool ok = true;
    int64_t attachments = cf_db_test_i64(cf_db_handle(scratch.db),
                                         "SELECT COUNT(*) FROM "
                                         "active_storage_attachments WHERE "
                                         "record_id = 1",
                                         &ok);
    CF_CHECK(ok && attachments == 1);

    /* replace_attachment(absent) removes it. */
    memset(&replace, 0, sizeof replace);
    replace.message = clone_message(&message);
    replace.blob_id = (cf_optional_i64){false, 0};
    CF_REQUIRE(cf_write(app, call_replace_attachment, &replace) == CF_OK);
    cf_message_dispose(&replace.message);
    CF_REQUIRE(cf_message_attachment(scratch.db, &message, &found, &attachment,
                                     &blob) == CF_OK);
    CF_CHECK(!found);
    attachments = cf_db_test_i64(cf_db_handle(scratch.db),
                                 "SELECT COUNT(*) FROM "
                                 "active_storage_attachments WHERE "
                                 "record_id = 1",
                                 &ok);
    CF_CHECK(ok && attachments == 0);

    cf_message_dispose(&message);
    cf_test_clock_clear();
    cf_writer_stop(app);
    cf_app_destroy(app);
    cf_db_scratch_close(&scratch);
}

CF_TEST(writer_destroy_removes_dependencies_and_index) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_base(scratch.db);
    seed_blob(scratch.db, 40, "doomed", "doomed.jpg", T0);
    cf_app *app = NULL;
    make_app(&app, scratch.path);

    cf_test_clock_set_fixed_us(T0 + 100 * SEC);
    cf_new_message attributes;
    memset(&attributes, 0, sizeof attributes);
    attributes.room_id = 10;
    attributes.creator_id = 1;
    attributes.client_message_id.present = true;
    attributes.client_message_id.value = lit("doomed");
    attributes.body.present = true;
    attributes.body.value = lit("sharks are circling");
    attributes.attachment_blob_id = (cf_optional_i64){true, 40};
    cf_message message;
    CF_REQUIRE(write_create(app, attributes, &message) == CF_OK);

    cf_message_vector found;
    CF_REQUIRE(cf_message_search_in_room(scratch.db, 10, lit("shark"), &found) ==
               CF_OK);
    CF_REQUIRE(found.len == 1);
    cf_message_vector_dispose(&found);

    seed_boost(scratch.db, 60, message.id, 2, "++", T0 + SEC);

    cf_test_clock_set_fixed_us(T0 + 500 * SEC);
    cf_message doomed = clone_message(&message);
    CF_REQUIRE(cf_write(app, call_destroy, &doomed) == CF_OK);
    cf_message_dispose(&doomed);

    cf_message missing;
    CF_CHECK(cf_message_find(scratch.db, message.id, &missing) ==
             CF_NOT_FOUND);
    CF_CHECK(missing.client_message_id.ptr == NULL);
    CF_REQUIRE(cf_message_search_in_room(scratch.db, 10, lit("shark"), &found) ==
               CF_OK);
    CF_CHECK(found.len == 0);
    cf_message_vector_dispose(&found);
    bool ok = true;
    sqlite3 *handle = cf_db_handle(scratch.db);
    CF_CHECK(cf_db_test_i64(handle,
                            "SELECT COUNT(*) FROM message_search_index",
                            &ok) == 0 && ok);
    CF_CHECK(cf_db_test_i64(handle,
                            "SELECT COUNT(*) FROM action_text_rich_texts",
                            &ok) == 0 && ok);
    CF_CHECK(cf_db_test_i64(handle,
                            "SELECT COUNT(*) FROM active_storage_attachments",
                            &ok) == 0 && ok);
    CF_CHECK(cf_db_test_i64(handle, "SELECT COUNT(*) FROM boosts", &ok) == 0 &&
             ok);
    /* The room was touched. */
    char updated[CF_DB_TIME_TEXT_CAP];
    char want[CF_DB_TIME_TEXT_CAP];
    CF_REQUIRE(cf_db_time_to_text(T0 + 500 * SEC, want) == CF_OK);
    CF_CHECK(cf_db_test_text(handle,
                             "SELECT updated_at FROM rooms WHERE id = 10",
                             updated, sizeof updated) != NULL);
    CF_CHECK(strcmp(updated, want) == 0);

    cf_message_dispose(&message);
    cf_test_clock_clear();
    cf_writer_stop(app);
    cf_app_destroy(app);
    cf_db_scratch_close(&scratch);
}

CF_TEST_MAIN()
