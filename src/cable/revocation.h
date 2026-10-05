/* src/cable/revocation.h — task C03: the non-droppable revocation barrier and
 * the stale-auth rejection gate (04-cable-jobs.md "C03: revocation barrier";
 * 00-contracts.md "Execution and waiting"; 06-cache-performance.md
 * "Version/snapshot algorithm").
 *
 * The writer commits, then calls the registered control handler for every
 * committed DISCONNECT_USER event (src/db/writer.h). The handler is
 * cf_cable_revocation_handler below; the frozen symbol
 * cf_cable_disconnect_user(cf_cable *, int64_t, bool) is its payload.
 *
 * One preallocated control slot exists per loop (cf_cable_revocation).
 * Registration happens when the loop attaches (before the loop can be
 * revoked) and unregisters when it detaches. The barrier fills every affected
 * slot, wakes each loop through the registered wake callback, and waits on a
 * shared acknowledgment count/condition until every affected loop consumed
 * its slot and revoked; it holds no DB, cache or subscriber lock while
 * waiting. A loop consumes its slot by calling cf_cable_revocation_service
 * from its owner thread: the control read does not touch the loop's ordinary
 * queues, so a full broadcast queue or a stalled socket cannot starve the
 * acknowledgment.
 *
 * The owner-side operations come from the wiring that owns the loop
 * (src/cable/pubsub.c and the C01 socket): the loop supplies
 *  - wake:        wake the owner thread (eventfd write / condition signal);
 *  - frame_partial: true when an application frame is partially sent, so the
 *                 disconnect frame must not be interleaved;
 *  - revoke:      owner thread, after the control was consumed and before the
 *                 acknowledgment: send the prebuilt reference disconnect
 *                 frame when control->attempt_disconnect, else close
 *                 immediately, then remove the loop's subscriptions and stop
 *                 accepting new application output. It must NOT call cf_write
 *                 (the writer is waiting for the acknowledgment);
 *  - cleanup:     owner thread, after the acknowledgment: presence cleanup
 *                 through cf_write; a full queue is logged and left to the
 *                 membership TTL (CF_MEMBERSHIP_CONNECTION_TTL_US).
 *
 * Auth results (connection and subscription) carry the data version captured
 * BEFORE their database read: cf_cable_auth_capture at the start of the read,
 * cf_cable_auth_install at the point where the loop would install the access.
 * An install whose version changed is refused (CF_BUSY, one bounded
 * resubmission may be attempted; the caller rejects the subscription when its
 * own queue cannot take it). An install on a revoking/revoked loop is refused
 * (CF_FORBIDDEN): access can never be installed after the acknowledgment.
 * Reconnects always take a fresh ticket, so they re-run the database
 * authentication/membership reads.
 *
 * Fatal internal failure (04 C03): when a loop does not acknowledge within
 * CF_CABLE_REVOCATION_ACK_TIMEOUT_MS, the barrier stops serving, logs a
 * sanitized fatal line and terminates with an error. The committed rows and
 * version stay; the failure is never reported as a rollback, retry or CF_BUSY.
 *
 * Wiring (integrator; exact lines in the C03 evidence):
 *  1. register each loop at attach and unregister it at detach;
 *  2. call cf_cable_revocation_service from the owner loop whenever control
 *     may be pending (each poll iteration; it is a cheap flag check);
 *  3. register cf_cable_revocation_handler through
 *     cf_writer_set_control_handler(app, ..., cable);
 *  4. capture/install tickets in the connection/subscription authorization.
 * Until 1 lands, a cable with attached loops that are not registered fails
 * closed (CF_INTERNAL, never a pretend success) so a connected client can
 * never be left serving after a committed revocation. */
#ifndef CF_CABLE_REVOCATION_H
#define CF_CABLE_REVOCATION_H

#include "cf.h"

/* 04 C03: a loop that has not acknowledged within five seconds makes serving
 * fail; this is not the client-driven 30 s send wait
 * (CF_CABLE_WRITE_STALL_MS). */
#define CF_CABLE_REVOCATION_ACK_TIMEOUT_MS UINT64_C(5000)

/* The processed reference disconnect frame is at most 55 bytes plus NUL
 * (`{"type":"disconnect","reason":"remote","reconnect":false}`). */
#define CF_CABLE_REVOCATION_FRAME_MAX 64

/* Bounded resubmissions after a stale data version: one immediate retry, then
 * the caller rejects the subscription (the "or rejects if full" arm). */
#define CF_CABLE_AUTH_MAX_RESUBMITS 1

typedef struct cf_cable_revocation cf_cable_revocation;

/* Writer-filled control for one loop; plain values only (no borrowed spans).
 * `frame` is the exact reference frame
 * `{"type":"disconnect","reason":"remote","reconnect":<flag>}`. */
typedef struct {
    uint64_t generation; /* barrier instance */
    int64_t user_id;
    bool reconnect;
    /* False when an application frame is partially sent: the loop must close
     * immediately instead of interleaving the disconnect frame. */
    bool attempt_disconnect;
    char frame[CF_CABLE_REVOCATION_FRAME_MAX];
    size_t frame_len;
} cf_cable_revocation_control;

