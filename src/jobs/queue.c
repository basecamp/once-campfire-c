/* Bounded per-kind job queues and accounting (J01).
 *
 * One FIFO per kind with config->job_queue pending slots, one mutex/condition
 * variable per kind, and per-kind workers (config->job_workers for the four
 * event kinds, CF_JOB_MEDIA_WORKERS for media). Enqueue is non-blocking and
 * fails cleanly before admission; the writer's post-commit consumer turns a
 * failure into a counted best-effort drop and never rewrites the commit.
 * Shutdown discards pending jobs, counts them, and joins the workers.
 * There is no persistence and no startup recovery: a crash loses queued jobs.
 * See src/jobs/jobs.h for the full contract.
 */
#include "jobs/jobs.h"

#include "core/error.h"
#include "db/writer.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- kinds --- */

static const char *const cf_job_kind_names[CF_JOB_KIND_COUNT] = {
    "push_message",
    "deliver_webhook",
    "remove_banned_content",
    "purge_blob",
    "media",
};

const char *cf_job_kind_name(cf_job_kind kind) {
    if ((int)kind < 0 || kind >= CF_JOB_KIND_COUNT) return "unknown";
    return cf_job_kind_names[kind];
}

bool cf_job_kind_of_event(cf_event_kind event, cf_job_kind *out) {
    if (out == NULL) return false;
    switch (event) {
    case CF_EVENT_PUSH_MESSAGE:
        *out = CF_JOB_PUSH_MESSAGE;
        return true;
    case CF_EVENT_DELIVER_WEBHOOK:
        *out = CF_JOB_DELIVER_WEBHOOK;
        return true;
    case CF_EVENT_REMOVE_BANNED_CONTENT:
        *out = CF_JOB_REMOVE_BANNED_CONTENT;
        return true;
    case CF_EVENT_PURGE_BLOB:
        *out = CF_JOB_PURGE_BLOB;
        return true;
    case CF_EVENT_DISCONNECT_USER:
        break; /* mandatory C03 control, never a queued job */
    }
    return false;
}

/* ------------------------------------------------------------- queue --- */

struct cf_job_queue {
    pthread_mutex_t mu;
    pthread_cond_t work_cv;
    cf_job_kind kind;       /* index into struct cf_jobs; for logs */
    bool initialized;       /* slots/threads allocated; dispose frees them */
    bool sync_ready;        /* mutex and condition variable are live */
    bool joined;            /* threads were joined by stop */

    cf_job *slots;          /* ring buffer, capacity entries */
    size_t capacity;
    size_t head;            /* index of the oldest pending job */
    size_t count;           /* pending jobs */
    size_t running;         /* handlers executing right now */
    size_t workers;         /* configured worker threads */
    size_t threads_created;
    pthread_t *threads;

    cf_job_handler_fn handler;
    void *handler_ctx;
    bool stopping;          /* guarded by mu */

    uint64_t accepted;
    uint64_t completed;
    uint64_t failed;
    uint64_t rejected;
    uint64_t shutdown_discarded;
};

struct cf_jobs {
    bool started;
    struct cf_job_queue q[CF_JOB_KIND_COUNT];
};

/* Owned payload strings are freed here; event payloads carry none. */
static void cf_job_dispose(cf_job *job) {
    free((void *)job->task_type);
    free((void *)job->variation);
    job->task_type = NULL;
    job->variation = NULL;
}

/* -------------------------------------------------------- worker loop --- */

static void *cf_job_worker_main(void *arg) {
    struct cf_job_queue *q = arg;

    for (;;) {
        pthread_mutex_lock(&q->mu);
        while (!q->stopping && q->count == 0) {
            pthread_cond_wait(&q->work_cv, &q->mu);
        }
        if (q->stopping) { /* pending work was discarded by stop */
            pthread_mutex_unlock(&q->mu);
            break;
        }
        cf_job job = q->slots[q->head];
        q->head = (q->head + 1) % q->capacity;
        q->count--;
        q->running++;
        cf_job_handler_fn fn = q->handler;
        void *ctx = q->handler_ctx;
        pthread_mutex_unlock(&q->mu);

        cf_err rc = fn != NULL ? fn(ctx, &job) : CF_INTERNAL;
        cf_job_dispose(&job);

        pthread_mutex_lock(&q->mu);
        q->running--;
        if (rc == CF_OK) {
            q->completed++;
        } else {
            q->failed++;
            /* Sanitized: kind name and stable error name only, no payload. */
            fprintf(stderr, "campfire: jobs: %s handler failed (%s)\n",
                    cf_job_kind_name(q->kind), cf_err_name(rc));
        }
        pthread_mutex_unlock(&q->mu);
    }
    return NULL;
}

