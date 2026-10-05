/* src/cable/channels.h — task C02: application channels, local subscription
 * maps and broadcasts (04-cable-jobs.md "C02: application channels"; source
 * tmp/rust-ref/crates/campfire/src/channels.rs, the channels submodule files
 * and channels/broadcasts.rs, plus crates/cable/src/{connection.rs,pubsub.rs,
 * turbo.rs}).
 *
 * This header carries the C02 surface: the `cf_cable` application object
 * (cf.h's frozen publish/disconnect pair), the C01 server hooks main.c wires
 * into the /cable mount, and the `channels/broadcasts.rs` payload source.
 *
 * Threading model: one `cf_cable_loop` per connection, owned by that
 * connection's owner thread (the C01 socket thread, or a test thread). The
 * loop holds the subscriptions and their stream map; model reads for
 * subscribe validation run on the owner thread with the loop's own reader
 * (never on an HTTP loop thread), and PresenceChannel's membership effects go
 * through cf_write. Cross-loop broadcasts enqueue immutable frame references
 * into each matching loop's bounded pending queue and deliver them to the
 * socket with C01's thread-safe send API. No total ordering between
 * separate broadcasts is invented.
 *
 * Evidence: docs/devel/evidence/C02.md. */
#ifndef CF_CABLE_CHANNELS_H
#define CF_CABLE_CHANNELS_H

#include "cf.h"

#include "cable/cable.h"
#include "cable/revocation.h"
#include "context.h"
#include "views.h"
#include "models/boost.h"
#include "models/membership.h"
#include "models/message.h"
#include "models/room.h"

/* ---- limits (04 C02) ------------------------------------------------------ */

/* One connection's subscriptions and raw-identifier size (connection.rs). */
#define CF_CABLE_MAX_SUBSCRIPTIONS 64
#define CF_CABLE_MAX_IDENTIFIER_BYTES 4096
/* Per loop: pending broadcast commands and bytes of payload references.
 * The byte count adds each command's frame length exactly once. */
#define CF_CABLE_LOOP_MAX_COMMANDS 1024
#define CF_CABLE_LOOP_MAX_BYTES ((size_t)16 << 20)

typedef struct cf_cable cf_cable;
typedef struct cf_cable_loop cf_cable_loop;

/* ---- creation and lifecycle ---------------------------------------------- */

typedef struct {
    /* Borrowed until cf_cable_destroy; supplies the config values, the
     * writer for membership effects and (through its config) the database
     * path and SECRET_KEY_BASE. Required. */
    cf_app *app;
    /* Override the database path readers open (default: app config's
     * database_path). Borrowed. */
    const char *database_path;
    /* Override SECRET_KEY_BASE (default: app config's). Empty = default. */
    cf_span secret_key_base;
    /* Bounds overrides; 0 selects the constants above. */
    size_t max_pending_commands;
    size_t max_pending_bytes;
} cf_cable_config;

/* Create the application cable. On success *out owns it until
 * cf_cable_destroy; on failure *out stays NULL. */
cf_err cf_cable_create(const cf_cable_config *config, cf_cable **out);

/* Stop publishing/attaching and wait (up to timeout_ms) for every connection
 * loop to detach. Production shutdown calls this after
 * cf_cable_server_destroy and before cf_app_stop: the server join returns
 * when its threads decrement the live count, while a thread's exit (which
 * detaches its loop and may run PresenceChannel's absent write) can lag
 * slightly. CF_BUSY means loops did not reach zero in time. */
cf_err cf_cable_stop(cf_cable *cable, uint64_t timeout_ms);

/* Stop serving and release. Connection loops attached on other threads hold
 * their own reference: run cf_cable_server_destroy (which joins every
 * connection thread, whose exit detaches its loop) and cf_cable_stop before
 * this call. */
void cf_cable_destroy(cf_cable *cable);

/* Fill the C01 server hooks (authenticate + on_text) for this cable. `out`
 * must be zeroed by the caller (cf_cable_server_config_default does). */
void cf_cable_server_hooks(cf_cable *cable, cf_cable_hooks *out);

/* ---- counters -------------------------------------------------------------- */

typedef struct {
    size_t loops;                /* attached connection loops */
    uint64_t published;          /* cf_cable_publish calls */
    uint64_t delivered;          /* commands handed to a loop sink */
    uint64_t dropped;            /* broadcasts dropped (bounds or send error) */
} cf_cable_stats;

void cf_cable_stats_get(const cf_cable *cable, cf_cable_stats *out);
uint64_t cf_cable_loop_dropped(const cf_cable_loop *loop);
size_t cf_cable_loop_pending(const cf_cable_loop *loop);

/* ---- identifiers ----------------------------------------------------------- */

/* `GlobalID#to_param` for a room (its STI class name, "Rooms::Open") and for
 * a user ("User"). Owned output; dispose with cf_str_dispose. */
