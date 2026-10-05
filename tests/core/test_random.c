/* F01 random tests: real OS entropy plus deterministic injection through
 * src/core/testrandom.h. */
#include "cf.h"

#include "core/testrandom.h"

#include "check.h"

#include <string.h>

int main(void) {
    unsigned char buf[64];

    CHECK(cf_random_bytes(buf, 0) == CF_OK); /* zero length is a no-op */
    CHECK(cf_random_bytes(NULL, 0) == CF_OK);
    CHECK(cf_random_bytes(NULL, 8) == CF_INVALID);

    cf_test_random_fill(0xAB);
    memset(buf, 0x11, sizeof buf);
    CHECK(cf_random_bytes(buf, sizeof buf) == CF_OK);
    for (size_t i = 0; i < sizeof buf; i++) {
        CHECK(buf[i] == 0xAB);
    }

    cf_test_random_fail();
    memset(buf, 0x5A, sizeof buf);
    CHECK(cf_random_bytes(buf, sizeof buf) == CF_IO);
    for (size_t i = 0; i < sizeof buf; i++) {
        CHECK(buf[i] == 0x00); /* failure zeroes the output, no partial entropy */
    }

    cf_test_random_clear();
    unsigned char real_a[32];
    unsigned char real_b[32];
    CHECK(cf_random_bytes(real_a, sizeof real_a) == CF_OK);
    CHECK(cf_random_bytes(real_b, sizeof real_b) == CF_OK);
    cf_test_random_clear();
    return check_summary("test_random");
}
