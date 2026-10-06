/* P01 FRONT-01: TLS boot policy, ALPN selection and peer rejection.
 *
 * Uses the pinned I01 test-CA pattern (tests/fixtures/.../testdata/tls):
 * SAN covers www.example.com (CN fcm.googleapis.com). Run from the
 * worktree root so the fixtures resolve; a missing prerequisite FAILS,
 * never skips. Real loopback handshakes (socketpair, non-blocking steps);
 * no mock TLS. */
#include "cf_test.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "front/tls.h"

static const char *kCert =
    "tests/fixtures/crates/campfire/src/integrations/testdata/tls/server.pem";
static const char *kKey =
    "tests/fixtures/crates/campfire/src/integrations/testdata/tls/server.key";
static const char *kCA =
    "tests/fixtures/crates/campfire/src/integrations/testdata/tls/ca.pem";

static int make_pair(int sv[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
    for (int i = 0; i < 2; i++) {
        int fl = fcntl(sv[i], F_GETFL, 0);
        if (fl < 0 || fcntl(sv[i], F_SETFL, fl | O_NONBLOCK) != 0) {
            close(sv[0]);
            close(sv[1]);
            return -1;
        }
    }
    return 0;
}

static cf_front_tls_step cli_step(SSL *cli, bool *done) {
    *done = false;
    int r = SSL_do_handshake(cli);
    if (r == 1) {
        *done = true;
        return CF_FRONT_TLS_DONE;
    }
    int e = SSL_get_error(cli, r);
    if (e == SSL_ERROR_WANT_READ) return CF_FRONT_TLS_WANT_READ;
    if (e == SSL_ERROR_WANT_WRITE) return CF_FRONT_TLS_WANT_WRITE;
    ERR_clear_error();
    return CF_FRONT_TLS_FAIL;
}

/* Drive server + client until both finish or either fails. */
static bool drive(cf_front_tls_conn *srv, SSL *cli) {
    bool srv_done = false, cli_done = false;
    for (int i = 0; i < 20000; i++) {
        if (!srv_done) {
            cf_front_tls_step s = cf_front_tls_handshake(srv);
            if (s == CF_FRONT_TLS_DONE) srv_done = true;
            if (s == CF_FRONT_TLS_FAIL) return false;
        }
        if (!cli_done) {
            bool d = false;
            cf_front_tls_step s = cli_step(cli, &d);
            if (s == CF_FRONT_TLS_FAIL) return false;
            if (d) cli_done = true;
        }
        if (srv_done && cli_done) return true;
    }
    return false;
}

static SSL_CTX *client_ctx(const char *ca_file, const char *host,
                           const unsigned char *alpn, unsigned alpn_len) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == NULL) return NULL;
    if (SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) != 1) {
        SSL_CTX_free(ctx);
        return NULL;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    if (SSL_CTX_load_verify_locations(ctx, ca_file, NULL) != 1) {
        SSL_CTX_free(ctx);
        ERR_clear_error();
        return NULL;
    }
    if (SSL_CTX_set_alpn_protos(ctx, alpn, alpn_len) != 0) {
        SSL_CTX_free(ctx);
        return NULL;
    }
    (void)host;
    return ctx;
}

static const unsigned char kOfferH2[] = {2, 'h', '2'};
static const unsigned char kOfferH1[] = {8, 'h', 't', 't', 'p',
                                         '/', '1', '.', '1'};

CF_TEST(boot_rejects_missing_files) {
    cf_front_tls_server *s = NULL;
    char err[256];
    CF_REQUIRE(cf_front_tls_server_create("no/such/cert.pem", kKey, &s, err,
                                          sizeof err) != CF_OK);
    CF_CHECK(s == NULL);
    CF_REQUIRE(cf_front_tls_server_create(kCert, "no/such/key.pem", &s, err,
                                          sizeof err) != CF_OK);
    CF_CHECK(s == NULL);
    CF_REQUIRE(cf_front_tls_server_create(NULL, kKey, &s, err, sizeof err) !=
               CF_OK);
    CF_CHECK(s == NULL);
    CF_REQUIRE(cf_front_tls_server_create(kCert, NULL, &s, err, sizeof err) !=
               CF_OK);
    CF_CHECK(s == NULL);
}

