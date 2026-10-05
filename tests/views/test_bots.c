/* V-B accounts/bots views: goldens (VIEW-03), the R-BOTS-VIEWS shim facts and
 * the bot/bot_form presenters.
 *
 * Golden inputs mirror tmp/rust-ref/crates/views/tests/parity_a.rs
 * `accounts_bots`: the case context from facts.json, bots as the active-bot
 * users with their facts `bot_key`/`avatar_path`, rooms as
 * `bot.rooms.without_directs.ordered` (root memberships joined to root
 * rooms, directs dropped, ASCII LOWER(name) order), and the BotForm facts
 * (Bender Bot's name/webhook_url, the case `avatar_url` for the avatar
 * variant).  The comparison is support/golden.c with only the runner's
 * forgery masks.
 *
 * Presenter cases seed a scratch database through raw SQL (the reference's
 * SQL is the oracle): ordering, key/title/avatar/room/webhook facts, the
 * current-key-only rotation display and the no-SQL render that follows.
 */
#include "cf_test.h"

#include "support/facts.h"
#include "support/golden.h"
#include "views.h"

#include "app.h"
#include "cf.h"
#include "config.h"
#include "context.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "models/room.h"
#include "models/user.h"
#include "presenters/bots.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define ORIGIN "http://campfire.test"
#define T1_TEXT "2026-09-26 13:00:20.000000"

/* --- small helpers --------------------------------------------------------- */

static cf_str own(const char *text) {
    cf_str out = {0};
    if (text == NULL) text = "";
    size_t len = strlen(text);
    char *copy = malloc(len + 1);
    if (copy == NULL) return out;
    memcpy(copy, text, len + 1);
    out.ptr = copy;
    out.len = len;
    return out;
}

static int contains(const cf_builder *body, const char *needle) {
    size_t len = strlen(needle);
    if (len == 0) return 1;
    if (body == NULL || body->len < len) return 0;
    for (size_t i = 0; i + len <= body->len; i++) {
        if (memcmp(body->ptr + i, needle, len) == 0) return 1;
    }
    return 0;
}

/* The facts.json root (rooms/memberships live there, not in the case), read
 * once like cf_facts_load does. */
static yyjson_doc *g_root_doc;

static yyjson_val *facts_root(void) {
    if (g_root_doc != NULL) return yyjson_doc_get_root(g_root_doc);
    const char *dir = getenv("CF_GOLDEN_DIR");
    if (dir == NULL || dir[0] == '\0') {
        dir = "tests/fixtures/crates/views/tests/golden";
    }
    char path[1024];
    int n = snprintf(path, sizeof path, "%s/a/facts.json", dir);
    if (n < 0 || (size_t)n >= sizeof path) return NULL;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }
    rewind(file);
    char *text = malloc((size_t)size + 1);
    if (text == NULL) {
        fclose(file);
        return NULL;
    }
    if (size != 0 && fread(text, 1, (size_t)size, file) != (size_t)size) {
        fclose(file);
        free(text);
        return NULL;
    }
    fclose(file);
    text[size] = '\0';
    yyjson_doc *doc = yyjson_read(text, (size_t)size, 0);
    free(text);
    if (doc == NULL) return NULL;
    g_root_doc = doc;
    return yyjson_doc_get_root(doc);
}

/* `User#title` over facts strings: blank bio contributes nothing. */
static cf_str facts_title(const char *name, const char *bio) {
    int blank = 1;
    if (bio != NULL) {
        for (const char *p = bio; *p != '\0'; p++) {
            if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
                blank = 0;
                break;
            }
        }
    }
    if (bio == NULL || bio[0] == '\0' || blank) return own(name);
    size_t name_len = strlen(name);
    size_t bio_len = strlen(bio);
    size_t len = name_len + 5 + bio_len;
    char *joined = malloc(len + 1);
    cf_str out = {0};
    if (joined == NULL) return out;
    memcpy(joined, name, name_len);
    memcpy(joined + name_len, " \xe2\x80\x93 ", 5);
    memcpy(joined + name_len + 5, bio, bio_len + 1);
    out.ptr = joined;
    out.len = len;
    return out;
}

/* ASCII-lower ordering shared with the presenter (sort_by_lower_name). */
static int lower_less(const cf_str *a, const cf_str *b) {
    size_t count = a->len < b->len ? a->len : b->len;
    for (size_t i = 0; i < count; i++) {
        unsigned char ca = (unsigned char)a->ptr[i];
        unsigned char cb = (unsigned char)b->ptr[i];
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return ca < cb;
    }
    return a->len < b->len;
}

