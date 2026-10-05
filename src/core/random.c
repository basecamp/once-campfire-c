/* cf_random_bytes: OS entropy through getrandom(2), with a /dev/urandom
 * fallback, per the F01 card. CF_IO is returned when no source could fill the
 * buffer; the output is zeroed first so callers cannot accidentally use
 * partial entropy. Failure behavior is deterministic under the test seam in
 * core/testrandom.h. */
#include "cf.h"

#include "core/testrandom.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

enum {
    CF_TEST_RANDOM_REAL = 0,
    CF_TEST_RANDOM_FILL = 1,
    CF_TEST_RANDOM_FAIL = 2
};

static _Atomic int cf_test_random_mode = CF_TEST_RANDOM_REAL;
static _Atomic int cf_test_random_byte = 0;

static cf_err cf_random_urandom(unsigned char *p, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return CF_IO;
    size_t done = 0;
    cf_err rc = CF_OK;
    while (done < len) {
        ssize_t n = read(fd, p + done, len - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            rc = CF_IO;
            break;
        }
        if (n == 0) {
            rc = CF_IO;
            break;
        }
        done += (size_t)n;
    }
    (void)close(fd);
    return rc;
}

cf_err cf_random_bytes(void *out, size_t len) {
    if (len == 0) return CF_OK;
    if (out == NULL) return CF_INVALID;
    unsigned char *p = out;

    int mode = atomic_load_explicit(&cf_test_random_mode, memory_order_relaxed);
    if (mode == CF_TEST_RANDOM_FAIL) {
        memset(p, 0, len);
        return CF_IO;
    }
    if (mode == CF_TEST_RANDOM_FILL) {
        memset(p, atomic_load_explicit(&cf_test_random_byte, memory_order_relaxed),
               len);
        return CF_OK;
    }

    size_t done = 0;
    while (done < len) {
        ssize_t n = getrandom(p + done, len - done, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            break; /* fall back to /dev/urandom for the remainder */
        }
        if (n == 0) break;
        done += (size_t)n;
    }
    if (done < len && cf_random_urandom(p + done, len - done) != CF_OK) {
        memset(p, 0, len);
        return CF_IO;
    }
    return CF_OK;
}

void cf_test_random_fill(unsigned char byte) {
    atomic_store_explicit(&cf_test_random_byte, (int)byte, memory_order_relaxed);
    atomic_store_explicit(&cf_test_random_mode, CF_TEST_RANDOM_FILL,
                          memory_order_relaxed);
}

void cf_test_random_fail(void) {
    atomic_store_explicit(&cf_test_random_mode, CF_TEST_RANDOM_FAIL,
                          memory_order_relaxed);
}

void cf_test_random_clear(void) {
    atomic_store_explicit(&cf_test_random_mode, CF_TEST_RANDOM_REAL,
                          memory_order_relaxed);
    atomic_store_explicit(&cf_test_random_byte, 0, memory_order_relaxed);
}
