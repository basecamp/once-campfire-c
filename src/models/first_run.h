/* src/models/first_run.h — frozen model header (D01; declarations only).
 *
 * Source: tmp/rust-ref/crates/db/src/models/first_run.rs
 * Tables: none of its own; writes accounts, users, rooms and memberships.
 *         Rails runs these as separate saves; C keeps the reference ordering
 *         inside the one caller transaction (post-commit callbacks become
 *         in-transaction work per spec 02 D02).
 */
#ifndef CF_MODELS_FIRST_RUN_H
#define CF_MODELS_FIRST_RUN_H

#include "types.h"

/* FirstRun::ACCOUNT_NAME */
#define CF_FIRST_RUN_ACCOUNT_NAME "Campfire"
/* FirstRun::FIRST_ROOM_NAME */
#define CF_FIRST_RUN_FIRST_ROOM_NAME "All Talk"

/* Rust: FirstRun::create — the singleton account "Campfire", the
 * administrator (password_digest already hashed by the A01 password service),
 * the open "All Talk" room, and the administrator's membership. Returns the
 * administrator. */
cf_err cf_first_run_create(cf_tx *tx, cf_str name, cf_str email_address, cf_str password_digest, cf_user *out);

/* Every model-functions.json symbol for this module is prototyped above. */

#endif /* CF_MODELS_FIRST_RUN_H */
