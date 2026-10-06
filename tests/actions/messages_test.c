/* tests/actions/messages_test.c — A-messages acceptance: `messages#index`
 * (route IDs 76/137), `#create` (77/138), `#edit` (79/140), `#show` (80/141),
 * `#update` (81/82/142/143) and `#destroy` (83/144), per
 * docs/devel/implementation/contracts/controller-packets.md "A-messages" and
 * 03-application.md's slice rows:
 *
 *   - index: empty page 204; page-of-40 before/after precedence from the
 *     model; bare HTML (no layout); conditional GET (weak ETag,
 *     Last-Modified, 304) with the flash part joined in map insertion order;
 *     404 for an unknown/unreachable room and for a
 *     present-but-uncastable before/after; 406 for an unacceptable format.
 *   - create: nested permit list; the message and its rich-text/FTS rows plus
 *     the room touch; the 200 Turbo Stream response; the broadcast of the
 *     stored message with the response's exact bytes; the canonical body
 *     (ActionText::Content.new(body, canonicalize: true).to_html()) stored
 *     with the reference's as-given rescue; a missing `message`
 *     param 400; an HTML client 406 *after* the message committed; a room
 *     the user left renders messages/room_not_found at 200; a non-upload
 *     attachment is the reference's 500; webhook delivery to an active bot.
 *   - show/edit: the rendered pages compared against golden/b
 *     (messages_show_text, messages_edit_text, messages_room_not_found)
 *     through the real DB presenters; 403 for a non-creator non-admin;
 *     404 for an unknown message.
 *   - update: 302 to the room message URL; the body and FTS update (in its
 *     canonical body form); the replace broadcast's exact bytes; JSON is the
 *     missing template (500); 403/404.
 *   - destroy: the golden messages_destroy stream; the row and its dependent
 *     rows are gone; the remove broadcast's exact bytes; an HTML client 406
 *     after the destroy.
 *   - D02 writer revalidation: a membership removed before the write makes
 *     the create callback fail with NotFound and write nothing.
 *
 * The route double (tests/app/support/route_double.c) binds the fourteen
 * route-ID rows to the real actions, so every case runs the real A00
 * dispatch path (params, cookies, before-actions, format negotiation).
 *
 * The action's broadcast accessor is app.h's cf_app_cable, set here with
 * cf_app_set_cable the way main.c sets it, so the packet's broadcast effects
 * are exercised end to end.  `msg_create_write_cb`/struct msg_create_write
 * are declared here to exercise the D02 in-transaction revalidation directly.
 *
 * The golden facts are the views-b fixture set (SECRET_KEY_BASE
 * "views-b-secret-key-base-..." from reference-tools/views/b/run.sh, pinned
 * in tmp/rust-ref): account 37signals/873240054 at 13:03:57, David
 * 127326141 (administrator) and Kevin 712064548 at 13:03:57/13:03:59, room
 * Designers 654632876, message 933434491 "rich".
 */
#include "cf_test.h"

#include "app.h"
#include "auth.h"
#include "cable/channels.h"
#include "config.h"
#include "context.h"
#include "core/testclock.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "db/writer.h"
#include "http/http_internal.h"
#include "models/active_storage.h"
#include "models/message.h"
#include "models/room.h"
#include "models/user.h"
#include "storage/active_storage.h"
#include "richtext.h"
#include "views.h"

#include "../app/support/route_double.h"
#include "../app/support/test_request.h"
#include "../cable/cable_testutil.h"
#include "../views/support/facts.h"
#include "../views/support/golden.h"
#include "../views/support/golden_b.h"

#include <dirent.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* cf.h is frozen and no packet-action header exists yet (the integrator's
 * routes.c rebind needs the same declarations). */
cf_err cf_action_messages_index(cf_ctx *ctx);
cf_err cf_action_messages_create(cf_ctx *ctx);
cf_err cf_action_messages_edit(cf_ctx *ctx);
cf_err cf_action_messages_show(cf_ctx *ctx);
cf_err cf_action_messages_update(cf_ctx *ctx);
cf_err cf_action_messages_destroy(cf_ctx *ctx);

/* The create writer callback, for the D02 revalidation case.  The struct
 * mirrors src/actions/messages.c exactly (staged blob arm included); a
 * layout change there breaks this declaration loudly. */
struct msg_create_write {
    int64_t room_id;
    int64_t creator_id;
    cf_new_message attributes;
    cf_message message;
    cf_blob blob; /* owned when staged != NULL */
    const cf_active_staged *staged; /* borrowed; NULL when no upload */
};
cf_err msg_create_write_cb(cf_tx *tx, void *arg);

#define ORIGIN "http://campfire.test"
#define NEWS_SECRET \
    "views-b-secret-key-base-0123456789abcdef0123456789abcdef"
#define NEWS_VAPID                                                         \
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8" \
    "cTriz_qYBVicY02_VxTQ="
#define GOLDEN_ACCOUNT_ID INT64_C(873240054)
#define GOLDEN_DAVID INT64_C(127326141)
#define GOLDEN_KEVIN INT64_C(712064548)
#define GOLDEN_JZ INT64_C(773523953)
#define GOLDEN_ROOM INT64_C(654632876)
#define GOLDEN_MESSAGE INT64_C(933434491)
#define GOLDEN_DESTROY_MESSAGE INT64_C(933434495)
#define GOLDEN_OTHER_ROOM INT64_C(999001)

/* The show/edit fixture's stored body (messages_edit_text's
 * editable_body_html). */
#define GOLDEN_RICH_BODY                                                   \
    "<div>Visit https://example.com/?a=1&amp;b=2 &amp; <strong>bold</"    \
    "strong> \"quoted\" it's &lt;script&gt;</div><ul><li>one</li></ul>"

static cf_span SP(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

static const char *TURBO_ACCEPT =
    "text/vnd.turbo-stream.html, text/html, application/xhtml+xml";

/* --- environment ------------------------------------------------------------ */

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    cf_cable *cable;
    bool writer_started;
    cf_event events[64];
    size_t event_count;
    int session_seq;
} msg_env;

static cf_err env_capture(void *ctx, const cf_event *event) {
    msg_env *env = ctx;
    if (env->event_count < 64) env->events[env->event_count++] = *event;
    return CF_OK;
}

/* The golden/b fixtures were cut with a non-hex SECRET_KEY_BASE
 * ("views-b-secret-key-base-...", reference-tools/views/b/run.sh), and the
 * pages embed signed avatar URLs; cf_config_parse only accepts hex, which is
 * a configuration-input rule, not an application rule.  The test builds the
 * immutable config directly so the pages' signed URLs match the fixtures. */
static char *dup_str(const char *text) {
    size_t len = strlen(text);
    char *copy = malloc(len + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, text, len + 1);
    return copy;
}

static const char *g_storage_override;

static cf_config *make_views_b_config(const char *database_path) {
    cf_config *config = calloc(1, sizeof *config);
    if (config == NULL) return NULL;
    config->host = dup_str("0.0.0.0");
    config->public_origin = dup_str(ORIGIN);
    config->database_path = dup_str(database_path);
    /* Attachment tests point the disk service at their own scratch root. */
    config->storage_path = dup_str(g_storage_override != NULL
                                       ? g_storage_override
                                       : "storage/files");
    config->secret_key_base = dup_str(NEWS_SECRET);
    config->secret_key_base_len = strlen(NEWS_SECRET);
    config->loops = 1;
    config->readers = 2;
    config->request_slots = 8;
    config->writer_queue = 8;
    config->connections_per_loop = 16;
    config->input_bytes = 1u << 20;
    config->output_bytes = 1u << 20;
    config->cache_bytes = 0;
    config->job_queue = 8;
    config->job_workers = 1;
    config->crypto_workers = 1;
    config->disable_ssl = true;
    config->bcrypt_cost = 4;
    config->vapid_public_key = dup_str(NEWS_VAPID);
    config->push_configured = true;
    if (config->host == NULL || config->public_origin == NULL ||
        config->database_path == NULL || config->storage_path == NULL ||
        config->secret_key_base == NULL || config->vapid_public_key == NULL) {
        cf_config_destroy(config);
        return NULL;
    }
    return config;
}

static bool env_open(msg_env *env) {
    memset(env, 0, sizeof *env);
    if (!cf_db_scratch_open(&env->scratch)) return false;
    env->config = make_views_b_config(env->scratch.path);
    if (env->config == NULL) return false;
    if (cf_app_create(env->config, &env->app) != CF_OK) {
        cf_config_destroy(env->config);
        env->config = NULL;
        return false;
    }
    env->config = NULL; /* owned by the app */
    if (cf_writer_start(env->app, cf_app_config(env->app)) != CF_OK) {
        return false;
    }
    env->writer_started = true;
    if (cf_writer_set_control_handler(env->app, env_capture, env) != CF_OK) {
        return false;
    }
    if (cf_writer_set_event_handler(env->app, CF_EVENT_DELIVER_WEBHOOK,
                                    env_capture, env) != CF_OK) {
        return false;
    }
    if (cf_writer_set_event_handler(env->app, CF_EVENT_PURGE_BLOB, env_capture,
                                    env) != CF_OK) {
        return false;
    }
    cf_richtext_configure(SP(NEWS_SECRET));
    if (!cf_test_views_setup()) return false;

    cf_cable_config cable_config;
    memset(&cable_config, 0, sizeof cable_config);
    cable_config.app = env->app;
    cable_config.database_path = env->scratch.path;
    if (cf_cable_create(&cable_config, &env->cable) != CF_OK) return false;
    cf_app_set_cable(env->app, env->cable);

    cf_test_routes_reset();
    /* The double's `(.:format)` grammar needs an extension for a literal last
     * segment (the same note as tests/actions/sessions_test.c), so the bare
     * forms are registered as well. */
    if (cf_test_routes_add("GET", "/rooms/:room_id/messages/:id/edit(.:format)",
                           79, cf_action_messages_edit) != CF_OK ||
        cf_test_routes_add("GET", "/rooms/:room_id/messages/:id/edit", 79,
                           cf_action_messages_edit) != CF_OK ||
        cf_test_routes_add("GET", "/rooms/:room_id/messages/:id(.:format)", 80,
                           cf_action_messages_show) != CF_OK ||
        cf_test_routes_add("PATCH", "/rooms/:room_id/messages/:id(.:format)",
                           81, cf_action_messages_update) != CF_OK ||
        cf_test_routes_add("PUT", "/rooms/:room_id/messages/:id(.:format)", 82,
                           cf_action_messages_update) != CF_OK ||
        cf_test_routes_add("DELETE", "/rooms/:room_id/messages/:id(.:format)",
                           83, cf_action_messages_destroy) != CF_OK ||
        cf_test_routes_add("GET", "/rooms/:room_id/messages(.:format)", 76,
                           cf_action_messages_index) != CF_OK ||
        cf_test_routes_add("GET", "/rooms/:room_id/messages", 76,
                           cf_action_messages_index) != CF_OK ||
        cf_test_routes_add("POST", "/rooms/:room_id/messages(.:format)", 77,
                           cf_action_messages_create) != CF_OK ||
        cf_test_routes_add("POST", "/rooms/:room_id/messages", 77,
                           cf_action_messages_create) != CF_OK ||
        cf_test_routes_add("GET", "/messages/:id/edit(.:format)", 140,
                           cf_action_messages_edit) != CF_OK ||
        cf_test_routes_add("GET", "/messages/:id/edit", 140,
                           cf_action_messages_edit) != CF_OK ||
        cf_test_routes_add("GET", "/messages/:id(.:format)", 141,
                           cf_action_messages_show) != CF_OK ||
        cf_test_routes_add("PATCH", "/messages/:id(.:format)", 142,
                           cf_action_messages_update) != CF_OK ||
        cf_test_routes_add("PUT", "/messages/:id(.:format)", 143,
                           cf_action_messages_update) != CF_OK ||
        cf_test_routes_add("DELETE", "/messages/:id(.:format)", 144,
                           cf_action_messages_destroy) != CF_OK ||
        cf_test_routes_add("GET", "/messages(.:format)", 137,
                           cf_action_messages_index) != CF_OK ||
        cf_test_routes_add("GET", "/messages", 137,
                           cf_action_messages_index) != CF_OK ||
        cf_test_routes_add("POST", "/messages(.:format)", 138,
                           cf_action_messages_create) != CF_OK ||
        cf_test_routes_add("POST", "/messages", 138,
                           cf_action_messages_create) != CF_OK) {
        return false;
    }
    return true;
}

