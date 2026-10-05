/* tests/cable/test_cable_revocation.c — C03 revocation barrier and stale-auth
 * rejection (04-cable-jobs.md "C03"; acceptance CABLE-03/04, AUTH-05).
 *
 * The owner-side operations of the barrier come from the loop's wiring
 * (revocation.h). Production wiring lives in src/cable/pubsub.c and the C01
 * socket and lands with the integrator; this suite wires real C02 test loops
 * under the same contract: a per-loop owner thread consumes control and
 * revokes through the public channels.h surface, removing subscriptions
 * without running unsubscribe effects (no cf_write before the ack) and
 * running the deferred cleanup after it. Every case drives the barrier and
 * asserts its result; nothing is skipped. */
#include "cable_channels_testutil.h"

#include "app_internal.h"
#include "cable/revocation.h"
#include "db/writer.h"

#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>

/* ---- a wired test loop ------------------------------------------------------ */

typedef struct {
    cf_cable *cable;
    cf_cable_loop *loop;
    cf_cable_revocation *slot;
    int64_t user_id;

    pthread_t thread;
    bool has_thread;
    pthread_mutex_t mutex;
    pthread_cond_t cv;
    bool stop;
    bool service_now;

    int wake_calls;
    int revoke_calls;
    int cleanup_calls;
    int services; /* service calls that returned */
    size_t sink_calls;
    size_t failed_sends;
    int sink_fd; /* >= 0: nonblocking socket sink (nonreading peer) */
    cf_cable_revocation_control last;

    bool frame_partial;
    bool hold_revoke; /* latch inside revoke (pre-ack) */
    bool in_revoke;
    bool hold_cleanup; /* latch inside cleanup (post-ack) */
    bool in_cleanup;
    bool cleanup_done;
} rev_loop;

static cf_err rev_sink(void *user, cf_cable_frame *frame) {
    (void)frame;
    rev_loop *r = user;
    if (r->sink_fd >= 0) {
        /* A socket whose peer never reads: its buffer is full, sends fail. */
        unsigned char byte = 0x1;
        ssize_t n = send(r->sink_fd, &byte, 1, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0) {
            r->failed_sends++;
            return CF_BUSY;
        }
    }
    r->sink_calls++;
    return CF_OK;
}

static void rev_wake(void *user) {
    rev_loop *r = user;
    pthread_mutex_lock(&r->mutex);
    r->wake_calls++;
    r->service_now = true;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mutex);
}

static bool rev_frame_partial(void *user) {
    rev_loop *r = user;
    pthread_mutex_lock(&r->mutex);
    bool partial = r->frame_partial;
    pthread_mutex_unlock(&r->mutex);
    return partial;
}

static void rev_revoke(void *user, const cf_cable_revocation_control *control) {
    rev_loop *r = user;
    pthread_mutex_lock(&r->mutex);
    r->revoke_calls++;
    r->last = *control;
    r->in_revoke = true;
    pthread_cond_broadcast(&r->cv);
    while (r->hold_revoke) pthread_cond_wait(&r->cv, &r->mutex);
    bool attempt = control->attempt_disconnect;
    pthread_mutex_unlock(&r->mutex);

    /* The reference frame precedes the close, and only when no application
     * frame is partially sent; otherwise the loop closes immediately. */
    if (attempt && control->frame_len != 0) {
        (void)cf_cable_loop_send_text(
            r->loop, (cf_span){(const unsigned char *)control->frame,
                               control->frame_len});
    }
    /* Remove every subscription from delivery before acknowledging. No
     * unsubscribe effects here: cf_write would block on the writer that is
     * waiting for this acknowledgment. */
    while (cf_cable_loop_subscription_count(r->loop) > 0) {
        cf_cable_loop_remove_subscription(
            r->loop, cf_cable_loop_subscription_count(r->loop) - 1);
    }
    pthread_mutex_lock(&r->mutex);
    r->in_revoke = false;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mutex);
}

static void rev_cleanup(void *user) {
    rev_loop *r = user;
    pthread_mutex_lock(&r->mutex);
    r->cleanup_calls++;
    r->in_cleanup = true;
    pthread_cond_broadcast(&r->cv);
    while (r->hold_cleanup) pthread_cond_wait(&r->cv, &r->mutex);
    r->in_cleanup = false;
    r->cleanup_done = true;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mutex);
}

