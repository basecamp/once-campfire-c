/* D02 single writer: bounded callback queue, one writer thread, transaction
 * and post-commit ownership (02-data-auth.md D02; 04-cable-jobs.md C03/J01;
 * 06-cache-performance.md version algorithm; 00-contracts.md execution order).
 *
 * Design:
 *  - cf_write enqueues a stack-owned request and blocks; the writer thread
 *    runs the callback inside BEGIN IMMEDIATE -> COMMIT/ROLLBACK.  Only a
 *    committed transaction advances the data version and hands off events.
 *  - The queue bound counts callbacks admitted and waiting to run; a full
 *    queue returns CF_BUSY before the callback executes.
 *  - Post-commit work runs on the writer thread in writer order and without
 *    holding the writer mutex, so a slow C03 revocation barrier postpones
 *    later writes instead of blocking their callers from enqueuing.
 *  - A registry refcount keeps the writer state alive while any caller is
 *    inside cf_write / registration / stats, so cf_writer_stop can free it
 *    without racing a returning caller.
 */
#include "db/writer.h"

#include "app_internal.h"
#include "db/db_internal.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Counters and the handler tables are indexed by cf_event_kind order. */
_Static_assert(CF_EVENT_DISCONNECT_USER == 0, "writer.c kind order");
_Static_assert(CF_EVENT_PURGE_BLOB == 4, "writer.c kind order");
_Static_assert(CF_WRITER_EVENT_KINDS == CF_EVENT_PURGE_BLOB + 1,
               "writer.c kind count");

/* --- transaction object -------------------------------------------------- */

struct cf_tx {
    cf_db *db;            /* borrowed writer connection; writer thread only */
    bool in_tx;           /* between BEGIN IMMEDIATE and COMMIT/ROLLBACK */
    cf_event *events;     /* owned; appended in callback order */
    size_t event_count, event_cap;
};

/* --- writer state -------------------------------------------------------- */

struct cf_writer_req {
    cf_write_fn fn;   /* borrowed until cf_write returns */
    void *arg;        /* borrowed until cf_write returns */
    cf_err result;
    bool done;
};

struct cf_writer {
    cf_app *app;                 /* borrowed; alive until app_destroy joins */
    char *database_path;         /* owned copy (diagnostics) */
    cf_db *db;                   /* opened by cf_writer_start; closed by thread */
    size_t refs;                 /* callers inside cf_write/set/get; reg mu */
    pthread_t thread;
    bool stop;                   /* cf_writer_stop requested */
    bool finished;               /* thread closed the connection and exited */
    bool init_done;
    cf_err init_err;

    pthread_mutex_t mu;          /* guards queue, handlers, counters, flags */
    pthread_cond_t work_cv;      /* pending work / stop */
    pthread_cond_t done_cv;      /* request completion broadcast */
    pthread_cond_t init_cv;
    pthread_cond_t finished_cv;

    /* Ring of pointers to caller-owned requests, in writer order. */
    struct cf_writer_req **queue;
    size_t capacity, head, tail, pending;
    bool callback_running;

    cf_writer_control_fn control_fn;
    void *control_ctx;
    cf_writer_event_fn event_fn[CF_WRITER_EVENT_KINDS];
    void *event_ctx[CF_WRITER_EVENT_KINDS];

    cf_writer_stats stats;
    struct cf_writer *next;
};

/* The registry mutex guards the registry, every writer's refcount and the
 * stop/handle handshake.  refs_cv wakes cf_writer_stop when the last caller
 * releases a removed writer. */
static pthread_mutex_t cf_writer_registry_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cf_writer_refs_cv = PTHREAD_COND_INITIALIZER;
static struct cf_writer *cf_writer_registry;
static size_t cf_writer_registry_count;

static struct cf_writer *cf_writer_find_locked(cf_app *app) {
    for (struct cf_writer *w = cf_writer_registry; w != NULL; w = w->next) {
        if (w->app == app) return w;
    }
    return NULL;
}

/* Check out a writer for a caller: +1 ref so cf_writer_stop cannot free it
 * while the caller is inside.  Returns NULL when this app has no writer. */
