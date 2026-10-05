/* H01 event loop: level-triggered epoll, connection slots with generation
 * handles, admission handoff, completion queue (mutex + eventfd), the file
 * chunk worker and orderly shutdown.
 *
 * Connections are confined to the loop thread; integer handles are
 * (loop, slot, generation), and reusing a slot increments its 64-bit
 * generation. Task completions and file chunk results are validated against
 * both connection generation and request sequence, so a stale completion
 * releases its resources without ever touching a reused descriptor
 * (00-contracts.md CORE-03).
 */
#include "http_internal.h"

#include "app_internal.h" /* cf_app_stop_requested (F03 internal surface) */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#define CF_HTTP_MAX_EVENTS 128

/* ------------------------------------------------------------- budget */

static size_t loop_input_limit(const struct cf_http_loop *loop) {
    return loop->cfg.input_bytes != 0 ? loop->cfg.input_bytes
                                      : CF_HTTP_DEFAULT_BYTES;
}

static size_t loop_output_limit(const struct cf_http_loop *loop) {
    return loop->cfg.output_bytes != 0 ? loop->cfg.output_bytes
                                       : CF_HTTP_DEFAULT_BYTES;
}

bool cf_http_loop_reserve_input(struct cf_http_loop *loop, size_t bytes) {
    size_t limit = loop_input_limit(loop);
    if (bytes > limit - loop->input_used) return false;
    loop->input_used += bytes;
    return true;
}

void cf_http_loop_release_input(struct cf_http_loop *loop, size_t bytes) {
    if (bytes <= loop->input_used) loop->input_used -= bytes;
    else loop->input_used = 0;
}

bool cf_http_loop_reserve_output(struct cf_http_loop *loop, size_t bytes) {
    size_t limit = loop_output_limit(loop);
    if (bytes > limit - loop->output_used) return false;
    loop->output_used += bytes;
    return true;
}

bool cf_http_loop_output_fits(const struct cf_http_loop *loop, size_t bytes) {
    size_t limit = loop_output_limit(loop);
    return bytes <= limit - loop->output_used;
}

void cf_http_loop_release_output(struct cf_http_loop *loop, size_t bytes) {
    if (bytes <= loop->output_used) loop->output_used -= bytes;
    else loop->output_used = 0;
}

/* --------------------------------------------------------- origin parse */

/* Parse the absolute origin into host (without brackets), port and scheme.
 * cf_config.parse_origin already validated the shape; this is the loop-side
 * check so tests and A00 get the same rule. */
static bool parse_origin(const char *origin, char *host, size_t host_cap,
                         size_t *host_len, unsigned *port, bool *explicit_port,
                         bool *is_https) {
    if (origin == NULL) return false;
    const char *p = origin;
    bool https;
    if (strncasecmp(p, "https://", 8) == 0) {
        https = true;
        p += 8;
    } else if (strncasecmp(p, "http://", 7) == 0) {
        https = false;
        p += 7;
    } else {
        return false;
    }
    const char *host_start = p;
    if (*p == '[') {
        p++;
        host_start = p;
        while (*p != '\0' && *p != ']') p++;
        if (*p != ']') return false;
    } else {
        while (*p != '\0' && *p != ':' && *p != '/') p++;
    }
    size_t hlen = (size_t)(p - host_start);
    if (hlen == 0 || hlen >= host_cap || hlen > 255) return false;
    memcpy(host, host_start, hlen);
    host[hlen] = '\0';

    unsigned prt = https ? 443u : 80u;
    bool have = false;
    if (*p == ']') p++;
    if (*p == ':') {
        p++;
        if (*p < '0' || *p > '9') return false;
        unsigned v = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (unsigned)(*p - '0');
            if (v > 65535) return false;
            p++;
            digits++;
        }
        if (digits == 0 || v == 0) return false;
        prt = v;
        have = true;
    }
    if (*p != '\0') return false; /* no path/query/userinfo */
    *host_len = hlen;
    *port = prt;
    *explicit_port = have;
    *is_https = https;
    return true;
}

/* ------------------------------------------------------------- wakeups */

