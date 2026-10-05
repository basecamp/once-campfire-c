/* Datetime text <-> int64 UTC microseconds helpers (D01 db-core).
 *
 * The reference is the pinned tmp/rust-ref/crates/db/src/time.rs Timestamp
 * (Rails datetime(6) quoting): "YYYY-MM-DD HH:MM:SS" plus ".ffffff" only when
 * the microseconds are non-zero; parsing also tolerates 'T', 'Z' and ' UTC'
 * and pads short fractions (e.g. SQLite STRFTIME milliseconds) to microseconds.
 */
#include "cf_test.h"

#include "db/db_internal.h"

static cf_span sp(const char *text) {
    cf_span span = {(const unsigned char *)text, strlen(text)};
    return span;
}

static int64_t time_from(const char *text, cf_err *err) {
    int64_t us = -1;
    *err = cf_db_time_from_text(sp(text), &us);
    return us;
}

static void check_text(const char *input, const char *expected) {
    char out[CF_DB_TIME_TEXT_CAP];
    cf_err err = CF_OK;
    int64_t us = time_from(input, &err);
    if (err != CF_OK) {
        CF_CHECK(!"parse failed");
        printf("    parse failed: %s (%s)\n", input, cf_db_last_error());
        return;
    }
    if (cf_db_time_to_text(us, out) != CF_OK) {
        CF_CHECK(!"format failed");
        return;
    }
    if (strcmp(out, expected) != 0) {
        CF_CHECK(!"datetime mismatch");
        printf("    %s -> %s, expected %s\n", input, out, expected);
    }
}

CF_TEST(time_formats_like_reference) {
    char out[CF_DB_TIME_TEXT_CAP];
    CF_REQUIRE(cf_db_time_to_text(0, out) == CF_OK);
    CF_CHECK(strcmp(out, "1970-01-01 00:00:00") == 0);
    CF_CHECK(strlen(out) == 19); /* zero microseconds: no fraction */

    CF_REQUIRE(cf_db_time_to_text(123456, out) == CF_OK);
    CF_CHECK(strcmp(out, "1970-01-01 00:00:00.123456") == 0);
    CF_CHECK(strlen(out) == 26); /* non-zero: exactly six digits */

    CF_REQUIRE(cf_db_time_to_text(-1, out) == CF_OK);
    CF_CHECK(strcmp(out, "1969-12-31 23:59:59.999999") == 0);

    CF_REQUIRE(cf_db_time_to_text(1700000000123456LL, out) == CF_OK);
    CF_CHECK(strcmp(out, "2023-11-14 22:13:20.123456") == 0);

    /* A value outside the storable four-digit year range is rejected, not
     * truncated or wrapped. */
    CF_CHECK(cf_db_time_to_text(INT64_MIN, out) == CF_INVALID);
    CF_CHECK(cf_db_time_to_text(INT64_MAX, out) == CF_INVALID);
}

CF_TEST(time_round_trips_and_tolerances) {
    check_text("2026-09-26 12:34:56.123456", "2026-09-26 12:34:56.123456");
    check_text("2026-01-01 00:00:00", "2026-01-01 00:00:00");
    check_text("2026-09-26 12:25:26.826", "2026-09-26 12:25:26.826000");
    check_text("2026-09-26 12:25:26.5", "2026-09-26 12:25:26.500000");
    check_text("2026-09-26 12:25:26.1234567", "2026-09-26 12:25:26.123456");
    check_text("2026-09-26T12:34:56Z", "2026-09-26 12:34:56");
    check_text("2026-09-26T12:34:56.000001Z", "2026-09-26 12:34:56.000001");
    check_text("2026-09-26 12:34:56 UTC", "2026-09-26 12:34:56");
    check_text("  2026-09-26 12:34:56  ", "2026-09-26 12:34:56");
    check_text("2024-02-29 00:00:00", "2024-02-29 00:00:00");
    check_text("9999-12-31 23:59:59.999999", "9999-12-31 23:59:59.999999");
    check_text("0001-01-01 00:00:00", "0001-01-01 00:00:00");

    /* Parsed microseconds equal the epoch math for a known instant. */
    cf_err err = CF_OK;
    CF_CHECK(time_from("1970-01-01 00:00:00", &err) == 0 && err == CF_OK);
    CF_CHECK(time_from("1970-01-01 00:00:00.000001", &err) == 1 &&
             err == CF_OK);
    CF_CHECK(time_from("1969-12-31 23:59:59.999999", &err) == -1 &&
             err == CF_OK);
    CF_CHECK(time_from("1970-01-02 00:00:00", &err) == 86400000000LL &&
             err == CF_OK);
}

CF_TEST(time_rejects_invalid) {
    static const char *const invalid[] = {
        "",
        "yesterday",
        "2026-13-01 00:00:00",
        "2026-02-30 00:00:00",
        "2025-02-29 00:00:00",
        "2026-00-10 00:00:00",
        "2026-09-00 00:00:00",
        "2026-09-26 24:00:00",
        "2026-09-26 12:60:00",
        "2026-09-26 12:34:60",
        "2026-09-26 12:34",
        "2026/09/26 12:34:56",
        "2026-09-26 12:34:56.",
        "2026-09-26 12:34:56.x",
        "2026-09-26 12:34:56.12345x",
        "2026-09-26  12:34:56",
        "2026-09-26 12:34:5",
        "10000-01-01 00:00:00",
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        int64_t us = -1;
        cf_err err = cf_db_time_from_text(sp(invalid[i]), &us);
        if (err != CF_INVALID || us != 0) {
            CF_CHECK(!"invalid datetime accepted");
            printf("    accepted: \"%s\"\n", invalid[i]);
        }
    }

    int64_t us = 5;
    CF_CHECK(cf_db_time_from_text((cf_span){NULL, 0}, &us) == CF_INVALID);
    CF_CHECK(us == 0); /* out pointers start empty */
    CF_CHECK(cf_db_time_from_text(sp("2026-01-01 00:00:00"), NULL) ==
             CF_INVALID);
    CF_CHECK(cf_db_time_to_text(0, NULL) == CF_INVALID);
}

CF_TEST_MAIN()
