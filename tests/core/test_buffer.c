/* F01 core ownership tests.
 * CORE-01: checked growth rejects overflow; allocation failures leave outputs
 *          empty and leak-free.
 * CORE-02: retained buffer survives cache eviction and partial send; last
 *          release frees once.
 * Allocation-failure injection uses the core-private hook installed through
 * src/core/alloc.h (defaults to malloc/realloc/free). */
#include "cf.h"

#include "core/alloc.h"

#include "check.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TRACK_MAX 512

static void *tracked_ptr[TRACK_MAX];
static unsigned char tracked_live[TRACK_MAX];
static size_t tracked_live_count;
static int64_t alloc_calls;
static int64_t realloc_calls;
static int64_t free_calls;
static int64_t double_free_count;
static int64_t track_failures;
static int64_t fail_countdown = -1;

static int track_find(void *p) {
    for (int i = 0; i < TRACK_MAX; i++) {
        if (tracked_live[i] != 0 && tracked_ptr[i] == p) return i;
    }
    return -1;
}

static int track_slot(void *p) {
    for (int i = 0; i < TRACK_MAX; i++) {
        if (tracked_live[i] == 0) {
            tracked_live[i] = 1;
            tracked_ptr[i] = p;
            tracked_live_count++;
            return i;
        }
    }
    return -1;
}

static int should_fail(void) {
    if (fail_countdown < 0) return 0;
    if (fail_countdown == 0) return 1;
    fail_countdown--;
    return 0;
}

static void *track_alloc(size_t size) {
    if (should_fail()) return NULL;
    alloc_calls++;
    void *p = malloc(size);
    if (p != NULL && track_slot(p) < 0) track_failures++;
    return p;
}

static void *track_realloc(void *ptr, size_t size) {
    if (should_fail()) return NULL;
    realloc_calls++;
    if (ptr == NULL) {
        void *p = realloc(NULL, size);
        if (p != NULL && track_slot(p) < 0) track_failures++;
        return p;
    }
    int i = track_find(ptr);
    void *p = realloc(ptr, size);
    if (p == NULL) return NULL; /* original block untouched */
    if (i >= 0) {
        tracked_ptr[i] = p;
    } else if (track_slot(p) < 0) {
        track_failures++;
    }
    return p;
}

static void track_free(void *ptr) {
    if (ptr == NULL) return;
    free_calls++;
    int i = track_find(ptr);
    if (i < 0) {
        double_free_count++;
    } else {
        tracked_live[i] = 0;
        tracked_ptr[i] = NULL;
        tracked_live_count--;
    }
    free(ptr);
}

static void track_install(void) {
    fail_countdown = -1;
    cf_core_set_allocator(track_alloc, track_realloc, track_free);
}

static void track_reset(void) {
    fail_countdown = -1;
    cf_core_reset_allocator();
}

static void track_fail_next(void) { fail_countdown = 0; }

static void track_fail_off(void) { fail_countdown = -1; }

static cf_span span_of(const char *s) {
    cf_span span;
    span.ptr = (const unsigned char *)s;
    span.len = strlen(s);
    return span;
}

static void test_builder_basics(void) {
    cf_builder b = {NULL, 0, 0};
    cf_builder_dispose(&b); /* empty dispose is a no-op */
    cf_builder_dispose(NULL);
    CHECK(cf_builder_append(NULL, span_of("x")) == CF_INVALID);
    CHECK(cf_builder_append(&b, (cf_span){NULL, (size_t)1}) == CF_INVALID);
    CHECK(cf_builder_append(&b, (cf_span){NULL, (size_t)0}) == CF_OK);

    track_install();
    CHECK(cf_builder_append(&b, span_of("hello")) == CF_OK);
    CHECK(cf_builder_append(&b, span_of(" ")) == CF_OK);
    CHECK(cf_builder_append(&b, span_of("world")) == CF_OK);
    CHECK(b.len == 11);
    CHECK(b.ptr != NULL && b.cap >= b.len);

    cf_buf *buf = NULL;
    CHECK(cf_builder_freeze(&b, &buf) == CF_OK);
    CHECK(buf != NULL);
    CHECK(b.ptr == NULL && b.len == 0 && b.cap == 0); /* consumed on OK */
    cf_span s = cf_buf_span(buf);
    CHECK(s.len == 11 && s.ptr != NULL && memcmp(s.ptr, "hello world", 11) == 0);
    cf_buf_release(buf);
    CHECK(tracked_live_count == 0);

    cf_builder_dispose(&b);
    track_reset();
}