cf_err cf_cable_room_gid_param(const cf_room *room, cf_str *out);
cf_err cf_cable_user_gid_param(int64_t user_id, cf_str *out);

/* ---- broadcasts (channels/broadcasts.rs) ----------------------------------- */

/* The HTML a broadcast carries, rendered by the caller: Rust's `Partials`
 * trait. `shared_room` and `direct_room` come from the sidebar packet of A02
 * that has not landed yet; the production provider reports them as
 * unavailable rather than inventing markup. */
typedef struct cf_broadcast_partials {
    cf_err (*message)(void *user, const cf_message *message, cf_builder *out);
    cf_err (*message_presentation)(void *user, const cf_message *message,
                                   cf_builder *out);
    cf_err (*boost)(void *user, const cf_boost *boost, cf_builder *out);
    cf_err (*shared_room)(void *user, const cf_room *room, cf_builder *out);
    cf_err (*direct_room)(void *user, const cf_membership *membership,
                          cf_builder *out);
    void *user;
} cf_broadcast_partials;

/* cf_broadcast_partials_views' borrowed pair: `ctx` drives A02's presenters
 * (it supplies the read connection) and `view` its renderers. The struct must
 * outlive the partials it was installed into. */
typedef struct {
    cf_ctx *ctx;
    const cf_view_ctx *view;
} cf_broadcast_views;

/* Fill the production partials: A02's presenters and renderers. The sidebar
 * room partials are a later A02 packet and report CF_NOT_FOUND (reported to
 * the integrator) instead of duplicating markup here. */
void cf_broadcast_partials_views(cf_broadcast_partials *out,
                                 cf_broadcast_views *views);

/* `action_tag` markup: extra attributes first, then action, then target, as
 * turbo_stream_action_tag builds its hash. `maintain_scroll` is the one
 * attribute the app's broadcasts pass. No template for remove/refresh. */
cf_err cf_broadcast_action_tag(cf_builder *out, cf_span action,
                               bool has_target, cf_span target,
                               bool has_template, cf_span html,
                               bool maintain_scroll);

/* Message::Broadcasts and the controllers (streams, targets and payloads
 * exactly as broadcasts.rs). Every function returns CF_BUSY when a loop's
 * pending queue overflowed (the caller records the post-commit delivery
 * failure); CF_OK otherwise. */
cf_err cf_broadcast_read_room(cf_cable *cable, int64_t user_id,
                              int64_t room_id);
cf_err cf_broadcast_unread_room(cf_db *db, cf_cable *cable,
                                const cf_room *room);
cf_err cf_broadcast_message_create(cf_db *db, cf_cable *cable,
                                   const cf_room *room, const cf_message *message,
                                   const cf_broadcast_partials *partials);
cf_err cf_broadcast_message_remove(cf_cable *cable, const cf_room *room,
                                   const cf_message *message);
cf_err cf_broadcast_message_replace(cf_cable *cable, const cf_room *room,
                                    const cf_message *message,
                                    const cf_broadcast_partials *partials);
cf_err cf_broadcast_boost_create(cf_cable *cable, const cf_room *room,
                                 const cf_message *message, const cf_boost *boost,
                                 const cf_broadcast_partials *partials);
cf_err cf_broadcast_boost_remove(cf_cable *cable, const cf_room *room,
                                 const cf_boost *boost);
cf_err cf_broadcast_room_remove(cf_cable *cable, const cf_room *room);
cf_err cf_broadcast_open_room_create(cf_cable *cable, const cf_room *room,
                                     const cf_broadcast_partials *partials);
cf_err cf_broadcast_open_room_update(cf_cable *cable, const cf_room *room,
                                     const cf_broadcast_partials *partials);
cf_err cf_broadcast_closed_room_create(cf_db *db, cf_cable *cable,
                                       const cf_room *room,
                                       const cf_broadcast_partials *partials);
cf_err cf_broadcast_closed_room_update(cf_db *db, cf_cable *cable,
                                       const cf_room *room,
                                       const cf_broadcast_partials *partials);
cf_err cf_broadcast_direct_room_create(cf_db *db, cf_cable *cable,
                                       const cf_room *room,
                                       const cf_broadcast_partials *partials);
/* `involvement_previously_was`; has_previous=false is Rails' nil previous
 * (the update saved without changing it), which raises NilInquiry where the
 * reference does and returns CF_INVALID. */
cf_err cf_broadcast_involvement_change(
    cf_cable *cable, const cf_room *room, const cf_membership *membership,
    bool has_previous, cf_involvement previous,
    const cf_broadcast_partials *partials);

/* ---- test support (not a production path) --------------------------------- */

/* A loop sink: production uses cf_cable_socket_send_frame; tests may supply
 * their own. Called with one retained immutable frame. */
typedef cf_err (*cf_cable_loop_send_fn)(void *user, cf_cable_frame *frame);

