/* D02 writer/transaction acceptance tests (07-verification.md DB-02/DB-06,
 * 02-data-auth.md D02, 06-cache-performance.md version rules).
 *
 * Compiled against the D01 db helpers and the D02 writer; the queue-boundary
 * cases fill the writer queue with real blocked callers so CF_BUSY is proven
 * to occur before callback execution.  No case silently skips: setup failure
 * is a CF_REQUIRE failure.
 */
#include "cf_test.h"

#include "app.h"
#include "config.h"
#include "db/db_testutil.h"
#include "db/writer.h"

#include <pthread.h>
#include <time.h>

static const char CF_TEST_HEX64[] =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

/* --- fixture: scratch db + app + started writer -------------------------- */

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    bool writer_started;
} writer_fixture;

static cf_config *make_config(const char *db_path, const char *queue) {
    cf_config_entry entries[4] = {
        {"PUBLIC_ORIGIN", "http://127.0.0.1:3999"},
        {"SECRET_KEY_BASE", CF_TEST_HEX64},
        {"DATABASE_PATH", db_path},
        {"CF_WRITER_QUEUE", queue},
    };
    cf_config *config = NULL;
    cf_config_error err;
    if (cf_config_parse(entries, 4, &err, &config) != CF_OK) return NULL;
    return config;
}

static bool fixture_open(writer_fixture *f, const char *queue) {
    memset(f, 0, sizeof *f);
    if (!cf_db_scratch_open(&f->scratch)) return false;
    f->config = make_config(f->scratch.path, queue);
    if (f->config == NULL) return false;
    if (cf_app_create(f->config, &f->app) != CF_OK) return false;
    if (cf_writer_start(f->app, f->config) != CF_OK) return false;
    f->writer_started = true;
    return true;
}

static void fixture_close(writer_fixture *f) {
    if (f->writer_started) cf_writer_stop(f->app);
    if (f->app != NULL) cf_app_destroy(f->app);
    cf_db_scratch_close(&f->scratch);
}

/* One seeded user and room so callbacks can insert FK-valid rows. */
static bool fixture_seed(writer_fixture *f) {
    sqlite3 *h = cf_db_handle(f->scratch.db);
    int rc = cf_db_test_exec(
        h,
        "INSERT INTO users (id,name,created_at,updated_at,role,status) "
        "VALUES (1,'Seed','2026-01-01 00:00:00','2026-01-01 00:00:00',0,0);"
        "INSERT INTO rooms (id,creator_id,type,created_at,updated_at) "
        "VALUES (1,1,'Rooms::Open','2026-01-01 00:00:00',"
        "'2026-01-01 00:00:00');");
    return rc == SQLITE_OK;
}

static int64_t message_count(writer_fixture *f) {
    bool ok = false;
    int64_t n = cf_db_test_i64(cf_db_handle(f->scratch.db),
                               "SELECT count(*) FROM messages", &ok);
    CF_REQUIRE(ok);
    return n;
}

/* --- statement set used by callbacks (D01 helper API) -------------------- */

enum { CF_TEST_STMT_INSERT_MESSAGE, CF_TEST_STMT_COUNT };

static const cf_stmt_def cf_test_stmts[] = {
    {"INSERT INTO messages (client_message_id, created_at, creator_id, "
     "room_id, updated_at) VALUES (?1, '2026-01-01 00:00:00', ?2, ?3, "
     "'2026-01-01 00:00:00')"},
};
static const cf_stmt_set cf_test_stmt_set = {cf_test_stmts,
                                             CF_TEST_STMT_COUNT};

static cf_err insert_message(cf_tx *tx, int64_t room_id, int64_t creator_id,
                             const char *client_id) {
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;
    sqlite3_stmt *stmt = NULL;
    cf_err rc = cf_db_stmt(db, &cf_test_stmt_set, CF_TEST_STMT_INSERT_MESSAGE,
                           &stmt);
    if (rc != CF_OK) return rc;
    rc = cf_stmt_bind_text(
        stmt, 1, (cf_span){(const unsigned char *)client_id,
                           strlen(client_id)});
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 2, creator_id);
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 3, room_id);
    if (rc == CF_OK) {
        rc = sqlite3_step(stmt) == SQLITE_DONE ? CF_OK : CF_DB;
    }
    cf_db_stmt_done(stmt);
    return rc;
}

/* --- event recorder shared by handler-driven cases ----------------------- */

typedef struct {
    pthread_mutex_t mu; /* handlers run on the writer thread */
    cf_event events[16];
    size_t count;
} event_log;

static void event_log_init(event_log *log) {
    memset(log, 0, sizeof *log);
    pthread_mutex_init(&log->mu, NULL);
}

static void event_log_dispose(event_log *log) {
    pthread_mutex_destroy(&log->mu);
}

static void event_log_push(event_log *log, const cf_event *event) {
    pthread_mutex_lock(&log->mu);
    if (log->count < 16) log->events[log->count++] = *event;
    pthread_mutex_unlock(&log->mu);
}

