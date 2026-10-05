/* src/cable/cable.h — task C01: the WebSocket wire contract of the /cable
 * front mount (04-cable-jobs.md "C01: WebSocket wire contract"; source
 * tmp/rust-ref/crates/cable/src/{socket.rs,protocol.rs,server.rs}).
 *
 * socket.c translates socket.rs: the RFC 6455 server handshake acceptance,
 * client frame reader (masking, variable lengths, fragmentation, control
 * interleave, UTF-8, close handshake, 1 MiB assembled/inflated cap),
 * permessage-deflate without context takeover and the bounded outbound
 * queue (4 MiB pending, 30 s stalled write, reference ping cadence).
 * protocol.c translates protocol.rs: the Action Cable JSON frame bodies.
 *
 * The server object is the HTTP upgrade hook (cf_cable_server_upgrade) the
 * loop's upgrade seam calls for /cable. On CF_HTTP_UPGRADE_TAKEN the socket
 * leaves HTTP and is owned by this module: an owner per connection, no
 * other thread sends, closes or mutates it. */
#ifndef CF_CABLE_H
#define CF_CABLE_H

#include "cf.h"

#include "http/http.h"
#include "models/types.h"

/* ---- protocol.rs constants ------------------------------------------------ */

#define CF_CABLE_DEFAULT_MOUNT "/cable"
#define CF_CABLE_SUBPROTOCOL_V1 "actioncable-v1-json"
#define CF_CABLE_SUBPROTOCOL_UNSUPPORTED "actioncable-unsupported"
#define CF_CABLE_BEAT_INTERVAL_MS UINT64_C(3000) /* BEAT_INTERVAL, seconds */
#define CF_CABLE_DEFLATE_RESPONSE                     \
    "permessage-deflate; server_no_context_takeover; " \
    "client_no_context_takeover"

/* socket.rs: the largest client message, assembled and inflated. */
#define CF_CABLE_MAX_MESSAGE ((size_t)1 << 20)
/* socket.rs: smaller frames go out uncompressed even when compression is on. */
#define CF_CABLE_MIN_COMPRESSED ((size_t)256)
/* 04 C01: pending Cable bytes that close a connection. */
#define CF_CABLE_MAX_PENDING_BYTES ((size_t)4 << 20)
/* socket.rs WRITE_TIMEOUT: no send progress while bytes are pending. */
#define CF_CABLE_WRITE_STALL_MS UINT64_C(30000)
/* server.rs close_timeout: how long to wait for the client's close frame. */
#define CF_CABLE_CLOSE_TIMEOUT_MS UINT64_C(5000)

/* websocket-driver's codes for what a client can get wrong (socket.rs). */
#define CF_CABLE_PROTOCOL_ERROR 1002
#define CF_CABLE_UNACCEPTABLE 1003
#define CF_CABLE_ENCODING_ERROR 1007
#define CF_CABLE_TOO_LARGE 1009

/* ---- protocol.c: Action Cable wire format --------------------------------- */

/* ActionCable::INTERNAL[:disconnect_reasons]. */
typedef enum {
    CF_CABLE_REASON_NONE = 0,
    CF_CABLE_REASON_UNAUTHORIZED,
    CF_CABLE_REASON_INVALID_REQUEST,
    CF_CABLE_REASON_SERVER_RESTART,
    CF_CABLE_REASON_REMOTE
} cf_cable_disconnect_reason;

const char *cf_cable_reason_name(cf_cable_disconnect_reason reason);

/* `{"type":"welcome"}`. */
cf_err cf_cable_welcome(cf_str *out);
/* `{"type":"ping","message":<unix seconds>}`. */
cf_err cf_cable_ping(int64_t unix_seconds, cf_str *out);
/* `{"type":"disconnect","reason":...,"reconnect":...}`; reason NONE writes
 * null (Connection::Base#close without one). reconnect_json is one complete
 * JSON value and is inserted verbatim (the remote value is not validated);
 * an empty span writes true, the reference default. */
cf_err cf_cable_disconnect_frame(cf_cable_disconnect_reason reason,
                                 cf_span reconnect_json, cf_str *out);
/* `{"identifier":<json>,"type":"confirm_subscription"}`. */
cf_err cf_cable_confirm_subscription(cf_span identifier, cf_str *out);
/* `{"identifier":<json>,"type":"reject_subscription"}`. */
cf_err cf_cable_reject_subscription(cf_span identifier, cf_str *out);
/* `{"identifier":<encoded>,"message":<encoded>}`; both parts are already
 * Active Support JSON and pass through verbatim. */
cf_err cf_cable_message_frame(cf_span encoded_identifier,
                              cf_span encoded_message, cf_str *out);

/* ---- socket.c: handshake, framing, deflate, bounded output ---------------- */

typedef struct cf_cable_socket cf_cable_socket;
typedef struct cf_cable_server cf_cable_server;

/* Limits from server.rs Config; a zero field selects the constant above. */
typedef struct {
    size_t max_message_bytes;  /* CF_CABLE_MAX_MESSAGE */
    size_t max_pending_bytes;  /* CF_CABLE_MAX_PENDING_BYTES */
    uint64_t write_timeout_ms; /* CF_CABLE_WRITE_STALL_MS */
    uint64_t close_timeout_ms; /* CF_CABLE_CLOSE_TIMEOUT_MS */
    uint64_t beat_interval_ms; /* CF_CABLE_BEAT_INTERVAL_MS; 0 disables */
} cf_cable_limits;

void cf_cable_limits_default(cf_cable_limits *out);

/* What the connection owner knows of the upgrade request. The bytes are
 * owned by the socket for the lifetime of the connection. */
