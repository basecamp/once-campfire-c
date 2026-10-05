/* Complete-body cache (K01b; 06-cache-performance.md "K01: complete-body
 * cache" and "Version/snapshot algorithm").
 *
 * One process-wide table: fixed CF_CACHE_BUCKETS (4096) buckets with separate
 * chaining, one mutex and a FIFO eviction list. The one mutex is the app's
 * cache/version mutex (cf_app_version_mutex, app_internal.h): lookup,
 * admission comparison and data-version advancement share it, so an admission
 * can never insert a body under a version a commit has already superseded,
 * and no check-then-insert race exists. The cache creates, locks and destroys
 * no other lock; it never renders, compresses or scans entries.
 *
 * The frozen surface in cf.h is the lookup/admission pair:
 *   cf_err cf_cache_get(cf_cache *, cf_span key, cf_cached_body *out);
 *   cf_err cf_cache_put(cf_cache *, cf_span key, uint64_t expected_version,
 *                       const cf_cached_body *);
 *   void cf_cached_body_dispose(cf_cached_body *);
 * This header adds only lifecycle, accounting and the constants the
 * integrator's budget configuration needs.
 *
 * Keys are opaque byte strings built by the caller (the versioned key
 * encoding of 06 is K01c's). The table hashes them with HMAC-SHA256 under a
 * per-cache random key taken from cf_random_bytes at create, truncated to
 * 64 bits (the first 8 digest bytes, little-endian); entries in one bucket
 * are compared by their full key bytes, never by hash alone. Keys longer
 * than CF_CACHE_KEY_MAX are never stored.
 *
 * Budget accounting (all sizes are requested allocation sizes):
 *   bucket array  CF_CACHE_BUCKET_BYTES; charged whenever the cache is
 *                 enabled. If a nonzero budget cannot hold it, the cache is
 *                 created disabled and emits exactly one diagnostic;
 *                 budget 0 is the deliberate off switch (no diagnostic).
 *                 A disabled cache allocates no table and get/put are safe
 *                 no-ops (get bypasses, put bypasses).
 *   key capacity  the stored key length;
 *   entry         CF_CACHE_ENTRY_METADATA_BYTES of entry metadata;
 *   body          the body allocation: payload length +
 *                 CF_CACHE_BUF_HEADER_BYTES (the cf_buf header of
 *                 src/core/buffer.c, counted so the cache's number is the
 *                 allocator's request).
 * bytes_used = bucket + key + entry + body (cf_cache_stats). One entry's
 * total is cf_cache_entry_bytes(key_len, body_len); above CF_CACHE_ENTRY_MAX
 * (1 MiB including metadata) the entry bypasses. When an admission does not
 * fit the remaining budget, FIFO eviction releases the oldest entries until
 * it fits; an entry that cannot fit an empty cache bypasses. Eviction drops
 * only the cache's own body reference: an in-flight send holding its own
 * cf_buf reference keeps reading the bytes.
 *
 * Version rules:
 *   - cf_cache_get returns a body only while its stored version equals the
 *     current data version (read through version_fn under the mutex). An
 *     entry whose version is not current is unusable and is evicted lazily
 *     when encountered (the "stale" counter) -- never by a table scan.
 *   - cf_cache_put admits only if expected_version still equals the current
 *     version under the mutex. If another builder already stored this key at
 *     the current version, that entry wins and the admission is discarded
 *     (the "duplicate_builds" counter). A stale entry for the same key is
 *     replaced.
 *
 * Return codes:
 *   cf_cache_get   CF_OK = *out owns one body reference (etag/gzip copied);
 *                  CF_NOT_FOUND = miss, disabled cache, stale entry or a key
 *                  over the cap (out is zeroed); CF_INVALID = NULL argument
 *                  or a span with NULL bytes and nonzero length.
 *   cf_cache_put   CF_OK = stored, body retained; CF_BUSY = not stored:
 *                  disabled cache, version mismatch or a duplicate entry
 *                  (counters distinguish); CF_LIMIT = not stored: key over
 *                  the cap, entry over CF_CACHE_ENTRY_MAX, or no room in the
 *                  budget (all counted as bypasses); CF_NOMEM = allocation
 *                  failure, nothing stored; CF_INVALID = NULL argument.
 *   cf_cached_body_dispose releases the body reference if any and clears the
 *   struct; it is idempotent and accepts NULL.
 *
 * Counters (cf_cache_stats_get): hits, misses, bypasses, stale, evictions,
 * version_rejects and duplicate_builds. stale counts lazy version failures
 * that removed an entry; evictions counts FIFO releases made for room; the
 * two are disjoint. version_rejects is the put admission refused because the
 * captured version is no longer current (it is not a bypass).
 *
 * Lifecycle: cf_cache_create/cf_cache_destroy run during app lifecycle only,
 * with no request worker calling get/put; they are safe on a disabled cache.
 * The version mutex and version source are borrowed and must outlive the
 * cache. cf_cache_get/cf_cache_put are callable from any request worker
 * concurrently.
 */
