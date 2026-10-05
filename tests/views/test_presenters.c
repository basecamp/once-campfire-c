/* A02 foundation: the view presenters (crates/campfire/src/controllers/
 * presenters/{view_context,accounts}.rs).
 *
 * Rows are inserted through raw SQL on a scratch database (the reference's
 * SQL is the oracle); the presenter reads them inside one read transaction
 * and the render context copies request state.  Rendering itself must not
 * touch the database: the render cases in the other views tests run without
 * one.
 */
#include "cf_test.h"

#include "app.h"
#include "cf.h"
#include "config.h"
#include "context.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "http/params.h"
#include "models/account.h"
#include "models/user.h"
#include "views.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define ORIGIN "http://campfire.test"

/* --- scratch app + database ------------------------------------------------ */

static cf_config *make_config(void) {
    cf_config_entry entries[2] = {
        {"PUBLIC_ORIGIN", ORIGIN},
        {"SECRET_KEY_BASE", HEX64},
    };
    cf_config *config = NULL;
    CF_REQUIRE(cf_config_parse(entries, 2, NULL, &config) == CF_OK);
    return config;
}

static cf_app *make_app(void) {
    cf_config *config = make_config();
    cf_app *app = NULL;
    CF_REQUIRE(cf_app_create(config, &app) == CF_OK);
    return app;
}

static void exec_sql(cf_db *db, const char *sql) {
    char *message = NULL;
    int rc = sqlite3_exec(cf_db_handle(db), sql, NULL, NULL, &message);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  sql failed: %s\n  %s\n", message != NULL ? message
                                                                   : "?",
                sql);
        sqlite3_free(message);
    }
    CF_REQUIRE(rc == SQLITE_OK);
}

static void time_text(int64_t us, char out[CF_DB_TIME_TEXT_CAP]) {
    CF_REQUIRE(cf_db_time_to_text(us, out) == CF_OK);
}

/* The pinned facts' timestamps: 2026-09-26 13:00:20.000000 UTC. */
#define T1_US INT64_C(1790427620000000)
#define T2_US INT64_C(1790427630000000)

static void seed_account_and_users(cf_db *db) {
    char t1[CF_DB_TIME_TEXT_CAP];
    time_text(T1_US, t1);
    exec_sql(db,
             "INSERT INTO accounts (id, created_at, custom_styles, join_code, "
             "name, settings, singleton_guard, updated_at) VALUES "
             "(1, '2026-09-26 13:00:20.000000', 'body { --x: 1; }', "
             "'CRMu-l8Ge-KB9B', '37signals', NULL, 0, "
             "'2026-09-26 13:00:20.000000')");
    exec_sql(db,
             "INSERT INTO users (id, created_at, email_address, name, role, "
             "status, updated_at) VALUES "
             "(1, '2026-09-26 13:00:20.000000', 'david@37signals.com', "
             "'David', 1, 0, '2026-09-26 13:00:20.000000')");
    exec_sql(db,
             "INSERT INTO users (id, created_at, email_address, name, role, "
             "status, updated_at) VALUES "
             "(2, '2026-09-26 13:00:20.000000', 'jz@37signals.com', 'JZ', 0, "
             "0, '2026-09-26 13:00:20.000000')");
    /* A second administrator with a NULL email address. */
    exec_sql(db,
             "INSERT INTO users (id, created_at, email_address, name, role, "
             "status, updated_at) VALUES "
             "(3, '2026-09-26 13:00:20.000000', NULL, 'Ada', 1, 0, "
             "'2026-09-26 13:00:20.000000')");
    exec_sql(db,
             "INSERT INTO active_storage_blobs (id, byte_size, checksum, "
             "content_type, created_at, filename, key, metadata, service_name) "
             "VALUES (1, 10, NULL, 'image/png', '2026-09-26 "
             "13:00:20.000000', 'logo.png', 'key1', NULL, 'local')");
    exec_sql(db,
             "INSERT INTO active_storage_attachments (id, blob_id, "
             "created_at, name, record_id, record_type) VALUES "
             "(1, 1, '2026-09-26 13:00:20.000000', 'logo', 1, 'Account')");
    exec_sql(db,
             "INSERT INTO rooms (id, created_at, creator_id, name, type, "
             "updated_at) VALUES (10, '2026-09-26 13:00:20.000000', 1, 'All "
             "Talk', 'Rooms::Open', '2026-09-26 13:00:20.000000')");
    exec_sql(db,
             "INSERT INTO rooms (id, created_at, creator_id, name, type, "
             "updated_at) VALUES (11, '2026-09-26 13:00:20.000000', 1, 'HQ', "
             "'Rooms::Closed', '2026-09-26 13:00:20.000000')");
    exec_sql(db,
             "INSERT INTO memberships (id, created_at, room_id, updated_at, "
             "user_id) VALUES (20, '2026-09-26 13:00:20.000000', 10, "
             "'2026-09-26 13:00:20.000000', 1)");
    exec_sql(db,
             "INSERT INTO memberships (id, created_at, room_id, updated_at, "
             "user_id) VALUES (21, '2026-09-26 13:00:20.000000', 11, "
             "'2026-09-26 13:00:20.000000', 1)");
}

