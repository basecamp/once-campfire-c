/* src/cache.c — complete-body cache core (K01b; 06-cache-performance.md
 * "K01: complete-body cache" and "Version/snapshot algorithm").
 *
 * Deliberately ordinary: fixed 4096 buckets with separate chaining, one
 * mutex (the app's cache/version mutex) and a FIFO eviction list. No
 * lock-free table, per-thread replica, LRU-on-hit mutation, generation
 * snapshot or background rebuild. Every field of the table is touched only
 * under the version mutex, so lookups, admissions, evictions and version
 * advancement are serialized by the one lock; cf_buf refcounts are atomic,
 * so an evicted body lives until the last retained reference is released.
 */
#include "cache.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One stored representation. The key bytes are a flexible array member, so
 * an entry is one allocation of metadata + key capacity. Four links make
 * removal from the bucket chain and the FIFO list O(1) from any position
 * (lazy stale eviction can remove the middle of the FIFO list). */
struct cf_cache_entry {
    struct cf_cache_entry *bucket_prev, *bucket_next; /* hash chain */
    struct cf_cache_entry *fifo_prev, *fifo_next;     /* insertion order */
    cf_buf *body;      /* owned reference while linked */
    uint64_t hash;     /* truncated keyed hash */
    uint64_t version;  /* data version at admission */
    uint32_t key_len;
    bool gzip;
    char weak_etag[69];
    unsigned char key[];
};

_Static_assert(sizeof(struct cf_cache_entry) == CF_CACHE_ENTRY_METADATA_BYTES,
               "cache.h accounting must match struct cf_cache_entry");

struct cf_cache {
    pthread_mutex_t *mutex;         /* borrowed cache/version mutex */
    cf_cache_version_fn version_fn; /* borrowed; called under the mutex */
    void *version_user;
    bool enabled;
    size_t budget_bytes;
    struct cf_cache_entry **buckets; /* NULL while disabled */
    struct cf_cache_entry *fifo_head, *fifo_tail;
    size_t key_bytes, entry_bytes, body_bytes;
    uint64_t entries;
    uint64_t hits, misses, bypasses, stale, evictions, version_rejects,
        duplicate_builds;
    unsigned char hash_key[32]; /* process-random; from cf_random_bytes */
};

/* Test seam (declared in cache.h): while armed every hash is this value. */
static _Atomic bool cf_cache_hash_forced = false;
static _Atomic uint64_t cf_cache_forced_hash = 0;

static size_t cf_cache_bucket_of(uint64_t hash) {
    return (size_t)(hash & (CF_CACHE_BUCKETS - 1));
}

/* HMAC-SHA256 under the per-cache random key, truncated to the first 64 bits
 * (first 8 digest bytes, little-endian). A digest failure cannot be reported
 * through the frozen api.h lookup; it degrades to one bucket while the
 * full-key comparison keeps results correct. */