/* --- golden model builders -------------------------------------------------- */

static cf_err push_golden_room(cf_view_accounts_bot *bot, int64_t id,
                               const char *name) {
    if (bot->rooms.len == bot->rooms.cap) {
        size_t cap = bot->rooms.cap == 0 ? 4 : bot->rooms.cap * 2;
        cf_view_accounts_bot_room *grown =
            realloc(bot->rooms.items, cap * sizeof *grown);
        CF_REQUIRE(grown != NULL);
        bot->rooms.items = grown;
        bot->rooms.cap = cap;
    }
    cf_view_accounts_bot_room *slot = &bot->rooms.items[bot->rooms.len];
    memset(slot, 0, sizeof *slot);
    slot->id = id;
    slot->name = own(name);
    CF_REQUIRE(slot->name.ptr != NULL);
    bot->rooms.len++;
    return CF_OK;
}

/* BotsIndex from facts, exactly as parity_a.rs `bots(name)` derives it. */
static cf_err build_index_model(const char *case_name,
                                cf_view_accounts_bots_index_model *out) {
    memset(out, 0, sizeof *out);
    CF_REQUIRE(cf_facts_load());
    yyjson_val *case_obj = cf_facts_case(case_name);
    CF_REQUIRE(case_obj != NULL);
    yyjson_val *root = facts_root();
    CF_REQUIRE(root != NULL);
    yyjson_val *users = yyjson_obj_get(case_obj, "users");
    CF_REQUIRE(yyjson_is_obj(users));
    yyjson_val *key, *user;
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(users, &iter);
    while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
        (void)key;
        user = yyjson_obj_iter_get_val(key);
        if (strcmp(cf_facts_str(user, "role"), "bot") != 0) continue;
        if (strcmp(cf_facts_str(user, "status"), "active") != 0) continue;
        if (out->bots.len == out->bots.cap) {
            size_t cap = out->bots.cap == 0 ? 4 : out->bots.cap * 2;
            cf_view_accounts_bot *grown =
                realloc(out->bots.items, cap * sizeof *grown);
            CF_REQUIRE(grown != NULL);
            out->bots.items = grown;
            out->bots.cap = cap;
        }
        cf_view_accounts_bot *slot = &out->bots.items[out->bots.len];
        memset(slot, 0, sizeof *slot);
        slot->id = cf_facts_i64(user, "id", 0);
        slot->name = own(cf_facts_str(user, "name"));
        CF_REQUIRE(slot->name.ptr != NULL);
        slot->title = facts_title(cf_facts_str(user, "name"),
                                  cf_facts_str(user, "bio"));
        CF_REQUIRE(slot->title.ptr != NULL);
        slot->avatar_url = own(cf_facts_str(user, "avatar_path"));
        CF_REQUIRE(slot->avatar_url.ptr != NULL);
        slot->bot_key = own(cf_facts_str(user, "bot_key"));
        CF_REQUIRE(slot->bot_key.ptr != NULL);
        out->bots.len++;
        /* without-directs rooms for this bot, LOWER(name) order. */
        yyjson_val *memberships = yyjson_obj_get(root, "memberships");
        yyjson_val *rooms = yyjson_obj_get(root, "rooms");
        size_t count = yyjson_arr_size(memberships);
        for (size_t i = 0; i < count; i++) {
            yyjson_val *member = yyjson_arr_get(memberships, i);
            if (yyjson_get_sint(yyjson_obj_get(member, "user_id")) !=
                slot->id) {
                continue;
            }
            int64_t room_id =
                yyjson_get_sint(yyjson_obj_get(member, "room_id"));
            yyjson_val *rkey, *room;
            yyjson_obj_iter riter;
            yyjson_obj_iter_init(rooms, &riter);
            while ((rkey = yyjson_obj_iter_next(&riter)) != NULL) {
                (void)rkey;
                room = yyjson_obj_iter_get_val(rkey);
                if (yyjson_get_sint(yyjson_obj_get(room, "id")) != room_id) {
                    continue;
                }
                if (strcmp(cf_facts_str(room, "type"), "Rooms::Direct") == 0) {
                    break;
                }
                CF_REQUIRE(push_golden_room(slot, room_id,
                                            cf_facts_str(room, "name")) ==
                           CF_OK);
                break;
            }
        }
        /* Stable insertion sort by ASCII-lower name. */
        for (size_t i = 1; i < slot->rooms.len; i++) {
            cf_view_accounts_bot_room current = slot->rooms.items[i];
            size_t j = i;
            while (j > 0 &&
                   lower_less(&current.name,
                              &slot->rooms.items[j - 1].name)) {
                slot->rooms.items[j] = slot->rooms.items[j - 1];
                j--;
            }
            slot->rooms.items[j] = current;
        }
    }
    /* The runner filters in facts order (Bender Bot only here); keep it. */
    return CF_OK;
}

