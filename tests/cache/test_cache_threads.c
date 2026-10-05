/* K01b cache concurrency tests: get/put from many request workers with FIFO
 * eviction and version advancement running underneath (06 "Version/snapshot
 * algorithm"; CACHE-02/CACHE-04). Built by `make tsan` for the race run and
 * by the ordinary suites as well; every wait is a condvar or a join, never a
 * sleep hoping to win a race.
 */
#include "cf_test.h"

#include "cache_testutil.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* Deterministic duplicate-build race: N builders released together all try to
 * admit the same key at the same version. Exactly one wins; the rest are
 * counted as duplicate builds and the first body stays. */
typedef struct {
    cache_env *env;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    size_t arrived;
    size_t total;
    bool go;
    _Atomic uint64_t ok, busy, unexpected;
} duplicate_build;

static void *duplicate_builder(void *user) {
    duplicate_build *d = user;
    pthread_mutex_lock(&d->mu);
    d->arrived++;
    pthread_cond_broadcast(&d->cv);
    while (!d->go) pthread_cond_wait(&d->cv, &d->mu);
    pthread_mutex_unlock(&d->mu);

    cf_cached_body body;
    if (cache_body_make("same-body", false, "same", &body) != CF_OK) {
        atomic_fetch_add(&d->unexpected, 1);
        return NULL;
    }
    cf_err rc = cf_cache_put(d->env->cache, cache_span("shared"), 1, &body);
    cf_cached_body_dispose(&body);
    if (rc == CF_OK) {
        atomic_fetch_add(&d->ok, 1);
    } else if (rc == CF_BUSY) {
        atomic_fetch_add(&d->busy, 1);
    } else {
        atomic_fetch_add(&d->unexpected, 1);
    }
    return NULL;
}

CF_TEST(cache_concurrent_duplicate_builds_one_winner) {
    enum { BUILDERS = 8 };
    cache_env env;
    cache_env_open(&env, 1, CF_CACHE_DEFAULT_BUDGET_BYTES);
    duplicate_build d;
    memset(&d, 0, sizeof d);
    d.env = &env;
    d.total = BUILDERS;
    pthread_mutex_init(&d.mu, NULL);
    pthread_cond_init(&d.cv, NULL);

    pthread_t threads[BUILDERS];
    for (int i = 0; i < BUILDERS; i++) {
        CF_REQUIRE(pthread_create(&threads[i], NULL, duplicate_builder, &d) ==
                   0);
    }
    pthread_mutex_lock(&d.mu);
    while (d.arrived < BUILDERS) pthread_cond_wait(&d.cv, &d.mu);
    d.go = true;
    pthread_cond_broadcast(&d.cv);
    pthread_mutex_unlock(&d.mu);
    for (int i = 0; i < BUILDERS; i++) (void)pthread_join(threads[i], NULL);

    CF_CHECK(atomic_load(&d.ok) == 1);
    CF_CHECK(atomic_load(&d.busy) == BUILDERS - 1);
    CF_CHECK(atomic_load(&d.unexpected) == 0);
    cf_cache_stats st;
    CF_REQUIRE(cf_cache_stats_get(env.cache, &st) == CF_OK);
    CF_CHECK(st.entries == 1);
    CF_CHECK(st.duplicate_builds == BUILDERS - 1);
    cf_cached_body got;
    CF_CHECK(cf_cache_get(env.cache, cache_span("shared"), &got) == CF_OK);
    CF_CHECK(cache_buf_is(got.body, "same-body"));
    cf_cached_body_dispose(&got);

    pthread_cond_destroy(&d.cv);
    pthread_mutex_destroy(&d.mu);
    cache_env_close(&env);
}

/* Mixed concurrent traffic: 4 workers get/put 8 keys in a 4-entry budget
 * while all of them advance the version. Invariants must hold under any
 * interleaving; TSan/ASan supply the race and leak findings. */
typedef struct {
    cache_env *env;
    unsigned seed;
    unsigned keys;
    unsigned iterations;
    unsigned advance_every;
    _Atomic uint64_t gets, puts, puts_ok, bad;
} cache_churn;

