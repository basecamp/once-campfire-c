/* tests/auth/test_password.c — A01's password service at the model boundary:
 * cf_user_authenticate / cf_user_authenticated (src/models/user.c) via
 * cf_password_verify, the pinned 72-byte bcrypt truncation, the injected test
 * cost (config injects 4; never an environment name), and the bounded crypto
 * queue (P12-04/P12-06: worker bound, fixed 32 pending capacity, saturation,
 * unconditional refusal on every failure path, counted discards, no inline
 * bcrypt once a queue has been started).
 */
#include "cf_test.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "models/user.h"

#include "support/auth_env.h"
#include "vectors.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#define LONG_72 \
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define LONG_79 LONG_72 "ignored"
#define LONG_71 \
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

/* The pinned cost-12 digest of "secret123456" (also used by the dummy case
 * above): the queue cases verify against it instead of creating a digest of
 * their own, so their submitted/completed counts cover exactly the jobs
 * under test. */
#define SEEDED_DIGEST \
    "$2a$12$kDVPfUd.VKsV9HzG8RHRFu0fyaW7gije1Krza2.A0J5S8XEcOQT4S"

/* Leave no queue, observer or injected copy failure behind from an earlier
 * case (a failed case may have aborted with references outstanding). */
static void crypto_queue_test_stop_all(void) {
    cf_crypto_queue_set_test_copy_failure(0);
    cf_crypto_queue_set_test_observer(NULL, NULL);
    cf_crypto_queue_stats stats;
    for (int i = 0;
         i < 16 && cf_crypto_queue_stats_get(&stats) && stats.running; i++) {
        cf_crypto_queue_stop();
    }
}

/* Enter the never-started-queue inline mode for a case.  Test-only: the
 * requirement is sticky in production, but Fil-C registers cases without
 * source order, so the inline cases cannot rely on running before the queue
 * cases.  The stickiness itself is covered by
 * bcrypt_after_a_stopped_queue_fails_closed_without_inline. */
static void crypto_queue_test_use_inline_mode(void) {
    crypto_queue_test_stop_all();
    cf_crypto_queue_test_reset_required();
}

CF_TEST(config_injects_the_test_bcrypt_cost) {
    cf_config_entry entries[] = {
        {"PUBLIC_ORIGIN", "http://127.0.0.1:32123"},
        {"SECRET_KEY_BASE", AUTH_VEC_SECRET_KEY_BASE},
    };
    cf_config *config = NULL;
    CF_REQUIRE(cf_config_parse(entries, 2, NULL, &config) == CF_OK);
    CF_CHECK(config->bcrypt_cost == CF_BCRYPT_COST);
    cf_config_test_set_bcrypt_cost(config, 4);
    CF_CHECK(config->bcrypt_cost == 4);
    cf_config_test_set_bcrypt_cost(config, 3);
    CF_CHECK(config->bcrypt_cost == 4); /* outside 4..31 is ignored */
    cf_config_destroy(config);
}

CF_TEST(user_authenticate_honors_the_72_byte_limit) {
    crypto_queue_test_use_inline_mode();
    cf_user user;
    memset(&user, 0, sizeof user);
    /* The pinned digest of 72 'a's. */
    static const char digest[] =
        "$2a$12$TZrwCKzycdLOZ2HeWOMW1OYHE8nrAWHoOY05FWLQ6N38GSUKn4DMW";
    user.password_digest = (cf_optional_str){
        .present = true,
        .value = {.ptr = (char *)digest, .len = sizeof digest - 1}};

    bool ok = false;
    CF_REQUIRE(cf_user_authenticate(&user, CF_STR_LIT(LONG_72), &ok) == CF_OK);
    CF_CHECK(ok);
    ok = false;
    CF_REQUIRE(cf_user_authenticate(&user, CF_STR_LIT(LONG_79), &ok) == CF_OK);
    CF_CHECK(ok); /* bytes beyond 72 are ignored */
    ok = true;
    CF_REQUIRE(cf_user_authenticate(&user, CF_STR_LIT(LONG_71), &ok) == CF_OK);
    CF_CHECK(!ok);
    ok = true;
    CF_REQUIRE(cf_user_authenticate(&user, CF_STR_LIT("anything"), &ok) == CF_OK);
    CF_CHECK(!ok);

    /* Absent or empty digests never verify. */
    cf_user none;
    memset(&none, 0, sizeof none);
    ok = true;
    CF_REQUIRE(cf_user_authenticate(&none, CF_STR_LIT("x"), &ok) == CF_OK);
    CF_CHECK(!ok);
    none.password_digest = (cf_optional_str){.present = true, .value = {NULL, 0}};
    ok = true;
    CF_REQUIRE(cf_user_authenticate(&none, CF_STR_LIT("x"), &ok) == CF_OK);
    CF_CHECK(!ok);
}

