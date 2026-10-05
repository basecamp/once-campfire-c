/* V-E rooms subclass forms + sidebar partials: VIEW-03 goldens for
 * rooms/opens|closeds|directs new/edit (golden/b) plus view-level cases the
 * fixture comparison cannot express (conditional controls, participant
 * selection, frame variants, broadcast partials incl. the involvements
 * update-from-invisible prepend path).
 *
 * The renderers live in src/views/rooms_forms.c; their declarations belong
 * in src/views.h (integrator).  Until they land there they are mirrored
 * here; the mirror must match rooms_forms.c exactly.
 */
#include "cf_test.h"
#include "support/golden.h"
#include "support/golden_b.h"
#include "views.h"

#include "app.h"
#include "cable/channels.h"
#include "config.h"
#include "db/db_testutil.h"
#include "models/membership.h"
#include "models/room.h"
#include "models/user.h"

#include <inttypes.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

/* ---- mirror of the proposed src/views.h addition (see rooms_forms.c) ------ */

typedef struct {
    bool is_new;
    int64_t room_id;
    bool has_name;
    cf_str name;
    bool can_administer;
    bool has_last_room_id;
    int64_t last_room_id;
    const cf_view_user *users;
    size_t user_count;
} cf_view_rooms_open_form_model;

typedef struct {
    bool is_new;
    int64_t room_id;
    bool has_name;
    cf_str name;
    bool can_administer;
    int64_t current_user_id;
    bool has_last_room_id;
    int64_t last_room_id;
    const cf_view_user *selected;
    size_t selected_count;
    const cf_view_user *unselected;
    size_t unselected_count;
} cf_view_rooms_closed_form_model;

typedef struct {
    int64_t room_id;
    cf_str display_name;
    bool has_last_room_id;
    int64_t last_room_id;
    const cf_view_user *users;
    size_t user_count;
} cf_view_rooms_direct_edit_model;

void cf_view_rooms_open_form_dispose(cf_view_rooms_open_form_model *model);
void cf_view_rooms_closed_form_dispose(cf_view_rooms_closed_form_model *model);
void cf_view_rooms_direct_edit_dispose(cf_view_rooms_direct_edit_model *model);
cf_err cf_view_rooms_open_form(const cf_view_ctx *ctx,
                               const cf_view_rooms_open_form_model *model,
                               cf_builder *out);
cf_err cf_view_rooms_open_form_frame(const cf_view_ctx *ctx,
                                     const cf_view_rooms_open_form_model *model,
                                     cf_builder *out);
cf_err cf_view_rooms_closed_form(const cf_view_ctx *ctx,
                                 const cf_view_rooms_closed_form_model *model,
                                 cf_builder *out);
cf_err cf_view_rooms_closed_form_frame(
    const cf_view_ctx *ctx, const cf_view_rooms_closed_form_model *model,
    cf_builder *out);
cf_err cf_view_rooms_direct_new(const cf_view_ctx *ctx, cf_builder *out);
cf_err cf_view_rooms_direct_new_frame(const cf_view_ctx *ctx, cf_builder *out);
cf_err cf_view_rooms_direct_edit(const cf_view_ctx *ctx,
                                 const cf_view_rooms_direct_edit_model *model,
                                 cf_builder *out);
cf_err cf_view_rooms_direct_edit_frame(
    const cf_view_ctx *ctx, const cf_view_rooms_direct_edit_model *model,
    cf_builder *out);
cf_err cf_view_rooms_shared_room_partial(void *user, const cf_room *room,
                                         cf_builder *out);
cf_err cf_view_rooms_direct_room_partial(void *user,
                                         const cf_membership *membership,
                                         cf_builder *out);

/* ---- helpers -------------------------------------------------------------- */