/* Owner-side operations for one registered loop. See the header comment for
 * the threading and cf_write rules of each callback. */
typedef struct {
    /* Called from the barrier thread with the C03 state mutex held; it must
     * not call back into C03 and must not block (an eventfd write or a
     * condition signal). May be NULL for a loop with no owner thread. */
    void (*wake)(void *user);
    /* Owner thread. NULL means "no partial frame". */
    bool (*frame_partial)(void *user);
    /* Owner thread; must not call cf_write. NULL is a no-op (a test loop with
     * nothing to revoke). */
    void (*revoke)(void *user, const cf_cable_revocation_control *control);
    /* Owner thread, after the acknowledgment; may call cf_write. NULL is a
     * no-op. */
    void (*cleanup)(void *user);
    void *user;
} cf_cable_revocation_ops;

/* Register one preallocated control slot for a loop. `scope` is the cf_cable
 * the slot belongs to; `app` supplies the data version and the serving-stop
 * request on the fatal path. Call once per loop before it can be revoked
 * (at attach) and unregister before the loop's memory is released (at
 * detach). Safe from any thread. On failure *out stays NULL. */
cf_err cf_cable_revocation_register(cf_cable *scope, cf_app *app,
                                    int64_t user_id,
                                    const cf_cable_revocation_ops *ops,
                                    cf_cable_revocation **out);

/* Remove a slot. A pending control for this slot is acknowledged (the loop is
 * going away, so it cannot serve it); the barrier must not time out because
 * of a loop that detached. Any thread, but never concurrently with a
 * cf_cable_revocation_service call on the same slot (the owner thread
 * unregisters after its last service call). NULL is a no-op. */
void cf_cable_revocation_unregister(cf_cable_revocation *slot);

/* Owner thread: consume the pending control, revoke, acknowledge, then run
 * the deferred cleanup. Returns CF_OK immediately when nothing is pending
 * (the loop-iteration fast path), CF_INVALID for NULL. */
cf_err cf_cable_revocation_service(cf_cable_revocation *slot);

/* True once the slot's acknowledgment was recorded: the loop must not accept
 * further application output or install access. */
bool cf_cable_revocation_revoked(const cf_cable_revocation *slot);

/* The writer's mandatory DISCONNECT_USER control handler (ctx is the
 * cf_cable*): cf_writer_set_control_handler(app, cf_cable_revocation_handler,
 * cable). Any non-CF_OK makes the committed write report CF_INTERNAL. */
cf_err cf_cable_revocation_handler(void *ctx, const cf_event *event);

/* ---- data-version gate for authorization results --------------------------- */

/* The data version captured before a database read (06: capture with acquire
 * ordering before BEGIN/the read snapshot). */
typedef struct {
    uint64_t version;
    bool valid;
} cf_cable_auth_ticket;

/* Capture before the read. NULL app yields an invalid ticket. */
cf_cable_auth_ticket cf_cable_auth_capture(const cf_app *app);

/* True when the ticket is valid and its version is still current. */
bool cf_cable_auth_version_current(const cf_app *app, cf_cable_auth_ticket t);

/* The install gate, on the loop's owner thread, serialized with
 * cf_cable_revocation_service (04 C03 "Loop-local install/control handling is
 * serialized"):
 *   CF_OK        version still current and the loop is not revoking/revoked:
 *                the caller installs the result;
 *   CF_BUSY      the version advanced; *resubmit is true while the bounded
 *                resubmission budget remains (re-run the authorization), and
 *                false when the budget is spent, so the caller rejects the
 *                subscription (as it also does when its own queue is full);
 *   CF_FORBIDDEN the loop is revoking or revoked: reject; an authorization
 *                result may not install after the acknowledgment;
 *   CF_INVALID   NULL slot/ticket or a slot without an app.
 * *resubmit is always written. A successful install resets the budget. */
cf_err cf_cable_auth_install(cf_cable_revocation *slot,
                             cf_cable_auth_ticket ticket, bool *resubmit);

/* ---- test-only controls (production never calls these) --------------------- */

/* Override the 5 s acknowledgment deadline for tests (0 restores the
 * constant). */
void cf_cable_revocation_test_ack_timeout_ms(uint64_t ms);

/* Replace the terminal action of the fatal acknowledgment-timeout path. The
 * default logs, requests app stop and aborts. Tests install a recording
 * handler to observe the path without terminating. Pass NULL to restore. */
typedef void (*cf_cable_revocation_fatal_fn)(void *user, int64_t user_id);
void cf_cable_revocation_test_fatal_fn(cf_cable_revocation_fatal_fn fn,
                                       void *user);

/* Clear the fatal/ack-timeout test state so one case cannot poison the next.
 * Only meaningful when no barrier is running. */
void cf_cable_revocation_test_reset(void);

#endif /* CF_CABLE_REVOCATION_H */
