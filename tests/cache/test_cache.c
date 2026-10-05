/* K01b cache core tests: table behavior, collision path, budget arithmetic,
 * FIFO/stale eviction, version rules, counters, caps and lifecycle
 * (06-cache-performance.md; CACHE-02/CACHE-04 module side, CORE-02).
 *
 * Every case is falsifiable by construction: the accounting expectations are
 * hand arithmetic over cache.h's constants, the collision and hash cases use
 * the cache.h test seam, and the boundary budgets are the exact bucket/metadata
 * numbers (see docs/devel/evidence/K01b.md for the scratch-copy mutations that
 * turn each core probe red).
 */
#include "cf_test.h"

#include "cache_testutil.h"

#include "core/testrandom.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <stdlib.h>
#include <string.h>

#define BUCKET ((size_t)CF_CACHE_BUCKET_BYTES)
#define META ((size_t)CF_CACHE_ENTRY_METADATA_BYTES)
#define HDR ((size_t)CF_CACHE_BUF_HEADER_BYTES)

#define CHECK_STATS(s, entries_, hits_, misses_, bypasses_, stale_, evictions_, \
                    vr_, dup_)                                                \
    do {                                                                      \
        CF_CHECK((s).entries == (entries_));                                  \
        CF_CHECK((s).hits == (hits_));                                        \
        CF_CHECK((s).misses == (misses_));                                    \
        CF_CHECK((s).bypasses == (bypasses_));                                \
        CF_CHECK((s).stale == (stale_));                                      \
        CF_CHECK((s).evictions == (evictions_));                              \
        CF_CHECK((s).version_rejects == (vr_));                               \
        CF_CHECK((s).duplicate_builds == (dup_));                             \
    } while (0)

struct create_capture {
    cache_version_fixture *version;
    size_t budget;
    cf_cache *cache;
    cf_err rc;
};

static void create_capture_fn(void *user) {
    struct create_capture *c = user;
    c->cache = NULL;
    c->rc = cf_cache_create(c->budget, cache_version_reader, c->version,
                            &c->version->mutex, &c->cache);
}

/* Disabled budgets: 0 is the silent off switch; a nonzero budget smaller than
 * the bucket array is disabled with exactly one diagnostic; exactly the
 * bucket array is enabled. */
CF_TEST(cache_disabled_budgets_and_one_diagnostic) {
    char buf[512];
    cache_version_fixture version;
    cache_version_fixture_init(&version, 1);
    struct create_capture cap = {&version, 0, NULL, CF_OK};
    cf_cache_stats st;

    size_t n = cache_capture_stderr(create_capture_fn, &cap, buf, sizeof buf);
    CF_CHECK(n == 0);
    CF_CHECK(cap.rc == CF_OK);
    CF_REQUIRE(cap.cache != NULL);
    CF_REQUIRE(cf_cache_stats_get(cap.cache, &st) == CF_OK);
    CF_CHECK(!st.enabled);
    CF_CHECK(st.budget_bytes == 0);
    CF_CHECK(st.bytes_used == 0);
    CF_CHECK(st.bucket_bytes == 0);
    CF_CHECK(st.entries == 0);

    cf_cached_body miss;
    CF_CHECK(cf_cache_get(cap.cache, cache_span("k"), &miss) == CF_NOT_FOUND);
    CF_CHECK(miss.body == NULL);
    cf_cached_body body;
    CF_REQUIRE(cache_body_make("x", false, "t", &body) == CF_OK);
    CF_CHECK(cf_cache_put(cap.cache, cache_span("k"), 1, &body) == CF_BUSY);
    cf_cached_body_dispose(&body);
    CF_REQUIRE(cf_cache_stats_get(cap.cache, &st) == CF_OK);
    CF_CHECK(st.bypasses == 2); /* disabled get and disabled put */
    cf_cache_destroy(cap.cache);

    const size_t needles[] = {1, CF_CACHE_BUCKET_BYTES - 1};
    for (size_t i = 0; i < sizeof needles / sizeof needles[0]; i++) {
        cap.budget = needles[i];
        cap.cache = NULL;
        cap.rc = CF_OK;
        memset(buf, 0, sizeof buf);
        n = cache_capture_stderr(create_capture_fn, &cap, buf, sizeof buf);
        CF_CHECK(cap.rc == CF_OK);
        CF_REQUIRE(cap.cache != NULL);
        CF_CHECK(n != 0);
        CF_CHECK(cache_count_occurrences(buf, "cache: disabled") == 1);
        CF_CHECK(cache_count_occurrences(buf, "bucket array") == 1);
        CF_REQUIRE(cf_cache_stats_get(cap.cache, &st) == CF_OK);
        CF_CHECK(!st.enabled);
        CF_CHECK(st.bytes_used == 0);
        cf_cache_destroy(cap.cache);
    }

    cap.budget = CF_CACHE_BUCKET_BYTES;
    cap.cache = NULL;
    cap.rc = CF_OK;
    memset(buf, 0, sizeof buf);
    n = cache_capture_stderr(create_capture_fn, &cap, buf, sizeof buf);
    CF_CHECK(n == 0);
    CF_CHECK(cap.rc == CF_OK);
    CF_REQUIRE(cap.cache != NULL);
    CF_REQUIRE(cf_cache_stats_get(cap.cache, &st) == CF_OK);
    CF_CHECK(st.enabled);
    CF_CHECK(st.bucket_bytes == BUCKET);
    CF_CHECK(st.bytes_used == BUCKET);
    cf_cache_destroy(cap.cache);
    cache_version_fixture_dispose(&version);
}

