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
 * loop holds the subscriptions and their stream map. On the production
 * reactor path the owner thread performs socket I/O and installs results
 * only: connection authentication and subscribe validation/model effects run
 * on the app's bounded request-worker pool (cf_app_submit_worker) with the
 * worker's own reader, and their results are installed through the C03
 * install gate/version check (a full worker queue rejects the subscription
 * and refuses the connection). Standalone sockets/tests keep the synchronous
 * path with the loop's own reader, and PresenceChannel's membership effects
 * go through cf_write either way. Cross-loop broadcasts enqueue immutable
 * frame references into each matching loop's bounded pending queue and
 * deliver them to the socket with C01's thread-safe send API. No total
 * ordering between separate broadcasts is invented.
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
    /* Number of HTTP loops the transport services (0 selects the bounded
     * default). Bounds the per-owner-loop shared-slot map: exactly one C03
     * control slot per owner loop, never one per connection. */
    size_t loops;
} cf_cable_config;

/* Upper bound on the owner-loop slot map when the loop count is not given. */
#define CF_CABLE_MAX_READERS ((size_t)64)

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
 * trait. `shared_room` renders once per broadcast (every recipient sees the
 * same bytes); `direct_room` is invoked once per membership by
 * cf_broadcast_direct_room_create, so it must render that recipient's own
 * markup (the reference renders SidebarDirect per membership). */
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

/* Fill the production partials: A02's presenters and renderers, including
 * the sidebar room renderers (`users/sidebars/rooms/_shared` behind
 * shared_room, and presenter.sidebar_direct + `_direct` behind direct_room,
 * both from A-users-sidebars / V-E). */
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
/* The loop's member of its owner loop's shared C03 control slot (production
 * reactor loops), or NULL (test loop / standalone legacy slot). */
cf_cable_revocation_member *cf_cable_loop_revocation_member(
    const cf_cable_loop *loop);
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

/* ---- the subscribe pipeline split for the worker handoff (P12-02b) --------- */

/* 04 C02: "Subscribe validation runs in a worker; confirmation is emitted only
 * after its successful model effects." The pipeline is split so the owner
 * thread never touches a DB statement and a request worker never touches the
 * loop, socket or subscription: the owner parses and installs the shell, the
 * worker builds a detached plan with its own reader, the owner installs the
 * plan through the C03 gate, an optional worker phase runs the deferred model
 * effect (PresenceChannel's present write), and only then does the owner emit
 * the confirmation. The synchronous dispatch path runs the same pieces on one
 * thread (the loop's own reader), so behavior is identical. */

/* A validation result that owns all of its model values (no borrowed loop,
 * subscription, socket or cable pointers). */
typedef struct {
    bool rejected;
    bool have_room;
    cf_room room;          /* owned while have_room */
    char **streams;        /* owned names the subscription reads */
    size_t stream_count;
    bool presence_present; /* PresenceChannel#subscribed effect is deferred */
    int64_t room_id;       /* effect target when presence_present */
} cf_cable_sub_plan;

void cf_cable_sub_plan_dispose(cf_cable_sub_plan *plan);

/* Owner thread: parse a subscribe command and install its subscription shell
 * and channel state, with no model reads. CF_OK with *out == NULL means the
 * command is ignored without a reply (duplicate identifier, bounds, unknown
 * class, malformed command), exactly as the reference. */
cf_err cf_cable_subscribe_begin(cf_cable_loop *loop, cf_span text,
                                cf_cable_subscription **out);

/* Worker (or the owner on the synchronous path): run the channel's subscribed
 * validation against `reader` and fill a detached plan. `identifier` is the
 * raw identifier stored on the shell; `secret` is SECRET_KEY_BASE for the
 * Turbo signed-stream verifier. */
cf_err cf_cable_subscribe_validate(const char *class_name, cf_db *reader,
                                   int64_t user_id, cf_span identifier,
                                   cf_span secret, cf_cable_sub_plan *plan);

/* Owner thread, no database access: install a validated plan onto the
 * subscription (room state, streams, rejection flag). */
cf_err cf_cable_subscribe_apply(cf_cable_subscription *sub,
                                cf_cable_sub_plan *plan);

/* Worker: the deferred model effect of a validated PresenceChannel
 * subscription (Membership::Connectable present + the read broadcast). Takes
 * values only; uses cf_write/cf_cable_publish. */
cf_err cf_cable_subscribe_presence_effect(cf_app *app, cf_cable *cable,
                                          int64_t user_id, int64_t room_id);

/* Owner thread: the reference reply once the model effects succeeded (or the
 * gate refused): confirm, reject + remove, or log and keep the subscription
 * (a raised callback sends neither reply). */
void cf_cable_subscribe_reply(cf_cable_loop *loop, cf_cable_subscription *sub,
                              bool refused, cf_err rc);

/* `message` commands: the kind of a raw command text (the production wiring
 * routes subscribe and a message's "subscribed" action through the worker
 * pool; every other command dispatches synchronously, no reader involved). */
typedef enum {
    CF_CABLE_CMD_OTHER = 0,
    CF_CABLE_CMD_SUBSCRIBE,
    CF_CABLE_CMD_MESSAGE_SUBSCRIBED
} cf_cable_command_kind;

cf_cable_command_kind cf_cable_command_kind_of(cf_span text);

/* Owner thread: the existing subscription a `message` command addresses, or
 * NULL (the caller logs the reference's "unable to find subscription"). */
cf_cable_subscription *cf_cable_message_subscription(cf_cable_loop *loop,
                                                     cf_span text);

/* Worker: the `subscribed` action of a message for an installed subscription.
 * *handled == false when the channel ignores the action (the caller logs
 * "unable to process", as channel_perform does); otherwise `plan` is filled
 * (rejected subscriptions are ignored before any read). `sub` is only read. */
cf_err cf_cable_message_subscribed_validate(const cf_cable_subscription *sub,
                                            cf_db *reader, int64_t user_id,
                                            cf_span secret, bool *handled,
                                            cf_cable_sub_plan *plan);

/* Owner thread, no reads: channel_perform's gating for a message command's
 * "subscribed" action (false when the channel ignores the action or the
 * subscription is already rejected, so the caller logs "unable to process").
 * The validation itself then runs on a worker with copied inputs. */
bool cf_cable_message_subscribed_handled(const cf_cable_subscription *sub);

/* Run one subscription's unsubscribed effects (channels.c; PresenceChannel's
 * absent). pubsub.c calls it on unsubscribe and on connection close.
 * `loop` may be NULL only when the caller has no live loop. */
void cf_cable_subscription_unsubscribed(cf_cable_loop *loop,
                                        cf_cable_subscription *sub);
void cf_cable_channel_dispose(void *channel);

#endif /* CF_CABLE_CHANNELS_H */
