/* H01 module-internal types and cross-file functions.
 *
 * Not a shared contract: only src/http/{loop,request,response,output}.c and
 * H01's tests include this header. 01-foundation-http.md fixes every limit
 * below; interpretations are called out individually. */
#ifndef CF_HTTP_INTERNAL_H
#define CF_HTTP_INTERNAL_H

#include "http.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

/* picohttpparser (vendor/src/picohttpparser; DEPS.json pin). Request headers
 * and chunk decoding only; framing policy lives in request.c. */
#include "picohttpparser.h"

/* ---- fixed limits (01 "Configuration: fixed defaults") ------------------ */

#define CF_HTTP_HEADER_MAX ((size_t)32768)   /* 32 KiB request headers */
#define CF_HTTP_HEADER_COUNT_MAX ((size_t)100)
#define CF_HTTP_TARGET_MAX ((size_t)8192)    /* 8 KiB request target */
#define CF_HTTP_BODY_MAX ((size_t)16 * 1024 * 1024) /* decoded body */
#define CF_HTTP_TRAILER_MAX ((size_t)8192)   /* bounded chunked trailers */
#define CF_HTTP_OUTPUT_MAX ((size_t)8 * 1024 * 1024) /* pending per conn */
#define CF_HTTP_FILE_CHUNK ((size_t)64 * 1024)
#define CF_HTTP_DEADLINE_MS ((uint64_t)30000) /* header/body/idle/stall */
#define CF_HTTP_DRAIN_MS ((uint64_t)5000)     /* shutdown drain */
#define CF_HTTP_READ_MAX ((size_t)65536)      /* one recv into loop scratch */
#define CF_HTTP_DEFAULT_CONNECTIONS ((size_t)2048)
#define CF_HTTP_DEFAULT_BYTES ((size_t)64 * 1024 * 1024)