static cf_span lit(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

static void copy_str(const char *text, cf_str *out) {
    memset(out, 0, sizeof *out);
    if (text == NULL) return;
    size_t len = strlen(text);
    char *copy = malloc(len + 1);
    if (copy == NULL) return;
    memcpy(copy, text, len + 1);
    out->ptr = copy;
    out->len = len;
}

/* last_room_visited_id from a golden/b context (absent -> no back link). */
static void golden_last_room(yyjson_doc *doc, bool *has_out, int64_t *id_out) {
    *has_out = false;
    *id_out = 0;
    yyjson_val *context =
        yyjson_obj_get(yyjson_doc_get_root(doc), "context");
    if (context == NULL) return;
    yyjson_val *last = yyjson_obj_get(context, "last_room_visited_id");
    if (last == NULL || !yyjson_is_int(last)) return;
    *has_out = true;
    *id_out = yyjson_get_sint(last);
}

static bool load_users(yyjson_val *array, cf_view_user **out, size_t *count) {
    *out = NULL;
    *count = 0;
    if (array == NULL || !yyjson_is_arr(array)) return true;
    size_t n = yyjson_arr_size(array);
    if (n == 0) return true;
    cf_view_user *users = calloc(n, sizeof *users);
    if (users == NULL) return false;
    size_t i = 0;
    yyjson_val *item;
    yyjson_arr_iter iter = yyjson_arr_iter_with(array);
    while ((item = yyjson_arr_iter_next(&iter)) != NULL && i < n) {
        cf_golden_b_user(item, &users[i]);
        i++;
    }
    *out = users;
    *count = n;
    return true;
}

static void free_users(cf_view_user *users, size_t count) {
    if (users == NULL) return;
    for (size_t i = 0; i < count; i++) cf_view_user_dispose(&users[i]);
    free(users);
}

/* ---- opens goldens -------------------------------------------------------- */

static void open_form_from(yyjson_doc *doc, bool is_new, int64_t room_id,
                           cf_view_ctx *ctx, cf_view_rooms_open_form_model *m,
                           cf_view_user **users, size_t *count) {
    memset(m, 0, sizeof *m);
    yyjson_val *input = yyjson_obj_get(yyjson_doc_get_root(doc), "input");
    yyjson_val *room = yyjson_obj_get(input, "room");
    m->is_new = is_new;
    m->room_id = room_id;
    yyjson_val *name = yyjson_obj_get(room, "name");
    if (name != NULL && yyjson_is_str(name)) {
        m->has_name = true;
        copy_str(yyjson_get_str(name), &m->name);
    }
    m->can_administer = yyjson_get_bool(yyjson_obj_get(input, "can_administer"));
    golden_last_room(doc, &m->has_last_room_id, &m->last_room_id);
    CF_REQUIRE(load_users(yyjson_obj_get(input, "users"), users, count));
    m->users = *users;
    m->user_count = *count;
    cf_golden_b_ctx(ctx, doc);
}

static void rooms_opens_matches(const char *name, bool is_new, int64_t id) {
    yyjson_doc *doc = cf_golden_b_load(name);
    CF_REQUIRE(doc != NULL);
    cf_view_ctx ctx;
    cf_view_rooms_open_form_model m;
    cf_view_user *users = NULL;
    size_t count = 0;
    open_form_from(doc, is_new, id, &ctx, &m, &users, &count);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_rooms_open_form(&ctx, &m, &out) == CF_OK);
    cf_golden_b_expect(doc, name, 1, out.ptr, out.len);
    cf_builder_dispose(&out);
    free_users(users, count);
    cf_view_rooms_open_form_dispose(&m);
    yyjson_doc_free(doc);
}

CF_TEST(rooms_opens_new_matches_golden) {
    rooms_opens_matches("rooms_opens_new", true, 0);
}

CF_TEST(rooms_opens_edit_matches_golden) {
    rooms_opens_matches("rooms_opens_edit", false, 201306877);
}

CF_TEST(rooms_opens_edit_member_matches_golden) {
    rooms_opens_matches("rooms_opens_edit_member", false, 201306877);
}

