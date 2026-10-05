/* H01 Host/PUBLIC_ORIGIN consistency for IPv6-literal origins (defect H01-F1:
 * a bracketed Host with an explicit port was always rejected 400) plus
 * IPv4/reg-name regression matrices. Real loopback sockets; the origin is
 * configured from the bound port, so `http://[::1]:<port>` is exercised over
 * the same listener the client connects to. This sandbox has no IPv6
 * loopback (`bind ::1` fails EADDRNOTAVAIL), so the transport stays IPv4 and
 * only the origin/Host text is IPv6; host_is_consistent is address-family
 * independent. */
#include "cf.h"
#include "cf_test.h"
#include "http/http.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---- tests/http/test_http_common.c (repeat prototypes per owned file) -- */
int htest_connect(unsigned port, int rcvbuf);
int htest_send_all(int fd, const void *buf, size_t len);
ssize_t htest_read_response(int fd, char *buf, size_t cap, int timeout_ms);
ssize_t htest_read_to_eof(int fd, char *buf, size_t cap, int timeout_ms);

/* Must match tests/http/test_http_common.c's layout. */
struct htest_echo {
    pthread_mutex_t mutex;
    unsigned admits;
    size_t body_len;
    char method[16];
    char target[512];
    char body[512];
    char peer[64];
    bool tls;
};

cf_err htest_echo_admit(void *user, cf_http_task *task);

static struct htest_echo *echo_new(void) {
    struct htest_echo *st = calloc(1, sizeof *st);
    pthread_mutex_init(&st->mutex, NULL);
    return st;
}

static void echo_free(struct htest_echo *st) {
    pthread_mutex_destroy(&st->mutex);
    free(st);
}

/* htest_start() hardcodes http://127.0.0.1:<port>; this starter takes an
 * origin format with one %u for the bound port so IPv6-literal and reg-name
 * origins can be configured over the same real loopback listener. */
struct otest {
    cf_http_loop *loop;
    int listen_fd;
    pthread_t thread;
    unsigned port;
    char origin[64];
};

static void *otest_run(void *arg) {
    struct otest *t = arg;
    (void)cf_http_loop_run(t->loop);
    return NULL;
}