CF_TEST(authenticated_runs_the_dummy_for_an_unknown_user) {
    crypto_queue_test_use_inline_mode();
    bool ok = true;
    /* candidate NULL: the constant dummy verification runs, result false. */
    CF_REQUIRE(cf_user_authenticated(NULL, CF_STR_LIT("secret123456"), &ok) ==
               CF_OK);
    CF_CHECK(!ok);
    /* Empty password: no lookup, no verification. */
    ok = true;
    CF_REQUIRE(cf_user_authenticated(NULL, CF_STR_LIT(""), &ok) == CF_OK);
    CF_CHECK(!ok);

    /* A real candidate with the pinned seeded digest verifies. */
    cf_user user;
    memset(&user, 0, sizeof user);
    static const char digest[] =
        "$2a$12$kDVPfUd.VKsV9HzG8RHRFu0fyaW7gije1Krza2.A0J5S8XEcOQT4S";
    user.password_digest = (cf_optional_str){
        .present = true,
        .value = {.ptr = (char *)digest, .len = sizeof digest - 1}};
    ok = false;
    CF_REQUIRE(cf_user_authenticated(&user, CF_STR_LIT("secret123456"), &ok) ==
               CF_OK);
    CF_CHECK(ok);
}

CF_TEST(digest_creation_and_round_trip_at_cost_4) {
    crypto_queue_test_use_inline_mode();
    cf_str digest = {0};
    CF_REQUIRE(cf_auth_password_digest(CF_STR_LIT("secret123456"), 4, &digest) ==
               CF_OK);
    CF_CHECK(digest.len == 60);
    CF_CHECK(strncmp(digest.ptr, "$2a$04$", 7) == 0);
    CF_CHECK(cf_password_verify(CF_STR_LIT("secret123456"), digest));
    CF_CHECK(!cf_password_verify(CF_STR_LIT("wrong"), digest));
    cf_str_dispose(&digest);
    CF_CHECK(cf_auth_password_digest(CF_STR_LIT("x"),
                                     CF_AUTH_BCRYPT_MIN_COST - 1,
                                     &digest) == CF_INVALID);
    CF_CHECK(cf_auth_password_digest(CF_STR_LIT("x"),
                                     CF_AUTH_BCRYPT_MAX_COST + 1,
                                     &digest) == CF_INVALID);
}

/* The inline mode exists only in a process that never starts the queue
 * (unit tests, tools).  Once a queue has been started, a bcrypt call is
 * refused and counted instead of hashing inline. */
CF_TEST(bcrypt_runs_inline_only_before_the_first_queue_start) {
    crypto_queue_test_use_inline_mode();
    cf_crypto_queue_stats before;
    CF_REQUIRE(cf_crypto_queue_stats_get(&before));
    CF_CHECK(!before.running);

    cf_str digest = {0};
    CF_REQUIRE(cf_auth_password_digest(CF_STR_LIT("secret123456"), 4,
                                       &digest) == CF_OK);
    CF_CHECK(cf_password_verify(CF_STR_LIT("secret123456"), digest));
    CF_CHECK(!cf_password_verify(CF_STR_LIT("wrong"), digest));
    /* A malformed digest still reaches crypt_r and is decided false. */
    CF_CHECK(!cf_password_verify(CF_STR_LIT("secret123456"),
                                 CF_STR_LIT("not a digest")));

    cf_crypto_queue_stats after;
    CF_REQUIRE(cf_crypto_queue_stats_get(&after));
    CF_CHECK(after.submitted == before.submitted);
    CF_CHECK(after.completed == before.completed);
    CF_CHECK(after.refused == before.refused);
    CF_CHECK(after.unprepared == before.unprepared);
    CF_CHECK(after.inline_fallbacks == before.inline_fallbacks + 4);
    cf_str_dispose(&digest);
}

/* ---- bounded crypto queue (P12-04 / P12-06) ------------------------------ */

/* The test observer records the high-water number of jobs holding a worker
 * slot, signals that a worker has claimed a job (`observed`), can sleep, and
 * can hold the slot on a release gate.  The stop cases use the gate plus
 * `observed`: the test waits for the worker to be provably in the observer,
 * starts the stop, waits for the queue to be stopping, and only then opens
 * the gate, so the worker can finish its claimed job but cannot take a queued
 * one.  Counters therefore do not depend on machine timing; a bounded wait
 * (see CRYPTO_TEST_WAIT_NS) fails a regressed case instead of hanging it.
 *
 * Harness rule for every case below: no fatal assertion may fire while a
 * spawned caller thread is still running, because CF_REQUIRE longjmps out of
 * the test frame that owns the task storage, stranding the caller on a
 * retired frame (verified root cause of the P12-06 stop-test SIGSEGV).  All
 * waits/joins use CF_CHECK and the case checks its accounting only after
 * every spawned thread has been joined. */
typedef struct {
    _Atomic int active;
    _Atomic int max_active;
    _Atomic int observed; /* observer entries (a worker claimed a job) */
    long sleep_ns;
    bool gated;      /* observer waits for gate_open before releasing the slot */
    bool gate_open;
    pthread_mutex_t gate_mutex;
    pthread_cond_t gate_cv;
} crypto_probe;

static void crypto_probe_init(crypto_probe *probe, long sleep_ns, bool gated) {
    memset(probe, 0, sizeof *probe);
    probe->sleep_ns = sleep_ns;
    probe->gated = gated;
    pthread_mutex_init(&probe->gate_mutex, NULL);
    pthread_cond_init(&probe->gate_cv, NULL);
}

static void crypto_probe_dispose(crypto_probe *probe) {
    pthread_mutex_destroy(&probe->gate_mutex);
    pthread_cond_destroy(&probe->gate_cv);
}

