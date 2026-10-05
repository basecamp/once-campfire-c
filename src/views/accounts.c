/* src/views/accounts.c — accounts Edit page, accounts users turbo stream and
 * accounts custom styles page (packet V-C; 03-application.md A02).
 *
 * Pinned sources (executable spec; Rails resolves fixture provenance):
 *   tmp/rust-ref/crates/views/src/accounts.rs (Edit, Invite, UserPartial,
 *     NextPageContainer, UsersIndexTurboStream, CustomStylesEdit)
 *   tmp/rust-ref/crates/views/templates/accounts/edit.html
 *   tmp/rust-ref/crates/views/templates/accounts/_invite.html
 *   tmp/rust-ref/crates/views/templates/accounts/users/_user.html
 *   tmp/rust-ref/crates/views/templates/accounts/users/_next_page_container.html
 *   tmp/rust-ref/crates/views/templates/accounts/users/index.turbo_stream.html
 *   tmp/rust-ref/crates/views/templates/accounts/custom_styles/edit.html
 *   tmp/rust-ref/crates/views/src/helpers/{forms,tag,links,users,application,
 *     translations,turbo,url}.rs and src/layouts.rs (see each renderer).
 *
 * Contract (00-contracts.md, 03-application.md A02): rendering does no SQL,
 * mutation, filesystem or network work; every renderer writes into a caller
 * cf_builder with the 8 MiB output cap and returns cf_err, leaving the
 * builder at its entry length on failure. Signed/cached URL facts (account
 * logo, user avatars, invite base URL) arrive through cf_view_ctx or the
 * view models; this file never touches raw storage paths.
 *
 * View models map the finite reference structs (same fields, explicit
 * presence flags, owned vectors). They carry exactly the fields the
 * consumers' static shims assert, so the integrator can rebind mechanically:
 *
 *   actions shims (read-only)                    this file
 *   accounts.c accounts_render_edit              cf_view_accounts_edit(_frame)
 *     account row (id, name, join_code),           account_id, join_code,
 *     restrict flag, admins/members rows           restrict flag, admin/member
 *     (id/role/status/name), next_page             view users (id, name,
 *                                                  title, avatar_path, role,
 *                                                  status), next_page
 *   users.c accounts_users_render_index          cf_view_accounts_users_stream
 *     user rows (id/role/status/name),             view users + next_page
 *     next_page
 *   custom_styles.c custom_styles_render_edit    cf_view_accounts_custom_styles_edit(_frame)
 *     account.custom_styles                       has_custom_styles/custom_styles
 *   logos.c stock fallback (no view shim)        logo URL facts via ctx
 *   join_codes.c (no view shim: redirect only)   join code via model
 *
 * The Turbo-Stream actions/targets are byte-exact per the pinned
 * index.turbo_stream.html template: `replace`/`next_page_container` for the
 * page items plus `append`/`account_users` with the next-page container when
 * the page is not last. Form names/methods, DOM IDs (`account_users`,
 * `next_page_container`, `role_user_<id>`, `invite_url`, `invite_label`,
 * `account_*` field IDs) and data-controller/actions match 03-application.md
 * A02 exactly; attribute order is preserved from the reference although the
 * golden comparison sorts attributes. There are no CSRF meta tags/fields:
 * forgery protection is Sec-Fetch-Site (the golden masks drop the reference
 * fixtures' Rails tokens).
 *
 * SHIMs (marked inline; integrator requests below):
 *  - accounts_tag_id_nested: shared cf_view_form_field's tag_id keeps the
 *    trailing '_' of a nested "account[settings]" key ("account_settings__"
 *    + method) while the reference sanitize_object_name strips it
 *    ("account_settings_" + method). The settings hidden field is rendered
 *    by hand here with the reference spelling; every other field uses the
 *    shared helper (flat "account"/"user" keys are unaffected).
 *  - accounts_translation_button: translation keys "account_name" and
 *    "custom_styles" have not landed in shared src/views/translations.c
 *    (which returns CF_NOT_FOUND for them). The pinned rows for exactly
 *    those two keys live here with byte-identical button output; no other
 *    key is translated by this file.
 *
 * Integrator requests:
 *  1. Move the `cf_view_account_user*` / `cf_view_accounts_*_model`
 *     declarations and the cf_view_accounts_* prototypes below into
 *     src/views.h (exact text in the PUBLIC API block), deleting the copies
 *     here; rebind src/actions/accounts.c edit to cf_view_accounts_edit
 *     (frame requests to cf_view_accounts_edit_frame), actions/accounts/
 *     users.c index to cf_view_accounts_users_stream, and actions/accounts/
 *     custom_styles.c edit to cf_view_accounts_custom_styles_edit
 *     (_frame for Turbo-Frame requests), replacing the static shims.
 *  2. Add "account_name" and "custom_styles" rows to
 *     src/views/translations.c from the pinned translations_table.rs, then
 *     switch this file's translation_button call sites to the shared
 *     cf_view_translation_button (output is byte-identical by
 *     construction).
 *  3. Fix shared form_tag_id (src/views/render.c) to strip one trailing '_'
 *     for nested object names (reference sanitize_object_name), then switch
 *     the settings hidden field to cf_view_form_field.
 *  4. D01/A02: account_users query + last_room_visited selection already
 *     run in the actions; the Edit model takes their results
 *     (has_last_room_visited/last_room_visited_id) as facts.
 *  5. A02 bots packet owns accounts/bots/... and accounts/_help_contact.html
 *     (users#new); this file deliberately renders neither.
 *
 * c_symbols: cf_view_account_user_assign,
 * cf_view_account_user_vector_push, cf_view_account_user_dispose,
 * cf_view_account_user_vector_dispose, cf_view_accounts_edit_model_dispose,
 * cf_view_accounts_users_model_dispose,
 * cf_view_accounts_custom_styles_model_dispose, cf_view_accounts_edit,
 * cf_view_accounts_edit_frame, cf_view_accounts_users_stream,
 * cf_view_accounts_custom_styles_edit,
 * cf_view_accounts_custom_styles_edit_frame, cf_view_accounts_invite,
 * cf_view_accounts_user_partial, cf_view_accounts_next_page.
 */
#include "views/internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------- PUBLIC API
 * (Integrator: move these declarations to src/views.h verbatim and delete
 * them here. The test mirror in tests/views/test_accounts.c must move with
 * them.) */

/* accounts::UserSummary as the accounts templates read it: id, name,
 * User#title (avatar link title), fresh_user_avatar_path and the role and
 * status enums the templates branch on. Avatar/title are signed facts
 * passed in (never computed here). Owned; dispose with
 * cf_view_account_user_dispose. */
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

/* Pure copy of row facts into an owned view user (no SQL/IO; the caller
 * supplies User#title and the signed avatar path). */
cf_err cf_view_account_user_assign(cf_view_account_user *out, int64_t id,
                                   cf_span name, cf_span title,
                                   cf_span avatar_path, cf_role role,
                                   cf_status status);

/* Grow the vector and assign the facts in place (same ownership as assign). */
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

/* accounts::UsersIndexTurboStream: the page users and the next page param. */
typedef struct cf_view_accounts_users_model {
    cf_view_account_user_vector users; /* owned */
    bool has_next_page;
    cf_str next_page; /* owned when has_next_page */
} cf_view_accounts_users_model;

void cf_view_accounts_users_model_dispose(cf_view_accounts_users_model *model);

/* accounts::CustomStylesEdit: the current custom CSS, if any. */
typedef struct cf_view_accounts_custom_styles_model {
    bool has_custom_styles;
    cf_str custom_styles; /* owned when has_custom_styles */
} cf_view_accounts_custom_styles_model;

void cf_view_accounts_custom_styles_model_dispose(
    cf_view_accounts_custom_styles_model *model);

/* accounts/edit.html.erb (page and turbo-rails frame). */
cf_err cf_view_accounts_edit(const cf_view_ctx *ctx,
                             const cf_view_accounts_edit_model *model,
                             cf_builder *out);
cf_err cf_view_accounts_edit_frame(const cf_view_ctx *ctx,
                                   const cf_view_accounts_edit_model *model,
                                   cf_builder *out);

/* accounts/users/index.turbo_stream.erb (turbo stream only). */
cf_err cf_view_accounts_users_stream(const cf_view_ctx *ctx,
                                     const cf_view_accounts_users_model *model,
                                     cf_builder *out);

/* accounts/custom_styles/edit.html.erb (page and turbo-rails frame). */
cf_err cf_view_accounts_custom_styles_edit(
    const cf_view_ctx *ctx, const cf_view_accounts_custom_styles_model *model,
    cf_builder *out);
cf_err cf_view_accounts_custom_styles_edit_frame(
    const cf_view_ctx *ctx, const cf_view_accounts_custom_styles_model *model,
    cf_builder *out);

/* accounts/_invite.html.erb on its own. */
cf_err cf_view_accounts_invite(const cf_view_ctx *ctx, cf_span join_code,
                               cf_builder *out);

/* accounts/users/_user.html.erb on its own. */
cf_err cf_view_accounts_user_partial(const cf_view_ctx *ctx,
                                     const cf_view_account_user *user,
                                     cf_builder *out);

/* accounts/users/_next_page_container.html.erb on its own. */
cf_err cf_view_accounts_next_page(cf_span page, cf_builder *out);

/* ---------------------------------------------------------- END PUBLIC API */

/* ------------------------------------------------------------- disposal */

static void accounts_clear_str(cf_str *value) {
    free(value->ptr);
    memset(value, 0, sizeof *value);
}

void cf_view_account_user_dispose(cf_view_account_user *user) {
    if (user == NULL) return;
    accounts_clear_str(&user->name);
    accounts_clear_str(&user->title);
    accounts_clear_str(&user->avatar_path);
    memset(user, 0, sizeof *user);
}

void cf_view_account_user_vector_dispose(cf_view_account_user_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len; i++) {
        cf_view_account_user_dispose(&vector->items[i]);
    }
    free(vector->items);
    memset(vector, 0, sizeof *vector);
}

void cf_view_accounts_edit_model_dispose(cf_view_accounts_edit_model *model) {
    if (model == NULL) return;
    accounts_clear_str(&model->join_code);
    cf_view_account_user_vector_dispose(&model->administrators);
    cf_view_account_user_vector_dispose(&model->members);
    if (model->has_next_page) accounts_clear_str(&model->next_page);
    memset(model, 0, sizeof *model);
}

void cf_view_accounts_users_model_dispose(cf_view_accounts_users_model *model) {
    if (model == NULL) return;
    cf_view_account_user_vector_dispose(&model->users);
    if (model->has_next_page) accounts_clear_str(&model->next_page);
    memset(model, 0, sizeof *model);
}

void cf_view_accounts_custom_styles_model_dispose(
    cf_view_accounts_custom_styles_model *model) {
    if (model == NULL) return;
    if (model->has_custom_styles) accounts_clear_str(&model->custom_styles);
    memset(model, 0, sizeof *model);
}