static void *rev_owner_main(void *arg) {
    rev_loop *r = arg;
    pthread_mutex_lock(&r->mutex);
    while (!r->stop) {
        if (!r->service_now) {
            pthread_cond_wait(&r->cv, &r->mutex);
            continue;
        }
        r->service_now = false;
        pthread_mutex_unlock(&r->mutex);
        (void)cf_cable_revocation_service(r->slot);
        pthread_mutex_lock(&r->mutex);
        r->services++;
        pthread_cond_broadcast(&r->cv);
    }
    pthread_mutex_unlock(&r->mutex);
    return NULL;
}

/* Wait (bounded) for a predicate under the participant's mutex. */
typedef bool (*rev_pred)(rev_loop *);

static bool rev_wait(rev_loop *r, rev_pred pred, int timeout_ms) {
    int64_t deadline = ct_now_ms() + timeout_ms;
    pthread_mutex_lock(&r->mutex);
    while (!pred(r)) {
        int64_t left = deadline - ct_now_ms();
        if (left <= 0) break;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += (time_t)(left / 1000);
        ts.tv_nsec += (long)((left % 1000) * 1000000);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&r->cv, &r->mutex, &ts);
    }
    bool ok = pred(r);
    pthread_mutex_unlock(&r->mutex);
    return ok;
}

/* The slot acknowledged and its service call returned (revoked recorded). */
static bool rev_pred_serviced(rev_loop *r) { return r->services > 0; }
static bool rev_pred_in_revoke(rev_loop *r) { return r->in_revoke; }
static bool rev_pred_cleanup_done(rev_loop *r) { return r->cleanup_done; }
static bool rev_pred_in_cleanup(rev_loop *r) { return r->in_cleanup; }

/* Attach a real C02 loop and register its preallocated control slot. */
static bool rev_register_loop(chan_fixture *f, int64_t user_id,
                              const char *name, rev_loop *r) {
    memset(r, 0, sizeof *r);
    r->cable = f->cable;
    r->user_id = user_id;
    r->sink_fd = -1;
    pthread_mutex_init(&r->mutex, NULL);
    pthread_cond_init(&r->cv, NULL);
    CF_REQUIRE(cf_cable_test_attach(f->cable, user_id, name, rev_sink, r,
                                    &r->loop) == CF_OK);
    cf_cable_revocation_ops ops;
    memset(&ops, 0, sizeof ops);
    ops.wake = rev_wake;
    ops.frame_partial = rev_frame_partial;
    ops.revoke = rev_revoke;
    ops.cleanup = rev_cleanup;
    ops.user = r;
    CF_REQUIRE(cf_cable_revocation_register(f->cable, f->app, user_id, &ops,
                                            &r->slot) == CF_OK);
    return true;
}

static bool rev_open(chan_fixture *f, int64_t user_id, const char *name,
                     rev_loop *r) {
    CF_REQUIRE(rev_register_loop(f, user_id, name, r));
    CF_REQUIRE(pthread_create(&r->thread, NULL, rev_owner_main, r) == 0);
    r->has_thread = true;
    return true;
}

static void rev_close(rev_loop *r) {
    if (r->has_thread) {
        pthread_mutex_lock(&r->mutex);
        r->stop = true;
        r->hold_revoke = false;
        r->hold_cleanup = false;
        pthread_cond_broadcast(&r->cv);
        pthread_mutex_unlock(&r->mutex);
        pthread_join(r->thread, NULL);
        r->has_thread = false;
    }
    if (r->slot != NULL) {
        cf_cable_revocation_unregister(r->slot);
        r->slot = NULL;
    }
    if (r->loop != NULL) {
        cf_cable_test_detach(r->loop);
        r->loop = NULL;
    }
    if (r->sink_fd >= 0) close(r->sink_fd);
    pthread_cond_destroy(&r->cv);
    pthread_mutex_destroy(&r->mutex);
}

/* A helper thread driving one barrier while the main thread observes it. */
typedef struct {
    cf_cable *cable;
    int64_t user_id;
    bool reconnect;
    cf_err rc;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cv;
    bool done;
} barrier_call;

static void *barrier_thread(void *arg) {
    barrier_call *bc = arg;
    cf_err rc = cf_cable_disconnect_user(bc->cable, bc->user_id,
                                         bc->reconnect);
    pthread_mutex_lock(&bc->mutex);
    bc->rc = rc;
    bc->done = true;
    pthread_cond_broadcast(&bc->cv);
    pthread_mutex_unlock(&bc->mutex);
    return NULL;
}

static void barrier_start(barrier_call *bc, cf_cable *cable, int64_t user_id,
                          bool reconnect) {
    memset(bc, 0, sizeof *bc);
    bc->cable = cable;
    bc->user_id = user_id;
    bc->reconnect = reconnect;
    pthread_mutex_init(&bc->mutex, NULL);
    pthread_cond_init(&bc->cv, NULL);
    CF_REQUIRE(pthread_create(&bc->thread, NULL, barrier_thread, bc) == 0);
}

