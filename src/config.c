/* cf_config implementation; see config.h for the contract.
 *
 * The table is parsed in two passes: duplicates/unknown names first, then
 * values. Every failure names the offending setting; values are never echoed,
 * so SECRET_KEY_BASE cannot leak through an error path. */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* All settings in the supported environment surface. */
static const char *const cf_config_known_names[] = {
    "HOST",
    "PORT",
    "PUBLIC_ORIGIN",
    "DATABASE_PATH",
    "STORAGE_PATH",
    "SECRET_KEY_BASE",
    "CF_LOOPS",
    "CF_READERS",
    "CF_REQUEST_SLOTS",
    "CF_WRITER_QUEUE",
    "CF_CONNECTIONS_PER_LOOP",
    "CF_INPUT_BYTES",
    "CF_OUTPUT_BYTES",
    "CF_CACHE_BYTES",
    "CF_JOB_QUEUE",
    "CF_JOB_WORKERS",
    "CF_CRYPTO_WORKERS",
    "DISABLE_SSL",
    "TLS_CERT_FILE",
    "TLS_KEY_FILE",
    "VAPID_PUBLIC_KEY",
    "VAPID_PRIVATE_KEY",
    "VAPID_SUBJECT",
};

#define CF_CONFIG_KNOWN_COUNT \
    (sizeof cf_config_known_names / sizeof cf_config_known_names[0])

static cf_err cf_config_fail(cf_config_error *err, const char *setting,
                             const char *reason) {
    if (err != NULL) {
        err->setting = setting;
        err->reason = reason;
    }
    return CF_INVALID;
}

static void cf_config_note_ok(cf_config_error *err) {
    if (err != NULL) {
        err->setting = NULL;
        err->reason = NULL;
    }
}

static char *cf_config_strdup(const char *s) {
    size_t len = strlen(s);
    char *copy = malloc(len + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, s, len + 1);
    return copy;
}

/* Replaces *field (which must be owned or NULL) with a copy of value. */
static bool cf_config_set_string(char **field, const char *value) {
    char *copy = cf_config_strdup(value);
    if (copy == NULL) return false;
    free(*field);
    *field = copy;
    return true;
}

/* Strict unsigned decimal: no sign, no whitespace, no unit suffix. */
static bool cf_config_parse_u64(const char *s, uint64_t *out) {
    if (s == NULL || *s == '\0') return false;
    uint64_t value = 0;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') return false;
        unsigned digit = (unsigned)(*p - '0');
        if (value > (UINT64_MAX - digit) / 10) return false; /* overflow */
        value = value * 10 + digit;
    }
    *out = value;
    return true;
}

/* Parses a bounded integer setting: digits only, min..max inclusive. The
 * range check also rejects values that would not fit the destination. */
static cf_err cf_config_set_bounded(cf_config_error *err, const char *name,
                                    const char *value, uint64_t min,
                                    uint64_t max, size_t *out) {
    uint64_t parsed = 0;
    if (!cf_config_parse_u64(value, &parsed)) {
        return cf_config_fail(err, name,
                              "must be a non-negative decimal integer");
    }
    if (parsed < min || parsed > max) {
        return cf_config_fail(err, name, "is outside the supported range");
    }
    *out = (size_t)parsed;
    return CF_OK;
}