/* The frame render carries the head and content blocks only. */
CF_TEST(rooms_opens_new_frame_has_form_and_no_page_chrome) {
    yyjson_doc *doc = cf_golden_b_load("rooms_opens_new");
    CF_REQUIRE(doc != NULL);
    cf_view_ctx ctx;
    cf_view_rooms_open_form_model m;
    cf_view_user *users = NULL;
    size_t count = 0;
    open_form_from(doc, true, 0, &ctx, &m, &users, &count);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_rooms_open_form_frame(&ctx, &m, &out) == CF_OK);
    CF_CHECK(cf_builder_contains(&out, "<form action=\"/rooms/opens\""));
    CF_CHECK(cf_builder_contains(&out, "name=\"room[name]\""));
    CF_CHECK(cf_builder_contains(&out, "Give only some access to this room"));
    CF_CHECK(cf_builder_contains(&out, "<nav id=\"nav\"") == 0);
    CF_CHECK(cf_builder_contains(&out, "<aside id=\"sidebar\"") == 0);
    cf_builder_dispose(&out);
    free_users(users, count);
    cf_view_rooms_open_form_dispose(&m);
    yyjson_doc_free(doc);
}

/* Open rows never carry user_ids[] grant controls (Everyone has access). */
CF_TEST(rooms_opens_rows_carry_no_grant_checkboxes) {
    yyjson_doc *doc = cf_golden_b_load("rooms_opens_new");
    CF_REQUIRE(doc != NULL);
    cf_view_ctx ctx;
    cf_view_rooms_open_form_model m;
    cf_view_user *users = NULL;
    size_t count = 0;
    open_form_from(doc, true, 0, &ctx, &m, &users, &count);
    CF_REQUIRE(m.can_administer);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_rooms_open_form(&ctx, &m, &out) == CF_OK);
    CF_CHECK(cf_builder_contains(&out, "user_ids[]") == 0);
    CF_CHECK(cf_builder_contains(&out, "switch__input") != 0); /* type switch */
    cf_builder_dispose(&out);
    free_users(users, count);
    cf_view_rooms_open_form_dispose(&m);
    yyjson_doc_free(doc);
}

CF_TEST(rooms_open_form_rejects_nulls) {
    cf_builder out = {0};
    CF_CHECK(cf_view_rooms_open_form(NULL, NULL, &out) == CF_INVALID);
    CF_CHECK(cf_view_rooms_open_form_frame(NULL, NULL, &out) == CF_INVALID);
    cf_builder_dispose(&out);
}

/* ---- closeds goldens ------------------------------------------------------ */

static void closed_form_from(yyjson_doc *doc, bool is_new, int64_t room_id,
                             cf_view_ctx *ctx,
                             cf_view_rooms_closed_form_model *m,
                             cf_view_user **sel, size_t *seln,
                             cf_view_user **unsel, size_t *unseln) {
    memset(m, 0, sizeof *m);
    yyjson_val *input = yyjson_obj_get(yyjson_doc_get_root(doc), "input");
    yyjson_val *room = yyjson_obj_get(input, "room");
    m->is_new = is_new;
    m->room_id = room_id;
    yyjson_val *name = yyjson_obj_get(room, "name");
    if (name != NULL && yyjson_is_str(name)) {
        m->has_name = true;
        copy_str(yyjson_get_str(name), &m->name);
    }
    m->can_administer = yyjson_get_bool(yyjson_obj_get(input, "can_administer"));
    yyjson_val *cuid = yyjson_obj_get(input, "current_user_id");
    m->current_user_id = (cuid != NULL && yyjson_is_int(cuid))
                             ? yyjson_get_sint(cuid)
                             : 0;
    golden_last_room(doc, &m->has_last_room_id, &m->last_room_id);
    CF_REQUIRE(load_users(yyjson_obj_get(input, "selected_users"), sel, seln));
    CF_REQUIRE(
        load_users(yyjson_obj_get(input, "unselected_users"), unsel, unseln));
    m->selected = *sel;
    m->selected_count = *seln;
    m->unselected = *unsel;
    m->unselected_count = *unseln;
    cf_golden_b_ctx(ctx, doc);
}