cf_err cf_view_account_user_assign(cf_view_account_user *out, int64_t id,
                                   cf_span name, cf_span title,
                                   cf_span avatar_path, cf_role role,
                                   cf_status status) {
    if (out == NULL) return CF_INVALID;
    if ((name.len != 0 && name.ptr == NULL) ||
        (title.len != 0 && title.ptr == NULL) ||
        (avatar_path.len != 0 && avatar_path.ptr == NULL)) {
        return CF_INVALID;
    }
    memset(out, 0, sizeof *out);
    out->id = id;
    out->role = role;
    out->status = status;
    cf_err rc = cf_view_str_dup(name, &out->name);
    if (rc == CF_OK) rc = cf_view_str_dup(title, &out->title);
    if (rc == CF_OK) rc = cf_view_str_dup(avatar_path, &out->avatar_path);
    if (rc != CF_OK) {
        cf_view_account_user_dispose(out);
        return rc;
    }
    return CF_OK;
}

cf_err cf_view_account_user_vector_push(cf_view_account_user_vector *vector,
                                        int64_t id, cf_span name,
                                        cf_span title, cf_span avatar_path,
                                        cf_role role, cf_status status) {
    if (vector == NULL) return CF_INVALID;
    if (vector->len == vector->cap) {
        size_t cap = vector->cap == 0 ? 8 : vector->cap * 2;
        if (cap < vector->cap) return CF_NOMEM;
        if (cap > SIZE_MAX / sizeof *vector->items) return CF_NOMEM;
        cf_view_account_user *items =
            realloc(vector->items, cap * sizeof *items);
        if (items == NULL) return CF_NOMEM;
        vector->items = items;
        vector->cap = cap;
    }
    cf_err rc = cf_view_account_user_assign(&vector->items[vector->len], id,
                                           name, title, avatar_path, role,
                                           status);
    if (rc != CF_OK) return rc;
    vector->len++;
    return CF_OK;
}

/* ------------------------------------------------------- user predicates */

static bool accounts_is_admin(const cf_view_account_user *user) {
    return user->role == CF_ROLE_ADMINISTRATOR;
}

static bool accounts_is_bot(const cf_view_account_user *user) {
    return user->role == CF_ROLE_BOT;
}

static bool accounts_is_active(const cf_view_account_user *user) {
    return user->status == CF_STATUS_ACTIVE;
}

static bool accounts_is_banned(const cf_view_account_user *user) {
    return user->status == CF_STATUS_BANNED;
}

static bool accounts_is_current(const cf_view_ctx *ctx,
                                const cf_view_account_user *user) {
    return ctx->current_user.has_user &&
           ctx->current_user.id == user->id;
}

/* ------------------------------------------------------- local encodings */

/* `Base64.urlsafe_encode64` (padded): qr_code_helper's link encoding, as in
 * src/views/rooms.c (packets cannot share code through shared files). */
static cf_err accounts_urlsafe_base64(cf_span input, cf_builder *out) {
    static const char *const TABLE =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t i = 0;
    while (i + 2 < input.len) {
        unsigned value = ((unsigned)input.ptr[i] << 16) |
                         ((unsigned)input.ptr[i + 1] << 8) |
                         (unsigned)input.ptr[i + 2];
        char chunk[4] = {TABLE[(value >> 18) & 63], TABLE[(value >> 12) & 63],
                         TABLE[(value >> 6) & 63], TABLE[value & 63]};
        cf_err rc =
            cf_view_raw(out, (cf_span){(const unsigned char *)chunk, 4});
        if (rc != CF_OK) return rc;
        i += 3;
    }
    if (input.len - i == 1) {
        unsigned value = (unsigned)input.ptr[i] << 16;
        char chunk[4] = {TABLE[(value >> 18) & 63], TABLE[(value >> 12) & 63],
                         '=', '='};
        return cf_view_raw(out, (cf_span){(const unsigned char *)chunk, 4});
    }
    if (input.len - i == 2) {
        unsigned value = ((unsigned)input.ptr[i] << 16) |
                         ((unsigned)input.ptr[i + 1] << 8);
        char chunk[4] = {TABLE[(value >> 18) & 63], TABLE[(value >> 12) & 63],
                         TABLE[(value >> 6) & 63], '='};
        return cf_view_raw(out, (cf_span){(const unsigned char *)chunk, 4});
    }
    return CF_OK;
}

/* `CGI.escape` (ruby_compat::cgi_escape): unreserved `A-Za-z0-9_.-~` stay
 * literal, space is `+`, every other byte is %XX uppercase. */
static cf_err accounts_cgi_escape(cf_span in, cf_builder *out) {
    static const char HEX[] = "0123456789ABCDEF";
    if (in.len != 0 && in.ptr == NULL) return CF_INVALID;
    for (size_t i = 0; i < in.len; i++) {
        unsigned char c = in.ptr[i];
        cf_err rc;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-' ||
            c == '~') {
            rc = cf_view_raw(out, (cf_span){&in.ptr[i], 1});
        } else if (c == ' ') {
            rc = cf_view_str(out, "+");
        } else {
            char esc[3] = {'%', HEX[c >> 4], HEX[c & 15]};
            rc = cf_view_raw(out, (cf_span){(const unsigned char *)esc, 3});
        }
        if (rc != CF_OK) return rc;
    }
    return CF_OK;
}

/* `dom_id(record, prefix)`: "prefix_model_id" (helpers/turbo.rs). */
static cf_err accounts_dom_id(const char *model, int64_t id,
                              const char *prefix, cf_builder *out) {
    char buf[96];
    int n;
    if (prefix != NULL) {
        n = snprintf(buf, sizeof buf, "%s_%s_%lld", prefix, model,
                     (long long)id);
    } else {
        n = snprintf(buf, sizeof buf, "%s_%lld", model, (long long)id);
    }
    if (n < 0 || (size_t)n >= sizeof buf) return CF_INTERNAL;
    return cf_view_raw(out, (cf_span){(const unsigned char *)buf, (size_t)n});
}

/* `Edit#account_action`: "/account.<id>" (the singular-resource quirk). */
static cf_err accounts_account_action(int64_t account_id, cf_builder *out) {
    char buf[48];
    int n = snprintf(buf, sizeof buf, "/account.%lld", (long long)account_id);
    if (n < 0 || (size_t)n >= sizeof buf) return CF_INTERNAL;
    return cf_view_raw(out, (cf_span){(const unsigned char *)buf, (size_t)n});
}

/* `ctx.url(routes::join(join_code))`: the absolute join URL. */
static cf_err accounts_invite_url(const cf_view_ctx *ctx, cf_span join_code,
                                  cf_builder *out) {
    cf_err rc = cf_view_ctx_url(ctx, cf_span_of_lit("/join/"), out);
    if (rc == CF_OK) rc = cf_view_raw(out, join_code);
    return rc;
}

/* `link_to(url, options) { content }`: href goes after the options. */
static cf_err accounts_link_to(cf_span url, const cf_view_attrs *options,
                               cf_span content, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_view_attrs attrs;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_view_attrs_init(&attrs);
    if (options != NULL) memcpy(&attrs, options, sizeof attrs);
    CF_VIEW_TRY(cf_view_attr(&attrs, "href", url));
    CF_VIEW_TRY(cf_view_content(out, "a", &attrs, content));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* `avatar_tag(user, loading: "lazy")`: the avatar image link
 * (helpers/users.rs). */
static cf_err accounts_avatar_tag(const cf_view_ctx *ctx,
                                  const cf_view_account_user *user,
                                  cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder image = {0};
    char idbuf[32];
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "loading", "lazy"));
        CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 48));
        CF_VIEW_TRY(cf_view_image_tag(
            ctx, (cf_span){(const unsigned char *)user->avatar_path.ptr,
                           user->avatar_path.len},
            &img, &image));
    }
    {
        int n = snprintf(idbuf, sizeof idbuf, "/users/%lld",
                         (long long)user->id);
        if (n < 0 || (size_t)n >= sizeof idbuf) {
            rc = CF_INTERNAL;
            goto fail;
        }
        cf_view_attrs link;
        cf_view_attrs_init(&link);
        CF_VIEW_TRY(cf_view_attr(
            &link, "title",
            (cf_span){(const unsigned char *)user->title.ptr,
                      user->title.len}));
        CF_VIEW_TRY(cf_view_attr_cstr(&link, "class", "btn avatar"));
        CF_VIEW_TRY(cf_view_attr_cstr(&link, "data-turbo-frame", "_top"));
        CF_VIEW_TRY(accounts_link_to(
            (cf_span){(const unsigned char *)idbuf, (size_t)n}, &link,
            cf_view_span_of(&image), out));
    }
    cf_builder_dispose(&image);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&image);
    return cf_view_fail(&guard, rc);
}

/* `form.check_box(method, options, checked_value, unchecked_value)` against
 * `current` (helpers/forms.rs): hidden unchecked input first, then the box.
 * `name`/`disabled`/`form` copy from the box options; `id` is fetch-or-set
 * (an explicit id keeps its position). */
