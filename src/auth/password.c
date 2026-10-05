/* src/auth/password.c — has_secure_password's bcrypt through the pinned
 * libxcrypt build (rails_compat password.rs: `$2a$` digests, cost 12, only
 * the first 72 password bytes count).  The ratified A01 boundary
 * `cf_password_verify` also serves src/models/user.c.
 *
 * This file is also the bounded crypto queue required by 00-contracts.md
 * ("Bcrypt goes to a separate bounded crypto queue; a request worker may wait
 * for it with no active read transaction/statement"): every crypt_r call in
 * the application is behind the queue below, so sign-in verification
 * (cf_user_authenticated -> cf_password_verify), the constant dummy
 * verification for unknown accounts, and first-run setup hashing
 * (cf_auth_password_digest) all run on CF_CRYPTO_WORKERS worker threads with
 * 01-foundation-http.md's fixed 32-job pending bound.  Submitters block while
 * the queue is full (backpressure; no dropped request), only the submitter
 * thread waits, and its reader connection stays idle.  Waiting callers must
 * have ended read transactions and reset statements first: the sign-in lookup
 * ends its statement in user_lookup, and setup hashes before cf_write, after
 * the last account-count read.
 *
 * The bound is unconditional once a queue has been started (P12-06): the
 * queue is sticky per process, and a submission that cannot be prepared or is
 * refused/discarded by a stopping queue fails the caller with the documented
 * failure instead of running bcrypt on the calling thread.  The only inline
 * bcrypt path left is a process that never starts the queue at all (unit
 * tests, tools); it is counted in inline_fallbacks.  See the wrappers below
 * for the exact per-arm semantics. */

#include "internal.h"

#include <crypt.h>

#include <pthread.h>
#include <stdatomic.h>

#include <stdlib.h>
#include <string.h>

/* ---- bcrypt body (queue worker and the no-queue-at-all mode) -------------- */

/* crypt_r takes C strings; bcrypt-ruby's C implementation reads to the first
 * NUL, which is the behavior both the D01 stand-in and the pinned Rust
 * verifier exhibit. */
static bool auth_crypt_matches(cf_span password, cf_span digest) {
    if (digest.ptr == NULL || digest.len == 0) return false;
    if (password.ptr == NULL && password.len != 0) return false;
    char *pw = malloc(password.len + 1);
    char *dg = malloc(digest.len + 1);
    if (pw == NULL || dg == NULL) {
        free(pw);
        free(dg);
        return false;
    }
    if (password.len != 0) memcpy(pw, password.ptr, password.len);
    pw[password.len] = '\0';
    memcpy(dg, digest.ptr, digest.len);
    dg[digest.len] = '\0';
    struct crypt_data data;
    memset(&data, 0, sizeof data);
    char *got = crypt_r(pw, dg, &data);
    bool ok = got != NULL && strcmp(got, dg) == 0;
    free(pw);
    free(dg);
    return ok;
}

static cf_span auth_str_span(cf_str str) {
    return (cf_span){(const unsigned char *)str.ptr, str.len};
}

/* BCrypt::Password.create body.  Reached only from the queue worker or the
 * no-queue-at-all mode (a process that never starts the queue); a started
 * queue never falls back to this on the calling thread.  Validates exactly as
 * the public wrapper always has (out cleared; cost range; NULL pointer with a
 * non-zero length). */