static void loop_wake(struct cf_http_loop *loop) {
    uint64_t one = 1;
    ssize_t n = write(loop->wake_fd, &one, sizeof one);
    (void)n; /* EAGAIN means a wake is already pending */
}

static void loop_drain_wake(struct cf_http_loop *loop) {
    uint64_t v;
    while (read(loop->wake_fd, &v, sizeof v) > 0) {
    }
}

/* ------------------------------------------------------------- slots */

struct cf_http_conn *cf_http_loop_find_conn(struct cf_http_loop *loop,
                                            cf_conn_id id) {
    if (id.slot >= loop->conn_cap) return NULL;
    struct cf_http_conn *conn = &loop->conns[id.slot];
    if (conn->id.generation != id.generation) return NULL;
    return conn;
}

void cf_http_conn_update_events(struct cf_http_conn *conn) {
    if (conn->fd < 0) return;
    uint32_t ev = EPOLLRDHUP;
    switch (conn->state) {
    case CF_HTTP_STATE_ACCEPTED:
    case CF_HTTP_STATE_HEADERS:
    case CF_HTTP_STATE_BODY:
        ev |= EPOLLIN;
        break;
    default:
        break;
    }
    if (conn->out_pending > 0) ev |= EPOLLOUT;

    struct epoll_event e = {.events = ev, .data.ptr = conn};
    int op = conn->registered ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (!conn->registered) {
        if (epoll_ctl(conn->loop->epfd, op, conn->fd, &e) == 0) {
            conn->registered = true;
            conn->events = ev;
        }
        return;
    }
    if (ev == conn->events) return;
    if (epoll_ctl(conn->loop->epfd, EPOLL_CTL_MOD, conn->fd, &e) == 0) {
        conn->events = ev;
    }
}

void cf_http_conn_check_deadline(struct cf_http_conn *conn, uint64_t now_ms) {
    if (conn->fd < 0) return;
    bool expired = false;
    if (conn->deadline_kind != CF_HTTP_DL_NONE && conn->deadline_ms != 0 &&
        now_ms >= conn->deadline_ms) {
        expired = true;
    }
    if (!expired && conn->out_pending > 0 && conn->stall_ms != 0 &&
        now_ms - conn->stall_ms >= CF_HTTP_DEADLINE_MS) {
        expired = true; /* stalled-write: no positive progress */
    }
    if (expired) {
        conn->loop->counters.timeouts++;
        cf_http_conn_close(conn->loop, conn);
    }
}

void cf_http_conn_close(struct cf_http_loop *loop, struct cf_http_conn *conn) {
    if (conn == NULL || conn->state == CF_HTTP_STATE_CLOSED) return;
    if (conn->fd >= 0) {
        if (conn->registered) {
            (void)epoll_ctl(loop->epfd, EPOLL_CTL_DEL, conn->fd, NULL);
            conn->registered = false;
        }
        close(conn->fd);
        conn->fd = -1;
        if (loop->active_conns != 0) loop->active_conns--;
    }
    cf_http_conn_stream_abandon(conn);
    cf_http_output_reset(conn);
    if (conn->pending_task != NULL) {
        if (conn->task_attached_to_output) {
            /* The response bytes are dropped with the socket; the task has no
             * later completion. */
            cf_http_loop_release_task(loop, conn->pending_task);
        }
        /* else: WORKING; the outstanding completion arrives stale. */
        conn->pending_task = NULL;
    }
    conn->task_attached_to_output = false;
    cf_http_request_abort(conn);
    conn->state = CF_HTTP_STATE_CLOSED;
    conn->pending_kind = CF_HTTP_PENDING_NONE;
    conn->deadline_kind = CF_HTTP_DL_NONE;
    conn->deadline_ms = 0;
    conn->events = 0;
    conn->chunk_done = false;
    conn->trailers_done = false;
    if (loop->free_count < loop->conn_cap) {
        loop->free_slots[loop->free_count++] = conn->id.slot;
    }
}

/* --------------------------------------------------------- task release */

