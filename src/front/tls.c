/* P01 TLS implementation; see tls.h for the contract. */
#include "tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

/* ALPN wire offer: h2 preferred, then http/1.1. */
static const unsigned char kAlpnOffer[] = {
    2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1',
};

struct cf_front_tls_server {
    SSL_CTX *ctx;
};

struct cf_front_tls_conn {
    SSL *ssl;
    int fd;
};

/* Server ALPN select: prefer h2, then http/1.1; NOACK when the client
 * offered neither (the connection then serves HTTP/1.1 only if the
 * integrator's policy allows; TLS itself stays mandatory). */
static int alpn_select(SSL *ssl, const unsigned char **out,
                       unsigned char *outlen, const unsigned char *in,
                       unsigned int inlen, void *arg) {
    (void)ssl;
    (void)arg;
    if (SSL_select_next_proto((unsigned char **)out, outlen, kAlpnOffer,
                              (unsigned int)sizeof kAlpnOffer, in,
                              inlen) != OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_NOACK;
    }
    return SSL_TLSEXT_ERR_OK;
}

static void note_openssl_top(char *errbuf, size_t errcap, const char *what,
                             const char *file) {
    if (errbuf == NULL || errcap == 0) return;
    unsigned long code = ERR_get_error();
    char detail[128];
    if (code != 0) {
        ERR_error_string_n(code, detail, sizeof detail);
    } else {
        detail[0] = '\0';
    }
    /* Sanitized: names the file and the operation, never key material. */
    if (detail[0] != '\0') {
        snprintf(errbuf, errcap, "%s: %s: %s", what, file, detail);
    } else {
        snprintf(errbuf, errcap, "%s: %s", what, file);
    }
    ERR_clear_error();
}

cf_err cf_front_tls_server_create(const char *cert_file, const char *key_file,
                                   cf_front_tls_server **out, char *errbuf,
                                   size_t errcap) {
    if (out == NULL) return CF_INVALID;
    *out = NULL;
    if (errbuf != NULL && errcap != 0) errbuf[0] = '\0';
    if (cert_file == NULL || *cert_file == '\0' || key_file == NULL ||
        *key_file == '\0') {
        if (errbuf != NULL && errcap != 0) {
            snprintf(errbuf, errcap,
                     "TLS certificate/key: explicit files are required");
        }
        return CF_INVALID;
    }
    cf_front_tls_server *server = calloc(1, sizeof *server);
    if (server == NULL) return CF_NOMEM;
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (ctx == NULL) {
        note_openssl_top(errbuf, errcap, "TLS context", "TLS_server_method");
        free(server);
        return CF_INTERNAL;
    }
    if (SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) != 1) {
        note_openssl_top(errbuf, errcap, "TLS minimum version", "TLS1.2");
        SSL_CTX_free(ctx);
        free(server);
        return CF_INTERNAL;
    }
    if (SSL_CTX_use_certificate_chain_file(ctx, cert_file) != 1) {
        note_openssl_top(errbuf, errcap, "TLS certificate", cert_file);
        SSL_CTX_free(ctx);
        free(server);
        return CF_INVALID;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM) != 1) {
        note_openssl_top(errbuf, errcap, "TLS private key", key_file);
        SSL_CTX_free(ctx);
        free(server);
        return CF_INVALID;
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        if (errbuf != NULL && errcap != 0) {
            snprintf(errbuf, errcap,
                     "TLS certificate/key: private key does not match the "
                     "certificate");
        }
        ERR_clear_error();
        SSL_CTX_free(ctx);
        free(server);
        return CF_INVALID;
    }
    SSL_CTX_set_alpn_select_cb(ctx, alpn_select, NULL);
    server->ctx = ctx;
    *out = server;
    return CF_OK;
}

void cf_front_tls_server_destroy(cf_front_tls_server *server) {
    if (server == NULL) return;
    SSL_CTX_free(server->ctx);
    free(server);
}