/* The bucket array is charged first: a budget of exactly the bucket array
 * cannot hold any entry (bypass, not eviction failure), and a budget of
 * bucket + one exact entry cost holds exactly one, evicting it for the next. */
CF_TEST(cache_budget_boundary_bucket_and_entry_fit) {
    size_t cost = cf_cache_entry_bytes(1, 4); /* key "k" + body "abcd" */
    CF_CHECK(cost == META + 1 + HDR + 4);
    CF_CHECK(cost == 157);

    cache_env env;
    cache_env_open(&env, 1, CF_CACHE_BUCKET_BYTES);
    cf_cached_body body;
    CF_REQUIRE(cache_body_make("abcd", false, "b", &body) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, cache_span("k"), 1, &body) == CF_LIMIT);
    cf_cached_body_dispose(&body);
    cf_cache_stats st;
    CF_REQUIRE(cf_cache_stats_get(env.cache, &st) == CF_OK);
    CF_CHECK(st.enabled);
    CF_CHECK(st.entries == 0);
    CF_CHECK(st.bypasses == 1);
    CF_CHECK(st.bytes_used == BUCKET);
    cache_env_close(&env);

    cache_env_open(&env, 1, CF_CACHE_BUCKET_BYTES + cost);
    CF_REQUIRE(cache_body_make("abcd", false, "b", &body) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, cache_span("k"), 1, &body) == CF_OK);
    cf_cached_body_dispose(&body);
    CF_REQUIRE(cf_cache_stats_get(env.cache, &st) == CF_OK);
    CHECK_STATS(st, 1, 0, 0, 0, 0, 0, 0, 0);
    CF_CHECK(st.bytes_used == CF_CACHE_BUCKET_BYTES + cost);
    CF_CHECK(st.key_bytes == 1 && st.entry_bytes == META && st.body_bytes == HDR + 4);

    CF_REQUIRE(cache_body_make("abcd", false, "c", &body) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, cache_span("j"), 1, &body) == CF_OK);
    cf_cached_body_dispose(&body);
    CF_REQUIRE(cf_cache_stats_get(env.cache, &st) == CF_OK);
    CHECK_STATS(st, 1, 0, 0, 0, 0, 1, 0, 0);
    CF_CHECK(st.bytes_used == CF_CACHE_BUCKET_BYTES + cost);
    cf_cached_body got;
    CF_CHECK(cf_cache_get(env.cache, cache_span("k"), &got) == CF_NOT_FOUND);
    CF_CHECK(cf_cache_get(env.cache, cache_span("j"), &got) == CF_OK);
    CF_CHECK(got.body != NULL && cache_buf_is(got.body, "abcd"));
    cf_cached_body_dispose(&got);
    cache_env_close(&env);
}

/* Entry metadata, key capacity and the body allocation are all counted, and
 * etag/gzip round-trip with an owned body reference. */
CF_TEST(cache_entry_metadata_and_pieces_accounted) {
    cache_env env;
    cache_env_open(&env, 3, CF_CACHE_DEFAULT_BUDGET_BYTES);
    cf_cache_stats before, after;
    CF_REQUIRE(cf_cache_stats_get(env.cache, &before) == CF_OK);
    CF_CHECK(before.bytes_used == BUCKET);

    cf_cached_body body;
    CF_REQUIRE(cache_body_make("0123456789", true, "meta", &body) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, cache_span("hello"), 3, &body) == CF_OK);
    cf_cached_body_dispose(&body); /* the cache keeps its own reference */
    CF_REQUIRE(cf_cache_stats_get(env.cache, &after) == CF_OK);
    CF_CHECK(after.key_bytes - before.key_bytes == 5);
    CF_CHECK(after.entry_bytes - before.entry_bytes == META);
    CF_CHECK(after.body_bytes - before.body_bytes == 10 + HDR);
    CF_CHECK(after.bytes_used == BUCKET + cf_cache_entry_bytes(5, 10));
    CF_CHECK(after.bytes_used == BUCKET + META + 5 + HDR + 10);
    CF_CHECK(after.entries == 1);

    cf_cached_body got;
    CF_CHECK(cf_cache_get(env.cache, cache_span("hello"), &got) == CF_OK);
    CF_CHECK(got.body != NULL && cache_buf_is(got.body, "0123456789"));
    CF_CHECK(got.gzip);
    CF_CHECK(strcmp(got.weak_etag, "W/\"meta\"") == 0);
    cf_cached_body_dispose(&got);
    cache_env_close(&env);
}

/* One entry, two entries, then more live entries than buckets: every stored
 * key stays reachable through its chain. */