static bool barrier_done(barrier_call *bc, int timeout_ms) {
    int64_t deadline = ct_now_ms() + timeout_ms;
    pthread_mutex_lock(&bc->mutex);
    while (!bc->done) {
        int64_t left = deadline - ct_now_ms();
        if (left <= 0) break;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += (time_t)(left / 1000);
        ts.tv_nsec += (long)((left % 1000) * 1000000);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&bc->cv, &bc->mutex, &ts);
    }
    bool done = bc->done;
    pthread_mutex_unlock(&bc->mutex);
    return done;
}

static void barrier_finish(barrier_call *bc) {
    pthread_join(bc->thread, NULL);
    pthread_cond_destroy(&bc->cv);
    pthread_mutex_destroy(&bc->mutex);
}

/* ---- helpers ---------------------------------------------------------------- */

static bool rev_subscribe(rev_loop *r, const char *channel, int64_t room_id) {
    char identifier[256];
    char cmd[512];
    chan_room_identifier(identifier, sizeof identifier, channel, room_id);
    char quoted[512];
    chan_json_string(quoted, sizeof quoted, identifier);
    snprintf(cmd, sizeof cmd, "{\"command\":\"subscribe\",\"identifier\":%s}",
             quoted);
    return cf_cable_test_feed(r->loop,
                              (cf_span){(const unsigned char *)cmd,
                                        strlen(cmd)}) == CF_OK;
}

static bool rev_publish(cf_cable *cable, const char *stream,
                        const char *payload) {
    cf_buf *buf = NULL;
    if (cf_buf_copy((cf_span){(const unsigned char *)payload, strlen(payload)},
                    &buf) != CF_OK) {
        return false;
    }
    cf_err rc = cf_cable_publish(
        cable, (cf_span){(const unsigned char *)stream, strlen(stream)}, buf);
    cf_buf_release(buf);
    return rc == CF_OK;
}

static void rev_room_stream(chan_fixture *f, int64_t room_id, char *out,
                            size_t cap) {
    cf_room room = {0};
    bool found = false;
    CF_REQUIRE(cf_room_find_by_id(f->scratch.db, room_id, &found, &room) ==
               CF_OK);
    CF_REQUIRE(found);
    cf_str gid = {0};
    CF_REQUIRE(cf_cable_room_gid_param(&room, &gid) == CF_OK);
    snprintf(out, cap, "room:%.*s", (int)gid.len, gid.ptr);
    cf_str_dispose(&gid);
    cf_room_dispose(&room);
}

typedef struct {
    cf_err rc;
    int64_t user_id;
    bool reconnect;
} event_call;

static cf_err cb_disconnect(cf_tx *tx, void *arg) {
    event_call *e = arg;
    e->rc = cf_tx_event(tx, (cf_event){.kind = CF_EVENT_DISCONNECT_USER,
                                       .user_id = e->user_id,
                                       .reconnect = e->reconnect});
    return e->rc;
}

static cf_err cb_noop(cf_tx *tx, void *arg) {
    (void)tx;
    (void)arg;
    return CF_OK;
}

static cf_err cb_membership_destroy(cf_tx *tx, void *arg) {
    event_call *e = arg;
    cf_membership membership;
    bool found = false;
    cf_err rc = cf_membership_find_by_room_and_user(cf_tx_db(tx), 10, 1,
                                                    &found, &membership);
    if (rc == CF_OK && found) rc = cf_membership_destroy(tx, &membership);
    if (found) cf_membership_dispose(&membership);
    e->rc = rc;
    return rc;
}

static cf_err cb_reset_remote(cf_tx *tx, void *arg) {
    event_call *e = arg;
    cf_user user;
    cf_err rc = cf_user_find(cf_tx_db(tx), 1, &user);
    if (rc == CF_OK) rc = cf_user_reset_remote_connections(tx, &user);
    if (rc == CF_OK) cf_user_dispose(&user);
    e->rc = rc;
    return rc;
}

static cf_err cb_ban(cf_tx *tx, void *arg) {
    event_call *e = arg;
    cf_user user;
    cf_err rc = cf_user_find(cf_tx_db(tx), 1, &user);
    if (rc == CF_OK) rc = cf_user_ban(tx, &user);
    if (rc == CF_OK) cf_user_dispose(&user);
    e->rc = rc;
    return rc;
}