static cf_err accounts_check_box(const cf_view_form *form, const char *method,
                                 const cf_view_attrs *options,
                                 const char *checked_value,
                                 const char *unchecked_value,
                                 const char *current, cf_builder *out) {
    if (form == NULL || method == NULL || checked_value == NULL ||
        unchecked_value == NULL || current == NULL || out == NULL) {
        return CF_INVALID;
    }
    cf_err rc;
    cf_view_guard guard;
    cf_builder name = {0}, id = {0};
    cf_view_attrs box;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_view_attrs_init(&box);
    if (options != NULL) memcpy(&box, options, sizeof box);
    CF_VIEW_TRY(cf_view_attr_cstr(&box, "type", "checkbox"));
    CF_VIEW_TRY(cf_view_attr_cstr(&box, "value", checked_value));
    if (strcmp(current, checked_value) == 0) {
        CF_VIEW_TRY(cf_view_attr_cstr(&box, "checked", "checked"));
    }
    {
        /* form_tag_name/tag_id for the flat object keys this packet uses
         * ("user", "account"); nested keys are rendered by hand (see the
         * accounts_tag_id_nested SHIM in the file header). */
        CF_VIEW_TRY(cf_view_str(&name, form->param_key));
        CF_VIEW_TRY(cf_view_str(&name, "["));
        CF_VIEW_TRY(cf_view_str(&name, method));
        CF_VIEW_TRY(cf_view_str(&name, "]"));
        for (const char *p = form->param_key; *p != '\0'; p++) {
            char c = *p;
            bool keep = (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '-' || c == ':' || c == '.';
            if (keep) {
                CF_VIEW_TRY(
                    cf_view_raw(&id, (cf_span){(const unsigned char *)p, 1}));
            } else {
                CF_VIEW_TRY(cf_view_str(&id, "_"));
            }
        }
        CF_VIEW_TRY(cf_view_str(&id, "_"));
        CF_VIEW_TRY(cf_view_str(&id, method));
        CF_VIEW_TRY(cf_view_attr_set_opt(&box, "name", true,
                                         cf_view_span_of(&name)));
        /* fetch_or_set: an explicit id keeps its value and position. */
        if (cf_view_attr_get(&box, "id") == NULL) {
            CF_VIEW_TRY(cf_view_attr_set_opt(&box, "id", true,
                                             cf_view_span_of(&id)));
        }
    }
    {
        /* The hidden unchecked input copies name/disabled/form. */
        cf_view_attrs hidden;
        cf_view_attrs_init(&hidden);
        const cf_span *slot = cf_view_attr_get(&box, "name");
        if (slot != NULL) {
            CF_VIEW_TRY(cf_view_attr(&hidden, "name", *slot));
        }
        slot = cf_view_attr_get(&box, "disabled");
        if (slot != NULL) {
            /* A present flag attribute has an empty value span; re-add it
             * as a flag so it renders disabled="disabled". */
            CF_VIEW_TRY(cf_view_attr_flag(&hidden, "disabled", true));
        }
        slot = cf_view_attr_get(&box, "form");
        if (slot != NULL) {
            CF_VIEW_TRY(cf_view_attr(&hidden, "form", *slot));
        }
        CF_VIEW_TRY(cf_view_attr_cstr(&hidden, "type", "hidden"));
        CF_VIEW_TRY(cf_view_attr_cstr(&hidden, "value", unchecked_value));
        CF_VIEW_TRY(cf_view_legacy_tag(out, "input", &hidden));
    }
    CF_VIEW_TRY(cf_view_legacy_tag(out, "input", &box));
    cf_builder_dispose(&name);
    cf_builder_dispose(&id);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&name);
    cf_builder_dispose(&id);
    return cf_view_fail(&guard, rc);
}

/* `form.text_area(method, value, options)` (helpers/forms.rs): name/id
 * fetch-or-set, then `<textarea ...>\n{escaped}</textarea>`. */
static cf_err accounts_text_area(const cf_view_form *form, const char *method,
                                 cf_span value, bool has_value,
                                 const cf_view_attrs *options,
                                 cf_builder *out) {
    if (form == NULL || method == NULL || out == NULL) return CF_INVALID;
    if (value.len != 0 && value.ptr == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder name = {0}, id = {0};
    cf_view_attrs area;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_view_attrs_init(&area);
    if (options != NULL) memcpy(&area, options, sizeof area);
    CF_VIEW_TRY(cf_view_str(&name, form->param_key));
    CF_VIEW_TRY(cf_view_str(&name, "["));
    CF_VIEW_TRY(cf_view_str(&name, method));
    CF_VIEW_TRY(cf_view_str(&name, "]"));
    for (const char *p = form->param_key; *p != '\0'; p++) {
        char c = *p;
        bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == ':' ||
                    c == '.';
        if (keep) {
            CF_VIEW_TRY(
                cf_view_raw(&id, (cf_span){(const unsigned char *)p, 1}));
        } else {
            CF_VIEW_TRY(cf_view_str(&id, "_"));
        }
    }
    CF_VIEW_TRY(cf_view_str(&id, "_"));
    CF_VIEW_TRY(cf_view_str(&id, method));
    CF_VIEW_TRY(cf_view_attr_set_opt(&area, "name", true,
                                     cf_view_span_of(&name)));
    if (cf_view_attr_get(&area, "id") == NULL) {
        CF_VIEW_TRY(cf_view_attr_set_opt(&area, "id", true,
                                         cf_view_span_of(&id)));
    }
    CF_VIEW_TRY(cf_view_open_start(out, "textarea", &area));
    CF_VIEW_TRY(cf_view_str(out, "\n"));
    if (has_value) CF_VIEW_TRY(cf_view_text(out, value));
    CF_VIEW_TRY(cf_view_close_tag(out, "textarea"));
    cf_builder_dispose(&name);
    cf_builder_dispose(&id);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&name);
    cf_builder_dispose(&id);
    return cf_view_fail(&guard, rc);
}

/* --------------------------------------- translation buttons (local keys) */

/* SHIM accounts_translation_button (see the file header): the pinned
 * translations_table.rs rows for exactly the two keys this packet renders.
 * Byte-identical output to shared cf_view_translation_button by
 * construction; only these two keys are known here. */
struct accounts_translation {
    const char *language;
    const char *text;
};

#define ACCOUNTS_KEY(name) static const struct accounts_translation name##_items[]
ACCOUNTS_KEY(account_name) = {
    {"\xF0" "\x9F" "\x87" "\xBA" "\xF0" "\x9F" "\x87" "\xB8", "Name this account"},
    {"\xF0" "\x9F" "\x87" "\xAA" "\xF0" "\x9F" "\x87" "\xB8", "Nombre de esta cuenta"},
    {"\xF0" "\x9F" "\x87" "\xAB" "\xF0" "\x9F" "\x87" "\xB7", "Nommez ce compte"},
    {"\xF0" "\x9F" "\x87" "\xAE" "\xF0" "\x9F" "\x87" "\xB3", "\xE0" "\xA4" "\x87" "\xE0" "\xA4" "\xB8" " " "\xE0" "\xA4" "\x96" "\xE0" "\xA4" "\xBE" "\xE0" "\xA4" "\xA4" "\xE0" "\xA5" "\x87" " " "\xE0" "\xA4" "\x95" "\xE0" "\xA4" "\xBE" " " "\xE0" "\xA4" "\xA8" "\xE0" "\xA4" "\xBE" "\xE0" "\xA4" "\xAE" " " "\xE0" "\xA4" "\xA6" "\xE0" "\xA5" "\x87" "\xE0" "\xA4" "\x82"},
    {"\xF0" "\x9F" "\x87" "\xA9" "\xF0" "\x9F" "\x87" "\xAA", "Benennen Sie dieses Konto"},
    {"\xF0" "\x9F" "\x87" "\xA7" "\xF0" "\x9F" "\x87" "\xB7", "D" "\xC3" "\xAA" " um nome a essa conta"},
    {"\xF0" "\x9F" "\x87" "\xAF" "\xF0" "\x9F" "\x87" "\xB5", "\xE3" "\x82" "\xA2" "\xE3" "\x82" "\xAB" "\xE3" "\x82" "\xA6" "\xE3" "\x83" "\xB3" "\xE3" "\x83" "\x88" "\xE3" "\x81" "\xAB" "\xE5" "\x90" "\x8D" "\xE5" "\x89" "\x8D" "\xE3" "\x82" "\x92" "\xE4" "\xBB" "\x98" "\xE3" "\x81" "\x91" "\xE3" "\x82" "\x8B"},
};
ACCOUNTS_KEY(custom_styles) = {
    {"\xF0" "\x9F" "\x87" "\xBA" "\xF0" "\x9F" "\x87" "\xB8", "Add custom CSS styles. Use Caution: you could break things."},
    {"\xF0" "\x9F" "\x87" "\xAA" "\xF0" "\x9F" "\x87" "\xB8", "Agrega estilos CSS personalizados. Usa precauci" "\xC3" "\xB3" "n: podr" "\xC3" "\xAD" "as romper cosas."},
    {"\xF0" "\x9F" "\x87" "\xAB" "\xF0" "\x9F" "\x87" "\xB7", "Ajoutez des styles CSS personnalis" "\xC3" "\xA9" "s. Utilisez avec pr" "\xC3" "\xA9" "caution : vous pourriez casser des choses."},
    {"\xF0" "\x9F" "\x87" "\xAE" "\xF0" "\x9F" "\x87" "\xB3", "\xE0" "\xA4" "\x95" "\xE0" "\xA4" "\xB8" "\xE0" "\xA5" "\x8D" "\xE0" "\xA4" "\x9F" "\xE0" "\xA4" "\xAE" " CSS " "\xE0" "\xA4" "\xB8" "\xE0" "\xA5" "\x8D" "\xE0" "\xA4" "\x9F" "\xE0" "\xA4" "\xBE" "\xE0" "\xA4" "\x87" "\xE0" "\xA4" "\xB2" " " "\xE0" "\xA4" "\x9C" "\xE0" "\xA5" "\x8B" "\xE0" "\xA4" "\xA1" "\xE0" "\xA4" "\xBC" "\xE0" "\xA5" "\x87" "\xE0" "\xA4" "\x82" "\xE0" "\xA5" "\xA4" " " "\xE0" "\xA4" "\xB8" "\xE0" "\xA4" "\xBE" "\xE0" "\xA4" "\xB5" "\xE0" "\xA4" "\xA7" "\xE0" "\xA4" "\xBE" "\xE0" "\xA4" "\xA8" "\xE0" "\xA5" "\x80" " " "\xE0" "\xA4" "\xAC" "\xE0" "\xA4" "\xB0" "\xE0" "\xA4" "\xA4" "\xE0" "\xA5" "\x87" "\xE0" "\xA4" "\x82" ": " "\xE0" "\xA4" "\x86" "\xE0" "\xA4" "\xAA" " " "\xE0" "\xA4" "\x9A" "\xE0" "\xA5" "\x80" "\xE0" "\xA4" "\x9C" "\xE0" "\xA4" "\xBC" "\xE0" "\xA5" "\x8B" "\xE0" "\xA4" "\x82" " " "\xE0" "\xA4" "\x95" "\xE0" "\xA5" "\x8B" " " "\xE0" "\xA4" "\xA4" "\xE0" "\xA5" "\x8B" "\xE0" "\xA4" "\xA1" "\xE0" "\xA4" "\xBC" " " "\xE0" "\xA4" "\xB8" "\xE0" "\xA4" "\x95" "\xE0" "\xA4" "\xA4" "\xE0" "\xA5" "\x87" " " "\xE0" "\xA4" "\xB9" "\xE0" "\xA5" "\x88" "\xE0" "\xA4" "\x82" "\xE0" "\xA5" "\xA4"},
    {"\xF0" "\x9F" "\x87" "\xA9" "\xF0" "\x9F" "\x87" "\xAA", "F" "\xC3" "\xBC" "gen Sie benutzerdefinierte CSS-Stile hinzu. Vorsicht: Sie k" "\xC3" "\xB6" "nnten Dinge kaputt machen."},
    {"\xF0" "\x9F" "\x87" "\xA7" "\xF0" "\x9F" "\x87" "\xB7", "Adicione estilos CSS personalizados. Use com cuidado: voc" "\xC3" "\xAA" " pode quebrar coisas."},
    {"\xF0" "\x9F" "\x87" "\xAF" "\xF0" "\x9F" "\x87" "\xB5", "\xE3" "\x82" "\xAB" "\xE3" "\x82" "\xB9" "\xE3" "\x82" "\xBF" "\xE3" "\x83" "\xA0" "CSS" "\xE3" "\x82" "\xB9" "\xE3" "\x82" "\xBF" "\xE3" "\x82" "\xA4" "\xE3" "\x83" "\xAB" "\xE3" "\x82" "\x92" "\xE8" "\xBF" "\xBD" "\xE5" "\x8A" "\xA0" "\xE3" "\x80" "\x82" "\xE6" "\xB3" "\xA8" "\xE6" "\x84" "\x8F" ": " "\xE3" "\x82" "\xB5" "\xE3" "\x82" "\xA4" "\xE3" "\x83" "\x88" "\xE3" "\x81" "\x8C" "\xE5" "\xA3" "\x8A" "\xE3" "\x82" "\x8C" "\xE3" "\x82" "\x8B" "\xE5" "\x8F" "\xAF" "\xE8" "\x83" "\xBD" "\xE6" "\x80" "\xA7" "\xE3" "\x81" "\x8C" "\xE3" "\x81" "\x82" "\xE3" "\x82" "\x8A" "\xE3" "\x81" "\xBE" "\xE3" "\x81" "\x99" "\xE3" "\x80" "\x82"},
};

