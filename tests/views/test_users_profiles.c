/* tests/views/test_users_profiles.c — V-D views packet: users/profiles/show +
 * _membership + _transfer and users/push_subscriptions/index +
 * _push_subscription against golden/a, plus view-level branches.
 *
 * Reference runners: tmp/rust-ref/crates/views/tests/parity_a.rs
 * (`users_profiles_show` over profile_{chrome_mac,chrome_windows,safari_mac,
 * safari_ios,chrome_android,firefox_mac,firefox_android,edge_windows,kevin,
 * with_avatar} and `users_push_subscriptions` over push_subscriptions).
 * Models are built from the case's facts (users/data), exactly as the Rust
 * runner builds ProfileShow/PushSubscriptionsIndex from facts.json.
 *
 * The renderers live in src/views/users_profiles.c and src/views/users_push.c;
 * their view models and declarations live in src/views.h since the V02
 * presenter packet landed them (this file uses the real types directly).
 */
#include "cf_test.h"

#include "support/facts.h"
#include "support/golden.h"
#include "views.h"

#include "auth/user_agent.h"

#include <stdlib.h>
#include <string.h>

#include "yyjson.h"

/* ---- small builders --------------------------------------------------------- */

static cf_err dup_span(cf_span span, cf_str *out) {
    memset(out, 0, sizeof *out);
    if (span.len != 0 && span.ptr == NULL) return CF_INVALID;
    char *copy = malloc(span.len + 1);
    if (copy == NULL) return CF_NOMEM;
    if (span.len != 0) memcpy(copy, span.ptr, span.len);
    copy[span.len] = '\0';
    out->ptr = copy;
    out->len = span.len;
    return CF_OK;
}

static cf_err set_str(cf_str *out, const char *text) {
    return dup_span((cf_span){(const unsigned char *)text, strlen(text)}, out);
}

static void clear_str(cf_str *value) {
    free(value->ptr);
    memset(value, 0, sizeof *value);
}

static void profile_dispose(cf_view_users_profile_model *model) {
    if (model == NULL) return;
    cf_view_users_profile_model_dispose(model);
}

static void push_dispose(cf_view_users_push_index_model *model) {
    if (model == NULL) return;
    cf_view_users_push_index_model_dispose(model);
}

static cf_err push_item(cf_view_push_subscription_vector *vector, cf_view_push_subscription *item) {
    if (vector->len == vector->cap) {
        size_t cap = vector->cap != 0 ? vector->cap * 2 : 4;
        cf_view_push_subscription *grown =
            realloc(vector->items, cap * sizeof *grown);
        if (grown == NULL) return CF_NOMEM;
        vector->items = grown;
        vector->cap = cap;
    }
    vector->items[vector->len++] = *item;
    memset(item, 0, sizeof *item);
    return CF_OK;
}

static cf_err membership_item(cf_view_profile_membership_vector *vector,
                              cf_view_profile_membership *item) {
    if (vector->len == vector->cap) {
        size_t cap = vector->cap != 0 ? vector->cap * 2 : 4;
        cf_view_profile_membership *grown =
            realloc(vector->items, cap * sizeof *grown);
        if (grown == NULL) return CF_NOMEM;
        vector->items = grown;
        vector->cap = cap;
    }
    vector->items[vector->len++] = *item;
    memset(item, 0, sizeof *item);
    return CF_OK;
}

/* Overlay the full platform facts (facts.json platforms[ua]) onto the ctx the
 * facts helper built (it only sets apple_messages/mobile/desktop). */
