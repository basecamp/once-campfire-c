/* Clocks: cf_now_us (UTC microseconds since epoch) and cf_monotonic_ms
 * (CLOCK_MONOTONIC milliseconds). See cf.h and 01-foundation-http.md
 * "Configuration: fixed defaults": monotonic time is for deadlines, UTC
 * microseconds for application data; tests inject clocks through
 * core/testclock.h instead of production shortcuts. */
#include "cf.h"

#include "core/testclock.h"

#include <stdatomic.h>
#include <time.h>

#define CF_CLOCK_UNFIXED_US INT64_MIN
#define CF_CLOCK_UNFIXED_MS UINT64_MAX

static _Atomic int64_t cf_test_fixed_us = CF_CLOCK_UNFIXED_US;
static _Atomic uint64_t cf_test_fixed_ms = CF_CLOCK_UNFIXED_MS;

int64_t cf_now_us(const cf_app *app) {
    (void)app; /* the process clock is global; app is borrowed for API shape */
    int64_t fixed = atomic_load_explicit(&cf_test_fixed_us, memory_order_relaxed);
    if (fixed != CF_CLOCK_UNFIXED_US) return fixed;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * INT64_C(1000000) +
           (int64_t)ts.tv_nsec / INT64_C(1000);
}

uint64_t cf_monotonic_ms(void) {
    uint64_t fixed = atomic_load_explicit(&cf_test_fixed_ms, memory_order_relaxed);
    if (fixed != CF_CLOCK_UNFIXED_MS) return fixed;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000) +
           (uint64_t)ts.tv_nsec / UINT64_C(1000000);
}

void cf_test_clock_set_fixed_us(int64_t us) {
    atomic_store_explicit(&cf_test_fixed_us, us, memory_order_relaxed);
}

void cf_test_clock_set_monotonic_ms(uint64_t ms) {
    atomic_store_explicit(&cf_test_fixed_ms, ms, memory_order_relaxed);
}

void cf_test_clock_clear(void) {
    atomic_store_explicit(&cf_test_fixed_us, CF_CLOCK_UNFIXED_US,
                          memory_order_relaxed);
    atomic_store_explicit(&cf_test_fixed_ms, CF_CLOCK_UNFIXED_MS,
                          memory_order_relaxed);
}
