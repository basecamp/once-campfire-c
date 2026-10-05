/* H01 response output: segment list, partial writes, budgets and file
 * streaming.
 *
 * Pending output is a list of owned/retained segments; bytes leave in order.
 * Only resident segments count toward the per-connection 8 MiB budget and the
 * loop output budget - a file's remaining on-disk length never does. The
 * stalled-write timer starts when pending bytes become nonzero and is reset
 * only by positive send progress (01 H01). File bodies stream in 64 KiB
 * chunks read by the loop's file worker; one chunk is outstanding per
 * connection, and chunk completions pass the same generation/sequence checks
 * as task completions.
 */
#include "http_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <stdarg.h>

/* ------------------------------------------------------------- segments */

static size_t seg_unsent(const struct cf_http_out_seg *s) {
    return s->len - s->sent;
}

size_t cf_http_output_pending(const struct cf_http_conn *conn) {
    return conn->out_pending;
}

static void seg_free(struct cf_http_conn *conn, struct cf_http_out_seg *s) {
    size_t unsent = seg_unsent(s);
    if (unsent != 0) {
        conn->out_pending -= unsent;
        cf_http_loop_release_output(conn->loop, unsent);
    }
    if (s->hold != NULL) cf_buf_release(s->hold);
    free(s->owned);
    free(s);
}

void cf_http_output_reset(struct cf_http_conn *conn) {
    struct cf_http_out_seg *s = conn->out_head;
    conn->out_head = NULL;
    conn->out_tail = NULL;
    while (s != NULL) {
        struct cf_http_out_seg *next = s->next;
        seg_free(conn, s);
        s = next;
    }
    conn->out_pending = 0;
    conn->stall_ms = 0;
}

/* Append one segment. ptr/len are the bytes; hold is retained for the
 * segment's life, owned is freed with it. Enforces the 8 MiB per-connection
 * budget and the loop output budget before growing. */
static cf_err seg_push(struct cf_http_conn *conn, const unsigned char *ptr,
                       size_t len, cf_buf *hold, unsigned char *owned) {
    if (len == 0) {
        if (hold != NULL) cf_buf_release(hold);
        free(owned);
        return CF_OK;
    }
    if (len > CF_HTTP_OUTPUT_MAX - conn->out_pending) {
        if (hold != NULL) cf_buf_release(hold);
        free(owned);
        return CF_LIMIT;
    }
    if (!cf_http_loop_reserve_output(conn->loop, len)) {
        if (hold != NULL) cf_buf_release(hold);
        free(owned);
        return CF_LIMIT;
    }
    struct cf_http_out_seg *s = calloc(1, sizeof *s);
    if (s == NULL) {
        cf_http_loop_release_output(conn->loop, len);
        if (hold != NULL) cf_buf_release(hold);
        free(owned);
        return CF_NOMEM;
    }
    s->ptr = ptr;
    s->len = len;
    s->hold = hold;
    s->owned = owned;
    if (conn->out_tail != NULL) {
        conn->out_tail->next = s;
    } else {
        conn->out_head = s;
    }
    conn->out_tail = s;
    if (conn->out_pending == 0) {
        conn->stall_ms = cf_monotonic_ms(); /* pending became nonzero */
    }
    conn->out_pending += len;
    return CF_OK;
}

/* --------------------------------------------------------------- flush */

static void finish_flow(struct cf_http_conn *conn);

cf_http_write_result cf_http_output_apply(struct cf_http_conn *conn,
                                          ssize_t n, int err) {
    if (n > 0) {
        size_t left = (size_t)n;
        while (left != 0 && conn->out_head != NULL) {
            struct cf_http_out_seg *s = conn->out_head;
            size_t take = seg_unsent(s);
            if (take > left) take = left;
            s->sent += take;
            left -= take;
            conn->out_pending -= take;
            cf_http_loop_release_output(conn->loop, take);
            conn->stall_ms = cf_monotonic_ms(); /* positive send progress */
            if (seg_unsent(s) == 0) {
                conn->out_head = s->next;
                if (conn->out_head == NULL) conn->out_tail = NULL;
                if (s->hold != NULL) cf_buf_release(s->hold);
                free(s->owned);
                free(s);
            }
        }
        if (conn->out_pending == 0) finish_flow(conn);
        return CF_HTTP_WRITE_PROGRESS;
    }
    if (n == 0) {
        /* A zero-length send cannot happen with len > 0; wait for EPOLLOUT. */
        return CF_HTTP_WRITE_AGAIN;
    }
    if (err == EAGAIN || err == EWOULDBLOCK) return CF_HTTP_WRITE_AGAIN;
    if (err == EINTR) return CF_HTTP_WRITE_RETRY;
    return CF_HTTP_WRITE_FAILED;
}