CF_TEST(cache_table_one_two_and_many_entries) {
    cache_env env;
    cache_env_open(&env, 1, CF_CACHE_DEFAULT_BUDGET_BYTES);
    cf_cached_body body, got;
    CF_REQUIRE(cache_body_make("1", false, "one", &body) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, cache_span("one"), 1, &body) == CF_OK);
    cf_cached_body_dispose(&body);
    CF_CHECK(cf_cache_get(env.cache, cache_span("one"), &got) == CF_OK);
    CF_CHECK(cache_buf_is(got.body, "1"));
    cf_cached_body_dispose(&got);

    CF_REQUIRE(cache_body_make("2", false, "two", &body) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, cache_span("two"), 1, &body) == CF_OK);
    cf_cached_body_dispose(&body);
    CF_CHECK(cf_cache_get(env.cache, cache_span("one"), &got) == CF_OK);
    CF_CHECK(cache_buf_is(got.body, "1"));
    cf_cached_body_dispose(&got);
    CF_CHECK(cf_cache_get(env.cache, cache_span("two"), &got) == CF_OK);
    CF_CHECK(cache_buf_is(got.body, "2"));
    cf_cached_body_dispose(&got);

    enum { MANY = 4202 };
    char key[16], payload[16];
    for (int i = 0; i < MANY; i++) {
        snprintf(key, sizeof key, "k%04d", i);
        snprintf(payload, sizeof payload, "v%04d", i);
        CF_REQUIRE(cache_body_make(payload, false, "m", &body) == CF_OK);
        CF_CHECK(cf_cache_put(env.cache, cache_span(key), 1, &body) == CF_OK);
        cf_cached_body_dispose(&body);
    }
    cf_cache_stats st;
    CF_REQUIRE(cf_cache_stats_get(env.cache, &st) == CF_OK);
    CF_CHECK(st.entries == MANY + 2);
    CF_CHECK(st.bytes_used <= st.budget_bytes);
    for (int i = 0; i < MANY; i += 53) {
        snprintf(key, sizeof key, "k%04d", i);
        snprintf(payload, sizeof payload, "v%04d", i);
        CF_CHECK(cf_cache_get(env.cache, cache_span(key), &got) == CF_OK);
        CF_CHECK(got.body != NULL && cache_buf_is(got.body, payload));
        cf_cached_body_dispose(&got);
    }
    cache_env_close(&env);
}

/* Forced hash: every key shares one bucket, so the full-key comparison is the
 * only thing that can tell same-length keys apart. */
CF_TEST(cache_collision_forced_hash_full_key_compare) {
    cf_cache_test_clear_hash();
    cache_env env;
    cache_env_open(&env, 1, CF_CACHE_DEFAULT_BUDGET_BYTES);
    cf_cache_test_force_hash(UINT64_C(0xC0FFEE));
    CF_CHECK(cf_cache_test_hash(env.cache, cache_span("alpha")) ==
             UINT64_C(0xC0FFEE));
    CF_CHECK(cf_cache_test_hash(env.cache, cache_span("charlie")) ==
             UINT64_C(0xC0FFEE));

    cf_cached_body a, b, c;
    CF_REQUIRE(cache_body_make("AAAA", false, "a", &a) == CF_OK);
    CF_REQUIRE(cache_body_make("BBBB", false, "b", &b) == CF_OK);
    CF_REQUIRE(cache_body_make("CCCCCC", false, "c", &c) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, cache_span("alpha"), 1, &a) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, cache_span("bravo"), 1, &b) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, cache_span("charlie"), 1, &c) == CF_OK);
    cf_cached_body_dispose(&a);
    cf_cached_body_dispose(&b);
    cf_cached_body_dispose(&c);

    cf_cached_body got;
    CF_CHECK(cf_cache_get(env.cache, cache_span("alpha"), &got) == CF_OK);
    CF_CHECK(cache_buf_is(got.body, "AAAA"));
    cf_cached_body_dispose(&got);
    CF_CHECK(cf_cache_get(env.cache, cache_span("bravo"), &got) == CF_OK);
    CF_CHECK(cache_buf_is(got.body, "BBBB"));
    cf_cached_body_dispose(&got);
    CF_CHECK(cf_cache_get(env.cache, cache_span("charlie"), &got) == CF_OK);
    CF_CHECK(cache_buf_is(got.body, "CCCCCC"));
    cf_cached_body_dispose(&got);
    CF_CHECK(cf_cache_get(env.cache, cache_span("delta"), &got) ==
             CF_NOT_FOUND);
    CF_CHECK(got.body == NULL);

    cf_cache_stats st;
    CF_REQUIRE(cf_cache_stats_get(env.cache, &st) == CF_OK);
    CF_CHECK(st.entries == 3);
    cf_cache_test_clear_hash();
    cache_env_close(&env);
}

/* The hash is HMAC-SHA256 under a 32-byte process-random key, truncated to
 * the first 64 digest bits (little-endian). The testrandom seam pins the key
 * to 0xAB*32, so the expected value is computed here with OpenSSL. */
