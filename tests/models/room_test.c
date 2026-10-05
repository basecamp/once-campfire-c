/* D01 model family "room" tests.
 *
 * Reference oracles:
 *  - tmp/rust-ref/crates/db/src/models/room.rs (pinned source, 35 functions)
 *  - tmp/rust-ref/crates/db/src/tests/room_test.rs and tmp/rails-ref/test/
 *    fixtures/{rooms,memberships,users}.yml (fixture-shaped rows below; ids
 *    are the reference ActiveRecord::FixtureSet.identify labels:
 *    zlib.crc32(label) % (2**30 - 1)).
 *
 * Build (from the repo root; add -fsanitize=address,undefined
 * -fno-sanitize-recover=all -fno-omit-frame-pointer -g for the sanitize run):
 *   clang -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra
 *         -Werror -pthread -Isrc -Itests
 *         -Ivendor/src/sqlite/sqlite-amalgamation-3530400
 *         tests/models/room_test.c src/models/<all 15 model .c files>
 *         src/db/schema.c src/db/reader.c src/db/statements.c
 *         src/db/writer.c src/core/<all core .c files> src/config.c src/app.c
 *         vendor/build/sqlite-clang/libsqlite3.a -lm
 *         -o build/d01-room/test_room
 *
 * The mutations run through the real D02 writer (cf_writer_start + cf_write),
 * so each test exercises the actual BEGIN IMMEDIATE / COMMIT path.
 *
 * Missing shared D01 helper: src/models/types.h declares
 * cf_str_dispose/cf_optional_str_dispose/cf_int64_vector_dispose, but no
 * src/models/types.c exists in this checkout.  The weak definitions below let
 * this binary link and run; a future strong definition overrides them.
 */
#include "cf_test.h"

#include "app.h"
#include "config.h"
#include "core/testclock.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "db/writer.h"
#include "models/membership.h"
#include "models/message.h"
#include "models/room.h"
#include "models/user.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The shared release helpers come from src/models/types.c; the R02 rich-text
 * pipeline and the A01 verifier are production sources in the full test link.
 * No room path calls them (room tests seed messages with raw SQL and only
 * destroy them).  The landed R01 cf_json_string comes from
 * src/views/escape.c in the full test link. */

/* --- fixture-shaped data --------------------------------------------------- */

#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

/* 2026-01-01 00:00:00 UTC, whole seconds (stored text has no fraction). */
#define T_BASE 1767225600000000LL
#define T_SEC 1000000LL
#define T_DAY (86400LL * T_SEC)

#define U_DAVID 127326141
#define U_JASON 149087659
#define U_JZ 773523953
#define U_KEVIN 712064548
#define U_BENDER 394959859

#define R_PETS 104393281
#define R_HQ 201306877
#define R_WATERCOOLER 486777696
#define R_DESIGNERS 654632876
#define R_DAVID_AND_JASON 186869642
#define R_DAVID_AND_KEVIN 699448325
#define R_BENDER_AND_KEVIN 340026324

static cf_str text(const char *bytes) {
    cf_str value = {(char *)bytes, strlen(bytes)};
    return value;
}

static void check_str(cf_str value, const char *expected) {
    size_t len = strlen(expected);
    if (value.ptr == NULL || value.len != len ||
        (len != 0 && memcmp(value.ptr, expected, len) != 0) ||
        value.ptr[value.len] != '\0') {
        CF_CHECK(!"text mismatch");
        printf("    got \"%.*s\" (len %zu), expected \"%s\"\n",
               (int)value.len, value.ptr != NULL ? value.ptr : "", value.len,
               expected);
    }
}

static void check_room(const cf_room *room, int64_t id, const char *name,
                       cf_room_type type, int64_t creator_id) {
    CF_CHECK(room->id == id);
    CF_CHECK(room->room_type == type);
    CF_CHECK(room->creator_id == creator_id);
    if (name == NULL) {
        CF_CHECK(!room->name.present);
    } else {
        CF_CHECK(room->name.present);
        if (room->name.present) check_str(room->name.value, name);
    }
}

/* --- test environment (fixtures + real D02 writer) ------------------------- */

typedef struct {
    cf_event items[64];
    size_t len;
} event_log;

static event_log g_log;

static cf_err log_capture(void *ctx, const cf_event *event) {
    event_log *log = ctx;
    if (log->len < sizeof log->items / sizeof log->items[0]) {
        log->items[log->len++] = *event;
    }
    return CF_OK;
}

typedef struct {
    char *dir;
    char path[CF_DB_TEST_PATH_CAP];
    cf_config *config;
    cf_app *app;
    cf_db *db; /* scratch connection for setup and raw assertions */
} room_env;

