/* src/cable/revocation.c — task C03: the revocation barrier
 * (04-cable-jobs.md "C03: revocation barrier"; src/db/writer.h post-commit
 * control handler; 00-contracts.md mutation order; 06-cache-performance.md
 * version/snapshot algorithm).
 *
 * One preallocated control slot per loop, one barrier at a time. The writer
 * fills the slots of the affected loops, wakes their owner threads and waits
 * on a shared acknowledgment count/condition; it holds no DB, cache or
 * subscriber lock while waiting. Each owner thread consumes its slot (a
 * separate flag, never the ordinary broadcast/transport queues), sends or
 * skips the reference disconnect frame, removes its subscriptions, stops new
 * application output and acknowledges; presence cleanup runs afterwards on
 * the owner thread, where a cf_write can no longer block the waiting writer
 * (a full queue is left to the membership TTL).
 *
 * The module is deliberately small and value-based: control is copied into
 * the slot, the disconnect frame is formatted into the slot's fixed buffer,
 * and the callbacks are supplied by the loop's wiring (see revocation.h).
 * There is no allocation on the barrier path and no best-effort delivery:
 * a missing acknowledgment is a fatal internal error, never a rollback. */

#include "cable/revocation.h"

#include "app.h"
#include "app_internal.h"
#include "cable/channels.h" /* cf_cable_stats_get */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct cf_cable_revocation {
    struct cf_cable_revocation *next;
    cf_cable *scope; /* borrowed: the cable whose barriers include this slot */
    cf_app *app;     /* borrowed: data version and fatal stop request */
    int64_t user_id;
    cf_cable_revocation_ops ops;

    /* Writer-filled control (guarded by g_mutex). There is one barrier at a
     * time, so a single pending control is enough; `generation` identifies
     * the barrier instance. */
    bool pending;
    uint64_t generation;
    int64_t control_user;
    bool control_reconnect;

    /* Barrier accounting and loop state (guarded by g_mutex). */
    bool outstanding; /* counted in g_outstanding until serviced/unregistered */
    bool servicing;   /* owner thread is inside service */
    bool revoked;     /* acknowledgment recorded: no more application output */
    unsigned auth_resubmits; /* bounded stale-auth resubmissions (C03 gate) */
};

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ack_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_barrier_mutex = PTHREAD_MUTEX_INITIALIZER;
static cf_cable_revocation *g_slots;
static uint64_t g_generation;
static size_t g_outstanding;
static bool g_fatal;
static uint64_t g_ack_timeout_ms; /* 0 = CF_CABLE_REVOCATION_ACK_TIMEOUT_MS */
static cf_cable_revocation_fatal_fn g_fatal_fn;
static void *g_fatal_user;

/* ---- registration ----------------------------------------------------------- */

cf_err cf_cable_revocation_register(cf_cable *scope, cf_app *app,
                                    int64_t user_id,
                                    const cf_cable_revocation_ops *ops,
                                    cf_cable_revocation **out) {
    if (out == NULL) return CF_INVALID;
    *out = NULL;
    if (scope == NULL || ops == NULL || user_id <= 0) return CF_INVALID;
    cf_cable_revocation *slot = calloc(1, sizeof *slot);
    if (slot == NULL) return CF_NOMEM;
    slot->scope = scope;
    slot->app = app;
    slot->user_id = user_id;
    slot->ops = *ops;
    pthread_mutex_lock(&g_mutex);
    slot->next = g_slots;
    g_slots = slot;
    pthread_mutex_unlock(&g_mutex);
    *out = slot;
    return CF_OK;
}

void cf_cable_revocation_unregister(cf_cable_revocation *slot) {
    if (slot == NULL) return;
    pthread_mutex_lock(&g_mutex);
    /* A loop that detaches while the barrier waits must not hang it: the loop
     * cannot serve the control anymore, so it is acknowledged here. */
    if (slot->outstanding) {
        slot->outstanding = false;
        if (g_outstanding > 0) g_outstanding--;
        pthread_cond_broadcast(&g_ack_cv);
    }
    cf_cable_revocation **link = &g_slots;
    while (*link != NULL && *link != slot) link = &(*link)->next;
    if (*link == slot) *link = slot->next;
    pthread_mutex_unlock(&g_mutex);
    free(slot);
}