CF_TEST(cache_hash_is_keyed_hmac_sha256_truncated_to_64_bits) {
    cf_cache_test_clear_hash();
    cf_test_random_fill(0xAB);
    cache_version_fixture version;
    cache_version_fixture_init(&version, 1);
    cf_cache *cache = NULL;
    cf_err rc = cf_cache_create(CF_CACHE_DEFAULT_BUDGET_BYTES,
                                cache_version_reader, &version,
                                &version.mutex, &cache);
    cf_test_random_clear(); /* clear before any assertion can longjmp out */
    CF_REQUIRE(rc == CF_OK);
    CF_REQUIRE(cache != NULL);

    unsigned char key[32];
    memset(key, 0xAB, sizeof key);
    const char *probe = "probe-key";
    const char *probe2 = "probe-key-2";
    unsigned char digest[EVP_MAX_MD_SIZE], digest2[EVP_MAX_MD_SIZE];
    unsigned int dlen = 0, dlen2 = 0;
    CF_REQUIRE(HMAC(EVP_sha256(), key, (int)sizeof key,
                    (const unsigned char *)probe, strlen(probe), digest,
                    &dlen) != NULL);
    CF_REQUIRE(HMAC(EVP_sha256(), key, (int)sizeof key,
                    (const unsigned char *)probe2, strlen(probe2), digest2,
                    &dlen2) != NULL);
    CF_REQUIRE(dlen == 32 && dlen2 == 32);
    uint64_t expected, expected2;
    memcpy(&expected, digest, sizeof expected);
    memcpy(&expected2, digest2, sizeof expected2);
    CF_CHECK(expected != expected2);
    CF_CHECK(cf_cache_test_hash(cache, cache_span(probe)) == expected);
    CF_CHECK(cf_cache_test_hash(cache, cache_span(probe2)) == expected2);

    cf_cache_destroy(cache);
    cache_version_fixture_dispose(&version);
}

/* A racing builder's entry wins: the second admission is refused and the
 * first body is what a reader gets. */
CF_TEST(cache_duplicate_admission_keeps_first_entry) {
    cache_env env;
    cache_env_open(&env, 7, CF_CACHE_DEFAULT_BUDGET_BYTES);
    cf_cached_body first, second, got;
    CF_REQUIRE(cache_body_make("first", false, "f", &first) == CF_OK);
    CF_REQUIRE(cache_body_make("second", false, "s", &second) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, cache_span("dup"), 7, &first) == CF_OK);
    cf_cache_stats before, after;
    CF_REQUIRE(cf_cache_stats_get(env.cache, &before) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, cache_span("dup"), 7, &second) == CF_BUSY);
    CF_REQUIRE(cf_cache_stats_get(env.cache, &after) == CF_OK);
    CHECK_STATS(after, 1, 0, 0, 0, 0, 0, 0, 1);
    CF_CHECK(after.bytes_used == before.bytes_used);
    CF_CHECK(cf_cache_get(env.cache, cache_span("dup"), &got) == CF_OK);
    CF_CHECK(cache_buf_is(got.body, "first"));
    cf_cached_body_dispose(&got);
    cf_cached_body_dispose(&first);
    cf_cached_body_dispose(&second);
    cache_env_close(&env);
}

/* expected_version must still equal the current version under the mutex; a
 * stale same-key entry is replaced, not treated as a duplicate. */
CF_TEST(cache_version_mismatch_rejects_and_replaces_stale_key) {
    cache_env env;
    cache_env_open(&env, 7, CF_CACHE_DEFAULT_BUDGET_BYTES);
    cf_cached_body body, got;
    cf_cache_stats st;
    CF_REQUIRE(cache_body_make("v1", false, "v1", &body) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, cache_span("k"), 6, &body) == CF_BUSY);
    cf_cached_body_dispose(&body);
    CF_REQUIRE(cf_cache_stats_get(env.cache, &st) == CF_OK);
    CHECK_STATS(st, 0, 0, 0, 0, 0, 0, 1, 0);
    CF_CHECK(st.bytes_used == BUCKET);

    CF_REQUIRE(cache_body_make("v1", false, "v1", &body) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, cache_span("k"), 7, &body) == CF_OK);
    cf_cached_body_dispose(&body);
    cache_version_advance(&env.version);
    CF_REQUIRE(cache_body_make("v2", false, "v2", &body) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, cache_span("k"), 7, &body) == CF_BUSY);
    cf_cached_body_dispose(&body);
    CF_REQUIRE(cf_cache_stats_get(env.cache, &st) == CF_OK);
    CF_CHECK(st.version_rejects == 2);
    CF_CHECK(st.entries == 1);

    CF_REQUIRE(cache_body_make("v2", false, "v2", &body) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, cache_span("k"), 8, &body) == CF_OK);
    cf_cached_body_dispose(&body);
    CF_REQUIRE(cf_cache_stats_get(env.cache, &st) == CF_OK);
    CF_CHECK(st.entries == 1);
    CF_CHECK(st.stale == 1); /* the stale same-key entry was replaced */
    CF_CHECK(st.duplicate_builds == 0);
    CF_CHECK(cf_cache_get(env.cache, cache_span("k"), &got) == CF_OK);
    CF_CHECK(cache_buf_is(got.body, "v2"));
    cf_cached_body_dispose(&got);
    cache_env_close(&env);
}

/* Version advance makes entries unusable; a get evicts the stale entry it
 * found and reports a miss, leaving other keys alone. */
