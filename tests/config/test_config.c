/* F03 configuration acceptance matrix: defaults, every required setting,
 * invalid / negative / overflowing / duplicate selections, the CLI option
 * surface, secret handling and the bcrypt-cost test injection. */
#include "cf_test.h"

#include "cf.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <sched.h>
#endif

#define HEX64 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define HEX128 \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef" \
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

#define MAX_EXTRA 32
#define MAX_ENTRIES (MAX_EXTRA + 2)

/* Parses the required base table plus the extra selections. An extra entry
 * whose name matches a base name replaces the base value. */
static cf_err parse_case(const cf_config_entry *extra, size_t extra_count,
                         cf_config_error *err, cf_config **out) {
    static const char *const base_names[2] = {"PUBLIC_ORIGIN",
                                              "SECRET_KEY_BASE"};
    static const char *const base_values[2] = {"http://localhost:3000",
                                               HEX64};
    cf_config_entry entries[MAX_ENTRIES];
    size_t count = 0;
    *out = NULL;
    if (extra_count > MAX_EXTRA) return CF_INVALID;

    for (size_t i = 0; i < 2; i++) {
        const char *value = base_values[i];
        for (size_t j = 0; j < extra_count; j++) {
            if (strcmp(extra[j].name, base_names[i]) == 0) {
                value = extra[j].value;
            }
        }
        entries[count].name = base_names[i];
        entries[count].value = value;
        count++;
    }
    for (size_t j = 0; j < extra_count; j++) {
        if (strcmp(extra[j].name, base_names[0]) == 0 ||
            strcmp(extra[j].name, base_names[1]) == 0) {
            continue;
        }
        entries[count++] = extra[j];
    }
    return cf_config_parse(entries, count, err, out);
}

static cf_err parse_one(const char *name, const char *value, cf_config_error *err,
                        cf_config **out) {
    cf_config_entry extra = {name, value};
    return parse_case(&extra, 1, err, out);
}

static void expect_setting_failure(const char *name, const char *value,
                                   const char *expected) {
    cf_config_error err = {NULL, NULL};
    cf_config *cfg = NULL;
    cf_err rc = parse_one(name, value, &err, &cfg);
    CF_CHECK(rc != CF_OK);
    CF_CHECK(cfg == NULL);
    CF_REQUIRE(err.setting != NULL);
    CF_CHECK(strcmp(err.setting, expected) == 0);
    CF_CHECK(err.reason != NULL && err.reason[0] != '\0');
}

CF_TEST(config_defaults_and_destroy) {
    cf_config *cfg = NULL;
    CF_REQUIRE(parse_case(NULL, 0, NULL, &cfg) == CF_OK);
    CF_REQUIRE(cfg != NULL);

    CF_CHECK(strcmp(cfg->host, CF_CONFIG_DEFAULT_HOST) == 0);
    CF_CHECK(cfg->port == CF_CONFIG_DEFAULT_PORT);
    CF_CHECK(strcmp(cfg->public_origin, "http://localhost:3000") == 0);
    CF_CHECK(strcmp(cfg->database_path, CF_CONFIG_DEFAULT_DATABASE_PATH) == 0);
    CF_CHECK(strcmp(cfg->storage_path, CF_CONFIG_DEFAULT_STORAGE_PATH) == 0);
    CF_CHECK(cfg->secret_key_base_len == 64);
    CF_CHECK(strcmp(cfg->secret_key_base, HEX64) == 0);
    CF_CHECK(cfg->loops >= 1 && cfg->loops <= CF_CONFIG_DEFAULT_READERS);
    CF_CHECK(cfg->readers == 4);
    CF_CHECK(cfg->request_slots == 256);
    CF_CHECK(cfg->writer_queue == 256);
    CF_CHECK(cfg->connections_per_loop == 2048);
    CF_CHECK(cfg->input_bytes == 64 * CF_CONFIG_MIB);
    CF_CHECK(cfg->output_bytes == 64 * CF_CONFIG_MIB);
    CF_CHECK(cfg->cache_bytes == 64 * CF_CONFIG_MIB);
    CF_CHECK(cfg->job_queue == 128);
    CF_CHECK(cfg->job_workers == 2);
    CF_CHECK(cfg->crypto_workers == 2);
    CF_CHECK(cfg->disable_ssl == true);
    CF_CHECK(cfg->tls_cert_file == NULL);
    CF_CHECK(cfg->tls_key_file == NULL);
    CF_CHECK(cfg->vapid_public_key == NULL);
    CF_CHECK(cfg->push_configured == false);
    CF_CHECK(cfg->bcrypt_cost == CF_BCRYPT_COST);
    CF_CHECK(CF_BCRYPT_COST == 12); /* test injection must not change this */
    cf_config_destroy(cfg);
    cf_config_destroy(NULL); /* destructors accept NULL */
}

