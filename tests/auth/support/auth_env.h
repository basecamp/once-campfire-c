/* tests/auth/support/auth_env.h — A01 test harness: a scratch database, the
 * D02 writer, a reader connection, the H03 route double (tests/app/support/
 * route_double.c is linked into the binary) and small seeding helpers.
 * Test-only; never part of the application.
 */
#ifndef CF_TESTS_AUTH_ENV_H
#define CF_TESTS_AUTH_ENV_H

#include "app.h"
#include "auth.h"
#include "cf.h"
#include "config.h"
#include "context.h"
#include "core/testclock.h"
#include "db/db_internal.h"
#include "db/db_testutil.h"
#include "db/writer.h"

#include "../../app/support/route_double.h"

#include "../vectors.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUTH_TEST_MAX_EVENTS 64

typedef struct {
    cf_db_scratch scratch;
    cf_app *app;
    cf_db *reader;
    cf_event events[AUTH_TEST_MAX_EVENTS];
    size_t event_count;
} auth_env;

static inline cf_err auth_env_capture(void *ctx, const cf_event *event) {
    auth_env *env = ctx;
    if (env->event_count < AUTH_TEST_MAX_EVENTS) {
        env->events[env->event_count++] = *event;
    }
    return CF_OK;
}

/* ssl_enabled selects the CSRF force-SSL configuration (DISABLE_SSL=0 with
 * the certificate paths the config requires; no listener here). */
static inline bool auth_env_open_opts(auth_env *env, bool ssl_enabled) {
    memset(env, 0, sizeof *env);
    if (!cf_db_scratch_open(&env->scratch)) return false;
    cf_config_entry entries[6] = {
        {"PUBLIC_ORIGIN", "http://127.0.0.1:32123"},
        {"SECRET_KEY_BASE", AUTH_VEC_SECRET_KEY_BASE},
        {"DATABASE_PATH", env->scratch.path},
    };
    size_t entry_count = 3;
    if (ssl_enabled) {
        entries[entry_count++] = (cf_config_entry){"DISABLE_SSL", "0"};
        entries[entry_count++] = (cf_config_entry){"TLS_CERT_FILE", "t.pem"};
        entries[entry_count++] = (cf_config_entry){"TLS_KEY_FILE", "t.key"};
    }
    cf_config *config = NULL;
    if (cf_config_parse(entries, entry_count, NULL, &config) != CF_OK) {
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
    if (cf_writer_set_control_handler(env->app, auth_env_capture, env) !=
        CF_OK) {
        return false;
    }
    if (cf_db_open(env->scratch.path, true, &env->reader) != CF_OK) {
        return false;
    }
    cf_test_routes_reset();
    if (cf_test_routes_add("GET", "/", 7, NULL) != CF_OK ||
        cf_test_routes_add("GET", "/session/new", 12, NULL) != CF_OK ||
        cf_test_routes_add("POST", "/session", 18, NULL) != CF_OK ||
        cf_test_routes_add("GET", "/*path", 1, NULL) != CF_OK ||
        cf_test_routes_add("POST", "/*path", 2, NULL) != CF_OK ||
        cf_test_routes_add("PUT", "/*path", 3, NULL) != CF_OK ||
        cf_test_routes_add("PATCH", "/*path", 4, NULL) != CF_OK ||
        cf_test_routes_add("DELETE", "/*path", 5, NULL) != CF_OK ||
        cf_test_routes_add("OPTIONS", "/*path", 6, NULL) != CF_OK) {
        return false;
    }
    return true;
}

static inline bool auth_env_open(auth_env *env) {
    return auth_env_open_opts(env, false);
}

static inline void auth_env_close(auth_env *env) {
    if (env->reader != NULL) cf_db_close(env->reader);
    cf_writer_stop(env->app);
    cf_app_destroy(env->app);
    cf_db_scratch_close(&env->scratch);
}

static inline bool auth_exec(auth_env *env, const char *sql) {
    return cf_db_test_exec(cf_db_handle(env->scratch.db), sql) == SQLITE_OK;
}

/* Insert one user row; returns the new id or 0. */
static inline int64_t auth_seed_user(auth_env *env, const char *name,
                                     const char *email, const char *digest,
                                     int role, int status) {
    sqlite3 *handle = cf_db_handle(env->scratch.db);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(
            handle,
            "INSERT INTO users (bio,bot_token,created_at,email_address,name,"
            "password_digest,role,status,updated_at) VALUES "
            "(NULL,NULL,'2026-01-02 03:04:05',?,?,?,?,?,"
            "'2026-01-02 03:04:05')",
            -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    if (email != NULL) {
        sqlite3_bind_text(stmt, 1, email, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 1);
    }
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, digest, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, role);
    sqlite3_bind_int(stmt, 5, status);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return 0;
    return sqlite3_last_insert_rowid(handle);
}

static inline int64_t auth_exec_rowid(auth_env *env, const char *sql) {
    bool ok = false;
    int64_t id = cf_db_test_i64(cf_db_handle(env->scratch.db), sql, &ok);
    return ok ? id : 0;
}

static inline int64_t auth_seed_bot(auth_env *env, const char *name,
                                    const char *bot_token) {
    sqlite3 *handle = cf_db_handle(env->scratch.db);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(
            handle,
            "INSERT INTO users (created_at,email_address,name,"
            "password_digest,role,status,updated_at,bot_token) VALUES "
            "('2026-01-02 03:04:05',NULL,?,NULL,2,0,'2026-01-02 03:04:05',?)",
            -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, bot_token, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return 0;
    return sqlite3_last_insert_rowid(handle);
}

static inline void auth_seed_room_membership(auth_env *env, int64_t room_id,
                                             int64_t user_id,
                                             const char *room_type) {
    sqlite3 *handle = cf_db_handle(env->scratch.db);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(
            handle,
            "INSERT INTO rooms (id,created_at,creator_id,name,type,updated_at)"
            " VALUES (?,'2026-01-02 03:04:05',?,'R',?,'2026-01-02 03:04:05')",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, room_id);
        sqlite3_bind_int64(stmt, 2, user_id);
        sqlite3_bind_text(stmt, 3, room_type, -1, SQLITE_TRANSIENT);
        (void)sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    if (sqlite3_prepare_v2(
            handle,
            "INSERT INTO memberships (created_at,involvement,room_id,user_id,"
            "updated_at) VALUES ('2026-01-02 03:04:05','mentions',?,?,"
            "'2026-01-02 03:04:05')",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, room_id);
        sqlite3_bind_int64(stmt, 2, user_id);
        (void)sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

/* Build a context around a request/response pair. */
static inline cf_err auth_env_ctx(auth_env *env, cf_request *request,
                                  cf_response *response, cf_ctx *ctx) {
    cf_response_init(response);
    return cf_ctx_create(ctx, env->app, env->reader, request, response);
}

static inline bool auth_span_is(cf_span span, const char *text) {
    size_t len = strlen(text);
    return span.len == len && memcmp(span.ptr, text, len) == 0;
}

static inline bool auth_str_is(cf_str value, const char *text) {
    size_t len = strlen(text);
    return value.ptr != NULL && value.len == len &&
           memcmp(value.ptr, text, len) == 0;
}

#endif /* CF_TESTS_AUTH_ENV_H */
