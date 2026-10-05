/* J01 job-queue acceptance tests (07-verification.md JOB-01/JOB-03;
 * 04-cable-jobs.md "J01/J02: bounded jobs"; 01-foundation-http.md
 * "Configuration: fixed defaults").
 *
 * Covers the pure queue contract: one FIFO per kind with 128 pending slots,
 * per-kind worker counts (2 default, media 4), exact counters at the
 * 127/128/129 boundary, clean failure before handler registration, per-kind
 * isolation while one handler is slow, FIFO order, handler-failure counting
 * with no retry, and shutdown discard with no startup recovery.
 *
 * The writer-integration half of JOB-01 (commit outcome never rewritten, no
 * automatic POST retry) lives in tests/jobs/test_jobs_writer.c. No case
 * silently skips: a setup failure is a CF_REQUIRE failure.
 */
#include "cf_test.h"

#include "config.h"
#include "jobs/jobs.h"

#include "jobs/jobs_testutil.h"

#include <string.h>

static const char CF_TEST_HEX64[] =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

static cf_config *make_config(const char *queue, const char *workers) {
    cf_config_entry entries[4] = {
        {"PUBLIC_ORIGIN", "http://127.0.0.1:3999"},
        {"SECRET_KEY_BASE", CF_TEST_HEX64},
        {"CF_JOB_QUEUE", queue},
        {"CF_JOB_WORKERS", workers},
    };
    size_t count = 2;
    if (queue != NULL) count = workers != NULL ? 4 : 3;
    cf_config *config = NULL;
    cf_config_error err;
    if (cf_config_parse(entries, count, &err, &config) != CF_OK) return NULL;
    return config;
}

static cf_event push_event(int64_t id) {
    cf_event event;
    memset(&event, 0, sizeof event);
    event.kind = CF_EVENT_PUSH_MESSAGE;
    event.room_id = 1;
    event.message_id = id;
    event.user_id = 99; /* unused by PUSH_MESSAGE: must be zeroed */
    return event;
}

static cf_event webhook_event(int64_t message_id) {
    cf_event event;
    memset(&event, 0, sizeof event);
    event.kind = CF_EVENT_DELIVER_WEBHOOK;
    event.message_id = message_id;
    return event;
}

static cf_event purge_event(int64_t blob_id) {
    cf_event event;
    memset(&event, 0, sizeof event);
    event.kind = CF_EVENT_PURGE_BLOB;
    event.blob_id = blob_id;
    return event;
}

/* --- fixed defaults (01 configuration table) ----------------------------- */

CF_TEST(jobs_default_capacity_and_worker_counts) {
    cf_config *config = make_config(NULL, NULL);
    CF_REQUIRE(config != NULL);
    cf_jobs *jobs = NULL;
    CF_REQUIRE(cf_jobs_start(config, &jobs) == CF_OK);
    CF_REQUIRE(jobs != NULL);

    for (int k = 0; k < CF_JOB_KIND_COUNT; k++) {
        cf_job_stats stats;
        CF_REQUIRE(cf_jobs_stats_get(jobs, (cf_job_kind)k, &stats) == CF_OK);
        CF_CHECK(stats.capacity == CF_CONFIG_DEFAULT_JOB_QUEUE);
        CF_CHECK(stats.capacity == 128);
        size_t expected = k == CF_JOB_MEDIA ? CF_JOB_MEDIA_WORKERS
                                            : CF_CONFIG_DEFAULT_JOB_WORKERS;
        CF_CHECK(stats.workers == expected);
        CF_CHECK(expected == (k == CF_JOB_MEDIA ? 4u : 2u));
        CF_CHECK(stats.accepted == 0);
        CF_CHECK(stats.pending == 0);
        CF_CHECK(!stats.handler_registered);
    }

    cf_jobs_stop(jobs);
    cf_jobs_destroy(jobs);
    cf_config_destroy(config);
}

CF_TEST(jobs_config_overrides_apply_per_kind_but_media_stays_four) {
    cf_config *config = make_config("3", "5");
    CF_REQUIRE(config != NULL);
    cf_jobs *jobs = NULL;
    CF_REQUIRE(cf_jobs_start(config, &jobs) == CF_OK);

    for (int k = 0; k < CF_JOB_KIND_COUNT; k++) {
        cf_job_stats stats;
        CF_REQUIRE(cf_jobs_stats_get(jobs, (cf_job_kind)k, &stats) == CF_OK);
        CF_CHECK(stats.capacity == 3);
        CF_CHECK(stats.workers == (k == CF_JOB_MEDIA ? 4u : 5u));
    }

    cf_jobs_destroy(jobs);
    cf_config_destroy(config);
}