CF_TEST(config_defaults_respect_assigned_cpus) {
#ifdef __linux__
    cpu_set_t original, assigned;
    CF_REQUIRE(sched_getaffinity(0, sizeof original, &original) == 0);
    CPU_ZERO(&assigned);
    size_t selected = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE && selected < 2; cpu++) {
        if (!CPU_ISSET(cpu, &original)) continue;
        CPU_SET(cpu, &assigned);
        selected++;
        CF_REQUIRE(sched_setaffinity(0, sizeof assigned, &assigned) == 0);
        cf_config *cfg = NULL;
        cf_err result = parse_case(NULL, 0, NULL, &cfg);
        /* Restore even if a later assertion fails. */
        int restored = sched_setaffinity(0, sizeof original, &original);
        CF_REQUIRE(restored == 0);
        CF_REQUIRE(result == CF_OK);
        CF_CHECK(cfg->loops == selected);
        cf_config_destroy(cfg);
    }
    CF_CHECK(selected > 0);
#else
    cf_config *cfg = NULL;
    CF_REQUIRE(parse_case(NULL, 0, NULL, &cfg) == CF_OK);
    CF_CHECK(cfg->loops == CF_CONFIG_DEFAULT_LOOPS);
    cf_config_destroy(cfg);
#endif
}

CF_TEST(config_all_valid_overrides) {
    static const cf_config_entry all[] = {
        {"HOST", "127.0.0.1"},
        {"PORT", "65535"},
        {"PUBLIC_ORIGIN", "https://campfire.example:8443"},
        {"DATABASE_PATH", "/tmp/f03/db.sqlite3"},
        {"STORAGE_PATH", "/tmp/f03/files"},
        {"SECRET_KEY_BASE", HEX128},
        {"CF_LOOPS", "4"},
        {"CF_READERS", "64"},
        {"CF_REQUEST_SLOTS", "1"},
        {"CF_WRITER_QUEUE", "2"},
        {"CF_CONNECTIONS_PER_LOOP", "3"},
        {"CF_INPUT_BYTES", "1048576"},
        {"CF_OUTPUT_BYTES", "7"},
        {"CF_CACHE_BYTES", "67108864"},
        {"CF_JOB_QUEUE", "5"},
        {"CF_JOB_WORKERS", "6"},
        {"CF_CRYPTO_WORKERS", "7"},
        {"DISABLE_SSL", "0"},
        {"TLS_CERT_FILE", "/tmp/f03/cert.pem"},
        {"TLS_KEY_FILE", "/tmp/f03/key.pem"},
        {"VAPID_PUBLIC_KEY", "public-key"},
        {"VAPID_PRIVATE_KEY", "private-key"},
        {"VAPID_SUBJECT", "mailto:push@example.com"},
    };
    const size_t count = sizeof all / sizeof all[0];
    cf_config *cfg = NULL;
    CF_REQUIRE(parse_case(all, count, NULL, &cfg) == CF_OK);
    CF_REQUIRE(cfg != NULL);

    CF_CHECK(strcmp(cfg->host, "127.0.0.1") == 0);
    CF_CHECK(cfg->port == 65535);
    CF_CHECK(strcmp(cfg->public_origin, "https://campfire.example:8443") == 0);
    CF_CHECK(strcmp(cfg->database_path, "/tmp/f03/db.sqlite3") == 0);
    CF_CHECK(strcmp(cfg->storage_path, "/tmp/f03/files") == 0);
    CF_CHECK(cfg->secret_key_base_len == 128);
    CF_CHECK(cfg->loops == 4);
    CF_CHECK(cfg->readers == 64);
    CF_CHECK(cfg->request_slots == 1);
    CF_CHECK(cfg->writer_queue == 2);
    CF_CHECK(cfg->connections_per_loop == 3);
    CF_CHECK(cfg->input_bytes == 1048576);
    CF_CHECK(cfg->output_bytes == 7);
    CF_CHECK(cfg->cache_bytes == 67108864);
    CF_CHECK(cfg->job_queue == 5);
    CF_CHECK(cfg->job_workers == 6);
    CF_CHECK(cfg->crypto_workers == 7);
    CF_CHECK(cfg->disable_ssl == false);
    CF_CHECK(strcmp(cfg->tls_cert_file, "/tmp/f03/cert.pem") == 0);
    CF_CHECK(strcmp(cfg->tls_key_file, "/tmp/f03/key.pem") == 0);
    CF_CHECK(cfg->push_configured == true);
    cf_config_destroy(cfg);
}