static bool cf_config_hex_secret_ok(const char *s) {
    if (s == NULL) return false;
    size_t len = strlen(s);
    if (len < CF_CONFIG_SECRET_HEX_MIN) return false;
    for (const char *p = s; *p != '\0'; p++) {
        char c = *p;
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                   (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

static bool cf_config_origin_port_ok(const char *s, const char **reason) {
    unsigned long value = 0;
    if (*s == '\0') {
        *reason = "has an empty port";
        return false;
    }
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9') {
            *reason = "has a non-numeric port";
            return false;
        }
        value = value * 10 + (unsigned long)(*s - '0');
        if (value > CF_CONFIG_PORT_MAX) {
            *reason = "has a port outside 1..65535";
            return false;
        }
    }
    if (value < CF_CONFIG_PORT_MIN) {
        *reason = "has a port outside 1..65535";
        return false;
    }
    return true;
}

/* Absolute origin only: scheme://host[:port], no path/query/fragment/userinfo. */
static bool cf_config_origin_ok(const char *s, const char **reason) {
    const char *authority;
    if (strncmp(s, "http://", 7) == 0) {
        authority = s + 7;
    } else if (strncmp(s, "https://", 8) == 0) {
        authority = s + 8;
    } else {
        *reason = "must be an absolute http:// or https:// origin";
        return false;
    }
    if (*authority == '\0') {
        *reason = "has an empty host";
        return false;
    }
    for (const char *p = authority; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '/') {
            *reason = "must not include a path";
            return false;
        }
        if (c == '?') {
            *reason = "must not include a query";
            return false;
        }
        if (c == '#') {
            *reason = "must not include a fragment";
            return false;
        }
        if (c == '@') {
            *reason = "must not include userinfo";
            return false;
        }
        if (c <= 0x20 || c == 0x7f) {
            *reason = "contains whitespace or control characters";
            return false;
        }
    }

    if (*authority == '[') {
        const char *close = strchr(authority, ']');
        if (close == NULL || close == authority + 1) {
            *reason = "has a malformed bracketed host";
            return false;
        }
        if (close[1] == '\0') return true;
        if (close[1] != ':') {
            *reason = "has trailing characters after the host";
            return false;
        }
        return cf_config_origin_port_ok(close + 2, reason);
    }

    const char *colon = strchr(authority, ':');
    if (colon == NULL) return true;
    if (colon == authority) {
        *reason = "has an empty host";
        return false;
    }
    if (strchr(colon + 1, ':') != NULL) {
        *reason = "must bracket an IPv6 host";
        return false;
    }
    return cf_config_origin_port_ok(colon + 1, reason);
}

