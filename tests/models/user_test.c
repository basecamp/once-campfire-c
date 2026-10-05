/* tests/models/user_test.c — D01 model family "user" tests.
 *
 * Reference oracles (pinned; hashes in reference-files.json):
 *  - tmp/rust-ref/crates/db/src/models/user.rs (the implemented source);
 *  - tests/fixtures/crates/db/src/tests/user_test.rs (the port of
 *    tmp/rails-ref/test/models/user_test.rb, user/bot_test.rb,
 *    user/role_test.rb and Bannable): long-password truncation, membership
 *    grants to open rooms, deactivation cleanup and email scrambling, bot
 *    creation/reset/authentication, deliver_webhook_later, can_administer,
 *    ban from session IPs and remove_banned_content;
 *  - tests/fixtures/vectors/rails_compat.json passwords: pinned bcrypt
 *    digests and expected verify results (including the 72-byte truncation
 *    pair) used for cf_user_authenticate/authenticated.  Verification is
 *    A01's service; this binary links the A01 stand-in noted in
 *    docs/devel/evidence/D01-model-user.md, which must satisfy the same
 *    vectors.
 *
 * The C fixture importer does not exist yet, so rows are seeded with fixed
 * raw SQL (schema.sql columns); timestamps use the reference SQL text form.
 * Reads run on a write-mode scratch connection (D01 db-core); mutations run
 * through the public cf_write path (D02 writer, opaque cf_tx) on a cf_app
 * whose DATABASE_PATH is the same scratch file, with cf_writer_start before
 * any cf_write; committed events are observed through the writer's control
 * and best-effort handlers.
 *
 * Build (plain; add -fsanitize=address,undefined -fno-sanitize-recover=all
 * -fno-omit-frame-pointer -g for the sanitize run; the password stand-in is
 * a local build artifact until A01 lands):
 *   clang -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra
 *         -Werror -pthread -Isrc -Itests
 *         -Ivendor/src/sqlite/sqlite-amalgamation-3530400
 *         tests/models/user_test.c src/models/[all 15 model translation
 *         units] src/db/{schema,reader,statements,writer}.c
 *         src/core/{alloc,buffer,clock,error,random}.c src/config.c
 *         src/app.c vendor/build/sqlite-clang/libsqlite3.a -lm
 *         -o build/d01-model-user/plain/test_user
 */
#include "cf_test.h"

#include "app.h"
#include "config.h"
#include "core/testclock.h"
#include "core/testrandom.h"
#include "db/db_testutil.h"
#include "db/writer.h"
#include "models/membership.h" /* concrete association records */
#include "models/message.h"
#include "models/session.h"
#include "models/user.h"
#include "models/webhook.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USER_TEST_SECRET_HEX \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

/* 2023-11-14 22:13:20.123456 UTC, the pinned reference clock test value. */
#define USER_TEST_NOW_US INT64_C(1700000000123456)
/* Same value in the reference SQL text form. */
#define USER_TEST_NOW_TEXT "2023-11-14 22:13:20.123456"
/* Stored updated_at of every seeded row (no fraction digits):
 * 2024-01-02 03:04:05 UTC. */
#define USER_TEST_SEED_TS_TEXT "2024-01-02 03:04:05"
#define USER_TEST_SEEDED_TS INT64_C(1704164645000000)

/* Pinned rails_compat.json password vectors. */
#define USER_TEST_PASSWORD "secret123456"
#define USER_TEST_DIGEST \
    "$2a$12$I/2qP3ixlNN..JU2dXSN0umG7/Folk3aUaqgSHn/Rl0G82O9WYk66"
#define USER_TEST_LONG_72 \
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define USER_TEST_LONG_71 \
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define USER_TEST_LONG_DIGEST \
    "$2a$12$7ymzJw79L3lWy0kZKREoMOyowLTl015hCs3J1dIPjo8LLPxayN5f6"

static cf_str S(const char *text) {
    return (cf_str){(char *)text, strlen(text)};
}

static bool str_is(cf_str value, const char *want) {
    size_t want_len = strlen(want);
    return value.ptr != NULL && value.len == want_len &&
           memcmp(value.ptr, want, want_len) == 0 &&
           value.ptr[value.len] == '\0';
}

static bool optional_str_is(cf_optional_str value, const char *want) {
    return value.present && str_is(value.value, want);
}

/* --- raw seeding ---------------------------------------------------------- */

static int raw_exec(cf_db *db, const char *sql) {
    return sqlite3_exec(cf_db_handle(db), sql, NULL, NULL, NULL);
}

static void seed_user(cf_db *db, int64_t id, const char *name,
                      const char *email, const char *digest, int role,
                      int status, const char *bio, const char *bot_token) {
    sqlite3 *handle = cf_db_handle(db);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(
                   handle,
                   "INSERT INTO users (id, name, email_address, "
                   "password_digest, role, status, bio, bot_token, created_at, "
                   "updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                   -1, &stmt, NULL) == SQLITE_OK);
    CF_REQUIRE(sqlite3_bind_int64(stmt, 1, id) == SQLITE_OK);
    CF_REQUIRE(sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT) ==
               SQLITE_OK);
    if (email != NULL) {
        CF_REQUIRE(sqlite3_bind_text(stmt, 3, email, -1, SQLITE_TRANSIENT) ==
                   SQLITE_OK);
    } else {
        CF_REQUIRE(sqlite3_bind_null(stmt, 3) == SQLITE_OK);
    }
    if (digest != NULL) {
        CF_REQUIRE(sqlite3_bind_text(stmt, 4, digest, -1, SQLITE_TRANSIENT) ==
                   SQLITE_OK);
    } else {
        CF_REQUIRE(sqlite3_bind_null(stmt, 4) == SQLITE_OK);
    }
    CF_REQUIRE(sqlite3_bind_int64(stmt, 5, role) == SQLITE_OK);
    CF_REQUIRE(sqlite3_bind_int64(stmt, 6, status) == SQLITE_OK);
    if (bio != NULL) {
        CF_REQUIRE(sqlite3_bind_text(stmt, 7, bio, -1, SQLITE_TRANSIENT) ==
                   SQLITE_OK);
    } else {
        CF_REQUIRE(sqlite3_bind_null(stmt, 7) == SQLITE_OK);
    }
    if (bot_token != NULL) {
        CF_REQUIRE(sqlite3_bind_text(stmt, 8, bot_token, -1, SQLITE_TRANSIENT) ==
                   SQLITE_OK);
    } else {
        CF_REQUIRE(sqlite3_bind_null(stmt, 8) == SQLITE_OK);
    }
    CF_REQUIRE(sqlite3_bind_text(stmt, 9, USER_TEST_NOW_TEXT, -1,
                                 SQLITE_TRANSIENT) == SQLITE_OK);
    CF_REQUIRE(sqlite3_bind_text(stmt, 10, USER_TEST_SEED_TS_TEXT, -1,
                                 SQLITE_TRANSIENT) == SQLITE_OK);
    CF_REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
}

static void seed_room(cf_db *db, int64_t id, const char *type,
                      const char *name) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO rooms (id, type, name, creator_id, created_at, "
             "updated_at) VALUES (%lld, '%s', %s, 1, '%s', '%s')",
             (long long)id, type,
             name != NULL ? "'seed-room'" : "NULL", USER_TEST_NOW_TEXT,
             USER_TEST_NOW_TEXT);
    CF_REQUIRE(raw_exec(db, sql) == SQLITE_OK);
}

static void seed_membership(cf_db *db, int64_t room_id, int64_t user_id,
                            const char *involvement) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO memberships (room_id, user_id, involvement, "
             "created_at, updated_at) VALUES (%lld, %lld, %s, '%s', '%s')",
             (long long)room_id, (long long)user_id,
             involvement != NULL ? "'mentions'" : "NULL", USER_TEST_NOW_TEXT,
             USER_TEST_NOW_TEXT);
    CF_REQUIRE(raw_exec(db, sql) == SQLITE_OK);
}

static void seed_session(cf_db *db, int64_t id, int64_t user_id,
                         const char *ip) {
    char sql[512];
    if (ip != NULL) {
        snprintf(sql, sizeof sql,
                 "INSERT INTO sessions (id, user_id, token, ip_address, "
                 "last_active_at, created_at, updated_at) VALUES (%lld, %lld, "
                 "'token-%lld', '%s', '%s', '%s', '%s')",
                 (long long)id, (long long)user_id, (long long)id, ip,
                 USER_TEST_NOW_TEXT, USER_TEST_NOW_TEXT, USER_TEST_NOW_TEXT);
    } else {
        snprintf(sql, sizeof sql,
                 "INSERT INTO sessions (id, user_id, token, ip_address, "
                 "last_active_at, created_at, updated_at) VALUES (%lld, %lld, "
                 "'token-%lld', NULL, '%s', '%s', '%s')",
                 (long long)id, (long long)user_id, (long long)id,
                 USER_TEST_NOW_TEXT, USER_TEST_NOW_TEXT, USER_TEST_NOW_TEXT);
    }
    CF_REQUIRE(raw_exec(db, sql) == SQLITE_OK);
}

static void seed_search(cf_db *db, int64_t user_id, const char *query_text) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO searches (user_id, query, created_at, updated_at) "
             "VALUES (%lld, '%s', '%s', '%s')",
             (long long)user_id, query_text, USER_TEST_NOW_TEXT,
             USER_TEST_NOW_TEXT);
    CF_REQUIRE(raw_exec(db, sql) == SQLITE_OK);
}

static void seed_push_subscription(cf_db *db, int64_t user_id) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO push_subscriptions (user_id, endpoint, created_at, "
             "updated_at) VALUES (%lld, 'https://push.example/%lld', '%s', "
             "'%s')",
             (long long)user_id, (long long)user_id, USER_TEST_NOW_TEXT,
             USER_TEST_NOW_TEXT);
    CF_REQUIRE(raw_exec(db, sql) == SQLITE_OK);
}

static void seed_message(cf_db *db, int64_t id, int64_t room_id,
                         int64_t creator_id) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "INSERT INTO messages (id, room_id, creator_id, "
             "client_message_id, created_at, updated_at) VALUES (%lld, %lld, "
             "%lld, 'cmid-%lld', '%s', '%s')",
             (long long)id, (long long)room_id, (long long)creator_id,
             (long long)id, USER_TEST_NOW_TEXT, USER_TEST_NOW_TEXT);
    CF_REQUIRE(raw_exec(db, sql) == SQLITE_OK);
}

static void seed_webhook(cf_db *db, int64_t user_id, const char *url) {
    sqlite3 *handle = cf_db_handle(db);
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(
                   handle,
                   "INSERT INTO webhooks (user_id, url, created_at, updated_at) "
                   "VALUES (?, ?, ?, ?)",
                   -1, &stmt, NULL) == SQLITE_OK);
    CF_REQUIRE(sqlite3_bind_int64(stmt, 1, user_id) == SQLITE_OK);
    if (url != NULL) {
        CF_REQUIRE(sqlite3_bind_text(stmt, 2, url, -1, SQLITE_TRANSIENT) ==
                   SQLITE_OK);
    } else {
        CF_REQUIRE(sqlite3_bind_null(stmt, 2) == SQLITE_OK);
    }
    CF_REQUIRE(sqlite3_bind_text(stmt, 3, USER_TEST_NOW_TEXT, -1,
                                 SQLITE_TRANSIENT) == SQLITE_OK);
    CF_REQUIRE(sqlite3_bind_text(stmt, 4, USER_TEST_NOW_TEXT, -1,
                                 SQLITE_TRANSIENT) == SQLITE_OK);
    CF_REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
}

/* --- scratch app + writer environment ------------------------------------ */

#define USER_TEST_MAX_EVENTS 32

typedef struct {
    cf_event items[USER_TEST_MAX_EVENTS];
    size_t len;
} user_event_log;

static cf_err capture_event(void *ctx, const cf_event *event) {
    user_event_log *log = ctx;
    if (log->len < USER_TEST_MAX_EVENTS) {
        log->items[log->len++] = *event;
    }
    return CF_OK;
}

typedef struct {
    cf_db_scratch scratch;
    cf_app *app;
    user_event_log events;
} user_env;

