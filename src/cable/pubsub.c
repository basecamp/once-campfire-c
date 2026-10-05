/* src/cable/pubsub.c — task C02: the cable application object, its connection
 * loops, the local stream maps and the bounded cross-loop broadcast queues.
 * Source: crates/campfire/src/channels.rs (server construction),
 * crates/cable/src/pubsub.rs (subscriptions and delivery) and
 * crates/cable/src/connection.rs (command dispatch, ownership and bounds).
 *
 * One cf_cable_loop per connection, owned by the connection's owner thread.
 * The loop holds its subscriptions (a simple per-subscription stream list)
 * and a bounded pending queue of immutable frame references. A broadcast
 * walks the loops, wraps the published payload once per distinct client
 * identifier (the reference's shared per-identifier frame) and enqueues one
 * command per matching stream subscription; a publisher that finds the owner
 * thread busy handling a client command leaves the queue to be flushed by
 * that handler, which is what keeps a subscribe's confirmation ahead of the
 * broadcast it triggers (reference connection.rs flush order).
 *
 * Worker seam (04 C02/C03, P12-02b). The production reactor performs socket
 * I/O only: upgrade authentication and subscribe validation run on the app's
 * bounded request-worker pool (cf_app_submit_worker) with the worker's own
 * reader; their results are installed back on the owner thread through the
 * C03 install gate/version check, the deferred presence effect runs as a
 * second worker phase, and only then is the confirmation emitted. A full
 * worker queue rejects the subscription (and refuses the connection on the
 * auth path). The reactor holds no DB reader. The standalone socket_run
 * driver (tests, direct callers) keeps the synchronous on-thread path with
 * its connection's own reader.
 *
 * Bounds per loop: CF_CABLE_LOOP_MAX_COMMANDS pending commands and
 * CF_CABLE_LOOP_MAX_BYTES of frame references (each command counts its frame
 * length once). On overflow the broadcast is dropped, the loop and cable
 * dropped-broadcast counters increment, a sanitized stream-class line is
 * logged and CF_BUSY is returned; callers record the post-commit delivery
 * failure, never a pretend rollback. A frame that a socket refuses is
 * dropped and counted the same way. There is no unbounded pubsub bus. */
#include "cable/channels.h"

#include "app.h"
#include "app_internal.h"
#include "auth.h"
#include "cable/revocation.h"
#include "config.h"
#include "db/db_internal.h"
#include "models/user.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

/* ---- the cable and its loops ---------------------------------------------- */

typedef struct cf_cable_cmd {
    struct cf_cable_cmd *next;
    cf_cable_frame *frame; /* retained immutable payload reference */
    size_t bytes;
} cf_cable_cmd;

struct cf_cable_loop {
    struct cf_cable_loop *next;
    cf_cable *cable; /* holds a cable reference while attached */
    pthread_mutex_t mutex;
    cf_cable_loop_send_fn send; /* NULL = a loop with no sink (test) */
    void *send_user;
    int64_t user_id;
    char *user_name;       /* owned NUL-terminated copy */
    char *internal_stream; /* owned "action_cable/<user gid param>" */
    cf_db *reader;         /* reactor-shared (borrowed) or own */
    bool reader_owned;     /* close it in detach only when owned */
    bool handling;         /* owner thread is inside handle_text */
    bool test_hold;        /* test-only: defer flushing (never in production) */
    cf_cable_subscription *subs;
    size_t sub_count, sub_cap;
    cf_cable_cmd *pending_head, *pending_tail;
    size_t pending_count, pending_bytes;
    uint64_t dropped;

    /* C03 production wiring. `revocation` is the legacy preallocated control
     * slot (NULL for a test loop and for a shared-slot member loop);
     * `member` is this connection's member of its owner loop's one shared C03
     * control slot (production reactor; NULL otherwise). `revoked` stops new
     * application output and is only read/written under the loop mutex. A
     * revoke moves the whole subscription array to `deferred` (O(1), no
     * allocation on the barrier path) so the presence effects run after the
     * acknowledgment. */
    cf_cable_revocation *revocation;
    cf_cable_revocation_member *member;
    bool revoked;
    cf_cable_subscription *deferred;
    size_t deferred_count, deferred_cap;
};

struct cf_cable {
    cf_app *app; /* borrowed */
    char *db_path;
    char *secret;
    size_t secret_len;
    size_t max_commands, max_bytes;
    pthread_mutex_t mutex;
    pthread_cond_t idle_cv; /* loops reach zero (shutdown ordering) */
    cf_cable_loop *loops;
    size_t loop_count;
    size_t refs; /* one per loop, one per live connection context, creator */
    bool stopping;
    uint64_t published, delivered, dropped;

    /* One shared C03 control slot per owner loop (one per cable reactor),
     * registered by the owner_start hook and unregistered by owner_stop.
     * `token` is the value cf_cable_socket_transport_token returns. */
    pthread_mutex_t owners_mutex;
    struct {
        void *token;
        cf_cable_revocation *slot;
    } *owners;
    size_t owner_count, owner_cap;

    /* P12-02b measurement: where the last authentication/subscribe model work
     * executed (test-only accessor). */
    cf_cable_work_identity ident_auth, ident_subscribe;
};

void cf_cable_log(const char *what, const char *detail) {
    fprintf(stderr, "campfire: cable: %s%s%s\n", what,
            detail != NULL ? ": " : "", detail != NULL ? detail : "");
}

/* ---- cable lifecycle --------------------------------------------------------- */

cf_err cf_cable_create(const cf_cable_config *config, cf_cable **out) {
    if (out == NULL) return CF_INVALID;
    *out = NULL;
    if (config == NULL || config->app == NULL) return CF_INVALID;
    const cf_config *app_config = cf_app_config(config->app);
    if (app_config == NULL) return CF_INVALID;
    const char *path = config->database_path != NULL
                           ? config->database_path
                           : app_config->database_path;
    if (path == NULL) return CF_INVALID;
    cf_span secret = config->secret_key_base;
    if (secret.len == 0) {
        secret = (cf_span){(const unsigned char *)app_config->secret_key_base,
                           app_config->secret_key_base_len};
    }
    if (secret.len == 0) return CF_INVALID;

    cf_cable *cable = calloc(1, sizeof *cable);
    if (cable == NULL) return CF_NOMEM;
    cable->app = config->app;
    cable->db_path = strdup(path);
    cable->secret = malloc(secret.len + 1);
    if (cable->db_path == NULL || cable->secret == NULL) {
        free(cable->db_path);
        free(cable->secret);
        free(cable);
        return CF_NOMEM;
    }
    memcpy(cable->secret, secret.ptr, secret.len);
    cable->secret[secret.len] = '\0';
    cable->secret_len = secret.len;
    cable->max_commands = config->max_pending_commands != 0
                              ? config->max_pending_commands
                              : CF_CABLE_LOOP_MAX_COMMANDS;
    cable->max_bytes = config->max_pending_bytes != 0
                           ? config->max_pending_bytes
                           : CF_CABLE_LOOP_MAX_BYTES;
    cable->refs = 1;
    if (pthread_mutex_init(&cable->mutex, NULL) != 0) {
        free(cable->db_path);
        free(cable->secret);
        free(cable);
        return CF_INTERNAL;
    }
    if (pthread_cond_init(&cable->idle_cv, NULL) != 0) {
        pthread_mutex_destroy(&cable->mutex);
        free(cable->db_path);
        free(cable->secret);
        free(cable);
        return CF_INTERNAL;
    }
    if (pthread_mutex_init(&cable->owners_mutex, NULL) != 0) {
        pthread_cond_destroy(&cable->idle_cv);
        pthread_mutex_destroy(&cable->mutex);
        free(cable->db_path);
        free(cable->secret);
        free(cable);
        return CF_INTERNAL;
    }
    cable->owner_cap = config->loops != 0 ? config->loops
                                          : CF_CABLE_MAX_REACTORS;
    cable->owners = calloc(cable->owner_cap, sizeof *cable->owners);
    if (cable->owners == NULL) {
        pthread_mutex_destroy(&cable->owners_mutex);
        pthread_cond_destroy(&cable->idle_cv);
        pthread_mutex_destroy(&cable->mutex);
        free(cable->db_path);
        free(cable->secret);
        free(cable);
        return CF_NOMEM;
    }
    *out = cable;
    return CF_OK;
}

/* The cable lock is held; frees the object when the last reference goes. The
 * shared slots and loops can only be in use while a loop or a connection
 * context holds a reference, so nothing can still be reading once refs
 * reaches zero. */
static void cable_release_locked(cf_cable *cable) {
    if (cable->refs > 0) cable->refs--;
    if (cable->refs != 0) {
        pthread_mutex_unlock(&cable->mutex);
        return;
    }
    pthread_mutex_unlock(&cable->mutex);
    free(cable->owners);
    pthread_mutex_destroy(&cable->owners_mutex);
    pthread_cond_destroy(&cable->idle_cv);
    pthread_mutex_destroy(&cable->mutex);
    free(cable->db_path);
    free(cable->secret);
    free(cable);
}

/* One reference for an in-flight connection context (its worker jobs may
 * outlive the socket, so the cable must outlive them). */
static cf_cable *cable_retain(cf_cable *cable) {
    if (cable == NULL) return NULL;
    pthread_mutex_lock(&cable->mutex);
    cable->refs++;
    pthread_mutex_unlock(&cable->mutex);
    return cable;
}

static void cable_release(cf_cable *cable) {
    if (cable == NULL) return;
    pthread_mutex_lock(&cable->mutex);
    cable_release_locked(cable);
}

void cf_cable_destroy(cf_cable *cable) {
    if (cable == NULL) return;
    pthread_mutex_lock(&cable->mutex);
    cable->stopping = true;
    cable_release_locked(cable);
}

/* ---- loops ------------------------------------------------------------------- */

static cf_err loop_send_socket(void *user, cf_cable_frame *frame) {
    return cf_cable_socket_send_frame(user, frame);
}

/* The C03 owner-side operations, defined after subscription_dispose. */
static void loop_revocation_wake(void *user);
static bool loop_revocation_frame_partial(void *user);
static void loop_revocation_revoke(
    void *user, const cf_cable_revocation_control *control);
static void loop_revocation_cleanup(void *user);

