/* Minimal C test framework for the Campfire port (F02).
 *
 * Header-only, no external dependencies, Linux x86_64 + C11 + clang and the
 * pinned Fil-C 0.685 pizfix driver (vendor/DEPS.json / vendor/README.md).
 *
 * Usage
 * -----
 *   #include "cf_test.h"
 *
 *   CF_TEST(params_basic_forms) {
 *       CF_CHECK(1 + 1 == 2);
 *       CF_REQUIRE(ptr != NULL);
 *       CF_CHECK(ptr->len == 4);
 *   }
 *
 *   CF_TEST_MAIN()          // exactly once per test binary
 *
 * Each CF_TEST registers a case from its own translation unit, so a test
 * binary may spread cases over several .c files as long as each includes this
 * header and exactly one file defines CF_TEST_MAIN().
 *
 * Registration
 * ------------
 * CF_TEST registers a case through two mechanisms, and CF_TEST_MAIN() runs
 * each case exactly once:
 *   1. the case lands in the "cf_test_cases" section, so an ordinary clang
 *      link synthesizes __start_cf_test_cases/__stop_cf_test_cases; and
 *   2. a constructor calls cf_test_register(), appending the case to a linked
 *      list before main().
 * CF_TEST_MAIN() prefers the section table whenever it is non-empty
 * (ordinary clang) and otherwise uses the constructor list.  The fallback is
 * required by Fil-C 0.685, whose compiler drops custom section attributes:
 * its binaries have no section table and would otherwise register zero
 * cases.  A binary with cases but no CF_TEST_MAIN() fails to link under both
 * compilers, because each constructor references cf_test_register(), which
 * only the CF_TEST_MAIN() translation unit defines.
 *
 * Runner
 * ------
 *   ./test_binary            run every registered case
 *   ./test_binary <filter>   run cases whose name equals the filter or
 *                            contains it as a substring
 *
 * The runner prints one verdict line per case ("PASS <name>" / "FAIL <name>"),
 * failure details with file:line, a summary, and exits nonzero on failure
 * (1), on a filter that matches no case (2), or when no cases are registered
 * (2).  A case that does not run is not reported as passing.
 *
 * Assertions
 * ----------
 *   CF_CHECK(cond)    record a failure and continue the case
 *   CF_REQUIRE(cond)  record a failure and abort the case
 *
 * Assertions are for the thread running the case; worker threads should
 * synchronize first and assert from the case thread.
 */

#ifndef CF_TEST_H
#define CF_TEST_H

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*cf_test_fn)(void);

struct cf_test_case {
    const char *name;
    cf_test_fn fn;
};

/* Constructor-registration list node; used only by CF_TEST_MAIN(). */
struct cf_test_node {
    const struct cf_test_case *tc;
    struct cf_test_node *next;
};

/* Cases land in the "cf_test_cases" output section (ordinary clang/gcc) and
 * register through a constructor (also under Fil-C 0.685, which drops custom
 * sections).  The weak __start_/__stop_ declarations keep a binary with no
 * case section linkable: the runner then uses the constructor list, and an
 * empty constructor list is reported as no cases (exit 2). */
#define CF_TEST(name)                                                        \
    static void cf_test_##name(void);                                        \
    static const struct cf_test_case cf_test_case_##name                     \
        __attribute__((used, section("cf_test_cases"))) = {                  \
            #name, cf_test_##name};                                          \
    static void cf_test_ctor_##name(void) __attribute__((constructor));      \
    static void cf_test_ctor_##name(void) {                                  \
        cf_test_register(&cf_test_case_##name);                              \
    }                                                                        \
    static void cf_test_##name(void)

extern const struct cf_test_case __start_cf_test_cases[] __attribute__((weak));
extern const struct cf_test_case __stop_cf_test_cases[] __attribute__((weak));

/* Defined by the CF_TEST_MAIN() translation unit and called by every CF_TEST
 * constructor before main().  It is undefined in a binary with cases but no
 * CF_TEST_MAIN(), which is therefore a link error. */
extern void cf_test_register(const struct cf_test_case *tc);

/* Shared runner state, defined by the CF_TEST_MAIN() translation unit. */
extern int cf_test_failed_checks;
extern int cf_test_aborting;
extern jmp_buf cf_test_abort;
extern void cf_test_note_failure(const char *file, int line, const char *expr, int fatal);
extern int cf_test_main(int argc, char **argv);

#define CF_CHECK(cond)                                                       \
    do {                                                                     \
        if (!(cond)) {                                                       \
            cf_test_note_failure(__FILE__, __LINE__, #cond, 0);              \
        }                                                                    \
    } while (0)

#define CF_REQUIRE(cond)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            cf_test_note_failure(__FILE__, __LINE__, #cond, 1);              \
        }                                                                    \
    } while (0)

