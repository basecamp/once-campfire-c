/* src/actions/first_runs.c — FirstRunsController (task A-first_runs; route
 * IDs 4 `show`, 8 `create`; contracts/routes.json).
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/first_runs.rs, the
 * pinned port of reference/app/controllers/first_runs_controller.rb:
 *
 *   allow_unauthenticated_access; before_action :prevent_repeats
 *   show:   respond_to HTML, render first_runs/show in the application layout
 *           (turbo-rails' frame layout for a Turbo-Frame request).
 *   create: params.require(:user).permit(:name, :avatar, :email_address,
 *           :password); hash the password (A01); one writer transaction
 *           creating the Campfire account, its administrator, the open
 *           "All Talk" room and the administrator's membership (D01
 *           cf_first_run_create); start a session; redirect to root.  A
 *           write that loses the setup race redirects to root instead of
 *           duplicating the account (DB-04).
 *
 * Callback order, permit list, status/redirect and the new-session effect are
 * the reference's.  The C context conventions follow src/auth.h: an action
 * is `cf_err cf_action_NAME(cf_ctx *)`, a helper that answers the request
 * sets ctx->response and returns CF_OK, and the caller stops with
 * cf_auth_halted.  Absolute URLs use PUBLIC_ORIGIN (03-application.md,
 * "URL helpers ... use PUBLIC_ORIGIN for absolute URLs"; D-C07).
 *
 * Multipart avatars are staged before the writer transaction; attachment
 * and blob rows commit with the administrator. Failed writes remove the
 * staged file; successful writes enqueue best-effort media analysis. */
#include "cf.h"
#include "actions/avatar_upload.h"
#include "http/params.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "models/account.h"
#include "models/active_storage.h"
#include "models/first_run.h"
#include "models/session.h"
#include "models/user.h"
#include "views.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- spans and owned strings -------------------------------------------- */

static cf_span fr_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* Borrowed cf_str over a literal (model functions borrow their arguments). */
static cf_str fr_cstr(const char *text) {
    return (cf_str){(char *)(uintptr_t)text, strlen(text)};
}

/* Owned non-NULL string copy; an empty span yields an empty string (the
 * cf_str contract). */
static cf_err fr_str_dup(cf_span span, cf_str *out) {
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

static void fr_str_dispose(cf_str *value) {
    if (value == NULL) return;
    free(value->ptr);
    memset(value, 0, sizeof *value);
}

/* ---- request predicates (kit ctx.rs) ------------------------------------ */

/* `Before::default().allow_unauthenticated_access()`: skip
 * require_authentication, keep deny_bots and forgery protection. */
static cf_before fr_before(void) {
    cf_before before = {0};
    before.authentication = CF_AUTH_SKIPPED;
    before.deny_bots = true;
    before.forgery_protection = true;
    return before;
}

static unsigned char fr_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c - 'A' + 'a') : c;
}

/* `c.is_turbo_frame_request()`: the first `Turbo-Frame` header, whose bytes
 * must pass http 1.5.0's `HeaderValue::to_str` (HTAB or visible ASCII only;
 * any other byte makes the header read as absent) and be nonempty after
 * `str::trim()` (only spaces and tabs). */
