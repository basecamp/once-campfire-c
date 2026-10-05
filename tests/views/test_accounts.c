/* tests/views/test_accounts.c — VIEW-03/VIEW-04 (packet V-C): the accounts
 * Edit page, the accounts users turbo stream and the custom styles page
 * against golden/a, plus view-level behavior cases.
 *
 * Reference runners: tests/fixtures/crates/views/tests/parity_a.rs
 * (accounts_edit, accounts_users_index_turbo_stream, accounts_custom_styles);
 * comparison is tests/views/support/golden.c with only the runner's forgery
 * masks (exact named masks only). Inputs rebuild the runner's view models
 * from facts.json: ordered users (lowercased-name order), the
 * administrator/active partition, page_users for the stream and the
 * last-room link from the viewing user's original_room_id.
 *
 * The cf_view_accounts_* declarations below mirror src/views/accounts.c's
 * PUBLIC API block (integrator: move both to src/views.h).
 */
#include "cf_test.h"

#include "support/facts.h"
#include "support/golden.h"
#include "views.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- mirror of src/views/accounts.c PUBLIC API (see above) ---- */

typedef struct cf_view_account_user {
    int64_t id;
    cf_str name;
    cf_str title;
    cf_str avatar_path;
    cf_role role;
    cf_status status;
} cf_view_account_user;

typedef struct cf_view_account_user_vector {
    cf_view_account_user *items;
    size_t len, cap;
} cf_view_account_user_vector;

typedef struct cf_view_accounts_edit_model {
    int64_t account_id;
    cf_str join_code;
    bool restrict_room_creation_to_administrators;
    cf_view_account_user_vector administrators;
    cf_view_account_user_vector members;
    bool has_last_room_visited;
    int64_t last_room_visited_id;
    bool has_next_page;
    cf_str next_page;
} cf_view_accounts_edit_model;

typedef struct cf_view_accounts_users_model {
    cf_view_account_user_vector users;
    bool has_next_page;
    cf_str next_page;
} cf_view_accounts_users_model;

typedef struct cf_view_accounts_custom_styles_model {
    bool has_custom_styles;
    cf_str custom_styles;
} cf_view_accounts_custom_styles_model;

cf_err cf_view_account_user_assign(cf_view_account_user *out, int64_t id,
                                   cf_span name, cf_span title,
                                   cf_span avatar_path, cf_role role,
                                   cf_status status);
cf_err cf_view_account_user_vector_push(cf_view_account_user_vector *vector,
                                        int64_t id, cf_span name,
                                        cf_span title, cf_span avatar_path,
                                        cf_role role, cf_status status);
void cf_view_account_user_dispose(cf_view_account_user *user);
void cf_view_account_user_vector_dispose(cf_view_account_user_vector *vector);
void cf_view_accounts_edit_model_dispose(cf_view_accounts_edit_model *model);
void cf_view_accounts_users_model_dispose(cf_view_accounts_users_model *model);
void cf_view_accounts_custom_styles_model_dispose(
    cf_view_accounts_custom_styles_model *model);
cf_err cf_view_accounts_edit(const cf_view_ctx *ctx,
                             const cf_view_accounts_edit_model *model,
                             cf_builder *out);
cf_err cf_view_accounts_edit_frame(const cf_view_ctx *ctx,
                                   const cf_view_accounts_edit_model *model,
                                   cf_builder *out);
cf_err cf_view_accounts_users_stream(const cf_view_ctx *ctx,
                                     const cf_view_accounts_users_model *model,
                                     cf_builder *out);
cf_err cf_view_accounts_custom_styles_edit(
    const cf_view_ctx *ctx, const cf_view_accounts_custom_styles_model *model,
    cf_builder *out);
cf_err cf_view_accounts_custom_styles_edit_frame(
    const cf_view_ctx *ctx, const cf_view_accounts_custom_styles_model *model,
    cf_builder *out);
cf_err cf_view_accounts_invite(const cf_view_ctx *ctx, cf_span join_code,
                               cf_builder *out);
cf_err cf_view_accounts_user_partial(const cf_view_ctx *ctx,
                                     const cf_view_account_user *user,
                                     cf_builder *out);