CF_TEST(config_required_settings) {
    cf_config_error err = {NULL, NULL};
    cf_config *cfg = NULL;
    cf_config_entry entry = {"SECRET_KEY_BASE", HEX64};

    cf_err rc = cf_config_parse(&entry, 1, &err, &cfg);
    CF_CHECK(rc != CF_OK);
    CF_CHECK(cfg == NULL);
    CF_REQUIRE(err.setting != NULL);
    CF_CHECK(strcmp(err.setting, "PUBLIC_ORIGIN") == 0);
    CF_CHECK(strcmp(err.reason, "is required") == 0);

    entry.name = "PUBLIC_ORIGIN";
    entry.value = "http://localhost:3000";
    rc = cf_config_parse(&entry, 1, &err, &cfg);
    CF_CHECK(rc != CF_OK);
    CF_CHECK(cfg == NULL);
    CF_REQUIRE(err.setting != NULL);
    CF_CHECK(strcmp(err.setting, "SECRET_KEY_BASE") == 0);
    CF_CHECK(strcmp(err.reason, "is required") == 0);

    rc = cf_config_parse(NULL, 0, &err, &cfg);
    CF_CHECK(rc != CF_OK);
    CF_CHECK(cfg == NULL);
    CF_REQUIRE(err.setting != NULL);
    CF_CHECK(strcmp(err.setting, "PUBLIC_ORIGIN") == 0);
}

CF_TEST(config_secret_key_base_matrix) {
    static const char *const bad[] = {
        "",          /* empty */
        "abc",       /* too short */
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde", /* 63 */
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg", /* non-hex */
        "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        expect_setting_failure("SECRET_KEY_BASE", bad[i], "SECRET_KEY_BASE");
    }
    static const char *const good[] = {
        HEX64,
        HEX128,
        "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF",
    };
    for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) {
        cf_config *cfg = NULL;
        CF_REQUIRE(parse_one("SECRET_KEY_BASE", good[i], NULL, &cfg) == CF_OK);
        CF_CHECK(cfg->secret_key_base_len == strlen(good[i]));
        cf_config_destroy(cfg);
    }
}

CF_TEST(config_public_origin_matrix) {
    static const char *const good[] = {
        "http://localhost:3000",
        "https://example.com",
        "https://example.com:443",
        "http://127.0.0.1:1",
        "https://[::1]:3000",
    };
    for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) {
        cf_config *cfg = NULL;
        CF_REQUIRE(parse_one("PUBLIC_ORIGIN", good[i], NULL, &cfg) == CF_OK);
        CF_CHECK(strcmp(cfg->public_origin, good[i]) == 0);
        cf_config_destroy(cfg);
    }
    static const char *const bad[] = {
        "",
        "example.com",             /* no scheme */
        "ftp://example.com",       /* unsupported scheme */
        "http://",                 /* empty host */
        "http:///path",            /* empty host, path */
        "https://example.com/",    /* path */
        "https://example.com/x",   /* path */
        "https://example.com?q=1", /* query */
        "https://example.com#f",   /* fragment */
        "https://user@example.com",/* userinfo */
        "https://:3000",           /* empty host */
        "https://[::1",            /* malformed bracket */
        "https://[::1]x",          /* trailing host text */
        "https://example.com:0",   /* port out of range */
        "https://example.com:70000",
        "https://example.com:abc",
        "https://example.com:",
        "https://exa mple.com",    /* whitespace */
        "https://[::1]:65536",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        expect_setting_failure("PUBLIC_ORIGIN", bad[i], "PUBLIC_ORIGIN");
    }
}

