/* A02 rooms/messages presenters: one read transaction over a scratch
 * database (raw SQL is the reference's own schema), mapping rows into the
 * view models and (for rooms#show) the signed stream name.
 *
 * This binary contains its own CF_TEST_MAIN(): it links the application
 * library the way tests/views/test_presenters.c does.
 */
#include "cf_test.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "models/account.h"
#include "models/message.h"
#include "models/room.h"
#include "models/user.h"
#include "richtext.h"
#include "storage/active_storage.h"
#include "views.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define ORIGIN "http://campfire.test"

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
        fprintf(stderr, "  sql failed: %s\n  %s\n",
                message != NULL ? message : "?", sql);
        sqlite3_free(message);
    }
    CF_REQUIRE(rc == SQLITE_OK);
}

typedef struct {
    cf_app *app;
    cf_response response;
    cf_request request;
    cf_ctx ctx;
} fixture;

static void fixture_init(fixture *f, cf_db *db) {
    memset(f, 0, sizeof *f);
    f->app = make_app();
    f->request.method = CF_GET;
    f->request.path = (cf_span){(const unsigned char *)"/rooms/10", 9};
    f->request.target = f->request.path;
    cf_response_init(&f->response);
    CF_REQUIRE(cf_ctx_create(&f->ctx, f->app, db, &f->request,
                             &f->response) == CF_OK);
    f->ctx.identity.kind = CF_AUTH_SESSION;
    f->ctx.identity.user_id = 1;
}

static void fixture_dispose(fixture *f) {
    cf_ctx_destroy(&f->ctx);
    cf_response_dispose(&f->response);
    cf_app_destroy(f->app);
}

/* room 10 (closed, HQ) with David (1, administrator) and JZ (2) as members;
 * messages 100 (David, "Hello <strong>world</strong>") and 101 (JZ, "👍");
 * a 🎉 boost by JZ on 100; the account and its join code. */
static void seed(cf_db *db) {
    exec_sql(db,
             "INSERT INTO accounts (id, created_at, custom_styles, join_code, "
             "name, settings, singleton_guard, updated_at) VALUES "
             "(1, '2026-09-26 13:00:20', NULL, 'CRMu-l8Ge-KB9B', "
             "'37signals', NULL, 0, '2026-09-26 13:00:20')");
    exec_sql(db,
             "INSERT INTO users (id, created_at, email_address, name, role, "
             "status, updated_at) VALUES "
             "(1, '2026-09-26 13:00:20', 'david@37signals.com', "
             "'David', 1, 0, '2026-09-26 13:00:20')");
    exec_sql(db,
             "INSERT INTO users (id, created_at, email_address, name, role, "
             "status, updated_at) VALUES "
             "(2, '2026-09-26 13:00:20', 'jz@37signals.com', 'JZ', 0, "
             "0, '2026-09-26 13:00:20')");
    exec_sql(db,
             "INSERT INTO rooms (id, created_at, creator_id, name, type, "
             "updated_at) VALUES (10, '2026-09-26 13:00:20', 1, 'HQ', "
             "'Rooms::Closed', '2026-09-26 13:00:20')");
    exec_sql(db,
             "INSERT INTO memberships (id, created_at, room_id, updated_at, "
             "user_id) VALUES (20, '2026-09-26 13:00:20', 10, "
             "'2026-09-26 13:00:20', 1)");
    exec_sql(db,
             "INSERT INTO memberships (id, created_at, room_id, updated_at, "
             "user_id) VALUES (21, '2026-09-26 13:00:20', 10, "
             "'2026-09-26 13:00:20', 2)");
    exec_sql(db,
             "INSERT INTO messages (id, client_message_id, created_at, "
             "creator_id, room_id, updated_at) VALUES "
             "(100, 'm1', '2026-09-26 13:01:00', 1, 10, "
             "'2026-09-26 13:01:00')");
    exec_sql(db,
             "INSERT INTO messages (id, client_message_id, created_at, "
             "creator_id, room_id, updated_at) VALUES "
             "(101, 'm2', '2026-09-26 13:02:00', 2, 10, "
             "'2026-09-26 13:02:00')");
    exec_sql(db,
             "INSERT INTO action_text_rich_texts (id, body, created_at, name, "
             "record_id, record_type, updated_at) VALUES "
             "(1, '<div>Hello <strong>world</strong></div>', "
             "'2026-09-26 13:01:00', 'body', 100, 'Message', "
             "'2026-09-26 13:01:00')");
    exec_sql(db,
             "INSERT INTO action_text_rich_texts (id, body, created_at, name, "
             "record_id, record_type, updated_at) VALUES "
             "(2, '\xF0\x9F\x91\x8D', '2026-09-26 13:02:00', 'body', "
             "101, 'Message', '2026-09-26 13:02:00')");
    /* A variation-selector emoji and a mixed text (the other all_emoji
     * shapes). */
    exec_sql(db,
             "INSERT INTO messages (id, client_message_id, created_at, "
             "creator_id, room_id, updated_at) VALUES "
             "(103, 'm3', '2026-09-26 13:02:30', 1, 10, "
             "'2026-09-26 13:02:30')");
    exec_sql(db,
             "INSERT INTO action_text_rich_texts (id, body, created_at, name, "
             "record_id, record_type, updated_at) VALUES "
             "(3, '\xE2\x9D\xA4\xEF\xB8\x8F', '2026-09-26 13:02:30', "
             "'body', 103, 'Message', '2026-09-26 13:02:30')");
    exec_sql(db,
             "INSERT INTO messages (id, client_message_id, created_at, "
             "creator_id, room_id, updated_at) VALUES "
             "(104, 'm4', '2026-09-26 13:02:40', 1, 10, "
             "'2026-09-26 13:02:40')");
    exec_sql(db,
             "INSERT INTO action_text_rich_texts (id, body, created_at, name, "
             "record_id, record_type, updated_at) VALUES "
             "(4, 'hi \xF0\x9F\x91\x8D', '2026-09-26 13:02:40', 'body', "
             "104, 'Message', '2026-09-26 13:02:40')");
    exec_sql(db,
             "INSERT INTO boosts (id, content, created_at, message_id, "
             "booster_id, updated_at) VALUES "
             "(30, '\xF0\x9F\x8E\x89', '2026-09-26 13:03:00', 100, 2, "
             "'2026-09-26 13:03:00')");
}

