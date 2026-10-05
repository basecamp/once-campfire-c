/* src/models/touch.h — shared record-touch helper (packet V-G).
 *
 * The avatar-delete path, the account/profile actions and the push actions
 * each bump `users.updated_at` with the same one-column UPDATE. This header
 * owns the single implementation so the call sites share one statement
 * instead of local copies. The integrator swaps the call sites; this packet
 * does not edit the callers.
 *
 * Exact SQL (byte-identical to the local copies it replaces):
 *   UPDATE "users" SET "updated_at" = ? WHERE "users"."id" = ?
 * with the reference UTC SQL text at 1 and the id at 2. This is also the
 * per-table touch shape of the landed siblings (message.c CF_MS_TOUCH,
 * room.c ROOM_STMT_TOUCH), specialized to the users table.
 *
 * Swap list for the integrator:
 *  - src/actions/users/avatars_destroy.c:72 (SQL), :81
 *    (avatars_destroy_touch_user), :126 (call) -> cf_touch_user_id
 *  - src/actions/users/profiles.c:412 (SQL), :421 (profiles_touch_user),
 *    :476 (call) -> cf_touch_user (record in hand: write->user)
 *  - src/actions/accounts/bots.c R-BOTS-TOUCH (:108-109, :339): no local
 *    copy yet, updated_at currently unchanged by avatar delete -> wire
 *    cf_touch_user_id (or cf_touch_user when the record is in hand)
 *  - src/actions/users/push_subscriptions.c:711 is the push_subscriptions
 *    table touch, out of scope for this helper.
 */
#ifndef CF_MODELS_TOUCH_H
#define CF_MODELS_TOUCH_H

#include "cf.h"
#include "models/user.h"

/* Bump `users.updated_at` to now for `user_id` inside the caller
 * transaction. CF_INVALID for a NULL transaction. Touching an absent id
 * changes no row and still reports CF_OK, like the sibling touches. */
cf_err cf_touch_user_id(cf_tx *tx, int64_t user_id);

/* Bump `users.updated_at` to now for `user->id` inside the caller
 * transaction, and set `user->updated_at` to that same value on success
 * (mirrors cf_message_touch's row half: no index or cascade here, users
 * have neither). CF_INVALID for a NULL transaction or user. */
cf_err cf_touch_user(cf_tx *tx, cf_user *user);

#endif /* CF_MODELS_TOUCH_H */