static struct cf_writer *cf_writer_acquire(cf_app *app) {
    pthread_mutex_lock(&cf_writer_registry_mu);
    struct cf_writer *w = cf_writer_find_locked(app);
    if (w != NULL) w->refs++;
    pthread_mutex_unlock(&cf_writer_registry_mu);
    return w;
}

static void cf_writer_release(struct cf_writer *w) {
    pthread_mutex_lock(&cf_writer_registry_mu);
    w->refs--;
    if (w->refs == 0) pthread_cond_broadcast(&cf_writer_refs_cv);
    pthread_mutex_unlock(&cf_writer_registry_mu);
}

static cf_err cf_writer_internal(const char *what) {
    return cf_db_failf(CF_INTERNAL, "writer: %s", what);
}

/* --- statistics (all under w->mu) ---------------------------------------- */

static void cf_writer_stat_begin_failure(struct cf_writer *w) {
    pthread_mutex_lock(&w->mu);
    w->stats.begin_failures++;
    pthread_mutex_unlock(&w->mu);
}

static void cf_writer_stat_rolled_back(struct cf_writer *w, bool commit) {
    pthread_mutex_lock(&w->mu);
    w->stats.rolled_back++;
    if (commit) w->stats.commit_failures++;
    pthread_mutex_unlock(&w->mu);
}

static void cf_writer_stat_committed(struct cf_writer *w) {
    pthread_mutex_lock(&w->mu);
    w->stats.committed++;
    pthread_mutex_unlock(&w->mu);
}

static void cf_writer_stat_mandatory_failure(struct cf_writer *w) {
    pthread_mutex_lock(&w->mu);
    w->stats.mandatory_failures++;
    pthread_mutex_unlock(&w->mu);
}

static void cf_writer_stat_drop(struct cf_writer *w, cf_event_kind kind) {
    pthread_mutex_lock(&w->mu);
    w->stats.best_effort_dropped[kind]++;
    pthread_mutex_unlock(&w->mu);
}

/* --- events -------------------------------------------------------------- */

/* Zero every field the kind does not use so delivered events never carry
 * stale caller data (02 D02: "Zero unused fields"). */
static cf_event cf_event_normalize(cf_event event) {
    switch (event.kind) {
    case CF_EVENT_DISCONNECT_USER:
        event.room_id = 0;
        event.message_id = 0;
        event.blob_id = 0;
        break;
    case CF_EVENT_PUSH_MESSAGE:
        event.user_id = 0;
        event.blob_id = 0;
        event.reconnect = false;
        break;
    case CF_EVENT_REMOVE_BANNED_CONTENT:
        event.room_id = 0;
        event.message_id = 0;
        event.blob_id = 0;
        event.reconnect = false;
        break;
    case CF_EVENT_DELIVER_WEBHOOK:
        event.room_id = 0;
        event.blob_id = 0;
        event.reconnect = false;
        break;
    case CF_EVENT_PURGE_BLOB:
        event.user_id = 0;
        event.room_id = 0;
        event.message_id = 0;
        event.reconnect = false;
        break;
    }
    return event;
}

static void cf_tx_dispose_events(struct cf_tx *tx) {
    free(tx->events);
    tx->events = NULL;
    tx->event_count = 0;
    tx->event_cap = 0;
    tx->in_tx = false;
}

cf_db *cf_tx_db(cf_tx *tx) {
    if (tx == NULL) return NULL;
    return tx->db;
}

cf_err cf_tx_event(cf_tx *tx, cf_event event) {
    if (tx == NULL || !tx->in_tx) {
        return cf_writer_internal("cf_tx_event outside a write transaction");
    }
    if ((int)event.kind < 0 || event.kind > CF_EVENT_PURGE_BLOB) {
        return cf_db_failf(CF_INVALID, "writer: unknown event kind %d",
                           (int)event.kind);
    }
    event = cf_event_normalize(event);

    /* Repeated identical disconnects may coalesce (02 D02).  AND preserves
     * "never change reconnect=false to true while coalescing"; the first
     * disconnect keeps its position so writer order is stable. */
    if (event.kind == CF_EVENT_DISCONNECT_USER) {
        for (size_t i = 0; i < tx->event_count; i++) {
            if (tx->events[i].kind == CF_EVENT_DISCONNECT_USER &&
                tx->events[i].user_id == event.user_id) {
                tx->events[i].reconnect =
                    tx->events[i].reconnect && event.reconnect;
                return CF_OK;
            }
        }
    }

    if (tx->event_count == tx->event_cap) {
        size_t next = tx->event_cap == 0 ? 8 : tx->event_cap * 2;
        if (next < tx->event_cap || next > SIZE_MAX / sizeof(cf_event)) {
            return cf_db_failf(CF_NOMEM, "writer: too many transaction events");
        }
        cf_event *grown = realloc(tx->events, next * sizeof *grown);
        if (grown == NULL) {
            return cf_db_failf(CF_NOMEM, "writer: event allocation failed");
        }
        tx->events = grown;
        tx->event_cap = next;
    }
    tx->events[tx->event_count++] = event;
    return CF_OK;
}