/* BotsNew: the default (all-absent) form. */
static void build_new_model(cf_view_accounts_bots_new_model *out) {
    memset(out, 0, sizeof *out);
}

/* BotsEdit from facts: Bender Bot's name/webhook_url, avatar for the
 * with-avatar variant. */
static cf_err build_edit_model(const char *case_name, int with_avatar,
                               cf_view_accounts_bots_edit_model *out) {
    memset(out, 0, sizeof *out);
    CF_REQUIRE(cf_facts_load());
    yyjson_val *case_obj = cf_facts_case(case_name);
    CF_REQUIRE(case_obj != NULL);
    yyjson_val *bender = cf_facts_user(case_obj, "Bender Bot");
    CF_REQUIRE(bender != NULL);
    out->bot_id = cf_facts_i64(bender, "id", 0);
    out->form.has_name = true;
    out->form.name = own(cf_facts_str(bender, "name"));
    CF_REQUIRE(out->form.name.ptr != NULL);
    const char *webhook = cf_facts_str(bender, "webhook_url");
    if (webhook[0] != '\0') {
        out->form.has_webhook_url = true;
        out->form.webhook_url = own(webhook);
        CF_REQUIRE(out->form.webhook_url.ptr != NULL);
    }
    if (with_avatar) {
        const char *avatar = cf_facts_str(case_obj, "avatar_url");
        CF_REQUIRE(avatar[0] != '\0');
        out->form.has_avatar_url = true;
        out->form.avatar_url = own(avatar);
        CF_REQUIRE(out->form.avatar_url.ptr != NULL);
    }
    return CF_OK;
}

/* --- VIEW-03 goldens -------------------------------------------------------- */

CF_TEST(accounts_bots_index_matches_golden) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "bots_index", NULL, NULL));
    cf_view_accounts_bots_index_model model;
    CF_REQUIRE(build_index_model("bots_index", &model) == CF_OK);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_accounts_bots_index(&ctx, &model, &out) == CF_OK);
    cf_golden_expect("bots_index", (const char *)out.ptr, out.len);
    cf_builder_dispose(&out);
    cf_view_accounts_bots_index_model_dispose(&model);
}

CF_TEST(accounts_bots_new_matches_golden) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "bots_new", NULL, NULL));
    cf_view_accounts_bots_new_model model;
    build_new_model(&model);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_accounts_bots_new(&ctx, &model, &out) == CF_OK);
    cf_golden_expect("bots_new", (const char *)out.ptr, out.len);
    cf_builder_dispose(&out);
    cf_view_accounts_bots_new_model_dispose(&model);
}

CF_TEST(accounts_bots_edit_matches_golden) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "bots_edit", NULL, NULL));
    cf_view_accounts_bots_edit_model model;
    CF_REQUIRE(build_edit_model("bots_edit", 0, &model) == CF_OK);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_accounts_bots_edit(&ctx, &model, &out) == CF_OK);
    cf_golden_expect("bots_edit", (const char *)out.ptr, out.len);
    cf_builder_dispose(&out);
    cf_view_accounts_bots_edit_model_dispose(&model);
}

CF_TEST(accounts_bots_edit_with_avatar_matches_golden) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "bots_edit_with_avatar", NULL, NULL));
    cf_view_accounts_bots_edit_model model;
    CF_REQUIRE(build_edit_model("bots_edit_with_avatar", 1, &model) == CF_OK);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_accounts_bots_edit(&ctx, &model, &out) == CF_OK);
    cf_golden_expect("bots_edit_with_avatar", (const char *)out.ptr, out.len);
    cf_builder_dispose(&out);
    cf_view_accounts_bots_edit_model_dispose(&model);
}

/* --- frames carry head + content only --------------------------------------- */