static bool user_env_open(user_env *env) {
    memset(env, 0, sizeof *env);
    if (!cf_db_scratch_open(&env->scratch)) return false;

    cf_config_entry entries[] = {
        {"PUBLIC_ORIGIN", "http://127.0.0.1:32123"},
        {"SECRET_KEY_BASE", USER_TEST_SECRET_HEX},
        {"DATABASE_PATH", env->scratch.path},
    };
    cf_config *config = NULL;
    if (cf_config_parse(entries, sizeof entries / sizeof entries[0], NULL,
                        &config) != CF_OK) {
        return false;
    }
    if (cf_app_create(config, &env->app) != CF_OK) {
        cf_config_destroy(config);
        return false;
    }
    if (cf_writer_start(env->app, config) != CF_OK) {
        cf_app_destroy(env->app);
        env->app = NULL;
        return false;
    }
    /* DISCONNECT_USER is mandatory (D02); the others best-effort.  One log
     * keeps delivery order across kinds for the assertions below. */
    if (cf_writer_set_control_handler(env->app, capture_event,
                                      &env->events) != CF_OK) {
        return false;
    }
    static const cf_event_kind best_effort[] = {
        CF_EVENT_PUSH_MESSAGE, CF_EVENT_REMOVE_BANNED_CONTENT,
        CF_EVENT_DELIVER_WEBHOOK, CF_EVENT_PURGE_BLOB};
    for (size_t i = 0; i < sizeof best_effort / sizeof best_effort[0]; i++) {
        if (cf_writer_set_event_handler(env->app, best_effort[i], capture_event,
                                        &env->events) != CF_OK) {
            return false;
        }
    }
    return true;
}

static void user_env_close(user_env *env) {
    cf_writer_stop(env->app);
    cf_app_destroy(env->app);
    env->app = NULL;
    cf_db_scratch_close(&env->scratch);
}

static void expect_event(const user_env *env, size_t index,
                         cf_event_kind kind, int64_t user_id, int64_t room_id,
                         int64_t message_id, bool reconnect) {
    CF_REQUIRE(index < env->events.len);
    const cf_event *event = &env->events.items[index];
    CF_CHECK(event->kind == kind);
    CF_CHECK(event->user_id == user_id);
    CF_CHECK(event->room_id == room_id);
    CF_CHECK(event->message_id == message_id);
    CF_CHECK(event->blob_id == 0);
    CF_CHECK(event->reconnect == reconnect);
}

/* --- pure helpers --------------------------------------------------------- */

CF_TEST(role_and_status_helpers) {
    CF_CHECK(strcmp(cf_role_name(CF_ROLE_MEMBER), "member") == 0);
    CF_CHECK(strcmp(cf_role_name(CF_ROLE_ADMINISTRATOR), "administrator") ==
             0);
    CF_CHECK(strcmp(cf_role_name(CF_ROLE_BOT), "bot") == 0);
    CF_CHECK(cf_role_name((cf_role)7) == NULL);

    cf_role role = (cf_role)99;
    CF_CHECK(cf_role_from_name(S("administrator"), &role));
    CF_CHECK(role == CF_ROLE_ADMINISTRATOR);
    CF_CHECK(cf_role_from_name(S("bot"), &role));
    CF_CHECK(role == CF_ROLE_BOT);
    CF_CHECK(cf_role_from_name(S("member"), &role));
    CF_CHECK(role == CF_ROLE_MEMBER);
    /* The source has no "admin" alias. */
    CF_CHECK(!cf_role_from_name(S("admin"), &role));
    CF_CHECK(role == CF_ROLE_MEMBER); /* unchanged on false */
    CF_CHECK(!cf_role_from_name(S(""), &role));
    CF_CHECK(!cf_role_from_name((cf_str){NULL, 3}, &role));
    CF_CHECK(!cf_role_from_name(S("member"), NULL));
    /* Borrowed span without NUL matches by length and bytes. */
    CF_CHECK(cf_role_from_name((cf_str){(char *)"botX", 3}, &role));
    CF_CHECK(role == CF_ROLE_BOT);

    CF_CHECK(strcmp(cf_status_name(CF_STATUS_ACTIVE), "active") == 0);
    CF_CHECK(strcmp(cf_status_name(CF_STATUS_DEACTIVATED), "deactivated") ==
             0);
    CF_CHECK(strcmp(cf_status_name(CF_STATUS_BANNED), "banned") == 0);
    CF_CHECK(cf_status_name((cf_status)9) == NULL);

    cf_status status = (cf_status)99;
    CF_CHECK(cf_status_from_name(S("deactivated"), &status));
    CF_CHECK(status == CF_STATUS_DEACTIVATED);
    CF_CHECK(cf_status_from_name(S("banned"), &status));
    CF_CHECK(status == CF_STATUS_BANNED);
    CF_CHECK(cf_status_from_name(S("active"), &status));
    CF_CHECK(status == CF_STATUS_ACTIVE);
    CF_CHECK(!cf_status_from_name(S("deactive"), &status));
    CF_CHECK(status == CF_STATUS_ACTIVE);
    CF_CHECK(!cf_status_from_name(S(""), &status));
    CF_CHECK(!cf_status_from_name(S("active"), NULL));
}

CF_TEST(initials_title_bot_key_attachable) {
    cf_user user = {0};
    user.name = S("JZ");
    user.bio = (cf_optional_str){.present = true, .value = S("Designer")};
    user.bot_token = (cf_optional_str){.present = true, .value = S("tok")};

    cf_str out = {0};
    CF_REQUIRE(cf_user_initials(&user, &out) == CF_OK);
    CF_CHECK(str_is(out, "J"));
    free(out.ptr);

    CF_REQUIRE(cf_user_title(&user, &out) == CF_OK);
    CF_CHECK(str_is(out, "JZ \xe2\x80\x93 Designer")); /* " – " */
    free(out.ptr);

    user.id = 42;
    CF_REQUIRE(cf_user_bot_key(&user, &out) == CF_OK);
    CF_CHECK(str_is(out, "42-tok"));
    free(out.ptr);

    CF_REQUIRE(cf_user_attachable_plain_text_representation(&user, &out) ==
               CF_OK);
    CF_CHECK(str_is(out, "@JZ"));
    free(out.ptr);

    /* Ruby's \b sees É as a word character, \w doesn't: only "Z". */
    cf_str name = S("\xc3\x89mile Zola");
    cf_user emile = {0};
    emile.name = name;
    CF_REQUIRE(cf_user_initials(&emile, &out) == CF_OK);
    CF_CHECK(str_is(out, "Z"));
    free(out.ptr);

    /* Bender Bot -> BB; underscore is a word character and blocks boundaaries */
    cf_user bender = {0};
    bender.name = S("Bender Bot");
    CF_REQUIRE(cf_user_initials(&bender, &out) == CF_OK);
    CF_CHECK(str_is(out, "BB"));
    free(out.ptr);
    bender.name = S("a_b c");
    CF_REQUIRE(cf_user_initials(&bender, &out) == CF_OK);
    CF_CHECK(str_is(out, "ac"));
    free(out.ptr);

    /* compact_blank drops a whitespace-only bio and a whitespace-only name. */
    cf_user titled = {0};
    titled.name = S("\xc3\x89mile Zola");
    titled.bio = (cf_optional_str){.present = true, .value = S("  ")};
    CF_REQUIRE(cf_user_title(&titled, &out) == CF_OK);
    CF_CHECK(str_is(out, "\xc3\x89mile Zola"));
    free(out.ptr);
    titled.name = S(" \t ");
    titled.bio = (cf_optional_str){.present = true, .value = S("Bio")};
    CF_REQUIRE(cf_user_title(&titled, &out) == CF_OK);
    CF_CHECK(str_is(out, "Bio"));
    free(out.ptr);
    titled.bio = (cf_optional_str){.present = true, .value = S("\xc2\xa0")};
    CF_REQUIRE(cf_user_title(&titled, &out) == CF_OK); /* NBSP is White_Space */
    CF_CHECK(out.len == 0 && out.ptr != NULL);
    free(out.ptr);

    /* No bot_token: the Rust unwrap_or("") yields "id-". */
    cf_user plain = {0};
    plain.id = 7;
    plain.name = S("N");
    CF_REQUIRE(cf_user_bot_key(&plain, &out) == CF_OK);
    CF_CHECK(str_is(out, "7-"));
    free(out.ptr);
    CF_REQUIRE(cf_user_attachable_plain_text_representation(&plain, &out) ==
               CF_OK);
    CF_CHECK(str_is(out, "@N"));
    free(out.ptr);

    /* Invalid arguments leave the output empty. */
    out = (cf_str){NULL, 0};
    CF_CHECK(cf_user_initials(NULL, &out) == CF_INVALID);
    CF_CHECK(out.ptr == NULL && out.len == 0);
    CF_CHECK(cf_user_initials(&user, NULL) == CF_INVALID);
    CF_CHECK(cf_user_title(NULL, &out) == CF_INVALID);
    CF_CHECK(cf_user_bot_key(NULL, &out) == CF_INVALID);
    CF_CHECK(cf_user_attachable_plain_text_representation(NULL, &out) ==
              CF_INVALID);
    CF_CHECK(out.ptr == NULL && out.len == 0);
}

CF_TEST(predicates_and_can_administer) {
    cf_user user = {0};
    CF_CHECK(!cf_user_is_member(NULL));
    CF_CHECK(!cf_user_is_administrator(NULL));
    CF_CHECK(!cf_user_is_bot(NULL));
    CF_CHECK(!cf_user_is_active(NULL));
    CF_CHECK(!cf_user_is_deactivated(NULL));
    CF_CHECK(!cf_user_is_banned(NULL));

    user.id = 5;
    user.role = CF_ROLE_MEMBER;
    user.status = CF_STATUS_ACTIVE;
    CF_CHECK(cf_user_is_member(&user));
    CF_CHECK(!cf_user_is_administrator(&user));
    CF_CHECK(!cf_user_is_bot(&user));
    CF_CHECK(cf_user_is_active(&user));
    CF_CHECK(!cf_user_is_deactivated(&user));
    CF_CHECK(!cf_user_is_banned(&user));

    /* can_administer?: administrator, the record's creator, or a new record. */
    CF_CHECK(!cf_user_can_administer(&user, (cf_optional_i64){false, 0},
                                     false));
    CF_CHECK(cf_user_can_administer(&user, (cf_optional_i64){true, 5}, false));
    CF_CHECK(cf_user_can_administer(&user, (cf_optional_i64){true, 6}, true));
    CF_CHECK(!cf_user_can_administer(&user, (cf_optional_i64){true, 6},
                                     false));
    CF_CHECK(cf_user_can_administer(&user, (cf_optional_i64){true, 5}, false));
    CF_CHECK(!cf_user_can_administer(NULL, (cf_optional_i64){true, 5}, false));

    user.role = CF_ROLE_ADMINISTRATOR;
    CF_CHECK(cf_user_can_administer(&user, (cf_optional_i64){false, 0},
                                    false));
    user.role = CF_ROLE_BOT;
    CF_CHECK(cf_user_is_bot(&user));
    user.status = CF_STATUS_DEACTIVATED;
    CF_CHECK(cf_user_is_deactivated(&user));
    user.status = CF_STATUS_BANNED;
    CF_CHECK(cf_user_is_banned(&user));
    CF_CHECK(!cf_user_is_active(&user));
}

