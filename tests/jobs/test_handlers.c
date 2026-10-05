/* J02 handler acceptance tests (07-verification.md JOB-02/JOB-03 and the
 * DB-02 re-read rule; 04-cable-jobs.md "J01/J02: bounded jobs").
 *
 * Exercises src/jobs/handlers.c through real committed state: a scratch
 * database with a started app, writer and jobs queue, the five default
 * handlers registered, and the writer post-commit consumer wired. No case
 * silently skips: a setup failure is a CF_REQUIRE failure. No network: the
 * I01/I02/S02 integration arms are absent (weak imports), so delivery tests
 * assert the recorded no-op counters.
 *
 *   - per-kind happy path with the missing-integration arm (push selects
 *     recipients through the model reads, webhook re-reads bot + message,
 *     purge re-reads the blob, media records the S03 stub);
 *   - deleted/revoked targets are recorded no-ops (CF_OK, counted
 *     completed, nothing resurrected);
 *   - queue-full CF_BUSY stays a drop (commit stays CF_OK, counted on both
 *     sides, no commit rewrite);
 *   - RemoveBannedContent destroys at most 100 messages per job and
 *     requeues the remainder (205 seeded messages converge to zero).
 *
 * Targeted build (the Makefile does not list src/jobs/handlers.c yet):
 *   cc -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra \
 *      -Werror -pthread -O1 -g -Isrc -Itests <dep includes> \
 *      -c tests/jobs/test_handlers.c -o test_handlers.o
 * then link test_handlers.o with the application library objects and the
 * recorded dependency archives (the same rule the Makefile uses for the
 * other tests/jobs binaries).
 */
#include "cf_test.h"

#include "app.h"
#include "config.h"
#include "db/db_testutil.h"
#include "db/writer.h"
#include "jobs/handlers.h"
#include "jobs/jobs.h"

#include "jobs/jobs_testutil.h"

#include <sqlite3.h>
#include <string.h>

static const char CF_TEST_HEX64[] =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    cf_jobs *jobs;
    cf_jobs_handler_ctx hctx;
    bool writer_started;
    bool jobs_started;
} handlers_fixture;

static cf_config *make_config(const char *db_path) {
    cf_config_entry entries[6] = {
        {"PUBLIC_ORIGIN", "http://127.0.0.1:3999"},
        {"SECRET_KEY_BASE", CF_TEST_HEX64},
        {"DATABASE_PATH", db_path},
        {"CF_WRITER_QUEUE", "8"},
        {"CF_JOB_QUEUE", "128"},
        {"CF_JOB_WORKERS", "2"},
    };
    cf_config *config = NULL;
    cf_config_error err;
    if (cf_config_parse(entries, 6, &err, &config) != CF_OK) return NULL;
    return config;
}

static bool fixture_open(handlers_fixture *f) {
    memset(f, 0, sizeof *f);
    if (!cf_db_scratch_open(&f->scratch)) return false;
    f->config = make_config(f->scratch.path);
    if (f->config == NULL) return false;
    if (cf_app_create(f->config, &f->app) != CF_OK) {
        cf_config_destroy(f->config);
        f->config = NULL;
        return false;
    }
    f->config = NULL; /* owned by the app now */
    if (cf_writer_start(f->app, cf_app_config(f->app)) != CF_OK) return false;
    f->writer_started = true;
    if (cf_jobs_start(cf_app_config(f->app), &f->jobs) != CF_OK) return false;
    f->jobs_started = true;
    memset(&f->hctx, 0, sizeof f->hctx);
    f->hctx.app = f->app;
    f->hctx.cable = NULL; /* broadcasts skipped: recorded, no sockets */
    if (cf_jobs_register_default_handlers(f->jobs, &f->hctx) != CF_OK) {
        return false;
    }
    if (cf_jobs_register_writer(f->jobs, f->app) != CF_OK) return false;
    return true;
}

static void fixture_close(handlers_fixture *f) {
    /* Shutdown order main.c/J02 must use: jobs join before app teardown. */
    if (f->jobs_started) {
        cf_jobs_stop(f->jobs);
        cf_jobs_destroy(f->jobs);
        f->jobs_started = false;
    }
    if (f->writer_started) cf_writer_stop(f->app);
    if (f->app != NULL) cf_app_destroy(f->app);
    cf_db_scratch_close(&f->scratch);
}

