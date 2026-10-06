/* P01 configured TLS front (05-storage-integrations.md P01, FRONT-01).
 *
 * TLS belongs to each connection's loop: this module loads the configured
 * certificate/key files once at boot and drives per-connection non-blocking
 * handshakes and I/O. OpenSSL WANT_READ/WANT_WRITE maps to epoll readiness;
 * no function here ever blocks a loop in SSL_accept/read/write. ALPN offers
 * "h2" and "http/1.1" with server preference for h2.
 *
 * Certificate/key load from explicit files at boot; invalid or mismatched
 * files fail startup loudly (no silent HTTP fallback when TLS was
 * requested). There is no ACME client (P02 deferred) and no proxy-compat
 * layer (D-C07): the listener owns scheme and peer address.
 *
 * Wiring (integrator-owned, proposed as patch snippets in the handoff, not
 * done here): create one cf_front_tls_server at boot from TLS_CERT_FILE /
 * TLS_KEY_FILE, wrap each accepted fd with cf_front_tls_conn_wrap before the
 * HTTP/1 state machine or the h2 session sees bytes, and map the WANT_READ /
 * WANT_WRITE steps below to epoll interest. Request workers observe
 * req->tls == true for TLS connections. */
#ifndef CF_FRONT_TLS_H
#define CF_FRONT_TLS_H

#include <stdbool.h>
#include <stddef.h>

#include "cf.h"

typedef struct cf_front_tls_server cf_front_tls_server;
typedef struct cf_front_tls_conn cf_front_tls_conn;

/* One non-blocking step outcome. WANT_READ / WANT_WRITE map directly to
 * epoll interest; the caller retries the same operation on readiness. DONE
 * means the operation completed (recv with *out_n == 0 is a clean peer
 * shutdown). FAIL means the connection must close; nothing was consumed. */
typedef enum {
    CF_FRONT_TLS_DONE = 0,
    CF_FRONT_TLS_WANT_READ,
    CF_FRONT_TLS_WANT_WRITE,
    CF_FRONT_TLS_FAIL
} cf_front_tls_step;

/* ALPN identifiers offered by the server, in server-preference order. */
#define CF_FRONT_TLS_ALPN_H2 "h2"
#define CF_FRONT_TLS_ALPN_HTTP11 "http/1.1"

/* Load the certificate chain and private key from explicit files and build
 * the loop-shared server context. Fails loudly (CF_INVALID for missing /
 * unreadable / unparsable files, CF_DB-free CF_INVALID when the key does
 * not match the certificate); on failure *out stays NULL and errbuf holds
 * a sanitized diagnostic naming the file, never key material. Either path
 * NULL fails. The context is shared across loops; one connection object is
 * created per accepted fd. */
cf_err cf_front_tls_server_create(const char *cert_file, const char *key_file,
                                   cf_front_tls_server **out, char *errbuf,
                                   size_t errcap);

/* Release a server context (accepts NULL). Live connections must be
 * destroyed first. */
void cf_front_tls_server_destroy(cf_front_tls_server *server);

/* Wrap an accepted non-blocking socket fd. Takes no fd ownership: the
 * caller still closes fd after cf_front_tls_conn_destroy(). */
cf_err cf_front_tls_conn_wrap(cf_front_tls_server *server, int fd,
                               cf_front_tls_conn **out);

/* Release a connection object (accepts NULL). Does not close the fd. */
void cf_front_tls_conn_destroy(cf_front_tls_conn *conn);

/* Drive the server-side handshake one step. DONE means the handshake
 * finished and cf_front_tls_alpn() is valid. Never blocks. */
cf_front_tls_step cf_front_tls_handshake(cf_front_tls_conn *conn);

/* Receive decrypted bytes. DONE with *out_n > 0 consumed bytes, DONE with
 * *out_n == 0 on clean peer shutdown, WANT_* on readiness, FAIL on error.
 * Never blocks. */
cf_front_tls_step cf_front_tls_recv(cf_front_tls_conn *conn,
                                     unsigned char *buf, size_t cap,
                                     size_t *out_n);

/* Send bytes. DONE reports *out_n accepted (possibly short: resend the
 * remainder), WANT_* on readiness, FAIL on error. Never blocks. */
cf_front_tls_step cf_front_tls_send(cf_front_tls_conn *conn,
                                     const unsigned char *buf, size_t len,
                                     size_t *out_n);

/* Negotiated ALPN protocol after a DONE handshake ("h2", "http/1.1", or
 * NULL when the client offered neither). Borrowed static string. */
const char *cf_front_tls_alpn(const cf_front_tls_conn *conn);

/* True when the negotiated protocol is h2. */
bool cf_front_tls_is_h2(const cf_front_tls_conn *conn);

/* Borrowed socket fd given at wrap time (-1 when conn is NULL). */
int cf_front_tls_fd(const cf_front_tls_conn *conn);

/* TLS is mandatory once configured: there is no silent HTTP fallback.
 * A connection that does not complete a TLS handshake is closed, never
 * served as plaintext HTTP. */
static inline bool cf_front_tls_required(void) {
    return true;
}

#endif /* CF_FRONT_TLS_H */