CF_TEST(generate_bot_token_deterministic_and_failure) {
    cf_str token = {0};
    cf_test_random_fill(0xA5); /* 165 % 62 = 41 -> 'p' */
    CF_REQUIRE(cf_user_generate_bot_token(&token) == CF_OK);
    CF_CHECK(str_is(token, "pppppppppppp"));
    free(token.ptr);

    token = (cf_str){NULL, 0};
    cf_test_random_fill(0x00); /* 'A' */
    CF_REQUIRE(cf_user_generate_bot_token(&token) == CF_OK);
    CF_CHECK(str_is(token, "AAAAAAAAAAAA"));
    free(token.ptr);

    /* 0xF8 = 248 is the rejection threshold; a stuck source fails instead of
     * spinning forever. */
    token = (cf_str){NULL, 0};
    cf_test_random_fill(0xF8);
    CF_CHECK(cf_user_generate_bot_token(&token) == CF_INTERNAL);
    CF_CHECK(token.ptr == NULL && token.len == 0);
    cf_test_random_clear();

    /* Entropy failure is CF_IO and leaves the output empty. */
    cf_test_random_fail();
    CF_CHECK(cf_user_generate_bot_token(&token) == CF_IO);
    CF_CHECK(token.ptr == NULL && token.len == 0);
    cf_test_random_clear();

    /* Real entropy: length 12 over the SecureRandom.alphanumeric alphabet. */
    CF_REQUIRE(cf_user_generate_bot_token(&token) == CF_OK);
    CF_CHECK(token.len == 12 && token.ptr != NULL && token.ptr[12] == '\0');
    for (size_t i = 0; i < token.len; i++) {
        char c = token.ptr[i];
        CF_CHECK((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9'));
    }
    free(token.ptr);

    CF_CHECK(cf_user_generate_bot_token(NULL) == CF_INVALID);
}

CF_TEST(authenticate_pinned_vectors) {
    cf_user user = {0};
    user.password_digest =
        (cf_optional_str){.present = true, .value = S(USER_TEST_DIGEST)};

    bool matched = true;
    CF_CHECK(cf_user_authenticate(&user, S(USER_TEST_PASSWORD), &matched) ==
              CF_OK);
    CF_CHECK(matched);
    CF_CHECK(cf_user_authenticate(&user, S("wrong"), &matched) == CF_OK);
    CF_CHECK(!matched);
    CF_CHECK(cf_user_authenticate(&user, S(USER_TEST_PASSWORD " "),
                                  &matched) == CF_OK);
    CF_CHECK(!matched);

    /* bcrypt counts the first 72 bytes (rails_compat vectors). */
    user.password_digest =
        (cf_optional_str){.present = true, .value = S(USER_TEST_LONG_DIGEST)};
    CF_CHECK(cf_user_authenticate(&user, S(USER_TEST_LONG_72), &matched) ==
              CF_OK);
    CF_CHECK(matched);
    CF_CHECK(cf_user_authenticate(&user, S(USER_TEST_LONG_71), &matched) ==
              CF_OK);
    CF_CHECK(!matched);

    /* An absent or empty digest never verifies, even for an empty password. */
    user.password_digest = (cf_optional_str){false, {NULL, 0}};
    CF_CHECK(cf_user_authenticate(&user, S(USER_TEST_PASSWORD), &matched) ==
              CF_OK);
    CF_CHECK(!matched);
    user.password_digest = (cf_optional_str){.present = true,
                                             .value = {NULL, 0}};
    CF_CHECK(cf_user_authenticate(&user, S(USER_TEST_PASSWORD), &matched) ==
              CF_OK);
    CF_CHECK(!matched);

    CF_CHECK(cf_user_authenticate(NULL, S("x"), &matched) == CF_INVALID);
    CF_CHECK(cf_user_authenticate(&user, (cf_str){NULL, 3}, &matched) ==
              CF_INVALID);
    CF_CHECK(cf_user_authenticate(&user, S("x"), NULL) == CF_INVALID);
}

CF_TEST(authenticated_candidate_and_dummy) {
    cf_user match = {0};
    match.id = 3;
    match.password_digest =
        (cf_optional_str){.present = true, .value = S(USER_TEST_DIGEST)};
    cf_user other = {0};
    other.id = 4;
    other.password_digest =
        (cf_optional_str){.present = true, .value = S(USER_TEST_LONG_DIGEST)};

    bool ok = true;
    /* A blank password returns before the lookup, even with a candidate. */
    CF_CHECK(cf_user_authenticated(&match, S(""), &ok) == CF_OK);
    CF_CHECK(!ok);
    /* A missing candidate still runs the dummy verification (returns false). */
    CF_CHECK(cf_user_authenticated(NULL, S(USER_TEST_PASSWORD), &ok) == CF_OK);
    CF_CHECK(!ok);
    CF_CHECK(cf_user_authenticated(NULL, S(""), &ok) == CF_OK);
    CF_CHECK(!ok);
    /* Candidate verification. */
    CF_CHECK(cf_user_authenticated(&match, S(USER_TEST_PASSWORD), &ok) ==
              CF_OK);
    CF_CHECK(ok);
    CF_CHECK(cf_user_authenticated(&match, S("wrong"), &ok) == CF_OK);
    CF_CHECK(!ok);
    CF_CHECK(cf_user_authenticated(&other, S(USER_TEST_LONG_72), &ok) == CF_OK);
    CF_CHECK(ok);
    /* No digest: false without any verification. */
    cf_user digestless = {0};
    digestless.id = 5;
    CF_CHECK(cf_user_authenticated(&digestless, S(USER_TEST_PASSWORD), &ok) ==
              CF_OK);
    CF_CHECK(!ok);

    CF_CHECK(cf_user_authenticated(NULL, S(USER_TEST_PASSWORD), NULL) ==
              CF_INVALID);
    CF_CHECK(cf_user_authenticated(NULL, (cf_str){NULL, 2}, &ok) == CF_INVALID);
}

/* --- reads on a seeded scratch database ----------------------------------- */

CF_TEST(find_by_id_and_find_found_notfound) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));

    cf_user missing = {0};
    bool found = true;
    CF_CHECK(cf_user_find_by_id(scratch.db, 1, &found, &missing) == CF_OK);
    CF_CHECK(!found);
    CF_CHECK(missing.id == 0 && missing.name.ptr == NULL);
    CF_CHECK(cf_user_find(scratch.db, 1, &missing) == CF_NOT_FOUND);
    CF_CHECK(missing.name.ptr == NULL);

    seed_user(scratch.db, 1, "JZ", "jz@example.com", USER_TEST_DIGEST,
              CF_ROLE_ADMINISTRATOR, CF_STATUS_ACTIVE, "Designer", "tok");
    CF_REQUIRE(cf_user_find_by_id(scratch.db, 1, &found, &missing) == CF_OK);
    CF_REQUIRE(found);
    CF_CHECK(missing.id == 1);
    CF_CHECK(str_is(missing.name, "JZ"));
    CF_CHECK(optional_str_is(missing.email_address, "jz@example.com"));
    CF_CHECK(optional_str_is(missing.password_digest, USER_TEST_DIGEST));
    CF_CHECK(missing.role == CF_ROLE_ADMINISTRATOR);
    CF_CHECK(missing.status == CF_STATUS_ACTIVE);
    CF_CHECK(optional_str_is(missing.bio, "Designer"));
    CF_CHECK(optional_str_is(missing.bot_token, "tok"));
    CF_CHECK(missing.created_at == USER_TEST_NOW_US);
    CF_CHECK(missing.updated_at == USER_TEST_SEEDED_TS);
    cf_user_dispose(&missing);
    CF_CHECK(missing.name.ptr == NULL && missing.id == 0);

    CF_REQUIRE(cf_user_find(scratch.db, 1, &missing) == CF_OK);
    CF_CHECK(missing.id == 1);
    cf_user_dispose(&missing);

    /* Out-of-range stored enums are an error, not a silent cast. */
    CF_REQUIRE(raw_exec(scratch.db, "UPDATE users SET role = 9 WHERE id = 1") ==
               SQLITE_OK);
    CF_CHECK(cf_user_find(scratch.db, 1, &missing) == CF_INVALID);
    CF_CHECK(missing.name.ptr == NULL);
    CF_REQUIRE(raw_exec(scratch.db,
                        "UPDATE users SET role = 0, status = 7 WHERE id = 1") ==
               SQLITE_OK);
    CF_CHECK(cf_user_find(scratch.db, 1, &missing) == CF_INVALID);
    CF_REQUIRE(raw_exec(scratch.db,
                        "UPDATE users SET status = 0 WHERE id = 1") ==
               SQLITE_OK);

    /* NULL arguments. */
    CF_CHECK(cf_user_find_by_id(scratch.db, 1, NULL, &missing) == CF_INVALID);
    CF_CHECK(cf_user_find_by_id(scratch.db, 1, &found, NULL) == CF_INVALID);
    CF_CHECK(cf_user_find(NULL, 1, &missing) == CF_INVALID);
    CF_CHECK(cf_user_find(scratch.db, 1, NULL) == CF_INVALID);

    cf_db_scratch_close(&scratch);
}

CF_TEST(find_active_status_gate) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_user(scratch.db, 1, "Active", NULL, NULL, CF_ROLE_MEMBER,
              CF_STATUS_ACTIVE, NULL, NULL);
    seed_user(scratch.db, 2, "Gone", NULL, NULL, CF_ROLE_MEMBER,
              CF_STATUS_DEACTIVATED, NULL, NULL);
    seed_user(scratch.db, 3, "Banned", NULL, NULL, CF_ROLE_MEMBER,
              CF_STATUS_BANNED, NULL, NULL);

    cf_user user = {0};
    CF_REQUIRE(cf_user_find_active(scratch.db, 1, &user) == CF_OK);
    CF_CHECK(user.id == 1 && user.status == CF_STATUS_ACTIVE);
    CF_CHECK(user.email_address.present == false); /* NULL column */
    CF_CHECK(user.bio.present == false);
    cf_user_dispose(&user);

    CF_CHECK(cf_user_find_active(scratch.db, 2, &user) == CF_NOT_FOUND);
    CF_CHECK(user.name.ptr == NULL);
    CF_CHECK(cf_user_find_active(scratch.db, 3, &user) == CF_NOT_FOUND);
    CF_CHECK(cf_user_find_active(scratch.db, 99, &user) == CF_NOT_FOUND);
    CF_CHECK(cf_user_find_active(NULL, 1, &user) == CF_INVALID);
    CF_CHECK(cf_user_find_active(scratch.db, 1, NULL) == CF_INVALID);

    cf_db_scratch_close(&scratch);
}

CF_TEST(find_by_email_exactness) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_user(scratch.db, 1, "JZ", "jz@example.com", NULL, CF_ROLE_MEMBER,
              CF_STATUS_ACTIVE, NULL, NULL);
    seed_user(scratch.db, 2, "Empty", NULL, NULL, CF_ROLE_MEMBER,
              CF_STATUS_ACTIVE, NULL, NULL);
    seed_user(scratch.db, 3, "Off", "off@example.com", NULL, CF_ROLE_MEMBER,
              CF_STATUS_DEACTIVATED, NULL, NULL);

    cf_user user = {0};
    bool found = false;
    CF_REQUIRE(cf_user_find_by_email_address(scratch.db, S("jz@example.com"),
                                             &found, &user) == CF_OK);
    CF_REQUIRE(found);
    CF_CHECK(user.id == 1);
    cf_user_dispose(&user);

    /* Exact match: no normalization, case or whitespace folding. */
    CF_CHECK(cf_user_find_by_email_address(scratch.db, S("JZ@example.com"),
                                           &found, &user) == CF_OK);
    CF_CHECK(!found);
    CF_CHECK(cf_user_find_by_email_address(scratch.db, S("jz@example.com "),
                                           &found, &user) == CF_OK);
    CF_CHECK(!found);
    CF_CHECK(cf_user_find_by_email_address(scratch.db, S("nobody@example.com"),
                                           &found, &user) == CF_OK);
    CF_CHECK(!found);

    /* find_active_by_email_address gates on status. */
    CF_CHECK(cf_user_find_active_by_email_address(
                 scratch.db, S("off@example.com"), &found, &user) == CF_OK);
    CF_CHECK(!found);
    CF_REQUIRE(cf_user_find_active_by_email_address(
                   scratch.db, S("jz@example.com"), &found, &user) == CF_OK);
    CF_CHECK(found && user.id == 1);
    cf_user_dispose(&user);

    CF_CHECK(cf_user_find_by_email_address(NULL, S("x"), &found, &user) ==
              CF_INVALID);
    CF_CHECK(cf_user_find_by_email_address(scratch.db, S("x"), NULL, &user) ==
              CF_INVALID);
    CF_CHECK(cf_user_find_active_by_email_address(scratch.db,
                                                  (cf_str){NULL, 3}, &found,
                                                  &user) == CF_INVALID);

    cf_db_scratch_close(&scratch);
}