CF_TEST(accounts_bots_frames_have_content_without_page_chrome) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "bots_index", NULL, NULL));
    cf_view_accounts_bots_index_model index;
    CF_REQUIRE(build_index_model("bots_index", &index) == CF_OK);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_accounts_bots_index_frame(&ctx, &index, &out) == CF_OK);
    CF_CHECK(contains(&out, "Chat bots"));
    CF_CHECK(contains(&out, "Bender Bot"));
    CF_CHECK(!contains(&out, "<!DOCTYPE html>"));
    CF_CHECK(!contains(&out, "<nav id=\"nav\""));
    cf_builder_dispose(&out);
    cf_view_accounts_bots_index_model_dispose(&index);

    CF_REQUIRE(cf_facts_view_ctx(&ctx, "bots_new", NULL, NULL));
    cf_view_accounts_bots_new_model fresh;
    build_new_model(&fresh);
    CF_REQUIRE(cf_view_accounts_bots_new_frame(&ctx, &fresh, &out) == CF_OK);
    CF_CHECK(contains(&out, "user[name]"));
    CF_CHECK(!contains(&out, "<!DOCTYPE html>"));
    cf_builder_dispose(&out);
    cf_view_accounts_bots_new_model_dispose(&fresh);

    CF_REQUIRE(cf_facts_view_ctx(&ctx, "bots_edit", NULL, NULL));
    cf_view_accounts_bots_edit_model edit;
    CF_REQUIRE(build_edit_model("bots_edit", 0, &edit) == CF_OK);
    CF_REQUIRE(cf_view_accounts_bots_edit_frame(&ctx, &edit, &out) == CF_OK);
    CF_CHECK(contains(&out, "value=\"Bender Bot\""));
    CF_CHECK(contains(&out, "/account/bots/394959859/key"));
    CF_CHECK(!contains(&out, "<!DOCTYPE html>"));
    cf_builder_dispose(&out);
    cf_view_accounts_bots_edit_model_dispose(&edit);
}

/* --- R-BOTS-VIEWS shim facts ------------------------------------------------- */
/*
 * The acceptance facts src/actions/accounts/bots.c asserts through its
 * static shims until the integrator rebinds it: the page titles, the bot
 * name/key rows and the nested `user` form fields (VIEW-04).
 */
CF_TEST(accounts_bots_renders_the_shim_facts) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "bots_index", NULL, NULL));
    cf_view_accounts_bots_index_model index;
    CF_REQUIRE(build_index_model("bots_index", &index) == CF_OK);
    cf_builder out = {0};
    CF_REQUIRE(cf_view_accounts_bots_index(&ctx, &index, &out) == CF_OK);
    CF_CHECK(contains(&out, "Chat bots"));
    CF_CHECK(contains(&out, "Bender Bot"));
    CF_CHECK(contains(&out, "394959859-e0LbMoZhDhOs"));
    CF_CHECK(contains(&out, "All Talk"));
    CF_CHECK(contains(&out, "curl -d"));
    cf_builder_dispose(&out);
    cf_view_accounts_bots_index_model_dispose(&index);

    CF_REQUIRE(cf_facts_view_ctx(&ctx, "bots_new", NULL, NULL));
    cf_view_accounts_bots_new_model fresh;
    build_new_model(&fresh);
    CF_REQUIRE(cf_view_accounts_bots_new(&ctx, &fresh, &out) == CF_OK);
    CF_CHECK(contains(&out, "New chat bot"));
    CF_CHECK(contains(&out, "user[name]"));
    CF_CHECK(contains(&out, "user[webhook_url]"));
    CF_CHECK(contains(&out, "user[avatar]"));
    cf_builder_dispose(&out);
    cf_view_accounts_bots_new_model_dispose(&fresh);

    CF_REQUIRE(cf_facts_view_ctx(&ctx, "bots_edit", NULL, NULL));
    cf_view_accounts_bots_edit_model edit;
    CF_REQUIRE(build_edit_model("bots_edit", 0, &edit) == CF_OK);
    CF_REQUIRE(cf_view_accounts_bots_edit(&ctx, &edit, &out) == CF_OK);
    CF_CHECK(contains(&out, "Edit bot"));
    CF_CHECK(contains(&out, "value=\"Bender Bot\""));
    CF_CHECK(contains(&out, "value=\"https://example.com/webhook?a=1&amp;b=2\""));
    CF_CHECK(contains(&out, "_method\" value=\"patch\""));
    CF_CHECK(contains(&out, "_method\" value=\"delete\""));
    CF_CHECK(contains(&out, "/account/bots/394959859/key"));
    cf_builder_dispose(&out);
    cf_view_accounts_bots_edit_model_dispose(&edit);
}