static void env_close(msg_env *env) {
    if (env->app != NULL) cf_app_set_cable(env->app, NULL);
    if (env->cable != NULL) cf_cable_destroy(env->cable);
    env->cable = NULL;
    if (env->writer_started) cf_writer_stop(env->app);
    if (env->app != NULL) cf_app_destroy(env->app);
    env->app = NULL;
    cf_db_scratch_close(&env->scratch);
    memset(env, 0, sizeof *env);
}

/* --- raw SQL helpers -------------------------------------------------------- */

static void exec_sql(cf_db *db, const char *sql) {
    char *message = NULL;
    int rc = sqlite3_exec(cf_db_handle(db), sql, NULL, NULL, &message);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  sql failed: %s\n  %s\n",
                message != NULL ? message : "?", sql);
    }
    CF_REQUIRE(rc == SQLITE_OK);
}

static int64_t count_rows(cf_db *db, const char *sql) {
    bool ok = false;
    int64_t value = cf_db_test_i64(cf_db_handle(db), sql, &ok);
    CF_REQUIRE(ok);
    return value;
}

/* The first column of the first row as owned text ("" when no row). */
static bool one_text(cf_db *db, const char *sql, char *out, size_t cap) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL) !=
        SQLITE_OK) {
        return false;
    }
    bool found = sqlite3_step(stmt) == SQLITE_ROW;
    if (found) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        snprintf(out, cap, "%s", text != NULL ? (const char *)text : "");
    } else {
        out[0] = '\0';
    }
    sqlite3_finalize(stmt);
    return found;
}

/* --- seeding ---------------------------------------------------------------- */

static void bind_text(sqlite3_stmt *stmt, int index, const char *text) {
    if (text == NULL) {
        sqlite3_bind_null(stmt, index);
    } else {
        sqlite3_bind_text(stmt, index, text, -1, SQLITE_TRANSIENT);
    }
}

static void exec_stmt(cf_db *db, const char *sql, sqlite3_stmt *stmt) {
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "  stmt failed (%d): %s\n  %s\n", rc,
                sqlite3_errmsg(cf_db_handle(db)), sql);
    }
    sqlite3_finalize(stmt);
    CF_REQUIRE(rc == SQLITE_DONE);
}

static void seed_account(cf_db *db, int64_t id, const char *name,
                         const char *updated_at) {
    static const char sql[] =
        "INSERT INTO accounts (id, created_at, custom_styles, join_code, "
        "name, settings, singleton_guard, updated_at) VALUES "
        "(?, '2026-09-26 12:00:00.000000', NULL, 'CRMu-l8Ge-KB9B', ?, NULL, "
        "0, ?)";
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL) ==
               SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, id);
    bind_text(stmt, 2, name);
    bind_text(stmt, 3, updated_at);
    exec_stmt(db, sql, stmt);
}

static void seed_user(cf_db *db, int64_t id, const char *name,
                      const char *bio, int role, int status,
                      const char *updated_at) {
    static const char sql[] =
        "INSERT INTO users (id, bio, bot_token, created_at, email_address, "
        "name, password_digest, role, status, updated_at) VALUES (?, ?, NULL, "
        "'2026-09-26 12:00:00.000000', NULL, ?, NULL, ?, ?, ?)";
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL) ==
               SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, id);
    bind_text(stmt, 2, bio);
    bind_text(stmt, 3, name);
    sqlite3_bind_int(stmt, 4, role);
    sqlite3_bind_int(stmt, 5, status);
    bind_text(stmt, 6, updated_at);
    exec_stmt(db, sql, stmt);
}

static void seed_room(cf_db *db, int64_t id, const char *name,
                      const char *type, int64_t creator, const char *created,
                      const char *updated) {
    static const char sql[] =
        "INSERT INTO rooms (id, created_at, creator_id, name, type, "
        "updated_at) VALUES (?, ?, ?, ?, ?, ?)";
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL) ==
               SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, id);
    bind_text(stmt, 2, created);
    sqlite3_bind_int64(stmt, 3, creator);
    bind_text(stmt, 4, name);
    bind_text(stmt, 5, type);
    bind_text(stmt, 6, updated);
    exec_stmt(db, sql, stmt);
}

static void seed_membership(cf_db *db, int64_t id, int64_t room_id,
                            int64_t user_id) {
    static const char sql[] =
        "INSERT INTO memberships (id, created_at, room_id, updated_at, "
        "user_id) VALUES (?, '2026-09-26 12:00:00.000000', ?, "
        "'2026-09-26 12:00:00.000000', ?)";
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL) ==
               SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_int64(stmt, 2, room_id);
    sqlite3_bind_int64(stmt, 3, user_id);
    exec_stmt(db, sql, stmt);
}

static void seed_message(cf_db *db, int64_t id, const char *client,
                         int64_t room_id, int64_t creator, const char *created,
                         const char *updated) {
    static const char sql[] =
        "INSERT INTO messages (id, client_message_id, created_at, creator_id, "
        "room_id, updated_at) VALUES (?, ?, ?, ?, ?, ?)";
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL) ==
               SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, id);
    bind_text(stmt, 2, client);
    bind_text(stmt, 3, created);
    sqlite3_bind_int64(stmt, 4, creator);
    sqlite3_bind_int64(stmt, 5, room_id);
    bind_text(stmt, 6, updated);
    exec_stmt(db, sql, stmt);
}

static void seed_fts(cf_db *db, int64_t message_id, const char *body) {
    static const char sql[] =
        "INSERT INTO message_search_index (rowid, body) VALUES (?, ?)";
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL) ==
               SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, message_id);
    bind_text(stmt, 2, body);
    exec_stmt(db, sql, stmt);
}

static void seed_rich_text(cf_db *db, int64_t id, int64_t message_id,
                           const char *body, const char *at) {
    static const char sql[] =
        "INSERT INTO action_text_rich_texts (id, body, created_at, name, "
        "record_id, record_type, updated_at) VALUES (?, ?, ?, 'body', ?, "
        "'Message', ?)";
    sqlite3_stmt *stmt = NULL;
    CF_REQUIRE(sqlite3_prepare_v2(cf_db_handle(db), sql, -1, &stmt, NULL) ==
               SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, id);
    bind_text(stmt, 2, body);
    bind_text(stmt, 3, at);
    sqlite3_bind_int64(stmt, 4, message_id);
    bind_text(stmt, 5, at);
    exec_stmt(db, sql, stmt);
}

/* The golden page's rows: account 37signals, David (administrator) and Kevin
 * in room Designers, and the "rich" message with its body. */
static void seed_golden(msg_env *env) {
    cf_db *db = env->scratch.db;
    seed_account(db, GOLDEN_ACCOUNT_ID, "37signals",
                 "2026-09-26 13:03:57");
    seed_user(db, GOLDEN_DAVID, "David", NULL, 1, 0,
              "2026-09-26 13:03:57");
    seed_user(db, GOLDEN_KEVIN, "Kevin", "Programmer & \"tester\" <3", 0, 0,
              "2026-09-26 13:03:59");
    seed_user(db, GOLDEN_JZ, "JZ", "Designer", 0, 0,
              "2026-09-26 13:03:57");
    seed_room(db, GOLDEN_ROOM, "Designers", "Rooms::Closed", GOLDEN_KEVIN,
              "2026-09-26 12:00:00.000000", "2026-09-26 13:03:59.852991");
    seed_membership(db, 900001, GOLDEN_ROOM, GOLDEN_DAVID);
    seed_membership(db, 900002, GOLDEN_ROOM, GOLDEN_KEVIN);
    seed_membership(db, 900003, GOLDEN_ROOM, GOLDEN_JZ);
    seed_message(db, GOLDEN_MESSAGE, "rich", GOLDEN_ROOM, GOLDEN_KEVIN,
                 "2026-09-26 13:01:07.148296",
                 "2026-09-26 13:03:59.852991");
    seed_rich_text(db, 1, GOLDEN_MESSAGE, GOLDEN_RICH_BODY,
                   "2026-09-26 13:01:07.148296");
    seed_fts(db, GOLDEN_MESSAGE, "Visit example bold quoted");
    /* The create-not-found room: exists, David is not a member. */
    seed_room(db, GOLDEN_OTHER_ROOM, "Private", "Rooms::Closed", GOLDEN_KEVIN,
              "2026-09-26 12:00:00.000000", "2026-09-26 12:00:00.000000");
}

/* The account and David (administrator), so layout pages and room scoping
 * work in the non-golden cases. */
static void seed_base(msg_env *env) {
    seed_account(env->scratch.db, GOLDEN_ACCOUNT_ID, "37signals",
                 "2026-09-26 13:03:57");
    seed_user(env->scratch.db, GOLDEN_DAVID, "David", NULL, 1, 0,
              "2026-09-26 13:03:57");
}

/* A minimal room for the mutation cases. */
static void seed_room_for(cf_db *db, int64_t room_id, int64_t user_id,
                          const char *type) {
    seed_room(db, room_id, type != NULL ? "Room" : NULL, type, user_id,
              "2026-09-26 12:00:00.000000", "2026-09-26 12:00:00.000000");
    seed_membership(db, room_id * 10 + 1, room_id, user_id);
}

/* --- sessions ---------------------------------------------------------------- */

static void make_session_cookie(msg_env *env, int64_t user_id, char *out,
                                size_t cap) {
    char token[64];
    snprintf(token, sizeof token, "session-%lld-%d", (long long)user_id,
             env->session_seq++);
    char sql[256];
    snprintf(sql, sizeof sql,
             "INSERT INTO sessions (created_at,last_active_at,token,"
             "updated_at,user_id) VALUES "
             "('2040-01-01 00:00:00.000000','2040-01-01 00:00:00.000000',"
             "'%s','2040-01-01 00:00:00.000000',%lld)",
             token, (long long)user_id);
    exec_sql(env->scratch.db, sql);
    cf_str signed_value = {0};
    CF_REQUIRE(cf_auth_signed_cookie_generate(
                   SP(NEWS_SECRET), SP("session_token"), SP(token), false, 0,
                   &signed_value) == CF_OK);
    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, cap - pos, "session_token=");
    for (size_t i = 0; i < signed_value.len && pos + 4 < cap; i++) {
        unsigned char c = (unsigned char)signed_value.ptr[i];
        if (c == '+') {
            pos += (size_t)snprintf(out + pos, cap - pos, "%%2B");
        } else if (c == '/') {
            pos += (size_t)snprintf(out + pos, cap - pos, "%%2F");
        } else if (c == '=') {
            pos += (size_t)snprintf(out + pos, cap - pos, "%%3D");
        } else {
            out[pos++] = (char)c;
        }
    }
    out[pos < cap ? pos : cap - 1] = '\0';
    cf_str_dispose(&signed_value);
}

/* --- requests ----------------------------------------------------------------- */

static bool run_request(msg_env *env, cf_request *req, cf_response *resp) {
    cf_err rc = cf_ctx_process(env->app, env->scratch.db, req, resp);
    if (rc != CF_OK) {
        fprintf(stderr, "  ctx_process failed: rc=%d path=%.*s\n", rc,
                (int)req->path.len, req->path.ptr);
    }
    return rc == CF_OK;
}

static void req_get(cf_request *req, const char *path, const char *query,
                    const char *cookie) {
    static char target[1024];
    cf_test_req_init(req);
    req->path = SP(path);
    if (query != NULL && query[0] != '\0') {
        snprintf(target, sizeof target, "%s?%s", path, query);
        req->target = SP(target);
        req->query = SP(target + strlen(path) + 1);
    } else {
        req->target = SP(path);
    }
    if (cookie != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(cookie)) == CF_OK);
    }
}

static void req_form(cf_request *req, cf_method method, const char *path,
                     const char *body, const char *cookie, const char *accept) {
    cf_test_req_init(req);
    req->method = method;
    req->original_method = method;
    req->path = SP(path);
    req->target = SP(path);
    req->body = SP(body);
    CF_REQUIRE(cf_test_req_header(req, SP("Content-Type"),
                                  SP("application/x-www-form-urlencoded")) ==
               CF_OK);
    CF_REQUIRE(cf_test_req_header(req, SP("Sec-Fetch-Site"),
                                  SP("same-origin")) == CF_OK);
    if (accept != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Accept"), SP(accept)) ==
                   CF_OK);
    }
    if (cookie != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(cookie)) == CF_OK);
    }
}

