/* src/actions/accounts/custom_styles.c — Accounts::CustomStylesController
 * (packet A-accounts-custom_styles; route IDs 40 `edit`, 41/42 `update`;
 * contracts/routes.json).
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/accounts/
 * custom_styles.rs, the pinned port of reference/app/controllers/accounts/
 * custom_styles_controller.rb:
 *
 *   edit:   before_actions(Before::default()); ensure_can_administer;
 *           Current.account; respond_to HTML; framed_page
 *           accounts::CustomStylesEdit { custom_styles }.
 *   update: before_actions(Before::default()); ensure_can_administer;
 *           Current.account;
 *           params.require(:account).permit(:custom_styles);
 *           custom_styles = permitted.contains_key ? Some(to_s) : None;
 *           write account.update(None, custom_styles, None);
 *           redirect_to edit_account_custom_styles with notice.
 *
 * Callback order, permit behavior (absent key leaves the column unchanged;
 * a present null becomes to_s "" — only an unstringable upload writes
 * NULL), status/redirect and the row effect are the reference's.
 *
 * Integrator requests: rebind src/routes.c rows
 * 40 -> cf_action_accounts_custom_styles_edit,
 * 41/42 -> cf_action_accounts_custom_styles_update, e.g.:
 *   {40, CF_GET, "/account/custom_styles/edit(.:format)", ...,
 *    cf_action_accounts_custom_styles_edit},
 *   {41, CF_PATCH, "/account/custom_styles(.:format)", ...,
 *    cf_action_accounts_custom_styles_update},
 *   {42, CF_PUT, "/account/custom_styles(.:format)", ...,
 *    cf_action_accounts_custom_styles_update},
 *
 * c_symbols: cf_action_accounts_custom_styles_edit,
 * cf_action_accounts_custom_styles_update.
 */
#include "cf.h"

#include "http/params.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "models/account.h"
#include "models/user.h"
#include "views.h"
#include "views/accounts_custom_styles.h"

#include <stdlib.h>
#include <string.h>

static cf_span custom_styles_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `c.is_turbo_frame_request()`: first Turbo-Frame header, visible-ASCII/HTAB
 * only, nonempty after trimming spaces/tabs. */
static unsigned char custom_styles_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c - 'A' + 'a') : c;
}