static cf_err cb_deactivate(cf_tx *tx, void *arg) {
    event_call *e = arg;
    cf_user user;
    cf_err rc = cf_user_find(cf_tx_db(tx), 1, &user);
    if (rc == CF_OK) rc = cf_user_deactivate(tx, &user);
    if (rc == CF_OK) cf_user_dispose(&user);
    e->rc = rc;
    return rc;
}

/* The slot's frame must be the exact reference bytes for `reconnect`. */
static bool frame_is(const cf_cable_revocation_control *control,
                     bool reconnect) {
    cf_str reference = {0};
    cf_err rc = cf_cable_disconnect_frame(
        CF_CABLE_REASON_REMOTE,
        (cf_span){(const unsigned char *)(reconnect ? "true" : "false"),
                  reconnect ? 4 : 5},
        &reference);
    if (rc != CF_OK) return false;
    bool same = control->frame_len == reference.len &&
                memcmp(control->frame, reference.ptr, reference.len) == 0;
    cf_str_dispose(&reference);
    return same;
}

static bool register_handler(chan_fixture *f) {
    return cf_writer_set_control_handler(f->app, cf_cable_revocation_handler,
                                         f->cable) == CF_OK;
}

/* ---- the barrier ------------------------------------------------------------ */

CF_TEST(barrier_revokes_every_loop_of_the_user_once_acknowledged) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    CF_REQUIRE(register_handler(&f));

    rev_loop jz1, jz2, kevin;
    CF_REQUIRE(rev_open(&f, CHAN_JZ, "JZ", &jz1));
    CF_REQUIRE(rev_open(&f, CHAN_JZ, "JZ", &jz2));
    CF_REQUIRE(rev_open(&f, CHAN_KEVIN, "Kevin", &kevin));
    CF_REQUIRE(rev_subscribe(&jz1, "RoomChannel", CHAN_DESIGNERS));
    CF_REQUIRE(rev_subscribe(&jz2, "RoomChannel", CHAN_DESIGNERS));
    CF_REQUIRE(rev_subscribe(&kevin, "RoomChannel", CHAN_DESIGNERS));
    jz1.sink_calls = jz2.sink_calls = kevin.sink_calls = 0;

    event_call call = {.user_id = CHAN_JZ, .reconnect = true};
    CF_CHECK(cf_write(f.app, cb_disconnect, &call) == CF_OK);
    CF_CHECK(call.rc == CF_OK);

    CF_REQUIRE(rev_wait(&jz1, rev_pred_serviced, 2000));
    CF_REQUIRE(rev_wait(&jz2, rev_pred_serviced, 2000));
    CF_CHECK(jz1.revoke_calls == 1 && jz2.revoke_calls == 1);
    CF_CHECK(jz1.wake_calls >= 1 && jz2.wake_calls >= 1);
    CF_CHECK(jz1.last.user_id == CHAN_JZ && jz2.last.user_id == CHAN_JZ);
    CF_CHECK(jz1.last.reconnect && jz2.last.reconnect);
    CF_CHECK(jz1.last.attempt_disconnect && jz2.last.attempt_disconnect);
    CF_CHECK(frame_is(&jz1.last, true));
    CF_CHECK(frame_is(&jz2.last, true));
    CF_CHECK(cf_cable_revocation_revoked(jz1.slot));
    CF_CHECK(cf_cable_revocation_revoked(jz2.slot));
    /* The disconnect frame was attempted once per loop, then the subscriptions
     * were removed before the acknowledgment. */
    CF_CHECK(jz1.sink_calls == 1 && jz2.sink_calls == 1);
    CF_CHECK(cf_cable_loop_subscription_count(jz1.loop) == 0);
    CF_CHECK(cf_cable_loop_subscription_count(jz2.loop) == 0);
    CF_REQUIRE(rev_wait(&jz1, rev_pred_cleanup_done, 2000));
    CF_REQUIRE(rev_wait(&jz2, rev_pred_cleanup_done, 2000));
    CF_CHECK(jz1.cleanup_calls == 1 && jz2.cleanup_calls == 1);

    /* Kevin was not touched: still subscribed, still receiving. */
    CF_CHECK(kevin.revoke_calls == 0);
    CF_CHECK(cf_cable_loop_subscription_count(kevin.loop) == 1);
    CF_CHECK(!cf_cable_revocation_revoked(kevin.slot));
    char stream[256];
    rev_room_stream(&f, CHAN_DESIGNERS, stream, sizeof stream);
    CF_REQUIRE(rev_publish(f.cable, stream, "{\"n\":1}"));
    CF_CHECK(kevin.sink_calls == 1);
    CF_CHECK(jz1.sink_calls == 1 && jz2.sink_calls == 1);

    rev_close(&kevin);
    rev_close(&jz2);
    rev_close(&jz1);
    chan_fixture_close(&f);
}