CF_TEST(boot_rejects_garbage_files) {
    /* Garbage PEM content fails loudly, naming the file. */
    const char *path = "/tmp/opencode/front-tls-garbage.pem";
    FILE *f = fopen(path, "w");
    CF_REQUIRE(f != NULL);
    CF_REQUIRE(fputs("not a certificate\n", f) >= 0);
    CF_REQUIRE(fclose(f) == 0);
    cf_front_tls_server *s = NULL;
    char err[256];
    memset(err, 0, sizeof err);
    CF_REQUIRE(cf_front_tls_server_create(path, kKey, &s, err, sizeof err) !=
               CF_OK);
    CF_CHECK(s == NULL);
    CF_CHECK(err[0] != '\0');
    CF_REQUIRE(cf_front_tls_server_create(kCert, path, &s, err, sizeof err) !=
               CF_OK);
    CF_CHECK(s == NULL);
}

CF_TEST(boot_rejects_mismatched_key) {
    /* A valid-PEM file that is not the matching key fails startup: the
     * certificate (not a key) can never match. */
    cf_front_tls_server *s = NULL;
    char err[256];
    memset(err, 0, sizeof err);
    CF_REQUIRE(cf_front_tls_server_create(kCert, kCA, &s, err, sizeof err) !=
               CF_OK);
    CF_CHECK(s == NULL);
    CF_CHECK(err[0] != '\0');
}

CF_TEST(boot_accepts_pinned_pair) {
    FILE *f = fopen(kCert, "r");
    CF_REQUIRE(f != NULL); /* LOUD: run from the worktree root */
    if (f != NULL) fclose(f);
    f = fopen(kKey, "r");
    CF_REQUIRE(f != NULL);
    if (f != NULL) fclose(f);
    cf_front_tls_server *s = NULL;
    char err[256];
    CF_REQUIRE(cf_front_tls_server_create(kCert, kKey, &s, err, sizeof err) ==
               CF_OK);
    CF_REQUIRE(s != NULL);
    cf_front_tls_server_destroy(s);
    cf_front_tls_server_destroy(NULL);
}

CF_TEST(alpn_selects_h2) {
    cf_front_tls_server *srv = NULL;
    char err[256];
    CF_REQUIRE(cf_front_tls_server_create(kCert, kKey, &srv, err,
                                          sizeof err) == CF_OK);
    int sv[2];
    CF_REQUIRE(make_pair(sv) == 0);
    cf_front_tls_conn *sc = NULL;
    CF_REQUIRE(cf_front_tls_conn_wrap(srv, sv[0], &sc) == CF_OK);
    SSL_CTX *cctx = client_ctx(kCA, "www.example.com", kOfferH2,
                               (unsigned)sizeof kOfferH2);
    CF_REQUIRE(cctx != NULL);
    SSL *cli = SSL_new(cctx);
    CF_REQUIRE(cli != NULL);
    CF_REQUIRE(SSL_set1_dnsname(cli, "www.example.com") == 1);
    CF_REQUIRE(SSL_set_fd(cli, sv[1]) == 1);
    SSL_set_connect_state(cli);
    CF_REQUIRE(drive(sc, cli));
    const char *alpn = cf_front_tls_alpn(sc);
    CF_REQUIRE(alpn != NULL);
    CF_CHECK(strcmp(alpn, "h2") == 0);
    CF_CHECK(cf_front_tls_is_h2(sc));
    const unsigned char *cp = NULL;
    unsigned int cpl = 0;
    SSL_get0_alpn_selected(cli, &cp, &cpl);
    CF_REQUIRE(cp != NULL && cpl == 2 && memcmp(cp, "h2", 2) == 0);
    SSL_free(cli);
    SSL_CTX_free(cctx);
    cf_front_tls_conn_destroy(sc);
    close(sv[0]);
    close(sv[1]);
    cf_front_tls_server_destroy(srv);
}

