/* HTTP/1.x byte transport: connection state machine, bounded input/output,
 * request admission handoff and completion handles (task H01,
 * 01-foundation-http.md "H01: HTTP state machine").
 *
 * Scope. This module owns the loop thread side of HTTP: accept, request
 * parsing/validation and freeze, request completion queue, response
 * serialization, partial writes, file streaming and deadline handling. It
 * does not route, render, authenticate or schedule workers; A00 supplies the
 * admission hook below and runs one loop thread per cf_config.loops value.
 *
 * Threading. A loop and every connection it owns are confined to the thread
 * that calls cf_http_loop_run(). The only functions safe to call from another
 * thread are cf_http_loop_stop(), cf_http_task_submit(),
 * cf_http_task_abandon() and the task accessors (which return borrowed data
 * the caller must not outlive).
 *
 * Admission. When a request is complete and validated, the loop freezes it
 * into owned storage and calls the admit callback exactly once. On CF_OK the
 * callee owns the task: it must eventually call cf_http_task_submit() (with
 * a response) or cf_http_task_abandon(), exactly once. On any other return
 * the loop keeps ownership and answers 503 (Retry-After: 1); no admission
 * slot or completion can be lost (00-contracts.md CORE-04).
 *
 * Shutdown. cf_http_loop_stop() starts the orderly sequence: stop accepting,
 * reject new admissions, drain admitted tasks and pending output for up to
 * five seconds, then close every connection and return from
 * cf_http_loop_run(). A00 must join its request workers before
 * cf_http_loop_destroy(); every task handed out must have been submitted or
 * abandoned by then. Queued completions that arrive but are never drained are
 * released by cf_http_loop_destroy(). */
#ifndef CF_HTTP_H
#define CF_HTTP_H

#include "cf.h"

typedef struct cf_http_loop cf_http_loop;
typedef struct cf_http_task cf_http_task;

/* Admission hook. Called on the loop thread with a frozen, validated task.
 * Returns CF_OK to take ownership (then exactly one of submit/abandon), or
 * CF_BUSY (or any failure) to decline: the loop answers 503 without
 * consuming storage and destroys the request itself. The callee must not
 * block on another loop thread or wait for a worker that needs this same
 * loop. */
typedef cf_err (*cf_http_admit_fn)(void *user, cf_http_task *task);

typedef struct {
    /* Bound listening socket owned by the caller. create() puts it in
     * nonblocking mode; destroy() leaves it open (the caller closes it). */
    int listen_fd;
    uint32_t loop_index; /* fills cf_conn_id.loop */
    /* Absolute http/https origin (as in cf_config.public_origin). Borrowed
     * for the loop's lifetime. Requests whose Host is inconsistent with it
     * are rejected 400; application URLs use it through cf_config. */
    const char *public_origin;
    /* 0 selects the 01 limit (2048). */
    size_t connections_per_loop;
    /* Per-loop share of the process-wide input/output reservations (0
     * selects the 64 MiB defaults). The loop enforces them as logical byte
     * totals; A00 divides CF_INPUT_BYTES/CF_OUTPUT_BYTES across loops. */
    size_t input_bytes;
    size_t output_bytes;
    /* Optional: when non-NULL, run() also leaves when cf_app_stop_requested
     * becomes true. The app itself is never dereferenced outside that call. */
    cf_app *app;
    cf_http_admit_fn admit; /* required */
    void *admit_user;
} cf_http_loop_config;

/* Create a loop. out stays NULL on failure; the listener stays owned by the
 * caller. */
cf_err cf_http_loop_create(const cf_http_loop_config *config,
                           cf_http_loop **out);

/* Run on the calling thread until cf_http_loop_stop() (or an app stop
 * request) triggers the five-second drain, then return CF_OK. Never called
 * twice without destroy. */
cf_err cf_http_loop_run(cf_http_loop *loop);

/* Async stop request; safe from any thread and idempotent. */
void cf_http_loop_stop(cf_http_loop *loop);

/* Release every loop resource. Only valid after run() returned (or was never
 * called) and after all admitted tasks have been submitted or abandoned.
 * Accepts NULL. */
void cf_http_loop_destroy(cf_http_loop *loop);

/* ---- task accessors (borrowed; valid until submit/abandon) ------------- */

const cf_request *cf_http_task_request(const cf_http_task *task);
cf_conn_id cf_http_task_connection(const cf_http_task *task);
uint64_t cf_http_task_sequence(const cf_http_task *task);

/* Submit the finished response. Callable from a worker thread. On CF_OK the
 * task and response ownership move to the loop (the response struct is
 * copied and reset) and the completion cannot be lost. On failure ownership
 * stays with the caller.
 *
 * Exactly one of submit/abandon must be called per admitted task, and the
 * pointer must not be used again afterwards: the loop releases the task as
 * soon as the response finishes (or is discarded), so a second call would be
 * a use-after-free. */
cf_err cf_http_task_submit(cf_http_task *task, cf_response *response);

/* Give the task back without a response (queued work discarded at
 * shutdown). The loop drops the request and closes the connection; the
 * completion is still accounted, so shutdown cannot hang. Callable from a
 * worker thread, exactly once per task (see submit). */
void cf_http_task_abandon(cf_http_task *task);

/* Loop counters ("must be counted", D-C04). Safe after run returned. */
typedef struct {
    uint64_t accepted;           /* sockets accepted */
    uint64_t rejected;           /* sockets refused (connection cap) */
    uint64_t requests;           /* fully parsed and validated requests */
    uint64_t admissions;         /* tasks handed to the admit hook */
    uint64_t admission_rejected; /* hook declined: 503 sent */
    uint64_t responses_sent;     /* responses fully written */
    uint64_t stale_completions;  /* CORE-03: released without touching fd */
    uint64_t budget_rejected;    /* input/output reservation failures */
    uint64_t timeouts;           /* connections closed by a deadline */
    uint64_t errors;             /* loop-generated 4xx/5xx responses */
} cf_http_counters;

/* Copy the loop's counters (see the typedef above). Only valid after the
 * loop has stopped and cf_http_loop_run() has returned: the counters are
 * mutated on the loop thread, so reading them from another thread while it
 * runs is a data race even though the pointer itself is stable. */
void cf_http_loop_counters(const cf_http_loop *loop, cf_http_counters *out);

#endif /* CF_HTTP_H */
