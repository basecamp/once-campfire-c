/* Application lifecycle (F03 seed extended by A00): config ownership,
 * data version + cache/version mutex, worker registry, the single writer, the
 * CF_READERS request-worker pool with CF_REQUEST_SLOTS admission accounting,
 * and the H01 admission hook.
 *
 * Shutdown order (00-contracts.md CORE-05, 01-foundation-http.md H01, A00):
 * the caller stops/drains the HTTP loops first, then cf_app_stop() (or
 * cf_app_destroy) requests stop, rejoins every registered worker (the writer
 * exits on the stop request too), and only then frees config/queues. A request
 * worker owns one read-only SQLite connection, opened on its own thread after
 * registering, and runs one synchronous handler at a time; a queued task is
 * submitted or abandoned exactly once, also during shutdown (CORE-04). */
#include "app.h"
#include "app_internal.h"

#include "auth.h" /* bounded crypto queue: start/stop wiring */
#include "cache.h" /* K01c body cache lifecycle */
#include "config.h"
#include "context.h"
#include "db/db_internal.h"
#include "db/writer.h"
#include "http/http.h"
#include "routes.h" /* cf_assets_serve (H03 static front mount) */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cf_worker {
    pthread_t thread;
    bool active;
};

/* One entry of the bounded request pool: either an HTTP task (admission) or a
 * submitted closure (cf_app_submit_worker). Both count against the same
 * CF_REQUEST_SLOTS budget through pool_admitted. */
enum cf_pool_item_kind { CF_POOL_TASK = 0, CF_POOL_CLOSURE };
struct cf_pool_item {
    int kind;
    cf_http_task *task;
    void (*fn)(void *user);
    void *user;
};

/* One request worker: its own reader connection and start handshake. */
struct cf_request_worker {
    cf_app *app;
    pthread_t thread;
    pthread_mutex_t ready_mutex;
    pthread_cond_t ready_cv;
    bool ready;
    cf_err err;
    bool registered; /* in the cf_app registry (join via cf_app_join_workers) */
    bool ready_init; /* ready_mutex/ready_cv were initialized */
    cf_db *reader;   /* opened by the worker; closed by the worker on exit */
};

struct cf_app {
    cf_config *config;               /* owned; freed only after workers join */
    cf_cable *cable;                 /* borrowed C02 cable (cf_app_set_cable) */
    cf_cache *cache;                 /* K01c: owned; created by cf_app_start */
    _Atomic uint64_t data_version;   /* 06: initialized to 1, one writer */
    _Atomic bool stop_requested;
    pthread_mutex_t version_mutex;   /* cache/version mutex; always exists */
    pthread_mutex_t workers_mutex;   /* guards the registry below */
    struct cf_worker workers[CF_APP_MAX_WORKERS];
    size_t worker_count;

    /* A00 request pool: bounded by CF_REQUEST_SLOTS (running + queued). HTTP
     * tasks and cf_app_submit_worker closures share this one queue and its
     * accounting. */
    bool serving_started;
    bool writer_started;
    bool crypto_started; /* this app started the process-wide crypto queue */
    pthread_mutex_t pool_mutex;
    pthread_cond_t pool_cv;
    struct cf_pool_item *pool_queue;
    size_t pool_cap, pool_head, pool_count, pool_admitted;
    struct cf_request_worker *rw;
    size_t rw_count;
    _Atomic uint64_t completed_requests;
    _Atomic uint64_t last_completed_sequence;
};

