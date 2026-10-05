/* Minimal standalone check harness for F01 core tests (F02 owns the shared
 * test runner; these tests must not depend on it). */
#ifndef CF_TESTS_CORE_CHECK_H
#define CF_TESTS_CORE_CHECK_H

#include <stdio.h>

static int cf_test_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            cf_test_failures++;                                              \
        }                                                                    \
    } while (0)

static inline int check_summary(const char *name) {
    if (cf_test_failures != 0) {
        fprintf(stderr, "%s: %d failure(s)\n", name, cf_test_failures);
        return 1;
    }
    printf("%s: ok\n", name);
    return 0;
}

#endif
