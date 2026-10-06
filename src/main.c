/* campfire: boot, serving path and orderly shutdown (F03 lifecycle extended
 * by A00).
 *
 * Boot: classify CLI options, load the fixed environment configuration, create
 * the app, start the writer and the request-worker pool, bind the listener,
 * create one H01 loop per CF_LOOPS and run them on their own threads, then
 * block SIGINT/SIGTERM with pthread_sigmask and wait with sigwait.
 *
 * Shutdown order (00-contracts.md CORE-05, 01 H01, A00, 04 C03): signal all
 * loops to stop (they drain admitted tasks and pending output for up to five
 * seconds), then cf_cable_server_stop (reactors join and submit their
 * upgrades' lease releases), join the loop threads (the releases complete
 * there), destroy the cable server, stop the app (workers abandon queued HTTP
 * tasks and drain submitted closures, writer released), destroy the loops and
 * the listener, then cf_app_destroy joins any remaining worker before freeing
 * app/config. SIGINT/SIGTERM are blocked before any thread exists, so sigwait
 * is their only consumer and a request delivered at any point stays pending
 * until consumed. */
#include "app.h"
#include "app_internal.h"
#include "cable/channels.h"
#include "cable/revocation.h"
#include "db/writer.h"
#include "cf.h"
#include "config.h"
#include "front/tls.h"
#include "http/http.h"
#include "http/http_internal.h" /* P01: cf_http_loop_set_tls_server */
#include "integrations/http.h"
#include "jobs/handlers.h"
#include "jobs/jobs.h"
#include "richtext.h"
#include "views.h"

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

    /* P01: DISABLE_SSL/TLS file validation lives in cf_config_parse
     * (DISABLE_SSL=0 requires both TLS_CERT_FILE and TLS_KEY_FILE); the
     * front server context is created after the listener, below. */

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

    /* A02: load the pinned asset manifest + import map once at boot; every
     * asset-bearing render needs them (cf_views_assets_configure falls back
     * to cf_static_root() when passed NULL). */
    if (cf_views_assets_configure(NULL) != CF_OK) {
        fprintf(stderr,
                "campfire: startup failed: view assets (static root)\n");
        cf_app_destroy(app);
        return 1;
    }

    if (cf_app_start(app) != CF_OK) {
        fprintf(stderr,
                "campfire: startup failed: writer or request workers\n");
        cf_app_destroy(app);
        return 1;
    }

    /* J01/J02: bounded per-kind job queues and the model-event handlers.
     * Started once the writer pool exists; the writer-consumer and handler
     * registration happens below once the cable exists, before serving. */
    cf_jobs *jobs = NULL;
    if (cf_jobs_start(cfg, &jobs) != CF_OK) {
        fprintf(stderr, "campfire: startup failed: job queues\n");
        cf_app_stop(app);
        cf_app_destroy(app);
        return 1;
    }
    /* I01: process-lifetime libcurl global (pthread_once; no destructor). */
    cf_http_global_init();
    /* Handler context: main's frame outlives every worker (loops join before
     * return), so this borrow is process-lifetime. `cable` is filled in once
     * the cable exists below. */
    cf_jobs_handler_ctx jobs_hctx;
    memset(&jobs_hctx, 0, sizeof jobs_hctx);
    jobs_hctx.app = app;

    /* C02: the application cable (channels, subscriptions, broadcasts) and
     * its /cable front mount. The loop upgrade seam owns the WebSocket
     * transport; authentication and command dispatch are this cable's
     * hooks. */
    cf_cable *cable = NULL;
    cf_cable_server *cable_server = NULL;
    {
        cf_cable_config cable_config;
        memset(&cable_config, 0, sizeof cable_config);
        cable_config.app = app;
        /* The cable's shared reader pool is bounded by the loop count (one
         * reader per reactor), never by connections. */
        cable_config.loops = cfg->loops;
        if (cf_cable_create(&cable_config, &cable) != CF_OK) {
            fprintf(stderr, "campfire: startup failed: cable creation\n");
            cf_jobs_stop(jobs);
            cf_jobs_destroy(jobs);
            cf_app_stop(app);
            cf_app_destroy(app);
            return 1;
        }
        cf_cable_server_config cable_server_config;
        cf_cable_server_config_default(&cable_server_config);
        /* D-C07: the scheme comes from the listener; TLS lands with P01. */
        cable_server_config.assume_ssl = !cfg->disable_ssl;
        /* One bounded cable reactor thread per HTTP loop, and the loop's
         * share of the process-wide input/output budgets. */
        cable_server_config.loops = cfg->loops;
        cable_server_config.input_bytes = cfg->input_bytes;
        cable_server_config.output_bytes = cfg->output_bytes;
        cf_cable_server_hooks(cable, &cable_server_config.hooks);
        if (cf_cable_server_create(&cable_server_config, &cable_server) !=
            CF_OK) {
            fprintf(stderr,
                    "campfire: startup failed: cable server creation\n");
            cf_cable_destroy(cable);
            cf_jobs_stop(jobs);
            cf_jobs_destroy(jobs);
            cf_app_stop(app);
            cf_app_destroy(app);
            return 1;
        }
        /* The controller actions' app->broadcasts seam: set once the cable
         * exists, before the loops can dispatch anything. The app borrows the
         * cable; both are released in the shutdown order below. */
        cf_app_set_cable(app, cable);
        /* C03: the writer's mandatory DISCONNECT_USER consumer (04 C03). A
         * committed sign-out/ban/deactivation returns only after every
         * affected connection acknowledged its revocation. */
        if (cf_writer_set_control_handler(app, cf_cable_revocation_handler,
                                          cable) != CF_OK) {
            fprintf(stderr,
                    "campfire: startup failed: revocation handler\n");
            cf_cable_server_destroy(cable_server);
            cf_cable_destroy(cable);
            cf_jobs_stop(jobs);
            cf_jobs_destroy(jobs);
            cf_app_stop(app);
            cf_app_destroy(app);
            return 1;
        }
    }

    /* J02: default model-event handlers plus the writer post-commit consumer
     * registration (the writer is started; the cable exists; nothing serves
     * yet). A full queue or missing handler fails the enqueue cleanly at
     * runtime — startup itself only fails on registration errors. */
    jobs_hctx.cable = cable;
    if (cf_jobs_register_default_handlers(jobs, &jobs_hctx) != CF_OK) {
        fprintf(stderr, "campfire: startup failed: job handlers\n");
        cf_cable_server_destroy(cable_server);
        cf_cable_destroy(cable);
        cf_jobs_stop(jobs);
        cf_jobs_destroy(jobs);
        cf_app_stop(app);
        cf_app_destroy(app);
        return 1;
    }
    if (cf_jobs_register_writer(jobs, app) != CF_OK) {
        fprintf(stderr, "campfire: startup failed: job consumers\n");
        cf_cable_server_destroy(cable_server);
        cf_cable_destroy(cable);
        cf_jobs_stop(jobs);
        cf_jobs_destroy(jobs);
        cf_app_stop(app);
        cf_app_destroy(app);
        return 1;
    }

    char errbuf[128];
    int listen_fd = cf_serve_listen(cfg, errbuf, sizeof errbuf);
    if (listen_fd < 0) {
        fprintf(stderr, "campfire: startup failed: listen on %s:%u: %s\n",
                cfg->host, cfg->port, errbuf);
        cf_cable_server_destroy(cable_server);
        cf_cable_destroy(cable);
        cf_jobs_stop(jobs);
        cf_jobs_destroy(jobs);
        cf_app_stop(app);
        cf_app_destroy(app);
        return 1;
    }

    /* P01: one loop-shared TLS server context when TLS is configured. Every
     * loop thread borrows it; each accepted connection handshakes without
     * blocking. Created after the listener so every later failure path
     * funnels through `shutdown` below, which destroys it after the loops
     * join and before app destroy (CORE-05 order preserved). */
    cf_front_tls_server *tls_server = NULL;
    if (!cfg->disable_ssl) {
        char tls_err[256];
        if (cf_front_tls_server_create(cfg->tls_cert_file, cfg->tls_key_file,
                                       &tls_server, tls_err,
                                       sizeof tls_err) != CF_OK) {
            fprintf(stderr, "campfire: startup failed: TLS: %s\n",
                    tls_err[0] != '\0' ? tls_err : "invalid configuration");
            close(listen_fd);
            cf_cable_server_destroy(cable_server);
            cf_cable_destroy(cable);
            cf_jobs_stop(jobs);
            cf_jobs_destroy(jobs);
            cf_app_stop(app);
            cf_app_destroy(app);
            return 1;
        }
    }

    size_t loop_count = cfg->loops;
    cf_http_loop **loops = calloc(loop_count, sizeof *loops);
    pthread_t *loop_threads = calloc(loop_count, sizeof *loop_threads);
    if (loops == NULL || loop_threads == NULL) {
        fprintf(stderr, "campfire: startup failed: loop allocation\n");
        free(loops);
        free(loop_threads);
        close(listen_fd);
        cf_cable_server_destroy(cable_server);
        cf_cable_destroy(cable);
        cf_jobs_stop(jobs);
        cf_jobs_destroy(jobs);
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
        loop_config.upgrade = cf_cable_server_upgrade;
        loop_config.upgrade_user = cable_server;
        if (cf_http_loop_create(&loop_config, &loops[i]) != CF_OK) {
            fprintf(stderr, "campfire: startup failed: loop %zu creation\n",
                    i);
            exit_code = 1;
            goto shutdown;
        }
        /* P01: NULL on plaintext loops (no behavior change there). */
        cf_http_loop_set_tls_server(loops[i], tls_server);
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
    /* Cable teardown follows the enforced caller order documented on
     * cf_cable_server_stop (cable.h):
     *   1. stop the HTTP loops (no new upgrade-hook calls);
     *   2. cf_cable_server_stop(): every reactor joins, shuts its upgraded
     *      sockets down and submits each upgrade's lifetime lease for release
     *      (the connection-slot release completes on the loop thread);
     *   3. join the HTTP loop threads: the lease releases run there and free
     *      every connection slot (this is the "release leases" step);
     *   4. only then cf_cable_server_destroy() frees the server: no loop
     *      thread can still be inside the upgrade hook and every lease is
     *      released.
     * This function performs exactly that order: loops stop, server stop,
     * loop join, server destroy. The cable server joins every reactor (whose
     * socket exit detaches its subscription loop) while the writer is still
     * up, so disconnect presence effects can run; the cable itself is
     * released last. */
    for (size_t i = 0; i < loops_created; i++) {
        cf_http_loop_stop(loops[i]);
    }
    if (cable_server != NULL) cf_cable_server_stop(cable_server);
    for (size_t i = 0; i < threads_created; i++) {
        (void)pthread_join(loop_threads[i], NULL);
    }
    /* No loop can be inside the upgrade hook now; every lease was released
     * by the loop completions joined above. */
    if (cable_server != NULL) {
        cf_cable_server_destroy(cable_server);
        cable_server = NULL;
    }
    if (cable != NULL &&
        cf_cable_stop(cable, 5000) != CF_OK) {
        fprintf(stderr,
                "campfire: shutdown: cable loops did not drain in time\n");
    }
    /* J01/J02 shutdown loss point: stop accepting, discard and count every
     * pending job, let running handlers finish, then release. This precedes
     * cf_app_stop so no handler can be inside cf_write when the writer goes
     * away; requests still draining enqueue into stopped queues (CF_BUSY,
     * counted drops). */
    cf_jobs_stop(jobs);
    cf_jobs_destroy(jobs);
    cf_app_stop(app);
    for (size_t i = 0; i < loops_created; i++) {
        cf_http_loop_destroy(loops[i]);
    }
    /* P01: no loop thread can still touch the shared context (every TLS and
     * H2 object died with its connection above); the app no longer needs
     * it either. Destroyed before app destroy per the handoff contract. */
    if (tls_server != NULL) {
        cf_front_tls_server_destroy(tls_server);
        tls_server = NULL;
    }
    close(listen_fd);
    free(loops);
    free(loop_threads);
    cf_app_destroy(app); /* joins any remaining worker before freeing state */
    if (cable != NULL) cf_cable_destroy(cable);
    fprintf(stderr, "campfire: shutdown: complete\n");
    return exit_code;
}