struct accounts_translation_set {
    const char *key;
    size_t count;
    const struct accounts_translation *items;
};

static const struct accounts_translation_set accounts_sets[] = {
    {"account_name", sizeof account_name_items / sizeof account_name_items[0],
     account_name_items},
    {"custom_styles",
     sizeof custom_styles_items / sizeof custom_styles_items[0],
     custom_styles_items},
};

static cf_err accounts_translations_for(const char *key, cf_builder *out) {
    const struct accounts_translation_set *set = NULL;
    for (size_t i = 0;
         i < sizeof accounts_sets / sizeof accounts_sets[0]; i++) {
        if (strcmp(accounts_sets[i].key, key) == 0) {
            set = &accounts_sets[i];
            break;
        }
    }
    if (set == NULL) return CF_NOT_FOUND;
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_view_attrs attrs;
    cf_view_attrs_init(&attrs);
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "language-list"));
    CF_VIEW_TRY(cf_view_open_start(out, "dl", &attrs));
    for (size_t i = 0; i < set->count; i++) {
        cf_view_attrs dt;
        cf_view_attrs_init(&dt);
        CF_VIEW_TRY(cf_view_content_text(
            out, "dt", &dt,
            (cf_span){(const unsigned char *)set->items[i].language,
                      strlen(set->items[i].language)}));
        cf_view_attrs dd;
        cf_view_attrs_init(&dd);
        CF_VIEW_TRY(cf_view_attr_cstr(&dd, "class", "margin-none"));
        CF_VIEW_TRY(cf_view_content_text(
            out, "dd", &dd,
            (cf_span){(const unsigned char *)set->items[i].text,
                      strlen(set->items[i].text)}));
    }
    CF_VIEW_TRY(cf_view_close_tag(out, "dl"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

static cf_err accounts_translation_button(const cf_view_ctx *ctx,
                                          const char *key, cf_builder *out) {
    if (ctx == NULL || key == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder inner = {0}, summary = {0}, menu = {0}, list = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(&summary,
                            "<summary class=\"btn\" tabindex=\"-1\">"));
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "size", "20"));
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "class", "color-icon"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("globe.svg"), &img,
                                      &summary));
        cf_view_attrs sr;
        cf_view_attrs_init(&sr);
        CF_VIEW_TRY(cf_view_attr_cstr(&sr, "class", "for-screen-reader"));
        CF_VIEW_TRY(cf_view_content_text(&summary, "span", &sr,
                                         cf_span_of_lit("Translate")));
    }
    CF_VIEW_TRY(cf_view_str(&summary, "</summary>"));
    CF_VIEW_TRY(cf_view_raw(&inner, cf_view_span_of(&summary)));
    cf_builder_dispose(&summary);
    CF_VIEW_TRY(accounts_translations_for(key, &list));
    {
        cf_view_attrs div;
        cf_view_attrs_init(&div);
        CF_VIEW_TRY(cf_view_attr_cstr(&div, "class",
                                      "language-list-menu shadow"));
        CF_VIEW_TRY(cf_view_attr_cstr(&div, "data-popup-target", "menu"));
        CF_VIEW_TRY(cf_view_content(&menu, "div", &div,
                                    cf_view_span_of(&list)));
    }
    CF_VIEW_TRY(cf_view_raw(&inner, cf_view_span_of(&menu)));
    cf_builder_dispose(&menu);
    cf_builder_dispose(&list);
    {
        cf_view_attrs details;
        cf_view_attrs_init(&details);
        CF_VIEW_TRY(cf_view_attr_cstr(&details, "class", "position-relative"));
        CF_VIEW_TRY(cf_view_attr_cstr(&details, "data-controller", "popup"));
        CF_VIEW_TRY(cf_view_attr_cstr(
            &details, "data-action",
            "keydown.esc->popup#close toggle->popup#toggle "
            "click@document->popup#closeOnClickOutside"));
        CF_VIEW_TRY(cf_view_attr_cstr(
            &details, "data-popup-orientation-top-class",
            "popup-orientation-top"));
        CF_VIEW_TRY(cf_view_content(out, "details", &details,
                                    cf_view_span_of(&inner)));
    }
    cf_builder_dispose(&inner);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&inner);
    cf_builder_dispose(&summary);
    cf_builder_dispose(&menu);
    cf_builder_dispose(&list);
    return cf_view_fail(&guard, rc);
}

/* -------------------------------------------------- invite fragment */

/* `accounts/_invite.html.erb`: the join link, its QR/copy/share controls and
 * the administrator's regenerate button (helpers/application.rs
 * link_to_zoom_qr_code, button_to_copy_to_clipboard,
 * web_share_session_button). */
cf_err cf_view_accounts_invite(const cf_view_ctx *ctx, cf_span join_code,
                               cf_builder *out) {
    if (ctx == NULL || out == NULL) return CF_INVALID;
    if (join_code.len != 0 && join_code.ptr == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder url = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(accounts_invite_url(ctx, join_code, &url));
    CF_VIEW_TRY(cf_view_str(
        out,
        "<div class=\"flex flex-column align-center gap\">\n"
        "  <label class=\"flex flex-column gap full-width\" "
        "style=\"--row-gap: 0.5em\">\n"
        "    <strong id=\"invite_label\" class=\"invite-label\">Share to "
        "invite more people</strong>\n"
        "    <span class=\"flex align-center gap input input--actor "
        "fill-white\">\n      "));
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", 20));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "colorize--black"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("person-add.svg"),
                                      &attrs, out));
    }
    /* The template's hand-written input: a bare `readonly`. */
    CF_VIEW_TRY(cf_view_str(
        out, "\n      <input type=\"text\" class=\"input\" id=\"invite_url\" "
             "value=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&url)));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\" aria-labelledby=\"invite_label\" readonly>\n"
        "    </span>\n"
        "  </label>\n\n"
        "  <div class=\"flex align-center gap\">\n    "));
    /* link_to_zoom_qr_code(url) */
    {
        cf_builder qr = {0};
        rc = cf_view_str(&qr, "/qr_code/");
        if (rc == CF_OK) rc = accounts_urlsafe_base64(cf_view_span_of(&url),
                                                      &qr);
        if (rc == CF_OK) {
            cf_view_attrs attrs;
            cf_view_attrs_init(&attrs);
            CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "btn"));
            CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-lightbox-target",
                                          "image"));
            CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-action",
                                          "lightbox#open"));
            CF_VIEW_TRY(cf_view_attr(&attrs, "data-lightbox-url-value",
                                     cf_view_span_of(&qr)));
            CF_VIEW_TRY(cf_view_attr(&attrs, "href", cf_view_span_of(&qr)));
            cf_builder body = {0};
            rc = cf_view_str(&body,
                             "\n      <span class=\"for-screen-reader\">Show "
                             "join link QR code</span>\n      ");
            if (rc == CF_OK) {
                cf_view_attrs img;
                cf_view_attrs_init(&img);
                rc = cf_view_attr_cstr(&img, "aria-hidden", "true");
                if (rc == CF_OK) rc = cf_view_attr_i64(&img, "size", 20);
                if (rc == CF_OK) {
                    rc = cf_view_attr_cstr(&img, "class", "colorize--black");
                }
                if (rc == CF_OK) {
                    rc = cf_view_image_tag(ctx, cf_span_of_lit("qr-code.svg"),
                                           &img, &body);
                }
            }
            if (rc == CF_OK) rc = cf_view_str(&body, "\n    ");
            if (rc == CF_OK) {
                rc = accounts_link_to(cf_view_span_of(&qr), &attrs,
                                      cf_view_span_of(&body), out);
            }
            cf_builder_dispose(&body);
        }
        cf_builder_dispose(&qr);
        if (rc != CF_OK) goto fail;
    }
    CF_VIEW_TRY(cf_view_str(out, "\n    "));
    /* button_to_copy_to_clipboard(url) */
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "btn"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-controller",
                                      "copy-to-clipboard"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-action",
                                      "copy-to-clipboard#copy"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs,
                                      "data-copy-to-clipboard-success-class",
                                      "btn--success"));
        CF_VIEW_TRY(cf_view_attr(&attrs,
                                 "data-copy-to-clipboard-content-value",
                                 cf_view_span_of(&url)));
        cf_builder body = {0};
        rc = cf_view_str(&body,
                         "\n      <span class=\"for-screen-reader\">Copy join "
                         "link</span>\n      ");
        if (rc == CF_OK) {
            cf_view_attrs img;
            cf_view_attrs_init(&img);
            rc = cf_view_attr_cstr(&img, "aria-hidden", "true");
            if (rc == CF_OK) rc = cf_view_attr_i64(&img, "size", 20);
            if (rc == CF_OK) {
                rc = cf_view_attr_cstr(&img, "class", "colorize--black");
            }
            if (rc == CF_OK) {
                rc = cf_view_image_tag(ctx,
                                       cf_span_of_lit("copy-paste.svg"), &img,
                                       &body);
            }
        }
        if (rc == CF_OK) rc = cf_view_str(&body, "\n    ");
        if (rc == CF_OK) {
            rc = cf_view_content(out, "button", &attrs,
                                 cf_view_span_of(&body));
        }
        cf_builder_dispose(&body);
        if (rc != CF_OK) goto fail;
    }
    CF_VIEW_TRY(cf_view_str(out, "\n    "));
    /* web_share_session_button(url, title, text) */
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "btn"));
        CF_VIEW_TRY(cf_view_attr_flag(&attrs, "hidden", true));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-controller", "web-share"));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-action",
                                      "web-share#share"));
        CF_VIEW_TRY(cf_view_attr(&attrs, "data-web-share-url-value",
                                 cf_view_span_of(&url)));
        CF_VIEW_TRY(cf_view_attr_cstr(
            &attrs, "data-web-share-text-value",
            "Hit this link to join me in Campfire and start chatting."));
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "data-web-share-title-value",
                                      "Link to join Campfire"));
        cf_builder body = {0};
        rc = cf_view_str(&body,
                         "\n      <span class=\"for-screen-reader\">Share "
                         "join link</span>\n      ");
        if (rc == CF_OK) {
            cf_view_attrs img;
            cf_view_attrs_init(&img);
            rc = cf_view_attr_cstr(&img, "aria-hidden", "true");
            if (rc == CF_OK) rc = cf_view_attr_i64(&img, "size", 20);
            if (rc == CF_OK) {
                rc = cf_view_attr_cstr(&img, "class", "colorize--black");
            }
            if (rc == CF_OK) {
                rc = cf_view_image_tag(ctx, cf_span_of_lit("share.svg"), &img,
                                       &body);
            }
        }
        if (rc == CF_OK) rc = cf_view_str(&body, "\n    ");
        if (rc == CF_OK) {
            rc = cf_view_content(out, "button", &attrs,
                                 cf_view_span_of(&body));
        }
        cf_builder_dispose(&body);
        if (rc != CF_OK) goto fail;
    }
    if (cf_view_ctx_can_administer(ctx)) {
        CF_VIEW_TRY(cf_view_str(out, "\n    "));
        cf_view_attrs button;
        cf_view_attrs_init(&button);
        CF_VIEW_TRY(cf_view_attr_cstr(&button, "class",
                                      "btn btn--regenerate"));
        cf_builder body = {0};
        rc = cf_view_str(&body, "\n        ");
        if (rc == CF_OK) {
            cf_view_attrs img;
            cf_view_attrs_init(&img);
            rc = cf_view_attr_cstr(&img, "aria-hidden", "true");
            if (rc == CF_OK) rc = cf_view_attr_i64(&img, "size", 20);
            if (rc == CF_OK) {
                rc = cf_view_attr_cstr(&img, "class", "colorize--black");
            }
            if (rc == CF_OK) {
                rc = cf_view_image_tag(ctx, cf_span_of_lit("refresh.svg"),
                                       &img, &body);
            }
        }
        if (rc == CF_OK) {
            rc = cf_view_str(&body,
                             "\n        <span "
                             "class=\"for-screen-reader\">Regenerate join "
                             "link</span>\n");
        }
        if (rc == CF_OK) {
            rc = cf_view_button_to(out, cf_span_of_lit("/account/join_code"),
                                   &button, (cf_span){NULL, 0},
                                   (cf_span){NULL, 0}, cf_view_span_of(&body));
        }
        cf_builder_dispose(&body);
        if (rc != CF_OK) goto fail;
    }
    CF_VIEW_TRY(cf_view_str(out, "\n  </div>\n</div>\n"));
    cf_builder_dispose(&url);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&url);
    return cf_view_fail(&guard, rc);
}