static bool seed_sql(handlers_fixture *f, const char *sql) {
    return cf_db_test_exec(cf_db_handle(f->scratch.db), sql) == SQLITE_OK;
}

/* One member user (id 1) and one open room (id 1). role/status: 0/0. */
static bool seed_user_room(handlers_fixture *f) {
    return seed_sql(f,
                    "INSERT INTO users (id,name,created_at,updated_at,role,"
                    "status) VALUES (1,'Seed','2026-01-01 "
                    "00:00:00','2026-01-01 00:00:00',0,0);"
                    "INSERT INTO rooms (id,creator_id,type,created_at,"
                    "updated_at) VALUES (1,1,'Rooms::Open','2026-01-01 "
                    "00:00:00','2026-01-01 00:00:00');");
}

static bool seed_message(handlers_fixture *f, int64_t id, int64_t room_id,
                         int64_t creator_id, int64_t seq) {
    char sql[256];
    snprintf(sql, sizeof sql,
             "INSERT INTO messages (id,client_message_id,created_at,"
             "creator_id,room_id,updated_at) VALUES (%lld,'cmid-%lld',"
             "'2026-01-01 00:00:00',%lld,%lld,'2026-01-01 00:00:00');",
             (long long)id, (long long)seq, (long long)creator_id,
             (long long)room_id);
    return seed_sql(f, sql);
}

static int64_t table_count(handlers_fixture *f, const char *table,
                           const char *where) {
    char sql[256];
    if (where != NULL) {
        snprintf(sql, sizeof sql, "SELECT count(*) FROM %s WHERE %s;", table,
                 where);
    } else {
        snprintf(sql, sizeof sql, "SELECT count(*) FROM %s;", table);
    }
    bool ok = false;
    int64_t n = cf_db_test_i64(cf_db_handle(f->scratch.db), sql, &ok);
    if (!ok) return -1;
    return n;
}

static uint64_t ctx_load(const _Atomic uint64_t *counter) {
    return atomic_load_explicit(counter, memory_order_relaxed);
}

static cf_job push_job(int64_t room_id, int64_t message_id) {
    cf_job job;
    memset(&job, 0, sizeof job);
    job.kind = CF_JOB_PUSH_MESSAGE;
    job.room_id = room_id;
    job.message_id = message_id;
    return job;
}

static cf_job webhook_job(int64_t user_id, int64_t message_id) {
    cf_job job;
    memset(&job, 0, sizeof job);
    job.kind = CF_JOB_DELIVER_WEBHOOK;
    job.user_id = user_id;
    job.message_id = message_id;
    return job;
}

static cf_job banned_job(int64_t user_id) {
    cf_job job;
    memset(&job, 0, sizeof job);
    job.kind = CF_JOB_REMOVE_BANNED_CONTENT;
    job.user_id = user_id;
    return job;
}

static cf_job purge_job(int64_t blob_id) {
    cf_job job;
    memset(&job, 0, sizeof job);
    job.kind = CF_JOB_PURGE_BLOB;
    job.blob_id = blob_id;
    return job;
}

typedef struct {
    cf_event event;
} event_write_arg;

static cf_err append_event_cb(cf_tx *tx, void *arg) {
    event_write_arg *w = arg;
    return cf_tx_event(tx, w->event);
}

static cf_err write_event(handlers_fixture *f, cf_event event) {
    event_write_arg arg = {.event = event};
    return cf_write(f->app, append_event_cb, &arg);
}

/* --- registration -------------------------------------------------------- */

CF_TEST(handlers_register_all_kinds) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    for (int k = 0; k < CF_JOB_KIND_COUNT; k++) {
        cf_job_stats stats;
        CF_REQUIRE(cf_jobs_stats_get(f.jobs, (cf_job_kind)k, &stats) == CF_OK);
        CF_CHECK(stats.handler_registered);
    }
    CF_CHECK(cf_jobs_register_default_handlers(NULL, &f.hctx) == CF_INVALID);
    CF_CHECK(cf_jobs_register_default_handlers(f.jobs, NULL) == CF_INVALID);
    fixture_close(&f);
}

/* --- PushMessage ---------------------------------------------------------- */