/* ---- the barrier -------------------------------------------------------------- */

/* The processed reference remote-disconnect frame ("remote", reconnect flag),
 * exactly protocol::disconnect(Some(Remote), reconnect). It is at most
 * CF_CABLE_REVOCATION_FRAME_MAX-1 bytes; snprintf cannot truncate it. */
static void control_frame(cf_cable_revocation_control *control) {
    int n = snprintf(control->frame, sizeof control->frame,
                     "{\"type\":\"disconnect\",\"reason\":\"remote\","
                     "\"reconnect\":%s}",
                     control->reconnect ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof control->frame) {
        control->frame_len = 0;
        control->frame[0] = '\0';
    } else {
        control->frame_len = (size_t)n;
    }
}

static void barrier_deadline(uint64_t timeout_ms, struct timespec *out) {
    clock_gettime(CLOCK_REALTIME, out);
    out->tv_sec += (time_t)(timeout_ms / 1000);
    out->tv_nsec += (long)((timeout_ms % 1000) * 1000000);
    if (out->tv_nsec >= 1000000000L) {
        out->tv_sec++;
        out->tv_nsec -= 1000000000L;
    }
}

/* One barrier; g_barrier_mutex is held. Never holds a DB/cache/subscriber
 * lock while waiting. Returns CF_OK when every affected loop acknowledged,
 * CF_INTERNAL when not (the transaction is already committed; the caller must
 * never turn this into a retry or a rollback). */
