/* src/actions/accounts.c — AccountsController (packet A-accounts; route IDs
 * 44 `edit`, 46/47 `update`; contracts/routes.json).
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/accounts.rs, the
 * pinned port of reference/app/controllers/accounts_controller.rb:
 *
 *   edit:   before_actions(Before::default()); Current.account (nil raises);
 *           respond_to HTML; account_users(can_administer) partitioned into
 *           administrators/members; geared page (per_page 500); framed_page
 *           accounts::Edit { account_id, join_code,
 *           restrict_room_creation_to_administrators, administrators, members,
 *           next_page }.
 *   update: before_actions(Before::default()); ensure_can_administer;
 *           Current.account;
 *           params.require(:account).permit(:name, :logo, settings: {});
 *           name = permitted name.to_s; settings = permitted settings hash
 *           mapped to (key, value.to_s default ""); logo =
 *           Assignment::from_params(...).stage(); one write of
 *           account.update(name, None, settings) + attachments::assign(logo);
 *           analyze_later(pending); redirect_to edit_account with notice.
 *
 * Callback order, permit list, status/redirect and row effects are the
 * reference's. No password hashing occurs on this path (03-application.md
 * family boundary). The C context conventions follow src/auth.h: a helper
 * that answers the request sets ctx->response and returns CF_OK, and the
 * caller stops with cf_auth_halted. Absolute URLs are PUBLIC_ORIGIN + the
 * route path.
 *
 * SHIMs (marked inline; integrator requests below):
 *  - page/arbitration: Page::number parsing reuses cf_views_integer_cast;
 *    absurd values clamp to 1_000_000_000 like the reference.
 *  - account_users for administrators is cf_user_all filtered in memory
 *    (status active/banned, role != bot, ASCII LOWER(name) order); the
 *    non-admin arm calls cf_user_active_ordered_without_bots directly.
 *    A D01 account_users query would replace the filter.
 *  - unknown settings keys are rejected with CF_INTERNAL before the write:
 *    the reference raises (500) while the C model reports CF_INVALID (400).
 *    Unknown keys are detected by field count because cf.h exposes no object
 *    key iterator (a second integrator request).
 *  - the Edit page body is a minimal deterministic render carrying the
 *    reference fields (name, join_code, restrict flag, administrator/member
 *    names, next_page); A02's accounts::Edit template replaces it.
 *  - logo Delete reuses the logos packet's destroy/touch/purge sequence
 *    (duplicated here because packets cannot share code through shared
 *    files); logo Upload returns CF_INTERNAL before the write (S02 staging
 *    has not landed) and a non-empty logo string returns CF_INTERNAL (the
 *    reference raises "expected attachable").
 *
 * Integrator requests:
 *  1. Rebind src/routes.c rows 44 -> cf_action_accounts_edit,
 *     46/47 -> cf_action_accounts_update, e.g.:
 *       {44, CF_GET, "/account/edit(.:format)", ..., cf_action_accounts_edit},
 *       {46, CF_PATCH, "/account(.:format)", ..., cf_action_accounts_update},
 *       {47, CF_PUT, "/account(.:format)", ..., cf_action_accounts_update},
 *  2. D01/A02: account_users query + accounts::Edit view (shims above).
 *  3. Object key iterator for cf.h (settings unknown-key detection).
 *
 * c_symbols: cf_action_accounts_edit, cf_action_accounts_update.
 */
#include "cf.h"

#include "http/params.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "db/db_internal.h"
#include "models/account.h"
#include "models/active_storage.h"
#include "models/user.h"
#include "views.h"
#include "views/internal.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cf_span accounts_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* Borrowed cf_str over a literal (model functions borrow their arguments). */
static cf_str accounts_cstr(const char *text) {
    return (cf_str){(char *)(uintptr_t)text, strlen(text)};
}

/* Owned copy of a span (the cf_str contract: NUL-terminated, len w/o NUL). */
static cf_err accounts_str_dup(cf_span span, cf_str *out) {
    if (out == NULL) return CF_INVALID;
    memset(out, 0, sizeof *out);
    if (span.len != 0 && span.ptr == NULL) return CF_INVALID;
    char *copy = malloc(span.len + 1);
    if (copy == NULL) return CF_NOMEM;
    if (span.len != 0) memcpy(copy, span.ptr, span.len);
    copy[span.len] = '\0';
    out->ptr = copy;
    out->len = span.len;
    return CF_OK;
}