static size_t event_log_count(event_log *log) {
    pthread_mutex_lock(&log->mu);
    size_t n = log->count;
    pthread_mutex_unlock(&log->mu);
    return n;
}

static cf_event event_log_at(event_log *log, size_t i) {
    pthread_mutex_lock(&log->mu);
    cf_event ev = i < log->count ? log->events[i] : (cf_event){0};
    pthread_mutex_unlock(&log->mu);
    return ev;
}

static cf_err log_control(void *ctx, const cf_event *event) {
    event_log_push(ctx, event);
    return CF_OK;
}

static cf_err log_event(void *ctx, const cf_event *event) {
    event_log_push(ctx, event);
    return CF_OK;
}

/* --- simple call states and callbacks ------------------------------------ */

typedef struct {
    cf_app *app;
    cf_err result;
    int calls;
} call_state;

static cf_err cb_insert_message(cf_tx *tx, void *arg) {
    call_state *st = arg;
    st->calls++;
    return insert_message(tx, 1, 1, "client-1");
}

/* DB-02: insert + events, then fail; everything must roll back. */
static cf_err cb_fail_after_effects(cf_tx *tx, void *arg) {
    call_state *st = arg;
    st->calls++;
    cf_err rc = insert_message(tx, 1, 1, "rolled-back");
    if (rc != CF_OK) return rc;
    cf_event push = {.kind = CF_EVENT_PUSH_MESSAGE,
                     .user_id = 777, /* unused: must be zeroed */
                     .room_id = 1,
                     .message_id = 1};
    rc = cf_tx_event(tx, push);
    if (rc != CF_OK) return rc;
    cf_event disconnect = {.kind = CF_EVENT_DISCONNECT_USER,
                           .user_id = 5,
                           .room_id = 999 /* unused */,
                           .reconnect = false};
    rc = cf_tx_event(tx, disconnect);
    if (rc != CF_OK) return rc;
    return CF_INVALID; /* callback failure */
}

/* Commit must fail: deferred FK violation is checked at COMMIT. */
static cf_err cb_deferred_fk_failure(cf_tx *tx, void *arg) {
    call_state *st = arg;
    st->calls++;
    sqlite3 *h = cf_db_handle(cf_tx_db(tx));
    if (sqlite3_exec(h, "PRAGMA defer_foreign_keys=ON", NULL, NULL, NULL) !=
        SQLITE_OK) {
        return CF_DB;
    }
    cf_err rc = insert_message(tx, 424242, 424242, "fk-violation");
    if (rc != CF_OK) return rc;
    cf_event push = {.kind = CF_EVENT_PUSH_MESSAGE,
                     .room_id = 1,
                     .message_id = 1};
    return cf_tx_event(tx, push);
}

/* --- version advance ----------------------------------------------------- */

CF_TEST(writer_commit_advances_version_and_stats) {
    writer_fixture f;
    CF_REQUIRE(fixture_open(&f, "8"));
    CF_REQUIRE(fixture_seed(&f));

    uint64_t start = cf_data_version(f.app);
    CF_CHECK(start == 1);

    call_state st = {.app = f.app, .calls = 0};
    CF_REQUIRE(cf_write(f.app, cb_insert_message, &st) == CF_OK);
    CF_CHECK(st.calls == 1);
    CF_CHECK(cf_data_version(f.app) == start + 1);
    CF_CHECK(message_count(&f) == 1);

    cf_writer_stats stats;
    CF_REQUIRE(cf_writer_stats_get(f.app, &stats) == CF_OK);
    CF_CHECK(stats.admitted == 1);
    CF_CHECK(stats.committed == 1);
    CF_CHECK(stats.rolled_back == 0);
    CF_CHECK(stats.queue_capacity == 8);
    CF_CHECK(stats.pending == 0);
    CF_CHECK(stats.callback_running == false);
    fixture_close(&f);
}

typedef struct {
    cf_app *app;
    uint64_t observed;
} version_watch;

static void *version_watch_thread(void *p) {
    version_watch *watch = p;
    for (int i = 0; i < 20000; i++) {
        uint64_t v = cf_data_version(watch->app);
        if (v > 1) {
            watch->observed = v;
            return NULL;
        }
        struct timespec ts = {0, 1000000}; /* 1 ms */
        nanosleep(&ts, NULL);
    }
    watch->observed = cf_data_version(watch->app);
    return NULL;
}

CF_TEST(writer_version_advance_is_visible_to_other_threads) {
    writer_fixture f;
    CF_REQUIRE(fixture_open(&f, "8"));
    CF_REQUIRE(fixture_seed(&f));

    version_watch watch = {.app = f.app, .observed = 0};
    pthread_t watcher;
    CF_REQUIRE(pthread_create(&watcher, NULL, version_watch_thread, &watch) ==
               0);
    struct timespec ts = {0, 2000000};
    nanosleep(&ts, NULL); /* let the watcher spin on the old version */

    call_state st = {.app = f.app, .calls = 0};
    CF_REQUIRE(cf_write(f.app, cb_insert_message, &st) == CF_OK);
    CF_REQUIRE(pthread_join(watcher, NULL) == 0);
    CF_CHECK(watch.observed == 2); /* release/acquire observed the advance */
    CF_CHECK(cf_data_version(f.app) == 2);
    fixture_close(&f);
}

