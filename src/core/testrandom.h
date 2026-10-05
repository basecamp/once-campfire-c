/* Test-only injection for src/core/random.c.
 *
 * Only tests in tests/core include this header; application code always uses
 * the OS entropy source. Calling cf_test_random_clear() (or never calling a
 * setter) leaves production behavior unchanged. */
#ifndef CF_CORE_TESTRANDOM_H
#define CF_CORE_TESTRANDOM_H

/* While set, every byte produced by cf_random_bytes() is this value. */
void cf_test_random_fill(unsigned char byte);

/* While set, cf_random_bytes() returns CF_IO without reading the OS source
 * (the output buffer is zeroed). */
void cf_test_random_fail(void);

/* Restore the real getrandom(2) / /dev/urandom source. */
void cf_test_random_clear(void);

#endif