CF_TEST(config_port_matrix) {
    static const char *const bad[] = {"0", "65536", "-1", "+5", "3000x",
                                      "", " 3000", "0x10",
                                      "99999999999999999999"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        expect_setting_failure("PORT", bad[i], "PORT");
    }
    static const char *const good[] = {"1", "3000", "65535", "03000"};
    for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) {
        cf_config *cfg = NULL;
        CF_REQUIRE(parse_one("PORT", good[i], NULL, &cfg) == CF_OK);
        cf_config_destroy(cfg);
    }
}

CF_TEST(config_loops_and_readers_bounds) {
    static const char *const loops_bad[] = {"0", "65", "-1", "1x", "",
                                            "18446744073709551616"};
    for (size_t i = 0; i < sizeof loops_bad / sizeof loops_bad[0]; i++) {
        expect_setting_failure("CF_LOOPS", loops_bad[i], "CF_LOOPS");
        expect_setting_failure("CF_READERS", loops_bad[i], "CF_READERS");
    }
    static const char *const good[] = {"1", "4", "64"};
    for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) {
        cf_config *cfg = NULL;
        CF_REQUIRE(parse_one("CF_LOOPS", good[i], NULL, &cfg) == CF_OK);
        CF_CHECK(cfg->loops == (size_t)atoi(good[i]));
        cf_config_destroy(cfg);
        CF_REQUIRE(parse_one("CF_READERS", good[i], NULL, &cfg) == CF_OK);
        CF_CHECK(cfg->readers == (size_t)atoi(good[i]));
        cf_config_destroy(cfg);
    }
}

CF_TEST(config_capacity_matrix) {
    static const char *const positives[] = {
        "CF_REQUEST_SLOTS",  "CF_WRITER_QUEUE", "CF_CONNECTIONS_PER_LOOP",
        "CF_INPUT_BYTES",    "CF_OUTPUT_BYTES", "CF_JOB_QUEUE",
        "CF_JOB_WORKERS",    "CF_CRYPTO_WORKERS",
    };
    static const char *const bad[] = {"0", "-1", "18446744073709551616",
                                      "99999999999999999999999", "12x", ""};
    for (size_t i = 0; i < sizeof positives / sizeof positives[0]; i++) {
        for (size_t j = 0; j < sizeof bad / sizeof bad[0]; j++) {
            expect_setting_failure(positives[i], bad[j], positives[i]);
        }
        cf_config *cfg = NULL;
        CF_REQUIRE(parse_one(positives[i], "1", NULL, &cfg) == CF_OK);
        cf_config_destroy(cfg);
    }

    /* CF_CACHE_BYTES accepts zero (disabled) but nothing negative/overflowing. */
    cf_config *cfg = NULL;
    CF_REQUIRE(parse_one("CF_CACHE_BYTES", "0", NULL, &cfg) == CF_OK);
    CF_CHECK(cfg->cache_bytes == 0);
    cf_config_destroy(cfg);
    CF_REQUIRE(parse_one("CF_CACHE_BYTES", "64", NULL, &cfg) == CF_OK);
    CF_CHECK(cfg->cache_bytes == 64);
    cf_config_destroy(cfg);
    expect_setting_failure("CF_CACHE_BYTES", "-1", "CF_CACHE_BYTES");
    expect_setting_failure("CF_CACHE_BYTES", "18446744073709551616",
                           "CF_CACHE_BYTES");
}