/* Markup in names renders escaped (the index shim uses cf_html_text). */
CF_TEST(accounts_bots_index_escapes_names) {
    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "bots_index", NULL, NULL));
    cf_view_accounts_bots_index_model model;
    memset(&model, 0, sizeof model);
    model.bots.items = calloc(1, sizeof *model.bots.items);
    CF_REQUIRE(model.bots.items != NULL);
    model.bots.cap = 1;
    model.bots.len = 1;
    cf_view_accounts_bot *bot = &model.bots.items[0];
    bot->id = 99;
    bot->name = own("<b>Bold</b>");
    bot->title = own("<b>Bold</b>");
    bot->avatar_url = own("/users/x/avatar?v=1");
    bot->bot_key = own("99-esctoken1234");
    CF_REQUIRE(bot->name.ptr != NULL);

    cf_builder out = {0};
    CF_REQUIRE(cf_view_accounts_bots_index(&ctx, &model, &out) == CF_OK);
    CF_CHECK(!contains(&out, "<b>Bold</b>"));
    CF_CHECK(contains(&out, "&lt;b&gt;Bold&lt;/b&gt;"));
    cf_builder_dispose(&out);
    cf_view_accounts_bots_index_model_dispose(&model);
}

/* Failed renders are CF_INVALID and leave the builder at its entry length. */
CF_TEST(accounts_bots_reject_null_arguments) {
    cf_builder out = {0};
    CF_REQUIRE(cf_builder_append(&out, (cf_span){(const unsigned char *)"kept",
                                                 4}) == CF_OK);
    cf_view_accounts_bots_index_model index;
    memset(&index, 0, sizeof index);
    CF_CHECK(cf_view_accounts_bots_index(NULL, &index, &out) == CF_INVALID);
    CF_CHECK(cf_view_accounts_bots_index_frame(NULL, &index, &out) ==
             CF_INVALID);
    CF_CHECK(out.len == 4);
    cf_view_accounts_bots_new_model fresh;
    memset(&fresh, 0, sizeof fresh);
    CF_CHECK(cf_view_accounts_bots_new(NULL, &fresh, &out) == CF_INVALID);
    CF_CHECK(out.len == 4);
    cf_view_accounts_bots_edit_model edit;
    memset(&edit, 0, sizeof edit);
    CF_CHECK(cf_view_accounts_bots_edit(NULL, &edit, &out) == CF_INVALID);
    CF_CHECK(cf_view_accounts_bots_edit_frame(NULL, &edit, &out) ==
             CF_INVALID);
    CF_CHECK(out.len == 4);
    CF_CHECK(cf_presenter_bots_index(NULL, &index) == CF_INVALID);
    CF_CHECK(cf_presenter_bot(NULL, NULL, NULL) == CF_INVALID);
    CF_CHECK(cf_presenter_bot_form(NULL, NULL, NULL) == CF_INVALID);
    cf_builder_dispose(&out);
}

/* --- presenter fixtures ------------------------------------------------------ */

static cf_config *make_config(void) {
    cf_config_entry entries[2] = {
        {"PUBLIC_ORIGIN", ORIGIN},
        {"SECRET_KEY_BASE", HEX64},
    };
    cf_config *config = NULL;
    CF_REQUIRE(cf_config_parse(entries, 2, NULL, &config) == CF_OK);
    return config;
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
    cf_ctx ctx;
} bots_fixture;

/* Presenters only read ctx->app (config) and ctx->reader (the database), so
 * the fixture fills those directly: no route match, no dispatch.  This keeps
 * the cases linkable in the views bucket (whose link has neither routes.o
 * nor the route double, and gc-sections drops the unreferenced match code). */
static void fixture_init(bots_fixture *fixture, cf_db *db) {
    memset(fixture, 0, sizeof *fixture);
    cf_config *config = make_config();
    CF_REQUIRE(cf_app_create(config, &fixture->app) == CF_OK);
    fixture->ctx.app = fixture->app;
    fixture->ctx.reader = db;
}

static void fixture_dispose(bots_fixture *fixture) {
    cf_app_destroy(fixture->app);
    memset(fixture, 0, sizeof *fixture);
}