/* Attach a loop for `user_id` on the calling thread, with a caller-provided
 * sink. CF_CABLE_MAX_SUBSCRIPTIONS etc. apply. The loop's reader is opened
 * on this thread; detach on the same thread. */
cf_err cf_cable_test_attach(cf_cable *cable, int64_t user_id,
                            const char *user_name, cf_cable_loop_send_fn send,
                            void *send_user, cf_cable_loop **out);
/* Feed one client text frame (the C01 on_text contract). */
cf_err cf_cable_test_feed(cf_cable_loop *loop, cf_span text);
void cf_cable_test_detach(cf_cable_loop *loop);
/* Test-only: hold automatic delivery so a test can fill the bounded queue
 * without a real stalled connection; releasing flushes it. Production never
 * calls this. */
void cf_cable_test_hold_delivery(cf_cable_loop *loop, bool hold);
const char *cf_cable_loop_user_name(const cf_cable_loop *loop);
int64_t cf_cable_loop_user_id(const cf_cable_loop *loop);
size_t cf_cable_loop_subscriptions(const cf_cable_loop *loop);
/* Test-only: `Channel::Naming.broadcasting_for` for a class name (the exact
 * naming.rs mapping, including namespaced classes). Owned output. */
cf_err cf_cable_test_broadcasting_for(const char *class_name, cf_span part,
                                      cf_str *out);

/* ---- loop internals shared with channels.c --------------------------------- */

/* `cf_cable_subscription`: one client subscription (connection.rs Entry). */
typedef struct cf_cable_subscription {
    struct cf_cable_loop *owner; /* owning loop (its mutex guards the array) */
    char *identifier;            /* owned raw identifier text (NUL-terminated) */
    size_t identifier_len;
    char *encoded;          /* owned JSON literal of `identifier` */
    char **streams;         /* owned stream names this subscription reads */
    size_t stream_count, stream_cap;
    const char *class_name; /* canonical registered class name */
    void *channel;          /* channels.c private per-subscription state */
    bool rejected;
} cf_cable_subscription;

/* Add a subscription; returns NULL when a bound is reached or on allocation
 * failure. The identifier is copied. */
cf_cable_subscription *cf_cable_loop_add_subscription(cf_cable_loop *loop,
                                                      cf_span identifier,
                                                      const char *class_name);
void cf_cable_loop_remove_subscription(cf_cable_loop *loop, size_t index);
cf_cable_subscription *cf_cable_loop_find_subscription(cf_cable_loop *loop,
                                                       cf_span identifier);
size_t cf_cable_loop_subscription_count(cf_cable_loop *loop);
cf_cable_subscription *cf_cable_loop_subscription_at(cf_cable_loop *loop,
                                                     size_t index);
/* Append one stream name to the subscription (stream_from). */
cf_err cf_cable_subscription_stream(cf_cable_subscription *sub,
                                    cf_span stream);
void cf_cable_subscription_reject(cf_cable_subscription *sub);
bool cf_cable_subscription_is_rejected(const cf_cable_subscription *sub);

cf_db *cf_cable_loop_reader(cf_cable_loop *loop);
cf_app *cf_cable_loop_app(const cf_cable_loop *loop);
cf_cable *cf_cable_loop_cable(cf_cable_loop *loop);
/* The loop's preallocated revocation slot: non-NULL for a production loop,
 * NULL for a test loop that registers its own (C03). */
cf_cable_revocation *cf_cable_loop_revocation(cf_cable_loop *loop);
/* Borrowed SECRET_KEY_BASE (for Turbo's signed-stream verifier). */
cf_span cf_cable_secret(cf_cable *cable);

/* Send one text frame to this loop's socket (owner thread only, used for
 * confirm/reject and transmissions). */
cf_err cf_cable_loop_send_text(cf_cable_loop *loop, cf_span text);
cf_err cf_cable_loop_send_confirm(cf_cable_loop *loop, cf_span identifier);
cf_err cf_cable_loop_send_reject(cf_cable_loop *loop, cf_span identifier);

/* Log one sanitized line (never identifiers or payloads). */
void cf_cable_log(const char *what, const char *detail);

/* One client text frame (channels.c). pubsub.c wraps it so a confirmation is
 * written before the broadcasts the handler triggered (reference order). */
cf_err cf_cable_channels_dispatch(cf_cable_loop *loop, cf_span text);
cf_err cf_cable_loop_handle_text(cf_cable_loop *loop, cf_span text);

/* Run one subscription's unsubscribed effects (channels.c; PresenceChannel's
 * absent). pubsub.c calls it on unsubscribe and on connection close.
 * `loop` may be NULL only when the caller has no live loop. */
void cf_cable_subscription_unsubscribed(cf_cable_loop *loop,
                                        cf_cable_subscription *sub);
void cf_cable_channel_dispose(void *channel);

#endif /* CF_CABLE_CHANNELS_H */