/* --- transaction execution (writer thread) ------------------------------- */

/* ROLLBACK; if a callback left a statement mid-step, clear the connection's
 * statement cache once and retry (D01: clearing finalizes and allows reuse). */
static void cf_writer_rollback(cf_db *db) {
    sqlite3 *handle = cf_db_handle(db);
    if (sqlite3_exec(handle, "ROLLBACK", NULL, NULL, NULL) != SQLITE_OK) {
        cf_db_stmt_cache_clear(db);
        (void)sqlite3_exec(handle, "ROLLBACK", NULL, NULL, NULL);
    }
}

/* Hand off committed events in append (writer) order.  Returns CF_OK unless a
 * mandatory revocation could not be applied, in which case the committed
 * write reports CF_INTERNAL and is never retried or rolled back. */
static cf_err cf_writer_post_commit(struct cf_writer *w, struct cf_tx *tx) {
    cf_err outcome = CF_OK;
    for (size_t i = 0; i < tx->event_count; i++) {
        const cf_event *event = &tx->events[i];
        if (event->kind == CF_EVENT_DISCONNECT_USER) {
            pthread_mutex_lock(&w->mu);
            cf_writer_control_fn fn = w->control_fn;
            void *ctx = w->control_ctx;
            pthread_mutex_unlock(&w->mu);
            if (fn == NULL || fn(ctx, event) != CF_OK) {
                cf_writer_stat_mandatory_failure(w);
                outcome = CF_INTERNAL;
            }
        } else {
            pthread_mutex_lock(&w->mu);
            cf_writer_event_fn fn = w->event_fn[event->kind];
            void *ctx = w->event_ctx[event->kind];
            pthread_mutex_unlock(&w->mu);
            if (fn == NULL || fn(ctx, event) != CF_OK) {
                cf_writer_stat_drop(w, event->kind);
            }
        }
    }
    return outcome;
}

static cf_err cf_writer_execute(struct cf_writer *w, struct cf_writer_req *req) {
    sqlite3 *handle = cf_db_handle(w->db);
    struct cf_tx tx;
    memset(&tx, 0, sizeof tx);
    tx.db = w->db;

    int rc = sqlite3_exec(handle, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        cf_writer_stat_begin_failure(w);
        return cf_db_err(rc);
    }
    tx.in_tx = true;

    cf_err cb = req->fn(&tx, req->arg);
    if ((int)cb < (int)CF_OK || cb > CF_INTERNAL) {
        /* A callback cannot invent return codes outside the contract. */
        cb = CF_INTERNAL;
    }
    if (cb != CF_OK) {
        cf_writer_rollback(w->db); /* events drop with the effects */
        cf_tx_dispose_events(&tx);
        cf_writer_stat_rolled_back(w, false);
        return cb;
    }

    rc = sqlite3_exec(handle, "COMMIT", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        /* 02 D02: commit failure discards effects and returns CF_DB. */
        cf_writer_rollback(w->db);
        cf_tx_dispose_events(&tx);
        cf_writer_stat_rolled_back(w, true);
        return CF_DB;
    }
    tx.in_tx = false;
    cf_writer_stat_committed(w);

    /* 06: commit, then publish the new version under the cache/version mutex
     * (release ordering; wrap is fatal inside app_internal). */
    cf_app_advance_data_version(w->app);

    cf_err post = cf_writer_post_commit(w, &tx);
    cf_tx_dispose_events(&tx);
    return post;
}

