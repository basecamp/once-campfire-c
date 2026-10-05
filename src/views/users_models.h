/* src/views/users_models.h — view models + renderer declarations for the
 * users packet (Phase 4 views packet V-A).
 *
 * PENDING-DEDUP (integrator): src/actions/users.c defines
 * `cf_view_users_new_model` / `cf_view_users_show_model` locally and declares
 * the four R1/R2 renderers, and src/actions/autocompletable/users.c defines
 * `cf_view_mention_user` locally and declares
 * `cf_view_autocompletable_users_index`.  Those local definitions are
 * copied field-for-field here so this TU can define the five functions with
 * identical signatures (C struct layout compatibility keeps the cross-TU
 * calls ABI-safe).  No TU in the current tree includes both this header and
 * either action file, so there is no collision today.  The integrator should
 * move these model definitions into src/views.h (or keep this header and
 * include it from the action files) and delete the action-local copies plus
 * the test stubs in tests/actions/users_test.c and
 * tests/actions/autocompletable_users_test.c when src/views/users.c joins
 * the build.
 *
 * Reference: tmp/rust-ref/crates/views/templates/users/{new,show}.html,
 * users/autocompletables/_template.html, users/_ban_button.html (inlined),
 * users/profiles/_transfer.html (inlined), accounts/_help_contact.html
 * (inlined); tmp/rust-ref/crates/views/src/users.rs, autocompletable.rs.
 */
#ifndef CF_VIEWS_USERS_MODELS_H
#define CF_VIEWS_USERS_MODELS_H

#include "cf.h"
#include "models/types.h"
#include "views.h"

/* users/new.html.erb: the account join_code plus the optional help contact
 * (the first administrator).  Borrowed. */
typedef struct {
    cf_str join_code; /* borrowed */
    bool has_help_contact;
    const cf_view_help_contact *help_contact; /* borrowed */
} cf_view_users_new_model;

cf_err cf_view_users_new(const cf_view_ctx *ctx,
                         const cf_view_users_new_model *model,
                         cf_builder *out);
cf_err cf_view_users_new_frame(const cf_view_ctx *ctx,
                               const cf_view_users_new_model *model,
                               cf_builder *out);

/* users/show.html.erb: the user summary plus `user.transfer_id` (signed id,
 * purpose "transfer") for the inlined profiles/_transfer partial.
 * Borrowed. */
typedef struct {
    int64_t id;
    cf_str name; /* borrowed */
    cf_optional_str bio; /* borrowed */
    cf_optional_str email_address; /* borrowed */
    cf_role role;
    cf_status status;
    cf_str avatar_path; /* borrowed, fresh_user_avatar */
    cf_str transfer_id; /* borrowed, purpose transfer */
} cf_view_users_show_model;

cf_err cf_view_users_show(const cf_view_ctx *ctx,
                          const cf_view_users_show_model *model,
                          cf_builder *out);
cf_err cf_view_users_show_frame(const cf_view_ctx *ctx,
                                const cf_view_users_show_model *model,
                                cf_builder *out);

/* autocompletable/users/index.html.erb: one <lexxy-prompt-item> per user,
 * rendered with no layout.  Borrowed.
 *
 * KNOWN MODEL GAP (integrator request G1): the reference MentionUser derefs
 * to UserSummary, so avatar_tag's title is `User#title` (name plus bio, e.g.
 * "JZ – Designer").  This struct carries no bio/title, so the renderer uses
 * the bare name for the title attribute.  The action layer owns the full
 * row (auto_mention_user shapes from cf_user, which has bio) and could fill
 * a `cf_str title` field if one is added here. */
typedef struct {
    int64_t id;
    cf_str name; /* borrowed */
    cf_str avatar_path; /* borrowed */
    cf_str attachable_sgid; /* borrowed */
} cf_view_mention_user;

cf_err cf_view_autocompletable_users_index(
    const cf_view_ctx *ctx, const cf_view_mention_user *users,
    size_t users_len, cf_builder *out);

/* `users/autocompletables/_template.html` (static client-side prompt
 * template; included by rooms/directs/new.html).  Public for that packet;
 * pending integrator approval. */
cf_err cf_view_autocompletable_template(const cf_view_ctx *ctx,
                                        cf_builder *out);

#endif /* CF_VIEWS_USERS_MODELS_H */