void cf_http_loop_release_task(struct cf_http_loop *loop,
                               struct cf_http_task *task) {
    if (task == NULL || task->released) return;
    task->released = true;
    if (task->request != NULL) {
        cf_http_request_free_input(loop, task->request, task->input_reserved);
        task->request = NULL;
    }
    if (task->has_response) {
        cf_response_dispose(&task->response);
        task->has_response = false;
    }
    if (loop->outstanding_tasks != 0) loop->outstanding_tasks--;
    free(task);
}

/* ------------------------------------------------------------- admission */

cf_err cf_http_loop_admit(struct cf_http_loop *loop, struct cf_http_conn *conn,
                          cf_request *req, size_t input_reserved,
                          uint64_t sequence) {
    loop->counters.requests++;
    if (loop->stopping || atomic_load_explicit(&loop->stop_flag,
                                               memory_order_relaxed)) {
        loop->counters.admission_rejected++;
        cf_http_request_free_input(loop, req, input_reserved);
        (void)cf_http_conn_queue_status(conn, 503, true,
                                        "Retry-After: 1\r\n");
        return CF_BUSY;
    }

    struct cf_http_task *task = calloc(1, sizeof *task);
    if (task == NULL) {
        cf_http_request_free_input(loop, req, input_reserved);
        loop->counters.errors++;
        (void)cf_http_conn_queue_status(conn, 500, true, NULL);
        return CF_NOMEM;
    }
    task->loop = loop;
    task->conn = conn->id;
    task->sequence = sequence;
    task->request = req;
    task->input_reserved = input_reserved;
    cf_response_init(&task->response);
    atomic_init(&task->submit_state, 0);

    conn->pending_task = task;
    conn->deadline_kind = CF_HTTP_DL_NONE;
    conn->deadline_ms = 0;

    cf_err rc = loop->cfg.admit(loop->cfg.admit_user, task);
    if (rc != CF_OK) {
        loop->counters.admission_rejected++;
        conn->pending_task = NULL;
        cf_http_request_free_input(loop, req, input_reserved);
        free(task);
        (void)cf_http_conn_queue_status(
            conn, 503, conn->close_after || conn->input_closed,
            "Retry-After: 1\r\n");
        return CF_BUSY;
    }
    loop->counters.admissions++;
    loop->outstanding_tasks++;
    conn->state = CF_HTTP_STATE_WORKING;
    cf_http_conn_update_events(conn);
    return CF_OK;
}

cf_err cf_http_task_submit(cf_http_task *task, cf_response *response) {
    if (task == NULL || response == NULL) return CF_INVALID;
    int expected = 0;
    if (!atomic_compare_exchange_strong(&task->submit_state, &expected, 1)) {
        return CF_BUSY; /* already submitted or abandoned */
    }
    task->response = *response; /* ownership moves to the task */
    task->has_response = true;
    cf_response_init(response); /* caller's struct is left empty */

    struct cf_http_loop *loop = task->loop;
    pthread_mutex_lock(&loop->completion_mutex);
    task->next = NULL;
    if (loop->task_tail != NULL) {
        loop->task_tail->next = task;
    } else {
        loop->task_head = task;
    }
    loop->task_tail = task;
    pthread_mutex_unlock(&loop->completion_mutex);
    loop_wake(loop);
    return CF_OK;
}

void cf_http_task_abandon(cf_http_task *task) {
    if (task == NULL) return;
    int expected = 0;
    if (!atomic_compare_exchange_strong(&task->submit_state, &expected, 1)) {
        return; /* already submitted or abandoned */
    }
    task->abandoned = true;
    struct cf_http_loop *loop = task->loop;
    pthread_mutex_lock(&loop->completion_mutex);
    task->next = NULL;
    if (loop->task_tail != NULL) {
        loop->task_tail->next = task;
    } else {
        loop->task_head = task;
    }
    loop->task_tail = task;
    pthread_mutex_unlock(&loop->completion_mutex);
    loop_wake(loop);
}

const cf_request *cf_http_task_request(const cf_http_task *task) {
    return task != NULL ? task->request : NULL;
}

cf_conn_id cf_http_task_connection(const cf_http_task *task) {
    cf_conn_id none = {0, 0, 0};
    return task != NULL ? task->conn : none;
}