static cf_err barrier_run(cf_cable *cable, int64_t user_id, bool reconnect) {
    /* Wiring check: every attached loop of this cable must have a registered
     * slot. The two counts live under different locks and the wiring links a
     * loop only after registering its slot and unlinks it before unregistering
     * (pubsub.c), so a single snapshot pair can skew while a connection
     * attaches or detaches; two interleaved samples tolerate that window (a
     * transient skew must not turn a committed logout into CF_INTERNAL) while
     * a persistent unregistered loop still fails closed. */
    size_t registered_max = 0;
    uint64_t loops_min = UINT64_MAX;
    for (int sample = 0; sample < 2; sample++) {
        size_t registered = 0;
        pthread_mutex_lock(&g_mutex);
        if (g_fatal) {
            pthread_mutex_unlock(&g_mutex);
            return CF_INTERNAL;
        }
        for (cf_cable_revocation *s = g_slots; s != NULL; s = s->next) {
            if (s->scope == cable) registered++;
        }
        pthread_mutex_unlock(&g_mutex);
        if (registered > registered_max) registered_max = registered;
        cf_cable_stats stats;
        memset(&stats, 0, sizeof stats);
        cf_cable_stats_get(cable, &stats); /* cable mutex only; never DB/cache */
        if (stats.loops < loops_min) loops_min = stats.loops;
    }
    if (registered_max < loops_min) {
        /* Wiring fault: a loop of this cable was never registered with the
         * barrier (see revocation.h). Fail closed rather than report success
         * while a connected client keeps serving. */
        cf_cable_log("revocation barrier", "unregistered cable loops");
        return CF_INTERNAL;
    }

    pthread_mutex_lock(&g_mutex);
    if (g_fatal) {
        pthread_mutex_unlock(&g_mutex);
        return CF_INTERNAL;
    }

    uint64_t generation = ++g_generation;
    size_t filled = 0;
    cf_app *stop_app = NULL;
    for (cf_cable_revocation *s = g_slots; s != NULL; s = s->next) {
        if (s->scope != cable || s->user_id != user_id || s->revoked) {
            continue;
        }
        s->pending = true;
        s->outstanding = true;
        s->generation = generation;
        s->control_user = user_id;
        s->control_reconnect = reconnect;
        if (stop_app == NULL) stop_app = s->app;
        filled++;
    }
    if (filled == 0) {
        pthread_mutex_unlock(&g_mutex);
        return CF_OK; /* no connection to revoke */
    }
    g_outstanding += filled;
    /* Wake under the state mutex: the callback must not call back into C03
     * (revocation.h), so this cannot deadlock. */
    for (cf_cable_revocation *s = g_slots; s != NULL; s = s->next) {
        if (s->outstanding && s->generation == generation &&
            s->ops.wake != NULL) {
            s->ops.wake(s->ops.user);
        }
    }

    uint64_t timeout_ms =
        g_ack_timeout_ms != 0 ? g_ack_timeout_ms
                              : CF_CABLE_REVOCATION_ACK_TIMEOUT_MS;
    struct timespec deadline;
    barrier_deadline(timeout_ms, &deadline);
    bool timed_out = false;
    while (g_outstanding != 0) {
        int rc = pthread_cond_timedwait(&g_ack_cv, &g_mutex, &deadline);
        if (rc == ETIMEDOUT) {
            timed_out = true;
            break;
        }
    }
    cf_cable_revocation_fatal_fn fatal_fn = g_fatal_fn;
    void *fatal_user = g_fatal_user;
    if (timed_out) {
        /* Stop serving and terminate with an error (04 C03). The committed
         * rows and version stay; this is never a rollback or a retry. A late
         * acknowledgment must not underflow the shared counter. */
        for (cf_cable_revocation *s = g_slots; s != NULL; s = s->next) {
            if (s->outstanding) s->outstanding = false;
        }
        g_outstanding = 0;
        g_fatal = true;
    }
    pthread_mutex_unlock(&g_mutex);

    if (timed_out) {
        fprintf(stderr,
                "campfire: cable: fatal: revocation acknowledgment timeout\n");
        if (stop_app != NULL) cf_app_request_stop(stop_app);
        if (fatal_fn != NULL) {
            fatal_fn(fatal_user, user_id);
        } else {
            abort(); /* terminate with an error */
        }
        return CF_INTERNAL;
    }
    return CF_OK;
}

cf_err cf_cable_disconnect_user(cf_cable *cable, int64_t user_id,
                                bool reconnect) {
    if (cable == NULL || user_id <= 0) return CF_INVALID;
    pthread_mutex_lock(&g_barrier_mutex); /* one barrier at a time */
    cf_err rc = barrier_run(cable, user_id, reconnect);
    pthread_mutex_unlock(&g_barrier_mutex);
    return rc;
}

cf_err cf_cable_revocation_handler(void *ctx, const cf_event *event) {
    if (ctx == NULL || event == NULL ||
        event->kind != CF_EVENT_DISCONNECT_USER || event->user_id <= 0) {
        return CF_INVALID;
    }
    return cf_cable_disconnect_user(ctx, event->user_id, event->reconnect);
}

/* ---- owner-thread service ------------------------------------------------------- */

cf_err cf_cable_revocation_service(cf_cable_revocation *slot) {
    if (slot == NULL) return CF_INVALID;
    pthread_mutex_lock(&g_mutex);
    if (!slot->pending) {
        pthread_mutex_unlock(&g_mutex);
        return CF_OK; /* the loop-iteration fast path */
    }
    cf_cable_revocation_control control;
    memset(&control, 0, sizeof control);
    control.generation = slot->generation;
    control.user_id = slot->control_user;
    control.reconnect = slot->control_reconnect;
    slot->pending = false;
    slot->servicing = true;
    pthread_mutex_unlock(&g_mutex);

    /* The partial-frame decision belongs to the owner thread. */
    control.attempt_disconnect = slot->ops.frame_partial == NULL ||
                                 !slot->ops.frame_partial(slot->ops.user);
    control_frame(&control);

    if (slot->ops.revoke != NULL) slot->ops.revoke(slot->ops.user, &control);

    pthread_mutex_lock(&g_mutex);
    slot->servicing = false;
    slot->revoked = true;
    if (slot->outstanding) {
        slot->outstanding = false;
        if (g_outstanding > 0) g_outstanding--;
    }
    pthread_cond_broadcast(&g_ack_cv);
    pthread_mutex_unlock(&g_mutex);

    /* Acknowledged: the writer is no longer waiting, so presence cleanup may
     * use cf_write. It cannot make the acknowledgment fail; a full writer
     * queue is logged and left to the membership TTL. */
    if (slot->ops.cleanup != NULL) slot->ops.cleanup(slot->ops.user);
    return CF_OK;
}