static void crypto_probe_gate_open(crypto_probe *probe) {
    pthread_mutex_lock(&probe->gate_mutex);
    probe->gate_open = true;
    pthread_cond_broadcast(&probe->gate_cv);
    pthread_mutex_unlock(&probe->gate_mutex);
}

static void crypto_probe_observe(void *ctx) {
    crypto_probe *probe = ctx;
    int active = atomic_fetch_add_explicit(&probe->active, 1,
                                           memory_order_acq_rel) + 1;
    int seen = atomic_load_explicit(&probe->max_active, memory_order_acquire);
    while (active > seen &&
           !atomic_compare_exchange_weak_explicit(
               &probe->max_active, &seen, active, memory_order_acq_rel,
               memory_order_acquire)) {
    }
    atomic_fetch_add_explicit(&probe->observed, 1, memory_order_release);
    if (probe->gated) {
        pthread_mutex_lock(&probe->gate_mutex);
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 30;
        while (!probe->gate_open) {
            if (pthread_cond_timedwait(&probe->gate_cv, &probe->gate_mutex,
                                       &deadline) != 0) {
                break; /* doomed case: never hang the binary */
            }
        }
        pthread_mutex_unlock(&probe->gate_mutex);
    }
    if (probe->sleep_ns > 0) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = probe->sleep_ns};
        nanosleep(&pause, NULL);
    }
    atomic_fetch_sub_explicit(&probe->active, 1, memory_order_acq_rel);
}

/* Harness waits under stress are not product behavior: 30 s is long enough
 * that a loaded machine does not turn a pass into a failure, and a genuine
 * regression still fails rather than hangs (the gate's own 30 s escape hatch
 * bounds the other side). */
#define CRYPTO_TEST_WAIT_NS 30000000000L /* 30 s */

/* Poll `pred` until true or the deadline.  Never fatal. */
static bool crypto_poll_deadline(bool (*pred)(void *), void *ctx) {
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        if (pred(ctx)) return true;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000000000L +
                       (now.tv_nsec - start.tv_nsec);
        if (elapsed > CRYPTO_TEST_WAIT_NS) return false;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
        nanosleep(&pause, NULL);
    }
}

/* "Stop case ready": a worker is provably in the observer (its slot cannot
 * free until the gate opens) and the expected number of jobs is queued and
 * submitters are blocked on capacity.  Both counters are monotone while the
 * worker is gated, so reaching the values means every spawned caller is
 * inside crypto_queue_run. */
typedef struct {
    crypto_probe *probe;
    size_t pending;
    size_t saturated;
} crypto_stop_ready;

static bool crypto_stop_ready_pred(void *ctx) {
    crypto_stop_ready *ready = ctx;
    if (atomic_load_explicit(&ready->probe->observed,
                             memory_order_acquire) < 1) {
        return false;
    }
    cf_crypto_queue_stats stats;
    return cf_crypto_queue_stats_get(&stats) &&
           stats.pending_max >= ready->pending &&
           stats.saturated >= ready->saturated;
}

/* True once the stop has begun (running is false from the moment stopping is
 * set, before the workers are joined). */
static bool crypto_stopping_pred(void *ctx) {
    (void)ctx;
    cf_crypto_queue_stats stats;
    return cf_crypto_queue_stats_get(&stats) && !stats.running;
}

/* Runs the stop on its own thread so the main thread can open the gate once
 * stopping is set (the stopper's worker join needs the gate open). */
static void *crypto_stop_thread(void *arg) {
    (void)arg;
    cf_crypto_queue_stop();
    return NULL;
}

/* Issue a stop while the gated worker holds its claimed slot: the stopper
 * sets `stopping`, the main thread waits for that, and only then opens the
 * gate, so the worker finishes the claimed job and exits without taking a
 * queued one.  Never fatal; the gate's timed wait bounds the other side. */
static bool crypto_run_gated_stop(crypto_probe *probe) {
    pthread_t stopper;
    bool stopper_ok =
        pthread_create(&stopper, NULL, crypto_stop_thread, NULL) == 0;
    CF_CHECK(stopper_ok);
    if (stopper_ok) {
        (void)crypto_poll_deadline(crypto_stopping_pred, NULL);
        crypto_probe_gate_open(probe);
        CF_CHECK(pthread_join(stopper, NULL) == 0);
    } else {
        /* Practically unreachable; still stop after releasing the worker. */
        crypto_probe_gate_open(probe);
        cf_crypto_queue_stop();
    }
    return stopper_ok;
}

typedef struct {
    cf_str password;
    cf_str digest;
    int rounds;
    bool all_ok;
} crypto_verify_task;

static void *crypto_verify_thread(void *arg) {
    crypto_verify_task *task = arg;
    task->all_ok = true;
    for (int i = 0; i < task->rounds; i++) {
        if (!cf_password_verify(task->password, task->digest)) {
            task->all_ok = false;
        }
    }
    return NULL;
}

/* Create up to `count` verify workers, each running `rounds` verifies, and
 * return how many were created.  No assertion here: a fatal one would strand
 * the already-created callers on this frame (see the harness rule above); the
 * caller joins `created` threads and then checks the count. */
static int crypto_spawn_verify_threads(crypto_verify_task *tasks,
                                       pthread_t *threads, int count,
                                       cf_str password, cf_str digest,
                                       int rounds) {
    int created = 0;
    for (int i = 0; i < count; i++) {
        tasks[i] = (crypto_verify_task){.password = password,
                                        .digest = digest,
                                        .rounds = rounds,
                                        .all_ok = false};
        if (pthread_create(&threads[i], NULL, crypto_verify_thread,
                           &tasks[i]) != 0) {
            break;
        }
        created++;
    }
    return created;
}