cf_err cf_app_create(cf_config *config, cf_app **out) {
    if (out == NULL) return CF_INVALID;
    *out = NULL;
    if (config == NULL) return CF_INVALID;

    cf_app *app = calloc(1, sizeof *app);
    if (app == NULL) return CF_NOMEM;

    atomic_init(&app->data_version, UINT64_C(1));
    atomic_init(&app->stop_requested, false);
    atomic_init(&app->completed_requests, 0);
    atomic_init(&app->last_completed_sequence, 0);

    if (pthread_mutex_init(&app->version_mutex, NULL) != 0) {
        free(app);
        return CF_INTERNAL;
    }
    if (pthread_mutex_init(&app->workers_mutex, NULL) != 0) {
        pthread_mutex_destroy(&app->version_mutex);
        free(app);
        return CF_INTERNAL;
    }
    if (pthread_mutex_init(&app->pool_mutex, NULL) != 0) {
        pthread_mutex_destroy(&app->workers_mutex);
        pthread_mutex_destroy(&app->version_mutex);
        free(app);
        return CF_INTERNAL;
    }
    if (pthread_cond_init(&app->pool_cv, NULL) != 0) {
        pthread_mutex_destroy(&app->pool_mutex);
        pthread_mutex_destroy(&app->workers_mutex);
        pthread_mutex_destroy(&app->version_mutex);
        free(app);
        return CF_INTERNAL;
    }

    app->config = config; /* ownership transfers only now, on success */
    app->worker_count = 0;
    *out = app;
    return CF_OK;
}

/* Free the pool storage; every pool thread must already be joined (the
 * registry sweep plus the start-failure path join both registered and
 * unregistered workers). */
static void cf_app_pool_dispose(cf_app *app) {
    if (app->rw != NULL) {
        for (size_t i = 0; i < app->rw_count; i++) {
            struct cf_request_worker *w = &app->rw[i];
            if (w->ready_init) {
                pthread_cond_destroy(&w->ready_cv);
                pthread_mutex_destroy(&w->ready_mutex);
            }
        }
        free(app->rw);
        app->rw = NULL;
        app->rw_count = 0;
    }
    free(app->pool_queue);
    app->pool_queue = NULL;
    app->pool_cap = 0;
    app->pool_head = 0;
    app->pool_count = 0;
    app->pool_admitted = 0;
}

void cf_app_destroy(cf_app *app) {
    if (app == NULL) return;
    cf_app_request_stop(app);
    cf_app_join_workers(app); /* no worker may reference app/config after this */
    /* K01c: destroy the cache after the workers joined (no get/put in flight)
     * and before the version mutex, which it borrows (06: lifecycle). */
    cf_cache_destroy(app->cache);
    app->cache = NULL;
    if (app->crypto_started) {
        /* Releases this app's reference; the last one drains and joins after
         * every submitter has been joined. */
        cf_crypto_queue_stop();
        app->crypto_started = false;
    }
    cf_writer_stop(app);      /* no-op when the writer was never started */
    app->writer_started = false;
    cf_app_pool_dispose(app);
    cf_config_destroy(app->config);
    app->config = NULL;
    pthread_cond_destroy(&app->pool_cv);
    pthread_mutex_destroy(&app->pool_mutex);
    pthread_mutex_destroy(&app->workers_mutex);
    pthread_mutex_destroy(&app->version_mutex);
    free(app);
}

const cf_config *cf_app_config(const cf_app *app) {
    if (app == NULL) return NULL;
    return app->config;
}

void cf_app_set_cable(cf_app *app, cf_cable *cable) {
    if (app == NULL) return;
    app->cable = cable;
}

cf_cable *cf_app_cable(const cf_app *app) {
    if (app == NULL) return NULL;
    return app->cable;
}

cf_err cf_app_register_worker(cf_app *app, pthread_t thread) {
    if (app == NULL) return CF_INVALID;
    pthread_mutex_lock(&app->workers_mutex);
    if (atomic_load_explicit(&app->stop_requested, memory_order_relaxed)) {
        pthread_mutex_unlock(&app->workers_mutex);
        return CF_BUSY; /* shutdown began: a late worker must not start */
    }
    if (app->worker_count >= CF_APP_MAX_WORKERS) {
        pthread_mutex_unlock(&app->workers_mutex);
        return CF_LIMIT;
    }
    app->workers[app->worker_count].thread = thread;
    app->workers[app->worker_count].active = true;
    app->worker_count++;
    pthread_mutex_unlock(&app->workers_mutex);
    return CF_OK;
}