/* --- DB-02: failed callback rolls back rows and events, no delivery ------ */

CF_TEST(writer_rollback_discards_rows_events_version_and_delivery) {
    writer_fixture f;
    CF_REQUIRE(fixture_open(&f, "8"));
    CF_REQUIRE(fixture_seed(&f));

    event_log log;
    event_log_init(&log);
    CF_REQUIRE(cf_writer_set_control_handler(f.app, log_control, &log) ==
               CF_OK);
    CF_REQUIRE(cf_writer_set_event_handler(f.app, CF_EVENT_PUSH_MESSAGE,
                                           log_event, &log) == CF_OK);

    uint64_t start = cf_data_version(f.app);
    call_state st = {.app = f.app, .calls = 0};
    CF_CHECK(cf_write(f.app, cb_fail_after_effects, &st) == CF_INVALID);
    CF_CHECK(st.calls == 1);
    CF_CHECK(message_count(&f) == 0);          /* rows rolled back */
    CF_CHECK(cf_data_version(f.app) == start); /* no version advance */
    CF_CHECK(event_log_count(&log) == 0);      /* no delivery */

    cf_writer_stats stats;
    CF_REQUIRE(cf_writer_stats_get(f.app, &stats) == CF_OK);
    CF_CHECK(stats.rolled_back == 1);
    CF_CHECK(stats.committed == 0);
    CF_CHECK(stats.mandatory_failures == 0);
    for (size_t i = 0; i < CF_WRITER_EVENT_KINDS; i++) {
        CF_CHECK(stats.best_effort_dropped[i] == 0);
    }

    /* The writer connection is still usable after the rollback. */
    call_state ok = {.app = f.app, .calls = 0};
    CF_CHECK(cf_write(f.app, cb_insert_message, &ok) == CF_OK);
    CF_CHECK(message_count(&f) == 1);
    CF_CHECK(cf_data_version(f.app) == start + 1);
    event_log_dispose(&log);
    fixture_close(&f);
}

/* --- COMMIT failure discards effects and returns CF_DB ------------------- */

CF_TEST(writer_commit_failure_returns_cf_db_and_rolls_back) {
    writer_fixture f;
    CF_REQUIRE(fixture_open(&f, "8"));
    CF_REQUIRE(fixture_seed(&f));

    event_log log;
    event_log_init(&log);
    CF_REQUIRE(cf_writer_set_event_handler(f.app, CF_EVENT_PUSH_MESSAGE,
                                           log_event, &log) == CF_OK);

    uint64_t start = cf_data_version(f.app);
    call_state st = {.app = f.app, .calls = 0};
    CF_CHECK(cf_write(f.app, cb_deferred_fk_failure, &st) == CF_DB);
    CF_CHECK(st.calls == 1);
    CF_CHECK(message_count(&f) == 0);
    CF_CHECK(cf_data_version(f.app) == start);
    CF_CHECK(event_log_count(&log) == 0);

    cf_writer_stats stats;
    CF_REQUIRE(cf_writer_stats_get(f.app, &stats) == CF_OK);
    CF_CHECK(stats.commit_failures == 1);
    CF_CHECK(stats.rolled_back == 1);
    CF_CHECK(stats.committed == 0);

    /* The connection survives and a later write commits. */
    call_state ok = {.app = f.app, .calls = 0};
    CF_CHECK(cf_write(f.app, cb_insert_message, &ok) == CF_OK);
    CF_CHECK(message_count(&f) == 1);
    event_log_dispose(&log);
    fixture_close(&f);
}

/* --- event ordering, unused-field zeroing, handoff after commit ---------- */

static cf_err cb_five_events(cf_tx *tx, void *arg) {
    call_state *st = arg;
    st->calls++;
    cf_err rc = insert_message(tx, 1, 1, "ordered");
    if (rc != CF_OK) return rc;
    /* Deliberately not in enum order; junk in every unused field. */
    cf_event push = {.kind = CF_EVENT_PUSH_MESSAGE, .user_id = 111,
                     .room_id = 21, .message_id = 22, .blob_id = 222,
                     .reconnect = true};
    cf_event hook = {.kind = CF_EVENT_DELIVER_WEBHOOK, .user_id = 31,
                     .room_id = 333, .message_id = 32, .blob_id = 444,
                     .reconnect = true};
    cf_event disc = {.kind = CF_EVENT_DISCONNECT_USER, .user_id = 11,
                     .room_id = 555, .message_id = 666, .blob_id = 777,
                     .reconnect = true};
    cf_event purge = {.kind = CF_EVENT_PURGE_BLOB, .user_id = 888,
                      .room_id = 999, .message_id = 1010, .blob_id = 51,
                      .reconnect = true};
    cf_event banned = {.kind = CF_EVENT_REMOVE_BANNED_CONTENT, .user_id = 41,
                       .room_id = 1212, .message_id = 1313, .blob_id = 1414,
                       .reconnect = true};
    if (cf_tx_event(tx, push) != CF_OK) return CF_DB;
    if (cf_tx_event(tx, hook) != CF_OK) return CF_DB;
    if (cf_tx_event(tx, disc) != CF_OK) return CF_DB;
    if (cf_tx_event(tx, purge) != CF_OK) return CF_DB;
    if (cf_tx_event(tx, banned) != CF_OK) return CF_DB;
    return CF_OK;
}