/* A context for `path` with an optional Cookie header; identity is set by
 * the caller. */
typedef struct {
    cf_app *app;
    cf_response response;
    cf_request request;
    cf_ctx ctx;
} presenter_fixture;

static void fixture_init(presenter_fixture *fixture, cf_db *db,
                         const char *path, const char *cookie) {
    memset(fixture, 0, sizeof *fixture);
    fixture->app = make_app();
    fixture->request.method = CF_GET;
    fixture->request.path = (cf_span){(const unsigned char *)path,
                                      strlen(path)};
    fixture->request.target = fixture->request.path;
    if (cookie != NULL) {
        fixture->request.headers[0].name =
            (cf_span){(const unsigned char *)"Cookie", 6};
        fixture->request.headers[0].value =
            (cf_span){(const unsigned char *)cookie, strlen(cookie)};
        fixture->request.header_count = 1;
    }
    cf_response_init(&fixture->response);
    CF_REQUIRE(cf_ctx_create(&fixture->ctx, fixture->app, db,
                             &fixture->request,
                             &fixture->response) == CF_OK);
}

static void fixture_dispose(presenter_fixture *fixture) {
    cf_ctx_destroy(&fixture->ctx);
    cf_response_dispose(&fixture->response);
    cf_app_destroy(fixture->app);
}

/* --- help_contact / no_users ----------------------------------------------- */

CF_TEST(presenter_help_contact_picks_the_first_administrator) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_account_and_users(scratch.db);

    bool found = false;
    cf_view_help_contact contact = {0};
    CF_REQUIRE(cf_presenter_help_contact(scratch.db, &found, &contact) ==
               CF_OK);
    CF_CHECK(found);
    CF_CHECK(strcmp(contact.name.ptr, "David") == 0);
    CF_CHECK(strcmp(contact.email_address.ptr, "david@37signals.com") == 0);
    cf_view_help_contact_dispose(&contact);
    cf_view_help_contact_dispose(&contact); /* second dispose is a no-op */

    /* With only the NULL-email administrator, the email is empty, not NULL. */
    exec_sql(scratch.db, "DELETE FROM users WHERE id = 1");
    found = false;
    contact = (cf_view_help_contact){0};
    CF_REQUIRE(cf_presenter_help_contact(scratch.db, &found, &contact) ==
               CF_OK);
    CF_CHECK(found);
    CF_CHECK(strcmp(contact.name.ptr, "Ada") == 0);
    CF_CHECK(contact.email_address.len == 0 &&
             contact.email_address.ptr != NULL &&
             contact.email_address.ptr[0] == '\0');
    cf_view_help_contact_dispose(&contact);
    cf_db_scratch_close(&scratch);
}

CF_TEST(presenter_help_contact_absent_without_administrators) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    bool found = true;
    cf_view_help_contact contact = {0};
    CF_REQUIRE(cf_presenter_help_contact(scratch.db, &found, &contact) ==
               CF_OK);
    CF_CHECK(!found);
    CF_CHECK(contact.name.ptr == NULL);
    cf_db_scratch_close(&scratch);
}