typedef struct {
    cf_method method;
    cf_span target, path, query, peer_ip;
    const cf_header *headers;
    size_t header_count;
} cf_cable_request;

/* ApplicationCable::Connection#connect (A01): resolve the request to a user
 * id, or leave *authenticated false. Runs on the connection's owner thread
 * before the welcome frame; it may perform database reads but must not
 * touch the socket. */
typedef cf_err (*cf_cable_authenticate_fn)(void *user,
                                           const cf_cable_request *request,
                                           bool *authenticated,
                                           int64_t *user_id);

/* A text message assembled and inflated from the client. Runs on the
 * connection's owner thread. */
typedef cf_err (*cf_cable_on_text_fn)(void *user, cf_cable_socket *socket,
                                      cf_span text);

typedef struct {
    cf_cable_authenticate_fn authenticate;
    void *authenticate_user;
    cf_cable_on_text_fn on_text;
    void *on_text_user;
} cf_cable_hooks;

/* Per-connection transport counters ("must be counted", D-C04). frames_sent
 * counts only frames fully written; a queued frame is never delivered. */
typedef struct {
    uint64_t messages_received;
    uint64_t binary_messages;
    uint64_t frames_queued;
    uint64_t frames_sent;
    uint64_t bytes_sent;
    uint64_t pings_sent; /* server heartbeat pings queued */
    uint64_t pongs_sent; /* pongs written in reply to client pings */
    uint64_t close_frames_sent;
    uint64_t close_frames_received;
    uint64_t protocol_closes;
    uint64_t queue_cap_closes;
    uint64_t write_timeout_closes;
    uint64_t unauthorized_closes;
    uint64_t deflate_failures; /* compression failed; sent uncompressed */
} cf_cable_socket_stats;

void cf_cable_socket_stats_add(cf_cable_socket_stats *acc,
                               const cf_cable_socket_stats *part);

/* One immutable text payload shared by every connection that sends it; its
 * deflated variant is computed at most once (compressed and uncompressed
 * variants stay separate per socket negotiation). */
typedef struct cf_cable_frame cf_cable_frame;

cf_err cf_cable_frame_create(cf_span text, cf_cable_frame **out);
cf_cable_frame *cf_cable_frame_retain(cf_cable_frame *frame);
void cf_cable_frame_release(cf_cable_frame *frame);

/* Run one accepted WebSocket connection on the calling thread until it
 * closes. The fd stays owned by the caller (it must be closed after run
 * returns); handshake bytes, when present, are written first (the server
 * hook's 101 response). config->request is borrowed for the authentication
 * call only (the socket copies what the callback sees). */
typedef struct {
    int fd;                     /* borrowed: the caller closes it */
    bool deflate;               /* permessage-deflate negotiated */
    const char *subprotocol;    /* negotiated, or NULL */
    const unsigned char *handshake; /* 101 response bytes, or NULL */
    size_t handshake_len;
    /* Bytes read after the HTTP request head (already-received frames); the
     * socket copies them and feeds its reader before any new input. */
    const unsigned char *pending;
    size_t pending_len;
    const cf_cable_request *request;
} cf_cable_socket_config;

cf_err cf_cable_socket_run(const cf_cable_socket_config *config,
                           const cf_cable_hooks *hooks,
                           const cf_cable_limits *limits,
                           cf_cable_socket_stats *stats);

/* Queue one text frame, in order, for the connection owner to write. Safe
 * from any thread. CF_BUSY means the pending cap was reached and the
 * connection is being closed; CF_IO means the connection is already gone. */
cf_err cf_cable_socket_send_text(cf_cable_socket *socket, cf_span text);
cf_err cf_cable_socket_send_frame(cf_cable_socket *socket,
                                  cf_cable_frame *frame);

/* ---- server: the /cable front mount --------------------------------------- */

typedef struct {
    const char *mount_path;             /* NULL = CF_CABLE_DEFAULT_MOUNT */
    bool allow_same_origin_as_host;     /* server.rs default true */
    bool assume_ssl;                    /* server.rs default true */
    bool disable_request_forgery_protection;
    /* Exact Origin values accepted in addition to the same-origin rule. */
    const char *const *allowed_request_origins;
    size_t allowed_request_origins_len;
    cf_cable_hooks hooks;
    cf_cable_limits limits;             /* zeroed = defaults */
} cf_cable_server_config;

void cf_cable_server_config_default(cf_cable_server_config *out);

cf_err cf_cable_server_create(const cf_cable_server_config *config,
                              cf_cable_server **out);
/* Stops and joins every live connection, then releases the server. */
void cf_cable_server_destroy(cf_cable_server *server);

/* The cf_http_loop_config.upgrade hook: mount_path is matched exactly; a
 * request that is not a valid WebSocket upgrade is answered with the
 * reference 404 ("Page not found") and stays ordinary HTTP. */
cf_http_upgrade_result cf_cable_server_upgrade(void *user,
                                               cf_http_upgrade_request *request);

size_t cf_cable_server_connections(const cf_cable_server *server);
void cf_cable_server_stats(const cf_cable_server *server,
                           cf_cable_socket_stats *out);

/* ---- A01 session authentication ------------------------------------------- */

/* ApplicationCable::Connection's SessionAuthenticator: the signed
 * session_token cookie -> Session::find_by_token -> User::find_by_id.
 * reader is borrowed and used only on the calling connection thread. */
typedef struct {
    cf_db *reader;
    cf_span secret_key_base;
} cf_cable_session_auth;

cf_err cf_cable_session_authenticate(void *user,
                                     const cf_cable_request *request,
                                     bool *authenticated, int64_t *user_id);

#endif /* CF_CABLE_H */
