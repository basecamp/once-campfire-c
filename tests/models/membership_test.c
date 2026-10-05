/* D01 model family "membership" tests.
 *
 * Reference oracles (pinned; hashes in contracts/reference-files.json):
 *  - tmp/rust-ref/crates/db/src/models/membership.rs (the implemented source,
 *    29 inventory symbols: involvement helpers, reads, predicates, mutations);
 *  - tests/fixtures/crates/db/src/tests/membership_test.rs porting
 *    tmp/rails-ref/test/models/membership_test.rb: connected/disconnected
 *    scopes and the 60 s TTL, connect counters and stale resets, present
 *    mark-read without touching updated_at, disconnect_all, destroy's
 *    reconnect event, and read's no-op when already read;
 *  - tmp/rails-ref/test/fixtures/{users,rooms,memberships}.yml: the rows below
 *    keep the fixture labels, relationships and involvement values, with the
 *    fixed timestamps this file asserts (the C tests have no YAML loader).
 *    Fixture ids are ActiveRecord::FixtureSet.identify(label) =
 *    Zlib.crc32(label) % (2**30 - 1); "david" = 127326141 matches the
 *    fixtures.rs identify_matches_rails oracle.
 *
 * Mutation cases run through D02's cf_write on a scratch database created by
 * cf_db_open (fresh schema, D-C01); a fixed test clock makes every touch
 * deterministic (no sleeps).  destroy's DISCONNECT_USER event is a mandatory
 * post-commit control event in D02, so that case registers a recording
 * control handler before writing.
 *
 * Build (plain; add -fsanitize=address,undefined -fno-sanitize-recover=all
 * -fno-omit-frame-pointer -g for the sanitize run).  The model set is all
 * fifteen families because room/user call across modules:
 *   clang -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra
 *         -Werror -pthread -Isrc -Itests
 *         -Ivendor/src/sqlite/sqlite-amalgamation-3530400
 *         -Ivendor/src/yyjson/src
 *         tests/models/membership_test.c (every src/models family .c)
 *         src/views/escape.c
 *         src/db/{schema,reader,statements,writer}.c
 *         src/core/{alloc,buffer,clock,error,random}.c
 *         src/config.c src/app.c
 *         vendor/build/sqlite-clang/libsqlite3.a
 *         vendor/build/yyjson-clang/libyyjson.a -lm
 *         -o build/d01-model-membership/plain/test_membership
 * A01 (cf_password_verify), R02 (cf_richtext_*) and the shared types.h
 * disposers are production sources in the full test link now; the membership
 * tests never execute the password/rich-text paths.
 */
#include "models/membership.h"

#include "app.h"
#include "cf.h"
#include "config.h"
#include "core/testclock.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "db/writer.h"
#include "models/user.h"
#include "cf_test.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MW_ORIGIN "http://127.0.0.1:32123"
#define MW_HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

/* Fixed instants: "2026-01-01 00:00:00" (no fractional text),
 * "2026-01-02 03:04:05.678901", that instant +61 s (past CONNECTION_TTL),
 * and -61 s / -60 s of it. */
#define MW_T1_TEXT "2026-01-01 00:00:00"
#define MW_T2_TEXT "2026-01-02 03:04:05.678901"
#define MW_T3_TEXT "2026-01-02 03:05:06.678901"
#define MW_STALE_TEXT "2026-01-02 03:03:04.678901"  /* T2 - 61 s */
#define MW_CUTOFF_TEXT "2026-01-02 03:03:05.678901" /* T2 - 60 s */
#define MW_T1_US INT64_C(1767225600000000)
#define MW_T2_US INT64_C(1767323045678901)
#define MW_T3_US INT64_C(1767323106678901)
#define MW_STALE_US (MW_T2_US - INT64_C(61000000))
#define MW_CUTOFF_US (MW_T2_US - INT64_C(60000000))

/* Reference fixture rows (labels and relationships from the YAML files). */
#define MW_U_DAVID 127326141
#define MW_U_JASON 149087659
#define MW_U_JZ 773523953
#define MW_U_KEVIN 712064548
#define MW_U_BENDER 394959859
#define MW_R_PETS 104393281
#define MW_R_HQ 201306877
#define MW_R_WATERCOOLER 486777696
#define MW_R_DESIGNERS 654632876
#define MW_R_DAVID_JASON 186869642
#define MW_M_DAVID_WATERCOOLER 774341722
#define MW_M_JASON_WATERCOOLER 505016279
#define MW_M_BENDER_WATERCOOLER 963867642
#define MW_M_DAVID_DESIGNERS 146174848
#define MW_M_JASON_DESIGNERS 897066373
#define MW_M_DAVID_DIRECT 449028908
#define MW_M_JASON_DIRECT 198590975
#define MW_M_DAVID_HQ 211379269

/* SQL fragments for nullable seed columns. */
#define MW_SQL_NULL "NULL"
#define MW_SQL_INV_EVERYTHING "'everything'"
#define MW_SQL_INV_INVISIBLE "'invisible'"

/* --- scratch database + app (D02 writer) --------------------------------- */

typedef struct {
    char *dir;
    char path[CF_DB_TEST_PATH_CAP];
    cf_app *app;
} mw_world;

static void mw_world_open(mw_world *world) {
    memset(world, 0, sizeof *world);
    world->dir = cf_db_test_dir();
    CF_REQUIRE(world->dir != NULL);
    cf_db_test_path(world->path, sizeof world->path, world->dir);

    /* Fresh schema (D-C01) so the writer's connection opens an existing
     * version-1 database. */
    cf_db *db = NULL;
    CF_REQUIRE(cf_db_open(world->path, false, &db) == CF_OK);
    cf_db_close(db);

    cf_config_entry entries[3] = {
        {"PUBLIC_ORIGIN", MW_ORIGIN},
        {"SECRET_KEY_BASE", MW_HEX64},
        {"DATABASE_PATH", world->path},
    };
    cf_config *config = NULL;
    CF_REQUIRE(cf_config_parse(entries, 3, NULL, &config) == CF_OK);
    CF_REQUIRE(cf_app_create(config, &world->app) == CF_OK);
    /* D02: cf_write returns CF_INTERNAL until the writer thread is started;
     * config is borrowed for the call, the app still owns it. */
    CF_REQUIRE(cf_writer_start(world->app, config) == CF_OK);
}

static void mw_world_dispose(mw_world *world) {
    cf_writer_stop(world->app);
    cf_app_destroy(world->app);
    world->app = NULL;
    if (world->dir != NULL) {
        cf_db_test_cleanup(world->dir);
        free(world->dir);
        world->dir = NULL;
    }
}

static cf_db *mw_rw(const mw_world *world) {
    cf_db *db = NULL;
    CF_REQUIRE(cf_db_open(world->path, false, &db) == CF_OK);
    return db;
}

static cf_db *mw_reader(const mw_world *world) {
    cf_db *db = NULL;
    CF_REQUIRE(cf_db_open(world->path, true, &db) == CF_OK);
    return db;
}

/* --- fixture-shaped seeding ----------------------------------------------- */

static void mw_exec(cf_db *db, const char *sql) {
    int rc = cf_db_test_exec(cf_db_handle(db), sql);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "    seed SQL failed (%d: %s): %s\n", rc,
                sqlite3_errmsg(cf_db_handle(db)), sql);
    }
    CF_REQUIRE(rc == SQLITE_OK);
}

static void mw_seed_user(cf_db *db, int64_t id, const char *name, int role) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO \"users\" (\"id\",\"name\",\"role\",\"status\","
             "\"created_at\",\"updated_at\") "
             "VALUES (%lld,'%s',%d,0,'" MW_T1_TEXT "','" MW_T2_TEXT "')",
             (long long)id, name, role);
    mw_exec(db, sql);
}

static void mw_seed_room(cf_db *db, int64_t id, const char *name,
                         const char *type, int64_t creator_id) {
    char text[256];
    if (name != NULL) {
        snprintf(text, sizeof text, "'%s'", name);
    } else {
        snprintf(text, sizeof text, "NULL");
    }
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO \"rooms\" (\"id\",\"name\",\"type\",\"creator_id\","
             "\"created_at\",\"updated_at\") "
             "VALUES (%lld,%s,'%s',%lld,'" MW_T1_TEXT "','" MW_T2_TEXT "')",
             (long long)id, text, type, (long long)creator_id);
    mw_exec(db, sql);
}

/* involvement=NULL omits the column (fixture rows without it get the schema
 * default "mentions"); otherwise it is a raw SQL fragment ("NULL",
 * "'everything'", "'invisible'").  unread_at/connected_at are raw fragments
 * ("NULL" or "'2026-01-02 03:04:05.678901'"). */
static void mw_seed_membership(cf_db *db, int64_t id, int64_t room_id,
                               int64_t user_id, const char *involvement,
                               const char *unread_at,
                               const char *connected_at,
                               int64_t connections) {
    char sql[768];
    if (involvement == NULL) {
        snprintf(sql, sizeof sql,
                 "INSERT INTO \"memberships\" (\"id\",\"room_id\",\"user_id\","
                 "\"unread_at\",\"connected_at\",\"connections\","
                 "\"created_at\",\"updated_at\") "
                 "VALUES (%lld,%lld,%lld,%s,%s,%lld,'" MW_T1_TEXT
                 "','" MW_T2_TEXT "')",
                 (long long)id, (long long)room_id, (long long)user_id,
                 unread_at, connected_at, (long long)connections);
    } else {
        snprintf(sql, sizeof sql,
                 "INSERT INTO \"memberships\" (\"id\",\"room_id\",\"user_id\","
                 "\"involvement\",\"unread_at\",\"connected_at\","
                 "\"connections\",\"created_at\",\"updated_at\") "
                 "VALUES (%lld,%lld,%lld,%s,%s,%s,%lld,'" MW_T1_TEXT
                 "','" MW_T2_TEXT "')",
                 (long long)id, (long long)room_id, (long long)user_id,
                 involvement, unread_at, connected_at,
                 (long long)connections);
    }
    mw_exec(db, sql);
}

/* The fixture set used by most read cases: David, Jason, Bender and the
 * watercooler/designers rooms, with the YAML involvement values. */
