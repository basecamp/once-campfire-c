/* src/models/first_run.c — D01 model family "first_run".
 *
 * Source: tmp/rust-ref/crates/db/src/models/first_run.rs (pinned; SHA-256 in
 * docs/devel/implementation/contracts/reference-files.json).
 * Tables: none of its own; it writes accounts, users, rooms and memberships
 * through the sibling model functions.
 *
 * Rust runs the three saves as separate statements and defers the room grant
 * to after_commit. Per 02-data-auth.md D02 and the frozen header, the C port
 * keeps the same statement order inside the one caller transaction, so the
 * deferred callbacks (the user's open-room memberships, the open room's grant
 * to every active user and the explicit grant) become in-transaction work and
 * the committed state is unchanged. */
#include "models/first_run.h"

#include "models/account.h"
#include "models/room.h"
#include "models/user.h"

cf_err cf_first_run_create(cf_tx *tx, cf_str name, cf_str email_address,
                           cf_str password_digest, cf_user *out) {
    if (tx == NULL || out == NULL) {
        return CF_INVALID;
    }

    /* FirstRun::ACCOUNT_NAME: the singleton account (its unique
     * singleton_guard makes a second setup fail with CF_INVALID). */
    cf_account account = {0};
    cf_err err =
        cf_account_create(tx, CF_STR_LIT(CF_FIRST_RUN_ACCOUNT_NAME), &account);
    if (err != CF_OK) {
        return err;
    }
    cf_account_dispose(&account);

    /* The administrator: NewUser with name, email_address, the already
     * hashed password_digest and Role::Administrator; every other attribute
     * keeps NewUser::default (absent). */
    cf_new_user attributes = {0};
    attributes.name = name;
    attributes.email_address.present = true;
    attributes.email_address.value = email_address;
    attributes.password_digest.present = true;
    attributes.password_digest.value = password_digest;
    attributes.role = CF_ROLE_ADMINISTRATOR;

    cf_user administrator = {0};
    err = cf_user_create(tx, &attributes, &administrator);
    if (err != CF_OK) {
        return err;
    }

    /* The first (open) "All Talk" room, created by the administrator. */
    cf_optional_str room_name = {
        .present = true,
        .value = CF_STR_LIT(CF_FIRST_RUN_FIRST_ROOM_NAME),
    };
    cf_room room = {0};
    err = cf_room_create(tx, CF_ROOM_OPEN, room_name, administrator.id, &room);
    if (err != CF_OK) {
        cf_user_dispose(&administrator);
        return err;
    }

    /* `tx.after_commit(move |tx| room.grant_to(tx, &[administrator_id]))`,
     * run in-transaction per D02. An open room also grants itself to every
     * active user when created; the membership insert skips an existing
     * (room, user) row, so the administrator ends with one membership. */
    int64_t administrator_id = administrator.id;
    err = cf_room_grant_to(tx, &room, &administrator_id, 1);
    cf_room_dispose(&room);
    if (err != CF_OK) {
        cf_user_dispose(&administrator);
        return err;
    }

    *out = administrator;
    return CF_OK;
}
