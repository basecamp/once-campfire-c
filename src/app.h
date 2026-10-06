/* Application lifecycle: config ownership, data version, writer and request
 * worker bootstrap, HTTP admission and shutdown (F03 seed extended by A00;
 * 03-application.md "A00"; 00-contracts.md "Execution and waiting"). */
#ifndef CF_APP_H
#define CF_APP_H

#include "cf.h"

struct cf_http_task; /* cf.h-visible H01 task handle (src/http/http.h) */

/* Create the application from a loaded immutable config.
 *
 * On success the app takes ownership of config and *out is the new app; the
 * data version starts at 1 and the cache/version mutex exists even when
 * caching is disabled (06-cache-performance.md "Version/snapshot algorithm").
 * On failure *out stays NULL and the caller still owns config.
 */
cf_err cf_app_create(cf_config *config, cf_app **out);

/* Orderly teardown per 00-contracts.md CORE-05 and 01's shutdown rules:
 * request stop, join every registered worker (a worker must observe
 * cf_app_stop_requested and return), stop the writer, only then free the
 * config and destroy the mutexes. Accepts NULL. */
void cf_app_destroy(cf_app *app);

/* Borrowed immutable config, valid until cf_app_destroy. NULL for NULL. */
const cf_config *cf_app_config(const cf_app *app);

/* The application cable: `c.app().broadcasts` for the controller packets
 * (cf_broadcast_*). Set once by main.c right after the cable is created and
 * before any request can be dispatched (tests inject one the same way);
 * borrowed, never owned or destroyed by cf_app_destroy. The getter returns
 * NULL until the setter ran and NULL for a NULL app. */
void cf_app_set_cable(cf_app *app, cf_cable *cable);
cf_cable *cf_app_cable(const cf_app *app);

/* Borrowed jobs queue set during startup before serving. Media enqueue is
 * nonblocking; CF_BUSY includes a missing queue (e.g. action unit fixtures).
 * The caller logs a dropped post-commit analysis without changing a committed
 * attachment result. Jobs must drain before application teardown. */
void cf_app_set_jobs(cf_app *app, cf_jobs *jobs);
cf_err cf_app_enqueue_media(cf_app *app, int64_t blob_id,
                            cf_span task_type, cf_span variation);

/* Start the serving path: create/validate the schema and start the single
 * writer (cf_writer_start), then open one read-only SQLite connection per
 * CF_READERS request worker on its own thread and queue them. Call before the
 * HTTP loops are created; a second call returns CF_BUSY. On any failure the
 * workers/writer are torn down and the app is stopped (start cannot be
 * retried on the same app). */
cf_err cf_app_start(cf_app *app);

/* Orderly stop per 01/A00: no new admissions, workers finish the running
 * handler and abandon queued tasks, every cf_write caller has returned, then
 * the writer releases its state. Idempotent; cf_app_destroy also stops.
 * Call after the HTTP loops have drained and their threads joined. */
void cf_app_stop(cf_app *app);

/* H01 admission hook (cf_http_admit_fn; pass the app as `user`). CF_OK takes
 * the task into the bounded CF_REQUEST_SLOTS pool (running + queued); any
 * non-OK value declines without consuming storage, and the loop answers 503 +
 * Retry-After (00-contracts.md; CORE-04). Never blocks the loop thread. */
cf_err cf_app_admit(void *app_user, struct cf_http_task *task);

/* Bounded worker submission (04-cable-jobs.md C02/C03, P12-02b): queue one
 * closure onto the app's existing request-worker pool. The pool is the same
 * bounded CF_REQUEST_SLOTS pool the HTTP admission hook uses, with one shared
 * accounting: pool_admitted counts every admitted item (running + queued)
 * whether it is an HTTP task or a closure, and the pool never holds more than
 * CF_REQUEST_SLOTS of them; no additional queue exists. CF_BUSY means the
 * pool is full (running + queued == the slot budget) or stopping, and the
 * caller must treat the closure as not submitted (the C02/C03 overload arm:
 * reject the subscription/connection rather than queue unbounded work).
 * A submitted closure runs exactly once on a request worker, with that
 * worker's read-only SQLite connection (cf_app_worker_reader); if shutdown
 * begins before it is dequeued it still runs while the workers drain, so the
 * caller must keep `user` alive until the closure returns or itself be
 * reference counted, and the closure must be cancellation-aware (it can
 * observe cf_app_stop_requested and its own state). A closure must not block
 * indefinitely: it occupies one of the CF_READERS workers. Never blocks. */
cf_err cf_app_submit_worker(cf_app *app, void (*fn)(void *user), void *user);

/* Per-loop share of the process-wide CF_INPUT_BYTES / CF_OUTPUT_BYTES
 * reservation (sums never exceed the configured total except when the config
 * total is smaller than CF_LOOPS: H01 interprets 0 as its 64 MiB default, so
 * A00 guarantees at least 1 byte per loop). Returns 0 for an out-of-range
 * loop index. */
size_t cf_app_loop_input_bytes(const cf_app *app, size_t loop_index);
size_t cf_app_loop_output_bytes(const cf_app *app, size_t loop_index);

#endif /* CF_APP_H */
