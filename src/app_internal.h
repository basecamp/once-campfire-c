/* Internal cf_app surface for D02 (writer/version) and H01/A00 (loops and
 * request workers). Not a shared contract: the integrator approves any
 * addition that another module consumes. F03/A00 own the definitions. */
#ifndef CF_APP_INTERNAL_H
#define CF_APP_INTERNAL_H

#include "cf.h"

#include <pthread.h>

/* Bounded worker registry. Every long-lived worker thread registers its own
 * joinable pthread_t once before its first observable side effect; shutdown
 * joins exactly these threads. CF_LIMIT means the registry is full and the
 * caller must not start the worker; CF_BUSY means shutdown already began. */
#define CF_APP_MAX_WORKERS 256
cf_err cf_app_register_worker(cf_app *app, pthread_t thread);

/* Join every registered worker and clear the registry. Idempotent. */
void cf_app_join_workers(cf_app *app);

/* Orderly shutdown request. Workers poll cf_app_stop_requested() and return;
 * cf_app_destroy sets this before joining. After the request, new worker
 * registrations fail with CF_BUSY. A00 also wakes its request pool. */
void cf_app_request_stop(cf_app *app);
bool cf_app_stop_requested(const cf_app *app);

/* D02 writer use only, after a successful commit (06 "Version/snapshot
 * algorithm"): take the cache/version mutex, increment the data version and
 * release-publish it. Version wrap is a fatal internal error. Does not
 * render, scan entries or wait for readers. */
void cf_app_advance_data_version(cf_app *app);

/* Cache/version mutex from 06: version capture, lookup and admission compare
 * under this one lock. It exists even when CF_CACHE_BYTES is 0. Borrowed
 * until cf_app_destroy. */
pthread_mutex_t *cf_app_version_mutex(cf_app *app);

/* K01c complete-body cache (06 "K01: complete-body cache"): the enabled
 * process-wide cache created by cf_app_start from the configured
 * CF_CACHE_BYTES, or NULL while caching is disabled (budget 0, a nonzero
 * budget too small for the bucket array, or a creation failure). Borrowed;
 * cf_app_destroy destroys it after every worker has joined. A non-NULL
 * return is always an enabled cache. */
cf_cache *cf_app_cache(cf_app *app);

/* Test seam (tests/cache/test_cache_admission.c): create and attach the cache
 * the way cf_app_start does, for suites that drive cf_app_create +
 * cf_writer_start directly and never call cf_app_start. Budget 0 and the
 * module's disabled-budget rule behave exactly as in cf_app_start; CF_BUSY
 * when a cache is already attached or the app is serving. No request worker
 * may be running. */
cf_err cf_app_cache_enable_for_test(cf_app *app, size_t budget_bytes);

/* A00 observability for tests and counters: tasks admitted and not yet
 * submitted/abandoned (running + queued). */
size_t cf_app_admitted_count(const cf_app *app);

/* Requests whose response was submitted or abandoned (worker completions). */
uint64_t cf_app_completed_requests(const cf_app *app);

/* The calling request worker's read-only SQLite connection (the one opened on
 * its own thread in cf_app_start), or NULL when the caller is not one of the
 * app's request workers. A cf_app_submit_worker closure runs on such a worker,
 * so model work in a closure reads through this borrowed handle; it must not
 * retain or use it after returning. Added for the P12-02b cable worker seam
 * (04-cable-jobs.md: workers perform auth/model work; the cable reactor holds
 * no DB reader). */
cf_db *cf_app_worker_reader(void);

#endif /* CF_APP_INTERNAL_H */