/* Join `count` spawned callers and check each verified successfully.  Joins
 * are CF_CHECK so one failure cannot abandon the remaining threads. */
static void crypto_join_verify_threads(crypto_verify_task *tasks,
                                       pthread_t *threads, int count) {
    for (int i = 0; i < count; i++) {
        CF_CHECK(pthread_join(threads[i], NULL) == 0);
        CF_CHECK(tasks[i].all_ok);
    }
}

static void crypto_run_verify_threads(crypto_verify_task *tasks,
                                      pthread_t *threads, int count,
                                      cf_str password, cf_str digest,
                                      int rounds) {
    int created = crypto_spawn_verify_threads(tasks, threads, count, password,
                                              digest, rounds);
    crypto_join_verify_threads(tasks, threads, created);
    CF_CHECK(created == count);
}

/* The queue is one per process and concurrent app instances share it; the
 * first start fixes the worker count and the last stop stops it. */
CF_TEST(crypto_queue_is_shared_by_concurrent_app_instances) {
    crypto_queue_test_stop_all();
    CF_REQUIRE(cf_crypto_queue_start(2) == CF_OK);
    CF_REQUIRE(cf_crypto_queue_start(2) == CF_OK); /* second app: shared */
    CF_CHECK(cf_crypto_queue_start(1) == CF_BUSY); /* count mismatch */

    cf_crypto_queue_stats stats;
    CF_REQUIRE(cf_crypto_queue_stats_get(&stats));
    CF_CHECK(stats.running && stats.workers == 2);

    cf_crypto_queue_stop(); /* first app leaves */
    CF_REQUIRE(cf_crypto_queue_stats_get(&stats));
    CF_CHECK(stats.running);

    cf_crypto_queue_stop(); /* last app leaves: queue stops */
    CF_REQUIRE(cf_crypto_queue_stats_get(&stats));
    CF_CHECK(!stats.running);
}

/* The setup call site (src/actions/first_runs.c) runs on the queue, and one
 * configured worker serializes every execution. */
CF_TEST(crypto_queue_routes_setup_hashing_and_one_worker_serializes) {
    crypto_queue_test_stop_all();
    crypto_probe probe;
    crypto_probe_init(&probe, 2000000, false); /* 2 ms per job across the slot */
    cf_crypto_queue_set_test_observer(crypto_probe_observe, &probe);
    CF_REQUIRE(cf_crypto_queue_start(1) == CF_OK);

    cf_str digest = {0};
    CF_REQUIRE(cf_auth_password_digest(CF_STR_LIT("secret123456"), 4,
                                       &digest) == CF_OK);
    CF_CHECK(digest.len == 60);
    CF_CHECK(cf_password_verify(CF_STR_LIT("secret123456"), digest));

    enum { THREADS = 4, ROUNDS = 2 };
    pthread_t threads[THREADS];
    crypto_verify_task tasks[THREADS];
    crypto_run_verify_threads(tasks, threads, THREADS,
                              CF_STR_LIT("secret123456"), digest, ROUNDS);

    cf_crypto_queue_stats stats;
    CF_REQUIRE(cf_crypto_queue_stats_get(&stats));
    CF_CHECK(stats.workers == 1);
    CF_CHECK(stats.pending_capacity == 32);
    CF_CHECK(stats.submitted == 2 + THREADS * ROUNDS);
    CF_CHECK(stats.completed == stats.submitted);
    CF_CHECK(stats.running_max == 1);
    CF_CHECK(atomic_load(&probe.max_active) == 1);
    CF_CHECK(stats.refused == 0);
    CF_CHECK(stats.discarded == 0);
    CF_CHECK(stats.unprepared == 0);
    CF_CHECK(stats.inline_fallbacks == 0);
    CF_CHECK(stats.running);

    cf_crypto_queue_set_test_observer(NULL, NULL);
    crypto_queue_test_stop_all();
    CF_REQUIRE(cf_crypto_queue_stats_get(&stats));
    CF_CHECK(!stats.running);
    crypto_probe_dispose(&probe);
    cf_str_dispose(&digest);
}

/* Two configured workers really overlap; the bound is the worker count. */
CF_TEST(crypto_queue_two_workers_overlap_up_to_the_bound) {
    crypto_queue_test_stop_all();
    crypto_probe probe;
    crypto_probe_init(&probe, 20000000, false); /* 20 ms holds both slots */
    cf_crypto_queue_set_test_observer(crypto_probe_observe, &probe);
    CF_REQUIRE(cf_crypto_queue_start(2) == CF_OK);

    enum { THREADS = 4, ROUNDS = 2 };
    pthread_t threads[THREADS];
    crypto_verify_task tasks[THREADS];
    crypto_run_verify_threads(tasks, threads, THREADS,
                              CF_STR_LIT("secret123456"),
                              CF_STR_LIT(SEEDED_DIGEST), ROUNDS);

    cf_crypto_queue_stats stats;
    CF_REQUIRE(cf_crypto_queue_stats_get(&stats));
    CF_CHECK(stats.workers == 2);
    CF_CHECK(stats.running_max == 2);
    CF_CHECK(atomic_load(&probe.max_active) == 2);
    CF_CHECK(stats.submitted == THREADS * ROUNDS);
    CF_CHECK(stats.completed == stats.submitted);
    CF_CHECK(stats.discarded == 0);
    CF_CHECK(stats.refused == 0);
    CF_CHECK(stats.unprepared == 0);
    CF_CHECK(stats.inline_fallbacks == 0); /* a started queue never inlines */

    cf_crypto_queue_set_test_observer(NULL, NULL);
    crypto_queue_test_stop_all();
    crypto_probe_dispose(&probe);
}