CF_TEST(presenter_no_users_counts_rows) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    bool none = false;
    CF_REQUIRE(cf_presenter_no_users(scratch.db, &none) == CF_OK);
    CF_CHECK(none);
    seed_account_and_users(scratch.db);
    none = true;
    CF_REQUIRE(cf_presenter_no_users(scratch.db, &none) == CF_OK);
    CF_CHECK(!none);
    cf_db_scratch_close(&scratch);
}

/* --- Layout::load ----------------------------------------------------------- */

CF_TEST(presenter_layout_load_maps_rows) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_account_and_users(scratch.db);

    presenter_fixture fixture;
    fixture_init(&fixture, scratch.db, "/", NULL);
    fixture.ctx.identity.kind = CF_AUTH_SESSION;
    fixture.ctx.identity.user_id = 1;

    cf_view_platform platform = {0};
    platform.apple_messages = true;
    cf_view_layout_model layout = {0};
    CF_REQUIRE(cf_presenter_layout_load(&fixture.ctx, &platform, &layout) ==
               CF_OK);

    CF_CHECK(layout.current_user.has_user);
    CF_CHECK(layout.current_user.id == 1);
    CF_CHECK(strcmp(layout.current_user.name.ptr, "David") == 0);
    CF_CHECK(layout.current_user.administrator);
    CF_CHECK(!layout.current_user.bot);
    /* fresh_user_avatar_path: signed token + %Y%m%d%H%M%S updated_at. */
    CF_CHECK(strncmp(layout.current_user.avatar_url.ptr, "/users/", 7) == 0);
    CF_CHECK(strstr(layout.current_user.avatar_url.ptr,
                    "/avatar?v=20260926130020") != NULL);

    CF_CHECK(strcmp(layout.account.name.ptr, "37signals") == 0);
    CF_CHECK(strcmp(layout.account.logo_url.ptr,
                    "/account/logo?v=20260926130020") == 0);
    CF_CHECK(layout.account.has_logo);
    CF_CHECK(layout.has_custom_styles);
    CF_CHECK(strcmp(layout.custom_styles.ptr, "body { --x: 1; }") == 0);
    CF_CHECK(layout.platform.apple_messages);
    /* No last_room cookie: Current.user.rooms.original (the oldest room). */
    CF_CHECK(layout.has_last_room_visited);
    CF_CHECK(layout.last_room_visited_id == 10);
    /* No VAPID key configured; app version falls back to the build value. */
    CF_CHECK(!layout.has_vapid_public_key);
    CF_CHECK(strcmp(layout.app_version.ptr, CF_VIEWS_APP_VERSION) == 0);

    cf_view_layout_model_dispose(&layout);
    cf_view_layout_model_dispose(&layout); /* idempotent */
    fixture_dispose(&fixture);
    cf_db_scratch_close(&scratch);
}

CF_TEST(presenter_layout_load_honours_the_last_room_cookie) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_account_and_users(scratch.db);

    /* A member room from the cookie wins. */
    presenter_fixture fixture;
    fixture_init(&fixture, scratch.db, "/", "last_room=11");
    fixture.ctx.identity.kind = CF_AUTH_SESSION;
    fixture.ctx.identity.user_id = 1;
    cf_view_layout_model layout = {0};
    CF_REQUIRE(cf_presenter_layout_load(&fixture.ctx, NULL, &layout) == CF_OK);
    CF_CHECK(layout.has_last_room_visited && layout.last_room_visited_id == 11);
    CF_CHECK(!layout.platform.apple_messages);
    cf_view_layout_model_dispose(&layout);
    fixture_dispose(&fixture);

    /* A room the user is not in falls back to the original room. */
    presenter_fixture nonmember;
    fixture_init(&nonmember, scratch.db, "/", "last_room=99");
    nonmember.ctx.identity.kind = CF_AUTH_SESSION;
    nonmember.ctx.identity.user_id = 1;
    layout = (cf_view_layout_model){0};
    CF_REQUIRE(cf_presenter_layout_load(&nonmember.ctx, NULL, &layout) ==
               CF_OK);
    CF_CHECK(layout.has_last_room_visited && layout.last_room_visited_id == 10);
    cf_view_layout_model_dispose(&layout);
    fixture_dispose(&nonmember);

    /* An unauthenticated request has no user and no last room. */
    presenter_fixture anonymous;
    fixture_init(&anonymous, scratch.db, "/", NULL);
    layout = (cf_view_layout_model){0};
    CF_REQUIRE(cf_presenter_layout_load(&anonymous.ctx, NULL, &layout) ==
               CF_OK);
    CF_CHECK(!layout.current_user.has_user);
    CF_CHECK(!layout.has_last_room_visited);
    CF_CHECK(strcmp(layout.account.name.ptr, "37signals") == 0);
    cf_view_layout_model_dispose(&layout);
    fixture_dispose(&anonymous);
    cf_db_scratch_close(&scratch);
}