static void req_delete(cf_request *req, const char *path, const char *cookie,
                       const char *accept) {
    cf_test_req_init(req);
    req->method = CF_DELETE;
    req->original_method = CF_DELETE;
    req->path = SP(path);
    req->target = SP(path);
    CF_REQUIRE(cf_test_req_header(req, SP("Sec-Fetch-Site"),
                                  SP("same-origin")) == CF_OK);
    if (accept != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Accept"), SP(accept)) ==
                   CF_OK);
    }
    if (cookie != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(cookie)) == CF_OK);
    }
}

static bool span_contains(cf_span span, const char *needle) {
    size_t len = strlen(needle);
    if (len == 0) return true;
    if (span.len < len) return false;
    for (size_t i = 0; i + len <= span.len; i++) {
        if (memcmp(span.ptr + i, needle, len) == 0) return true;
    }
    return false;
}

static bool body_contains(const cf_response *resp, const char *needle) {
    return resp->body != NULL && span_contains(cf_buf_span(resp->body), needle);
}

static bool head_contains(cf_response *resp, cf_request *req,
                          const char *needle) {
    if (resp->status == 0) resp->status = 200;
    cf_http_serialized ser = {0};
    CF_REQUIRE(cf_http_response_serialize(resp, req, &ser) == CF_OK);
    bool found = ser.headers != NULL &&
                 span_contains(cf_buf_span(ser.headers), needle);
    cf_buf_release(ser.headers);
    return found;
}

/* The raw value after `"Name: "` in the serialized response headers. */
static bool header_value(cf_response *resp, cf_request *req, const char *name,
                         char *out, size_t cap) {
    if (resp->status == 0) resp->status = 200;
    cf_http_serialized ser = {0};
    CF_REQUIRE(cf_http_response_serialize(resp, req, &ser) == CF_OK);
    cf_span head = cf_buf_span(ser.headers);
    size_t len = strlen(name);
    bool found = false;
    for (size_t i = 0; i + len <= head.len; i++) {
        if (memcmp(head.ptr + i, name, len) != 0) continue;
        size_t at = i + len, end = at;
        while (end < head.len && head.ptr[end] != '\r') end++;
        size_t n = end - at < cap - 1 ? end - at : cap - 1;
        memcpy(out, head.ptr + at, n);
        out[n] = '\0';
        found = true;
        break;
    }
    cf_buf_release(ser.headers);
    return found;
}

/* --- golden comparison --------------------------------------------------------- */

static void expect_golden(const char *name, const cf_response *resp) {
    yyjson_doc *doc = cf_golden_b_load(name);
    CF_REQUIRE(doc != NULL);
    cf_span body = cf_buf_span(resp->body);
    cf_golden_b_expect(doc, name, 1, body.ptr, body.len);
    yyjson_doc_free(doc);
}

/* --- broadcast capture --------------------------------------------------------- */

typedef struct {
    ct_run run;
    cf_cable_request request;
    cf_header headers[1];
    char cookie[1024];
    char identifier[1024];
} msg_conn;

/* A signed Turbo stream name for the room's message stream, and the channel
 * identifier carrying it. */
static void conn_identifier(msg_env *env, int64_t room_id, char *out,
                            size_t cap) {
    cf_room room = {0};
    bool found = false;
    CF_REQUIRE(cf_room_find_by_id(env->scratch.db, room_id, &found, &room) ==
               CF_OK);
    CF_REQUIRE(found);
    cf_str gid = {0};
    CF_REQUIRE(cf_cable_room_gid_param(&room, &gid) == CF_OK);
    cf_span parts[2] = {{(const unsigned char *)gid.ptr, gid.len},
                        {(const unsigned char *)"messages", 8}};
    cf_str signed_name = {0};
    CF_REQUIRE(cf_auth_turbo_signed_stream_name(SP(NEWS_SECRET), parts, 2,
                                                &signed_name) == CF_OK);
    snprintf(out, cap,
             "{\"channel\":\"RoomMessagesChannel\","
             "\"signed_stream_name\":\"%.*s\"}",
             (int)signed_name.len, signed_name.ptr);
    cf_str_dispose(&signed_name);
    cf_str_dispose(&gid);
    cf_room_dispose(&room);
}

static bool conn_open(msg_env *env, int64_t user_id, msg_conn *c) {
    memset(c, 0, sizeof *c);
    make_session_cookie(env, user_id, c->cookie, sizeof c->cookie);
    c->headers[0].name = SP("Cookie");
    c->headers[0].value = SP(c->cookie);
    c->request.method = CF_GET;
    c->request.headers = c->headers;
    c->request.header_count = 1;
    cf_cable_hooks hooks;
    cf_cable_server_hooks(env->cable, &hooks);
    if (!ct_run_start_with_request(&c->run, &hooks, NULL, false, &c->request)) {
        return false;
    }
    unsigned opcode = 0;
    bool rsv1 = false;
    unsigned char payload[256];
    ssize_t n = ct_read_server_frame(c->run.peer_fd, &opcode, &rsv1, payload,
                                     sizeof payload, 3000);
    return n == 18 && memcmp(payload, "{\"type\":\"welcome\"}", 18) == 0;
}

/* The next non-ping text frame on the socket; false on timeout/close. */
static bool conn_next_text(msg_conn *c, char *buf, size_t cap, int timeout_ms) {
    int64_t deadline = ct_now_ms() + timeout_ms;
    for (;;) {
        int left = (int)(deadline - ct_now_ms());
        if (left <= 0) return false;
        unsigned opcode = 0;
        bool rsv1 = false;
        ssize_t n = ct_read_server_frame(c->run.peer_fd, &opcode, &rsv1,
                                         (unsigned char *)buf, cap - 1, left);
        if (n < 0) return false;
        buf[n] = '\0';
        if (opcode != 0x1) continue;
        if (strncmp(buf, "{\"type\":\"ping\"", 14) == 0) continue;
        return true;
    }
}

static bool conn_subscribe(msg_env *env, msg_conn *c, int64_t room_id) {
    conn_identifier(env, room_id, c->identifier, sizeof c->identifier);
    cf_builder quoted = {0};
    if (cf_json_string(&quoted, SP(c->identifier)) != CF_OK) return false;
    char command[2048];
    snprintf(command, sizeof command,
             "{\"command\":\"subscribe\",\"identifier\":%.*s}",
             (int)quoted.len, quoted.ptr);
    cf_builder_dispose(&quoted);
    if (!ct_send_client_frame(c->run.peer_fd, 0x1, true, false,
                              (const unsigned char *)command,
                              strlen(command))) {
        return false;
    }
    /* confirm_subscription */
    char got[262144];
    if (!conn_next_text(c, got, sizeof got, 5000)) {
        fprintf(stderr, "  no subscribe confirmation\n");
        return false;
    }
    /* The confirmation is `{"identifier":...,"type":"confirm_subscription"}`;
     * its identifier encoding is the channel's own, so compare the tail. */
    return strstr(got, "\"type\":\"confirm_subscription\"") != NULL;
}

/* Read one frame and compare it with the exact delivery frame the broadcast
 * source produces for `payload` on this subscription. */
static bool conn_expect_payload(msg_conn *c, cf_span payload, int timeout_ms) {
    cf_builder ident_json = {0};
    cf_builder payload_json = {0};
    cf_err rc = cf_json_string(&ident_json, SP(c->identifier));
    if (rc == CF_OK) rc = cf_json_string(&payload_json, payload);
    cf_str expected = {0};
    if (rc == CF_OK) {
        rc = cf_cable_message_frame((cf_span){ident_json.ptr, ident_json.len},
                                    (cf_span){payload_json.ptr,
                                              payload_json.len},
                                    &expected);
    }
    cf_builder_dispose(&ident_json);
    cf_builder_dispose(&payload_json);
    CF_REQUIRE(rc == CF_OK);

    char got[262144];
    if (!conn_next_text(c, got, sizeof got, timeout_ms)) {
        fprintf(stderr, "  no broadcast frame\n");
        cf_str_dispose(&expected);
        return false;
    }
    bool ok = strlen(got) == expected.len &&
              memcmp(got, expected.ptr, expected.len) == 0;
    if (!ok) {
        fprintf(stderr, "  broadcast mismatch\n  got:      %s\n  expected: %s\n",
                got, expected.ptr);
    }
    cf_str_dispose(&expected);
    return ok;
}

static void conn_close(msg_conn *c) {
    ct_run_finish(&c->run);
    ct_run_join(&c->run);
}

/* Render the presentation partial the update broadcast carries, through
 * A02's presenter and renderer. */
static cf_err render_presentation(msg_env *env, const cf_message *message,
                                  cf_builder *out) {
    cf_ctx ctx = {0};
    ctx.app = env->app;
    ctx.reader = env->scratch.db;
    cf_view_ctx view_ctx = {0};
    cf_test_views_assets_ctx(&view_ctx);
    cf_view_message view = {0};
    cf_err rc = cf_presenter_message(&ctx, message, &view);
    if (rc == CF_OK) {
        rc = cf_view_message_presentation(&view_ctx, &view, out);
    }
    cf_view_message_dispose(&view);
    return rc;
}

/* --- acceptance: routes -------------------------------------------------------- */

CF_TEST(messages_route_ids_bind_the_actions) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    CF_CHECK(cf_route_action(76) == cf_action_messages_index);
    CF_CHECK(cf_route_action(137) == cf_action_messages_index);
    CF_CHECK(cf_route_action(77) == cf_action_messages_create);
    CF_CHECK(cf_route_action(138) == cf_action_messages_create);
    CF_CHECK(cf_route_action(79) == cf_action_messages_edit);
    CF_CHECK(cf_route_action(140) == cf_action_messages_edit);
    CF_CHECK(cf_route_action(80) == cf_action_messages_show);
    CF_CHECK(cf_route_action(141) == cf_action_messages_show);
    CF_CHECK(cf_route_action(81) == cf_action_messages_update);
    CF_CHECK(cf_route_action(82) == cf_action_messages_update);
    CF_CHECK(cf_route_action(142) == cf_action_messages_update);
    CF_CHECK(cf_route_action(143) == cf_action_messages_update);
    CF_CHECK(cf_route_action(83) == cf_action_messages_destroy);
    CF_CHECK(cf_route_action(144) == cf_action_messages_destroy);
    env_close(&env);
}

/* --- acceptance: unauthenticated ------------------------------------------------- */

CF_TEST(messages_unauthenticated_redirects_to_sign_in) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_room_for(env.scratch.db, 100, GOLDEN_DAVID, "Rooms::Closed");

    cf_request req;
    cf_response resp;
    req_get(&req, "/rooms/100/messages", NULL, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN "/session/new\r\n"));
    cf_response_dispose(&resp);

    /* Every action carries Before::default(): the create is redirected
     * before any parameter work. */
    req_form(&req, CF_POST, "/rooms/100/messages", "message[body]=hi", NULL,
             TURBO_ACCEPT);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM messages") == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- acceptance: index ---------------------------------------------------------- */