CF_TEST(cache_stale_reads_evict_lazily_under_version_advance) {
    cache_env env;
    cache_env_open(&env, 1, CF_CACHE_DEFAULT_BUDGET_BYTES);
    cf_cached_body body, got;
    CF_REQUIRE(cache_body_make("old", false, "s", &body) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, cache_span("stale"), 1, &body) == CF_OK);
    cf_cached_body_dispose(&body);
    CF_REQUIRE(cache_body_make("stay", false, "l", &body) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, cache_span("live"), 1, &body) == CF_OK);
    cf_cached_body_dispose(&body);
    cf_cache_stats st;
    CF_REQUIRE(cf_cache_stats_get(env.cache, &st) == CF_OK);
    CF_CHECK(st.entries == 2);
    size_t both = st.bytes_used;

    cache_version_advance(&env.version); /* one writer publishes 2 */
    CF_CHECK(cf_cache_get(env.cache, cache_span("stale"), &got) ==
             CF_NOT_FOUND);
    CF_CHECK(got.body == NULL);
    CF_REQUIRE(cf_cache_stats_get(env.cache, &st) == CF_OK);
    CHECK_STATS(st, 1, 0, 1, 0, 1, 0, 0, 0);
    CF_CHECK(st.bytes_used ==
             both - cf_cache_entry_bytes(strlen("stale"), strlen("old")));

    CF_CHECK(cf_cache_get(env.cache, cache_span("live"), &got) ==
             CF_NOT_FOUND);
    CF_REQUIRE(cf_cache_stats_get(env.cache, &st) == CF_OK);
    CHECK_STATS(st, 0, 0, 2, 0, 2, 0, 0, 0);
    CF_CHECK(st.bytes_used == BUCKET);
    /* nothing left to evict on a third read */
    CF_CHECK(cf_cache_get(env.cache, cache_span("live"), &got) ==
             CF_NOT_FOUND);
    CF_REQUIRE(cf_cache_stats_get(env.cache, &st) == CF_OK);
    CF_CHECK(st.misses == 3 && st.stale == 2 && st.entries == 0);
    cache_env_close(&env);
}

/* The 1 MiB entry cap includes metadata: exactly the cap is admitted, one
 * byte more bypasses and leaves the admitted entry alone. */
CF_TEST(cache_oversized_entry_and_key_bypass_boundaries) {
    cache_env env;
    cache_env_open(&env, 1, CF_CACHE_DEFAULT_BUDGET_BYTES);
    size_t max_body = CF_CACHE_ENTRY_MAX - META - 1 - HDR; /* key "k" */
    CF_CHECK(max_body == CF_CACHE_ENTRY_MAX - 153);
    CF_CHECK(cf_cache_entry_bytes(1, max_body) == CF_CACHE_ENTRY_MAX);

    cf_cached_body big, got;
    CF_REQUIRE(cache_body_make_fill('x', max_body, &big) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, cache_span("k"), 1, &big) == CF_OK);
    cf_cached_body_dispose(&big);
    cf_cache_stats st;
    CF_REQUIRE(cf_cache_stats_get(env.cache, &st) == CF_OK);
    CHECK_STATS(st, 1, 0, 0, 0, 0, 0, 0, 0);
    CF_CHECK(st.bytes_used == BUCKET + CF_CACHE_ENTRY_MAX);

    CF_REQUIRE(cache_body_make_fill('x', max_body + 1, &big) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, cache_span("k"), 1, &big) == CF_LIMIT);
    cf_cached_body_dispose(&big);
    CF_REQUIRE(cf_cache_stats_get(env.cache, &st) == CF_OK);
    CF_CHECK(st.bypasses == 1 && st.entries == 1);
    CF_CHECK(st.bytes_used == BUCKET + CF_CACHE_ENTRY_MAX);
    CF_CHECK(cf_cache_get(env.cache, cache_span("k"), &got) == CF_OK);
    CF_CHECK(cf_buf_span(got.body).len == max_body);
    cf_cached_body_dispose(&got);

    char over[2049];
    memset(over, 'a', sizeof over);
    cf_cached_body small;
    CF_REQUIRE(cache_body_make("s", false, "s", &small) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, cache_span_bytes(over, 2049), 1,
                          &small) == CF_LIMIT);
    cf_cached_body_dispose(&small);
    CF_REQUIRE(cf_cache_stats_get(env.cache, &st) == CF_OK);
    CF_CHECK(st.bypasses == 2 && st.entries == 1);
    cache_env_close(&env);
}

/* 2048-byte keys are accepted, 2049-byte keys bypass and can never be a hit;
 * the empty key is a valid key and is accounted at zero key capacity. */
