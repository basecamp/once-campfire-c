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

/* Per-loop share of the process-wide CF_INPUT_BYTES / CF_OUTPUT_BYTES
 * reservation (sums never exceed the configured total except when the config
 * total is smaller than CF_LOOPS: H01 interprets 0 as its 64 MiB default, so
 * A00 guarantees at least 1 byte per loop). Returns 0 for an out-of-range
 * loop index. */
size_t cf_app_loop_input_bytes(const cf_app *app, size_t loop_index);
size_t cf_app_loop_output_bytes(const cf_app *app, size_t loop_index);

#endif /* CF_APP_H */