#ifndef CF_CACHE_H
#define CF_CACHE_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cf.h"

/* Fixed table shape and caps (06: 4096 buckets; entry 1 MiB including
 * metadata; key 2048 bytes; default 64 MiB when enabled). */
#define CF_CACHE_BUCKETS ((size_t)4096)
#define CF_CACHE_KEY_MAX ((size_t)2048)
#define CF_CACHE_ENTRY_MAX ((size_t)(1024 * 1024))
#define CF_CACHE_DEFAULT_BUDGET_BYTES ((size_t)(64 * 1024 * 1024))

/* Accounting units, exact on the pinned x86_64 ABI. The entry metadata size
 * is static-asserted against struct cf_cache_entry in src/cache.c; the cf_buf
 * header mirrors the refcount + length header of src/core/buffer.c. */
#define CF_CACHE_ENTRY_METADATA_BYTES ((size_t)136)
#define CF_CACHE_BUF_HEADER_BYTES ((size_t)16)

/* The bucket array's own cost (charged as soon as the cache is enabled). */
#define CF_CACHE_BUCKET_BYTES ((size_t)CF_CACHE_BUCKETS * sizeof(void *))

/* Reads the current data version. Called with version_mutex held; it must not
 * take locks itself (cf_data_version is one acquire load). */
typedef uint64_t (*cf_cache_version_fn)(void *user);

/* Cache counters and gauges. Every counter is monotonic; byte gauges count
 * requested allocation sizes exactly as documented above. When disabled,
 * enabled=false, bucket_bytes and the byte gauges are 0 and only the
 * bypass/hit/miss counters move. */
typedef struct {
    bool enabled;
    size_t budget_bytes;
    size_t bytes_used;   /* bucket_bytes + key_bytes + entry_bytes + body_bytes */
    size_t bucket_bytes; /* CF_CACHE_BUCKET_BYTES, or 0 while disabled */
    size_t key_bytes;
    size_t entry_bytes;
    size_t body_bytes;
    uint64_t entries;
    uint64_t hits;
    uint64_t misses;
    uint64_t bypasses;
    uint64_t stale;
    uint64_t evictions;
    uint64_t version_rejects;
    uint64_t duplicate_builds;
} cf_cache_stats;

/* Create a cache with the configured budget. version_fn/version_user supply
 * the current data version and version_mutex is the app's cache/version mutex
 * (borrowed, not destroyed here); both are required. CF_OK returns an object
 * in *out even when the budget disables it (bucket array too large: one
 * diagnostic to stderr; budget 0: silent off switch). CF_IO when
 * cf_random_bytes fails for an enabled cache; CF_NOMEM when the table cannot
 * be allocated; *out stays NULL on any non-OK return.
 *
 * Production wiring (K01c):
 *   static uint64_t app_version(void *user) {
 *       return cf_data_version(user);
 *   }
 *   cf_cache_create(config->cache_bytes, app_version, app,
 *                   cf_app_version_mutex(app), &cache); */
cf_err cf_cache_create(size_t budget_bytes, cf_cache_version_fn version_fn,
                       void *version_user, pthread_mutex_t *version_mutex,
                       cf_cache **out);
/* Release every entry and body reference, then the cache. NULL is a no-op.
 * No get/put may be in flight. */
void cf_cache_destroy(cf_cache *cache);

/* Snapshot the counters and gauges under the version mutex. */
cf_err cf_cache_stats_get(const cf_cache *cache, cf_cache_stats *out);

/* Exact bytes one entry with this key and body pays for: entry metadata +
 * key capacity + body allocation capacity. SIZE_MAX on overflow. */
size_t cf_cache_entry_bytes(size_t key_len, size_t body_len);

/* ---- test seam (tests/cache only; the application never calls these) ----- */
/* While forced, every computed hash is `value` (all keys share one bucket),
 * which exercises the collision path deterministically; clear restores the
 * real HMAC-SHA256 hash. Single-threaded test setup only. */
void cf_cache_test_force_hash(uint64_t value);
void cf_cache_test_clear_hash(void);
/* The effective truncated hash for key (the forced value while armed). */
uint64_t cf_cache_test_hash(const cf_cache *cache, cf_span key);

#endif /* CF_CACHE_H */