static bool fr_turbo_frame_request(const cf_request *request) {
    if (request == NULL) return false;
    cf_span value = {NULL, 0};
    bool found = false;
    for (size_t i = 0; i < request->header_count; i++) {
        const cf_header *header = &request->headers[i];
        if (header->name.len != 11) continue;
        static const char name[] = "turbo-frame";
        bool match = true;
        for (size_t k = 0; k < 11; k++) {
            if (fr_lower(header->name.ptr[k]) != (unsigned char)name[k]) {
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
        /* Not through `HeaderValue::to_str` -> the header reads as absent. */
        if (!((c >= 32 && c < 127) || c == '\t')) return false;
        if (c != ' ' && c != '\t') blank = false;
    }
    return !blank;
}

/* ---- responses ---------------------------------------------------------- */

/* `redirect_to root_url`: 302, the absolute root URL, the reference
 * redirect's content type. */
static cf_err fr_redirect_root(cf_ctx *ctx) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    cf_builder location = {0};
    cf_err rc = cf_builder_append(&location, fr_span(config->public_origin));
    if (rc == CF_OK) rc = cf_builder_append(&location, fr_span("/"));
    if (rc == CF_OK) {
        ctx->response->status = 302;
        rc = cf_response_header(ctx->response, fr_span("Location"),
                                (cf_span){location.ptr, location.len});
        if (rc == CF_OK) {
            rc = cf_response_header(ctx->response, fr_span("Content-Type"),
                                    fr_span("text/html; charset=utf-8"));
        }
    }
    cf_builder_dispose(&location);
    return rc;
}

/* `prevent_repeats`: `redirect_to root_url if Account.any?`. */
static cf_err fr_prevent_repeats(cf_ctx *ctx) {
    if (ctx->reader == NULL) return CF_INVALID;
    int64_t count = 0;
    cf_err rc = cf_account_count(ctx->reader, &count);
    if (rc != CF_OK) return rc;
    if (count > 0) return fr_redirect_root(ctx);
    return CF_OK;
}

/* `Layout#page`: `append_preload_links` of the stylesheet preload links; the
 * frame layout carries none.  An unconfigured asset module has no links,
 * which is the same state in which the layout renders no tag blocks. */
static cf_err fr_link_header(cf_ctx *ctx) {
    cf_builder links = {0};
    cf_err rc = cf_views_preload_links(&links);
    if (rc == CF_INVALID) return CF_OK;
    if (rc == CF_OK && links.len != 0) {
        rc = cf_response_header(ctx->response, fr_span("Link"),
                                (cf_span){links.ptr, links.len});
    }
    cf_builder_dispose(&links);
    return rc;
}

/* Freeze a rendered page into the response: 200, `text/html; charset=utf-8`,
 * plus the page's preload header.  Consumes the builder. */
static cf_err fr_page_response(cf_ctx *ctx, cf_builder *body, bool frame) {
    cf_buf *buf = NULL;
    cf_err rc = cf_builder_freeze(body, &buf);
    if (rc != CF_OK) {
        cf_builder_dispose(body);
        return rc;
    }
    ctx->response->status = 200;
    rc = cf_response_body(ctx->response, buf);
    cf_buf_release(buf);
    if (rc != CF_OK) return rc;
    rc = cf_response_header(ctx->response, fr_span("Content-Type"),
                            fr_span("text/html; charset=utf-8"));
    if (rc != CF_OK) return rc;
    if (!frame) rc = fr_link_header(ctx);
    return rc;
}

/* ---- show --------------------------------------------------------------- */

/* `framed_page!(c, OK, first_runs::Show)`: Layout::load, then the template in
 * the application layout, or turbo-rails' frame layout for a Turbo-Frame
 * request. */
static cf_err fr_render_show(cf_ctx *ctx) {
    cf_view_layout_model layout = {0};
    cf_err rc = cf_presenter_layout_load(ctx, cf_ctx_platform(ctx), &layout);
    if (rc != CF_OK) return rc;
    cf_view_ctx view_ctx = {0};
    cf_view_ctx_init(&view_ctx, ctx, &layout);
    bool frame = fr_turbo_frame_request(ctx->request);
    cf_builder body = {0};
    rc = frame ? cf_view_first_run_frame(&view_ctx, &body)
               : cf_view_first_run(&view_ctx, &body);
    if (rc == CF_OK) {
        rc = fr_page_response(ctx, &body, frame);
    } else {
        cf_builder_dispose(&body);
    }
    cf_view_layout_model_dispose(&layout);
    return rc;
}

cf_err cf_action_first_runs_show(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_err rc = cf_before_actions(ctx, fr_before());
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;
    rc = fr_prevent_repeats(ctx);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;
    const cf_format *offered[1] = {&cf_format_html};
    const cf_format *chosen = NULL;
    rc = cf_ctx_respond_to(ctx, offered, 1, &chosen);
    if (rc != CF_OK) return rc;
    return fr_render_show(ctx);
}

/* ---- create: nested user parameters ------------------------------------- */

/* `Nothing` when the key is absent or holds an array/object; the permitted
 * scalar value otherwise (strong parameters `is_permitted_scalar`). */
static const cf_param *fr_permitted(const cf_param *user, const char *key) {
    if (user == NULL || cf_param_type(user) != CF_PARAM_OBJECT) return NULL;
    const cf_param *value = cf_param_field(user, fr_span(key));
    if (value == NULL) return NULL;
    cf_param_kind kind = cf_param_type(value);
    if (kind == CF_PARAM_ARRAY || kind == CF_PARAM_OBJECT) return NULL;
    return value;
}

/* `Params::require`: present (not blank), or the false literal. */
static bool fr_param_present(const cf_param *param) {
    if (param == NULL) return false;
    switch (cf_param_type(param)) {
    case CF_PARAM_NULL:
        return false;
    case CF_PARAM_BOOL:
        return true; /* true is present; false is what require accepts */
    case CF_PARAM_STRING: {
        cf_span text;
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

/* `Param::to_s`: the owned string form of a permitted scalar.
 * CF_NOT_FOUND is the reference's None (an upload is not stringable);
 * CF_INVALID means the C accessors cannot reproduce it (a non-integral JSON
 * number keeps no lexeme — reported to the integrator). */
static cf_err fr_param_to_s(const cf_param *param, cf_str *out) {
    memset(out, 0, sizeof *out);
    switch (cf_param_type(param)) {
    case CF_PARAM_NULL:
        return fr_str_dup((cf_span){NULL, 0}, out);
    case CF_PARAM_STRING: {
        cf_span text;
        cf_err rc = cf_param_string(param, &text);
        if (rc != CF_OK) return rc;
        return fr_str_dup(text, out);
    }
    case CF_PARAM_BOOL: {
        bool present = false;
        bool value = false;
        cf_err rc = cf_param_bool(param, &present, &value);
        if (rc != CF_OK) return rc;
        return fr_str_dup(fr_span(value ? "true" : "false"), out);
    }
    case CF_PARAM_NUMBER: {
        /* Reference `Param::to_s` (serde_json Display): exact lexeme text
         * for integral and non-integral numbers alike (cf_param_to_s). */
        cf_span text = {0};
        cf_err rc = cf_param_to_s(param, &text);
        if (rc != CF_OK) return rc == CF_NOT_FOUND ? CF_NOT_FOUND : CF_INVALID;
        return fr_str_dup(text, out);
    }
    default:
        return CF_NOT_FOUND;
    }
}

/* `record.avatar = value` for a permitted param (attachments::Assignment). */
typedef enum {
    FR_AVATAR_UNCHANGED = 0,
    FR_AVATAR_DELETE,
    FR_AVATAR_CREATE,
    FR_AVATAR_INVALID
} fr_avatar;

static fr_avatar fr_avatar_assignment(const cf_param *user) {
    const cf_param *avatar = fr_permitted(user, "avatar");
    if (avatar == NULL) return FR_AVATAR_UNCHANGED;
    switch (cf_param_type(avatar)) {
    case CF_PARAM_NULL:
        return FR_AVATAR_DELETE;
    case CF_PARAM_STRING: {
        cf_span text;
        if (cf_param_string(avatar, &text) != CF_OK) return FR_AVATAR_INVALID;
        return text.len == 0 ? FR_AVATAR_DELETE : FR_AVATAR_INVALID;
    }
    case CF_PARAM_UPLOAD:
        return FR_AVATAR_CREATE;
    default:
        return FR_AVATAR_INVALID;
    }
}

struct fr_fields {
    cf_str name;          /* owned; only valid when has_name */
    bool has_name;
    cf_str email_address; /* owned; "" when absent */
    cf_str password;      /* owned; "" when absent */
    fr_avatar avatar;
};

static void fr_fields_dispose(struct fr_fields *fields) {
    fr_str_dispose(&fields->name);
    fr_str_dispose(&fields->email_address);
    fr_str_dispose(&fields->password);
    fields->has_name = false;
    fields->avatar = FR_AVATAR_UNCHANGED;
}

/* `params.require(:user).permit(:name, :avatar, :email_address, :password)`
 * and the reference's three reads.  CF_INVALID is the reference's
 * ParameterMissing (dispatch answers 400); CF_INTERNAL is the failure of
 * Param::to_s the C accessors cannot reproduce. */
static cf_err fr_parse_user(cf_ctx *ctx, struct fr_fields *fields) {
    memset(fields, 0, sizeof *fields);
    const cf_param *user = cf_ctx_param(ctx, fr_span("user"));
    if (!fr_param_present(user)) return CF_INVALID;

    const cf_param *name = fr_permitted(user, "name");
    if (name != NULL) {
        cf_err rc = fr_param_to_s(name, &fields->name);
        if (rc == CF_OK) {
            fields->has_name = true;
        } else if (rc != CF_NOT_FOUND) {
            fr_fields_dispose(fields);
            return CF_INTERNAL;
        }
    }

    /* `and_then(to_s).unwrap_or_default()`: an unstringable value is "". */
    static const char *const strings[] = {"email_address", "password"};
    cf_str *targets[] = {&fields->email_address, &fields->password};
    for (size_t i = 0; i < 2; i++) {
        const cf_param *value = fr_permitted(user, strings[i]);
        cf_err rc = fr_str_dup((cf_span){NULL, 0}, targets[i]);
        if (rc != CF_OK) {
            fr_fields_dispose(fields);
            return rc;
        }
        if (value != NULL) {
            cf_str text = {0};
            rc = fr_param_to_s(value, &text);
            if (rc == CF_OK) {
                fr_str_dispose(targets[i]);
                *targets[i] = text;
            } else if (rc != CF_NOT_FOUND) {
                fr_fields_dispose(fields);
                return CF_INTERNAL;
            }
        }
    }

    fields->avatar = fr_avatar_assignment(user);
    return CF_OK;
}

/* ---- create: transaction ------------------------------------------------- */

struct fr_write {
    cf_str name;             /* borrowed from fr_fields */
    cf_str email_address;    /* borrowed */
    cf_str password_digest;  /* borrowed */
    fr_avatar avatar;
    const cf_active_staged *staged;
    int64_t blob_id;
    cf_user administrator; /* out on success */
};

static cf_err fr_write_cb(cf_tx *tx, void *arg) {
    struct fr_write *write = arg;
    cf_err rc = cf_first_run_create(tx, write->name, write->email_address,
                                    write->password_digest,
                                    &write->administrator);
    if (rc != CF_OK) return rc;

    switch (write->avatar) {
    case FR_AVATAR_UNCHANGED:
        return CF_OK;
    case FR_AVATAR_DELETE: {
        /* `attachments::assign` Delete: `record.avatar.destroy`.  The
         * administrator was created in this transaction and no other writer
         * can attach to it, so the reference's lookup always misses and
         * destroy/touch/purge-later is a no-op here.  A present attachment
         * would need S02's assignment helper; fail loudly instead of
         * silently skipping it. */
        bool found = false;
        cf_attachment attachment = {0};
        rc = cf_attachment_find_for(cf_tx_db(tx), fr_cstr("User"),
                                    write->administrator.id,
                                    fr_cstr("avatar"), &found, &attachment);
        cf_attachment_dispose(&attachment);
        if (rc != CF_OK) return rc;
        return found ? CF_INTERNAL : CF_OK;
    }
    case FR_AVATAR_CREATE:
        return cf_avatar_attach(tx, &write->administrator, write->staged, &write->blob_id);
    case FR_AVATAR_INVALID:
        /* "Could not find or build blob: expected attachable". */
        return CF_INTERNAL;
    }
    return CF_INTERNAL;
}

/* Account.count after a failed write: the reference's RecordNotUnique rescue
 * decision (see the evidence for why the C error surface cannot carry the
 * SQLite extended code across the writer thread). */
static cf_err fr_account_exists(cf_ctx *ctx, bool *out) {
    *out = false;
    if (ctx->reader == NULL) return CF_INVALID;
    int64_t count = 0;
    cf_err rc = cf_account_count(ctx->reader, &count);
    if (rc != CF_OK) return rc;
    *out = count > 0;
    return CF_OK;
}

cf_err cf_action_first_runs_create(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    cf_err rc = cf_before_actions(ctx, fr_before());
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;
    rc = fr_prevent_repeats(ctx);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    struct fr_fields fields;
    rc = fr_parse_user(ctx, &fields);
    if (rc != CF_OK) return rc;
    if (!fields.has_name) {
        /* users.name is NOT NULL: the reference's raised
         * ActiveRecord::NotNullViolation (an internal error, not a 400). */
        fr_fields_dispose(&fields);
        return CF_INTERNAL;
    }

    /* `PasswordDigest::hash(password, bcrypt_cost)` through A01, ahead of the
     * write (the writer never runs bcrypt). */
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL) {
        fr_fields_dispose(&fields);
        return CF_INTERNAL;
    }
    cf_str digest = {0};
    rc = cf_auth_password_digest(fields.password, config->bcrypt_cost,
                                 &digest);
    if (rc != CF_OK) {
        fr_fields_dispose(&fields);
        return rc;
    }

    cf_storage *storage = NULL;
    cf_active_staged staged = {0};
    if (fields.avatar == FR_AVATAR_CREATE)
        rc = cf_avatar_stage(ctx, cf_param_field(cf_ctx_param(ctx, fr_span("user")), fr_span("avatar")), &storage, &staged);

    struct fr_write write = {
        .name = fields.name,
        .email_address = fields.email_address,
        .password_digest = digest,
        .avatar = fields.avatar,
        .staged = fields.avatar == FR_AVATAR_CREATE ? &staged : NULL,
    };
    if (rc == CF_OK) rc = cf_write(ctx->app, fr_write_cb, &write);
    if (rc == CF_OK && write.staged != NULL) {
        rc = cf_active_staged_commit(&staged);
        if (rc == CF_OK) cf_avatar_analyze_later(ctx, write.blob_id);
    }
    cf_active_staged_dispose(&staged);
    cf_storage_close(storage);
    fr_str_dispose(&digest);

    if (rc == CF_OK) {
        cf_session session = {0};
        cf_err session_rc =
            cf_auth_start_new_session_for(ctx, &write.administrator, &session);
        cf_session_dispose(&session);
        cf_user_dispose(&write.administrator);
        fr_fields_dispose(&fields);
        if (session_rc != CF_OK) return session_rc;
        return fr_redirect_root(ctx);
    }

    /* The callback can have loaded the administrator row before a later step
     * failed: release it on every failure arm. */
    cf_user_dispose(&write.administrator);
    bool concurrent = false;
    if (rc == CF_INVALID) {
        /* A constraint refused the write: the reference distinguishes
         * RecordNotUnique (a concurrent setup won) from other failures. The C
         * model layer maps every SQLITE_CONSTRAINT to CF_INVALID, so the
         * committed state decides: an existing account means the concurrent
         * setup won. */
        cf_err read_rc = fr_account_exists(ctx, &concurrent);
        if (read_rc != CF_OK) {
            fr_fields_dispose(&fields);
            return read_rc;
        }
    }
    fr_fields_dispose(&fields);
    if (concurrent) return fr_redirect_root(ctx);
    if (rc == CF_BUSY) return rc; /* D-C04 overload, mapping to 503 */
    return CF_INTERNAL;
}