static void test_checked_growth_overflow(void) {
    cf_builder b = {NULL, 0, 0};
    b.len = SIZE_MAX;
    CHECK(cf_builder_append(&b, span_of("x")) == CF_LIMIT);
    CHECK(b.ptr == NULL && b.len == SIZE_MAX); /* unchanged, nothing allocated */
    b.len = SIZE_MAX - 3;
    CHECK(cf_builder_append(&b, span_of("abcd")) == CF_LIMIT);
    cf_builder_dispose(&b);

    cf_builder f = {NULL, SIZE_MAX, SIZE_MAX};
    cf_buf *out = (cf_buf *)(void *)&tracked_live; /* sentinel, must be cleared */
    CHECK(cf_builder_freeze(&f, &out) == CF_LIMIT);
    CHECK(out == NULL && f.ptr == NULL && f.len == SIZE_MAX);
    cf_builder_dispose(&f);

    cf_buf *sentinel = (cf_buf *)(void *)&tracked_live;
    CHECK(cf_buf_copy((cf_span){NULL, (size_t)4}, &sentinel) == CF_INVALID);
    CHECK(sentinel == NULL);
    CHECK(cf_buf_copy((cf_span){(const unsigned char *)"x", SIZE_MAX}, &sentinel) ==
          CF_LIMIT); /* size overflow rejected before any allocation */
    CHECK(sentinel == NULL);
    CHECK(cf_buf_copy(span_of("x"), NULL) == CF_INVALID);
    CHECK(cf_builder_freeze(&f, NULL) == CF_INVALID);
}

static void test_dispose_resets_nonempty(void) {
    track_install();
    cf_builder b = {NULL, 0, 0};
    CHECK(cf_builder_append(&b, span_of("abc")) == CF_OK);
    CHECK(b.ptr != NULL && b.len == 3);
    cf_builder_dispose(&b); /* releases the allocation and resets state */
    CHECK(b.ptr == NULL && b.len == 0 && b.cap == 0);
    CHECK(tracked_live_count == 0);
    track_reset();
}

static void test_copy_empty_is_valid(void) {
    track_install();
    cf_buf *buf = NULL;
    CHECK(cf_buf_copy((cf_span){NULL, (size_t)0}, &buf) == CF_OK);
    CHECK(buf != NULL);
    CHECK(cf_buf_span(buf).len == 0);
    cf_buf_release(buf);
    CHECK(tracked_live_count == 0);
    track_reset();
}

static void test_nomem_leaves_out_empty(void) {
    track_install();
    track_fail_next();
    cf_buf *out = (cf_buf *)(void *)&tracked_live; /* sentinel */
    CHECK(cf_buf_copy(span_of("data"), &out) == CF_NOMEM);
    CHECK(out == NULL);
    CHECK(tracked_live_count == 0);
    track_reset();
}

static void test_nomem_builder_still_usable(void) {
    static const unsigned char big[200] = {1};
    track_install();
    cf_builder b = {NULL, 0, 0};
    CHECK(cf_builder_append(&b, span_of("abc")) == CF_OK);
    unsigned char *ptr_before = b.ptr;
    size_t cap_before = b.cap;

    track_fail_next();
    CHECK(cf_builder_append(&b, (cf_span){big, sizeof big}) == CF_NOMEM);
    CHECK(b.ptr == ptr_before && b.len == 3 && b.cap == cap_before);
    CHECK(memcmp(b.ptr, "abc", 3) == 0);

    cf_buf *out = NULL;
    track_fail_next();
    CHECK(cf_builder_freeze(&b, &out) == CF_NOMEM);
    CHECK(out == NULL);
    CHECK(b.ptr == ptr_before && b.len == 3 && memcmp(b.ptr, "abc", 3) == 0);

    track_fail_off(); /* builder, tracked block and buffer stay on one allocator */
    CHECK(cf_builder_append(&b, span_of("def")) == CF_OK);
    CHECK(cf_builder_freeze(&b, &out) == CF_OK);
    cf_span s = cf_buf_span(out);
    CHECK(s.len == 6 && s.ptr != NULL && memcmp(s.ptr, "abcdef", 6) == 0);
    cf_buf_release(out);
    cf_builder_dispose(&b);
    CHECK(tracked_live_count == 0);
    track_reset();
}

