/* campfire: boot, serving path and orderly shutdown (F03 lifecycle extended
 * by A00).
 *
 * Boot: classify CLI options, load the fixed environment configuration, create
 * the app, start the writer and the request-worker pool, bind the listener,
 * create one H01 loop per CF_LOOPS and run them on their own threads, then
 * block SIGINT/SIGTERM with pthread_sigmask and wait with sigwait.
 *
 * Shutdown order (00-contracts.md CORE-05, 01 H01, A00): signal all loops to
 * stop (they drain admitted tasks and pending output for up to five seconds),
 * join the loop threads, stop the app (workers abandon queued tasks and exit,
 * writer released), destroy the loops and the listener, then cf_app_destroy
 * joins any remaining worker before freeing app/config. SIGINT/SIGTERM are
 * blocked before any thread exists, so sigwait is their only consumer and a
 * request delivered at any point stays pending until consumed. */
#include "app.h"
#include "app_internal.h"
#include "cf.h"
#include "config.h"
#include "http/http.h"
#include "richtext.h"

#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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

/* Bind and listen on config->host:config->port. Returns the listening fd or
 * -1 with a sanitized message in errbuf. */
static int cf_serve_listen(const cf_config *config, char *errbuf,
                           size_t errcap) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG;
    char port_text[16];
    snprintf(port_text, sizeof port_text, "%u", config->port);

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(config->host, port_text, &hints, &res);
    if (gai != 0) {
        snprintf(errbuf, errcap, "%s", gai_strerror(gai));
        return -1;
    }
    int fd = -1;
    int last_errno = 0;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        int sock = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC,
                          ai->ai_protocol);
        if (sock < 0) {
            last_errno = errno;
            continue;
        }
        int one = 1;
        (void)setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(sock, ai->ai_addr, ai->ai_addrlen) == 0 &&
            listen(sock, 128) == 0) {
            fd = sock;
            break;
        }
        last_errno = errno;
        close(sock);
    }
    freeaddrinfo(res);
    if (fd < 0) {
        snprintf(errbuf, errcap, "%s",
                 last_errno != 0 ? strerror(last_errno) : "no address");
    }
    return fd;
}

static void *cf_loop_run_main(void *arg) {
    cf_http_loop *loop = arg;
    (void)cf_http_loop_run(loop);
    return NULL;
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

    /* TLS with configured certificate files is P01; DISABLE_SSL must stay 1
     * until then (01-foundation-http.md D-C06). */
    if (!config->disable_ssl) {
        fprintf(stderr,
                "campfire: startup failed: TLS serving is not implemented "
                "until P01 (set DISABLE_SSL=1)\n");
        cf_config_destroy(config);
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
    const cf_config *cfg = cf_app_config(app);

    /* R02: signed mentions/attachments need the configured secret before any
     * request is dispatched (cf_richtext_configure; borrowed from config). */
    {
        cf_span richtext_secret = {
            (const unsigned char *)cfg->secret_key_base,
            cfg->secret_key_base_len,
        };
        cf_richtext_configure(richtext_secret);
    }

    if (cf_app_start(app) != CF_OK) {
        fprintf(stderr,
                "campfire: startup failed: writer or request workers\n");
        cf_app_destroy(app);
        return 1;
    }

    char errbuf[128];
    int listen_fd = cf_serve_listen(cfg, errbuf, sizeof errbuf);
    if (listen_fd < 0) {
        fprintf(stderr, "campfire: startup failed: listen on %s:%u: %s\n",
                cfg->host, cfg->port, errbuf);
        cf_app_stop(app);
        cf_app_destroy(app);
        return 1;
    }

    size_t loop_count = cfg->loops;
    cf_http_loop **loops = calloc(loop_count, sizeof *loops);
    pthread_t *loop_threads = calloc(loop_count, sizeof *loop_threads);
    if (loops == NULL || loop_threads == NULL) {
        fprintf(stderr, "campfire: startup failed: loop allocation\n");
        free(loops);
        free(loop_threads);
        close(listen_fd);
        cf_app_stop(app);
        cf_app_destroy(app);
        return 1;
    }
    size_t loops_created = 0;
    size_t threads_created = 0;
    int exit_code = 0;

    for (size_t i = 0; i < loop_count; i++) {
        cf_http_loop_config loop_config;
        memset(&loop_config, 0, sizeof loop_config);
        loop_config.listen_fd = listen_fd;
        loop_config.loop_index = (uint32_t)i;
        loop_config.public_origin = cfg->public_origin;
        loop_config.connections_per_loop = cfg->connections_per_loop;
        loop_config.input_bytes = cf_app_loop_input_bytes(app, i);
        loop_config.output_bytes = cf_app_loop_output_bytes(app, i);
        loop_config.app = app;
        loop_config.admit = cf_app_admit;
        loop_config.admit_user = app;
        if (cf_http_loop_create(&loop_config, &loops[i]) != CF_OK) {
            fprintf(stderr, "campfire: startup failed: loop %zu creation\n",
                    i);
            exit_code = 1;
            goto shutdown;
        }
        loops_created++;
    }
    for (size_t i = 0; i < loop_count; i++) {
        if (pthread_create(&loop_threads[i], NULL, cf_loop_run_main,
                           loops[i]) != 0) {
            fprintf(stderr,
                    "campfire: startup failed: loop %zu thread creation\n",
                    i);
            exit_code = 1;
            goto shutdown;
        }
        threads_created++;
    }

    fprintf(stderr,
            "campfire: serving: host=%s port=%u loops=%zu readers=%zu "
            "request_slots=%zu\n",
            cfg->host, cfg->port, cfg->loops, cfg->readers,
            cfg->request_slots);

    int signo = 0;
    int wait_rc;
    do {
        wait_rc = sigwait(&shutdown_signals, &signo);
    } while (wait_rc == EINTR);
    if (wait_rc != 0) {
        fprintf(stderr, "campfire: shutdown: wait failed: %s\n",
                strerror(wait_rc));
        exit_code = 1;
    } else {
        fprintf(stderr, "campfire: shutdown: signal=%d requested\n", signo);
    }

shutdown:
    /* Stop accepting and drain admitted tasks first (H01), then stop the
     * workers, then destroy the loops; cf_app_destroy frees app/config. */
    for (size_t i = 0; i < loops_created; i++) {
        cf_http_loop_stop(loops[i]);
    }
    for (size_t i = 0; i < threads_created; i++) {
        (void)pthread_join(loop_threads[i], NULL);
    }
    cf_app_stop(app);
    for (size_t i = 0; i < loops_created; i++) {
        cf_http_loop_destroy(loops[i]);
    }
    close(listen_fd);
    free(loops);
    free(loop_threads);
    cf_app_destroy(app); /* joins any remaining worker before freeing state */
    fprintf(stderr, "campfire: shutdown: complete\n");
    return exit_code;
}