CF_TEST(handlers_push_message_missing_message_is_noop) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(seed_user_room(&f));

    cf_job job = push_job(1, 4242); /* no such message */
    CF_CHECK(cf_jobs_handle_push_message(&f.hctx, &job) == CF_OK);
    CF_CHECK(ctx_load(&f.hctx.push_noop_gone) == 1);
    CF_CHECK(ctx_load(&f.hctx.push_attempted) == 0);

    CF_CHECK(cf_jobs_handle_push_message(NULL, &job) == CF_INVALID);
    CF_CHECK(cf_jobs_handle_push_message(&f.hctx, NULL) == CF_INVALID);
    fixture_close(&f);
}

CF_TEST(handlers_push_message_missing_integration_noop) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(seed_user_room(&f));
    CF_REQUIRE(seed_message(&f, 1, 1, 1, 1));

    /* Happy path up to delivery: the model reads run on real rows, then
     * the absent I02 symbol takes the recorded no-op arm. */
    cf_job job = push_job(1, 1);
    CF_CHECK(cf_jobs_handle_push_message(&f.hctx, &job) == CF_OK);
    CF_CHECK(ctx_load(&f.hctx.push_noop_gone) == 0);
    CF_CHECK(ctx_load(&f.hctx.push_noop_no_integration) == 1);
    CF_CHECK(ctx_load(&f.hctx.push_attempted) == 0);
    CF_CHECK(table_count(&f, "messages", NULL) == 1); /* untouched */
    fixture_close(&f);
}

CF_TEST(handlers_push_message_room_mismatch_is_noop) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(seed_user_room(&f));
    CF_REQUIRE(seed_sql(&f,
                        "INSERT INTO rooms (id,creator_id,type,created_at,"
                        "updated_at) VALUES (2,1,'Rooms::Open','2026-01-01 "
                        "00:00:00','2026-01-01 00:00:00');"));
    CF_REQUIRE(seed_message(&f, 1, 1, 1, 1));

    cf_job job = push_job(2, 1); /* message 1 lives in room 1 */
    CF_CHECK(cf_jobs_handle_push_message(&f.hctx, &job) == CF_OK);
    CF_CHECK(ctx_load(&f.hctx.push_noop_gone) == 1);
    fixture_close(&f);
}

/* --- DeliverWebhook -------------------------------------------------------- */

CF_TEST(handlers_deliver_webhook_missing_bot_is_noop) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(seed_user_room(&f));

    cf_job job = webhook_job(55, 1); /* no such bot, no such message */
    CF_CHECK(cf_jobs_handle_deliver_webhook(&f.hctx, &job) == CF_OK);
    CF_CHECK(ctx_load(&f.hctx.webhook_noop_gone) == 1);
    CF_CHECK(ctx_load(&f.hctx.webhook_attempted) == 0);
    fixture_close(&f);
}

CF_TEST(handlers_deliver_webhook_missing_integration_noop) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(seed_user_room(&f));
    CF_REQUIRE(seed_sql(
        &f,
        "INSERT INTO users (id,name,created_at,updated_at,role,status) "
        "VALUES (5,'Bot','2026-01-01 00:00:00','2026-01-01 00:00:00',2,0);"
        "INSERT INTO webhooks (user_id,url,created_at,updated_at) VALUES "
        "(5,'https://bot.example/hook','2026-01-01 "
        "00:00:00','2026-01-01 00:00:00');"));
    CF_REQUIRE(seed_message(&f, 1, 1, 1, 1));

    /* Active bot + message + webhook URL re-read, then the absent I01
     * symbol takes the recorded no-op arm. */
    cf_job job = webhook_job(5, 1);
    CF_CHECK(cf_jobs_handle_deliver_webhook(&f.hctx, &job) == CF_OK);
    CF_CHECK(ctx_load(&f.hctx.webhook_noop_gone) == 0);
    CF_CHECK(ctx_load(&f.hctx.webhook_noop_no_integration) == 1);
    CF_CHECK(ctx_load(&f.hctx.webhook_attempted) == 0);
    fixture_close(&f);
}