static cf_err auth_password_digest_inline(cf_str password, int cost,
                                          cf_str *out) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    if (cost < CF_AUTH_BCRYPT_MIN_COST || cost > CF_AUTH_BCRYPT_MAX_COST) {
        return CF_INVALID;
    }
    if (password.ptr == NULL && password.len != 0) return CF_INVALID;
    /* BCrypt::Password.create: a fresh 16-byte salt from the OS entropy
     * source, then the `$2a$` hash. */
    unsigned char entropy[16];
    cf_err rc = cf_random_bytes(entropy, sizeof entropy);
    if (rc != CF_OK) return rc;
    char setting[CRYPT_GENSALT_OUTPUT_SIZE];
    memset(setting, 0, sizeof setting);
    if (crypt_gensalt_rn("$2a$", (unsigned long)cost,
                         (const char *)entropy, (int)sizeof entropy, setting,
                         (int)sizeof setting) == NULL) {
        return CF_INTERNAL;
    }
    char *pw = malloc(password.len + 1);
    if (pw == NULL) return CF_NOMEM;
    if (password.len != 0) memcpy(pw, password.ptr, password.len);
    pw[password.len] = '\0';
    struct crypt_data data;
    memset(&data, 0, sizeof data);
    char *got = crypt_r(pw, setting, &data);
    free(pw);
    if (got == NULL) return CF_INTERNAL;
    rc = auth_str_dup(auth_cstr_span(got), out);
    return rc;
}

/* ---- bounded crypto queue ------------------------------------------------- */

/* 01-foundation-http.md: CF_CRYPTO_WORKERS=2, queue 32.  The pending bound is
 * fixed (a constant, like the other initial limits); only the worker count is
 * configuration. */
#define CF_CRYPTO_QUEUE_PENDING_CAP 32u

typedef enum {
    CF_CRYPTO_JOB_VERIFY = 0,
    CF_CRYPTO_JOB_DIGEST
} cf_crypto_job_kind;

/* One job: the caller's thread owns the struct on its stack and blocks in
 * crypto_queue_run until `done`; the worker writes the result fields.  The
 * queue link is preallocated in the struct, and every input byte is an owned
 * copy made by the caller (task carries owned inputs). */
typedef struct cf_crypto_job {
    cf_crypto_job_kind kind;
    cf_str password;   /* owned copy */
    cf_str digest;     /* owned copy; verify only */
    int cost;          /* digest only */
    bool ok;           /* verify result */
    cf_err rc;         /* execution result; CF_BUSY when discarded at stop */
    cf_str digest_out; /* owned digest result */
    bool done;
    pthread_mutex_t done_mutex;
    pthread_cond_t done_cv;
    struct cf_crypto_job *next;
} cf_crypto_job;

/* All state is guarded by crypto_mutex except crypto_required,
 * crypto_inline_fallbacks and crypto_unprepared (atomics for the lock-free
 * fast path).  Lifecycle (start/stop) is single-threaded application
 * lifecycle code. */
static pthread_mutex_t crypto_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t crypto_work_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t crypto_space_cv = PTHREAD_COND_INITIALIZER;
static cf_crypto_job *crypto_head, *crypto_tail;
static size_t crypto_pending, crypto_running;
static pthread_t *crypto_threads;
static size_t crypto_workers;
static size_t crypto_refs; /* started apps sharing the one process queue */
static bool crypto_started, crypto_stopping;
static cf_crypto_queue_stats crypto_stats;
static cf_crypto_queue_observer crypto_observer;
static void *crypto_observer_ctx;

/* Sticky: set by the first successful start and never cleared.  From then on
 * every bcrypt call must be executed by the queue or fail; a caller that
 * arrives while the queue is stopping, or after it stopped, is refused
 * (counted) rather than silently running bcrypt inline.  Only a process that
 * never starts a queue (unit tests, tools) uses the inline mode. */
static _Atomic bool crypto_required;

/* bcrypt-boundary executions on the calling thread because no queue was ever
 * started in this process (the no-queue-at-all mode).  A started queue never
 * increments this: its failure arms refuse instead (see the wrappers). */
static _Atomic uint64_t crypto_inline_fallbacks;

/* Submissions that could not be prepared for the queue (the owned-input copy
 * or the job's synchronization init failed).  The caller received the
 * documented failure; bcrypt did not run for the job. */
static _Atomic uint64_t crypto_unprepared;

/* Test-only fault injection (same style as the observer below): the next
 * `count` owned-input copies fail, so job preparation returns CF_NOMEM
 * without touching the allocator.  0 disarms. */
static _Atomic unsigned crypto_test_copy_failures;

static void crypto_count_inline_fallback(void) {
    atomic_fetch_add_explicit(&crypto_inline_fallbacks, 1,
                              memory_order_relaxed);
}