bool cf_cable_revocation_revoked(const cf_cable_revocation *slot) {
    if (slot == NULL) return false;
    pthread_mutex_lock(&g_mutex);
    bool revoked = slot->revoked || slot->pending || slot->servicing || g_fatal;
    pthread_mutex_unlock(&g_mutex);
    return revoked;
}

/* ---- data-version gate ----------------------------------------------------------- */

cf_cable_auth_ticket cf_cable_auth_capture(const cf_app *app) {
    cf_cable_auth_ticket ticket = {0, false};
    if (app == NULL) return ticket;
    ticket.version = cf_data_version(app); /* acquire, before the read */
    ticket.valid = true;
    return ticket;
}

bool cf_cable_auth_version_current(const cf_app *app, cf_cable_auth_ticket t) {
    if (app == NULL || !t.valid) return false;
    return cf_data_version(app) == t.version;
}

cf_err cf_cable_auth_install(cf_cable_revocation *slot,
                             cf_cable_auth_ticket ticket, bool *resubmit) {
    if (resubmit != NULL) *resubmit = false;
    if (slot == NULL || !ticket.valid || slot->app == NULL) return CF_INVALID;

    pthread_mutex_lock(&g_mutex);
    bool blocked =
        g_fatal || slot->revoked || slot->pending || slot->servicing;
    pthread_mutex_unlock(&g_mutex);
    if (blocked) return CF_FORBIDDEN;

    if (cf_data_version(slot->app) != ticket.version) {
        pthread_mutex_lock(&g_mutex);
        bool budget = slot->auth_resubmits < CF_CABLE_AUTH_MAX_RESUBMITS;
        if (budget) slot->auth_resubmits++;
        pthread_mutex_unlock(&g_mutex);
        if (resubmit != NULL) *resubmit = budget;
        return CF_BUSY;
    }

    /* Narrow the revoked-check/install window: the loop-local install is
     * serialized with service on the owner thread, and this re-check catches a
     * control filled while the version was being compared. */
    pthread_mutex_lock(&g_mutex);
    blocked = g_fatal || slot->revoked || slot->pending || slot->servicing;
    if (!blocked) slot->auth_resubmits = 0;
    pthread_mutex_unlock(&g_mutex);
    return blocked ? CF_FORBIDDEN : CF_OK;
}

/* ---- test-only controls ------------------------------------------------------------ */

void cf_cable_revocation_test_ack_timeout_ms(uint64_t ms) {
    pthread_mutex_lock(&g_mutex);
    g_ack_timeout_ms = ms;
    pthread_mutex_unlock(&g_mutex);
}

void cf_cable_revocation_test_fatal_fn(cf_cable_revocation_fatal_fn fn,
                                       void *user) {
    pthread_mutex_lock(&g_mutex);
    g_fatal_fn = fn;
    g_fatal_user = user;
    pthread_mutex_unlock(&g_mutex);
}

void cf_cable_revocation_test_reset(void) {
    pthread_mutex_lock(&g_mutex);
    g_fatal = false;
    g_outstanding = 0;
    g_ack_timeout_ms = 0;
    g_fatal_fn = NULL;
    g_fatal_user = NULL;
    pthread_mutex_unlock(&g_mutex);
}