CF_TEST(all_and_count) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));

    cf_user_vector users;
    int64_t count = -1;
    CF_REQUIRE(cf_user_count(scratch.db, &count) == CF_OK);
    CF_CHECK(count == 0);
    CF_REQUIRE(cf_user_all(scratch.db, &users) == CF_OK);
    CF_CHECK(users.items == NULL && users.len == 0);
    cf_user_vector_dispose(&users);

    seed_user(scratch.db, 1, "One", NULL, NULL, CF_ROLE_MEMBER,
              CF_STATUS_ACTIVE, NULL, NULL);
    seed_user(scratch.db, 2, "Two", NULL, NULL, CF_ROLE_BOT,
              CF_STATUS_DEACTIVATED, NULL, NULL);
    CF_REQUIRE(cf_user_count(scratch.db, &count) == CF_OK);
    CF_CHECK(count == 2);
    CF_REQUIRE(cf_user_all(scratch.db, &users) == CF_OK);
    CF_REQUIRE(users.len == 2);
    CF_CHECK(users.items[0].id == 1 && users.items[1].id == 2);
    CF_CHECK(users.cap >= users.len);
    /* Rows are copied: values survive the statement reset. */
    CF_CHECK(str_is(users.items[0].name, "One"));
    cf_user_vector_dispose(&users);
    CF_CHECK(users.items == NULL && users.len == 0 && users.cap == 0);
    cf_user_vector_dispose(&users); /* zero state is a no-op */

    CF_CHECK(cf_user_count(scratch.db, NULL) == CF_INVALID);
    CF_CHECK(cf_user_count(NULL, &count) == CF_INVALID);
    CF_CHECK(cf_user_all(NULL, &users) == CF_INVALID);
    CF_CHECK(cf_user_all(scratch.db, NULL) == CF_INVALID);

    cf_db_scratch_close(&scratch);
}

CF_TEST(where_ids_set_and_order) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    for (int64_t id = 1; id <= 5; id++) {
        char name[16];
        snprintf(name, sizeof name, "U%lld", (long long)id);
        seed_user(scratch.db, id, name, NULL, NULL, CF_ROLE_MEMBER,
                  CF_STATUS_ACTIVE, NULL, NULL);
    }

    cf_user_vector users;
    const int64_t ids[] = {4, 2, 2};
    CF_REQUIRE(cf_user_where_ids(scratch.db, ids, 3, &users) == CF_OK);
    /* IN set semantics in users rowid (id) order, duplicates once. */
    CF_REQUIRE(users.len == 2);
    CF_CHECK(users.items[0].id == 2 && users.items[1].id == 4);
    cf_user_vector_dispose(&users);

    const int64_t unknown[] = {99, 3};
    CF_REQUIRE(cf_user_where_ids(scratch.db, unknown, 2, &users) == CF_OK);
    CF_REQUIRE(users.len == 1);
    CF_CHECK(users.items[0].id == 3);
    cf_user_vector_dispose(&users);

    /* The reference's runtime-built IN () matches nothing. */
    CF_REQUIRE(cf_user_where_ids(scratch.db, NULL, 0, &users) == CF_OK);
    CF_CHECK(users.items == NULL && users.len == 0);
    cf_user_vector_dispose(&users);

    const int64_t big[] = {INT64_C(9007199254740993), 1};
    CF_REQUIRE(cf_user_where_ids(scratch.db, big, 2, &users) == CF_OK);
    CF_REQUIRE(users.len == 1);
    CF_CHECK(users.items[0].id == 1);
    cf_user_vector_dispose(&users);

    CF_CHECK(cf_user_where_ids(scratch.db, NULL, 2, &users) == CF_INVALID);
    CF_CHECK(cf_user_where_ids(NULL, ids, 3, &users) == CF_INVALID);
    CF_CHECK(cf_user_where_ids(scratch.db, ids, 3, NULL) == CF_INVALID);

    cf_db_scratch_close(&scratch);
}

CF_TEST(active_scopes_ordering_and_filters) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_user(scratch.db, 1, "Bender", "b@example.com", NULL, CF_ROLE_BOT,
              CF_STATUS_ACTIVE, NULL, "tok-b");
    seed_user(scratch.db, 2, "amy", NULL, NULL, CF_ROLE_MEMBER,
              CF_STATUS_ACTIVE, NULL, NULL);
    seed_user(scratch.db, 3, "bender", NULL, NULL, CF_ROLE_MEMBER,
              CF_STATUS_ACTIVE, NULL, NULL);
    seed_user(scratch.db, 4, "Zed", NULL, NULL, CF_ROLE_ADMINISTRATOR,
              CF_STATUS_DEACTIVATED, NULL, NULL);

    cf_user_vector users;
    CF_REQUIRE(cf_user_active_ordered(scratch.db, &users) == CF_OK);
    CF_REQUIRE(users.len == 3);
    CF_CHECK(users.items[0].id == 2); /* LOWER(name): amy < bender */
    CF_CHECK(users.items[1].id == 1); /* Bender/bender tie -> rowid */
    CF_CHECK(users.items[2].id == 3);
    cf_user_vector_dispose(&users);

    CF_REQUIRE(cf_user_active(scratch.db, &users) == CF_OK);
    CF_CHECK(users.len == 3); /* no ORDER BY: rowid order */
    CF_CHECK(users.items[0].id == 1 && users.items[1].id == 2 &&
             users.items[2].id == 3);
    cf_user_vector_dispose(&users);

    CF_REQUIRE(cf_user_active_ordered_without_bots(scratch.db, &users) ==
               CF_OK);
    CF_REQUIRE(users.len == 2);
    CF_CHECK(users.items[0].id == 2 && users.items[1].id == 3);
    cf_user_vector_dispose(&users);

    CF_REQUIRE(cf_user_active_bots_ordered(scratch.db, &users) == CF_OK);
    CF_REQUIRE(users.len == 1);
    CF_CHECK(users.items[0].id == 1);
    cf_user_vector_dispose(&users);

    /* LIKE is case-insensitive for ASCII and the bound %query% keeps
     * wildcards and escaping exactly as the reference binding does. */
    CF_REQUIRE(cf_user_active_filtered_by_ordered(scratch.db, S("BEN"),
                                                  &users) == CF_OK);
    CF_REQUIRE(users.len == 2);
    CF_CHECK(users.items[0].id == 1 && users.items[1].id == 3);
    cf_user_vector_dispose(&users);

    CF_REQUIRE(cf_user_active_filtered_by_ordered(scratch.db, S("_"), &users) ==
               CF_OK);
    CF_CHECK(users.len == 3); /* '_' is a LIKE wildcard, not literal */
    cf_user_vector_dispose(&users);

    CF_REQUIRE(cf_user_active_filtered_by_ordered(scratch.db, S("nomatch"),
                                                  &users) == CF_OK);
    CF_CHECK(users.items == NULL && users.len == 0);
    cf_user_vector_dispose(&users);

    CF_CHECK(cf_user_active_ordered(NULL, &users) == CF_INVALID);
    CF_CHECK(cf_user_active(scratch.db, NULL) == CF_INVALID);
    CF_CHECK(cf_user_active_filtered_by_ordered(scratch.db, S("x"), NULL) ==
              CF_INVALID);
    CF_CHECK(cf_user_active_filtered_by_ordered(scratch.db, (cf_str){NULL, 2},
                                                &users) == CF_INVALID);
    CF_CHECK(cf_user_active_ordered_without_bots(NULL, &users) == CF_INVALID);
    CF_CHECK(cf_user_active_bots_ordered(scratch.db, NULL) == CF_INVALID);

    cf_db_scratch_close(&scratch);
}

CF_TEST(find_active_bot_gate) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_user(scratch.db, 1, "Bot", NULL, NULL, CF_ROLE_BOT, CF_STATUS_ACTIVE,
              NULL, "tok");
    seed_user(scratch.db, 2, "BannedBot", NULL, NULL, CF_ROLE_BOT,
              CF_STATUS_BANNED, NULL, "tok2");
    seed_user(scratch.db, 3, "Member", NULL, NULL, CF_ROLE_MEMBER,
              CF_STATUS_ACTIVE, NULL, NULL);

    cf_user bot = {0};
    CF_REQUIRE(cf_user_find_active_bot(scratch.db, 1, &bot) == CF_OK);
    CF_CHECK(bot.id == 1 && bot.role == CF_ROLE_BOT);
    cf_user_dispose(&bot);
    CF_CHECK(cf_user_find_active_bot(scratch.db, 2, &bot) == CF_NOT_FOUND);
    CF_CHECK(cf_user_find_active_bot(scratch.db, 3, &bot) == CF_NOT_FOUND);
    CF_CHECK(cf_user_find_active_bot(scratch.db, 99, &bot) == CF_NOT_FOUND);
    CF_CHECK(cf_user_find_active_bot(NULL, 1, &bot) == CF_INVALID);
    CF_CHECK(cf_user_find_active_bot(scratch.db, 1, NULL) == CF_INVALID);

    cf_db_scratch_close(&scratch);
}

CF_TEST(authenticate_bot_split_semantics) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_user(scratch.db, 10, "Bot", NULL, NULL, CF_ROLE_BOT, CF_STATUS_ACTIVE,
              NULL, "tok");
    seed_user(scratch.db, 11, "EmptyTokenBot", NULL, NULL, CF_ROLE_BOT,
              CF_STATUS_ACTIVE, NULL, "");
    seed_user(scratch.db, 12, "NotABot", NULL, NULL, CF_ROLE_MEMBER,
              CF_STATUS_ACTIVE, NULL, "tok12");
    seed_user(scratch.db, 13, "BannedBot", NULL, NULL, CF_ROLE_BOT,
              CF_STATUS_BANNED, NULL, "tok13");

    cf_user user = {0};
    bool found = false;
    CF_REQUIRE(cf_user_authenticate_bot(scratch.db, S("10-tok"), &found,
                                        &user) == CF_OK);
    CF_REQUIRE(found);
    CF_CHECK(user.id == 10);
    cf_user_dispose(&user);

    /* str::split('-') keeps trailing empties: "10-" binds an empty token. */
    CF_CHECK(cf_user_authenticate_bot(scratch.db, S("10-"), &found, &user) ==
              CF_OK);
    CF_CHECK(!found);
    /* ... and a bot stored with an empty token matches "11-". */
    CF_REQUIRE(cf_user_authenticate_bot(scratch.db, S("11-"), &found, &user) ==
               CF_OK);
    CF_CHECK(found && user.id == 11);
    cf_user_dispose(&user);

    /* Only the first two dash fields are used. */
    CF_REQUIRE(cf_user_authenticate_bot(scratch.db, S("10-tok-extra"), &found,
                                        &user) == CF_OK);
    CF_CHECK(found && user.id == 10);
    cf_user_dispose(&user);

    CF_CHECK(cf_user_authenticate_bot(scratch.db, S("10-other"), &found,
                                      &user) == CF_OK);
    CF_CHECK(!found);
    CF_CHECK(cf_user_authenticate_bot(scratch.db, S("nonsense"), &found,
                                      &user) == CF_OK);
    CF_CHECK(!found);
    CF_CHECK(cf_user_authenticate_bot(scratch.db, S(""), &found, &user) ==
              CF_OK);
    CF_CHECK(!found);
    CF_CHECK(cf_user_authenticate_bot(scratch.db, S("-"), &found, &user) ==
              CF_OK);
    CF_CHECK(!found);
    /* id text must match an active bot: member 12 and banned bot 13 fail. */
    CF_CHECK(cf_user_authenticate_bot(scratch.db, S("12-tok"), &found, &user) ==
              CF_OK);
    CF_CHECK(!found);
    CF_CHECK(cf_user_authenticate_bot(scratch.db, S("13-tok"), &found, &user) ==
              CF_OK);
    CF_CHECK(!found);
    /* id affinity: the leading zero still matches the INTEGER id. */
    CF_REQUIRE(cf_user_authenticate_bot(scratch.db, S("010-tok"), &found,
                                        &user) == CF_OK);
    CF_CHECK(found && user.id == 10);
    cf_user_dispose(&user);

    CF_CHECK(cf_user_authenticate_bot(NULL, S("10-tok"), &found, &user) ==
              CF_INVALID);
    CF_CHECK(cf_user_authenticate_bot(scratch.db, S("10-tok"), NULL, &user) ==
              CF_INVALID);

    cf_db_scratch_close(&scratch);
}

