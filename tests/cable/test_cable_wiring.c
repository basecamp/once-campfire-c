/* tests/cable/test_cable_wiring.c — the C03 production wiring over the exact
 * main.c hooks: a live connection attaches its loop at C01's on_open, the
 * socket's owner loop consumes revocation control, the barrier disconnects a
 * real socket, the mandatory DISCONNECT_USER write returns CF_OK, the
 * subscribe install gate answers under data-version churn, and the C01
 * lifetime window (socket free vs a foreign barrier wake) is hammered under
 * ASan/TSan. */
#include "cable_channels_testutil.h"

#include "cable/revocation.h"
#include "db/writer.h"

#include <stdatomic.h>

static const char WIRING_DISCONNECT_TRUE[] =
    "{\"type\":\"disconnect\",\"reason\":\"remote\",\"reconnect\":true}";

/* Read the revocation wire: the exact remote-disconnect frame, then the
 * server's close frame (the reference close(1000)). */
static bool wiring_expect_revoked(chan_conn *c, int timeout_ms) {
    char got[512];
    if (!chan_next(c, got, sizeof got, timeout_ms)) {
        fprintf(stderr, "  no disconnect frame\n");
        return false;
    }
    if (strcmp(got, WIRING_DISCONNECT_TRUE) != 0) {
        fprintf(stderr, "  disconnect frame mismatch:\n  got: %s\n", got);
        return false;
    }
    unsigned opcode = 0;
    bool rsv1 = false;
    unsigned char payload[64];
    ssize_t n = ct_read_server_frame(c->run.peer_fd, &opcode, &rsv1, payload,
                                     sizeof payload, timeout_ms);
    if (n < 0 || opcode != 0x8) {
        fprintf(stderr, "  no close frame after the disconnect\n");
        return false;
    }
    return true;
}

CF_TEST(production_revocation_disconnects_a_live_connection) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    chan_conn c;
    CF_REQUIRE(chan_connect(&f, CHAN_JZ, &c));

    char id[128];
    chan_room_identifier(id, sizeof id, "RoomChannel", CHAN_DESIGNERS);
    CF_REQUIRE(chan_subscribe(&c, id));
    CF_CHECK(chan_confirm(&c, id));

    /* The barrier goes through the socket owner loop: it services control,
     * sends the exact reference frame, removes the subscription and closes. */
    CF_CHECK(cf_cable_disconnect_user(f.cable, CHAN_JZ, true) == CF_OK);
    CF_CHECK(wiring_expect_revoked(&c, 3000));

    /* Once revoked, a late subscribe is ignored: the loop accepts no new
     * access or output (no reply). */
    char heartbeat[64];
    chan_channel_identifier(heartbeat, sizeof heartbeat, "HeartbeatChannel");
    CF_REQUIRE(chan_subscribe(&c, heartbeat));
    CF_CHECK(chan_silent(&c, 400));

    /* No loops remain: a second barrier is a no-op success. */
    CF_CHECK(cf_cable_disconnect_user(f.cable, CHAN_JZ, true) == CF_OK);
    CF_CHECK(cf_cable_disconnect_user(f.cable, CHAN_JZ + 100, true) == CF_OK);

    chan_disconnect(&c);
    /* The loop detached before the socket memory was released. */
    cf_cable_stats stats;
    cf_cable_stats_get(f.cable, &stats);
    CF_CHECK(stats.loops == 0);

    /* Reconnect: fresh DB authentication, a fresh slot, and a normal
     * confirm (no stale authorization is carried over). */
    chan_conn fresh;
    CF_REQUIRE(chan_connect(&f, CHAN_JZ, &fresh));
    char heartbeat2[64];
    chan_channel_identifier(heartbeat2, sizeof heartbeat2, "HeartbeatChannel");
    CF_REQUIRE(chan_subscribe(&fresh, heartbeat2));
    CF_CHECK(chan_confirm(&fresh, heartbeat2));
    chan_disconnect(&fresh);
    chan_fixture_close(&f);
}

/* ---- the mandatory D02 path over the real wiring --------------------------- */