CF_TEST(presenter_user_view_maps_name_title_and_avatar) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed(scratch.db);
    fixture f;
    fixture_init(&f, scratch.db);

    cf_user user = {0};
    CF_REQUIRE(cf_user_find(scratch.db, 2, &user) == CF_OK);
    cf_view_user view = {0};
    CF_REQUIRE(cf_presenter_user_view(&f.ctx, &user, &view) == CF_OK);
    CF_CHECK(view.id == 2);
    CF_CHECK(strcmp(view.name.ptr, "JZ") == 0);
    CF_CHECK(strcmp(view.title.ptr, "JZ") == 0);
    CF_CHECK(strncmp(view.avatar_url.ptr, "/users/", 7) == 0);
    CF_CHECK(strstr(view.avatar_url.ptr, "/avatar?v=20260926130020") != NULL);
    cf_view_user_dispose(&view);
    cf_user_dispose(&user);
    fixture_dispose(&f);
    cf_db_scratch_close(&scratch);
}

CF_TEST(presenter_message_maps_text_content_boosts_and_emoji) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed(scratch.db);
    cf_richtext_configure((cf_span){(const unsigned char *)HEX64, 64});
    fixture f;
    fixture_init(&f, scratch.db);

    cf_message row = {0};
    CF_REQUIRE(cf_message_find(scratch.db, 100, &row) == CF_OK);
    cf_view_message view = {0};
    CF_REQUIRE(cf_presenter_message(&f.ctx, &row, &view) == CF_OK);
    CF_CHECK(view.id == 100);
    CF_CHECK(strcmp(view.client_message_id.ptr, "m1") == 0);
    CF_CHECK(view.room_id == 10);
    CF_CHECK(strcmp(view.room_name.ptr, "HQ") == 0);
    CF_CHECK(strcmp(view.creator.name.ptr, "David") == 0);
    CF_CHECK(!view.all_emoji);
    CF_CHECK(view.content_kind == CF_VIEW_CONTENT_TEXT);
    CF_CHECK(strstr(view.text_html.ptr, "Hello") != NULL);
    CF_REQUIRE(view.boosts.len == 1);
    CF_CHECK(view.boosts.items[0].id == 30);
    CF_CHECK(strcmp(view.boosts.items[0].content.ptr, "\xF0\x9F\x8E\x89") == 0);
    CF_CHECK(view.boosts.items[0].all_emoji);
    CF_CHECK(strcmp(view.boosts.items[0].booster.name.ptr, "JZ") == 0);
    cf_view_message_dispose(&view);

    /* A body of only emoji is all_emoji. */
    cf_message emoji_row = {0};
    CF_REQUIRE(cf_message_find(scratch.db, 101, &emoji_row) == CF_OK);
    CF_REQUIRE(cf_presenter_message(&f.ctx, &emoji_row, &view) == CF_OK);
    CF_CHECK(view.all_emoji);
    CF_CHECK(view.content_kind == CF_VIEW_CONTENT_TEXT);
    cf_view_message_dispose(&view);

    /* U+2764 U+FE0F: the heart alone is not Emoji_Presentation; the
     * variation selector makes the whole string emoji. */
    cf_message heart_row = {0};
    CF_REQUIRE(cf_message_find(scratch.db, 103, &heart_row) == CF_OK);
    CF_REQUIRE(cf_presenter_message(&f.ctx, &heart_row, &view) == CF_OK);
    CF_CHECK(view.all_emoji);
    cf_view_message_dispose(&view);
    cf_message_dispose(&heart_row);

    /* Emoji mixed with text is not all-emoji. */
    cf_message mixed_row = {0};
    CF_REQUIRE(cf_message_find(scratch.db, 104, &mixed_row) == CF_OK);
    CF_REQUIRE(cf_presenter_message(&f.ctx, &mixed_row, &view) == CF_OK);
    CF_CHECK(!view.all_emoji);
    cf_view_message_dispose(&view);
    cf_message_dispose(&mixed_row);
    cf_message_dispose(&emoji_row);
    cf_message_dispose(&row);
    fixture_dispose(&f);
    cf_db_scratch_close(&scratch);
}