CF_TEST(barrier_blocks_until_ack_and_cleanup_never_blocks_it) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    rev_loop jz;
    CF_REQUIRE(rev_open(&f, CHAN_JZ, "JZ", &jz));
    pthread_mutex_lock(&jz.mutex);
    jz.hold_revoke = true;
    jz.hold_cleanup = true;
    pthread_mutex_unlock(&jz.mutex);

    barrier_call bc;
    barrier_start(&bc, f.cable, CHAN_JZ, false);
    CF_REQUIRE(rev_wait(&jz, rev_pred_in_revoke, 2000));
    /* The loop is in revoke; the barrier must still be waiting for the ack. */
    CF_CHECK(!barrier_done(&bc, 150));

    pthread_mutex_lock(&jz.mutex);
    jz.hold_revoke = false;
    pthread_cond_broadcast(&jz.cv);
    pthread_mutex_unlock(&jz.mutex);

    /* Cleanup is latched after the ack: it must not hold the barrier. */
    CF_REQUIRE(rev_wait(&jz, rev_pred_in_cleanup, 2000));
    CF_REQUIRE(barrier_done(&bc, 2000));
    pthread_mutex_lock(&bc.mutex);
    CF_CHECK(bc.rc == CF_OK);
    pthread_mutex_unlock(&bc.mutex);
    CF_CHECK(jz.cleanup_calls == 1);
    CF_CHECK(cf_cable_revocation_revoked(jz.slot));

    pthread_mutex_lock(&jz.mutex);
    jz.hold_cleanup = false;
    pthread_cond_broadcast(&jz.cv);
    pthread_mutex_unlock(&jz.mutex);
    CF_REQUIRE(rev_wait(&jz, rev_pred_cleanup_done, 2000));

    barrier_finish(&bc);
    rev_close(&jz);
    chan_fixture_close(&f);
}

CF_TEST(full_broadcast_queue_does_not_starve_control) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    rev_loop jz;
    CF_REQUIRE(rev_open(&f, CHAN_JZ, "JZ", &jz));
    CF_REQUIRE(rev_subscribe(&jz, "RoomChannel", CHAN_DESIGNERS));
    jz.sink_calls = 0; /* ignore the subscribe confirmation */
    char stream[256];
    rev_room_stream(&f, CHAN_DESIGNERS, stream, sizeof stream);

    /* Fill the loop's ordinary pending queue to its command bound and keep
     * automatic delivery held. */
    cf_cable_test_hold_delivery(jz.loop, true);
    for (size_t i = 0; i < CF_CABLE_LOOP_MAX_COMMANDS; i++) {
        CF_REQUIRE(rev_publish(f.cable, stream, "{\"n\":1}"));
    }
    CF_CHECK(cf_cable_loop_pending(jz.loop) == CF_CABLE_LOOP_MAX_COMMANDS);

    int64_t start = ct_now_ms();
    CF_CHECK(cf_cable_disconnect_user(f.cable, CHAN_JZ, true) == CF_OK);
    int64_t elapsed = ct_now_ms() - start;

    /* Control was consumed out of band: the acknowledgment is prompt even
     * while the ordinary queue is full, and the queued commands are not a
     * substitute for it. */
    CF_REQUIRE(rev_wait(&jz, rev_pred_serviced, 2000));
    CF_CHECK(elapsed < 2000);
    CF_CHECK(cf_cable_revocation_revoked(jz.slot));
    CF_CHECK(cf_cable_loop_subscription_count(jz.loop) == 0);
    CF_CHECK(cf_cable_loop_pending(jz.loop) == CF_CABLE_LOOP_MAX_COMMANDS);
    CF_CHECK(jz.sink_calls == 1); /* only the disconnect frame */

    rev_close(&jz); /* detach frees the held queue */
    chan_fixture_close(&f);
}