/* --- writer thread ------------------------------------------------------- */

static bool cf_writer_should_stop(struct cf_writer *w) {
    return w->stop || cf_app_stop_requested(w->app);
}

static void cf_writer_deadline(struct timespec *ts, long millis) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_nsec += millis * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += ts->tv_nsec / 1000000000L;
        ts->tv_nsec %= 1000000000L;
    }
}

static void *cf_writer_thread(void *arg) {
    struct cf_writer *w = arg;

    /* Register before the first observable side effect (F03 contract); this
     * is what lets cf_app_destroy join the writer even without an explicit
     * cf_writer_stop. */
    cf_err init = cf_app_register_worker(w->app, pthread_self());
    pthread_mutex_lock(&w->mu);
    w->init_err = init;
    w->init_done = true;
    pthread_cond_broadcast(&w->init_cv);
    pthread_mutex_unlock(&w->mu);

    if (init == CF_OK) {
        for (;;) {
            pthread_mutex_lock(&w->mu);
            while (w->pending == 0 && !cf_writer_should_stop(w)) {
                struct timespec ts;
                cf_writer_deadline(&ts, 50);
                (void)pthread_cond_timedwait(&w->work_cv, &w->mu, &ts);
            }
            if (w->pending == 0 && cf_writer_should_stop(w)) {
                pthread_mutex_unlock(&w->mu);
                break;
            }
            struct cf_writer_req *req = w->queue[w->head];
            w->head = (w->head + 1) % w->capacity;
            w->pending--;
            w->callback_running = true;
            pthread_mutex_unlock(&w->mu);

            req->result = cf_writer_execute(w, req);

            pthread_mutex_lock(&w->mu);
            w->callback_running = false;
            req->done = true;
            pthread_cond_broadcast(&w->done_cv);
            pthread_mutex_unlock(&w->mu);
        }
    }

    cf_db_close(w->db);
    w->db = NULL;

    pthread_mutex_lock(&w->mu);
    w->finished = true;
    pthread_cond_broadcast(&w->finished_cv);
    pthread_mutex_unlock(&w->mu);
    return NULL;
}

/* --- public API ---------------------------------------------------------- */

cf_err cf_write(cf_app *app, cf_write_fn fn, void *arg) {
    if (app == NULL || fn == NULL) {
        return cf_db_failf(CF_INVALID, "writer: cf_write needs app and fn");
    }

    struct cf_writer *w = cf_writer_acquire(app);
    if (w == NULL) return cf_writer_internal("cf_write before cf_writer_start");

    struct cf_writer_req req;
    memset(&req, 0, sizeof req);
    req.fn = fn;
    req.arg = arg;

    pthread_mutex_lock(&w->mu);
    /* A callback executing on the writer thread must not recurse (00: the
     * writer never calls cf_write); this also covers other writer-thread use. */
    if (pthread_equal(pthread_self(), w->thread)) {
        pthread_mutex_unlock(&w->mu);
        cf_writer_release(w);
        return cf_writer_internal("cf_write called from the writer thread");
    }
    if (w->finished || w->stop || cf_app_stop_requested(app)) {
        pthread_mutex_unlock(&w->mu);
        cf_writer_release(w);
        return cf_db_failf(CF_BUSY, "writer: shutting down");
    }
    if (w->pending == w->capacity) {
        w->stats.queue_rejected++;
        pthread_mutex_unlock(&w->mu);
        cf_writer_release(w);
        return cf_db_failf(CF_BUSY, "writer: queue full");
    }

    w->queue[w->tail] = &req;
    w->tail = (w->tail + 1) % w->capacity;
    w->pending++;
    w->stats.admitted++;
    pthread_cond_signal(&w->work_cv);

    while (!req.done) {
        pthread_cond_wait(&w->done_cv, &w->mu);
    }
    cf_err result = req.result;
    pthread_mutex_unlock(&w->mu);
    cf_writer_release(w);
    return result;
}

/* Init the synchronization primitives, tracking which ones to destroy on a
 * partial failure (destroying an uninitialized mutex/cond is undefined). */