/* More submissions than one worker plus 32 pending slots: submitters block
 * (counted as saturated), nothing is refused or dropped, and the pending
 * high-water stays exactly at the fixed bound. */
CF_TEST(crypto_queue_saturation_stays_within_the_32_job_bound) {
    crypto_queue_test_stop_all();
    crypto_probe probe;
    crypto_probe_init(&probe, 10000000, false); /* 10 ms per job keeps the
                                                 * queue full while the
                                                 * submitter threads start */
    cf_crypto_queue_set_test_observer(crypto_probe_observe, &probe);
    CF_REQUIRE(cf_crypto_queue_start(1) == CF_OK);

    /* One blocked submitter holds one job, so saturating the 32-slot queue
     * needs more than 33 concurrent submitters. */
    enum { THREADS = 40, ROUNDS = 1 };
    pthread_t threads[THREADS];
    crypto_verify_task tasks[THREADS];
    crypto_run_verify_threads(tasks, threads, THREADS,
                              CF_STR_LIT("secret123456"),
                              CF_STR_LIT(SEEDED_DIGEST), ROUNDS);

    cf_crypto_queue_stats stats;
    CF_REQUIRE(cf_crypto_queue_stats_get(&stats));
    CF_CHECK(stats.workers == 1);
    CF_CHECK(stats.pending_capacity == 32);
    CF_CHECK(stats.submitted == THREADS * ROUNDS);
    CF_CHECK(stats.completed == stats.submitted);
    CF_CHECK(stats.pending_max == 32);
    CF_CHECK(stats.saturated >= 1);
    CF_CHECK(stats.refused == 0);
    CF_CHECK(stats.discarded == 0);
    CF_CHECK(stats.unprepared == 0);
    CF_CHECK(stats.inline_fallbacks == 0);
    CF_CHECK(atomic_load(&probe.max_active) == 1);

    cf_crypto_queue_set_test_observer(NULL, NULL);
    crypto_queue_test_stop_all();
    crypto_probe_dispose(&probe);
}

/* P12-06: once a queue has been started, a job that cannot be prepared (the
 * owned-input copy allocation fails) must fail the caller, never run bcrypt
 * on the submitting thread.  The injected copy failure models the allocation
 * failure while the queue is running; the observer proves no worker ran. */
CF_TEST(crypto_queue_job_allocation_failure_fails_closed_without_inline) {
    crypto_queue_test_stop_all();
    crypto_probe probe;
    crypto_probe_init(&probe, 0, false);
    cf_crypto_queue_set_test_observer(crypto_probe_observe, &probe);
    CF_REQUIRE(cf_crypto_queue_start(1) == CF_OK);

    cf_crypto_queue_stats before;
    CF_REQUIRE(cf_crypto_queue_stats_get(&before));

    enum { THREADS = 8 };
    pthread_t threads[THREADS];
    crypto_verify_task tasks[THREADS];
    cf_crypto_queue_set_test_copy_failure(THREADS);
    int created = crypto_spawn_verify_threads(
        tasks, threads, THREADS, CF_STR_LIT("secret123456"),
        CF_STR_LIT(SEEDED_DIGEST), 1);
    for (int i = 0; i < created; i++) {
        CF_CHECK(pthread_join(threads[i], NULL) == 0);
        CF_CHECK(!tasks[i].all_ok); /* documented failure: false, not inline */
    }
    cf_crypto_queue_set_test_copy_failure(0); /* no armed failure may remain */
    CF_CHECK(created == THREADS);

    cf_crypto_queue_stats stats;
    CF_REQUIRE(cf_crypto_queue_stats_get(&stats));
    CF_CHECK(stats.unprepared == before.unprepared + THREADS);
    CF_CHECK(stats.submitted == before.submitted); /* nothing entered the queue */
    CF_CHECK(stats.completed == before.completed);
    CF_CHECK(stats.refused == before.refused);
    CF_CHECK(stats.discarded == before.discarded);
    CF_CHECK(stats.running_max == 0);
    CF_CHECK(atomic_load(&probe.max_active) == 0); /* no bcrypt anywhere */
    CF_CHECK(stats.inline_fallbacks == before.inline_fallbacks);

    /* Disarmed, the same boundary executes on the worker. */
    CF_CHECK(cf_password_verify(CF_STR_LIT("secret123456"),
                                CF_STR_LIT(SEEDED_DIGEST)));
    CF_REQUIRE(cf_crypto_queue_stats_get(&stats));
    CF_CHECK(stats.submitted == before.submitted + 1);
    CF_CHECK(stats.completed == before.completed + 1);
    CF_CHECK(stats.unprepared == before.unprepared + THREADS);
    CF_CHECK(atomic_load(&probe.max_active) == 1);

    crypto_queue_test_stop_all();
    crypto_probe_dispose(&probe);
}

