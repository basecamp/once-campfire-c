/* campfire: boot and orderly shutdown.
 *
 * F03 owns the lifecycle only. The serving path (listener, routes, workers)
 * is integrated later by H01/A00; this binary must not invent one. Boot:
 * classify CLI options, load the fixed environment configuration, create the
 * minimal cf_app, ignore SIGPIPE, block SIGINT/SIGTERM and wait for a
 * shutdown request with sigwait. Shutdown joins workers before app state is
 * freed and exits 0.
 *
 * SIGINT/SIGTERM are blocked with pthread_sigmask before any worker thread
 * can exist, so a request delivered at any point (even between the boot log
 * and the wait) stays pending until sigwait consumes it. No signal handler
 * runs, so no request can be lost to a handler/wait race. */
#include "app.h"
#include "app_internal.h"
#include "cf.h"
#include "config.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#define CF_CAMPFIRE_VERSION "0.1.0"

static void cf_print_usage(FILE *out) {
    fprintf(out,
            "usage: campfire [--help] [--version]\n"
            "\n"
            "The Campfire C port. Configuration is read once from the fixed\n"
            "environment table in docs/devel/implementation/01-foundation-http.md;\n"
            "there is no config reload. --help and --version are the only CLI options.\n");
}

/* Blocks SIGINT/SIGTERM in the calling thread; threads created later inherit
 * the mask, so sigwait is the only consumer of these signals.  Returns 0 or
 * the pthread error code. */
static int cf_block_shutdown_signals(sigset_t *set) {
    sigemptyset(set);
    sigaddset(set, SIGINT);
    sigaddset(set, SIGTERM);
    return pthread_sigmask(SIG_BLOCK, set, NULL);
}

static int cf_ignore_sigpipe(void) {
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) return -1;
    return 0;
}

int main(int argc, char **argv) {
    cf_config_cli_mode cli = CF_CONFIG_CLI_RUN;
    if (cf_config_parse_cli(argc, argv, &cli) != CF_OK) {
        fprintf(stderr, "campfire: unrecognized option or argument\n");
        cf_print_usage(stderr);
        return 2;
    }
    if (cli == CF_CONFIG_CLI_HELP) {
        cf_print_usage(stdout);
        return 0;
    }
    if (cli == CF_CONFIG_CLI_VERSION) {
        printf("campfire %s\n", CF_CAMPFIRE_VERSION);
        return 0;
    }

    /* Block the shutdown signals before anything can create a thread. */
    sigset_t shutdown_signals;
    int sig_rc = cf_block_shutdown_signals(&shutdown_signals);
    if (sig_rc != 0) {
        fprintf(stderr, "campfire: startup failed: block SIGINT/SIGTERM: %s\n",
                strerror(sig_rc));
        return 1;
    }
    if (cf_ignore_sigpipe() != 0) {
        fprintf(stderr, "campfire: startup failed: ignore SIGPIPE: %s\n",
                strerror(errno));
        return 1;
    }

    cf_config *config = NULL;
    if (cf_config_load(&config) != CF_OK) {
        fprintf(stderr, "campfire: startup failed: invalid configuration\n");
        return 1;
    }

    /* Sanitized boot log: no secret, cookie, key or body values. */
    fprintf(stderr,
            "campfire: config: host=%s port=%u database=%s storage=%s "
            "loops=%zu readers=%zu request_slots=%zu cache_bytes=%zu ssl=%s\n",
            config->host, config->port, config->database_path,
            config->storage_path, config->loops, config->readers,
            config->request_slots, config->cache_bytes,
            config->disable_ssl ? "disabled" : "configured");

    cf_app *app = NULL;
    if (cf_app_create(config, &app) != CF_OK) {
        fprintf(stderr, "campfire: startup failed: app creation\n");
        cf_config_destroy(config); /* create did not take ownership */
        return 1;
    }
    config = NULL; /* owned by app now */

    fprintf(stderr,
            "campfire: boot: app created (data_version=%llu); serving path "
            "is not integrated yet (H01/A00), waiting for SIGINT/SIGTERM\n",
            (unsigned long long)cf_data_version(app));

    /* H01/A00 replace this wait with the serving loops. Blocking here is the
     * whole lifecycle for F03; SIGINT/SIGTERM are blocked, so sigwait cannot
     * miss a request delivered during boot. */
    int signo = 0;
    int wait_rc;
    do {
        wait_rc = sigwait(&shutdown_signals, &signo);
    } while (wait_rc == EINTR);
    if (wait_rc != 0) {
        fprintf(stderr, "campfire: shutdown: wait failed: %s\n",
                strerror(wait_rc));
        cf_app_request_stop(app);
        cf_app_destroy(app);
        return 1;
    }

    fprintf(stderr, "campfire: shutdown: signal=%d requested\n", signo);
    cf_app_request_stop(app);
    cf_app_destroy(app); /* joins workers before freeing config/queues */
    fprintf(stderr, "campfire: shutdown: complete\n");
    return 0;
}