CF_TEST(handlers_deliver_webhook_revoked_targets_are_noop) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(seed_user_room(&f));
    CF_REQUIRE(seed_sql(
        &f,
        /* Active bot with no webhook row: hook revoked. */
        "INSERT INTO users (id,name,created_at,updated_at,role,status) "
        "VALUES (6,'Bot6','2026-01-01 00:00:00','2026-01-01 00:00:00',2,0);"
        /* Deactivated bot with a webhook row: no longer active. */
        "INSERT INTO users (id,name,created_at,updated_at,role,status) "
        "VALUES (7,'Bot7','2026-01-01 00:00:00','2026-01-01 00:00:00',2,1);"
        "INSERT INTO webhooks (user_id,url,created_at,updated_at) VALUES "
        "(7,'https://bot.example/hook7','2026-01-01 "
        "00:00:00','2026-01-01 00:00:00');"));
    CF_REQUIRE(seed_message(&f, 1, 1, 1, 1));

    CF_CHECK(cf_jobs_handle_deliver_webhook(&f.hctx,
                                            &(cf_job){0}) == CF_INVALID);
    cf_job no_hook = webhook_job(6, 1);
    CF_CHECK(cf_jobs_handle_deliver_webhook(&f.hctx, &no_hook) == CF_OK);
    cf_job inactive = webhook_job(7, 1);
    CF_CHECK(cf_jobs_handle_deliver_webhook(&f.hctx, &inactive) == CF_OK);
    CF_CHECK(ctx_load(&f.hctx.webhook_noop_gone) == 2);
    CF_CHECK(ctx_load(&f.hctx.webhook_noop_no_integration) == 0);
    fixture_close(&f);
}

/* --- RemoveBannedContent ---------------------------------------------------- */

CF_TEST(handlers_remove_banned_batches_100_and_requeues) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(seed_sql(
        &f,
        "INSERT INTO users (id,name,created_at,updated_at,role,status) "
        "VALUES (9,'Banned','2026-01-01 00:00:00','2026-01-01 "
        "00:00:00',0,2);"
        "INSERT INTO rooms (id,creator_id,type,created_at,updated_at) "
        "VALUES (1,1,'Rooms::Open','2026-01-01 "
        "00:00:00','2026-01-01 00:00:00');"));
    for (int64_t i = 1; i <= 205; i++) {
        CF_REQUIRE(seed_message(&f, i, 1, 9, i));
    }
    CF_REQUIRE(table_count(&f, "messages", "creator_id = 9") == 205);

    /* Hold the queue's own consumer so the requeue lands observably: the
     * direct handler call below must destroy exactly one batch of 100 and
     * leave one pending remainder. */
    job_gate gate;
    job_gate_init(&gate);
    job_gate_block(&gate, CF_OK);
    CF_REQUIRE(cf_jobs_set_handler(f.jobs, CF_JOB_REMOVE_BANNED_CONTENT,
                                   job_gate_handler, &gate) == CF_OK);

    cf_job job = banned_job(9);
    CF_REQUIRE(cf_jobs_handle_remove_banned_content(&f.hctx, &job) == CF_OK);
    CF_CHECK(table_count(&f, "messages", "creator_id = 9") == 105);
    CF_CHECK(ctx_load(&f.hctx.banned_batches) == 1);
    CF_CHECK(ctx_load(&f.hctx.banned_destroyed) == 100);
    CF_CHECK(ctx_load(&f.hctx.banned_requeued) == 1);
    CF_CHECK(ctx_load(&f.hctx.banned_requeue_dropped) == 0);
    CF_REQUIRE(job_gate_wait_entered(&gate, 1, 5000));
    cf_job_stats stats;
    CF_REQUIRE(cf_jobs_stats_get(f.jobs, CF_JOB_REMOVE_BANNED_CONTENT,
                                 &stats) == CF_OK);
    CF_CHECK(stats.accepted == 1);
    CF_CHECK(stats.running == 1);

    /* Release the gate (its job completes without touching rows), hand the
     * kind back to the default handler, and run the second batch directly:
     * the final remainder converges through the queue worker. */
    job_gate_release(&gate);
    CF_REQUIRE(cf_jobs_wait_for(f.jobs, CF_JOB_REMOVE_BANNED_CONTENT,
                                cf_job_stat_completed, 1, 5000));
    CF_REQUIRE(cf_jobs_set_handler(
                   f.jobs, CF_JOB_REMOVE_BANNED_CONTENT,
                   cf_jobs_handle_remove_banned_content, &f.hctx) == CF_OK);
    CF_REQUIRE(cf_jobs_handle_remove_banned_content(&f.hctx, &job) == CF_OK);
    CF_REQUIRE(cf_jobs_wait_for(f.jobs, CF_JOB_REMOVE_BANNED_CONTENT,
                                cf_job_stat_completed, 2, 5000));
    CF_CHECK(table_count(&f, "messages", "creator_id = 9") == 0);
    CF_CHECK(ctx_load(&f.hctx.banned_batches) == 3);
    CF_CHECK(ctx_load(&f.hctx.banned_destroyed) == 205);
    CF_CHECK(ctx_load(&f.hctx.banned_requeued) == 2);
    CF_CHECK(ctx_load(&f.hctx.banned_requeue_dropped) == 0);
    CF_REQUIRE(cf_jobs_stats_get(f.jobs, CF_JOB_REMOVE_BANNED_CONTENT,
                                 &stats) == CF_OK);
    CF_CHECK(stats.accepted == 2);
    CF_CHECK(stats.completed == 2);
    CF_CHECK(stats.failed == 0);

    job_gate_dispose(&gate);
    fixture_close(&f);
}