CF_TEST(cache_key_cap_2048_and_empty_key) {
    cache_env env;
    cache_env_open(&env, 1, CF_CACHE_DEFAULT_BUDGET_BYTES);
    char *key = malloc(2048);
    CF_REQUIRE(key != NULL);
    memset(key, 'a', 2048);
    char over[2049];
    memset(over, 'b', sizeof over);

    cf_cached_body body, got;
    CF_REQUIRE(cache_body_make("x", false, "cap", &body) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, cache_span_bytes(key, 2048), 1,
                          &body) == CF_OK);
    CF_CHECK(cf_cache_get(env.cache, cache_span_bytes(key, 2048), &got) ==
             CF_OK);
    CF_CHECK(cache_buf_is(got.body, "x"));
    cf_cached_body_dispose(&got);
    CF_CHECK(cf_cache_put(env.cache, cache_span_bytes(over, 2049), 1,
                          &body) == CF_LIMIT);
    CF_CHECK(cf_cache_get(env.cache, cache_span_bytes(over, 2049), &got) ==
             CF_NOT_FOUND);
    CF_CHECK(got.body == NULL);
    cf_cached_body_dispose(&body);

    CF_REQUIRE(cache_body_make("e", false, "empty", &body) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, (cf_span){NULL, 0}, 1, &body) == CF_OK);
    CF_CHECK(cf_cache_get(env.cache, (cf_span){NULL, 0}, &got) == CF_OK);
    CF_CHECK(cache_buf_is(got.body, "e"));
    cf_cached_body_dispose(&got);
    cf_cached_body_dispose(&body);

    cf_cache_stats st;
    CF_REQUIRE(cf_cache_stats_get(env.cache, &st) == CF_OK);
    CF_CHECK(st.entries == 2);
    CF_CHECK(st.key_bytes == 2048);
    CF_CHECK(st.misses == 1); /* the over-cap get */
    CF_CHECK(st.bypasses == 1);
    free(key);
    cache_env_close(&env);
}

/* FIFO order across evictions, and an evicted body stays readable while an
 * in-flight owner retains it (CORE-02). */
CF_TEST(cache_fifo_eviction_order_and_retained_body) {
    size_t cost = cf_cache_entry_bytes(2, 2);
    CF_CHECK(cost == META + 2 + HDR + 2);
    CF_CHECK(cost == 156);
    cache_env env;
    cache_env_open(&env, 1, CF_CACHE_BUCKET_BYTES + 3 * cost);
    const char *keys[5] = {"k0", "k1", "k2", "k3", "k4"};
    const char *vals[5] = {"aa", "bb", "cc", "dd", "ee"};
    cf_cached_body body, got;
    for (int i = 0; i < 3; i++) {
        CF_REQUIRE(cache_body_make(vals[i], false, keys[i], &body) == CF_OK);
        CF_CHECK(cf_cache_put(env.cache, cache_span(keys[i]), 1, &body) ==
                 CF_OK);
        cf_cached_body_dispose(&body);
    }
    cf_cache_stats st;
    CF_REQUIRE(cf_cache_stats_get(env.cache, &st) == CF_OK);
    CF_CHECK(st.entries == 3);
    CF_CHECK(st.bytes_used == st.budget_bytes);

    /* hold k0's body like an in-flight send, then evict it */
    cf_cached_body held;
    CF_CHECK(cf_cache_get(env.cache, cache_span("k0"), &held) == CF_OK);
    CF_CHECK(cache_buf_is(held.body, "aa"));
    CF_REQUIRE(cache_body_make(vals[3], false, keys[3], &body) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, cache_span(keys[3]), 1, &body) == CF_OK);
    cf_cached_body_dispose(&body);
    CF_REQUIRE(cf_cache_stats_get(env.cache, &st) == CF_OK);
    CHECK_STATS(st, 3, 1, 0, 0, 0, 1, 0, 0);
    CF_CHECK(st.bytes_used == st.budget_bytes);
    CF_CHECK(cf_cache_get(env.cache, cache_span("k0"), &got) == CF_NOT_FOUND);
    CF_CHECK(cf_cache_get(env.cache, cache_span("k1"), &got) == CF_OK);
    CF_CHECK(cache_buf_is(got.body, "bb"));
    cf_cached_body_dispose(&got);
    CF_CHECK(cf_cache_get(env.cache, cache_span("k2"), &got) == CF_OK);
    CF_CHECK(cache_buf_is(got.body, "cc"));
    cf_cached_body_dispose(&got);
    CF_CHECK(cf_cache_get(env.cache, cache_span("k3"), &got) == CF_OK);
    CF_CHECK(cache_buf_is(got.body, "dd"));
    cf_cached_body_dispose(&got);
    /* the eviction released only the cache's reference */
    CF_CHECK(cache_buf_is(held.body, "aa"));
    cf_cached_body_dispose(&held);

    /* the next insertion takes k1, the new FIFO head */
    CF_REQUIRE(cache_body_make(vals[4], false, keys[4], &body) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, cache_span(keys[4]), 1, &body) == CF_OK);
    cf_cached_body_dispose(&body);
    CF_REQUIRE(cf_cache_stats_get(env.cache, &st) == CF_OK);
    CF_CHECK(st.entries == 3 && st.evictions == 2);
    CF_CHECK(cf_cache_get(env.cache, cache_span("k1"), &got) == CF_NOT_FOUND);
    CF_CHECK(cf_cache_get(env.cache, cache_span("k2"), &got) == CF_OK);
    cf_cached_body_dispose(&got);
    CF_CHECK(cf_cache_get(env.cache, cache_span("k3"), &got) == CF_OK);
    cf_cached_body_dispose(&got);
    CF_CHECK(cf_cache_get(env.cache, cache_span("k4"), &got) == CF_OK);
    CF_CHECK(cache_buf_is(got.body, "ee"));
    cf_cached_body_dispose(&got);
    cache_env_close(&env);
}