static void mw_seed_core_fixture(cf_db *db) {
    mw_seed_user(db, MW_U_DAVID, "David", 1 /* administrator */);
    mw_seed_user(db, MW_U_JASON, "Jason", 1 /* administrator */);
    mw_seed_user(db, MW_U_BENDER, "Bender Bot", 2 /* bot */);
    mw_seed_room(db, MW_R_WATERCOOLER, "All Talk", "Rooms::Closed",
                 MW_U_DAVID);
    mw_seed_room(db, MW_R_DESIGNERS, "Designers", "Rooms::Closed",
                 MW_U_DAVID);
    mw_seed_membership(db, MW_M_DAVID_WATERCOOLER, MW_R_WATERCOOLER,
                       MW_U_DAVID, MW_SQL_INV_EVERYTHING, MW_SQL_NULL,
                       MW_SQL_NULL, 0);
    mw_seed_membership(db, MW_M_JASON_WATERCOOLER, MW_R_WATERCOOLER,
                       MW_U_JASON, MW_SQL_INV_EVERYTHING, MW_SQL_NULL,
                       MW_SQL_NULL, 0);
    mw_seed_membership(db, MW_M_BENDER_WATERCOOLER, MW_R_WATERCOOLER,
                       MW_U_BENDER, NULL, MW_SQL_NULL, MW_SQL_NULL, 0);
    mw_seed_membership(db, MW_M_DAVID_DESIGNERS, MW_R_DESIGNERS, MW_U_DAVID,
                       NULL, MW_SQL_NULL, MW_SQL_NULL, 0);
    mw_seed_membership(db, MW_M_JASON_DESIGNERS, MW_R_DESIGNERS, MW_U_JASON,
                       MW_SQL_INV_EVERYTHING, MW_SQL_NULL, MW_SQL_NULL, 0);
}

/* --- query helpers -------------------------------------------------------- */

static int64_t mw_i64(cf_db *db, const char *sql) {
    bool ok = false;
    int64_t value = cf_db_test_i64(cf_db_handle(db), sql, &ok);
    CF_REQUIRE(ok);
    return value;
}

static void mw_expect_text(cf_db *db, const char *sql, const char *expected) {
    char buf[128];
    const char *got = cf_db_test_text(cf_db_handle(db), sql, buf, sizeof buf);
    CF_CHECK(got != NULL);
    if (got != NULL) CF_CHECK(strcmp(got, expected) == 0);
}

static bool mw_membership_is(const cf_membership *m, int64_t id,
                             int64_t room_id, int64_t user_id,
                             bool inv_present, cf_involvement inv, bool unread,
                             bool connected, int64_t connections,
                             int64_t created_at, int64_t updated_at) {
    return m->id == id && m->room_id == room_id && m->user_id == user_id &&
           m->involvement.present == inv_present &&
           (!inv_present || m->involvement.value == inv) &&
           m->unread_at.present == unread &&
           m->connected_at.present == connected &&
           m->connections == connections && m->created_at == created_at &&
           m->updated_at == updated_at;
}

static bool mw_membership_empty(const cf_membership *m) {
    return m->id == 0 && m->room_id == 0 && m->user_id == 0 &&
           !m->involvement.present && !m->unread_at.present &&
           !m->connected_at.present && m->connections == 0 &&
           m->created_at == 0 && m->updated_at == 0;
}

/* --- involvement helpers -------------------------------------------------- */

CF_TEST(involvement_names_round_trip_in_declaration_order) {
    CF_CHECK(strcmp(cf_involvement_name(CF_INVOLVEMENT_INVISIBLE),
                    "invisible") == 0);
    CF_CHECK(strcmp(cf_involvement_name(CF_INVOLVEMENT_NOTHING),
                    "nothing") == 0);
    CF_CHECK(strcmp(cf_involvement_name(CF_INVOLVEMENT_MENTIONS),
                    "mentions") == 0);
    CF_CHECK(strcmp(cf_involvement_name(CF_INVOLVEMENT_EVERYTHING),
                    "everything") == 0);

    static const struct {
        const char *text;
        cf_involvement value;
    } names[] = {
        {"invisible", CF_INVOLVEMENT_INVISIBLE},
        {"nothing", CF_INVOLVEMENT_NOTHING},
        {"mentions", CF_INVOLVEMENT_MENTIONS},
        {"everything", CF_INVOLVEMENT_EVERYTHING},
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        cf_involvement out = (cf_involvement)99;
        cf_str text = {(char *)names[i].text, strlen(names[i].text)};
        CF_CHECK(cf_involvement_from_name(text, &out));
        CF_CHECK(out == names[i].value);
    }
}

CF_TEST(involvement_from_name_rejects_unknown_empty_and_null) {
    cf_involvement out = CF_INVOLVEMENT_INVISIBLE;
    /* Exact match only: case and prefixes are not accepted, and an unknown
     * stored value must not silently map (the reference FromSql errors). */
    cf_str unknown = {(char *)"Mentions", 8};
    CF_CHECK(!cf_involvement_from_name(unknown, &out));
    CF_CHECK(out == CF_INVOLVEMENT_INVISIBLE); /* untouched on failure */
    cf_str partial = {(char *)"ment", 4};
    CF_CHECK(!cf_involvement_from_name(partial, &out));
    cf_str empty = {NULL, 0};
    CF_CHECK(!cf_involvement_from_name(empty, &out));
    CF_CHECK(!cf_involvement_from_name(empty, NULL));
}

/* --- read functions ------------------------------------------------------- */

CF_TEST(find_reads_every_column_and_missing_is_not_found) {
    mw_world world;
    mw_world_open(&world);
    cf_db *db = mw_rw(&world);
    mw_seed_core_fixture(db);

    cf_membership out;
    memset(&out, 0xA5, sizeof out);
    CF_CHECK(cf_membership_find(db, MW_M_DAVID_WATERCOOLER, &out) == CF_OK);
    CF_CHECK(mw_membership_is(&out, MW_M_DAVID_WATERCOOLER, MW_R_WATERCOOLER,
                              MW_U_DAVID, true, CF_INVOLVEMENT_EVERYTHING,
                              false, false, 0, MW_T1_US, MW_T2_US));
    cf_db_close(db);

    /* Missing id: CF_NOT_FOUND with an empty record, not an empty success. */
    cf_db *reader = mw_reader(&world);
    cf_membership missing;
    memset(&missing, 0xA5, sizeof missing);
    CF_CHECK(cf_membership_find(reader, 999999, &missing) == CF_NOT_FOUND);
    CF_CHECK(mw_membership_empty(&missing));

    /* Argument failure paths. */
    cf_membership scratch;
    memset(&scratch, 0xA5, sizeof scratch);
    CF_CHECK(cf_membership_find(NULL, 1, &scratch) == CF_INVALID);
    CF_CHECK(mw_membership_empty(&scratch));
    CF_CHECK(cf_membership_find(reader, 1, NULL) == CF_INVALID);
    cf_db_close(reader);
    mw_world_dispose(&world);
}

CF_TEST(find_keeps_null_involvement_absent) {
    mw_world world;
    mw_world_open(&world);
    cf_db *db = mw_rw(&world);
    mw_seed_user(db, MW_U_DAVID, "David", 1);
    mw_seed_room(db, MW_R_WATERCOOLER, "All Talk", "Rooms::Closed",
                 MW_U_DAVID);
    mw_seed_membership(db, MW_M_DAVID_WATERCOOLER, MW_R_WATERCOOLER,
                       MW_U_DAVID, MW_SQL_NULL, MW_SQL_NULL, MW_SQL_NULL, 0);

    cf_membership out;
    memset(&out, 0, sizeof out);
    CF_CHECK(cf_membership_find(db, MW_M_DAVID_WATERCOOLER, &out) == CF_OK);
    CF_CHECK(!out.involvement.present);
    CF_CHECK(mw_membership_is(&out, MW_M_DAVID_WATERCOOLER, MW_R_WATERCOOLER,
                              MW_U_DAVID, false, CF_INVOLVEMENT_MENTIONS,
                              false, false, 0, MW_T1_US, MW_T2_US));
    cf_db_close(db);
    mw_world_dispose(&world);
}

CF_TEST(find_rejects_unknown_involvement_and_bad_datetime_text) {
    mw_world world;
    mw_world_open(&world);
    cf_db *db = mw_rw(&world);
    mw_seed_user(db, MW_U_DAVID, "David", 1);
    mw_seed_room(db, MW_R_WATERCOOLER, "All Talk", "Rooms::Closed",
                 MW_U_DAVID);
    mw_seed_membership(db, MW_M_DAVID_WATERCOOLER, MW_R_WATERCOOLER,
                       MW_U_DAVID, MW_SQL_NULL, MW_SQL_NULL, MW_SQL_NULL, 0);

    /* A stored enum value the Rust FromSql rejects. */
    char sql[128];
    snprintf(sql, sizeof sql,
             "UPDATE \"memberships\" SET \"involvement\" = 'shouting' "
             "WHERE \"id\" = %lld",
             (long long)MW_M_DAVID_WATERCOOLER);
    mw_exec(db, sql);
    cf_membership out;
    memset(&out, 0, sizeof out);
    CF_CHECK(cf_membership_find(db, MW_M_DAVID_WATERCOOLER, &out) == CF_DB);

    snprintf(sql, sizeof sql,
             "UPDATE \"memberships\" SET \"involvement\" = 'mentions', "
             "\"created_at\" = 'not a datetime' WHERE \"id\" = %lld",
             (long long)MW_M_DAVID_WATERCOOLER);
    mw_exec(db, sql);
    CF_CHECK(cf_membership_find(db, MW_M_DAVID_WATERCOOLER, &out) == CF_DB);
    cf_db_close(db);
    mw_world_dispose(&world);
}

CF_TEST(count_counts_every_membership) {
    mw_world world;
    mw_world_open(&world);
    cf_db *db = mw_rw(&world);
    mw_seed_core_fixture(db);

    int64_t count = -1;
    CF_CHECK(cf_membership_count(db, &count) == CF_OK);
    CF_CHECK(count == 5);
    CF_CHECK(cf_membership_count(db, NULL) == CF_INVALID);
    CF_CHECK(cf_membership_count(NULL, &count) == CF_INVALID);
    cf_db_close(db);
    mw_world_dispose(&world);
}

CF_TEST(for_user_returns_users_memberships_only) {
    mw_world world;
    mw_world_open(&world);
    cf_db *db = mw_rw(&world);
    mw_seed_core_fixture(db);

    cf_membership_vector davids = {0};
    CF_CHECK(cf_membership_for_user(db, MW_U_DAVID, &davids) == CF_OK);
    CF_REQUIRE(davids.len == 2);
    bool saw_watercooler = false, saw_designers = false;
    for (size_t i = 0; i < davids.len; i++) {
        if (davids.items[i].id == MW_M_DAVID_WATERCOOLER) {
            saw_watercooler = true;
            CF_CHECK(davids.items[i].involvement.present &&
                     davids.items[i].involvement.value ==
                         CF_INVOLVEMENT_EVERYTHING);
        }
        if (davids.items[i].id == MW_M_DAVID_DESIGNERS) {
            saw_designers = true;
            CF_CHECK(davids.items[i].involvement.present &&
                     davids.items[i].involvement.value ==
                         CF_INVOLVEMENT_MENTIONS); /* column default */
        }
    }
    CF_CHECK(saw_watercooler && saw_designers);
    cf_membership_vector_dispose(&davids);

    cf_membership_vector unknown = {0};
    CF_CHECK(cf_membership_for_user(db, 424242, &unknown) == CF_OK);
    CF_CHECK(unknown.len == 0 && unknown.items == NULL);

    CF_CHECK(cf_membership_for_user(db, MW_U_DAVID, NULL) == CF_INVALID);
    CF_CHECK(cf_membership_for_user(NULL, MW_U_DAVID, &unknown) == CF_INVALID);
    cf_db_close(db);
    mw_world_dispose(&world);
}