static uint64_t cf_cache_hash(const cf_cache *cache, cf_span key) {
    if (atomic_load_explicit(&cf_cache_hash_forced, memory_order_relaxed)) {
        return atomic_load_explicit(&cf_cache_forced_hash,
                                    memory_order_relaxed);
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    const unsigned char empty = 0;
    const unsigned char *data = key.len != 0 ? key.ptr : &empty;
    if (HMAC(EVP_sha256(), cache->hash_key, (int)sizeof cache->hash_key, data,
             key.len, digest, &len) == NULL ||
        len < 8) {
        return 0;
    }
    uint64_t hash;
    memcpy(&hash, digest, sizeof hash);
    return hash;
}

static bool cf_cache_key_matches(const struct cf_cache_entry *e, cf_span key,
                                 uint64_t hash) {
    return e->hash == hash && e->key_len == key.len &&
           (key.len == 0 || memcmp(e->key, key.ptr, key.len) == 0);
}

static void cf_cache_entry_unlink(cf_cache *cache, struct cf_cache_entry *e) {
    if (e->bucket_prev != NULL) {
        e->bucket_prev->bucket_next = e->bucket_next;
    } else {
        cache->buckets[cf_cache_bucket_of(e->hash)] = e->bucket_next;
    }
    if (e->bucket_next != NULL) e->bucket_next->bucket_prev = e->bucket_prev;

    if (e->fifo_prev != NULL) {
        e->fifo_prev->fifo_next = e->fifo_next;
    } else {
        cache->fifo_head = e->fifo_next;
    }
    if (e->fifo_next != NULL) {
        e->fifo_next->fifo_prev = e->fifo_prev;
    } else {
        cache->fifo_tail = e->fifo_prev;
    }
}

/* Drop one linked entry and its body reference (an in-flight send may hold
 * its own reference; cf_buf counts it), keeping the byte gauges exact. */
static void cf_cache_entry_release(cf_cache *cache, struct cf_cache_entry *e) {
    cache->key_bytes -= e->key_len;
    cache->entry_bytes -= CF_CACHE_ENTRY_METADATA_BYTES;
    cache->body_bytes -= cf_buf_span(e->body).len + CF_CACHE_BUF_HEADER_BYTES;
    cache->entries--;
    cf_buf_release(e->body);
    free(e);
}

static void cf_cache_evict_fifo(cf_cache *cache) {
    struct cf_cache_entry *e = cache->fifo_head;
    cf_cache_entry_unlink(cache, e);
    cf_cache_entry_release(cache, e);
    cache->evictions++;
}

static void cf_cache_insert(cf_cache *cache, struct cf_cache_entry *e) {
    size_t bucket = cf_cache_bucket_of(e->hash);
    e->bucket_prev = NULL;
    e->bucket_next = cache->buckets[bucket];
    if (e->bucket_next != NULL) e->bucket_next->bucket_prev = e;
    cache->buckets[bucket] = e;

    e->fifo_next = NULL;
    e->fifo_prev = cache->fifo_tail;
    if (e->fifo_prev != NULL) {
        e->fifo_prev->fifo_next = e;
    } else {
        cache->fifo_head = e;
    }
    cache->fifo_tail = e;

    cache->key_bytes += e->key_len;
    cache->entry_bytes += CF_CACHE_ENTRY_METADATA_BYTES;
    cache->body_bytes += cf_buf_span(e->body).len + CF_CACHE_BUF_HEADER_BYTES;
    cache->entries++;
}

cf_err cf_cache_create(size_t budget_bytes, cf_cache_version_fn version_fn,
                       void *version_user, pthread_mutex_t *version_mutex,
                       cf_cache **out) {
    if (out == NULL) return CF_INVALID;
    *out = NULL;
    if (version_fn == NULL || version_mutex == NULL) return CF_INVALID;

    cf_cache *cache = calloc(1, sizeof *cache);
    if (cache == NULL) return CF_NOMEM;
    cache->mutex = version_mutex;
    cache->version_fn = version_fn;
    cache->version_user = version_user;
    cache->budget_bytes = budget_bytes;

    if (budget_bytes == 0) {
        *out = cache; /* deliberate off switch: no table, no diagnostic */
        return CF_OK;
    }
    if (budget_bytes < CF_CACHE_BUCKET_BYTES) {
        fprintf(stderr,
                "campfire: cache: disabled: budget %zu bytes cannot hold the "
                "%zu-byte bucket array\n",
                budget_bytes, (size_t)CF_CACHE_BUCKET_BYTES);
        *out = cache;
        return CF_OK;
    }
    if (cf_random_bytes(cache->hash_key, sizeof cache->hash_key) != CF_OK) {
        free(cache);
        return CF_IO;
    }
    cache->buckets = calloc(CF_CACHE_BUCKETS, sizeof *cache->buckets);
    if (cache->buckets == NULL) {
        free(cache);
        return CF_NOMEM;
    }
    cache->enabled = true;
    *out = cache;
    return CF_OK;
}

void cf_cache_destroy(cf_cache *cache) {
    if (cache == NULL) return;
    pthread_mutex_lock(cache->mutex);
    struct cf_cache_entry *e = cache->fifo_head;
    while (e != NULL) {
        struct cf_cache_entry *next = e->fifo_next;
        cf_buf_release(e->body);
        free(e);
        e = next;
    }
    free(cache->buckets);
    pthread_mutex_unlock(cache->mutex);
    free(cache);
}

cf_err cf_cache_get(cf_cache *cache, cf_span key, cf_cached_body *out) {
    if (cache == NULL || out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    if (key.len != 0 && key.ptr == NULL) return CF_INVALID;

    pthread_mutex_lock(cache->mutex);
    if (!cache->enabled) {
        cache->bypasses++;
        pthread_mutex_unlock(cache->mutex);
        return CF_NOT_FOUND;
    }
    if (key.len > CF_CACHE_KEY_MAX) {
        cache->misses++; /* a key over the cap can never have been admitted */
        pthread_mutex_unlock(cache->mutex);
        return CF_NOT_FOUND;
    }

    uint64_t current = cache->version_fn(cache->version_user);
    uint64_t hash = cf_cache_hash(cache, key);
    size_t bucket = cf_cache_bucket_of(hash);
    for (struct cf_cache_entry *e = cache->buckets[bucket]; e != NULL;
         e = e->bucket_next) {
        if (!cf_cache_key_matches(e, key, hash)) continue;
        if (e->version != current) {
            /* Unusable: evict lazily, exactly where it was encountered. */
            cf_cache_entry_unlink(cache, e);
            cf_cache_entry_release(cache, e);
            cache->stale++;
            break;
        }
        out->body = cf_buf_retain(e->body);
        memcpy(out->weak_etag, e->weak_etag, sizeof out->weak_etag);
        out->gzip = e->gzip;
        cache->hits++;
        pthread_mutex_unlock(cache->mutex);
        return CF_OK;
    }
    cache->misses++;
    pthread_mutex_unlock(cache->mutex);
    return CF_NOT_FOUND;
}

cf_err cf_cache_put(cf_cache *cache, cf_span key, uint64_t expected_version,
                    const cf_cached_body *body) {
    if (cache == NULL || body == NULL || body->body == NULL) return CF_INVALID;
    if (key.len != 0 && key.ptr == NULL) return CF_INVALID;

    pthread_mutex_lock(cache->mutex);
    if (!cache->enabled) {
        cache->bypasses++;
        pthread_mutex_unlock(cache->mutex);
        return CF_BUSY;
    }
    if (key.len > CF_CACHE_KEY_MAX) {
        cache->bypasses++;
        pthread_mutex_unlock(cache->mutex);
        return CF_LIMIT;
    }
    uint64_t current = cache->version_fn(cache->version_user);
    if (expected_version != current) {
        cache->version_rejects++;
        pthread_mutex_unlock(cache->mutex);
        return CF_BUSY;
    }

    size_t cost = cf_cache_entry_bytes(key.len, cf_buf_span(body->body).len);
    if (cost > CF_CACHE_ENTRY_MAX || cost > cache->budget_bytes) {
        cache->bypasses++;
        pthread_mutex_unlock(cache->mutex);
        return CF_LIMIT;
    }

    uint64_t hash = cf_cache_hash(cache, key);
    size_t bucket = cf_cache_bucket_of(hash);
    for (struct cf_cache_entry *e = cache->buckets[bucket]; e != NULL;
         e = e->bucket_next) {
        if (!cf_cache_key_matches(e, key, hash)) continue;
        if (e->version == current) {
            /* A racing builder already admitted this key: it wins. */
            cache->duplicate_builds++;
            pthread_mutex_unlock(cache->mutex);
            return CF_BUSY;
        }
        /* Same key admitted under an older version: replace it. */
        cf_cache_entry_unlink(cache, e);
        cf_cache_entry_release(cache, e);
        cache->stale++;
        break;
    }

    /* FIFO eviction until the entry fits (used <= budget is invariant). */
    for (;;) {
        size_t used = CF_CACHE_BUCKET_BYTES + cache->key_bytes +
                      cache->entry_bytes + cache->body_bytes;
        if (cache->budget_bytes - used >= cost) break;
        if (cache->fifo_head == NULL) {
            cache->bypasses++; /* cannot fit even an empty cache */
            pthread_mutex_unlock(cache->mutex);
            return CF_LIMIT;
        }
        cf_cache_evict_fifo(cache);
    }

    struct cf_cache_entry *e = malloc(sizeof *e + key.len);
    if (e == NULL) {
        pthread_mutex_unlock(cache->mutex);
        return CF_NOMEM;
    }
    e->hash = hash;
    e->version = current;
    e->key_len = (uint32_t)key.len;
    e->gzip = body->gzip;
    memcpy(e->weak_etag, body->weak_etag, sizeof e->weak_etag);
    if (key.len != 0) memcpy(e->key, key.ptr, key.len);
    e->body = cf_buf_retain(body->body);
    cf_cache_insert(cache, e);
    pthread_mutex_unlock(cache->mutex);
    return CF_OK;
}

void cf_cached_body_dispose(cf_cached_body *body) {
    if (body == NULL) return;
    cf_buf_release(body->body);
    memset(body, 0, sizeof *body);
}

cf_err cf_cache_stats_get(const cf_cache *cache, cf_cache_stats *out) {
    if (cache == NULL || out == NULL) return CF_INVALID;
    cf_cache *mutable_cache = (cf_cache *)cache;
    pthread_mutex_lock(mutable_cache->mutex);
    out->enabled = mutable_cache->enabled;
    out->budget_bytes = mutable_cache->budget_bytes;
    out->bucket_bytes =
        mutable_cache->enabled ? CF_CACHE_BUCKET_BYTES : (size_t)0;
    out->key_bytes = mutable_cache->key_bytes;
    out->entry_bytes = mutable_cache->entry_bytes;
    out->body_bytes = mutable_cache->body_bytes;
    out->bytes_used = out->bucket_bytes + out->key_bytes + out->entry_bytes +
                      out->body_bytes;
    out->entries = mutable_cache->entries;
    out->hits = mutable_cache->hits;
    out->misses = mutable_cache->misses;
    out->bypasses = mutable_cache->bypasses;
    out->stale = mutable_cache->stale;
    out->evictions = mutable_cache->evictions;
    out->version_rejects = mutable_cache->version_rejects;
    out->duplicate_builds = mutable_cache->duplicate_builds;
    pthread_mutex_unlock(mutable_cache->mutex);
    return CF_OK;
}

size_t cf_cache_entry_bytes(size_t key_len, size_t body_len) {
    if (body_len > SIZE_MAX - CF_CACHE_BUF_HEADER_BYTES) return SIZE_MAX;
    size_t body = body_len + CF_CACHE_BUF_HEADER_BYTES;
    if (key_len > SIZE_MAX - CF_CACHE_ENTRY_METADATA_BYTES) return SIZE_MAX;
    size_t meta = CF_CACHE_ENTRY_METADATA_BYTES + key_len;
    if (body > SIZE_MAX - meta) return SIZE_MAX;
    return meta + body;
}

/* ---- test seam (cache.h) -------------------------------------------------- */

void cf_cache_test_force_hash(uint64_t value) {
    atomic_store_explicit(&cf_cache_forced_hash, value, memory_order_relaxed);
    atomic_store_explicit(&cf_cache_hash_forced, true, memory_order_relaxed);
}

void cf_cache_test_clear_hash(void) {
    atomic_store_explicit(&cf_cache_hash_forced, false, memory_order_relaxed);
    atomic_store_explicit(&cf_cache_forced_hash, 0, memory_order_relaxed);
}

uint64_t cf_cache_test_hash(const cf_cache *cache, cf_span key) {
    if (atomic_load_explicit(&cf_cache_hash_forced, memory_order_relaxed)) {
        return atomic_load_explicit(&cf_cache_forced_hash,
                                    memory_order_relaxed);
    }
    if (cache == NULL) return 0;
    return cf_cache_hash(cache, key);
}
