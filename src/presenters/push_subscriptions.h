/* src/presenters/push_subscriptions.h — the users/push_subscriptions#index
 * presenter.
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/users/push_subscriptions.rs
 * (`index`), the pinned port of reference/app/controllers/users/
 * push_subscriptions_controller.rb: `PushSubscription::for_user` mapped
 * through `presenters::accounts::push_subscription` (UA parse), plus
 * `last_room_visited` for `link_back_to_last_room_visited`.
 *
 * Contract: presenters load rows and assemble the view models inside one read
 * transaction; rendering performs no SQL, mutation, filesystem or network
 * work.  The view-model types and the renderers live in views.h; the pure row
 * mapping (cf_view_push_subscription_from_row / _parse) lives with the
 * renderer in src/views/users_push.c.
 */
#ifndef CF_PRESENTERS_PUSH_SUBSCRIPTIONS_H
#define CF_PRESENTERS_PUSH_SUBSCRIPTIONS_H

#include "views.h"

/* `users/push_subscriptions#index`: `PushSubscription::for_user` mapped
 * through the UA parser and `last_room_visited_in` (the last_room cookie's
 * room when the user is a member, else the user's original room), inside one
 * read transaction over ctx->reader.  `user` is the authenticated row the
 * controller resolved. */
cf_err cf_presenter_users_push_index(cf_ctx *ctx, const cf_user *user,
                                     cf_view_users_push_index_model *out);

#endif /* CF_PRESENTERS_PUSH_SUBSCRIPTIONS_H */