CF_TEST(config_ssl_and_tls_matrix) {
    expect_setting_failure("DISABLE_SSL", "2", "DISABLE_SSL");
    expect_setting_failure("DISABLE_SSL", "true", "DISABLE_SSL");
    expect_setting_failure("DISABLE_SSL", "yes", "DISABLE_SSL");
    expect_setting_failure("DISABLE_SSL", "", "DISABLE_SSL");

    /* DISABLE_SSL=0 requires both certificate settings. */
    {
        cf_config_entry extra = {"DISABLE_SSL", "0"};
        cf_config_error err = {NULL, NULL};
        cf_config *cfg = NULL;
        CF_CHECK(parse_case(&extra, 1, &err, &cfg) != CF_OK);
        CF_CHECK(cfg == NULL);
        CF_REQUIRE(err.setting != NULL);
        CF_CHECK(strcmp(err.setting, "TLS_CERT_FILE") == 0);
    }
    {
        cf_config_entry extra[] = {{"DISABLE_SSL", "0"},
                                   {"TLS_CERT_FILE", "/tmp/cert.pem"}};
        cf_config_error err = {NULL, NULL};
        cf_config *cfg = NULL;
        CF_CHECK(parse_case(extra, 2, &err, &cfg) != CF_OK);
        CF_REQUIRE(err.setting != NULL);
        CF_CHECK(strcmp(err.setting, "TLS_KEY_FILE") == 0);
    }
    {
        cf_config_entry extra[] = {{"DISABLE_SSL", "0"},
                                   {"TLS_KEY_FILE", "/tmp/key.pem"}};
        cf_config_error err = {NULL, NULL};
        cf_config *cfg = NULL;
        CF_CHECK(parse_case(extra, 2, &err, &cfg) != CF_OK);
        CF_REQUIRE(err.setting != NULL);
        CF_CHECK(strcmp(err.setting, "TLS_CERT_FILE") == 0);
    }
    /* A lone certificate setting fails even while SSL stays disabled. */
    {
        cf_config_entry extra = {"TLS_CERT_FILE", "/tmp/cert.pem"};
        cf_config_error err = {NULL, NULL};
        cf_config *cfg = NULL;
        CF_CHECK(parse_case(&extra, 1, &err, &cfg) != CF_OK);
        CF_REQUIRE(err.setting != NULL);
        CF_CHECK(strcmp(err.setting, "TLS_KEY_FILE") == 0);
    }
    {
        cf_config_entry extra[] = {{"DISABLE_SSL", "0"},
                                   {"TLS_CERT_FILE", "/tmp/cert.pem"},
                                   {"TLS_KEY_FILE", "/tmp/key.pem"}};
        cf_config *cfg = NULL;
        CF_REQUIRE(parse_case(extra, 3, NULL, &cfg) == CF_OK);
        CF_CHECK(cfg->disable_ssl == false);
        CF_CHECK(strcmp(cfg->tls_cert_file, "/tmp/cert.pem") == 0);
        CF_CHECK(strcmp(cfg->tls_key_file, "/tmp/key.pem") == 0);
        cf_config_destroy(cfg);
    }
    /* Files may be configured while SSL stays disabled (P01 preparation). */
    {
        cf_config_entry extra[] = {{"TLS_CERT_FILE", "/tmp/cert.pem"},
                                   {"TLS_KEY_FILE", "/tmp/key.pem"}};
        cf_config *cfg = NULL;
        CF_REQUIRE(parse_case(extra, 2, NULL, &cfg) == CF_OK);
        CF_CHECK(cfg->disable_ssl == true);
        CF_CHECK(cfg->tls_cert_file != NULL && cfg->tls_key_file != NULL);
        cf_config_destroy(cfg);
    }
    expect_setting_failure("TLS_CERT_FILE", "", "TLS_CERT_FILE");
    expect_setting_failure("TLS_KEY_FILE", "", "TLS_KEY_FILE");
}