/* --- 127/128/129 boundary, exact counters -------------------------------- */

CF_TEST(jobs_boundary_127_128_129_counters_exact) {
    cf_config *config = make_config("128", "2");
    CF_REQUIRE(config != NULL);
    cf_jobs *jobs = NULL;
    CF_REQUIRE(cf_jobs_start(config, &jobs) == CF_OK);

    job_gate gate;
    job_gate_init(&gate);
    job_gate_block(&gate, CF_OK);
    CF_REQUIRE(cf_jobs_set_handler(jobs, CF_JOB_PUSH_MESSAGE,
                                   job_gate_handler, &gate) == CF_OK);

    /* Two blocked workers keep the FIFO exactly as full as we make it. */
    CF_REQUIRE(cf_job_enqueue(jobs, push_event(1)) == CF_OK);
    CF_REQUIRE(cf_job_enqueue(jobs, push_event(2)) == CF_OK);
    CF_REQUIRE(job_gate_wait_entered(&gate, 2, 5000));

    cf_job_stats stats;
    for (int64_t id = 3; id <= 129; id++) { /* 127 more accepted */
        CF_REQUIRE(cf_job_enqueue(jobs, push_event(id)) == CF_OK);
    }
    CF_REQUIRE(cf_jobs_stats_get(jobs, CF_JOB_PUSH_MESSAGE, &stats) == CF_OK);
    CF_CHECK(stats.accepted == 129);
    CF_CHECK(stats.pending == 127);
    CF_CHECK(stats.running == 2);
    CF_CHECK(stats.rejected == 0);

    CF_REQUIRE(cf_job_enqueue(jobs, push_event(130)) == CF_OK); /* 128th */
    CF_REQUIRE(cf_jobs_stats_get(jobs, CF_JOB_PUSH_MESSAGE, &stats) == CF_OK);
    CF_CHECK(stats.accepted == 130);
    CF_CHECK(stats.pending == 128); /* full */
    CF_CHECK(stats.capacity == 128);
    CF_CHECK(stats.rejected == 0);

    /* 129th attempt is refused before admission and counted as rejected. */
    CF_CHECK(cf_job_enqueue(jobs, push_event(131)) == CF_BUSY);
    CF_REQUIRE(cf_jobs_stats_get(jobs, CF_JOB_PUSH_MESSAGE, &stats) == CF_OK);
    CF_CHECK(stats.accepted == 130);
    CF_CHECK(stats.pending == 128);
    CF_CHECK(stats.rejected == 1);
    CF_CHECK(stats.completed == 0);

    job_gate_release(&gate);
    CF_REQUIRE(cf_jobs_wait_for(jobs, CF_JOB_PUSH_MESSAGE,
                                cf_job_stat_completed, 130, 5000));
    CF_REQUIRE(cf_jobs_stats_get(jobs, CF_JOB_PUSH_MESSAGE, &stats) == CF_OK);
    CF_CHECK(stats.accepted == 130);
    CF_CHECK(stats.completed == 130);
    CF_CHECK(stats.failed == 0);
    CF_CHECK(stats.rejected == 1);
    CF_CHECK(stats.shutdown_discarded == 0);
    CF_CHECK(stats.pending == 0);
    CF_CHECK(stats.running == 0);
    CF_CHECK(gate.calls == 130);

    cf_jobs_stop(jobs);
    cf_jobs_destroy(jobs);
    job_gate_dispose(&gate);
    cf_config_destroy(config);
}

/* --- FIFO order and event normalization ---------------------------------- */