/* ----------------------------------------------------- user partial */

/* The administrator's role toggle for one roster row: a patch form whose
 * label carries the role text, the crown and the check_box. */
static cf_err accounts_role_form(const cf_view_ctx *ctx,
                                 const cf_view_account_user *user,
                                 bool is_current, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_view_form form;
    cf_builder action = {0}, domid = {0}, body = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_view_form_init(&form, &body);
    {
        char idbuf[32];
        int n = snprintf(idbuf, sizeof idbuf, "/account/users/%lld",
                         (long long)user->id);
        if (n < 0 || (size_t)n >= sizeof idbuf) {
            rc = CF_INTERNAL;
            goto fail;
        }
        CF_VIEW_TRY(cf_view_raw(&action, (cf_span){
            (const unsigned char *)idbuf, (size_t)n}));
    }
    form.action = cf_view_span_of(&action);
    form.method = "patch";
    form.param_key = "user";
    cf_view_attrs_init(&form.data);
    form.has_data = true;
    CF_VIEW_TRY(cf_view_attr_cstr(&form.data, "data-controller", "form"));
    CF_VIEW_TRY(cf_view_str(
        &body,
        "\n        <label class=\"btn txt-small flex-item-no-shrink\" "
        "for=\""));
    CF_VIEW_TRY(accounts_dom_id("user", user->id, "role", &domid));
    CF_VIEW_TRY(cf_view_html_attr(&body, cf_view_span_of(&domid)));
    CF_VIEW_TRY(cf_view_str(&body, "\">\n          <span "
                            "class=\"for-screen-reader\">Role: "));
    CF_VIEW_TRY(cf_view_str(&body, accounts_is_admin(user) ? "Administrator"
                                                           : "Member"));
    CF_VIEW_TRY(cf_view_str(&body, "</span>\n          "));
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 20));
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("crown.svg"), &img,
                                      &body));
    }
    CF_VIEW_TRY(cf_view_str(&body, "\n          "));
    {
        cf_view_attrs options;
        cf_view_attrs_init(&options);
        CF_VIEW_TRY(cf_view_attr_cstr(&options, "data-action", "form#submit"));
        CF_VIEW_TRY(cf_view_attr_flag(&options, "hidden", true));
        CF_VIEW_TRY(cf_view_attr(&options, "id", cf_view_span_of(&domid)));
        CF_VIEW_TRY(cf_view_attr_flag(&options, "disabled", is_current));
        CF_VIEW_TRY(accounts_check_box(
            &form, "role", &options, "administrator", "member",
            accounts_is_admin(user) ? "administrator" : "member", &body));
    }
    CF_VIEW_TRY(cf_view_str(&body, "\n        </label>\n"));
    form.out = out;
    CF_VIEW_TRY(cf_view_form_open(&form));
    CF_VIEW_TRY(cf_view_raw(out, cf_view_span_of(&body)));
    CF_VIEW_TRY(cf_view_str(out, "</form>"));
    cf_builder_dispose(&action);
    cf_builder_dispose(&domid);
    cf_builder_dispose(&body);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&action);
    cf_builder_dispose(&domid);
    cf_builder_dispose(&body);
    return cf_view_fail(&guard, rc);
}

/* The administrator's delete control for one roster row. */
static cf_err accounts_delete_form(const cf_view_ctx *ctx,
                                   const cf_view_account_user *user,
                                   cf_builder *out) {
    char idbuf[32];
    int n = snprintf(idbuf, sizeof idbuf, "/account/users/%lld",
                     (long long)user->id);
    if (n < 0 || (size_t)n >= sizeof idbuf) return CF_INTERNAL;
    cf_err rc;
    cf_view_guard guard;
    cf_builder body = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_view_attrs button;
    cf_view_attrs_init(&button);
    CF_VIEW_TRY(cf_view_attr_cstr(&button, "class",
                                  "btn txt-small flex-item-no-shrink "
                                  "btn--negative"));
    CF_VIEW_TRY(cf_view_attr_cstr(
        &button, "data-turbo-confirm",
        "Are you sure you want to permanently remove this person "
        "from the account? This can’t be undone."));
    CF_VIEW_TRY(cf_view_str(&body, "\n        "));
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 20));
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("minus.svg"), &img,
                                      &body));
    }
    CF_VIEW_TRY(cf_view_str(&body, "\n        <span "
                            "class=\"for-screen-reader\">Delete "));
    CF_VIEW_TRY(cf_view_text(
        &body,
        (cf_span){(const unsigned char *)user->name.ptr, user->name.len}));
    CF_VIEW_TRY(cf_view_str(&body, "</span>\n"));
    CF_VIEW_TRY(cf_view_button_to(
        out, (cf_span){(const unsigned char *)idbuf, (size_t)n}, &button,
        cf_span_of_lit("delete"), (cf_span){NULL, 0},
        cf_view_span_of(&body)));
    cf_builder_dispose(&body);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&body);
    return cf_view_fail(&guard, rc);
}

/* `accounts/users/_user.html.erb`: one roster row with the administrator's
 * role toggle and delete controls, or the current user's settings link. */
