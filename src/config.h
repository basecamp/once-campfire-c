/* Configuration: the fixed environment table of
 * docs/devel/implementation/01-foundation-http.md ("Configuration: fixed
 * defaults"). Values are parsed once into an immutable cf_config; invalid,
 * negative, overflowing or duplicate selections fail startup naming the
 * setting. Only --help and --version are CLI options. No config hot reload.
 *
 * SECRET_KEY_BASE is held only as opaque key material: it is never written to
 * a log and is zeroed before its allocation is released. Logging elsewhere
 * must not print it (00-contracts.md "C rules"). */
#ifndef CF_CONFIG_H
#define CF_CONFIG_H

#include "cf.h"

/* Fixed bcrypt cost (01: "Fix bcrypt cost at 12"). Tests inject 4 through
 * cf_config_test_set_bcrypt_cost; the value is never inferred from an
 * environment name. */
#define CF_BCRYPT_COST 12

/* Table defaults and bounds. Byte capacities are decimal bytes in the
 * environment; MiB defaults below are exact multiples of 1048576. */
#define CF_CONFIG_DEFAULT_PORT 3000
#define CF_CONFIG_PORT_MIN 1
#define CF_CONFIG_PORT_MAX 65535
#define CF_CONFIG_DEFAULT_HOST "0.0.0.0"
#define CF_CONFIG_DEFAULT_DATABASE_PATH "storage/campfire.sqlite3"
#define CF_CONFIG_DEFAULT_STORAGE_PATH "storage/files"
#define CF_CONFIG_SECRET_HEX_MIN 64
#define CF_CONFIG_DEFAULT_LOOPS 1
#define CF_CONFIG_LOOPS_MAX 64
#define CF_CONFIG_DEFAULT_READERS 4
#define CF_CONFIG_READERS_MAX 64
#define CF_CONFIG_DEFAULT_REQUEST_SLOTS 256
#define CF_CONFIG_DEFAULT_WRITER_QUEUE 256
#define CF_CONFIG_DEFAULT_CONNECTIONS_PER_LOOP 2048
#define CF_CONFIG_MIB ((size_t)1048576)
#define CF_CONFIG_DEFAULT_INPUT_BYTES (64 * CF_CONFIG_MIB)
#define CF_CONFIG_DEFAULT_OUTPUT_BYTES (64 * CF_CONFIG_MIB)
#define CF_CONFIG_DEFAULT_CACHE_BYTES 0
#define CF_CONFIG_DEFAULT_JOB_QUEUE 128
#define CF_CONFIG_DEFAULT_JOB_WORKERS 2
#define CF_CONFIG_DEFAULT_CRYPTO_WORKERS 2

struct cf_config {
    /* HOST / PORT: listener address, copied verbatim. */
    char *host;
    unsigned port;
    /* PUBLIC_ORIGIN: absolute http/https origin, no path/query/userinfo. */
    char *public_origin;
    char *database_path;
    char *storage_path;
    /* SECRET_KEY_BASE: >= 64 hex characters. Never logged; zeroed on destroy. */
    char *secret_key_base;
    size_t secret_key_base_len;
    /* Bounded capacities. */
    size_t loops;                /* CF_LOOPS, 1..64 */
    size_t readers;              /* CF_READERS, 1..64 */
    size_t request_slots;        /* CF_REQUEST_SLOTS, >0 */
    size_t writer_queue;         /* CF_WRITER_QUEUE, >0 */
    size_t connections_per_loop; /* CF_CONNECTIONS_PER_LOOP, >0 */
    size_t input_bytes;          /* CF_INPUT_BYTES, >0 */
    size_t output_bytes;         /* CF_OUTPUT_BYTES, >0 */
    size_t cache_bytes;          /* CF_CACHE_BYTES, >=0 (0 disables) */
    size_t job_queue;            /* CF_JOB_QUEUE, >0 */
    size_t job_workers;          /* CF_JOB_WORKERS, >0 */
    size_t crypto_workers;       /* CF_CRYPTO_WORKERS, >0 */
    /* TLS: DISABLE_SSL=1 until P01; 0 requires both certificate files. */
    bool disable_ssl;
    char *tls_cert_file;
    char *tls_key_file;
    /* Push (I02): optional; absent or empty disables push. Key validation is
     * I02's contract (invalid keys log one sanitized diagnostic and disable
     * push); F03 only records presence. */
    char *vapid_public_key;
    char *vapid_private_key;
    char *vapid_subject;
    bool push_configured;
    int bcrypt_cost; /* CF_BCRYPT_COST in production builds */
};

/* One selection in an ordered configuration table. name is never NULL. */
typedef struct {
    const char *name;
    const char *value;
} cf_config_entry;

/* Failure report from cf_config_parse: the offending setting name (a static
 * string or entries[].name, never the secret value) and a short reason. */
typedef struct {
    const char *setting;
    const char *reason;
} cf_config_error;

/* CLI classification. --help and --version are the only options; everything
 * else fails and startup must not continue. */
typedef enum {
    CF_CONFIG_CLI_RUN = 0,
    CF_CONFIG_CLI_HELP,
    CF_CONFIG_CLI_VERSION
} cf_config_cli_mode;

/* Classify argv (argv[0] is the program name). Any unknown option or extra
 * argument returns CF_INVALID. mode must not be NULL. */
cf_err cf_config_parse_cli(int argc, char **argv, cf_config_cli_mode *mode);

/* Parse an explicit ordered selection table. Unknown names and duplicate
 * names fail naming the setting; every value is validated per the table.
 * On success *out owns a complete config. On failure *out stays NULL and,
 * when err != NULL, err->setting/err->reason are set. */
cf_err cf_config_parse(const cf_config_entry *entries, size_t count,
                       cf_config_error *err, cf_config **out);

/* Production entry point: parses the process environment exactly once. On
 * failure *out is NULL and one sanitized diagnostic naming the setting is
 * written to stderr (never the value of SECRET_KEY_BASE or any other value). */
cf_err cf_config_load(cf_config **out);

/* Destroy and release a config (accepts NULL). Zeroes SECRET_KEY_BASE first. */
void cf_config_destroy(cf_config *config);

/* Test-only injection of the bcrypt cost (the test runner injects 4).
 * Production cf_config_load always leaves CF_BCRYPT_COST. Values outside
 * 4..31 are ignored. Never inferred from an environment name. */
void cf_config_test_set_bcrypt_cost(cf_config *config, int cost);

#endif /* CF_CONFIG_H */
