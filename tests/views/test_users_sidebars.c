/* tests/views/test_users_sidebars.c — A-users-sidebars view family:
 * users/sidebars/show.html.erb against golden/a.
 *
 * Reference runner: tests/fixtures/crates/views/tests/parity_a.rs
 * (`users_sidebars_show`): the page render of sidebar_david and sidebar_kevin,
 * then sidebar_david's data through layouts::frame (sidebar_frame).  The
 * runner builds users::SidebarShow from the case's `data.sidebar` object and
 * the case's users, which is what these cases do — the model is driven by the
 * fixture, not by a database (the action-level render is in
 * tests/actions/users_sidebars_test.c).
 */
#include "cf_test.h"

#include "support/facts.h"
#include "support/golden.h"
#include "views.h"

#include <stdlib.h>
#include <string.h>

#include "yyjson.h"

static yyjson_doc *g_root;

static yyjson_val *facts_root(void) {
    if (g_root == NULL) {
        size_t len = 0;
        char *text = cf_golden_read("a", "facts", "json", &len);
        CF_REQUIRE(text != NULL);
        g_root = yyjson_read(text, len, 0);
        free(text);
        CF_REQUIRE(g_root != NULL);
    }
    return yyjson_doc_get_root(g_root);
}

static cf_err dup(cf_span span, cf_str *out) {
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

static cf_err set(cf_str *out, const char *text) {
    return dup((cf_span){(const unsigned char *)text, strlen(text)}, out);
}

/* One case user as `user_summary` sees it (id, name, avatar_path). */
static cf_err build_user(yyjson_val *user, cf_view_sidebar_user *out) {
    memset(out, 0, sizeof *out);
    out->id = cf_facts_i64(user, "id", 0);
    cf_err rc = set(&out->name, cf_facts_str(user, "name"));
    if (rc == CF_OK) {
        rc = set(&out->avatar_path, cf_facts_str(user, "avatar_path"));
    }
    if (rc != CF_OK) cf_view_sidebar_user_dispose(out);
    return rc;
}

static cf_err push_user(cf_view_sidebar_user_vector *vector,
                        cf_view_sidebar_user *item) {
    if (vector->len == vector->cap) {
        size_t cap = vector->cap != 0 ? vector->cap * 2 : 4;
        cf_view_sidebar_user *grown =
            realloc(vector->items, cap * sizeof *vector->items);
        if (grown == NULL) return CF_NOMEM;
        vector->items = grown;
        vector->cap = cap;
    }
    vector->items[vector->len++] = *item;
    memset(item, 0, sizeof *item);
    return CF_OK;
}

static cf_err push_direct(cf_view_sidebar_direct_vector *vector,
                          cf_view_sidebar_direct *item) {
    if (vector->len == vector->cap) {
        size_t cap = vector->cap != 0 ? vector->cap * 2 : 4;
        cf_view_sidebar_direct *grown =
            realloc(vector->items, cap * sizeof *vector->items);
        if (grown == NULL) return CF_NOMEM;
        vector->items = grown;
        vector->cap = cap;
    }
    vector->items[vector->len++] = *item;
    memset(item, 0, sizeof *item);
    return CF_OK;
}

static cf_err push_room(cf_view_sidebar_room_vector *vector,
                        cf_view_sidebar_room *item) {
    if (vector->len == vector->cap) {
        size_t cap = vector->cap != 0 ? vector->cap * 2 : 4;
        cf_view_sidebar_room *grown =
            realloc(vector->items, cap * sizeof *vector->items);
        if (grown == NULL) return CF_NOMEM;
        vector->items = grown;
        vector->cap = cap;
    }
    vector->items[vector->len++] = *item;
    memset(item, 0, sizeof *item);
    return CF_OK;
}

/* parity_a.rs sidebar(): data[name]["sidebar"] into the view model. */
static cf_err build_model(const char *name, cf_view_sidebar_model *model) {
    memset(model, 0, sizeof *model);
    yyjson_val *root = facts_root();
    yyjson_val *cases = yyjson_obj_get(root, "cases");
    yyjson_val *object = yyjson_obj_get(cases, name);
    CF_REQUIRE(object != NULL);
    yyjson_val *data = yyjson_obj_get(object, "data");
    yyjson_val *sidebar = yyjson_obj_get(data, "sidebar");
    CF_REQUIRE(sidebar != NULL);

    /* `as`: the current user's email, exactly the runner's lookup. */
    const char *as = cf_facts_str(object, "as");
    yyjson_val *users = yyjson_obj_get(object, "users");
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
    cf_err rc = build_user(me, &model->current_user);
    CF_REQUIRE(rc == CF_OK);

    /* facts["signed_streams"]. */
    {
        yyjson_val *streams = yyjson_obj_get(root, "signed_streams");
        rc = set(&model->rooms_stream,
                 cf_facts_str(streams, "rooms"));
        if (rc == CF_OK) {
            yyjson_val *user_rooms = yyjson_obj_get(streams, "user_rooms");
            rc = set(&model->user_rooms_stream,
                     cf_facts_str(user_rooms, cf_facts_str(me, "name")));
        }
        CF_REQUIRE(rc == CF_OK);
    }

    yyjson_val *direct = yyjson_obj_get(sidebar, "direct");
    for (size_t i = 0; i < yyjson_arr_size(direct); i++) {
        yyjson_val *item = yyjson_arr_get(direct, i);
        cf_view_sidebar_direct entry;
        memset(&entry, 0, sizeof entry);
        entry.room_id = cf_facts_i64(item, "room_id", 0);
        entry.unread = cf_facts_bool(item, "unread");
        rc = set(&entry.updated_at_epoch,
                 cf_facts_str(item, "updated_at_epoch"));
        CF_REQUIRE(rc == CF_OK);
        yyjson_val *member_names = yyjson_obj_get(item, "member_names");
        for (size_t m = 0; m < yyjson_arr_size(member_names); m++) {
            const char *member =
                yyjson_get_str(yyjson_arr_get(member_names, m));
            cf_view_sidebar_user user;
            rc = build_user(cf_facts_user(object, member), &user);
            if (rc == CF_OK) rc = push_user(&entry.members, &user);
            CF_REQUIRE(rc == CF_OK);
        }
        rc = push_direct(&model->direct_memberships, &entry);
        CF_REQUIRE(rc == CF_OK);
    }

    yyjson_val *placeholders = yyjson_obj_get(sidebar, "placeholders");
    for (size_t i = 0; i < yyjson_arr_size(placeholders); i++) {
        const char *member = yyjson_get_str(yyjson_arr_get(placeholders, i));
        cf_view_sidebar_user user;
        rc = build_user(cf_facts_user(object, member), &user);
        if (rc == CF_OK) rc = push_user(&model->direct_placeholder_users, &user);
        CF_REQUIRE(rc == CF_OK);
    }

    yyjson_val *shared = yyjson_obj_get(sidebar, "shared");
    for (size_t i = 0; i < yyjson_arr_size(shared); i++) {
        yyjson_val *item = yyjson_arr_get(shared, i);
        cf_view_sidebar_room room;
        memset(&room, 0, sizeof room);
        room.id = cf_facts_i64(item, "room_id", 0);
        room.unread = cf_facts_bool(item, "unread");
        rc = set(&room.param_key, cf_facts_str(item, "param_key"));
        if (rc == CF_OK) rc = set(&room.name, cf_facts_str(item, "name"));
        if (rc == CF_OK) rc = push_room(&model->other_memberships, &room);
        CF_REQUIRE(rc == CF_OK);
    }

    model->can_create_rooms = cf_facts_bool(sidebar, "can_create_rooms");
    return CF_OK;
}

/* --- golden comparisons ---------------------------------------------------- */

CF_TEST(users_sidebars_david_matches_golden) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "sidebar_david", NULL, NULL));
    cf_view_sidebar_model model;
    CF_REQUIRE(build_model("sidebar_david", &model) == CF_OK);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_users_sidebar_show(&ctx, &model, &out) == CF_OK);
    cf_golden_expect("sidebar_david", (const char *)out.ptr, out.len);
    cf_builder_dispose(&out);
    cf_view_sidebar_model_dispose(&model);
}