/* ------------------------------------------------------------ push --- */

/* Append one payload, taking ownership of its owned strings on CF_OK only.
 * Caller performs no allocation while the queue lock is held. */
static cf_err cf_job_queue_push(struct cf_job_queue *q, const cf_job *job) {
    pthread_mutex_lock(&q->mu);
    if (q->stopping) {
        pthread_mutex_unlock(&q->mu);
        return CF_BUSY; /* not accepting: never acknowledged as queued */
    }
    if (q->handler == NULL) {
        pthread_mutex_unlock(&q->mu);
        return CF_INVALID; /* enqueue before registration fails cleanly */
    }
    if (q->count >= q->capacity) {
        q->rejected++;
        pthread_mutex_unlock(&q->mu);
        fprintf(stderr,
                "campfire: jobs: %s queue full (%zu pending), dropping job\n",
                cf_job_kind_name(q->kind), q->capacity);
        return CF_BUSY;
    }
    q->slots[(q->head + q->count) % q->capacity] = *job;
    q->count++;
    q->accepted++;
    pthread_cond_signal(&q->work_cv);
    pthread_mutex_unlock(&q->mu);
    return CF_OK;
}

/* ---------------------------------------------------------- enqueue --- */

cf_err cf_job_enqueue(cf_jobs *jobs, cf_event event) {
    if (jobs == NULL || !jobs->started) return CF_INVALID;
    cf_job_kind kind;
    if (!cf_job_kind_of_event(event.kind, &kind)) return CF_INVALID;

    /* Mirror the writer's committed-event normalization: only the fields the
     * kind uses reach a handler. */
    cf_job job;
    memset(&job, 0, sizeof job);
    job.kind = kind;
    job.user_id = event.user_id;
    job.room_id = event.room_id;
    job.message_id = event.message_id;
    job.blob_id = event.blob_id;
    switch (kind) {
    case CF_JOB_PUSH_MESSAGE:
        job.user_id = 0;
        job.blob_id = 0;
        break;
    case CF_JOB_REMOVE_BANNED_CONTENT:
        job.room_id = 0;
        job.message_id = 0;
        job.blob_id = 0;
        break;
    case CF_JOB_DELIVER_WEBHOOK:
        job.room_id = 0;
        job.blob_id = 0;
        break;
    case CF_JOB_PURGE_BLOB:
        job.user_id = 0;
        job.room_id = 0;
        job.message_id = 0;
        break;
    case CF_JOB_MEDIA:
    case CF_JOB_KIND_COUNT:
        return CF_INVALID; /* unreachable via cf_job_kind_of_event */
    }
    return cf_job_queue_push(&jobs->q[kind], &job);
}

cf_err cf_jobs_enqueue_media(cf_jobs *jobs, int64_t blob_id,
                             cf_span task_type, cf_span variation) {
    if (jobs == NULL || !jobs->started) return CF_INVALID;
    if (task_type.ptr == NULL || task_type.len == 0) return CF_INVALID;
    if (memchr(task_type.ptr, '\0', task_type.len) != NULL) return CF_INVALID;
    if (variation.len != 0 && variation.ptr == NULL) return CF_INVALID;
    if (variation.len != 0 &&
        memchr(variation.ptr, '\0', variation.len) != NULL) {
        return CF_INVALID;
    }

    cf_job job;
    memset(&job, 0, sizeof job);
    job.kind = CF_JOB_MEDIA;
    job.blob_id = blob_id;

    char *type_copy = malloc(task_type.len + 1);
    if (type_copy == NULL) return CF_NOMEM;
    memcpy(type_copy, task_type.ptr, task_type.len);
    type_copy[task_type.len] = '\0';
    job.task_type = type_copy;

    if (variation.len != 0) {
        char *variation_copy = malloc(variation.len + 1);
        if (variation_copy == NULL) {
            cf_job_dispose(&job);
            return CF_NOMEM;
        }
        memcpy(variation_copy, variation.ptr, variation.len);
        variation_copy[variation.len] = '\0';
        job.variation = variation_copy;
    }

    cf_err rc = cf_job_queue_push(&jobs->q[CF_JOB_MEDIA], &job);
    if (rc != CF_OK) cf_job_dispose(&job); /* never owned on failure */
    return rc;
}