CF_TEST(handlers_remove_banned_unbanned_user_is_noop) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(seed_user_room(&f)); /* user 1 is active, not banned */
    for (int64_t i = 1; i <= 3; i++) {
        CF_REQUIRE(seed_message(&f, i, 1, 1, i));
    }

    cf_job job = banned_job(1);
    CF_CHECK(cf_jobs_handle_remove_banned_content(&f.hctx, &job) == CF_OK);
    CF_CHECK(ctx_load(&f.hctx.banned_noop_gone) == 1);
    CF_CHECK(ctx_load(&f.hctx.banned_batches) == 0);
    CF_CHECK(table_count(&f, "messages", NULL) == 3); /* kept */

    cf_job missing = banned_job(404);
    CF_CHECK(cf_jobs_handle_remove_banned_content(&f.hctx, &missing) ==
             CF_OK);
    CF_CHECK(ctx_load(&f.hctx.banned_noop_gone) == 2);
    fixture_close(&f);
}

/* --- PurgeBlob --------------------------------------------------------------- */

CF_TEST(handlers_purge_blob_missing_is_noop) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));

    cf_job job = purge_job(4242); /* no such blob */
    CF_CHECK(cf_jobs_handle_purge_blob(&f.hctx, &job) == CF_OK);
    CF_CHECK(ctx_load(&f.hctx.purge_noop_gone) == 1);
    fixture_close(&f);
}

CF_TEST(handlers_purge_blob_present_defers_to_s02) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(seed_sql(&f,
                        "INSERT INTO active_storage_blobs (id,byte_size,"
                        "created_at,filename,\"key\",service_name) VALUES "
                        "(7,10,'2026-01-01 "
                        "00:00:00','file.bin','abcdefghijklmnopqrstuvwxyz01',"
                        "'disk');"));

    /* The blob exists but S02 has not landed: recorded no-op, nothing on
     * disk or in the row is touched. */
    cf_job job = purge_job(7);
    CF_CHECK(cf_jobs_handle_purge_blob(&f.hctx, &job) == CF_OK);
    CF_CHECK(ctx_load(&f.hctx.purge_deferred_no_s02) == 1);
    CF_CHECK(ctx_load(&f.hctx.purge_deleted) == 0);
    CF_CHECK(table_count(&f, "active_storage_blobs", NULL) == 1);
    fixture_close(&f);
}

/* --- Media ------------------------------------------------------------------ */

CF_TEST(handlers_media_is_recorded_noop) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));

    /* Direct call and queue-level delivery both record the S03 stub arm. */
    cf_job job;
    memset(&job, 0, sizeof job);
    job.kind = CF_JOB_MEDIA;
    job.blob_id = 7;
    CF_CHECK(cf_jobs_handle_media(&f.hctx, &job) == CF_OK);
    CF_CHECK(cf_jobs_handle_media(NULL, &job) == CF_INVALID);
    CF_CHECK(cf_jobs_enqueue_media(f.jobs, 7, cf_span_lit("analyze"),
                                   cf_span_lit("thumb")) == CF_OK);
    CF_REQUIRE(cf_jobs_wait_for(f.jobs, CF_JOB_MEDIA, cf_job_stat_completed,
                                1, 5000));
    CF_CHECK(ctx_load(&f.hctx.media_noop) == 2);
    cf_job_stats stats;
    CF_REQUIRE(cf_jobs_stats_get(f.jobs, CF_JOB_MEDIA, &stats) == CF_OK);
    CF_CHECK(stats.accepted == 1);
    CF_CHECK(stats.completed == 1);
    CF_CHECK(stats.failed == 0);
    fixture_close(&f);
}

