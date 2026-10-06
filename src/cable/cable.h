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
 * leaves HTTP and is owned by this module: each upgraded connection is
 * enrolled on exactly one bounded cable reactor thread per HTTP loop index
 * (counted in the loop budget, never per connection), which is the only
 * thread that sends, closes or mutates its sockets. The upgraded connection
 * keeps occupying its HTTP connection-slot reservation for its whole
 * lifetime (see cf_http_upgrade_lease); the reactor releases that lease when
 * the socket ends, whatever the ending (close, reset, timeout, shutdown).
 * The reactor performs socket I/O only: authentication and model work are
 * handed to the app's bounded request-worker pool (cf_app_submit_worker),
 * and their results are installed on the owner thread through the C03
 * install gate/version check. No DB reader lives on the reactor; no
 * per-connection threads or readers exist. */
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
/* Upper bound on distinct HTTP loop indexes one server services: one bounded
 * reactor thread per index, never one thread per connection. */
#define CF_CABLE_MAX_REACTORS ((size_t)64)
/* Default process-wide aggregate input/output budgets for upgraded sockets
 * (divided across the configured loops). */
#define CF_CABLE_DEFAULT_REACTOR_BYTES ((size_t)64 << 20)

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
typedef struct cf_cable cf_cable; /* C02 application object (channels.h) */

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
 * id, or leave *authenticated false. Runs on the socket's owner thread
 * before the welcome frame; a synchronous implementation (a test/standalone
 * hook, or a hook with no worker pool) may perform database reads but must
 * not touch the socket's transport. The production wiring instead submits the
 * model work to the app's bounded request-worker pool and returns CF_BUSY:
 * the socket then defers the welcome/unauthorized step until the owner thread
 * calls cf_cable_socket_complete_auth with the result (installed through the
 * C03 gate). The hook may attach per-connection application state to the
 * socket (cf_cable_socket_set_app_user) and must release it from on_close;
 * socket is the connection the result belongs to. Any other non-CF_OK value
 * refuses the connection. */
typedef cf_err (*cf_cable_authenticate_fn)(void *user, cf_cable_socket *socket,
                                           const cf_cable_request *request,
                                           bool *authenticated,
                                           int64_t *user_id);

/* A text message assembled and inflated from the client. Runs on the
 * connection's owner thread. CF_BUSY means the wiring handed the command's
 * model work to a worker and needs the socket to stop feeding commands until
 * the owner thread resumes it with cf_cable_socket_resume (message ordering
 * is preserved: later frames stay buffered and unparsed). Any other value
 * keeps parsing. */
typedef cf_err (*cf_cable_on_text_fn)(void *user, cf_cable_socket *socket,
                                      cf_span text);

/* Owner thread: consume pending per-connection service work (C03 revocation
 * control) for this specific socket, independent of every other socket the
 * same owner services. */
typedef void (*cf_cable_service_fn)(void *user, cf_cable_socket *socket);

typedef struct {
    cf_cable_authenticate_fn authenticate;
    void *authenticate_user;
    /* Owner thread, once authentication succeeded and before the welcome
     * frame: attach the application loop and install the connection's
     * authorization result (C03). A non-CF_OK result refuses the connection
     * (the unauthorized disconnect path). Optional. */
    cf_err (*on_open)(void *user, cf_cable_socket *socket);
    void *on_open_user;
    cf_cable_on_text_fn on_text;
    void *on_text_user;
    /* Owner thread, once per poll iteration and again after the wake eventfd
     * was drained: consume pending revocation control (C03). Optional. */
    cf_cable_service_fn service;
    void *service_user;
    /* Owner thread, once the connection has stopped and before the socket
     * releases itself: detach anything that can still send to this socket, so
     * no foreign send observes freed memory (C03 lifetime). Optional. */
    void (*on_close)(void *user, cf_cable_socket *socket);
    void *on_close_user;

    /* Owner loop (the shared transport owner: one bounded cable reactor, or a
     * standalone run's calling thread). owner_start runs once on the owner
     * thread before it services any socket; `token` is the value
     * cf_cable_socket_transport_token returns for its sockets, and
     * wake_owner(token) writes the owner's wake eventfd from any thread
     * (the C03 barrier's wake callback). The wiring registers the loop's one
     * preallocated C03 control slot here (cf_cable_revocation_loop_register).
     * A non-CF_OK result makes the owner unusable (the upgrade is refused).
     * owner_stop runs once after every socket of the owner has finished and
     * before the owner thread exits; the wiring unregisters the slot there.
     * Both are optional (NULL = no per-owner wiring; channel work fails
     * closed with CF_INTERNAL rather than pretending a revocation can reach
     * it). */
    cf_err (*owner_start)(void *user, void *token,
                          void (*wake_owner)(void *token));
    void *owner_start_user;
    void (*owner_stop)(void *user, void *token);
    void *owner_stop_user;
} cf_cable_hooks;