cf_err cf_view_accounts_user_partial(const cf_view_ctx *ctx,
                                     const cf_view_account_user *user,
                                     cf_builder *out) {
    if (ctx == NULL || user == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    bool can_admin = cf_view_ctx_can_administer(ctx);
    bool is_current = accounts_is_current(ctx, user);
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(
        out, "<li class=\"flex align-center gap margin-none "));
    if (accounts_is_banned(user)) CF_VIEW_TRY(cf_view_str(out, "banned"));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\">\n  <figure class=\"avatar flex-item-no-shrink\" "
        "style=\"--avatar-size: 3.75ch;\">\n    "));
    CF_VIEW_TRY(accounts_avatar_tag(ctx, user, out));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n  </figure>\n\n  <div class=\"min-width\">\n    <div "
        "class=\"overflow-ellipsis fill-shade\"><strong>"));
    CF_VIEW_TRY(cf_view_text(
        out, (cf_span){(const unsigned char *)user->name.ptr,
                       user->name.len}));
    CF_VIEW_TRY(cf_view_str(
        out,
        "</strong></div>\n  </div>\n\n  <hr class=\"separator\" "
        "aria-hidden=\"true\">\n\n"));
    if (can_admin && accounts_is_active(user)) {
        if (!accounts_is_bot(user)) {
            CF_VIEW_TRY(accounts_role_form(ctx, user, is_current, out));
        }
        if (!is_current) {
            /* Template whitespace between the forms collapses to one run. */
            CF_VIEW_TRY(cf_view_str(out, "\n      "));
            CF_VIEW_TRY(accounts_delete_form(ctx, user, out));
        }
    }
    if (is_current) {
        CF_VIEW_TRY(cf_view_str(out, "    "));
        cf_view_attrs link;
        cf_view_attrs_init(&link);
        CF_VIEW_TRY(cf_view_attr_cstr(&link, "class",
                                      "btn txt-small flex-item-no-shrink"));
        CF_VIEW_TRY(cf_view_attr_cstr(&link, "target", "_top"));
        cf_builder body = {0};
        rc = cf_view_str(&body, "\n      ");
        if (rc == CF_OK) {
            cf_view_attrs img;
            cf_view_attrs_init(&img);
            rc = cf_view_attr_i64(&img, "size", 20);
            if (rc == CF_OK) {
                rc = cf_view_attr_cstr(&img, "aria-hidden", "true");
            }
            if (rc == CF_OK) {
                rc = cf_view_image_tag(ctx, cf_span_of_lit("pencil.svg"),
                                       &img, &body);
            }
        }
        if (rc == CF_OK) {
            rc = cf_view_str(&body, "\n      <span "
                                    "class=\"for-screen-reader\">My "
                                    "settings</span>\n");
        }
        if (rc == CF_OK) {
            rc = accounts_link_to(cf_span_of_lit("/users/me/profile"), &link,
                                  cf_view_span_of(&body), out);
        }
        cf_builder_dispose(&body);
        if (rc != CF_OK) goto fail;
    }
    CF_VIEW_TRY(cf_view_str(out, "</li>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* --------------------------------------------- next-page container */

/* `accounts/users/_next_page_container.html.erb`: the lazy turbo frame that
 * loads the next user page. */
cf_err cf_view_accounts_next_page(cf_span page, cf_builder *out) {
    if (page.len != 0 && page.ptr == NULL) return CF_INVALID;
    if (out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder src = {0}, body = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(&src, "/account/users.turbo_stream?page="));
    CF_VIEW_TRY(accounts_cgi_escape(page, &src));
    CF_VIEW_TRY(cf_view_str(
        &body, "\n  <div class=\"spinner center\"></div>\n"));
    {
        cf_view_attrs frame;
        cf_view_attrs_init(&frame);
        CF_VIEW_TRY(cf_view_attr_cstr(&frame, "loading", "lazy"));
        CF_VIEW_TRY(cf_view_attr(&frame, "src", cf_view_span_of(&src)));
        CF_VIEW_TRY(cf_view_attr_cstr(&frame, "class", "flex center"));
        cf_builder id = {0};
        rc = cf_view_str(&id, "next_page_container");
        if (rc != CF_OK) {
            cf_builder_dispose(&src);
            cf_builder_dispose(&body);
            cf_builder_dispose(&id);
            goto fail;
        }
        /* turbo_frame_tag(id, src:, ...): id goes after the given options
         * (the src stays where the caller put it). */
        CF_VIEW_TRY(cf_view_attr(&frame, "id", cf_view_span_of(&id)));
        CF_VIEW_TRY(cf_view_content(out, "turbo-frame", &frame,
                                    cf_view_span_of(&body)));
        cf_builder_dispose(&id);
    }
    cf_builder_dispose(&src);
    cf_builder_dispose(&body);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&src);
    cf_builder_dispose(&body);
    return cf_view_fail(&guard, rc);
}

/* ---------------------------------------------------------- edit nav */

/* `link_back_to_last_room_visited`: the room link, else the root. */
static cf_err accounts_back_link(const cf_view_ctx *ctx,
                                 bool has_room, int64_t room_id,
                                 cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder body = {0}, dest = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    if (has_room) {
        char buf[32];
        int n = snprintf(buf, sizeof buf, "/rooms/%lld", (long long)room_id);
        if (n < 0 || (size_t)n >= sizeof buf) {
            rc = CF_INTERNAL;
            goto fail;
        }
        CF_VIEW_TRY(cf_view_raw(&dest, (cf_span){
            (const unsigned char *)buf, (size_t)n}));
    } else {
        CF_VIEW_TRY(cf_view_str(&dest, "/"));
    }
    /* link_back_to builds its content in code (no template whitespace). */
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 20));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("arrow-left.svg"),
                                      &img, &body));
    }
    {
        cf_view_attrs sr;
        cf_view_attrs_init(&sr);
        CF_VIEW_TRY(cf_view_attr_cstr(&sr, "class", "for-screen-reader"));
        CF_VIEW_TRY(cf_view_content_text(&body, "span", &sr,
                                         cf_span_of_lit("Go Back")));
    }
    {
        cf_view_attrs link;
        cf_view_attrs_init(&link);
        CF_VIEW_TRY(cf_view_attr_cstr(&link, "class", "btn"));
        CF_VIEW_TRY(accounts_link_to(cf_view_span_of(&dest), &link,
                                    cf_view_span_of(&body), out));
    }
    cf_builder_dispose(&body);
    cf_builder_dispose(&dest);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&body);
    cf_builder_dispose(&dest);
    return cf_view_fail(&guard, rc);
}

/* The Edit nav: the back button plus, for administrators, the bots and
 * custom-styles links. */
static cf_err accounts_edit_nav(const cf_view_ctx *ctx,
                                const cf_view_accounts_edit_model *model,
                                cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(out, "  <div class=\"flex-item-justify-start\">\n"
                                 "    "));
    CF_VIEW_TRY(accounts_back_link(ctx, model->has_last_room_visited,
                                   model->last_room_visited_id, out));
    CF_VIEW_TRY(cf_view_str(out, "\n  </div>\n\n"));
    if (cf_view_ctx_can_administer(ctx)) {
        CF_VIEW_TRY(cf_view_str(
            out, "    <div class=\"flex align-center gap "
                 "flex-item-justify-end\">\n      "));
        {
            cf_view_attrs link;
            cf_view_attrs_init(&link);
            CF_VIEW_TRY(cf_view_attr_cstr(&link, "class", "btn"));
            CF_VIEW_TRY(cf_view_attr_cstr(&link, "style",
                                          "view-transition-name: chat-bots"));
            cf_builder body = {0};
            rc = cf_view_str(&body, "\n        ");
            if (rc == CF_OK) {
                cf_view_attrs img;
                cf_view_attrs_init(&img);
                rc = cf_view_attr_cstr(&img, "aria-hidden", "true");
                if (rc == CF_OK) rc = cf_view_attr_i64(&img, "size", 20);
                if (rc == CF_OK) {
                    rc = cf_view_image_tag(ctx, cf_span_of_lit("bot.svg"),
                                           &img, &body);
                }
            }
            if (rc == CF_OK) {
                rc = cf_view_str(&body, "\n        <span "
                                        "class=\"for-screen-reader\">Set up "
                                        "chat bots</span>\n");
            }
            if (rc == CF_OK) {
                rc = accounts_link_to(cf_span_of_lit("/account/bots"), &link,
                                      cf_view_span_of(&body), out);
            }
            cf_builder_dispose(&body);
            if (rc != CF_OK) goto fail;
        }
        CF_VIEW_TRY(cf_view_str(out, "\n      "));
        {
            cf_view_attrs link;
            cf_view_attrs_init(&link);
            CF_VIEW_TRY(cf_view_attr_cstr(&link, "class", "btn"));
            CF_VIEW_TRY(cf_view_attr_cstr(&link, "style",
                                          "view-transition-name: "
                                          "custom-styles"));
            cf_builder body = {0};
            rc = cf_view_str(&body, "\n        ");
            if (rc == CF_OK) {
                cf_view_attrs img;
                cf_view_attrs_init(&img);
                rc = cf_view_attr_cstr(&img, "aria-hidden", "true");
                if (rc == CF_OK) rc = cf_view_attr_i64(&img, "size", 20);
                if (rc == CF_OK) {
                    rc = cf_view_image_tag(ctx, cf_span_of_lit("art.svg"),
                                           &img, &body);
                }
            }
            if (rc == CF_OK) {
                rc = cf_view_str(&body, "\n        <span "
                                        "class=\"for-screen-reader\">Custom "
                                        "styles</span>\n");
            }
            if (rc == CF_OK) {
                rc = accounts_link_to(
                    cf_span_of_lit("/account/custom_styles/edit"), &link,
                    cf_view_span_of(&body), out);
            }
            cf_builder_dispose(&body);
            if (rc != CF_OK) goto fail;
        }
        CF_VIEW_TRY(cf_view_str(out, "    </div>\n\n"));
    }
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* ------------------------------------------------- edit admin block */

/* The shared logo file input both logo forms carry. */
static cf_err accounts_logo_file_input(const cf_view_form *form) {
    cf_view_attrs attrs;
    cf_view_attrs_init(&attrs);
    cf_err rc = cf_view_attr_cstr(&attrs, "class", "input");
    if (rc == CF_OK) {
        rc = cf_view_attr_cstr(&attrs, "accept", "image/*");
    }
    if (rc == CF_OK) {
        rc = cf_view_attr_cstr(&attrs, "data-action",
                               "upload-preview#previewImage "
                               "change->form#submit");
    }
    if (rc != CF_OK) return rc;
    bool multipart = false;
    rc = cf_view_form_file_field(form, "logo", &attrs, &multipart);
    if (rc != CF_OK) return rc;
    return multipart ? CF_OK : CF_INTERNAL;
}

/* One logo upload form: `label_class` wraps the `image` and the file input
 * (`form_class` NULL omits the class). */
static cf_err accounts_logo_form(const cf_view_ctx *ctx, cf_span action,
                                 const char *form_class,
                                 const char *label_class, bool logo_image,
                                 cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder body = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_view_form form;
    cf_view_form_init(&form, &body);
    form.action = action;
    form.method = "patch";
    form.param_key = "account";
    form.class_attr = form_class;
    cf_view_attrs_init(&form.data);
    form.has_data = true;
    CF_VIEW_TRY(cf_view_attr_cstr(&form.data, "data-controller", "form"));
    CF_VIEW_TRY(cf_view_str(&body, "\n        <label class=\""));
    CF_VIEW_TRY(cf_view_str(&body, label_class));
    CF_VIEW_TRY(cf_view_str(&body, "\">\n          "));
    if (logo_image) {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "role", "presentation"));
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "data-upload-preview-target",
                                      "image"));
        CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 48));
        CF_VIEW_TRY(cf_view_image_tag(
            ctx,
            (cf_span){(const unsigned char *)ctx->account.logo_url.ptr,
                      ctx->account.logo_url.len},
            &img, &body));
    } else {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 20));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("camera.svg"),
                                      &img, &body));
    }
    CF_VIEW_TRY(cf_view_str(&body, "\n          "));
    CF_VIEW_TRY(accounts_logo_file_input(&form));
    CF_VIEW_TRY(cf_view_str(
        &body, "\n          <span class=\"for-screen-reader\">Upload "
               "logo</span>\n        </label>\n"));
    /* The file_field turned the form multipart (form_with learns it from
     * the rendered fields before wrapping). */
    form.out = out;
    form.multipart = true;
    CF_VIEW_TRY(cf_view_form_open(&form));
    CF_VIEW_TRY(cf_view_raw(out, cf_view_span_of(&body)));
    CF_VIEW_TRY(cf_view_str(out, "</form>"));
    cf_builder_dispose(&body);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&body);
    return cf_view_fail(&guard, rc);
}

/* The account-logo delete control (only with an attached logo). */
static cf_err accounts_logo_delete(const cf_view_ctx *ctx, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder body = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_view_attrs button;
    cf_view_attrs_init(&button);
    CF_VIEW_TRY(cf_view_attr_cstr(&button, "class",
                                  "btn btn--negative txt-small "
                                  "avatar__delete-btn"));
    CF_VIEW_TRY(cf_view_str(&body, "\n          "));
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 20));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("minus.svg"), &img,
                                      &body));
    }
    CF_VIEW_TRY(cf_view_str(
        &body, "\n          <span class=\"for-screen-reader\">Delete "
               "logo</span>\n"));
    CF_VIEW_TRY(cf_view_button_to(
        out,
        (cf_span){(const unsigned char *)ctx->account.logo_url.ptr,
                  ctx->account.logo_url.len},
        &button, cf_span_of_lit("delete"), (cf_span){NULL, 0},
        cf_view_span_of(&body)));
    cf_builder_dispose(&body);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&body);
    return cf_view_fail(&guard, rc);
}