CF_TEST(config_vapid_presence) {
    cf_config *cfg = NULL;
    /* Absent VAPID disables push without failing startup. */
    CF_REQUIRE(parse_case(NULL, 0, NULL, &cfg) == CF_OK);
    CF_CHECK(cfg->push_configured == false);
    cf_config_destroy(cfg);

    /* Partial configuration stays startup-valid but push stays disabled. */
    cf_config_entry extra = {"VAPID_PUBLIC_KEY", "public-key"};
    CF_REQUIRE(parse_case(&extra, 1, NULL, &cfg) == CF_OK);
    CF_CHECK(cfg->push_configured == false);
    cf_config_destroy(cfg);

    /* Empty strings are present-but-unusable: push disabled, parse succeeds. */
    cf_config_entry empty[3] = {{"VAPID_PUBLIC_KEY", ""},
                                {"VAPID_PRIVATE_KEY", ""},
                                {"VAPID_SUBJECT", ""}};
    CF_REQUIRE(parse_case(empty, 3, NULL, &cfg) == CF_OK);
    CF_CHECK(cfg->push_configured == false);
    cf_config_destroy(cfg);

    cf_config_entry all[3] = {{"VAPID_PUBLIC_KEY", "pub"},
                              {"VAPID_PRIVATE_KEY", "priv"},
                              {"VAPID_SUBJECT", "mailto:a@b.example"}};
    CF_REQUIRE(parse_case(all, 3, NULL, &cfg) == CF_OK);
    CF_CHECK(cfg->push_configured == true);
    cf_config_destroy(cfg);
}

CF_TEST(config_duplicates_and_unknown) {
    cf_config_error err = {NULL, NULL};
    cf_config *cfg = NULL;
    cf_config_entry dup[] = {
        {"PUBLIC_ORIGIN", "http://localhost:3000"},
        {"SECRET_KEY_BASE", HEX64},
        {"PORT", "1"},
        {"PORT", "2"},
    };
    CF_CHECK(cf_config_parse(dup, 4, &err, &cfg) != CF_OK);
    CF_CHECK(cfg == NULL);
    CF_REQUIRE(err.setting != NULL);
    CF_CHECK(strcmp(err.setting, "PORT") == 0);
    CF_CHECK(strcmp(err.reason, "was selected more than once") == 0);

    cf_config_entry dup2[] = {
        {"PUBLIC_ORIGIN", "http://localhost:3000"},
        {"SECRET_KEY_BASE", HEX64},
        {"PUBLIC_ORIGIN", "http://localhost:3001"},
    };
    CF_CHECK(cf_config_parse(dup2, 3, &err, &cfg) != CF_OK);
    CF_REQUIRE(err.setting != NULL);
    CF_CHECK(strcmp(err.setting, "PUBLIC_ORIGIN") == 0);

    cf_config_entry unknown = {"NOT_A_SETTING", "1"};
    CF_CHECK(cf_config_parse(&unknown, 1, &err, &cfg) != CF_OK);
    CF_CHECK(cfg == NULL);
    CF_REQUIRE(err.setting != NULL);
    CF_CHECK(strcmp(err.setting, "NOT_A_SETTING") == 0);
    CF_CHECK(strcmp(err.reason, "is not a supported setting") == 0);

    /* A successful parse clears the error report. */
    err.setting = "stale";
    err.reason = "stale";
    CF_REQUIRE(parse_case(NULL, 0, &err, &cfg) == CF_OK);
    CF_CHECK(err.setting == NULL && err.reason == NULL);
    cf_config_destroy(cfg);
}

CF_TEST(config_bcrypt_cost) {
    cf_config *cfg = NULL;
    CF_REQUIRE(parse_case(NULL, 0, NULL, &cfg) == CF_OK);
    CF_CHECK(cfg->bcrypt_cost == 12); /* fixed constant, not env-derived */
    cf_config_test_set_bcrypt_cost(cfg, 4); /* test injection */
    CF_CHECK(cfg->bcrypt_cost == 4);
    cf_config_test_set_bcrypt_cost(cfg, 0); /* out of bcrypt range: ignored */
    CF_CHECK(cfg->bcrypt_cost == 4);
    cf_config_test_set_bcrypt_cost(cfg, 32);
    CF_CHECK(cfg->bcrypt_cost == 4);
    cf_config_test_set_bcrypt_cost(NULL, 4); /* accepts NULL */
    cf_config_destroy(cfg);
}