/* Per-socket application context for the hooks above (O(1), owner thread).
 * The transport itself never interprets it. */
void *cf_cable_socket_app_user(const cf_cable_socket *socket);
void cf_cable_socket_set_app_user(cf_cable_socket *socket, void *user);

/* The upgrade request the socket was created from, borrowed for the socket's
 * whole lifetime (NULL for a direct cf_cable_socket_run without one). */
const cf_cable_request *cf_cable_socket_request(const cf_cable_socket *socket);

/* Non-NULL when the socket is serviced by a shared cable reactor (the
 * production transport): a stable per-reactor token that keys the owner
 * loop's one shared C03 control slot (one per HTTP loop; never one per
 * connection). NULL for a standalone cf_cable_socket_run. */
void *cf_cable_socket_transport_token(const cf_cable_socket *socket);

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
    /* Aggregate per-reactor input/output budget exceeded (the connection is
     * closed; per-socket limits were not the cause). */
    uint64_t over_budget_closes;
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
    struct cf_front_tls_conn *tls; /* optional, borrowed TLS transport */
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

/* Owner thread: finish an upgrade whose authenticate hook returned CF_BUSY.
 * Installs the asynchronous result exactly like a synchronous hook would:
 * the welcome (or the reference unauthorized disconnect + close) is queued,
 * the connection's on_open runs, and bytes the client pipelined after the
 * handshake are consumed. `authenticated` is the value the worker resolved;
 * user_id is informational for the wiring (the hook contract keeps the
 * identity on the socket's app_user). Idempotent after the first call. */
cf_err cf_cable_socket_complete_auth(cf_cable_socket *socket,
                                     bool authenticated, int64_t user_id);

/* Owner thread: resume a socket paused by an on_text hook's CF_BUSY, feeding
 * any commands already buffered behind the paused one. No-op when the socket
 * is not paused or is finishing. */
void cf_cable_socket_resume(cf_cable_socket *socket);

/* Test-only measurement of where cable model work executed (P12-02b): the
 * last connection authentication and the last subscription validation/effect.
 * `ran` is false until one completed; `on_worker` is true when the executing
 * thread held a request-worker reader (cf_app_worker_reader); `thread` is the
 * executor and `owner_thread` the owner loop that submitted it; owner_has_reader
 * records whether the owner loop still held a DB reader at submit time (the
 * production contract: false). */
typedef struct {
    bool ran;
    bool on_worker;
    uintptr_t thread;
    uintptr_t owner_thread;
    bool owner_has_reader;
} cf_cable_work_identity;

/* `subscribe` selects the subscription identity (validate/effect) instead of
 * the connection authentication. Zeroed when nothing ran yet. */
void cf_cable_test_work_identity(const cf_cable *cable, bool subscribe,
                                 cf_cable_work_identity *out);
void cf_cable_test_reset_work_identity(cf_cable *cable);

/* Queue one text frame, in order, for the connection owner to write. Safe
 * from any thread. CF_BUSY means the pending cap was reached and the
 * connection is being closed; CF_IO means the connection is already gone. */
cf_err cf_cable_socket_send_text(cf_cable_socket *socket, cf_span text);
cf_err cf_cable_socket_send_frame(cf_cable_socket *socket,
                                  cf_cable_frame *frame);