static void accounts_str_dispose(cf_str *value) {
    if (value == NULL) return;
    free(value->ptr);
    memset(value, 0, sizeof *value);
}

/* `c.param_str(key)`: Some only for a string param. */
static bool accounts_param_str(cf_ctx *ctx, const char *name, cf_span *out) {
    const cf_param *param = cf_ctx_param(ctx, accounts_span(name));
    if (param == NULL || cf_param_type(param) != CF_PARAM_STRING) return false;
    return cf_param_string(param, out) == CF_OK;
}

/* `c.is_turbo_frame_request()`: first Turbo-Frame header, visible-ASCII/HTAB
 * only, nonempty after trimming spaces/tabs. */
static unsigned char accounts_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c - 'A' + 'a') : c;
}

static bool accounts_turbo_frame_request(const cf_request *request) {
    if (request == NULL) return false;
    cf_span value = {NULL, 0};
    bool found = false;
    for (size_t i = 0; i < request->header_count; i++) {
        const cf_header *header = &request->headers[i];
        if (header->name.len != 11) continue;
        static const char name[] = "turbo-frame";
        bool match = true;
        for (size_t k = 0; k < 11; k++) {
            if (accounts_lower(header->name.ptr[k]) != (unsigned char)name[k]) {
                match = false;
                break;
            }
        }
        if (match) {
            value = header->value;
            found = true;
            break;
        }
    }
    if (!found) return false;
    bool blank = true;
    for (size_t i = 0; i < value.len; i++) {
        unsigned char c = value.ptr[i];
        if (!((c >= 32 && c < 127) || c == '\t')) return false;
        if (c != ' ' && c != '\t') blank = false;
    }
    return !blank;
}

/* `ensure_can_administer`: 403 head unless the current user can administer
 * (no record). Runs after before_actions, before any lookup, so a non-admin
 * is 403 even for a missing account. */
static cf_err accounts_ensure_can_administer(cf_ctx *ctx, int64_t *actor_id) {
    bool found = false;
    cf_user current = {0};
    cf_err rc = cf_auth_current_user(ctx, &found, &current);
    bool allowed =
        rc == CF_OK && found &&
        cf_user_can_administer(&current, (cf_optional_i64){false, 0}, false);
    if (allowed && actor_id != NULL) *actor_id = current.id;
    cf_user_dispose(&current);
    if (rc != CF_OK) return rc;
    if (!allowed) {
        ctx->response->status = 403;
        const cf_format *format = cf_ctx_rendered_format(ctx);
        return cf_response_header(ctx->response,
                                  accounts_span("Content-Type"),
                                  accounts_span(format->string));
    }
    return CF_OK;
}

/* `Current.account` dereference: Account::first, internal error when absent
 * (the reference raises NoMethodError on nil). */
static cf_err accounts_current_account(cf_ctx *ctx, cf_account *out) {
    bool found = false;
    memset(out, 0, sizeof *out);
    cf_err rc = cf_account_first(ctx->reader, &found, out);
    if (rc != CF_OK) {
        cf_account_dispose(out);
        return rc;
    }
    if (!found) {
        cf_account_dispose(out);
        return CF_INTERNAL;
    }
    return CF_OK;
}

/* In-transaction administrator revalidation (02-data-auth.md D02): the
 * actor is reloaded on cf_tx_db and must be active and an administrator.
 * CF_FORBIDDEN rolls the mutation back with no row changes. */
static cf_err accounts_revalidate_admin(cf_tx *tx, int64_t actor_id) {
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;
    bool found = false;
    cf_user actor = {0};
    cf_err rc = cf_user_find_by_id(db, actor_id, &found, &actor);
    if (rc != CF_OK) {
        cf_user_dispose(&actor);
        return rc;
    }
    bool allowed = found && cf_user_is_active(&actor) &&
                   cf_user_can_administer(&actor, (cf_optional_i64){false, 0},
                                          false);
    cf_user_dispose(&actor);
    return allowed ? CF_OK : CF_FORBIDDEN;
}