CF_TEST(for_room_returns_rooms_memberships_only) {
    mw_world world;
    mw_world_open(&world);
    cf_db *db = mw_rw(&world);
    mw_seed_core_fixture(db);

    cf_membership_vector room = {0};
    CF_CHECK(cf_membership_for_room(db, MW_R_WATERCOOLER, &room) == CF_OK);
    CF_REQUIRE(room.len == 3);
    bool saw_david = false, saw_jason = false, saw_bender = false;
    for (size_t i = 0; i < room.len; i++) {
        CF_CHECK(room.items[i].room_id == MW_R_WATERCOOLER);
        saw_david = saw_david || room.items[i].user_id == MW_U_DAVID;
        saw_jason = saw_jason || room.items[i].user_id == MW_U_JASON;
        saw_bender = saw_bender || room.items[i].user_id == MW_U_BENDER;
    }
    CF_CHECK(saw_david && saw_jason && saw_bender);
    cf_membership_vector_dispose(&room);

    cf_membership_vector designers = {0};
    CF_CHECK(cf_membership_for_room(db, MW_R_DESIGNERS, &designers) == CF_OK);
    CF_CHECK(designers.len == 2);
    cf_membership_vector_dispose(&designers);

    cf_membership_vector unknown = {0};
    CF_CHECK(cf_membership_for_room(db, 424242, &unknown) == CF_OK);
    CF_CHECK(unknown.len == 0 && unknown.items == NULL);

    CF_CHECK(cf_membership_for_room(db, MW_R_WATERCOOLER, NULL) == CF_INVALID);
    CF_CHECK(cf_membership_for_room(NULL, MW_R_WATERCOOLER, &unknown) ==
             CF_INVALID);
    cf_db_close(db);
    mw_world_dispose(&world);
}