void cf_app_join_workers(cf_app *app) {
    if (app == NULL) return;
    pthread_t threads[CF_APP_MAX_WORKERS];
    size_t count = 0;

    pthread_mutex_lock(&app->workers_mutex);
    for (size_t i = 0; i < app->worker_count; i++) {
        if (app->workers[i].active) {
            threads[count++] = app->workers[i].thread;
            app->workers[i].active = false;
        }
    }
    app->worker_count = 0;
    pthread_mutex_unlock(&app->workers_mutex);

    /* Join outside the lock: workers may finish concurrently. */
    for (size_t i = 0; i < count; i++) {
        (void)pthread_join(threads[i], NULL);
    }
}

void cf_app_request_stop(cf_app *app) {
    if (app == NULL) return;
    atomic_store_explicit(&app->stop_requested, true, memory_order_relaxed);
    /* Wake the request pool: workers waiting for a task must observe the
     * stop instead of sleeping forever. */
    pthread_mutex_lock(&app->pool_mutex);
    pthread_cond_broadcast(&app->pool_cv);
    pthread_mutex_unlock(&app->pool_mutex);
}

bool cf_app_stop_requested(const cf_app *app) {
    if (app == NULL) return true;
    return atomic_load_explicit(&app->stop_requested, memory_order_relaxed);
}

void cf_app_advance_data_version(cf_app *app) {
    if (app == NULL) return;
    pthread_mutex_lock(&app->version_mutex);
    uint64_t current =
        atomic_load_explicit(&app->data_version, memory_order_relaxed);
    if (current == UINT64_MAX) {
        pthread_mutex_unlock(&app->version_mutex);
        fprintf(stderr, "campfire: fatal: data version wrapped\n");
        abort(); /* 06: version wrap is a fatal internal error */
    }
    atomic_store_explicit(&app->data_version, current + 1,
                          memory_order_release);
    pthread_mutex_unlock(&app->version_mutex);
}

pthread_mutex_t *cf_app_version_mutex(cf_app *app) {
    if (app == NULL) return NULL;
    return &app->version_mutex;
}

/* K01c: the version source cf_cache_create borrows. Called with the
 * cache/version mutex held; cf_data_version is one acquire load and takes no
 * locks (cache.h documents the contract). */
static uint64_t cf_app_cache_version(void *user) {
    return cf_data_version((const cf_app *)user);
}

/* Create and attach the K01c cache when the configured budget asks for one.
 * Returns non-OK only when a nonzero budget could not be turned into an
 * enabled cache (allocation/entropy); callers serve uncached (06: "Cache
 * failure does not fail the page"). Budget 0 is the deliberate off switch; a
 * nonzero budget below the bucket-array cost leaves caching off after the
 * cache module's one diagnostic. */
static cf_err cf_app_cache_attach(cf_app *app, size_t budget_bytes) {
    if (budget_bytes == 0) return CF_OK;
    if (app->cache != NULL) return CF_BUSY; /* already attached */
    cf_cache *cache = NULL;
    cf_err rc = cf_cache_create(budget_bytes, cf_app_cache_version, app,
                                &app->version_mutex, &cache);
    if (rc != CF_OK) return rc;
    cf_cache_stats stats;
    if (cf_cache_stats_get(cache, &stats) == CF_OK && stats.enabled) {
        app->cache = cache;
        return CF_OK;
    }
    cf_cache_destroy(cache); /* disabled by the module's budget rule */
    return CF_OK;
}

cf_cache *cf_app_cache(cf_app *app) {
    if (app == NULL) return NULL;
    return app->cache;
}

cf_err cf_app_cache_enable_for_test(cf_app *app, size_t budget_bytes) {
    if (app == NULL) return CF_INVALID;
    if (app->cache != NULL || app->serving_started) return CF_BUSY;
    return cf_app_cache_attach(app, budget_bytes);
}

uint64_t cf_data_version(const cf_app *app) {
    if (app == NULL) return 0;
    return atomic_load_explicit(&app->data_version, memory_order_acquire);
}

size_t cf_app_admitted_count(const cf_app *app) {
    if (app == NULL) return 0;
    cf_app *mutable_app = (cf_app *)app;
    pthread_mutex_lock(&mutable_app->pool_mutex);
    size_t count = app->pool_admitted;
    pthread_mutex_unlock(&mutable_app->pool_mutex);
    return count;
}