/* `redirect_to <PUBLIC_ORIGIN><path>`: 302, text/html; charset=utf-8. */
static cf_err accounts_redirect(cf_ctx *ctx, const char *path) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    cf_builder location = {0};
    cf_err rc =
        cf_builder_append(&location, accounts_span(config->public_origin));
    if (rc == CF_OK) rc = cf_builder_append(&location, accounts_span(path));
    if (rc == CF_OK) {
        ctx->response->status = 302;
        rc = cf_response_header(ctx->response, accounts_span("Location"),
                                (cf_span){location.ptr, location.len});
        if (rc == CF_OK) {
            rc = cf_response_header(ctx->response,
                                    accounts_span("Content-Type"),
                                    accounts_span("text/html; charset=utf-8"));
        }
    }
    cf_builder_dispose(&location);
    return rc;
}

/* `Layout#page` preload links; absent when assets are unconfigured. */
static cf_err accounts_link_header(cf_ctx *ctx) {
    cf_builder links = {0};
    cf_err rc = cf_views_preload_links(&links);
    if (rc == CF_INVALID) return CF_OK;
    if (rc == CF_OK && links.len != 0) {
        rc = cf_response_header(ctx->response, accounts_span("Link"),
                                (cf_span){links.ptr, links.len});
    }
    cf_builder_dispose(&links);
    return rc;
}

/* Freeze a rendered page: status, HTML content type, preload links unless a
 * Turbo-Frame request. */
static cf_err accounts_page_response(cf_ctx *ctx, unsigned status,
                                     cf_builder *body, bool frame) {
    cf_buf *buf = NULL;
    cf_err rc = cf_builder_freeze(body, &buf);
    if (rc != CF_OK) {
        cf_builder_dispose(body);
        return rc;
    }
    ctx->response->status = status;
    rc = cf_response_body(ctx->response, buf);
    cf_buf_release(buf);
    if (rc != CF_OK) return rc;
    rc = cf_response_header(ctx->response, accounts_span("Content-Type"),
                            accounts_span("text/html; charset=utf-8"));
    if (rc != CF_OK) return rc;
    if (!frame) rc = accounts_link_header(ctx);
    return rc;
}

/* ---- page arithmetic (geared_pagination, single 500 ratio) ---------------- */

#define ACCOUNTS_PER_PAGE INT64_C(500)
#define ACCOUNTS_MAX_PAGE INT64_C(1000000000)

static int64_t accounts_page_number(cf_ctx *ctx) {
    cf_span text = {NULL, 0};
    int64_t number = 0;
    if (!accounts_param_str(ctx, "page", &text)) return 1;
    if (!cf_views_integer_cast(text, &number)) return 1;
    if (number < 1) return 1;
    if (number > ACCOUNTS_MAX_PAGE) return ACCOUNTS_MAX_PAGE;
    return number;
}

/* ---- account_users (presenters/accounts.rs) -------------------------------- */

/* ASCII-only lowercase compare (SQLite LOWER is ASCII-only). */
static int accounts_name_cmp(const cf_user *a, const cf_user *b) {
    size_t i = 0;
    for (;;) {
        unsigned char ca = i < a->name.len ? (unsigned char)a->name.ptr[i] : 0;
        unsigned char cb = i < b->name.len ? (unsigned char)b->name.ptr[i] : 0;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return ca < cb ? -1 : 1;
        if (ca == 0) break;
        i++;
    }
    if (a->id != b->id) return a->id < b->id ? -1 : 1;
    return 0;
}

static int accounts_name_qsort(const void *pa, const void *pb) {
    return accounts_name_cmp(pa, pb);
}

/* `presenters::accounts::account_users`: active+banned for administrators,
 * active only otherwise; without bots; ORDER BY LOWER(name). The
 * administrator arm filters cf_user_all in memory (SHIM: a D01 query). */