/* Case-insensitive span/literal compare used across this module. */
static inline bool cf_http_span_ieq(const unsigned char *p, size_t n,
                                    const char *lit) {
    size_t m = 0;
    while (lit[m] != '\0') m++;
    if (n != m) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char a = p[i];
        unsigned char b = (unsigned char)lit[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

/* ---- connection state machine (01 H01) ---------------------------------- */

typedef enum {
    CF_HTTP_STATE_ACCEPTED = 0,
    CF_HTTP_STATE_HEADERS,
    CF_HTTP_STATE_BODY,
    CF_HTTP_STATE_WORKING, /* admitted task owns the request */
    CF_HTTP_STATE_WRITING, /* serialized response draining to the socket */
    CF_HTTP_STATE_CLOSED
} cf_http_state;

typedef enum {
    CF_HTTP_DL_NONE = 0,
    CF_HTTP_DL_HEADER,
    CF_HTTP_DL_BODY,
    CF_HTTP_DL_IDLE,
    CF_HTTP_DL_WRITE
} cf_http_deadline_kind;

/* One pending response segment. Bytes are sent strictly in segment order.
 * A segment either holds a reference to an immutable cf_buf (serialized
 * header block or a buffered response body: "Cache and pending output take
 * separate refs") or owns a malloc'd file chunk read by the file worker. */
struct cf_http_out_seg {
    struct cf_http_out_seg *next;
    const unsigned char *ptr;
    size_t len;   /* total segment bytes */
    size_t sent;  /* bytes already accepted by the socket */
    cf_buf *hold; /* retained reference, or NULL */
    unsigned char *owned; /* malloc'd bytes to free, or NULL */
};

/* File body stream. Refcounted because a chunk read can be in flight while
 * the connection closes: the fd closes only when the last reference goes. */
struct cf_http_file_stream {
    atomic_size_t refs;
    cf_http_loop *loop;
    int fd;
    uint64_t offset;    /* next chunk file offset */
    uint64_t remaining; /* bytes still to send */
    bool chunk_pending; /* one outstanding chunk per connection (01 H01) */
    bool abandoned;     /* connection gone; close when reads return */
};

/* A file chunk read request/result. The worker allocates data; the loop
 * either attaches it as an output segment or frees it. */
struct cf_http_chunk_req {
    struct cf_http_chunk_req *next;
    struct cf_http_file_stream *stream; /* holds one reference */
    cf_conn_id conn;
    uint64_t sequence;
    uint64_t offset;
    size_t want;
    unsigned char *data; /* worker-allocated */
    size_t len;          /* bytes read (0 on error) */
    int err;             /* 0, or errno/ENOMEM */
};

struct cf_http_loop;

struct cf_http_conn {
    struct cf_http_loop *loop;
    cf_conn_id id;
    int fd;                /* -1 once closed */
    cf_http_state state;
    uint32_t events;       /* current epoll interest */
    bool registered;       /* in epoll set */
    bool input_closed;     /* peer half-closed / EOF observed */
    bool close_after;      /* this response closes the connection */
    bool must_close_input; /* framing error: stop reading, close after send */
    uint64_t sequence;     /* requests started on this connection */
    cf_http_deadline_kind deadline_kind;
    uint64_t deadline_ms;  /* monotonic; 0 = none */

    /* Request input. `in` holds unconsumed raw bytes starting at the current
     * parse position (the Content-Length body, or pipelined bytes). The
     * validated header block is copied once into `head` (stable memory);
     * `hdr_refs` address headers as offsets into it. Chunked decoding
     * appends decoded bytes to `body` and trailer bytes to `trailers`. */
    cf_builder in;
    cf_builder head;
    cf_builder body;
    cf_builder trailers;
    struct cf_http_hdr_ref {
        size_t name_off, name_len, value_off, value_len;
    } hdr_refs[CF_HTTP_HEADER_COUNT_MAX];
    size_t hdr_count;
    size_t off_method, len_method;
    size_t off_target, len_target;
    size_t off_path, len_path;
    size_t off_query, len_query;
    cf_method method;
    bool expect_100;
    bool chunked;
    bool chunk_done;       /* decoder reached the last chunk */
    struct phr_chunked_decoder chunk_dec;
    uint64_t body_expected; /* Content-Length (0 when none) */
    bool trailers_done; /* chunked trailer section finished */
    /* True once a complete request has been parsed and offered; guards
     * freeze/admit from running twice. */
    bool request_offered;

    /* Admitted request awaiting/being written (loop thread only). */
    struct cf_http_task *pending_task;
    /* The task's response is serialized into pending output; conn_close
     * releases the task itself (no later completion will). */
    bool task_attached_to_output;
    /* What the pending output is: none, an interim 100-continue, or the one
     * final response of the current request. */
    enum { CF_HTTP_PENDING_NONE = 0, CF_HTTP_PENDING_INTERIM,
           CF_HTTP_PENDING_FINAL } pending_kind;

    /* Pending output. */
    struct cf_http_out_seg *out_head, *out_tail;
    size_t out_pending;    /* logical unsent bytes (budget + stall timer) */
    uint64_t stall_ms;     /* when pending became nonzero / last progress */
    struct cf_http_file_stream *file_stream;

    /* Loop input reservation held by this connection's live buffers and by
     * its admitted task's frozen request. */
    size_t input_reserved;
    /* Output reservation held by pending segments. */
    size_t output_reserved;

    char peer_ip[64];
};

/* One admitted request: one allocation includes the completion link, so a
 * failing second allocation can never lose the completion (00-contracts.md). */
struct cf_http_task {
    cf_http_loop *loop;
    cf_conn_id conn;
    uint64_t sequence;
    cf_request *request;
    cf_response response; /* copied from the worker on submit */
    bool has_response;
    bool abandoned;       /* abandoned without a response */
    bool released;
    size_t input_reserved; /* frozen request backing reservation */
    atomic_int submit_state; /* 0 admitted, 1 submitted/abandoned */
    struct cf_http_task *next; /* completion queue link */
};

/* File chunk worker (one per loop; joined by destroy). */
struct cf_http_file_worker {
    pthread_t thread;
    bool started;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    struct cf_http_chunk_req *head, *tail;
    bool stop;
};

struct cf_http_loop {
    cf_http_loop_config cfg;
    char *public_origin; /* copied */
    char origin_host[256];
    size_t origin_host_len;
    unsigned origin_port;
    bool origin_explicit_port;
    bool origin_is_https;

    int epfd;
    int wake_fd; /* eventfd: completions and stop */
    bool stopping;
    bool run_called;
    bool run_returned;
    uint64_t drain_deadline_ms; /* shutdown drain deadline (monotonic) */

    struct cf_http_conn *conns; /* fixed slot array (stable memory) */
    size_t conn_cap;
    size_t active_conns;
    int32_t *free_slots;
    size_t free_count;

    unsigned char *scratch; /* CF_HTTP_READ_MAX loop-thread read buffer */

    /* Completion queue: mutex + eventfd, bounded by admitted tasks (task
     * links are preallocated inside the task allocation). */
    pthread_mutex_t completion_mutex;
    struct cf_http_task *task_head, *task_tail;
    struct cf_http_chunk_req *chunk_head, *chunk_tail;

    struct cf_http_file_worker file_worker;

    size_t input_used;
    size_t output_used;

    _Atomic bool stop_flag;

    cf_http_counters counters;
    uint64_t outstanding_tasks; /* admitted, not yet released */
};

/* ---- request.c ---------------------------------------------------------- */

/* Reset per-request parse state (keeps connection identity/deadlines). */
void cf_http_request_start(struct cf_http_conn *conn, bool idle);
/* Feed bytes read from the socket. Drives HEADERS/BODY; on completion freezes
 * and offers the request through conn->loop's admit hook. */
void cf_http_request_input(struct cf_http_conn *conn, const unsigned char *data,
                           size_t len);
/* Peer stopped writing. */
void cf_http_request_eof(struct cf_http_conn *conn);
/* Release parse buffers and per-request reservations. */
void cf_http_request_abort(struct cf_http_conn *conn);
/* Release one request and its input reservation ("frozen request backing"). */
void cf_http_request_free_input(struct cf_http_loop *loop, cf_request *req,
                                size_t reserved);

/* ---- output.c ----------------------------------------------------------- */

/* Queue an internally generated status response (no body beyond a short
 * fixed text; 01 error translation). close_after forces Connection: close. */
cf_err cf_http_conn_queue_status(struct cf_http_conn *conn, unsigned status,
                                 bool close_after, const char *retry_after);
/* Queue the validated interim "HTTP/1.1 100 Continue" response. */
cf_err cf_http_conn_queue_continue(struct cf_http_conn *conn);
/* Append raw owned bytes as an output segment (internal reply path; also the
 * deterministic seam for H01's short-write/EINTR/EAGAIN unit tests). */
cf_err cf_http_conn_queue_bytes(struct cf_http_conn *conn,
                                const unsigned char *bytes, size_t len);
/* Finish the current response: release the task, then close or start the
 * next request on this connection. sent_ok is false on transport failure. */
void cf_http_conn_response_done(struct cf_http_conn *conn, bool sent_ok);
/* Serialize and attach a completed task response ("headers stay stable in
 * the pending response": serialization happens once here). */
cf_err cf_http_conn_attach_response(struct cf_http_conn *conn,
                                    struct cf_http_task *task);
/* Send as much pending output as the socket accepts; handles short writes,
 * EAGAIN, EINTR and completion transitions. */
void cf_http_conn_flush(struct cf_http_conn *conn);
/* Apply one send outcome (n = bytes sent > 0, or -1 with errno) to the output
 * state. Separated so tests can drive short-write/EINTR/EAGAIN sequences
 * deterministically; the loop calls it through cf_http_conn_flush(). */
typedef enum {
    CF_HTTP_WRITE_PROGRESS = 0, /* made progress; keep flushing */
    CF_HTTP_WRITE_AGAIN,        /* wait for EPOLLOUT */
    CF_HTTP_WRITE_RETRY,        /* EINTR: retry immediately */
    CF_HTTP_WRITE_FAILED        /* transport failure: close */
} cf_http_write_result;
cf_http_write_result cf_http_output_apply(struct cf_http_conn *conn,
                                          ssize_t n, int err);
/* Drop all pending output (connection close) and release reservations. */
void cf_http_output_reset(struct cf_http_conn *conn);
/* Pending unsent bytes (resident only; file remaining does not count). */
size_t cf_http_output_pending(const struct cf_http_conn *conn);
/* File stream lifetime; the loop calls attach/detach around a file body. */
void cf_http_file_stream_retain(struct cf_http_file_stream *s);
void cf_http_file_stream_release(struct cf_http_file_stream *s);
/* Ask the file worker for the next 64 KiB chunk (one outstanding). */
void cf_http_file_stream_pump(struct cf_http_conn *conn);
/* Detach the connection's file stream; closes the fd when reads return. */
void cf_http_conn_stream_abandon(struct cf_http_conn *conn);
/* Handle one completed chunk read (loop thread, from the completion queue). */
void cf_http_file_chunk_complete(struct cf_http_loop *loop,
                                 struct cf_http_chunk_req *req);

/* ---- response.c --------------------------------------------------------- */

/* Serialize status line, Content-Length, Date, Connection and application
 * headers into one cf_buf. HEAD/204/304 transmit no body; the caller decides
 * body attachment from the returned flags. */
typedef struct {
    cf_buf *headers;
    bool send_body;      /* body bytes (if any) are transmitted */
    uint64_t body_length; /* Content-Length value that was written */
} cf_http_serialized;

cf_err cf_http_response_serialize(const cf_response *resp,
                                  const cf_request *req,
                                  cf_http_serialized *out);

/* Standard reason phrase ("" when unknown); never NULL. */
const char *cf_http_status_reason(unsigned status);

/* -------- freeze/admission hooks implemented in loop.c ------------------- */

/* Offer a frozen request; callbacks into the admit hook. Returns CF_OK when
 * the callee owns the task, CF_BUSY when declined (loop answers 503). */
cf_err cf_http_loop_admit(struct cf_http_loop *loop, struct cf_http_conn *conn,
                          cf_request *req, size_t input_reserved,
                          uint64_t sequence);
/* Close a connection immediately (errors, EOF, drain). */
void cf_http_conn_close(struct cf_http_loop *loop, struct cf_http_conn *conn);
/* Release a task (request, response, reservation, allocation) exactly once. */
void cf_http_loop_release_task(struct cf_http_loop *loop,
                               struct cf_http_task *task);
/* Recompute epoll interest from state + pending output. */
void cf_http_conn_update_events(struct cf_http_conn *conn);
/* Deadline check used by the loop and by tests (injected monotonic clock). */
void cf_http_conn_check_deadline(struct cf_http_conn *conn, uint64_t now_ms);
/* Reserve/release loop-wide input/output logical bytes. */
bool cf_http_loop_reserve_input(struct cf_http_loop *loop, size_t bytes);
void cf_http_loop_release_input(struct cf_http_loop *loop, size_t bytes);
bool cf_http_loop_reserve_output(struct cf_http_loop *loop, size_t bytes);
void cf_http_loop_release_output(struct cf_http_loop *loop, size_t bytes);
/* True when `bytes` more pending output would fit the loop budget. */
bool cf_http_loop_output_fits(const struct cf_http_loop *loop, size_t bytes);

/* Resolve a completion's (slot, generation) against the live connection
 * table; returns NULL for a stale id or a slot outside the table. */
struct cf_http_conn *cf_http_loop_find_conn(struct cf_http_loop *loop,
                                            cf_conn_id id);
/* Hand a chunk read to this loop's file worker. */
void cf_http_file_worker_submit(struct cf_http_loop *loop,
                                struct cf_http_chunk_req *req);

#endif /* CF_HTTP_INTERNAL_H */