#define CF_TEST_MAIN()                                                       \
    int cf_test_failed_checks = 0;                                           \
    int cf_test_aborting = 0;                                                \
    jmp_buf cf_test_abort;                                                   \
                                                                             \
    static struct cf_test_node *cf_test_ctor_head;                           \
                                                                             \
    void cf_test_register(const struct cf_test_case *tc) {                   \
        struct cf_test_node *node = malloc(sizeof *node);                    \
        if (node == NULL) {                                                  \
            fprintf(stderr, "cf_test: cannot register case %s\n", tc->name); \
            exit(2);                                                         \
        }                                                                    \
        node->tc = tc;                                                       \
        node->next = cf_test_ctor_head;                                      \
        cf_test_ctor_head = node;                                            \
    }                                                                        \
                                                                             \
    void cf_test_note_failure(const char *file, int line, const char *expr,  \
                              int fatal) {                                   \
        printf("    %s:%d: %s\n", file, line, expr);                         \
        cf_test_failed_checks++;                                             \
        if (fatal) {                                                         \
            cf_test_aborting = 1;                                            \
            longjmp(cf_test_abort, 1);                                       \
        }                                                                    \
    }                                                                        \
                                                                             \
    static int cf_test_match(const char *name, const char *filter) {         \
        return filter == NULL || strcmp(name, filter) == 0 ||                \
               strstr(name, filter) != NULL;                                 \
    }                                                                        \
                                                                             \
    static void cf_test_run_one(const struct cf_test_case *c,                \
                                const char *filter, int *matched,            \
                                int *passed, int *failed) {                  \
        if (!cf_test_match(c->name, filter)) {                               \
            return;                                                          \
        }                                                                    \
        (*matched)++;                                                        \
        cf_test_failed_checks = 0;                                           \
        cf_test_aborting = 0;                                                \
        if (setjmp(cf_test_abort) == 0) {                                    \
            c->fn();                                                         \
        }                                                                    \
        if (cf_test_failed_checks == 0) {                                    \
            printf("PASS %s\n", c->name);                                    \
            (*passed)++;                                                     \
        } else {                                                             \
            printf("FAIL %s\n", c->name);                                    \
            (*failed)++;                                                     \
        }                                                                    \
    }                                                                        \
                                                                             \
    int cf_test_main(int argc, char **argv) {                                \
        const char *filter = NULL;                                           \
        if (argc > 2) {                                                      \
            fprintf(stderr, "usage: %s [case-name-filter]\n", argv[0]);      \
            return 2;                                                        \
        }                                                                    \
        if (argc == 2) {                                                     \
            filter = argv[1];                                                \
        }                                                                    \
                                                                             \
        int matched = 0;                                                     \
        int passed = 0;                                                      \
        int failed = 0;                                                      \
                                                                             \
        const struct cf_test_case *case_start = __start_cf_test_cases;       \
        const struct cf_test_case *case_stop = __stop_cf_test_cases;         \
        int have_section = case_start != NULL && case_start != case_stop;    \
        if (!have_section && cf_test_ctor_head == NULL) {                    \
            fprintf(stderr, "cf_test: no test cases registered\n");          \
            return 2;                                                        \
        }                                                                    \
                                                                             \
        if (have_section) {                                                  \
            for (const struct cf_test_case *c = case_start; c < case_stop;   \
                 c++) {                                                      \
                cf_test_run_one(c, filter, &matched, &passed, &failed);      \
            }                                                                \
        } else {                                                             \
            for (const struct cf_test_node *n = cf_test_ctor_head;           \
                 n != NULL; n = n->next) {                                   \
                cf_test_run_one(n->tc, filter, &matched, &passed, &failed);  \
            }                                                                \
        }                                                                    \
                                                                             \
        if (filter != NULL && matched == 0) {                                \
            fprintf(stderr, "cf_test: no case matched filter \"%s\"\n",      \
                    filter);                                                 \
            return 2;                                                        \
        }                                                                    \
                                                                             \
        printf("cf_test: %d case(s), %d passed, %d failed\n",                \
               passed + failed, passed, failed);                             \
        return failed == 0 ? 0 : 1;                                          \
    }                                                                        \
                                                                             \
    int main(int argc, char **argv) {                                        \
        return cf_test_main(argc, argv);                                     \
    }

#endif /* CF_TEST_H */