CF_TEST(messages_index_empty_page_is_204) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_room_for(env.scratch.db, 100, GOLDEN_DAVID, "Rooms::Closed");
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_get(&req, "/rooms/100/messages", NULL, cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 204);
    CF_CHECK(resp.body == NULL);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(messages_index_pages_by_before_and_after) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_room_for(env.scratch.db, 100, GOLDEN_DAVID, "Rooms::Closed");
    for (int i = 1; i <= 45; i++) {
        /* Non-zero microseconds: the D01 time writer drops the ".%06d"
         * suffix at zero, and a whole-second row compares greater than its
         * own bound text (reported to the integrator). */
        char created[64];
        snprintf(created, sizeof created,
                 "2026-09-26 12:%02d:%02d.%06d", i / 60, i % 60, i);
        char updated[64];
        snprintf(updated, sizeof updated,
                 "2026-09-26 13:%02d:%02d.%06d", i / 60, i % 60, i);
        char client[32];
        snprintf(client, sizeof client, "c%02d", i);
        seed_message(env.scratch.db, 1000 + i, client, 100, GOLDEN_DAVID,
                     created, updated);
        char body[64];
        snprintf(body, sizeof body, "<div>m%02d</div>", i);
        seed_rich_text(env.scratch.db, 2000 + i, 1000 + i, body, created);
    }
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    /* Last page: the newest 40, oldest first. */
    cf_request req;
    cf_response resp;
    req_get(&req, "/rooms/100/messages", NULL, cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "id=\"message_c06\"") ||
             body_contains(&resp, "id=\"message_c6\""));
    CF_CHECK(!body_contains(&resp, "id=\"message_c01\"") &&
             !body_contains(&resp, "id=\"message_c1\""));
    cf_response_dispose(&resp);

    /* before wins over after; the page ends at the named message. */
    req_get(&req, "/rooms/100/messages",
            "before=1045&after=1000", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "id=\"message_c05\"") ||
             body_contains(&resp, "id=\"message_c5\""));
    cf_response_dispose(&resp);

    /* A message after the last one has an empty page. */
    req_get(&req, "/rooms/100/messages", "after=1045", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 204);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(messages_index_conditional_get) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_room_for(env.scratch.db, 100, GOLDEN_DAVID, "Rooms::Closed");
    seed_message(env.scratch.db, 1001, "c1", 100, GOLDEN_DAVID,
                 "2026-09-26 12:00:01.000000", "2026-09-26 12:00:02.000000");
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_get(&req, "/rooms/100/messages", NULL, cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    char etag[128] = {0};
    char last_modified[64] = {0};
    CF_REQUIRE(header_value(&resp, &req, "ETag: ", etag, sizeof etag));
    CF_REQUIRE(header_value(&resp, &req, "Last-Modified: ", last_modified,
                            sizeof last_modified));
    CF_CHECK(strncmp(etag, "W/\"", 3) == 0);
    CF_CHECK(strlen(etag) == 3 + 32 + 1); /* weak + 32 hex + quote */
    /* `Time#httpdate` of the max updated_at (2026-09-26 12:00:02 UTC). */
    CF_CHECK(strcmp(last_modified, "Sat, 26 Sep 2026 12:00:02 GMT") == 0);
    cf_response_dispose(&resp);

    /* If-None-Match with the returned validator is fresh. */
    req_get(&req, "/rooms/100/messages", NULL, cookie);
    CF_REQUIRE(cf_test_req_header(&req, SP("If-None-Match"), SP(etag)) ==
               CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 304);
    CF_CHECK(resp.body == NULL);
    cf_response_dispose(&resp);

    /* If-Modified-Since at the Last-Modified instant is fresh. */
    req_get(&req, "/rooms/100/messages", NULL, cookie);
    CF_REQUIRE(cf_test_req_header(&req, SP("If-Modified-Since"),
                                  SP(last_modified)) == CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 304);
    cf_response_dispose(&resp);

    /* One second earlier is stale (the comparison is not vacuous). */
    req_get(&req, "/rooms/100/messages", NULL, cookie);
    CF_REQUIRE(cf_test_req_header(&req, SP("If-Modified-Since"),
                                  SP("Sat, 26 Sep 2026 12:00:01 GMT")) ==
               CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    cf_response_dispose(&resp);

    /* A stale validator renders. */
    req_get(&req, "/rooms/100/messages", NULL, cookie);
    CF_REQUIRE(cf_test_req_header(&req, SP("If-None-Match"),
                                  SP("W/\"ffffffffffffffffffffffffffffffff\"")) ==
               CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* kit ctx.rs's `is_fresh` reads If-None-Match / If-Modified-Since through
 * `request.header()`, so the to_str gate applies: an unreadable value is the
 * reference's None.  An unreadable If-None-Match must not consume the
 * conditional -- the fresh If-Modified-Since still answers 304 -- and an
 * unreadable If-Modified-Since reads as absent (stale). */
CF_TEST(messages_index_unreadable_conditional_headers) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_room_for(env.scratch.db, 100, GOLDEN_DAVID, "Rooms::Closed");
    seed_message(env.scratch.db, 1001, "c1", 100, GOLDEN_DAVID,
                 "2026-09-26 12:00:01.000000", "2026-09-26 12:00:02.000000");
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    char etag[128] = {0};
    char last_modified[64] = {0};
    req_get(&req, "/rooms/100/messages", NULL, cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_REQUIRE(resp.status == 200);
    CF_REQUIRE(header_value(&resp, &req, "ETag: ", etag, sizeof etag));
    CF_REQUIRE(header_value(&resp, &req, "Last-Modified: ", last_modified,
                            sizeof last_modified));
    cf_response_dispose(&resp);

    char inm_obs[160] = {0};
    snprintf(inm_obs, sizeof inm_obs, "%s\xc3\xa9", etag);
    char inm_del[160] = {0};
    snprintf(inm_del, sizeof inm_del, "%s\x7f", etag);
    char ims_obs[96] = {0};
    snprintf(ims_obs, sizeof ims_obs, "%s\xc3\xa9", last_modified);

    struct {
        const char *label;
        const char *inm; /* NULL = absent */
        const char *ims; /* NULL = absent */
        unsigned expected;
    } cases[] = {
        {"obs-text If-None-Match + fresh If-Modified-Since", inm_obs,
         last_modified, 304},
        {"DEL If-None-Match + fresh If-Modified-Since", inm_del,
         last_modified, 304},
        {"unreadable If-None-Match alone", inm_obs, NULL, 200},
        {"unreadable If-Modified-Since alone", NULL, ims_obs, 200},
        {"readable If-Modified-Since control", NULL, last_modified, 304},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        req_get(&req, "/rooms/100/messages", NULL, cookie);
        if (cases[i].inm != NULL) {
            CF_REQUIRE(cf_test_req_header(&req, SP("If-None-Match"),
                                          SP(cases[i].inm)) == CF_OK);
        }
        if (cases[i].ims != NULL) {
            CF_REQUIRE(cf_test_req_header(&req, SP("If-Modified-Since"),
                                          SP(cases[i].ims)) == CF_OK);
        }
        CF_REQUIRE(run_request(&env, &req, &resp));
        if (resp.status != cases[i].expected) {
            printf("    %s: expected %u got %u\n", cases[i].label,
                   cases[i].expected, resp.status);
            CF_CHECK(resp.status == cases[i].expected);
        }
        cf_response_dispose(&resp);
    }
    env_close(&env);
}

/* `msg_turbo_frame_request` feeds `combine_etags`' "frame" part: the pinned
 * predicate is true only for a header whose bytes all pass http 1.5.0's
 * `HeaderValue::to_str` (HTAB or visible ASCII).  The expected validators are
 * the sha256 (first 16 bytes, weak-quoted) of the reference preimages
 * `messages/1001-20260926120002000000/messages/index` and
 * `messages/1001-20260926120002000000/frame/messages/index`. */
CF_TEST(messages_index_turbo_frame_predicate_etag) {
    static const char *PAGE_ETAG = "W/\"942f8bd28c6ae48b4491b41d50d5b51f\"";
    static const char *FRAME_ETAG = "W/\"b479834bdbdb7cf4b20682352942dfd8\"";

    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_room_for(env.scratch.db, 100, GOLDEN_DAVID, "Rooms::Closed");
    seed_message(env.scratch.db, 1001, "c1", 100, GOLDEN_DAVID,
                 "2026-09-26 12:00:01.000000", "2026-09-26 12:00:02.000000");
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    char etag[128];

    /* No header: the page validator. */
    req_get(&req, "/rooms/100/messages", NULL, cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    etag[0] = '\0';
    CF_REQUIRE(header_value(&resp, &req, "ETag: ", etag, sizeof etag));
    CF_CHECK(strcmp(etag, PAGE_ETAG) == 0);
    cf_response_dispose(&resp);

    /* A normal value: the "frame" part joins the preimage. */
    req_get(&req, "/rooms/100/messages", NULL, cookie);
    CF_REQUIRE(cf_test_req_header(&req, SP("Turbo-Frame"),
                                  SP("message_rich")) == CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    etag[0] = '\0';
    CF_REQUIRE(header_value(&resp, &req, "ETag: ", etag, sizeof etag));
    CF_CHECK(strcmp(etag, FRAME_ETAG) == 0);
    cf_response_dispose(&resp);

    /* A non-ASCII value fails `HeaderValue::to_str`: the header reads as
     * absent, so the validator stays the page one. */
    req_get(&req, "/rooms/100/messages", NULL, cookie);
    CF_REQUIRE(cf_test_req_header(
                   &req, SP("Turbo-Frame"),
                   (cf_span){(const unsigned char *)"\xC3\xA9", 2}) == CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    etag[0] = '\0';
    CF_REQUIRE(header_value(&resp, &req, "ETag: ", etag, sizeof etag));
    CF_CHECK(strcmp(etag, PAGE_ETAG) == 0);
    cf_response_dispose(&resp);

    /* A space-only ASCII value is blank under `trim`: also the page one. */
    req_get(&req, "/rooms/100/messages", NULL, cookie);
    CF_REQUIRE(cf_test_req_header(&req, SP("Turbo-Frame"), SP(" ")) == CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    etag[0] = '\0';
    CF_REQUIRE(header_value(&resp, &req, "ETag: ", etag, sizeof etag));
    CF_CHECK(strcmp(etag, PAGE_ETAG) == 0);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* The reference's combine_etags joins
 * `flash.keys().map(|k| format!("{k}={:?}", flash.get(k))).join("&")` as one
 * etag part, in the flash map's insertion order (kit/src/ctx.rs): an
 * overwrite keeps the entry's position, a delete removes it.  Flash is set on
 * the live context before cf_dispatch so the insertion order is exact.  The
 * expected validators are the sha256 (first 16 bytes, weak-quoted) of the
 * reference preimages
 * `messages/1001-20260926120002000000/messages/index/<flash part>`. */
CF_TEST(messages_index_etag_follows_flash_insertion_order) {
    static const char *ALERT_FIRST =
        "W/\"02e48e1857692a0e89ddf33aa63eee62\""; /* alert=Some("A")&notice=Some("N") */
    static const char *NOTICE_FIRST =
        "W/\"2fd401207020916fcd8afbb0b23679cc\""; /* notice=Some("N")&alert=Some("A") */
    static const char *NOTICE_OVERWRITTEN =
        "W/\"19ddca50638ce88ce9b54a4bf2cb2ced\""; /* notice=Some("N2")&alert=Some("A") */
    static const char *REINSERTED_ALERT =
        "W/\"0304fdebad731e7ac3a54e0eed2016da\""; /* notice=Some("N2")&alert=Some("B") */
#define CHECK_ETAG(actual, expected, what)                                 \
    do {                                                                   \
        if (strcmp((actual), (expected)) != 0) {                           \
            fprintf(stderr, "  %s: got %s want %s\n", what, actual,        \
                    expected);                                             \
        }                                                                  \
        CF_CHECK(strcmp((actual), (expected)) == 0);                       \
    } while (0)
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_room_for(env.scratch.db, 100, GOLDEN_DAVID, "Rooms::Closed");
    seed_message(env.scratch.db, 1001, "c1", 100, GOLDEN_DAVID,
                 "2026-09-26 12:00:01.000000", "2026-09-26 12:00:02.000000");
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    cf_ctx ctx;
    char etag[128] = {0};

    /* alert inserted first, then notice: the etag joins alert then notice. */
    req_get(&req, "/rooms/100/messages", NULL, cookie);
    cf_response_init(&resp);
    CF_REQUIRE(cf_ctx_create(&ctx, env.app, env.scratch.db, &req, &resp) ==
               CF_OK);
    CF_REQUIRE(cf_ctx_flash_set(&ctx, SP("alert"), SP("A")) == CF_OK);
    CF_REQUIRE(cf_ctx_flash_set(&ctx, SP("notice"), SP("N")) == CF_OK);
    CF_REQUIRE(cf_dispatch(&ctx) == CF_OK);
    CF_CHECK(resp.status == 200);
    CF_REQUIRE(header_value(&resp, &req, "ETag: ", etag, sizeof etag));
    CHECK_ETAG(etag, ALERT_FIRST, "alert inserted first");
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);

    /* notice inserted first, then alert: a different digest. */
    req_get(&req, "/rooms/100/messages", NULL, cookie);
    cf_response_init(&resp);
    CF_REQUIRE(cf_ctx_create(&ctx, env.app, env.scratch.db, &req, &resp) ==
               CF_OK);
    CF_REQUIRE(cf_ctx_flash_set(&ctx, SP("notice"), SP("N")) == CF_OK);
    CF_REQUIRE(cf_ctx_flash_set(&ctx, SP("alert"), SP("A")) == CF_OK);
    CF_REQUIRE(cf_dispatch(&ctx) == CF_OK);
    memset(etag, 0, sizeof etag);
    CF_REQUIRE(header_value(&resp, &req, "ETag: ", etag, sizeof etag));
    CHECK_ETAG(etag, NOTICE_FIRST, "notice inserted first");
    CF_CHECK(strcmp(etag, ALERT_FIRST) != 0);
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);

    /* An overwrite replaces the value in place (the key order is unchanged:
     * notice, alert). */
    req_get(&req, "/rooms/100/messages", NULL, cookie);
    cf_response_init(&resp);
    CF_REQUIRE(cf_ctx_create(&ctx, env.app, env.scratch.db, &req, &resp) ==
               CF_OK);
    CF_REQUIRE(cf_ctx_flash_set(&ctx, SP("notice"), SP("N")) == CF_OK);
    CF_REQUIRE(cf_ctx_flash_set(&ctx, SP("alert"), SP("A")) == CF_OK);
    CF_REQUIRE(cf_ctx_flash_set(&ctx, SP("notice"), SP("N2")) == CF_OK);
    CF_REQUIRE(cf_dispatch(&ctx) == CF_OK);
    memset(etag, 0, sizeof etag);
    CF_REQUIRE(header_value(&resp, &req, "ETag: ", etag, sizeof etag));
    CHECK_ETAG(etag, NOTICE_OVERWRITTEN, "notice overwritten in place");
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);

    /* delete removes the entry; a re-insert appends at the end. */
    req_get(&req, "/rooms/100/messages", NULL, cookie);
    cf_response_init(&resp);
    CF_REQUIRE(cf_ctx_create(&ctx, env.app, env.scratch.db, &req, &resp) ==
               CF_OK);
    CF_REQUIRE(cf_ctx_flash_set(&ctx, SP("notice"), SP("N")) == CF_OK);
    CF_REQUIRE(cf_ctx_flash_set(&ctx, SP("alert"), SP("A")) == CF_OK);
    CF_REQUIRE(cf_ctx_flash_set(&ctx, SP("notice"), SP("N2")) == CF_OK);
    CF_REQUIRE(cf_ctx_flash_delete(&ctx, SP("alert")) == CF_OK);
    CF_REQUIRE(cf_ctx_flash_set(&ctx, SP("alert"), SP("B")) == CF_OK);
    CF_REQUIRE(cf_dispatch(&ctx) == CF_OK);
    memset(etag, 0, sizeof etag);
    CF_REQUIRE(header_value(&resp, &req, "ETag: ", etag, sizeof etag));
    CHECK_ETAG(etag, REINSERTED_ALERT, "alert deleted and re-inserted");
    cf_ctx_destroy(&ctx);
    cf_response_dispose(&resp);

    env_close(&env);
}

/* The httpdate formatter/parser over the year wrap: December and
 * January/February rows (Hinnant's month mapping). */
CF_TEST(messages_index_last_modified_winter_dates) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_room_for(env.scratch.db, 100, GOLDEN_DAVID, "Rooms::Closed");
    /* Whole-second updated_at: If-Modified-Since is second-precision, and
     * the reference's `since >= last_modified` keeps sub-second rows stale. */
    seed_message(env.scratch.db, 1001, "dec", 100, GOLDEN_DAVID,
                 "2026-12-05 13:00:00.000000",
                 "2026-12-05 13:00:00.000000");
    seed_message(env.scratch.db, 1002, "feb", 100, GOLDEN_DAVID,
                 "2027-02-03 09:30:00.000000",
                 "2027-02-03 09:30:00.000000");
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_get(&req, "/rooms/100/messages", NULL, cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    char last_modified[64] = {0};
    CF_REQUIRE(header_value(&resp, &req, "Last-Modified: ", last_modified,
                            sizeof last_modified));
    CF_CHECK(strcmp(last_modified, "Wed, 03 Feb 2027 09:30:00 GMT") == 0);
    cf_response_dispose(&resp);

    req_get(&req, "/rooms/100/messages", NULL, cookie);
    CF_REQUIRE(cf_test_req_header(&req, SP("If-Modified-Since"),
                                  SP("Wed, 03 Feb 2027 09:30:00 GMT")) ==
               CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 304);
    cf_response_dispose(&resp);

    req_get(&req, "/rooms/100/messages", NULL, cookie);
    CF_REQUIRE(cf_test_req_header(&req, SP("If-Modified-Since"),
                                  SP("Wed, 03 Feb 2027 09:29:59 GMT")) ==
               CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(messages_index_not_found_cases) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_room_for(env.scratch.db, 100, GOLDEN_DAVID, "Rooms::Closed");
    seed_message(env.scratch.db, 1001, "c1", 100, GOLDEN_DAVID,
                 "2026-09-26 12:00:01.000000", "2026-09-26 12:00:01.000000");
    seed_rich_text(env.scratch.db, 1, 1001, "<div>hi</div>",
                   "2026-09-26 12:00:01.000000");
    seed_room(env.scratch.db, 200, "Other", "Rooms::Closed", GOLDEN_KEVIN,
              "2026-09-26 12:00:00.000000", "2026-09-26 12:00:00.000000");
    seed_membership(env.scratch.db, 2001, 200, GOLDEN_KEVIN);
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    /* Unknown room. */
    req_get(&req, "/rooms/999/messages", NULL, cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);

    /* Room without a membership. */
    req_get(&req, "/rooms/200/messages", NULL, cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);

    /* present-but-uncastable before/after are RecordNotFound. */
    req_get(&req, "/rooms/100/messages", "before=0", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);
    req_get(&req, "/rooms/100/messages", "before=abc", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);
    req_get(&req, "/rooms/100/messages", "before[]=1", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);

    /* An unacceptable format is UnknownFormat (406). */
    req_get(&req, "/rooms/100/messages", NULL, cookie);
    CF_REQUIRE(cf_test_req_header(&req, SP("Accept"),
                                  SP("application/xml")) == CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 406);
    cf_response_dispose(&resp);

    /* The shallow route without a room_id has no scope (404). */
    req_get(&req, "/messages", NULL, cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);

    /* A non-numeric room_id is RecordNotFound, not a 400. */
    req_get(&req, "/rooms/abc/messages", NULL, cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);

    /* A membership whose room is gone is the reference's 500. */
    seed_membership(env.scratch.db, 3001, 300, GOLDEN_DAVID);
    req_get(&req, "/rooms/300/messages", NULL, cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- acceptance: show/edit ------------------------------------------------------ */

CF_TEST(messages_show_matches_the_golden) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_golden(&env);
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);
    char with_last_room[1152];
    snprintf(with_last_room, sizeof with_last_room, "%s; last_room=%lld",
             cookie, (long long)GOLDEN_ROOM);

    cf_request req;
    cf_response resp;
    req_get(&req, "/rooms/654632876/messages/933434491", NULL, with_last_room);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/html; charset=utf-8\r\n"));
    CF_CHECK(head_contains(&resp, &req, "Link: "));
    expect_golden("messages_show_text", &resp);
    cf_response_dispose(&resp);

    /* MessagesController declares its own layout: a Turbo-Frame request still
     * gets the application layout. */
    req_get(&req, "/rooms/654632876/messages/933434491", NULL, with_last_room);
    CF_REQUIRE(cf_test_req_header(&req, SP("Turbo-Frame"), SP("message_rich")) ==
               CF_OK);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "<html"));
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(messages_show_unknown_message_is_404) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_golden(&env);
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_get(&req, "/rooms/654632876/messages/999999", NULL, cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 404);
    cf_response_dispose(&resp);

    /* A shallow route reads room_id from the query. */
    req_get(&req, "/messages/933434491", "room_id=654632876", cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(messages_edit_matches_the_golden) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_golden(&env);
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);
    char with_last_room[1152];
    snprintf(with_last_room, sizeof with_last_room, "%s; last_room=%lld",
             cookie, (long long)GOLDEN_ROOM);

    cf_request req;
    cf_response resp;
    req_get(&req, "/rooms/654632876/messages/933434491/edit", NULL,
            with_last_room);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    expect_golden("messages_edit_text", &resp);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(messages_edit_forbidden_for_a_member) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_golden(&env);
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_JZ, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_get(&req, "/rooms/654632876/messages/933434491/edit", NULL, cookie);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    CF_CHECK(head_contains(&resp, &req, "Content-Type: text/html\r\n"));
    CF_CHECK(resp.body == NULL);
    cf_response_dispose(&resp);
    env_close(&env);
}

/* --- acceptance: create --------------------------------------------------------- */

CF_TEST(messages_create_appends_and_writes_rows) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_room_for(env.scratch.db, 100, GOLDEN_DAVID, "Rooms::Closed");
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_form(&req, CF_POST, "/rooms/100/messages",
             "message[body]=%3Cp%3EHello%20%3Cstrong%3Ethere%3C%2Fstrong%3E%3C%2Fp%3E"
             "&message[client_message_id]=abc-123",
             cookie, TURBO_ACCEPT);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/vnd.turbo-stream.html; "
                           "charset=utf-8\r\n"));
    CF_CHECK(body_contains(
        &resp,
        "<turbo-stream action=\"append\" "
        "target=\"messages_rooms_closed_100\"><template>"));
    CF_CHECK(body_contains(&resp, "id=\"message_abc-123\""));
    CF_CHECK(body_contains(&resp, "Hello <strong>there</strong>"));
    cf_response_dispose(&resp);

    /* The stored rows: the message, its rich text and the FTS index. */
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM messages") == 1);
    char text[1024];
    CF_CHECK(one_text(env.scratch.db,
                      "SELECT client_message_id FROM messages", text,
                      sizeof text) &&
             strcmp(text, "abc-123") == 0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM action_text_rich_texts WHERE "
                        "record_type='Message' AND name='body'") == 1);
    CF_CHECK(one_text(env.scratch.db,
                      "SELECT body FROM action_text_rich_texts", text,
                      sizeof text) &&
             strcmp(text, "<p>Hello <strong>there</strong></p>") == 0);
    CF_CHECK(one_text(env.scratch.db,
                      "SELECT body FROM message_search_index", text,
                      sizeof text) &&
             strstr(text, "Hello there") != NULL);
    env_close(&env);
}