cf_err cf_view_accounts_next_page(cf_span page, cf_builder *out);

/* ---- facts helpers ---- */

static cf_span span_of(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

static cf_str dup_str(const char *text) {
    size_t len = strlen(text);
    char *copy = malloc(len + 1);
    if (copy == NULL) return (cf_str){NULL, 0};
    memcpy(copy, text, len + 1);
    return (cf_str){copy, len};
}

static bool is_blank(const char *text) {
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0';
         p++) {
        if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\v' &&
            *p != '\f' && *p != '\r') {
            return false;
        }
    }
    return true;
}

/* User#title from facts: [name, bio].compact_blank.join(" – "). */
static cf_str facts_title(yyjson_val *fact) {
    const char *name = cf_facts_str(fact, "name");
    yyjson_val *bio_val = yyjson_obj_get(fact, "bio");
    const char *bio =
        (bio_val != NULL && yyjson_is_str(bio_val)) ? yyjson_get_str(bio_val)
                                                    : NULL;
    if (bio == NULL || is_blank(bio)) return dup_str(name);
    /* " – " is space + U+2013 (0xE2 0x80 0x93) + space. */
    size_t namelen = strlen(name);
    size_t len = namelen + 5 + strlen(bio);
    char *copy = malloc(len + 1);
    if (copy == NULL) return (cf_str){NULL, 0};
    memcpy(copy, name, namelen);
    copy[namelen] = ' ';
    copy[namelen + 1] = (char)0xE2;
    copy[namelen + 2] = (char)0x80;
    copy[namelen + 3] = (char)0x93;
    copy[namelen + 4] = ' ';
    memcpy(copy + namelen + 5, bio, strlen(bio) + 1);
    return (cf_str){copy, len};
}

static cf_role facts_role(yyjson_val *fact) {
    const char *role = cf_facts_str(fact, "role");
    if (strcmp(role, "administrator") == 0) return CF_ROLE_ADMINISTRATOR;
    if (strcmp(role, "bot") == 0) return CF_ROLE_BOT;
    return CF_ROLE_MEMBER;
}

static cf_status facts_status(yyjson_val *fact) {
    const char *status = cf_facts_str(fact, "status");
    if (strcmp(status, "deactivated") == 0) return CF_STATUS_DEACTIVATED;
    if (strcmp(status, "banned") == 0) return CF_STATUS_BANNED;
    return CF_STATUS_ACTIVE;
}

static int push_fact(cf_view_account_user_vector *vector, yyjson_val *fact) {
    cf_str title = facts_title(fact);
    cf_err rc = cf_view_account_user_vector_push(
        vector, cf_facts_i64(fact, "id", 0), span_of(cf_facts_str(fact,
                                                                  "name")),
        (cf_span){(const unsigned char *)title.ptr,
                  title.ptr != NULL ? title.len : 0},
        span_of(cf_facts_str(fact, "avatar_path")), facts_role(fact),
        facts_status(fact));
    free(title.ptr);
    return rc == CF_OK;
}

/* ASCII-lowercase name order (User.ordered: ORDER BY LOWER(name)). */
static int user_name_cmp(const void *pa, const void *pb) {
    yyjson_val *const *a = pa, *const *b = pb;
    const char *na = cf_facts_str(*a, "name"), *nb = cf_facts_str(*b, "name");
    for (;; na++, nb++) {
        unsigned char ca = (unsigned char)*na, cb = (unsigned char)*nb;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb || ca == '\0') return (int)ca - (int)cb;
    }
}

/* The runner's Edit user lists: ordered, without bots, active (+banned for
 * administrators), partitioned administrators-first (stable). */