static cf_err loop_open(cf_cable *cable, int64_t user_id, const char *user_name,
                        cf_cable_loop_send_fn send, void *send_user,
                        cf_db *reader, bool open_reader_if_null,
                        bool with_revocation,
                        cf_cable_revocation *owner_slot,
                        cf_cable_loop **out) {
    *out = NULL;
    if (cable == NULL || user_name == NULL) return CF_INVALID;
    cf_cable_loop *loop = calloc(1, sizeof *loop);
    if (loop == NULL) return CF_NOMEM;
    loop->cable = cable;
    loop->user_id = user_id;
    loop->send = send;
    loop->send_user = send_user;
    loop->user_name = strdup(user_name);
    if (loop->user_name == NULL) {
        free(loop);
        return CF_NOMEM;
    }
    if (pthread_mutex_init(&loop->mutex, NULL) != 0) {
        free(loop->user_name);
        free(loop);
        return CF_INTERNAL;
    }
    cf_str gid_param = {0};
    cf_err rc = cf_cable_user_gid_param(user_id, &gid_param);
    if (rc == CF_OK) {
        /* action_cable/<user gid param> (server.rs internal_channel). */
        static const char prefix[] = "action_cable/";
        size_t len = sizeof prefix - 1 + gid_param.len;
        loop->internal_stream = malloc(len + 1);
        if (loop->internal_stream == NULL) {
            rc = CF_NOMEM;
        } else {
            memcpy(loop->internal_stream, prefix, sizeof prefix - 1);
            memcpy(loop->internal_stream + sizeof prefix - 1, gid_param.ptr,
                   gid_param.len);
            loop->internal_stream[len] = '\0';
        }
    }
    cf_str_dispose(&gid_param);
    if (rc != CF_OK) {
        pthread_mutex_destroy(&loop->mutex);
        free(loop->user_name);
        free(loop->internal_stream);
        free(loop);
        return rc;
    }
    /* The owner reads through its reader on the synchronous paths: the
     * production reactor hands a connection no reader at all (all model work
     * runs on the request-worker pool and the loop's reader stays NULL), a
     * standalone production socket hands its private reader, and a test loop
     * opens one on this thread (a reader belongs to the thread that opened
     * it). */
    if (reader != NULL) {
        loop->reader = reader;
        loop->reader_owned = false;
    } else if (open_reader_if_null) {
        rc = cf_db_open(cable->db_path, true, &loop->reader);
        if (rc != CF_OK) {
            pthread_mutex_destroy(&loop->mutex);
            free(loop->user_name);
            free(loop->internal_stream);
            free(loop);
            return rc;
        }
        loop->reader_owned = true;
    } else {
        loop->reader = NULL;
        loop->reader_owned = false;
    }

    /* Register the C03 control membership before the loop is linked into the
     * cable, so a barrier can never see an attached loop without its slot (a
     * legacy per-loop slot for standalone/test loops; one member of the owner
     * loop's shared slot for the production reactor). */
    if (owner_slot != NULL) {
        cf_cable_revocation_ops ops;
        memset(&ops, 0, sizeof ops);
        ops.frame_partial = loop_revocation_frame_partial;
        ops.revoke = loop_revocation_revoke;
        ops.cleanup = loop_revocation_cleanup;
        ops.user = loop;
        rc = cf_cable_revocation_member_add(owner_slot, user_id, &ops,
                                            &loop->member);
        if (rc != CF_OK) {
            if (loop->reader_owned) cf_db_close(loop->reader);
            pthread_mutex_destroy(&loop->mutex);
            free(loop->user_name);
            free(loop->internal_stream);
            free(loop);
            return rc;
        }
    } else if (with_revocation) {
        cf_cable_revocation_ops ops;
        memset(&ops, 0, sizeof ops);
        ops.wake = loop_revocation_wake;
        ops.frame_partial = loop_revocation_frame_partial;
        ops.revoke = loop_revocation_revoke;
        ops.cleanup = loop_revocation_cleanup;
        ops.user = loop;
        rc = cf_cable_revocation_register(cable, cable->app, user_id, &ops,
                                          &loop->revocation);
        if (rc != CF_OK) {
            if (loop->reader_owned) cf_db_close(loop->reader);
            pthread_mutex_destroy(&loop->mutex);
            free(loop->user_name);
            free(loop->internal_stream);
            free(loop);
            return rc;
        }
    }

    pthread_mutex_lock(&cable->mutex);
    if (cable->stopping) {
        pthread_mutex_unlock(&cable->mutex);
        /* The slot/member is already registered; remove it before the loop
         * memory goes away so no barrier can wake a freed loop. */
        if (loop->revocation != NULL) {
            cf_cable_revocation_unregister(loop->revocation);
            loop->revocation = NULL;
        }
        if (loop->member != NULL) {
            cf_cable_revocation_member_remove(loop->member);
            loop->member = NULL;
        }
        if (loop->reader_owned) cf_db_close(loop->reader);
        pthread_mutex_destroy(&loop->mutex);
        free(loop->user_name);
        free(loop->internal_stream);
        free(loop);
        return CF_BUSY;
    }
    loop->next = cable->loops;
    cable->loops = loop;
    cable->loop_count++;
    cable->refs++;
    pthread_mutex_unlock(&cable->mutex);
    *out = loop;
    return CF_OK;
}

/* No cable or loop lock held; the subscription is already out of every
 * shared structure. */
static void subscription_dispose(cf_cable_subscription *sub) {
    if (sub->channel != NULL) cf_cable_channel_dispose(sub->channel);
    free(sub->identifier);
    free(sub->encoded);
    for (size_t i = 0; i < sub->stream_count; i++) free(sub->streams[i]);
    free(sub->streams);
    memset(sub, 0, sizeof *sub);
}

/* ---- C03 owner-side revocation operations ---------------------------------- */

/* The barrier thread calls this with the C03 state mutex held; it must not
 * block or call back into C03 (revocation.h). The eventfd write is lock-free;
 * registration lives until after the socket-run teardown (on_close), so the
 * socket is still alive whenever a slot can still be woken. */
static void loop_revocation_wake(void *user) {
    cf_cable_loop *loop = user;
    cf_cable_socket_wake(loop->send_user);
}

/* Owner thread: is an application frame partially sent? */
static bool loop_revocation_frame_partial(void *user) {
    cf_cable_loop *loop = user;
    return cf_cable_socket_frame_partial(loop->send_user);
}

/* Owner thread, before the acknowledgment: attempt the reference disconnect
 * frame when no application frame is partially sent, then close, and remove
 * every subscription from delivery without running the unsubscribe effects
 * (cf_write would block on the writer that is waiting for the ack). The whole
 * subscription array moves to the deferred list in O(1), with no allocation
 * on the barrier path. */
static void loop_revocation_revoke(
    void *user, const cf_cable_revocation_control *control) {
    cf_cable_loop *loop = user;
    cf_cable_socket *socket = loop->send_user;
    pthread_mutex_lock(&loop->mutex);
    loop->revoked = true;
    if (control->attempt_disconnect && control->frame_len != 0 &&
        socket != NULL) {
        (void)cf_cable_socket_send_text(
            socket, (cf_span){(const unsigned char *)control->frame,
                              control->frame_len});
    }
    /* revoke and cleanup run in one service call on this thread, so no
     * deferred array can already be present. */
    if (loop->subs != NULL && loop->deferred == NULL) {
        loop->deferred = loop->subs;
        loop->deferred_count = loop->sub_count;
        loop->deferred_cap = loop->sub_cap;
        loop->subs = NULL;
        loop->sub_count = 0;
        loop->sub_cap = 0;
    }
    pthread_mutex_unlock(&loop->mutex);

    /* A normal 1000 close after everything already queued, exactly like the
     * reference (close immediately when the disconnect frame was skipped). */
    if (socket != NULL) cf_cable_socket_request_close(socket);
}

/* Owner thread, after the acknowledgment (the writer is no longer waiting):
 * run the deferred subscriptions' unsubscribed effects, which may cf_write.
 * A full writer queue is logged by the presence path and left to the
 * membership TTL (C03). */
static void loop_revocation_cleanup(void *user) {
    cf_cable_loop *loop = user;
    pthread_mutex_lock(&loop->mutex);
    cf_cable_subscription *subs = loop->deferred;
    size_t count = loop->deferred_count;
    loop->deferred = NULL;
    loop->deferred_count = 0;
    loop->deferred_cap = 0;
    pthread_mutex_unlock(&loop->mutex);
    for (size_t i = 0; i < count; i++) {
        cf_cable_subscription_unsubscribed(loop, &subs[i]);
    }
    for (size_t i = count; i > 0; i--) {
        subscription_dispose(&subs[i - 1]);
    }
    free(subs);
}

/* Detach must run on the loop's owner thread: it closes the loop reader and
 * runs every subscription's unsubscribed effects. It first unlinks the loop
 * under the cable lock, so no publisher can be using it afterwards. */
void cf_cable_loop_detach(cf_cable_loop *loop) {
    if (loop == NULL) return;
    cf_cable *cable = loop->cable;
    pthread_mutex_lock(&cable->mutex);
    cf_cable_loop **link = &cable->loops;
    while (*link != NULL && *link != loop) link = &(*link)->next;
    if (*link == loop) {
        *link = loop->next;
        cable->loop_count--;
    }
    pthread_cond_broadcast(&cable->idle_cv);
    pthread_mutex_unlock(&cable->mutex);

    /* Acknowledge any pending barrier control and remove the slot/member
     * before the loop memory is released; the owner thread unregisters after
     * its last service call. Removing the member also acknowledges a pending
     * shared-slot control when it was the last match (revocation.c), so a
     * closing session cannot stall the writer. */
    if (loop->revocation != NULL) {
        cf_cable_revocation_unregister(loop->revocation);
        loop->revocation = NULL;
    }
    if (loop->member != NULL) {
        cf_cable_revocation_member_remove(loop->member);
        loop->member = NULL;
    }

    /* A revoke that never reached cleanup (the loop detached while its control
     * was pending) still runs its presence effects now: unregister already
     * released the waiting writer. */
    for (size_t i = 0; i < loop->deferred_count; i++) {
        cf_cable_subscription_unsubscribed(loop, &loop->deferred[i]);
    }
    for (size_t i = loop->deferred_count; i > 0; i--) {
        subscription_dispose(&loop->deferred[i - 1]);
    }
    free(loop->deferred);
    loop->deferred = NULL;
    loop->deferred_count = 0;
    loop->deferred_cap = 0;

    for (size_t i = 0; i < loop->sub_count; i++) {
        cf_cable_subscription_unsubscribed(loop, &loop->subs[i]);
    }
    for (size_t i = loop->sub_count; i > 0; i--) {
        subscription_dispose(&loop->subs[i - 1]);
    }
    free(loop->subs);
    loop->sub_count = loop->sub_cap = 0;

    cf_cable_cmd *cmd = loop->pending_head;
    while (cmd != NULL) {
        cf_cable_cmd *next = cmd->next;
        cf_cable_frame_release(cmd->frame);
        free(cmd);
        cmd = next;
    }
    loop->pending_head = loop->pending_tail = NULL;
    loop->pending_count = loop->pending_bytes = 0;

    if (loop->reader_owned && loop->reader != NULL) cf_db_close(loop->reader);
    pthread_mutex_destroy(&loop->mutex);
    free(loop->user_name);
    free(loop->internal_stream);
    free(loop);

    pthread_mutex_lock(&cable->mutex);
    cable_release_locked(cable);
}