CF_TEST(writer_hands_off_events_in_writer_order_with_zeroed_unused_fields) {
    writer_fixture f;
    CF_REQUIRE(fixture_open(&f, "8"));
    CF_REQUIRE(fixture_seed(&f));

    event_log log;
    event_log_init(&log);
    CF_REQUIRE(cf_writer_set_control_handler(f.app, log_control, &log) ==
               CF_OK);
    CF_REQUIRE(cf_writer_set_event_handler(f.app, CF_EVENT_PUSH_MESSAGE,
                                           log_event, &log) == CF_OK);
    CF_REQUIRE(cf_writer_set_event_handler(f.app,
                                           CF_EVENT_REMOVE_BANNED_CONTENT,
                                           log_event, &log) == CF_OK);
    CF_REQUIRE(cf_writer_set_event_handler(f.app, CF_EVENT_DELIVER_WEBHOOK,
                                           log_event, &log) == CF_OK);
    CF_REQUIRE(cf_writer_set_event_handler(f.app, CF_EVENT_PURGE_BLOB,
                                           log_event, &log) == CF_OK);

    uint64_t start = cf_data_version(f.app);
    call_state st = {.app = f.app, .calls = 0};
    CF_REQUIRE(cf_write(f.app, cb_five_events, &st) == CF_OK);
    CF_REQUIRE(event_log_count(&log) == 5);
    CF_CHECK(cf_data_version(f.app) == start + 1);

    cf_event e0 = event_log_at(&log, 0);
    CF_CHECK(e0.kind == CF_EVENT_PUSH_MESSAGE);
    CF_CHECK(e0.room_id == 21 && e0.message_id == 22);
    CF_CHECK(e0.user_id == 0 && e0.blob_id == 0 && e0.reconnect == false);

    cf_event e1 = event_log_at(&log, 1);
    CF_CHECK(e1.kind == CF_EVENT_DELIVER_WEBHOOK);
    CF_CHECK(e1.user_id == 31 && e1.message_id == 32);
    CF_CHECK(e1.room_id == 0 && e1.blob_id == 0 && e1.reconnect == false);

    cf_event e2 = event_log_at(&log, 2);
    CF_CHECK(e2.kind == CF_EVENT_DISCONNECT_USER);
    CF_CHECK(e2.user_id == 11 && e2.reconnect == true);
    CF_CHECK(e2.room_id == 0 && e2.message_id == 0 && e2.blob_id == 0);

    cf_event e3 = event_log_at(&log, 3);
    CF_CHECK(e3.kind == CF_EVENT_PURGE_BLOB && e3.blob_id == 51);
    CF_CHECK(e3.user_id == 0 && e3.room_id == 0 && e3.message_id == 0 &&
             e3.reconnect == false);

    cf_event e4 = event_log_at(&log, 4);
    CF_CHECK(e4.kind == CF_EVENT_REMOVE_BANNED_CONTENT && e4.user_id == 41);
    CF_CHECK(e4.room_id == 0 && e4.message_id == 0 && e4.blob_id == 0 &&
             e4.reconnect == false);

    cf_writer_stats stats;
    CF_REQUIRE(cf_writer_stats_get(f.app, &stats) == CF_OK);
    CF_CHECK(stats.mandatory_failures == 0);
    for (size_t i = 0; i < CF_WRITER_EVENT_KINDS; i++) {
        CF_CHECK(stats.best_effort_dropped[i] == 0);
    }
    event_log_dispose(&log);
    fixture_close(&f);
}

/* --- repeated disconnects coalesce without upgrading reconnect ----------- */

static cf_err cb_coalesce(cf_tx *tx, void *arg) {
    call_state *st = arg;
    st->calls++;
    cf_event first = {.kind = CF_EVENT_DISCONNECT_USER, .user_id = 7,
                      .room_id = 3, .message_id = 4, .blob_id = 5,
                      .reconnect = false};
    cf_event second = {.kind = CF_EVENT_DISCONNECT_USER, .user_id = 7,
                       .reconnect = true};
    cf_event third = {.kind = CF_EVENT_DISCONNECT_USER, .user_id = 8,
                      .reconnect = true};
    cf_event fourth = {.kind = CF_EVENT_DISCONNECT_USER, .user_id = 8,
                       .reconnect = true};
    if (cf_tx_event(tx, first) != CF_OK) return CF_DB;
    if (cf_tx_event(tx, second) != CF_OK) return CF_DB;
    if (cf_tx_event(tx, third) != CF_OK) return CF_DB;
    if (cf_tx_event(tx, fourth) != CF_OK) return CF_DB;
    return CF_OK;
}