/* The verifier's member-room-2 / original-room-9 scenario: room 9 is the
 * oldest membership (the original room), rooms 2 and 5 are newer member rooms.
 * seed_account_and_users supplies the account and user 1; rooms 9/2/5 predate
 * its rooms 10/11 so the original room is deterministic (oldest created_at). */
static void seed_cast_rooms(cf_db *db) {
    static const struct {
        int64_t id;
        const char *name;
        const char *created_at;
        int64_t membership_id;
    } rows[] = {
        {9, "All Talk", "2026-01-01 00:00:00.000000", 201},
        {2, "HQ", "2026-02-01 00:00:00.000000", 202},
        {5, "Lab", "2026-03-01 00:00:00.000000", 203},
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        char sql[512];
        snprintf(sql, sizeof sql,
                 "INSERT INTO rooms (id, created_at, creator_id, name, type, "
                 "updated_at) VALUES (%lld, '%s', 1, '%s', 'Rooms::Open', "
                 "'%s')",
                 (long long)rows[i].id, rows[i].created_at, rows[i].name,
                 rows[i].created_at);
        exec_sql(db, sql);
        snprintf(sql, sizeof sql,
                 "INSERT INTO memberships (id, created_at, room_id, updated_at, "
                 "user_id) VALUES (%lld, '2026-09-26 13:00:20.000000', %lld, "
                 "'2026-09-26 13:00:20.000000', 1)",
                 (long long)rows[i].membership_id, (long long)rows[i].id);
        exec_sql(db, sql);
    }
}

/* The layout presenter's last_room cookie cast must be the same
 * ruby_compat::integer_cast the welcome action pins (welcome_test.c's
 * `welcome_last_room_cookie_matches_the_pinned_integer_cast`): trim Ruby
 * ISSPACE (space, tab, LF, VT, FF, CR), one optional sign, then an integer
 * (`0d`/`0D` prefix, one `_` between two digits, stop at the first other
 * byte, nil out of i64 range).  Both paths now call the shared
 * cf_views_integer_cast; before the dedup, layout.c's older cast selected
 * room 9 for `0_2`/`0d2`/`0D2`/VT+2/FF+2/`0_5`/`0d5`/`0d0_5` and room 2 for
 * `2_5` where the action selected rooms 2/5/9.  Same 23 rows as welcome_test. */