CF_TEST(jobs_fifo_order_per_kind_and_normalized_payload) {
    cf_config *config = make_config("16", "1"); /* one worker: strict order */
    CF_REQUIRE(config != NULL);
    cf_jobs *jobs = NULL;
    CF_REQUIRE(cf_jobs_start(config, &jobs) == CF_OK);

    job_gate gate;
    job_gate_init(&gate);
    CF_REQUIRE(cf_jobs_set_handler(jobs, CF_JOB_PUSH_MESSAGE,
                                   job_gate_handler, &gate) == CF_OK);

    const int n = 12;
    for (int i = 1; i <= n; i++) {
        CF_REQUIRE(cf_job_enqueue(jobs, push_event(i)) == CF_OK);
    }
    CF_REQUIRE(cf_jobs_wait_for(jobs, CF_JOB_PUSH_MESSAGE,
                                cf_job_stat_completed, (uint64_t)n, 5000));
    CF_REQUIRE(gate.id_count == (size_t)n);
    for (int i = 0; i < n; i++) {
        CF_CHECK(gate.ids[i] == i + 1); /* FIFO within the kind */
    }
    CF_CHECK(gate.last_user_id == 0); /* unused field zeroed, not 99 */
    CF_CHECK(gate.last_room_id == 1);
    CF_CHECK(gate.last_message_id == n);

    cf_jobs_stop(jobs);
    cf_jobs_destroy(jobs);
    job_gate_dispose(&gate);
    cf_config_destroy(config);
}

/* --- enqueue before registration fails cleanly --------------------------- */

CF_TEST(jobs_enqueue_before_registration_fails_cleanly) {
    cf_config *config = make_config(NULL, NULL);
    CF_REQUIRE(config != NULL);
    cf_jobs *jobs = NULL;
    CF_REQUIRE(cf_jobs_start(config, &jobs) == CF_OK);

    /* No handler registered: refusals are clean, nothing is counted. */
    CF_CHECK(cf_job_enqueue(jobs, push_event(1)) == CF_INVALID);
    CF_CHECK(cf_job_enqueue(jobs, webhook_event(1)) == CF_INVALID);
    CF_CHECK(cf_job_enqueue(jobs, purge_event(1)) == CF_INVALID);
    CF_CHECK(cf_jobs_enqueue_media(jobs, 1, cf_span_lit("analyze"),
                                   cf_span_lit("thumb")) == CF_INVALID);

    cf_job_stats stats;
    CF_REQUIRE(cf_jobs_stats_get(jobs, CF_JOB_PUSH_MESSAGE, &stats) == CF_OK);
    CF_CHECK(stats.accepted == 0);
    CF_CHECK(stats.rejected == 0);
    CF_CHECK(stats.pending == 0);
    CF_CHECK(!stats.handler_registered);

    /* Misuse and non-queueable kinds stay errors. */
    cf_event disconnect;
    memset(&disconnect, 0, sizeof disconnect);
    disconnect.kind = CF_EVENT_DISCONNECT_USER;
    CF_CHECK(cf_job_enqueue(jobs, disconnect) == CF_INVALID);
    cf_event bogus;
    memset(&bogus, 0, sizeof bogus);
    bogus.kind = (cf_event_kind)99;
    CF_CHECK(cf_job_enqueue(jobs, bogus) == CF_INVALID);
    CF_CHECK(cf_job_enqueue(NULL, push_event(1)) == CF_INVALID);
    CF_CHECK(cf_jobs_stats_get(NULL, CF_JOB_PUSH_MESSAGE, &stats) == CF_INVALID);
    CF_CHECK(cf_jobs_stats_get(jobs, (cf_job_kind)99, &stats) == CF_INVALID);
    CF_CHECK(cf_jobs_set_handler(jobs, CF_JOB_PUSH_MESSAGE, NULL, NULL) ==
             CF_INVALID);
    CF_CHECK(cf_jobs_set_handler(NULL, CF_JOB_PUSH_MESSAGE, job_gate_handler,
                                 NULL) == CF_INVALID);

    /* Media payload validation before the media handler exists. */
    CF_CHECK(cf_jobs_enqueue_media(jobs, 1, cf_span_lit(""), cf_span_lit("")) ==
             CF_INVALID);
    cf_span embedded = {(const unsigned char *)"a\0b", 3};
    CF_CHECK(cf_jobs_enqueue_media(jobs, 1, embedded, cf_span_lit("")) ==
             CF_INVALID);

    /* Registration makes the kind usable. */
    job_gate gate;
    job_gate_init(&gate);
    CF_REQUIRE(cf_jobs_set_handler(jobs, CF_JOB_PUSH_MESSAGE,
                                   job_gate_handler, &gate) == CF_OK);
    CF_REQUIRE(cf_jobs_stats_get(jobs, CF_JOB_PUSH_MESSAGE, &stats) == CF_OK);
    CF_CHECK(stats.handler_registered);
    CF_REQUIRE(cf_job_enqueue(jobs, push_event(7)) == CF_OK);
    CF_REQUIRE(cf_jobs_wait_for(jobs, CF_JOB_PUSH_MESSAGE,
                                cf_job_stat_completed, 1, 5000));

    cf_jobs_stop(jobs);
    cf_jobs_destroy(jobs);
    job_gate_dispose(&gate);
    cf_config_destroy(config);
}