/* Attach a connection's loop. The production reactor path passes `async` with
 * a NULL reader (the loop holds no DB reader; model work runs on the pool)
 * and the owner loop's shared C03 slot; the standalone/test path passes its
 * own reader (or NULL to open one here) and registers a legacy per-loop
 * slot. */
cf_err cf_cable_loop_attach_socket(cf_cable *cable, cf_cable_socket *socket,
                                   int64_t user_id, const char *user_name,
                                   cf_db *reader, bool async,
                                   cf_cable_revocation *owner_slot,
                                   cf_cable_loop **out) {
    return loop_open(cable, user_id, user_name, loop_send_socket, socket,
                     reader, false, !async, async ? owner_slot : NULL, out);
}

cf_cable_revocation_member *cf_cable_loop_revocation_member(
    const cf_cable_loop *loop) {
    return loop != NULL ? loop->member : NULL;
}

cf_cable_revocation *cf_cable_loop_revocation(cf_cable_loop *loop) {
    return loop != NULL ? loop->revocation : NULL;
}

int64_t cf_cable_loop_user_id(const cf_cable_loop *loop) {
    return loop != NULL ? loop->user_id : 0;
}

const char *cf_cable_loop_user_name(const cf_cable_loop *loop) {
    return loop != NULL ? loop->user_name : NULL;
}

cf_db *cf_cable_loop_reader(cf_cable_loop *loop) {
    return loop != NULL ? loop->reader : NULL;
}

cf_app *cf_cable_loop_app(const cf_cable_loop *loop) {
    return loop != NULL ? loop->cable->app : NULL;
}

cf_cable *cf_cable_loop_cable(cf_cable_loop *loop) {
    return loop != NULL ? loop->cable : NULL;
}

cf_span cf_cable_secret(cf_cable *cable) {
    if (cable == NULL) return (cf_span){NULL, 0};
    return (cf_span){(const unsigned char *)cable->secret, cable->secret_len};
}

cf_err cf_cable_stop(cf_cable *cable, uint64_t timeout_ms) {
    if (cable == NULL) return CF_INVALID;
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(timeout_ms / 1000);
    deadline.tv_nsec += (long)((timeout_ms % 1000) * 1000000);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&cable->mutex);
    cable->stopping = true;
    cf_err rc = CF_OK;
    while (cable->loop_count != 0) {
        int wait_rc = pthread_cond_timedwait(&cable->idle_cv, &cable->mutex,
                                             &deadline);
        if (wait_rc == ETIMEDOUT) {
            rc = CF_BUSY;
            break;
        }
    }
    pthread_mutex_unlock(&cable->mutex);
    return rc;
}

void cf_cable_stats_get(const cf_cable *cable, cf_cable_stats *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof *out);
    if (cable == NULL) return;
    cf_cable *mutable_cable = (cf_cable *)cable;
    pthread_mutex_lock(&mutable_cable->mutex);
    out->loops = cable->loop_count;
    out->published = cable->published;
    out->delivered = cable->delivered;
    out->dropped = cable->dropped;
    pthread_mutex_unlock(&mutable_cable->mutex);
}

uint64_t cf_cable_loop_dropped(const cf_cable_loop *loop) {
    if (loop == NULL) return 0;
    cf_cable_loop *mutable_loop = (cf_cable_loop *)loop;
    pthread_mutex_lock(&mutable_loop->mutex);
    uint64_t dropped = loop->dropped;
    pthread_mutex_unlock(&mutable_loop->mutex);
    return dropped;
}

size_t cf_cable_loop_pending(const cf_cable_loop *loop) {
    if (loop == NULL) return 0;
    cf_cable_loop *mutable_loop = (cf_cable_loop *)loop;
    pthread_mutex_lock(&mutable_loop->mutex);
    size_t pending = loop->pending_count;
    pthread_mutex_unlock(&mutable_loop->mutex);
    return pending;
}

/* ---- subscriptions ------------------------------------------------------------ */

cf_cable_subscription *cf_cable_loop_add_subscription(cf_cable_loop *loop,
                                                      cf_span identifier,
                                                      const char *class_name) {
    if (loop == NULL || class_name == NULL) return NULL;
    if (identifier.len > CF_CABLE_MAX_IDENTIFIER_BYTES) return NULL;
    if (loop->sub_count >= CF_CABLE_MAX_SUBSCRIPTIONS) return NULL;
    char *raw = malloc(identifier.len + 1);
    if (raw == NULL) return NULL;
    memcpy(raw, identifier.ptr, identifier.len);
    raw[identifier.len] = '\0';
    cf_builder encoded = {0};
    cf_err rc = cf_json_string(&encoded, identifier);
    if (rc != CF_OK) {
        cf_builder_dispose(&encoded);
        free(raw);
        return NULL;
    }
    pthread_mutex_lock(&loop->mutex);
    if (loop->sub_count == loop->sub_cap) {
        size_t cap = loop->sub_cap != 0 ? loop->sub_cap * 2 : 8;
        cf_cable_subscription *grown =
            realloc(loop->subs, cap * sizeof *grown);
        if (grown == NULL) {
            pthread_mutex_unlock(&loop->mutex);
            cf_builder_dispose(&encoded);
            free(raw);
            return NULL;
        }
        loop->subs = grown;
        loop->sub_cap = cap;
    }
    cf_cable_subscription *sub = &loop->subs[loop->sub_count];
    memset(sub, 0, sizeof *sub);
    sub->owner = loop;
    sub->identifier = raw;
    sub->identifier_len = identifier.len;
    sub->encoded = malloc(encoded.len + 1);
    if (sub->encoded == NULL) {
        pthread_mutex_unlock(&loop->mutex);
        cf_builder_dispose(&encoded);
        free(raw);
        return NULL;
    }
    memcpy(sub->encoded, encoded.ptr, encoded.len);
    sub->encoded[encoded.len] = '\0';
    cf_builder_dispose(&encoded);
    sub->class_name = class_name;
    loop->sub_count++;
    pthread_mutex_unlock(&loop->mutex);
    return sub;
}

void cf_cable_loop_remove_subscription(cf_cable_loop *loop, size_t index) {
    if (loop == NULL) return;
    pthread_mutex_lock(&loop->mutex);
    if (index >= loop->sub_count) {
        pthread_mutex_unlock(&loop->mutex);
        return;
    }
    cf_cable_subscription removed = loop->subs[index];
    memmove(&loop->subs[index], &loop->subs[index + 1],
            (loop->sub_count - index - 1) * sizeof *loop->subs);
    loop->sub_count--;
    pthread_mutex_unlock(&loop->mutex);
    subscription_dispose(&removed);
}

cf_cable_subscription *cf_cable_loop_find_subscription(cf_cable_loop *loop,
                                                       cf_span identifier) {
    if (loop == NULL || identifier.len > CF_CABLE_MAX_IDENTIFIER_BYTES) {
        return NULL;
    }
    for (size_t i = 0; i < loop->sub_count; i++) {
        if (loop->subs[i].identifier_len == identifier.len &&
            memcmp(loop->subs[i].identifier, identifier.ptr,
                   identifier.len) == 0) {
            return &loop->subs[i];
        }
    }
    return NULL;
}

size_t cf_cable_loop_subscription_count(cf_cable_loop *loop) {
    return loop != NULL ? loop->sub_count : 0;
}

size_t cf_cable_loop_subscriptions(const cf_cable_loop *loop) {
    return loop != NULL ? loop->sub_count : 0;
}

cf_cable_subscription *cf_cable_loop_subscription_at(cf_cable_loop *loop,
                                                     size_t index) {
    if (loop == NULL || index >= loop->sub_count) return NULL;
    return &loop->subs[index];
}

cf_err cf_cable_subscription_stream(cf_cable_subscription *sub,
                                    cf_span stream) {
    if (sub == NULL || (stream.len != 0 && stream.ptr == NULL)) {
        return CF_INVALID;
    }
    char *copy = malloc(stream.len + 1);
    if (copy == NULL) return CF_NOMEM;
    memcpy(copy, stream.ptr, stream.len);
    copy[stream.len] = '\0';
    pthread_mutex_lock(&sub->owner->mutex);
    if (sub->stream_count == sub->stream_cap) {
        size_t cap = sub->stream_cap != 0 ? sub->stream_cap * 2 : 4;
        char **grown = realloc(sub->streams, cap * sizeof *grown);
        if (grown == NULL) {
            pthread_mutex_unlock(&sub->owner->mutex);
            free(copy);
            return CF_NOMEM;
        }
        sub->streams = grown;
        sub->stream_cap = cap;
    }
    sub->streams[sub->stream_count++] = copy;
    pthread_mutex_unlock(&sub->owner->mutex);
    return CF_OK;
}

void cf_cable_subscription_reject(cf_cable_subscription *sub) {
    if (sub != NULL) sub->rejected = true;
}

bool cf_cable_subscription_is_rejected(const cf_cable_subscription *sub) {
    return sub != NULL && sub->rejected;
}

/* ---- direct sends (owner thread) ---------------------------------------------- */