static void overlay_platform(cf_view_ctx *ctx, const char *ua) {
    /* Mirrors tmp/rust-ref facts.rs platform(): the pinned per-UA table. */
    struct {
        const char *ua;
        bool ios, android, mac, windows, chrome, firefox, safari, edge;
        bool mobile, desktop;
        const char *browser, *os;
    } table[] = {
        {"chrome_mac", false, false, true, false, true, false, false, false,
         false, true, "Chrome", "macOS"},
        {"chrome_windows", false, false, false, true, true, false, false,
         false, false, true, "Chrome", "Windows"},
        {"safari_mac", false, false, true, false, false, false, true, false,
         false, true, "Safari", "macOS"},
        {"safari_ios", true, false, false, false, false, false, true, false,
         true, false, "Safari", "iPhone"},
        {"chrome_android", false, true, false, false, true, false, false,
         false, true, false, "Chrome", "Android"},
        {"firefox_mac", false, false, true, false, false, true, false, false,
         false, true, "Firefox", "macOS"},
        {"firefox_android", false, true, false, false, false, true, false,
         false, true, false, "Firefox", "Android"},
        {"edge_windows", false, false, false, true, true, false, false, false,
         false, true, "Chrome", "Windows"},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (strcmp(table[i].ua, ua) != 0) continue;
        ctx->platform.ios = table[i].ios;
        ctx->platform.android = table[i].android;
        ctx->platform.mac = table[i].mac;
        ctx->platform.windows = table[i].windows;
        ctx->platform.chrome = table[i].chrome;
        ctx->platform.firefox = table[i].firefox;
        ctx->platform.safari = table[i].safari;
        ctx->platform.edge = table[i].edge;
        ctx->platform.mobile = table[i].mobile;
        ctx->platform.desktop = table[i].desktop;
        ctx->platform.browser = (cf_span){
            (const unsigned char *)table[i].browser, strlen(table[i].browser)};
        ctx->platform.operating_system = (cf_span){
            (const unsigned char *)table[i].os, strlen(table[i].os)};
        return;
    }
}

/* parity_a.rs users_profiles_show(): case data into the view model. */
static cf_err build_profile(const char *name, cf_view_users_profile_model *model) {
    memset(model, 0, sizeof *model);
    yyjson_val *case_obj = cf_facts_case(name);
    CF_REQUIRE(case_obj != NULL);
    const char *as = cf_facts_str(case_obj, "as");
    yyjson_val *users = yyjson_obj_get(case_obj, "users");
    yyjson_val *me = NULL;
    {
        yyjson_val *key, *value;
        yyjson_obj_iter iter;
        yyjson_obj_iter_init(users, &iter);
        while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
            value = yyjson_obj_iter_get_val(key);
            if (strcmp(cf_facts_str(value, "email_address"), as) == 0) {
                me = value;
                break;
            }
        }
    }
    CF_REQUIRE(me != NULL);
    model->user.id = cf_facts_i64(me, "id", 0);
    CF_REQUIRE(set_str(&model->user.name, cf_facts_str(me, "name")) == CF_OK);
    {
        yyjson_val *bio = yyjson_obj_get(me, "bio");
        if (bio != NULL && yyjson_is_str(bio)) {
            model->user.has_bio = true;
            CF_REQUIRE(set_str(&model->user.bio, yyjson_get_str(bio)) ==
                       CF_OK);
        }
        yyjson_val *email = yyjson_obj_get(me, "email_address");
        if (email != NULL && yyjson_is_str(email)) {
            model->user.has_email = true;
            CF_REQUIRE(set_str(&model->user.email_address,
                               yyjson_get_str(email)) == CF_OK);
        }
    }
    {
        const char *role = cf_facts_str(me, "role");
        model->user.role = strcmp(role, "administrator") == 0
                               ? CF_ROLE_ADMINISTRATOR
                           : strcmp(role, "bot") == 0 ? CF_ROLE_BOT
                                                      : CF_ROLE_MEMBER;
        const char *status = cf_facts_str(me, "status");
        model->user.status = strcmp(status, "deactivated") == 0
                                 ? CF_STATUS_DEACTIVATED
                             : strcmp(status, "banned") == 0
                                 ? CF_STATUS_BANNED
                                 : CF_STATUS_ACTIVE;
    }
    CF_REQUIRE(set_str(&model->user.avatar_path,
                       cf_facts_str(me, "avatar_path")) == CF_OK);
    CF_REQUIRE(set_str(&model->transfer_id, cf_facts_str(me, "transfer_id")) ==
               CF_OK);
    model->avatar_attached = cf_facts_bool(me, "avatar_attached");

    yyjson_val *data = yyjson_obj_get(case_obj, "data");
    yyjson_val *profile = yyjson_obj_get(data, "profile");
    const char *sides[2] = {"shared", "direct"};
    for (int s = 0; s < 2; s++) {
        yyjson_val *list = yyjson_obj_get(profile, sides[s]);
        for (size_t i = 0; i < yyjson_arr_size(list); i++) {
            yyjson_val *item = yyjson_arr_get(list, i);
            cf_view_profile_membership entry;
            memset(&entry, 0, sizeof entry);
            entry.room_id = cf_facts_i64(item, "room_id", 0);
            entry.direct = cf_facts_bool(item, "direct");
            cf_err rc = set_str(&entry.room_param_key,
                                cf_facts_str(item, "param_key"));
            if (rc == CF_OK) {
                rc = set_str(&entry.room_display_name,
                             cf_facts_str(item, "display_name"));
            }
            if (rc == CF_OK) {
                rc = set_str(&entry.involvement,
                             cf_facts_str(item, "involvement"));
            }
            CF_REQUIRE(rc == CF_OK);
            CF_REQUIRE(membership_item(s == 0 ? &model->shared_memberships
                                              : &model->direct_memberships,
                                       &entry) == CF_OK);
        }
    }
    return CF_OK;
}