static bool custom_styles_turbo_frame_request(const cf_request *request) {
    if (request == NULL) return false;
    cf_span value = {NULL, 0};
    bool found = false;
    for (size_t i = 0; i < request->header_count; i++) {
        const cf_header *header = &request->headers[i];
        if (header->name.len != 11) continue;
        static const char name[] = "turbo-frame";
        bool match = true;
        for (size_t k = 0; k < 11; k++) {
            if (custom_styles_lower(header->name.ptr[k]) !=
                (unsigned char)name[k]) {
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
 * (no record). */
static cf_err custom_styles_ensure_can_administer(cf_ctx *ctx,
                                                 int64_t *actor_id) {
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
                                  custom_styles_span("Content-Type"),
                                  custom_styles_span(format->string));
    }
    return CF_OK;
}

/* `Current.account` dereference: Account::first, internal error when absent. */
static cf_err custom_styles_current_account(cf_ctx *ctx, cf_account *out) {
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

/* In-transaction administrator revalidation (02-data-auth.md D02). */
static cf_err custom_styles_revalidate_admin(cf_tx *tx, int64_t actor_id) {
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

/* `Layout#page` preload links; absent when assets are unconfigured. */
static cf_err custom_styles_link_header(cf_ctx *ctx) {
    cf_builder links = {0};
    cf_err rc = cf_views_preload_links(&links);
    if (rc == CF_INVALID) return CF_OK;
    if (rc == CF_OK && links.len != 0) {
        rc = cf_response_header(ctx->response, custom_styles_span("Link"),
                                (cf_span){links.ptr, links.len});
    }
    cf_builder_dispose(&links);
    return rc;
}

static cf_err custom_styles_page_response(cf_ctx *ctx, unsigned status,
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
    rc = cf_response_header(ctx->response,
                            custom_styles_span("Content-Type"),
                            custom_styles_span("text/html; charset=utf-8"));
    if (rc != CF_OK) return rc;
    if (!frame) rc = custom_styles_link_header(ctx);
    return rc;
}

/* ---- edit --------------------------------------------------------------- */

static cf_err custom_styles_render_edit(cf_ctx *ctx, const cf_account *account,
                                        bool frame, cf_builder *out) {
    cf_view_layout_model layout = {0};
    cf_err rc = cf_presenter_layout_load(ctx, cf_ctx_platform(ctx), &layout);
    if (rc != CF_OK) return rc;
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, &layout);
    /* The account owns these bytes through the synchronous render. */
    cf_view_accounts_custom_styles_model model = {
        .has_custom_styles = account->custom_styles.present,
        .custom_styles = account->custom_styles.value,
    };
    rc = frame ? cf_view_accounts_custom_styles_edit_frame(&view_ctx, &model,
                                                           out)
               : cf_view_accounts_custom_styles_edit(&view_ctx, &model, out);
    cf_view_layout_model_dispose(&layout);
    return rc;
}

cf_err cf_action_accounts_custom_styles_edit(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    rc = custom_styles_ensure_can_administer(ctx, NULL);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_account account = {0};
    rc = custom_styles_current_account(ctx, &account);
    if (rc != CF_OK) return rc;

    const cf_format *offered[1] = {&cf_format_html};
    const cf_format *chosen = NULL;
    rc = cf_ctx_respond_to(ctx, offered, 1, &chosen);
    if (rc != CF_OK) {
        cf_account_dispose(&account);
        return rc;
    }

    bool frame = custom_styles_turbo_frame_request(ctx->request);
    cf_builder body = {0};
    rc = custom_styles_render_edit(ctx, &account, frame, &body);
    cf_account_dispose(&account);
    if (rc == CF_OK) rc = custom_styles_page_response(ctx, 200, &body, frame);
    else cf_builder_dispose(&body);
    return rc;
}

/* ---- update --------------------------------------------------------------- */

/* `Params::require`: present (not blank), or the false literal. */
static bool custom_styles_param_present(const cf_param *param) {
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

typedef struct {
    bool given; /* the key was permitted (column changes) */
    bool is_null; /* given but unstringable: write NULL */
    cf_str value; /* owned iff given && !is_null */
} custom_styles_value;

static void custom_styles_value_dispose(custom_styles_value *value) {
    if (value == NULL) return;
    free(value->value.ptr);
    memset(value, 0, sizeof *value);
}

/* `params.require(:account).permit(:custom_styles)` with the reference read:
 * contains_key ? Some(to_s) : None. CF_INVALID is ParameterMissing (400). */
static cf_err custom_styles_parse_account(cf_ctx *ctx,
                                          custom_styles_value *out) {
    memset(out, 0, sizeof *out);
    const cf_param *account = cf_ctx_param(ctx, custom_styles_span("account"));
    if (!custom_styles_param_present(account)) return CF_INVALID;
    if (cf_param_type(account) != CF_PARAM_OBJECT) return CF_INVALID;

    /* permit(:custom_styles) keeps permitted scalars only; arrays and
     * objects are dropped (the key reads as absent). */
    const cf_param *field =
        cf_param_field(account, custom_styles_span("custom_styles"));
    if (field != NULL) {
        cf_param_kind kind = cf_param_type(field);
        if (kind == CF_PARAM_ARRAY || kind == CF_PARAM_OBJECT) {
            field = NULL;
        }
    }
    if (field == NULL) return CF_OK;

    out->given = true;
    cf_span text = {NULL, 0};
    cf_err rc = cf_param_to_s(field, &text);
    if (rc == CF_NOT_FOUND) {
        /* Unstringable (upload): the reference's None writes NULL. */
        out->is_null = true;
        return CF_OK;
    }
    if (rc != CF_OK) {
        custom_styles_value_dispose(out);
        return rc;
    }
    out->value.ptr = malloc(text.len + 1);
    if (out->value.ptr == NULL) {
        custom_styles_value_dispose(out);
        return CF_NOMEM;
    }
    if (text.len != 0) {
        if (text.ptr == NULL) {
            custom_styles_value_dispose(out);
            return CF_INVALID;
        }
        memcpy(out->value.ptr, text.ptr, text.len);
    }
    out->value.ptr[text.len] = '\0';
    out->value.len = text.len;
    return CF_OK;
}

typedef struct {
    int64_t actor_id;
    int64_t account_id;
    bool given;
    bool is_null;
    cf_str value; /* borrowed */
} custom_styles_write_arg;

static cf_err custom_styles_write_cb(cf_tx *tx, void *arg) {
    const custom_styles_write_arg *write = arg;
    cf_err rc = custom_styles_revalidate_admin(tx, write->actor_id);
    if (rc != CF_OK) return rc;
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;
    cf_account account = {0};
    rc = cf_account_find(db, write->account_id, &account);
    if (rc != CF_OK) {
        cf_account_dispose(&account);
        return rc;
    }
    const cf_optional_str *styles = NULL;
    cf_optional_str value;
    memset(&value, 0, sizeof value);
    if (write->given) {
        if (write->is_null) {
            value.present = false;
        } else {
            value.present = true;
            value.value = write->value;
        }
        styles = &value;
    }
    cf_optional_str absent_name;
    memset(&absent_name, 0, sizeof absent_name);
    rc = cf_account_update(tx, &account, absent_name, styles, NULL, 0);
    cf_account_dispose(&account);
    return rc;
}

cf_err cf_action_accounts_custom_styles_update(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    int64_t actor_id = 0;
    rc = custom_styles_ensure_can_administer(ctx, &actor_id);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_account account = {0};
    rc = custom_styles_current_account(ctx, &account);
    if (rc != CF_OK) return rc;
    int64_t account_id = account.id;
    cf_account_dispose(&account);

    custom_styles_value parsed;
    rc = custom_styles_parse_account(ctx, &parsed);
    if (rc != CF_OK) return rc;

    custom_styles_write_arg arg;
    memset(&arg, 0, sizeof arg);
    arg.actor_id = actor_id;
    arg.account_id = account_id;
    arg.given = parsed.given;
    arg.is_null = parsed.is_null;
    arg.value = parsed.value;
    rc = cf_write(ctx->app, custom_styles_write_cb, &arg);
    custom_styles_value_dispose(&parsed);
    if (rc != CF_OK) return rc;

    rc = cf_ctx_flash_set(ctx, custom_styles_span("notice"),
                           custom_styles_span("\xE2\x9C\x93"));
    if (rc != CF_OK) return rc;

    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    cf_builder location = {0};
    rc = cf_builder_append(&location, custom_styles_span(config->public_origin));
    if (rc == CF_OK) {
        rc = cf_builder_append(&location,
                               custom_styles_span("/account/custom_styles/edit"));
    }
    if (rc == CF_OK) {
        ctx->response->status = 302;
        rc = cf_response_header(ctx->response, custom_styles_span("Location"),
                                (cf_span){location.ptr, location.len});
        if (rc == CF_OK) {
            rc = cf_response_header(
                ctx->response, custom_styles_span("Content-Type"),
                custom_styles_span("text/html; charset=utf-8"));
        }
    }
    cf_builder_dispose(&location);
    return rc;
}