uint64_t cf_http_task_sequence(const cf_http_task *task) {
    return task != NULL ? task->sequence : 0;
}

/* ---------------------------------------------------------- completions */

static void handle_task_completion(struct cf_http_loop *loop,
                                   struct cf_http_task *task) {
    struct cf_http_conn *conn = cf_http_loop_find_conn(loop, task->conn);
    bool usable = conn != NULL && conn->fd >= 0 &&
                  conn->state == CF_HTTP_STATE_WORKING &&
                  conn->sequence == task->sequence &&
                  conn->pending_task == task;
    if (!usable) {
        loop->counters.stale_completions++;
        cf_http_loop_release_task(loop, task); /* CORE-03: resources released */
        return;
    }
    conn->pending_task = task;
    if (task->abandoned) {
        conn->pending_task = NULL;
        cf_http_loop_release_task(loop, task);
        cf_http_conn_close(loop, conn);
        return;
    }
    if (!task->has_response) {
        /* Defensive: submitted without a copied response cannot happen. */
        conn->pending_task = NULL;
        cf_http_loop_release_task(loop, task);
        cf_http_conn_close(loop, conn);
        return;
    }
    cf_err rc = cf_http_conn_attach_response(conn, task);
    if (rc != CF_OK) {
        conn->pending_task = NULL;
        conn->task_attached_to_output = false;
        cf_http_loop_release_task(loop, task);
        if (conn->out_head == NULL) {
            /* Budget pressure before any response bytes: 503 is formable. */
            loop->counters.budget_rejected++;
            (void)cf_http_conn_queue_status(conn, 503, true,
                                            "Retry-After: 1\r\n");
        } else {
            loop->counters.errors++;
            cf_http_conn_close(loop, conn);
        }
    }
}

static void loop_drain_completions(struct cf_http_loop *loop) {
    /* Consume the eventfd first, then detach the queue: an enqueue that wins
     * the race after the read leaves its wake pending, so no completion is
     * lost (01 H00 contract: enqueue/drain race). */
    loop_drain_wake(loop);

    pthread_mutex_lock(&loop->completion_mutex);
    struct cf_http_task *tasks = loop->task_head;
    struct cf_http_chunk_req *chunks = loop->chunk_head;
    loop->task_head = loop->task_tail = NULL;
    loop->chunk_head = loop->chunk_tail = NULL;
    pthread_mutex_unlock(&loop->completion_mutex);

    while (tasks != NULL) {
        struct cf_http_task *next = tasks->next;
        handle_task_completion(loop, tasks);
        tasks = next;
    }
    while (chunks != NULL) {
        struct cf_http_chunk_req *next = chunks->next;
        cf_http_file_chunk_complete(loop, chunks);
        chunks = next;
    }
}

/* --------------------------------------------------------- file worker */

static void *file_worker_main(void *arg) {
    struct cf_http_loop *loop = arg;
    struct cf_http_file_worker *w = &loop->file_worker;
    for (;;) {
        pthread_mutex_lock(&w->mutex);
        while (w->head == NULL && !w->stop) {
            pthread_cond_wait(&w->cond, &w->mutex);
        }
        if (w->head == NULL && w->stop) {
            pthread_mutex_unlock(&w->mutex);
            break;
        }
        struct cf_http_chunk_req *req = w->head;
        w->head = req->next;
        if (w->head == NULL) w->tail = NULL;
        req->next = NULL;
        bool stopping = w->stop;
        pthread_mutex_unlock(&w->mutex);

        if (stopping) {
            /* Teardown: no loop reader remains; drop the read reference. */
            req->stream->chunk_pending = false;
            cf_http_file_stream_release(req->stream);
            free(req);
            continue;
        }

        size_t want = req->want;
        unsigned char *data = malloc(want == 0 ? 1 : want);
        if (data == NULL) {
            req->err = ENOMEM;
            req->len = 0;
        } else {
            ssize_t n;
            do {
                n = pread(req->stream->fd, data, want, (off_t)req->offset);
            } while (n < 0 && errno == EINTR);
            if (n < 0) {
                req->err = errno;
                req->len = 0;
            } else {
                req->err = 0;
                req->len = (size_t)n; /* 0 means premature EOF */
            }
            req->data = data;
        }

        pthread_mutex_lock(&loop->completion_mutex);
        req->next = NULL;
        if (loop->chunk_tail != NULL) {
            loop->chunk_tail->next = req;
        } else {
            loop->chunk_head = req;
        }
        loop->chunk_tail = req;
        pthread_mutex_unlock(&loop->completion_mutex);
        loop_wake(loop);
    }
    return NULL;
}