static struct otest *otest_start(cf_http_admit_fn admit, void *user,
                                 const char *origin_fmt) {
    struct otest *t = calloc(1, sizeof *t);
    if (t == NULL) return NULL;
    t->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (t->listen_fd < 0) {
        free(t);
        return NULL;
    }
    int one = 1;
    setsockopt(t->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(t->listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(t->listen_fd, 128) != 0) {
        close(t->listen_fd);
        free(t);
        return NULL;
    }
    socklen_t alen = sizeof addr;
    if (getsockname(t->listen_fd, (struct sockaddr *)&addr, &alen) != 0) {
        close(t->listen_fd);
        free(t);
        return NULL;
    }
    t->port = ntohs(addr.sin_port);
    snprintf(t->origin, sizeof t->origin, origin_fmt, t->port);

    cf_http_loop_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.listen_fd = t->listen_fd;
    cfg.loop_index = 0;
    cfg.public_origin = t->origin;
    cfg.admit = admit;
    cfg.admit_user = user;
    if (cf_http_loop_create(&cfg, &t->loop) != CF_OK) {
        close(t->listen_fd);
        free(t);
        return NULL;
    }
    if (pthread_create(&t->thread, NULL, otest_run, t) != 0) {
        cf_http_loop_destroy(t->loop);
        close(t->listen_fd);
        free(t);
        return NULL;
    }
    return t;
}

static void otest_stop(struct otest *t) {
    if (t == NULL) return;
    if (t->loop != NULL) {
        cf_http_loop_stop(t->loop);
        pthread_join(t->thread, NULL);
        cf_http_loop_destroy(t->loop);
        t->loop = NULL;
    }
    if (t->listen_fd >= 0) close(t->listen_fd);
    free(t);
}

/* Send raw bytes, require a status line, optionally require a close. */
static void expect_status(struct otest *t, const unsigned char *raw,
                          size_t raw_len, const char *status, int closed) {
    int fd = htest_connect(t->port, 0);
    CF_REQUIRE(fd >= 0);
    CF_CHECK(htest_send_all(fd, raw, raw_len) == 0);
    char resp[16384];
    ssize_t n = htest_read_response(fd, resp, sizeof resp, 5000);
    CF_CHECK(n > 0);
    CF_CHECK(strstr(resp, status) != NULL);
    if (closed) {
        char tail[8];
        CF_CHECK(htest_read_to_eof(fd, tail, sizeof tail, 5000) == 0);
    }
    close(fd);
}

/* Format "Host: <value>" into req and check the status. */
static void expect_host(struct otest *t, const char *value, const char *status,
                        int closed) {
    char req[1024];
    snprintf(req, sizeof req, "GET / HTTP/1.1\r\nHost: %s\r\n\r\n", value);
    expect_status(t, (const unsigned char *)req, strlen(req), status, closed);
}

CF_TEST(ipv6_host_explicit_port_origin) {
    struct htest_echo *st = echo_new();
    struct otest *t = otest_start(htest_echo_admit, st, "http://[::1]:%u");
    CF_REQUIRE(t != NULL);
    unsigned port = t->port;
    CF_REQUIRE(port != 80 && port != 443 && port != 1 && port != 8080);
    char host[128];

    /* Matching bracketed literal with the explicit port: accepted. */
    snprintf(host, sizeof host, "[::1]:%u", port);
    expect_host(t, host, "HTTP/1.1 200 OK", 0);
    /* OWS around the value is trimmed before the check. */
    snprintf(host, sizeof host, " [::1]:%u ", port);
    expect_host(t, host, "HTTP/1.1 200 OK", 0);

    /* Portless Host implies default 80, not the origin's explicit port. */
    expect_host(t, "[::1]", "HTTP/1.1 400 Bad Request", 1);
    /* Explicit but wrong ports. */
    expect_host(t, "[::1]:80", "HTTP/1.1 400 Bad Request", 1);
    expect_host(t, "[::1]:1", "HTTP/1.1 400 Bad Request", 1);

    /* Mismatched / non-canonical literals. */
    snprintf(host, sizeof host, "[::2]:%u", port);
    expect_host(t, host, "HTTP/1.1 400 Bad Request", 1);
    snprintf(host, sizeof host, "[0:0:0:0:0:0:0:1]:%u", port);
    expect_host(t, host, "HTTP/1.1 400 Bad Request", 1);
    expect_host(t, "localhost", "HTTP/1.1 400 Bad Request", 1);
    snprintf(host, sizeof host, "localhost:%u", port);
    expect_host(t, host, "HTTP/1.1 400 Bad Request", 1);
    snprintf(host, sizeof host, "127.0.0.1:%u", port);
    expect_host(t, host, "HTTP/1.1 400 Bad Request", 1);

    /* Bare IPv6 needs brackets; missing/empty/extra/malformed brackets. */
    snprintf(host, sizeof host, "::1:%u", port);
    expect_host(t, host, "HTTP/1.1 400 Bad Request", 1);
    expect_host(t, "::1", "HTTP/1.1 400 Bad Request", 1);
    expect_host(t, "[::1", "HTTP/1.1 400 Bad Request", 1);
    expect_host(t, "[]:1", "HTTP/1.1 400 Bad Request", 1);
    snprintf(host, sizeof host, "[::1]]:%u", port);
    expect_host(t, host, "HTTP/1.1 400 Bad Request", 1);
    snprintf(host, sizeof host, "[::1]:%u:%u", port, port);
    expect_host(t, host, "HTTP/1.1 400 Bad Request", 1);

    /* Malformed port: empty, zero, non-digit, too many digits. */
    expect_host(t, "[::1]:", "HTTP/1.1 400 Bad Request", 1);
    expect_host(t, "[::1]:0", "HTTP/1.1 400 Bad Request", 1);
    expect_host(t, "[::1]:80x", "HTTP/1.1 400 Bad Request", 1);
    expect_host(t, "[::1]: 80", "HTTP/1.1 400 Bad Request", 1);
    expect_host(t, "[::1]:123456", "HTTP/1.1 400 Bad Request", 1);

    /* Over the 255-byte Host cap. */
    char big[300];
    big[0] = '[';
    memset(big + 1, 'a', 254);
    big[255] = ']';
    big[256] = '\0';
    expect_host(t, big, "HTTP/1.1 400 Bad Request", 1);

    /* Embedded NUL in the Host value. */
    {
        char raw[256];
        int n = snprintf(raw, sizeof raw, "GET / HTTP/1.1\r\nHost: [::1]:%u",
                         port);
        raw[n++] = '\0';
        memcpy(raw + n, "x\r\n\r\n", 5);
        expect_status(t, (const unsigned char *)raw, (size_t)n + 5,
                      "HTTP/1.1 400 Bad Request", 1);
    }
    /* Embedded CR (line is not terminated after the value). */
    {
        char raw[256];
        int n = snprintf(raw, sizeof raw, "GET / HTTP/1.1\r\nHost: [::1]");
        raw[n++] = '\r';
        memcpy(raw + n, ":1\r\n\r\n", 6);
        expect_status(t, (const unsigned char *)raw, (size_t)n + 6,
                      "HTTP/1.1 400 Bad Request", 1);
    }
    /* Embedded LF. */
    {
        char raw[256];
        int n = snprintf(raw, sizeof raw,
                         "GET / HTTP/1.1\r\nHost: [::1]:%u\nx\r\n\r\n", port);
        expect_status(t, (const unsigned char *)raw, (size_t)n,
                      "HTTP/1.1 400 Bad Request", 1);
    }

    otest_stop(t);
    echo_free(st);
}

CF_TEST(ipv6_host_default_port_origin) {
    struct htest_echo *st = echo_new();
    struct otest *t = otest_start(htest_echo_admit, st,
                                  "http://[::1]"); /* effective port 80 */
    CF_REQUIRE(t != NULL);

    expect_host(t, "[::1]", "HTTP/1.1 200 OK", 0);
    expect_host(t, "[::1]:80", "HTTP/1.1 200 OK", 0);
    expect_host(t, "[::1]:8080", "HTTP/1.1 400 Bad Request", 1);
    expect_host(t, "[::1]:443", "HTTP/1.1 400 Bad Request", 1);
    expect_host(t, "[::2]", "HTTP/1.1 400 Bad Request", 1);
    expect_host(t, "localhost", "HTTP/1.1 400 Bad Request", 1);

    otest_stop(t);
    echo_free(st);
}

CF_TEST(ipv6_host_explicit_default_port_origin) {
    struct htest_echo *st = echo_new();
    struct otest *t = otest_start(htest_echo_admit, st,
                                  "http://[::1]:80"); /* explicit default */
    CF_REQUIRE(t != NULL);

    expect_host(t, "[::1]:80", "HTTP/1.1 200 OK", 0);
    /* Portless Host implies the same default the origin states. */
    expect_host(t, "[::1]", "HTTP/1.1 200 OK", 0);
    expect_host(t, "[::1]:8080", "HTTP/1.1 400 Bad Request", 1);
    expect_host(t, "[::1]:", "HTTP/1.1 400 Bad Request", 1);

    otest_stop(t);
    echo_free(st);
}

CF_TEST(ipv6_host_https_default_ports) {
    struct htest_echo *st = echo_new();
    struct otest *t = otest_start(htest_echo_admit, st, "https://[::1]");
    CF_REQUIRE(t != NULL);
    expect_host(t, "[::1]", "HTTP/1.1 200 OK", 0);
    expect_host(t, "[::1]:443", "HTTP/1.1 200 OK", 0);
    expect_host(t, "[::1]:80", "HTTP/1.1 400 Bad Request", 1);
    otest_stop(t);

    t = otest_start(htest_echo_admit, st, "https://[::1]:%u");
    CF_REQUIRE(t != NULL);
    CF_REQUIRE(t->port != 443 && t->port != 80);
    char host[128];
    snprintf(host, sizeof host, "[::1]:%u", t->port);
    expect_host(t, host, "HTTP/1.1 200 OK", 0);
    expect_host(t, "[::1]:443", "HTTP/1.1 400 Bad Request", 1);
    expect_host(t, "[::1]:80", "HTTP/1.1 400 Bad Request", 1);
    otest_stop(t);

    echo_free(st);
}

CF_TEST(ipv6_host_literal_compare) {
    struct htest_echo *st = echo_new();
    struct otest *t = otest_start(htest_echo_admit, st, "http://[FE80::A]:%u");
    CF_REQUIRE(t != NULL);
    char host[128];

    /* Hex literal comparison is case-insensitive, exact otherwise. */
    snprintf(host, sizeof host, "[fe80::a]:%u", t->port);
    expect_host(t, host, "HTTP/1.1 200 OK", 0);
    snprintf(host, sizeof host, "[FE80::A]:%u", t->port);
    expect_host(t, host, "HTTP/1.1 200 OK", 0);
    snprintf(host, sizeof host, "[fe80::b]:%u", t->port);
    expect_host(t, host, "HTTP/1.1 400 Bad Request", 1);

    otest_stop(t);
    echo_free(st);
}

CF_TEST(ipv4_and_regname_host_regressions) {
    struct htest_echo *st = echo_new();
    struct otest *t = otest_start(htest_echo_admit, st, "http://127.0.0.1:%u");
    CF_REQUIRE(t != NULL);
    unsigned port = t->port;
    CF_REQUIRE(port != 1);
    char host[128];

    snprintf(host, sizeof host, "127.0.0.1:%u", port);
    expect_host(t, host, "HTTP/1.1 200 OK", 0);
    snprintf(host, sizeof host, "127.0.0.1:%u ", port);
    expect_host(t, host, "HTTP/1.1 200 OK", 0);

    expect_host(t, "127.0.0.1", "HTTP/1.1 400 Bad Request", 1);
    expect_host(t, "127.0.0.1:1", "HTTP/1.1 400 Bad Request", 1);
    expect_host(t, "127.0.0.1:", "HTTP/1.1 400 Bad Request", 1);
    expect_host(t, "127.0.0.1:abc", "HTTP/1.1 400 Bad Request", 1);
    expect_host(t, "127.0.0.1:0", "HTTP/1.1 400 Bad Request", 1);
    expect_host(t, "127.0.0.1:65536", "HTTP/1.1 400 Bad Request", 1);
    expect_host(t, "127.0.0.1: 80", "HTTP/1.1 400 Bad Request", 1);
    snprintf(host, sizeof host, "10.0.0.1:%u", port);
    expect_host(t, host, "HTTP/1.1 400 Bad Request", 1);
    snprintf(host, sizeof host, "user@127.0.0.1:%u", port);
    expect_host(t, host, "HTTP/1.1 400 Bad Request", 1);
    /* Brackets delimit the optional port; the literal itself is compared
     * byte-wise against the origin host (no IPv6 normalization). */
    snprintf(host, sizeof host, "[::1]:%u", port);
    expect_host(t, host, "HTTP/1.1 400 Bad Request", 1);
    snprintf(host, sizeof host, "[127.0.0.1]:%u", port);
    expect_host(t, host, "HTTP/1.1 200 OK", 0);
    expect_host(t, "[127.0.0.1]", "HTTP/1.1 400 Bad Request", 1);
    otest_stop(t);

    /* Reg-name origin, explicit non-default port. */
    t = otest_start(htest_echo_admit, st, "http://localhost:%u");
    CF_REQUIRE(t != NULL);
    port = t->port;
    CF_REQUIRE(port != 1);
    snprintf(host, sizeof host, "localhost:%u", port);
    expect_host(t, host, "HTTP/1.1 200 OK", 0);
    snprintf(host, sizeof host, "LOCALHOST:%u", port);
    expect_host(t, host, "HTTP/1.1 200 OK", 0);
    expect_host(t, "localhost", "HTTP/1.1 400 Bad Request", 1);
    expect_host(t, "localhost:1", "HTTP/1.1 400 Bad Request", 1);
    snprintf(host, sizeof host, "xlocalhost:%u", port);
    expect_host(t, host, "HTTP/1.1 400 Bad Request", 1);
    snprintf(host, sizeof host, "[::1]:%u", port);
    expect_host(t, host, "HTTP/1.1 400 Bad Request", 1);
    otest_stop(t);

    /* Reg-name origin on the default port: portless and :80 both match. */
    t = otest_start(htest_echo_admit, st, "http://localhost");
    CF_REQUIRE(t != NULL);
    expect_host(t, "localhost", "HTTP/1.1 200 OK", 0);
    expect_host(t, "localhost:80", "HTTP/1.1 200 OK", 0);
    expect_host(t, "localhost:8080", "HTTP/1.1 400 Bad Request", 1);
    otest_stop(t);

    echo_free(st);
}

CF_TEST_MAIN()