/* P12-06: the digest wrapper has an error channel, so an unprepared job
 * returns its CF_NOMEM with *out cleared (setup then fails before cf_write). */
CF_TEST(crypto_queue_digest_job_allocation_failure_returns_nomem) {
    crypto_queue_test_stop_all();
    CF_REQUIRE(cf_crypto_queue_start(1) == CF_OK);

    cf_crypto_queue_stats before;
    CF_REQUIRE(cf_crypto_queue_stats_get(&before));

    cf_crypto_queue_set_test_copy_failure(1);
    cf_str out = CF_STR_LIT("sentinel"); /* must be cleared on failure */
    CF_CHECK(cf_auth_password_digest(CF_STR_LIT("secret123456"), 4, &out) ==
             CF_NOMEM);
    CF_CHECK(out.ptr == NULL && out.len == 0);

    cf_crypto_queue_stats stats;
    CF_REQUIRE(cf_crypto_queue_stats_get(&stats));
    CF_CHECK(stats.unprepared == before.unprepared + 1);
    CF_CHECK(stats.submitted == before.submitted);
    CF_CHECK(stats.completed == before.completed);
    CF_CHECK(stats.refused == before.refused);
    CF_CHECK(stats.inline_fallbacks == before.inline_fallbacks);

    cf_str digest = {0};
    CF_REQUIRE(cf_auth_password_digest(CF_STR_LIT("secret123456"), 4,
                                       &digest) == CF_OK); /* disarmed: queued */
    CF_CHECK(cf_password_verify(CF_STR_LIT("secret123456"), digest));
    cf_str_dispose(&digest);
    crypto_queue_test_stop_all();
}

/* P12-06: a stop discards queued jobs and wakes their callers with CF_BUSY;
 * each caller must receive the documented failure instead of hashing inline.
 * One bcrypt runs per worker that had already claimed a slot.
 *
 * No fatal assertion runs while a spawned caller is unjoined (see the
 * harness rule): creating, waiting, stopping and joining use CF_CHECK, and
 * the accounting is checked after every thread is joined. */
CF_TEST(crypto_queue_stop_discards_queued_work_without_losing_callers) {
    crypto_queue_test_stop_all();
    crypto_probe probe;
    crypto_probe_init(&probe, 0, true);
    cf_crypto_queue_set_test_observer(crypto_probe_observe, &probe);
    CF_REQUIRE(cf_crypto_queue_start(1) == CF_OK);

    enum { BLOCKERS = 5 };
    pthread_t running_threads[1];
    crypto_verify_task running_tasks[1];
    int running_created = crypto_spawn_verify_threads(
        running_tasks, running_threads, 1, CF_STR_LIT("secret123456"),
        CF_STR_LIT(SEEDED_DIGEST), 1);
    /* The worker must claim the running caller's job (the only job queued at
     * this point) and be held in the observer before the blockers exist. */
    crypto_stop_ready claimed = {.probe = &probe, .pending = 0,
                                 .saturated = 0};
    bool claimed_ok = running_created == 1 &&
                      crypto_poll_deadline(crypto_stop_ready_pred, &claimed);
    pthread_t blockers[BLOCKERS];
    crypto_verify_task blocked[BLOCKERS];
    int blockers_created = 0;
    if (running_created == 1) {
        blockers_created = crypto_spawn_verify_threads(
            blocked, blockers, BLOCKERS, CF_STR_LIT("secret123456"),
            CF_STR_LIT(SEEDED_DIGEST), 1);
    }

    /* Ready only when all five blocker jobs are queued: every spawned caller
     * is then inside the queue (the gated worker cannot free a slot). */
    crypto_stop_ready ready = {.probe = &probe, .pending = BLOCKERS,
                               .saturated = 0};
    bool ready_ok = blockers_created == BLOCKERS &&
                    crypto_poll_deadline(crypto_stop_ready_pred, &ready);
    cf_crypto_queue_stats before_stop = {0};
    (void)cf_crypto_queue_stats_get(&before_stop);

    (void)crypto_run_gated_stop(&probe);

    for (int i = 0; i < running_created; i++) {
        CF_CHECK(pthread_join(running_threads[i], NULL) == 0);
    }
    for (int i = 0; i < blockers_created; i++) {
        CF_CHECK(pthread_join(blockers[i], NULL) == 0);
    }
    /* Every spawned thread is joined: the checks below cannot strand one. */
    CF_CHECK(running_created == 1 && blockers_created == BLOCKERS);
    CF_CHECK(claimed_ok);
    CF_CHECK(ready_ok);
    CF_CHECK(running_created == 1 && running_tasks[0].all_ok);
    for (int i = 0; i < blockers_created; i++) {
        CF_CHECK(!blocked[i].all_ok); /* CF_BUSY: documented failure */
    }

    cf_crypto_queue_stats stats;
    CF_REQUIRE(cf_crypto_queue_stats_get(&stats));
    CF_CHECK(!stats.running);
    CF_CHECK(stats.submitted == 1 + BLOCKERS);
    CF_CHECK(stats.completed == 1);
    CF_CHECK(stats.discarded == BLOCKERS);
    CF_CHECK(stats.refused == 0);
    CF_CHECK(stats.unprepared == 0);
    CF_CHECK(stats.inline_fallbacks == before_stop.inline_fallbacks);
    crypto_queue_test_stop_all();
    crypto_probe_dispose(&probe);
}