uint64_t cf_app_completed_requests(const cf_app *app) {
    if (app == NULL) return 0;
    return atomic_load_explicit(&app->completed_requests,
                                memory_order_acquire);
}

/* ------------------------------------------------------- worker pool --- */

static void cf_request_worker_ready(struct cf_request_worker *w, cf_err err) {
    pthread_mutex_lock(&w->ready_mutex);
    w->err = err;
    w->ready = true;
    pthread_cond_broadcast(&w->ready_cv);
    pthread_mutex_unlock(&w->ready_mutex);
}

/* One synchronous handler per pop. The task is submitted or abandoned
 * exactly once; stale completions are H01's checks, so the worker never
 * touches the task after either call. */
static void cf_request_worker_run_task(cf_app *app, cf_db *reader,
                                       cf_http_task *task) {    uint64_t sequence = cf_http_task_sequence(task); /* borrowed accessor */
    cf_response response;
    cf_response_init(&response);
    const cf_request *request = cf_http_task_request(task);

    /* H03's static front mount runs before the application context exists
     * (it takes request/response only). When it answers, the app dispatch
     * does not run; the response is submitted exactly like a dispatch one. */
    bool handled = false;
    cf_err rc = cf_assets_serve(request, &response, &handled);
    if (rc != CF_OK) {
        /* Internal failure (allocation): map it like any other resource
         * failure, matching cf_finish_mapped's 500. */
        cf_response_dispose(&response);
        cf_response_init(&response);
        response.status = 500;
        handled = true;
        rc = CF_OK;
    }
    if (!handled) rc = cf_ctx_process(app, reader, request, &response);
    if (rc == CF_OK) {
        cf_err submitted = cf_http_task_submit(task, &response);
        if (submitted != CF_OK) {
            /* Ownership stayed with the caller on submit failure; give it
             * back exactly once. */
            cf_http_task_abandon(task);
        }
        cf_response_dispose(&response);
    } else {
        cf_http_task_abandon(task);
        cf_response_dispose(&response);
    }
    atomic_fetch_add_explicit(&app->completed_requests, 1,
                              memory_order_relaxed);
    atomic_store_explicit(&app->last_completed_sequence, sequence,
                          memory_order_relaxed);
}

/* The calling request worker's reader (cf_app_worker_reader). A worker sets
 * it once its connection is open and clears it before closing, so a
 * submitted closure always sees the worker's own connection, never the
 * reactor's or another thread's. */
static _Thread_local cf_db *g_worker_reader;

static void *cf_request_worker_main(void *arg) {
    struct cf_request_worker *w = arg;
    cf_app *app = w->app;

    cf_err rc = cf_app_register_worker(app, pthread_self());
    w->registered = rc == CF_OK;
    if (rc != CF_OK) {
        cf_request_worker_ready(w, rc);
        return NULL;
    }

    cf_db *reader = NULL;
    rc = cf_db_open(app->config->database_path, true, &reader);
    if (rc != CF_OK) {
        cf_request_worker_ready(w, rc);
        return NULL;
    }
    w->reader = reader;
    g_worker_reader = reader;
    cf_request_worker_ready(w, CF_OK);

    for (;;) {
        pthread_mutex_lock(&app->pool_mutex);
        while (app->pool_count == 0 && !cf_app_stop_requested(app)) {
            pthread_cond_wait(&app->pool_cv, &app->pool_mutex);
        }
        if (app->pool_count == 0) { /* stop requested and queue drained */
            pthread_mutex_unlock(&app->pool_mutex);
            break;
        }
        struct cf_pool_item item = app->pool_queue[app->pool_head];
        app->pool_head = (app->pool_head + 1) % app->pool_cap;
        app->pool_count--;
        bool stopping = cf_app_stop_requested(app);
        pthread_mutex_unlock(&app->pool_mutex);

        if (item.kind == CF_POOL_TASK) {
            if (stopping) {
                /* Shutdown: queued HTTP work is discarded without losing its
                 * completion (CORE-04). */
                cf_http_task_abandon(item.task);
            } else {
                cf_request_worker_run_task(app, reader, item.task);
            }
        } else {
            /* A submitted closure always runs exactly once: even during
             * shutdown it must complete so its own reference/state is
             * released (cf_app_submit_worker). It runs on this worker with
             * this worker's reader and is expected to be cancellation-aware
             * by checking its owner's state. */
            item.fn(item.user);
        }

        pthread_mutex_lock(&app->pool_mutex);
        app->pool_admitted--;
        pthread_mutex_unlock(&app->pool_mutex);
    }

    g_worker_reader = NULL;
    cf_db_close(w->reader);
    w->reader = NULL;
    return NULL;
}