/* R02 canonical_body: the writer stores
 * `ActionText::Content.new(body, canonicalize: true).to_html()`, computed on
 * a reader before the transaction.  The response and the stored rows both
 * carry the canonical form (whitespace stripped, unclosed tags closed, the
 * entity re-escaped), and a body the pipeline raises on is stored as given
 * (`unwrap_or_else(|_| body.to_string())`). */
CF_TEST(messages_create_stores_the_canonical_body) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_room_for(env.scratch.db, 100, GOLDEN_DAVID, "Rooms::Closed");
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_form(&req, CF_POST, "/rooms/100/messages",
             "message[body]=%20%20%3Cb%3Ehi%3C%2Fb%3E%20%0A"
             "&message[client_message_id]=canon-1",
             cookie, TURBO_ACCEPT);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "id=\"message_canon-1\""));
    CF_CHECK(body_contains(&resp, "<b>hi</b>"));
    cf_response_dispose(&resp);

    /* An unparseable trix attachment raises during conversion; the rescue
     * stores the body exactly as submitted. */
    req_form(&req, CF_POST, "/rooms/100/messages",
             "message[body]=%3Cdiv%3E%3Cfigure%20data-trix-attachment%3D%22"
             "%26quot%3Bstr%26quot%3B%22%3Ex%3C%2Ffigure%3E%3C%2Fdiv%3E"
             "&message[client_message_id]=canon-fallback",
             cookie, TURBO_ACCEPT);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    cf_response_dispose(&resp);

    /* A present-but-empty body (`message[body]=`, the param's {NULL,0}) is the
     * reference's empty String: canonical "" and the message still created. */
    req_form(&req, CF_POST, "/rooms/100/messages",
             "message[body]=&message[client_message_id]=canon-empty", cookie,
             TURBO_ACCEPT);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "id=\"message_canon-empty\""));
    cf_response_dispose(&resp);

    char text[1024];
    CF_CHECK(one_text(env.scratch.db,
                      "SELECT r.body FROM action_text_rich_texts r JOIN messages "
                      "m ON m.id = r.record_id WHERE m.client_message_id = "
                      "'canon-1'",
                      text, sizeof text) &&
             strcmp(text, "<b>hi</b>") == 0);
    CF_CHECK(one_text(env.scratch.db,
                      "SELECT body FROM message_search_index WHERE rowid = "
                      "(SELECT id FROM messages WHERE client_message_id = "
                      "'canon-1')",
                      text, sizeof text) &&
             strcmp(text, "hi") == 0);
    CF_CHECK(one_text(env.scratch.db,
                      "SELECT r.body FROM action_text_rich_texts r JOIN messages "
                      "m ON m.id = r.record_id WHERE m.client_message_id = "
                      "'canon-fallback'",
                      text, sizeof text) &&
             strcmp(text, "<div><figure data-trix-attachment=\"&quot;str&quot;\""
                          ">x</figure></div>") == 0);
    CF_CHECK(one_text(env.scratch.db,
                      "SELECT r.body FROM action_text_rich_texts r JOIN messages "
                      "m ON m.id = r.record_id WHERE m.client_message_id = "
                      "'canon-empty'",
                      text, sizeof text) &&
             strcmp(text, "") == 0);
    env_close(&env);
}