static cf_err cf_config_apply(cf_config *config, const char *name,
                              const char *value, cf_config_error *err) {
    if (value == NULL) {
        return cf_config_fail(err, name, "has no value");
    }

    if (strcmp(name, "HOST") == 0) {
        if (*value == '\0') return cf_config_fail(err, name, "must not be empty");
        if (!cf_config_set_string(&config->host, value)) {
            return cf_config_fail(err, name, "out of memory");
        }
        return CF_OK;
    }
    if (strcmp(name, "PORT") == 0) {
        size_t port = 0;
        cf_err rc = cf_config_set_bounded(err, name, value, CF_CONFIG_PORT_MIN,
                                          CF_CONFIG_PORT_MAX, &port);
        if (rc != CF_OK) return rc;
        config->port = (unsigned)port;
        return CF_OK;
    }
    if (strcmp(name, "PUBLIC_ORIGIN") == 0) {
        const char *reason = NULL;
        if (!cf_config_origin_ok(value, &reason)) {
            return cf_config_fail(err, name, reason);
        }
        if (!cf_config_set_string(&config->public_origin, value)) {
            return cf_config_fail(err, name, "out of memory");
        }
        return CF_OK;
    }
    if (strcmp(name, "DATABASE_PATH") == 0) {
        if (*value == '\0') return cf_config_fail(err, name, "must not be empty");
        if (!cf_config_set_string(&config->database_path, value)) {
            return cf_config_fail(err, name, "out of memory");
        }
        return CF_OK;
    }
    if (strcmp(name, "STORAGE_PATH") == 0) {
        if (*value == '\0') return cf_config_fail(err, name, "must not be empty");
        if (!cf_config_set_string(&config->storage_path, value)) {
            return cf_config_fail(err, name, "out of memory");
        }
        return CF_OK;
    }
    if (strcmp(name, "SECRET_KEY_BASE") == 0) {
        if (!cf_config_hex_secret_ok(value)) {
            return cf_config_fail(
                err, name,
                "must be at least 64 hexadecimal characters");
        }
        if (!cf_config_set_string(&config->secret_key_base, value)) {
            return cf_config_fail(err, name, "out of memory");
        }
        config->secret_key_base_len = strlen(value);
        return CF_OK;
    }
    if (strcmp(name, "CF_LOOPS") == 0) {
        return cf_config_set_bounded(err, name, value, 1, CF_CONFIG_LOOPS_MAX,
                                     &config->loops);
    }
    if (strcmp(name, "CF_READERS") == 0) {
        return cf_config_set_bounded(err, name, value, 1, CF_CONFIG_READERS_MAX,
                                     &config->readers);
    }
    if (strcmp(name, "CF_REQUEST_SLOTS") == 0) {
        return cf_config_set_bounded(err, name, value, 1, SIZE_MAX,
                                     &config->request_slots);
    }
    if (strcmp(name, "CF_WRITER_QUEUE") == 0) {
        return cf_config_set_bounded(err, name, value, 1, SIZE_MAX,
                                     &config->writer_queue);
    }
    if (strcmp(name, "CF_CONNECTIONS_PER_LOOP") == 0) {
        return cf_config_set_bounded(err, name, value, 1, SIZE_MAX,
                                     &config->connections_per_loop);
    }
    if (strcmp(name, "CF_INPUT_BYTES") == 0) {
        return cf_config_set_bounded(err, name, value, 1, SIZE_MAX,
                                     &config->input_bytes);
    }
    if (strcmp(name, "CF_OUTPUT_BYTES") == 0) {
        return cf_config_set_bounded(err, name, value, 1, SIZE_MAX,
                                     &config->output_bytes);
    }
    if (strcmp(name, "CF_CACHE_BYTES") == 0) {
        return cf_config_set_bounded(err, name, value, 0, SIZE_MAX,
                                     &config->cache_bytes);
    }
    if (strcmp(name, "CF_JOB_QUEUE") == 0) {
        return cf_config_set_bounded(err, name, value, 1, SIZE_MAX,
                                     &config->job_queue);
    }
    if (strcmp(name, "CF_JOB_WORKERS") == 0) {
        return cf_config_set_bounded(err, name, value, 1, SIZE_MAX,
                                     &config->job_workers);
    }
    if (strcmp(name, "CF_CRYPTO_WORKERS") == 0) {
        return cf_config_set_bounded(err, name, value, 1, SIZE_MAX,
                                     &config->crypto_workers);
    }
    if (strcmp(name, "DISABLE_SSL") == 0) {
        if (strcmp(value, "0") == 0) {
            config->disable_ssl = false;
            return CF_OK;
        }
        if (strcmp(value, "1") == 0) {
            config->disable_ssl = true;
            return CF_OK;
        }
        return cf_config_fail(err, name, "must be 0 or 1");
    }
    if (strcmp(name, "TLS_CERT_FILE") == 0) {
        if (*value == '\0') return cf_config_fail(err, name, "must not be empty");
        if (!cf_config_set_string(&config->tls_cert_file, value)) {
            return cf_config_fail(err, name, "out of memory");
        }
        return CF_OK;
    }
    if (strcmp(name, "TLS_KEY_FILE") == 0) {
        if (*value == '\0') return cf_config_fail(err, name, "must not be empty");
        if (!cf_config_set_string(&config->tls_key_file, value)) {
            return cf_config_fail(err, name, "out of memory");
        }
        return CF_OK;
    }
    if (strcmp(name, "VAPID_PUBLIC_KEY") == 0) {
        if (!cf_config_set_string(&config->vapid_public_key, value)) {
            return cf_config_fail(err, name, "out of memory");
        }
        return CF_OK;
    }
    if (strcmp(name, "VAPID_PRIVATE_KEY") == 0) {
        if (!cf_config_set_string(&config->vapid_private_key, value)) {
            return cf_config_fail(err, name, "out of memory");
        }
        return CF_OK;
    }
    if (strcmp(name, "VAPID_SUBJECT") == 0) {
        if (!cf_config_set_string(&config->vapid_subject, value)) {
            return cf_config_fail(err, name, "out of memory");
        }
        return CF_OK;
    }
    return cf_config_fail(err, name, "is not a supported setting");
}