/* ----------------------------------------------------- registration --- */

cf_err cf_jobs_set_handler(cf_jobs *jobs, cf_job_kind kind,
                           cf_job_handler_fn fn, void *ctx) {
    if (jobs == NULL || !jobs->started) return CF_INVALID;
    if (fn == NULL || (int)kind < 0 || kind >= CF_JOB_KIND_COUNT) {
        return CF_INVALID;
    }
    struct cf_job_queue *q = &jobs->q[kind];
    pthread_mutex_lock(&q->mu);
    if (q->stopping) {
        pthread_mutex_unlock(&q->mu);
        return CF_BUSY;
    }
    q->handler = fn;
    q->handler_ctx = ctx;
    pthread_mutex_unlock(&q->mu);
    return CF_OK;
}

static cf_err cf_jobs_writer_consumer(void *ctx, const cf_event *event) {
    if (event == NULL) return CF_INVALID;
    return cf_job_enqueue((cf_jobs *)ctx, *event);
}

cf_err cf_jobs_register_writer(cf_jobs *jobs, cf_app *app) {
    if (jobs == NULL || !jobs->started || app == NULL) return CF_INVALID;
    static const cf_event_kind best_effort[4] = {
        CF_EVENT_PUSH_MESSAGE,
        CF_EVENT_DELIVER_WEBHOOK,
        CF_EVENT_REMOVE_BANNED_CONTENT,
        CF_EVENT_PURGE_BLOB,
    };
    for (size_t i = 0; i < sizeof best_effort / sizeof best_effort[0]; i++) {
        cf_err rc = cf_writer_set_event_handler(
            app, best_effort[i], cf_jobs_writer_consumer, jobs);
        if (rc != CF_OK) return rc;
    }
    return CF_OK;
}

/* ------------------------------------------------------------ stats --- */

cf_err cf_jobs_stats_get(const cf_jobs *jobs, cf_job_kind kind,
                         cf_job_stats *out) {
    if (jobs == NULL || out == NULL) return CF_INVALID;
    if (!jobs->started || (int)kind < 0 || kind >= CF_JOB_KIND_COUNT) {
        return CF_INVALID;
    }
    struct cf_job_queue *q = (struct cf_job_queue *)&jobs->q[kind];
    pthread_mutex_lock(&q->mu);
    memset(out, 0, sizeof *out);
    out->capacity = q->capacity;
    out->pending = q->count;
    out->running = q->running;
    out->workers = q->workers;
    out->handler_registered = q->handler != NULL;
    out->accepted = q->accepted;
    out->completed = q->completed;
    out->failed = q->failed;
    out->rejected = q->rejected;
    out->shutdown_discarded = q->shutdown_discarded;
    pthread_mutex_unlock(&q->mu);
    return CF_OK;
}

/* ------------------------------------------------------- lifecycle --- */

static void cf_job_queue_dispose(struct cf_job_queue *q) {
    if (!q->initialized) return;
    free(q->slots);
    free(q->threads);
    q->slots = NULL;
    q->threads = NULL;
    if (q->sync_ready) {
        pthread_cond_destroy(&q->work_cv);
        pthread_mutex_destroy(&q->mu);
        q->sync_ready = false;
    }
    q->initialized = false;
}