CF_TEST(partially_sent_frame_closes_without_interleaving) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    rev_loop jz;
    CF_REQUIRE(rev_open(&f, CHAN_JZ, "JZ", &jz));
    CF_REQUIRE(rev_subscribe(&jz, "RoomChannel", CHAN_DESIGNERS));
    jz.sink_calls = 0; /* ignore the subscribe confirmation */
    pthread_mutex_lock(&jz.mutex);
    jz.frame_partial = true;
    pthread_mutex_unlock(&jz.mutex);

    CF_CHECK(cf_cable_disconnect_user(f.cable, CHAN_JZ, false) == CF_OK);
    CF_REQUIRE(rev_wait(&jz, rev_pred_serviced, 2000));
    CF_CHECK(!jz.last.attempt_disconnect);
    /* The frame bytes are still the exact reference frame, but no frame was
     * attempted: the loop closes immediately to avoid interleaving. */
    CF_CHECK(frame_is(&jz.last, false));
    CF_CHECK(jz.sink_calls == 0);
    CF_CHECK(jz.revoke_calls == 1);

    rev_close(&jz);
    chan_fixture_close(&f);
}

CF_TEST(nonreading_socket_does_not_delay_the_acknowledgment) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    int fds[2];
    CF_REQUIRE(ct_socketpair(fds));
    int small = 4096;
    (void)setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof small);

    rev_loop jz;
    CF_REQUIRE(rev_register_loop(&f, CHAN_JZ, "JZ", &jz));
    pthread_mutex_lock(&jz.mutex);
    jz.sink_fd = fds[0];
    pthread_mutex_unlock(&jz.mutex);
    CF_REQUIRE(pthread_create(&jz.thread, NULL, rev_owner_main, &jz) == 0);
    jz.has_thread = true;
    CF_REQUIRE(rev_subscribe(&jz, "RoomChannel", CHAN_DESIGNERS));

    /* Fill the socket: the peer never reads, so sends now fail. */
    unsigned char fill[1024];
    memset(fill, 0, sizeof fill);
    for (size_t i = 0; i < 4096; i++) {
        if (send(fds[0], fill, sizeof fill, MSG_NOSIGNAL | MSG_DONTWAIT) < 0) {
            break;
        }
    }

    int64_t start = ct_now_ms();
    CF_CHECK(cf_cable_disconnect_user(f.cable, CHAN_JZ, true) == CF_OK);
    CF_CHECK(ct_now_ms() - start < 2000);
    CF_REQUIRE(rev_wait(&jz, rev_pred_serviced, 2000));
    CF_CHECK(jz.last.attempt_disconnect);
    CF_CHECK(jz.failed_sends >= 1); /* the disconnect send itself failed */
    CF_CHECK(jz.revoke_calls == 1);
    CF_CHECK(cf_cable_revocation_revoked(jz.slot));

    rev_close(&jz);
    close(fds[1]);
    chan_fixture_close(&f);
}

CF_TEST(barrier_is_scoped_to_its_cable_and_user) {
    chan_fixture a, b;
    CF_REQUIRE(chan_fixture_open(&a));
    CF_REQUIRE(chan_fixture_open(&b));
    rev_loop in_a, in_b, other;
    CF_REQUIRE(rev_open(&a, CHAN_JZ, "JZ", &in_a));
    CF_REQUIRE(rev_open(&b, CHAN_JZ, "JZ", &in_b));
    /* A loop for another user on the same cable is untouched too. */
    CF_REQUIRE(rev_open(&a, CHAN_KEVIN, "Kevin", &other));

    CF_CHECK(cf_cable_disconnect_user(a.cable, CHAN_JZ, true) == CF_OK);
    CF_REQUIRE(rev_wait(&in_a, rev_pred_serviced, 2000));
    CF_CHECK(cf_cable_revocation_revoked(in_a.slot));
    CF_CHECK(!cf_cable_revocation_revoked(in_b.slot));
    CF_CHECK(!cf_cable_revocation_revoked(other.slot));
    CF_CHECK(in_b.revoke_calls == 0 && other.revoke_calls == 0);

    rev_close(&other);
    rev_close(&in_b);
    rev_close(&in_a);
    chan_fixture_close(&b);
    chan_fixture_close(&a);
}

/* ---- CABLE-03: stale-auth rejection ---------------------------------------- */