CF_TEST(find_by_room_and_user_hits_exact_pair_and_misses_are_empty) {
    mw_world world;
    mw_world_open(&world);
    cf_db *db = mw_rw(&world);
    mw_seed_core_fixture(db);

    bool found = false;
    cf_membership out;
    memset(&out, 0, sizeof out);
    CF_CHECK(cf_membership_find_by_room_and_user(
                 db, MW_R_WATERCOOLER, MW_U_DAVID, &found, &out) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(mw_membership_is(&out, MW_M_DAVID_WATERCOOLER, MW_R_WATERCOOLER,
                              MW_U_DAVID, true, CF_INVOLVEMENT_EVERYTHING,
                              false, false, 0, MW_T1_US, MW_T2_US));

    /* David is not a member of pets in this seed. */
    found = true;
    memset(&out, 0xA5, sizeof out);
    CF_CHECK(cf_membership_find_by_room_and_user(db, MW_R_PETS, MW_U_DAVID,
                                                 &found, &out) == CF_OK);
    CF_CHECK(!found);
    CF_CHECK(mw_membership_empty(&out));
    found = true;
    memset(&out, 0, sizeof out);
    CF_CHECK(cf_membership_find_by_room_and_user(db, MW_R_WATERCOOLER, 424242,
                                                 &found, &out) == CF_OK);
    CF_CHECK(!found);
    CF_CHECK(mw_membership_empty(&out));

    CF_CHECK(cf_membership_find_by_room_and_user(db, MW_R_WATERCOOLER,
                                                 MW_U_DAVID, NULL, &out) ==
             CF_INVALID);
    CF_CHECK(cf_membership_find_by_room_and_user(db, MW_R_WATERCOOLER,
                                                 MW_U_DAVID, &found, NULL) ==
             CF_INVALID);
    CF_CHECK(cf_membership_find_by_room_and_user(NULL, MW_R_WATERCOOLER,
                                                 MW_U_DAVID, &found, &out) ==
             CF_INVALID);
    cf_db_close(db);
    mw_world_dispose(&world);
}

/* Seed the watercooler/designers/hq rooms, an invisible HQ membership for
 * David, and two mixed-case rooms proving LOWER() ordering. */
static void mw_seed_ordered_rooms(cf_db *db) {
    mw_seed_user(db, MW_U_DAVID, "David", 1);
    mw_seed_room(db, MW_R_WATERCOOLER, "All Talk", "Rooms::Closed",
                 MW_U_DAVID);
    mw_seed_room(db, MW_R_DESIGNERS, "Designers", "Rooms::Closed",
                 MW_U_DAVID);
    mw_seed_room(db, MW_R_HQ, "HQ", "Rooms::Open", MW_U_DAVID);
    mw_seed_room(db, 900001, "alpha room", "Rooms::Closed", MW_U_DAVID);
    mw_seed_room(db, 900002, "Zulu room", "Rooms::Closed", MW_U_DAVID);
    mw_seed_membership(db, MW_M_DAVID_WATERCOOLER, MW_R_WATERCOOLER,
                       MW_U_DAVID, MW_SQL_INV_EVERYTHING, MW_SQL_NULL,
                       MW_SQL_NULL, 0);
    mw_seed_membership(db, MW_M_DAVID_DESIGNERS, MW_R_DESIGNERS, MW_U_DAVID,
                       NULL, MW_SQL_NULL, MW_SQL_NULL, 0);
    mw_seed_membership(db, MW_M_DAVID_HQ, MW_R_HQ, MW_U_DAVID,
                       MW_SQL_INV_INVISIBLE, MW_SQL_NULL, MW_SQL_NULL, 0);
    mw_seed_membership(db, 900003, 900001, MW_U_DAVID, NULL,
                       MW_SQL_NULL, MW_SQL_NULL, 0);
    mw_seed_membership(db, 900004, 900002, MW_U_DAVID, NULL,
                       MW_SQL_NULL, MW_SQL_NULL, 0);
}

CF_TEST(visible_with_ordered_room_excludes_invisible_and_sorts_by_lower_name) {
    mw_world world;
    mw_world_open(&world);
    cf_db *db = mw_rw(&world);
    mw_seed_ordered_rooms(db);

    cf_membership_room_pair_vector pairs = {0};
    CF_CHECK(cf_membership_visible_with_ordered_room(db, MW_U_DAVID, &pairs) ==
             CF_OK);
    CF_REQUIRE(pairs.len == 4);
    static const char *expected_names[] = {"All Talk", "alpha room",
                                           "Designers", "Zulu room"};
    for (size_t i = 0; i < pairs.len; i++) {
        CF_REQUIRE(pairs.items[i].room.name.present);
        char *name = pairs.items[i].room.name.value.ptr;
        CF_CHECK(name != NULL && strcmp(name, expected_names[i]) == 0);
    }
    /* The pair carries the decoded room and the joined membership's own
     * columns. */
    CF_CHECK(pairs.items[0].membership.id == MW_M_DAVID_WATERCOOLER);
    CF_CHECK(pairs.items[0].room.id == MW_R_WATERCOOLER);
    CF_CHECK(pairs.items[0].room.room_type == CF_ROOM_CLOSED);
    CF_CHECK(pairs.items[0].room.creator_id == MW_U_DAVID);
    CF_CHECK(pairs.items[0].room.created_at == MW_T1_US);
    CF_CHECK(pairs.items[0].room.updated_at == MW_T2_US);
    cf_membership_room_pair_vector_dispose(&pairs);

    cf_membership_room_pair_vector unknown = {0};
    CF_CHECK(cf_membership_visible_with_ordered_room(db, 424242, &unknown) ==
             CF_OK);
    CF_CHECK(unknown.len == 0 && unknown.items == NULL);

    CF_CHECK(cf_membership_visible_with_ordered_room(db, MW_U_DAVID, NULL) ==
             CF_INVALID);
    CF_CHECK(cf_membership_visible_with_ordered_room(NULL, MW_U_DAVID,
                                                     &unknown) == CF_INVALID);
    cf_db_close(db);
    mw_world_dispose(&world);
}

CF_TEST(with_ordered_room_includes_invisible_memberships) {
    mw_world world;
    mw_world_open(&world);
    cf_db *db = mw_rw(&world);
    mw_seed_ordered_rooms(db);

    cf_membership_room_pair_vector pairs = {0};
    CF_CHECK(cf_membership_with_ordered_room(db, MW_U_DAVID, &pairs) == CF_OK);
    CF_REQUIRE(pairs.len == 5);
    bool saw_hq = false;
    static const char *expected_names[] = {"All Talk", "alpha room",
                                           "Designers", "HQ", "Zulu room"};
    for (size_t i = 0; i < pairs.len; i++) {
        char *name = pairs.items[i].room.name.value.ptr;
        CF_CHECK(name != NULL && strcmp(name, expected_names[i]) == 0);
        if (pairs.items[i].room.id == MW_R_HQ) {
            saw_hq = true;
            CF_CHECK(pairs.items[i].membership.involvement.present &&
                     pairs.items[i].membership.involvement.value ==
                         CF_INVOLVEMENT_INVISIBLE);
        }
    }
    CF_CHECK(saw_hq);
    cf_membership_room_pair_vector_dispose(&pairs);

    CF_CHECK(cf_membership_with_ordered_room(db, MW_U_DAVID, NULL) ==
             CF_INVALID);
    CF_CHECK(cf_membership_with_ordered_room(NULL, MW_U_DAVID, NULL) ==
             CF_INVALID);
    cf_db_close(db);
    mw_world_dispose(&world);
}

CF_TEST(count_without_direct_rooms_skips_direct_rooms) {
    mw_world world;
    mw_world_open(&world);
    cf_db *db = mw_rw(&world);
    mw_seed_user(db, MW_U_DAVID, "David", 1);
    mw_seed_user(db, MW_U_JASON, "Jason", 1);
    mw_seed_room(db, MW_R_WATERCOOLER, "All Talk", "Rooms::Closed",
                 MW_U_DAVID);
    mw_seed_room(db, MW_R_DESIGNERS, "Designers", "Rooms::Closed",
                 MW_U_DAVID);
    mw_seed_room(db, MW_R_DAVID_JASON, NULL, "Rooms::Direct", MW_U_DAVID);
    mw_seed_membership(db, MW_M_DAVID_WATERCOOLER, MW_R_WATERCOOLER,
                       MW_U_DAVID, MW_SQL_INV_EVERYTHING, MW_SQL_NULL,
                       MW_SQL_NULL, 0);
    mw_seed_membership(db, MW_M_DAVID_DESIGNERS, MW_R_DESIGNERS, MW_U_DAVID,
                       NULL, MW_SQL_NULL, MW_SQL_NULL, 0);
    mw_seed_membership(db, MW_M_DAVID_DIRECT, MW_R_DAVID_JASON, MW_U_DAVID,
                       MW_SQL_INV_EVERYTHING, MW_SQL_NULL, MW_SQL_NULL, 0);
    mw_seed_membership(db, MW_M_JASON_WATERCOOLER, MW_R_WATERCOOLER,
                       MW_U_JASON, MW_SQL_INV_EVERYTHING, MW_SQL_NULL,
                       MW_SQL_NULL, 0);
    mw_seed_membership(db, MW_M_JASON_DIRECT, MW_R_DAVID_JASON, MW_U_JASON,
                       MW_SQL_INV_EVERYTHING, MW_SQL_NULL, MW_SQL_NULL, 0);

    int64_t count = -1;
    CF_CHECK(cf_membership_count_without_direct_rooms(db, MW_U_DAVID,
                                                      &count) == CF_OK);
    CF_CHECK(count == 2); /* watercooler + designers; the direct room is out */
    CF_CHECK(cf_membership_count_without_direct_rooms(db, MW_U_JASON,
                                                      &count) == CF_OK);
    CF_CHECK(count == 1);
    CF_CHECK(cf_membership_count_without_direct_rooms(db, 424242, &count) ==
             CF_OK);
    CF_CHECK(count == 0);
    CF_CHECK(cf_membership_count_without_direct_rooms(db, MW_U_DAVID, NULL) ==
             CF_INVALID);
    CF_CHECK(cf_membership_count_without_direct_rooms(NULL, MW_U_DAVID,
                                                      &count) == CF_INVALID);
    cf_db_close(db);
    mw_world_dispose(&world);
}

CF_TEST(unread_count_counts_only_present_unread_at) {
    mw_world world;
    mw_world_open(&world);
    cf_db *db = mw_rw(&world);
    mw_seed_user(db, MW_U_DAVID, "David", 1);
    mw_seed_room(db, MW_R_WATERCOOLER, "All Talk", "Rooms::Closed",
                 MW_U_DAVID);
    mw_seed_room(db, MW_R_DESIGNERS, "Designers", "Rooms::Closed",
                 MW_U_DAVID);
    mw_seed_room(db, MW_R_HQ, "HQ", "Rooms::Open", MW_U_DAVID);
    mw_seed_membership(db, MW_M_DAVID_WATERCOOLER, MW_R_WATERCOOLER,
                       MW_U_DAVID, MW_SQL_INV_EVERYTHING, "'" MW_T1_TEXT "'",
                       MW_SQL_NULL, 0);
    mw_seed_membership(db, MW_M_DAVID_DESIGNERS, MW_R_DESIGNERS, MW_U_DAVID,
                       NULL, MW_SQL_NULL, MW_SQL_NULL, 0);
    mw_seed_membership(db, 900010, MW_R_HQ, MW_U_DAVID, NULL,
                       "'" MW_T2_TEXT "'", MW_SQL_NULL, 0);

    int64_t count = -1;
    CF_CHECK(cf_membership_unread_count(db, MW_U_DAVID, &count) == CF_OK);
    CF_CHECK(count == 2);
    CF_CHECK(cf_membership_unread_count(db, 424242, &count) == CF_OK);
    CF_CHECK(count == 0);
    CF_CHECK(cf_membership_unread_count(db, MW_U_DAVID, NULL) == CF_INVALID);
    CF_CHECK(cf_membership_unread_count(NULL, MW_U_DAVID, &count) ==
             CF_INVALID);
    cf_db_close(db);
    mw_world_dispose(&world);
}

CF_TEST(connection_cutoff_and_scope_helpers_follow_the_60s_ttl) {
    mw_world world;
    mw_world_open(&world);
    cf_db *db = mw_rw(&world);
    mw_seed_user(db, MW_U_DAVID, "David", 1);
    mw_seed_user(db, MW_U_JASON, "Jason", 1);
    mw_seed_user(db, MW_U_JZ, "JZ", 0);
    mw_seed_user(db, MW_U_KEVIN, "Kevin", 0);
    mw_seed_room(db, MW_R_WATERCOOLER, "All Talk", "Rooms::Closed",
                 MW_U_DAVID);
    /* disconnected, fresh (T2), stale (T2-61 s), exactly at the cutoff. */
    mw_seed_membership(db, MW_M_DAVID_WATERCOOLER, MW_R_WATERCOOLER,
                       MW_U_DAVID, MW_SQL_INV_EVERYTHING, MW_SQL_NULL,
                       MW_SQL_NULL, 0);
    mw_seed_membership(db, 900020, MW_R_WATERCOOLER, MW_U_JASON,
                       MW_SQL_INV_EVERYTHING, MW_SQL_NULL, "'" MW_T2_TEXT "'",
                       1);
    mw_seed_membership(db, 900021, MW_R_WATERCOOLER, MW_U_JZ,
                       MW_SQL_INV_EVERYTHING, MW_SQL_NULL,
                       "'" MW_STALE_TEXT "'", 2);
    mw_seed_membership(db, 900022, MW_R_WATERCOOLER, MW_U_KEVIN,
                       MW_SQL_INV_EVERYTHING, MW_SQL_NULL,
                       "'" MW_CUTOFF_TEXT "'", 1);

    CF_CHECK(cf_membership_connection_cutoff(MW_T2_US) == MW_CUTOFF_US);

    /* is_connected: NULL and older-than-cutoff are disconnected; exactly at
     * the cutoff is still connected (>= in the source). */
    cf_membership disconnected = {.connected_at = {false, 0}};
    CF_CHECK(!cf_membership_is_connected(&disconnected, MW_T2_US));
    CF_CHECK(!cf_membership_is_connected(NULL, MW_T2_US));
    cf_membership fresh = {.connected_at = {true, MW_T2_US}};
    CF_CHECK(cf_membership_is_connected(&fresh, MW_T2_US));
    /* Connected while connected_at >= now - 60 s: exactly +60 s still
     * counts, one microsecond later does not. */
    CF_CHECK(cf_membership_is_connected(&fresh,
                                        MW_T2_US + INT64_C(60000000)));
    CF_CHECK(!cf_membership_is_connected(&fresh,
                                         MW_T2_US + INT64_C(60000001)));
    cf_membership boundary = {.connected_at = {true, MW_CUTOFF_US}};
    CF_CHECK(cf_membership_is_connected(&boundary, MW_T2_US));

    bool out = true;
    CF_CHECK(cf_membership_connected_exists(db, MW_M_DAVID_WATERCOOLER,
                                            MW_T2_US, &out) == CF_OK);
    CF_CHECK(!out); /* NULL connected_at */
    CF_CHECK(cf_membership_connected_exists(db, 900020, MW_T2_US, &out) ==
             CF_OK);
    CF_CHECK(out);
    CF_CHECK(cf_membership_connected_exists(db, 900020, MW_T3_US, &out) ==
             CF_OK);
    CF_CHECK(!out); /* T2 < T3 - 60 s */
    CF_CHECK(cf_membership_connected_exists(db, 900021, MW_T2_US, &out) ==
             CF_OK);
    CF_CHECK(!out); /* T2-61 s < cutoff */
    CF_CHECK(cf_membership_connected_exists(db, 900022, MW_T2_US, &out) ==
             CF_OK);
    CF_CHECK(out); /* equal to the cutoff still counts */
    CF_CHECK(cf_membership_connected_exists(db, 999999, MW_T2_US, &out) ==
             CF_OK);
    CF_CHECK(!out);

    CF_CHECK(cf_membership_disconnected_exists(db, MW_M_DAVID_WATERCOOLER,
                                               MW_T2_US, &out) == CF_OK);
    CF_CHECK(out);
    CF_CHECK(cf_membership_disconnected_exists(db, 900020, MW_T2_US, &out) ==
             CF_OK);
    CF_CHECK(!out);
    CF_CHECK(cf_membership_disconnected_exists(db, 900020, MW_T3_US, &out) ==
             CF_OK);
    CF_CHECK(out); /* stale at T3 */
    CF_CHECK(cf_membership_disconnected_exists(db, 900022, MW_T2_US, &out) ==
             CF_OK);
    CF_CHECK(!out); /* equal-to-cutoff is connected, not disconnected */
    CF_CHECK(cf_membership_disconnected_exists(db, 999999, MW_T2_US, &out) ==
             CF_OK);
    CF_CHECK(!out); /* absent row matches neither scope */

    CF_CHECK(cf_membership_connected_exists(db, 1, MW_T2_US, NULL) ==
             CF_INVALID);
    CF_CHECK(cf_membership_disconnected_exists(db, 1, MW_T2_US, NULL) ==
             CF_INVALID);
    CF_CHECK(cf_membership_connected_exists(NULL, 1, MW_T2_US, &out) ==
             CF_INVALID);
    CF_CHECK(cf_membership_disconnected_exists(NULL, 1, MW_T2_US, &out) ==
             CF_INVALID);
    cf_db_close(db);
    mw_world_dispose(&world);
}

CF_TEST(room_and_user_delegate_to_their_models) {
    mw_world world;
    mw_world_open(&world);
    cf_db *db = mw_rw(&world);
    mw_seed_core_fixture(db);

    cf_membership m;
    memset(&m, 0, sizeof m);
    CF_REQUIRE(cf_membership_find(db, MW_M_DAVID_WATERCOOLER, &m) == CF_OK);

    cf_room room;
    memset(&room, 0, sizeof room);
    CF_CHECK(cf_membership_room(db, &m, &room) == CF_OK);
    CF_CHECK(room.id == MW_R_WATERCOOLER);
    CF_CHECK(room.room_type == CF_ROOM_CLOSED);
    CF_CHECK(room.creator_id == MW_U_DAVID);
    CF_CHECK(room.name.present && room.name.value.ptr != NULL &&
             strcmp(room.name.value.ptr, "All Talk") == 0);
    cf_room_dispose(&room);

    cf_user user;
    memset(&user, 0, sizeof user);
    CF_CHECK(cf_membership_user(db, &m, &user) == CF_OK);
    CF_CHECK(user.id == MW_U_DAVID);
    CF_CHECK(user.role == CF_ROLE_ADMINISTRATOR);
    CF_CHECK(user.name.ptr != NULL && strcmp(user.name.ptr, "David") == 0);
    cf_user_dispose(&user);

    /* A reference to a missing row is CF_NOT_FOUND. */
    cf_membership orphan = m;
    orphan.room_id = 424242;
    memset(&room, 0, sizeof room);
    CF_CHECK(cf_membership_room(db, &orphan, &room) == CF_NOT_FOUND);
    CF_CHECK(room.id == 0);
    orphan.user_id = 424242;
    memset(&user, 0, sizeof user);
    CF_CHECK(cf_membership_user(db, &orphan, &user) == CF_NOT_FOUND);
    CF_CHECK(user.id == 0);

    CF_CHECK(cf_membership_room(db, NULL, &room) == CF_INVALID);
    CF_CHECK(cf_membership_room(db, &m, NULL) == CF_INVALID);
    CF_CHECK(cf_membership_user(db, NULL, &user) == CF_INVALID);
    CF_CHECK(cf_membership_user(db, &m, NULL) == CF_INVALID);

    cf_membership_dispose(&m);
    cf_db_close(db);
    mw_world_dispose(&world);
}

CF_TEST(involved_in_and_unread_predicates) {
    cf_membership everything = {
        .involvement = {true, CF_INVOLVEMENT_EVERYTHING},
        .unread_at = {true, MW_T1_US},
    };
    CF_CHECK(cf_membership_involved_in(&everything,
                                       CF_INVOLVEMENT_EVERYTHING));
    CF_CHECK(!cf_membership_involved_in(&everything, CF_INVOLVEMENT_MENTIONS));
    CF_CHECK(cf_membership_unread(&everything));
    CF_CHECK(!cf_membership_involved_in(NULL, CF_INVOLVEMENT_EVERYTHING));
    CF_CHECK(!cf_membership_unread(NULL));

    cf_membership null_involvement = {0};
    CF_CHECK(!cf_membership_involved_in(&null_involvement,
                                        CF_INVOLVEMENT_EVERYTHING));
    CF_CHECK(!cf_membership_unread(&null_involvement));
}

/* --- mutations (cf_tx via D02 cf_write) ----------------------------------- */

/* update_involvement: NULL -> NULL is a no-op, a change writes the stored
 * name and touches updated_at, an equal value does not touch it. */
typedef struct {
    cf_err rc_null_noop;
    cf_err rc_set;
    cf_err rc_repeat;
    cf_err rc_null_write;
    cf_err rc_repeat_null;
    cf_err rc_invalid;
    cf_membership after_null_noop;
    cf_membership after_set;
    cf_membership after_repeat;
    cf_membership after_null_write;
    cf_membership after_repeat_null;
    cf_membership after_invalid;
    bool invalid_unchanged;
} mw_involvement_arg;

static cf_err mw_write_involvement(cf_tx *tx, void *argp) {
    mw_involvement_arg *a = argp;
    cf_membership m;
    memset(&m, 0, sizeof m);
    cf_err rc =
        cf_membership_find(cf_tx_db(tx), MW_M_DAVID_WATERCOOLER, &m);
    if (rc != CF_OK) return rc;

    /* NULL -> NULL: equal, so no UPDATE and no touch. */
    cf_test_clock_set_fixed_us(MW_T2_US);
    a->rc_null_noop = cf_membership_update_involvement(
        tx, &m, (cf_optional_involvement){false, CF_INVOLVEMENT_MENTIONS});
    a->after_null_noop = m;

    /* NULL -> everything: writes the stored name text and touches. */
    a->rc_set = cf_membership_update_involvement(
        tx, &m, (cf_optional_involvement){true, CF_INVOLVEMENT_EVERYTHING});
    a->after_set = m;

    /* everything -> everything at a later clock: no touch. */
    cf_test_clock_set_fixed_us(MW_T3_US);
    a->rc_repeat = cf_membership_update_involvement(
        tx, &m, (cf_optional_involvement){true, CF_INVOLVEMENT_EVERYTHING});
    a->after_repeat = m;

    /* everything -> NULL writes NULL and touches. */
    a->rc_null_write = cf_membership_update_involvement(
        tx, &m, (cf_optional_involvement){false, CF_INVOLVEMENT_MENTIONS});
    a->after_null_write = m;

    /* A repeated NULL stays untouched. */
    a->rc_repeat_null = cf_membership_update_involvement(
        tx, &m, (cf_optional_involvement){false, CF_INVOLVEMENT_MENTIONS});
    a->after_repeat_null = m;

    /* An out-of-range enum fails the bind before SQL and leaves the record. */
    cf_membership before = m;
    a->rc_invalid = cf_membership_update_involvement(
        tx, &m, (cf_optional_involvement){true, (cf_involvement)99});
    a->after_invalid = m;
    a->invalid_unchanged =
        m.involvement.present == before.involvement.present &&
        m.involvement.value == before.involvement.value &&
        m.updated_at == before.updated_at;
    return CF_OK; /* commit the successful writes; sub-results are asserted */
}

CF_TEST(update_involvement_writes_and_touches_only_on_change) {
    mw_world world;
    mw_world_open(&world);
    cf_db *db = mw_rw(&world);
    mw_seed_user(db, MW_U_DAVID, "David", 1);
    mw_seed_room(db, MW_R_WATERCOOLER, "All Talk", "Rooms::Closed",
                 MW_U_DAVID);
    mw_seed_membership(db, MW_M_DAVID_WATERCOOLER, MW_R_WATERCOOLER,
                       MW_U_DAVID, MW_SQL_NULL, MW_SQL_NULL, MW_SQL_NULL, 0);
    char sql[160];
    snprintf(sql, sizeof sql,
             "UPDATE \"memberships\" SET \"updated_at\" = '" MW_T1_TEXT
             "' WHERE \"id\" = %lld",
             (long long)MW_M_DAVID_WATERCOOLER);
    mw_exec(db, sql);
    cf_db_close(db);

    mw_involvement_arg arg;
    memset(&arg, 0, sizeof arg);
    CF_CHECK(cf_write(world.app, mw_write_involvement, &arg) == CF_OK);
    cf_test_clock_clear();

    CF_CHECK(arg.rc_null_noop == CF_OK);
    CF_CHECK(!arg.after_null_noop.involvement.present);
    CF_CHECK(arg.after_null_noop.updated_at == MW_T1_US); /* no touch */
    CF_CHECK(arg.rc_set == CF_OK);
    CF_CHECK(arg.after_set.involvement.present &&
             arg.after_set.involvement.value == CF_INVOLVEMENT_EVERYTHING);
    CF_CHECK(arg.after_set.updated_at == MW_T2_US);
    CF_CHECK(arg.rc_repeat == CF_OK);
    CF_CHECK(arg.after_repeat.updated_at == MW_T2_US); /* equal: no touch */
    CF_CHECK(arg.rc_null_write == CF_OK);
    CF_CHECK(!arg.after_null_write.involvement.present);
    CF_CHECK(arg.after_null_write.updated_at == MW_T3_US);
    CF_CHECK(arg.rc_repeat_null == CF_OK);
    CF_CHECK(arg.after_repeat_null.updated_at == MW_T3_US);
    CF_CHECK(arg.rc_invalid == CF_INVALID);
    CF_CHECK(arg.invalid_unchanged);

    /* Committed state: NULL involvement, touched once at T3. */
    cf_db *reader = mw_reader(&world);
    CF_CHECK(mw_i64(reader,
                    "SELECT \"involvement\" IS NULL FROM \"memberships\" "
                    "WHERE \"id\" = 774341722") == 1);
    mw_expect_text(reader,
                   "SELECT \"updated_at\" FROM \"memberships\" "
                   "WHERE \"id\" = 774341722",
                   MW_T3_TEXT);
    cf_db_close(reader);

    /* Null argument paths. */
    cf_membership m = {0};
    CF_CHECK(cf_membership_update_involvement(NULL, &m,
                                              (cf_optional_involvement){false,
                                                                        0}) ==
             CF_INVALID);
    CF_CHECK(cf_membership_update_involvement(NULL, NULL,
                                              (cf_optional_involvement){false,
                                                                        0}) ==
             CF_INVALID);
    mw_world_dispose(&world);
}

/* read clears unread_at (touching updated_at) and is a no-op with no unread
 * mark, even when called later. */
typedef struct {
    cf_err rc_clear;
    cf_err rc_repeat;
    cf_err rc_already_read;
    cf_membership after_clear;
    cf_membership after_repeat;
    cf_membership after_already_read;
} mw_read_arg;

static cf_err mw_write_read(cf_tx *tx, void *argp) {
    mw_read_arg *a = argp;
    cf_membership m;
    memset(&m, 0, sizeof m);
    cf_err rc =
        cf_membership_find(cf_tx_db(tx), MW_M_DAVID_WATERCOOLER, &m);
    if (rc != CF_OK) return rc;

    cf_test_clock_set_fixed_us(MW_T2_US);
    a->rc_clear = cf_membership_read(tx, &m);
    a->after_clear = m;

    cf_test_clock_set_fixed_us(MW_T3_US);
    a->rc_repeat = cf_membership_read(tx, &m);
    a->after_repeat = m;

    /* A membership whose unread_at is already NULL: read must not touch. */
    cf_membership other;
    memset(&other, 0, sizeof other);
    rc = cf_membership_find(cf_tx_db(tx), 900030, &other);
    if (rc != CF_OK) return rc;
    a->rc_already_read = cf_membership_read(tx, &other);
    a->after_already_read = other;
    return CF_OK;
}

CF_TEST(read_clears_unread_and_is_a_noop_when_already_read) {
    mw_world world;
    mw_world_open(&world);
    cf_db *db = mw_rw(&world);
    mw_seed_user(db, MW_U_DAVID, "David", 1);
    mw_seed_room(db, MW_R_WATERCOOLER, "All Talk", "Rooms::Closed",
                 MW_U_DAVID);
    mw_seed_room(db, MW_R_DESIGNERS, "Designers", "Rooms::Closed",
                 MW_U_DAVID);
    mw_seed_membership(db, MW_M_DAVID_WATERCOOLER, MW_R_WATERCOOLER,
                       MW_U_DAVID, MW_SQL_INV_EVERYTHING, "'" MW_T1_TEXT "'",
                       MW_SQL_NULL, 0);
    mw_seed_membership(db, 900030, MW_R_DESIGNERS, MW_U_DAVID, NULL,
                       MW_SQL_NULL, MW_SQL_NULL, 0);
    mw_exec(db, "UPDATE \"memberships\" SET \"updated_at\" = '" MW_T1_TEXT
                "'");
    cf_db_close(db);

    mw_read_arg arg;
    memset(&arg, 0, sizeof arg);
    CF_CHECK(cf_write(world.app, mw_write_read, &arg) == CF_OK);
    cf_test_clock_clear();

    CF_CHECK(arg.rc_clear == CF_OK);
    CF_CHECK(!arg.after_clear.unread_at.present);
    CF_CHECK(arg.after_clear.updated_at == MW_T2_US);
    CF_CHECK(arg.rc_repeat == CF_OK);
    CF_CHECK(arg.after_repeat.updated_at == MW_T2_US); /* no second touch */
    CF_CHECK(arg.rc_already_read == CF_OK);
    CF_CHECK(arg.after_already_read.updated_at == MW_T1_US); /* untouched */

    cf_db *reader = mw_reader(&world);
    CF_CHECK(mw_i64(reader,
                    "SELECT \"unread_at\" IS NULL FROM \"memberships\" "
                    "WHERE \"id\" = 774341722") == 1);
    mw_expect_text(reader,
                   "SELECT \"updated_at\" FROM \"memberships\" "
                   "WHERE \"id\" = 774341722",
                   MW_T2_TEXT);
    mw_expect_text(reader,
                   "SELECT \"updated_at\" FROM \"memberships\" "
                   "WHERE \"id\" = 900030",
                   MW_T1_TEXT);
    cf_db_close(reader);

    cf_membership m = {0};
    CF_CHECK(cf_membership_read(NULL, &m) == CF_INVALID);
    CF_CHECK(cf_membership_read(NULL, NULL) == CF_INVALID);
    mw_world_dispose(&world);
}

/* destroy deletes the row and queues one DISCONNECT_USER event with
 * reconnect=true per removed membership (D02's mandatory control event). */
typedef struct {
    size_t count;
    cf_event events[4];
} mw_event_log;

static cf_err mw_record_control(void *ctx, const cf_event *event) {
    mw_event_log *log = ctx;
    if (log->count < sizeof log->events / sizeof log->events[0]) {
        log->events[log->count] = *event;
    }
    log->count++;
    return CF_OK;
}

typedef struct {
    cf_err rc;
} mw_destroy_arg;

static cf_err mw_write_destroy(cf_tx *tx, void *argp) {
    mw_destroy_arg *a = argp;
    static const int64_t ids[] = {MW_M_DAVID_WATERCOOLER,
                                  MW_M_JASON_WATERCOOLER};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        cf_membership m;
        memset(&m, 0, sizeof m);
        a->rc = cf_membership_find(cf_tx_db(tx), ids[i], &m);
        if (a->rc != CF_OK) return a->rc;
        a->rc = cf_membership_destroy(tx, &m);
        cf_membership_dispose(&m);
        if (a->rc != CF_OK) return a->rc;
    }
    return CF_OK;
}

