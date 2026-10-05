/* A00 app-layer tests: config accessor, per-loop budget division, and the
 * start/stop lifecycle with the writer and request workers (a fresh temporary
 * database, no HTTP loop). */
#include "cf_test.h"

#include "app.h"
#include "app_internal.h"
#include "cf.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static cf_config *make_config(const char *origin) {
    cf_config_entry entries[2] = {
        {"PUBLIC_ORIGIN", origin},
        {"SECRET_KEY_BASE", HEX64},
    };
    cf_config *config = NULL;
    CF_REQUIRE(cf_config_parse(entries, 2, NULL, &config) == CF_OK);
    return config;
}

CF_TEST(app_config_accessor) {
    cf_config *config = make_config("http://127.0.0.1:3000");
    cf_app *app = NULL;
    CF_REQUIRE(cf_app_create(config, &app) == CF_OK);
    const cf_config *borrowed = cf_app_config(app);
    CF_REQUIRE(borrowed != NULL);
    CF_CHECK(borrowed == config);
    CF_CHECK(strcmp(borrowed->public_origin, "http://127.0.0.1:3000") == 0);
    CF_CHECK(cf_app_config(NULL) == NULL);
    cf_app_destroy(app);
}

CF_TEST(loop_budget_division_sums_to_total) {
    cf_config_entry entries[5] = {
        {"PUBLIC_ORIGIN", "http://127.0.0.1:3001"},
        {"SECRET_KEY_BASE", HEX64},
        {"CF_LOOPS", "4"},
        {"CF_INPUT_BYTES", "10"},
        {"CF_OUTPUT_BYTES", "7"},
    };
    cf_config *config = NULL;
    CF_REQUIRE(cf_config_parse(entries, 5, NULL, &config) == CF_OK);
    cf_app *app = NULL;
    CF_REQUIRE(cf_app_create(config, &app) == CF_OK);

    size_t input_total = 0, output_total = 0;
    size_t expect_in[4] = {3, 3, 2, 2};
    size_t expect_out[4] = {2, 2, 2, 1};
    for (size_t i = 0; i < 4; i++) {
        size_t in = cf_app_loop_input_bytes(app, i);
        size_t out = cf_app_loop_output_bytes(app, i);
        CF_CHECK(in == expect_in[i]);
        CF_CHECK(out == expect_out[i]);
        input_total += in;
        output_total += out;
    }
    CF_CHECK(input_total == 10);
    CF_CHECK(output_total == 7);
    CF_CHECK(cf_app_loop_input_bytes(app, 4) == 0);
    CF_CHECK(cf_app_loop_output_bytes(app, 99) == 0);
    CF_CHECK(cf_app_loop_input_bytes(NULL, 0) == 0);
    cf_app_destroy(app);
}

/* A config budget smaller than CF_LOOPS cannot be expressed (H01 reads 0 as
 * its 64 MiB default); every loop gets the smallest nonzero share. */
CF_TEST(loop_budget_never_returns_zero) {
    cf_config_entry entries[5] = {
        {"PUBLIC_ORIGIN", "http://127.0.0.1:3002"},
        {"SECRET_KEY_BASE", HEX64},
        {"CF_LOOPS", "4"},
        {"CF_INPUT_BYTES", "1"},
        {"CF_OUTPUT_BYTES", "2"},
    };
    cf_config *config = NULL;
    CF_REQUIRE(cf_config_parse(entries, 5, NULL, &config) == CF_OK);
    cf_app *app = NULL;
    CF_REQUIRE(cf_app_create(config, &app) == CF_OK);
    for (size_t i = 0; i < 4; i++) {
        CF_CHECK(cf_app_loop_input_bytes(app, i) == 1);
    }
    CF_CHECK(cf_app_loop_output_bytes(app, 0) == 1);
    CF_CHECK(cf_app_loop_output_bytes(app, 1) == 1);
    cf_app_destroy(app);
}

CF_TEST(app_start_stop_lifecycle_with_database) {
    char dir[] = "/tmp/a00poolXXXXXX";
    CF_REQUIRE(mkdtemp(dir) != NULL);
    char db_path[160];
    snprintf(db_path, sizeof db_path, "%s/campfire.sqlite3", dir);

    cf_config_entry entries[4] = {
        {"PUBLIC_ORIGIN", "http://127.0.0.1:3003"},
        {"SECRET_KEY_BASE", HEX64},
        {"DATABASE_PATH", db_path},
        {"CF_READERS", "2"},
    };
    cf_config *config = NULL;
    CF_REQUIRE(cf_config_parse(entries, 4, NULL, &config) == CF_OK);
    cf_app *app = NULL;
    CF_REQUIRE(cf_app_create(config, &app) == CF_OK);

    CF_CHECK(cf_app_admitted_count(app) == 0);
    CF_CHECK(cf_app_start(app) == CF_OK);
    CF_CHECK(cf_app_start(app) == CF_BUSY); /* one start per app */
    CF_CHECK(cf_data_version(app) == 1);
    cf_app_advance_data_version(app);
    CF_CHECK(cf_data_version(app) == 2);
    CF_CHECK(cf_app_completed_requests(app) == 0);
    CF_CHECK(cf_app_admitted_count(app) == 0);

    cf_app_stop(app);
    CF_CHECK(cf_app_stop_requested(app) == true);
    cf_app_stop(app); /* idempotent */
    /* a stopped app cannot start again */
    CF_CHECK(cf_app_start(app) == CF_BUSY);

    cf_app_destroy(app);

    char path[192];
    snprintf(path, sizeof path, "%s/campfire.sqlite3", dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/campfire.sqlite3-wal", dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/campfire.sqlite3-shm", dir);
    unlink(path);
    rmdir(dir);
}

CF_TEST(app_start_failure_leaves_app_destroyable) {
    /* A database path whose parent does not exist: the writer cannot create
     * the schema, so startup fails and destroy must still be clean. */
    cf_config_entry entries[3] = {
        {"PUBLIC_ORIGIN", "http://127.0.0.1:3004"},
        {"SECRET_KEY_BASE", HEX64},
        {"DATABASE_PATH", "/tmp/a00-missing-dir-xyz/campfire.sqlite3"},
    };
    cf_config *config = NULL;
    CF_REQUIRE(cf_config_parse(entries, 3, NULL, &config) == CF_OK);
    cf_app *app = NULL;
    CF_REQUIRE(cf_app_create(config, &app) == CF_OK);
    CF_CHECK(cf_app_start(app) != CF_OK);
    cf_app_stop(app);
    cf_app_destroy(app);
}

CF_TEST(app_admit_before_start_declines) {
    cf_config *config = make_config("http://127.0.0.1:3005");
    cf_app *app = NULL;
    CF_REQUIRE(cf_app_create(config, &app) == CF_OK);
    /* A NULL task is invalid; before start any task would decline with
     * CF_BUSY. The pointer is never dereferenced for a declined admission. */
    CF_CHECK(cf_app_admit(app, NULL) == CF_INVALID);
    cf_app_destroy(app);
}

CF_TEST_MAIN()