static int edit_users(const char *fixture, bool viewer_admin,
                      cf_view_account_user_vector *admins,
                      cf_view_account_user_vector *members) {
    yyjson_val *users = yyjson_obj_get(cf_facts_case(fixture), "users");
    if (users == NULL) return 0;
    yyjson_val *key, *value;
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(users, &iter);
    yyjson_val *all[2048];
    size_t count = 0;
    while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
        value = yyjson_obj_iter_get_val(key);
        if (count < sizeof all / sizeof all[0]) all[count++] = value;
    }
    qsort(all, count, sizeof all[0], user_name_cmp);
    for (size_t i = 0; i < count; i++) {
        cf_role role = facts_role(all[i]);
        cf_status status = facts_status(all[i]);
        if (role == CF_ROLE_BOT) continue;
        if (status != CF_STATUS_ACTIVE &&
            !(viewer_admin && status == CF_STATUS_BANNED)) {
            continue;
        }
        if (!push_fact(role == CF_ROLE_ADMINISTRATOR ? admins : members,
                       all[i])) {
            return 0;
        }
    }
    return 1;
}

/* The viewing user's original_room_id (the last-room link fact). */
static bool last_room_visited(const char *fixture, int64_t viewer_id,
                              int64_t *room_id) {
    yyjson_val *users = yyjson_obj_get(cf_facts_case(fixture), "users");
    if (users == NULL) return false;
    yyjson_val *key, *value;
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(users, &iter);
    while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
        value = yyjson_obj_iter_get_val(key);
        if (cf_facts_i64(value, "id", -1) != viewer_id) continue;
        yyjson_val *room = yyjson_obj_get(value, "original_room_id");
        if (room == NULL || !yyjson_is_int(room)) return false;
        *room_id = yyjson_get_sint(room);
        return true;
    }
    return false;
}

static int edit_model(const char *fixture, const char *flash_notice,
                      cf_view_ctx *ctx, cf_view_accounts_edit_model *model) {
    memset(model, 0, sizeof *model);
    if (!cf_test_views_setup()) return 0;
    if (!cf_facts_view_ctx(ctx, fixture, flash_notice, NULL)) return 0;
    yyjson_val *account = yyjson_obj_get(cf_facts_case(fixture), "account");
    model->account_id = cf_facts_i64(account, "id", 0);
    model->join_code = dup_str(cf_facts_str(account, "join_code"));
    model->restrict_room_creation_to_administrators =
        cf_facts_bool(account, "restrict_room_creation_to_administrators");
    if (!edit_users(fixture, ctx->current_user.administrator,
                    &model->administrators, &model->members)) {
        return 0;
    }
    int64_t room_id = 0;
    model->has_last_room_visited =
        last_room_visited(fixture, ctx->current_user.id, &room_id);
    model->last_room_visited_id = room_id;
    if (strcmp(fixture, "account_edit_paginated") == 0) {
        model->has_next_page = true;
        model->next_page = dup_str("2");
    }
    return model->join_code.ptr != NULL;
}

/* ---- golden pages ---- */

CF_TEST(accounts_edit_admin_matches_golden) {
    cf_view_ctx ctx;
    cf_view_accounts_edit_model model;
    CF_REQUIRE(edit_model("account_edit_admin", NULL, &ctx, &model));
    cf_builder out = {0};
    CF_REQUIRE(cf_view_accounts_edit(&ctx, &model, &out) == CF_OK);
    cf_golden_expect("account_edit_admin", (const char *)out.ptr, out.len);
    cf_builder_dispose(&out);
    cf_view_accounts_edit_model_dispose(&model);
}

CF_TEST(accounts_edit_member_matches_golden) {
    cf_view_ctx ctx;
    cf_view_accounts_edit_model model;
    CF_REQUIRE(edit_model("account_edit_member", NULL, &ctx, &model));
    cf_builder out = {0};
    CF_REQUIRE(cf_view_accounts_edit(&ctx, &model, &out) == CF_OK);
    cf_golden_expect("account_edit_member", (const char *)out.ptr, out.len);
    cf_builder_dispose(&out);
    cf_view_accounts_edit_model_dispose(&model);
}

CF_TEST(accounts_edit_notice_matches_golden) {
    cf_view_ctx ctx;
    cf_view_accounts_edit_model model;
    CF_REQUIRE(edit_model("account_edit_notice", "\xE2\x9C\x93", &ctx,
                          &model));
    cf_builder out = {0};
    CF_REQUIRE(cf_view_accounts_edit(&ctx, &model, &out) == CF_OK);
    cf_golden_expect("account_edit_notice", (const char *)out.ptr, out.len);
    cf_builder_dispose(&out);
    cf_view_accounts_edit_model_dispose(&model);
}