CF_TEST(messages_create_broadcasts_the_response_bytes) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_room_for(env.scratch.db, 100, GOLDEN_DAVID, "Rooms::Closed");
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    msg_conn conn;
    CF_REQUIRE(conn_open(&env, GOLDEN_DAVID, &conn));
    CF_REQUIRE(conn_subscribe(&env, &conn, 100));

    cf_request req;
    cf_response resp;
    req_form(&req, CF_POST, "/rooms/100/messages",
             "message[body]=straight%20from%20the%20writer"
             "&message[client_message_id]=wire-1",
             cookie, TURBO_ACCEPT);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    cf_span body = cf_buf_span(resp.body);
    CF_CHECK(conn_expect_payload(&conn, body, 5000));
    cf_response_dispose(&resp);
    conn_close(&conn);
    env_close(&env);
}

CF_TEST(messages_create_requires_the_message_param) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_room_for(env.scratch.db, 100, GOLDEN_DAVID, "Rooms::Closed");
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_form(&req, CF_POST, "/rooms/100/messages", "body=hi", cookie,
             TURBO_ACCEPT);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 400);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM messages") == 0);
    env_close(&env);
}

CF_TEST(messages_create_unacceptable_format_is_406_after_commit) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_room_for(env.scratch.db, 100, GOLDEN_DAVID, "Rooms::Closed");
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    /* No Accept header: the implicit render only has the turbo-stream
     * template, so UnknownFormat (406) — after the create committed. */
    req_form(&req, CF_POST, "/rooms/100/messages", "message[body]=hi", cookie,
             NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 406);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM messages") == 1);
    env_close(&env);
}

/* The shallow POST finds its room through the query (the reference's
 * `params[:room_id]` merge). */
CF_TEST(messages_create_shallow_route_uses_the_query_room) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_room_for(env.scratch.db, 100, GOLDEN_DAVID, "Rooms::Closed");
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_form(&req, CF_POST, "/messages",
             "room_id=100&message[body]=shallow&message[client_message_id]=s-1",
             cookie, TURBO_ACCEPT);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "id=\"message_s-1\""));
    cf_response_dispose(&resp);
    char text[128];
    CF_CHECK(one_text(env.scratch.db,
                      "SELECT room_id FROM messages WHERE "
                      "client_message_id='s-1'",
                      text, sizeof text) &&
             strcmp(text, "100") == 0);
    env_close(&env);
}

CF_TEST(messages_create_in_a_left_room_renders_room_not_found) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_golden(&env);
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);
    char with_last_room[1152];
    snprintf(with_last_room, sizeof with_last_room, "%s; last_room=%lld",
             cookie, (long long)GOLDEN_ROOM);

    cf_request req;
    cf_response resp;
    req_form(&req, CF_POST, "/rooms/999001/messages", "message[body]=hi",
             with_last_room, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "This room was deleted."));
    CF_CHECK(body_contains(&resp, "<html"));
    expect_golden("messages_room_not_found", &resp);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM messages") == 1);
    env_close(&env);
}

CF_TEST(messages_create_invalid_attachment_is_500) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_room_for(env.scratch.db, 100, GOLDEN_DAVID, "Rooms::Closed");
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_form(&req, CF_POST, "/rooms/100/messages",
             "message[body]=hi&message[attachment]=not-a-blob", cookie,
             TURBO_ACCEPT);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM messages") == 0);
    env_close(&env);
}

/* --- acceptance: attachment upload (H02 cf_param_upload + S02 staging) --- */

/* A per-test storage root under the system temp dir; the config override is
 * set before env_open and removed with the tree at the end. */
static bool storage_root_make(char *out, size_t cap) {
    snprintf(out, cap, "/tmp/cf-msg-attach-XXXXXX");
    if (mkdtemp(out) == NULL) return false;
    g_storage_override = out;
    return true;
}

static void storage_root_remove(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d != NULL) {
            struct dirent *entry;
            while ((entry = readdir(d)) != NULL) {
                if (strcmp(entry->d_name, ".") == 0 ||
                    strcmp(entry->d_name, "..") == 0) {
                    continue;
                }
                char child[4096];
                snprintf(child, sizeof child, "%s/%s", path, entry->d_name);
                storage_root_remove(child);
            }
            closedir(d);
        }
        (void)rmdir(path);
    } else {
        (void)unlink(path);
    }
}

static void storage_key_path(const char *root, const char *key, char *out,
                             size_t cap) {
    snprintf(out, cap, "%s/%c%c/%c%c/%s", root, key[0], key[1], key[2],
             key[3], key);
}

static int dir_entry_count(const char *path) {
    DIR *d = opendir(path);
    if (d == NULL) return -1;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        count++;
    }
    closedir(d);
    return count;
}

/* --- multipart builder ------------------------------------------------------ */

struct mpbuf {
    char *p;
    size_t len, cap;
};

static void mp_need(struct mpbuf *m, size_t extra) {
    if (m->len + extra + 1 <= m->cap) return;
    size_t ncap = m->cap ? m->cap : 256;
    while (ncap < m->len + extra + 1) ncap *= 2;
    char *np = realloc(m->p, ncap);
    if (np == NULL) abort();
    m->p = np;
    m->cap = ncap;
}

static void mp_add(struct mpbuf *m, const void *bytes, size_t n) {
    mp_need(m, n);
    memcpy(m->p + m->len, bytes, n);
    m->len += n;
    m->p[m->len] = '\0';
}

static void mp_adds(struct mpbuf *m, const char *text) {
    mp_add(m, text, strlen(text));
}

static void mp_start(struct mpbuf *m, const char *boundary) {
    memset(m, 0, sizeof *m);
    mp_adds(m, "--");
    mp_adds(m, boundary);
    mp_adds(m, "\r\n");
}

static void mp_next(struct mpbuf *m, const char *boundary) {
    mp_adds(m, "--");
    mp_adds(m, boundary);
    mp_adds(m, "\r\n");
}

static void mp_field(struct mpbuf *m, const char *name, const char *value) {
    mp_adds(m, "Content-Disposition: form-data; name=\"");
    mp_adds(m, name);
    mp_adds(m, "\"\r\n\r\n");
    mp_adds(m, value);
    mp_adds(m, "\r\n");
}

static void mp_file(struct mpbuf *m, const char *name, const char *filename,
                    const char *content_type, const void *data, size_t len) {
    mp_adds(m, "Content-Disposition: form-data; name=\"");
    mp_adds(m, name);
    mp_adds(m, "\"; filename=\"");
    mp_adds(m, filename);
    mp_adds(m, "\"\r\nContent-Type: ");
    mp_adds(m, content_type);
    mp_adds(m, "\r\n\r\n");
    mp_add(m, data, len);
    mp_adds(m, "\r\n");
}

static void mp_finish(struct mpbuf *m, const char *boundary) {
    mp_adds(m, "--");
    mp_adds(m, boundary);
    mp_adds(m, "--\r\n");
}

/* A multipart POST whose body span borrows the builder (the request must not
 * outlive it). */
static void req_multipart(cf_request *req, cf_method method, const char *path,
                          const struct mpbuf *m, const char *cookie,
                          const char *accept) {
    cf_test_req_init(req);
    req->method = method;
    req->original_method = method;
    req->path = SP(path);
    req->target = SP(path);
    req->body = (cf_span){(const unsigned char *)m->p, m->len};
    CF_REQUIRE(cf_test_req_header(req, SP("Content-Type"),
                                  SP("multipart/form-data; boundary=B")) ==
               CF_OK);
    CF_REQUIRE(cf_test_req_header(req, SP("Sec-Fetch-Site"),
                                  SP("same-origin")) == CF_OK);
    if (accept != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Accept"), SP(accept)) == CF_OK);
    }
    if (cookie != NULL) {
        CF_REQUIRE(cf_test_req_header(req, SP("Cookie"), SP(cookie)) == CF_OK);
    }
}

CF_TEST(messages_create_with_attachment_stages_and_renders) {
    char root[64];
    CF_REQUIRE(storage_root_make(root, sizeof root));
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_room_for(env.scratch.db, 100, GOLDEN_DAVID, "Rooms::Closed");
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    static const char payload[] = "attachment payload";
    struct mpbuf m;
    mp_start(&m, "B");
    mp_field(&m, "message[body]", "hello");
    mp_next(&m, "B");
    mp_field(&m, "message[client_message_id]", "attach-1");
    mp_next(&m, "B");
    mp_file(&m, "message[attachment]", "notes.txt", "text/plain", payload,
            sizeof payload - 1);
    mp_finish(&m, "B");

    cf_request req;
    cf_response resp;
    req_multipart(&req, CF_POST, "/rooms/100/messages", &m, cookie,
                  TURBO_ACCEPT);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(body_contains(&resp, "id=\"message_attach-1\""));
    CF_CHECK(body_contains(&resp, "<span>notes.txt</span>"));
    CF_CHECK(body_contains(&resp, "/rails/active_storage/blobs/redirect/"));
    cf_response_dispose(&resp);
    free(m.p);

    /* Rows: the message, the blob (identified + analyzed) and the
     * attachment. */
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM messages") == 1);
    char text[2048];
    CF_CHECK(one_text(env.scratch.db,
                      "SELECT filename FROM active_storage_blobs", text,
                      sizeof text) &&
             strcmp(text, "notes.txt") == 0);
    CF_CHECK(one_text(env.scratch.db,
                      "SELECT content_type FROM active_storage_blobs", text,
                      sizeof text) &&
             strcmp(text, "text/plain") == 0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT byte_size FROM active_storage_blobs") ==
             (int64_t)(sizeof payload - 1));
    CF_CHECK(one_text(env.scratch.db,
                      "SELECT metadata FROM active_storage_blobs", text,
                      sizeof text) &&
             strcmp(text, "{\"identified\":true,\"analyzed\":true}") == 0);
    CF_CHECK(one_text(env.scratch.db,
                      "SELECT service_name FROM active_storage_blobs", text,
                      sizeof text) &&
             strcmp(text, "local") == 0);
    CF_CHECK(one_text(env.scratch.db,
                      "SELECT checksum FROM active_storage_blobs", text,
                      sizeof text) &&
             strcmp(text, "vvPgt6kVohmiNxyJvEdg8w==") == 0);
    char key[64];
    CF_REQUIRE(one_text(env.scratch.db,
                        "SELECT key FROM active_storage_blobs", key,
                        sizeof key));
    CF_CHECK(strlen(key) == 28);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM active_storage_attachments "
                        "WHERE name='attachment' AND record_type='Message' AND "
                        "blob_id=(SELECT id FROM active_storage_blobs)") == 1);
    /* The published file holds the bytes; staging left nothing in tmp. */
    char path[512];
    storage_key_path(root, key, path, sizeof path);
    size_t file_len = 0;
    char *file = NULL;
    {
        FILE *f = fopen(path, "rb");
        CF_REQUIRE(f != NULL);
        CF_REQUIRE(fseek(f, 0, SEEK_END) == 0);
        long len = ftell(f);
        CF_REQUIRE(len >= 0 && fseek(f, 0, SEEK_SET) == 0);
        file = malloc((size_t)len + 1);
        CF_REQUIRE(file != NULL);
        file_len = fread(file, 1, (size_t)len, f);
        fclose(f);
    }
    CF_CHECK(file_len == sizeof payload - 1 &&
             memcmp(file, payload, file_len) == 0);
    free(file);
    char tmp_path[512];
    snprintf(tmp_path, sizeof tmp_path, "%s/tmp", root);
    CF_CHECK(dir_entry_count(tmp_path) == 0);

    env_close(&env);
    storage_root_remove(root);
    g_storage_override = NULL;
}