CF_TEST(config_cli_options) {
    char *run_argv[] = {"campfire", NULL};
    char *help_argv[] = {"campfire", "--help", NULL};
    char *version_argv[] = {"campfire", "--version", NULL};
    char *unknown_argv[] = {"campfire", "--port", NULL};
    char *word_argv[] = {"campfire", "serve", NULL};
    char *extra_argv[] = {"campfire", "--help", "extra", NULL};
    cf_config_cli_mode mode = CF_CONFIG_CLI_RUN;

    CF_CHECK(cf_config_parse_cli(1, run_argv, &mode) == CF_OK);
    CF_CHECK(mode == CF_CONFIG_CLI_RUN);
    CF_CHECK(cf_config_parse_cli(2, help_argv, &mode) == CF_OK);
    CF_CHECK(mode == CF_CONFIG_CLI_HELP);
    CF_CHECK(cf_config_parse_cli(2, version_argv, &mode) == CF_OK);
    CF_CHECK(mode == CF_CONFIG_CLI_VERSION);

    /* Only --help and --version are options. */
    CF_CHECK(cf_config_parse_cli(2, unknown_argv, &mode) != CF_OK);
    CF_CHECK(cf_config_parse_cli(2, word_argv, &mode) != CF_OK);
    CF_CHECK(cf_config_parse_cli(3, extra_argv, &mode) != CF_OK);
    CF_CHECK(cf_config_parse_cli(1, NULL, &mode) == CF_INVALID);
    CF_CHECK(cf_config_parse_cli(1, run_argv, NULL) == CF_INVALID);
}

CF_TEST(config_load_reads_environment) {
    CF_REQUIRE(setenv("PUBLIC_ORIGIN", "http://127.0.0.1:3999", 1) == 0);
    CF_REQUIRE(setenv("SECRET_KEY_BASE", HEX64, 1) == 0);
    CF_REQUIRE(setenv("HOST", "127.0.0.1", 1) == 0);
    CF_REQUIRE(setenv("PORT", "3999", 1) == 0);
    CF_REQUIRE(setenv("CF_LOOPS", "4", 1) == 0);
    CF_REQUIRE(setenv("CF_CACHE_BYTES", "0", 1) == 0);

    cf_config *cfg = NULL;
    CF_REQUIRE(cf_config_load(&cfg) == CF_OK);
    CF_REQUIRE(cfg != NULL);
    CF_CHECK(strcmp(cfg->host, "127.0.0.1") == 0);
    CF_CHECK(cfg->port == 3999);
    CF_CHECK(cfg->loops == 4);
    CF_CHECK(cfg->bcrypt_cost == CF_BCRYPT_COST); /* load never injects 4 */
    cf_config_destroy(cfg);

    /* Missing SECRET_KEY_BASE fails load with a NULL result. */
    CF_REQUIRE(unsetenv("SECRET_KEY_BASE") == 0);
    cfg = NULL;
    CF_CHECK(cf_config_load(&cfg) != CF_OK);
    CF_CHECK(cfg == NULL);

    CF_REQUIRE(unsetenv("PUBLIC_ORIGIN") == 0);
    CF_CHECK(cf_config_load(&cfg) != CF_OK);
    CF_CHECK(cfg == NULL);

    (void)unsetenv("HOST");
    (void)unsetenv("PORT");
    (void)unsetenv("CF_LOOPS");
    (void)unsetenv("CF_CACHE_BYTES");
}

CF_TEST(config_parse_argument_errors) {
    cf_config *cfg = NULL;
    cf_config_error err = {NULL, NULL};
    CF_CHECK(cf_config_parse(NULL, 0, &err, NULL) == CF_INVALID);
    cf_config_entry unnamed = {NULL, "1"};
    CF_CHECK(cf_config_parse(&unnamed, 1, &err, &cfg) != CF_OK);
    CF_CHECK(cfg == NULL);
    CF_REQUIRE(err.setting != NULL);
    cf_config_entry null_value = {"PORT", NULL};
    CF_CHECK(cf_config_parse(&null_value, 1, &err, &cfg) != CF_OK);
    CF_REQUIRE(err.setting != NULL);
    CF_CHECK(strcmp(err.setting, "PORT") == 0);
}

CF_TEST_MAIN()