cf_err cf_cable_loop_send_text(cf_cable_loop *loop, cf_span text) {
    if (loop == NULL || (text.len != 0 && text.ptr == NULL)) return CF_INVALID;
    cf_cable_frame *frame = NULL;
    cf_err rc = cf_cable_frame_create(text, &frame);
    if (rc != CF_OK) return rc;
    if (loop->send != NULL) {
        rc = loop->send(loop->send_user, frame);
    } else {
        rc = CF_IO;
    }
    cf_cable_frame_release(frame);
    return rc;
}

cf_err cf_cable_loop_send_confirm(cf_cable_loop *loop, cf_span identifier) {
    cf_str frame = {0};
    cf_err rc = cf_cable_confirm_subscription(identifier, &frame);
    if (rc == CF_OK) {
        rc = cf_cable_loop_send_text(
            loop, (cf_span){(const unsigned char *)frame.ptr, frame.len});
    }
    cf_str_dispose(&frame);
    return rc;
}

cf_err cf_cable_loop_send_reject(cf_cable_loop *loop, cf_span identifier) {
    cf_str frame = {0};
    cf_err rc = cf_cable_reject_subscription(identifier, &frame);
    if (rc == CF_OK) {
        rc = cf_cable_loop_send_text(
            loop, (cf_span){(const unsigned char *)frame.ptr, frame.len});
    }
    cf_str_dispose(&frame);
    return rc;
}

/* ---- queueing and delivery ------------------------------------------------------ */

/* The sanitized stream class for a drop log: the channel prefix, never the
 * GID/user part (04 C02: "log the stream class without identifiers"). */
static const char *stream_class(cf_span stream, char *buf, size_t cap) {
    static const char user_reads[] = "user_reads";
    static const char user_unreads[] = "user_unreads";
    static const char action_cable[] = "action_cable";
    if (stream.len >= 11 && memcmp(stream.ptr, "user_", 5) == 0 &&
        memcmp(stream.ptr + stream.len - 6, "_reads", 6) == 0) {
        return user_reads;
    }
    if (stream.len >= 13 && memcmp(stream.ptr, "user_", 5) == 0 &&
        memcmp(stream.ptr + stream.len - 8, "_unreads", 8) == 0) {
        return user_unreads;
    }
    if (stream.len >= 13 && memcmp(stream.ptr, "action_cable/", 13) == 0) {
        return action_cable;
    }
    if (stream.ptr == NULL || stream.len == 0) return "";
    const unsigned char *colon = memchr(stream.ptr, ':', stream.len);
    size_t len = colon != NULL ? (size_t)(colon - stream.ptr) : stream.len;
    if (len >= cap) len = cap - 1;
    memcpy(buf, stream.ptr, len);
    buf[len] = '\0';
    return buf;
}

/* Returns CF_OK, CF_BUSY (bound reached; the drop is counted here) or a
 * memory error. The caller logs the stream class once per loop. */
static cf_err loop_enqueue_locked(cf_cable_loop *loop, cf_cable_frame *frame,
                                  size_t bytes) {
    cf_cable *cable = loop->cable;
    if (loop->pending_count >= cable->max_commands ||
        bytes > cable->max_bytes ||
        loop->pending_bytes > cable->max_bytes - bytes) {
        loop->dropped++;
        cable->dropped++;
        return CF_BUSY;
    }
    cf_cable_cmd *cmd = calloc(1, sizeof *cmd);
    if (cmd == NULL) return CF_NOMEM;
    cmd->frame = cf_cable_frame_retain(frame);
    cmd->bytes = bytes;
    if (loop->pending_tail != NULL) {
        loop->pending_tail->next = cmd;
    } else {
        loop->pending_head = cmd;
    }
    loop->pending_tail = cmd;
    loop->pending_count++;
    loop->pending_bytes += bytes;
    return CF_OK;
}

static void loop_flush_locked(cf_cable_loop *loop) {
    while (loop->pending_head != NULL) {
        cf_cable_cmd *cmd = loop->pending_head;
        loop->pending_head = cmd->next;
        if (loop->pending_head == NULL) loop->pending_tail = NULL;
        loop->pending_count--;
        loop->pending_bytes -= cmd->bytes;
        cf_err rc = loop->send != NULL
                        ? loop->send(loop->send_user, cmd->frame)
                        : CF_IO;
        if (rc == CF_OK) {
            loop->cable->delivered++;
        } else {
            /* The sink refused (CF_BUSY: the transport queue cap; CF_IO: the
             * connection is gone). The frame is dropped and counted: callers
             * record the post-commit delivery failure. */
            loop->dropped++;
            loop->cable->dropped++;
            cf_cable_log("dropped broadcast", "delivery failed");
        }
        cf_cable_frame_release(cmd->frame);
        free(cmd);
        if (rc != CF_OK) {
            while (loop->pending_head != NULL) {
                cf_cable_cmd *rest = loop->pending_head;
                loop->pending_head = rest->next;
                loop->pending_count--;
                loop->pending_bytes -= rest->bytes;
                loop->dropped++;
                loop->cable->dropped++;
                cf_cable_frame_release(rest->frame);
                free(rest);
            }
            loop->pending_tail = NULL;
            break;
        }
    }
}

/* ---- publish ------------------------------------------------------------------- */

/* One wrapped frame per distinct encoded identifier in this publish (the
 * reference wraps a payload once per identifier and shares the frame). */
typedef struct {
    const char *identifier; /* encoded JSON literal, or NULL for internal */
    cf_cable_frame *frame;
    size_t bytes;
} publish_frame;

typedef struct {
    publish_frame *items;
    size_t len, cap;
} publish_cache;

static void publish_cache_dispose(publish_cache *cache) {
    for (size_t i = 0; i < cache->len; i++) {
        cf_cable_frame_release(cache->items[i].frame);
    }
    free(cache->items);
    memset(cache, 0, sizeof *cache);
}

static cf_cable_frame *publish_frame_for(publish_cache *cache,
                                         const char *encoded,
                                         cf_span payload, size_t *bytes_out) {
    for (size_t i = 0; i < cache->len; i++) {
        if (cache->items[i].identifier == NULL && encoded == NULL) {
            *bytes_out = cache->items[i].bytes;
            return cache->items[i].frame;
        }
        if (cache->items[i].identifier != NULL && encoded != NULL &&
            strcmp(cache->items[i].identifier, encoded) == 0) {
            *bytes_out = cache->items[i].bytes;
            return cache->items[i].frame;
        }
    }
    cf_builder text = {0};
    cf_err rc = CF_OK;
    if (encoded == NULL) {
        rc = cf_builder_append(&text, payload);
    } else {
        cf_str wrapped = {0};
        rc = cf_cable_message_frame(
            (cf_span){(const unsigned char *)encoded, strlen(encoded)},
            payload, &wrapped);
        if (rc == CF_OK) {
            rc = cf_builder_append(&text,
                                   (cf_span){(const unsigned char *)wrapped.ptr,
                                             wrapped.len});
            cf_str_dispose(&wrapped);
        }
    }
    if (rc != CF_OK) {
        cf_builder_dispose(&text);
        return NULL;
    }
    cf_cable_frame *frame = NULL;
    rc = cf_cable_frame_create(
        (cf_span){(const unsigned char *)text.ptr, text.len}, &frame);
    size_t bytes = text.len;
    cf_builder_dispose(&text);
    if (rc != CF_OK) return NULL;
    if (cache->len == cache->cap) {
        size_t cap = cache->cap != 0 ? cache->cap * 2 : 8;
        publish_frame *grown = realloc(cache->items, cap * sizeof *grown);
        if (grown == NULL) {
            cf_cable_frame_release(frame);
            return NULL;
        }
        cache->items = grown;
        cache->cap = cap;
    }
    cache->items[cache->len].identifier = encoded;
    cache->items[cache->len].frame = frame;
    cache->items[cache->len].bytes = bytes;
    cache->len++;
    *bytes_out = bytes;
    return frame;
}

static bool stream_is(cf_span stream, const char *name) {
    size_t len = strlen(name);
    if (stream.len != len) return false;
    if (len == 0) return true;
    if (stream.ptr == NULL) return false;
    return memcmp(stream.ptr, name, len) == 0;
}

/* Deliver one payload to one loop: its internal channel (raw) and every
 * local stream subscription that matches. Returns CF_OK, CF_BUSY (one or
 * more commands were dropped) or a memory error. The cable and loop locks
 * are held. */
static cf_err loop_publish_locked(cf_cable_loop *loop, cf_span stream,
                                  cf_span payload, publish_cache *cache) {
    /* A revoked loop accepts no new application output (C03). The loop mutex
     * is held. */
    if (loop->revoked) return CF_OK;
    cf_err result = CF_OK;
    bool logged = false;
    if (loop->internal_stream != NULL &&
        stream_is(stream, loop->internal_stream)) {
        size_t bytes = 0;
        cf_cable_frame *frame = publish_frame_for(cache, NULL, payload, &bytes);
        if (frame == NULL) return CF_NOMEM;
        cf_err rc = loop_enqueue_locked(loop, frame, bytes);
        if (rc == CF_BUSY) {
            char cls[64];
            cf_cable_log("dropped broadcast",
                         stream_class(stream, cls, sizeof cls));
            return CF_BUSY;
        }
        if (rc != CF_OK) return rc;
    }
    for (size_t i = 0; i < loop->sub_count; i++) {
        cf_cable_subscription *sub = &loop->subs[i];
        for (size_t j = 0; j < sub->stream_count; j++) {
            if (!stream_is(stream, sub->streams[j])) continue;
            size_t bytes = 0;
            cf_cable_frame *frame =
                publish_frame_for(cache, sub->encoded, payload, &bytes);
            if (frame == NULL) return CF_NOMEM;
            cf_err rc = loop_enqueue_locked(loop, frame, bytes);
            if (rc == CF_BUSY) {
                result = CF_BUSY;
                if (!logged) {
                    char cls[64];
                    cf_cable_log("dropped broadcast",
                                 stream_class(stream, cls, sizeof cls));
                    logged = true;
                }
                break; /* further commands to this loop would drop too */
            }
            if (rc != CF_OK) return rc;
        }
    }
    if (!loop->handling && !loop->test_hold) loop_flush_locked(loop);
    return result;
}