/* The account name form. */
static cf_err accounts_name_form(const cf_view_ctx *ctx, cf_span action,
                                 cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_view_form form;
    cf_view_form_init(&form, out);
    form.action = action;
    form.method = "patch";
    form.param_key = "account";
    form.class_attr = "flex flex-column gap";
    cf_view_attrs_init(&form.data);
    form.has_data = true;
    CF_VIEW_TRY(cf_view_attr_cstr(&form.data, "data-controller", "form"));
    CF_VIEW_TRY(cf_view_form_open(&form));
    CF_VIEW_TRY(cf_view_str(out, "\n      <div class=\"flex align-center "
                                 "gap\">\n        "));
    CF_VIEW_TRY(accounts_translation_button(ctx, "account_name", out));
    CF_VIEW_TRY(cf_view_str(
        out, "\n\n        <label class=\"flex align-center gap "
             "flex-item-grow\">\n          "));
    {
        cf_view_attrs field;
        cf_view_attrs_init(&field);
        CF_VIEW_TRY(cf_view_attr_cstr(&field, "class", "input txt-large"));
        CF_VIEW_TRY(cf_view_attr_cstr(&field, "autocomplete", "off"));
        CF_VIEW_TRY(cf_view_attr_cstr(&field, "placeholder",
                                      "Name this account"));
        CF_VIEW_TRY(cf_view_attr_flag(&field, "autofocus", true));
        CF_VIEW_TRY(cf_view_attr_cstr(&field, "data-action",
                                      "keydown.enter->form#submit"));
        CF_VIEW_TRY(cf_view_form_field(
            &form, "text", "name", cf_str_span(ctx->account.name), true,
            &field));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n        </label>\n\n        "));
    {
        cf_builder button = {0};
        rc = cf_view_str(&button, "\n          ");
        if (rc == CF_OK) {
            cf_view_attrs img;
            cf_view_attrs_init(&img);
            rc = cf_view_attr_cstr(&img, "aria-hidden", "true");
            if (rc == CF_OK) rc = cf_view_attr_i64(&img, "size", 20);
            if (rc == CF_OK) {
                rc = cf_view_image_tag(ctx, cf_span_of_lit("check.svg"),
                                       &img, &button);
            }
        }
        if (rc == CF_OK) {
            rc = cf_view_str(&button, "\n          <span "
                                      "class=\"for-screen-reader\">Save "
                                      "changes</span>\n");
        }
        if (rc == CF_OK) {
            cf_view_attrs submit;
            cf_view_attrs_init(&submit);
            rc = cf_view_attr_cstr(&submit, "class",
                                   "btn btn--reversed center");
            if (rc == CF_OK) rc = cf_view_attr_cstr(&submit, "type",
                                                    "submit");
            if (rc == CF_OK) {
                rc = cf_view_button(out, &submit, cf_view_span_of(&button));
            }
        }
        cf_builder_dispose(&button);
        if (rc != CF_OK) goto fail;
    }
    CF_VIEW_TRY(cf_view_str(out, "      </div>\n"));
    CF_VIEW_TRY(cf_view_str(out, "</form>"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* The room-creation restriction toggle. The settings hidden field is
 * hand-rendered: shared form_tag_id keeps the nested key's trailing '_'
 * (accounts_tag_id_nested SHIM). */
static cf_err accounts_settings_form(const cf_view_ctx *ctx, cf_span action,
                                     bool restricted, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_view_form form;
    cf_view_form_init(&form, out);
    form.action = action;
    form.method = "put";
    form.param_key = "account";
    form.class_attr = "flex align-center gap center";
    cf_view_attrs_init(&form.data);
    form.has_data = true;
    CF_VIEW_TRY(cf_view_attr_cstr(&form.data, "data-controller", "form"));
    CF_VIEW_TRY(cf_view_form_open(&form));
    CF_VIEW_TRY(cf_view_str(
        out,
        "\n        <div class=\"flex-item-grow flex align-center gap "
        "txt-align-start\">\n          "));
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "class", "colorize--black"));
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 18));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("crown.svg"), &img,
                                      out));
    }
    CF_VIEW_TRY(cf_view_str(out, " Must be admin to create new rooms\n"
                                 "        </div>\n        "));
    {
        /* settings_form.hidden_field("restrict_room_creation" +
         * "_to_administrators", None, value((!restrict).to_s)): value,
         * then type, then the nested name/id. */
        cf_view_attrs hidden;
        cf_view_attrs_init(&hidden);
        CF_VIEW_TRY(cf_view_attr_cstr(&hidden, "value",
                                      restricted ? "false" : "true"));
        CF_VIEW_TRY(cf_view_attr_cstr(&hidden, "type", "hidden"));
        CF_VIEW_TRY(cf_view_attr_cstr(
            &hidden, "name",
            "account[settings][restrict_room_creation_to_administrators]"));
        CF_VIEW_TRY(cf_view_attr_cstr(
            &hidden, "id",
            "account_settings_restrict_room_creation_to_administrators"));
        CF_VIEW_TRY(cf_view_legacy_tag(out, "input", &hidden));
    }
    /* The template's hand-written checkbox: a bare `checked` when set. */
    CF_VIEW_TRY(cf_view_str(out, "\n\n        <label class=\"switch\">\n"
                                 "          <input type=\"checkbox\"\n"
                                 "                class=\"switch__input\"\n"
                                 "                "));
    if (restricted) CF_VIEW_TRY(cf_view_str(out, "checked\n                "));
    CF_VIEW_TRY(cf_view_str(out, "data-action=\"change->form#submit\">\n"
                                 "          <span "
                                 "class=\"switch__btn round\"></span>\n"
                                 "          <span "
                                 "class=\"for-screen-reader\">\n"
                                 "            Must be admin to create new "
                                 "rooms\n"
                                 "          </span>\n"
                                 "        </label>\n\n"));
    CF_VIEW_TRY(cf_view_str(out, "</form>"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* ------------------------------------------------- edit content */

/* The roster menu: the partitioned users inside the account_users frame,
 * with the group separator and the next-page loader when present. */
static cf_err accounts_users_menu(const cf_view_ctx *ctx,
                                  const cf_view_account_user *admins,
                                  size_t admin_count,
                                  const cf_view_account_user *members,
                                  size_t member_count, bool has_next_page,
                                  cf_span next_page, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(
        out, "    <menu class=\"flex flex-column gap margin-none pad\">\n"
             "      <turbo-frame id=\"account_users\">\n"));
    for (size_t i = 0; i < admin_count; i++) {
        CF_VIEW_TRY(cf_view_str(out, "        "));
        CF_VIEW_TRY(cf_view_accounts_user_partial(ctx, &admins[i], out));
    }
    if (admin_count != 0 && member_count != 0) {
        CF_VIEW_TRY(cf_view_str(
            out, "\n          <hr class=\"separator full-width\" "
                 "style=\"--border-style: solid\">\n\n"));
    }
    for (size_t i = 0; i < member_count; i++) {
        CF_VIEW_TRY(cf_view_str(out, "        "));
        CF_VIEW_TRY(cf_view_accounts_user_partial(ctx, &members[i], out));
    }
    if (has_next_page) {
        CF_VIEW_TRY(cf_view_str(out, "\n        "));
        CF_VIEW_TRY(cf_view_accounts_next_page(next_page, out));
        CF_VIEW_TRY(cf_view_str(out, "\n"));
    }
    CF_VIEW_TRY(cf_view_str(out, "      </turbo-frame>\n    </menu>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* The Edit content block. */
static cf_err accounts_edit_content(
    const cf_view_ctx *ctx, const cf_view_accounts_edit_model *model,
    cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder action = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(accounts_account_action(model->account_id, &action));
    CF_VIEW_TRY(cf_view_str(
        out, "<section class=\"panel txt-align-center flex flex-column "
             "gap\" style=\"view-transition-name: account-settings\">\n"));
    if (cf_view_ctx_can_administer(ctx)) {
        CF_VIEW_TRY(cf_view_str(
            out, "    <div class=\"align-center center avatar__form gap\" "
                 "data-controller=\"upload-preview\">\n      "));
        CF_VIEW_TRY(accounts_logo_form(ctx, cf_view_span_of(&action),
                                       "txt--medium", "btn input--file",
                                       false, out));
        CF_VIEW_TRY(cf_view_str(out, "\n      "));
        CF_VIEW_TRY(accounts_logo_form(
            ctx, cf_view_span_of(&action), NULL,
            "btn avatar input--file account-logo txt-xx-large", true, out));
        if (ctx->account.has_logo) {
            CF_VIEW_TRY(cf_view_str(out, "\n        "));
            CF_VIEW_TRY(accounts_logo_delete(ctx, out));
        }
        CF_VIEW_TRY(cf_view_str(out, "    </div>\n\n"));
        CF_VIEW_TRY(cf_view_str(out, "    "));
        CF_VIEW_TRY(accounts_name_form(ctx, cf_view_span_of(&action), out));
        CF_VIEW_TRY(cf_view_str(
            out,
            "\n    <div class=\"margin-block-start pad-block "
            "pad-inline-double fill-shade border-radius\">\n      "));
        CF_VIEW_TRY(accounts_settings_form(
            ctx, cf_view_span_of(&action),
            model->restrict_room_creation_to_administrators, out));
        CF_VIEW_TRY(cf_view_str(out, "    </div>\n\n"));
    } else {
        CF_VIEW_TRY(cf_view_str(out, "    "));
        CF_VIEW_TRY(cf_view_account_logo(ctx, "txt-xx-large center", true,
                                         out));
        CF_VIEW_TRY(cf_view_str(out, "\n    <h1 "
                                     "class=\"flex-item-grow "
                                     "txt-x-large\">"));
        CF_VIEW_TRY(cf_view_text(out, cf_str_span(ctx->account.name)));
        CF_VIEW_TRY(cf_view_str(out, "</h1>\n\n"));
    }
    CF_VIEW_TRY(cf_view_str(
        out, "  <div class=\"margin-block pad-inline pad-block-start "
             "fill-shade border-radius\">\n    "));
    CF_VIEW_TRY(cf_view_accounts_invite(
        ctx,
        (cf_span){(const unsigned char *)model->join_code.ptr,
                  model->join_code.len},
        out));
    CF_VIEW_TRY(cf_view_str(
        out, "\n\n    <hr class=\"margin-block separator full-width\" "
             "style=\"--border-style: solid\">\n\n"));
    CF_VIEW_TRY(accounts_users_menu(
        ctx, model->administrators.items, model->administrators.len,
        model->members.items, model->members.len, model->has_next_page,
        model->has_next_page
            ? (cf_span){(const unsigned char *)model->next_page.ptr,
                        model->next_page.len}
            : (cf_span){NULL, 0},
        out));
    CF_VIEW_TRY(cf_view_str(out, "  </div>\n</section>\n"));
    cf_builder_dispose(&action);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&action);
    return cf_view_fail(&guard, rc);
}

/* The Edit footer: the version badge line. */
static cf_err accounts_edit_footer(const cf_view_ctx *ctx, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(
        out, "  <div class=\"txt-align-center center margin-block-double "
             "txt-subtle\">Campfire&trade; version "));
    CF_VIEW_TRY(cf_view_version_badge(ctx, out));
    CF_VIEW_TRY(cf_view_str(out, "</div>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_accounts_edit(const cf_view_ctx *ctx,
                             const cf_view_accounts_edit_model *model,
                             cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder head = {0}, content = {0}, nav = {0}, footer = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = accounts_edit_nav(ctx, model, &nav);
    if (rc == CF_OK) rc = accounts_edit_content(ctx, model, &content);
    if (rc == CF_OK) rc = accounts_edit_footer(ctx, &footer);
    if (rc == CF_OK) {
        rc = cf_view_layout_page(ctx, cf_span_of_lit("Account settings"),
                                 true, (cf_span){NULL, 0}, false,
                                 cf_view_span_of(&head),
                                 cf_view_span_of(&content),
                                 cf_view_span_of(&nav),
                                 cf_view_span_of(&footer), (cf_span){NULL, 0},
                                 out);
    }
    cf_builder_dispose(&head);
    cf_builder_dispose(&content);
    cf_builder_dispose(&nav);
    cf_builder_dispose(&footer);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_accounts_edit_frame(const cf_view_ctx *ctx,
                                   const cf_view_accounts_edit_model *model,
                                   cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder head = {0}, content = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = accounts_edit_content(ctx, model, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_frame(ctx, cf_view_span_of(&head),
                                  cf_view_span_of(&content), out);
    }
    cf_builder_dispose(&head);
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

/* -------------------------------------------------- users stream */

/* `accounts/users/index.turbo_stream.erb`: the replace stream for the page
 * items plus the append stream with the next-page loader. Byte-exact
 * actions/targets per the pinned template. */
cf_err cf_view_accounts_users_stream(const cf_view_ctx *ctx,
                                     const cf_view_accounts_users_model *model,
                                     cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(out, "<turbo-stream action=\"replace\" "
                                 "target=\"next_page_container\">"
                                 "<template>"));
    for (size_t i = 0; i < model->users.len; i++) {
        CF_VIEW_TRY(cf_view_accounts_user_partial(ctx, &model->users.items[i],
                                                  out));
    }
    CF_VIEW_TRY(cf_view_str(out, "</template></turbo-stream>\n"));
    if (model->has_next_page) {
        CF_VIEW_TRY(cf_view_str(out, "\n<turbo-stream action=\"append\" "
                                     "target=\"account_users\">"
                                     "<template>"));
        CF_VIEW_TRY(cf_view_accounts_next_page(
            (cf_span){(const unsigned char *)model->next_page.ptr,
                      model->next_page.len},
            out));
        CF_VIEW_TRY(cf_view_str(out, "</template></turbo-stream>\n"));
    } else {
        CF_VIEW_TRY(cf_view_str(out, "\n"));
    }
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* -------------------------------------------------- custom styles */

/* The CustomStyles nav: back to the account page. */
static cf_err accounts_custom_styles_nav(const cf_view_ctx *ctx,
                                         cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder body = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(out, "  <div class=\"flex-item-justify-start\">\n"
                                 "    "));
    /* link_back_to builds its content in code (no template whitespace). */
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 20));
        CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("arrow-left.svg"),
                                      &img, &body));
    }
    {
        cf_view_attrs sr;
        cf_view_attrs_init(&sr);
        CF_VIEW_TRY(cf_view_attr_cstr(&sr, "class", "for-screen-reader"));
        CF_VIEW_TRY(cf_view_content_text(&body, "span", &sr,
                                         cf_span_of_lit("Go Back")));
    }
    {
        cf_view_attrs link;
        cf_view_attrs_init(&link);
        CF_VIEW_TRY(cf_view_attr_cstr(&link, "class", "btn"));
        CF_VIEW_TRY(accounts_link_to(cf_span_of_lit("/account/edit"), &link,
                                    cf_view_span_of(&body), out));
    }
    cf_builder_dispose(&body);
    CF_VIEW_TRY(cf_view_str(out, "\n  </div>\n\n"));
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&body);
    return cf_view_fail(&guard, rc);
}