CF_TEST(messages_update_replaces_and_deletes_attachment) {
    char root[64];
    CF_REQUIRE(storage_root_make(root, sizeof root));
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_room_for(env.scratch.db, 100, GOLDEN_DAVID, "Rooms::Closed");
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    /* Create an attachment message first. */
    struct mpbuf m;
    mp_start(&m, "B");
    mp_field(&m, "message[body]", "with file");
    mp_next(&m, "B");
    mp_file(&m, "message[attachment]", "first.txt", "text/plain",
            "first payload", 13);
    mp_finish(&m, "B");
    cf_request req;
    cf_response resp;
    req_multipart(&req, CF_POST, "/rooms/100/messages", &m, cookie,
                  TURBO_ACCEPT);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    cf_response_dispose(&resp);
    free(m.p);
    int64_t message_id = count_rows(env.scratch.db, "SELECT max(id) FROM messages");
    int64_t old_blob = count_rows(env.scratch.db,
                                  "SELECT max(id) FROM active_storage_blobs");
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM active_storage_attachments") ==
             1);

    /* Replace through the real PATCH route (multipart upload). */
    char path[64];
    snprintf(path, sizeof path, "/rooms/100/messages/%lld",
             (long long)message_id);
    mp_start(&m, "B");
    mp_field(&m, "message[body]", "with file");
    mp_next(&m, "B");
    mp_file(&m, "message[attachment]", "second.txt", "text/plain",
            "second payload", 14);
    mp_finish(&m, "B");
    req_multipart(&req, CF_PATCH, path, &m, cookie, TURBO_ACCEPT);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    cf_response_dispose(&resp);
    free(m.p);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM active_storage_blobs") == 2);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM active_storage_attachments") ==
             1);
    CF_CHECK(count_rows(
                 env.scratch.db,
                 "SELECT blob_id FROM active_storage_attachments") !=
             old_blob);
    CF_CHECK(env.event_count >= 1);
    CF_CHECK(env.events[env.event_count - 1].kind == CF_EVENT_PURGE_BLOB);
    CF_CHECK(env.events[env.event_count - 1].blob_id == old_blob);

    /* Detach with an empty permitted value (the reference's Delete arm). */
    int64_t new_blob = count_rows(env.scratch.db,
                                  "SELECT max(id) FROM active_storage_blobs");
    mp_start(&m, "B");
    mp_field(&m, "message[attachment]", "");
    mp_finish(&m, "B");
    req_multipart(&req, CF_PATCH, path, &m, cookie, TURBO_ACCEPT);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    cf_response_dispose(&resp);
    free(m.p);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM active_storage_attachments") ==
             0);
    CF_CHECK(env.events[env.event_count - 1].kind == CF_EVENT_PURGE_BLOB);
    CF_CHECK(env.events[env.event_count - 1].blob_id == new_blob);

    env_close(&env);
    storage_root_remove(root);
    g_storage_override = NULL;
}

CF_TEST(messages_attachment_signed_blob_reference) {
    char root[64];
    CF_REQUIRE(storage_root_make(root, sizeof root));
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_room_for(env.scratch.db, 100, GOLDEN_DAVID, "Rooms::Closed");
    seed_message(env.scratch.db, 300, "seed", 100, GOLDEN_DAVID,
                 "2026-09-26 12:00:00.000000", "2026-09-26 12:00:00.000000");
    /* An existing (direct-uploaded) blob. */
    exec_sql(env.scratch.db,
             "INSERT INTO active_storage_blobs (id, byte_size, checksum, "
             "content_type, created_at, filename, key, metadata, service_name) "
             "VALUES (7, 4, 'abc', 'text/plain', '2026-09-26 12:00:00', "
             "'ext.txt', '0123456789abcdefghijklmnopqrs', "
             "'{\"identified\":true}', 'local')");
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    cf_span secret = SP(NEWS_SECRET);
    cf_str signed_id = {0};
    CF_REQUIRE(cf_active_blob_sign(secret, 7, false, 0, &signed_id) == CF_OK);

    struct mpbuf m;
    mp_start(&m, "B");
    mp_adds(&m, "Content-Disposition: form-data; name=\"message[attachment]\""
                "\r\n\r\n");
    mp_add(&m, signed_id.ptr, signed_id.len);
    mp_adds(&m, "\r\n");
    mp_finish(&m, "B");
    cf_request req;
    cf_response resp;
    req_multipart(&req, CF_PATCH, "/rooms/100/messages/300", &m, cookie,
                  TURBO_ACCEPT);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    cf_response_dispose(&resp);
    free(m.p);
    cf_str_dispose(&signed_id);

    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT blob_id FROM active_storage_attachments WHERE "
                        "record_id=300 AND name='attachment'") == 7);
    /* The update's analyze job runs the tool-free analyzer (metadata merge,
     * attachment-record touch). */
    char text[256];
    CF_CHECK(one_text(env.scratch.db,
                      "SELECT metadata FROM active_storage_blobs WHERE id=7",
                      text, sizeof text) &&
             strcmp(text, "{\"identified\":true,\"analyzed\":true}") == 0);

    env_close(&env);
    storage_root_remove(root);
    g_storage_override = NULL;
}

CF_TEST(messages_attachment_over_cap_is_400_and_leaves_nothing) {
    char root[64];
    CF_REQUIRE(storage_root_make(root, sizeof root));
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_room_for(env.scratch.db, 100, GOLDEN_DAVID, "Rooms::Closed");
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    /* 16 MiB + 1 decoded bytes: the S01 staging cap rejects it. */
    size_t payload_len = (size_t)16 * 1024 * 1024 + 1;
    char *payload = malloc(payload_len);
    CF_REQUIRE(payload != NULL);
    memset(payload, 'x', payload_len);
    struct mpbuf m;
    mp_start(&m, "B");
    mp_field(&m, "message[body]", "too big");
    mp_next(&m, "B");
    mp_file(&m, "message[attachment]", "big.bin", "application/octet-stream",
            payload, payload_len);
    mp_finish(&m, "B");
    free(payload);

    cf_request req;
    cf_response resp;
    req_multipart(&req, CF_POST, "/rooms/100/messages", &m, cookie,
                  TURBO_ACCEPT);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 400);
    cf_response_dispose(&resp);
    free(m.p);
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM messages") == 0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM active_storage_blobs") == 0);
    char tmp_path[512];
    snprintf(tmp_path, sizeof tmp_path, "%s/tmp", root);
    CF_CHECK(dir_entry_count(tmp_path) == 0);

    env_close(&env);
    storage_root_remove(root);
    g_storage_override = NULL;
}

/* An image upload commits the rows, then the synchronous analyze arm stops at
 * the documented S03 boundary (no pinned vips): 500 with the message/blob/
 * attachment committed and the metadata unanalyzed, exactly the reference's
 * failure shape for an analyzer that raises. */
CF_TEST(messages_attachment_image_stops_at_s03) {
    char root[64];
    CF_REQUIRE(storage_root_make(root, sizeof root));
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_room_for(env.scratch.db, 100, GOLDEN_DAVID, "Rooms::Closed");
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    static const char png[] = "\x89PNG\r\n\x1a\n\x00\x00";
    struct mpbuf m;
    mp_start(&m, "B");
    mp_field(&m, "message[client_message_id]", "img-1");
    mp_next(&m, "B");
    mp_file(&m, "message[attachment]", "pic.png", "application/octet-stream",
            png, sizeof png - 1);
    mp_finish(&m, "B");
    cf_request req;
    cf_response resp;
    req_multipart(&req, CF_POST, "/rooms/100/messages", &m, cookie,
                  TURBO_ACCEPT);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    cf_response_dispose(&resp);
    free(m.p);

    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM messages") == 1);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM active_storage_blobs") == 1);
    char text[256];
    CF_CHECK(one_text(env.scratch.db,
                      "SELECT content_type FROM active_storage_blobs", text,
                      sizeof text) &&
             strcmp(text, "image/png") == 0);
    CF_CHECK(one_text(env.scratch.db,
                      "SELECT metadata FROM active_storage_blobs", text,
                      sizeof text) &&
             strcmp(text, "{\"identified\":true}") == 0);

    env_close(&env);
    storage_root_remove(root);
    g_storage_override = NULL;
}

CF_TEST(messages_create_delivers_webhooks_to_active_bots) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_room(env.scratch.db, 100, NULL, "Rooms::Direct", GOLDEN_DAVID,
              "2026-09-26 12:00:00.000000", "2026-09-26 12:00:00.000000");
    seed_membership(env.scratch.db, 1001, 100, GOLDEN_DAVID);
    seed_membership(env.scratch.db, 1002, 100, 500);
    seed_user(env.scratch.db, 500, "Bender", NULL, 2, 0,
              "2026-09-26 12:00:00.000000");
    exec_sql(env.scratch.db,
             "INSERT INTO webhooks (id, created_at, updated_at, url, user_id) "
             "VALUES (1, '2026-09-26 12:00:00.000000', "
             "'2026-09-26 12:00:00.000000', 'https://bot.example/hook', 500)");
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_form(&req, CF_POST, "/rooms/100/messages", "message[body]=hello",
             cookie, TURBO_ACCEPT);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    cf_response_dispose(&resp);
    CF_CHECK(env.event_count >= 1);
    CF_CHECK(env.events[0].kind == CF_EVENT_DELIVER_WEBHOOK);
    CF_CHECK(env.events[0].user_id == 500);
    CF_CHECK(env.events[0].message_id > 0);
    env_close(&env);
}

/* --- acceptance: update --------------------------------------------------------- */

CF_TEST(messages_update_redirects_and_writes) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_golden(&env);
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_form(&req, CF_PATCH, "/rooms/654632876/messages/933434491",
             "message[body]=%3Cp%3EEdited%3C%2Fp%3E", cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    CF_CHECK(head_contains(&resp, &req,
                           "Location: " ORIGIN
                           "/rooms/654632876/messages/933434491\r\n"));
    cf_response_dispose(&resp);

    char text[1024];
    CF_CHECK(one_text(env.scratch.db,
                      "SELECT body FROM action_text_rich_texts", text,
                      sizeof text) &&
             strcmp(text, "<p>Edited</p>") == 0);
    CF_CHECK(one_text(env.scratch.db,
                      "SELECT body FROM message_search_index", text,
                      sizeof text) &&
             strstr(text, "Edited") != NULL);
    CF_CHECK(one_text(env.scratch.db,
                      "SELECT updated_at FROM messages WHERE id=933434491",
                      text, sizeof text) &&
             strcmp(text, "2026-09-26 13:03:59.852991") != 0);
    env_close(&env);
}

/* update stores the same canonical_body form as create
 * (`update_body(tx, &canonicalize_body(..))`, messages.rs). */
CF_TEST(messages_update_stores_the_canonical_body) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_golden(&env);
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_form(&req, CF_PATCH, "/rooms/654632876/messages/933434491",
             "message[body]=%26amp%3B%20%3Ci%3Ex", cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    cf_response_dispose(&resp);

    char text[1024];
    CF_CHECK(one_text(env.scratch.db,
                      "SELECT body FROM action_text_rich_texts", text,
                      sizeof text) &&
             strcmp(text, "&amp; <i>x</i>") == 0);
    CF_CHECK(one_text(env.scratch.db,
                      "SELECT body FROM message_search_index", text,
                      sizeof text) &&
             strstr(text, "& x") != NULL);
    env_close(&env);
}

CF_TEST(messages_update_broadcasts_the_presentation) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_golden(&env);
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    msg_conn conn;
    CF_REQUIRE(conn_open(&env, GOLDEN_DAVID, &conn));
    CF_REQUIRE(conn_subscribe(&env, &conn, GOLDEN_ROOM));

    cf_request req;
    cf_response resp;
    req_form(&req, CF_PATCH, "/rooms/654632876/messages/933434491",
             "message[body]=%3Cp%3EEdited%20in%20place%3C%2Fp%3E", cookie,
             NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    cf_response_dispose(&resp);

    cf_message message = {0};
    CF_REQUIRE(cf_message_find(env.scratch.db, GOLDEN_MESSAGE, &message) ==
               CF_OK);
    cf_builder presentation = {0};
    CF_REQUIRE(render_presentation(&env, &message, &presentation) == CF_OK);
    cf_builder payload = {0};
    {
        cf_err rc = cf_builder_append(
            &payload,
            SP("<turbo-stream maintain_scroll=\"true\" action=\"replace\" "
               "target=\"presentation_message_rich\"><template>"));
        if (rc == CF_OK) {
            rc = cf_builder_append(
                &payload, (cf_span){presentation.ptr, presentation.len});
        }
        if (rc == CF_OK) {
            rc = cf_builder_append(&payload, SP("</template></turbo-stream>"));
        }
        CF_REQUIRE(rc == CF_OK);
    }
    CF_CHECK(conn_expect_payload(&conn, (cf_span){payload.ptr, payload.len},
                                 3000));
    cf_builder_dispose(&payload);
    cf_builder_dispose(&presentation);
    cf_message_dispose(&message);
    conn_close(&conn);
    env_close(&env);
}