static bool cf_config_name_known(const char *name) {
    for (size_t i = 0; i < CF_CONFIG_KNOWN_COUNT; i++) {
        if (strcmp(cf_config_known_names[i], name) == 0) return true;
    }
    return false;
}

void cf_config_destroy(cf_config *config) {
    if (config == NULL) return;
    if (config->secret_key_base != NULL) {
        memset(config->secret_key_base, 0, config->secret_key_base_len);
    }
    free(config->host);
    free(config->public_origin);
    free(config->database_path);
    free(config->storage_path);
    free(config->secret_key_base);
    free(config->tls_cert_file);
    free(config->tls_key_file);
    free(config->vapid_public_key);
    free(config->vapid_private_key);
    free(config->vapid_subject);
    memset(config, 0, sizeof *config);
    free(config);
}

void cf_config_test_set_bcrypt_cost(cf_config *config, int cost) {
    if (config == NULL) return;
    if (cost < 4 || cost > 31) return; /* outside bcrypt's valid range */
    config->bcrypt_cost = cost;
}

cf_err cf_config_parse(const cf_config_entry *entries, size_t count,
                       cf_config_error *err, cf_config **out) {
    if (out == NULL) return CF_INVALID;
    *out = NULL;
    cf_config_note_ok(err);
    if (count != 0 && entries == NULL) {
        return cf_config_fail(err, "config", "has no selection table");
    }

    /* Pass 1: reject unknown and duplicate names before touching values. */
    for (size_t i = 0; i < count; i++) {
        const char *name = entries[i].name;
        if (name == NULL || *name == '\0') {
            return cf_config_fail(err, "config", "has an unnamed setting");
        }
        for (size_t j = 0; j < i; j++) {
            if (entries[j].name != NULL &&
                strcmp(entries[j].name, name) == 0) {
                return cf_config_fail(err, name, "was selected more than once");
            }
        }
        if (!cf_config_name_known(name)) {
            return cf_config_fail(err, name, "is not a supported setting");
        }
    }

    cf_config *config = calloc(1, sizeof *config);
    if (config == NULL) return CF_NOMEM;
    config->port = CF_CONFIG_DEFAULT_PORT;
    config->loops = CF_CONFIG_DEFAULT_LOOPS;
    config->readers = CF_CONFIG_DEFAULT_READERS;
    config->request_slots = CF_CONFIG_DEFAULT_REQUEST_SLOTS;
    config->writer_queue = CF_CONFIG_DEFAULT_WRITER_QUEUE;
    config->connections_per_loop = CF_CONFIG_DEFAULT_CONNECTIONS_PER_LOOP;
    config->input_bytes = CF_CONFIG_DEFAULT_INPUT_BYTES;
    config->output_bytes = CF_CONFIG_DEFAULT_OUTPUT_BYTES;
    config->cache_bytes = CF_CONFIG_DEFAULT_CACHE_BYTES;
    config->job_queue = CF_CONFIG_DEFAULT_JOB_QUEUE;
    config->job_workers = CF_CONFIG_DEFAULT_JOB_WORKERS;
    config->crypto_workers = CF_CONFIG_DEFAULT_CRYPTO_WORKERS;
    config->disable_ssl = true; /* 01: DISABLE_SSL defaults to 1 until P01 */
    config->bcrypt_cost = CF_BCRYPT_COST;
    if (!cf_config_set_string(&config->host, CF_CONFIG_DEFAULT_HOST) ||
        !cf_config_set_string(&config->database_path,
                              CF_CONFIG_DEFAULT_DATABASE_PATH) ||
        !cf_config_set_string(&config->storage_path,
                              CF_CONFIG_DEFAULT_STORAGE_PATH)) {
        cf_config_destroy(config);
        return cf_config_fail(err, "config", "out of memory");
    }

    /* Pass 2: apply values. */
    for (size_t i = 0; i < count; i++) {
        cf_err rc = cf_config_apply(config, entries[i].name, entries[i].value,
                                    err);
        if (rc != CF_OK) {
            cf_config_destroy(config);
            return rc;
        }
    }

    /* Cross-setting rules. */
    if (config->public_origin == NULL) {
        cf_config_destroy(config);
        return cf_config_fail(err, "PUBLIC_ORIGIN", "is required");
    }
    if (config->secret_key_base == NULL) {
        cf_config_destroy(config);
        return cf_config_fail(err, "SECRET_KEY_BASE", "is required");
    }
    if (config->tls_cert_file == NULL && config->tls_key_file != NULL) {
        cf_config_destroy(config);
        return cf_config_fail(err, "TLS_CERT_FILE",
                              "is required when TLS_KEY_FILE is set");
    }
    if (config->tls_key_file == NULL && config->tls_cert_file != NULL) {
        cf_config_destroy(config);
        return cf_config_fail(err, "TLS_KEY_FILE",
                              "is required when TLS_CERT_FILE is set");
    }
    if (!config->disable_ssl &&
        (config->tls_cert_file == NULL || config->tls_key_file == NULL)) {
        const char *missing =
            config->tls_cert_file == NULL ? "TLS_CERT_FILE" : "TLS_KEY_FILE";
        cf_config_destroy(config);
        return cf_config_fail(err, missing,
                              "is required when DISABLE_SSL=0");
    }
    config->push_configured = config->vapid_public_key != NULL &&
                              config->vapid_private_key != NULL &&
                              config->vapid_subject != NULL &&
                              config->vapid_public_key[0] != '\0' &&
                              config->vapid_private_key[0] != '\0' &&
                              config->vapid_subject[0] != '\0';

    cf_config_note_ok(err);
    *out = config;
    return CF_OK;
}