cf_err cf_jobs_start(const cf_config *config, cf_jobs **out) {
    if (out == NULL) return CF_INVALID;
    *out = NULL;
    if (config == NULL) return CF_INVALID;

    size_t capacity = config->job_queue;
    size_t event_workers = config->job_workers;
    if (capacity == 0 || event_workers == 0) return CF_INVALID;
    if (capacity > SIZE_MAX / sizeof(cf_job)) return CF_LIMIT;

    cf_jobs *jobs = calloc(1, sizeof *jobs);
    if (jobs == NULL) return CF_NOMEM;

    cf_err rc = CF_OK;
    for (size_t k = 0; k < CF_JOB_KIND_COUNT; k++) {
        struct cf_job_queue *q = &jobs->q[k];
        q->kind = (cf_job_kind)k;
        q->capacity = capacity;
        q->workers = k == CF_JOB_MEDIA ? CF_JOB_MEDIA_WORKERS : event_workers;
        q->slots = calloc(q->capacity, sizeof *q->slots);
        q->threads = calloc(q->workers, sizeof *q->threads);
        q->initialized = true; /* dispose frees whatever was allocated */
        if (q->slots == NULL || q->threads == NULL) {
            rc = CF_NOMEM;
            goto fail;
        }
        if (pthread_mutex_init(&q->mu, NULL) != 0) {
            rc = CF_INTERNAL;
            goto fail;
        }
        if (pthread_cond_init(&q->work_cv, NULL) != 0) {
            pthread_mutex_destroy(&q->mu);
            rc = CF_INTERNAL;
            goto fail;
        }
        q->sync_ready = true;
    }

    for (size_t k = 0; k < CF_JOB_KIND_COUNT; k++) {
        struct cf_job_queue *q = &jobs->q[k];
        for (size_t i = 0; i < q->workers; i++) {
            if (pthread_create(&q->threads[i], NULL, cf_job_worker_main, q) !=
                0) {
                rc = CF_INTERNAL;
                goto fail;
            }
            q->threads_created++;
        }
    }

    jobs->started = true;
    *out = jobs;
    return CF_OK;

fail:
    /* Stop every initialized queue, then join only the threads that were
     * created. On failure nothing is returned to the caller. */
    for (size_t k = 0; k < CF_JOB_KIND_COUNT; k++) {
        struct cf_job_queue *q = &jobs->q[k];
        if (!q->sync_ready) continue; /* no worker was created for it */
        pthread_mutex_lock(&q->mu);
        q->stopping = true;
        for (size_t i = 0; i < q->count; i++) {
            cf_job_dispose(&q->slots[(q->head + i) % q->capacity]);
        }
        q->count = 0;
        q->head = 0;
        pthread_cond_broadcast(&q->work_cv);
        pthread_mutex_unlock(&q->mu);
    }
    for (size_t k = 0; k < CF_JOB_KIND_COUNT; k++) {
        struct cf_job_queue *q = &jobs->q[k];
        for (size_t i = 0; i < q->threads_created; i++) {
            (void)pthread_join(q->threads[i], NULL);
        }
        cf_job_queue_dispose(q);
    }
    free(jobs);
    return rc;
}

void cf_jobs_stop(cf_jobs *jobs) {
    if (jobs == NULL || !jobs->started) return;

    /* Stop accepting and discard every pending job, counting it. A handler
     * already running is allowed to finish; a blocked subprocess is killed at
     * its own timeout (S01). */
    for (size_t k = 0; k < CF_JOB_KIND_COUNT; k++) {
        struct cf_job_queue *q = &jobs->q[k];
        if (!q->initialized) continue;
        pthread_mutex_lock(&q->mu);
        if (!q->stopping) {
            q->stopping = true;
            size_t discarded = q->count;
            for (size_t i = 0; i < discarded; i++) {
                cf_job_dispose(&q->slots[(q->head + i) % q->capacity]);
            }
            q->count = 0;
            q->head = 0;
            q->shutdown_discarded += discarded;
            if (discarded > 0) {
                fprintf(stderr,
                        "campfire: jobs: %s discarded %zu queued jobs at "
                        "shutdown\n",
                        cf_job_kind_name((cf_job_kind)k), discarded);
            }
        }
        pthread_cond_broadcast(&q->work_cv);
        pthread_mutex_unlock(&q->mu);
    }

    for (size_t k = 0; k < CF_JOB_KIND_COUNT; k++) {
        struct cf_job_queue *q = &jobs->q[k];
        if (!q->initialized || q->joined) continue;
        for (size_t i = 0; i < q->threads_created; i++) {
            (void)pthread_join(q->threads[i], NULL);
        }
        q->joined = true;
    }
}

void cf_jobs_destroy(cf_jobs *jobs) {
    if (jobs == NULL) return;
    cf_jobs_stop(jobs);
    for (size_t k = 0; k < CF_JOB_KIND_COUNT; k++) {
        struct cf_job_queue *q = &jobs->q[k];
        if (!q->initialized) continue;
        /* Nothing can be pending after stop; owned payloads were freed. */
        cf_job_queue_dispose(q);
    }
    jobs->started = false;
    free(jobs);
}