static cf_err cf_writer_sync_init(struct cf_writer *w, int *done) {
    if (pthread_mutex_init(&w->mu, NULL) != 0) return CF_NOMEM;
    *done = 1;
    if (pthread_cond_init(&w->work_cv, NULL) != 0) return CF_NOMEM;
    *done = 2;
    if (pthread_cond_init(&w->done_cv, NULL) != 0) return CF_NOMEM;
    *done = 3;
    if (pthread_cond_init(&w->init_cv, NULL) != 0) return CF_NOMEM;
    *done = 4;
    if (pthread_cond_init(&w->finished_cv, NULL) != 0) return CF_NOMEM;
    *done = 5;
    return CF_OK;
}

static void cf_writer_sync_destroy(struct cf_writer *w, int done) {
    if (done >= 5) pthread_cond_destroy(&w->finished_cv);
    if (done >= 4) pthread_cond_destroy(&w->init_cv);
    if (done >= 3) pthread_cond_destroy(&w->done_cv);
    if (done >= 2) pthread_cond_destroy(&w->work_cv);
    if (done >= 1) pthread_mutex_destroy(&w->mu);
}

cf_err cf_writer_start(cf_app *app, const cf_config *config) {
    if (app == NULL || config == NULL || config->database_path == NULL) {
        return cf_db_failf(CF_INVALID, "writer: start needs app and config");
    }
    size_t capacity = config->writer_queue;
    if (capacity == 0) {
        return cf_db_failf(CF_INVALID, "writer: CF_WRITER_QUEUE must be > 0");
    }

    pthread_mutex_lock(&cf_writer_registry_mu);
    if (cf_writer_find_locked(app) != NULL) {
        pthread_mutex_unlock(&cf_writer_registry_mu);
        return cf_writer_internal("writer already started");
    }
    if (cf_writer_registry_count >= CF_WRITER_MAX_APPS) {
        pthread_mutex_unlock(&cf_writer_registry_mu);
        return cf_db_failf(CF_LIMIT, "writer: too many app writers");
    }

    struct cf_writer *w = calloc(1, sizeof *w);
    if (w == NULL) {
        pthread_mutex_unlock(&cf_writer_registry_mu);
        return CF_NOMEM;
    }
    w->app = app;
    w->capacity = capacity;
    w->database_path = strdup(config->database_path);
    w->queue = calloc(capacity, sizeof *w->queue);
    int sync_done = 0;
    cf_err rc = cf_writer_sync_init(w, &sync_done);
    if (w->database_path == NULL || w->queue == NULL || rc != CF_OK) {
        cf_writer_sync_destroy(w, sync_done);
        free(w->queue);
        free(w->database_path);
        free(w);
        pthread_mutex_unlock(&cf_writer_registry_mu);
        return CF_NOMEM;
    }

    rc = cf_db_open(config->database_path, false, &w->db);
    if (rc != CF_OK) { /* message stays in this calling thread's TLS */
        cf_writer_sync_destroy(w, sync_done);
        free(w->queue);
        free(w->database_path);
        free(w);
        pthread_mutex_unlock(&cf_writer_registry_mu);
        return rc;
    }

    if (pthread_create(&w->thread, NULL, cf_writer_thread, w) != 0) {
        cf_db_close(w->db);
        cf_writer_sync_destroy(w, sync_done);
        free(w->queue);
        free(w->database_path);
        free(w);
        pthread_mutex_unlock(&cf_writer_registry_mu);
        return cf_writer_internal("cannot start writer thread");
    }

    pthread_mutex_lock(&w->mu);
    while (!w->init_done) {
        pthread_cond_wait(&w->init_cv, &w->mu);
    }
    cf_err init_err = w->init_err;
    pthread_mutex_unlock(&w->mu);

    if (init_err != CF_OK) {
        /* The thread did not register with the app; join it here. */
        pthread_mutex_lock(&w->mu);
        w->stop = true;
        pthread_cond_broadcast(&w->work_cv);
        while (!w->finished) {
            pthread_cond_wait(&w->finished_cv, &w->mu);
        }
        pthread_mutex_unlock(&w->mu);
        pthread_join(w->thread, NULL);
        cf_writer_sync_destroy(w, sync_done);
        free(w->queue);
        free(w->database_path);
        free(w);
        pthread_mutex_unlock(&cf_writer_registry_mu);
        return init_err;
    }

    w->next = cf_writer_registry;
    cf_writer_registry = w;
    cf_writer_registry_count++;
    pthread_mutex_unlock(&cf_writer_registry_mu);
    return CF_OK;
}