static cf_err wiring_destroy_jz_membership(cf_tx *tx, void *arg) {
    (void)arg;
    cf_membership membership;
    bool found = false;
    cf_err rc = cf_membership_find_by_room_and_user(
        cf_tx_db(tx), CHAN_DESIGNERS, CHAN_JZ, &found, &membership);
    if (rc == CF_OK && found) rc = cf_membership_destroy(tx, &membership);
    if (found) cf_membership_dispose(&membership);
    return rc;
}

CF_TEST(mandatory_disconnect_user_write_returns_ok) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    CF_REQUIRE(cf_writer_set_control_handler(f.app, cf_cable_revocation_handler,
                                             f.cable) == CF_OK);
    chan_conn c;
    CF_REQUIRE(chan_connect(&f, CHAN_JZ, &c));
    char id[128];
    chan_room_identifier(id, sizeof id, "RoomChannel", CHAN_DESIGNERS);
    CF_REQUIRE(chan_subscribe(&c, id));
    CF_CHECK(chan_confirm(&c, id));

    /* Membership destroy commits DISCONNECT_USER (reconnect=true) and the
     * write returns CF_OK only after the live loop acknowledged. */
    CF_CHECK(cf_write(f.app, wiring_destroy_jz_membership, NULL) == CF_OK);
    CF_CHECK(wiring_expect_revoked(&c, 3000));

    chan_disconnect(&c);
    chan_fixture_close(&f);
}

/* ---- the subscription install gate under data-version churn ---------------- */

typedef struct {
    chan_fixture *f;
    _Atomic bool stop;
    _Atomic int calls;
    cf_err last;
} wiring_churn;

static cf_err wiring_churn_write(cf_tx *tx, void *arg) {
    (void)tx;
    (void)arg;
    return CF_OK;
}

static void *wiring_churn_main(void *arg) {
    wiring_churn *churn = arg;
    while (!atomic_load_explicit(&churn->stop, memory_order_relaxed)) {
        churn->last = cf_write(churn->f->app, wiring_churn_write, NULL);
        atomic_fetch_add_explicit(&churn->calls, 1, memory_order_relaxed);
    }
    return NULL;
}

/* Every subscribe either confirms or rejects; the stale-version resubmission
 * runs through the loop's bounded worker queue and never leaves the
 * connection hanging or half-installed. */
CF_TEST(subscribe_install_gate_answers_under_version_churn) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));
    chan_conn c;
    CF_REQUIRE(chan_connect(&f, CHAN_JZ, &c));

    wiring_churn churn;
    churn.f = &f;
    atomic_init(&churn.stop, false);
    atomic_init(&churn.calls, 0);
    churn.last = CF_OK;
    pthread_t thread;
    CF_REQUIRE(pthread_create(&thread, NULL, wiring_churn_main, &churn) == 0);

    int confirmed = 0, rejected = 0;
    for (int i = 0; i < 20; i++) {
        /* A real model read (find_room) lengthens the capture-to-install
         * window, so the churn is likely to invalidate some results. */
        char identifier[128];
        snprintf(identifier, sizeof identifier,
                 "{\"channel\":\"RoomChannel\",\"room_id\":%d,\"n\":%d}",
                 CHAN_DESIGNERS, i);
        CF_REQUIRE(chan_subscribe(&c, identifier));
        char expected[1024];
        chan_confirm_expected(expected, sizeof expected, identifier);
        char got[1024];
        if (!chan_next(&c, got, sizeof got, 3000)) {
            CF_CHECK(false && "no reply to a subscribe under version churn");
            break;
        }
        if (strcmp(got, expected) == 0) {
            confirmed++;
        } else {
            chan_reject_expected(expected, sizeof expected, identifier);
            if (strcmp(got, expected) != 0) {
                fprintf(stderr, "  unexpected reply: %s\n", got);
                CF_CHECK(false);
                break;
            }
            rejected++;
        }
    }
    atomic_store_explicit(&churn.stop, true, memory_order_relaxed);
    pthread_join(thread, NULL);
    CF_CHECK(atomic_load(&churn.calls) > 0);
    CF_CHECK(churn.last == CF_OK);
    fprintf(stderr, "  version churn: %d confirmed, %d rejected, %d writes\n",
            confirmed, rejected, atomic_load(&churn.calls));

    chan_disconnect(&c);
    chan_fixture_close(&f);
}