CF_TEST(destroy_deletes_and_emits_one_reconnect_event_per_membership) {
    mw_world world;
    mw_world_open(&world);
    cf_db *db = mw_rw(&world);
    mw_seed_core_fixture(db);
    cf_db_close(db);

    mw_event_log log;
    memset(&log, 0, sizeof log);
    CF_REQUIRE(cf_writer_set_control_handler(world.app, mw_record_control,
                                             &log) == CF_OK);

    mw_destroy_arg arg = {0};
    CF_CHECK(cf_write(world.app, mw_write_destroy, &arg) == CF_OK);
    cf_test_clock_clear();
    CF_CHECK(arg.rc == CF_OK);

    CF_REQUIRE(log.count == 2);
    CF_CHECK(log.events[0].kind == CF_EVENT_DISCONNECT_USER);
    CF_CHECK(log.events[0].user_id == MW_U_DAVID);
    CF_CHECK(log.events[0].reconnect);
    CF_CHECK(log.events[0].room_id == 0 && log.events[0].message_id == 0 &&
             log.events[0].blob_id == 0);
    CF_CHECK(log.events[1].kind == CF_EVENT_DISCONNECT_USER);
    CF_CHECK(log.events[1].user_id == MW_U_JASON);
    CF_CHECK(log.events[1].reconnect);

    cf_db *reader = mw_reader(&world);
    CF_CHECK(mw_i64(reader, "SELECT count(*) FROM \"memberships\"") == 3);
    CF_CHECK(mw_i64(reader, "SELECT count(*) FROM \"memberships\" "
                            "WHERE \"id\" IN (774341722, 505016279)") == 0);
    cf_db_close(reader);

    cf_membership m = {0};
    CF_CHECK(cf_membership_destroy(NULL, &m) == CF_INVALID);
    CF_CHECK(cf_membership_destroy(NULL, NULL) == CF_INVALID);
    mw_world_dispose(&world);
}

