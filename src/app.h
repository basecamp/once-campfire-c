/* Minimal cf_app seed (F03). A00 extends src/app.c later; the public surface
 * here stays this small until the integrator approves additions. */
#ifndef CF_APP_H
#define CF_APP_H

#include "cf.h"

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
 * cf_app_stop_requested and return), only then free the config and destroy
 * the mutexes. Accepts NULL. */
void cf_app_destroy(cf_app *app);

#endif /* CF_APP_H */