static void seed_bots(cf_db *db) {
    exec_sql(db,
             "INSERT INTO users (id, bio, bot_token, created_at, "
             "email_address, name, password_digest, role, status, "
             "updated_at) VALUES (1, NULL, NULL, '" T1_TEXT "', "
             "'david@37signals.com', 'David', NULL, 1, 0, '" T1_TEXT "')");
    exec_sql(db,
             "INSERT INTO users (id, bio, bot_token, created_at, "
             "email_address, name, password_digest, role, status, "
             "updated_at) VALUES (7, 'Beep boop', 'zulutoken123', '" T1_TEXT
             "', NULL, 'Zulu', NULL, 2, 0, '" T1_TEXT "')");
    exec_sql(db,
             "INSERT INTO users (id, bio, bot_token, created_at, "
             "email_address, name, password_digest, role, status, "
             "updated_at) VALUES (8, NULL, 'alphatoken12', '" T1_TEXT "', "
             "NULL, 'alpha', NULL, 2, 0, '" T1_TEXT "')");
    exec_sql(db,
             "INSERT INTO users (id, bio, bot_token, created_at, "
             "email_address, name, password_digest, role, status, "
             "updated_at) VALUES (9, NULL, 'gonetoken123', '" T1_TEXT "', "
             "NULL, 'Gone', NULL, 2, 1, '" T1_TEXT "')");
    exec_sql(db,
             "INSERT INTO users (id, bio, bot_token, created_at, "
             "email_address, name, password_digest, role, status, "
             "updated_at) VALUES (2, NULL, NULL, '" T1_TEXT "', "
             "'member@example.com', 'Member', NULL, 0, 0, '" T1_TEXT "')");
    exec_sql(db,
             "INSERT INTO rooms (id, created_at, creator_id, name, type, "
             "updated_at) VALUES (10, '" T1_TEXT "', 1, 'All Talk', "
             "'Rooms::Open', '" T1_TEXT "')");
    exec_sql(db,
             "INSERT INTO rooms (id, created_at, creator_id, name, type, "
             "updated_at) VALUES (11, '" T1_TEXT "', 1, 'zeta', "
             "'Rooms::Closed', '" T1_TEXT "')");
    exec_sql(db,
             "INSERT INTO rooms (id, created_at, creator_id, name, type, "
             "updated_at) VALUES (12, '" T1_TEXT "', 1, NULL, "
             "'Rooms::Direct', '" T1_TEXT "')");
    exec_sql(db,
             "INSERT INTO memberships (id, created_at, room_id, updated_at, "
             "user_id) VALUES (20, '" T1_TEXT "', 10, '" T1_TEXT "', 7)");
    exec_sql(db,
             "INSERT INTO memberships (id, created_at, room_id, updated_at, "
             "user_id) VALUES (21, '" T1_TEXT "', 11, '" T1_TEXT "', 7)");
    exec_sql(db,
             "INSERT INTO memberships (id, created_at, room_id, updated_at, "
             "user_id) VALUES (22, '" T1_TEXT "', 12, '" T1_TEXT "', 7)");
    exec_sql(db,
             "INSERT INTO memberships (id, created_at, room_id, updated_at, "
             "user_id) VALUES (23, '" T1_TEXT "', 12, '" T1_TEXT "', 2)");
    exec_sql(db,
             "INSERT INTO webhooks (created_at, updated_at, url, user_id) "
             "VALUES ('" T1_TEXT "', '" T1_TEXT "', "
             "'https://hooks.example/bot', 7)");
    exec_sql(db,
             "INSERT INTO active_storage_blobs (id, byte_size, checksum, "
             "content_type, created_at, filename, key, metadata, "
             "service_name) VALUES (1, 10, NULL, 'image/png', '" T1_TEXT "', "
             "'bender.png', 'key1', NULL, 'local')");
    exec_sql(db,
             "INSERT INTO active_storage_attachments (id, blob_id, "
             "created_at, name, record_id, record_type) VALUES "
             "(1, 1, '" T1_TEXT "', 'avatar', 7, 'User')");
}

static int str_contains(const cf_str *value, const char *needle) {
    size_t len = strlen(needle);
    if (len == 0) return 1;
    if (value == NULL || value->len < len) return 0;
    for (size_t i = 0; i + len <= value->len; i++) {
        if (memcmp(value->ptr + i, needle, len) == 0) return 1;
    }
    return 0;
}
static int starts_with(const cf_str *value, const char *prefix) {
    size_t len = strlen(prefix);
    return value->len >= len && memcmp(value->ptr, prefix, len) == 0;
}