/* `message[attachment]=` is the Attached::Changes::DeleteOne assignment: the
 * attachment row is removed and its blob is purged after commit. */
CF_TEST(messages_update_clears_an_attachment) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_golden(&env);
    exec_sql(env.scratch.db,
             "INSERT INTO active_storage_blobs (id, byte_size, checksum, "
             "content_type, created_at, filename, key, metadata, "
             "service_name) VALUES (1, 3, NULL, 'image/png', "
             "'2026-09-26 13:00:00.000000', 'x.png', 'k1', NULL, 'local')");
    exec_sql(env.scratch.db,
             "INSERT INTO active_storage_attachments (id, blob_id, "
             "created_at, name, record_id, record_type) VALUES "
             "(1, 1, '2026-09-26 13:00:00.000000', 'attachment', 933434491, "
             "'Message')");
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_form(&req, CF_PATCH, "/rooms/654632876/messages/933434491",
             "message[body]=%3Cp%3Ekept%3C%2Fp%3E&message[attachment]=",
             cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 302);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM active_storage_attachments") ==
             0);
    CF_CHECK(env.event_count >= 1);
    CF_CHECK(env.events[0].kind == CF_EVENT_PURGE_BLOB);
    CF_CHECK(env.events[0].blob_id == 1);
    env_close(&env);
}

CF_TEST(messages_update_json_is_missing_template) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_golden(&env);
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_form(&req, CF_PATCH, "/rooms/654632876/messages/933434491.json",
             "message[body]=x", cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 500);
    cf_response_dispose(&resp);
    env_close(&env);
}

CF_TEST(messages_update_forbidden_is_403) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_golden(&env);
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_JZ, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_form(&req, CF_PATCH, "/rooms/654632876/messages/933434491",
             "message[body]=x", cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    cf_response_dispose(&resp);
    char text[1024];
    CF_CHECK(one_text(env.scratch.db,
                      "SELECT body FROM action_text_rich_texts", text,
                      sizeof text) &&
             strcmp(text, GOLDEN_RICH_BODY) == 0);
    env_close(&env);
}

/* --- acceptance: destroy --------------------------------------------------------- */

CF_TEST(messages_destroy_matches_the_golden_and_removes) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_golden(&env);
    seed_message(env.scratch.db, GOLDEN_DESTROY_MESSAGE, "fresh-1",
                 GOLDEN_ROOM, GOLDEN_DAVID, "2026-09-26 13:04:06.407066",
                 "2026-09-26 13:04:06.410580");
    seed_rich_text(env.scratch.db, 2, GOLDEN_DESTROY_MESSAGE, "<div>x</div>",
                   "2026-09-26 13:04:06.407066");
    seed_fts(env.scratch.db, GOLDEN_DESTROY_MESSAGE, "x");
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    msg_conn conn;
    CF_REQUIRE(conn_open(&env, GOLDEN_DAVID, &conn));
    CF_REQUIRE(conn_subscribe(&env, &conn, GOLDEN_ROOM));

    cf_request req;
    cf_response resp;
    req_delete(&req, "/rooms/654632876/messages/933434495", cookie,
               TURBO_ACCEPT);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 200);
    CF_CHECK(head_contains(&resp, &req,
                           "Content-Type: text/vnd.turbo-stream.html; "
                           "charset=utf-8\r\n"));
    expect_golden("messages_destroy", &resp);
    cf_response_dispose(&resp);

    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM messages WHERE id=933434495") ==
             0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM action_text_rich_texts WHERE "
                        "record_id=933434495") == 0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM message_search_index WHERE "
                        "rowid=933434495") == 0);

    CF_CHECK(conn_expect_payload(
        &conn,
        SP("<turbo-stream action=\"remove\" "
           "target=\"message_fresh-1\"></turbo-stream>"),
        3000));
    conn_close(&conn);
    env_close(&env);
}

/* --- Rack method override (V01's UI form shapes) -------------------------- */

/* V01's message-edit form: POST + `_method=patch` must reach messages#update
 * and match the direct PATCH's response and write byte-for-byte. */
CF_TEST(messages_update_reached_via_post_method_override) {
    /* Direct PATCH. */
    msg_env direct_env;
    CF_REQUIRE(env_open(&direct_env));
    seed_golden(&direct_env);
    char direct_cookie[1024];
    make_session_cookie(&direct_env, GOLDEN_DAVID, direct_cookie,
                        sizeof direct_cookie);
    cf_request direct_req;
    cf_response direct_resp;
    req_form(&direct_req, CF_PATCH, "/rooms/654632876/messages/933434491",
             "message[body]=%3Cp%3EEdited%3C%2Fp%3E", direct_cookie, NULL);
    CF_REQUIRE(run_request(&direct_env, &direct_req, &direct_resp));
    CF_CHECK(direct_resp.status == 302);
    char direct_location[512];
    CF_REQUIRE(header_value(&direct_resp, &direct_req, "Location: ",
                            direct_location, sizeof direct_location));
    cf_span direct_body = {NULL, 0};
    if (direct_resp.body != NULL) direct_body = cf_buf_span(direct_resp.body);
    char direct_text[1024];
    CF_CHECK(one_text(direct_env.scratch.db,
                      "SELECT body FROM action_text_rich_texts", direct_text,
                      sizeof direct_text) &&
             strcmp(direct_text, "<p>Edited</p>") == 0);

    /* The form's shape: POST with the hidden `_method` field. */
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_golden(&env);
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);
    cf_request req;
    cf_response resp;
    req_form(&req, CF_POST, "/rooms/654632876/messages/933434491",
             "_method=patch&message[body]=%3Cp%3EEdited%3C%2Fp%3E", cookie,
             NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == direct_resp.status);
    char location[512];
    CF_REQUIRE(header_value(&resp, &req, "Location: ", location,
                            sizeof location));
    CF_CHECK(strcmp(location, direct_location) == 0);
    cf_span body = {NULL, 0};
    if (resp.body != NULL) body = cf_buf_span(resp.body);
    CF_CHECK(body.len == direct_body.len);
    CF_CHECK(body.len == 0 ||
             memcmp(body.ptr, direct_body.ptr, body.len) == 0);
    char text[1024];
    CF_CHECK(one_text(env.scratch.db, "SELECT body FROM action_text_rich_texts",
                      text, sizeof text) &&
             strcmp(text, "<p>Edited</p>") == 0);

    cf_response_dispose(&resp);
    cf_response_dispose(&direct_resp);
    env_close(&env);
    env_close(&direct_env);
}

/* V01's message-delete form: POST + `_method=delete` must reach
 * messages#destroy with the direct DELETE's turbo-stream bytes and remove the
 * same rows. */
CF_TEST(messages_destroy_reached_via_post_method_override) {
    /* Direct DELETE. */
    msg_env direct_env;
    CF_REQUIRE(env_open(&direct_env));
    seed_golden(&direct_env);
    seed_message(direct_env.scratch.db, GOLDEN_DESTROY_MESSAGE, "fresh-1",
                 GOLDEN_ROOM, GOLDEN_DAVID, "2026-09-26 13:04:06.407066",
                 "2026-09-26 13:04:06.410580");
    seed_rich_text(direct_env.scratch.db, 2, GOLDEN_DESTROY_MESSAGE,
                   "<div>x</div>", "2026-09-26 13:04:06.407066");
    seed_fts(direct_env.scratch.db, GOLDEN_DESTROY_MESSAGE, "x");
    char direct_cookie[1024];
    make_session_cookie(&direct_env, GOLDEN_DAVID, direct_cookie,
                        sizeof direct_cookie);
    cf_request direct_req;
    cf_response direct_resp;
    req_delete(&direct_req, "/rooms/654632876/messages/933434495",
               direct_cookie, TURBO_ACCEPT);
    CF_REQUIRE(run_request(&direct_env, &direct_req, &direct_resp));
    CF_CHECK(direct_resp.status == 200);
    CF_REQUIRE(direct_resp.body != NULL);
    cf_span direct_body = cf_buf_span(direct_resp.body);

    /* The form's shape: POST with the hidden `_method` field. */
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_golden(&env);
    seed_message(env.scratch.db, GOLDEN_DESTROY_MESSAGE, "fresh-1",
                 GOLDEN_ROOM, GOLDEN_DAVID, "2026-09-26 13:04:06.407066",
                 "2026-09-26 13:04:06.410580");
    seed_rich_text(env.scratch.db, 2, GOLDEN_DESTROY_MESSAGE, "<div>x</div>",
                   "2026-09-26 13:04:06.407066");
    seed_fts(env.scratch.db, GOLDEN_DESTROY_MESSAGE, "x");
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);
    cf_request req;
    cf_response resp;
    req_form(&req, CF_POST, "/rooms/654632876/messages/933434495",
             "_method=delete", cookie, TURBO_ACCEPT);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == direct_resp.status);
    CF_REQUIRE(resp.body != NULL);
    cf_span body = cf_buf_span(resp.body);
    CF_CHECK(body.len == direct_body.len);
    CF_CHECK(body.len == 0 ||
             memcmp(body.ptr, direct_body.ptr, body.len) == 0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM messages WHERE id=933434495") ==
             0);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM action_text_rich_texts WHERE "
                        "record_id=933434495") == 0);

    cf_response_dispose(&resp);
    cf_response_dispose(&direct_resp);
    env_close(&env);
    env_close(&direct_env);
}

CF_TEST(messages_destroy_unacceptable_format_is_406_after_destroy) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_golden(&env);
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_DAVID, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_delete(&req, "/rooms/654632876/messages/933434491", cookie, NULL);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 406);
    cf_response_dispose(&resp);
    /* The source destroys before respond_to. */
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM messages WHERE id=933434491") ==
             0);
    env_close(&env);
}

CF_TEST(messages_destroy_forbidden_is_403) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_golden(&env);
    char cookie[1024];
    make_session_cookie(&env, GOLDEN_JZ, cookie, sizeof cookie);

    cf_request req;
    cf_response resp;
    req_delete(&req, "/rooms/654632876/messages/933434491", cookie,
               TURBO_ACCEPT);
    CF_REQUIRE(run_request(&env, &req, &resp));
    CF_CHECK(resp.status == 403);
    cf_response_dispose(&resp);
    CF_CHECK(count_rows(env.scratch.db,
                        "SELECT count(*) FROM messages WHERE id=933434491") ==
             1);
    env_close(&env);
}

/* --- D02 writer revalidation ------------------------------------------------------ */

CF_TEST(messages_create_write_revalidates_the_membership) {
    msg_env env;
    CF_REQUIRE(env_open(&env));
    seed_base(&env);
    seed_room_for(env.scratch.db, 100, GOLDEN_DAVID, "Rooms::Closed");
    /* The membership disappears after set_room but before the write. */
    exec_sql(env.scratch.db,
             "DELETE FROM memberships WHERE room_id=100 AND user_id=127326141");

    struct msg_create_write write;
    memset(&write, 0, sizeof write);
    write.room_id = 100;
    write.creator_id = GOLDEN_DAVID;
    write.attributes.room_id = 100;
    write.attributes.creator_id = GOLDEN_DAVID;
    cf_str body = {(char *)(uintptr_t)"<p>x</p>", 8};
    write.attributes.body.present = true;
    write.attributes.body.value = body;
    cf_err rc = cf_write(env.app, msg_create_write_cb, &write);
    cf_message_dispose(&write.message);
    CF_CHECK(rc == CF_NOT_FOUND);
    CF_CHECK(count_rows(env.scratch.db, "SELECT count(*) FROM messages") == 0);
    env_close(&env);
}

CF_TEST_MAIN()