CF_TEST(writer_disconnect_coalescing_keeps_false_and_second_user) {
    writer_fixture f;
    CF_REQUIRE(fixture_open(&f, "8"));

    event_log log;
    event_log_init(&log);
    CF_REQUIRE(cf_writer_set_control_handler(f.app, log_control, &log) ==
               CF_OK);

    call_state st = {.app = f.app, .calls = 0};
    CF_REQUIRE(cf_write(f.app, cb_coalesce, &st) == CF_OK);
    CF_REQUIRE(event_log_count(&log) == 2);
    cf_event e0 = event_log_at(&log, 0);
    CF_CHECK(e0.user_id == 7);
    CF_CHECK(e0.reconnect == false); /* false never upgraded to true */
    CF_CHECK(e0.room_id == 0 && e0.message_id == 0 && e0.blob_id == 0);
    cf_event e1 = event_log_at(&log, 1);
    CF_CHECK(e1.user_id == 8 && e1.reconnect == true);
    event_log_dispose(&log);
    fixture_close(&f);
}

/* --- best-effort drops are counted per kind and never fail the write ----- */

static int g_push_calls;
static cf_err push_refusing(void *ctx, const cf_event *event) {
    (void)ctx;
    (void)event;
    g_push_calls++;
    return CF_BUSY;
}

static cf_err cb_mixed(cf_tx *tx, void *arg) {
    call_state *st = arg;
    st->calls++;
    cf_event push1 = {.kind = CF_EVENT_PUSH_MESSAGE, .room_id = 1,
                      .message_id = 1};
    cf_event push2 = {.kind = CF_EVENT_PUSH_MESSAGE, .room_id = 1,
                      .message_id = 2};
    cf_event hook = {.kind = CF_EVENT_DELIVER_WEBHOOK, .user_id = 1,
                     .message_id = 1};
    cf_event banned = {.kind = CF_EVENT_REMOVE_BANNED_CONTENT, .user_id = 1};
    cf_event purge = {.kind = CF_EVENT_PURGE_BLOB, .blob_id = 1};
    if (cf_tx_event(tx, push1) != CF_OK) return CF_DB;
    if (cf_tx_event(tx, push2) != CF_OK) return CF_DB;
    if (cf_tx_event(tx, hook) != CF_OK) return CF_DB;
    if (cf_tx_event(tx, banned) != CF_OK) return CF_DB;
    if (cf_tx_event(tx, purge) != CF_OK) return CF_DB;
    return CF_OK;
}

CF_TEST(writer_best_effort_events_drop_and_count_without_failing_write) {
    writer_fixture f;
    CF_REQUIRE(fixture_open(&f, "8"));

    uint64_t start = cf_data_version(f.app);

    /* No consumers registered: every best-effort event drops and counts. */
    call_state st = {.app = f.app, .calls = 0};
    CF_REQUIRE(cf_write(f.app, cb_mixed, &st) == CF_OK);
    cf_writer_stats stats;
    CF_REQUIRE(cf_writer_stats_get(f.app, &stats) == CF_OK);
    CF_CHECK(stats.best_effort_dropped[CF_EVENT_PUSH_MESSAGE] == 2);
    CF_CHECK(stats.best_effort_dropped[CF_EVENT_DELIVER_WEBHOOK] == 1);
    CF_CHECK(stats.best_effort_dropped[CF_EVENT_REMOVE_BANNED_CONTENT] == 1);
    CF_CHECK(stats.best_effort_dropped[CF_EVENT_PURGE_BLOB] == 1);
    CF_CHECK(stats.best_effort_dropped[CF_EVENT_DISCONNECT_USER] == 0);
    CF_CHECK(stats.mandatory_failures == 0);
    CF_CHECK(cf_data_version(f.app) == start + 1);

    /* A registered consumer that refuses still drops and counts, and the
     * committed write keeps its CF_OK outcome (JOB-01 policy). */
    g_push_calls = 0;
    CF_REQUIRE(cf_writer_set_event_handler(f.app, CF_EVENT_PUSH_MESSAGE,
                                           push_refusing, NULL) == CF_OK);
    call_state st2 = {.app = f.app, .calls = 0};
    CF_REQUIRE(cf_write(f.app, cb_mixed, &st2) == CF_OK);
    CF_REQUIRE(cf_writer_stats_get(f.app, &stats) == CF_OK);
    CF_CHECK(stats.best_effort_dropped[CF_EVENT_PUSH_MESSAGE] == 4);
    CF_CHECK(g_push_calls == 2); /* attempted, then counted as dropped */
    CF_CHECK(cf_data_version(f.app) == start + 2);

    fixture_close(&f);
}

