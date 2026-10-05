/* A02 rooms family: VIEW-03 goldens for rooms#show (golden/b) and the
 * view-level cases the fixture comparison cannot express.
 *
 * rooms#index has no template in the reference (the action redirects to the
 * last room), so there is nothing to render for it here.
 */
#include "cf_test.h"
#include "support/facts.h"
#include "support/golden.h"
#include "support/golden_b.h"
#include "views.h"

#include <stdlib.h>
#include <string.h>

static cf_span lit(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* Render golden/b/<name> with its own input/context and assert_dom. */
static void rooms_show_matches(const char *name) {
    yyjson_doc *doc = cf_golden_b_load(name);
    CF_REQUIRE(doc != NULL);
    cf_view_ctx ctx;
    cf_golden_b_ctx(&ctx, doc);
    cf_view_room_show_model show;
    cf_golden_b_show(yyjson_obj_get(yyjson_doc_get_root(doc), "input"), &show);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_room_show(&ctx, &show, &out) == CF_OK);
    cf_golden_b_expect(doc, name, 1, out.ptr, out.len);
    cf_builder_dispose(&out);
    cf_view_room_show_model_dispose(&show);
    yyjson_doc_free(doc);
}

CF_TEST(rooms_show_closed_matches_golden) {
    rooms_show_matches("rooms_show_closed");
}

CF_TEST(rooms_show_original_invitation_matches_golden) {
    rooms_show_matches("rooms_show_original");
}

CF_TEST(rooms_show_direct_matches_golden) {
    rooms_show_matches("rooms_show_direct");
}

CF_TEST(rooms_show_member_matches_golden) {
    rooms_show_matches("rooms_show_member");
}

CF_TEST(rooms_show_around_message_matches_golden) {
    rooms_show_matches("rooms_show_at_message");
}

/* The frame render carries the head and content blocks only. */
CF_TEST(rooms_show_frame_has_the_message_area_and_no_page_chrome) {
    yyjson_doc *doc = cf_golden_b_load("rooms_show_member");
    CF_REQUIRE(doc != NULL);
    cf_view_ctx ctx;
    cf_golden_b_ctx(&ctx, doc);
    cf_view_room_show_model show;
    cf_golden_b_show(yyjson_obj_get(yyjson_doc_get_root(doc), "input"), &show);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_room_show_frame(&ctx, &show, &out) == CF_OK);
    CF_CHECK(cf_builder_contains(&out, "<div id=\"message-area\""));
    CF_CHECK(cf_builder_contains(&out, "id=\"messages_rooms_open_201306877\""));
    CF_CHECK(cf_builder_contains(&out, "id=\"composer-frame\"") == 0);
    CF_CHECK(cf_builder_contains(&out, "<nav id=\"nav\"") == 0);
    CF_CHECK(cf_builder_contains(&out, "<aside id=\"sidebar\"") == 0);
    CF_CHECK(cf_builder_contains(&out, "turbo-cable-stream-source"));
    cf_builder_dispose(&out);
    cf_view_room_show_model_dispose(&show);
    yyjson_doc_free(doc);
}

/* `room_display_name` (rooms.rs): the direct-room sentence and fallbacks. */
CF_TEST(room_display_name_sentences) {
    cf_str out = {0};
    CF_REQUIRE(cf_view_room_display_name(
                   lit("HQ"), false, NULL, 0,
                   lit("David"), &out) == CF_OK);
    CF_CHECK(strcmp(out.ptr, "HQ") == 0);
    cf_str_dispose(&out);

    cf_span names[2] = {lit("Jason"), lit("JZ")};
    CF_REQUIRE(cf_view_room_display_name((cf_span){NULL, 0}, true, names, 2,
                                         lit("David"),
                                         &out) == CF_OK);
    CF_CHECK(strcmp(out.ptr, "Jason and JZ") == 0);
    cf_str_dispose(&out);

    CF_REQUIRE(cf_view_room_display_name((cf_span){NULL, 0}, true, NULL, 0,
                                         lit("David"),
                                         &out) == CF_OK);
    CF_CHECK(strcmp(out.ptr, "David") == 0);
    cf_str_dispose(&out);

    cf_span three[3] = {lit("A"), lit("B"),
                        lit("C")};
    CF_REQUIRE(cf_view_room_display_name((cf_span){NULL, 0}, true, three, 3,
                                         (cf_span){NULL, 0}, &out) == CF_OK);
    CF_CHECK(strcmp(out.ptr, "A, B, and C") == 0);
    cf_str_dispose(&out);
}