CF_TEST(alpn_selects_http11) {
    cf_front_tls_server *srv = NULL;
    char err[256];
    CF_REQUIRE(cf_front_tls_server_create(kCert, kKey, &srv, err,
                                          sizeof err) == CF_OK);
    int sv[2];
    CF_REQUIRE(make_pair(sv) == 0);
    cf_front_tls_conn *sc = NULL;
    CF_REQUIRE(cf_front_tls_conn_wrap(srv, sv[0], &sc) == CF_OK);
    SSL_CTX *cctx = client_ctx(kCA, "www.example.com", kOfferH1,
                               (unsigned)sizeof kOfferH1);
    CF_REQUIRE(cctx != NULL);
    SSL *cli = SSL_new(cctx);
    CF_REQUIRE(cli != NULL);
    CF_REQUIRE(SSL_set1_dnsname(cli, "www.example.com") == 1);
    CF_REQUIRE(SSL_set_fd(cli, sv[1]) == 1);
    SSL_set_connect_state(cli);
    CF_REQUIRE(drive(sc, cli));
    const char *alpn = cf_front_tls_alpn(sc);
    CF_REQUIRE(alpn != NULL);
    CF_CHECK(strcmp(alpn, "http/1.1") == 0);
    CF_CHECK(!cf_front_tls_is_h2(sc));
    SSL_free(cli);
    SSL_CTX_free(cctx);
    cf_front_tls_conn_destroy(sc);
    close(sv[0]);
    close(sv[1]);
    cf_front_tls_server_destroy(srv);
}

CF_TEST(invalid_peer_fails) {
    /* Wrong hostname against the pinned CA: the client aborts, so the
     * server handshake never completes. */
    cf_front_tls_server *srv = NULL;
    char err[256];
    CF_REQUIRE(cf_front_tls_server_create(kCert, kKey, &srv, err,
                                          sizeof err) == CF_OK);
    int sv[2];
    CF_REQUIRE(make_pair(sv) == 0);
    cf_front_tls_conn *sc = NULL;
    CF_REQUIRE(cf_front_tls_conn_wrap(srv, sv[0], &sc) == CF_OK);
    SSL_CTX *cctx = client_ctx(kCA, "wrong.example.com", kOfferH2,
                               (unsigned)sizeof kOfferH2);
    CF_REQUIRE(cctx != NULL);
    SSL *cli = SSL_new(cctx);
    CF_REQUIRE(cli != NULL);
    CF_REQUIRE(SSL_set1_dnsname(cli, "wrong.example.com") == 1);
    CF_REQUIRE(SSL_set_fd(cli, sv[1]) == 1);
    SSL_set_connect_state(cli);
    CF_CHECK(!drive(sc, cli)); /* must fail, never silently accept */
    SSL_free(cli);
    SSL_CTX_free(cctx);
    cf_front_tls_conn_destroy(sc);
    close(sv[0]);
    close(sv[1]);
    /* An untrusted (garbage) CA fails loudly at setup, never at serving. */
    const char *bogus = "/tmp/opencode/front-tls-bogus-ca.pem";
    FILE *f = fopen(bogus, "w");
    CF_REQUIRE(f != NULL);
    CF_REQUIRE(fputs("not a certificate\n", f) >= 0);
    CF_REQUIRE(fclose(f) == 0);
    SSL_CTX *bad = client_ctx(bogus, "www.example.com", kOfferH2,
                              (unsigned)sizeof kOfferH2);
    CF_CHECK(bad == NULL);
    if (bad != NULL) SSL_CTX_free(bad);
    cf_front_tls_server_destroy(srv);
}

CF_TEST(no_silent_http_fallback) {
    /* Plaintext HTTP bytes on a TLS-required connection never produce a
     * handshake completion (and hence no application response). */
    CF_CHECK(cf_front_tls_required());
    cf_front_tls_server *srv = NULL;
    char err[256];
    CF_REQUIRE(cf_front_tls_server_create(kCert, kKey, &srv, err,
                                          sizeof err) == CF_OK);
    int sv[2];
    CF_REQUIRE(make_pair(sv) == 0);
    cf_front_tls_conn *sc = NULL;
    CF_REQUIRE(cf_front_tls_conn_wrap(srv, sv[0], &sc) == CF_OK);
    static const char plain[] = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    ssize_t w = write(sv[1], plain, sizeof plain - 1);
    CF_REQUIRE(w == (ssize_t)(sizeof plain - 1));
    bool ever_done = false;
    for (int i = 0; i < 50; i++) {
        cf_front_tls_step s = cf_front_tls_handshake(sc);
        if (s == CF_FRONT_TLS_DONE) ever_done = true;
        if (s == CF_FRONT_TLS_FAIL) break;
    }
    CF_CHECK(!ever_done); /* closed, never served as plaintext HTTP */
    cf_front_tls_conn_destroy(sc);
    close(sv[0]);
    close(sv[1]);
    cf_front_tls_server_destroy(srv);
}

CF_TEST_MAIN()