CF_TEST(presenter_messages_maps_a_page_in_order) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed(scratch.db);
    cf_richtext_configure((cf_span){(const unsigned char *)HEX64, 64});
    fixture f;
    fixture_init(&f, scratch.db);

    cf_message_vector rows = {0};
    CF_REQUIRE(cf_message_for_room(scratch.db, 10, &rows) == CF_OK);
    cf_view_message_item_vector items = {0};
    CF_REQUIRE(cf_presenter_messages(&f.ctx, rows.items, rows.len, &items) ==
               CF_OK);
    CF_REQUIRE(items.len == 4);
    CF_CHECK(items.items[0].message.id == 100);
    CF_CHECK(items.items[0].message.boosts.len == 1);
    CF_CHECK(items.items[1].message.id == 101);
    CF_CHECK(items.items[1].message.all_emoji);
    CF_CHECK(items.items[2].message.id == 103);
    CF_CHECK(items.items[2].message.all_emoji);
    CF_CHECK(items.items[3].message.id == 104);
    CF_CHECK(!items.items[3].message.all_emoji);
    cf_view_message_item_vector_dispose(&items);
    cf_message_vector_dispose(&rows);
    fixture_dispose(&f);
    cf_db_scratch_close(&scratch);
}

CF_TEST(presenter_message_edit_maps_the_editable_body) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed(scratch.db);
    cf_richtext_configure((cf_span){(const unsigned char *)HEX64, 64});
    fixture f;
    fixture_init(&f, scratch.db);

    cf_message row = {0};
    CF_REQUIRE(cf_message_find(scratch.db, 100, &row) == CF_OK);
    cf_view_message_edit_model edit = {0};
    CF_REQUIRE(cf_presenter_message_edit(&f.ctx, &row, &edit) == CF_OK);
    CF_CHECK(edit.message.id == 100);
    CF_CHECK(strstr(edit.editable_body_html.ptr, "Hello") != NULL);
    CF_CHECK(strstr(edit.editable_body_html.ptr, "<strong>world</strong>") !=
             NULL);
    cf_view_message_edit_model_dispose(&edit);
    cf_message_dispose(&row);
    fixture_dispose(&f);
    cf_db_scratch_close(&scratch);
}