CF_TEST(users_sidebars_kevin_matches_golden) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "sidebar_kevin", NULL, NULL));
    cf_view_sidebar_model model;
    CF_REQUIRE(build_model("sidebar_kevin", &model) == CF_OK);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_users_sidebar_show(&ctx, &model, &out) == CF_OK);
    cf_golden_expect("sidebar_kevin", (const char *)out.ptr, out.len);
    cf_builder_dispose(&out);
    cf_view_sidebar_model_dispose(&model);
}

CF_TEST(users_sidebars_frame_matches_golden) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "sidebar_frame", NULL, NULL));
    cf_view_sidebar_model model;
    CF_REQUIRE(build_model("sidebar_frame", &model) == CF_OK);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_users_sidebar_show_frame(&ctx, &model, &out) == CF_OK);
    cf_golden_expect("sidebar_frame", (const char *)out.ptr, out.len);
    /* turbo-rails' frame layout around the content, no application layout. */
    CF_CHECK(cf_builder_contains(&out, "<html>\n  <head>"));
    CF_CHECK(cf_builder_contains(&out, "<body>"));
    CF_CHECK(cf_builder_contains(&out, "id=\"user_sidebar\""));
    CF_CHECK(!cf_builder_contains(&out, "<title>Campfire</title>"));
    CF_CHECK(!cf_builder_contains(&out, "<aside id=\"sidebar\""));
    cf_builder_dispose(&out);
    cf_view_sidebar_model_dispose(&model);
}