static int ends_with(const cf_str *value, const char *suffix) {
    size_t len = strlen(suffix);
    return value->len >= len &&
           memcmp(value->ptr + value->len - len, suffix, len) == 0;
}

CF_TEST(presenter_bots_index_maps_active_bots_ordered) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_bots(scratch.db);
    bots_fixture fixture;
    fixture_init(&fixture, scratch.db);

    cf_view_accounts_bots_index_model model;
    CF_REQUIRE(cf_presenter_bots_index(&fixture.ctx, &model) == CF_OK);
    /* ORDER BY LOWER(name): alpha before Zulu; Gone (deactivated) and
     * Member (not a bot) are absent. */
    CF_REQUIRE(model.bots.len == 2);
    CF_CHECK(strcmp(model.bots.items[0].name.ptr, "alpha") == 0);
    CF_CHECK(strcmp(model.bots.items[1].name.ptr, "Zulu") == 0);
    CF_CHECK(strcmp(model.bots.items[0].bot_key.ptr, "8-alphatoken12") == 0);
    CF_CHECK(strcmp(model.bots.items[1].bot_key.ptr, "7-zulutoken123") == 0);
    /* User#title joins name and bio; the fresh avatar path is signed. */
    CF_CHECK(strcmp(model.bots.items[1].title.ptr, "Zulu \xe2\x80\x93 Beep boop") ==
             0);
    CF_CHECK(starts_with(&model.bots.items[1].avatar_url, "/users/"));
    CF_CHECK(str_contains(&model.bots.items[1].avatar_url,
                          "/avatar?v=20260926130020"));
    /* Without directs, LOWER(name) order: All Talk before zeta. */
    CF_REQUIRE(model.bots.items[1].rooms.len == 2);
    CF_CHECK(strcmp(model.bots.items[1].rooms.items[0].name.ptr, "All Talk") ==
             0);
    CF_CHECK(model.bots.items[1].rooms.items[0].id == 10);
    CF_CHECK(strcmp(model.bots.items[1].rooms.items[1].name.ptr, "zeta") == 0);
    CF_CHECK(model.bots.items[1].rooms.items[1].id == 11);
    CF_REQUIRE(model.bots.items[0].rooms.len == 0);

    cf_view_accounts_bots_index_model_dispose(&model);
    cf_view_accounts_bots_index_model_dispose(&model); /* idempotent */
    fixture_dispose(&fixture);
    cf_db_scratch_close(&scratch);
}

CF_TEST(presenter_bot_maps_a_single_row) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_bots(scratch.db);
    bots_fixture fixture;
    fixture_init(&fixture, scratch.db);

    cf_user row = {0};
    CF_REQUIRE(cf_user_find_active_bot(scratch.db, 8, &row) == CF_OK);
    cf_view_accounts_bot bot;
    CF_REQUIRE(cf_presenter_bot(&fixture.ctx, &row, &bot) == CF_OK);
    CF_CHECK(bot.id == 8);
    CF_CHECK(strcmp(bot.name.ptr, "alpha") == 0);
    CF_CHECK(strcmp(bot.title.ptr, "alpha") == 0);
    CF_CHECK(strcmp(bot.bot_key.ptr, "8-alphatoken12") == 0);
    CF_CHECK(bot.rooms.len == 0);
    cf_view_accounts_bot_dispose(&bot);
    cf_user_dispose(&row);
    fixture_dispose(&fixture);
    cf_db_scratch_close(&scratch);
}

CF_TEST(presenter_bot_form_maps_webhook_and_avatar) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_bots(scratch.db);
    bots_fixture fixture;
    fixture_init(&fixture, scratch.db);

    cf_user row = {0};
    CF_REQUIRE(cf_user_find_active_bot(scratch.db, 7, &row) == CF_OK);
    cf_view_accounts_bot_form form;
    CF_REQUIRE(cf_presenter_bot_form(&fixture.ctx, &row, &form) == CF_OK);
    CF_CHECK(form.has_name);
    CF_CHECK(strcmp(form.name.ptr, "Zulu") == 0);
    CF_CHECK(form.has_webhook_url);
    CF_CHECK(strcmp(form.webhook_url.ptr, "https://hooks.example/bot") == 0);
    CF_CHECK(form.has_avatar_url);
    CF_CHECK(starts_with(&form.avatar_url, ORIGIN "/rails/active_storage/blobs/redirect/"));
    CF_CHECK(ends_with(&form.avatar_url, "/bender.png"));
    cf_view_accounts_bot_form_dispose(&form);
    cf_user_dispose(&row);

    /* No webhook row and no attachment: both fields absent. */
    CF_REQUIRE(cf_user_find_active_bot(scratch.db, 8, &row) == CF_OK);
    memset(&form, 0, sizeof form);
    CF_REQUIRE(cf_presenter_bot_form(&fixture.ctx, &row, &form) == CF_OK);
    CF_CHECK(form.has_name);
    CF_CHECK(!form.has_webhook_url);
    CF_CHECK(!form.has_avatar_url);
    cf_view_accounts_bot_form_dispose(&form);
    cf_user_dispose(&row);
    fixture_dispose(&fixture);
    cf_db_scratch_close(&scratch);
}