CF_TEST(presenter_room_show_maps_the_page_and_signed_stream) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed(scratch.db);
    cf_richtext_configure((cf_span){(const unsigned char *)HEX64, 64});
    fixture f;
    fixture_init(&f, scratch.db);

    cf_room room = {0};
    CF_REQUIRE(cf_room_find(scratch.db, 10, &room) == CF_OK);
    cf_user user = {0};
    CF_REQUIRE(cf_user_find(scratch.db, 1, &user) == CF_OK);
    cf_view_room_show_model show = {0};
    CF_REQUIRE(cf_presenter_room_show(&f.ctx, &room, &user, false, 0, &show) ==
               CF_OK);
    CF_CHECK(show.room.id == 10);
    CF_CHECK(show.room.kind == CF_ROOM_CLOSED);
    CF_CHECK(strcmp(show.room.display_name.ptr, "HQ") == 0);
    CF_CHECK(strcmp(show.user.name.ptr, "David") == 0);
    CF_REQUIRE(show.messages.len == 4);
    CF_CHECK(show.messages.items[0].message.id == 100);
    /* the original room with a single page: the invitation renders. */
    CF_CHECK(show.invitation);
    CF_CHECK(strcmp(show.join_code.ptr, "CRMu-l8Ge-KB9B") == 0);

    /* The signed stream name is `Turbo::StreamsChannel.signed_stream_name(
     * [room_gid.to_param, "messages"])`. */
    cf_str param = {0};
    const char *gid = "gid://campfire/Rooms::Closed/10";
    CF_REQUIRE(cf_auth_global_id_param(
                   (cf_span){(const unsigned char *)gid, strlen(gid)},
                   &param) == CF_OK);
    char expected[128];
    snprintf(expected, sizeof expected, "%s:messages", param.ptr);
    cf_str verified = {0};
    bool found = false;
    CF_REQUIRE(cf_auth_turbo_verified_stream_name(
                   (cf_span){(const unsigned char *)HEX64, 64},
                   (cf_span){(const unsigned char *)show.messages_stream_name.ptr,
                             show.messages_stream_name.len},
                   &verified, &found) == CF_OK);
    CF_CHECK(found);
    CF_CHECK(strcmp(verified.ptr, expected) == 0);
    cf_str_dispose(&verified);
    cf_str_dispose(&param);

    /* The page around a message id in this room, and the last page otherwise. */
    cf_view_room_show_model around = {0};
    CF_REQUIRE(cf_presenter_room_show(&f.ctx, &room, &user, true, 100,
                                      &around) == CF_OK);
    CF_REQUIRE(around.messages.len == 4);
    CF_CHECK(around.messages.items[0].message.id == 100);
    CF_CHECK(around.messages.items[3].message.id == 104);
    cf_view_room_show_model_dispose(&around);
    cf_view_room_show_model_dispose(&show);
    cf_user_dispose(&user);
    cf_room_dispose(&room);
    fixture_dispose(&f);
    cf_db_scratch_close(&scratch);
}

CF_TEST(presenter_message_with_a_missing_creator_is_unrenderable) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed(scratch.db);
    cf_richtext_configure((cf_span){(const unsigned char *)HEX64, 64});
    fixture f;
    fixture_init(&f, scratch.db);

    /* A message whose creator row is gone (the reference's rescue path);
     * the FK is waived for this row on purpose. */
    exec_sql(scratch.db, "PRAGMA foreign_keys=OFF");
    exec_sql(scratch.db,
             "INSERT INTO messages (id, client_message_id, created_at, "
             "creator_id, room_id, updated_at) VALUES "
             "(102, 'gone', '2026-09-26 13:03:00', 99, 10, "
             "'2026-09-26 13:03:00')");
    exec_sql(scratch.db, "PRAGMA foreign_keys=ON");
    cf_message row = {0};
    CF_REQUIRE(cf_message_find(scratch.db, 102, &row) == CF_OK);
    cf_view_message view = {0};
    CF_REQUIRE(cf_presenter_message(&f.ctx, &row, &view) == CF_OK);
    CF_CHECK(view.content_kind == CF_VIEW_CONTENT_UNRENDERABLE);
    CF_CHECK(!view.all_emoji);
    CF_CHECK(view.creator.id == 99);
    CF_CHECK(strcmp(view.room_name.ptr, "HQ") == 0);
    cf_view_message_dispose(&view);
    cf_message_dispose(&row);
    fixture_dispose(&f);
    cf_db_scratch_close(&scratch);
}

/* The S02 attachment arm: the presenter builds the signed blob/representation
 * paths (never inventing them) and the preview arm follows the content type.
 * The expected paths are computed with the same S02 signing helpers the
 * presenter calls, so this pins the wiring (sanitized filename, disposition
 * query, variation order) rather than re-deriving the signature here. */