CF_TEST(reload_and_associations) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_user(scratch.db, 1, "JZ", "jz@example.com", NULL, CF_ROLE_MEMBER,
              CF_STATUS_ACTIVE, "Bio", NULL);
    seed_user(scratch.db, 2, "Other", NULL, NULL, CF_ROLE_MEMBER,
              CF_STATUS_ACTIVE, NULL, NULL);
    seed_room(scratch.db, 1, "Rooms::Open", "Open Room");
    seed_room(scratch.db, 2, "Rooms::Direct", NULL);
    seed_membership(scratch.db, 1, 1, NULL);
    seed_membership(scratch.db, 2, 1, NULL);
    seed_session(scratch.db, 1, 1, "8.8.8.8");
    seed_session(scratch.db, 2, 1, "1.1.1.1");
    seed_session(scratch.db, 3, 2, NULL);

    cf_user user = {0};
    CF_REQUIRE(cf_user_find(scratch.db, 1, &user) == CF_OK);
    CF_REQUIRE(raw_exec(scratch.db,
                        "UPDATE users SET name = 'JZ2', bio = NULL "
                        "WHERE id = 1") == SQLITE_OK);
    CF_REQUIRE(cf_user_reload(scratch.db, &user) == CF_OK);
    CF_CHECK(str_is(user.name, "JZ2"));
    CF_CHECK(user.bio.present == false);
    cf_user_dispose(&user);

    CF_CHECK(cf_user_reload(scratch.db, &user) == CF_NOT_FOUND);
    CF_CHECK(user.name.ptr == NULL);
    CF_CHECK(cf_user_reload(NULL, &user) == CF_INVALID);
    CF_CHECK(cf_user_reload(scratch.db, NULL) == CF_INVALID);

    CF_REQUIRE(cf_user_find(scratch.db, 1, &user) == CF_OK);
    cf_membership_vector memberships;
    CF_REQUIRE(cf_user_memberships(scratch.db, &user, &memberships) == CF_OK);
    CF_REQUIRE(memberships.len == 2);
    CF_CHECK(memberships.items[0].room_id == 1 &&
             memberships.items[1].room_id == 2);
    CF_CHECK(memberships.items[0].user_id == 1);
    cf_membership_vector_dispose(&memberships);

    cf_session_vector sessions;
    CF_REQUIRE(cf_user_sessions(scratch.db, &user, &sessions) == CF_OK);
    CF_REQUIRE(sessions.len == 2);
    CF_CHECK(sessions.items[0].id == 1 && sessions.items[1].id == 2);
    CF_CHECK(optional_str_is(sessions.items[0].ip_address, "8.8.8.8"));
    CF_CHECK(sessions.items[1].user_id == 1);
    cf_session_vector_dispose(&sessions);

    CF_CHECK(cf_user_memberships(scratch.db, NULL, &memberships) == CF_INVALID);
    CF_CHECK(cf_user_memberships(scratch.db, &user, NULL) == CF_INVALID);
    CF_CHECK(cf_user_sessions(scratch.db, NULL, &sessions) == CF_INVALID);
    CF_CHECK(cf_user_sessions(scratch.db, &user, NULL) == CF_INVALID);
    cf_user_dispose(&user);

    cf_db_scratch_close(&scratch);
}

CF_TEST(webhook_lookup_and_url) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_user(scratch.db, 1, "WithUrl", NULL, NULL, CF_ROLE_BOT,
              CF_STATUS_ACTIVE, NULL, "tok");
    seed_user(scratch.db, 2, "NullUrl", NULL, NULL, CF_ROLE_BOT,
              CF_STATUS_ACTIVE, NULL, "tok2");
    seed_user(scratch.db, 3, "NoWebhook", NULL, NULL, CF_ROLE_BOT,
              CF_STATUS_ACTIVE, NULL, "tok3");
    seed_webhook(scratch.db, 1, "http://x");
    seed_webhook(scratch.db, 2, NULL);

    cf_user user = {0};
    bool found = false;
    cf_webhook webhook;
    CF_REQUIRE(cf_user_find(scratch.db, 1, &user) == CF_OK);
    CF_REQUIRE(cf_user_webhook(scratch.db, &user, &found, &webhook) == CF_OK);
    CF_REQUIRE(found);
    CF_CHECK(webhook.user_id == 1);
    CF_CHECK(optional_str_is(webhook.url, "http://x"));
    cf_webhook_dispose(&webhook);

    cf_str url = {0};
    CF_REQUIRE(cf_user_webhook_url(scratch.db, &user, &found, &url) == CF_OK);
    CF_REQUIRE(found);
    CF_CHECK(str_is(url, "http://x"));
    free(url.ptr);

    /* A webhook row with a NULL url is Option::None, not an error. */
    cf_user_dispose(&user);
    CF_REQUIRE(cf_user_find(scratch.db, 2, &user) == CF_OK);
    CF_REQUIRE(cf_user_webhook(scratch.db, &user, &found, &webhook) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(webhook.url.present == false);
    cf_webhook_dispose(&webhook);
    url = (cf_str){NULL, 0};
    found = true;
    CF_REQUIRE(cf_user_webhook_url(scratch.db, &user, &found, &url) == CF_OK);
    CF_CHECK(!found && url.ptr == NULL);

    cf_user_dispose(&user);
    CF_REQUIRE(cf_user_find(scratch.db, 3, &user) == CF_OK);
    webhook = (cf_webhook){0};
    found = true;
    CF_REQUIRE(cf_user_webhook(scratch.db, &user, &found, &webhook) == CF_OK);
    CF_CHECK(!found && webhook.id == 0);
    url = (cf_str){NULL, 0};
    found = true;
    CF_REQUIRE(cf_user_webhook_url(scratch.db, &user, &found, &url) == CF_OK);
    CF_CHECK(!found && url.ptr == NULL);

    CF_CHECK(cf_user_webhook(scratch.db, NULL, &found, &webhook) == CF_INVALID);
    CF_CHECK(cf_user_webhook(scratch.db, &user, NULL, &webhook) == CF_INVALID);
    CF_CHECK(cf_user_webhook_url(scratch.db, &user, NULL, &url) == CF_INVALID);
    CF_CHECK(cf_user_webhook_url(NULL, &user, &found, &url) == CF_INVALID);
    cf_user_dispose(&user);

    cf_db_scratch_close(&scratch);
}

/* --- create --------------------------------------------------------------- */

struct create_call {
    cf_new_user attributes;
    cf_err rc;
    cf_user user;
};

static cf_err create_user_cb(cf_tx *tx, void *arg) {
    struct create_call *call = arg;
    call->rc = cf_user_create(tx, &call->attributes, &call->user);
    return call->rc;
}

struct create_bot_call {
    const char *name;
    bool use_webhook;
    cf_optional_str webhook_url;
    cf_err rc;
    cf_user user;
};

static cf_err create_bot_cb(cf_tx *tx, void *arg) {
    struct create_bot_call *call = arg;
    call->rc = cf_user_create_bot(
        tx, S(call->name),
        call->use_webhook ? call->webhook_url
                          : (cf_optional_str){false, {NULL, 0}},
        &call->user);
    return call->rc;
}

CF_TEST(create_inserts_user_and_grants_open_rooms) {
    user_env env;
    CF_REQUIRE(user_env_open(&env));
    /* Two open rooms, one closed and one direct: only the open rooms grant. */
    seed_room(env.scratch.db, 1, "Rooms::Open", "One");
    seed_room(env.scratch.db, 2, "Rooms::Open", "Two");
    seed_room(env.scratch.db, 3, "Rooms::Closed", "Three");
    seed_room(env.scratch.db, 4, "Rooms::Direct", NULL);
    seed_user(env.scratch.db, 100, "Existing", NULL, NULL, CF_ROLE_MEMBER,
              CF_STATUS_ACTIVE, NULL, NULL);

    struct create_call call;
    memset(&call, 0, sizeof call);
    call.attributes.name = S("User");
    call.attributes.email_address =
        (cf_optional_str){.present = true, .value = S("user@example.com")};
    call.attributes.password_digest =
        (cf_optional_str){.present = true, .value = S(USER_TEST_DIGEST)};
    call.attributes.bio =
        (cf_optional_str){.present = true, .value = S("hello")};

    cf_test_clock_set_fixed_us(USER_TEST_NOW_US);
    cf_err write_rc = cf_write(env.app, create_user_cb, &call);
    cf_test_clock_clear();

    CF_CHECK(write_rc == CF_OK);
    CF_REQUIRE(call.rc == CF_OK);
    CF_REQUIRE(call.user.id != 0);
    CF_CHECK(str_is(call.user.name, "User"));
    CF_CHECK(call.user.role == CF_ROLE_MEMBER); /* NewUser default */
    CF_CHECK(call.user.status == CF_STATUS_ACTIVE);
    CF_CHECK(optional_str_is(call.user.email_address, "user@example.com"));
    CF_CHECK(optional_str_is(call.user.password_digest, USER_TEST_DIGEST));
    CF_CHECK(optional_str_is(call.user.bio, "hello"));
    CF_CHECK(call.user.bot_token.present == false);
    CF_CHECK(call.user.created_at == USER_TEST_NOW_US);
    CF_CHECK(call.user.updated_at == USER_TEST_NOW_US);
    int64_t new_id = call.user.id;
    cf_user_dispose(&call.user);

    /* Exactly the open rooms, involvement left to the column default. */
    bool ok = false;
    char buf[128];
    sqlite3 *handle = cf_db_handle(env.scratch.db);
    char sql[256];
    snprintf(sql, sizeof sql,
             "SELECT count(*) FROM memberships WHERE user_id = %lld",
             (long long)new_id);
    CF_CHECK(cf_db_test_i64(handle, sql, &ok) == 2);
    CF_CHECK(ok);
    snprintf(sql, sizeof sql,
             "SELECT involvement FROM memberships WHERE user_id = %lld "
             "ORDER BY room_id LIMIT 1",
             (long long)new_id);
    CF_CHECK(cf_db_test_text(handle, sql, buf, sizeof buf) != NULL);
    CF_CHECK(strcmp(buf, "mentions") == 0);
    snprintf(sql, sizeof sql,
             "SELECT created_at FROM memberships WHERE user_id = %lld "
             "ORDER BY room_id LIMIT 1",
             (long long)new_id);
    CF_CHECK(cf_db_test_text(handle, sql, buf, sizeof buf) != NULL);
    /* The reference lets SQLite stamp the grant rows with
     * STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW') (user.rs
     * grant_membership_to_open_rooms; time.rs SQLITE_NOW), not the injected
     * process clock, so the text is the wall clock at millisecond precision:
     * "YYYY-MM-DD HH:MM:SS.mmm" (23 chars), unlike the fixed-clock text
     * USER_TEST_NOW_TEXT ("...SS.ffffff", 26 chars). */
    CF_CHECK(strlen(buf) == strlen("2026-09-26 12:25:26.826"));
    CF_CHECK(buf[4] == '-' && buf[7] == '-' && buf[10] == ' ' &&
              buf[13] == ':' && buf[16] == ':' && buf[19] == '.');
    int64_t stamped_us = 0;
    CF_CHECK(cf_db_time_from_text(
                 (cf_span){(const unsigned char *)buf, strlen(buf)},
                 &stamped_us) == CF_OK);
    CF_CHECK(stamped_us != USER_TEST_NOW_US);

    /* A second create for the seeded user would conflict; creating another
     * user grants only the two open rooms again (the seeded user keeps none:
     * open-room grants are per created user). */
    struct create_call again;
    memset(&again, 0, sizeof again);
    again.attributes.name = S("User2");
    CF_REQUIRE(cf_write(env.app, create_user_cb, &again) == CF_OK);
    CF_REQUIRE(again.rc == CF_OK);
    snprintf(sql, sizeof sql,
             "SELECT count(*) FROM memberships WHERE user_id = %lld",
             (long long)again.user.id);
    CF_CHECK(cf_db_test_i64(handle, sql, &ok) == 2);
    CF_CHECK(ok);
    cf_user_dispose(&again.user);

    user_env_close(&env);
}