static cf_err accounts_account_users(cf_ctx *ctx, bool can_admin,
                                     cf_user_vector *out) {
    memset(out, 0, sizeof *out);
    if (!can_admin) {
        return cf_user_active_ordered_without_bots(ctx->reader, out);
    }
    cf_user_vector all = {0};
    cf_err rc = cf_user_all(ctx->reader, &all);
    if (rc != CF_OK) return rc;
    cf_user_vector kept = {0};
    for (size_t i = 0; i < all.len && rc == CF_OK; i++) {
        const cf_user *user = &all.items[i];
        bool status_ok = user->status == CF_STATUS_ACTIVE ||
                         user->status == CF_STATUS_BANNED;
        if (!status_ok || user->role == CF_ROLE_BOT) continue;
        if (kept.len == kept.cap) {
            size_t cap = kept.cap == 0 ? 16 : kept.cap * 2;
            if (cap < kept.cap) {
                rc = CF_NOMEM;
                break;
            }
            cf_user *items = realloc(kept.items, cap * sizeof *items);
            if (items == NULL) {
                rc = CF_NOMEM;
                break;
            }
            kept.items = items;
            kept.cap = cap;
        }
        kept.items[kept.len] = *user;
        memset(&all.items[i], 0, sizeof all.items[i]);
        kept.len++;
    }
    cf_user_vector_dispose(&all);
    if (rc != CF_OK) {
        cf_user_vector_dispose(&kept);
        return rc;
    }
    qsort(kept.items, kept.len, sizeof *kept.items, accounts_name_qsort);
    *out = kept;
    return CF_OK;
}

/* ---- Edit render shim (A02 accounts::Edit replaces this) ------------------ */

static cf_err accounts_emit_text(cf_builder *out, cf_span text) {
    if (out->len > CF_VIEWS_MAX_OUTPUT) return CF_LIMIT;
    return cf_html_text(out, text);
}

static cf_err accounts_render_edit(const cf_account *account,
                                   bool restrict_creation, const cf_user *admins,
                                   size_t admin_count, const cf_user *members,
                                   size_t member_count,
                                   const char *next_page, bool frame,
                                   cf_builder *out) {
    cf_err rc = CF_OK;
    if (!frame) {
        rc = cf_builder_append(out, accounts_span("<!DOCTYPE html><html><head><title>Account settings</title></head><body>"));
        if (rc != CF_OK) return rc;
    }
    char idbuf[32];
    int n = snprintf(idbuf, sizeof idbuf, "%" PRId64, account->id);
    if (n < 0 || (size_t)n >= sizeof idbuf) return CF_INTERNAL;
    rc = cf_builder_append(out, accounts_span("<section data-page=\"account-edit\" data-account-id=\""));
    if (rc == CF_OK) rc = cf_builder_append(out, accounts_span(idbuf));
    if (rc == CF_OK) {
        rc = cf_builder_append(out, accounts_span("\" data-restrict=\""));
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(out, accounts_span(restrict_creation ? "true" : "false"));
    }
    if (rc == CF_OK) rc = cf_builder_append(out, accounts_span("\">"));
    if (rc == CF_OK) rc = cf_builder_append(out, accounts_span("<h1>"));
    if (rc == CF_OK) {
        rc = accounts_emit_text(out, (cf_span){(const unsigned char *)account->name.ptr,
                                              account->name.len});
    }
    if (rc == CF_OK) rc = cf_builder_append(out, accounts_span("</h1>"));
    if (rc == CF_OK) rc = cf_builder_append(out, accounts_span("<div data-join-code=\""));
    if (rc == CF_OK) {
        rc = accounts_emit_text(out, (cf_span){(const unsigned char *)account->join_code.ptr,
                                              account->join_code.len});
    }
    if (rc == CF_OK) rc = cf_builder_append(out, accounts_span("\"></div>"));
    if (rc == CF_OK) {
        rc = cf_builder_append(out, accounts_span("<ul data-group=\"administrators\">"));
    }
    for (size_t i = 0; i < admin_count && rc == CF_OK; i++) {
        char ubuf[64];
        n = snprintf(ubuf, sizeof ubuf, "<li data-user-id=\"%" PRId64 "\">",
                     admins[i].id);
        if (n < 0 || (size_t)n >= sizeof ubuf) return CF_INTERNAL;
        rc = cf_builder_append(out, accounts_span(ubuf));
        if (rc == CF_OK) {
            rc = accounts_emit_text(out, (cf_span){(const unsigned char *)admins[i].name.ptr,
                                                  admins[i].name.len});
        }
        if (rc == CF_OK) rc = cf_builder_append(out, accounts_span("</li>"));
    }
    if (rc == CF_OK) {
        rc = cf_builder_append(out, accounts_span("</ul><ul data-group=\"members\">"));
    }
    for (size_t i = 0; i < member_count && rc == CF_OK; i++) {
        char ubuf[64];
        n = snprintf(ubuf, sizeof ubuf, "<li data-user-id=\"%" PRId64 "\">",
                     members[i].id);
        if (n < 0 || (size_t)n >= sizeof ubuf) return CF_INTERNAL;
        rc = cf_builder_append(out, accounts_span(ubuf));
        if (rc == CF_OK) {
            rc = accounts_emit_text(out, (cf_span){(const unsigned char *)members[i].name.ptr,
                                                  members[i].name.len});
        }
        if (rc == CF_OK) rc = cf_builder_append(out, accounts_span("</li>"));
    }
    if (rc == CF_OK) rc = cf_builder_append(out, accounts_span("</ul>"));
    if (rc == CF_OK && next_page != NULL) {
        rc = cf_builder_append(out, accounts_span("<turbo-frame id=\"next_page_container\" src=\"/account/users.turbo_stream?page="));
        if (rc == CF_OK) rc = cf_builder_append(out, accounts_span(next_page));
        if (rc == CF_OK) rc = cf_builder_append(out, accounts_span("\"></turbo-frame>"));
    }
    if (rc == CF_OK) rc = cf_builder_append(out, accounts_span("</section>"));
    if (rc == CF_OK && !frame) {
        rc = cf_builder_append(out, accounts_span("</body></html>"));
    }
    if (out->len > CF_VIEWS_MAX_OUTPUT) return CF_LIMIT;
    return rc;
}

