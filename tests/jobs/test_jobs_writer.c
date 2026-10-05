/* J01 writer-integration acceptance tests (07-verification.md JOB-01:
 * "Queue-full is recorded per kind; HTTP commit outcome isn't rewritten; no
 * automatic POST retry"; 04-cable-jobs.md "J01/J02"; D02's post-commit
 * consumer seam in src/db/writer.h).
 *
 * These cases drive real committed transactions through cf_write and the
 * writer's post-commit handoff into cf_jobs_register_writer's consumer:
 *  - a committed event reaches exactly one handler per kind, once;
 *  - a full per-kind queue is counted by both the writer
 *    (best_effort_dropped) and the job kind (rejected), and the committed
 *    cf_write still returns CF_OK (never CF_BUSY, no pretend rollback);
 *  - an event for a kind with no registered handler is dropped cleanly and
 *    counted, without rewriting the commit;
 *  - a failing handler is counted once (failed) and never retried.
 *
 * No case silently skips: a setup failure is a CF_REQUIRE failure.
 */
#include "cf_test.h"

#include "app.h"
#include "config.h"
#include "db/db_testutil.h"
#include "db/writer.h"
#include "jobs/jobs.h"

#include "jobs/jobs_testutil.h"

#include <pthread.h>
#include <string.h>

static const char CF_TEST_HEX64[] =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

typedef struct {
    cf_db_scratch scratch;
    cf_config *config;
    cf_app *app;
    cf_jobs *jobs;
    bool writer_started;
    bool jobs_started;
} jobs_fixture;

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

static bool fixture_open(jobs_fixture *f) {
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
    if (cf_jobs_register_writer(f->jobs, f->app) != CF_OK) return false;
    return true;
}