void cf_http_file_worker_submit(struct cf_http_loop *loop,
                                struct cf_http_chunk_req *req) {
    struct cf_http_file_worker *w = &loop->file_worker;
    pthread_mutex_lock(&w->mutex);
    req->next = NULL;
    if (w->tail != NULL) {
        w->tail->next = req;
    } else {
        w->head = req;
    }
    w->tail = req;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->mutex);
}

/* --------------------------------------------------------------- accept */

static void conn_init(struct cf_http_loop *loop, struct cf_http_conn *conn,
                      int fd) {
    uint64_t gen = conn->id.generation + 1;
    if (gen == 0) {
        /* Exhausted: retire the slot for the process lifetime. */
        close(fd);
        return;
    }
    conn->loop = loop;
    conn->id.loop = loop->cfg.loop_index;
    conn->id.slot = (uint32_t)(conn - loop->conns);
    conn->id.generation = gen;
    conn->fd = fd;
    conn->state = CF_HTTP_STATE_ACCEPTED;
    conn->events = 0;
    conn->registered = false;
    conn->input_closed = false;
    conn->close_after = false;
    conn->must_close_input = false;
    conn->sequence = 0;
    conn->deadline_kind = CF_HTTP_DL_NONE;
    conn->deadline_ms = 0;
    conn->pending_task = NULL;
    conn->task_attached_to_output = false;
    conn->pending_kind = CF_HTTP_PENDING_NONE;
    conn->out_head = conn->out_tail = NULL;
    conn->out_pending = 0;
    conn->stall_ms = 0;
    conn->file_stream = NULL;
    conn->input_reserved = 0;
    conn->output_reserved = 0;
    conn->hdr_count = 0;
    conn->chunk_done = false;
    conn->trailers_done = false;
    conn->request_offered = false;
    /* in/head/body/trailers were disposed on close; clear lengths anyway. */
    conn->in.len = conn->head.len = conn->body.len = conn->trailers.len = 0;

    conn->peer_ip[0] = '\0';
    struct sockaddr_storage ss;
    socklen_t slen = sizeof ss;
    if (getpeername(fd, (struct sockaddr *)&ss, &slen) == 0) {
        if (ss.ss_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
            (void)inet_ntop(AF_INET, &sin->sin_addr, conn->peer_ip,
                            sizeof conn->peer_ip);
        } else if (ss.ss_family == AF_INET6) {
            struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&ss;
            (void)inet_ntop(AF_INET6, &sin6->sin6_addr, conn->peer_ip,
                            sizeof conn->peer_ip);
        }
    }
    if (conn->peer_ip[0] == '\0') {
        memcpy(conn->peer_ip, "local", 6);
    }

    loop->active_conns++;
    cf_http_request_start(conn, false); /* ACCEPTED -> HEADERS */
    cf_http_conn_update_events(conn);
}

static void loop_accept(struct cf_http_loop *loop) {
    for (;;) {
        int fd = accept4(loop->cfg.listen_fd, NULL, NULL,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break; /* EAGAIN or a transient accept failure */
        }
        if (loop->active_conns >= loop->conn_cap ||
            loop->free_count == 0) {
            loop->counters.rejected++;
            close(fd);
            continue;
        }
        size_t slot = (size_t)loop->free_slots[--loop->free_count];
        struct cf_http_conn *conn = &loop->conns[slot];
        loop->counters.accepted++;
        conn_init(loop, conn, fd);
    }
}

/* ------------------------------------------------------------ requests */