/* --- DB-06: mandatory revocation unavailable -> CF_INTERNAL after commit - */

static cf_err cb_disconnect_user(cf_tx *tx, void *arg) {
    call_state *st = arg;
    st->calls++;
    cf_err rc = insert_message(tx, 1, 1, "committed");
    if (rc != CF_OK) return rc;
    cf_event disc = {.kind = CF_EVENT_DISCONNECT_USER, .user_id = 9,
                     .reconnect = false};
    return cf_tx_event(tx, disc);
}

static cf_err control_refusing(void *ctx, const cf_event *event) {
    (void)ctx;
    (void)event;
    return CF_DB;
}

CF_TEST(writer_post_commit_mandatory_failure_is_internal_not_retryable) {
    writer_fixture f;
    CF_REQUIRE(fixture_open(&f, "8"));
    CF_REQUIRE(fixture_seed(&f));

    uint64_t start = cf_data_version(f.app);

    /* No control consumer yet: committed data stays, result is CF_INTERNAL. */
    call_state st = {.app = f.app, .calls = 0};
    cf_err rc = cf_write(f.app, cb_disconnect_user, &st);
    CF_CHECK(rc == CF_INTERNAL);
    CF_CHECK(rc != CF_BUSY); /* never labeled retryable */
    CF_CHECK(rc != CF_DB);   /* never a pretend commit failure */
    CF_CHECK(message_count(&f) == 1); /* no pretend rollback */
    CF_CHECK(cf_data_version(f.app) == start + 1);

    cf_writer_stats stats;
    CF_REQUIRE(cf_writer_stats_get(f.app, &stats) == CF_OK);
    CF_CHECK(stats.committed == 1);
    CF_CHECK(stats.mandatory_failures == 1);

    /* A refusing control handler is also a post-commit internal failure. */
    CF_REQUIRE(cf_writer_set_control_handler(f.app, control_refusing, NULL) ==
               CF_OK);
    call_state st2 = {.app = f.app, .calls = 0};
    CF_CHECK(cf_write(f.app, cb_disconnect_user, &st2) == CF_INTERNAL);
    CF_CHECK(message_count(&f) == 2);
    CF_REQUIRE(cf_writer_stats_get(f.app, &stats) == CF_OK);
    CF_CHECK(stats.mandatory_failures == 2);

    /* Registering C03's consumer restores CF_OK and delivers the event. */
    event_log log;
    event_log_init(&log);
    CF_REQUIRE(cf_writer_set_control_handler(f.app, log_control, &log) ==
               CF_OK);
    call_state st3 = {.app = f.app, .calls = 0};
    CF_CHECK(cf_write(f.app, cb_disconnect_user, &st3) == CF_OK);
    CF_CHECK(event_log_count(&log) == 1);
    cf_event delivered = event_log_at(&log, 0);
    CF_CHECK(delivered.kind == CF_EVENT_DISCONNECT_USER &&
             delivered.user_id == 9);
    CF_REQUIRE(cf_writer_stats_get(f.app, &stats) == CF_OK);
    CF_CHECK(stats.mandatory_failures == 2);
    event_log_dispose(&log);
    fixture_close(&f);
}

/* --- no-recursion guard --------------------------------------------------- */

typedef struct {
    call_state outer;
    call_state inner;
    cf_err inner_rc;
} recursion_state;

static cf_err cb_count_only(cf_tx *tx, void *arg) {
    (void)tx;
    call_state *st = arg;
    st->calls++;
    return CF_OK;
}

static cf_err cb_recursion(cf_tx *tx, void *arg) {
    recursion_state *s = arg;
    s->outer.calls++;
    s->inner_rc = cf_write(s->outer.app, cb_count_only, &s->inner);
    return insert_message(tx, 1, 1, "recursion");
}

CF_TEST(writer_callback_cannot_recurse_into_cf_write) {
    writer_fixture f;
    CF_REQUIRE(fixture_open(&f, "8"));
    CF_REQUIRE(fixture_seed(&f));

    recursion_state s;
    memset(&s, 0, sizeof s);
    s.outer.app = f.app;
    uint64_t start = cf_data_version(f.app);
    CF_REQUIRE(cf_write(f.app, cb_recursion, &s) == CF_OK);
    CF_CHECK(s.outer.calls == 1);
    CF_CHECK(s.inner_rc == CF_INTERNAL);
    CF_CHECK(s.inner.calls == 0); /* the nested callback never ran */
    CF_CHECK(message_count(&f) == 1);
    CF_CHECK(cf_data_version(f.app) == start + 1);
    fixture_close(&f);
}

/* --- event validation and misuse ----------------------------------------- */

typedef struct {
    call_state st;
    cf_err null_tx_rc;
    cf_err bad_kind_rc;
    bool db_null;
} tx_check_state;