/* --- slow handler in one kind never occupies another --------------------- */

CF_TEST(jobs_kinds_stay_isolated_when_one_handler_is_slow) {
    cf_config *config = make_config("128", "2");
    CF_REQUIRE(config != NULL);
    cf_jobs *jobs = NULL;
    CF_REQUIRE(cf_jobs_start(config, &jobs) == CF_OK);

    job_gate push_gate, hook_gate, media_gate;
    job_gate_init(&push_gate);
    job_gate_init(&hook_gate);
    job_gate_init(&media_gate);
    job_gate_block(&push_gate, CF_OK); /* push is the slow class */

    CF_REQUIRE(cf_jobs_set_handler(jobs, CF_JOB_PUSH_MESSAGE,
                                   job_gate_handler, &push_gate) == CF_OK);
    CF_REQUIRE(cf_jobs_set_handler(jobs, CF_JOB_DELIVER_WEBHOOK,
                                   job_gate_handler, &hook_gate) == CF_OK);
    CF_REQUIRE(cf_jobs_set_handler(jobs, CF_JOB_MEDIA, job_gate_handler,
                                   &media_gate) == CF_OK);

    /* Saturate push: two blocked workers, 128 pending, one rejection. */
    CF_REQUIRE(cf_job_enqueue(jobs, push_event(1)) == CF_OK);
    CF_REQUIRE(cf_job_enqueue(jobs, push_event(2)) == CF_OK);
    CF_REQUIRE(job_gate_wait_entered(&push_gate, 2, 5000));
    for (int i = 0; i < 128; i++) {
        CF_REQUIRE(cf_job_enqueue(jobs, push_event(100 + i)) == CF_OK);
    }
    CF_CHECK(cf_job_enqueue(jobs, push_event(999)) == CF_BUSY);

    /* Webhooks and media make progress while push is blocked. */
    for (int i = 1; i <= 5; i++) {
        CF_REQUIRE(cf_job_enqueue(jobs, webhook_event(i)) == CF_OK);
    }
    CF_REQUIRE(cf_jobs_wait_for(jobs, CF_JOB_DELIVER_WEBHOOK,
                                cf_job_stat_completed, 5, 5000));
    CF_REQUIRE(cf_jobs_enqueue_media(jobs, 42, cf_span_lit("analyze"),
                                     cf_span_lit("thumb")) == CF_OK);
    CF_REQUIRE(cf_jobs_wait_for(jobs, CF_JOB_MEDIA, cf_job_stat_completed, 1,
                                5000));

    cf_job_stats push_stats, hook_stats, media_stats;
    CF_REQUIRE(cf_jobs_stats_get(jobs, CF_JOB_PUSH_MESSAGE, &push_stats) ==
               CF_OK);
    CF_REQUIRE(cf_jobs_stats_get(jobs, CF_JOB_DELIVER_WEBHOOK, &hook_stats) ==
               CF_OK);
    CF_REQUIRE(cf_jobs_stats_get(jobs, CF_JOB_MEDIA, &media_stats) == CF_OK);
    CF_CHECK(push_stats.completed == 0); /* still blocked */
    CF_CHECK(push_stats.pending == 128);
    CF_CHECK(push_stats.running == 2);
    CF_CHECK(push_stats.rejected == 1);
    CF_CHECK(hook_stats.accepted == 5);
    CF_CHECK(hook_stats.completed == 5);
    CF_CHECK(hook_stats.rejected == 0);
    CF_CHECK(media_stats.accepted == 1);
    CF_CHECK(media_stats.completed == 1);
    CF_CHECK(media_stats.workers == 4);
    CF_CHECK(strcmp(media_gate.last_task_type, "analyze") == 0);
    CF_CHECK(strcmp(media_gate.last_variation, "thumb") == 0);
    CF_CHECK(media_gate.last_blob_id == 42);

    /* Unused kinds are untouched by another kind's overload. */
    cf_job_stats purge_stats;
    CF_REQUIRE(cf_jobs_stats_get(jobs, CF_JOB_PURGE_BLOB, &purge_stats) ==
               CF_OK);
    CF_CHECK(purge_stats.accepted == 0);

    job_gate_release(&push_gate);
    CF_REQUIRE(cf_jobs_wait_for(jobs, CF_JOB_PUSH_MESSAGE,
                                cf_job_stat_completed, 130, 5000));
    CF_REQUIRE(cf_jobs_stats_get(jobs, CF_JOB_PUSH_MESSAGE, &push_stats) ==
               CF_OK);
    CF_CHECK(push_stats.completed == 130);
    CF_CHECK(push_stats.pending == 0);

    cf_jobs_stop(jobs);
    cf_jobs_destroy(jobs);
    job_gate_dispose(&push_gate);
    job_gate_dispose(&hook_gate);
    job_gate_dispose(&media_gate);
    cf_config_destroy(config);
}