cf_db *cf_app_worker_reader(void) { return g_worker_reader; }

cf_err cf_app_start(cf_app *app) {
    if (app == NULL) return CF_INVALID;
    if (app->serving_started) return CF_BUSY;
    if (cf_app_stop_requested(app)) return CF_BUSY;
    const cf_config *config = app->config;
    size_t created = 0;

    /* K01c: create the body cache before any worker can look up or admit; a
     * creation failure keeps the app serving uncached (06). */
    (void)cf_app_cache_attach(app, config->cache_bytes);

    cf_err rc = cf_writer_start(app, config);
    if (rc != CF_OK) return rc;
    app->writer_started = true;

    /* The bounded crypto queue exists before the first request worker can
     * serve (00-contracts): bcrypt for sign-in, the unknown-account dummy
     * verification and setup hashing runs on these CF_CRYPTO_WORKERS threads,
     * never on the request worker. */
    rc = cf_crypto_queue_start(config->crypto_workers);
    if (rc != CF_OK) {
        cf_writer_stop(app);
        app->writer_started = false;
        return rc;
    }
    app->crypto_started = true;

    size_t slots = config->request_slots;
    size_t readers = config->readers;
    if (slots == 0 || readers == 0) {
        rc = CF_INVALID;
        goto fail;
    }
    if (slots > SIZE_MAX / sizeof(cf_http_task *)) {
        rc = CF_LIMIT;
        goto fail;
    }
    app->pool_queue = calloc(slots, sizeof *app->pool_queue);
    if (app->pool_queue == NULL) {
        rc = CF_NOMEM;
        goto fail;
    }
    app->pool_cap = slots;
    app->pool_head = 0;
    app->pool_count = 0;
    app->pool_admitted = 0;

    app->rw = calloc(readers, sizeof *app->rw);
    if (app->rw == NULL) {
        rc = CF_NOMEM;
        goto fail;
    }
    app->rw_count = readers;

    for (size_t i = 0; i < readers; i++) {
        struct cf_request_worker *w = &app->rw[i];
        w->app = app;
        w->ready = false;
        w->err = CF_OK;
        w->registered = false;
        w->reader = NULL;
        w->thread = 0;
        if (pthread_mutex_init(&w->ready_mutex, NULL) != 0) {
            rc = CF_INTERNAL;
            goto fail;
        }
        if (pthread_cond_init(&w->ready_cv, NULL) != 0) {
            pthread_mutex_destroy(&w->ready_mutex);
            rc = CF_INTERNAL;
            goto fail;
        }
        w->ready_init = true;
        if (pthread_create(&w->thread, NULL, cf_request_worker_main, w) !=
            0) {
            w->thread = 0;
            rc = CF_INTERNAL;
            goto fail;
        }
        created++;
    }

    for (size_t i = 0; i < created; i++) {
        struct cf_request_worker *w = &app->rw[i];
        pthread_mutex_lock(&w->ready_mutex);
        while (!w->ready) pthread_cond_wait(&w->ready_cv, &w->ready_mutex);
        cf_err worker_err = w->err;
        pthread_mutex_unlock(&w->ready_mutex);
        if (worker_err != CF_OK) {
            rc = worker_err;
            goto fail;
        }
    }

    app->serving_started = true;
    return CF_OK;

fail:
    cf_app_request_stop(app);
    /* Every created worker signals ready exactly once (registration failure
     * or reader open). Wait for that, then join the ones that never reached
     * the registry; registered ones are joined by the registry sweep. */
    for (size_t i = 0; i < created; i++) {
        struct cf_request_worker *w = &app->rw[i];
        pthread_mutex_lock(&w->ready_mutex);
        while (!w->ready) pthread_cond_wait(&w->ready_cv, &w->ready_mutex);
        pthread_mutex_unlock(&w->ready_mutex);
    }
    for (size_t i = 0; i < created; i++) {
        struct cf_request_worker *w = &app->rw[i];
        if (!w->registered) {
            (void)pthread_join(w->thread, NULL);
        }
        w->thread = 0;
    }
    cf_app_join_workers(app);
    if (app->crypto_started) {
        cf_crypto_queue_stop();
        app->crypto_started = false;
    }
    cf_writer_stop(app);
    app->writer_started = false;
    cf_app_pool_dispose(app);
    return rc;
}