static cf_err cb_tx_validation(cf_tx *tx, void *arg) {
    tx_check_state *s = arg;
    s->st.calls++;
    s->db_null = cf_tx_db(tx) == NULL;
    cf_event good = {.kind = CF_EVENT_PUSH_MESSAGE, .room_id = 1,
                     .message_id = 1};
    s->null_tx_rc = cf_tx_event(NULL, good);
    cf_event bad = {.kind = (cf_event_kind)7};
    s->bad_kind_rc = cf_tx_event(tx, bad);
    return CF_OK;
}

CF_TEST(writer_tx_event_rejects_unknown_kind_and_null_tx) {
    writer_fixture f;
    CF_REQUIRE(fixture_open(&f, "8"));

    tx_check_state s;
    memset(&s, 0, sizeof s);
    s.st.app = f.app;
    CF_REQUIRE(cf_write(f.app, cb_tx_validation, &s) == CF_OK);
    CF_CHECK(s.st.calls == 1);
    CF_CHECK(s.db_null == false);
    CF_CHECK(s.null_tx_rc == CF_INTERNAL);
    CF_CHECK(s.bad_kind_rc == CF_INVALID);
    CF_CHECK(cf_tx_db(NULL) == NULL);

    /* Registration misuse is rejected, not silently accepted. */
    CF_CHECK(cf_writer_set_event_handler(f.app, CF_EVENT_DISCONNECT_USER,
                                         log_event, NULL) == CF_INVALID);
    CF_CHECK(cf_writer_set_control_handler(f.app, NULL, NULL) == CF_INVALID);
    fixture_close(&f);
}

/* --- concurrent writers serialize; no version updates are lost ----------- */

typedef struct {
    cf_app *app;
    int index;
    cf_err result;
    int calls;
} stress_job;

static cf_err cb_stress(cf_tx *tx, void *arg) {
    stress_job *job = arg;
    job->calls++;
    char client_id[32];
    snprintf(client_id, sizeof client_id, "stress-%d", job->index);
    return insert_message(tx, 1, 1, client_id);
}

static void *stress_thread(void *p) {
    stress_job *job = p;
    for (int i = 0; i < 16; i++) {
        cf_err rc = cf_write(job->app, cb_stress, job);
        if (rc != CF_OK) {
            job->result = rc;
            return NULL;
        }
    }
    job->result = CF_OK;
    return NULL;
}

CF_TEST(writer_serializes_concurrent_writers_without_lost_updates) {
    enum { THREADS = 16, WRITES = 16 };
    writer_fixture f;
    CF_REQUIRE(fixture_open(&f, "64")); /* capacity >= THREADS */
    CF_REQUIRE(fixture_seed(&f));

    uint64_t start = cf_data_version(f.app);
    stress_job jobs[THREADS];
    pthread_t tids[THREADS];
    memset(jobs, 0, sizeof jobs);
    for (int i = 0; i < THREADS; i++) {
        jobs[i].app = f.app;
        jobs[i].index = i;
        CF_REQUIRE(pthread_create(&tids[i], NULL, stress_thread, &jobs[i]) ==
                   0);
    }
    for (int i = 0; i < THREADS; i++) {
        CF_REQUIRE(pthread_join(tids[i], NULL) == 0);
        CF_CHECK(jobs[i].result == CF_OK);
        CF_CHECK(jobs[i].calls == WRITES);
    }
    CF_CHECK(cf_data_version(f.app) == start + THREADS * WRITES);
    CF_CHECK(message_count(&f) == THREADS * WRITES);

    cf_writer_stats stats;
    CF_REQUIRE(cf_writer_stats_get(f.app, &stats) == CF_OK);
    CF_CHECK(stats.admitted == THREADS * WRITES);
    CF_CHECK(stats.committed == THREADS * WRITES);
    CF_CHECK(stats.rolled_back == 0);
    CF_CHECK(stats.pending == 0);
    fixture_close(&f);
}

/* --- queue boundary: CF_BUSY before callback execution ------------------- */

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool entered, release;
} gate;

typedef struct {
    cf_app *app;
    gate *gate;
    cf_err result;
    int calls;
} fill_job;

static void gate_init(gate *g) {
    memset(g, 0, sizeof *g);
    pthread_mutex_init(&g->mu, NULL);
    pthread_cond_init(&g->cv, NULL);
}

static void gate_dispose(gate *g) {
    pthread_mutex_destroy(&g->mu);
    pthread_cond_destroy(&g->cv);
}

static void gate_wait(gate *g) {
    pthread_mutex_lock(&g->mu);
    g->entered = true;
    pthread_cond_broadcast(&g->cv);
    while (!g->release) pthread_cond_wait(&g->cv, &g->mu);
    pthread_mutex_unlock(&g->mu);
}

static void gate_release(gate *g) {
    pthread_mutex_lock(&g->mu);
    g->release = true;
    pthread_cond_broadcast(&g->cv);
    pthread_mutex_unlock(&g->mu);
}

static void gate_wait_entered(gate *g) {
    pthread_mutex_lock(&g->mu);
    while (!g->entered) pthread_cond_wait(&g->cv, &g->mu);
    pthread_mutex_unlock(&g->mu);
}