static void crypto_count_unprepared(void) {
    atomic_fetch_add_explicit(&crypto_unprepared, 1, memory_order_relaxed);
}

/* Consumes one armed test failure, if any. */
static bool crypto_copy_test_fails(void) {
    unsigned count = atomic_load_explicit(&crypto_test_copy_failures,
                                          memory_order_relaxed);
    while (count > 0) {
        if (atomic_compare_exchange_weak_explicit(
                &crypto_test_copy_failures, &count, count - 1,
                memory_order_relaxed, memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

static bool crypto_copy(cf_span src, cf_str *out) {
    out->ptr = NULL;
    out->len = 0;
    if (src.ptr == NULL && src.len != 0) return false;
    if (crypto_copy_test_fails()) return false;
    char *copy = malloc(src.len + 1); /* +1 keeps the empty span owned */
    if (copy == NULL) return false;
    if (src.len != 0) memcpy(copy, src.ptr, src.len);
    copy[src.len] = '\0';
    out->ptr = copy;
    out->len = src.len;
    return true;
}

/* Prepares a stack job; returns CF_NOMEM/CF_INTERNAL without leaving owned
 * bytes behind.  The caller disposes a prepared job exactly once. */
static cf_err crypto_job_init(cf_crypto_job *job, cf_crypto_job_kind kind,
                              cf_str password, cf_str digest, int cost) {
    memset(job, 0, sizeof *job);
    job->kind = kind;
    job->cost = cost;
    job->rc = CF_INTERNAL;
    if (!crypto_copy(auth_str_span(password), &job->password)) {
        return CF_NOMEM;
    }
    if (kind == CF_CRYPTO_JOB_VERIFY &&
        !crypto_copy(auth_str_span(digest), &job->digest)) {
        cf_str_dispose(&job->password);
        return CF_NOMEM;
    }
    if (pthread_mutex_init(&job->done_mutex, NULL) != 0) {
        cf_str_dispose(&job->password);
        cf_str_dispose(&job->digest);
        return CF_INTERNAL;
    }
    if (pthread_cond_init(&job->done_cv, NULL) != 0) {
        pthread_mutex_destroy(&job->done_mutex);
        cf_str_dispose(&job->password);
        cf_str_dispose(&job->digest);
        return CF_INTERNAL;
    }
    return CF_OK;
}

static void crypto_job_dispose(cf_crypto_job *job) {
    cf_str_dispose(&job->password);
    cf_str_dispose(&job->digest);
    cf_str_dispose(&job->digest_out);
    pthread_cond_destroy(&job->done_cv);
    pthread_mutex_destroy(&job->done_mutex);
}

static void crypto_execute(cf_crypto_job *job) {
    if (job->kind == CF_CRYPTO_JOB_VERIFY) {
        job->ok = auth_crypt_matches(auth_str_span(job->password),
                                     auth_str_span(job->digest));
        job->rc = CF_OK;
    } else {
        job->rc = auth_password_digest_inline(job->password, job->cost,
                                              &job->digest_out);
    }
}

static void *crypto_worker_main(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&crypto_mutex);
        while (crypto_head == NULL && !crypto_stopping) {
            pthread_cond_wait(&crypto_work_cv, &crypto_mutex);
        }
        if (crypto_stopping) {
            /* Shutdown: the stop path discards queued jobs, counts them and
             * wakes their waiters to the documented failure; a worker never
             * takes new work after it.  Nothing is executed inline. */
            pthread_mutex_unlock(&crypto_mutex);
            break;
        }
        cf_crypto_job *job = crypto_head;
        crypto_head = job->next;
        if (crypto_head == NULL) crypto_tail = NULL;
        crypto_pending--;
        crypto_running++;
        if (crypto_running > crypto_stats.running_max) {
            crypto_stats.running_max = crypto_running;
        }
        cf_crypto_queue_observer observer = crypto_observer;
        void *observer_ctx = crypto_observer_ctx;
        pthread_cond_broadcast(&crypto_space_cv); /* capacity freed */
        pthread_mutex_unlock(&crypto_mutex);

        if (observer != NULL) observer(observer_ctx);
        crypto_execute(job);

        pthread_mutex_lock(&crypto_mutex);
        crypto_running--;
        crypto_stats.completed++;
        pthread_mutex_unlock(&crypto_mutex);

        pthread_mutex_lock(&job->done_mutex);
        job->done = true;
        pthread_cond_signal(&job->done_cv);
        pthread_mutex_unlock(&job->done_mutex);
    }
    return NULL;
}

/* Submits and waits.  CF_OK: a worker executed the job (job->rc is the
 * result).  CF_BUSY: the job was not accepted (the queue is stopping or
 * already stopped) or was discarded by a racing stop; the caller must
 * propagate its documented failure (never run bcrypt inline).  Nothing is
 * left queued, so no request hangs or is silently dropped. */
static cf_err crypto_queue_run(cf_crypto_job *job) {
    pthread_mutex_lock(&crypto_mutex);
    if (!crypto_started || crypto_stopping) {
        crypto_stats.refused++;
        pthread_mutex_unlock(&crypto_mutex);
        return CF_BUSY;
    }
    /* Queue full: explicit backpressure, counted.  The request worker may
     * wait (00-contracts); only the submitter blocks, its reader idle. */
    while (crypto_pending >= CF_CRYPTO_QUEUE_PENDING_CAP && !crypto_stopping) {
        crypto_stats.saturated++;
        pthread_cond_wait(&crypto_space_cv, &crypto_mutex);
    }
    if (crypto_stopping || !crypto_started) {
        crypto_stats.refused++;
        pthread_mutex_unlock(&crypto_mutex);
        return CF_BUSY;
    }
    job->next = NULL;
    if (crypto_tail != NULL) {
        crypto_tail->next = job;
    } else {
        crypto_head = job;
    }
    crypto_tail = job;
    crypto_pending++;
    if (crypto_pending > crypto_stats.pending_max) {
        crypto_stats.pending_max = crypto_pending;
    }
    crypto_stats.submitted++;
    pthread_cond_signal(&crypto_work_cv);
    pthread_mutex_unlock(&crypto_mutex);

    /* The request worker waits here with no read transaction or statement
     * active; the queue delivers the completion to this thread. */
    pthread_mutex_lock(&job->done_mutex);
    while (!job->done) pthread_cond_wait(&job->done_cv, &job->done_mutex);
    pthread_mutex_unlock(&job->done_mutex);
    return job->rc;
}

cf_err cf_crypto_queue_start(size_t workers) {
    if (workers == 0) return CF_INVALID;
    pthread_mutex_lock(&crypto_mutex);
    if (crypto_started) {
        /* One queue per process shared by concurrent app instances (test
         * fixtures, embedders): the first start fixes the worker count, later
         * starts must agree and take a reference.  A queue being torn down
         * cannot be joined. */
        if (crypto_stopping || workers != crypto_workers) {
            pthread_mutex_unlock(&crypto_mutex);
            return CF_BUSY;
        }
        crypto_refs++;
        pthread_mutex_unlock(&crypto_mutex);
        return CF_OK;
    }
    pthread_mutex_unlock(&crypto_mutex);

    pthread_t *threads = calloc(workers, sizeof *threads);
    if (threads == NULL) return CF_NOMEM;
    size_t created = 0;
    while (created < workers) {
        if (pthread_create(&threads[created], NULL, crypto_worker_main,
                           NULL) != 0) {
            break;
        }
        created++;
    }
    if (created != workers) {
        pthread_mutex_lock(&crypto_mutex);
        crypto_stopping = true;
        pthread_cond_broadcast(&crypto_work_cv);
        pthread_mutex_unlock(&crypto_mutex);
        for (size_t i = 0; i < created; i++) {
            (void)pthread_join(threads[i], NULL);
        }
        free(threads);
        pthread_mutex_lock(&crypto_mutex);
        crypto_stopping = false;
        pthread_mutex_unlock(&crypto_mutex);
        return CF_INTERNAL;
    }

    atomic_store_explicit(&crypto_inline_fallbacks, 0, memory_order_relaxed);
    atomic_store_explicit(&crypto_unprepared, 0, memory_order_relaxed);
    pthread_mutex_lock(&crypto_mutex);
    memset(&crypto_stats, 0, sizeof crypto_stats);
    crypto_stats.workers = workers;
    crypto_stats.pending_capacity = CF_CRYPTO_QUEUE_PENDING_CAP;
    crypto_head = NULL;
    crypto_tail = NULL;
    crypto_pending = 0;
    crypto_running = 0;
    crypto_threads = threads;
    crypto_workers = workers;
    crypto_refs = 1;
    crypto_stopping = false;
    crypto_started = true;
    /* Sticky while the mutex still publishes crypto_started: from a
     * successful start on, bcrypt is queue-or-fail for this process. */
    atomic_store_explicit(&crypto_required, true, memory_order_release);
    pthread_mutex_unlock(&crypto_mutex);
    return CF_OK;
}

void cf_crypto_queue_stop(void) {
    pthread_mutex_lock(&crypto_mutex);
    if (!crypto_started) {
        pthread_mutex_unlock(&crypto_mutex);
        return;
    }
    if (crypto_refs > 1) { /* another app instance still uses the queue */
        crypto_refs--;
        pthread_mutex_unlock(&crypto_mutex);
        return;
    }
    crypto_stopping = true;
    pthread_cond_broadcast(&crypto_work_cv);
    pthread_cond_broadcast(&crypto_space_cv);
    pthread_t *threads = crypto_threads;
    size_t workers = crypto_workers;
    pthread_mutex_unlock(&crypto_mutex);

    /* Workers finish the job in their slot and exit; joins bound the stop by
     * one bcrypt per worker. */
    for (size_t i = 0; i < workers; i++) {
        (void)pthread_join(threads[i], NULL);
    }
    free(threads);

    pthread_mutex_lock(&crypto_mutex);
    /* Queued work is discarded here and counted, and every waiting submitter
     * is woken with CF_BUSY: the caller propagates its documented failure and
     * never runs bcrypt inline, so the request is never silently dropped and
     * the worker bound is not bypassed at shutdown. */
    cf_crypto_job *job = crypto_head;
    crypto_head = NULL;
    crypto_tail = NULL;
    while (job != NULL) {
        cf_crypto_job *next = job->next;
        job->next = NULL;
        job->rc = CF_BUSY;
        crypto_stats.discarded++;
        pthread_mutex_lock(&job->done_mutex);
        job->done = true;
        pthread_cond_signal(&job->done_cv);
        pthread_mutex_unlock(&job->done_mutex);
        job = next;
    }
    crypto_pending = 0;
    crypto_running = 0;
    crypto_threads = NULL;
    crypto_workers = 0;
    crypto_refs = 0;
    crypto_started = false;
    crypto_stopping = false;
    /* crypto_required stays true: a stopped process still refuses bcrypt
     * inline; only a process that never started a queue has that mode. */
    pthread_mutex_unlock(&crypto_mutex);
}

bool cf_crypto_queue_stats_get(cf_crypto_queue_stats *out) {
    if (out == NULL) return false;
    pthread_mutex_lock(&crypto_mutex);
    *out = crypto_stats;
    out->running = crypto_started && !crypto_stopping;
    pthread_mutex_unlock(&crypto_mutex);
    out->inline_fallbacks =
        atomic_load_explicit(&crypto_inline_fallbacks, memory_order_relaxed);
    out->unprepared =
        atomic_load_explicit(&crypto_unprepared, memory_order_relaxed);
    return true;
}

void cf_crypto_queue_set_test_observer(cf_crypto_queue_observer fn,
                                       void *ctx) {
    pthread_mutex_lock(&crypto_mutex);
    crypto_observer = fn;
    crypto_observer_ctx = ctx;
    pthread_mutex_unlock(&crypto_mutex);
}

void cf_crypto_queue_set_test_copy_failure(unsigned count) {
    atomic_store_explicit(&crypto_test_copy_failures, count,
                          memory_order_relaxed);
}

void cf_crypto_queue_test_reset_required(void) {
    /* Test-only: restore the never-started-queue mode so an inline case can
     * run after a queue case in the same process (Fil-C registers cases
     * without source order).  Production never clears the sticky
     * requirement. */
    atomic_store_explicit(&crypto_required, false, memory_order_release);
}

/* ---- public bcrypt boundary ---------------------------------------------- */

/* P12-06 failure semantics (the bound is unconditional once a queue has been
 * started):
 *   - malformed spans (empty/absent digest, NULL pointer with a non-zero
 *     length) are false decisions, exactly as before, and run no bcrypt;
 *   - a process that never started a queue runs bcrypt inline and counts it
 *     in inline_fallbacks;
 *   - once a queue has been started, a job that cannot be prepared
 *     (CF_NOMEM/CF_INTERNAL) is counted in unprepared and the caller receives
 *     the documented failure (false here), and a submission the stopping or
 *     stopped queue refuses or discards yields CF_BUSY (counted in refused
 *     or discarded).  Neither arm ever hashes on the calling thread.
 * `false` is the only failure value the ratified bool boundary can carry, so
 * sign-in treats a refused verification as a non-match (fail closed); see the
 * evidence for the caller-by-caller behavior. */
bool cf_password_verify(cf_str password, cf_str digest) {
    cf_span pw = auth_str_span(password);
    cf_span dg = auth_str_span(digest);
    /* The malformed-span answers are decided without bcrypt, exactly as
     * before, and never touch the queue (a NULL pointer with a non-zero
     * length must not be copied). */
    if (dg.ptr == NULL || dg.len == 0 ||
        (pw.ptr == NULL && pw.len != 0)) {
        return false;
    }
    if (atomic_load_explicit(&crypto_required, memory_order_acquire)) {
        cf_crypto_job job;
        cf_err init = crypto_job_init(&job, CF_CRYPTO_JOB_VERIFY, password,
                                      digest, 0);
        if (init != CF_OK) {
            crypto_count_unprepared();
            return false; /* documented failure; never inline */
        }
        cf_err rc = crypto_queue_run(&job);
        bool ok = rc == CF_OK && job.ok;
        crypto_job_dispose(&job);
        return ok;
    }
    crypto_count_inline_fallback();
    return auth_crypt_matches(pw, dg);
}

/* Same P12-06 failure semantics as cf_password_verify, with an error channel:
 * an unprepared job returns its CF_NOMEM/CF_INTERNAL, and a refusal/discard
 * returns CF_BUSY.  *out is cleared on every failure, so the setup action
 * (src/actions/first_runs.c) fails before its cf_write and the request
 * answers its 500 path.  Never hashes on the calling thread once a queue has
 * been started. */
cf_err cf_auth_password_digest(cf_str password, int cost, cf_str *out) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    if (cost < CF_AUTH_BCRYPT_MIN_COST || cost > CF_AUTH_BCRYPT_MAX_COST) {
        return CF_INVALID;
    }
    if (password.ptr == NULL && password.len != 0) return CF_INVALID;
    if (atomic_load_explicit(&crypto_required, memory_order_acquire)) {
        cf_crypto_job job;
        cf_err init = crypto_job_init(&job, CF_CRYPTO_JOB_DIGEST, password,
                                      (cf_str){NULL, 0}, cost);
        if (init != CF_OK) {
            crypto_count_unprepared();
            return init; /* documented failure; never inline */
        }
        cf_err rc = crypto_queue_run(&job);
        if (rc == CF_OK) {
            cf_err job_rc = job.rc;
            if (job_rc == CF_OK) {
                *out = job.digest_out;
                job.digest_out = (cf_str){NULL, 0};
            }
            crypto_job_dispose(&job);
            return job_rc;
        }
        crypto_job_dispose(&job);
        return rc; /* CF_BUSY: refused or discarded at a stop; never inline */
    }
    crypto_count_inline_fallback();
    return auth_password_digest_inline(password, cost, out);
}