CF_TEST(create_validation_and_unique_email) {
    user_env env;
    CF_REQUIRE(user_env_open(&env));
    seed_user(env.scratch.db, 100, "Existing", "taken@example.com", NULL,
              CF_ROLE_MEMBER, CF_STATUS_ACTIVE, NULL, NULL);

    /* Duplicate non-NULL email violates the unique index; the transaction
     * rolls back and the output stays empty. */
    struct create_call call;
    memset(&call, 0, sizeof call);
    call.attributes.name = S("Copy");
    call.attributes.email_address =
        (cf_optional_str){.present = true, .value = S("taken@example.com")};
    CF_CHECK(cf_write(env.app, create_user_cb, &call) == CF_INVALID);
    CF_CHECK(call.user.name.ptr == NULL);

    sqlite3 *handle = cf_db_handle(env.scratch.db);
    bool ok = false;
    CF_CHECK(cf_db_test_i64(handle, "SELECT count(*) FROM users", &ok) == 1);
    CF_CHECK(ok);

    /* An invalid role never reaches SQL. */
    memset(&call, 0, sizeof call);
    call.attributes.name = S("BadRole");
    call.attributes.role = (cf_role)7;
    CF_CHECK(cf_write(env.app, create_user_cb, &call) == CF_INVALID);
    CF_CHECK(call.user.name.ptr == NULL);

    /* NULL names are not a C-level crash; the Rust type cannot express one,
     * so a span without bytes is rejected before SQL. */
    memset(&call, 0, sizeof call);
    call.attributes.name = (cf_str){NULL, 3};
    CF_CHECK(cf_write(env.app, create_user_cb, &call) == CF_INVALID);

    /* cf_user_create with no transaction. */
    cf_user out = {0};
    struct create_call direct;
    memset(&direct, 0, sizeof direct);
    direct.attributes.name = S("Direct");
    CF_CHECK(cf_user_create(NULL, &direct.attributes, &out) == CF_INVALID);
    CF_CHECK(out.name.ptr == NULL);
    CF_CHECK(cf_user_create(NULL, NULL, &out) == CF_INVALID);

    user_env_close(&env);
}

CF_TEST(create_bot_token_and_webhook) {
    user_env env;
    CF_REQUIRE(user_env_open(&env));
    seed_room(env.scratch.db, 1, "Rooms::Open", "Open");

    /* create_bot generates its own token; the fixed source drives 12 'p's. */
    struct create_bot_call bot;
    memset(&bot, 0, sizeof bot);
    bot.name = "Bender";
    bot.use_webhook = true;
    bot.webhook_url = (cf_optional_str){.present = true,
                                        .value = S("http://x")};

    cf_test_random_fill(0xA5);
    cf_test_clock_set_fixed_us(USER_TEST_NOW_US);
    cf_err write_rc = cf_write(env.app, create_bot_cb, &bot);
    cf_test_clock_clear();
    cf_test_random_clear();

    CF_CHECK(write_rc == CF_OK);
    CF_REQUIRE(bot.rc == CF_OK);
    CF_CHECK(bot.user.role == CF_ROLE_BOT);
    CF_CHECK(optional_str_is(bot.user.bot_token, "pppppppppppp"));
    CF_CHECK(bot.user.password_digest.present == false);
    cf_str key = {0};
    CF_REQUIRE(cf_user_bot_key(&bot.user, &key) == CF_OK);
    char want_key[64];
    snprintf(want_key, sizeof want_key, "%lld-pppppppppppp",
             (long long)bot.user.id);
    CF_CHECK(str_is(key, want_key));
    free(key.ptr);
    /* Membership grant applies to bots too (create inserts a users row). */
    bool ok = false;
    char sql[128];
    snprintf(sql, sizeof sql,
             "SELECT count(*) FROM memberships WHERE user_id = %lld",
             (long long)bot.user.id);
    CF_CHECK(cf_db_test_i64(cf_db_handle(env.scratch.db), sql, &ok) == 1);
    CF_CHECK(ok);

    int64_t bot_id = bot.user.id;
    cf_user_dispose(&bot.user);

    cf_str url = {0};
    bool found = false;
    cf_user loaded = {0};
    CF_REQUIRE(cf_user_find(env.scratch.db, bot_id, &loaded) == CF_OK);
    CF_REQUIRE(cf_user_webhook_url(env.scratch.db, &loaded, &found, &url) ==
               CF_OK);
    CF_CHECK(found && str_is(url, "http://x"));
    free(url.ptr);
    cf_user_dispose(&loaded);

    /* authenticate_bot accepts the generated key. */
    found = false;
    CF_REQUIRE(cf_user_authenticate_bot(env.scratch.db, S(want_key), &found,
                                        &loaded) == CF_OK);
    CF_CHECK(found && loaded.id == bot_id);
    cf_user_dispose(&loaded);

    /* Without a webhook argument no webhooks row is created. */
    struct create_bot_call plain;
    memset(&plain, 0, sizeof plain);
    plain.name = "Plain";
    write_rc = cf_write(env.app, create_bot_cb, &plain);
    CF_REQUIRE(write_rc == CF_OK);
    CF_REQUIRE(plain.rc == CF_OK);
    CF_CHECK(plain.user.role == CF_ROLE_BOT);
    CF_CHECK(plain.user.bot_token.present &&
             plain.user.bot_token.value.len == 12);
    cf_user_dispose(&plain.user);

    user_env_close(&env);
}

/* --- update --------------------------------------------------------------- */

struct update_call {
    int64_t user_id;
    cf_user_changes changes;
    cf_err rc;
    cf_user user;
};

static cf_err update_user_cb(cf_tx *tx, void *arg) {
    struct update_call *call = arg;
    cf_err rc = cf_user_find(cf_tx_db(tx), call->user_id, &call->user);
    if (rc != CF_OK) {
        call->rc = rc;
        return rc;
    }
    call->rc = cf_user_update(tx, &call->user, &call->changes);
    return call->rc;
}

/* The same callback with a NULL changes pointer (the zero value). */
static cf_err update_null_changes_cb(cf_tx *tx, void *arg) {
    struct update_call *call = arg;
    cf_err rc = cf_user_find(cf_tx_db(tx), call->user_id, &call->user);
    if (rc != CF_OK) {
        call->rc = rc;
        return rc;
    }
    call->rc = cf_user_update(tx, &call->user, NULL);
    return call->rc;
}

CF_TEST(update_changes_noop_and_null) {
    user_env env;
    CF_REQUIRE(user_env_open(&env));
    seed_user(env.scratch.db, 1, "JZ", "jz@example.com", USER_TEST_DIGEST,
              CF_ROLE_MEMBER, CF_STATUS_ACTIVE, "Old bio", "tok");

    /* No changes at all: nothing written, not even updated_at. */
    struct update_call call;
    memset(&call, 0, sizeof call);
    call.user_id = 1;
    cf_test_clock_set_fixed_us(USER_TEST_NOW_US);
    cf_err write_rc = cf_write(env.app, update_user_cb, &call);
    cf_test_clock_clear();
    CF_CHECK(write_rc == CF_OK);
    CF_CHECK(call.rc == CF_OK);
    CF_CHECK(call.user.updated_at == USER_TEST_SEEDED_TS);
    cf_user_dispose(&call.user);

    /* Values equal to the current ones are dropped by the filters. */
    const cf_str same_name = S("JZ");
    memset(&call, 0, sizeof call);
    call.user_id = 1;
    call.changes.name = &same_name;
    cf_test_clock_set_fixed_us(USER_TEST_NOW_US);
    write_rc = cf_write(env.app, update_user_cb, &call);
    cf_test_clock_clear();
    CF_CHECK(write_rc == CF_OK);
    CF_CHECK(call.user.updated_at == USER_TEST_SEEDED_TS);
    cf_user_dispose(&call.user);

    /* password_digest has no equality filter: the same digest still touches. */
    const cf_str same_digest = S(USER_TEST_DIGEST);
    memset(&call, 0, sizeof call);
    call.user_id = 1;
    call.changes.password_digest = &same_digest;
    cf_test_clock_set_fixed_us(USER_TEST_NOW_US);
    write_rc = cf_write(env.app, update_user_cb, &call);
    cf_test_clock_clear();
    CF_CHECK(write_rc == CF_OK);
    CF_CHECK(call.rc == CF_OK);
    CF_CHECK(call.user.updated_at == USER_TEST_NOW_US);
    cf_user_dispose(&call.user);

    /* One change per column, NULL outer leaves alone, present=false clears. */
    const cf_str new_name = S("JZ2");
    const cf_str new_digest = S(USER_TEST_LONG_DIGEST);
    const cf_optional_str clear_bio = {false, {NULL, 0}};
    const cf_optional_str clear_token = {false, {NULL, 0}};
    const cf_role admin = CF_ROLE_ADMINISTRATOR;
    const cf_status banned = CF_STATUS_BANNED;
    memset(&call, 0, sizeof call);
    call.user_id = 1;
    call.changes.name = &new_name;
    call.changes.bio = &clear_bio;
    call.changes.password_digest = &new_digest;
    call.changes.role = &admin;
    call.changes.status = &banned;
    call.changes.bot_token = &clear_token;
    cf_test_clock_set_fixed_us(USER_TEST_NOW_US);
    write_rc = cf_write(env.app, update_user_cb, &call);
    cf_test_clock_clear();
    CF_CHECK(write_rc == CF_OK);
    CF_REQUIRE(call.rc == CF_OK);
    CF_CHECK(str_is(call.user.name, "JZ2"));
    CF_CHECK(call.user.bio.present == false);
    CF_CHECK(optional_str_is(call.user.password_digest,
                             USER_TEST_LONG_DIGEST));
    CF_CHECK(call.user.role == CF_ROLE_ADMINISTRATOR);
    CF_CHECK(call.user.status == CF_STATUS_BANNED);
    CF_CHECK(call.user.bot_token.present == false);
    CF_CHECK(call.user.updated_at == USER_TEST_NOW_US);
    CF_CHECK(optional_str_is(call.user.email_address, "jz@example.com"));
    cf_user_dispose(&call.user);

    /* The same values reached the stored row. */
    cf_user stored = {0};
    CF_REQUIRE(cf_user_find(env.scratch.db, 1, &stored) == CF_OK);
    CF_CHECK(str_is(stored.name, "JZ2"));
    CF_CHECK(stored.bio.present == false);
    CF_CHECK(stored.role == CF_ROLE_ADMINISTRATOR);
    CF_CHECK(stored.status == CF_STATUS_BANNED);
    CF_CHECK(stored.bot_token.present == false);
    CF_CHECK(stored.updated_at == USER_TEST_NOW_US);
    cf_user_dispose(&stored);

    /* A NULL changes pointer is the zero value: no-op. */
    memset(&call, 0, sizeof call);
    call.user_id = 1;
    call.changes.name = NULL;
    cf_test_clock_set_fixed_us(USER_TEST_NOW_US);
    write_rc = cf_write(env.app, update_null_changes_cb, &call);
    cf_test_clock_clear();
    CF_CHECK(write_rc == CF_OK);
    CF_CHECK(call.rc == CF_OK);
    CF_CHECK(call.user.updated_at == USER_TEST_NOW_US); /* unchanged */
    cf_user_dispose(&call.user);

    /* NULL transaction/user/callbacks. */
    cf_user zero_user = {0};
    CF_CHECK(cf_user_update(NULL, &zero_user, NULL) == CF_INVALID);
    CF_CHECK(cf_user_update(NULL, NULL, NULL) == CF_INVALID);

    user_env_close(&env);
}

struct update_bot_call {
    int64_t user_id;
    cf_user_changes changes;
    cf_optional_str webhook_url;
    cf_err rc;
    cf_user user;
};

static cf_err update_bot_cb(cf_tx *tx, void *arg) {
    struct update_bot_call *call = arg;
    cf_err rc = cf_user_find(cf_tx_db(tx), call->user_id, &call->user);
    if (rc != CF_OK) {
        call->rc = rc;
        return rc;
    }
    call->rc = cf_user_update_bot(tx, &call->user, &call->changes,
                                  call->webhook_url);
    return call->rc;
}