static void test_retained_survives_eviction_and_partial_send(void) {
    static const char payload[] =
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    const size_t n = sizeof payload - 1;

    track_install();
    cf_buf *cache = NULL;
    CHECK(cf_buf_copy((cf_span){(const unsigned char *)payload, n}, &cache) ==
          CF_OK);
    CHECK(tracked_live_count == 1);

    cf_buf *sender = cf_buf_retain(cache); /* pending send takes a reference */
    CHECK(sender == cache);
    cf_buf_release(cache); /* cache eviction drops its reference */

    cf_span live = cf_buf_span(sender);
    CHECK(live.len == n && live.ptr != NULL && memcmp(live.ptr, payload, n) == 0);

    int64_t frees_before = free_calls;
    const size_t chunk = n / 3;
    for (size_t off = 0; off < n; off += chunk) {
        size_t take = n - off < chunk ? n - off : chunk;
        CHECK(memcmp(live.ptr + off, payload + off, take) == 0); /* partial send */
    }
    CHECK(free_calls == frees_before); /* nothing freed mid-send */

    cf_buf_release(sender); /* last release frees exactly once */
    CHECK(free_calls == frees_before + 1);
    CHECK(tracked_live_count == 0);
    cf_buf_release(NULL);
    track_reset();
}

static void test_retains_free_once(void) {
    track_install();
    cf_buf *a = NULL;
    CHECK(cf_buf_copy(span_of("x"), &a) == CF_OK);
    cf_buf *b = cf_buf_retain(a);
    cf_buf *c = cf_buf_retain(b);
    int64_t frees_before = free_calls;
    cf_buf_release(a);
    cf_buf_release(b);
    CHECK(free_calls == frees_before); /* c still owns a reference */
    cf_span s = cf_buf_span(c);
    CHECK(s.len == 1 && s.ptr != NULL && s.ptr[0] == 'x');
    cf_buf_release(c);
    CHECK(free_calls == frees_before + 1); /* freed once, on the last release */
    CHECK(tracked_live_count == 0);
    CHECK(cf_buf_retain(NULL) == NULL);
    track_reset();
}

struct thread_arg {
    cf_buf *buf;
    long iters;
};

static void *retain_release_worker(void *p) {
    struct thread_arg *arg = p;
    for (long i = 0; i < arg->iters; i++) {
        cf_buf *r = cf_buf_retain(arg->buf);
        if (r != arg->buf) abort();
        cf_buf_release(r);
    }
    return NULL;
}

static void test_threaded_refcount(void) {
    enum { THREADS = 4 };
    track_install();
    cf_buf *buf = NULL;
    CHECK(cf_buf_copy(span_of("threaded"), &buf) == CF_OK);
    pthread_t tids[THREADS];
    struct thread_arg arg = {buf, 20000};
    for (int i = 0; i < THREADS; i++) {
        CHECK(pthread_create(&tids[i], NULL, retain_release_worker, &arg) == 0);
    }
    for (int i = 0; i < THREADS; i++) {
        CHECK(pthread_join(tids[i], NULL) == 0);
    }
    CHECK(cf_buf_span(buf).len == 8);
    cf_buf_release(buf); /* last release after all workers joined */
    CHECK(tracked_live_count == 0);
    track_reset();
}

int main(void) {
    test_builder_basics();
    test_checked_growth_overflow();
    test_dispose_resets_nonempty();
    test_copy_empty_is_valid();
    test_nomem_leaves_out_empty();
    test_nomem_builder_still_usable();
    test_retained_survives_eviction_and_partial_send();
    test_retains_free_once();
    test_threaded_refcount();
    CHECK(double_free_count == 0);
    CHECK(track_failures == 0);
    CHECK(alloc_calls > 0);   /* both allocation paths were exercised */
    CHECK(realloc_calls > 0);
    return check_summary("test_buffer");
}