cf_err cf_action_accounts_edit(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_account account = {0};
    rc = accounts_current_account(ctx, &account);
    if (rc != CF_OK) return rc;

    const cf_format *offered[1] = {&cf_format_html};
    const cf_format *chosen = NULL;
    rc = cf_ctx_respond_to(ctx, offered, 1, &chosen);
    if (rc != CF_OK) {
        cf_account_dispose(&account);
        return rc;
    }

    bool can_admin = false;
    {
        bool found = false;
        cf_user current = {0};
        rc = cf_auth_current_user(ctx, &found, &current);
        if (rc == CF_OK && found) {
            can_admin = cf_user_can_administer(&current,
                                               (cf_optional_i64){false, 0},
                                               false);
        }
        cf_user_dispose(&current);
        if (rc != CF_OK) {
            cf_account_dispose(&account);
            return rc;
        }
    }

    cf_user_vector users = {0};
    rc = accounts_account_users(ctx, can_admin, &users);
    if (rc != CF_OK) {
        cf_account_dispose(&account);
        return rc;
    }

    cf_account_settings settings = {0};
    rc = cf_account_settings_of(&account, &settings);
    bool restrict_creation = false;
    if (rc == CF_OK) {
        restrict_creation =
            cf_account_settings_restrict_room_creation_to_administrators(
                &settings);
    }
    cf_account_settings_dispose(&settings);
    if (rc != CF_OK) {
        cf_user_vector_dispose(&users);
        cf_account_dispose(&account);
        return rc;
    }

    /* Stable partition: administrators first (reference .partition). */
    size_t admin_count = 0;
    for (size_t i = 0; i < users.len; i++) {
        if (users.items[i].role == CF_ROLE_ADMINISTRATOR) {
            cf_user tmp = users.items[i];
            users.items[i] = users.items[admin_count];
            users.items[admin_count] = tmp;
            admin_count++;
        }
    }

    int64_t total = (int64_t)users.len;
    int64_t number = accounts_page_number(ctx);
    /* Geared page count over the whole list (single 500 ratio); the edit
     * page renders everyone, administrators first — the page only decides
     * whether the next-page loader follows. */
    int64_t residual = total;
    int64_t pages = 0;
    while (residual > 0) {
        pages++;
        residual -= ACCOUNTS_PER_PAGE;
    }
    if (pages < 1) pages = 1;
    bool last = number == pages;
    char nextbuf[32];
    const char *next_page = NULL;
    if (!last) {
        int n = snprintf(nextbuf, sizeof nextbuf, "%" PRId64, number + 1);
        if (n < 0 || (size_t)n >= sizeof nextbuf) {
            cf_user_vector_dispose(&users);
            cf_account_dispose(&account);
            return CF_INTERNAL;
        }
        next_page = nextbuf;
    }

    bool frame = accounts_turbo_frame_request(ctx->request);
    cf_builder body = {0};
    rc = accounts_render_edit(&account, restrict_creation, users.items,
                              admin_count, users.items + admin_count,
                              users.len - admin_count, next_page, frame,
                              &body);
    if (rc == CF_OK) rc = accounts_page_response(ctx, 200, &body, frame);
    else cf_builder_dispose(&body);
    cf_user_vector_dispose(&users);
    cf_account_dispose(&account);
    return rc;
}