cf_err cf_cable_publish(cf_cable *cable, cf_span stream, cf_buf *payload) {
    if (cable == NULL || payload == NULL) return CF_INVALID;
    if (stream.len != 0 && stream.ptr == NULL) return CF_INVALID;
    cf_span payload_span = cf_buf_span(payload);
    cf_err result = CF_OK;
    publish_cache cache = {0};
    pthread_mutex_lock(&cable->mutex);
    if (cable->stopping) {
        pthread_mutex_unlock(&cable->mutex);
        return CF_BUSY;
    }
    cable->published++;
    for (cf_cable_loop *loop = cable->loops; loop != NULL; loop = loop->next) {
        pthread_mutex_lock(&loop->mutex);
        cf_err rc = loop_publish_locked(loop, stream, payload_span, &cache);
        pthread_mutex_unlock(&loop->mutex);
        if (rc != CF_OK && result == CF_OK) result = rc;
    }
    pthread_mutex_unlock(&cable->mutex);
    publish_cache_dispose(&cache);
    return result;
}

/* ---- production hooks ----------------------------------------------------------- */

/* One connection's identity and application loop, attached to the socket at
 * authenticate and released by on_close. This replaces the old per-thread
 * identity: one reactor thread services every upgraded socket of an HTTP
 * loop, so per-connection state must live on the connection, never on the
 * thread. */
typedef enum {
    CONN_AUTH_NONE = 0,
    CONN_AUTH_QUEUED,   /* auth closure submitted to the worker pool */
    CONN_AUTH_READY,    /* worker result stored; owner must install it */
    CONN_AUTH_INSTALLING, /* owner is inside the install step */
    CONN_AUTH_REFUSED,  /* pool full, worker failure, or gate refusal */
    CONN_AUTH_DONE
} conn_auth_phase;

typedef enum {
    CMD_NONE = 0,
    CMD_VALIDATE_QUEUED,
    CMD_VALIDATED,      /* validation plan stored; owner must gate/apply */
    CMD_GATING,
    CMD_EFFECTS_QUEUED, /* deferred presence effect submitted */
    CMD_EFFECTS_DONE
} conn_cmd_phase;

typedef struct cf_cable_conn {
    cf_cable *cable;         /* retained for this context's lifetime */
    cf_cable_socket *socket; /* NULL once on_close ran (under mutex) */
    cf_cable_loop *loop;     /* owner thread only; NULL until/after attach */
    cf_cable_revocation_member *member; /* the loop's shared-slot member */
    bool async;              /* production reactor transport */
    cf_db *reader;           /* standalone/test path only */
    bool reader_owned;
    const cf_cable_request *borrowed_request; /* sync path: socket lifetime */
    bool have_identity;      /* sync path resolved an identity */
    bool refused;            /* the install gate refused this connection */
    int64_t user_id;
    char user_name[256];
    cf_cable_auth_ticket ticket;

    /* Worker handoff. A submitted closure holds one reference; the owner
     * holds one until on_close; the last reference frees. `closed` and the
     * phase/result fields are guarded by `mutex`, which also makes waking the
     * socket safe against a concurrent free (on_close runs under it before
     * the socket is released). */
    pthread_mutex_t mutex;
    int refs;
    bool closed;
    uintptr_t submit_thread;    /* owner loop that submitted the last job */
    bool submit_owner_reader;   /* owner loop still had a reader (false) */

    /* Async connection authentication. */
    conn_auth_phase auth_phase;
    cf_err auth_rc;
    bool auth_ok;
    int64_t auth_user_id;
    char auth_name[256];
    cf_cable_request request;   /* owned copy for the worker */
    unsigned char *req_strings;
    cf_header *req_headers;

    /* Async subscribe/message command. */
    conn_cmd_phase cmd_phase;
    bool cmd_subscribe;             /* subscribe (reply) vs message */
    cf_cable_subscription *cmd_sub; /* owner-side shell/target */
    const char *cmd_class;          /* static class-table string */
    char *cmd_identifier;           /* owned copy the worker reads */
    size_t cmd_identifier_len;
    cf_cable_auth_ticket cmd_ticket;
    cf_cable_sub_plan plan;
    cf_err cmd_rc;
} cf_cable_conn;

/* The session/user reads behind one connection identity. Reconnect and the
 * stale-version resubmission both re-run it, so it always reads the database
 * fresh. reader belongs to the calling (owner) thread. */
static cf_err cable_read_identity(cf_db *reader, cf_cable *cable,
                                  const cf_cable_request *request,
                                  bool *authenticated, int64_t *user_id,
                                  char *user_name, size_t user_name_cap) {
    *authenticated = false;
    *user_id = 0;
    if (reader == NULL) return CF_IO;
    cf_cable_session_auth auth = {
        .reader = reader,
        .secret_key_base = (cf_span){(const unsigned char *)cable->secret,
                                     cable->secret_len},
    };
    cf_err rc = cf_cable_session_authenticate(&auth, request, authenticated,
                                              user_id);
    if (rc != CF_OK || !*authenticated) return rc;
    cf_user row;
    bool found = false;
    rc = cf_user_find_by_id(reader, *user_id, &found, &row);
    if (rc != CF_OK || !found) {
        if (rc == CF_OK) rc = CF_NOT_FOUND;
        *authenticated = false;
        return rc;
    }
    size_t name_len = row.name.len;
    if (name_len >= user_name_cap) name_len = user_name_cap - 1;
    if (name_len != 0) memcpy(user_name, row.name.ptr, name_len);
    user_name[name_len] = '\0';
    cf_user_dispose(&row);
    return CF_OK;
}

/* ---- one shared C03 control slot per owner loop ----------------------------- */

/* The reactor's owner_start hook: register the loop's one preallocated
 * control slot before it enrolls any connection (P12-02b). */
static cf_err cable_owner_start(void *user, void *token,
                                void (*wake_owner)(void *token)) {
    cf_cable *cable = user;
    if (cable == NULL || token == NULL || wake_owner == NULL) return CF_INVALID;
    cf_cable_revocation_loop_ops ops;
    memset(&ops, 0, sizeof ops);
    ops.wake = wake_owner;
    ops.user = token;
    cf_cable_revocation *slot = NULL;
    cf_err rc = cf_cable_revocation_loop_register(cable, cable->app, &ops,
                                                  &slot);
    if (rc != CF_OK) return rc;
    pthread_mutex_lock(&cable->mutex);
    bool stopping = cable->stopping;
    pthread_mutex_unlock(&cable->mutex);
    if (stopping) {
        cf_cable_revocation_loop_unregister(slot);
        return CF_BUSY;
    }
    pthread_mutex_lock(&cable->owners_mutex);
    if (cable->owner_count == cable->owner_cap) {
        size_t cap = cable->owner_cap != 0 ? cable->owner_cap * 2 : 8;
        void *grown = realloc(cable->owners, cap * sizeof *cable->owners);
        if (grown == NULL) {
            pthread_mutex_unlock(&cable->owners_mutex);
            cf_cable_revocation_loop_unregister(slot);
            return CF_NOMEM;
        }
        cable->owners = grown;
        cable->owner_cap = cap;
    }
    cable->owners[cable->owner_count].token = token;
    cable->owners[cable->owner_count].slot = slot;
    cable->owner_count++;
    pthread_mutex_unlock(&cable->owners_mutex);
    return CF_OK;
}

/* The reactor's owner_stop hook: remove the loop's slot after its last
 * socket finished (a pending control is acknowledged by the unregister). */
static void cable_owner_stop(void *user, void *token) {
    cf_cable *cable = user;
    if (cable == NULL || token == NULL) return;
    cf_cable_revocation *slot = NULL;
    pthread_mutex_lock(&cable->owners_mutex);
    for (size_t i = 0; i < cable->owner_count; i++) {
        if (cable->owners[i].token != token) continue;
        slot = cable->owners[i].slot;
        cable->owners[i] = cable->owners[cable->owner_count - 1];
        cable->owner_count--;
        break;
    }
    pthread_mutex_unlock(&cable->owners_mutex);
    if (slot != NULL) cf_cable_revocation_loop_unregister(slot);
}

/* The socket's owner-loop slot (NULL for a standalone socket, which has no
 * reactor token and uses its loop's legacy slot). */
static cf_cable_revocation *cable_slot_for(cf_cable *cable, void *token) {
    if (cable == NULL || token == NULL) return NULL;
    cf_cable_revocation *slot = NULL;
    pthread_mutex_lock(&cable->owners_mutex);
    for (size_t i = 0; i < cable->owner_count; i++) {
        if (cable->owners[i].token == token) {
            slot = cable->owners[i].slot;
            break;
        }
    }
    pthread_mutex_unlock(&cable->owners_mutex);
    return slot;
}

/* ---- connection context lifetime and worker handoff ------------------------ */

static void conn_free(cf_cable_conn *ctx) {
    cf_cable_sub_plan_dispose(&ctx->plan);
    free(ctx->cmd_identifier);
    free(ctx->req_strings);
    free(ctx->req_headers);
    if (ctx->reader_owned && ctx->reader != NULL) cf_db_close(ctx->reader);
    pthread_mutex_destroy(&ctx->mutex);
    cable_release(ctx->cable);
    free(ctx);
}

static void conn_ref(cf_cable_conn *ctx) {
    pthread_mutex_lock(&ctx->mutex);
    ctx->refs++;
    pthread_mutex_unlock(&ctx->mutex);
}

static void conn_unref(cf_cable_conn *ctx) {
    bool last = false;
    pthread_mutex_lock(&ctx->mutex);
    if (--ctx->refs == 0) last = true;
    pthread_mutex_unlock(&ctx->mutex);
    if (last) conn_free(ctx);
}

/* The worker's completion signal. The socket wake happens under the context
 * mutex: on_close (which runs before the socket memory is released) takes the
 * same mutex to clear `socket`, so a wake can never touch a freed socket. */
static void conn_wake_locked(cf_cable_conn *ctx) {
    if (!ctx->closed && ctx->socket != NULL) cf_cable_socket_wake(ctx->socket);
}

/* Owned copy of the upgrade request for worker reads: the socket's request
 * spans die with the connection, the worker may outlive it. */
