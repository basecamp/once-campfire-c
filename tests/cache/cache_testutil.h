/* Test-only helpers for the K01b cache tests (tests/cache).
 *
 * The version fixture stands in for the app's data-version atomic and its
 * cache/version mutex: the cache calls the reader with the mutex held, and
 * the reader is one atomic load, exactly like cf_data_version. The body
 * helpers build caller-owned cf_bufs and cf_cached_body values; the stderr
 * capture helper counts the disabled-cache diagnostic. Not production code
 * and not part of any application API.
 */
#ifndef CF_CACHE_TESTUTIL_H
#define CF_CACHE_TESTUTIL_H

#include "cf_test.h"

#include "cache.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    _Atomic uint64_t version;
    pthread_mutex_t mutex;
} cache_version_fixture;

static inline void cache_version_fixture_init(cache_version_fixture *f,
                                              uint64_t version) {
    atomic_init(&f->version, version);
    pthread_mutex_init(&f->mutex, NULL);
}

static inline void cache_version_fixture_dispose(cache_version_fixture *f) {
    pthread_mutex_destroy(&f->mutex);
}

/* The cache's version source: called under the fixture mutex. */
static inline uint64_t cache_version_reader(void *user) {
    cache_version_fixture *f = user;
    return atomic_load_explicit(&f->version, memory_order_acquire);
}

static inline uint64_t cache_version_now(const cache_version_fixture *f) {
    return atomic_load_explicit(&f->version, memory_order_acquire);
}

/* The writer's advance: atomic store published under the same mutex. */
static inline void cache_version_advance(cache_version_fixture *f) {
    pthread_mutex_lock(&f->mutex);
    uint64_t next =
        atomic_load_explicit(&f->version, memory_order_relaxed) + 1;
    atomic_store_explicit(&f->version, next, memory_order_release);
    pthread_mutex_unlock(&f->mutex);
}

static inline cf_span cache_span(const char *s) {
    cf_span span = {(const unsigned char *)s, strlen(s)};
    return span;
}

static inline cf_span cache_span_bytes(const void *bytes, size_t len) {
    cf_span span = {(const unsigned char *)bytes, len};
    return span;
}

static inline bool cache_buf_is(const cf_buf *b, const char *expected) {
    cf_span s = cf_buf_span(b);
    size_t n = strlen(expected);
    return s.len == n && (n == 0 || memcmp(s.ptr, expected, n) == 0);
}

/* Caller-owned body value around `payload`; dispose with
 * cf_cached_body_dispose (which releases the one body reference). */
static inline cf_err cache_body_make(const char *payload, bool gzip,
                                     const char *etag, cf_cached_body *out) {
    memset(out, 0, sizeof *out);
    cf_err rc = cf_buf_copy(cache_span(payload), &out->body);
    if (rc != CF_OK) return rc;
    out->gzip = gzip;
    snprintf(out->weak_etag, sizeof out->weak_etag, "W/\"%s\"", etag);
    return CF_OK;
}

/* Caller-owned body of `len` bytes all `fill`, for cap/budget boundaries. */
static inline cf_err cache_body_make_fill(char fill, size_t len,
                                          cf_cached_body *out) {
    memset(out, 0, sizeof *out);
    unsigned char *bytes = malloc(len != 0 ? len : 1);
    if (bytes == NULL) return CF_NOMEM;
    memset(bytes, (unsigned char)fill, len);
    cf_err rc = cf_buf_copy(cache_span_bytes(bytes, len), &out->body);
    free(bytes);
    if (rc != CF_OK) return rc;
    snprintf(out->weak_etag, sizeof out->weak_etag, "W/\"fill\"");
    return CF_OK;
}

/* Run fn(user) with stderr redirected into a temp file; copy what was written
 * into buf (NUL-terminated) and return its length. */
typedef void (*cache_capture_fn)(void *user);

static inline size_t cache_capture_stderr(cache_capture_fn fn, void *user,
                                          char *buf, size_t cap) {
    buf[0] = '\0';
    fflush(stderr);
    int saved = dup(fileno(stderr));
    CF_REQUIRE(saved >= 0);
    FILE *tmp = tmpfile();
    CF_REQUIRE(tmp != NULL);
    CF_REQUIRE(dup2(fileno(tmp), fileno(stderr)) >= 0);
    fn(user);
    fflush(stderr);
    CF_REQUIRE(dup2(saved, fileno(stderr)) >= 0);
    close(saved);
    rewind(tmp);
    size_t n = fread(buf, 1, cap > 0 ? cap - 1 : 0, tmp);
    buf[n] = '\0';
    fclose(tmp);
    return n;
}

static inline size_t cache_count_occurrences(const char *haystack,
                                             const char *needle) {
    size_t count = 0;
    for (const char *at = haystack; (at = strstr(at, needle)) != NULL;
         at += strlen(needle)) {
        count++;
    }
    return count;
}

/* One-cache environment: fixture + cache. */
typedef struct {
    cache_version_fixture version;
    cf_cache *cache;
} cache_env;

static inline void cache_env_open(cache_env *env, uint64_t version,
                                  size_t budget) {
    cache_version_fixture_init(&env->version, version);
    env->cache = NULL;
    CF_REQUIRE(cf_cache_create(budget, cache_version_reader, &env->version,
                               &env->version.mutex, &env->cache) == CF_OK);
    CF_REQUIRE(env->cache != NULL);
}

static inline void cache_env_close(cache_env *env) {
    cf_cache_destroy(env->cache);
    env->cache = NULL;
    cache_version_fixture_dispose(&env->version);
}

#endif /* CF_CACHE_TESTUTIL_H */