/* The invitation only appears when the model says so. */
CF_TEST(rooms_show_invitation_condition_controls_the_block) {
    yyjson_doc *doc = cf_golden_b_load("rooms_show_original");
    CF_REQUIRE(doc != NULL);
    cf_view_ctx ctx;
    cf_golden_b_ctx(&ctx, doc);
    cf_view_room_show_model show;
    cf_golden_b_show(yyjson_obj_get(yyjson_doc_get_root(doc), "input"), &show);
    CF_CHECK(show.invitation);
    cf_builder with = {0};
    CF_REQUIRE(cf_view_room_show(&ctx, &show, &with) == CF_OK);
    CF_CHECK(cf_builder_contains(&with, "id=\"system_welcome\""));
    cf_builder_dispose(&with);
    show.invitation = false;
    cf_builder without = {0};
    CF_REQUIRE(cf_view_room_show(&ctx, &show, &without) == CF_OK);
    CF_CHECK(cf_builder_contains(&without, "id=\"system_welcome\"") == 0);
    CF_CHECK(cf_builder_contains(&without, "turbo-cable-stream-source"));
    cf_builder_dispose(&without);
    cf_view_room_show_model_dispose(&show);
    yyjson_doc_free(doc);
}

/* The pwa notification-help branches the bell embeds (browser_settings /
 * system_settings / install_instructions), checked at the string level for
 * the platforms the rooms goldens do not cover. */
static void bell_for(cf_builder *out, const cf_view_ctx *ctx,
                     const cf_view_room *room) {
    CF_REQUIRE(cf_view_room_bell(ctx, room, out) == CF_OK);
}

static cf_view_ctx platform_ctx(void) {
    cf_view_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.base_url = lit("http://campfire.test");
    cf_test_views_assets_ctx(&ctx);
    return ctx;
}

CF_TEST(room_bell_firefox_android_help) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = platform_ctx();
    ctx.platform.firefox = true;
    ctx.platform.android = true;
    ctx.platform.mobile = true;
    ctx.platform.browser = lit("firefox");
    ctx.platform.operating_system = lit("Android");
    cf_view_room room = {0};
    room.id = 10;
    room.kind = CF_ROOM_OPEN;
    room.display_name = (cf_str){(char *)"HQ", 2};
    cf_builder out = {0};
    bell_for(&out, &ctx, &room);
    CF_CHECK(cf_builder_contains(&out, "Check your Firefox settings"));
    CF_CHECK(cf_builder_contains(
        &out, "<li>Tap <em>Notification</em> to change to "));
    /* install instructions are included for firefox android */
    CF_CHECK(cf_builder_contains(&out, "Install Campfire as a web app."));
    CF_CHECK(cf_builder_contains(&out, "Tap <em>Install</em> in the menu."));
    cf_builder_dispose(&out);
}

CF_TEST(room_bell_safari_ios_help) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = platform_ctx();
    ctx.platform.safari = true;
    ctx.platform.ios = true;
    ctx.platform.mobile = true;
    ctx.platform.browser = lit("safari");
    ctx.platform.operating_system = lit("iOS");
    cf_view_room room = {0};
    room.id = 11;
    room.kind = CF_ROOM_DIRECT;
    room.display_name = (cf_str){(char *)"JZ", 2};
    cf_builder out = {0};
    bell_for(&out, &ctx, &room);
    /* browser settings is skipped on iOS Safari; the install instructions
     * stay (the "install as a web app" flow) and the system settings use the
     * iOS branch. */
    CF_CHECK(cf_builder_contains(&out, "Check your Safari settings") == 0);
    CF_CHECK(cf_builder_contains(&out, "Check your iOS settings"));
    CF_CHECK(cf_builder_contains(&out, "Open the <em>"));
    CF_CHECK(cf_builder_contains(&out, "Settings app."));
    CF_CHECK(cf_builder_contains(&out, "Install Campfire as a web app."));
    CF_CHECK(cf_builder_contains(&out, "Add to Home Screen"));
    CF_CHECK(cf_builder_contains(&out, "Notification settings for this Ping"));
    cf_builder_dispose(&out);
}

CF_TEST(room_bell_windows_chrome_help) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = platform_ctx();
    ctx.platform.chrome = true;
    ctx.platform.windows = true;
    ctx.platform.desktop = true;
    ctx.platform.browser = lit("chrome");
    ctx.platform.operating_system = lit("Windows");
    cf_view_room room = {0};
    room.id = 12;
    room.kind = CF_ROOM_CLOSED;
    room.display_name = (cf_str){(char *)"Designers", 9};
    cf_builder out = {0};
    bell_for(&out, &ctx, &room);
    CF_CHECK(cf_builder_contains(&out, "Check your Chrome settings"));
    CF_CHECK(cf_builder_contains(&out, "Click <em>Site Settings</em>."));
    CF_CHECK(cf_builder_contains(
        &out, "<em>ON</em> for Chrome."));
    CF_CHECK(cf_builder_contains(&out, "Check your Windows settings"));
    CF_CHECK(cf_builder_contains(&out, "System &gt; Notification"));
    CF_CHECK(cf_builder_contains(&out, "Notification settings for this room"));
    cf_builder_dispose(&out);
}