static void rooms_closeds_matches(const char *name, bool is_new, int64_t id) {
    yyjson_doc *doc = cf_golden_b_load(name);
    CF_REQUIRE(doc != NULL);
    cf_view_ctx ctx;
    cf_view_rooms_closed_form_model m;
    cf_view_user *sel = NULL, *unsel = NULL;
    size_t seln = 0, unseln = 0;
    closed_form_from(doc, is_new, id, &ctx, &m, &sel, &seln, &unsel, &unseln);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_rooms_closed_form(&ctx, &m, &out) == CF_OK);
    cf_golden_b_expect(doc, name, 1, out.ptr, out.len);
    cf_builder_dispose(&out);
    free_users(sel, seln);
    free_users(unsel, unseln);
    cf_view_rooms_closed_form_dispose(&m);
    yyjson_doc_free(doc);
}

CF_TEST(rooms_closeds_new_matches_golden) {
    rooms_closeds_matches("rooms_closeds_new", true, 0);
}

CF_TEST(rooms_closeds_edit_matches_golden) {
    rooms_closeds_matches("rooms_closeds_edit", false, 654632876);
}

CF_TEST(rooms_closeds_edit_member_matches_golden) {
    rooms_closeds_matches("rooms_closeds_edit_member", false, 654632876);
}

CF_TEST(rooms_closeds_new_frame_has_grant_controls) {
    yyjson_doc *doc = cf_golden_b_load("rooms_closeds_new");
    CF_REQUIRE(doc != NULL);
    cf_view_ctx ctx;
    cf_view_rooms_closed_form_model m;
    cf_view_user *sel = NULL, *unsel = NULL;
    size_t seln = 0, unseln = 0;
    closed_form_from(doc, true, 0, &ctx, &m, &sel, &seln, &unsel, &unseln);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_rooms_closed_form_frame(&ctx, &m, &out) == CF_OK);
    CF_CHECK(cf_builder_contains(&out, "<form action=\"/rooms/closeds\""));
    CF_CHECK(cf_builder_contains(&out, "name=\"user_ids[]\""));
    CF_CHECK(cf_builder_contains(&out, "<nav id=\"nav\"") == 0);
    cf_builder_dispose(&out);
    free_users(sel, seln);
    free_users(unsel, unseln);
    cf_view_rooms_closed_form_dispose(&m);
    yyjson_doc_free(doc);
}

/* New closed rooms pin the creator: hidden input + check, no checkbox. */
CF_TEST(rooms_closeds_new_pins_creator_without_checkbox) {
    yyjson_doc *doc = cf_golden_b_load("rooms_closeds_new");
    CF_REQUIRE(doc != NULL);
    cf_view_ctx ctx;
    cf_view_rooms_closed_form_model m;
    cf_view_user *sel = NULL, *unsel = NULL;
    size_t seln = 0, unseln = 0;
    closed_form_from(doc, true, 0, &ctx, &m, &sel, &seln, &unsel, &unseln);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_rooms_closed_form(&ctx, &m, &out) == CF_OK);
    char want[128];
    snprintf(want, sizeof want,
             "<input type=\"hidden\" name=\"user_ids[]\" value=\"%lld\"",
             (long long)m.current_user_id);
    CF_CHECK(cf_builder_contains(&out, want));
    /* The pinned creator row carries the check image, other rows checkboxes. */
    CF_CHECK(cf_builder_contains(&out, "type=\"checkbox\" name=\"user_ids[]\""));
    cf_builder_dispose(&out);
    free_users(sel, seln);
    free_users(unsel, unseln);
    cf_view_rooms_closed_form_dispose(&m);
    yyjson_doc_free(doc);
}