/* disconnect_all resets every membership whose connected_at is inside the TTL
 * (the source has no user scope). */
typedef struct {
    cf_err rc;
    size_t rows;
} mw_disconnect_all_arg;

static cf_err mw_write_disconnect_all(cf_tx *tx, void *argp) {
    mw_disconnect_all_arg *a = argp;
    cf_test_clock_set_fixed_us(MW_T2_US);
    a->rc = cf_membership_disconnect_all(tx, &a->rows);
    return a->rc;
}

CF_TEST(disconnect_all_resets_only_fresh_connected_memberships) {
    mw_world world;
    mw_world_open(&world);
    cf_db *db = mw_rw(&world);
    mw_seed_user(db, MW_U_DAVID, "David", 1);
    mw_seed_user(db, MW_U_JASON, "Jason", 1);
    mw_seed_user(db, MW_U_JZ, "JZ", 0);
    mw_seed_user(db, MW_U_KEVIN, "Kevin", 0);
    mw_seed_room(db, MW_R_WATERCOOLER, "All Talk", "Rooms::Closed",
                 MW_U_DAVID);
    mw_seed_membership(db, MW_M_DAVID_WATERCOOLER, MW_R_WATERCOOLER,
                       MW_U_DAVID, MW_SQL_INV_EVERYTHING, MW_SQL_NULL,
                       "'" MW_T2_TEXT "'", 2);
    mw_seed_membership(db, 900022, MW_R_WATERCOOLER, MW_U_KEVIN,
                       MW_SQL_INV_EVERYTHING, MW_SQL_NULL,
                       "'" MW_CUTOFF_TEXT "'", 1);
    mw_seed_membership(db, 900021, MW_R_WATERCOOLER, MW_U_JZ,
                       MW_SQL_INV_EVERYTHING, MW_SQL_NULL,
                       "'" MW_STALE_TEXT "'", 3);
    mw_seed_membership(db, MW_M_JASON_WATERCOOLER, MW_R_WATERCOOLER,
                       MW_U_JASON, MW_SQL_INV_EVERYTHING, MW_SQL_NULL,
                       MW_SQL_NULL, 0);
    /* Mark the untouchable row so "unchanged" is distinguishable from seed. */
    mw_exec(db, "UPDATE \"memberships\" SET \"updated_at\" = '" MW_T1_TEXT
                "' WHERE \"id\" = 505016279");
    cf_db_close(db);

    mw_disconnect_all_arg arg = {0};
    CF_CHECK(cf_write(world.app, mw_write_disconnect_all, &arg) == CF_OK);
    cf_test_clock_clear();
    CF_CHECK(arg.rc == CF_OK);
    CF_CHECK(arg.rows == 2); /* fresh T2 and exactly-at-cutoff */

    cf_db *reader = mw_reader(&world);
    /* Reset rows: connected_at NULL, connections 0, touched at T2. */
    CF_CHECK(mw_i64(reader,
                    "SELECT \"connected_at\" IS NULL AND \"connections\" = 0 "
                    "FROM \"memberships\" WHERE \"id\" = 774341722") == 1);
    mw_expect_text(reader,
                   "SELECT \"updated_at\" FROM \"memberships\" "
                   "WHERE \"id\" = 774341722",
                   MW_T2_TEXT);
    CF_CHECK(mw_i64(reader,
                    "SELECT \"connected_at\" IS NULL AND \"connections\" = 0 "
                    "FROM \"memberships\" WHERE \"id\" = 900022") == 1);
    mw_expect_text(reader,
                   "SELECT \"updated_at\" FROM \"memberships\" "
                   "WHERE \"id\" = 900022",
                   MW_T2_TEXT);
    /* Stale row untouched. */
    mw_expect_text(reader,
                   "SELECT \"connected_at\" FROM \"memberships\" "
                   "WHERE \"id\" = 900021",
                   MW_STALE_TEXT);
    CF_CHECK(mw_i64(reader,
                    "SELECT \"connections\" FROM \"memberships\" "
                    "WHERE \"id\" = 900021") == 3);
    /* Already-disconnected row untouched, not touched at all. */
    CF_CHECK(mw_i64(reader,
                    "SELECT \"connected_at\" IS NULL AND \"connections\" = 0 "
                    "FROM \"memberships\" WHERE \"id\" = 505016279") == 1);
    mw_expect_text(reader,
                   "SELECT \"updated_at\" FROM \"memberships\" "
                   "WHERE \"id\" = 505016279",
                   MW_T1_TEXT);
    cf_db_close(reader);

    size_t rows = 99;
    CF_CHECK(cf_membership_disconnect_all(NULL, &rows) == CF_INVALID);
    mw_world_dispose(&world);
}

/* connect sets connections/connected_at and clears unread_at but never
 * touches updated_at, and is a no-op on a missing id. */
typedef struct {
    cf_err rc_set;
    cf_err rc_absent;
} mw_connect_arg;

static cf_err mw_write_connect(cf_tx *tx, void *argp) {
    mw_connect_arg *a = argp;
    cf_test_clock_set_fixed_us(MW_T2_US);
    a->rc_set = cf_membership_connect(tx, MW_M_DAVID_WATERCOOLER, 3);
    a->rc_absent = cf_membership_connect(tx, 424242, 1);
    return a->rc_set == CF_OK ? CF_OK : a->rc_set;
}

CF_TEST(connect_sets_state_without_touching_updated_at) {
    mw_world world;
    mw_world_open(&world);
    cf_db *db = mw_rw(&world);
    mw_seed_user(db, MW_U_DAVID, "David", 1);
    mw_seed_room(db, MW_R_WATERCOOLER, "All Talk", "Rooms::Closed",
                 MW_U_DAVID);
    mw_seed_membership(db, MW_M_DAVID_WATERCOOLER, MW_R_WATERCOOLER,
                       MW_U_DAVID, MW_SQL_INV_EVERYTHING, "'" MW_T1_TEXT "'",
                       MW_SQL_NULL, 0);
    mw_exec(db, "UPDATE \"memberships\" SET \"updated_at\" = '" MW_T1_TEXT
                "' WHERE \"id\" = 774341722");
    cf_db_close(db);

    mw_connect_arg arg = {0};
    CF_CHECK(cf_write(world.app, mw_write_connect, &arg) == CF_OK);
    cf_test_clock_clear();
    CF_CHECK(arg.rc_set == CF_OK);
    CF_CHECK(arg.rc_absent == CF_OK); /* UPDATE matched no row: still Ok */

    cf_db *reader = mw_reader(&world);
    CF_CHECK(mw_i64(reader,
                    "SELECT \"connections\" FROM \"memberships\" "
                    "WHERE \"id\" = 774341722") == 3);
    mw_expect_text(reader,
                   "SELECT \"connected_at\" FROM \"memberships\" "
                   "WHERE \"id\" = 774341722",
                   MW_T2_TEXT);
    CF_CHECK(mw_i64(reader,
                    "SELECT \"unread_at\" IS NULL FROM \"memberships\" "
                    "WHERE \"id\" = 774341722") == 1);
    mw_expect_text(reader,
                   "SELECT \"updated_at\" FROM \"memberships\" "
                   "WHERE \"id\" = 774341722",
                   MW_T1_TEXT); /* connect deliberately does not touch it */
    cf_db_close(reader);

    CF_CHECK(cf_membership_connect(NULL, 1, 1) == CF_INVALID);
    mw_world_dispose(&world);
}

/* present: connect with connections = previous + 1 when still connected,
 * otherwise 1; it never mutates the in-memory record. */
typedef struct {
    cf_err rc_one;
    cf_err rc_two;
    cf_membership before_one;
    cf_membership after_one;
    cf_membership before_two;
    cf_membership after_two;
} mw_present_arg;

static cf_err mw_write_present(cf_tx *tx, void *argp) {
    mw_present_arg *a = argp;
    cf_membership m;
    memset(&m, 0, sizeof m);
    cf_err rc =
        cf_membership_find(cf_tx_db(tx), MW_M_DAVID_WATERCOOLER, &m);
    if (rc != CF_OK) return rc;
    a->before_one = m;
    cf_test_clock_set_fixed_us(MW_T2_US);
    a->rc_one = cf_membership_present(tx, &m);
    a->after_one = m;
    if (a->rc_one != CF_OK) return a->rc_one;

    cf_membership other;
    memset(&other, 0, sizeof other);
    rc = cf_membership_find(cf_tx_db(tx), 900030, &other);
    if (rc != CF_OK) return rc;
    a->before_two = other;
    a->rc_two = cf_membership_present(tx, &other);
    a->after_two = other;
    return a->rc_two == CF_OK ? CF_OK : a->rc_two;
}