/* The page carries the same turbo-frame but inside the application layout:
 * title, body class (administrator), the empty sidebar aside, tools. */
CF_TEST(users_sidebars_page_has_the_layout_and_content) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "sidebar_david", NULL, NULL));
    cf_view_sidebar_model model;
    CF_REQUIRE(build_model("sidebar_david", &model) == CF_OK);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_users_sidebar_show(&ctx, &model, &out) == CF_OK);
    CF_CHECK(cf_builder_contains(&out, "<title>Campfire</title>"));
    CF_CHECK(cf_builder_contains(&out, "<body class=\"admin\""));
    CF_CHECK(cf_builder_contains(&out, "<main id=\"main-content\">"));
    /* The content block wraps itself in sidebar_turbo_frame_tag with no src. */
    CF_CHECK(cf_builder_contains(&out, "id=\"user_sidebar\" target=\"_top\""));
    CF_CHECK(!cf_builder_contains(&out, "/users/me/sidebar\""));
    CF_CHECK(cf_builder_contains(&out, "<aside id=\"sidebar\""));
    /* Both signed stream sources, in order. */
    {
        const char *rooms = cf_builder_contains(
                                &out, "signed-stream-name=\"InJvb21zIg==")
                                ? "first"
                                : NULL;
        CF_CHECK(rooms != NULL);
    }
    CF_CHECK(cf_builder_contains(&out, "signed-stream-name=\"IloybGtPaTh2"));
    CF_CHECK(cf_builder_contains(
        &out, "<div id=\"shared_rooms\" contents data-controller=\"sorted-list\">"));
    CF_CHECK(cf_builder_contains(&out, "id=\"list_rooms_direct_699448325\""));
    CF_CHECK(cf_builder_contains(&out, "id=\"list_rooms_open_104393281\""));
    CF_CHECK(cf_builder_contains(
        &out, "action=\"/rooms/directs?user_ids%5B%5D=394959859\""));
    CF_CHECK(cf_builder_contains(
        &out, "href=\"/rooms/opens/new\""));
    CF_CHECK(cf_builder_contains(&out, "href=\"/users/me/profile\""));
    CF_CHECK(cf_builder_contains(&out, "view-transition-name: avatar-127326141"));
    cf_builder_dispose(&out);
    cf_view_sidebar_model_dispose(&model);
}

/* A user with no rooms at all: only the frame chrome, the new-ping link, the
 * static tools and (can_create_rooms false) no new-room button. */
CF_TEST(users_sidebars_empty_memberships_render_cleanly) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "sidebar_david", NULL, NULL));
    cf_view_sidebar_model model = {0};
    CF_REQUIRE(set(&model.current_user.name, "Solo") == CF_OK);
    model.current_user.id = 7;
    CF_REQUIRE(set(&model.current_user.avatar_path, "/users/tok/avatar?v=1") ==
               CF_OK);
    CF_REQUIRE(set(&model.rooms_stream, "rooms-stream") == CF_OK);
    CF_REQUIRE(set(&model.user_rooms_stream, "user-rooms-stream") == CF_OK);
    model.can_create_rooms = false;
    cf_builder out = {0};
    CF_REQUIRE(cf_view_users_sidebar_show(&ctx, &model, &out) == CF_OK);
    CF_CHECK(cf_builder_contains(&out, "id=\"direct_rooms\""));
    CF_CHECK(cf_builder_contains(&out, "id=\"shared_rooms\""));
    CF_CHECK(cf_builder_contains(&out, "Ping"));
    CF_CHECK(!cf_builder_contains(&out, "rooms__new-btn"));
    CF_CHECK(!cf_builder_contains(&out, "<form class=\"button_to\""));
    cf_builder_dispose(&out);
    cf_view_sidebar_model_dispose(&model);
}

/* The disclosed S02 gap: the sidebar emits token-based avatar URLs
 * (`/users/<signed>/avatar?v=<number>`) and has no attachment-variant branch,
 * exactly the pinned source's `fresh_user_avatar_path`.  A variant-looking
 * path in the model is passed through unchanged because the renderer never
 * consults Active Storage. */