CF_TEST(accounts_edit_paginated_matches_golden) {
    cf_view_ctx ctx;
    cf_view_accounts_edit_model model;
    CF_REQUIRE(edit_model("account_edit_paginated", NULL, &ctx, &model));
    CF_REQUIRE(model.has_next_page);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_accounts_edit(&ctx, &model, &out) == CF_OK);
    cf_golden_expect("account_edit_paginated", (const char *)out.ptr, out.len);
    cf_builder_dispose(&out);
    cf_view_accounts_edit_model_dispose(&model);
}

CF_TEST(accounts_edit_with_logo_matches_golden) {
    cf_view_ctx ctx;
    cf_view_accounts_edit_model model;
    CF_REQUIRE(edit_model("account_edit_with_logo", NULL, &ctx, &model));
    CF_REQUIRE(ctx.account.has_logo);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_accounts_edit(&ctx, &model, &out) == CF_OK);
    cf_golden_expect("account_edit_with_logo", (const char *)out.ptr, out.len);
    cf_builder_dispose(&out);
    cf_view_accounts_edit_model_dispose(&model);
}

CF_TEST(accounts_edit_with_logo_member_matches_golden) {
    cf_view_ctx ctx;
    cf_view_accounts_edit_model model;
    CF_REQUIRE(edit_model("account_edit_with_logo_member", NULL, &ctx,
                          &model));
    cf_builder out = {0};
    CF_REQUIRE(cf_view_accounts_edit(&ctx, &model, &out) == CF_OK);
    cf_golden_expect("account_edit_with_logo_member", (const char *)out.ptr,
                     out.len);
    cf_builder_dispose(&out);
    cf_view_accounts_edit_model_dispose(&model);
}

/* cf_golden_expect only reads golden/a/<name>.html; the stream fixture has a
 * turbo_stream.html extension, so this mirrors its comparison. */
static void stream_expect(const char *fixture, const cf_builder *generated) {
    size_t golden_len = 0;
    char *golden = cf_golden_read("a", fixture, "turbo_stream.html",
                                  &golden_len);
    CF_REQUIRE(golden != NULL);
    cf_golden_tokens want = {0}, got = {0};
    CF_REQUIRE(cf_golden_normalize(golden, golden_len, &want));
    CF_REQUIRE(cf_golden_normalize((const char *)generated->ptr,
                                   generated->len, &got));
    char *report = cf_golden_diff(&want, &got);
    if (report != NULL) {
        fprintf(stderr, "golden %s: DOM differs from the reference\n%s",
                fixture, report);
    }
    CF_CHECK(report == NULL);
    free(report);
    free(golden);
    cf_golden_tokens_dispose(&want);
    cf_golden_tokens_dispose(&got);
}

CF_TEST(accounts_users_stream_matches_golden) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "account_users_page_2", NULL, NULL));
    yyjson_val *page =
        yyjson_obj_get(cf_facts_case("account_users_page_2"), "page_users");
    CF_REQUIRE(page != NULL);
    cf_view_accounts_users_model model = {0};
    size_t index = 0, count = yyjson_arr_size(page);
    yyjson_val *name = NULL;
    yyjson_arr_iter iter = yyjson_arr_iter_with(page);
    while ((name = yyjson_arr_iter_next(&iter)) != NULL) {
        yyjson_val *users =
            yyjson_obj_get(cf_facts_case("account_users_page_2"), "users");
        yyjson_val *fact = yyjson_obj_get(users, yyjson_get_str(name));
        CF_REQUIRE(fact != NULL);
        CF_REQUIRE(push_fact(&model.users, fact));
        index++;
    }
    CF_REQUIRE(index == count && count == 11);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_accounts_users_stream(&ctx, &model, &out) == CF_OK);
    stream_expect("account_users_page_2", &out);
    cf_builder_dispose(&out);
    cf_view_accounts_users_model_dispose(&model);
}