/* --- handler failure: counted once, never retried ------------------------ */

CF_TEST(jobs_handler_failure_counts_failed_without_retry) {
    cf_config *config = make_config(NULL, NULL);
    CF_REQUIRE(config != NULL);
    cf_jobs *jobs = NULL;
    CF_REQUIRE(cf_jobs_start(config, &jobs) == CF_OK);

    job_gate gate;
    job_gate_init(&gate);
    gate.result = CF_IO; /* every purge handler fails */
    CF_REQUIRE(cf_jobs_set_handler(jobs, CF_JOB_PURGE_BLOB, job_gate_handler,
                                   &gate) == CF_OK);
    for (int i = 1; i <= 3; i++) {
        CF_REQUIRE(cf_job_enqueue(jobs, purge_event(i)) == CF_OK);
    }
    CF_REQUIRE(cf_jobs_wait_for(jobs, CF_JOB_PURGE_BLOB, cf_job_stat_failed, 3,
                                5000));
    cf_job_stats stats;
    CF_REQUIRE(cf_jobs_stats_get(jobs, CF_JOB_PURGE_BLOB, &stats) == CF_OK);
    CF_CHECK(stats.accepted == 3);
    CF_CHECK(stats.failed == 3);
    CF_CHECK(stats.completed == 0);
    CF_CHECK(stats.pending == 0);
    CF_CHECK(gate.calls == 3); /* exactly one attempt each: no retry */

    /* A later unrelated event cannot trigger a retry either. */
    job_gate other;
    job_gate_init(&other);
    CF_REQUIRE(cf_jobs_set_handler(jobs, CF_JOB_PUSH_MESSAGE, job_gate_handler,
                                   &other) == CF_OK);
    CF_REQUIRE(cf_job_enqueue(jobs, push_event(1)) == CF_OK);
    CF_REQUIRE(cf_jobs_wait_for(jobs, CF_JOB_PUSH_MESSAGE,
                                cf_job_stat_completed, 1, 5000));
    CF_CHECK(gate.calls == 3);

    cf_jobs_stop(jobs);
    cf_jobs_destroy(jobs);
    job_gate_dispose(&gate);
    job_gate_dispose(&other);
    cf_config_destroy(config);
}

/* --- shutdown discards and counts; no startup recovery -------------------- */

typedef struct {
    cf_jobs *jobs;
} stopper_arg;

static void *stop_thread_main(void *arg) {
    stopper_arg *a = arg;
    cf_jobs_stop(a->jobs);
    return NULL;
}

