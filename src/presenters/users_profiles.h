/* src/presenters/users_profiles.h — the users/profiles#show presenter.
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/users/profiles.rs
 * (`show`), the pinned port of reference/app/controllers/users/profiles_controller.rb:
 * transfer_id signing, `attachments::attached_blob` and
 * `presenters::accounts::profile_memberships` in one read, then
 * `presenters::user_summary`.
 *
 * Contract: presenters load rows and assemble the view models inside one read
 * transaction; rendering performs no SQL, mutation, filesystem or network
 * work.  The view-model types and the renderers live in views.h.
 */
#ifndef CF_PRESENTERS_USERS_PROFILES_H
#define CF_PRESENTERS_USERS_PROFILES_H

#include "views.h"

/* `users/profiles#show`: the signed transfer id, the avatar attachment check,
 * `profile_memberships` (with_ordered_room, direct/shared partition, display
 * names, param keys and involvements) and the UserSummary (token-based
 * fresh_user_avatar_path), inside one read transaction over ctx->reader.
 * `user` is the authenticated row the controller resolved. */
cf_err cf_presenter_users_profile(cf_ctx *ctx, const cf_user *user,
                                  cf_view_users_profile_model *out);

#endif /* CF_PRESENTERS_USERS_PROFILES_H */