/* Complete the current flow once every resident byte has left. */
static void finish_flow(struct cf_http_conn *conn) {
    if (conn->out_pending != 0) return;
    if (conn->file_stream != NULL) {
        if (conn->file_stream->remaining > 0) {
            cf_http_file_stream_pump(conn);
            return;
        }
        cf_http_conn_stream_abandon(conn); /* normal end: fd closes now */
    }
    if (conn->pending_kind == CF_HTTP_PENDING_INTERIM) {
        conn->pending_kind = CF_HTTP_PENDING_NONE;
        return;
    }
    if (conn->pending_kind == CF_HTTP_PENDING_FINAL) {
        conn->pending_kind = CF_HTTP_PENDING_NONE;
        cf_http_conn_response_done(conn, true);
    }
}

/* hyper caps a vectored flush at 64 buffers (MAX_WRITEV_BUFS in
 * hyper-1.11.1 src/proto/h1/io.rs); the port batches the same way. */
#define CF_HTTP_FLUSH_IOV_MAX 64

void cf_http_conn_flush(struct cf_http_conn *conn) {
    while (conn->fd >= 0 && conn->out_head != NULL) {
        /* The reference's HTTP/1 server queues the serialized head and the
         * first body bytes into one write buffer and flushes them with a
         * single vectored write (hyper io.rs WriteStrategy::Queue ->
         * poll_write_vectored), so head+body leave together instead of as
         * two small sends. MSG_NOSIGNAL keeps SIGPIPE suppressed per call
         * exactly as the previous single-segment send did. */
        struct iovec iov[CF_HTTP_FLUSH_IOV_MAX];
        int iovcnt = 0;
        for (struct cf_http_out_seg *s = conn->out_head;
             s != NULL && iovcnt < CF_HTTP_FLUSH_IOV_MAX; s = s->next) {
            size_t len = seg_unsent(s);
            if (len == 0) continue; /* defensive: empty segments never queue */
            iov[iovcnt].iov_base = (void *)(s->ptr + s->sent);
            iov[iovcnt].iov_len = len;
            iovcnt++;
        }
        if (iovcnt == 0) break; /* defensive: no unsent segment to write */

        ssize_t n;
        if (iovcnt == 1) {
            n = send(conn->fd, iov[0].iov_base, iov[0].iov_len,
                     MSG_NOSIGNAL);
        } else {
            struct msghdr msg;
            memset(&msg, 0, sizeof msg);
            msg.msg_iov = iov;
            msg.msg_iovlen = (size_t)iovcnt;
            n = sendmsg(conn->fd, &msg, MSG_NOSIGNAL);
        }
        cf_http_write_result r =
            cf_http_output_apply(conn, n, n < 0 ? errno : 0);
        if (r == CF_HTTP_WRITE_RETRY) continue;
        if (r != CF_HTTP_WRITE_PROGRESS) break;
        if (conn->state == CF_HTTP_STATE_CLOSED) break;
    }
    cf_http_conn_update_events(conn);
}

/* ------------------------------------------------------- internal replies */

cf_err cf_http_conn_queue_bytes(struct cf_http_conn *conn,
                                const unsigned char *bytes, size_t len) {
    unsigned char *copy = malloc(len == 0 ? 1 : len);
    if (copy == NULL) return CF_NOMEM;
    if (len != 0) memcpy(copy, bytes, len);
    return seg_push(conn, copy, len, NULL, copy);
}

static cf_err push_bytes(struct cf_http_conn *conn, const char *text,
                         size_t len) {
    return cf_http_conn_queue_bytes(conn, (const unsigned char *)text, len);
}

cf_err cf_http_conn_queue_continue(struct cf_http_conn *conn) {
    static const char interim[] = "HTTP/1.1 100 Continue\r\n\r\n";
    if (conn->pending_kind == CF_HTTP_PENDING_FINAL) return CF_BUSY;
    cf_err rc = push_bytes(conn, interim, sizeof interim - 1);
    if (rc == CF_OK && conn->pending_kind == CF_HTTP_PENDING_NONE) {
        conn->pending_kind = CF_HTTP_PENDING_INTERIM;
    }
    cf_http_conn_flush(conn);
    return rc;
}

