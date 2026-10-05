/* Internal cf_app surface for D02 (writer/version) and H01/A00 (loops and
 * request workers). Not a shared contract: the integrator approves any
 * addition that another module consumes. F03 owns the definitions in app.c. */
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
 * registrations fail with CF_BUSY. */
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

#endif /* CF_APP_INTERNAL_H */