static void conn_on_readable(struct cf_http_loop *loop,
                             struct cf_http_conn *conn) {
    unsigned char *buf = loop->scratch;
    for (;;) {
        ssize_t n = recv(conn->fd, buf, CF_HTTP_READ_MAX, 0);
        if (n > 0) {
            cf_http_request_input(conn, buf, (size_t)n);
            if (conn->fd < 0) return;
            /* One recv per event keeps a single read bounded; level-triggered
             * epoll reports the rest. */
            return;
        }
        if (n == 0) {
            cf_http_request_eof(conn);
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        cf_http_conn_close(loop, conn);
        return;
    }
}

/* Peer half-close while a response is in flight: peek without consuming so
 * the response still goes out, but keep-alive is disallowed (01 H01
 * half-close handling). */
static void conn_note_half_close(struct cf_http_loop *loop,
                                 struct cf_http_conn *conn) {
    if (conn->input_closed || conn->fd < 0) return;
    unsigned char byte;
    ssize_t n = recv(conn->fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0) {
        conn->input_closed = true;
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
               errno != EINTR) {
        cf_http_conn_close(loop, conn);
    }
    (void)loop;
}

/* ----------------------------------------------------------- shutdown */

static void loop_begin_stop(struct cf_http_loop *loop) {
    loop->stopping = true;
    (void)epoll_ctl(loop->epfd, EPOLL_CTL_DEL, loop->cfg.listen_fd, NULL);
    /* Reject new admissions; connections still reading a request cannot be
     * answered reliably, so they close. Working/writing connections drain. */
    for (size_t i = 0; i < loop->conn_cap; i++) {
        struct cf_http_conn *conn = &loop->conns[i];
        if (conn->state == CF_HTTP_STATE_ACCEPTED ||
            conn->state == CF_HTTP_STATE_HEADERS ||
            conn->state == CF_HTTP_STATE_BODY) {
            cf_http_conn_close(loop, conn);
        }
    }
}

static bool loop_drain_done(const struct cf_http_loop *loop) {
    if (loop->outstanding_tasks != 0) return false;
    for (size_t i = 0; i < loop->conn_cap; i++) {
        const struct cf_http_conn *conn = &loop->conns[i];
        if (conn->state == CF_HTTP_STATE_WORKING ||
            conn->out_pending > 0 || conn->file_stream != NULL) {
            return false;
        }
    }
    return true;
}

static int loop_next_timeout(const struct cf_http_loop *loop, uint64_t now) {
    uint64_t best = UINT64_MAX;
    if (loop->stopping && loop->drain_deadline_ms != 0) {
        best = loop->drain_deadline_ms; /* drain deadline */
    }
    for (size_t i = 0; i < loop->conn_cap; i++) {
        const struct cf_http_conn *conn = &loop->conns[i];
        if (conn->fd < 0) continue;
        if (conn->deadline_kind != CF_HTTP_DL_NONE && conn->deadline_ms != 0 &&
            conn->deadline_ms < best) {
            best = conn->deadline_ms;
        }
        if (conn->out_pending > 0 && conn->stall_ms != 0) {
            uint64_t stall = conn->stall_ms + CF_HTTP_DEADLINE_MS;
            if (stall < best) best = stall;
        }
    }
    if (best == UINT64_MAX) {
        /* With an app attached, wake periodically to observe its stop
         * request (there is no eventfd from cf_app). */
        return loop->cfg.app != NULL ? 250 : -1;
    }
    if (best <= now) return 0;
    uint64_t diff = best - now;
    if (diff > 250) diff = 250; /* re-check injected/advanced clocks */
    return (int)diff;
}

/* --------------------------------------------------------------- create */

cf_err cf_http_loop_create(const cf_http_loop_config *config,
                           cf_http_loop **out) {
    if (out == NULL) return CF_INVALID;
    *out = NULL;
    if (config == NULL || config->listen_fd < 0 || config->admit == NULL ||
        config->public_origin == NULL) {
        return CF_INVALID;
    }

    struct cf_http_loop *loop = calloc(1, sizeof *loop);
    if (loop == NULL) return CF_NOMEM;
    loop->epfd = -1;
    loop->wake_fd = -1;
    loop->cfg = *config;
    loop->cfg.connections_per_loop = config->connections_per_loop != 0
                                         ? config->connections_per_loop
                                         : CF_HTTP_DEFAULT_CONNECTIONS;
    loop->public_origin = strdup(config->public_origin);
    if (loop->public_origin == NULL) {
        free(loop);
        return CF_NOMEM;
    }
    if (!parse_origin(loop->public_origin, loop->origin_host,
                      sizeof loop->origin_host, &loop->origin_host_len,
                      &loop->origin_port, &loop->origin_explicit_port,
                      &loop->origin_is_https)) {
        free(loop->public_origin);
        free(loop);
        return CF_INVALID;
    }

    loop->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (loop->epfd < 0) goto fail;
    loop->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (loop->wake_fd < 0) goto fail;

    if (loop->cfg.connections_per_loop > UINT32_MAX) goto fail;
    loop->conn_cap = loop->cfg.connections_per_loop;
    loop->conns = calloc(loop->conn_cap, sizeof *loop->conns);
    loop->free_slots = malloc(loop->conn_cap * sizeof *loop->free_slots);
    loop->scratch = malloc(CF_HTTP_READ_MAX);
    if (loop->conns == NULL || loop->free_slots == NULL ||
        loop->scratch == NULL) {
        goto fail;
    }
    for (size_t i = 0; i < loop->conn_cap; i++) {
        loop->free_slots[i] = (int32_t)i;
        loop->conns[i].fd = -1;
        loop->conns[i].state = CF_HTTP_STATE_CLOSED;
        loop->conns[i].loop = loop;
        loop->conns[i].id.slot = (uint32_t)i;
    }
    loop->free_count = loop->conn_cap;

    if (pthread_mutex_init(&loop->completion_mutex, NULL) != 0) goto fail;
    if (pthread_mutex_init(&loop->file_worker.mutex, NULL) != 0) goto fail;
    if (pthread_cond_init(&loop->file_worker.cond, NULL) != 0) goto fail;

    int flags = fcntl(loop->cfg.listen_fd, F_GETFL, 0);
    if (flags < 0 ||
        fcntl(loop->cfg.listen_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        goto fail;
    }
    struct epoll_event le = {.events = EPOLLIN};
    le.data.u64 = 2; /* listener tag; connection pointers are never 1 or 2 */
    if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, loop->cfg.listen_fd, &le) != 0) {
        goto fail;
    }
    struct epoll_event we = {.events = EPOLLIN};
    we.data.u64 = 1; /* wake tag */
    if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, loop->wake_fd, &we) != 0) {
        goto fail;
    }

    if (pthread_create(&loop->file_worker.thread, NULL, file_worker_main,
                       loop) != 0) {
        goto fail;
    }
    loop->file_worker.started = true;
    atomic_init(&loop->stop_flag, false);

    *out = loop;
    return CF_OK;

