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
 * Bounds per loop: CF_CABLE_LOOP_MAX_COMMANDS pending commands and
 * CF_CABLE_LOOP_MAX_BYTES of frame references (each command counts its frame
 * length once). On overflow the broadcast is dropped, the loop and cable
 * dropped-broadcast counters increment, a sanitized stream-class line is
 * logged and CF_BUSY is returned; callers record the post-commit delivery
 * failure, never a pretend rollback. A frame that a socket refuses is
 * dropped and counted the same way. There is no unbounded pubsub bus. */
#include "cable/channels.h"

#include "app.h"
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
    cf_db *reader;         /* owned; opened on the attaching (owner) thread */
    bool handling;         /* owner thread is inside handle_text */
    bool test_hold;        /* test-only: defer flushing (never in production) */
    cf_cable_subscription *subs;
    size_t sub_count, sub_cap;
    cf_cable_cmd *pending_head, *pending_tail;
    size_t pending_count, pending_bytes;
    uint64_t dropped;

    /* C03 production wiring. `revocation` is the preallocated control slot
     * (NULL for a test loop); `revoked` stops new application output and is
     * only read/written under the loop mutex. A revoke moves the whole
     * subscription array to `deferred` (O(1), no allocation on the barrier
     * path) so the presence effects run after the acknowledgment. */
    cf_cable_revocation *revocation;
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
    size_t refs; /* one per loop plus the creator */
    bool stopping;
    uint64_t published, delivered, dropped;
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
    *out = cable;
    return CF_OK;
}

