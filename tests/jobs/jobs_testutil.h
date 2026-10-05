/* Test-only helpers for the J01 job-queue tests (tests/jobs).
 *
 * A controllable handler ("gate") whose invocation count, blocking state,
 * return value and observed payload are settable from the test thread, plus
 * bounded waits for observable state. Waits spin on counters with a deadline;
 * they never assume a race was won by sleeping. Not production code and not
 * part of any application API.
 */
#ifndef CF_JOBS_TESTUTIL_H
#define CF_JOBS_TESTUTIL_H

#include "cf_test.h"
#include "jobs/jobs.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CF_JOB_GATE_IDS 512

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    size_t calls;        /* handler invocations, total */
    size_t entered;      /* invocations that reached the gate */
    bool blocking;       /* while true, wait for release */
    bool released;       /* test sets this to let blocked handlers return */
    cf_err result;       /* value every invocation returns */
    int64_t ids[CF_JOB_GATE_IDS]; /* observed message/blob ids, in order */
    size_t id_count;
    int64_t last_blob_id, last_message_id, last_room_id, last_user_id;
    char last_task_type[64];
    char last_variation[64];
    bool last_kind_set;
    cf_job_kind last_kind;
} job_gate;

static inline void job_gate_init(job_gate *g) {
    memset(g, 0, sizeof *g);
    g->result = CF_OK;
    pthread_mutex_init(&g->mu, NULL);
    pthread_cond_init(&g->cv, NULL);
}

static inline void job_gate_dispose(job_gate *g) {
    pthread_cond_destroy(&g->cv);
    pthread_mutex_destroy(&g->mu);
}

/* Let every blocked handler return; blocking stays off afterwards. */
static inline void job_gate_release(job_gate *g) {
    pthread_mutex_lock(&g->mu);
    g->blocking = false;
    g->released = true;
    pthread_cond_broadcast(&g->cv);
    pthread_mutex_unlock(&g->mu);
}

static inline void job_gate_block(job_gate *g, cf_err result) {
    pthread_mutex_lock(&g->mu);
    g->blocking = true;
    g->released = false;
    g->result = result;
    pthread_mutex_unlock(&g->mu);
}

static inline cf_err job_gate_handler(void *ctx, const cf_job *job) {
    job_gate *g = ctx;
    pthread_mutex_lock(&g->mu);
    g->calls++;
    g->entered++;
    g->last_kind = job->kind;
    g->last_kind_set = true;
    g->last_blob_id = job->blob_id;
    g->last_message_id = job->message_id;
    g->last_room_id = job->room_id;
    g->last_user_id = job->user_id;
    if (job->task_type != NULL) {
        snprintf(g->last_task_type, sizeof g->last_task_type, "%s",
                 job->task_type);
    }
    if (job->variation != NULL) {
        snprintf(g->last_variation, sizeof g->last_variation, "%s",
                 job->variation);
    }
    if (g->id_count < CF_JOB_GATE_IDS) {
        g->ids[g->id_count++] =
            job->blob_id != 0 ? job->blob_id : job->message_id;
    }
    pthread_cond_broadcast(&g->cv);
    while (g->blocking && !g->released) {
        pthread_cond_wait(&g->cv, &g->mu);
    }
    cf_err rc = g->result;
    pthread_mutex_unlock(&g->mu);
    return rc;
}

static inline void cf_job_test_deadline(struct timespec *ts, long ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

/* Wait (bounded) for at least n handler invocations to have entered. */
static inline bool job_gate_wait_entered(job_gate *g, size_t n, long ms) {
    struct timespec deadline;
    cf_job_test_deadline(&deadline, ms);
    pthread_mutex_lock(&g->mu);
    bool ok = true;
    while (g->entered < n) {
        if (pthread_cond_timedwait(&g->cv, &g->mu, &deadline) == ETIMEDOUT) {
            ok = false;
            break;
        }
    }
    ok = ok && g->entered >= n;
    pthread_mutex_unlock(&g->mu);
    return ok;
}

/* Bounded wait for a counter read through cf_jobs_stats_get: polls at 1 ms.
 * Used to await a definite observable state (never to win a race). */
typedef uint64_t (*cf_job_stat_field_fn)(const cf_job_stats *);

static inline uint64_t cf_job_stat_completed(const cf_job_stats *s) {
    return s->completed;
}
static inline uint64_t cf_job_stat_accepted(const cf_job_stats *s) {
    return s->accepted;
}
static inline uint64_t cf_job_stat_failed(const cf_job_stats *s) {
    return s->failed;
}
static inline uint64_t cf_job_stat_discarded(const cf_job_stats *s) {
    return s->shutdown_discarded;
}
static inline uint64_t cf_job_stat_pending(const cf_job_stats *s) {
    return s->pending;
}

static inline bool cf_jobs_wait_for(const cf_jobs *jobs, cf_job_kind kind,
                                    cf_job_stat_field_fn field, uint64_t target,
                                    long ms) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        cf_job_stats stats;
        if (cf_jobs_stats_get(jobs, kind, &stats) != CF_OK) return false;
        if (field(&stats) >= target) return true;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (long)((now.tv_sec - start.tv_sec) * 1000 +
                              (now.tv_nsec - start.tv_nsec) / 1000000L);
        if (elapsed >= ms) return false;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000L};
        nanosleep(&pause, NULL);
    }
}

static inline bool cf_jobs_wait_pending(const cf_jobs *jobs, cf_job_kind kind,
                                        uint64_t target, long ms) {
    return cf_jobs_wait_for(jobs, kind, cf_job_stat_pending, target, ms);
}

static inline cf_span cf_span_lit(const char *s) {
    cf_span span = {(const unsigned char *)s, strlen(s)};
    return span;
}

#endif /* CF_JOBS_TESTUTIL_H */