CF_TEST(present_marks_read_and_counts_connections) {
    mw_world world;
    mw_world_open(&world);
    cf_db *db = mw_rw(&world);
    mw_seed_user(db, MW_U_DAVID, "David", 1);
    mw_seed_room(db, MW_R_WATERCOOLER, "All Talk", "Rooms::Closed",
                 MW_U_DAVID);
    mw_seed_room(db, MW_R_DESIGNERS, "Designers", "Rooms::Closed",
                 MW_U_DAVID);
    /* Disconnected with an unread mark. */
    mw_seed_membership(db, MW_M_DAVID_WATERCOOLER, MW_R_WATERCOOLER,
                       MW_U_DAVID, MW_SQL_INV_EVERYTHING, "'" MW_T1_TEXT "'",
                       MW_SQL_NULL, 0);
    /* Connected inside the TTL with two connections. */
    mw_seed_membership(db, 900030, MW_R_DESIGNERS, MW_U_DAVID, NULL,
                       MW_SQL_NULL, "'" MW_T2_TEXT "'", 2);
    mw_exec(db, "UPDATE \"memberships\" SET \"updated_at\" = '" MW_T1_TEXT
                "'");
    cf_db_close(db);

    mw_present_arg arg;
    memset(&arg, 0, sizeof arg);
    CF_CHECK(cf_write(world.app, mw_write_present, &arg) == CF_OK);
    cf_test_clock_clear();

    CF_CHECK(arg.rc_one == CF_OK);
    CF_CHECK(arg.rc_two == CF_OK);
    /* present -> Self::connect only writes the row; the record is unchanged
     * (the reference test asserts updated_at is not touched in the DB). */
    CF_CHECK(arg.after_one.connections == arg.before_one.connections);
    CF_CHECK(arg.after_one.unread_at.present ==
             arg.before_one.unread_at.present);
    CF_CHECK(arg.after_one.updated_at == arg.before_one.updated_at);
    CF_CHECK(arg.after_two.connections == arg.before_two.connections);
    CF_CHECK(arg.after_two.updated_at == arg.before_two.updated_at);

    cf_db *reader = mw_reader(&world);
    /* Disconnected membership: connection count resets to 1, unread cleared,
     * updated_at untouched. */
    CF_CHECK(mw_i64(reader,
                    "SELECT \"connections\" FROM \"memberships\" "
                    "WHERE \"id\" = 774341722") == 1);
    CF_CHECK(mw_i64(reader,
                    "SELECT \"unread_at\" IS NULL FROM \"memberships\" "
                    "WHERE \"id\" = 774341722") == 1);
    mw_expect_text(reader,
                   "SELECT \"connected_at\" FROM \"memberships\" "
                   "WHERE \"id\" = 774341722",
                   MW_T2_TEXT);
    mw_expect_text(reader,
                   "SELECT \"updated_at\" FROM \"memberships\" "
                   "WHERE \"id\" = 774341722",
                   MW_T1_TEXT);
    /* Connected membership: 2 + 1, and connected_at refreshed. */
    CF_CHECK(mw_i64(reader,
                    "SELECT \"connections\" FROM \"memberships\" "
                    "WHERE \"id\" = 900030") == 3);
    mw_expect_text(reader,
                   "SELECT \"updated_at\" FROM \"memberships\" "
                   "WHERE \"id\" = 900030",
                   MW_T1_TEXT);
    cf_db_close(reader);

    CF_CHECK(cf_membership_present(NULL, NULL) == CF_INVALID);
    cf_membership m = {0};
    CF_CHECK(cf_membership_present(NULL, &m) == CF_INVALID);
    mw_world_dispose(&world);
}

/* connected: increments (or resets a stale count to 1) and touches
 * connected_at/updated_at. */
typedef struct {
    cf_err rc_one;
    cf_err rc_two;
    cf_err rc_three;
    cf_membership one;
    cf_membership two;
    cf_membership three;
} mw_connected_arg;

static cf_err mw_write_connected(cf_tx *tx, void *argp) {
    mw_connected_arg *a = argp;
    static const int64_t ids[] = {MW_M_DAVID_WATERCOOLER, 900030, 900031};
    cf_membership *outs[] = {&a->one, &a->two, &a->three};
    cf_err *rcs[] = {&a->rc_one, &a->rc_two, &a->rc_three};
    for (size_t i = 0; i < 3; i++) {
        cf_membership m;
        memset(&m, 0, sizeof m);
        cf_err rc = cf_membership_find(cf_tx_db(tx), ids[i], &m);
        if (rc != CF_OK) return rc;
        cf_test_clock_set_fixed_us(MW_T2_US);
        *rcs[i] = cf_membership_connected(tx, &m);
        *outs[i] = m;
        if (*rcs[i] != CF_OK) return *rcs[i];
    }
    return CF_OK;
}

CF_TEST(connected_increments_or_resets_and_touches) {
    mw_world world;
    mw_world_open(&world);
    cf_db *db = mw_rw(&world);
    mw_seed_user(db, MW_U_DAVID, "David", 1);
    mw_seed_room(db, MW_R_WATERCOOLER, "All Talk", "Rooms::Closed",
                 MW_U_DAVID);
    mw_seed_room(db, MW_R_DESIGNERS, "Designers", "Rooms::Closed",
                 MW_U_DAVID);
    mw_seed_room(db, MW_R_HQ, "HQ", "Rooms::Open", MW_U_DAVID);
    mw_seed_membership(db, MW_M_DAVID_WATERCOOLER, MW_R_WATERCOOLER,
                       MW_U_DAVID, MW_SQL_INV_EVERYTHING, MW_SQL_NULL,
                       MW_SQL_NULL, 0);
    mw_seed_membership(db, 900030, MW_R_DESIGNERS, MW_U_DAVID, NULL,
                       MW_SQL_NULL, "'" MW_T2_TEXT "'", 2);
    mw_seed_membership(db, 900031, MW_R_HQ, MW_U_DAVID, NULL,
                       MW_SQL_NULL, "'" MW_STALE_TEXT "'", 5);
    mw_exec(db, "UPDATE \"memberships\" SET \"updated_at\" = '" MW_T1_TEXT
                "'");
    cf_db_close(db);

    mw_connected_arg arg;
    memset(&arg, 0, sizeof arg);
    CF_CHECK(cf_write(world.app, mw_write_connected, &arg) == CF_OK);
    cf_test_clock_clear();
    CF_CHECK(arg.rc_one == CF_OK && arg.rc_two == CF_OK &&
             arg.rc_three == CF_OK);

    /* Disconnected -> 1. */
    CF_CHECK(arg.one.connections == 1 && arg.one.connected_at.present &&
             arg.one.connected_at.value == MW_T2_US &&
             arg.one.updated_at == MW_T2_US);
    /* Connected -> +1. */
    CF_CHECK(arg.two.connections == 3 && arg.two.updated_at == MW_T2_US);
    /* Stale -> the count is reset to 1, not incremented. */
    CF_CHECK(arg.three.connections == 1 &&
             arg.three.connected_at.value == MW_T2_US &&
             arg.three.updated_at == MW_T2_US);

    cf_db *reader = mw_reader(&world);
    CF_CHECK(mw_i64(reader,
                    "SELECT \"connections\" FROM \"memberships\" "
                    "WHERE \"id\" = 774341722") == 1);
    CF_CHECK(mw_i64(reader,
                    "SELECT \"connections\" FROM \"memberships\" "
                    "WHERE \"id\" = 900030") == 3);
    CF_CHECK(mw_i64(reader,
                    "SELECT \"connections\" FROM \"memberships\" "
                    "WHERE \"id\" = 900031") == 1);
    mw_expect_text(reader,
                   "SELECT \"connected_at\" FROM \"memberships\" "
                   "WHERE \"id\" = 900031",
                   MW_T2_TEXT);
    mw_expect_text(reader,
                   "SELECT \"updated_at\" FROM \"memberships\" "
                   "WHERE \"id\" = 774341722",
                   MW_T2_TEXT);
    cf_db_close(reader);

    CF_CHECK(cf_membership_connected(NULL, NULL) == CF_INVALID);
    cf_membership m = {0};
    CF_CHECK(cf_membership_connected(NULL, &m) == CF_INVALID);
    mw_world_dispose(&world);
}

/* disconnected: decrement while connected, reset to 0 when stale, and clear
 * connected_at once the count reaches 0. */
typedef struct {
    cf_err rc_one_a;
    cf_err rc_one_b;
    cf_err rc_two;
    cf_err rc_three;
    cf_membership one;
    cf_membership two;
    cf_membership three;
} mw_disconnected_arg;

static cf_err mw_write_disconnected(cf_tx *tx, void *argp) {
    mw_disconnected_arg *a = argp;
    cf_membership m;
    memset(&m, 0, sizeof m);
    cf_err rc =
        cf_membership_find(cf_tx_db(tx), MW_M_DAVID_WATERCOOLER, &m);
    if (rc != CF_OK) return rc;
    cf_test_clock_set_fixed_us(MW_T2_US);
    a->rc_one_a = cf_membership_disconnected(tx, &m);
    a->rc_one_b = cf_membership_disconnected(tx, &m);
    a->one = m;
    if (a->rc_one_a != CF_OK) return a->rc_one_a;
    if (a->rc_one_b != CF_OK) return a->rc_one_b;

    memset(&m, 0, sizeof m);
    rc = cf_membership_find(cf_tx_db(tx), 900030, &m);
    if (rc != CF_OK) return rc;
    a->rc_two = cf_membership_disconnected(tx, &m);
    a->two = m;
    if (a->rc_two != CF_OK) return a->rc_two;

    memset(&m, 0, sizeof m);
    rc = cf_membership_find(cf_tx_db(tx), 900031, &m);
    if (rc != CF_OK) return rc;
    a->rc_three = cf_membership_disconnected(tx, &m);
    a->three = m;
    return a->rc_three == CF_OK ? CF_OK : a->rc_three;
}