static cf_err conn_copy_request(cf_cable_conn *ctx,
                                const cf_cable_request *src) {
    ctx->request = *src;
    size_t len = src->target.len + src->path.len + src->query.len +
                 src->peer_ip.len;
    for (size_t i = 0; i < src->header_count; i++) {
        len += src->headers[i].name.len + src->headers[i].value.len;
    }
    ctx->req_strings = malloc(len != 0 ? len : 1);
    ctx->req_headers = calloc(src->header_count != 0 ? src->header_count : 1,
                              sizeof *ctx->req_headers);
    if (ctx->req_strings == NULL || ctx->req_headers == NULL) return CF_NOMEM;
    size_t off = 0;
#define CONN_COPY_SPAN(dst, s)                          \
    do {                                                \
        (dst) = (cf_span){ctx->req_strings + off, (s).len}; \
        if ((s).len != 0) {                             \
            memcpy(ctx->req_strings + off, (s).ptr, (s).len); \
            off += (s).len;                             \
        }                                               \
    } while (0)
    CONN_COPY_SPAN(ctx->request.target, src->target);
    CONN_COPY_SPAN(ctx->request.path, src->path);
    CONN_COPY_SPAN(ctx->request.query, src->query);
    CONN_COPY_SPAN(ctx->request.peer_ip, src->peer_ip);
#undef CONN_COPY_SPAN
    for (size_t i = 0; i < src->header_count; i++) {
        cf_span name = {ctx->req_strings + off, src->headers[i].name.len};
        if (name.len != 0) {
            memcpy(ctx->req_strings + off, src->headers[i].name.ptr, name.len);
            off += name.len;
        }
        cf_span value = {ctx->req_strings + off, src->headers[i].value.len};
        if (value.len != 0) {
            memcpy(ctx->req_strings + off, src->headers[i].value.ptr,
                   value.len);
            off += value.len;
        }
        ctx->req_headers[i].name = name;
        ctx->req_headers[i].value = value;
    }
    ctx->request.headers = ctx->req_headers;
    return CF_OK;
}

/* P12-02b measurement: where the model work ran. */
static void cable_note_work(cf_cable *cable, bool subscribe, uintptr_t thread,
                            bool on_worker, uintptr_t owner_thread,
                            bool owner_has_reader) {
    cf_cable_work_identity ident = {
        .ran = true,
        .on_worker = on_worker,
        .thread = thread,
        .owner_thread = owner_thread,
        .owner_has_reader = owner_has_reader,
    };
    pthread_mutex_lock(&cable->mutex);
    if (subscribe) {
        cable->ident_subscribe = ident;
    } else {
        cable->ident_auth = ident;
    }
    pthread_mutex_unlock(&cable->mutex);
}

void cf_cable_test_work_identity(const cf_cable *cable, bool subscribe,
                                 cf_cable_work_identity *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof *out);
    if (cable == NULL) return;
    cf_cable *mutable_cable = (cf_cable *)cable;
    pthread_mutex_lock(&mutable_cable->mutex);
    *out = subscribe ? cable->ident_subscribe : cable->ident_auth;
    pthread_mutex_unlock(&mutable_cable->mutex);
}

void cf_cable_test_reset_work_identity(cf_cable *cable) {
    if (cable == NULL) return;
    pthread_mutex_lock(&cable->mutex);
    memset(&cable->ident_auth, 0, sizeof cable->ident_auth);
    memset(&cable->ident_subscribe, 0, sizeof cable->ident_subscribe);
    pthread_mutex_unlock(&cable->mutex);
}

/* Worker: resolve the connection identity with this worker's reader. */
static void conn_auth_work(void *user) {
    cf_cable_conn *ctx = user;
    cf_db *reader = cf_app_worker_reader();
    bool on_worker = reader != NULL;
    bool authenticated = false;
    int64_t uid = 0;
    char name[256];
    memset(name, 0, sizeof name);
    cf_err rc;
    if (reader == NULL) {
        rc = CF_IO;
    } else {
        rc = cable_read_identity(reader, ctx->cable, &ctx->request,
                                 &authenticated, &uid, name, sizeof name);
    }
    pthread_mutex_lock(&ctx->mutex);
    ctx->auth_rc = rc;
    ctx->auth_ok = rc == CF_OK && authenticated;
    ctx->auth_user_id = uid;
    memcpy(ctx->auth_name, name, sizeof ctx->auth_name);
    ctx->auth_phase = CONN_AUTH_READY;
    conn_wake_locked(ctx);
    pthread_mutex_unlock(&ctx->mutex);
    cable_note_work(ctx->cable, false, (uintptr_t)pthread_self(), on_worker,
                    ctx->submit_thread, ctx->submit_owner_reader);
    conn_unref(ctx);
}

/* Owner: submit (or resubmit) the connection authentication. The ticket is
 * captured here, before the worker's database read (C03). */
static cf_err conn_submit_auth(cf_cable_conn *ctx) {
    ctx->ticket = cf_cable_auth_capture(ctx->cable->app);
    ctx->submit_thread = (uintptr_t)pthread_self();
    ctx->submit_owner_reader = ctx->loop != NULL && ctx->loop->reader != NULL;
    pthread_mutex_lock(&ctx->mutex);
    ctx->auth_phase = CONN_AUTH_QUEUED;
    pthread_mutex_unlock(&ctx->mutex);
    conn_ref(ctx);
    cf_err rc = cf_app_submit_worker(ctx->cable->app, conn_auth_work, ctx);
    if (rc != CF_OK) {
        /* Bounded-pool overload (or stopping): the connection is refused. */
        conn_unref(ctx);
        pthread_mutex_lock(&ctx->mutex);
        ctx->auth_phase = CONN_AUTH_REFUSED;
        pthread_mutex_unlock(&ctx->mutex);
    }
    return rc;
}

/* Owner: attach the connection's loop and install the worker's identity
 * through the C03 member gate. Returns CF_BUSY when a bounded resubmission
 * was queued, otherwise CF_OK or a refusal. */
static cf_err conn_install_auth(cf_cable_conn *ctx, cf_cable_socket *socket,
                                bool ok, int64_t uid, const char *name) {
    if (!ok) return CF_NOT_FOUND;
    if (ctx->loop == NULL) {
        cf_cable_revocation *slot = cable_slot_for(
            ctx->cable, cf_cable_socket_transport_token(socket));
        if (slot == NULL) {
            /* No owner-loop slot: fail closed rather than serve a connection
             * a committed revocation cannot reach (revocation.h). */
            return CF_INTERNAL;
        }
        ctx->user_id = uid;
        size_t n = strlen(name);
        if (n >= sizeof ctx->user_name) n = sizeof ctx->user_name - 1;
        memcpy(ctx->user_name, name, n);
        ctx->user_name[n] = '\0';
        cf_err rc = cf_cable_loop_attach_socket(
            ctx->cable, socket, uid, ctx->user_name, NULL, true, slot,
            &ctx->loop);
        if (rc != CF_OK) return rc;
        ctx->member = cf_cable_loop_revocation_member(ctx->loop);
    }
    bool resubmit = false;
    cf_err gate = cf_cable_revocation_member_install(ctx->member, ctx->ticket,
                                                     &resubmit);
    if (gate == CF_BUSY && resubmit && conn_submit_auth(ctx) == CF_OK) {
        return CF_BUSY; /* the loop stays attached for the fresh result */
    }
    if (gate != CF_OK) {
        if (ctx->loop != NULL) {
            cf_cable_loop_detach(ctx->loop);
            ctx->loop = NULL;
            ctx->member = NULL;
        }
        return CF_FORBIDDEN;
    }
    ctx->have_identity = true;
    return CF_OK;
}

/* Worker: validate a subscribe (or message/subscribed) command against this
 * worker's reader. Only the copied identifier, the static class name, the
 * retained cable secret and the plan in the context are touched: never the
 * loop, socket or subscription (a concurrent revocation may free them). */
static void conn_validate_work(void *user) {
    cf_cable_conn *ctx = user;
    cf_db *reader = cf_app_worker_reader();
    bool on_worker = reader != NULL;
    cf_err rc;
    if (reader == NULL) {
        rc = CF_IO;
    } else {
        rc = cf_cable_subscribe_validate(
            ctx->cmd_class, reader, ctx->user_id,
            (cf_span){(const unsigned char *)ctx->cmd_identifier,
                      ctx->cmd_identifier_len},
            cf_cable_secret(ctx->cable), &ctx->plan);
    }
    pthread_mutex_lock(&ctx->mutex);
    ctx->cmd_rc = rc;
    ctx->cmd_phase = CMD_VALIDATED;
    conn_wake_locked(ctx);
    pthread_mutex_unlock(&ctx->mutex);
    cable_note_work(ctx->cable, true, (uintptr_t)pthread_self(), on_worker,
                    ctx->submit_thread, ctx->submit_owner_reader);
    conn_unref(ctx);
}

/* Worker: the deferred model effect (PresenceChannel's present write and the
 * read broadcast). Values only; cf_write/cf_cable_publish are thread-safe. */
static void conn_effect_work(void *user) {
    cf_cable_conn *ctx = user;
    bool on_worker = cf_app_worker_reader() != NULL;
    cf_err rc = cf_cable_subscribe_presence_effect(
        ctx->cable->app, ctx->cable, ctx->user_id, ctx->plan.room_id);
    pthread_mutex_lock(&ctx->mutex);
    ctx->cmd_rc = rc;
    ctx->cmd_phase = CMD_EFFECTS_DONE;
    conn_wake_locked(ctx);
    pthread_mutex_unlock(&ctx->mutex);
    cable_note_work(ctx->cable, true, (uintptr_t)pthread_self(), on_worker,
                    ctx->submit_thread, ctx->submit_owner_reader);
    conn_unref(ctx);
}

static cf_err conn_submit_validate(cf_cable_conn *ctx) {
    ctx->submit_thread = (uintptr_t)pthread_self();
    ctx->submit_owner_reader = ctx->loop != NULL && ctx->loop->reader != NULL;
    pthread_mutex_lock(&ctx->mutex);
    ctx->cmd_phase = CMD_VALIDATE_QUEUED;
    pthread_mutex_unlock(&ctx->mutex);
    conn_ref(ctx);
    cf_err rc = cf_app_submit_worker(ctx->cable->app, conn_validate_work, ctx);
    if (rc != CF_OK) conn_unref(ctx);
    return rc;
}

static cf_err conn_submit_effect(cf_cable_conn *ctx) {
    ctx->submit_thread = (uintptr_t)pthread_self();
    ctx->submit_owner_reader = ctx->loop != NULL && ctx->loop->reader != NULL;
    pthread_mutex_lock(&ctx->mutex);
    ctx->cmd_phase = CMD_EFFECTS_QUEUED;
    pthread_mutex_unlock(&ctx->mutex);
    conn_ref(ctx);
    cf_err rc = cf_app_submit_worker(ctx->cable->app, conn_effect_work, ctx);
    if (rc != CF_OK) conn_unref(ctx);
    return rc;
}