void cf_app_stop(cf_app *app) {
    if (app == NULL) return;
    cf_app_request_stop(app);
    cf_app_join_workers(app);
    if (app->crypto_started) {
        cf_crypto_queue_stop(); /* after the request workers that wait on it */
        app->crypto_started = false;
    }
    cf_writer_stop(app);
    app->writer_started = false;
}

cf_err cf_app_admit(void *app_user, struct cf_http_task *task) {
    cf_app *app = app_user;
    if (app == NULL || task == NULL) return CF_INVALID;
    pthread_mutex_lock(&app->pool_mutex);
    if (!app->serving_started || cf_app_stop_requested(app) ||
        app->pool_cap == 0 || app->pool_admitted >= app->pool_cap) {
        pthread_mutex_unlock(&app->pool_mutex);
        return CF_BUSY;
    }
    size_t tail = (app->pool_head + app->pool_count) % app->pool_cap;
    app->pool_queue[tail].kind = CF_POOL_TASK;
    app->pool_queue[tail].task = task;
    app->pool_queue[tail].fn = NULL;
    app->pool_queue[tail].user = NULL;
    app->pool_count++;
    app->pool_admitted++;
    pthread_cond_signal(&app->pool_cv);
    pthread_mutex_unlock(&app->pool_mutex);
    return CF_OK;
}

cf_err cf_app_submit_worker(cf_app *app, void (*fn)(void *user), void *user) {
    if (app == NULL || fn == NULL) return CF_INVALID;
    pthread_mutex_lock(&app->pool_mutex);
    if (!app->serving_started || cf_app_stop_requested(app) ||
        app->pool_cap == 0 || app->pool_admitted >= app->pool_cap) {
        /* The same bounded admission budget as HTTP tasks: the pool is full
         * (running + queued == CF_REQUEST_SLOTS) or stopping. The caller
         * treats the closure as not submitted. */
        pthread_mutex_unlock(&app->pool_mutex);
        return CF_BUSY;
    }
    size_t tail = (app->pool_head + app->pool_count) % app->pool_cap;
    app->pool_queue[tail].kind = CF_POOL_CLOSURE;
    app->pool_queue[tail].task = NULL;
    app->pool_queue[tail].fn = fn;
    app->pool_queue[tail].user = user;
    app->pool_count++;
    app->pool_admitted++;
    pthread_cond_signal(&app->pool_cv);
    pthread_mutex_unlock(&app->pool_mutex);
    return CF_OK;
}

static size_t cf_app_loop_share(size_t total, size_t loops, size_t index) {
    if (loops == 0 || index >= loops) return 0;
    size_t base = total / loops;
    size_t remainder = total % loops;
    size_t share = base + (index < remainder ? 1 : 0);
    if (share == 0) {
        /* H01 treats 0 as its 64 MiB default; a config smaller than CF_LOOPS
         * cannot be expressed, so each loop gets the smallest nonzero share. */
        share = 1;
    }
    return share;
}

size_t cf_app_loop_input_bytes(const cf_app *app, size_t loop_index) {
    if (app == NULL || app->config == NULL) return 0;
    return cf_app_loop_share(app->config->input_bytes, app->config->loops,
                             loop_index);
}

size_t cf_app_loop_output_bytes(const cf_app *app, size_t loop_index) {
    if (app == NULL || app->config == NULL) return 0;
    return cf_app_loop_share(app->config->output_bytes, app->config->loops,
                             loop_index);
}