/* `accounts/custom_styles/edit.html.erb`: the custom CSS form. */
static cf_err accounts_custom_styles_content(
    const cf_view_ctx *ctx,
    const cf_view_accounts_custom_styles_model *model, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder action = {0}, body = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_ctx_url(ctx, cf_span_of_lit("/account/custom_styles"),
                                &action));
    CF_VIEW_TRY(cf_view_str(
        out,
        "<section class=\"panel panel--wide txt-align-center flex "
        "flex-column position-relative\" "
        "style=\"view-transition-name: custom-styles\">\n  "));
    {
        cf_view_form form;
        cf_view_form_init(&form, &body);
        form.action = cf_view_span_of(&action);
        form.method = "patch";
        form.param_key = "account";
        form.class_attr = "flex flex-column gap";
        cf_view_attrs_init(&form.data);
        form.has_data = true;
        CF_VIEW_TRY(cf_view_attr_cstr(&form.data, "data-controller", "form"));
        CF_VIEW_TRY(cf_view_attr_cstr(
            &form.data, "data-action",
            "keydown.ctrl+enter->form#submit keydown.meta+enter->form#submit"));
        CF_VIEW_TRY(cf_view_str(&body, "\n    <div class=\"panel__button\">\n"
                                       "      "));
        CF_VIEW_TRY(accounts_translation_button(ctx, "custom_styles", &body));
        CF_VIEW_TRY(cf_view_str(
            &body,
            "\n    </div>\n\n"
            "    <div class=\"pad-inline-double margin-inline\">\n"
            "      <h1 class=\"margin-none\">Custom CSS</h1>\n"
            "      <p class=\"flex flex-wrap align-center justify-center gap "
            "margin-none-block-start\" style=\"--column-gap: 0.5ch; "
            "--row-gap: 0\">\n"
            "        <span>Add custom CSS styles.</span>\n"
            "        "));
        {
            cf_view_attrs img;
            cf_view_attrs_init(&img);
            CF_VIEW_TRY(cf_view_attr_cstr(&img, "class",
                                          "flex-inline colorize--black"));
            CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 16));
            CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("alert.svg"),
                                          &img, &body));
        }
        CF_VIEW_TRY(cf_view_str(
            &body,
            "\n        <span>Use Caution: you could break things.</span>\n"
            "      </p>\n"
            "    </div>\n\n"
            "    <label class=\"flex align-start gap flex-item-grow\">\n"
            "      "));
        {
            cf_view_attrs area;
            cf_view_attrs_init(&area);
            CF_VIEW_TRY(cf_view_attr_cstr(&area, "class",
                                          "input input--code txt--small"));
            CF_VIEW_TRY(cf_view_attr_cstr(&area, "placeholder",
                                          "Add CSS styles\xE2\x80\xA6"));
            CF_VIEW_TRY(cf_view_attr_cstr(&area, "autocomplete", "off"));
            CF_VIEW_TRY(cf_view_attr_cstr(&area, "spellcheck", "false"));
            CF_VIEW_TRY(cf_view_attr_cstr(&area, "autocorrect", "off"));
            CF_VIEW_TRY(cf_view_attr_cstr(&area, "autocapitalize", "off"));
            CF_VIEW_TRY(cf_view_attr_i64(&area, "rows", 16));
            CF_VIEW_TRY(accounts_text_area(
                &form, "custom_styles",
                model->has_custom_styles
                    ? (cf_span){
                          (const unsigned char *)model->custom_styles.ptr,
                          model->custom_styles.len}
                    : (cf_span){NULL, 0},
                model->has_custom_styles, &area, &body));
        }
        CF_VIEW_TRY(cf_view_str(&body, "\n    </label>\n\n    "));
        {
            cf_builder button = {0};
            rc = cf_view_str(&button, "\n      ");
            if (rc == CF_OK) {
                cf_view_attrs img;
                cf_view_attrs_init(&img);
                rc = cf_view_attr_cstr(&img, "aria-hidden", "true");
                if (rc == CF_OK) rc = cf_view_attr_i64(&img, "size", 20);
                if (rc == CF_OK) {
                    rc = cf_view_image_tag(ctx, cf_span_of_lit("check.svg"),
                                           &img, &button);
                }
            }
            if (rc == CF_OK) {
                rc = cf_view_str(&button, "\n      <span "
                                          "class=\"for-screen-reader\">Save "
                                          "changes</span>\n");
            }
            if (rc == CF_OK) {
                cf_view_attrs submit;
                cf_view_attrs_init(&submit);
                rc = cf_view_attr_cstr(&submit, "class",
                                       "btn btn--reversed center txt-large");
                if (rc == CF_OK) {
                    rc = cf_view_attr_cstr(&submit, "type", "submit");
                }
                if (rc == CF_OK) {
                    rc = cf_view_button(&body, &submit,
                                        cf_view_span_of(&button));
                }
            }
            cf_builder_dispose(&button);
            if (rc != CF_OK) goto fail;
        }
        form.out = out;
        CF_VIEW_TRY(cf_view_form_open(&form));
        CF_VIEW_TRY(cf_view_raw(out, cf_view_span_of(&body)));
        CF_VIEW_TRY(cf_view_str(out, "</form>"));
    }
    CF_VIEW_TRY(cf_view_str(out, "</section>\n"));
    cf_builder_dispose(&action);
    cf_builder_dispose(&body);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&action);
    cf_builder_dispose(&body);
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_accounts_custom_styles_edit(
    const cf_view_ctx *ctx, const cf_view_accounts_custom_styles_model *model,
    cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder head = {0}, content = {0}, nav = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = accounts_custom_styles_nav(ctx, &nav);
    if (rc == CF_OK) rc = accounts_custom_styles_content(ctx, model,
                                                         &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_page(ctx, cf_span_of_lit("Custom styles"), true,
                                 (cf_span){NULL, 0}, false,
                                 cf_view_span_of(&head),
                                 cf_view_span_of(&content),
                                 cf_view_span_of(&nav), (cf_span){NULL, 0},
                                 (cf_span){NULL, 0}, out);
    }
    cf_builder_dispose(&head);
    cf_builder_dispose(&content);
    cf_builder_dispose(&nav);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_accounts_custom_styles_edit_frame(
    const cf_view_ctx *ctx, const cf_view_accounts_custom_styles_model *model,
    cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder head = {0}, content = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = accounts_custom_styles_content(ctx, model, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_frame(ctx, cf_view_span_of(&head),
                                  cf_view_span_of(&content), out);
    }
    cf_builder_dispose(&head);
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}