/* Every counter, exact, across one scripted sequence: hit, miss, stale,
 * version reject, duplicate, bypass and FIFO eviction in order. */
CF_TEST(cache_counters_exact_scripted_sequence) {
    size_t cost = cf_cache_entry_bytes(1, 1);
    CF_CHECK(cost == META + 1 + HDR + 1);
    CF_CHECK(cost == 154);
    cache_env env;
    cache_env_open(&env, 1, CF_CACHE_BUCKET_BYTES + 3 * cost);
    cf_cache_stats s;
    CF_REQUIRE(cf_cache_stats_get(env.cache, &s) == CF_OK);
    CHECK_STATS(s, 0, 0, 0, 0, 0, 0, 0, 0);
    CF_CHECK(s.bytes_used == BUCKET);

    cf_cached_body body, got;
    CF_REQUIRE(cache_body_make("x", false, "x", &body) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, cache_span("a"), 1, &body) == CF_OK);
    CF_REQUIRE(cf_cache_stats_get(env.cache, &s) == CF_OK);
    CHECK_STATS(s, 1, 0, 0, 0, 0, 0, 0, 0);

    CF_CHECK(cf_cache_get(env.cache, cache_span("a"), &got) == CF_OK);
    cf_cached_body_dispose(&got);
    CF_REQUIRE(cf_cache_stats_get(env.cache, &s) == CF_OK);
    CHECK_STATS(s, 1, 1, 0, 0, 0, 0, 0, 0);

    CF_CHECK(cf_cache_get(env.cache, cache_span("b"), &got) == CF_NOT_FOUND);
    CF_REQUIRE(cf_cache_stats_get(env.cache, &s) == CF_OK);
    CHECK_STATS(s, 1, 1, 1, 0, 0, 0, 0, 0);

    CF_CHECK(cf_cache_put(env.cache, cache_span("b"), 1, &body) == CF_OK);
    CF_REQUIRE(cf_cache_stats_get(env.cache, &s) == CF_OK);
    CHECK_STATS(s, 2, 1, 1, 0, 0, 0, 0, 0);

    cache_version_advance(&env.version); /* 2 */
    CF_CHECK(cf_cache_get(env.cache, cache_span("a"), &got) == CF_NOT_FOUND);
    CF_REQUIRE(cf_cache_stats_get(env.cache, &s) == CF_OK);
    CHECK_STATS(s, 1, 1, 2, 0, 1, 0, 0, 0);

    CF_CHECK(cf_cache_put(env.cache, cache_span("c"), 1, &body) == CF_BUSY);
    CF_REQUIRE(cf_cache_stats_get(env.cache, &s) == CF_OK);
    CHECK_STATS(s, 1, 1, 2, 0, 1, 0, 1, 0);

    CF_CHECK(cf_cache_put(env.cache, cache_span("c"), 2, &body) == CF_OK);
    CF_REQUIRE(cf_cache_stats_get(env.cache, &s) == CF_OK);
    CHECK_STATS(s, 2, 1, 2, 0, 1, 0, 1, 0);

    /* "c" is live at the current version: a second admission is a duplicate */
    CF_CHECK(cf_cache_put(env.cache, cache_span("c"), 2, &body) == CF_BUSY);
    CF_REQUIRE(cf_cache_stats_get(env.cache, &s) == CF_OK);
    CHECK_STATS(s, 2, 1, 2, 0, 1, 0, 1, 1);

    char over[2049];
    memset(over, 'o', sizeof over);
    CF_CHECK(cf_cache_put(env.cache, cache_span_bytes(over, 2049), 2, &body) ==
             CF_LIMIT);
    CF_REQUIRE(cf_cache_stats_get(env.cache, &s) == CF_OK);
    CHECK_STATS(s, 2, 1, 2, 1, 1, 0, 1, 1);
    cf_cached_body_dispose(&body); /* done with "x" before reusing body */

    CF_REQUIRE(cache_body_make("z", false, "z", &body) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, cache_span("e"), 2, &body) == CF_OK);
    cf_cached_body_dispose(&body);
    CF_REQUIRE(cf_cache_stats_get(env.cache, &s) == CF_OK);
    CHECK_STATS(s, 3, 1, 2, 1, 1, 0, 1, 1);
    CF_CHECK(s.bytes_used == BUCKET + 3 * cost);

    CF_REQUIRE(cache_body_make("w", false, "w", &body) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, cache_span("f"), 2, &body) == CF_OK);
    cf_cached_body_dispose(&body);
    CF_REQUIRE(cf_cache_stats_get(env.cache, &s) == CF_OK);
    CHECK_STATS(s, 3, 1, 2, 1, 1, 1, 1, 1); /* "b" (oldest) was evicted */
    CF_CHECK(s.bytes_used == BUCKET + 3 * cost);
    CF_CHECK(cf_cache_get(env.cache, cache_span("b"), &got) == CF_NOT_FOUND);
    CF_REQUIRE(cf_cache_stats_get(env.cache, &s) == CF_OK);
    CHECK_STATS(s, 3, 1, 3, 1, 1, 1, 1, 1);
    cache_env_close(&env);
}