CF_TEST(presenter_layout_last_room_cookie_matches_the_pinned_integer_cast) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_account_and_users(scratch.db);
    seed_cast_rooms(scratch.db);

    static const struct cast_case {
        const char *cookie;
        int64_t room_id;
    } cases[] = {
        {"last_room=2", 2},
        {"last_room=+2", 2},
        {"last_room=0002", 2},
        {"last_room=2abc", 2},
        {"last_room=0_2", 2},      /* '_' between two digits */
        {"last_room=0d2", 2},      /* String#to_i 0d prefix */
        {"last_room=0D2", 2},
        {"last_room=\x0b" "2", 2}, /* VT is Ruby ISSPACE */
        {"last_room=\x0c" "2", 2}, /* FF is Ruby ISSPACE */
        {"last_room=0_5", 5},
        {"last_room=0d5", 5},
        {"last_room=0d0_5", 5},  /* 0d prefix, then 0_5 -> 5 */
        {"last_room=5__6", 5},   /* a second '_' ends the number */
        {"last_room=2_5", 9},    /* parses 25 -> not a member */
        {"last_room=0x5", 9},    /* parses 0 -> not a member */
        {"last_room=abc", 9},
        {"last_room=", 9},
        {"last_room=9223372036854775808", 9},  /* i64 overflow -> nil */
        {"last_room=99999999999999999999", 9},
        {"last_room=-9223372036854775809", 9},
        {"last_room=\xc2\xa0" "2", 9}, /* NBSP is not ISSPACE */
        {"last_room=--2", 9},
        {"last_room=_2", 9},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        presenter_fixture fixture;
        fixture_init(&fixture, scratch.db, "/", cases[i].cookie);
        fixture.ctx.identity.kind = CF_AUTH_SESSION;
        fixture.ctx.identity.user_id = 1;
        cf_view_layout_model layout = {0};
        CF_REQUIRE(cf_presenter_layout_load(&fixture.ctx, NULL, &layout) ==
                   CF_OK);
        if (!layout.has_last_room_visited ||
            layout.last_room_visited_id != cases[i].room_id) {
            fprintf(stderr,
                    "  cast case %s: expected room %lld, got found=%d id=%lld\n",
                    cases[i].cookie, (long long)cases[i].room_id,
                    layout.has_last_room_visited,
                    (long long)layout.last_room_visited_id);
        }
        CF_CHECK(layout.has_last_room_visited);
        CF_CHECK(layout.last_room_visited_id == cases[i].room_id);
        cf_view_layout_model_dispose(&layout);
        fixture_dispose(&fixture);
    }
    cf_db_scratch_close(&scratch);
}

CF_TEST(presenter_layout_load_without_an_account) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    presenter_fixture fixture;
    fixture_init(&fixture, scratch.db, "/", NULL);
    cf_view_layout_model layout = {0};
    CF_REQUIRE(cf_presenter_layout_load(&fixture.ctx, NULL, &layout) == CF_OK);
    CF_CHECK(layout.account.name.len == 0 && layout.account.name.ptr != NULL);
    CF_CHECK(strcmp(layout.account.logo_url.ptr, "/account/logo") == 0);
    CF_CHECK(!layout.account.has_logo);
    CF_CHECK(!layout.has_custom_styles);
    cf_view_layout_model_dispose(&layout);
    fixture_dispose(&fixture);
    cf_db_scratch_close(&scratch);
}

/* --- cf_view_ctx_init -------------------------------------------------------- */

CF_TEST(view_ctx_init_copies_flash_and_base_url) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    presenter_fixture fixture;
    fixture_init(&fixture, scratch.db, "/", NULL);
    CF_REQUIRE(cf_ctx_flash_set(&fixture.ctx,
                                (cf_span){(const unsigned char *)"notice", 6},
                                (cf_span){(const unsigned char *)"Saved", 5}) ==
               CF_OK);
    CF_REQUIRE(cf_ctx_flash_set(&fixture.ctx,
                                (cf_span){(const unsigned char *)"alert", 5},
                                (cf_span){(const unsigned char *)"Nope", 4}) ==
               CF_OK);

    cf_view_layout_model layout = {0};
    layout.current_user.has_user = true;
    layout.current_user.id = 7;
    layout.app_version = (cf_str){(char *)"0", 1};
    layout.account.logo_url = (cf_str){(char *)"/account/logo", 13};

    cf_view_ctx vc = {0};
    cf_view_ctx_init(&vc, &fixture.ctx, &layout);
    CF_CHECK(vc.has_flash_notice &&
             strncmp((const char *)vc.flash_notice.ptr, "Saved", 5) == 0);
    CF_CHECK(vc.has_flash_alert &&
             strncmp((const char *)vc.flash_alert.ptr, "Nope", 4) == 0);
    CF_CHECK(strncmp((const char *)vc.base_url.ptr, ORIGIN, strlen(ORIGIN)) ==
             0);
    CF_CHECK(vc.current_user.has_user && vc.current_user.id == 7);
    /* Without the asset module configured through the test helper, the tag
     * spans are empty; the initializer never guesses. */
    CF_CHECK(vc.asset_path != NULL);
    fixture_dispose(&fixture);
    cf_db_scratch_close(&scratch);
}

CF_TEST_MAIN()
