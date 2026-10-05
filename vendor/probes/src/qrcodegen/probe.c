/*
 * F00 probe for qrcodegen v1.8.0 (Nayuki QR Code generator, C).
 *
 * Generates QR matrices for fixed strings at explicitly requested
 * versions and ECC levels and verifies:
 *   - the module dimensions equal 4 * version + 17 for the requested
 *     version (versions 1 and 5 are pinned as min=max);
 *   - all three finder patterns (7x7) and their light separators are
 *     present at the top-left, top-right and bottom-left corners;
 *   - the requested ECC level is applied (same version/text at LOW and
 *     HIGH yields different matrices, both with the requested size);
 *   - encoding is deterministic across repeated calls.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "qrcodegen.h"

static int failures;

static void check(bool cond, const char *what)
{
    if (cond) {
        printf("ok: %s\n", what);
    } else {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

/* A finder pattern with its top-left module at (x0, y0):
 * 7x7 dark border, 3x3 dark core, light ring in between. */
static bool finder_ok(const uint8_t qr[], int x0, int y0)
{
    int x, y;

    for (y = 0; y < 7; y++) {
        for (x = 0; x < 7; x++) {
            bool expected = (x == 0 || x == 6 || y == 0 || y == 6) ||
                            (x >= 2 && x <= 4 && y >= 2 && y <= 4);
            if (qrcodegen_getModule(qr, x0 + x, y0 + y) != expected)
                return false;
        }
    }
    return true;
}

static bool all_light(const uint8_t qr[], int x0, int y0, int w, int h)
{
    int x, y;

    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            if (qrcodegen_getModule(qr, x0 + x, y0 + y))
                return false;
    return true;
}

/* Finder + separator geometry for a matrix of width `size`. */
static void check_corners(const uint8_t qr[], int size, const char *label)
{
    char what[160];

    snprintf(what, sizeof(what), "%s: finder top-left", label);
    check(finder_ok(qr, 0, 0), what);
    snprintf(what, sizeof(what), "%s: finder top-right", label);
    check(finder_ok(qr, size - 7, 0), what);
    snprintf(what, sizeof(what), "%s: finder bottom-left", label);
    check(finder_ok(qr, 0, size - 7), what);

    snprintf(what, sizeof(what), "%s: separators light", label);
    check(all_light(qr, 0, 7, 8, 1) &&
              all_light(qr, 7, 0, 1, 8) &&
              all_light(qr, size - 8, 7, 8, 1) &&
              all_light(qr, size - 8, 0, 1, 8) &&
              all_light(qr, 0, size - 8, 8, 1) &&
              all_light(qr, 7, size - 8, 1, 8),
          what);
}

int main(void)
{
    static uint8_t qr1[qrcodegen_BUFFER_LEN_FOR_VERSION(1)];
    static uint8_t qr5[qrcodegen_BUFFER_LEN_FOR_VERSION(5)];
    static uint8_t qr5b[qrcodegen_BUFFER_LEN_FOR_VERSION(5)];
    static uint8_t temp[qrcodegen_BUFFER_LEN_MAX];
    char what[160];
    int size;

    /* Version 1, ECC MEDIUM, mask auto, no ECC boosting. */
    bool ok1 = qrcodegen_encodeText("HELLO WORLD", temp, qr1,
                                    qrcodegen_Ecc_MEDIUM, 1, 1,
                                    qrcodegen_Mask_AUTO, false);
    check(ok1, "version 1 MEDIUM encode succeeds");
    if (!ok1) {
        printf("probe_result: FAIL (%d failure(s))\n", failures);
        return 1;
    }
    size = qrcodegen_getSize(qr1);
    snprintf(what, sizeof(what), "version 1 dimensions: %d == 4*1+17", size);
    check(size == 21, what);
    check_corners(qr1, size, "v1");

    /* Version 5, ECC HIGH, mask auto, no ECC boosting. */
    bool ok5 = qrcodegen_encodeText("HELLO WORLD 12345", temp, qr5,
                                    qrcodegen_Ecc_HIGH, 5, 5,
                                    qrcodegen_Mask_AUTO, false);
    check(ok5, "version 5 HIGH encode succeeds");
    if (!ok5) {
        printf("probe_result: FAIL (%d failure(s))\n", failures);
        return 1;
    }
    size = qrcodegen_getSize(qr5);
    snprintf(what, sizeof(what), "version 5 dimensions: %d == 4*5+17", size);
    check(size == 37, what);
    check_corners(qr5, size, "v5");

    /* Requested ECC level is honored: LOW vs HIGH at the same pinned
     * version and text must differ while keeping the same dimensions. */
    bool okl = qrcodegen_encodeText("HELLO WORLD 12345", temp, qr5b,
                                    qrcodegen_Ecc_LOW, 5, 5,
                                    qrcodegen_Mask_AUTO, false);
    check(okl && qrcodegen_getSize(qr5b) == 37,
          "version 5 LOW encode succeeds with size 37");
    check(memcmp(qr5, qr5b, qrcodegen_BUFFER_LEN_FOR_VERSION(5)) != 0,
          "ECC LOW and HIGH yield different matrices at version 5");

    /* Determinism: repeating the version 5 HIGH call reproduces it. */
    bool ok5b = qrcodegen_encodeText("HELLO WORLD 12345", temp, qr5b,
                                     qrcodegen_Ecc_HIGH, 5, 5,
                                     qrcodegen_Mask_AUTO, false);
    check(ok5b &&
              memcmp(qr5, qr5b, qrcodegen_BUFFER_LEN_FOR_VERSION(5)) == 0,
          "repeated version 5 HIGH encode is deterministic");

    printf("probe_result: %s (%d failure(s))\n",
           failures ? "FAIL" : "READY", failures);
    return failures ? 1 : 0;
}