cf_err cf_http_conn_queue_status(struct cf_http_conn *conn, unsigned status,
                                 bool close_after, const char *retry_after) {
    if (conn->pending_kind == CF_HTTP_PENDING_FINAL) return CF_BUSY;
    const char *reason = cf_http_status_reason(status);
    char head[256];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %u%s%s\r\n"
                     "Content-Length: 0\r\n"
                     "%s"
                     "Connection: %s\r\n\r\n",
                     status, reason[0] != '\0' ? " " : "", reason,
                     retry_after != NULL ? retry_after : "",
                     (close_after || conn->input_closed) ? "close"
                                                         : "keep-alive");
    if (n < 0 || (size_t)n >= sizeof head) return CF_LIMIT;
    cf_err rc = push_bytes(conn, head, (size_t)n);
    if (rc != CF_OK) {
        cf_http_conn_close(conn->loop, conn);
        return rc;
    }
    conn->pending_kind = CF_HTTP_PENDING_FINAL;
    conn->state = CF_HTTP_STATE_WRITING;
    if (close_after || conn->input_closed) conn->close_after = true;
    cf_http_conn_flush(conn);
    return CF_OK;
}

/* --------------------------------------------------------- response attach */

cf_err cf_http_conn_attach_response(struct cf_http_conn *conn,
                                    struct cf_http_task *task) {
    cf_http_serialized ser;
    cf_err rc = cf_http_response_serialize(&task->response, task->request,
                                           &ser);
    if (rc != CF_OK) return rc;

    /* Check the whole resident response against the loop output budget
     * before queueing anything, so a rejection can still answer 503. */
    size_t header_len = cf_buf_span(ser.headers).len;
    size_t needed = header_len;
    if (ser.send_body && task->response.body_kind == CF_BODY_BUFFER) {
        size_t body_len = cf_buf_span(task->response.body).len;
        if (body_len > SIZE_MAX - needed) {
            cf_buf_release(ser.headers);
            return CF_LIMIT;
        }
        needed += body_len;
    }
    if (needed > CF_HTTP_OUTPUT_MAX ||
        !cf_http_loop_output_fits(conn->loop, needed)) {
        cf_buf_release(ser.headers);
        return CF_LIMIT;
    }

    conn->close_after = conn->close_after || task->response.close_after ||
                        task->request->close_after || conn->input_closed;
    conn->pending_kind = CF_HTTP_PENDING_FINAL;
    conn->state = CF_HTTP_STATE_WRITING;

    rc = seg_push(conn, cf_buf_span(ser.headers).ptr,
                  cf_buf_span(ser.headers).len, ser.headers, NULL);
    if (rc != CF_OK) return rc;

    cf_response *resp = &task->response;
    if (ser.send_body) {
        if (resp->body_kind == CF_BODY_BUFFER) {
            cf_span body = cf_buf_span(resp->body);
            rc = seg_push(conn, body.ptr, body.len, cf_buf_retain(resp->body),
                          NULL);
            if (rc != CF_OK) return rc;
        } else if (resp->body_kind == CF_BODY_FILE) {
            struct cf_http_file_stream *stream = calloc(1, sizeof *stream);
            if (stream == NULL) return CF_NOMEM;
            atomic_init(&stream->refs, (size_t)1);
            stream->loop = conn->loop;
            stream->fd = resp->file_fd;
            stream->offset = resp->file_offset;
            stream->remaining = resp->file_length;
            resp->file_fd = -1; /* fd ownership moves to the stream */
            conn->file_stream = stream;
            cf_http_file_stream_pump(conn);
        }
    } else if (resp->body_kind == CF_BODY_FILE) {
        /* HEAD/204/304: no bytes are transmitted; the task release closes
         * the fd through cf_response_dispose. */
    }

    conn->task_attached_to_output = true;
    cf_http_conn_flush(conn);
    return CF_OK;
}

/* ------------------------------------------------------------ file body */

void cf_http_file_stream_retain(struct cf_http_file_stream *s) {
    if (s != NULL) atomic_fetch_add_explicit(&s->refs, (size_t)1,
                                             memory_order_relaxed);
}