CF_TEST(jobs_shutdown_discards_pending_counts_and_never_recovers) {
    cf_config *config = make_config("128", "2");
    CF_REQUIRE(config != NULL);
    cf_jobs *jobs = NULL;
    CF_REQUIRE(cf_jobs_start(config, &jobs) == CF_OK);

    job_gate gate;
    job_gate_init(&gate);
    job_gate_block(&gate, CF_OK);
    CF_REQUIRE(cf_jobs_set_handler(jobs, CF_JOB_PUSH_MESSAGE,
                                   job_gate_handler, &gate) == CF_OK);
    CF_REQUIRE(cf_job_enqueue(jobs, push_event(1)) == CF_OK);
    CF_REQUIRE(cf_job_enqueue(jobs, push_event(2)) == CF_OK);
    CF_REQUIRE(job_gate_wait_entered(&gate, 2, 5000));
    for (int i = 0; i < 5; i++) {
        CF_REQUIRE(cf_job_enqueue(jobs, push_event(10 + i)) == CF_OK);
    }
    CF_REQUIRE(cf_jobs_wait_pending(jobs, CF_JOB_PUSH_MESSAGE, 5, 5000));

    /* Stop in another thread; it blocks joining the two running handlers.
     * We wait for the discard to become visible, then release the handlers:
     * pending work can never run once the discard is observable. */
    stopper_arg arg = {.jobs = jobs};
    pthread_t stopper;
    CF_REQUIRE(pthread_create(&stopper, NULL, stop_thread_main, &arg) == 0);
    CF_REQUIRE(cf_jobs_wait_for(jobs, CF_JOB_PUSH_MESSAGE,
                                cf_job_stat_discarded, 5, 5000));
    cf_job_stats stats;
    CF_REQUIRE(cf_jobs_stats_get(jobs, CF_JOB_PUSH_MESSAGE, &stats) == CF_OK);
    CF_CHECK(stats.pending == 0);
    job_gate_release(&gate);
    CF_REQUIRE(pthread_join(stopper, NULL) == 0);

    CF_REQUIRE(cf_jobs_stats_get(jobs, CF_JOB_PUSH_MESSAGE, &stats) == CF_OK);
    CF_CHECK(stats.accepted == 7);
    CF_CHECK(stats.completed == 2); /* the two running jobs finished */
    CF_CHECK(stats.failed == 0);
    CF_CHECK(stats.shutdown_discarded == 5); /* declared queue loss */
    CF_CHECK(stats.pending == 0);
    CF_CHECK(stats.running == 0);

    /* After stop: no new admission, nothing acknowledged as queued. */
    uint64_t accepted_after_stop = stats.accepted;
    CF_CHECK(cf_job_enqueue(jobs, push_event(50)) == CF_BUSY);
    CF_REQUIRE(cf_jobs_stats_get(jobs, CF_JOB_PUSH_MESSAGE, &stats) == CF_OK);
    CF_CHECK(stats.accepted == accepted_after_stop);
    CF_CHECK(stats.rejected == 0); /* stop refusal is not a queue-full reject */
    cf_jobs_stop(jobs);            /* idempotent */

    /* A fresh start is empty: no recovery or replay of the discarded jobs. */
    cf_jobs *fresh = NULL;
    CF_REQUIRE(cf_jobs_start(config, &fresh) == CF_OK);
    job_gate fresh_gate;
    job_gate_init(&fresh_gate);
    CF_REQUIRE(cf_jobs_set_handler(fresh, CF_JOB_PUSH_MESSAGE,
                                   job_gate_handler, &fresh_gate) == CF_OK);
    CF_REQUIRE(cf_jobs_stats_get(fresh, CF_JOB_PUSH_MESSAGE, &stats) == CF_OK);
    CF_CHECK(stats.accepted == 0);
    CF_CHECK(stats.completed == 0);
    CF_CHECK(stats.shutdown_discarded == 0);
    cf_jobs_stop(fresh);
    cf_jobs_destroy(fresh);
    CF_CHECK(fresh_gate.calls == 0);

    cf_jobs_destroy(jobs);
    job_gate_dispose(&gate);
    job_gate_dispose(&fresh_gate);
    cf_config_destroy(config);
}

CF_TEST(jobs_destroy_stops_and_releases) {
    cf_config *config = make_config(NULL, NULL);
    CF_REQUIRE(config != NULL);
    cf_jobs *jobs = NULL;
    CF_REQUIRE(cf_jobs_start(config, &jobs) == CF_OK);
    job_gate gate;
    job_gate_init(&gate);
    CF_REQUIRE(cf_jobs_set_handler(jobs, CF_JOB_PUSH_MESSAGE,
                                   job_gate_handler, &gate) == CF_OK);
    for (int i = 1; i <= 4; i++) {
        CF_REQUIRE(cf_job_enqueue(jobs, push_event(i)) == CF_OK);
    }
    /* Destroy without an explicit stop must stop, join and free (ASan/TSan
     * runs verify the release; no counters are asserted after destroy). */
    cf_jobs_destroy(jobs);
    job_gate_dispose(&gate);
    cf_config_destroy(config);
}

CF_TEST_MAIN()