fail: {
    cf_err rc = CF_NOMEM;
    if (loop->scratch != NULL) rc = CF_INTERNAL;
    if (loop->file_worker.started) {
        pthread_mutex_lock(&loop->file_worker.mutex);
        loop->file_worker.stop = true;
        pthread_cond_broadcast(&loop->file_worker.cond);
        pthread_mutex_unlock(&loop->file_worker.mutex);
        pthread_join(loop->file_worker.thread, NULL);
    }
    if (loop->epfd >= 0) close(loop->epfd);
    if (loop->wake_fd >= 0) close(loop->wake_fd);
    free(loop->scratch);
    free(loop->conns);
    free(loop->free_slots);
    free(loop->public_origin);
    free(loop);
    return rc;
}
}

void cf_http_loop_stop(struct cf_http_loop *loop) {
    if (loop == NULL) return;
    atomic_store_explicit(&loop->stop_flag, true, memory_order_relaxed);
    loop_wake(loop);
}

cf_err cf_http_loop_run(struct cf_http_loop *loop) {
    if (loop == NULL || loop->run_called) return CF_INVALID;
    loop->run_called = true;

    struct epoll_event events[CF_HTTP_MAX_EVENTS];
    while (true) {
        if (loop->cfg.app != NULL &&
            cf_app_stop_requested(loop->cfg.app)) {
            atomic_store_explicit(&loop->stop_flag, true,
                                  memory_order_relaxed);
        }
        if (atomic_load_explicit(&loop->stop_flag, memory_order_relaxed) &&
            !loop->stopping) {
            loop_begin_stop(loop);
            loop->drain_deadline_ms = cf_monotonic_ms() + CF_HTTP_DRAIN_MS;
        }
        if (loop->stopping && loop_drain_done(loop)) {
            break;
        }
        if (loop->stopping) {
            uint64_t now = cf_monotonic_ms();
            if (now >= loop->drain_deadline_ms) break;
            /* Re-check any connection whose deadline expired while waiting
             * for completions. */
        }

        uint64_t now = cf_monotonic_ms();
        for (size_t i = 0; i < loop->conn_cap; i++) {
            cf_http_conn_check_deadline(&loop->conns[i], now);
        }

        int timeout = loop_next_timeout(loop, now);
        int n = epoll_wait(loop->epfd, events, CF_HTTP_MAX_EVENTS, timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            struct epoll_event *e = &events[i];
            if (e->data.u64 == 1) { /* wake fd */
                loop_drain_completions(loop);
                continue;
            }
            if (e->data.u64 == 2) { /* listener */
                if (!loop->stopping) loop_accept(loop);
                continue;
            }
            if (e->data.ptr != NULL) {
                /* Connections register their own pointer. */
                struct cf_http_conn *conn = e->data.ptr;
                if (conn->fd < 0) continue;
                if (e->events & (EPOLLERR | EPOLLHUP)) {
                    cf_http_conn_close(loop, conn);
                    continue;
                }
                if (e->events & EPOLLOUT) {
                    cf_http_conn_flush(conn);
                    if (conn->fd < 0) continue;
                }
                if ((e->events & (EPOLLIN | EPOLLRDHUP)) &&
                    (conn->state == CF_HTTP_STATE_ACCEPTED ||
                     conn->state == CF_HTTP_STATE_HEADERS ||
                     conn->state == CF_HTTP_STATE_BODY)) {
                    conn_on_readable(loop, conn);
                } else if (e->events & EPOLLRDHUP) {
                    conn_note_half_close(loop, conn);
                }
                continue;
            }
        }
    }

    /* Final teardown of live connections: after the drain window every
     * admitted task still outstanding is released through its completion. */
    for (size_t i = 0; i < loop->conn_cap; i++) {
        struct cf_http_conn *conn = &loop->conns[i];
        if (conn->state != CF_HTTP_STATE_CLOSED) cf_http_conn_close(loop, conn);
    }
    loop->run_returned = true;
    return CF_OK;
}