/* Persisted rooms render selected/unselected as checked/unchecked boxes. */
CF_TEST(rooms_closeds_edit_checks_selected_users) {
    yyjson_doc *doc = cf_golden_b_load("rooms_closeds_edit");
    CF_REQUIRE(doc != NULL);
    cf_view_ctx ctx;
    cf_view_rooms_closed_form_model m;
    cf_view_user *sel = NULL, *unsel = NULL;
    size_t seln = 0, unseln = 0;
    closed_form_from(doc, false, 654632876, &ctx, &m, &sel, &seln, &unsel,
                     &unseln);
    CF_REQUIRE(seln > 0 && unseln > 0);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_rooms_closed_form(&ctx, &m, &out) == CF_OK);
    char want[160];
    snprintf(want, sizeof want,
             "name=\"user_ids[]\" value=\"%lld\" class=\"switch__input\" "
             "checked=\"checked\"",
             (long long)sel[0].id);
    CF_CHECK(cf_builder_contains(&out, want));
    snprintf(want, sizeof want,
             "name=\"user_ids[]\" value=\"%lld\" class=\"switch__input\" />",
             (long long)unsel[0].id);
    CF_CHECK(cf_builder_contains(&out, want));
    /* No pinning on persisted rooms: no hidden user_ids[] input. */
    CF_CHECK(cf_builder_contains(&out, "<input type=\"hidden\" "
                                       "name=\"user_ids[]\"") == 0);
    cf_builder_dispose(&out);
    free_users(sel, seln);
    free_users(unsel, unseln);
    cf_view_rooms_closed_form_dispose(&m);
    yyjson_doc_free(doc);
}

CF_TEST(rooms_closed_form_rejects_nulls) {
    cf_builder out = {0};
    CF_CHECK(cf_view_rooms_closed_form(NULL, NULL, &out) == CF_INVALID);
    CF_CHECK(cf_view_rooms_closed_form_frame(NULL, NULL, &out) == CF_INVALID);
    cf_builder_dispose(&out);
}

/* ---- directs goldens ------------------------------------------------------ */

CF_TEST(rooms_directs_new_matches_golden) {
    yyjson_doc *doc = cf_golden_b_load("rooms_directs_new");
    CF_REQUIRE(doc != NULL);
    cf_view_ctx ctx;
    cf_golden_b_ctx(&ctx, doc);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_rooms_direct_new(&ctx, &out) == CF_OK);
    cf_golden_b_expect(doc, "rooms_directs_new", 1, out.ptr, out.len);
    cf_builder_dispose(&out);
    yyjson_doc_free(doc);
}

static void direct_edit_from(yyjson_doc *doc, cf_view_ctx *ctx,
                             cf_view_rooms_direct_edit_model *m,
                             cf_view_user **users, size_t *count) {
    memset(m, 0, sizeof *m);
    yyjson_val *input = yyjson_obj_get(yyjson_doc_get_root(doc), "input");
    m->room_id = yyjson_get_sint(yyjson_obj_get(input, "room_id"));
    copy_str(yyjson_get_str(yyjson_obj_get(input, "display_name")),
             &m->display_name);
    golden_last_room(doc, &m->has_last_room_id, &m->last_room_id);
    CF_REQUIRE(load_users(yyjson_obj_get(input, "users"), users, count));
    m->users = *users;
    m->user_count = *count;
    cf_golden_b_ctx(ctx, doc);
}

CF_TEST(rooms_directs_edit_matches_golden) {
    yyjson_doc *doc = cf_golden_b_load("rooms_directs_edit");
    CF_REQUIRE(doc != NULL);
    cf_view_ctx ctx;
    cf_view_rooms_direct_edit_model m;
    cf_view_user *users = NULL;
    size_t count = 0;
    direct_edit_from(doc, &ctx, &m, &users, &count);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_rooms_direct_edit(&ctx, &m, &out) == CF_OK);
    cf_golden_b_expect(doc, "rooms_directs_edit", 1, out.ptr, out.len);
    cf_builder_dispose(&out);
    free_users(users, count);
    cf_view_rooms_direct_edit_dispose(&m);
    yyjson_doc_free(doc);
}