CF_TEST(update_bot_webhook_lifecycle) {
    user_env env;
    CF_REQUIRE(user_env_open(&env));
    seed_user(env.scratch.db, 1, "Bot", NULL, NULL, CF_ROLE_BOT,
              CF_STATUS_ACTIVE, NULL, "tok");
    seed_webhook(env.scratch.db, 1, "http://old");
    seed_user(env.scratch.db, 2, "Bot2", NULL, NULL, CF_ROLE_BOT,
              CF_STATUS_ACTIVE, NULL, "tok2");

    /* Existing webhook: update url (webhook first, then the user). */
    struct update_bot_call call;
    memset(&call, 0, sizeof call);
    call.user_id = 1;
    const cf_str new_name = S("Bot Renamed");
    call.changes.name = &new_name;
    call.webhook_url = (cf_optional_str){.present = true,
                                         .value = S("http://new")};
    cf_test_clock_set_fixed_us(USER_TEST_NOW_US);
    cf_err write_rc = cf_write(env.app, update_bot_cb, &call);
    cf_test_clock_clear();
    CF_CHECK(write_rc == CF_OK);
    CF_REQUIRE(call.rc == CF_OK);
    CF_CHECK(str_is(call.user.name, "Bot Renamed"));
    cf_user_dispose(&call.user);

    cf_user loaded = {0};
    cf_str url = {0};
    bool found = false;
    CF_REQUIRE(cf_user_find(env.scratch.db, 1, &loaded) == CF_OK);
    CF_REQUIRE(cf_user_webhook_url(env.scratch.db, &loaded, &found, &url) ==
               CF_OK);
    CF_CHECK(found && str_is(url, "http://new"));
    free(url.ptr);

    /* Blank (whitespace) url destroys the webhook. */
    memset(&call, 0, sizeof call);
    call.user_id = 1;
    call.webhook_url = (cf_optional_str){.present = true,
                                         .value = S("   ")};
    write_rc = cf_write(env.app, update_bot_cb, &call);
    CF_CHECK(write_rc == CF_OK);
    CF_CHECK(call.rc == CF_OK);
    cf_user_dispose(&call.user);
    url = (cf_str){NULL, 0};
    found = true;
    CF_REQUIRE(cf_user_webhook_url(env.scratch.db, &loaded, &found, &url) ==
               CF_OK);
    CF_CHECK(!found && url.ptr == NULL);

    /* No webhook: a present url creates one. */
    memset(&call, 0, sizeof call);
    call.user_id = 2;
    call.webhook_url = (cf_optional_str){.present = true,
                                         .value = S("http://fresh")};
    write_rc = cf_write(env.app, update_bot_cb, &call);
    CF_CHECK(write_rc == CF_OK);
    CF_CHECK(call.rc == CF_OK);
    cf_user_dispose(&call.user);
    cf_user_dispose(&loaded);
    CF_REQUIRE(cf_user_find(env.scratch.db, 2, &loaded) == CF_OK);
    url = (cf_str){NULL, 0};
    found = false;
    CF_REQUIRE(cf_user_webhook_url(env.scratch.db, &loaded, &found, &url) ==
               CF_OK);
    CF_CHECK(found && str_is(url, "http://fresh"));
    free(url.ptr);

    /* Absent url with no webhook is a no-op; absent with one destroys. */
    memset(&call, 0, sizeof call);
    call.user_id = 2;
    call.webhook_url = (cf_optional_str){false, {NULL, 0}};
    write_rc = cf_write(env.app, update_bot_cb, &call);
    CF_CHECK(write_rc == CF_OK);
    CF_CHECK(call.rc == CF_OK);
    cf_user_dispose(&call.user);
    url = (cf_str){NULL, 0};
    found = true;
    CF_REQUIRE(cf_user_webhook_url(env.scratch.db, &loaded, &found, &url) ==
               CF_OK);
    CF_CHECK(!found);

    CF_CHECK(cf_user_update_bot(NULL, &loaded, NULL,
                                (cf_optional_str){false, {NULL, 0}}) ==
              CF_INVALID);
    CF_CHECK(cf_user_update_bot(NULL, NULL, NULL,
                                (cf_optional_str){false, {NULL, 0}}) ==
              CF_INVALID);
    cf_user_dispose(&loaded);

    user_env_close(&env);
}

struct reset_call {
    cf_err rc;
    cf_user user;
};

static cf_err reset_bot_key_cb(cf_tx *tx, void *arg) {
    struct reset_call *call = arg;
    call->rc = cf_user_find(cf_tx_db(tx), 1, &call->user);
    if (call->rc == CF_OK) {
        call->rc = cf_user_reset_bot_key(tx, &call->user);
    }
    return call->rc;
}

CF_TEST(reset_bot_key_authentication) {
    user_env env;
    CF_REQUIRE(user_env_open(&env));
    seed_user(env.scratch.db, 1, "Bot", NULL, NULL, CF_ROLE_BOT,
              CF_STATUS_ACTIVE, NULL, "oldtoken");

    struct reset_call call;
    memset(&call, 0, sizeof call);
    cf_test_random_fill(0x00); /* deterministic 12 'A' token */
    cf_test_clock_set_fixed_us(USER_TEST_NOW_US);
    cf_err write_rc = cf_write(env.app, reset_bot_key_cb, &call);
    cf_test_clock_clear();
    cf_test_random_clear();

    CF_CHECK(write_rc == CF_OK);
    CF_REQUIRE(call.rc == CF_OK);
    CF_CHECK(optional_str_is(call.user.bot_token, "AAAAAAAAAAAA"));
    CF_CHECK(call.user.updated_at == USER_TEST_NOW_US);
    cf_user_dispose(&call.user);

    bool found = true;
    cf_user bot = {0};
    CF_CHECK(cf_user_authenticate_bot(env.scratch.db, S("1-oldtoken"), &found,
                                      &bot) == CF_OK);
    CF_CHECK(!found);
    CF_REQUIRE(cf_user_authenticate_bot(env.scratch.db, S("1-AAAAAAAAAAAA"),
                                        &found, &bot) == CF_OK);
    CF_CHECK(found && bot.id == 1);
    cf_user_dispose(&bot);

    CF_CHECK(cf_user_reset_bot_key(NULL, &bot) == CF_INVALID);
    CF_CHECK(cf_user_reset_bot_key(NULL, NULL) == CF_INVALID);

    user_env_close(&env);
}

/* --- deactivate / ban ----------------------------------------------------- */

struct mutate_call {
    int64_t user_id;
    cf_err rc;
    cf_user user;
    cf_message_vector messages;
    bool has_messages;
};

static cf_err deactivate_cb(cf_tx *tx, void *arg) {
    struct mutate_call *call = arg;
    call->rc = cf_user_find(cf_tx_db(tx), call->user_id, &call->user);
    if (call->rc == CF_OK) call->rc = cf_user_deactivate(tx, &call->user);
    return call->rc;
}

CF_TEST(deactivate_cleanup_email_and_event) {
    user_env env;
    CF_REQUIRE(user_env_open(&env));
    seed_user(env.scratch.db, 1, "david", "david@37signals.com", USER_TEST_DIGEST,
              CF_ROLE_ADMINISTRATOR, CF_STATUS_ACTIVE, NULL, NULL);
    seed_user(env.scratch.db, 2, "other", NULL, NULL, CF_ROLE_MEMBER,
              CF_STATUS_ACTIVE, NULL, NULL);
    seed_room(env.scratch.db, 1, "Rooms::Open", "Open");
    seed_room(env.scratch.db, 2, "Rooms::Direct", NULL);
    seed_room(env.scratch.db, 3, "Rooms::Closed", "Closed");
    seed_membership(env.scratch.db, 1, 1, NULL);
    seed_membership(env.scratch.db, 2, 1, NULL);
    seed_membership(env.scratch.db, 3, 1, NULL);
    seed_membership(env.scratch.db, 1, 2, NULL);
    seed_push_subscription(env.scratch.db, 1);
    seed_push_subscription(env.scratch.db, 2);
    seed_search(env.scratch.db, 1, "query");
    seed_search(env.scratch.db, 2, "other");
    seed_session(env.scratch.db, 1, 1, "8.8.8.8");
    seed_session(env.scratch.db, 2, 2, "1.1.1.1");

    struct mutate_call call;
    memset(&call, 0, sizeof call);
    call.user_id = 1;
    cf_test_random_fill(0x00); /* uuid 00000000-0000-4000-8000-... */
    cf_test_clock_set_fixed_us(USER_TEST_NOW_US);
    cf_err write_rc = cf_write(env.app, deactivate_cb, &call);
    cf_test_clock_clear();
    cf_test_random_clear();

    CF_CHECK(write_rc == CF_OK);
    CF_REQUIRE(call.rc == CF_OK);
    CF_CHECK(call.user.status == CF_STATUS_DEACTIVATED);
    CF_CHECK(optional_str_is(
        call.user.email_address,
        "david-deactivated-00000000-0000-4000-8000-000000000000"
        "@37signals.com"));
    CF_CHECK(call.user.updated_at == USER_TEST_NOW_US);
    cf_user_dispose(&call.user);

    sqlite3 *handle = cf_db_handle(env.scratch.db);
    bool ok = false;
    /* Non-direct memberships of user 1 only; direct stays, user 2 untouched. */
    CF_CHECK(cf_db_test_i64(handle,
                            "SELECT count(*) FROM memberships WHERE "
                            "user_id = 1",
                            &ok) == 1);
    CF_CHECK(ok);
    CF_CHECK(cf_db_test_i64(handle,
                            "SELECT count(*) FROM memberships WHERE "
                            "room_id = 2 AND user_id = 1",
                            &ok) == 1);
    CF_CHECK(cf_db_test_i64(handle,
                            "SELECT count(*) FROM push_subscriptions WHERE "
                            "user_id = 1",
                            &ok) == 0);
    CF_CHECK(cf_db_test_i64(handle,
                            "SELECT count(*) FROM searches WHERE user_id = 1",
                            &ok) == 0);
    CF_CHECK(cf_db_test_i64(handle,
                            "SELECT count(*) FROM sessions WHERE user_id = 1",
                            &ok) == 0);
    CF_CHECK(cf_db_test_i64(handle,
                            "SELECT count(*) FROM push_subscriptions WHERE "
                            "user_id = 2",
                            &ok) == 1);
    CF_CHECK(ok);

    CF_REQUIRE(env.events.len == 1);
    expect_event(&env, 0, CF_EVENT_DISCONNECT_USER, 1, 0, 0, false);

    user_env_close(&env);
}

CF_TEST(deactivate_without_email) {
    user_env env;
    CF_REQUIRE(user_env_open(&env));
    seed_user(env.scratch.db, 1, "NoMail", NULL, NULL, CF_ROLE_MEMBER,
              CF_STATUS_ACTIVE, NULL, NULL);

    struct mutate_call call;
    memset(&call, 0, sizeof call);
    call.user_id = 1;
    cf_test_random_fill(0x00);
    cf_err write_rc = cf_write(env.app, deactivate_cb, &call);
    cf_test_random_clear();
    CF_CHECK(write_rc == CF_OK);
    CF_REQUIRE(call.rc == CF_OK);
    CF_CHECK(call.user.status == CF_STATUS_DEACTIVATED);
    CF_CHECK(call.user.email_address.present == false);
    cf_user_dispose(&call.user);
    CF_REQUIRE(env.events.len == 1);
    expect_event(&env, 0, CF_EVENT_DISCONNECT_USER, 1, 0, 0, false);

    user_env_close(&env);
}

static cf_err ban_cb(cf_tx *tx, void *arg) {
    struct mutate_call *call = arg;
    call->rc = cf_user_find(cf_tx_db(tx), call->user_id, &call->user);
    if (call->rc == CF_OK) call->rc = cf_user_ban(tx, &call->user);
    return call->rc;
}

static cf_err unban_cb(cf_tx *tx, void *arg) {
    struct mutate_call *call = arg;
    call->rc = cf_user_find(cf_tx_db(tx), call->user_id, &call->user);
    if (call->rc == CF_OK) call->rc = cf_user_unban(tx, &call->user);
    return call->rc;
}