void cf_writer_stop(cf_app *app) {
    if (app == NULL) return;
    pthread_mutex_lock(&cf_writer_registry_mu);
    struct cf_writer **link = &cf_writer_registry;
    while (*link != NULL && (*link)->app != app) {
        link = &(*link)->next;
    }
    struct cf_writer *w = *link;
    if (w == NULL) {
        pthread_mutex_unlock(&cf_writer_registry_mu);
        return;
    }
    /* Unlink first: no new caller can acquire a reference. */
    *link = w->next;
    cf_writer_registry_count--;
    pthread_mutex_unlock(&cf_writer_registry_mu);

    /* No new cf_write is admitted; admitted callbacks drain first. */
    pthread_mutex_lock(&w->mu);
    w->stop = true;
    pthread_cond_broadcast(&w->work_cv);
    while (!w->finished) {
        pthread_cond_wait(&w->finished_cv, &w->mu);
    }
    pthread_mutex_unlock(&w->mu);

    /* Wait for callers that already hold a reference to leave. */
    pthread_mutex_lock(&cf_writer_registry_mu);
    while (w->refs > 0) {
        pthread_cond_wait(&cf_writer_refs_cv, &cf_writer_registry_mu);
    }
    pthread_mutex_unlock(&cf_writer_registry_mu);

    /* The thread has exited and no caller references w.  cf_app_destroy (or a
     * prior call) joins the registered pthread; stop only releases state. */
    cf_writer_sync_destroy(w, 5);
    free(w->queue);
    free(w->database_path);
    free(w);
}

cf_err cf_writer_set_control_handler(cf_app *app, cf_writer_control_fn fn,
                                     void *ctx) {
    if (app == NULL || fn == NULL) {
        return cf_db_failf(CF_INVALID, "writer: control handler needs fn");
    }
    struct cf_writer *w = cf_writer_acquire(app);
    if (w == NULL) return cf_writer_internal("control handler without writer");
    pthread_mutex_lock(&w->mu);
    if (w->finished) {
        pthread_mutex_unlock(&w->mu);
        cf_writer_release(w);
        return cf_db_failf(CF_BUSY, "writer: shutting down");
    }
    w->control_fn = fn;
    w->control_ctx = ctx;
    pthread_mutex_unlock(&w->mu);
    cf_writer_release(w);
    return CF_OK;
}

cf_err cf_writer_set_event_handler(cf_app *app, cf_event_kind kind,
                                   cf_writer_event_fn fn, void *ctx) {
    if (app == NULL || fn == NULL || (int)kind < 0 ||
        kind > CF_EVENT_PURGE_BLOB) {
        return cf_db_failf(CF_INVALID, "writer: bad event handler");
    }
    if (kind == CF_EVENT_DISCONNECT_USER) {
        return cf_db_failf(CF_INVALID,
                           "writer: DISCONNECT_USER uses the control handler");
    }
    struct cf_writer *w = cf_writer_acquire(app);
    if (w == NULL) return cf_writer_internal("event handler without writer");
    pthread_mutex_lock(&w->mu);
    if (w->finished) {
        pthread_mutex_unlock(&w->mu);
        cf_writer_release(w);
        return cf_db_failf(CF_BUSY, "writer: shutting down");
    }
    w->event_fn[kind] = fn;
    w->event_ctx[kind] = ctx;
    pthread_mutex_unlock(&w->mu);
    cf_writer_release(w);
    return CF_OK;
}

cf_err cf_writer_stats_get(cf_app *app, cf_writer_stats *out) {
    if (app == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "writer: stats needs app and out");
    }
    struct cf_writer *w = cf_writer_acquire(app);
    if (w == NULL) return cf_writer_internal("stats without writer");
    pthread_mutex_lock(&w->mu);
    *out = w->stats;
    out->queue_capacity = w->capacity;
    out->pending = w->pending;
    out->callback_running = w->callback_running;
    pthread_mutex_unlock(&w->mu);
    cf_writer_release(w);
    return CF_OK;
}