static void custom_styles_matches(const char *fixture) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, fixture, NULL, NULL));
    yyjson_val *account = yyjson_obj_get(cf_facts_case(fixture), "account");
    yyjson_val *styles = yyjson_obj_get(account, "custom_styles");
    cf_view_accounts_custom_styles_model model = {0};
    if (styles != NULL && yyjson_is_str(styles)) {
        model.has_custom_styles = true;
        model.custom_styles = dup_str(yyjson_get_str(styles));
    }
    cf_builder out = {0};
    CF_REQUIRE(cf_view_accounts_custom_styles_edit(&ctx, &model, &out) ==
               CF_OK);
    cf_golden_expect(fixture, (const char *)out.ptr, out.len);
    cf_builder_dispose(&out);
    cf_view_accounts_custom_styles_model_dispose(&model);
}

CF_TEST(accounts_custom_styles_edit_matches_golden) {
    custom_styles_matches("custom_styles_edit");
}

CF_TEST(accounts_custom_styles_layout_matches_golden) {
    custom_styles_matches("custom_styles_layout");
}

/* ---- frames carry head/content only ---- */

CF_TEST(accounts_edit_frame_has_no_page_chrome) {
    cf_view_ctx ctx;
    cf_view_accounts_edit_model model;
    CF_REQUIRE(edit_model("account_edit_admin", NULL, &ctx, &model));
    cf_builder out = {0};
    CF_REQUIRE(cf_view_accounts_edit_frame(&ctx, &model, &out) == CF_OK);
    CF_CHECK(out.len > 0);
    CF_CHECK(cf_builder_contains(&out, "view-transition-name: "
                                       "account-settings"));
    CF_CHECK(cf_builder_contains(&out, "id=\"account_users\""));
    CF_CHECK(!cf_builder_contains(&out, "<!DOCTYPE html>"));
    CF_CHECK(!cf_builder_contains(&out, "<nav id=\"nav\""));
    CF_CHECK(!cf_builder_contains(&out, "<aside id=\"sidebar\""));
    cf_builder_dispose(&out);
    cf_view_accounts_edit_model_dispose(&model);
}

CF_TEST(accounts_custom_styles_frame_has_no_page_chrome) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "custom_styles_edit", NULL, NULL));
    cf_view_accounts_custom_styles_model model = {0};
    cf_builder out = {0};
    CF_REQUIRE(cf_view_accounts_custom_styles_edit_frame(&ctx, &model, &out) ==
               CF_OK);
    CF_CHECK(cf_builder_contains(&out, "Custom CSS"));
    CF_CHECK(cf_builder_contains(&out, "account[custom_styles]"));
    CF_CHECK(!cf_builder_contains(&out, "<!DOCTYPE html>"));
    CF_CHECK(!cf_builder_contains(&out, "<nav id=\"nav\""));
    cf_builder_dispose(&out);
    cf_view_accounts_custom_styles_model_dispose(&model);
}

/* ---- view-level behavior ---- */

static int admin_ctx(cf_view_ctx *ctx) {
    if (!cf_test_views_setup()) return 0;
    return cf_facts_view_ctx(ctx, "account_edit_admin", NULL, NULL);
}

static int member_ctx(cf_view_ctx *ctx) {
    if (!cf_test_views_setup()) return 0;
    return cf_facts_view_ctx(ctx, "account_edit_member", NULL, NULL);
}

static int push_named(cf_view_account_user_vector *vector,
                      const char *fixture, const char *name) {
    yyjson_val *fact = cf_facts_user(cf_facts_case(fixture), name);
    if (fact == NULL) return 0;
    return push_fact(vector, fact);
}

CF_TEST(accounts_user_partial_admin_sees_role_and_delete) {
    cf_view_ctx ctx;
    CF_REQUIRE(admin_ctx(&ctx));
    cf_view_account_user_vector users = {0};
    CF_REQUIRE(push_named(&users, "account_edit_admin", "Kevin"));
    cf_builder out = {0};
    CF_REQUIRE(cf_view_accounts_user_partial(&ctx, &users.items[0], &out) ==
               CF_OK);
    CF_CHECK(cf_builder_contains(&out, "Role: Member"));
    CF_CHECK(cf_builder_contains(&out, "for=\"role_user_"));
    CF_CHECK(cf_builder_contains(&out, "name=\"user[role]\""));
    CF_CHECK(cf_builder_contains(&out, "value=\"administrator\""));
    CF_CHECK(cf_builder_contains(&out, "Are you sure you want to "
                                       "permanently remove"));
    CF_CHECK(cf_builder_contains(&out, "Delete Kevin"));
    CF_CHECK(!cf_builder_contains(&out, "My settings"));
    cf_builder_dispose(&out);
    cf_view_account_user_vector_dispose(&users);
}

