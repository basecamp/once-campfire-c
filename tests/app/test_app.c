/* F03 app-seed tests: ownership, data version, and CORE-05 (shutdown joins
 * workers before freeing app/config). The CORE-05 probe registers a real
 * worker thread through src/app_internal.h and reads app/config memory after
 * the stop request; cf_app_destroy must join it before those frees happen. */
#include "cf_test.h"

#include "app.h"
#include "app_internal.h"
#include "cf.h"
#include "config.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static cf_config *make_config(unsigned port) {
    char port_text[16];
    snprintf(port_text, sizeof port_text, "%u", port);
    cf_config_entry entries[3] = {
        {"PUBLIC_ORIGIN", "http://127.0.0.1:32123"},
        {"SECRET_KEY_BASE", HEX64},
        {"PORT", port_text},
    };
    cf_config *config = NULL;
    CF_REQUIRE(cf_config_parse(entries, 3, NULL, &config) == CF_OK);
    return config;
}

CF_TEST(app_create_invalid_arguments) {
    cf_config *config = make_config(3000);
    cf_app *sentinel = (cf_app *)&sentinel;
    cf_app *app = sentinel;

    CF_CHECK(cf_app_create(NULL, &app) == CF_INVALID);
    CF_CHECK(app == NULL); /* out pointers start empty on failure */

    app = NULL;
    CF_CHECK(cf_app_create(config, NULL) == CF_INVALID);
    CF_CHECK(config->port == 3000); /* caller still owns config on failure */
    cf_config_destroy(config);
    cf_app_destroy(NULL); /* destructor accepts NULL */
}

CF_TEST(app_data_version_init_and_advance) {
    cf_config *config = make_config(3000);
    cf_app *app = NULL;
    CF_REQUIRE(cf_app_create(config, &app) == CF_OK);
    CF_REQUIRE(app != NULL);

    CF_CHECK(cf_data_version(app) == 1); /* initialized to 1, spec 06 */
    cf_app_advance_data_version(app);
    CF_CHECK(cf_data_version(app) == 2);
    cf_app_advance_data_version(app);
    CF_CHECK(cf_data_version(app) == 3);

    /* The cache/version mutex exists even with caching disabled (cache_bytes
     * defaults to 0). */
    CF_CHECK(cf_app_version_mutex(app) != NULL);
    CF_CHECK(cf_data_version(NULL) == 0);
    CF_CHECK(cf_app_version_mutex(NULL) == NULL);

    cf_app_destroy(app);
}

struct version_watch {
    cf_app *app;
    _Atomic int observed;
    uint64_t value;
};

static void *version_watcher(void *arg) {
    struct version_watch *watch = arg;
    uint64_t value;
    while ((value = cf_data_version(watch->app)) < 4) {
        sched_yield();
    }
    watch->value = value;
    atomic_store_explicit(&watch->observed, 1, memory_order_release);
    return NULL;
}

CF_TEST(app_data_version_visible_across_threads) {
    cf_config *config = make_config(3000);
    cf_app *app = NULL;
    CF_REQUIRE(cf_app_create(config, &app) == CF_OK);

    struct version_watch watch;
    memset(&watch, 0, sizeof watch);
    watch.app = app;
    pthread_t tid;
    CF_REQUIRE(pthread_create(&tid, NULL, version_watcher, &watch) == 0);

    cf_app_advance_data_version(app);
    cf_app_advance_data_version(app);
    cf_app_advance_data_version(app);

    CF_REQUIRE(pthread_join(tid, NULL) == 0); /* watcher stops at >= 4 */
    CF_CHECK(atomic_load(&watch.observed) == 1);
    CF_CHECK(watch.value == 4);
    CF_CHECK(cf_data_version(app) == 4);
    cf_app_destroy(app);
}

struct join_probe {
    cf_app *app;
    cf_config *config;
    _Atomic int saw_stop;
    _Atomic int exited;
    unsigned observed_port;
    uint64_t observed_version;
};

static void *join_probe_worker(void *arg) {
    struct join_probe *probe = arg;
    while (!cf_app_stop_requested(probe->app)) {
        sched_yield();
    }
    atomic_store_explicit(&probe->saw_stop, 1, memory_order_relaxed);
    /* These reads must still be valid: cf_app_destroy joins this thread before
     * freeing config or app. A free-before-join is a use-after-free (caught by
     * the ASan/TSan runs). */
    probe->observed_port = probe->config->port;
    probe->observed_version = cf_data_version(probe->app);
    atomic_store_explicit(&probe->exited, 1, memory_order_release);
    return NULL;
}

/* CORE-05: shutdown joins workers before freeing app/config/queues. */
CF_TEST(app_shutdown_joins_workers_before_free) {
    cf_config *config = make_config(32123);
    cf_app *app = NULL;
    CF_REQUIRE(cf_app_create(config, &app) == CF_OK);

    struct join_probe probe;
    memset(&probe, 0, sizeof probe);
    probe.app = app;
    probe.config = config;

    pthread_t tid;
    CF_REQUIRE(pthread_create(&tid, NULL, join_probe_worker, &probe) == 0);
    CF_REQUIRE(cf_app_register_worker(app, tid) == CF_OK);

    cf_app_destroy(app); /* request stop, join, then free config/app */

    /* destroy returned only after the worker completed its post-stop reads */
    CF_CHECK(atomic_load(&probe.saw_stop) == 1);
    CF_CHECK(atomic_load(&probe.exited) == 1);
    CF_CHECK(probe.observed_port == 32123);
    CF_CHECK(probe.observed_version == 1);
}

CF_TEST(app_join_workers_explicit) {
    cf_config *config = make_config(3000);
    cf_app *app = NULL;
    CF_REQUIRE(cf_app_create(config, &app) == CF_OK);

    struct join_probe probe;
    memset(&probe, 0, sizeof probe);
    probe.app = app;
    probe.config = config;

    pthread_t tid;
    CF_REQUIRE(pthread_create(&tid, NULL, join_probe_worker, &probe) == 0);
    CF_REQUIRE(cf_app_register_worker(app, tid) == CF_OK);

    CF_CHECK(cf_app_stop_requested(app) == false);
    cf_app_request_stop(app);
    CF_CHECK(cf_app_stop_requested(app) == true);
    cf_app_join_workers(app);
    CF_CHECK(atomic_load(&probe.exited) == 1);
    cf_app_join_workers(app); /* idempotent: registry already cleared */
    cf_app_destroy(app);
}

CF_TEST(app_register_after_stop_is_busy) {
    cf_config *config = make_config(3000);
    cf_app *app = NULL;
    CF_REQUIRE(cf_app_create(config, &app) == CF_OK);
    cf_app_request_stop(app);
    CF_CHECK(cf_app_register_worker(app, pthread_self()) == CF_BUSY);
    cf_app_destroy(app);
}

CF_TEST_MAIN()