/* ---- update: nested account parameters ------------------------------------ */

/* `Params::require`: present (not blank), or the false literal. */
static bool accounts_param_present(const cf_param *param) {
    if (param == NULL) return false;
    switch (cf_param_type(param)) {
    case CF_PARAM_NULL:
        return false;
    case CF_PARAM_BOOL:
        return true;
    case CF_PARAM_STRING: {
        cf_span text = {NULL, 0};
        if (cf_param_string(param, &text) != CF_OK) return false;
        for (size_t i = 0; i < text.len; i++) {
            unsigned char c = text.ptr[i];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\v' &&
                c != '\f' && c != '\r') {
                return true;
            }
        }
        return false;
    }
    case CF_PARAM_NUMBER:
    case CF_PARAM_UPLOAD:
        return true;
    case CF_PARAM_ARRAY:
    case CF_PARAM_OBJECT:
        return cf_param_count(param) > 0;
    }
    return false;
}

/* A permitted scalar (strong parameters is_permitted_scalar): anything but
 * an array or object. */
static const cf_param *accounts_permitted_scalar(const cf_param *obj,
                                                 const char *key) {
    if (obj == NULL || cf_param_type(obj) != CF_PARAM_OBJECT) return NULL;
    const cf_param *value = cf_param_field(obj, accounts_span(key));
    if (value == NULL) return NULL;
    cf_param_kind kind = cf_param_type(value);
    if (kind == CF_PARAM_ARRAY || kind == CF_PARAM_OBJECT) return NULL;
    return value;
}

typedef enum {
    ACCOUNTS_LOGO_UNCHANGED = 0,
    ACCOUNTS_LOGO_DELETE,
    ACCOUNTS_LOGO_CREATE,
    ACCOUNTS_LOGO_INVALID
} accounts_logo;

typedef struct {
    bool has_name;
    cf_str name; /* owned iff has_name */
    accounts_logo logo;
    bool settings_given; /* settings hash was given (slice non-NULL) */
    cf_account_setting setting; /* owned pair iff settings_len == 1 */
    size_t settings_len;
} accounts_fields;

static void accounts_fields_dispose(accounts_fields *fields) {
    accounts_str_dispose(&fields->name);
    free(fields->setting.key.ptr);
    free(fields->setting.value.ptr);
    memset(fields, 0, sizeof *fields);
}

static const char accounts_settings_key[] =
    "restrict_room_creation_to_administrators";

/* `params.require(:account).permit(:name, :logo, settings: {})` with the
 * reference reads. CF_INVALID is ParameterMissing (400); CF_INTERNAL is a
 * value the reference raises on (unknown settings key, non-empty logo
 * string) or media work that has not landed (logo upload). */