CF_TEST(disconnected_decrements_and_clears_connected_at_at_zero) {
    mw_world world;
    mw_world_open(&world);
    cf_db *db = mw_rw(&world);
    mw_seed_user(db, MW_U_DAVID, "David", 1);
    mw_seed_room(db, MW_R_WATERCOOLER, "All Talk", "Rooms::Closed",
                 MW_U_DAVID);
    mw_seed_room(db, MW_R_DESIGNERS, "Designers", "Rooms::Closed",
                 MW_U_DAVID);
    mw_seed_room(db, MW_R_HQ, "HQ", "Rooms::Open", MW_U_DAVID);
    /* Two fresh connections: each call decrements. */
    mw_seed_membership(db, MW_M_DAVID_WATERCOOLER, MW_R_WATERCOOLER,
                       MW_U_DAVID, MW_SQL_INV_EVERYTHING, MW_SQL_NULL,
                       "'" MW_T2_TEXT "'", 2);
    /* Stale count: reset to 0 and clear connected_at. */
    mw_seed_membership(db, 900030, MW_R_DESIGNERS, MW_U_DAVID, NULL,
                       MW_SQL_NULL, "'" MW_STALE_TEXT "'", 4);
    /* Already fully disconnected: untouched. */
    mw_seed_membership(db, 900031, MW_R_HQ, MW_U_DAVID, NULL,
                       MW_SQL_NULL, MW_SQL_NULL, 0);
    mw_exec(db, "UPDATE \"memberships\" SET \"updated_at\" = '" MW_T1_TEXT
                "'");
    cf_db_close(db);

    mw_disconnected_arg arg;
    memset(&arg, 0, sizeof arg);
    CF_CHECK(cf_write(world.app, mw_write_disconnected, &arg) == CF_OK);
    cf_test_clock_clear();
    CF_CHECK(arg.rc_one_a == CF_OK && arg.rc_one_b == CF_OK &&
             arg.rc_two == CF_OK && arg.rc_three == CF_OK);

    CF_CHECK(arg.one.connections == 0 && !arg.one.connected_at.present &&
             arg.one.updated_at == MW_T2_US);
    CF_CHECK(arg.two.connections == 0 && !arg.two.connected_at.present &&
             arg.two.updated_at == MW_T2_US);
    CF_CHECK(arg.three.connections == 0 && !arg.three.connected_at.present &&
             arg.three.updated_at == MW_T1_US);

    cf_db *reader = mw_reader(&world);
    CF_CHECK(mw_i64(reader,
                    "SELECT \"connected_at\" IS NULL FROM \"memberships\" "
                    "WHERE \"id\" = 774341722") == 1);
    CF_CHECK(mw_i64(reader,
                    "SELECT \"connected_at\" IS NULL FROM \"memberships\" "
                    "WHERE \"id\" = 900030") == 1);
    mw_expect_text(reader,
                   "SELECT \"updated_at\" FROM \"memberships\" "
                   "WHERE \"id\" = 900031",
                   MW_T1_TEXT);
    cf_db_close(reader);

    CF_CHECK(cf_membership_disconnected(NULL, NULL) == CF_INVALID);
    cf_membership m = {0};
    CF_CHECK(cf_membership_disconnected(NULL, &m) == CF_INVALID);
    mw_world_dispose(&world);
}

/* refresh_connection: a stale connection is reset to 1, a live one keeps its
 * count; both touch connected_at/updated_at. */
typedef struct {
    cf_err rc_one;
    cf_err rc_two;
    cf_membership one;
    cf_membership two;
} mw_refresh_arg;

static cf_err mw_write_refresh(cf_tx *tx, void *argp) {
    mw_refresh_arg *a = argp;
    cf_membership m;
    memset(&m, 0, sizeof m);
    cf_err rc =
        cf_membership_find(cf_tx_db(tx), MW_M_DAVID_WATERCOOLER, &m);
    if (rc != CF_OK) return rc;
    cf_test_clock_set_fixed_us(MW_T2_US);
    a->rc_one = cf_membership_refresh_connection(tx, &m);
    a->one = m;
    if (a->rc_one != CF_OK) return a->rc_one;

    memset(&m, 0, sizeof m);
    rc = cf_membership_find(cf_tx_db(tx), 900030, &m);
    if (rc != CF_OK) return rc;
    a->rc_two = cf_membership_refresh_connection(tx, &m);
    a->two = m;
    return a->rc_two == CF_OK ? CF_OK : a->rc_two;
}

CF_TEST(refresh_connection_resets_stale_count_and_touches) {
    mw_world world;
    mw_world_open(&world);
    cf_db *db = mw_rw(&world);
    mw_seed_user(db, MW_U_DAVID, "David", 1);
    mw_seed_room(db, MW_R_WATERCOOLER, "All Talk", "Rooms::Closed",
                 MW_U_DAVID);
    mw_seed_room(db, MW_R_DESIGNERS, "Designers", "Rooms::Closed",
                 MW_U_DAVID);
    mw_seed_membership(db, MW_M_DAVID_WATERCOOLER, MW_R_WATERCOOLER,
                       MW_U_DAVID, MW_SQL_INV_EVERYTHING, MW_SQL_NULL,
                       "'" MW_STALE_TEXT "'", 5);
    mw_seed_membership(db, 900030, MW_R_DESIGNERS, MW_U_DAVID, NULL,
                       MW_SQL_NULL, "'" MW_T2_TEXT "'", 2);
    mw_exec(db, "UPDATE \"memberships\" SET \"updated_at\" = '" MW_T1_TEXT
                "'");
    cf_db_close(db);

    mw_refresh_arg arg;
    memset(&arg, 0, sizeof arg);
    CF_CHECK(cf_write(world.app, mw_write_refresh, &arg) == CF_OK);
    cf_test_clock_clear();
    CF_CHECK(arg.rc_one == CF_OK && arg.rc_two == CF_OK);

    CF_CHECK(arg.one.connections == 1 &&
             arg.one.connected_at.value == MW_T2_US &&
             arg.one.updated_at == MW_T2_US);
    CF_CHECK(arg.two.connections == 2 &&
             arg.two.connected_at.value == MW_T2_US &&
             arg.two.updated_at == MW_T2_US);

    cf_db *reader = mw_reader(&world);
    CF_CHECK(mw_i64(reader,
                    "SELECT \"connections\" FROM \"memberships\" "
                    "WHERE \"id\" = 774341722") == 1);
    CF_CHECK(mw_i64(reader,
                    "SELECT \"connections\" FROM \"memberships\" "
                    "WHERE \"id\" = 900030") == 2);
    cf_db_close(reader);

    CF_CHECK(cf_membership_refresh_connection(NULL, NULL) == CF_INVALID);
    cf_membership m = {0};
    CF_CHECK(cf_membership_refresh_connection(NULL, &m) == CF_INVALID);
    mw_world_dispose(&world);
}

/* reload re-reads the row (propagating CF_NOT_FOUND) and keeps the record
 * unchanged when the read fails. */
typedef struct {
    cf_err rc;
} mw_reload_arg;

static cf_err mw_write_clear_unread(cf_tx *tx, void *argp) {
    mw_reload_arg *a = argp;
    cf_membership m;
    memset(&m, 0, sizeof m);
    a->rc = cf_membership_find(cf_tx_db(tx), MW_M_DAVID_WATERCOOLER, &m);
    if (a->rc != CF_OK) return a->rc;
    cf_test_clock_set_fixed_us(MW_T2_US);
    a->rc = cf_membership_read(tx, &m);
    cf_membership_dispose(&m);
    return a->rc;
}

CF_TEST(reload_refreshes_from_the_row_and_propagates_not_found) {
    mw_world world;
    mw_world_open(&world);
    cf_db *db = mw_rw(&world);
    mw_seed_user(db, MW_U_DAVID, "David", 1);
    mw_seed_room(db, MW_R_WATERCOOLER, "All Talk", "Rooms::Closed",
                 MW_U_DAVID);
    mw_seed_membership(db, MW_M_DAVID_WATERCOOLER, MW_R_WATERCOOLER,
                       MW_U_DAVID, MW_SQL_INV_EVERYTHING, "'" MW_T1_TEXT "'",
                       MW_SQL_NULL, 0);
    mw_exec(db, "UPDATE \"memberships\" SET \"updated_at\" = '" MW_T1_TEXT
                "' WHERE \"id\" = 774341722");
    cf_db_close(db);

    cf_db *reader = mw_reader(&world);
    cf_membership m;
    memset(&m, 0, sizeof m);
    CF_REQUIRE(cf_membership_find(reader, MW_M_DAVID_WATERCOOLER, &m) ==
               CF_OK);
    CF_CHECK(m.unread_at.present && m.updated_at == MW_T1_US);

    mw_reload_arg arg = {0};
    CF_CHECK(cf_write(world.app, mw_write_clear_unread, &arg) == CF_OK);
    cf_test_clock_clear();
    CF_CHECK(arg.rc == CF_OK);

    CF_CHECK(cf_membership_reload(reader, &m) == CF_OK);
    CF_CHECK(!m.unread_at.present && m.updated_at == MW_T2_US);

    m.id = 424242;
    cf_membership before = m;
    CF_CHECK(cf_membership_reload(reader, &m) == CF_NOT_FOUND);
    CF_CHECK(m.id == before.id && m.updated_at == before.updated_at &&
             !m.unread_at.present);

    CF_CHECK(cf_membership_reload(reader, NULL) == CF_INVALID);
    CF_CHECK(cf_membership_reload(NULL, &m) == CF_INVALID);
    cf_membership_dispose(&m);
    cf_db_close(reader);
    mw_world_dispose(&world);
}

/* --- disposal ------------------------------------------------------------- */

CF_TEST(disposals_are_null_safe_and_release_owned_memory) {
    cf_membership_dispose(NULL);
    cf_membership_vector_dispose(NULL);
    cf_membership_room_pair_dispose(NULL);
    cf_membership_room_pair_vector_dispose(NULL);

    mw_world world;
    mw_world_open(&world);
    cf_db *db = mw_rw(&world);
    mw_seed_ordered_rooms(db);
    cf_db_close(db);

    cf_db *reader = mw_reader(&world);

    cf_membership_vector vector = {0};
    CF_REQUIRE(cf_membership_for_user(reader, MW_U_DAVID, &vector) == CF_OK);
    CF_REQUIRE(vector.len > 0);
    cf_membership_vector_dispose(&vector);
    CF_CHECK(vector.items == NULL && vector.len == 0 && vector.cap == 0);
    cf_membership_vector_dispose(&vector); /* double dispose is a no-op */

    cf_membership_room_pair_vector pairs = {0};
    CF_REQUIRE(cf_membership_with_ordered_room(reader, MW_U_DAVID, &pairs) ==
               CF_OK);
    CF_REQUIRE(pairs.len > 0);
    CF_CHECK(pairs.items[0].room.name.present); /* owned copies to release */
    cf_membership_room_pair_vector_dispose(&pairs);
    CF_CHECK(pairs.items == NULL && pairs.len == 0 && pairs.cap == 0);

    /* A single pair with an owned room name, disposed twice. */
    cf_membership_room_pair pair = {0};
    pair.room.name.present = true;
    pair.room.name.value.ptr = strdup("Designers");
    CF_REQUIRE(pair.room.name.value.ptr != NULL);
    pair.room.name.value.len = strlen("Designers");
    cf_membership_room_pair_dispose(&pair);
    CF_CHECK(pair.room.name.value.ptr == NULL &&
             !pair.room.name.present);
    cf_membership_room_pair_dispose(&pair);

    cf_db_close(reader);
    mw_world_dispose(&world);
}

CF_TEST_MAIN()