CF_TEST(users_sidebars_avatars_are_token_paths_without_s02_variants) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "sidebar_david", NULL, NULL));
    cf_view_sidebar_model model = {0};
    CF_REQUIRE(set(&model.current_user.name, "David") == CF_OK);
    model.current_user.id = 1;
    CF_REQUIRE(set(&model.current_user.avatar_path,
                   "/users/tok/avatar?v=20260926130020") == CF_OK);
    CF_REQUIRE(set(&model.rooms_stream, "r") == CF_OK);
    CF_REQUIRE(set(&model.user_rooms_stream, "u") == CF_OK);
    /* A placeholder whose path has an S02 variant shape must still be the
     * stored value: the renderer has no branch that rewrites it. */
    cf_view_sidebar_user user;
    memset(&user, 0, sizeof user);
    user.id = 2;
    CF_REQUIRE(set(&user.name, "Anna Bea Cole") == CF_OK);
    CF_REQUIRE(set(&user.avatar_path,
                   "/users/tok/avatar?v=1&variant=abc") == CF_OK);
    CF_REQUIRE(push_user(&model.direct_placeholder_users, &user) == CF_OK);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_users_sidebar_show(&ctx, &model, &out) == CF_OK);
    CF_CHECK(cf_builder_contains(&out, "src=\"/users/tok/avatar?v=1&amp;variant=abc\""));
    CF_CHECK(cf_builder_contains(&out, "view-transition-name: avatar-1"));
    /* No attachment/representation path is invented. */
    CF_CHECK(!cf_builder_contains(&out, "/rails/active_storage"));
    CF_CHECK(!cf_builder_contains(&out, "representations"));
    cf_builder_dispose(&out);
    cf_view_sidebar_model_dispose(&model);
}

/* The branches no golden fixture reaches: a direct room with more than one
 * other member takes the first four avatars and labels the row with every
 * member's up-to-three capitalized initials joined by "+"; an unread
 * membership carries the "unread" class on both partials. */
CF_TEST(users_sidebars_multi_member_and_unread_branches) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "sidebar_david", NULL, NULL));
    cf_view_sidebar_model model = {0};
    cf_err rc = set(&model.current_user.name, "David");
    rc = rc == CF_OK ? set(&model.current_user.avatar_path, "/users/t/avatar?v=1")
                     : rc;
    rc = rc == CF_OK ? set(&model.rooms_stream, "rooms") : rc;
    rc = rc == CF_OK ? set(&model.user_rooms_stream, "user-rooms") : rc;
    CF_REQUIRE(rc == CF_OK);

    static const char *const names[5] = {"Anna Bea Cole",
                                        "Ada Lovelace Byron King", "JZ",
                                        "Kevin", "David"};
    cf_view_sidebar_direct direct;
    memset(&direct, 0, sizeof direct);
    direct.room_id = 42;
    direct.unread = true;
    CF_REQUIRE(set(&direct.updated_at_epoch, "1790427620300") == CF_OK);
    for (size_t i = 0; i < 5; i++) {
        cf_view_sidebar_user user;
        memset(&user, 0, sizeof user);
        user.id = (int64_t)i + 1;
        CF_REQUIRE(set(&user.name, names[i]) == CF_OK);
        CF_REQUIRE(set(&user.avatar_path, "/users/t/avatar?v=1") == CF_OK);
        CF_REQUIRE(push_user(&direct.members, &user) == CF_OK);
    }
    CF_REQUIRE(push_direct(&model.direct_memberships, &direct) == CF_OK);

    cf_view_sidebar_room room;
    memset(&room, 0, sizeof room);
    room.id = 7;
    room.unread = true;
    CF_REQUIRE(set(&room.param_key, "rooms_closed") == CF_OK);
    CF_REQUIRE(set(&room.name, "Designers") == CF_OK);
    CF_REQUIRE(push_room(&model.other_memberships, &room) == CF_OK);

    cf_builder out = {0};
    CF_REQUIRE(cf_view_users_sidebar_show(&ctx, &model, &out) == CF_OK);
    CF_CHECK(cf_builder_contains(&out, "class=\"direct unread\""));
    CF_CHECK(cf_builder_contains(&out, "<div class=\"avatar__group\">"));
    /* Four avatars (take(4)) despite five members. */
    CF_CHECK(cf_builder_contains(&out, "data-room-id=\"42\""));
    {
        size_t count = 0;
        const char *needle =
            "/users/t/avatar?v=1\" width=\"20\" height=\"20\"";
        size_t len = strlen(needle);
        for (size_t i = 0; i + len <= out.len; i++) {
            if (memcmp(out.ptr + i, needle, len) == 0) count++;
        }
        CF_CHECK(count == 4);
    }
    /* Every member's initials, plus-joined (not just the four avatars). */
    CF_CHECK(cf_builder_contains(&out, "ABC+ALB+J+K+D"));
    CF_CHECK(cf_builder_contains(
        &out, "class=\"align-center gap room btn txt-nowrap unread\""));
    cf_builder_dispose(&out);
    cf_view_sidebar_model_dispose(&model);
}