CF_TEST(ban_and_unban) {
    user_env env;
    CF_REQUIRE(user_env_open(&env));
    seed_user(env.scratch.db, 1, "kevin", NULL, NULL, CF_ROLE_MEMBER,
              CF_STATUS_ACTIVE, NULL, NULL);
    seed_session(env.scratch.db, 1, 1, "8.8.8.8");
    seed_session(env.scratch.db, 2, 1, "8.8.8.8");
    seed_session(env.scratch.db, 3, 1, "");
    seed_session(env.scratch.db, 4, 1, NULL);
    seed_session(env.scratch.db, 5, 1, "1.1.1.1");

    struct mutate_call call;
    memset(&call, 0, sizeof call);
    call.user_id = 1;
    cf_err write_rc = cf_write(env.app, ban_cb, &call);
    CF_CHECK(write_rc == CF_OK);
    CF_REQUIRE(call.rc == CF_OK);
    CF_CHECK(call.user.status == CF_STATUS_BANNED);
    cf_user_dispose(&call.user);

    sqlite3 *handle = cf_db_handle(env.scratch.db);
    bool ok = false;
    /* compact_blank.uniq: public addresses, first occurrence order. */
    CF_CHECK(cf_db_test_i64(handle,
                            "SELECT count(*) FROM bans WHERE user_id = 1",
                            &ok) == 2);
    CF_CHECK(ok);
    char buf[128];
    CF_CHECK(cf_db_test_text(handle,
                             "SELECT ip_address FROM bans WHERE user_id = 1 "
                             "ORDER BY id LIMIT 1",
                             buf, sizeof buf) != NULL);
    CF_CHECK(strcmp(buf, "8.8.8.8") == 0);
    CF_CHECK(cf_db_test_text(handle,
                             "SELECT ip_address FROM bans WHERE user_id = 1 "
                             "ORDER BY id LIMIT 1 OFFSET 1",
                             buf, sizeof buf) != NULL);
    CF_CHECK(strcmp(buf, "1.1.1.1") == 0);
    CF_CHECK(cf_db_test_i64(handle,
                            "SELECT count(*) FROM sessions WHERE user_id = 1",
                            &ok) == 0);
    CF_CHECK(ok);

    CF_REQUIRE(env.events.len == 2);
    expect_event(&env, 0, CF_EVENT_DISCONNECT_USER, 1, 0, 0, false);
    expect_event(&env, 1, CF_EVENT_REMOVE_BANNED_CONTENT, 1, 0, 0, false);

    /* Unban follows the source: delete the bans and reactivate. */
    memset(&call, 0, sizeof call);
    call.user_id = 1;
    write_rc = cf_write(env.app, unban_cb, &call);
    CF_CHECK(write_rc == CF_OK);
    CF_REQUIRE(call.rc == CF_OK);
    CF_CHECK(call.user.status == CF_STATUS_ACTIVE);
    cf_user_dispose(&call.user);
    CF_CHECK(cf_db_test_i64(handle,
                            "SELECT count(*) FROM bans WHERE user_id = 1",
                            &ok) == 0);
    CF_CHECK(ok);

    user_env_close(&env);
}

CF_TEST(ban_rejects_private_ip_rolls_back) {
    user_env env;
    CF_REQUIRE(user_env_open(&env));
    seed_user(env.scratch.db, 1, "kevin", NULL, NULL, CF_ROLE_MEMBER,
              CF_STATUS_ACTIVE, NULL, NULL);
    seed_session(env.scratch.db, 1, 1, "192.168.1.1");

    struct mutate_call call;
    memset(&call, 0, sizeof call);
    call.user_id = 1;
    cf_err write_rc = cf_write(env.app, ban_cb, &call);
    CF_CHECK(write_rc == CF_INVALID);
    CF_CHECK(call.rc == CF_INVALID);
    cf_user_dispose(&call.user);

    bool ok = false;
    sqlite3 *handle = cf_db_handle(env.scratch.db);
    CF_CHECK(cf_db_test_i64(handle,
                            "SELECT count(*) FROM bans WHERE user_id = 1",
                            &ok) == 0);
    CF_CHECK(ok);
    CF_CHECK(cf_db_test_i64(handle,
                            "SELECT count(*) FROM sessions WHERE user_id = 1",
                            &ok) == 1);
    CF_CHECK(ok);
    cf_user stored = {0};
    CF_REQUIRE(cf_user_find(env.scratch.db, 1, &stored) == CF_OK);
    CF_CHECK(stored.status == CF_STATUS_ACTIVE);
    cf_user_dispose(&stored);
    /* Rolled back: no event is delivered. */
    CF_CHECK(env.events.len == 0);

    user_env_close(&env);
}

static cf_err remove_content_cb(cf_tx *tx, void *arg) {
    struct mutate_call *call = arg;
    call->rc = cf_user_find(cf_tx_db(tx), call->user_id, &call->user);
    if (call->rc == CF_OK) {
        call->rc = cf_user_remove_banned_content(tx, &call->user,
                                                 &call->messages);
        call->has_messages = call->rc == CF_OK;
    }
    return call->rc;
}

CF_TEST(remove_banned_content) {
    user_env env;
    CF_REQUIRE(user_env_open(&env));
    seed_user(env.scratch.db, 1, "jz", NULL, NULL, CF_ROLE_MEMBER,
              CF_STATUS_ACTIVE, NULL, NULL);
    seed_user(env.scratch.db, 2, "other", NULL, NULL, CF_ROLE_MEMBER,
              CF_STATUS_ACTIVE, NULL, NULL);
    seed_room(env.scratch.db, 1, "Rooms::Open", "Open");
    seed_message(env.scratch.db, 1, 1, 1);
    seed_message(env.scratch.db, 2, 1, 2);
    seed_message(env.scratch.db, 3, 1, 1);

    struct mutate_call call;
    memset(&call, 0, sizeof call);
    call.user_id = 1;
    cf_err write_rc = cf_write(env.app, remove_content_cb, &call);
    CF_CHECK(write_rc == CF_OK);
    CF_REQUIRE(call.rc == CF_OK);
    CF_REQUIRE(call.has_messages);
    /* Copies are returned after the rows are destroyed, id order. */
    CF_REQUIRE(call.messages.len == 2);
    CF_CHECK(call.messages.items[0].id == 1);
    CF_CHECK(call.messages.items[1].id == 3);
    CF_CHECK(call.messages.items[0].creator_id == 1);
    cf_message_vector_dispose(&call.messages);
    cf_user_dispose(&call.user);

    sqlite3 *handle = cf_db_handle(env.scratch.db);
    bool ok = false;
    CF_CHECK(cf_db_test_i64(handle,
                            "SELECT count(*) FROM messages WHERE "
                            "creator_id = 1",
                            &ok) == 0);
    CF_CHECK(ok);
    CF_CHECK(cf_db_test_i64(handle, "SELECT count(*) FROM messages", &ok) == 1);
    CF_CHECK(ok);

    CF_CHECK(cf_user_remove_banned_content(NULL, &call.user, &call.messages) ==
              CF_INVALID);
    CF_CHECK(cf_user_remove_banned_content(NULL, NULL,
                                           &call.messages) == CF_INVALID);
    CF_CHECK(cf_user_remove_banned_content(NULL, &call.user, NULL) ==
              CF_INVALID);

    user_env_close(&env);
}

static cf_err disconnect_cb(cf_tx *tx, void *arg) {
    struct mutate_call *call = arg;
    call->rc = cf_user_find(cf_tx_db(tx), call->user_id, &call->user);
    if (call->rc == CF_OK) {
        call->rc = cf_user_reset_remote_connections(tx, &call->user);
    }
    return call->rc;
}

static cf_err webhook_later_cb(cf_tx *tx, void *arg) {
    struct mutate_call *call = arg;
    call->rc = cf_user_find(cf_tx_db(tx), call->user_id, &call->user);
    if (call->rc == CF_OK) {
        call->rc = cf_user_deliver_webhook_later(tx, &call->user, 77);
    }
    return call->rc;
}

CF_TEST(reset_remote_connections_and_deliver_webhook) {
    user_env env;
    CF_REQUIRE(user_env_open(&env));
    seed_user(env.scratch.db, 1, "jz", NULL, NULL, CF_ROLE_MEMBER,
              CF_STATUS_ACTIVE, NULL, NULL);
    seed_user(env.scratch.db, 2, "bender", NULL, NULL, CF_ROLE_BOT,
              CF_STATUS_ACTIVE, NULL, "tok");
    seed_webhook(env.scratch.db, 2, "http://x");

    struct mutate_call call;
    memset(&call, 0, sizeof call);
    call.user_id = 1;
    cf_err write_rc = cf_write(env.app, disconnect_cb, &call);
    CF_CHECK(write_rc == CF_OK);
    CF_REQUIRE(call.rc == CF_OK);
    cf_user_dispose(&call.user);
    CF_REQUIRE(env.events.len == 1);
    expect_event(&env, 0, CF_EVENT_DISCONNECT_USER, 1, 0, 0, true);

    /* No webhook: no job. */
    memset(&call, 0, sizeof call);
    call.user_id = 1;
    write_rc = cf_write(env.app, webhook_later_cb, &call);
    CF_CHECK(write_rc == CF_OK);
    CF_REQUIRE(call.rc == CF_OK);
    cf_user_dispose(&call.user);
    CF_CHECK(env.events.len == 1);

    /* Bot with a webhook: DeliverWebhook{bot_id, message_id}. */
    memset(&call, 0, sizeof call);
    call.user_id = 2;
    write_rc = cf_write(env.app, webhook_later_cb, &call);
    CF_CHECK(write_rc == CF_OK);
    CF_REQUIRE(call.rc == CF_OK);
    cf_user_dispose(&call.user);
    CF_REQUIRE(env.events.len == 2);
    expect_event(&env, 1, CF_EVENT_DELIVER_WEBHOOK, 2, 0, 77, false);

    CF_CHECK(cf_user_reset_remote_connections(NULL, &call.user) == CF_INVALID);
    CF_CHECK(cf_user_reset_remote_connections(NULL, NULL) ==
              CF_INVALID);
    CF_CHECK(cf_user_deliver_webhook_later(NULL, &call.user, 1) == CF_INVALID);
    CF_CHECK(cf_user_deliver_webhook_later(NULL, NULL, 1) ==
              CF_INVALID);

    user_env_close(&env);
}

/* --- disposal edges ------------------------------------------------------- */

CF_TEST(dispose_and_failure_edges) {
    cf_user_dispose(NULL);
    cf_user_vector_dispose(NULL);

    cf_user user = {0};
    user.name.ptr = malloc(2);
    CF_REQUIRE(user.name.ptr != NULL);
    memcpy(user.name.ptr, "a", 2);
    user.name.len = 1;
    user.email_address = (cf_optional_str){.present = true,
                                           .value = {malloc(2), 1}};
    CF_REQUIRE(user.email_address.value.ptr != NULL);
    memcpy(user.email_address.value.ptr, "e", 2);
    user.password_digest = (cf_optional_str){.present = true,
                                             .value = {malloc(2), 1}};
    CF_REQUIRE(user.password_digest.value.ptr != NULL);
    memcpy(user.password_digest.value.ptr, "d", 2);
    user.bio = (cf_optional_str){.present = true, .value = {malloc(2), 1}};
    CF_REQUIRE(user.bio.value.ptr != NULL);
    memcpy(user.bio.value.ptr, "b", 2);
    user.bot_token = (cf_optional_str){.present = true,
                                       .value = {malloc(2), 1}};
    CF_REQUIRE(user.bot_token.value.ptr != NULL);
    memcpy(user.bot_token.value.ptr, "t", 2);
    user.id = 9;
    cf_user_dispose(&user);
    CF_CHECK(user.id == 0 && user.name.ptr == NULL);
    CF_CHECK(user.email_address.present == false);
    CF_CHECK(user.password_digest.present == false);
    CF_CHECK(user.bio.present == false);
    CF_CHECK(user.bot_token.present == false);
    cf_user_dispose(&user); /* zero state is a no-op */

    cf_user_vector vector = {0};
    vector.items = malloc(2 * sizeof *vector.items);
    CF_REQUIRE(vector.items != NULL);
    memset(&vector.items[0], 0, sizeof vector.items[0]);
    vector.items[0].name.ptr = malloc(2);
    CF_REQUIRE(vector.items[0].name.ptr != NULL);
    memcpy(vector.items[0].name.ptr, "x", 2);
    vector.items[0].name.len = 1;
    vector.len = 1;
    vector.cap = 2;
    cf_user_vector_dispose(&vector);
    CF_CHECK(vector.items == NULL && vector.len == 0 && vector.cap == 0);
}

CF_TEST_MAIN()