/* parity_a.rs users_push_subscriptions(): case data into the view model. */
static cf_err build_push(const char *name, cf_view_users_push_index_model *model) {
    memset(model, 0, sizeof *model);
    yyjson_val *case_obj = cf_facts_case(name);
    CF_REQUIRE(case_obj != NULL);
    yyjson_val *data = yyjson_obj_get(case_obj, "data");
    yyjson_val *list = yyjson_obj_get(data, "push_subscriptions");
    for (size_t i = 0; i < yyjson_arr_size(list); i++) {
        yyjson_val *item = yyjson_arr_get(list, i);
        cf_view_push_subscription entry;
        memset(&entry, 0, sizeof entry);
        entry.id = cf_facts_i64(item, "id", 0);
        cf_err rc = set_str(&entry.endpoint, cf_facts_str(item, "endpoint"));
        if (rc == CF_OK) {
            rc = set_str(&entry.browser, cf_facts_str(item, "browser"));
        }
        if (rc == CF_OK) {
            rc = set_str(&entry.version, cf_facts_str(item, "version"));
        }
        if (rc == CF_OK) {
            rc = set_str(&entry.platform, cf_facts_str(item, "platform"));
        }
        CF_REQUIRE(rc == CF_OK);
        CF_REQUIRE(push_item(&model->subscriptions, &entry) == CF_OK);
    }
    /* last_room_visited: the current user's original_room_id (facts.rs). */
    {
        const char *as = cf_facts_str(case_obj, "as");
        yyjson_val *users = yyjson_obj_get(case_obj, "users");
        yyjson_val *key, *value;
        yyjson_obj_iter iter;
        yyjson_obj_iter_init(users, &iter);
        while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
            value = yyjson_obj_iter_get_val(key);
            if (strcmp(cf_facts_str(value, "email_address"), as) == 0) {
                yyjson_val *room = yyjson_obj_get(value, "original_room_id");
                if (room != NULL && yyjson_is_int(room)) {
                    model->has_last_room = true;
                    model->last_room_id = yyjson_get_sint(room);
                }
                break;
            }
        }
    }
    return CF_OK;
}

static const char *profile_cases[] = {
    "profile_chrome_mac",   "profile_chrome_windows", "profile_safari_mac",
    "profile_safari_ios",   "profile_chrome_android", "profile_firefox_mac",
    "profile_firefox_android", "profile_edge_windows",
};

static const char *profile_uas[] = {
    "chrome_mac", "chrome_windows", "safari_mac",     "safari_ios",
    "chrome_android", "firefox_mac",    "firefox_android", "edge_windows",
};

/* --- golden comparisons ------------------------------------------------------ */

CF_TEST(users_profiles_show_matches_golden) {
    CF_REQUIRE(cf_test_views_setup());
    for (size_t i = 0; i < sizeof profile_cases / sizeof profile_cases[0];
         i++) {
        cf_view_ctx ctx = {0};
        CF_REQUIRE(
            cf_facts_view_ctx(&ctx, profile_cases[i], NULL, NULL));
        overlay_platform(&ctx, profile_uas[i]);
        cf_view_users_profile_model model;
        CF_REQUIRE(build_profile(profile_cases[i], &model) == CF_OK);
        cf_builder out = {0};
        CF_REQUIRE(cf_view_users_profile_show(&ctx, &model, &out) == CF_OK);
        cf_golden_expect(profile_cases[i], (const char *)out.ptr, out.len);
        cf_builder_dispose(&out);
        profile_dispose(&model);
    }
}

