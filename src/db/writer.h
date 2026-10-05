/* Single-writer transaction engine (D02, 02-data-auth.md "D02: writer and
 * transaction semantics"; 06-cache-performance.md "Version/snapshot
 * algorithm"; 00-contracts.md "Execution and waiting").
 *
 * Public contract (src/cf.h, frozen): cf_write / cf_tx_db / cf_tx_event /
 * cf_data_version.  This header adds only the bootstrap and the post-commit
 * consumer registration points that C03/J01/I02 will own; the integrator
 * approves any further cross-module API.
 *
 * Lifecycle (the `cf_app` seed cannot reach its own config; F03 owns app.c):
 *   cf_app_create(config, &app)      (owner: A00/F03)
 *   cf_writer_start(app, config)     once, before serving
 *   ... cf_write(...) from request workers ...
 *   cf_writer_stop(app)              no cf_write caller may be active
 *   cf_app_destroy(app)              joins the registered writer thread
 * Until cf_writer_start succeeds, cf_write returns CF_INTERNAL.  The writer
 * thread registers itself in the app worker registry, so cf_app_destroy joins
 * it (CORE-05) even when cf_writer_stop was not called; cf_writer_stop is
 * still required to release the writer state and close the connection in an
 * orderly way.
 *
 * Callback rules (enforced by the engine where it can):
 *  - cf_write borrows fn/arg until it returns; fn runs on the writer thread
 *    inside BEGIN IMMEDIATE, and the transaction commits only on CF_OK.
 *  - fn must not perform network/file/media/bcrypt I/O; external effects stay
 *    after commit.  fn must not call cf_write (returns CF_INTERNAL) and must
 *    not wait for request/crypto/media workers.
 *  - Callers revalidate authorization (user status, ownership) inside the
 *    transaction; the writer itself never re-authorizes.
 *
 * Post-commit phase, in writer order, after COMMIT and before the caller is
 * acknowledged: advance the global data version, apply mandatory
 * DISCONNECT_USER revocations, then hand off best-effort events.
 *  - A committed transaction always advances the version (failed or rolled
 *    back transactions do not).
 *  - DISCONNECT_USER is mandatory.  While no control handler is registered
 *    (C03 owns the registration) or its handler fails, cf_write returns
 *    CF_INTERNAL after the commit: never CF_BUSY, never a pretend rollback,
 *    never a retry.  CF_OK means "committed AND required revocations
 *    applied".
 *  - The other four kinds are best-effort: a missing consumer or a handler
 *    failure drops the event and increments the per-kind counter.  It never
 *    changes the committed write's outcome and never pretends delivery.
 *    J01/I02 register the per-kind consumers (J01's enqueue is the queue
 *    owner); C03 owns the mandatory control handler.
 *  - Handlers run on the writer thread with no DB/cache lock held.  The
 *    event pointer is borrowed for the call only.  Events are plain values
 *    (no borrowed strings) and are discarded on rollback.
 */
#ifndef CF_DB_WRITER_H
#define CF_DB_WRITER_H

#include "cf.h"
#include "config.h"

/* Number of cf_event_kind values; counters are indexed by kind. */
#define CF_WRITER_EVENT_KINDS 5

/* Bounded registry of running writers (one per cf_app). */
#define CF_WRITER_MAX_APPS 64

/* Mandatory control handler (C03 revocation barrier).  Called on the writer
 * thread for each committed DISCONNECT_USER event; must return only after
 * every loop applied the revocation.  Any non-CF_OK return makes the
 * committed write report CF_INTERNAL. */
typedef cf_err (*cf_writer_control_fn)(void *ctx, const cf_event *event);

/* Best-effort consumer for one non-DISCONNECT kind (J01 queue enqueue,
 * I02/C03 through it).  Any non-CF_OK return drops and counts the event. */
typedef cf_err (*cf_writer_event_fn)(void *ctx, const cf_event *event);

typedef struct {
    size_t queue_capacity;  /* CF_WRITER_QUEUE */
    size_t pending;         /* callbacks admitted and waiting to run */
    bool callback_running;  /* the writer thread is executing one callback */
    uint64_t admitted;      /* cf_write calls enqueued */
    uint64_t committed;     /* transactions committed */
    uint64_t rolled_back;   /* callback or COMMIT failure */
    uint64_t begin_failures;
    uint64_t commit_failures;
    uint64_t queue_rejected;     /* CF_BUSY returned because the queue was full */
    uint64_t mandatory_failures; /* DISCONNECT_USER events not applied */
    uint64_t best_effort_dropped[CF_WRITER_EVENT_KINDS];
} cf_writer_stats;

/* Open the writer connection and start the writer thread.
 *
 * Creates/validates the schema through cf_db_open(path, false, ...) and copies
 * CF_WRITER_QUEUE as the pending-callback bound; `config` is borrowed for the
 * call only.  One start per app; concurrent calls for the same app are not
 * supported.  On failure nothing is registered and *app is unchanged. */
cf_err cf_writer_start(cf_app *app, const cf_config *config);

/* Stop the writer: no new cf_write is admitted, queued callbacks still run to
 * completion, the connection closes on the writer thread and the state is
 * released.  Call after every cf_write caller has returned and before
 * cf_app_destroy (destroy alone still joins the registered writer thread, but
 * only stop releases the writer state).  NULL is a no-op; an app without a
 * started writer is a no-op. */
void cf_writer_stop(cf_app *app);

/* Register the single mandatory DISCONNECT_USER handler (C03).  fn must not
 * be NULL; ctx is borrowed.  A later registration replaces the previous one. */
cf_err cf_writer_set_control_handler(cf_app *app, cf_writer_control_fn fn,
                                     void *ctx);

/* Register the single best-effort consumer for `kind` (J01/I02).  kind must
 * not be CF_EVENT_DISCONNECT_USER.  fn must not be NULL. */
cf_err cf_writer_set_event_handler(cf_app *app, cf_event_kind kind,
                                   cf_writer_event_fn fn, void *ctx);

/* Copy the writer's counters; out must not be NULL. */
cf_err cf_writer_stats_get(cf_app *app, cf_writer_stats *out);

#endif /* CF_DB_WRITER_H */
