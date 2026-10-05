/* J02 bounded job handlers (04-cable-jobs.md "J01/J02: bounded jobs").
 *
 * One handler per queueable kind (CF_JOB_PUSH_MESSAGE,
 * CF_JOB_DELIVER_WEBHOOK, CF_JOB_REMOVE_BANNED_CONTENT, CF_JOB_PURGE_BLOB,
 * CF_JOB_MEDIA). Payloads carry database IDs and queue-owned copies only;
 * each handler re-reads its targets with its own reader connection (opened
 * per invocation from the app config's database_path, or from
 * ctx->db_path in unit tests) and uses cf_write where a mutation is needed.
 * A handler never touches a request, task, connection, SQLite or cache
 * pointer from the enqueueing request.
 *
 * Outcome policy (JOB-02/JOB-03, DB-02):
 *   - Deleted, revoked or otherwise absent targets are a recorded no-op:
 *     return CF_OK (counted completed, never resurrected, never retried).
 *   - Genuine failures (unreadable database, failed commit, failed render,
 *     a present integration hook that fails) return non-CF_OK (counted
 *     failed, no retry, no retry scheduler).
 *   - The queue-full arm is owned by the queue/writer: a CF_BUSY enqueue is
 *     a drop counted by the queue (rejected) and the writer
 *     (best_effort_dropped); the committed write outcome is never rewritten.
 *   - No blocking the writer: handlers run on job worker threads, open
 *     short-lived connections, and never wait on request/crypto/media
 *     workers. No persistence: nothing here recovers or replays jobs.
 *   - No direct socket writes. The only cable surface used is the existing
 *     broadcast API (cf_broadcast_message_remove); loop-queue overflow
 *     (CF_BUSY) follows the request path's post-commit policy
 *     (msg_broadcast_outcome in src/actions/messages.c): logged and treated
 *     as delivered, never a pretend rollback.
 *
 * Batching (RemoveBannedContent): at most CF_JOBS_REMOVE_BANNED_BATCH (100)
 * messages are destroyed per job inside one cf_write transaction. When the
 * bounded scan finds more, the handler requeues one follow-up
 * REMOVE_BANNED_CONTENT event for the same user through ctx->jobs
 * (requeue-remainder semantics). A full queue drops the remainder and counts
 * it (rejected); the committed batch still reports CF_OK.
 *
 * Missing integrations (recorded no-ops, counted):
 *   - I02 push delivery and I01 webhook posting have not landed: the
 *     handlers perform the full recipient/bot/message reads and then take
 *     the missing-integration arm. The proposed integrator-owned symbols
 *     are declared as weak imports in handlers.c (cf_push_send,
 *     cf_webhook_post): while they are absent the arm is a counted no-op;
 *     once the integrator provides them the handlers call them.
 *   - PurgeBlob re-reads the blob, then defers to S02's purge (weak import
 *     cf_blob_purge_if_unreferenced), which owns the reference recheck and
 *     the in-transaction row delete. TODO(S02): wire the real purge. The
 *     handler never calls S01 file deletes directly: S01's contract
 *     requires the caller to recheck references first, and the only
 *     reference view lives in S02's transaction.
 *   - CF_JOB_MEDIA is an explicit storage-owned stub: it records the call
 *     and returns CF_OK until S03 lands the real worker.
 *
 * Threading: handlers run on job worker threads with no lock held. The
 * context is borrowed and read-only after registration except for its
 * atomic counters. ctx must outlive the jobs instance; the register helper
 * stores jobs back into ctx->jobs for the requeue arm.
 *
 * Lifecycle (integrator wiring; NOT done here):
 *   static cf_jobs_handler_ctx hctx = { .app = app, .cable = cable };
 *   cf_jobs_register_default_handlers(jobs, &hctx); // before serving
 *   cf_jobs_register_writer(jobs, app);              // after cf_writer_start
 */
#ifndef CF_JOBS_HANDLERS_H
#define CF_JOBS_HANDLERS_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "cf.h"
#include "jobs/jobs.h"