cf_err cf_front_tls_conn_wrap(cf_front_tls_server *server, int fd,
                               cf_front_tls_conn **out) {
    if (out == NULL) return CF_INVALID;
    *out = NULL;
    if (server == NULL || server->ctx == NULL || fd < 0) return CF_INVALID;
    cf_front_tls_conn *conn = calloc(1, sizeof *conn);
    if (conn == NULL) return CF_NOMEM;
    SSL *ssl = SSL_new(server->ctx);
    if (ssl == NULL) {
        free(conn);
        ERR_clear_error();
        return CF_NOMEM;
    }
    if (SSL_set_fd(ssl, fd) != 1) {
        SSL_free(ssl);
        free(conn);
        ERR_clear_error();
        return CF_INVALID;
    }
    SSL_set_accept_state(ssl);
    conn->ssl = ssl;
    conn->fd = fd;
    *out = conn;
    return CF_OK;
}

void cf_front_tls_conn_destroy(cf_front_tls_conn *conn) {
    if (conn == NULL) return;
    SSL_free(conn->ssl);
    free(conn);
}

static cf_front_tls_step map_ssl_error(cf_front_tls_conn *conn, int ret) {
    int err = SSL_get_error(conn->ssl, ret);
    switch (err) {
    case SSL_ERROR_WANT_READ:
        return CF_FRONT_TLS_WANT_READ;
    case SSL_ERROR_WANT_WRITE:
        return CF_FRONT_TLS_WANT_WRITE;
    default:
        ERR_clear_error();
        return CF_FRONT_TLS_FAIL;
    }
}

cf_front_tls_step cf_front_tls_handshake(cf_front_tls_conn *conn) {
    if (conn == NULL || conn->ssl == NULL) return CF_FRONT_TLS_FAIL;
    int ret = SSL_accept(conn->ssl);
    if (ret == 1) {
        ERR_clear_error();
        return CF_FRONT_TLS_DONE;
    }
    return map_ssl_error(conn, ret);
}

cf_front_tls_step cf_front_tls_recv(cf_front_tls_conn *conn,
                                     unsigned char *buf, size_t cap,
                                     size_t *out_n) {
    if (out_n != NULL) *out_n = 0;
    if (conn == NULL || conn->ssl == NULL || buf == NULL || cap == 0) {
        return CF_FRONT_TLS_FAIL;
    }
    int ret = SSL_read(conn->ssl, buf, (int)cap > 0 ? (int)(cap > INT32_MAX ? INT32_MAX : cap) : 0);
    if (ret > 0) {
        if (out_n != NULL) *out_n = (size_t)ret;
        return CF_FRONT_TLS_DONE;
    }
    if (ret == 0) {
        /* Clean peer shutdown. */
        ERR_clear_error();
        return CF_FRONT_TLS_DONE;
    }
    return map_ssl_error(conn, ret);
}

cf_front_tls_step cf_front_tls_send(cf_front_tls_conn *conn,
                                     const unsigned char *buf, size_t len,
                                     size_t *out_n) {
    if (out_n != NULL) *out_n = 0;
    if (conn == NULL || conn->ssl == NULL || buf == NULL || len == 0) {
        return CF_FRONT_TLS_FAIL;
    }
    int want = len > (size_t)INT32_MAX ? INT32_MAX : (int)len;
    int ret = SSL_write(conn->ssl, buf, want);
    if (ret > 0) {
        if (out_n != NULL) *out_n = (size_t)ret;
        return CF_FRONT_TLS_DONE;
    }
    return map_ssl_error(conn, ret);
}

const char *cf_front_tls_alpn(const cf_front_tls_conn *conn) {
    if (conn == NULL || conn->ssl == NULL) return NULL;
    const unsigned char *proto = NULL;
    unsigned int proto_len = 0;
    SSL_get0_alpn_selected(conn->ssl, &proto, &proto_len);
    if (proto == NULL || proto_len == 0) return NULL;
    if (proto_len == 2 && memcmp(proto, "h2", 2) == 0) {
        return CF_FRONT_TLS_ALPN_H2;
    }
    if (proto_len == 8 && memcmp(proto, "http/1.1", 8) == 0) {
        return CF_FRONT_TLS_ALPN_HTTP11;
    }
    return NULL;
}

bool cf_front_tls_is_h2(const cf_front_tls_conn *conn) {
    const char *alpn = cf_front_tls_alpn(conn);
    return alpn != NULL && strcmp(alpn, CF_FRONT_TLS_ALPN_H2) == 0;
}

int cf_front_tls_fd(const cf_front_tls_conn *conn) {
    return conn != NULL ? conn->fd : -1;
}

bool cf_front_tls_pending(const cf_front_tls_conn *conn) {
    return conn != NULL && SSL_pending(conn->ssl) > 0;
}