static cf_err accounts_parse_account(cf_ctx *ctx, accounts_fields *fields) {
    memset(fields, 0, sizeof *fields);
    const cf_param *account = cf_ctx_param(ctx, accounts_span("account"));
    if (!accounts_param_present(account)) return CF_INVALID;
    if (cf_param_type(account) != CF_PARAM_OBJECT) return CF_INVALID;

    const cf_param *name = accounts_permitted_scalar(account, "name");
    if (name != NULL) {
        cf_span text = {NULL, 0};
        cf_err rc = cf_param_to_s(name, &text);
        if (rc == CF_OK) {
            rc = accounts_str_dup(text, &fields->name);
            if (rc != CF_OK) {
                accounts_fields_dispose(fields);
                return rc;
            }
            fields->has_name = true;
        } else if (rc != CF_NOT_FOUND) {
            accounts_fields_dispose(fields);
            return rc;
        }
        /* CF_NOT_FOUND (upload) is the reference's None: unchanged. */
    }

    const cf_param *logo = accounts_permitted_scalar(account, "logo");
    if (logo == NULL) {
        fields->logo = ACCOUNTS_LOGO_UNCHANGED;
    } else {
        switch (cf_param_type(logo)) {
        case CF_PARAM_NULL:
            fields->logo = ACCOUNTS_LOGO_DELETE;
            break;
        case CF_PARAM_STRING: {
            cf_span text = {NULL, 0};
            if (cf_param_string(logo, &text) != CF_OK) {
                accounts_fields_dispose(fields);
                return CF_INTERNAL;
            }
            fields->logo = text.len == 0 ? ACCOUNTS_LOGO_DELETE
                                         : ACCOUNTS_LOGO_INVALID;
            break;
        }
        case CF_PARAM_UPLOAD:
            fields->logo = ACCOUNTS_LOGO_CREATE;
            break;
        default:
            fields->logo = ACCOUNTS_LOGO_INVALID;
            break;
        }
    }

    const cf_param *settings = cf_param_field(account,
                                              accounts_span("settings"));
    if (settings != NULL && cf_param_type(settings) == CF_PARAM_OBJECT) {
        size_t count = cf_param_count(settings);
        const cf_param *known =
            cf_param_field(settings, accounts_span(accounts_settings_key));
        if (known == NULL) {
            if (count > 0) {
                /* An unknown settings key: the reference raises (500);
                 * cf.h exposes no object key iterator, so the count is the
                 * only unknown-key signal (SHIM). */
                accounts_fields_dispose(fields);
                return CF_INTERNAL;
            }
            fields->settings_given = true;
            fields->settings_len = 0;
        } else {
            if (count > 1) {
                accounts_fields_dispose(fields);
                return CF_INTERNAL;
            }
            cf_span text = {NULL, 0};
            cf_err rc = cf_param_to_s(known, &text);
            if (rc != CF_OK && rc != CF_NOT_FOUND) {
                accounts_fields_dispose(fields);
                return rc;
            }
            if (accounts_str_dup(accounts_span(accounts_settings_key),
                                 &fields->setting.key) != CF_OK) {
                accounts_fields_dispose(fields);
                return CF_NOMEM;
            }
            if (rc == CF_OK) {
                if (accounts_str_dup(text, &fields->setting.value) != CF_OK) {
                    accounts_fields_dispose(fields);
                    return CF_NOMEM;
                }
            } else {
                /* Unstringable (upload): to_s is None, the reference's "". */
                if (accounts_str_dup(accounts_span(""),
                                     &fields->setting.value) != CF_OK) {
                    accounts_fields_dispose(fields);
                    return CF_NOMEM;
                }
            }
            fields->settings_given = true;
            fields->settings_len = 1;
        }
    }
    return CF_OK;
}

/* SHIM: `belongs_to :record, touch: true` for accounts. Fixed SQL, mirroring
 * presenters/accounts.rs touch(). Integrator request: a model or presenter
 * touch helper so actions own no SQL. */
enum { ACCOUNTS_STMT_TOUCH };
static const cf_stmt_def accounts_stmt_defs[] = {
    [ACCOUNTS_STMT_TOUCH] = {
        "UPDATE \"accounts\" SET \"updated_at\" = ? "
        "WHERE \"accounts\".\"id\" = ?"},
};
static const cf_stmt_set accounts_stmt_set = {
    accounts_stmt_defs,
    sizeof accounts_stmt_defs / sizeof accounts_stmt_defs[0]};

static cf_err accounts_touch_account(cf_tx *tx, int64_t account_id) {
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;
    int64_t now = cf_now_us(NULL);
    char now_text[CF_DB_TIME_TEXT_CAP];
    cf_err rc = cf_db_time_to_text(now, now_text);
    if (rc != CF_OK) return rc;
    sqlite3_stmt *stmt = NULL;
    rc = cf_db_stmt(db, &accounts_stmt_set, ACCOUNTS_STMT_TOUCH, &stmt);
    if (rc == CF_OK) {
        rc = cf_stmt_bind_text(
            stmt, 1,
            (cf_span){(const unsigned char *)now_text, strlen(now_text)});
    }
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 2, account_id);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step != SQLITE_DONE) rc = cf_db_err(step);
    }
    if (stmt != NULL) cf_db_stmt_done(stmt);
    return rc;
}

