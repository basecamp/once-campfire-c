/* Bounded per-kind job queues and accounting (J01; 04-cable-jobs.md
 * "J01/J02: bounded jobs"; 01-foundation-http.md "Configuration: fixed
 * defaults").
 *
 * One FIFO per job kind with CF_JOB_QUEUE pending slots (default 128); each
 * kind has its own mutex/condition variable and its own workers, so a slow or
 * full kind never holds up another (JOB-01/JOB-02). The four model-event
 * kinds use CF_JOB_WORKERS workers each (default 2); the storage-owned media
 * kind is separately capped at CF_JOB_MEDIA_WORKERS (4).
 *
 * Payloads carry database IDs and queue-owned copies only. They never carry a
 * request, task, connection, SQLite or cache pointer, so a job can run after
 * the request that produced it is gone. Event payloads are plain cf_job
 * values; the media payload owns copies of its task-type/variation strings.
 *
 * Enqueue is non-blocking (the writer thread calls it from post-commit) and
 * never acknowledges a dropped job:
 *   - CF_OK    accepted into the kind's FIFO (accepted++);
 *   - CF_BUSY  FIFO full (rejected++ and a sanitized log) or the queue is
 *              stopped. CF_BUSY always means "not enqueued".
 *   - CF_INVALID  no handler is registered for that kind yet, the kind is not
 *              a queueable job, or an argument is invalid. Refusals before
 *              admission are not counted as accepted and are not counted as
 *              rejected (rejected is the queue-full counter); the event
 *              source records its own failure. The writer's post-commit
 *              consumer counts the drop in best_effort_dropped.
 *
 * Accounting per kind (queryable with cf_jobs_stats_get):
 *   accepted    appended to the FIFO
 *   completed   handler returned CF_OK
 *   failed      handler returned any non-CF_OK value (counted once, no retry)
 *   rejected    append refused because the FIFO was full
 *   shutdown_discarded  accepted and still pending when cf_jobs_stop began,
 *                       never run
 * Invariant while running: accepted == completed + failed + pending + running.
 * Invariant after stop and worker join:
 *   accepted == completed + failed + shutdown_discarded.
 *
 * Durability: there is none. Queues live in memory only; startup creates empty
 * queues and never recovers or replays jobs lost to a crash, and no job is
 * persisted. cf_jobs_stop is the declared shutdown loss point: it stops
 * accepting, discards every pending job (counting it as shutdown-discarded),
 * lets a handler already running finish (a blocked subprocess is killed at its
 * own timeout; that belongs to S01), and joins the kind's workers.
 *
 * Lifecycle (J02 wires this in main.c alongside cf_writer_start):
 *   cf_jobs_start(config, &jobs);            // before serving
 *   cf_jobs_set_handler(jobs, kind, fn, ctx);// at startup, before enqueue
 *   cf_jobs_register_writer(jobs, app);      // after cf_writer_start
 *   ... enqueue ...
 *   cf_jobs_stop(jobs);                      // before cf_app_destroy
 *   cf_jobs_destroy(jobs);
 * Handler registration is per kind and must happen before the first enqueue
 * of that kind; enqueueing an unregistered kind fails cleanly with CF_INVALID.
 */
#ifndef CF_JOBS_H
#define CF_JOBS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cf.h"
#include "config.h"

/* Queueable kinds: the four best-effort model events plus the explicit
 * storage-owned media task. CF_EVENT_DISCONNECT_USER is the mandatory C03
 * control path, not a queued job, and has no kind here. */
typedef enum {
    CF_JOB_PUSH_MESSAGE = 0,     /* CF_EVENT_PUSH_MESSAGE */
    CF_JOB_DELIVER_WEBHOOK,      /* CF_EVENT_DELIVER_WEBHOOK */
    CF_JOB_REMOVE_BANNED_CONTENT,/* CF_EVENT_REMOVE_BANNED_CONTENT */
    CF_JOB_PURGE_BLOB,           /* CF_EVENT_PURGE_BLOB */
    CF_JOB_MEDIA,                /* S03 storage task; not an event */
    CF_JOB_KIND_COUNT
} cf_job_kind;

/* Media workers are separate from CF_JOB_WORKERS and capped at four
 * (01: "CF_JOB_WORKERS | 2 per kind; media separately capped at 4"). */
#define CF_JOB_MEDIA_WORKERS 4

/* Stable short name for sanitized logs and tests. Never returns NULL. */
const char *cf_job_kind_name(cf_job_kind kind);

/* Map a committed event kind to its queue kind. Returns false for
 * CF_EVENT_DISCONNECT_USER (mandatory control, not queued) and for values
 * outside cf_event_kind. */