CF_TEST(accounts_user_partial_current_admin_is_disabled) {
    cf_view_ctx ctx;
    CF_REQUIRE(admin_ctx(&ctx));
    cf_view_account_user_vector users = {0};
    CF_REQUIRE(push_named(&users, "account_edit_admin", "David"));
    cf_builder out = {0};
    CF_REQUIRE(cf_view_accounts_user_partial(&ctx, &users.items[0], &out) ==
               CF_OK);
    CF_CHECK(cf_builder_contains(&out, "Role: Administrator"));
    CF_CHECK(cf_builder_contains(&out, "disabled=\"disabled\""));
    CF_CHECK(cf_builder_contains(&out, "checked=\"checked\""));
    CF_CHECK(cf_builder_contains(&out, "My settings"));
    CF_CHECK(cf_builder_contains(&out, "/users/me/profile"));
    /* No delete control for the current user. */
    CF_CHECK(!cf_builder_contains(&out, "Are you sure you want to "
                                       "permanently remove"));
    cf_builder_dispose(&out);
    cf_view_account_user_vector_dispose(&users);
}

CF_TEST(accounts_user_partial_banned_has_no_controls) {
    cf_view_ctx ctx;
    CF_REQUIRE(admin_ctx(&ctx));
    cf_view_account_user_vector users = {0};
    CF_REQUIRE(push_named(&users, "account_edit_admin", "Spam Ham"));
    cf_builder out = {0};
    CF_REQUIRE(cf_view_accounts_user_partial(&ctx, &users.items[0], &out) ==
               CF_OK);
    CF_CHECK(cf_builder_contains(&out, "margin-none banned"));
    CF_CHECK(cf_builder_contains(&out, "Spam Ham"));
    CF_CHECK(!cf_builder_contains(&out, "user[role]"));
    CF_CHECK(!cf_builder_contains(&out, "My settings"));
    cf_builder_dispose(&out);
    cf_view_account_user_vector_dispose(&users);
}

CF_TEST(accounts_user_partial_member_view_hides_admin_controls) {
    cf_view_ctx ctx;
    CF_REQUIRE(member_ctx(&ctx));
    cf_view_account_user_vector users = {0};
    CF_REQUIRE(push_named(&users, "account_edit_member", "David"));
    CF_REQUIRE(push_named(&users, "account_edit_member", "Kevin"));
    cf_builder out = {0};
    CF_REQUIRE(cf_view_accounts_user_partial(&ctx, &users.items[0], &out) ==
               CF_OK);
    CF_CHECK(!cf_builder_contains(&out, "user[role]"));
    CF_CHECK(!cf_builder_contains(&out, "My settings"));
    cf_builder_dispose(&out);
    CF_REQUIRE(cf_view_accounts_user_partial(&ctx, &users.items[1], &out) ==
               CF_OK);
    CF_CHECK(!cf_builder_contains(&out, "user[role]"));
    CF_CHECK(cf_builder_contains(&out, "My settings"));
    cf_builder_dispose(&out);
    cf_view_account_user_vector_dispose(&users);
}

CF_TEST(accounts_stream_actions_and_targets_are_byte_exact) {
    cf_view_ctx ctx;
    CF_REQUIRE(admin_ctx(&ctx));
    cf_view_accounts_users_model model = {0};
    CF_REQUIRE(push_named(&model.users, "account_edit_admin", "Kevin"));
    model.has_next_page = true;
    model.next_page = dup_str("3");
    cf_builder out = {0};
    CF_REQUIRE(cf_view_accounts_users_stream(&ctx, &model, &out) == CF_OK);
    CF_CHECK(cf_builder_contains(
        &out, "<turbo-stream action=\"replace\" "
              "target=\"next_page_container\">"));
    CF_CHECK(cf_builder_contains(
        &out, "<turbo-stream action=\"append\" target=\"account_users\">"));
    CF_CHECK(cf_builder_contains(&out, "/account/users.turbo_stream?page=3"));
    cf_builder_dispose(&out);
    cf_view_accounts_users_model_dispose(&model);
}