/* Owner: forget the finished command's worker-owned state. */
static void conn_clear_command(cf_cable_conn *ctx) {
    pthread_mutex_lock(&ctx->mutex);
    ctx->cmd_phase = CMD_NONE;
    ctx->cmd_subscribe = false;
    ctx->cmd_sub = NULL;
    ctx->cmd_class = NULL;
    ctx->cmd_rc = CF_OK;
    pthread_mutex_unlock(&ctx->mutex);
    free(ctx->cmd_identifier);
    ctx->cmd_identifier = NULL;
    ctx->cmd_identifier_len = 0;
    cf_cable_sub_plan_dispose(&ctx->plan);
}

static void conn_finish_command(cf_cable_conn *ctx, cf_cable_socket *socket,
                                bool refused, cf_err rc, bool already_logged) {
    cf_cable_loop *loop = ctx->loop;
    if (loop != NULL) {
        if (ctx->cmd_subscribe && ctx->cmd_sub != NULL) {
            cf_cable_subscribe_reply(loop, ctx->cmd_sub, refused, rc);
        } else if (!already_logged && !refused && rc != CF_OK) {
            cf_cable_log("could not execute command", "perform");
        }
        pthread_mutex_lock(&loop->mutex);
        loop->handling = false;
        loop_flush_locked(loop);
        pthread_mutex_unlock(&loop->mutex);
    }
    conn_clear_command(ctx);
    if (socket != NULL) cf_cable_socket_resume(socket);
}

/* Owner: discard a command whose connection was revoked or detached; no
 * subscription state can be touched (a revoke may already have freed it). */
static void conn_discard_command(cf_cable_conn *ctx, cf_cable_socket *socket) {
    cf_cable_loop *loop = ctx->loop;
    if (loop != NULL) {
        pthread_mutex_lock(&loop->mutex);
        loop->handling = false;
        loop_flush_locked(loop);
        pthread_mutex_unlock(&loop->mutex);
    }
    conn_clear_command(ctx);
    if (socket != NULL) cf_cable_socket_resume(socket);
}

/* Owner: start a command whose validation needs model reads. The subscription
 * shell (subscribe) or target (message) is resolved here; the worker gets
 * only copies. Returns CF_BUSY so the socket pauses parsing until the
 * completion reply (ordering), CF_OK when the command was ignored/dropped. */
static cf_err conn_begin_command(cf_cable_conn *ctx, cf_span text,
                                 cf_cable_command_kind kind) {
    /* A revoking/revoked connection accepts no new command (its subscription
     * array may already be gone). */
    if (ctx->member != NULL &&
        cf_cable_revocation_member_revoked(ctx->member)) {
        return CF_OK;
    }
    cf_cable_loop *loop = ctx->loop;
    cf_cable_subscription *sub = NULL;
    const char *class_name = NULL;
    bool respond = false;
    if (kind == CF_CABLE_CMD_SUBSCRIBE) {
        cf_err rc = cf_cable_subscribe_begin(loop, text, &sub);
        if (rc != CF_OK || sub == NULL) return CF_OK; /* ignored */
        class_name = sub->class_name;
        respond = true;
    } else {
        sub = cf_cable_message_subscription(loop, text);
        if (sub == NULL) {
            cf_cable_log("unable to find subscription", "identifier");
            return CF_OK;
        }
        if (!cf_cable_message_subscribed_handled(sub)) {
            cf_cable_log("unable to process", "subscribed");
            return CF_OK;
        }
        class_name = sub->class_name;
    }
    size_t identifier_len = sub->identifier_len;
    char *identifier = malloc(identifier_len + 1);
    if (identifier == NULL) {
        if (respond) {
            cf_cable_subscribe_reply(loop, sub, true, CF_OK);
        } else {
            cf_cable_log("could not execute command", "perform");
        }
        return CF_OK;
    }
    memcpy(identifier, sub->identifier, identifier_len);
    identifier[identifier_len] = '\0';

    ctx->cmd_sub = sub;
    ctx->cmd_class = class_name;
    ctx->cmd_identifier = identifier;
    ctx->cmd_identifier_len = identifier_len;
    ctx->cmd_subscribe = respond;
    ctx->cmd_ticket = cf_cable_auth_capture(ctx->cable->app);

    /* The reference flushes a command's broadcasts after its reply: holding
     * delivery until the worker result is installed keeps the confirmation
     * ahead of anything the command triggers. */
    pthread_mutex_lock(&loop->mutex);
    loop->handling = true;
    pthread_mutex_unlock(&loop->mutex);

    if (conn_submit_validate(ctx) != CF_OK) {
        /* The bounded worker queue is full or stopping: C02/C03 reject the
         * subscription (or drop the command) instead of confirming early. */
        pthread_mutex_lock(&loop->mutex);
        loop->handling = false;
        loop_flush_locked(loop);
        pthread_mutex_unlock(&loop->mutex);
        if (respond) {
            cf_cable_subscribe_reply(loop, sub, true, CF_OK);
        } else {
            cf_cable_log("dropped command", "worker queue full");
        }
        conn_clear_command(ctx);
        return CF_OK;
    }
    return CF_BUSY;
}

/* Owner: install a validated plan, run the deferred effect on a worker when
 * there is one, then reply. */
static void conn_finish_validate(cf_cable_conn *ctx, cf_cable_socket *socket) {
    cf_err rc = ctx->cmd_rc;
    bool rejected = ctx->plan.rejected;
    bool refused = false;
    if (ctx->loop == NULL) {
        conn_discard_command(ctx, socket);
        return;
    }
    if (ctx->member != NULL &&
        cf_cable_revocation_member_revoked(ctx->member)) {
        conn_discard_command(ctx, socket);
        return;
    }
    if (rc == CF_OK && !rejected && ctx->member != NULL) {
        bool resubmit = false;
        cf_err gate = cf_cable_revocation_member_install(
            ctx->member, ctx->cmd_ticket, &resubmit);
        if (gate == CF_BUSY && resubmit) {
            /* One bounded resubmission: fresh ticket before the fresh read;
             * nothing was applied yet. A full worker queue rejects instead. */
            ctx->cmd_ticket = cf_cable_auth_capture(ctx->cable->app);
            if (conn_submit_validate(ctx) == CF_OK) return;
        }
        if (gate != CF_OK) refused = true;
    }
    if (rc == CF_OK && !refused) {
        cf_err arc = cf_cable_subscribe_apply(ctx->cmd_sub, &ctx->plan);
        if (arc != CF_OK) rc = arc;
    }
    if (rc == CF_OK && !refused && !rejected && ctx->plan.presence_present) {
        if (conn_submit_effect(ctx) == CF_OK) return;
        /* The bounded pool could not take the model effect: the C02/C03
         * overload arm rejects the subscription rather than confirming
         * before its effects. */
        if (ctx->cmd_subscribe) {
            refused = true;
        } else {
            cf_cable_log("could not execute command", "perform");
            conn_finish_command(ctx, socket, false, CF_OK, true);
            return;
        }
    }
    conn_finish_command(ctx, socket, refused, rc, false);
}

static void conn_service_auth(cf_cable_conn *ctx, cf_cable_socket *socket) {
    for (;;) {
        pthread_mutex_lock(&ctx->mutex);
        conn_auth_phase phase = ctx->auth_phase;
        bool ok = ctx->auth_ok;
        int64_t uid = ctx->auth_user_id;
        char name[256];
        memcpy(name, ctx->auth_name, sizeof name);
        bool closed = ctx->closed;
        if (phase == CONN_AUTH_READY) {
            ctx->auth_phase = CONN_AUTH_INSTALLING;
        } else if (phase == CONN_AUTH_REFUSED) {
            ctx->auth_phase = CONN_AUTH_DONE;
        }
        pthread_mutex_unlock(&ctx->mutex);
        if (closed) return;
        if (phase == CONN_AUTH_READY) {
            cf_err irc = conn_install_auth(ctx, socket, ok, uid, name);
            if (irc == CF_BUSY) return; /* resubmission queued */
            pthread_mutex_lock(&ctx->mutex);
            ctx->auth_phase = CONN_AUTH_DONE;
            pthread_mutex_unlock(&ctx->mutex);
            if (irc == CF_OK) {
                cf_cable_socket_complete_auth(socket, true, ctx->user_id);
            } else {
                ctx->refused = true;
                cf_cable_socket_complete_auth(socket, false, 0);
            }
            return;
        }
        if (phase == CONN_AUTH_REFUSED) {
            ctx->refused = true;
            cf_cable_socket_complete_auth(socket, false, 0);
            return;
        }
        return; /* NONE/QUEUED/INSTALLING/DONE: nothing to do */
    }
}

static void conn_service_commands(cf_cable_conn *ctx, cf_cable_socket *socket) {
    for (;;) {
        pthread_mutex_lock(&ctx->mutex);
        conn_cmd_phase phase = ctx->cmd_phase;
        bool closed = ctx->closed;
        if (phase == CMD_VALIDATED) ctx->cmd_phase = CMD_GATING;
        pthread_mutex_unlock(&ctx->mutex);
        if (closed) return;
        if (phase == CMD_VALIDATED) {
            conn_finish_validate(ctx, socket);
            continue; /* a resubmission/effect may already have completed */
        }
        if (phase == CMD_EFFECTS_DONE) {
            /* A revocation may have freed the subscription while the effect
             * ran on a worker: never reply to a revoked connection. */
            if (ctx->member != NULL &&
                cf_cable_revocation_member_revoked(ctx->member)) {
                conn_discard_command(ctx, socket);
            } else {
                conn_finish_command(ctx, socket, false, ctx->cmd_rc, false);
            }
        }
        return;
    }
}