/* Stop while 32 jobs are queued and 7 submitters wait for capacity: the
 * running job completes, the queued work is discarded and counted, the
 * blocked submitters are refused and counted, and every caller either got the
 * worker's answer (exactly one) or the documented failure (the other 39).
 * Nothing hangs, nothing is dropped silently, nothing hashes inline.
 *
 * No fatal assertion runs while a spawned caller is unjoined: the 2 s
 * `running_max == 1` poll that used to longjmp and strand 40 callers on the
 * retired test frame is replaced by a gated, monotone readiness condition
 * (worker in the observer, 32 queued, 7 capacity waiters) polled with
 * CF_CHECK up to 30 s, followed by the stop and a join of every caller
 * before any accounting check.  A regression fails the case; it cannot
 * leave a live thread behind. */
CF_TEST(crypto_queue_stop_refuses_space_blocked_submitters) {
    crypto_queue_test_stop_all();
    crypto_probe probe;
    crypto_probe_init(&probe, 0, true);
    cf_crypto_queue_set_test_observer(crypto_probe_observe, &probe);
    CF_REQUIRE(cf_crypto_queue_start(1) == CF_OK);

    enum { CALLERS = 40 };
    pthread_t threads[CALLERS];
    crypto_verify_task tasks[CALLERS];
    int created = crypto_spawn_verify_threads(
        tasks, threads, CALLERS, CF_STR_LIT("secret123456"),
        CF_STR_LIT(SEEDED_DIGEST), 1);

    /* pending_max == 32 (the fixed cap) and 7 capacity waits mean: 1 job in
     * the gated worker's slot, 32 queued, and the remaining 7 callers blocked
     * in crypto_queue_run with nothing able to free a slot before the stop. */
    crypto_stop_ready ready = {.probe = &probe, .pending = 32,
                               .saturated = CALLERS - 33};
    bool ready_ok = created == CALLERS &&
                    crypto_poll_deadline(crypto_stop_ready_pred, &ready);
    cf_crypto_queue_stats before_stop = {0};
    (void)cf_crypto_queue_stats_get(&before_stop);

    (void)crypto_run_gated_stop(&probe);

    int verified = 0;
    for (int i = 0; i < created; i++) {
        CF_CHECK(pthread_join(threads[i], NULL) == 0);
        if (tasks[i].all_ok) verified++;
    }
    /* Every spawned caller is joined: the checks below cannot strand one. */
    CF_CHECK(created == CALLERS);
    CF_CHECK(ready_ok);
    CF_CHECK(verified == 1); /* only the joined worker's job completed */
    cf_crypto_queue_stats stats;
    CF_REQUIRE(cf_crypto_queue_stats_get(&stats));
    CF_CHECK(!stats.running);
    CF_CHECK(stats.submitted == 33);   /* 1 running + 32 queued */
    CF_CHECK(stats.completed == 1);
    CF_CHECK(stats.discarded == 32);
    CF_CHECK(stats.refused == CALLERS - 33);
    CF_CHECK(stats.unprepared == 0);
    CF_CHECK(stats.inline_fallbacks == before_stop.inline_fallbacks);
    crypto_queue_test_stop_all();
    crypto_probe_dispose(&probe);
}

/* P12-06: after the queue has stopped, the requirement stays sticky: every
 * bcrypt boundary refuses (verify fails closed, digest returns CF_BUSY) and
 * nothing runs inline. */
CF_TEST(bcrypt_after_a_stopped_queue_fails_closed_without_inline) {
    crypto_queue_test_stop_all();
    CF_REQUIRE(cf_crypto_queue_start(1) == CF_OK);
    crypto_queue_test_stop_all();

    cf_crypto_queue_stats before;
    CF_REQUIRE(cf_crypto_queue_stats_get(&before));
    CF_CHECK(!before.running);

    /* cf_user_authenticated maps the refused verification to a fail-closed
     * non-match and still returns CF_OK (the ratified bool boundary has no
     * error channel). */
    cf_user user;
    memset(&user, 0, sizeof user);
    static const char seeded[] = SEEDED_DIGEST;
    user.password_digest = (cf_optional_str){
        .present = true,
        .value = {.ptr = (char *)seeded, .len = sizeof seeded - 1}};
    bool ok = true;
    CF_REQUIRE(cf_user_authenticated(&user, CF_STR_LIT("secret123456"), &ok) ==
               CF_OK);
    CF_CHECK(!ok);
    ok = true;
    CF_REQUIRE(cf_user_authenticated(NULL, CF_STR_LIT("secret123456"), &ok) ==
               CF_OK);
    CF_CHECK(!ok); /* unknown account: refused, so no dummy run at all */

    CF_CHECK(!cf_password_verify(CF_STR_LIT("secret123456"),
                                 CF_STR_LIT(SEEDED_DIGEST)));

    cf_str out = CF_STR_LIT("sentinel");
    CF_CHECK(cf_auth_password_digest(CF_STR_LIT("secret123456"), 4, &out) ==
             CF_BUSY);
    CF_CHECK(out.ptr == NULL && out.len == 0);

    cf_crypto_queue_stats after;
    CF_REQUIRE(cf_crypto_queue_stats_get(&after));
    CF_CHECK(after.refused == before.refused + 4);
    CF_CHECK(after.submitted == before.submitted);
    CF_CHECK(after.completed == before.completed);
    CF_CHECK(after.unprepared == before.unprepared);
    CF_CHECK(after.inline_fallbacks == before.inline_fallbacks);
    CF_CHECK(!after.running);
}