CF_TEST(users_profiles_kevin_and_avatar_golden) {
    CF_REQUIRE(cf_test_views_setup());
    for (size_t i = 0; i < 2; i++) {
        const char *name =
            i == 0 ? "profile_kevin" : "profile_with_avatar";
        cf_view_ctx ctx = {0};
        CF_REQUIRE(cf_facts_view_ctx(&ctx, name, NULL, NULL));
        overlay_platform(&ctx, "chrome_mac");
        cf_view_users_profile_model model;
        CF_REQUIRE(build_profile(name, &model) == CF_OK);
        cf_builder out = {0};
        CF_REQUIRE(cf_view_users_profile_show(&ctx, &model, &out) == CF_OK);
        cf_golden_expect(name, (const char *)out.ptr, out.len);
        cf_builder_dispose(&out);
        profile_dispose(&model);
    }
}

CF_TEST(users_push_subscriptions_matches_golden) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "push_subscriptions", NULL, NULL));
    overlay_platform(&ctx, "chrome_mac");
    cf_view_users_push_index_model model;
    CF_REQUIRE(build_push("push_subscriptions", &model) == CF_OK);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_users_push_index(&ctx, &model, &out) == CF_OK);
    cf_golden_expect("push_subscriptions", (const char *)out.ptr, out.len);
    cf_builder_dispose(&out);
    push_dispose(&model);
}

/* --- frame layouts ------------------------------------------------------------- */

CF_TEST(users_profiles_frame_has_content_without_page_chrome) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "profile_kevin", NULL, NULL));
    overlay_platform(&ctx, "chrome_mac");
    cf_view_users_profile_model model;
    CF_REQUIRE(build_profile("profile_kevin", &model) == CF_OK);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_users_profile_show_frame(&ctx, &model, &out) ==
               CF_OK);
    CF_CHECK(cf_builder_contains(&out, "id=\"session_transfer_url\""));
    CF_CHECK(cf_builder_contains(&out, "name=\"user[name]\""));
    CF_CHECK(cf_builder_contains(&out, "<html>\n  <head>"));
    CF_CHECK(cf_builder_contains(&out, "<body>"));
    CF_CHECK(!cf_builder_contains(&out, "<title>Kevin</title>"));
    CF_CHECK(!cf_builder_contains(&out, "<nav id=\"nav\">"));
    CF_CHECK(!cf_builder_contains(&out, "<aside id=\"sidebar\""));
    cf_builder_dispose(&out);
    profile_dispose(&model);
}

CF_TEST(users_push_frame_has_content_without_page_chrome) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "push_subscriptions", NULL, NULL));
    overlay_platform(&ctx, "chrome_mac");
    cf_view_users_push_index_model model;
    CF_REQUIRE(build_push("push_subscriptions", &model) == CF_OK);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_users_push_index_frame(&ctx, &model, &out) == CF_OK);
    CF_CHECK(cf_builder_contains(&out, "id=\"push_subscriptions\""));
    CF_CHECK(cf_builder_contains(&out, "<html>\n  <head>"));
    CF_CHECK(!cf_builder_contains(&out, "Push notification subscriptions</title>"));
    CF_CHECK(!cf_builder_contains(&out, "<nav id=\"nav\">"));
    cf_builder_dispose(&out);
    push_dispose(&model);
}

/* --- page structure (VIEW-03/VIEW-04 DOM + forms) ------------------------------- */

CF_TEST(users_profiles_page_has_the_layout_and_forms) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "profile_kevin", NULL, NULL));
    overlay_platform(&ctx, "chrome_mac");
    cf_view_users_profile_model model;
    CF_REQUIRE(build_profile("profile_kevin", &model) == CF_OK);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_users_profile_show(&ctx, &model, &out) == CF_OK);
    CF_CHECK(cf_builder_contains(&out, "<title>Kevin</title>"));
    CF_CHECK(cf_builder_contains(&out, "view-transition-name: avatar-712064548"));
    /* Logout form (session delete + push endpoint hidden field). */
    CF_CHECK(cf_builder_contains(&out, "action=\"/session\""));
    CF_CHECK(cf_builder_contains(&out, "name=\"push_subscription_endpoint\""));
    CF_CHECK(cf_builder_contains(&out, "sessions#logout:prevent"));
    /* Avatar forms (multipart patch) + profile fields. */
    CF_CHECK(cf_builder_contains(&out, "enctype=\"multipart/form-data\""));
    CF_CHECK(cf_builder_contains(&out, "name=\"user[avatar]\""));
    CF_CHECK(cf_builder_contains(&out, "name=\"user[name]\""));
    CF_CHECK(cf_builder_contains(&out, "value=\"Kevin\""));
    CF_CHECK(cf_builder_contains(&out, "name=\"user[email_address]\""));
    CF_CHECK(cf_builder_contains(&out, "value=\"kevin@37signals.com\""));
    CF_CHECK(cf_builder_contains(&out, "name=\"user[password]\""));
    CF_CHECK(cf_builder_contains(&out, "maxlength=\"72\""));
    CF_CHECK(cf_builder_contains(&out, "name=\"user[bio]\""));
    CF_CHECK(cf_builder_contains(&out, "Programmer</textarea>"));
    /* Memberships + transfer. */
    CF_CHECK(cf_builder_contains(&out, "id=\"involvement_rooms_closed_654632876\""));
    CF_CHECK(cf_builder_contains(&out, "id=\"involvement_rooms_direct_699448325\""));
    CF_CHECK(cf_builder_contains(&out, "involvement?involvement=everything"));
    CF_CHECK(cf_builder_contains(&out, "id=\"session_transfer_url\""));
    CF_CHECK(cf_builder_contains(&out, "/qr_code/"));
    CF_CHECK(!cf_builder_contains(&out, "Delete avatar"));
    cf_builder_dispose(&out);
    profile_dispose(&model);
}