static bool execf(room_env *env, const char *fmt, ...) {
    char sql[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(sql, sizeof sql, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof sql) return false;
    return cf_db_test_exec(cf_db_handle(env->db), sql) == SQLITE_OK;
}

static bool env_open(room_env *env) {
    memset(env, 0, sizeof *env);
    env->dir = cf_db_test_dir();
    if (env->dir == NULL) return false;
    cf_db_test_path(env->path, sizeof env->path, env->dir);

    cf_config_entry entries[3] = {
        {"PUBLIC_ORIGIN", "http://127.0.0.1:3000"},
        {"SECRET_KEY_BASE", HEX64},
        {"DATABASE_PATH", env->path},
    };
    if (cf_config_parse(entries, 3, NULL, &env->config) != CF_OK) return false;
    if (cf_app_create(env->config, &env->app) != CF_OK) {
        cf_config_destroy(env->config);
        env->config = NULL;
        return false;
    }
    if (cf_writer_start(env->app, env->config) != CF_OK) return false;
    if (cf_writer_set_control_handler(env->app, log_capture, &g_log) != CF_OK) {
        return false;
    }
    if (cf_writer_set_event_handler(env->app, CF_EVENT_PUSH_MESSAGE,
                                    log_capture, &g_log) != CF_OK) {
        return false;
    }
    if (cf_db_open(env->path, false, &env->db) != CF_OK) return false;
    return true;
}

static void env_close(room_env *env) {
    cf_db_close(env->db);
    env->db = NULL;
    cf_writer_stop(env->app);
    cf_app_destroy(env->app); /* owns env->config */
    env->app = NULL;
    env->config = NULL;
    cf_db_test_cleanup(env->dir);
    free(env->dir);
    env->dir = NULL;
}

/* The reference fixtures: 5 active users, 7 rooms and the memberships the
 * scoped-query tests read.  created_at values are explicit and distinct so
 * ORDER BY is observable. */
static bool load_fixtures(room_env *env) {
    struct {
        int64_t id;
        const char *name;
        const char *email_sql; /* quoted literal or NULL */
        int role;
        const char *bio_sql;
    } users[] = {
        {U_DAVID, "David", "'david@37signals.com'", 1, "NULL"},
        {U_JASON, "Jason", "'jason@37signals.com'", 1, "NULL"},
        {U_JZ, "JZ", "'jz@37signals.com'", 0, "'Designer'"},
        {U_KEVIN, "Kevin", "'kevin@37signals.com'", 0, "'Programmer'"},
        {U_BENDER, "Bender Bot", "NULL", 2, "NULL"},
    };
    for (size_t i = 0; i < sizeof users / sizeof users[0]; i++) {
        if (!execf(env,
                   "INSERT INTO users (id,name,email_address,"
                   "password_digest,role,status,bio,bot_token,created_at,"
                   "updated_at) VALUES (%lld,'%s',%s,'digest',%d,0,%s,NULL,"
                   "'2026-01-01 00:00:00','2026-01-01 00:00:00')",
                   (long long)users[i].id, users[i].name, users[i].email_sql,
                   users[i].role, users[i].bio_sql)) {
            return false;
        }
    }

    struct {
        int64_t id;
        const char *name_sql; /* quoted literal or NULL */
        const char *type;
        int64_t creator;
        const char *created;
    } rooms[] = {
        {R_WATERCOOLER, "'All Talk'", "Rooms::Closed", U_DAVID,
         "2026-01-01 00:00:00"},
        {R_PETS, "'All Pets'", "Rooms::Open", U_DAVID, "2026-01-02 00:00:00"},
        {R_HQ, "'HQ'", "Rooms::Open", U_DAVID, "2026-01-03 00:00:00"},
        {R_DESIGNERS, "'Designers'", "Rooms::Closed", U_DAVID,
         "2026-01-04 00:00:00"},
        {R_DAVID_AND_JASON, "NULL", "Rooms::Direct", U_DAVID,
         "2026-01-05 00:00:00"},
        {R_DAVID_AND_KEVIN, "NULL", "Rooms::Direct", U_DAVID,
         "2026-01-06 00:00:00"},
        {R_BENDER_AND_KEVIN, "NULL", "Rooms::Direct", U_KEVIN,
         "2026-01-07 00:00:00"},
    };
    for (size_t i = 0; i < sizeof rooms / sizeof rooms[0]; i++) {
        if (!execf(env,
                   "INSERT INTO rooms (id,name,type,creator_id,created_at,"
                   "updated_at) VALUES (%lld,%s,'%s',%lld,'%s','%s')",
                   (long long)rooms[i].id, rooms[i].name_sql, rooms[i].type,
                   (long long)rooms[i].creator, rooms[i].created,
                   rooms[i].created)) {
            return false;
        }
    }

    struct {
        int64_t room, user;
        const char *involvement;
    } memberships[] = {
        {R_DESIGNERS, U_DAVID, "mentions"},
        {R_DESIGNERS, U_JASON, "everything"},
        {R_DESIGNERS, U_JZ, "everything"},
        {R_DESIGNERS, U_KEVIN, "mentions"},
        {R_PETS, U_DAVID, "everything"},
        {R_PETS, U_JASON, "everything"},
        {R_WATERCOOLER, U_DAVID, "everything"},
        {R_WATERCOOLER, U_JASON, "everything"},
        {R_WATERCOOLER, U_BENDER, "mentions"},
        {R_HQ, U_DAVID, "everything"},
        {R_HQ, U_JASON, "everything"},
        {R_HQ, U_JZ, "everything"},
        {R_HQ, U_KEVIN, "everything"},
        {R_DAVID_AND_JASON, U_DAVID, "everything"},
        {R_DAVID_AND_JASON, U_JASON, "everything"},
        {R_DAVID_AND_KEVIN, U_DAVID, "everything"},
        {R_DAVID_AND_KEVIN, U_KEVIN, "everything"},
        {R_BENDER_AND_KEVIN, U_BENDER, "everything"},
        {R_BENDER_AND_KEVIN, U_KEVIN, "everything"},
    };
    for (size_t i = 0; i < sizeof memberships / sizeof memberships[0]; i++) {
        if (!execf(env,
                   "INSERT INTO memberships (room_id,user_id,involvement,"
                   "created_at,updated_at) VALUES (%lld,%lld,'%s',"
                   "'2026-01-01 00:00:00','2026-01-01 00:00:00')",
                   (long long)memberships[i].room, (long long)memberships[i].user,
                   memberships[i].involvement)) {
            return false;
        }
    }
    return true;
}

/* Start a test: fixtures loaded, fixed clock, empty event log. */
static bool test_begin(room_env *env) {
    g_log.len = 0;
    if (!env_open(env)) {
        CF_CHECK(!"environment open failed");
        env_close(env);
        return false;
    }
    cf_test_clock_set_fixed_us(T_BASE);
    if (!load_fixtures(env)) {
        CF_CHECK(!"fixture load failed");
        cf_test_clock_clear();
        env_close(env);
        return false;
    }
    return true;
}

static void test_end(room_env *env) {
    cf_test_clock_clear();
    env_close(env);
}

/* Raw single-value helpers on the scratch connection. */
static bool raw_i64(room_env *env, int64_t *out, const char *fmt, ...) {
    char sql[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(sql, sizeof sql, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof sql) return false;
    bool ok = false;
    int64_t value = cf_db_test_i64(cf_db_handle(env->db), sql, &ok);
    if (ok) *out = value;
    return ok;
}

static bool raw_text(room_env *env, char *buf, size_t cap, const char *fmt,
                     ...) {
    char sql[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(sql, sizeof sql, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof sql) return false;
    return cf_db_test_text(cf_db_handle(env->db), sql, buf, cap) != NULL;
}

/* --- write-operation trampoline -------------------------------------------- */

typedef cf_err (*room_test_op)(cf_tx *tx, void *arg);

struct op_call {
    room_test_op fn;
    void *arg;
};

static cf_err op_trampoline(cf_tx *tx, void *arg) {
    struct op_call *call = arg;
    return call->fn(tx, call->arg);
}

static cf_err write_op(room_env *env, room_test_op fn, void *arg) {
    struct op_call call = {fn, arg};
    return cf_write(env->app, op_trampoline, &call);
}

struct create_op {
    cf_room_type type;
    const char *name; /* NULL: absent */
    int64_t creator;
    bool create_for;
    const int64_t *user_ids;
    size_t user_ids_len;
    cf_room room;
};

static cf_err op_create(cf_tx *tx, void *arg) {
    struct create_op *op = arg;
    cf_optional_str name = {0};
    if (op->name != NULL) {
        name.present = true;
        name.value = text(op->name);
    }
    if (op->create_for) {
        return cf_room_create_for(tx, op->type, name, op->creator,
                                  op->user_ids, op->user_ids_len, &op->room);
    }
    return cf_room_create(tx, op->type, name, op->creator, &op->room);
}

struct find_or_create_op {
    const int64_t *user_ids;
    size_t user_ids_len;
    int64_t creator;
    cf_room room;
};

static cf_err op_find_or_create_direct(cf_tx *tx, void *arg) {
    struct find_or_create_op *op = arg;
    return cf_room_find_or_create_direct_for(tx, op->user_ids,
                                             op->user_ids_len, op->creator,
                                             &op->room);
}

struct room_ids_op {
    const cf_room *room;
    const int64_t *user_ids;
    size_t user_ids_len;
};

static cf_err op_grant(cf_tx *tx, void *arg) {
    struct room_ids_op *op = arg;
    return cf_room_grant_to(tx, op->room, op->user_ids, op->user_ids_len);
}

static cf_err op_revoke(cf_tx *tx, void *arg) {
    struct room_ids_op *op = arg;
    return cf_room_revoke_from(tx, op->room, op->user_ids, op->user_ids_len);
}

struct revise_op {
    const cf_room *room;
    const int64_t *granted;
    size_t granted_len;
    const int64_t *revoked;
    size_t revoked_len;
};

static cf_err op_revise(cf_tx *tx, void *arg) {
    struct revise_op *op = arg;
    return cf_room_revise(tx, op->room, op->granted, op->granted_len,
                          op->revoked, op->revoked_len);
}

struct update_op {
    cf_room *room;
    const cf_optional_str *name; /* NULL: leave unchanged */
    const cf_room_type *type;    /* NULL: leave unchanged */
};

static cf_err op_update(cf_tx *tx, void *arg) {
    struct update_op *op = arg;
    return cf_room_update(tx, op->room, op->name, op->type);
}

struct touch_op {
    int64_t room_id;
};

static cf_err op_touch(cf_tx *tx, void *arg) {
    struct touch_op *op = arg;
    return cf_room_touch(tx, op->room_id);
}

struct destroy_op {
    const cf_room *room;
};

static cf_err op_destroy(cf_tx *tx, void *arg) {
    struct destroy_op *op = arg;
    return cf_room_destroy(tx, op->room);
}

struct receive_op {
    int64_t room_id;
    cf_message message;
};

static cf_err op_receive(cf_tx *tx, void *arg) {
    struct receive_op *op = arg;
    return cf_room_receive(tx, op->room_id, &op->message);
}

/* --- read helpers ---------------------------------------------------------- */

static bool room_vector_has(const cf_room_vector *rooms, int64_t id) {
    for (size_t i = 0; i < rooms->len; i++) {
        if (rooms->items[i].id == id) return true;
    }
    return false;
}

/* --- tests ----------------------------------------------------------------- */

CF_TEST(find_and_find_by_id) {
    room_env env;
    CF_REQUIRE(test_begin(&env));

    cf_room room = {0};
    CF_CHECK(cf_room_find(env.db, R_WATERCOOLER, &room) == CF_OK);
    check_room(&room, R_WATERCOOLER, "All Talk", CF_ROOM_CLOSED, U_DAVID);
    cf_room_dispose(&room);

    /* Direct rooms have no name. */
    CF_CHECK(cf_room_find(env.db, R_DAVID_AND_JASON, &room) == CF_OK);
    check_room(&room, R_DAVID_AND_JASON, NULL, CF_ROOM_DIRECT, U_DAVID);
    cf_room_dispose(&room);

    /* Room::find: CF_NOT_FOUND when absent, out stays empty. */
    room.id = 42;
    CF_CHECK(cf_room_find(env.db, 999999, &room) == CF_NOT_FOUND);
    CF_CHECK(room.id == 0 && room.name.value.ptr == NULL && !room.name.present);

    bool found = true;
    CF_CHECK(cf_room_find_by_id(env.db, R_HQ, &found, &room) == CF_OK);
    CF_CHECK(found);
    check_room(&room, R_HQ, "HQ", CF_ROOM_OPEN, U_DAVID);
    cf_room_dispose(&room);

    found = true;
    CF_CHECK(cf_room_find_by_id(env.db, 999999, &found, &room) == CF_OK);
    CF_CHECK(!found);
    CF_CHECK(room.id == 0 && room.name.value.ptr == NULL && !room.name.present);

    test_end(&env);
}

CF_TEST(all_of_type_count_and_original) {
    room_env env;
    CF_REQUIRE(test_begin(&env));

    cf_room_vector all = {0};
    CF_REQUIRE(cf_room_all(env.db, &all) == CF_OK);
    CF_CHECK(all.len == 7);
    CF_CHECK(room_vector_has(&all, R_WATERCOOLER));
    CF_CHECK(room_vector_has(&all, R_BENDER_AND_KEVIN));
    cf_room_vector_dispose(&all);

    cf_room_vector opens = {0};
    CF_REQUIRE(cf_room_of_type(env.db, CF_ROOM_OPEN, &opens) == CF_OK);
    CF_CHECK(opens.len == 2);
    CF_CHECK(room_vector_has(&opens, R_PETS) && room_vector_has(&opens, R_HQ));
    cf_room_vector_dispose(&opens);

    cf_room_vector directs = {0};
    CF_REQUIRE(cf_room_of_type(env.db, CF_ROOM_DIRECT, &directs) == CF_OK);
    CF_CHECK(directs.len == 3);
    cf_room_vector_dispose(&directs);

    int64_t count = -1;
    CF_REQUIRE(cf_room_count_of_type(env.db, CF_ROOM_CLOSED, &count) == CF_OK);
    CF_CHECK(count == 2);
    CF_REQUIRE(cf_room_count_of_type(env.db, CF_ROOM_OPEN, &count) == CF_OK);
    CF_CHECK(count == 2);
    CF_REQUIRE(cf_room_count_of_type(env.db, CF_ROOM_DIRECT, &count) == CF_OK);
    CF_CHECK(count == 3);

    /* No closed rooms in a fresh database: empty vector, no error. */
    bool found = true;
    cf_room original = {0};
    CF_REQUIRE(cf_room_original(env.db, &found, &original) == CF_OK);
    CF_CHECK(found);
    check_room(&original, R_WATERCOOLER, "All Talk", CF_ROOM_CLOSED, U_DAVID);
    cf_room_dispose(&original);

    test_end(&env);
}

CF_TEST(user_scopes) {
    room_env env;
    CF_REQUIRE(test_begin(&env));

    cf_room_vector rooms = {0};
    CF_REQUIRE(cf_room_for_user(env.db, U_DAVID, &rooms) == CF_OK);
    CF_CHECK(rooms.len == 6);
    CF_CHECK(room_vector_has(&rooms, R_WATERCOOLER));
    CF_CHECK(room_vector_has(&rooms, R_DAVID_AND_KEVIN));
    CF_CHECK(!room_vector_has(&rooms, R_BENDER_AND_KEVIN));
    cf_room_vector_dispose(&rooms);

    bool found = true;
    cf_room room = {0};
    CF_REQUIRE(cf_room_find_for_user(env.db, U_DAVID, R_WATERCOOLER, &found,
                                     &room) == CF_OK);
    CF_CHECK(found);
    check_room(&room, R_WATERCOOLER, "All Talk", CF_ROOM_CLOSED, U_DAVID);
    cf_room_dispose(&room);

    /* kevin is not in pets. */
    found = true;
    CF_REQUIRE(cf_room_find_for_user(env.db, U_KEVIN, R_PETS, &found, &room) ==
               CF_OK);
    CF_CHECK(!found);
    CF_CHECK(room.id == 0);

    CF_REQUIRE(cf_room_for_user_of_type(env.db, U_DAVID, CF_ROOM_DIRECT,
                                        &rooms) == CF_OK);
    CF_CHECK(rooms.len == 2);
    cf_room_vector_dispose(&rooms);

    CF_REQUIRE(cf_room_for_user_of_type(env.db, U_DAVID, CF_ROOM_CLOSED,
                                        &rooms) == CF_OK);
    CF_CHECK(rooms.len == 2);
    cf_room_vector_dispose(&rooms);

    CF_REQUIRE(cf_room_for_user_without_directs(env.db, U_DAVID, &rooms) ==
               CF_OK);
    CF_CHECK(rooms.len == 4);
    CF_CHECK(!room_vector_has(&rooms, R_DAVID_AND_JASON));
    cf_room_vector_dispose(&rooms);

    CF_REQUIRE(cf_room_original_for_user(env.db, U_DAVID, &found, &room) ==
               CF_OK);
    CF_CHECK(found && room.id == R_WATERCOOLER);
    cf_room_dispose(&room);
    /* kevin's rooms: designers (01-04), hq (01-03), david_and_kevin,
     * bender_and_kevin; hq is the oldest. */
    CF_REQUIRE(cf_room_original_for_user(env.db, U_KEVIN, &found, &room) ==
               CF_OK);
    CF_CHECK(found && room.id == R_HQ);
    cf_room_dispose(&room);

    CF_REQUIRE(cf_room_last_for_user(env.db, U_DAVID, &found, &room) == CF_OK);
    CF_CHECK(found && room.id == R_DAVID_AND_KEVIN);
    cf_room_dispose(&room);

    /* A user with no memberships: empty scope, no error. */
    cf_room_vector none = {0};
    CF_REQUIRE(cf_room_for_user(env.db, 999999, &none) == CF_OK);
    CF_CHECK(none.len == 0 && none.items == NULL);
    CF_CHECK(cf_room_original_for_user(env.db, 999999, &found, &room) == CF_OK);
    CF_CHECK(!found);
    CF_CHECK(cf_room_last_for_user(env.db, 999999, &found, &room) == CF_OK);
    CF_CHECK(!found);

    test_end(&env);
}

CF_TEST(associations_users_user_ids_active_bots_memberships) {
    room_env env;
    CF_REQUIRE(test_begin(&env));

    cf_room room = {0};
    CF_REQUIRE(cf_room_find(env.db, R_DAVID_AND_KEVIN, &room) == CF_OK);

    cf_user_vector users = {0};
    CF_REQUIRE(cf_room_users(env.db, &room, &users) == CF_OK);
    CF_CHECK(users.len == 2);
    bool saw_david = false, saw_kevin = false;
    for (size_t i = 0; i < users.len; i++) {
        if (users.items[i].id == U_DAVID) {
            saw_david = true;
            check_str(users.items[i].name, "David");
        }
        if (users.items[i].id == U_KEVIN) saw_kevin = true;
    }
    CF_CHECK(saw_david && saw_kevin);
    cf_user_vector_dispose(&users);

    cf_int64_vector ids = {0};
    CF_REQUIRE(cf_room_user_ids(env.db, &room, &ids) == CF_OK);
    CF_CHECK(ids.len == 2);
    CF_CHECK((ids.items[0] == U_DAVID && ids.items[1] == U_KEVIN) ||
             (ids.items[0] == U_KEVIN && ids.items[1] == U_DAVID));
    cf_int64_vector_dispose(&ids);
    cf_room_dispose(&room);

    /* active_bots: only active (status 0) bots (role 2). */
    CF_REQUIRE(cf_room_find(env.db, R_WATERCOOLER, &room) == CF_OK);
    cf_user_vector bots = {0};
    CF_REQUIRE(cf_room_active_bots(env.db, &room, &bots) == CF_OK);
    CF_REQUIRE(bots.len == 1);
    CF_CHECK(bots.items[0].id == U_BENDER);
    CF_CHECK(bots.items[0].role == CF_ROLE_BOT);
    CF_CHECK(bots.items[0].status == CF_STATUS_ACTIVE);
    cf_user_vector_dispose(&bots);

    cf_membership_vector memberships = {0};
    CF_REQUIRE(cf_room_memberships(env.db, &room, &memberships) == CF_OK);
    CF_CHECK(memberships.len == 3);
    bool saw_involvement = false;
    for (size_t i = 0; i < memberships.len; i++) {
        if (memberships.items[i].user_id == U_JASON) {
            CF_CHECK(memberships.items[i].involvement.present);
            CF_CHECK(memberships.items[i].involvement.value ==
                     CF_INVOLVEMENT_EVERYTHING);
            saw_involvement = true;
        }
    }
    CF_CHECK(saw_involvement);
    cf_membership_vector_dispose(&memberships);
    cf_room_dispose(&room);

    test_end(&env);
}

CF_TEST(type_predicates_and_helpers) {
    room_env env;
    CF_REQUIRE(test_begin(&env));

    cf_room room = {0};
    CF_REQUIRE(cf_room_find(env.db, R_PETS, &room) == CF_OK);
    CF_CHECK(cf_room_open(&room) && !cf_room_closed(&room) &&
             !cf_room_direct(&room));
    CF_CHECK(cf_room_default_involvement(&room) == CF_INVOLVEMENT_MENTIONS);
    cf_room_dispose(&room);
    CF_REQUIRE(cf_room_find(env.db, R_DESIGNERS, &room) == CF_OK);
    CF_CHECK(cf_room_closed(&room) && !cf_room_open(&room));
    cf_room_dispose(&room);
    CF_REQUIRE(cf_room_find(env.db, R_DAVID_AND_JASON, &room) == CF_OK);
    CF_CHECK(cf_room_direct(&room) && !cf_room_open(&room));
    CF_CHECK(cf_room_default_involvement(&room) ==
             CF_INVOLVEMENT_EVERYTHING);
    cf_room_dispose(&room);

    CF_CHECK(strcmp(cf_room_type_class_name(CF_ROOM_OPEN), "Rooms::Open") == 0);
    CF_CHECK(strcmp(cf_room_type_class_name(CF_ROOM_CLOSED),
                    "Rooms::Closed") == 0);
    CF_CHECK(strcmp(cf_room_type_class_name(CF_ROOM_DIRECT),
                    "Rooms::Direct") == 0);
    CF_CHECK(cf_room_type_class_name((cf_room_type)7) == NULL);

    cf_room_type parsed = (cf_room_type)99;
    CF_REQUIRE(cf_room_type_from_class_name(CF_STR_LIT("Rooms::Open"),
                                            &parsed));
    CF_CHECK(parsed == CF_ROOM_OPEN);
    CF_REQUIRE(cf_room_type_from_class_name(CF_STR_LIT("Rooms::Closed"),
                                            &parsed));
    CF_CHECK(parsed == CF_ROOM_CLOSED);
    CF_REQUIRE(cf_room_type_from_class_name(CF_STR_LIT("Rooms::Direct"),
                                            &parsed));
    CF_CHECK(parsed == CF_ROOM_DIRECT);
    parsed = (cf_room_type)99;
    CF_CHECK(!cf_room_type_from_class_name(CF_STR_LIT("Rooms::Other"),
                                           &parsed));
    CF_CHECK(!cf_room_type_from_class_name(CF_STR_LIT(""), &parsed));
    CF_CHECK(parsed == (cf_room_type)99); /* failure leaves out untouched */
    CF_CHECK(!cf_room_type_from_class_name(CF_STR_LIT("Rooms::Open"), NULL));
    cf_str bad = {NULL, 3};
    CF_CHECK(!cf_room_type_from_class_name(bad, &parsed));

    CF_CHECK(cf_room_type_default_involvement(CF_ROOM_DIRECT) ==
             CF_INVOLVEMENT_EVERYTHING);
    CF_CHECK(cf_room_type_default_involvement(CF_ROOM_OPEN) ==
             CF_INVOLVEMENT_MENTIONS);
    CF_CHECK(cf_room_type_default_involvement(CF_ROOM_CLOSED) ==
             CF_INVOLVEMENT_MENTIONS);
    CF_CHECK(cf_room_type_default_involvement((cf_room_type)7) ==
             CF_INVOLVEMENT_MENTIONS);

    test_end(&env);
}

CF_TEST(create_closed_and_open_grants_all_active_users) {
    room_env env;
    CF_REQUIRE(test_begin(&env));

    /* Closed with a name: no automatic grants. */
    struct create_op closed = {
        .type = CF_ROOM_CLOSED, .name = "Hello!", .creator = U_DAVID};
    CF_REQUIRE(write_op(&env, op_create, &closed) == CF_OK);
    check_room(&closed.room, closed.room.id, "Hello!", CF_ROOM_CLOSED,
               U_DAVID);
    CF_CHECK(closed.room.created_at == T_BASE);
    CF_CHECK(closed.room.updated_at == T_BASE);
    int64_t members = -1;
    CF_REQUIRE(raw_i64(&env, &members,
                       "SELECT COUNT(*) FROM memberships WHERE room_id = %lld",
                       (long long)closed.room.id));
    CF_CHECK(members == 0);
    cf_room_dispose(&closed.room);

    /* Open: grants itself to every active user (status = 0). */
    struct create_op open = {
        .type = CF_ROOM_OPEN, .name = "My open room!", .creator = U_DAVID};
    CF_REQUIRE(write_op(&env, op_create, &open) == CF_OK);
    CF_CHECK(cf_room_open(&open.room));
    CF_REQUIRE(raw_i64(&env, &members,
                       "SELECT COUNT(*) FROM memberships WHERE room_id = %lld",
                       (long long)open.room.id));
    CF_CHECK(members == 5);
    int64_t mentions = -1;
    CF_REQUIRE(raw_i64(&env, &mentions,
                       "SELECT COUNT(*) FROM memberships WHERE room_id = %lld "
                       "AND involvement = 'mentions'",
                       (long long)open.room.id));
    CF_CHECK(mentions == 5);
    /* SQLite stamped the rows: STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW') text. */
    char stamp[64] = {0};
    CF_REQUIRE(raw_text(&env, stamp, sizeof stamp,
                        "SELECT created_at FROM memberships WHERE room_id = "
                        "%lld LIMIT 1",
                        (long long)open.room.id));
    CF_CHECK(strlen(stamp) == strlen("2026-09-26 12:25:26.826"));
    cf_room_dispose(&open.room);

    /* Absent name stays SQL NULL. */
    struct create_op direct = {
        .type = CF_ROOM_DIRECT, .name = NULL, .creator = U_JASON};
    CF_REQUIRE(write_op(&env, op_create, &direct) == CF_OK);
    check_room(&direct.room, direct.room.id, NULL, CF_ROOM_DIRECT, U_JASON);
    CF_CHECK(direct.room.name.present == false);
    cf_room_dispose(&direct.room);

    test_end(&env);
}

CF_TEST(create_for_grants_given_users) {
    room_env env;
    CF_REQUIRE(test_begin(&env));

    const int64_t granted[] = {U_KEVIN, U_DAVID};
    struct create_op op = {.type = CF_ROOM_CLOSED,
                           .name = "Hello!",
                           .creator = U_DAVID,
                           .create_for = true,
                           .user_ids = granted,
                           .user_ids_len = 2};
    CF_REQUIRE(write_op(&env, op_create, &op) == CF_OK);

    int64_t members = -1;
    CF_REQUIRE(raw_i64(&env, &members,
                       "SELECT COUNT(*) FROM memberships WHERE room_id = %lld",
                       (long long)op.room.id));
    CF_CHECK(members == 2);
    int64_t mentions = -1;
    CF_REQUIRE(raw_i64(&env, &mentions,
                       "SELECT COUNT(*) FROM memberships WHERE room_id = %lld "
                       "AND involvement = 'mentions'",
                       (long long)op.room.id));
    CF_CHECK(mentions == 2);
    cf_room_dispose(&op.room);

    test_end(&env);
}

CF_TEST(find_or_create_direct_for_matches_member_sets) {
    room_env env;
    CF_REQUIRE(test_begin(&env));

    const int64_t jz_kevin[] = {U_JZ, U_KEVIN};
    struct find_or_create_op first = {.user_ids = jz_kevin,
                                      .user_ids_len = 2,
                                      .creator = U_JZ};
    CF_REQUIRE(write_op(&env, op_find_or_create_direct, &first) == CF_OK);
    CF_CHECK(cf_room_direct(&first.room));

    /* The same users in another order find the same room. */
    const int64_t kevin_jz[] = {U_KEVIN, U_JZ};
    struct find_or_create_op second = {.user_ids = kevin_jz,
                                       .user_ids_len = 2,
                                       .creator = U_KEVIN};
    CF_REQUIRE(write_op(&env, op_find_or_create_direct, &second) == CF_OK);
    CF_CHECK(second.room.id == first.room.id);

    /* The existing fixture direct room is found by its member set. */
    const int64_t david_kevin[] = {U_DAVID, U_KEVIN};
    struct find_or_create_op existing = {.user_ids = david_kevin,
                                         .user_ids_len = 2,
                                         .creator = U_DAVID};
    CF_REQUIRE(write_op(&env, op_find_or_create_direct, &existing) == CF_OK);
    CF_CHECK(existing.room.id == R_DAVID_AND_KEVIN);

    /* Duplicate ids are a set in the reference; one membership each. */
    const int64_t jz_twice[] = {U_JZ, U_JZ, U_KEVIN};
    struct find_or_create_op dup = {.user_ids = jz_twice,
                                    .user_ids_len = 3,
                                    .creator = U_JZ};
    CF_REQUIRE(write_op(&env, op_find_or_create_direct, &dup) == CF_OK);
    CF_CHECK(dup.room.id == first.room.id);

    int64_t members = -1;
    CF_REQUIRE(raw_i64(&env, &members,
                       "SELECT COUNT(*) FROM memberships WHERE room_id = %lld",
                       (long long)first.room.id));
    CF_CHECK(members == 2);
    int64_t everything = -1;
    CF_REQUIRE(raw_i64(&env, &everything,
                       "SELECT COUNT(*) FROM memberships WHERE room_id = %lld "
                       "AND involvement = 'everything'",
                       (long long)first.room.id));
    CF_CHECK(everything == 2);

    cf_room_dispose(&first.room);
    cf_room_dispose(&second.room);
    cf_room_dispose(&existing.room);
    cf_room_dispose(&dup.room);

    /* find_direct_for on its own: found and not found. */
    bool found = true;
    cf_room room = {0};
    CF_REQUIRE(cf_room_find_direct_for(env.db, david_kevin, 2, &found,
                                       &room) == CF_OK);
    CF_CHECK(found && room.id == R_DAVID_AND_KEVIN);
    cf_room_dispose(&room);

    const int64_t nobody[] = {U_JASON, U_BENDER};
    found = true;
    CF_REQUIRE(cf_room_find_direct_for(env.db, nobody, 2, &found, &room) ==
               CF_OK);
    CF_CHECK(!found && room.id == 0);

    /* Empty set matches no room (and must not crash). */
    found = true;
    CF_REQUIRE(cf_room_find_direct_for(env.db, NULL, 0, &found, &room) ==
               CF_OK);
    CF_CHECK(!found);

    test_end(&env);
}

CF_TEST(update_name_type_and_direct_guard) {
    room_env env;
    CF_REQUIRE(test_begin(&env));

    cf_room room = {0};
    CF_REQUIRE(cf_room_find(env.db, R_WATERCOOLER, &room) == CF_OK);

    /* Present name equal to the current one: filtered out, no write. */
    cf_test_clock_set_fixed_us(T_BASE + 10 * T_SEC);
    cf_optional_str same = {.present = true, .value = {"All Talk", 8}};
    struct update_op noop = {.room = &room, .name = &same, .type = NULL};
    CF_REQUIRE(write_op(&env, op_update, &noop) == CF_OK);
    CF_CHECK(room.updated_at == T_BASE);
    char stamp[64] = {0};
    CF_REQUIRE(raw_text(&env, stamp, sizeof stamp,
                        "SELECT updated_at FROM rooms WHERE id = %lld",
                        (long long)R_WATERCOOLER));
    CF_CHECK(strcmp(stamp, "2026-01-01 00:00:00") == 0);

    /* Rename. */
    cf_optional_str renamed = {.present = true, .value = {"Renamed", 7}};
    struct update_op rename = {.room = &room, .name = &renamed, .type = NULL};
    CF_REQUIRE(write_op(&env, op_update, &rename) == CF_OK);
    CF_CHECK(room.name.present);
    check_str(room.name.value, "Renamed");
    CF_CHECK(room.updated_at == T_BASE + 10 * T_SEC);
    CF_REQUIRE(raw_text(&env, stamp, sizeof stamp,
                        "SELECT name FROM rooms WHERE id = %lld",
                        (long long)R_WATERCOOLER));
    CF_CHECK(strcmp(stamp, "Renamed") == 0);
    CF_REQUIRE(raw_text(&env, stamp, sizeof stamp,
                        "SELECT updated_at FROM rooms WHERE id = %lld",
                        (long long)R_WATERCOOLER));
    CF_CHECK(strcmp(stamp, "2026-01-01 00:00:10") == 0);

    /* NULL outer name leaves it unchanged. */
    struct update_op untouched = {.room = &room, .name = NULL, .type = NULL};
    CF_REQUIRE(write_op(&env, op_update, &untouched) == CF_OK);
    CF_CHECK(room.name.present && strcmp(room.name.value.ptr, "Renamed") == 0);

    /* present=false writes SQL NULL. */
    cf_optional_str absent = {.present = false};
    struct update_op clear = {.room = &room, .name = &absent, .type = NULL};
    CF_REQUIRE(write_op(&env, op_update, &clear) == CF_OK);
    CF_CHECK(!room.name.present);
    int64_t is_null = -1;
    CF_REQUIRE(raw_i64(&env, &is_null,
                       "SELECT name IS NULL FROM rooms WHERE id = %lld",
                       (long long)R_WATERCOOLER));
    CF_CHECK(is_null == 1);
    cf_room_dispose(&room);

    /* A direct room cannot change type: CF_INVALID, row unchanged. */
    CF_REQUIRE(cf_room_find(env.db, R_DAVID_AND_JASON, &room) == CF_OK);
    cf_room_type open = CF_ROOM_OPEN;
    struct update_op illegal = {.room = &room, .name = NULL, .type = &open};
    CF_CHECK(write_op(&env, op_update, &illegal) == CF_INVALID);
    CF_CHECK(room.room_type == CF_ROOM_DIRECT);
    CF_REQUIRE(raw_text(&env, stamp, sizeof stamp,
                        "SELECT type FROM rooms WHERE id = %lld",
                        (long long)R_DAVID_AND_JASON));
    CF_CHECK(strcmp(stamp, "Rooms::Direct") == 0);
    cf_room_dispose(&room);

    test_end(&env);
}

CF_TEST(update_becoming_open_grants_every_active_user) {
    room_env env;
    CF_REQUIRE(test_begin(&env));

    cf_room room = {0};
    CF_REQUIRE(cf_room_find(env.db, R_DESIGNERS, &room) == CF_OK);
    cf_room_type open = CF_ROOM_OPEN;
    cf_optional_str name = {.present = true, .value = {"Open Designers", 14}};
    cf_test_clock_set_fixed_us(T_BASE + 20 * T_SEC);
    struct update_op update = {.room = &room, .name = &name, .type = &open};
    CF_REQUIRE(write_op(&env, op_update, &update) == CF_OK);
    CF_CHECK(room.room_type == CF_ROOM_OPEN);
    CF_CHECK(room.updated_at == T_BASE + 20 * T_SEC);

    int64_t members = -1;
    CF_REQUIRE(raw_i64(&env, &members,
                       "SELECT COUNT(*) FROM memberships WHERE room_id = %lld",
                       (long long)R_DESIGNERS));
    CF_CHECK(members == 5); /* 4 existing + bender */
    int64_t bender = -1;
    CF_REQUIRE(raw_i64(&env, &bender,
                       "SELECT COUNT(*) FROM memberships WHERE room_id = %lld "
                       "AND user_id = %lld AND involvement = 'mentions'",
                       (long long)R_DESIGNERS, (long long)U_BENDER));
    CF_CHECK(bender == 1);
    cf_room_dispose(&room);

    test_end(&env);
}

CF_TEST(touch_and_reload) {
    room_env env;
    CF_REQUIRE(test_begin(&env));

    cf_test_clock_set_fixed_us(T_BASE + 30 * T_SEC);
    struct touch_op touch = {.room_id = R_HQ};
    CF_REQUIRE(write_op(&env, op_touch, &touch) == CF_OK);

    cf_room room = {0};
    CF_REQUIRE(cf_room_find(env.db, R_HQ, &room) == CF_OK);
    CF_CHECK(room.updated_at == T_BASE + 30 * T_SEC);
    CF_CHECK(room.created_at == T_BASE + 2 * T_DAY);
    cf_room_dispose(&room);

    /* Touch of a missing room updates no row but is not an error. */
    struct touch_op missing = {.room_id = 999999};
    CF_REQUIRE(write_op(&env, op_touch, &missing) == CF_OK);

    /* Reload picks up a raw external update. */
    CF_REQUIRE(cf_room_find(env.db, R_PETS, &room) == CF_OK);
    CF_REQUIRE(execf(&env,
                     "UPDATE rooms SET name = 'New Pets', type = 'Rooms::Closed'"
                     " WHERE id = %lld",
                     (long long)R_PETS));
    CF_REQUIRE(cf_room_reload(env.db, &room) == CF_OK);
    check_room(&room, R_PETS, "New Pets", CF_ROOM_CLOSED, U_DAVID);
    cf_room_dispose(&room);

    /* Reload of a deleted room: CF_NOT_FOUND and the record unchanged. */
    CF_REQUIRE(cf_room_find(env.db, R_HQ, &room) == CF_OK);
    CF_REQUIRE(execf(&env, "DELETE FROM rooms WHERE id = %lld",
                     (long long)R_HQ));
    CF_CHECK(cf_room_reload(env.db, &room) == CF_NOT_FOUND);
    check_room(&room, R_HQ, "HQ", CF_ROOM_OPEN, U_DAVID);
    cf_room_dispose(&room);

    test_end(&env);
}

CF_TEST(grant_to_adds_and_skips_existing) {
    room_env env;
    CF_REQUIRE(test_begin(&env));

    cf_room room = {0};
    CF_REQUIRE(cf_room_find(env.db, R_WATERCOOLER, &room) == CF_OK);

    /* kevin is not a member yet: closed rooms default to mentions. */
    const int64_t kevin[] = {U_KEVIN};
    struct room_ids_op grant = {.room = &room, .user_ids = kevin,
                                .user_ids_len = 1};
    CF_REQUIRE(write_op(&env, op_grant, &grant) == CF_OK);
    int64_t count = -1;
    CF_REQUIRE(raw_i64(&env, &count,
                       "SELECT COUNT(*) FROM memberships WHERE room_id = %lld",
                       (long long)R_WATERCOOLER));
    CF_CHECK(count == 4);
    CF_REQUIRE(raw_i64(&env, &count,
                       "SELECT COUNT(*) FROM memberships WHERE room_id = %lld "
                       "AND user_id = %lld AND involvement = 'mentions'",
                       (long long)R_WATERCOOLER, (long long)U_KEVIN));
    CF_CHECK(count == 1);

    /* Granting david again conflicts and does nothing (no duplicates). */
    const int64_t david[] = {U_DAVID};
    struct room_ids_op again = {.room = &room, .user_ids = david,
                                .user_ids_len = 1};
    CF_REQUIRE(write_op(&env, op_grant, &again) == CF_OK);
    CF_REQUIRE(raw_i64(&env, &count,
                       "SELECT COUNT(*) FROM memberships WHERE room_id = %lld",
                       (long long)R_WATERCOOLER));
    CF_CHECK(count == 4);

    /* Empty slice: no rows, no error. */
    struct room_ids_op empty = {.room = &room, .user_ids = NULL,
                                .user_ids_len = 0};
    CF_REQUIRE(write_op(&env, op_grant, &empty) == CF_OK);
    CF_REQUIRE(raw_i64(&env, &count,
                       "SELECT COUNT(*) FROM memberships WHERE room_id = %lld",
                       (long long)R_WATERCOOLER));
    CF_CHECK(count == 4);

    cf_room_dispose(&room);
    test_end(&env);
}

CF_TEST(revoke_from_destroys_and_disconnects) {
    room_env env;
    CF_REQUIRE(test_begin(&env));

    cf_room room = {0};
    CF_REQUIRE(cf_room_find(env.db, R_WATERCOOLER, &room) == CF_OK);
    const int64_t david[] = {U_DAVID};
    struct room_ids_op revoke = {.room = &room, .user_ids = david,
                                 .user_ids_len = 1};
    CF_REQUIRE(write_op(&env, op_revoke, &revoke) == CF_OK);
    int64_t count = -1;
    CF_REQUIRE(raw_i64(&env, &count,
                       "SELECT COUNT(*) FROM memberships WHERE room_id = %lld",
                       (long long)R_WATERCOOLER));
    CF_CHECK(count == 2);

    /* Each removed member reconnects after commit. */
    CF_REQUIRE(g_log.len == 1);
    CF_CHECK(g_log.items[0].kind == CF_EVENT_DISCONNECT_USER);
    CF_CHECK(g_log.items[0].user_id == U_DAVID);
    CF_CHECK(g_log.items[0].reconnect == true);
    CF_CHECK(g_log.items[0].room_id == 0 && g_log.items[0].message_id == 0 &&
             g_log.items[0].blob_id == 0);

    /* Non-members and empty lists remove nothing. */
    cf_room room2 = {0};
    CF_REQUIRE(cf_room_find(env.db, R_PETS, &room2) == CF_OK);
    g_log.len = 0;
    const int64_t missing[] = {U_BENDER};
    struct room_ids_op nohit = {.room = &room2, .user_ids = missing,
                                .user_ids_len = 1};
    CF_REQUIRE(write_op(&env, op_revoke, &nohit) == CF_OK);
    CF_CHECK(g_log.len == 0);
    struct room_ids_op empty = {.room = &room2, .user_ids = NULL,
                                .user_ids_len = 0};
    CF_REQUIRE(write_op(&env, op_revoke, &empty) == CF_OK);

    cf_room_dispose(&room);
    cf_room_dispose(&room2);
    test_end(&env);
}

CF_TEST(revise_grants_then_revokes) {
    room_env env;
    CF_REQUIRE(test_begin(&env));

    cf_room room = {0};
    CF_REQUIRE(cf_room_find(env.db, R_WATERCOOLER, &room) == CF_OK);
    const int64_t granted[] = {U_KEVIN};
    const int64_t revoked[] = {U_JASON};
    struct revise_op revise = {.room = &room,
                               .granted = granted,
                               .granted_len = 1,
                               .revoked = revoked,
                               .revoked_len = 1};
    CF_REQUIRE(write_op(&env, op_revise, &revise) == CF_OK);

    int64_t count = -1;
    CF_REQUIRE(raw_i64(&env, &count,
                       "SELECT COUNT(*) FROM memberships WHERE room_id = %lld "
                       "AND user_id = %lld",
                       (long long)R_WATERCOOLER, (long long)U_KEVIN));
    CF_CHECK(count == 1);
    CF_REQUIRE(raw_i64(&env, &count,
                       "SELECT COUNT(*) FROM memberships WHERE room_id = %lld "
                       "AND user_id = %lld",
                       (long long)R_WATERCOOLER, (long long)U_JASON));
    CF_CHECK(count == 0);
    CF_REQUIRE(g_log.len == 1);
    CF_CHECK(g_log.items[0].user_id == U_JASON &&
             g_log.items[0].reconnect == true);

    /* Both empty: no writes, no events. */
    g_log.len = 0;
    struct revise_op none = {.room = &room,
                             .granted = NULL,
                             .granted_len = 0,
                             .revoked = NULL,
                             .revoked_len = 0};
    CF_REQUIRE(write_op(&env, op_revise, &none) == CF_OK);
    CF_CHECK(g_log.len == 0);

    cf_room_dispose(&room);
    test_end(&env);
}

CF_TEST(destroy_removes_memberships_messages_and_dependents) {
    room_env env;
    CF_REQUIRE(test_begin(&env));

    const int64_t granted[] = {U_DAVID, U_KEVIN};
    struct create_op created = {.type = CF_ROOM_CLOSED,
                                .name = "Doomed",
                                .creator = U_DAVID,
                                .create_for = true,
                                .user_ids = granted,
                                .user_ids_len = 2};
    CF_REQUIRE(write_op(&env, op_create, &created) == CF_OK);
    int64_t room_id = created.room.id;
    cf_room_dispose(&created.room);

    CF_REQUIRE(execf(&env,
                     "INSERT INTO messages (id,client_message_id,created_at,"
                     "creator_id,room_id,updated_at) VALUES "
                     "(555,'cmid','2026-01-01 00:00:00',%lld,%lld,"
                     "'2026-01-01 00:00:00')",
                     (long long)U_KEVIN, (long long)room_id));
    CF_REQUIRE(execf(&env,
                     "INSERT INTO action_text_rich_texts (record_type,"
                     "record_id,name,body,created_at,updated_at) VALUES "
                     "('Message',555,'body','<div>hi</div>',"
                     "'2026-01-01 00:00:00','2026-01-01 00:00:00')"));
    CF_REQUIRE(execf(&env,
                     "INSERT INTO boosts (message_id,booster_id,content,"
                     "created_at,updated_at) VALUES (555,%lld,'+1',"
                     "'2026-01-01 00:00:00','2026-01-01 00:00:00')",
                     (long long)U_DAVID));

    cf_room room = {0};
    CF_REQUIRE(cf_room_find(env.db, room_id, &room) == CF_OK);
    struct destroy_op destroy = {.room = &room};
    CF_REQUIRE(write_op(&env, op_destroy, &destroy) == CF_OK);
    cf_room_dispose(&room);

    int64_t count = -1;
    CF_REQUIRE(raw_i64(&env, &count,
                       "SELECT COUNT(*) FROM memberships WHERE room_id = %lld",
                       (long long)room_id));
    CF_CHECK(count == 0);
    CF_REQUIRE(raw_i64(&env, &count,
                       "SELECT COUNT(*) FROM messages WHERE room_id = %lld",
                       (long long)room_id));
    CF_CHECK(count == 0);
    CF_REQUIRE(raw_i64(&env, &count,
                       "SELECT COUNT(*) FROM action_text_rich_texts WHERE "
                       "record_type='Message' AND record_id=555"));
    CF_CHECK(count == 0);
    CF_REQUIRE(raw_i64(&env, &count,
                       "SELECT COUNT(*) FROM boosts WHERE message_id=555"));
    CF_CHECK(count == 0);
    bool found = true;
    CF_CHECK(cf_room_find_by_id(env.db, room_id, &found, &room) == CF_OK);
    CF_CHECK(!found);

    test_end(&env);
}

CF_TEST(receive_marks_unread_and_emits_push) {
    room_env env;
    CF_REQUIRE(test_begin(&env));

    /* Members who should be marked unread: jason and bender (connected_at
     * NULL).  david is the creator (excluded); add a recently connected
     * member and an invisible member to cover the remaining predicates. */
    CF_REQUIRE(execf(&env,
                     "INSERT INTO users (id,name,role,status,created_at,"
                     "updated_at) VALUES (424242,'Fresh',0,0,"
                     "'2026-01-01 00:00:00','2026-01-01 00:00:00')"));
    CF_REQUIRE(execf(&env,
                     "INSERT INTO memberships (room_id,user_id,involvement,"
                     "connected_at,created_at,updated_at) VALUES "
                     "(%lld,424242,'mentions','2026-01-01 00:00:30',"
                     "'2026-01-01 00:00:00','2026-01-01 00:00:00')",
                     (long long)R_WATERCOOLER));
    CF_REQUIRE(execf(&env,
                     "INSERT INTO users (id,name,role,status,created_at,"
                     "updated_at) VALUES (424243,'Lurker',0,0,"
                     "'2026-01-01 00:00:00','2026-01-01 00:00:00')"));
    CF_REQUIRE(execf(&env,
                     "INSERT INTO memberships (room_id,user_id,involvement,"
                     "created_at,updated_at) VALUES (%lld,424243,'invisible',"
                     "'2026-01-01 00:00:00','2026-01-01 00:00:00')",
                     (long long)R_WATERCOOLER));

    /* The receive instant is T_BASE + 60s, so the connection cutoff is
     * T_BASE and the 00:00:30 connection is still connected. */
    cf_test_clock_set_fixed_us(T_BASE + 60 * T_SEC);
    cf_message message = {.id = 777,
                          .room_id = R_WATERCOOLER,
                          .creator_id = U_DAVID,
                          .client_message_id = {"cmid", 4},
                          .created_at = T_BASE + 59 * T_SEC,
                          .updated_at = T_BASE + 59 * T_SEC};
    struct receive_op receive = {.room_id = R_WATERCOOLER,
                                 .message = message};
    CF_REQUIRE(write_op(&env, op_receive, &receive) == CF_OK);

    int64_t unread = -1;
    CF_REQUIRE(raw_i64(&env, &unread,
                       "SELECT COUNT(*) FROM memberships WHERE room_id = %lld "
                       "AND unread_at = '2026-01-01 00:00:59'",
                       (long long)R_WATERCOOLER));
    CF_CHECK(unread == 2); /* jason, bender */
    CF_REQUIRE(raw_i64(&env, &unread,
                       "SELECT COUNT(*) FROM memberships WHERE room_id = %lld "
                       "AND user_id = %lld AND unread_at IS NULL",
                       (long long)R_WATERCOOLER, (long long)U_DAVID));
    CF_CHECK(unread == 1); /* the creator */
    CF_REQUIRE(raw_i64(&env, &unread,
                       "SELECT COUNT(*) FROM memberships WHERE room_id = %lld "
                       "AND user_id IN (424242,424243) AND unread_at IS NULL",
                       (long long)R_WATERCOOLER));
    CF_CHECK(unread == 2); /* connected + invisible */

    char stamp[64] = {0};
    CF_REQUIRE(raw_text(&env, stamp, sizeof stamp,
                        "SELECT updated_at FROM memberships WHERE room_id = "
                        "%lld AND user_id = %lld",
                        (long long)R_WATERCOOLER, (long long)U_JASON));
    CF_CHECK(strcmp(stamp, "2026-01-01 00:01:00") == 0);

    /* tx.emit_after_commit(Event::PushMessage { room_id, message_id }) */
    CF_REQUIRE(g_log.len == 1);
    CF_CHECK(g_log.items[0].kind == CF_EVENT_PUSH_MESSAGE);
    CF_CHECK(g_log.items[0].room_id == R_WATERCOOLER);
    CF_CHECK(g_log.items[0].message_id == 777);
    CF_CHECK(g_log.items[0].user_id == 0 && g_log.items[0].blob_id == 0 &&
             g_log.items[0].reconnect == false);

    test_end(&env);
}

CF_TEST(invalid_arguments_and_failure_paths) {
    room_env env;
    CF_REQUIRE(test_begin(&env));

    cf_room room = {0};
    bool found = false;
    cf_room_vector rooms = {0};
    cf_user_vector users = {0};
    cf_int64_vector ids = {0};
    cf_membership_vector memberships = {0};

    CF_CHECK(cf_room_find(NULL, 1, &room) == CF_INVALID);
    CF_CHECK(cf_room_find(env.db, 1, NULL) == CF_INVALID);
    CF_CHECK(cf_room_find_by_id(env.db, 1, NULL, &room) == CF_INVALID);
    CF_CHECK(cf_room_find_by_id(env.db, 1, &found, NULL) == CF_INVALID);
    CF_CHECK(cf_room_all(NULL, &rooms) == CF_INVALID);
    CF_CHECK(cf_room_all(env.db, NULL) == CF_INVALID);
    CF_CHECK(cf_room_of_type(env.db, (cf_room_type)9, &rooms) == CF_INVALID);
    int64_t count = 0;
    CF_CHECK(cf_room_count_of_type(env.db, (cf_room_type)9, &count) ==
             CF_INVALID);
    CF_CHECK(count == 0);
    CF_CHECK(cf_room_count_of_type(env.db, CF_ROOM_OPEN, NULL) == CF_INVALID);
    CF_CHECK(cf_room_users(env.db, NULL, &users) == CF_INVALID);
    CF_CHECK(cf_room_user_ids(env.db, NULL, &ids) == CF_INVALID);
    CF_CHECK(cf_room_active_bots(NULL, &room, &users) == CF_INVALID);
    CF_CHECK(cf_room_memberships(env.db, NULL, &memberships) == CF_INVALID);
    CF_CHECK(cf_room_update(NULL, &room, NULL, NULL) == CF_INVALID);
    CF_CHECK(cf_room_touch(NULL, 1) == CF_INVALID);
    CF_CHECK(cf_room_destroy(NULL, &room) == CF_INVALID);
    CF_CHECK(cf_room_grant_to(NULL, &room, NULL, 0) == CF_INVALID);
    CF_CHECK(cf_room_revoke_from(NULL, &room, NULL, 0) == CF_INVALID);
    CF_CHECK(cf_room_create(NULL, CF_ROOM_OPEN, (cf_optional_str){0}, U_DAVID,
                            &room) == CF_INVALID);
    CF_CHECK(cf_room_create_for(NULL, CF_ROOM_OPEN, (cf_optional_str){0},
                                U_DAVID, NULL, 1, &room) == CF_INVALID);
    CF_CHECK(cf_room_find_or_create_direct_for(NULL, NULL, 0, U_DAVID,
                                               &room) == CF_INVALID);
    CF_CHECK(cf_room_find_direct_for(NULL, NULL, 0, &found, &room) ==
             CF_INVALID);
    CF_CHECK(cf_room_receive(NULL, 1, NULL) == CF_INVALID);
    CF_CHECK(!cf_room_open(NULL) && !cf_room_closed(NULL) &&
             !cf_room_direct(NULL));

    /* An unknown room type cannot be written. */
    struct create_op bad = {.type = (cf_room_type)9, .name = "Bad"};
    CF_CHECK(write_op(&env, op_create, &bad) == CF_INVALID);

    /* A NULL room handle is rejected, not dereferenced. */
    cf_room_dispose(NULL);

    /* Reload needs a room handle. */
    CF_CHECK(cf_room_reload(env.db, NULL) == CF_INVALID);

    test_end(&env);
}

CF_TEST(dispose_paths_are_leak_free) {
    room_env env;
    CF_REQUIRE(test_begin(&env));

    cf_room room = {0};
    CF_REQUIRE(cf_room_find(env.db, R_WATERCOOLER, &room) == CF_OK);
    cf_room_dispose(&room); /* releases the owned name and resets */
    CF_CHECK(room.id == 0 && room.name.value.ptr == NULL &&
             room.name.value.len == 0 && !room.name.present);
    cf_room_dispose(&room); /* zero state is a no-op */
    cf_room_dispose(NULL);

    cf_room_vector rooms = {0};
    CF_REQUIRE(cf_room_all(env.db, &rooms) == CF_OK);
    CF_CHECK(rooms.len == 7 && rooms.items != NULL);
    cf_room_vector_dispose(&rooms);
    CF_CHECK(rooms.items == NULL && rooms.len == 0 && rooms.cap == 0);
    cf_room_vector_dispose(&rooms); /* idempotent */
    cf_room_vector_dispose(NULL);

    cf_user_vector users = {0};
    CF_REQUIRE(cf_room_users(env.db, NULL, &users) == CF_INVALID);
    CF_CHECK(users.items == NULL && users.len == 0);

    cf_room empty_room = {.id = 999999};
    cf_int64_vector ids = {0};
    CF_REQUIRE(cf_room_user_ids(env.db, &empty_room, &ids) == CF_OK);
    CF_CHECK(ids.len == 0 && ids.items == NULL);
    cf_int64_vector_dispose(&ids);

    test_end(&env);
}

CF_TEST_MAIN()