static void *churn_thread(void *user) {
    cache_churn *c = user;
    for (unsigned i = 0; i < c->iterations; i++) {
        unsigned k = (i + c->seed) % c->keys;
        char key[16], payload[16];
        snprintf(key, sizeof key, "k%02u", k);
        snprintf(payload, sizeof payload, "v%02u", k);
        if (i % 3 == 0) {
            cf_cached_body got;
            cf_err rc = cf_cache_get(c->env->cache, cache_span(key), &got);
            atomic_fetch_add(&c->gets, 1);
            if (rc == CF_OK) {
                if (got.body == NULL || !cache_buf_is(got.body, payload)) {
                    atomic_fetch_add(&c->bad, 1);
                }
                cf_cached_body_dispose(&got);
            } else if (rc != CF_NOT_FOUND) {
                atomic_fetch_add(&c->bad, 1);
            }
        } else {
            cf_cached_body body;
            if (cache_body_make(payload, k % 2 == 0, "t", &body) != CF_OK) {
                continue;
            }
            uint64_t expected = cache_version_now(&c->env->version);
            cf_err rc =
                cf_cache_put(c->env->cache, cache_span(key), expected, &body);
            cf_cached_body_dispose(&body);
            atomic_fetch_add(&c->puts, 1);
            if (rc == CF_OK) {
                atomic_fetch_add(&c->puts_ok, 1);
            } else if (rc != CF_BUSY && rc != CF_LIMIT) {
                atomic_fetch_add(&c->bad, 1);
            }
        }
        if (c->advance_every != 0 && i != 0 && i % c->advance_every == 0) {
            cache_version_advance(&c->env->version);
        }
    }
    return NULL;
}

CF_TEST(cache_concurrent_get_put_evict_advance_invariants) {
    size_t cost = cf_cache_entry_bytes(4, 4); /* "k00" key, "v00" body */
    cache_env env;
    cache_env_open(&env, 1, CF_CACHE_BUCKET_BYTES + 4 * cost);

    /* Deterministic warm phase before any thread exists: 8 keys through a
     * 4-entry budget already evict 4 and leave a readable hit. */
    cf_cached_body body, got;
    for (unsigned k = 0; k < 8; k++) {
        char key[16], payload[16];
        snprintf(key, sizeof key, "k%02u", k);
        snprintf(payload, sizeof payload, "v%02u", k);
        CF_REQUIRE(cache_body_make(payload, false, "w", &body) == CF_OK);
        CF_CHECK(cf_cache_put(env.cache, cache_span(key), 1, &body) == CF_OK);
        cf_cached_body_dispose(&body);
    }
    CF_CHECK(cf_cache_get(env.cache, cache_span("k07"), &got) == CF_OK);
    CF_CHECK(cache_buf_is(got.body, "v07"));
    cf_cached_body_dispose(&got);
    cf_cache_stats st;
    CF_REQUIRE(cf_cache_stats_get(env.cache, &st) == CF_OK);
    CF_CHECK(st.entries == 4);
    CF_CHECK(st.evictions == 4);

    enum { THREADS = 4, ITERATIONS = 3000 };
    cache_churn churn[THREADS];
    pthread_t threads[THREADS];
    for (int t = 0; t < THREADS; t++) {
        memset(&churn[t], 0, sizeof churn[t]);
        churn[t].env = &env;
        churn[t].seed = (unsigned)t * 7;
        churn[t].keys = 8;
        churn[t].iterations = ITERATIONS;
        churn[t].advance_every = 97;
        CF_REQUIRE(pthread_create(&threads[t], NULL, churn_thread,
                                  &churn[t]) == 0);
    }
    for (int t = 0; t < THREADS; t++) (void)pthread_join(threads[t], NULL);

    /* The warm phase is part of the same accounting: 1 get (a hit), 8 puts,
     * all admitted. */
    uint64_t gets = 1, puts = 8, puts_ok = 8, bad = 0;
    for (int t = 0; t < THREADS; t++) {
        gets += atomic_load(&churn[t].gets);
        puts += atomic_load(&churn[t].puts);
        puts_ok += atomic_load(&churn[t].puts_ok);
        bad += atomic_load(&churn[t].bad);
    }
    CF_CHECK(bad == 0);
    CF_REQUIRE(cf_cache_stats_get(env.cache, &st) == CF_OK);
    CF_CHECK(gets == st.hits + st.misses);
    CF_CHECK(puts_ok <= puts);
    CF_CHECK(st.entries <= 4);
    CF_CHECK(st.evictions >= 4); /* the warm phase proved eviction works */
    CF_CHECK(st.bytes_used <= st.budget_bytes);
    CF_CHECK(st.bytes_used ==
             st.bucket_bytes + st.key_bytes + st.entry_bytes + st.body_bytes);
    /* Every admitted entry is still linked, FIFO-evicted or stale-evicted. */
    CF_CHECK(puts_ok == st.entries + st.evictions + st.stale);
    CF_CHECK(st.hits >= 1); /* the warm hit */
    cache_env_close(&env);
}

CF_TEST_MAIN()