CF_TEST(users_profiles_avatar_attached_shows_delete) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "profile_with_avatar", NULL, NULL));
    overlay_platform(&ctx, "chrome_mac");
    cf_view_users_profile_model model;
    CF_REQUIRE(build_profile("profile_with_avatar", &model) == CF_OK);
    CF_REQUIRE(model.avatar_attached);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_users_profile_show(&ctx, &model, &out) == CF_OK);
    CF_CHECK(cf_builder_contains(&out, "Delete avatar"));
    CF_CHECK(cf_builder_contains(&out, "/users/712064548/avatar"));
    CF_CHECK(cf_builder_contains(&out, "name=\"_method\" value=\"delete\""));
    cf_builder_dispose(&out);
    profile_dispose(&model);
}

CF_TEST(users_profiles_transfer_other_user_arm) {
    CF_REQUIRE(cf_test_views_setup());
    /* David (admin) viewing Kevin's transfer: the crown/share-others arm. */
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "profile_kevin", NULL, NULL));
    overlay_platform(&ctx, "chrome_mac");
    /* ctx current user is Kevin (the `as` user); force David to take the
     * non-self arm. */
    ctx.current_user.id = 127326141;
    cf_view_users_profile_model model;
    CF_REQUIRE(build_profile("profile_kevin", &model) == CF_OK);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_users_profile_transfer(
                   &ctx, &model.user,
                   (cf_span){(const unsigned char *)model.transfer_id.ptr,
                             model.transfer_id.len},
                   &out) == CF_OK);
    CF_CHECK(cf_builder_contains(&out, "crown-"));
    CF_CHECK(cf_builder_contains(
        &out, "Share to get them back into their account"));
    CF_CHECK(!cf_builder_contains(&out, "Use this link to login"));
    cf_builder_dispose(&out);
    profile_dispose(&model);
}

CF_TEST(users_profiles_empty_memberships_render_cleanly) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "profile_kevin", NULL, NULL));
    overlay_platform(&ctx, "chrome_mac");
    cf_view_users_profile_model model;
    memset(&model, 0, sizeof model);
    model.user.id = 9;
    CF_REQUIRE(set_str(&model.user.name, "Solo") == CF_OK);
    model.user.role = CF_ROLE_MEMBER;
    model.user.status = CF_STATUS_ACTIVE;
    CF_REQUIRE(set_str(&model.user.avatar_path, "/users/tok/avatar?v=1") ==
               CF_OK);
    CF_REQUIRE(set_str(&model.transfer_id, "transfer-token") == CF_OK);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_users_profile_show(&ctx, &model, &out) == CF_OK);
    /* No separator without both sides, no membership items. */
    CF_CHECK(!cf_builder_contains(&out, "membership-item\">\n  <a"));
    CF_CHECK(!cf_builder_contains(&out, "separator full-width"));
    CF_CHECK(cf_builder_contains(&out, "id=\"session_transfer_url\""));
    cf_builder_dispose(&out);
    profile_dispose(&model);
}