/* ---- the C01 socket lifetime window ---------------------------------------- */

/* A foreign barrier thread hammers `target` in a tight loop while the case
 * thread opens connections as JZ and closes them. The target stays a user
 * with no connections until the peer close, then switches to JZ: the barrier
 * first sees the victim's slot exactly around the socket teardown, which is
 * the window the lifetime fix eliminates (on_close detaches and unregisters
 * before the socket is freed). ASan makes a regression visible; TSan checks
 * the barrier/wake/detach synchronization. */
typedef struct {
    cf_cable *cable;
    _Atomic int64_t target;
    _Atomic bool stop;
    _Atomic int64_t calls;
} wiring_hammer;

static void *wiring_hammer_main(void *arg) {
    wiring_hammer *hammer = arg;
    while (!atomic_load_explicit(&hammer->stop, memory_order_relaxed)) {
        int64_t user = atomic_load_explicit(&hammer->target,
                                            memory_order_relaxed);
        (void)cf_cable_disconnect_user(hammer->cable, user, true);
        atomic_fetch_add_explicit(&hammer->calls, 1, memory_order_relaxed);
    }
    return NULL;
}

CF_TEST(socket_lifetime_survives_a_revocation_hammer) {
    chan_fixture f;
    CF_REQUIRE(chan_fixture_open(&f));

    wiring_hammer hammer;
    hammer.cable = f.cable;
    atomic_init(&hammer.target, CHAN_KEVIN); /* no Kevin connections */
    atomic_init(&hammer.stop, false);
    atomic_init(&hammer.calls, 0);
    pthread_t thread;
    CF_REQUIRE(pthread_create(&thread, NULL, wiring_hammer_main, &hammer) == 0);

    /* A PresenceChannel subscription also makes the thread-exit unsubscribe
     * path do a cf_write (membership absent), holding the detached-loop work
     * longer on the teardown path. */
    for (int i = 0; i < 40; i++) {
        chan_conn c;
        CF_REQUIRE(chan_connect(&f, CHAN_JZ, &c));
        char presence[128];
        chan_room_identifier(presence, sizeof presence, "PresenceChannel",
                             CHAN_DESIGNERS);
        CF_REQUIRE(chan_subscribe(&c, presence));
        CF_CHECK(chan_confirm(&c, presence));

        /* Abrupt peer close, then aim the barrier at JZ: the slot is still
         * unrevoked when the socket run ends and frees the socket. */
        close(c.run.peer_fd);
        c.run.peer_fd = -1;
        atomic_store_explicit(&hammer.target, CHAN_JZ, memory_order_relaxed);
        usleep(300);
        atomic_store_explicit(&hammer.target, CHAN_KEVIN,
                              memory_order_relaxed);

        ct_run_join(&c.run);
        close(c.run.fd);
        c.run.fd = -1;
        /* Let any late barrier drain before the next connection. */
        (void)cf_cable_disconnect_user(f.cable, CHAN_JZ, true);
    }

    atomic_store_explicit(&hammer.stop, true, memory_order_relaxed);
    pthread_join(thread, NULL);
    CF_CHECK(atomic_load(&hammer.calls) > 0);
    fprintf(stderr, "  lifetime hammer: %lld barrier calls\n",
            (long long)atomic_load(&hammer.calls));

    /* The cable is still healthy: a fresh connection attaches and works. */
    chan_conn c;
    CF_REQUIRE(chan_connect(&f, CHAN_JZ, &c));
    char id[128];
    chan_room_identifier(id, sizeof id, "RoomChannel", CHAN_DESIGNERS);
    CF_REQUIRE(chan_subscribe(&c, id));
    CF_CHECK(chan_confirm(&c, id));
    chan_disconnect(&c);
    chan_fixture_close(&f);
}

CF_TEST_MAIN()