/* Destroy with live entries releases every body reference; NULL and disabled
 * caches destroy too. The ASan run is the leak probe. */
CF_TEST(cache_destroy_with_entries_releases_all) {
    cache_env env;
    cache_env_open(&env, 1, CF_CACHE_DEFAULT_BUDGET_BYTES);
    for (int i = 0; i < 64; i++) {
        char key[16], payload[16];
        snprintf(key, sizeof key, "d%03d", i);
        snprintf(payload, sizeof payload, "body-%03d", i);
        cf_cached_body body;
        CF_REQUIRE(cache_body_make(payload, i % 2 == 0, key, &body) == CF_OK);
        CF_CHECK(cf_cache_put(env.cache, cache_span(key), 1, &body) == CF_OK);
        cf_cached_body_dispose(&body);
    }
    cf_cache_stats st;
    CF_REQUIRE(cf_cache_stats_get(env.cache, &st) == CF_OK);
    CF_CHECK(st.entries == 64);
    /* a hit's reference is owned by the caller and outlives the cache */
    cf_cached_body held;
    CF_CHECK(cf_cache_get(env.cache, cache_span("d000"), &held) == CF_OK);
    cache_env_close(&env);
    CF_CHECK(cache_buf_is(held.body, "body-000"));
    cf_cached_body_dispose(&held);

    cf_cache_destroy(NULL);

    cache_version_fixture version;
    cache_version_fixture_init(&version, 1);
    cf_cache *disabled = NULL;
    CF_REQUIRE(cf_cache_create(0, cache_version_reader, &version,
                               &version.mutex, &disabled) == CF_OK);
    cf_cache_destroy(disabled);
    cache_version_fixture_dispose(&version);
}

/* An enabled cache needs entropy; a disabled one does not. */
CF_TEST(cache_create_entropy_failure_and_disabled_creation) {
    cache_version_fixture version;
    cache_version_fixture_init(&version, 1);
    cf_cache *cache = (cf_cache *)0x1; /* must be reset even on failure */
    cf_test_random_fail();
    cf_err rc = cf_cache_create(CF_CACHE_DEFAULT_BUDGET_BYTES,
                                cache_version_reader, &version,
                                &version.mutex, &cache);
    cf_test_random_clear();
    CF_CHECK(rc == CF_IO);
    CF_CHECK(cache == NULL);

    cache = NULL;
    cf_test_random_fail();
    rc = cf_cache_create(0, cache_version_reader, &version, &version.mutex,
                         &cache);
    cf_test_random_clear();
    CF_CHECK(rc == CF_OK);
    CF_CHECK(cache != NULL);
    cf_cache_destroy(cache);
    cache_version_fixture_dispose(&version);
}

/* Invalid arguments and overflow-safe cost arithmetic. */
CF_TEST(cache_invalid_arguments_and_cost_overflow) {
    CF_CHECK(cf_cache_entry_bytes(SIZE_MAX, 0) == SIZE_MAX);
    CF_CHECK(cf_cache_entry_bytes(0, SIZE_MAX) == SIZE_MAX);
    CF_CHECK(cf_cache_entry_bytes(10, 20) == META + 10 + HDR + 20);

    cf_cache_stats st;
    CF_CHECK(cf_cache_stats_get(NULL, &st) == CF_INVALID);
    cache_env env;
    cache_env_open(&env, 1, CF_CACHE_DEFAULT_BUDGET_BYTES);
    CF_CHECK(cf_cache_stats_get(env.cache, NULL) == CF_INVALID);

    cf_cached_body out;
    CF_CHECK(cf_cache_get(NULL, cache_span("k"), &out) == CF_INVALID);
    CF_CHECK(cf_cache_get(env.cache, cache_span("k"), NULL) == CF_INVALID);
    const unsigned char byte = 0;
    CF_CHECK(cf_cache_get(env.cache, (cf_span){&byte, 0}, &out) ==
             CF_NOT_FOUND);
    CF_CHECK(cf_cache_get(env.cache, (cf_span){NULL, 3}, &out) == CF_INVALID);
    cf_cached_body empty;
    memset(&empty, 0, sizeof empty);
    CF_CHECK(cf_cache_put(env.cache, cache_span("k"), 1, &empty) ==
             CF_INVALID);
    CF_CHECK(cf_cache_put(env.cache, cache_span("k"), 1, NULL) == CF_INVALID);
    CF_CHECK(cf_cache_put(NULL, cache_span("k"), 1, &empty) == CF_INVALID);

    cf_cached_body body;
    CF_REQUIRE(cache_body_make("v", false, "v", &body) == CF_OK);
    CF_CHECK(cf_cache_put(env.cache, (cf_span){NULL, 3}, 1, &body) ==
             CF_INVALID);
    CF_CHECK(cf_cache_put(env.cache, (cf_span){&byte, 0}, 1, &body) == CF_OK);
    cf_cached_body_dispose(&body);
    cf_cached_body_dispose(NULL);
    cache_env_close(&env);
}

CF_TEST_MAIN()