CF_TEST(users_push_page_has_the_layout_and_buttons) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "push_subscriptions", NULL, NULL));
    overlay_platform(&ctx, "chrome_mac");
    cf_view_users_push_index_model model;
    CF_REQUIRE(build_push("push_subscriptions", &model) == CF_OK);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_users_push_index(&ctx, &model, &out) == CF_OK);
    CF_CHECK(cf_builder_contains(&out, "<title>Push notification "
                                       "subscriptions</title>"));
    CF_CHECK(cf_builder_contains(&out, "<body class=\"admin\""));
    /* Back to the last room (David's original room). */
    CF_CHECK(cf_builder_contains(&out, "href=\"/rooms/654632876\""));
    CF_CHECK(cf_builder_contains(&out, "Chrome 113.0.0.0 on Macintosh"));
    CF_CHECK(cf_builder_contains(&out, "https://fcm.googleapis.com/fcm/send/123"));
    CF_CHECK(cf_builder_contains(
        &out, "/users/me/push_subscriptions/56887440/test_notifications"));
    CF_CHECK(cf_builder_contains(
        &out, "action=\"/users/me/push_subscriptions/56887440\""));
    CF_CHECK(cf_builder_contains(&out, "name=\"_method\" value=\"delete\""));
    CF_CHECK(cf_builder_contains(&out, "Send test notification"));
    CF_CHECK(cf_builder_contains(&out, "Delete subscription"));
    cf_builder_dispose(&out);
    push_dispose(&model);
}

CF_TEST(users_push_empty_renders_the_shell) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "push_subscriptions", NULL, NULL));
    overlay_platform(&ctx, "chrome_mac");
    cf_view_users_push_index_model model;
    memset(&model, 0, sizeof model);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_users_push_index(&ctx, &model, &out) == CF_OK);
    CF_CHECK(cf_builder_contains(&out, "id=\"push_subscriptions\""));
    CF_CHECK(!cf_builder_contains(&out, "membership-item"));
    cf_builder_dispose(&out);
    push_dispose(&model);
}

/* --- UA mapping (the R1 browser/version/platform strings) ----------------------- */

CF_TEST(users_push_ua_mapping_matches_the_gem) {
    cf_str browser = {0}, version = {0}, platform = {0};
    static const char chrome_ua[] =
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/113.0.0.0 Safari/537.36";
    CF_REQUIRE(cf_view_push_subscription_parse(
                   (cf_span){(const unsigned char *)chrome_ua,
                             sizeof chrome_ua - 1},
                   &browser, &version, &platform) == CF_OK);
    CF_CHECK(browser.len == 6 && memcmp(browser.ptr, "Chrome", 6) == 0);
    CF_CHECK(version.len == 9 &&
             memcmp(version.ptr, "113.0.0.0", 9) == 0);
    CF_CHECK(platform.len == 9 &&
             memcmp(platform.ptr, "Macintosh", 9) == 0);
    clear_str(&browser);
    clear_str(&version);
    clear_str(&platform);
    /* Empty agent parses as Mozilla/4.0 (compatible): no crash, emptyish. */
    CF_REQUIRE(cf_view_push_subscription_parse((cf_span){NULL, 0}, &browser,
                                               &version,
                                               &platform) == CF_OK);
    CF_CHECK(browser.ptr != NULL && version.ptr != NULL &&
             platform.ptr != NULL);
    clear_str(&browser);
    clear_str(&version);
    clear_str(&platform);
}

/* --- failure atomicity (A02: the builder is unchanged on error) ------------------ */

CF_TEST(users_profiles_failure_leaves_the_builder_unchanged) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "profile_kevin", NULL, NULL));
    overlay_platform(&ctx, "chrome_mac");
    cf_builder out = {0};
    CF_REQUIRE(cf_builder_append(
                   &out, (cf_span){(const unsigned char *)"prefix", 6}) ==
               CF_OK);
    size_t len = out.len;
    /* NULL model is CF_INVALID and must not append. */
    CF_CHECK(cf_view_users_profile_show(&ctx, NULL, &out) == CF_INVALID);
    CF_CHECK(out.len == len);
    CF_CHECK(cf_view_users_profile_transfer(&ctx, NULL, (cf_span){NULL, 0},
                                            &out) == CF_INVALID);
    CF_CHECK(out.len == len);
    cf_view_users_push_index_model push;
    memset(&push, 0, sizeof push);
    CF_CHECK(cf_view_users_push_index(NULL, &push, &out) == CF_INVALID);
    CF_CHECK(out.len == len);
    cf_builder_dispose(&out);
}
