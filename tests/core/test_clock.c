/* F01 clock tests: real clocks plus deterministic injection through
 * src/core/testclock.h (no environment-variable switching). */
#include "cf.h"

#include "core/testclock.h"

#include "check.h"

#include <stdint.h>

int main(void) {
    cf_test_clock_clear();
    int64_t before = cf_now_us(NULL);
    CHECK(before >= INT64_C(1600000000000000)); /* >= 2020-09-13 */
    CHECK(before <= INT64_C(4102444800000000)); /* <= 2100-01-01 */

    const int64_t fixed_us = INT64_C(1754300000000000);
    cf_test_clock_set_fixed_us(fixed_us);
    CHECK(cf_now_us(NULL) == fixed_us);
    CHECK(cf_now_us(NULL) == fixed_us); /* deterministic across calls */
    CHECK(cf_monotonic_ms() > 0);      /* now_us injection left monotonic real */

    cf_test_clock_set_monotonic_ms(UINT64_C(424242));
    CHECK(cf_monotonic_ms() == UINT64_C(424242));
    CHECK(cf_now_us(NULL) == fixed_us);

    cf_test_clock_clear();
    int64_t after = cf_now_us(NULL);
    CHECK(after >= before);
    CHECK(after <= INT64_C(4102444800000000));
    uint64_t m1 = cf_monotonic_ms();
    uint64_t m2 = cf_monotonic_ms();
    CHECK(m2 >= m1);
    CHECK(m1 > 0);
    return check_summary("test_clock");
}
