/* Minimal cf_app seed (F03): config ownership, data version + cache/version
 * mutex, and the worker registry used by shutdown (CORE-05).
 *
 * A00 extends this file later for dispatch/completion; D02 uses
 * cf_app_advance_data_version from src/app_internal.h. Destruction order is
 * fixed: request stop, join workers, then free config and destroy mutexes. */
#include "app.h"
#include "app_internal.h"

#include "config.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

struct cf_worker {
    pthread_t thread;
    bool active;
};

struct cf_app {
    cf_config *config;               /* owned; freed only after workers join */
    _Atomic uint64_t data_version;   /* 06: initialized to 1, one writer */
    _Atomic bool stop_requested;
    pthread_mutex_t version_mutex;   /* cache/version mutex; always exists */
    pthread_mutex_t workers_mutex;   /* guards the registry below */
    struct cf_worker workers[CF_APP_MAX_WORKERS];
    size_t worker_count;
};

cf_err cf_app_create(cf_config *config, cf_app **out) {
    if (out == NULL) return CF_INVALID;
    *out = NULL;
    if (config == NULL) return CF_INVALID;

    cf_app *app = calloc(1, sizeof *app);
    if (app == NULL) return CF_NOMEM;

    atomic_init(&app->data_version, UINT64_C(1));
    atomic_init(&app->stop_requested, false);

    if (pthread_mutex_init(&app->version_mutex, NULL) != 0) {
        free(app);
        return CF_INTERNAL;
    }
    if (pthread_mutex_init(&app->workers_mutex, NULL) != 0) {
        pthread_mutex_destroy(&app->version_mutex);
        free(app);
        return CF_INTERNAL;
    }

    app->config = config; /* ownership transfers only now, on success */
    app->worker_count = 0;
    *out = app;
    return CF_OK;
}

void cf_app_destroy(cf_app *app) {
    if (app == NULL) return;
    cf_app_request_stop(app);
    cf_app_join_workers(app); /* no worker may reference app/config after this */
    cf_config_destroy(app->config);
    app->config = NULL;
    pthread_mutex_destroy(&app->workers_mutex);
    pthread_mutex_destroy(&app->version_mutex);
    free(app);
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

uint64_t cf_data_version(const cf_app *app) {
    if (app == NULL) return 0;
    return atomic_load_explicit(&app->data_version, memory_order_acquire);
}