/* Per-job destroy bound for RemoveBannedContent (JOB-02). The scan reads one
 * more (101) to detect a remainder worth requeueing. */
#define CF_JOBS_REMOVE_BANNED_BATCH 100
#define CF_JOBS_REMOVE_BANNED_SCAN (CF_JOBS_REMOVE_BANNED_BATCH + 1)

typedef struct cf_jobs_handler_ctx {
    cf_app *app;         /* borrowed; required for kinds that mutate
                          * (RemoveBannedContent via cf_write, and the future
                          * S02 purge hook). May be NULL in unit tests that
                          * only exercise read-only no-op paths. */
    const char *db_path; /* borrowed NUL-terminated SQLite path used only
                          * when app == NULL (unit tests). Ignored when app
                          * supplies a config database_path. */
    cf_cable *cable;     /* borrowed; NULL skips cable broadcasts (unit
                          * tests). Broadcasts otherwise go through the
                          * existing cable broadcast API only. */
    cf_jobs *jobs;       /* borrowed requeue target for the remainder arm;
                          * set by cf_jobs_register_default_handlers. NULL
                          * means a remainder cannot be requeued and is
                          * counted as dropped. */

    /* Recorded no-op / progress counters (JOB-02/JOB-03 observability). */
    _Atomic uint64_t push_noop_gone;            /* message/room missing or
                                                 * room mismatch */
    _Atomic uint64_t push_noop_no_integration;  /* I02 hook absent */
    _Atomic uint64_t push_attempted;            /* I02 hook calls */
    _Atomic uint64_t webhook_noop_gone;         /* bot/message/webhook
                                                 * missing, inactive or
                                                 * revoked */
    _Atomic uint64_t webhook_noop_no_integration; /* I01 hook absent */
    _Atomic uint64_t webhook_attempted;          /* I01 hook calls */
    _Atomic uint64_t banned_batches;            /* cf_write batches run */
    _Atomic uint64_t banned_destroyed;          /* messages destroyed */
    _Atomic uint64_t banned_requeued;           /* remainder requeues sent */
    _Atomic uint64_t banned_requeue_dropped;    /* remainder lost: no jobs
                                                 * ctx or CF_BUSY */
    _Atomic uint64_t banned_noop_gone;          /* user missing or no longer
                                                 * banned */
    _Atomic uint64_t banned_broadcasts;         /* message_remove calls */
    _Atomic uint64_t purge_noop_gone;           /* blob missing or still
                                                 * referenced */
    _Atomic uint64_t purge_deferred_no_s02;     /* blob present but the S02
                                                 * purge hook is absent */
    _Atomic uint64_t purge_deleted;             /* S02-reported purges */
    _Atomic uint64_t media_noop;                /* S03 stub calls */
} cf_jobs_handler_ctx;

/* One handler per queueable kind, suitable for cf_jobs_set_handler with a
 * cf_jobs_handler_ctx as ctx. See the file header for the outcome policy. */
cf_err cf_jobs_handle_push_message(void *ctx, const cf_job *job);
cf_err cf_jobs_handle_deliver_webhook(void *ctx, const cf_job *job);
cf_err cf_jobs_handle_remove_banned_content(void *ctx, const cf_job *job);
cf_err cf_jobs_handle_purge_blob(void *ctx, const cf_job *job);
/* Explicit storage-owned stub: records the call in media_noop and returns
 * CF_OK until S03 lands the real media worker. */
cf_err cf_jobs_handle_media(void *ctx, const cf_job *job);

/* Register all five default handlers with ctx (borrowed, must outlive
 * jobs). Stores jobs back into ctx->jobs for the remainder arm. Returns
 * the first registration error, or CF_OK. */
cf_err cf_jobs_register_default_handlers(cf_jobs *jobs,
                                         cf_jobs_handler_ctx *ctx);

#endif /* CF_JOBS_HANDLERS_H */