/* --- queue-level accounting --------------------------------------------------- */

CF_TEST(handlers_queue_level_completed_and_failed_accounting) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(seed_user_room(&f));

    /* A committed push for a deleted message completes as a recorded
     * no-op through the queue worker (JOB-03: completed, not failed). */
    cf_event gone;
    memset(&gone, 0, sizeof gone);
    gone.kind = CF_EVENT_PUSH_MESSAGE;
    gone.room_id = 1;
    gone.message_id = 4242;
    CF_REQUIRE(cf_job_enqueue(f.jobs, gone) == CF_OK);
    CF_REQUIRE(cf_jobs_wait_for(f.jobs, CF_JOB_PUSH_MESSAGE,
                                cf_job_stat_completed, 1, 5000));
    CF_CHECK(ctx_load(&f.hctx.push_noop_gone) == 1);

    /* An invalid payload fails the handler: counted once, never retried. */
    cf_event invalid = gone;
    invalid.message_id = 0;
    CF_REQUIRE(cf_job_enqueue(f.jobs, invalid) == CF_OK);
    CF_REQUIRE(cf_jobs_wait_for(f.jobs, CF_JOB_PUSH_MESSAGE,
                                cf_job_stat_failed, 1, 5000));
    cf_job_stats stats;
    CF_REQUIRE(cf_jobs_stats_get(f.jobs, CF_JOB_PUSH_MESSAGE, &stats) ==
               CF_OK);
    CF_CHECK(stats.accepted == 2);
    CF_CHECK(stats.completed == 1);
    CF_CHECK(stats.failed == 1);
    CF_CHECK(stats.pending == 0);
    fixture_close(&f);
}

CF_TEST(handlers_queue_full_drop_never_rewrites_commit) {
    handlers_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(seed_user_room(&f));

    /* Occupy both webhook workers with the default handler replaced by a
     * blocking gate, then fill the 128-slot FIFO exactly. */
    job_gate gate;
    job_gate_init(&gate);
    job_gate_block(&gate, CF_OK);
    CF_REQUIRE(cf_jobs_set_handler(f.jobs, CF_JOB_DELIVER_WEBHOOK,
                                   job_gate_handler, &gate) == CF_OK);
    cf_event hook;
    memset(&hook, 0, sizeof hook);
    hook.kind = CF_EVENT_DELIVER_WEBHOOK;
    hook.user_id = 1;
    hook.message_id = 1;
    CF_REQUIRE(cf_job_enqueue(f.jobs, hook) == CF_OK);
    CF_REQUIRE(cf_job_enqueue(f.jobs, hook) == CF_OK);
    CF_REQUIRE(job_gate_wait_entered(&gate, 2, 5000));
    for (int i = 0; i < 128; i++) {
        CF_REQUIRE(cf_job_enqueue(f.jobs, hook) == CF_OK);
    }
    CF_CHECK(cf_job_enqueue(f.jobs, hook) == CF_BUSY);

    /* The committed write still returns CF_OK: the drop is counted on both
     * sides (queue rejected, writer best_effort_dropped) and the commit
     * outcome is never rewritten. */
    cf_writer_stats writer_before;
    CF_REQUIRE(cf_writer_stats_get(f.app, &writer_before) == CF_OK);
    CF_CHECK(write_event(&f, hook) == CF_OK);
    cf_writer_stats writer_after;
    CF_REQUIRE(cf_writer_stats_get(f.app, &writer_after) == CF_OK);
    CF_CHECK(writer_after.committed - writer_before.committed == 1);
    CF_CHECK(writer_after.best_effort_dropped[CF_EVENT_DELIVER_WEBHOOK] -
                 writer_before.best_effort_dropped[CF_EVENT_DELIVER_WEBHOOK] ==
             1);
    cf_job_stats stats;
    CF_REQUIRE(cf_jobs_stats_get(f.jobs, CF_JOB_DELIVER_WEBHOOK, &stats) ==
               CF_OK);
    CF_CHECK(stats.accepted == 130);
    CF_CHECK(stats.rejected == 2); /* 1 direct + 1 post-commit */

    job_gate_release(&gate);
    CF_REQUIRE(cf_jobs_wait_for(f.jobs, CF_JOB_DELIVER_WEBHOOK,
                                cf_job_stat_completed, 130, 5000));
    job_gate_dispose(&gate);
    fixture_close(&f);
}

CF_TEST_MAIN()