CF_TEST(rooms_directs_new_frame_has_autocomplete_and_no_chrome) {
    yyjson_doc *doc = cf_golden_b_load("rooms_directs_new");
    CF_REQUIRE(doc != NULL);
    cf_view_ctx ctx;
    cf_golden_b_ctx(&ctx, doc);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_rooms_direct_new_frame(&ctx, &out) == CF_OK);
    CF_CHECK(cf_builder_contains(&out, "id=\"direct_rooms_control\""));
    CF_CHECK(cf_builder_contains(&out, "name=\"user_ids[]\""));
    CF_CHECK(cf_builder_contains(&out, "<nav id=\"nav\"") == 0);
    CF_CHECK(cf_builder_contains(&out, "<aside id=\"sidebar\"") == 0);
    cf_builder_dispose(&out);
    yyjson_doc_free(doc);
}

CF_TEST(rooms_directs_edit_has_delete_ping_form) {
    yyjson_doc *doc = cf_golden_b_load("rooms_directs_edit");
    CF_REQUIRE(doc != NULL);
    cf_view_ctx ctx;
    cf_view_rooms_direct_edit_model m;
    cf_view_user *users = NULL;
    size_t count = 0;
    direct_edit_from(doc, &ctx, &m, &users, &count);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_rooms_direct_edit(&ctx, &m, &out) == CF_OK);
    CF_CHECK(cf_builder_contains(&out, "name=\"_method\" value=\"delete\""));
    CF_CHECK(cf_builder_contains(&out, "Delete Ping"));
    cf_builder_dispose(&out);
    free_users(users, count);
    cf_view_rooms_direct_edit_dispose(&m);
    yyjson_doc_free(doc);
}

CF_TEST(rooms_direct_forms_reject_nulls) {
    cf_builder out = {0};
    CF_CHECK(cf_view_rooms_direct_new(NULL, &out) == CF_INVALID);
    CF_CHECK(cf_view_rooms_direct_new_frame(NULL, &out) == CF_INVALID);
    CF_CHECK(cf_view_rooms_direct_edit(NULL, NULL, &out) == CF_INVALID);
    CF_CHECK(cf_view_rooms_direct_edit_frame(NULL, NULL, &out) == CF_INVALID);
    cf_builder_dispose(&out);
}

/* ---- sidebar partials ----------------------------------------------------- */

/* Minimal asset resolver for partial tests (no digests needed). */
static cf_err stub_asset(void *user, cf_span logical, cf_builder *out) {
    (void)user;
    if (logical.ptr == NULL || out == NULL) return CF_INVALID;
    cf_err rc = cf_builder_append(out, lit("/assets/"));
    if (rc == CF_OK) rc = cf_builder_append(out, logical);
    return rc;
}

CF_TEST(rooms_shared_room_partial_renders_list_anchor) {
    cf_view_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.asset_path = stub_asset;
    cf_room room;
    memset(&room, 0, sizeof room);
    room.id = 42;
    room.room_type = CF_ROOM_OPEN;
    room.name.present = true;
    room.name.value.ptr = "HQ";
    room.name.value.len = 2;
    cf_builder out = {0};
    CF_REQUIRE(cf_view_rooms_shared_room_partial(NULL, &room, &out) == CF_OK);
    CF_CHECK(cf_builder_contains(&out, "id=\"list_rooms_open_42\""));
    CF_CHECK(cf_builder_contains(&out, "href=\"/rooms/42\""));
    CF_CHECK(cf_builder_contains(&out, ">HQ</span>"));
    CF_CHECK(cf_builder_contains(&out, " unread\"") == 0);
    cf_builder_dispose(&out);
    CF_CHECK(cf_view_rooms_shared_room_partial(NULL, NULL, &out) ==
             CF_INVALID);
    CF_CHECK(cf_view_rooms_shared_room_partial(NULL, &room, NULL) ==
             CF_INVALID);
    cf_builder_dispose(&out);
}

CF_TEST(rooms_shared_room_partial_closed_and_nameless) {
    cf_view_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.asset_path = stub_asset;
    (void)ctx;
    cf_room room;
    memset(&room, 0, sizeof room);
    room.id = 43;
    room.room_type = CF_ROOM_CLOSED;
    room.name.present = false;
    cf_builder out = {0};
    CF_REQUIRE(cf_view_rooms_shared_room_partial(NULL, &room, &out) == CF_OK);
    CF_CHECK(cf_builder_contains(&out, "id=\"list_rooms_closed_43\""));
    cf_builder_dispose(&out);
}