void cf_http_file_stream_release(struct cf_http_file_stream *s) {
    if (s == NULL) return;
    if (atomic_fetch_sub_explicit(&s->refs, (size_t)1, memory_order_acq_rel) ==
        1) {
        if (s->fd >= 0) close(s->fd);
        free(s);
    }
}

void cf_http_conn_stream_abandon(struct cf_http_conn *conn) {
    struct cf_http_file_stream *s = conn->file_stream;
    if (s == NULL) return;
    conn->file_stream = NULL;
    s->abandoned = true;
    cf_http_file_stream_release(s); /* the chunk read, if any, holds a ref */
}

void cf_http_file_stream_pump(struct cf_http_conn *conn) {
    struct cf_http_file_stream *s = conn->file_stream;
    if (s == NULL || s->chunk_pending || s->abandoned) return;
    if (s->remaining == 0) return;
    struct cf_http_chunk_req *req = calloc(1, sizeof *req);
    if (req == NULL) {
        cf_http_conn_close(conn->loop, conn);
        return;
    }
    req->stream = s;
    req->conn = conn->id;
    req->sequence = conn->sequence;
    req->offset = s->offset;
    req->want = s->remaining < CF_HTTP_FILE_CHUNK ? (size_t)s->remaining
                                                  : CF_HTTP_FILE_CHUNK;
    s->chunk_pending = true;
    cf_http_file_stream_retain(s);
    cf_http_file_worker_submit(conn->loop, req);
}

/* Chunk result handling, called by the loop thread. */
void cf_http_file_chunk_complete(struct cf_http_loop *loop,
                                 struct cf_http_chunk_req *req) {
    struct cf_http_file_stream *s = req->stream;
    struct cf_http_conn *conn = NULL;
    if (!s->abandoned) {
        conn = cf_http_loop_find_conn(loop, req->conn);
        if (conn == NULL || conn->file_stream != s || conn->fd < 0 ||
            conn->sequence != req->sequence) {
            conn = NULL;
        }
    }

    if (conn == NULL || s->abandoned) {
        /* Stale chunk: release the buffer and the read reference only. The
         * fd closes when the connection's reference also drops. */
        free(req->data);
        req->data = NULL;
        s->chunk_pending = false;
        cf_http_file_stream_release(s);
        free(req);
        return;
    }

    s->chunk_pending = false;
    if (req->err != 0 || req->len == 0) {
        /* Read failure or premature EOF: the declared Content-Length cannot
         * be honored. Close the transport, then release the read reference. */
        free(req->data);
        req->data = NULL;
        cf_http_conn_close(loop, conn); /* abandons the stream */
        cf_http_file_stream_release(s);
        free(req);
        return;
    }

    s->offset += req->len;
    s->remaining -= req->len;
    size_t len = req->len;
    unsigned char *data = req->data;
    req->data = NULL;
    cf_err rc = seg_push(conn, data, len, NULL, data);
    if (rc != CF_OK) {
        free(data);
        cf_http_conn_close(loop, conn);
        cf_http_file_stream_release(s);
        free(req);
        return;
    }
    cf_http_file_stream_release(s); /* the resident segment owns the bytes */
    free(req);
    cf_http_conn_flush(conn);
}

/* --------------------------------------------------------- response done */

void cf_http_conn_response_done(struct cf_http_conn *conn, bool sent_ok) {
    struct cf_http_loop *loop = conn->loop;
    struct cf_http_task *task = conn->pending_task;
    conn->pending_task = NULL;

    if (!sent_ok) cf_http_output_reset(conn);
    if (conn->file_stream != NULL) cf_http_conn_stream_abandon(conn);

    if (task != NULL) {
        if (sent_ok) loop->counters.responses_sent++;
        cf_http_loop_release_task(loop, task);
    }
    conn->task_attached_to_output = false;

    if (!sent_ok || conn->close_after || conn->input_closed ||
        conn->must_close_input) {
        cf_http_conn_close(loop, conn);
        return;
    }
    conn->close_after = false;
    conn->must_close_input = false;
    conn->state = CF_HTTP_STATE_HEADERS;
    cf_http_request_start(conn, true);
    cf_http_conn_update_events(conn);
    /* Already-received pipelined bytes stay in bounded input storage and are
     * executed now that the preceding response has finished. */
    cf_http_request_input(conn, NULL, 0);
}