/* cf_app_start creates the queue before its workers can serve and
 * cf_app_stop destroys it: the sign-in boundary then runs on the queue with
 * the configured worker count, and after the stop the requirement is sticky
 * (bcrypt fails closed on the caller instead of running inline). */
CF_TEST(app_start_stop_wires_the_crypto_queue) {
    crypto_queue_test_stop_all();
    cf_db_scratch scratch;
    CF_REQUIRE(cf_db_scratch_open(&scratch));
    cf_config_entry entries[4] = {
        {"PUBLIC_ORIGIN", "http://127.0.0.1:32123"},
        {"SECRET_KEY_BASE", AUTH_VEC_SECRET_KEY_BASE},
        {"DATABASE_PATH", scratch.path},
        {"CF_CRYPTO_WORKERS", "1"}, /* the configured bound is observable */
    };
    cf_config *config = NULL;
    CF_REQUIRE(cf_config_parse(entries, 4, NULL, &config) == CF_OK);
    cf_app *app = NULL;
    CF_REQUIRE(cf_app_create(config, &app) == CF_OK);
    CF_REQUIRE(cf_app_start(app) == CF_OK);

    cf_crypto_queue_stats stats;
    CF_REQUIRE(cf_crypto_queue_stats_get(&stats));
    CF_CHECK(stats.running);
    CF_CHECK(stats.workers == 1);
    CF_CHECK(stats.pending_capacity == 32);

    static const char seeded[] =
        "$2a$12$kDVPfUd.VKsV9HzG8RHRFu0fyaW7gije1Krza2.A0J5S8XEcOQT4S";
    cf_user user;
    memset(&user, 0, sizeof user);
    user.password_digest = (cf_optional_str){
        .present = true,
        .value = {.ptr = (char *)seeded, .len = sizeof seeded - 1}};
    bool ok = false;
    CF_REQUIRE(cf_user_authenticated(&user, CF_STR_LIT("secret123456"),
                                     &ok) == CF_OK);
    CF_CHECK(ok);
    ok = true;
    CF_REQUIRE(cf_user_authenticated(NULL, CF_STR_LIT("secret123456"),
                                     &ok) == CF_OK);
    CF_CHECK(!ok);
    CF_REQUIRE(cf_crypto_queue_stats_get(&stats));
    CF_CHECK(stats.submitted == 2); /* sign-in + dummy, both queued */

    cf_app_stop(app);
    CF_REQUIRE(cf_crypto_queue_stats_get(&stats));
    CF_CHECK(!stats.running);
    /* After the queue stops the requirement is sticky: the same boundary
     * fails closed (documented failure) and never hashes inline. */
    size_t refused_before = stats.refused;
    ok = true;
    CF_REQUIRE(cf_user_authenticated(&user, CF_STR_LIT("secret123456"),
                                     &ok) == CF_OK);
    CF_CHECK(!ok);
    CF_REQUIRE(cf_crypto_queue_stats_get(&stats));
    CF_CHECK(stats.refused == refused_before + 1);
    CF_CHECK(stats.inline_fallbacks == 0);
    cf_app_destroy(app);
    cf_db_scratch_close(&scratch);
}

/* The exact boundary the sign-in action calls (cf_user_authenticated) and
 * the unknown-account dummy verification both run on the queue; a blank
 * password still short-circuits without a job. */
CF_TEST(sign_in_verification_and_dummy_use_the_queue) {
    crypto_queue_test_stop_all();
    static const char seeded[] =
        "$2a$12$kDVPfUd.VKsV9HzG8RHRFu0fyaW7gije1Krza2.A0J5S8XEcOQT4S";
    cf_user user;
    memset(&user, 0, sizeof user);
    user.password_digest = (cf_optional_str){
        .present = true,
        .value = {.ptr = (char *)seeded, .len = sizeof seeded - 1}};

    CF_REQUIRE(cf_crypto_queue_start(1) == CF_OK);
    bool ok = false;
    CF_REQUIRE(cf_user_authenticated(&user, CF_STR_LIT("secret123456"),
                                     &ok) == CF_OK);
    CF_CHECK(ok);
    ok = true;
    CF_REQUIRE(cf_user_authenticated(NULL, CF_STR_LIT("secret123456"),
                                     &ok) == CF_OK);
    CF_CHECK(!ok); /* dummy verification ran, result discarded */
    ok = true;
    CF_REQUIRE(cf_user_authenticated(NULL, CF_STR_LIT(""), &ok) == CF_OK);
    CF_CHECK(!ok); /* blank password: no job at all */

    cf_crypto_queue_stats stats;
    CF_REQUIRE(cf_crypto_queue_stats_get(&stats));
    CF_CHECK(stats.submitted == 2);
    CF_CHECK(stats.completed == 2);
    CF_CHECK(stats.running_max == 1);
    CF_CHECK(stats.refused == 0);
    CF_CHECK(stats.discarded == 0);
    CF_CHECK(stats.unprepared == 0);
    CF_CHECK(stats.inline_fallbacks == 0);
    crypto_queue_test_stop_all();
}

CF_TEST_MAIN()