void cf_http_loop_destroy(cf_http_loop *loop) {
    if (loop == NULL) return;
    cf_http_loop_stop(loop);
    if (loop->file_worker.started) {
        pthread_mutex_lock(&loop->file_worker.mutex);
        loop->file_worker.stop = true;
        pthread_cond_broadcast(&loop->file_worker.cond);
        pthread_mutex_unlock(&loop->file_worker.mutex);
        pthread_join(loop->file_worker.thread, NULL);
        loop->file_worker.started = false;
    }

    /* Release connections (closes stream fds), then anything queued. */
    for (size_t i = 0; i < loop->conn_cap; i++) {
        cf_http_conn_close(loop, &loop->conns[i]);
    }
    loop_drain_completions(loop);

    pthread_mutex_destroy(&loop->file_worker.mutex);
    pthread_cond_destroy(&loop->file_worker.cond);
    pthread_mutex_destroy(&loop->completion_mutex);
    if (loop->epfd >= 0) close(loop->epfd);
    if (loop->wake_fd >= 0) close(loop->wake_fd);
    free(loop->scratch);
    free(loop->conns);
    free(loop->free_slots);
    free(loop->public_origin);
    free(loop);
}

void cf_http_loop_counters(const cf_http_loop *loop, cf_http_counters *out) {
    if (out == NULL) return;
    if (loop == NULL) {
        memset(out, 0, sizeof *out);
        return;
    }
    *out = loop->counters;
}
