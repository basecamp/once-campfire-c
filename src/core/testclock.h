/* Test-only injection for src/core/clock.c.
 *
 * Only tests in tests/core include this header; application code always uses
 * the real clocks. There is no environment-variable switching. Calling
 * cf_test_clock_clear() (or never calling a setter) leaves production
 * behavior unchanged. */
#ifndef CF_CORE_TESTCLOCK_H
#define CF_CORE_TESTCLOCK_H

#include <stdint.h>

/* While set, cf_now_us() returns exactly this value. */
void cf_test_clock_set_fixed_us(int64_t us);

/* While set, cf_monotonic_ms() returns exactly this value. */
void cf_test_clock_set_monotonic_ms(uint64_t ms);

/* Restore both clocks to the real CLOCK_REALTIME / CLOCK_MONOTONIC. */
void cf_test_clock_clear(void);

#endif