CF_TEST(accounts_stream_without_next_page_has_no_append) {
    cf_view_ctx ctx;
    CF_REQUIRE(admin_ctx(&ctx));
    cf_view_accounts_users_model model = {0};
    cf_builder out = {0};
    CF_REQUIRE(cf_view_accounts_users_stream(&ctx, &model, &out) == CF_OK);
    CF_CHECK(cf_builder_contains(
        &out, "<turbo-stream action=\"replace\" "
              "target=\"next_page_container\">"));
    CF_CHECK(!cf_builder_contains(&out, "action=\"append\""));
    CF_CHECK(!cf_builder_contains(&out, "next_page_container\" src="));
    cf_builder_dispose(&out);
    cf_view_accounts_users_model_dispose(&model);
}

CF_TEST(accounts_invite_shows_regenerate_only_to_admins) {
    cf_view_ctx admin, member;
    CF_REQUIRE(admin_ctx(&admin));
    CF_REQUIRE(member_ctx(&member));
    cf_builder a = {0}, m = {0};
    CF_REQUIRE(cf_view_accounts_invite(&admin, span_of("CRMu-l8Ge-KB9B"),
                                       &a) == CF_OK);
    CF_REQUIRE(cf_view_accounts_invite(&member, span_of("CRMu-l8Ge-KB9B"),
                                       &m) == CF_OK);
    CF_CHECK(cf_builder_contains(&a, "http://campfire.test/join/"
                                     "CRMu-l8Ge-KB9B"));
    CF_CHECK(cf_builder_contains(&a, "/account/join_code"));
    CF_CHECK(cf_builder_contains(&a, "Regenerate join link"));
    CF_CHECK(cf_builder_contains(&m, "id=\"invite_url\""));
    CF_CHECK(!cf_builder_contains(&m, "/account/join_code"));
    CF_CHECK(!cf_builder_contains(&m, "Regenerate join link"));
    cf_builder_dispose(&a);
    cf_builder_dispose(&m);
}

CF_TEST(accounts_settings_toggle_reflects_the_flag) {
    cf_view_ctx admin;
    CF_REQUIRE(admin_ctx(&admin));
    /* Restricted: the hidden field offers "false" and the box is checked. */
    cf_view_accounts_edit_model on = {0};
    on.account_id = 1;
    on.join_code = dup_str("ABCD-EFGH-IJKL");
    on.restrict_room_creation_to_administrators = true;
    on.has_last_room_visited = false;
    cf_builder out = {0};
    CF_REQUIRE(cf_view_accounts_edit(&admin, &on, &out) == CF_OK);
    CF_CHECK(cf_builder_contains(
        &out, "name=\"account[settings]"
              "[restrict_room_creation_to_administrators]\""));
    CF_CHECK(cf_builder_contains(
        &out, "id=\"account_settings_restrict_room_creation_to_"
              "administrators\""));
    CF_CHECK(cf_builder_contains(&out, "value=\"false\""));
    CF_CHECK(cf_builder_contains(&out, "checked"));
    cf_builder_dispose(&out);
    cf_view_accounts_edit_model_dispose(&on);
    /* Unrestricted: the hidden field offers "true", the box is unchecked. */
    cf_view_accounts_edit_model off = {0};
    off.account_id = 1;
    off.join_code = dup_str("ABCD-EFGH-IJKL");
    cf_builder bare = {0};
    CF_REQUIRE(cf_view_accounts_edit(&admin, &off, &bare) == CF_OK);
    CF_CHECK(cf_builder_contains(&bare, "value=\"true\""));
    CF_CHECK(!cf_builder_contains(&bare, "checked"));
    cf_builder_dispose(&bare);
    cf_view_accounts_edit_model_dispose(&off);
}