/* The cable lock is held; frees the object when the last reference goes. */
static void cable_release_locked(cf_cable *cable) {
    if (cable->refs > 0) cable->refs--;
    if (cable->refs != 0) {
        pthread_mutex_unlock(&cable->mutex);
        return;
    }
    pthread_mutex_unlock(&cable->mutex);
    pthread_cond_destroy(&cable->idle_cv);
    pthread_mutex_destroy(&cable->mutex);
    free(cable->db_path);
    free(cable->secret);
    free(cable);
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
                        bool with_revocation, cf_cable_loop **out) {
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
    /* The owner thread reads through its own connection (02: a reader belongs
     * to the thread that opened it). */
    rc = cf_db_open(cable->db_path, true, &loop->reader);
    if (rc != CF_OK) {
        pthread_mutex_destroy(&loop->mutex);
        free(loop->user_name);
        free(loop->internal_stream);
        free(loop);
        return rc;
    }

    /* Register the control slot before the loop is linked into the cable, so
     * a barrier can never see an attached loop without its slot (C03). Test
     * loops stay unregistered and drive their own slots. */
    if (with_revocation) {
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
            cf_db_close(loop->reader);
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
        /* The slot is already registered; remove it before the loop memory
         * goes away so no barrier can wake a freed loop. */
        if (loop->revocation != NULL) {
            cf_cable_revocation_unregister(loop->revocation);
            loop->revocation = NULL;
        }
        cf_db_close(loop->reader);
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

    /* Acknowledge any pending barrier control and remove the slot before the
     * loop memory is released; the owner thread unregisters after its last
     * service call. */
    if (loop->revocation != NULL) {
        cf_cable_revocation_unregister(loop->revocation);
        loop->revocation = NULL;
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

    if (loop->reader != NULL) cf_db_close(loop->reader);
    pthread_mutex_destroy(&loop->mutex);
    free(loop->user_name);
    free(loop->internal_stream);
    free(loop);

    pthread_mutex_lock(&cable->mutex);
    cable_release_locked(cable);
}

cf_err cf_cable_loop_attach_socket(cf_cable *cable, cf_cable_socket *socket,
                                   int64_t user_id, const char *user_name,
                                   cf_cable_loop **out) {
    return loop_open(cable, user_id, user_name, loop_send_socket, socket, true,
                     out);
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

/* One connection thread's identity and reader. Authenticate runs on the
 * connection owner thread before the welcome frame; on_text runs on the same
 * thread. The destructor detaches the loop (running unsubscribe effects,
 * closing the loop reader) and closes the authentication reader when the
 * thread exits, which is where C01's connection thread ends. */
typedef struct {
    cf_db *reader; /* per-thread authentication reader */
    cf_cable_loop *loop;
    cf_cable_socket *socket;
    cf_cable *cable;
    const cf_cable_request *request; /* valid for the connection's run */
    cf_cable_auth_ticket ticket;     /* captured before the identity reads */
    bool have_identity;
    bool refused; /* the connection install gate refused this connection */
    int64_t user_id;
    char user_name[256];
} cf_cable_thread;

static pthread_key_t cable_tls_key;
static pthread_once_t cable_tls_once = PTHREAD_ONCE_INIT;

static void cable_thread_dispose(void *ptr) {
    cf_cable_thread *tls = ptr;
    if (tls->loop != NULL) cf_cable_loop_detach(tls->loop);
    if (tls->reader != NULL) cf_db_close(tls->reader);
    free(tls);
}

static void cable_tls_init(void) {
    (void)pthread_key_create(&cable_tls_key, cable_thread_dispose);
}

static cf_cable_thread *cable_thread_get(void) {
    (void)pthread_once(&cable_tls_once, cable_tls_init);
    cf_cable_thread *tls = pthread_getspecific(cable_tls_key);
    if (tls == NULL) {
        tls = calloc(1, sizeof *tls);
        if (tls == NULL) return NULL;
        if (pthread_setspecific(cable_tls_key, tls) != 0) {
            free(tls);
            return NULL;
        }
    }
    return tls;
}

/* The session/user reads behind one connection identity. Reconnect and the
 * stale-version resubmission both re-run it, so it always reads the database
 * fresh. */
static cf_err cable_thread_read_identity(cf_cable_thread *tls, cf_cable *cable,
                                         const cf_cable_request *request,
                                         bool *authenticated, int64_t *user_id) {
    *authenticated = false;
    *user_id = 0;
    if (tls->reader == NULL) {
        cf_err rc = cf_db_open(cable->db_path, true, &tls->reader);
        if (rc != CF_OK) return rc;
    }
    cf_cable_session_auth auth = {
        .reader = tls->reader,
        .secret_key_base = (cf_span){(const unsigned char *)cable->secret,
                                     cable->secret_len},
    };
    cf_err rc = cf_cable_session_authenticate(&auth, request, authenticated,
                                              user_id);
    if (rc != CF_OK || !*authenticated) return rc;
    cf_user row;
    bool found = false;
    rc = cf_user_find_by_id(tls->reader, *user_id, &found, &row);
    if (rc != CF_OK || !found) {
        if (rc == CF_OK) rc = CF_NOT_FOUND;
        *authenticated = false;
        return rc;
    }
    size_t name_len = row.name.len;
    if (name_len >= sizeof tls->user_name) name_len = sizeof tls->user_name - 1;
    if (name_len != 0) memcpy(tls->user_name, row.name.ptr, name_len);
    tls->user_name[name_len] = '\0';
    cf_user_dispose(&row);
    tls->have_identity = true;
    tls->user_id = *user_id;
    tls->cable = cable;
    return CF_OK;
}

static cf_err cf_cable_authenticate(void *user,
                                    const cf_cable_request *request,
                                    bool *authenticated, int64_t *user_id) {
    if (user == NULL || request == NULL || authenticated == NULL ||
        user_id == NULL) {
        return CF_INVALID;
    }
    cf_cable *cable = user;
    *authenticated = false;
    *user_id = 0;
    cf_cable_thread *tls = cable_thread_get();
    if (tls == NULL) return CF_NOMEM;
    /* A new connection on this thread: the previous loop (if any) is done. */
    if (tls->loop != NULL) {
        cf_cable_loop_detach(tls->loop);
        tls->loop = NULL;
    }
    tls->socket = NULL;
    tls->cable = NULL;
    tls->request = request;
    tls->have_identity = false;
    tls->refused = false;
    /* The data version is captured before the session/user reads; the
     * connection install gate compares it when the loop attaches (C03). */
    tls->ticket = cf_cable_auth_capture(cable->app);
    return cable_thread_read_identity(tls, cable, request, authenticated,
                                      user_id);
}

/* C01 on_open (owner thread, before the welcome frame): attach the connection
 * loop and install the identity's authorization result. A stale version
 * re-runs the identity reads once (fresh DB auth, bounded); a revoked or
 * still-stale result refuses the connection, never a pretend success. */
static cf_err cf_cable_open(void *user, cf_cable_socket *socket) {
    if (user == NULL || socket == NULL) return CF_INVALID;
    cf_cable *cable = user;
    cf_cable_thread *tls = cable_thread_get();
    if (tls == NULL) return CF_NOMEM;
    if (!tls->have_identity || tls->cable != cable) return CF_OK;
    if (tls->loop != NULL) {
        cf_cable_loop_detach(tls->loop);
        tls->loop = NULL;
    }
    tls->socket = socket;
    cf_err rc = cf_cable_loop_attach_socket(cable, socket, tls->user_id,
                                            tls->user_name, &tls->loop);
    if (rc != CF_OK) return rc;

    bool resubmit = false;
    rc = cf_cable_auth_install(tls->loop->revocation, tls->ticket, &resubmit);
    if (rc == CF_BUSY && resubmit && tls->request != NULL) {
        bool authenticated = false;
        int64_t user_id = 0;
        tls->ticket = cf_cable_auth_capture(cable->app);
        cf_err fresh = cable_thread_read_identity(tls, cable, tls->request,
                                                  &authenticated, &user_id);
        if (fresh == CF_OK && authenticated) {
            rc = cf_cable_auth_install(tls->loop->revocation, tls->ticket,
                                       &resubmit);
        } else {
            rc = fresh != CF_OK ? fresh : CF_NOT_FOUND;
        }
    }
    if (rc != CF_OK) {
        cf_cable_loop_detach(tls->loop);
        tls->loop = NULL;
        tls->refused = true;
        return rc;
    }
    return CF_OK;
}

/* C01 service hook (owner thread, each poll iteration): consume any pending
 * revocation control for this connection's loop. */
static void cf_cable_service(void *user) {
    if (user == NULL) return;
    cf_cable *cable = user;
    cf_cable_thread *tls = cable_thread_get();
    if (tls == NULL || tls->loop == NULL || tls->cable != cable) return;
    (void)cf_cable_revocation_service(tls->loop->revocation);
}

/* C01 on_close (owner thread, before the socket releases itself): detach the
 * loop so its slot is unregistered and no foreign barrier or publisher can
 * touch the socket after it is freed. */
static void cf_cable_close(void *user, cf_cable_socket *socket) {
    (void)socket;
    if (user == NULL) return;
    cf_cable_thread *tls = cable_thread_get();
    if (tls == NULL) return;
    if (tls->loop != NULL) {
        cf_cable_loop_detach(tls->loop);
        tls->loop = NULL;
    }
    tls->socket = NULL;
    tls->cable = NULL;
    tls->have_identity = false;
    tls->request = NULL;
}

static cf_err cf_cable_on_text(void *user, cf_cable_socket *socket,
                               cf_span text) {
    if (user == NULL || socket == NULL) return CF_INVALID;
    cf_cable *cable = user;
    cf_cable_thread *tls = cable_thread_get();
    if (tls == NULL) return CF_NOMEM;
    /* The connection install gate refused this connection: it is closing. */
    if (tls->refused) return CF_OK;
    if (tls->loop == NULL) {
        if (!tls->have_identity || tls->cable != cable) {
            /* No authenticated identity for this connection: C01 closes it;
             * anything the reader assembled before that is ignored. */
            return CF_OK;
        }
        cf_err rc = cf_cable_loop_attach_socket(cable, socket, tls->user_id,
                                                tls->user_name, &tls->loop);
        if (rc != CF_OK) return rc;
    }
    return cf_cable_loop_handle_text(tls->loop, text);
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
}

/* ---- dispatch wrapper (keeps confirmation-before-broadcast ordering) ---------- */

cf_err cf_cable_loop_handle_text(cf_cable_loop *loop, cf_span text) {
    if (loop == NULL) return CF_INVALID;
    /* A revoking/revoked loop ignores every command (C03: no access may be
     * installed after the acknowledgment, and no new output is accepted). */
    if (cf_cable_revocation_revoked(loop->revocation)) return CF_OK;
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
    return loop_open(cable, user_id, user_name, send, send_user, false, out);
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