/* ---- broadcast-level env (prepend path) ----------------------------------- */

#define FORM_ORIGIN "http://campfire.test"
#define FORM_HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    cf_cable *cable;
} forms_env;

static bool forms_env_open(forms_env *env) {
    memset(env, 0, sizeof *env);
    if (!cf_db_scratch_open(&env->scratch)) return false;
    cf_config_entry entries[4] = {
        {"PUBLIC_ORIGIN", FORM_ORIGIN},
        {"SECRET_KEY_BASE", FORM_HEX64},
        {"VAPID_PUBLIC_KEY",
         "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8c"
         "Triz_qYBVicY02_VxTQ="},
        {"DATABASE_PATH", env->scratch.path},
    };
    if (cf_config_parse(entries, 4, NULL, &env->config) != CF_OK) return false;
    if (cf_app_create(env->config, &env->app) != CF_OK) return false;
    cf_cable_config cable_config;
    memset(&cable_config, 0, sizeof cable_config);
    cable_config.app = env->app;
    cable_config.database_path = env->scratch.path;
    if (cf_cable_create(&cable_config, &env->cable) != CF_OK) return false;
    return true;
}

static void forms_env_close(forms_env *env) {
    if (env->cable != NULL) cf_cable_destroy(env->cable);
    if (env->app != NULL) cf_app_destroy(env->app);
    env->config = NULL;
    cf_db_scratch_close(&env->scratch);
    memset(env, 0, sizeof *env);
}

/* The involvements update-from-invisible prepend branch: with the real
 * shared-room partial the broadcast builds (previously CF_NOT_FOUND after
 * commit); the NULL slot still reports CF_NOT_FOUND. */
CF_TEST(rooms_involvement_prepend_branch_uses_shared_partial) {
    forms_env env;
    CF_REQUIRE(forms_env_open(&env));
    cf_room room;
    memset(&room, 0, sizeof room);
    room.id = 10;
    room.room_type = CF_ROOM_OPEN;
    room.name.present = true;
    room.name.value.ptr = "HQ";
    room.name.value.len = 2;
    cf_membership membership;
    memset(&membership, 0, sizeof membership);
    membership.id = 21;
    membership.room_id = 10;
    membership.user_id = 7;
    membership.involvement.present = true;
    membership.involvement.value = CF_INVOLVEMENT_EVERYTHING;

    cf_broadcast_partials partials;
    memset(&partials, 0, sizeof partials);
    partials.shared_room = cf_view_rooms_shared_room_partial;
    CF_CHECK(cf_broadcast_involvement_change(env.cable, &room, &membership,
                                             true,
                                             CF_INVOLVEMENT_INVISIBLE,
                                             &partials) == CF_OK);

    /* The old gap: no partial installed. */
    cf_broadcast_partials missing;
    memset(&missing, 0, sizeof missing);
    CF_CHECK(cf_broadcast_involvement_change(env.cable, &room, &membership,
                                             true,
                                             CF_INVOLVEMENT_INVISIBLE,
                                             &missing) == CF_NOT_FOUND);

    /* To-invisible removes without any partial. */
    membership.involvement.value = CF_INVOLVEMENT_INVISIBLE;
    CF_CHECK(cf_broadcast_involvement_change(env.cable, &room, &membership,
                                             true,
                                             CF_INVOLVEMENT_MENTIONS,
                                             &missing) == CF_OK);
    forms_env_close(&env);
}

/* The direct-room partial needs the broadcast views pair; NULL is rejected. */
CF_TEST(rooms_direct_room_partial_rejects_null_views) {
    cf_membership membership;
    memset(&membership, 0, sizeof membership);
    cf_builder out = {0};
    CF_CHECK(cf_view_rooms_direct_room_partial(NULL, &membership, &out) ==
             CF_INVALID);
    CF_CHECK(cf_view_rooms_direct_room_partial((void *)1, NULL, &out) ==
             CF_INVALID);
    cf_builder_dispose(&out);
}