bool cf_job_kind_of_event(cf_event_kind event, cf_job_kind *out);

/* One queue payload as seen by a handler: IDs plus, for media, NUL-terminated
 * strings owned by the queue. The handler borrows the pointer only for the
 * duration of the call; the strings are freed after it returns. Event
 * payloads carry no strings (NULL). */
typedef struct {
    cf_job_kind kind;
    int64_t user_id, room_id, message_id, blob_id;
    bool reconnect;
    const char *task_type; /* media only, else NULL */
    const char *variation; /* media only, else NULL (may be NULL) */
} cf_job;

/* Worker-side handler, one per kind, registered before that kind is first
 * enqueued. Runs on a job worker thread with no lock held. Returning CF_OK
 * completes the job; any other value fails it. There is no retry. */
typedef cf_err (*cf_job_handler_fn)(void *ctx, const cf_job *job);

/* Per-kind accounting snapshot. */
typedef struct {
    size_t capacity;            /* FIFO slots, CF_JOB_QUEUE */
    size_t pending;             /* queued and waiting for a worker */
    size_t running;             /* handlers currently executing */
    size_t workers;             /* worker threads for this kind */
    bool handler_registered;
    uint64_t accepted;
    uint64_t completed;
    uint64_t failed;
    uint64_t rejected;          /* queue full at enqueue */
    uint64_t shutdown_discarded;
} cf_job_stats;

/* Create one FIFO per kind (config->job_queue slots each) and start each
 * kind's workers (config->job_workers for event kinds, CF_JOB_MEDIA_WORKERS
 * for media). config is borrowed for the call. On success *out owns the jobs
 * and all workers are running; on failure *out stays NULL and nothing was
 * started. Counters start at zero: no persistence, no recovery. */
cf_err cf_jobs_start(const cf_config *config, cf_jobs **out);

/* Register the single handler for one kind. Allowed before or after start of
 * serving, but a kind cannot be enqueued until its handler is registered
 * (enqueue returns CF_INVALID). fn must not be NULL. A later call replaces
 * the previous handler. Registered at startup by J02/I01/I02; ctx is
 * borrowed and passed through unchanged. Invalid jobs/kind/fn: CF_INVALID;
 * after cf_jobs_stop: CF_BUSY. */
cf_err cf_jobs_set_handler(cf_jobs *jobs, cf_job_kind kind,
                           cf_job_handler_fn fn, void *ctx);

/* Enqueue one committed event into its kind. Never blocks the caller (the
 * writer thread and request workers may call it). The event's unused fields
 * are ignored. CF_INVALID for CF_EVENT_DISCONNECT_USER or an unknown kind,
 * and (cleanly, counting nothing) when that kind has no registered handler.
 * See the accounting rules above for CF_OK/CF_BUSY. */
cf_err cf_job_enqueue(cf_jobs *jobs, cf_event event); /* cf.h contract */

/* Enqueue one storage-owned media task (S03). task_type must be non-empty and
 * free of embedded NUL; variation may be empty. The queue copies both into
 * owned memory; the caller keeps its spans. Fails cleanly with CF_INVALID
 * before the media handler is registered. */
cf_err cf_jobs_enqueue_media(cf_jobs *jobs, int64_t blob_id,
                             cf_span task_type, cf_span variation);

/* Register the four best-effort event kinds as this queue's writer
 * post-commit consumers (cf_writer_set_event_handler): the writer then calls
 * cf_job_enqueue for each committed event of those kinds. Requires a started
 * writer (call after cf_writer_start) and a started jobs instance. A full
 * queue or a missing handler makes the enqueue fail cleanly; the writer drops
 * the event, counts it in best_effort_dropped, and still reports the commit
 * outcome unchanged. Returns the first registration error. */
cf_err cf_jobs_register_writer(cf_jobs *jobs, cf_app *app);

/* Copy one kind's counters. out must not be NULL. */
cf_err cf_jobs_stats_get(const cf_jobs *jobs, cf_job_kind kind,
                         cf_job_stats *out);

/* Stop accepting, discard and count every pending job, let running handlers
 * finish, and join the kind's workers. Idempotent; call once, before
 * cf_app_destroy. NULL is a no-op. Not safe for concurrent callers. */
void cf_jobs_stop(cf_jobs *jobs);

/* Stop if needed, then release the queues and their owned payloads. NULL is a
 * no-op. Counters are no longer queryable after this call. */
void cf_jobs_destroy(cf_jobs *jobs);

#endif /* CF_JOBS_H */