CF_TEST(auth_gate_rejects_stale_versions_and_bounds_resubmission) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    rev_loop jz;
    CF_REQUIRE(rev_open(&f, CHAN_JZ, "JZ", &jz));

    cf_cable_auth_ticket stale = cf_cable_auth_capture(f.app);
    CF_CHECK(stale.valid);
    CF_CHECK(cf_cable_auth_version_current(f.app, stale));
    CF_REQUIRE(cf_write(f.app, cb_noop, NULL) == CF_OK);
    CF_CHECK(!cf_cable_auth_version_current(f.app, stale));

    bool resubmit = false;
    CF_CHECK(cf_cable_auth_install(jz.slot, stale, &resubmit) == CF_BUSY);
    CF_CHECK(resubmit); /* one bounded resubmission remains */
    CF_CHECK(cf_cable_auth_install(jz.slot, stale, &resubmit) == CF_BUSY);
    CF_CHECK(!resubmit); /* budget spent: the caller rejects the subscription */

    cf_cable_auth_ticket fresh = cf_cable_auth_capture(f.app);
    CF_CHECK(cf_cable_auth_install(jz.slot, fresh, &resubmit) == CF_OK);
    CF_CHECK(!resubmit);
    /* The successful install reset the budget for the next result. */
    CF_CHECK(cf_cable_auth_install(jz.slot, fresh, &resubmit) == CF_OK);

    cf_cable_auth_ticket invalid = {0, false};
    CF_CHECK(cf_cable_auth_install(jz.slot, invalid, &resubmit) == CF_INVALID);
    CF_CHECK(cf_cable_auth_install(NULL, fresh, &resubmit) == CF_INVALID);

    rev_close(&jz);
    chan_fixture_close(&f);
}

CF_TEST(auth_install_is_refused_during_and_after_revocation) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    rev_loop jz;
    CF_REQUIRE(rev_open(&f, CHAN_JZ, "JZ", &jz));
    pthread_mutex_lock(&jz.mutex);
    jz.hold_revoke = true;
    pthread_mutex_unlock(&jz.mutex);

    cf_cable_auth_ticket before = cf_cable_auth_capture(f.app);
    barrier_call bc;
    barrier_start(&bc, f.cable, CHAN_JZ, false);
    CF_REQUIRE(rev_wait(&jz, rev_pred_in_revoke, 2000));

    /* An authorization result racing the barrier may not install: the loop
     * is revoking. */
    bool resubmit = true;
    CF_CHECK(cf_cable_auth_install(jz.slot, before, &resubmit) ==
             CF_FORBIDDEN);
    CF_CHECK(!resubmit);

    pthread_mutex_lock(&jz.mutex);
    jz.hold_revoke = false;
    pthread_cond_broadcast(&jz.cv);
    pthread_mutex_unlock(&jz.mutex);
    CF_REQUIRE(barrier_done(&bc, 2000));
    pthread_mutex_lock(&bc.mutex);
    CF_CHECK(bc.rc == CF_OK);
    pthread_mutex_unlock(&bc.mutex);

    /* After the acknowledgment even a fresh result is refused. */
    cf_cable_auth_ticket after = cf_cable_auth_capture(f.app);
    CF_CHECK(cf_cable_auth_install(jz.slot, after, &resubmit) == CF_FORBIDDEN);
    CF_CHECK(cf_cable_revocation_revoked(jz.slot));

    barrier_finish(&bc);
    rev_close(&jz);
    chan_fixture_close(&f);
}

/* ---- AUTH-05: the event's reconnect flag reaches the loop ------------------- */

static void auth05_case(cf_write_fn fn, bool reconnect) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    CF_REQUIRE(register_handler(&f));
    rev_loop jz;
    CF_REQUIRE(rev_open(&f, CHAN_JZ, "JZ", &jz));

    event_call call = {.user_id = CHAN_JZ, .reconnect = reconnect};
    CF_CHECK(cf_write(f.app, fn, &call) == CF_OK);
    CF_CHECK(call.rc == CF_OK);
    CF_REQUIRE(rev_wait(&jz, rev_pred_serviced, 2000));
    CF_CHECK(jz.last.user_id == CHAN_JZ);
    CF_CHECK(jz.last.reconnect == reconnect);
    CF_CHECK(jz.last.attempt_disconnect);
    CF_CHECK(frame_is(&jz.last, reconnect));

    rev_close(&jz);
    chan_fixture_close(&f);
}

CF_TEST(membership_removal_and_logout_disconnect_with_reconnect) {
    auth05_case(cb_membership_destroy, true);
    auth05_case(cb_reset_remote, true);
}

CF_TEST(ban_and_deactivation_disconnect_without_reconnect) {
    auth05_case(cb_ban, false);
    auth05_case(cb_deactivate, false);
}

/* ---- writer integration and fail-closed wiring ------------------------------ */