/* Write the socket's poll eventfd (the C03 wake callback). Lock-free and safe
 * from any thread, but the caller must guarantee the socket outlives the
 * call: production loops unregister their revocation slot before the socket
 * is freed (on_close), so a barrier cannot wake a freed socket. */
void cf_cable_socket_wake(cf_cable_socket *socket);

/* Owner thread: begin the reference close (1000 close frame queued after
 * everything already pending, then wait for the peer's close, bounded by
 * close_timeout). Used by the C03 revoke path. */
void cf_cable_socket_request_close(cf_cable_socket *socket);

/* True when the head outbound node is an application frame partially sent
 * (some bytes accepted, not all), so a disconnect frame must not interleave.
 * Owner thread (the C03 frame_partial callback). */
bool cf_cable_socket_frame_partial(const cf_cable_socket *socket);

/* ---- server: the /cable front mount --------------------------------------- */

typedef struct {
    const char *mount_path;             /* NULL = CF_CABLE_DEFAULT_MOUNT */
    bool allow_same_origin_as_host;     /* server.rs default true */
    bool assume_ssl;                    /* server.rs default true */
    bool disable_request_forgery_protection;
    /* Exact Origin values accepted in addition to the same-origin rule. */
    const char *const *allowed_request_origins;
    size_t allowed_request_origins_len;
    /* Number of HTTP loops whose upgraded sockets this server may service
     * (0 = CF_CABLE_MAX_REACTORS default). One bounded reactor thread per
     * loop index, created on first use, never one per connection. */
    size_t loops;
    /* Process-wide aggregate budgets for upgraded connections, divided across
     * the loops (0 = the 64 MiB defaults). While an upgrade lives it counts
     * against its reactor's share, independent of the HTTP connection-slot
     * admission, which the upgrade keeps occupying for its whole lifetime. */
    size_t input_bytes;
    size_t output_bytes;
    cf_cable_hooks hooks;
    cf_cable_limits limits;             /* zeroed = defaults */
} cf_cable_server_config;

void cf_cable_server_config_default(cf_cable_server_config *out);

cf_err cf_cable_server_create(const cf_cable_server_config *config,
                              cf_cable_server **out);
/* Stop serving and join every reactor: the upgraded sockets are shut down
 * and their lifetime reservations released, but the server object stays
 * valid. Enforced caller order (P12-02b; main.c follows it):
 *   1. stop the HTTP loops (no new upgrade hook calls),
 *   2. cf_cable_server_stop(): joins every reactor, which submits each
 *      upgrade's lifetime lease for release and shuts its sockets down,
 *   3. drain/join the HTTP loop threads: their completion handlers finish the
 *      lease releases and free the connection slots (this is the "release
 *      leases" step; stop itself returns once every reactor has joined, not
 *      once every loop has processed its release),
 *   4. cf_cable_server_destroy(): only now, when no loop thread may still be
 *      inside the upgrade hook and every lease is released.
 * Calling destroy before the HTTP loops have returned is a use-after-free
 * (a loop could still call cf_cable_server_upgrade on the freed server); the
 * server cannot observe the loops, so the order is a caller contract.
 * Async-safe and idempotent; cf_cable_server_destroy calls it first. */
void cf_cable_server_stop(cf_cable_server *server);
/* Stops and joins every reactor, then releases the server. Only call after
 * the HTTP loops that may still call cf_cable_server_upgrade have returned
 * (cf_cable_server_stop, join the loop threads, then destroy: see the order
 * above). */
void cf_cable_server_destroy(cf_cable_server *server);

/* The cf_http_loop_config.upgrade hook: mount_path is matched exactly; a
 * request that is not a valid WebSocket upgrade is answered with the
 * reference 404 ("Page not found") and stays ordinary HTTP. */
cf_http_upgrade_result cf_cable_server_upgrade(void *user,
                                               cf_http_upgrade_request *request);

size_t cf_cable_server_connections(const cf_cable_server *server);
/* Started reactor threads (one per HTTP loop index with upgraded sockets);
 * a measurement seam, never per-connection. */
size_t cf_cable_server_reactors(const cf_cable_server *server);
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