static void fixture_close(jobs_fixture *f) {
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

/* One seeded user and room so callbacks can insert FK-valid rows if a case
 * wants a mutation; the writer itself does not parse events. */
static bool fixture_seed(jobs_fixture *f) {
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

typedef struct {
    cf_event event;
} event_write_arg;

static cf_err append_event_cb(cf_tx *tx, void *arg) {
    event_write_arg *w = arg;
    return cf_tx_event(tx, w->event);
}

static cf_err write_event(jobs_fixture *f, cf_event event) {
    event_write_arg arg = {.event = event};
    return cf_write(f->app, append_event_cb, &arg);
}

static cf_event webhook_event(int64_t message_id) {
    cf_event event;
    memset(&event, 0, sizeof event);
    event.kind = CF_EVENT_DELIVER_WEBHOOK;
    event.message_id = message_id;
    return event;
}

static cf_event push_event(int64_t message_id) {
    cf_event event;
    memset(&event, 0, sizeof event);
    event.kind = CF_EVENT_PUSH_MESSAGE;
    event.room_id = 1;
    event.message_id = message_id;
    return event;
}

/* --- commit delivers the event to its kind, exactly once ----------------- */

CF_TEST(job_commit_delivers_event_once_and_never_retries) {
    jobs_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(fixture_seed(&f));

    job_gate hook_gate, push_gate;
    job_gate_init(&hook_gate);
    job_gate_init(&push_gate);
    CF_REQUIRE(cf_jobs_set_handler(f.jobs, CF_JOB_DELIVER_WEBHOOK,
                                   job_gate_handler, &hook_gate) == CF_OK);
    CF_REQUIRE(cf_jobs_set_handler(f.jobs, CF_JOB_PUSH_MESSAGE,
                                   job_gate_handler, &push_gate) == CF_OK);

    cf_writer_stats before;
    CF_REQUIRE(cf_writer_stats_get(f.app, &before) == CF_OK);

    CF_CHECK(write_event(&f, webhook_event(7)) == CF_OK);
    CF_REQUIRE(cf_jobs_wait_for(f.jobs, CF_JOB_DELIVER_WEBHOOK,
                                cf_job_stat_completed, 1, 5000));

    cf_job_stats hook_stats;
    CF_REQUIRE(cf_jobs_stats_get(f.jobs, CF_JOB_DELIVER_WEBHOOK,
                                 &hook_stats) == CF_OK);
    CF_CHECK(hook_stats.accepted == 1);
    CF_CHECK(hook_stats.completed == 1);
    CF_CHECK(hook_stats.failed == 0);
    CF_CHECK(hook_stats.rejected == 0);
    CF_CHECK(hook_gate.calls == 1);
    CF_CHECK(hook_gate.last_message_id == 7);

    cf_writer_stats after;
    CF_REQUIRE(cf_writer_stats_get(f.app, &after) == CF_OK);
    CF_CHECK(after.committed - before.committed == 1);
    CF_CHECK(after.best_effort_dropped[CF_EVENT_DELIVER_WEBHOOK] ==
             before.best_effort_dropped[CF_EVENT_DELIVER_WEBHOOK]);

    /* A later commit of another kind must not re-run the webhook handler. */
    CF_CHECK(write_event(&f, push_event(8)) == CF_OK);
    CF_REQUIRE(cf_jobs_wait_for(f.jobs, CF_JOB_PUSH_MESSAGE,
                                cf_job_stat_completed, 1, 5000));
    CF_CHECK(hook_gate.calls == 1);

    fixture_close(&f);
    job_gate_dispose(&hook_gate);
    job_gate_dispose(&push_gate);
}

/* --- queue full: recorded per kind, commit outcome untouched ------------- */

CF_TEST(job_queue_full_recorded_per_kind_commit_not_rewritten) {
    jobs_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(fixture_seed(&f));

    job_gate hook_gate, push_gate;
    job_gate_init(&hook_gate);
    job_gate_init(&push_gate);
    job_gate_block(&hook_gate, CF_OK);
    CF_REQUIRE(cf_jobs_set_handler(f.jobs, CF_JOB_DELIVER_WEBHOOK,
                                   job_gate_handler, &hook_gate) == CF_OK);
    CF_REQUIRE(cf_jobs_set_handler(f.jobs, CF_JOB_PUSH_MESSAGE,
                                   job_gate_handler, &push_gate) == CF_OK);

    /* Occupy both webhook workers, then fill the 128-slot FIFO. */
    CF_REQUIRE(cf_job_enqueue(f.jobs, webhook_event(1)) == CF_OK);
    CF_REQUIRE(cf_job_enqueue(f.jobs, webhook_event(2)) == CF_OK);
    CF_REQUIRE(job_gate_wait_entered(&hook_gate, 2, 5000));
    for (int i = 0; i < 128; i++) {
        CF_REQUIRE(cf_job_enqueue(f.jobs, webhook_event(100 + i)) == CF_OK);
    }
    CF_CHECK(cf_job_enqueue(f.jobs, webhook_event(999)) == CF_BUSY);

    cf_job_stats hook_before;
    CF_REQUIRE(cf_jobs_stats_get(f.jobs, CF_JOB_DELIVER_WEBHOOK,
                                 &hook_before) == CF_OK);
    CF_CHECK(hook_before.accepted == 130);
    CF_CHECK(hook_before.pending == 128);
    CF_CHECK(hook_before.rejected == 1);

    cf_writer_stats writer_before;
    CF_REQUIRE(cf_writer_stats_get(f.app, &writer_before) == CF_OK);

    /* The committed write is still CF_OK: never CF_BUSY, no pretend
     * rollback, no retry. */
    CF_CHECK(write_event(&f, webhook_event(1000)) == CF_OK);

    cf_writer_stats writer_after;
    CF_REQUIRE(cf_writer_stats_get(f.app, &writer_after) == CF_OK);
    CF_CHECK(writer_after.committed - writer_before.committed == 1);
    CF_CHECK(writer_after.best_effort_dropped[CF_EVENT_DELIVER_WEBHOOK] -
                 writer_before.best_effort_dropped[CF_EVENT_DELIVER_WEBHOOK] ==
             1);
    CF_CHECK(writer_after.rolled_back == writer_before.rolled_back);

    cf_job_stats hook_after;
    CF_REQUIRE(cf_jobs_stats_get(f.jobs, CF_JOB_DELIVER_WEBHOOK,
                                 &hook_after) == CF_OK);
    CF_CHECK(hook_after.accepted == 130); /* the dropped event is not queued */
    CF_CHECK(hook_after.rejected == 2);   /* 1 direct + 1 post-commit */
    CF_CHECK(hook_after.pending == 128);

    /* A full webhook queue does not affect the push kind. */
    CF_CHECK(write_event(&f, push_event(1000)) == CF_OK);
    CF_REQUIRE(cf_jobs_wait_for(f.jobs, CF_JOB_PUSH_MESSAGE,
                                cf_job_stat_completed, 1, 5000));
    cf_job_stats push_stats;
    CF_REQUIRE(cf_jobs_stats_get(f.jobs, CF_JOB_PUSH_MESSAGE, &push_stats) ==
               CF_OK);
    CF_CHECK(push_stats.accepted == 1);
    CF_CHECK(push_stats.completed == 1);
    CF_CHECK(push_stats.rejected == 0);
    CF_REQUIRE(cf_writer_stats_get(f.app, &writer_after) == CF_OK);
    CF_CHECK(writer_after.best_effort_dropped[CF_EVENT_PUSH_MESSAGE] == 0);

    job_gate_release(&hook_gate);
    CF_REQUIRE(cf_jobs_wait_for(f.jobs, CF_JOB_DELIVER_WEBHOOK,
                                cf_job_stat_completed, 130, 5000));
    CF_REQUIRE(cf_jobs_stats_get(f.jobs, CF_JOB_DELIVER_WEBHOOK,
                                 &hook_after) == CF_OK);
    CF_CHECK(hook_after.completed == 130);
    CF_CHECK(hook_after.failed == 0);
    CF_CHECK(hook_after.pending == 0);

    fixture_close(&f);
    job_gate_dispose(&hook_gate);
    job_gate_dispose(&push_gate);
}

/* --- no handler yet: writer drops cleanly, commit untouched -------------- */

CF_TEST(job_pre_registration_event_dropped_without_rewriting_commit) {
    jobs_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(fixture_seed(&f));

    job_gate push_gate;
    job_gate_init(&push_gate);
    CF_REQUIRE(cf_jobs_set_handler(f.jobs, CF_JOB_PUSH_MESSAGE,
                                   job_gate_handler, &push_gate) == CF_OK);
    /* DELIVER_WEBHOOK deliberately has no handler yet. */

    cf_writer_stats before;
    CF_REQUIRE(cf_writer_stats_get(f.app, &before) == CF_OK);

    CF_CHECK(write_event(&f, webhook_event(1)) == CF_OK);
    cf_writer_stats after;
    CF_REQUIRE(cf_writer_stats_get(f.app, &after) == CF_OK);
    CF_CHECK(after.committed - before.committed == 1);
    CF_CHECK(after.best_effort_dropped[CF_EVENT_DELIVER_WEBHOOK] -
                 before.best_effort_dropped[CF_EVENT_DELIVER_WEBHOOK] ==
             1);

    cf_job_stats hook_stats;
    CF_REQUIRE(cf_jobs_stats_get(f.jobs, CF_JOB_DELIVER_WEBHOOK,
                                 &hook_stats) == CF_OK);
    CF_CHECK(!hook_stats.handler_registered);
    CF_CHECK(hook_stats.accepted == 0);
    CF_CHECK(hook_stats.rejected == 0); /* clean refusal, not a queue-full */

    /* Registering the handler makes later commits deliver normally. */
    job_gate hook_gate;
    job_gate_init(&hook_gate);
    CF_REQUIRE(cf_jobs_set_handler(f.jobs, CF_JOB_DELIVER_WEBHOOK,
                                   job_gate_handler, &hook_gate) == CF_OK);
    CF_CHECK(write_event(&f, webhook_event(2)) == CF_OK);
    CF_REQUIRE(cf_jobs_wait_for(f.jobs, CF_JOB_DELIVER_WEBHOOK,
                                cf_job_stat_completed, 1, 5000));
    CF_CHECK(hook_gate.calls == 1);
    CF_CHECK(hook_gate.last_message_id == 2);

    fixture_close(&f);
    job_gate_dispose(&push_gate);
    job_gate_dispose(&hook_gate);
}

/* --- handler failure: counted once, no automatic POST retry -------------- */

CF_TEST(job_handler_failure_counted_once_not_retried) {
    jobs_fixture f;
    CF_REQUIRE(fixture_open(&f));
    CF_REQUIRE(fixture_seed(&f));

    job_gate hook_gate;
    job_gate_init(&hook_gate);
    hook_gate.result = CF_IO; /* the webhook POST fails every time */
    CF_REQUIRE(cf_jobs_set_handler(f.jobs, CF_JOB_DELIVER_WEBHOOK,
                                   job_gate_handler, &hook_gate) == CF_OK);

    cf_writer_stats before;
    CF_REQUIRE(cf_writer_stats_get(f.app, &before) == CF_OK);

    CF_CHECK(write_event(&f, webhook_event(1)) == CF_OK);
    CF_REQUIRE(cf_jobs_wait_for(f.jobs, CF_JOB_DELIVER_WEBHOOK,
                                cf_job_stat_failed, 1, 5000));
    CF_CHECK(hook_gate.calls == 1);
    CF_CHECK(write_event(&f, webhook_event(2)) == CF_OK);
    CF_REQUIRE(cf_jobs_wait_for(f.jobs, CF_JOB_DELIVER_WEBHOOK,
                                cf_job_stat_failed, 2, 5000));
    CF_CHECK(hook_gate.calls == 2); /* one attempt per committed event */

    cf_job_stats hook_stats;
    CF_REQUIRE(cf_jobs_stats_get(f.jobs, CF_JOB_DELIVER_WEBHOOK,
                                 &hook_stats) == CF_OK);
    CF_CHECK(hook_stats.accepted == 2);
    CF_CHECK(hook_stats.failed == 2);
    CF_CHECK(hook_stats.completed == 0);
    CF_CHECK(hook_stats.pending == 0);

    /* The writer's drop counter is only for handoff refusals: an accepted
     * event whose handler later failed is a job-level failure (failed),
     * and the committed write stayed CF_OK. */
    cf_writer_stats after;
    CF_REQUIRE(cf_writer_stats_get(f.app, &after) == CF_OK);
    CF_CHECK(after.committed - before.committed == 2);
    CF_CHECK(after.best_effort_dropped[CF_EVENT_DELIVER_WEBHOOK] ==
             before.best_effort_dropped[CF_EVENT_DELIVER_WEBHOOK]);

    fixture_close(&f);
    job_gate_dispose(&hook_gate);
}

CF_TEST_MAIN()