static cf_err cb_block_commit(cf_tx *tx, void *arg) {
    (void)tx;
    fill_job *job = arg;
    job->calls++;
    gate_wait(job->gate);
    return CF_OK;
}

static cf_err cb_light_commit(cf_tx *tx, void *arg) {
    fill_job *job = arg;
    job->calls++;
    return insert_message(tx, 1, 1, "light");
}

static void *thread_block(void *p) {
    fill_job *job = p;
    job->result = cf_write(job->app, cb_block_commit, job);
    return NULL;
}

static void *thread_light(void *p) {
    fill_job *job = p;
    job->result = cf_write(job->app, cb_light_commit, job);
    return NULL;
}

/* Fill the queue with `capacity` real blocked callers, prove the next
 * cf_write is refused before its callback executes, then drain. */
static void run_queue_boundary(const char *capacity_str, size_t capacity) {
    writer_fixture f;
    CF_REQUIRE(fixture_open(&f, capacity_str));
    CF_REQUIRE(fixture_seed(&f));

    gate g;
    gate_init(&g);
    fill_job block_job = {.app = f.app, .gate = &g};
    pthread_t block_tid;
    CF_REQUIRE(pthread_create(&block_tid, NULL, thread_block, &block_job) ==
               0);
    gate_wait_entered(&g);
    CF_CHECK(g.entered);

    fill_job *jobs = calloc(capacity, sizeof *jobs);
    pthread_t *tids = calloc(capacity, sizeof *tids);
    CF_REQUIRE(jobs != NULL && tids != NULL);
    pthread_attr_t attr;
    CF_REQUIRE(pthread_attr_init(&attr) == 0);
    CF_REQUIRE(pthread_attr_setstacksize(&attr, 512 * 1024) == 0);
    for (size_t i = 0; i < capacity; i++) {
        jobs[i].app = f.app;
        CF_REQUIRE(pthread_create(&tids[i], &attr, thread_light, &jobs[i]) ==
                   0);
    }
    pthread_attr_destroy(&attr);

    /* Wait until every admitted callback is queued behind the blocked one. */
    cf_writer_stats stats;
    uint64_t deadline = cf_monotonic_ms() + 30000;
    for (;;) {
        CF_REQUIRE(cf_writer_stats_get(f.app, &stats) == CF_OK);
        if (stats.pending == capacity && stats.callback_running) break;
        CF_REQUIRE(cf_monotonic_ms() < deadline);
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }
    CF_CHECK(stats.queue_capacity == capacity);

    /* Queue full: CF_BUSY before this callback executes, no mutation. */
    int64_t messages_before = message_count(&f);
    call_state rejected = {.app = f.app, .calls = 0};
    CF_CHECK(cf_write(f.app, cb_count_only, &rejected) == CF_BUSY);
    CF_CHECK(rejected.calls == 0);
    CF_CHECK(message_count(&f) == messages_before);
    CF_REQUIRE(cf_writer_stats_get(f.app, &stats) == CF_OK);
    CF_CHECK(stats.queue_rejected >= 1);

    uint64_t version_before = cf_data_version(f.app);
    gate_release(&g);
    CF_REQUIRE(pthread_join(block_tid, NULL) == 0);
    for (size_t i = 0; i < capacity; i++) {
        CF_REQUIRE(pthread_join(tids[i], NULL) == 0);
    }
    CF_CHECK(block_job.result == CF_OK && block_job.calls == 1);
    for (size_t i = 0; i < capacity; i++) {
        CF_CHECK(jobs[i].result == CF_OK && jobs[i].calls == 1);
    }
    /* All admitted writes committed: the block callback plus the fillers. */
    CF_CHECK(cf_data_version(f.app) == version_before + capacity + 1);
    CF_CHECK(message_count(&f) == (int64_t)capacity);

    free(jobs);
    free(tids);
    gate_dispose(&g);
    fixture_close(&f);
}

CF_TEST(writer_queue_full_small_boundary_returns_busy_before_mutation) {
    run_queue_boundary("4", 4);
}

CF_TEST(writer_queue_full_default_boundary_256) {
    run_queue_boundary("256", 256);
}

/* --- writer not started --------------------------------------------------- */

CF_TEST(writer_without_start_returns_internal_and_never_runs_callback) {
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    cf_config *config = make_config(scratch.path, "8");
    CF_REQUIRE(config != NULL);
    cf_app *app = NULL;
    CF_REQUIRE(cf_app_create(config, &app) == CF_OK);

    call_state st = {.app = app, .calls = 0};
    CF_CHECK(cf_write(app, cb_insert_message, &st) == CF_INTERNAL);
    CF_CHECK(st.calls == 0);
    cf_writer_stats stats;
    CF_CHECK(cf_writer_stats_get(app, &stats) == CF_INTERNAL);

    cf_app_destroy(app);
    cf_db_scratch_close(&scratch);
}

CF_TEST_MAIN()