/* `attachments::destroy(tx, Record::account(id), "logo")`: delete the row,
 * touch the record, purge the blob after commit. Nothing happens without an
 * attachment. */
static cf_err accounts_logo_destroy(cf_tx *tx, int64_t account_id,
                                    bool *destroyed) {
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;
    *destroyed = false;
    bool found = false;
    cf_attachment attachment = {0};
    cf_err rc = cf_attachment_find_for(db, accounts_cstr("Account"),
                                       account_id, accounts_cstr("logo"),
                                       &found, &attachment);
    if (rc != CF_OK) return rc;
    if (!found) return CF_OK;
    int64_t blob_id = attachment.blob_id;
    rc = cf_attachment_delete(tx, &attachment);
    cf_attachment_dispose(&attachment);
    if (rc != CF_OK) return rc;
    rc = accounts_touch_account(tx, account_id);
    if (rc != CF_OK) return rc;
    *destroyed = true;
    cf_event event;
    memset(&event, 0, sizeof event);
    event.kind = CF_EVENT_PURGE_BLOB;
    event.blob_id = blob_id;
    return cf_tx_event(tx, event);
}

typedef struct {
    int64_t actor_id;
    int64_t account_id;
    cf_optional_str name; /* borrowed */
    const cf_account_setting *settings; /* borrowed */
    size_t settings_len;
    bool settings_given;
    accounts_logo logo;
} accounts_write_arg;

static cf_err accounts_write_cb(cf_tx *tx, void *arg) {
    const accounts_write_arg *write = arg;
    cf_err rc = accounts_revalidate_admin(tx, write->actor_id);
    if (rc != CF_OK) return rc;
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;
    cf_account account = {0};
    rc = cf_account_find(db, write->account_id, &account);
    if (rc != CF_OK) {
        cf_account_dispose(&account);
        return rc;
    }
    rc = cf_account_update(tx, &account, write->name, NULL,
                           write->settings_given ? write->settings : NULL,
                           write->settings_given ? write->settings_len : 0);
    if (rc != CF_OK) {
        cf_account_dispose(&account);
        return rc;
    }
    if (write->logo == ACCOUNTS_LOGO_DELETE) {
        bool destroyed = false;
        rc = accounts_logo_destroy(tx, account.id, &destroyed);
        (void)destroyed;
    }
    cf_account_dispose(&account);
    return rc;
}

cf_err cf_action_accounts_update(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    int64_t actor_id = 0;
    rc = accounts_ensure_can_administer(ctx, &actor_id);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_account account = {0};
    rc = accounts_current_account(ctx, &account);
    if (rc != CF_OK) return rc;
    int64_t account_id = account.id;
    cf_account_dispose(&account);

    accounts_fields fields;
    rc = accounts_parse_account(ctx, &fields);
    if (rc != CF_OK) return rc;

    /* The reference stages the logo before the write and raises on an
     * invalid value: both fail before any row changes (500). */
    if (fields.logo == ACCOUNTS_LOGO_CREATE ||
        fields.logo == ACCOUNTS_LOGO_INVALID) {
        accounts_fields_dispose(&fields);
        return CF_INTERNAL;
    }

    accounts_write_arg arg;
    memset(&arg, 0, sizeof arg);
    arg.actor_id = actor_id;
    arg.account_id = account_id;
    arg.logo = fields.logo;
    if (fields.has_name) {
        arg.name.present = true;
        arg.name.value = fields.name;
    }
    if (fields.settings_given) {
        arg.settings = fields.settings_len == 0 ? NULL : &fields.setting;
        arg.settings_len = fields.settings_len;
        arg.settings_given = true;
    }
    rc = cf_write(ctx->app, accounts_write_cb, &arg);
    accounts_fields_dispose(&fields);
    if (rc != CF_OK) return rc;

    /* `redirect_to_with(notice: "✓")`: the flash persists through A01's
     * session commit when it lands (rooms evidence notes the gap). */
    rc = cf_ctx_flash_set(ctx, accounts_span("notice"),
                           accounts_span("\xE2\x9C\x93"));
    if (rc != CF_OK) return rc;
    return accounts_redirect(ctx, "/account/edit");
}