CF_TEST(accounts_member_view_hides_admin_forms) {
    cf_view_ctx ctx;
    CF_REQUIRE(member_ctx(&ctx));
    cf_view_accounts_edit_model model = {0};
    CF_REQUIRE(push_named(&model.members, "account_edit_member", "Kevin"));
    model.account_id = 7;
    model.join_code = dup_str("ABCD-EFGH-IJKL");
    cf_builder out = {0};
    CF_REQUIRE(cf_view_accounts_edit(&ctx, &model, &out) == CF_OK);
    CF_CHECK(cf_builder_contains(&out, "account-logo avatar"));
    CF_CHECK(!cf_builder_contains(&out, "account[name]"));
    CF_CHECK(!cf_builder_contains(&out, "account[logo]"));
    CF_CHECK(!cf_builder_contains(&out, "/account/bots"));
    CF_CHECK(!cf_builder_contains(&out, "user[role]"));
    cf_builder_dispose(&out);
    cf_view_accounts_edit_model_dispose(&model);
}

CF_TEST(accounts_next_page_container_points_at_the_page) {
    cf_builder out = {0};
    CF_REQUIRE(cf_view_accounts_next_page(span_of("9"), &out) == CF_OK);
    CF_CHECK(cf_builder_contains(
        &out, "src=\"/account/users.turbo_stream?page=9\""));
    CF_CHECK(cf_builder_contains(&out, "id=\"next_page_container\""));
    CF_CHECK(cf_builder_contains(&out, "spinner"));
    cf_builder_dispose(&out);
}

CF_TEST(accounts_renders_reject_null_inputs) {
    cf_builder out = {0};
    cf_view_accounts_edit_model edit = {0};
    cf_view_accounts_users_model users = {0};
    cf_view_accounts_custom_styles_model styles = {0};
    cf_view_ctx ctx;
    CF_REQUIRE(admin_ctx(&ctx));
    cf_view_account_user_vector vector = {0};
    CF_REQUIRE(push_named(&vector, "account_edit_admin", "Kevin"));
    CF_CHECK(cf_view_accounts_edit(NULL, &edit, &out) == CF_INVALID);
    CF_CHECK(cf_view_accounts_edit(&ctx, NULL, &out) == CF_INVALID);
    CF_CHECK(cf_view_accounts_edit(&ctx, &edit, NULL) == CF_INVALID);
    CF_CHECK(cf_view_accounts_edit_frame(&ctx, NULL, &out) == CF_INVALID);
    CF_CHECK(cf_view_accounts_users_stream(NULL, &users, &out) == CF_INVALID);
    CF_CHECK(cf_view_accounts_users_stream(&ctx, NULL, &out) == CF_INVALID);
    CF_CHECK(cf_view_accounts_users_stream(&ctx, &users, NULL) == CF_INVALID);
    CF_CHECK(cf_view_accounts_custom_styles_edit(NULL, &styles, &out) ==
             CF_INVALID);
    CF_CHECK(cf_view_accounts_custom_styles_edit_frame(&ctx, NULL, &out) ==
             CF_INVALID);
    CF_CHECK(cf_view_accounts_invite(NULL, span_of("x"), &out) == CF_INVALID);
    CF_CHECK(cf_view_accounts_invite(&ctx, span_of("x"), NULL) == CF_INVALID);
    CF_CHECK(cf_view_accounts_user_partial(NULL, &vector.items[0], &out) ==
             CF_INVALID);
    CF_CHECK(cf_view_accounts_user_partial(&ctx, NULL, &out) == CF_INVALID);
    CF_CHECK(cf_view_accounts_next_page(span_of("2"), NULL) == CF_INVALID);
    CF_CHECK(cf_view_account_user_assign(NULL, 1, span_of("a"),
                                         span_of("a"), span_of("/x"),
                                         CF_ROLE_MEMBER,
                                         CF_STATUS_ACTIVE) == CF_INVALID);
    CF_CHECK(cf_view_account_user_vector_push(NULL, 1, span_of("a"),
                                              span_of("a"), span_of("/x"),
                                              CF_ROLE_MEMBER,
                                              CF_STATUS_ACTIVE) == CF_INVALID);
    CF_CHECK(out.len == 0);
    cf_builder_dispose(&out);
    cf_view_account_user_vector_dispose(&vector);
}