CF_TEST(empty_cable_succeeds_but_unregistered_loops_fail_closed) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    CF_REQUIRE(register_handler(&f));

    /* No connected loop: there is nothing to revoke, the committed write
     * succeeds. */
    uint64_t before = cf_data_version(f.app);
    event_call call = {.user_id = CHAN_JZ, .reconnect = true};
    CF_CHECK(cf_write(f.app, cb_disconnect, &call) == CF_OK);
    CF_CHECK(cf_data_version(f.app) == before + 1);

    /* A loop attached without the barrier wiring: fail closed, never report a
     * revocation that was not applied. */
    cf_cable_loop *loop = NULL;
    CF_REQUIRE(cf_cable_test_attach(f.cable, CHAN_JZ, "JZ", NULL, NULL,
                                    &loop) == CF_OK);
    before = cf_data_version(f.app);
    CF_CHECK(cf_write(f.app, cb_disconnect, &call) == CF_INTERNAL);
    CF_CHECK(cf_data_version(f.app) == before + 1); /* committed after all */
    cf_cable_test_detach(loop);

    CF_CHECK(cf_write(f.app, cb_disconnect, &call) == CF_OK);
    chan_fixture_close(&f);
}

/* ---- the 5 s acknowledgment deadline ---------------------------------------- */

typedef struct {
    int calls;
    int64_t user_id;
} fatal_record;

static void record_fatal(void *user, int64_t user_id) {
    fatal_record *fr = user;
    fr->calls++;
    fr->user_id = user_id;
}

CF_TEST(ack_timeout_stops_serving_and_reports_internal) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    rev_loop jz;
    CF_REQUIRE(rev_register_loop(&f, CHAN_JZ, "JZ", &jz));
    /* No owner thread: the registered loop never consumes its control. */

    fatal_record fatal = {0, 0};
    cf_cable_revocation_test_fatal_fn(record_fatal, &fatal);
    cf_cable_revocation_test_ack_timeout_ms(80);

    cf_err rc = cf_cable_disconnect_user(f.cable, CHAN_JZ, true);
    CF_CHECK(rc == CF_INTERNAL); /* committed; never CF_OK, never CF_BUSY */
    CF_CHECK(fatal.calls == 1);
    CF_CHECK(fatal.user_id == CHAN_JZ);
    CF_CHECK(cf_app_stop_requested(f.app)); /* stop serving */

    /* The default deadline is the barrier's own 5 s, distinct from C01's
     * 30 s send-progress wait. */
    CF_CHECK(CF_CABLE_REVOCATION_ACK_TIMEOUT_MS == 5000);
    CF_CHECK(CF_CABLE_REVOCATION_ACK_TIMEOUT_MS != CF_CABLE_WRITE_STALL_MS);

    cf_cable_revocation_test_reset();
    rev_close(&jz);
    chan_fixture_close(&f);
}

/* The default terminal action must terminate the process with an error. The
 * child is forked before any thread exists in it and reports through its exit
 * status: 0 would mean the barrier returned without terminating. */
#define AUTH05_CHILD_SETUP_FAILED 42

static int ack_timeout_child(void) {
    struct rlimit no_core = {0, 0};
    (void)setrlimit(RLIMIT_CORE, &no_core);
    signal(SIGABRT, SIG_DFL);
    chan_fixture f;
    if (!chan_fixture_open(&f)) return AUTH05_CHILD_SETUP_FAILED;
    cf_cable_revocation *slot = NULL;
    cf_cable_revocation_ops ops;
    memset(&ops, 0, sizeof ops);
    if (cf_cable_revocation_register(f.cable, f.app, 1, &ops, &slot) !=
        CF_OK) {
        return AUTH05_CHILD_SETUP_FAILED;
    }
    cf_cable_revocation_test_ack_timeout_ms(120);
    (void)cf_cable_disconnect_user(f.cable, 1, false);
    return 0; /* must not be reached: the default action terminates */
}

CF_TEST(ack_timeout_default_terminates_with_an_error) {
    fflush(stdout); /* the forked child must not flush our buffered output */
    pid_t pid = fork();
    CF_REQUIRE(pid >= 0);
    if (pid == 0) {
        _exit(ack_timeout_child());
    }
    int status = 0;
    CF_REQUIRE(waitpid(pid, &status, 0) == pid);
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        CF_CHECK(code != 0); /* 0 means the barrier returned */
        CF_CHECK(code != AUTH05_CHILD_SETUP_FAILED); /* setup must succeed */
    } else if (WIFSIGNALED(status)) {
        /* The default fatal action aborts. The exact death signal differs by
         * runtime: glibc raises SIGABRT, Fil-C's panic path traps, sanitizer
         * runtimes may exit nonzero instead. Any non-clean termination proves
         * the error path; exit status 0 would prove the barrier returned. */
        CF_CHECK(WTERMSIG(status) != 0);
    } else {
        fprintf(stderr, "  child status raw=0x%x\n", status);
        CF_CHECK(false);
    }
}

CF_TEST_MAIN()