static cf_err cf_cable_authenticate(void *user, cf_cable_socket *socket,
                                    const cf_cable_request *request,
                                    bool *authenticated, int64_t *user_id) {
    if (user == NULL || socket == NULL || request == NULL ||
        authenticated == NULL || user_id == NULL) {
        return CF_INVALID;
    }
    cf_cable *cable = user;
    *authenticated = false;
    *user_id = 0;
    cf_cable_conn *ctx = calloc(1, sizeof *ctx);
    if (ctx == NULL) return CF_NOMEM;
    if (pthread_mutex_init(&ctx->mutex, NULL) != 0) {
        free(ctx);
        return CF_INTERNAL;
    }
    ctx->cable = cable_retain(cable);
    ctx->socket = socket;
    ctx->refs = 1;
    ctx->ticket = cf_cable_auth_capture(cable->app);
    ctx->async = cf_cable_socket_transport_token(socket) != NULL;
    cf_cable_socket_set_app_user(socket, ctx);

    if (!ctx->async) {
        /* Standalone/direct socket (tests, direct callers): the owner thread
         * resolves the identity with its own reader, P12-02 behavior. */
        cf_err rc = cf_db_open(cable->db_path, true, &ctx->reader);
        if (rc != CF_OK) return rc;
        ctx->reader_owned = true;
        ctx->borrowed_request = request;
        rc = cable_read_identity(ctx->reader, cable, request, authenticated,
                                 user_id, ctx->user_name,
                                 sizeof ctx->user_name);
        if (rc == CF_OK && *authenticated) {
            ctx->user_id = *user_id;
            ctx->have_identity = true;
        }
        return rc;
    }

    /* Production reactor: the model work runs on the app's bounded worker
     * pool; the owner installs the result through the C03 gate. */
    cf_err rc = conn_copy_request(ctx, request);
    if (rc != CF_OK) {
        ctx->auth_phase = CONN_AUTH_REFUSED;
        return CF_BUSY;
    }
    (void)conn_submit_auth(ctx); /* QUEUED, or REFUSED when the pool is full */
    return CF_BUSY;
}

/* C01 on_open (owner thread, before the welcome frame): the synchronous
 * (standalone/test) path attaches the connection loop and installs the
 * identity's authorization result. A stale version re-runs the identity reads
 * once (fresh DB auth, bounded); a revoked or still-stale result refuses the
 * connection, never a pretend success. The production reactor path installs
 * from the service hook instead (see conn_install_auth). */
static cf_err cf_cable_open(void *user, cf_cable_socket *socket) {
    if (user == NULL || socket == NULL) return CF_INVALID;
    cf_cable *cable = user;
    cf_cable_conn *ctx = cf_cable_socket_app_user(socket);
    if (ctx == NULL || ctx->async || !ctx->have_identity ||
        ctx->cable != cable) {
        return CF_OK;
    }
    if (ctx->loop != NULL) {
        cf_cable_loop_detach(ctx->loop);
        ctx->loop = NULL;
    }
    cf_err rc = cf_cable_loop_attach_socket(
        cable, socket, ctx->user_id, ctx->user_name, ctx->reader, false, NULL,
        &ctx->loop);
    if (rc != CF_OK) return rc;

    bool resubmit = false;
    rc = cf_cable_auth_install(ctx->loop->revocation, ctx->ticket, &resubmit);
    if (rc == CF_BUSY && resubmit && ctx->borrowed_request != NULL) {
        bool authenticated = false;
        int64_t user_id = 0;
        ctx->ticket = cf_cable_auth_capture(cable->app);
        cf_err fresh =
            cable_read_identity(ctx->reader, cable, ctx->borrowed_request,
                                &authenticated, &user_id, ctx->user_name,
                                sizeof ctx->user_name);
        if (fresh == CF_OK && authenticated) {
            ctx->user_id = user_id;
            ctx->have_identity = true;
            rc = cf_cable_auth_install(ctx->loop->revocation, ctx->ticket,
                                       &resubmit);
        } else {
            rc = fresh != CF_OK ? fresh : CF_NOT_FOUND;
        }
    }
    if (rc != CF_OK) {
        cf_cable_loop_detach(ctx->loop);
        ctx->loop = NULL;
        ctx->refused = true;
        return rc;
    }
    return CF_OK;
}

/* C01 service hook (owner thread, each pass): install finished worker results
 * (authentication, subscribe validation/effects) through the C03 gate, and
 * consume any pending revocation control for this socket's owner loop. */
static void cf_cable_service(void *user, cf_cable_socket *socket) {
    if (user == NULL || socket == NULL) return;
    cf_cable *cable = user;
    cf_cable_conn *ctx = cf_cable_socket_app_user(socket);
    if (ctx == NULL || ctx->cable != cable) return;
    /* C03 control first: a consumed control marks the loop's matching members
     * revoked, so anything installed afterwards is refused by the gate. The
     * production reactor loop has one shared slot per owner; a standalone
     * socket services its loop's legacy slot. */
    cf_cable_revocation *slot =
        cable_slot_for(cable, cf_cable_socket_transport_token(socket));
    if (slot == NULL && ctx->loop != NULL) slot = ctx->loop->revocation;
    if (slot != NULL) (void)cf_cable_revocation_service(slot);
    conn_service_auth(ctx, socket);
    conn_service_commands(ctx, socket);
}

/* C01 on_close (owner thread, before the socket releases itself): detach the
 * loop so its slot/member is unregistered and no foreign barrier or publisher
 * can touch the socket after it is freed, then release the connection
 * context. Worker jobs hold their own references; the last one frees. */
static void cf_cable_close(void *user, cf_cable_socket *socket) {
    if (user == NULL || socket == NULL) return;
    cf_cable_conn *ctx = cf_cable_socket_app_user(socket);
    if (ctx == NULL) return;
    /* Claim the loop under the mutex before dropping the owner reference: a
     * worker may hold the last reference and free the context as soon as it
     * observes `closed`, so no field may be read outside the lock afterwards
     * (detach only needs the claimed pointer). */
    pthread_mutex_lock(&ctx->mutex);
    ctx->closed = true;
    ctx->socket = NULL;
    cf_cable_loop *loop = ctx->loop;
    ctx->loop = NULL;
    ctx->member = NULL;
    ctx->refs--;
    bool last = ctx->refs == 0;
    pthread_mutex_unlock(&ctx->mutex);
    if (loop != NULL) cf_cable_loop_detach(loop);
    cf_cable_socket_set_app_user(socket, NULL);
    if (last) conn_free(ctx);
}

static cf_err cf_cable_on_text(void *user, cf_cable_socket *socket,
                               cf_span text) {
    if (user == NULL || socket == NULL) return CF_INVALID;
    cf_cable *cable = user;
    cf_cable_conn *ctx = cf_cable_socket_app_user(socket);
    if (ctx == NULL) return CF_OK;
    /* The connection install gate refused this connection: it is closing. */
    if (ctx->refused) return CF_OK;

    if (ctx->async) {
        if (ctx->loop == NULL) return CF_OK; /* refused/closing: no dispatch */
        cf_cable_command_kind kind = cf_cable_command_kind_of(text);
        if (kind == CF_CABLE_CMD_OTHER) {
            /* No model read: dispatch synchronously on the owner thread. */
            return cf_cable_loop_handle_text(ctx->loop, text);
        }
        return conn_begin_command(ctx, text, kind);
    }

    if (ctx->loop == NULL) {
        if (!ctx->have_identity || ctx->cable != cable) {
            /* No authenticated identity for this connection: C01 closes it;
             * anything the reader assembled before that is ignored. */
            return CF_OK;
        }
        cf_err rc = cf_cable_loop_attach_socket(
            cable, socket, ctx->user_id, ctx->user_name, ctx->reader, false,
            NULL, &ctx->loop);
        if (rc != CF_OK) return rc;
    }
    return cf_cable_loop_handle_text(ctx->loop, text);
}

void cf_cable_server_hooks(cf_cable *cable, cf_cable_hooks *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof *out);
    out->authenticate = cf_cable_authenticate;
    out->authenticate_user = cable;
    out->on_open = cf_cable_open;
    out->on_open_user = cable;
    out->on_text = cf_cable_on_text;
    out->on_text_user = cable;
    out->service = cf_cable_service;
    out->service_user = cable;
    out->on_close = cf_cable_close;
    out->on_close_user = cable;
    out->owner_start = cable_owner_start;
    out->owner_start_user = cable;
    out->owner_stop = cable_owner_stop;
    out->owner_stop_user = cable;
}

/* ---- dispatch wrapper (keeps confirmation-before-broadcast ordering) ---------- */

/* A loop is revoked through its shared-slot member (production reactor) or
 * its legacy per-loop slot (standalone/test). */
static bool loop_revoked(const cf_cable_loop *loop) {
    if (loop == NULL) return false;
    if (loop->member != NULL) {
        return cf_cable_revocation_member_revoked(loop->member);
    }
    return cf_cable_revocation_revoked(loop->revocation);
}

cf_err cf_cable_loop_handle_text(cf_cable_loop *loop, cf_span text) {
    if (loop == NULL) return CF_INVALID;
    /* A revoking/revoked loop ignores every command (C03: no access may be
     * installed after the acknowledgment, and no new output is accepted). */
    if (loop_revoked(loop)) return CF_OK;
    pthread_mutex_lock(&loop->mutex);
    loop->handling = true;
    pthread_mutex_unlock(&loop->mutex);
    cf_err rc = cf_cable_channels_dispatch(loop, text);
    pthread_mutex_lock(&loop->mutex);
    loop->handling = false;
    loop_flush_locked(loop);
    pthread_mutex_unlock(&loop->mutex);
    return rc;
}

/* ---- test support ------------------------------------------------------------- */

cf_err cf_cable_test_attach(cf_cable *cable, int64_t user_id,
                            const char *user_name, cf_cable_loop_send_fn send,
                            void *send_user, cf_cable_loop **out) {
    /* Test loops open their own reader, register no control slot, and stay on
     * the synchronous dispatch path. */
    return loop_open(cable, user_id, user_name, send, send_user, NULL, true,
                     false, NULL, out);
}

cf_err cf_cable_test_feed(cf_cable_loop *loop, cf_span text) {
    return cf_cable_loop_handle_text(loop, text);
}

void cf_cable_test_detach(cf_cable_loop *loop) {
    cf_cable_loop_detach(loop);
}

void cf_cable_test_hold_delivery(cf_cable_loop *loop, bool hold) {
    if (loop == NULL) return;
    pthread_mutex_lock(&loop->mutex);
    loop->test_hold = hold;
    if (!hold) loop_flush_locked(loop);
    pthread_mutex_unlock(&loop->mutex);
}