CF_TEST(presenter_message_attachment_view) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed(scratch.db);
    cf_richtext_configure((cf_span){(const unsigned char *)HEX64, 64});
    fixture f;
    fixture_init(&f, scratch.db);
    cf_span secret = {(const unsigned char *)HEX64, 64};
    /* `weird name.png` exercises the sanitizer (" " stays, "%" would map). */
    exec_sql(scratch.db,
             "INSERT INTO active_storage_blobs (id, byte_size, checksum, "
             "content_type, created_at, filename, key, metadata, service_name) "
             "VALUES (1, 10, NULL, 'image/png', '2026-09-26 "
             "13:00:20', 'weird name.png', 'key1', NULL, 'local')");
    exec_sql(scratch.db,
             "INSERT INTO active_storage_attachments (id, blob_id, "
             "created_at, name, record_id, record_type) VALUES "
             "(1, 1, '2026-09-26 13:00:20', 'attachment', 100, "
             "'Message')");
    cf_message row = {0};
    CF_REQUIRE(cf_message_find(scratch.db, 100, &row) == CF_OK);
    cf_view_message view = {0};
    CF_REQUIRE(cf_presenter_message(&f.ctx, &row, &view) == CF_OK);
    CF_REQUIRE(view.content_kind == CF_VIEW_CONTENT_ATTACHMENT);
    const cf_view_attachment *att = &view.attachment;
    CF_CHECK(att->filename.len == 14 &&
             memcmp(att->filename.ptr, "weird name.png", 14) == 0);
    CF_CHECK(att->preview == CF_VIEW_PREVIEW_IMAGE);
    CF_CHECK(!att->has_width && !att->has_height);

    char expected[1024];
    cf_builder path = {0};
    CF_REQUIRE(cf_active_blob_redirect_path(
                   secret, 1, (cf_span){(const unsigned char *)att->filename.ptr, att->filename.len},
                   (cf_span){NULL, 0}, false, &path) == CF_OK);
    snprintf(expected, sizeof expected, "%.*s", (int)path.len, path.ptr);
    CF_CHECK(att->blob_path.len == path.len &&
             memcmp(att->blob_path.ptr, path.ptr, path.len) == 0);
    cf_builder_dispose(&path);
    CF_REQUIRE(cf_active_blob_redirect_path(
                   secret, 1, (cf_span){(const unsigned char *)att->filename.ptr, att->filename.len},
                   (cf_span){(const unsigned char *)"attachment", 10}, true,
                   &path) == CF_OK);
    CF_CHECK(att->download_path.len == path.len &&
             memcmp(att->download_path.ptr, path.ptr, path.len) == 0);
    cf_builder_dispose(&path);
    /* The thumb variation: resize_to_limit 1200x800 defaulted with `format` =
     * png (web image, extension matches), format first. */
    cf_active_ventries variation = {0};
    cf_active_vval *resize = NULL;
    cf_active_vval *dims = NULL;
    CF_REQUIRE(cf_active_varr(&dims) == CF_OK);
    cf_active_vval *w = NULL, *h = NULL;
    CF_REQUIRE(cf_active_vint(1200, &w) == CF_OK);
    CF_REQUIRE(cf_active_vint(800, &h) == CF_OK);
    CF_REQUIRE(cf_active_varr_push(dims, w) == CF_OK);
    CF_REQUIRE(cf_active_varr_push(dims, h) == CF_OK);
    CF_REQUIRE(cf_active_vstr((cf_span){(const unsigned char *)"png", 3},
                              &resize) == CF_OK);
    CF_REQUIRE(cf_active_ventries_push(
                   &variation, (cf_span){(const unsigned char *)"format", 6},
                   resize) == CF_OK);
    CF_REQUIRE(cf_active_ventries_push(
                   &variation,
                   (cf_span){(const unsigned char *)"resize_to_limit", 15},
                   dims) == CF_OK);
    CF_REQUIRE(cf_active_representation_redirect_path(
                   secret, 1,
                   (cf_span){(const unsigned char *)"weird name.png", 14},
                   &variation, &path) == CF_OK);
    CF_CHECK(att->preview_url.len == path.len &&
             memcmp(att->preview_url.ptr, path.ptr, path.len) == 0);
    cf_builder_dispose(&path);
    cf_active_ventries_dispose(&variation);
    cf_view_message_dispose(&view);
    cf_message_dispose(&row);

    /* A plain-text blob renders the File arm (no preview URL). */
    exec_sql(scratch.db,
             "UPDATE active_storage_blobs SET content_type = 'text/plain', "
             "filename = 'notes.txt' WHERE id = 1");
    CF_REQUIRE(cf_message_find(scratch.db, 100, &row) == CF_OK);
    CF_REQUIRE(cf_presenter_message(&f.ctx, &row, &view) == CF_OK);
    CF_REQUIRE(view.content_kind == CF_VIEW_CONTENT_ATTACHMENT);
    CF_CHECK(view.attachment.preview == CF_VIEW_PREVIEW_FILE);
    CF_CHECK(view.attachment.preview_url.len == 0);
    CF_CHECK(view.attachment.filename.len == 9 &&
             memcmp(view.attachment.filename.ptr, "notes.txt", 9) == 0);
    cf_view_message_dispose(&view);
    cf_message_dispose(&row);

    fixture_dispose(&f);
    cf_db_scratch_close(&scratch);
}

CF_TEST_MAIN()
