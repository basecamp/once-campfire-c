/* src/presenters/accounts.h — the accounts Edit presenter.
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/accounts.rs (`edit`),
 * the pinned port of reference/app/controllers/accounts_controller.rb:
 * `Current.account`, `account_users` (active+banned for administrators,
 * active otherwise, bots dropped, LOWER(name) order), the
 * administrators-first partition, `user_summary` per row (User#title and
 * fresh_user_avatar_path), the geared page's `next_param` and
 * `last_room_visited_in` for link_back_to_last_room_visited.
 *
 * Contract: the presenter loads rows and assembles the view model inside one
 * read transaction on ctx->reader; rendering afterwards performs no SQL,
 * mutation, filesystem or network work.
 *
 * PENDING-DEDUP (integrator): src/views/accounts.c defines the
 * cf_view_account_user* / cf_view_accounts_edit_model types and the
 * cf_view_accounts_edit(_frame) renderers in its PUBLIC API block, and the
 * test mirror in tests/views/test_accounts.c repeats them; the integrator
 * moves that block into src/views.h and deletes the copies here (the
 * users_models.h precedent: the definitions below are field-for-field
 * identical, so the cross-TU calls are ABI-safe).  Only the view API this
 * presenter and its action call is declared here.
 */
#ifndef CF_PRESENTERS_ACCOUNTS_H
#define CF_PRESENTERS_ACCOUNTS_H

#include "views.h"

/* accounts::UserSummary as the accounts templates read it: id, name,
 * User#title (avatar link title), fresh_user_avatar_path and the role and
 * status enums the templates branch on.  Owned; dispose with
 * cf_view_account_user_dispose (or the vector disposer). */
typedef struct cf_view_account_user {
    int64_t id;
    cf_str name;        /* owned */
    cf_str title;       /* owned: User#title */
    cf_str avatar_path; /* owned: fresh_user_avatar_path */
    cf_role role;       /* CF_ROLE_MEMBER/ADMINISTRATOR/BOT */
    cf_status status;   /* CF_STATUS_ACTIVE/DEACTIVATED/BANNED */
} cf_view_account_user;

void cf_view_account_user_dispose(cf_view_account_user *user);

typedef struct cf_view_account_user_vector {
    cf_view_account_user *items;
    size_t len, cap;
} cf_view_account_user_vector;

void cf_view_account_user_vector_dispose(cf_view_account_user_vector *vector);

/* Grow the vector and assign owned copies of the facts in place. */
cf_err cf_view_account_user_vector_push(cf_view_account_user_vector *vector,
                                        int64_t id, cf_span name,
                                        cf_span title, cf_span avatar_path,
                                        cf_role role, cf_status status);

/* accounts::Edit: the account id (the singular-resource form action
 * "/account.<id>"), the join code, the room-creation setting, the
 * partitioned user lists and the next user-page param, plus the resolved
 * last-room link for the nav back button (None links to the root). */
typedef struct cf_view_accounts_edit_model {
    int64_t account_id;
    cf_str join_code; /* owned */
    bool restrict_room_creation_to_administrators;
    cf_view_account_user_vector administrators; /* owned */
    cf_view_account_user_vector members;        /* owned */
    bool has_last_room_visited;
    int64_t last_room_visited_id;
    bool has_next_page;
    cf_str next_page; /* owned when has_next_page */
} cf_view_accounts_edit_model;

void cf_view_accounts_edit_model_dispose(cf_view_accounts_edit_model *model);

/* accounts/edit.html.erb (page and turbo-rails frame). */
cf_err cf_view_accounts_edit(const cf_view_ctx *ctx,
                             const cf_view_accounts_edit_model *model,
                             cf_builder *out);
cf_err cf_view_accounts_edit_frame(const cf_view_ctx *ctx,
                                   const cf_view_accounts_edit_model *model,
                                   cf_builder *out);

/* `AccountsController#edit`: `account_users(can_administer)` partitioned into
 * administrators/members (each side in LOWER(name) order), each row mapped
 * through `user_summary`; the geared page (single 500 ratio) decides
 * `next_page`; `last_room_visited_in(current_user)` fills the nav link.  All
 * reads run inside one transaction on ctx->reader.  `account` and
 * `current_user` are the rows the controller already resolved; the presenter
 * does not re-read them. */
cf_err cf_presenter_accounts_edit(cf_ctx *ctx, const cf_account *account,
                                  const cf_user *current_user,
                                  bool can_administer,
                                  cf_view_accounts_edit_model *out);

#endif /* CF_PRESENTERS_ACCOUNTS_H */