cf_err cf_config_load(cf_config **out) {
    if (out == NULL) return CF_INVALID;
    *out = NULL;

    cf_config_entry entries[CF_CONFIG_KNOWN_COUNT];
    size_t count = 0;
    for (size_t i = 0; i < CF_CONFIG_KNOWN_COUNT; i++) {
        const char *value = getenv(cf_config_known_names[i]);
        if (value == NULL) continue;
        entries[count].name = cf_config_known_names[i];
        entries[count].value = value;
        count++;
    }

    cf_config_error err;
    cf_err rc = cf_config_parse(entries, count, &err, out);
    if (rc != CF_OK) {
        if (err.setting != NULL) {
            fprintf(stderr, "campfire: config: %s: %s\n", err.setting,
                    err.reason != NULL ? err.reason : "is invalid");
        } else {
            fprintf(stderr, "campfire: config: invalid configuration\n");
        }
    }
    return rc;
}

cf_err cf_config_parse_cli(int argc, char **argv, cf_config_cli_mode *mode) {
    if (mode == NULL || argc < 1 || argv == NULL) return CF_INVALID;
    *mode = CF_CONFIG_CLI_RUN;
    if (argc == 1) return CF_OK;
    if (argc == 2 && argv[1] != NULL && strcmp(argv[1], "--help") == 0) {
        *mode = CF_CONFIG_CLI_HELP;
        return CF_OK;
    }
    if (argc == 2 && argv[1] != NULL && strcmp(argv[1], "--version") == 0) {
        *mode = CF_CONFIG_CLI_VERSION;
        return CF_OK;
    }
    /* Only --help and --version are initial CLI options. */
    return CF_INVALID;
}