/* Key rotation display: the presenter shows the row's current key only. */
CF_TEST(presenter_bot_key_tracks_rotation) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_bots(scratch.db);
    bots_fixture fixture;
    fixture_init(&fixture, scratch.db);

    cf_user row = {0};
    CF_REQUIRE(cf_user_find_active_bot(scratch.db, 7, &row) == CF_OK);
    cf_view_accounts_bot before;
    CF_REQUIRE(cf_presenter_bot(&fixture.ctx, &row, &before) == CF_OK);
    CF_CHECK(strcmp(before.bot_key.ptr, "7-zulutoken123") == 0);
    cf_view_accounts_bot_dispose(&before);
    cf_user_dispose(&row);

    exec_sql(scratch.db,
             "UPDATE users SET bot_token = 'freshrotkey1' WHERE id = 7");
    CF_REQUIRE(cf_user_find_active_bot(scratch.db, 7, &row) == CF_OK);
    cf_view_accounts_bot after;
    CF_REQUIRE(cf_presenter_bot(&fixture.ctx, &row, &after) == CF_OK);
    CF_CHECK(strcmp(after.bot_key.ptr, "7-freshrotkey1") == 0);
    cf_view_accounts_bot_dispose(&after);
    cf_user_dispose(&row);
    fixture_dispose(&fixture);
    cf_db_scratch_close(&scratch);
}

/* Rendering performs no SQL: the presenter-built models render after the
 * database is closed. */
CF_TEST(accounts_bots_render_without_a_database) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    seed_bots(scratch.db);
    /* Alpha owns no rooms in seed_bots, and the reference `_bot` partial
     * renders the "id-token" key only inside room curl blocks: give alpha a
     * room so both keys appear (a room-less bot shows its name alone). */
    exec_sql(scratch.db,
             "INSERT INTO memberships (id, created_at, room_id, updated_at, "
             "user_id) VALUES (24, '" T1_TEXT "', 10, '" T1_TEXT "', 8)");
    bots_fixture fixture;
    fixture_init(&fixture, scratch.db);

    cf_view_accounts_bots_index_model index;
    CF_REQUIRE(cf_presenter_bots_index(&fixture.ctx, &index) == CF_OK);
    cf_user row = {0};
    CF_REQUIRE(cf_user_find_active_bot(scratch.db, 7, &row) == CF_OK);
    cf_view_accounts_bot_form form;
    CF_REQUIRE(cf_presenter_bot_form(&fixture.ctx, &row, &form) == CF_OK);
    cf_user_dispose(&row);
    fixture_dispose(&fixture);
    cf_db_scratch_close(&scratch);

    CF_REQUIRE(cf_test_views_setup());
    cf_view_ctx ctx = {0};
    CF_REQUIRE(cf_facts_view_ctx(&ctx, "bots_index", NULL, NULL));
    cf_builder out = {0};
    CF_REQUIRE(cf_view_accounts_bots_index(&ctx, &index, &out) == CF_OK);
    CF_CHECK(contains(&out, "7-zulutoken123"));
    CF_CHECK(contains(&out, "8-alphatoken12"));
    cf_builder_dispose(&out);
    cf_view_accounts_bots_index_model_dispose(&index);

    cf_view_accounts_bots_edit_model edit;
    memset(&edit, 0, sizeof edit);
    edit.bot_id = 7;
    edit.form = form;
    CF_REQUIRE(cf_view_accounts_bots_edit(&ctx, &edit, &out) == CF_OK);
    CF_CHECK(contains(&out, "value=\"Zulu\""));
    CF_CHECK(contains(&out, "https://hooks.example/bot"));
    cf_builder_dispose(&out);
    cf_view_accounts_bots_edit_model_dispose(&edit);
}
