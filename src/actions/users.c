/* src/actions/users.c — UsersController (task A-users; route ID 50 `new`,
 * route ID 51 `create`, route ID 74 `show`; contracts/routes.json).
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/users.rs, the pinned
 * port of reference/app/controllers/users_controller.rb:
 *
 *   require_unauthenticated_access only: %i[new create]
 *   before_action :set_user, only: :show
 *   before_action :verify_join_code, only: %i[new create]
 *
 *   new:    verify_join_code; respond_to HTML; framed_page users::New with
 *           the account join_code and the help contact (first administrator).
 *   create: verify_join_code; user_params =
 *           params.require(:user).permit(:name, :avatar, :email_address,
 *           :password); name missing/unstringable raises the NOT NULL
 *           internal error; email/password are and_then(to_s) (None for a
 *           missing key or an upload); a nonempty password is hashed ahead
 *           of the write; the avatar is staged and assigned; User.create!
 *           in the writer; on success the pending blob is analyzed later, a
 *           session starts and the response redirects to root; a
 *           RecordNotUnique redirects to new_session_url with the submitted
 *           email_address as the query value; anything else is internal.
 *   show:   set_user (`User.find(params[:id])`: integer_cast else 404, row
 *           absent 404); respond_to HTML; page_or_frame users::Show with the
 *           user summary and `user.transfer_id` (signed id, purpose
 *           "transfer", 4-hour expiry).
 *
 * `User.create!` carries no role/status overrides, so the row is the model
 * default (member, active): the pinned source has no first-user or
 * administrator promotion in this packet (that lives in FirstRun::create,
 * the A-first_runs packet).  The join-code gate reads `Current.account`
 * (`Account::first`); a missing account row is the reference's NoMethodError
 * (an internal error here), a mismatched code is `head :not_found`.
 *
 * Response conventions are the landed actions' (A-welcome/A-first_runs/
 * A-sessions/A-rooms): absolute PUBLIC_ORIGIN locations, 302 + text/html
 * redirects, `head` with the rendered format's content type, page responses
 * with the `Link` preload header and frame responses without.
 *
 * c_symbols for the integrator's route rebind (src/routes.c rows 50/51/74):
 *   cf_action_users_new, cf_action_users_create, cf_action_users_show.
 *
 * Integrator requests (views packet; each static shim below is marked):
 *  R1. cf_view_users_new / cf_view_users_new_frame — users/new.html.erb.
 *      Proposed declarations (src/views.h):
 *        typedef struct {
 *            cf_str join_code;                 // borrowed
 *            bool has_help_contact;
 *            const cf_view_help_contact *help_contact; // borrowed
 *        } cf_view_users_new_model;
 *        cf_err cf_view_users_new(const cf_view_ctx *,
 *                                 const cf_view_users_new_model *,
 *                                 cf_builder *);
 *        cf_err cf_view_users_new_frame(const cf_view_ctx *,
 *                                       const cf_view_users_new_model *,
 *                                       cf_builder *);
 *      Semantics: the join page (title "Sign up", body class "signup") with
 *      the account join_code and the optional help contact, in the page or
 *      the turbo frame layout per the caller.
 *  R2. cf_view_users_show / cf_view_users_show_frame — users/show.html.erb.
 *      Proposed declarations (src/views.h):
 *        typedef struct {
 *            int64_t id; cf_str name;             // borrowed
 *            cf_optional_str bio;                 // borrowed
 *            cf_optional_str email_address;       // borrowed
 *            cf_role role; cf_status status;
 *            cf_str avatar_path;                  // borrowed, fresh_user_avatar
 *            cf_str transfer_id;                  // borrowed, purpose transfer
 *        } cf_view_users_show_model;
 *        cf_err cf_view_users_show(const cf_view_ctx *,
 *                                  const cf_view_users_show_model *,
 *                                  cf_builder *);
 *        cf_err cf_view_users_show_frame(const cf_view_ctx *,
 *                                        const cf_view_users_show_model *,
 *                                        cf_builder *);
 *      Semantics: the user page (title = user name) with the transfer id for
 *      the profiles/_transfer partial, in the page (nav + content blocks) or
 *      the turbo frame layout per the caller.
 */
#include "cf.h"
#include "actions/avatar_upload.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "http/params.h"
#include "models/account.h"
#include "models/active_storage.h"
#include "models/session.h"
#include "models/types.h" /* cf_role/cf_status for the R2 model */
#include "models/user.h"
#include "views.h"
#include "views/internal.h" /* cf_views_integer_cast */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- INTEGRATOR SHIM R1/R2 proposals (delete when views.h lands them) ---- */

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

typedef struct {
    int64_t id;
    cf_str name; /* borrowed */
    cf_optional_str bio; /* borrowed */
    cf_optional_str email_address; /* borrowed */
    cf_role role;
    cf_status status;
    cf_str avatar_path; /* borrowed */
    cf_str transfer_id; /* borrowed */
} cf_view_users_show_model;

cf_err cf_view_users_show(const cf_view_ctx *ctx,
                          const cf_view_users_show_model *model,
                          cf_builder *out);
cf_err cf_view_users_show_frame(const cf_view_ctx *ctx,
                                const cf_view_users_show_model *model,
                                cf_builder *out);

/* ---- small helpers (the landed actions' shared shapes) -------------------- */

static cf_span users_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

static cf_str users_cstr(const char *text) {
    return (cf_str){(char *)(uintptr_t)text, strlen(text)};
}

static unsigned char users_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c - 'A' + 'a') : c;
}

static bool users_turbo_frame_request(const cf_request *request) {
    if (request == NULL) return false;
    cf_span value = {NULL, 0};
    bool found = false;
    for (size_t i = 0; i < request->header_count; i++) {
        const cf_header *header = &request->headers[i];
        if (header->name.len != 11) continue;
        static const char name[] = "turbo-frame";
        bool match = true;
        for (size_t k = 0; k < 11; k++) {
            if (users_lower(header->name.ptr[k]) != (unsigned char)name[k]) {
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

static cf_err users_redirect_to(cf_ctx *ctx, cf_span location) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    ctx->response->status = 302;
    cf_err rc = cf_response_header(ctx->response, users_span("Location"),
                                   location);
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response, users_span("Content-Type"),
                                users_span("text/html; charset=utf-8"));
    }
    return rc;
}

/* `url_for(<path>)` for the C port: PUBLIC_ORIGIN + the route path. */
static cf_err users_redirect_path(cf_ctx *ctx, cf_span path) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    cf_builder location = {0};
    cf_err rc =
        cf_builder_append(&location, users_span(config->public_origin));
    if (rc == CF_OK) rc = cf_builder_append(&location, path);
    if (rc == CF_OK) {
        rc = users_redirect_to(ctx, (cf_span){location.ptr, location.len});
    }
    cf_builder_dispose(&location);
    return rc;
}

/* `concerns::head(status)`: empty body with the rendered format's content
 * type (kit Ctx::head). */
static cf_err users_head(cf_ctx *ctx, unsigned status) {
    ctx->response->status = status;
    const cf_format *format = cf_ctx_rendered_format(ctx);
    return cf_response_header(ctx->response, users_span("Content-Type"),
                              users_span(format->string));
}

static cf_err users_link_header(cf_ctx *ctx) {
    cf_builder links = {0};
    cf_err rc = cf_views_preload_links(&links);
    if (rc == CF_INVALID) return CF_OK;
    if (rc == CF_OK && links.len != 0) {
        rc = cf_response_header(ctx->response, users_span("Link"),
                                (cf_span){links.ptr, links.len});
    }
    cf_builder_dispose(&links);
    return rc;
}

static cf_err users_page_response(cf_ctx *ctx, unsigned status,
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
    rc = cf_response_header(ctx->response, users_span("Content-Type"),
                            users_span("text/html; charset=utf-8"));
    if (rc != CF_OK) return rc;
    if (!frame) rc = users_link_header(ctx);
    return rc;
}

/* ---- verify_join_code ------------------------------------------------------ */

/* `head :not_found if Current.account.join_code != params[:join_code]`.
 * On the mismatch arm the response is answered and the caller stops with
 * cf_auth_halted.  A missing account row is the reference's NoMethodError
 * on nil (CF_INTERNAL here).  On success *account_out holds the row. */
static cf_err users_verify_join_code(cf_ctx *ctx, cf_account *account_out) {
    bool found = false;
    cf_err rc = cf_account_first(ctx->reader, &found, account_out);
    if (rc != CF_OK) return rc;
    if (!found) {
        cf_account_dispose(account_out);
        memset(account_out, 0, sizeof *account_out);
        return CF_INTERNAL;
    }
    const cf_param *param = cf_ctx_param(ctx, users_span("join_code"));
    cf_span given = {NULL, 0};
    bool matches = param != NULL &&
                   cf_param_type(param) == CF_PARAM_STRING &&
                   cf_param_string(param, &given) == CF_OK &&
                   given.len == account_out->join_code.len &&
                   (given.len == 0 ||
                    memcmp(given.ptr, account_out->join_code.ptr, given.len) ==
                        0);
    if (!matches) {
        cf_account_dispose(account_out);
        memset(account_out, 0, sizeof *account_out);
        return users_head(ctx, 404);
    }
    return CF_OK;
}

/* ---- user_params (params.require(:user).permit(...)) ----------------------- */

/* `Params::require`: present (not blank), or the false literal. */
static bool users_param_present(const cf_param *param) {
    if (param == NULL) return false;
    switch (cf_param_type(param)) {
    case CF_PARAM_NULL:
        return false;
    case CF_PARAM_BOOL:
        return true;
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

/* The permitted scalar value, or NULL when absent/array/object (strong
 * parameters `is_permitted_scalar`). */
static const cf_param *users_permitted(const cf_param *user, const char *key) {
    if (user == NULL || cf_param_type(user) != CF_PARAM_OBJECT) return NULL;
    const cf_param *value = cf_param_field(user, users_span(key));
    if (value == NULL) return NULL;
    cf_param_kind kind = cf_param_type(value);
    if (kind == CF_PARAM_ARRAY || kind == CF_PARAM_OBJECT) return NULL;
    return value;
}

static cf_err users_str_dup(cf_span span, cf_str *out) {
    memset(out, 0, sizeof *out);
    if (span.len != 0 && span.ptr == NULL) return CF_INVALID;
    if (span.len == 0) return CF_OK;
    out->ptr = malloc(span.len + 1);
    if (out->ptr == NULL) return CF_NOMEM;
    memcpy(out->ptr, span.ptr, span.len);
    out->ptr[span.len] = '\0';
    out->len = span.len;
    return CF_OK;
}

/* `Param::to_s` for a permitted scalar (cf_param_to_s, kit params.rs):
 * CF_NOT_FOUND is the reference's None (an upload is not stringable). */
static cf_err users_to_owned_string(const cf_param *param, cf_str *out) {
    memset(out, 0, sizeof *out);
    if (param == NULL) return CF_NOT_FOUND;
    switch (cf_param_type(param)) {
    case CF_PARAM_NULL:
        return CF_OK; /* `to_s` of nil is "" */
    case CF_PARAM_STRING: {
        cf_span text;
        if (cf_param_string(param, &text) != CF_OK) return CF_INTERNAL;
        return users_str_dup(text, out);
    }
    case CF_PARAM_BOOL: {
        bool present = false;
        bool value = false;
        if (cf_param_bool(param, &present, &value) != CF_OK) {
            return CF_INTERNAL;
        }
        return users_str_dup(users_span(value ? "true" : "false"), out);
    }
    case CF_PARAM_NUMBER: {
        cf_span text = {0};
        cf_err rc = cf_param_to_s(param, &text);
        if (rc != CF_OK) return CF_INTERNAL;
        return users_str_dup(text, out);
    }
    default:
        return CF_NOT_FOUND;
    }
}

/* `record.avatar = value` for a permitted param (attachments::Assignment):
 * absent is unchanged, null deletes, an upload creates, anything else
 * ("Could not find or build blob") is invalid. */
typedef enum {
    USERS_AVATAR_UNCHANGED = 0,
    USERS_AVATAR_DELETE,
    USERS_AVATAR_CREATE,
    USERS_AVATAR_INVALID
} users_avatar;

static users_avatar users_avatar_assignment(const cf_param *user) {
    const cf_param *avatar = users_permitted(user, "avatar");
    if (avatar == NULL) return USERS_AVATAR_UNCHANGED;
    switch (cf_param_type(avatar)) {
    case CF_PARAM_NULL:
        return USERS_AVATAR_DELETE;
    case CF_PARAM_STRING: {
        cf_span text;
        if (cf_param_string(avatar, &text) != CF_OK) {
            return USERS_AVATAR_INVALID;
        }
        return text.len == 0 ? USERS_AVATAR_DELETE : USERS_AVATAR_INVALID;
    }
    case CF_PARAM_UPLOAD:
        return USERS_AVATAR_CREATE;
    default:
        return USERS_AVATAR_INVALID;
    }
}

struct users_fields {
    cf_str name; /* owned; valid only when has_name */
    bool has_name;
    cf_str email_address; /* owned; valid only when has_email (None else) */
    bool has_email;
    cf_str password; /* owned; "" when absent/unstringable */
    users_avatar avatar;
};

static void users_fields_dispose(struct users_fields *fields) {
    if (fields->has_name) cf_str_dispose(&fields->name);
    if (fields->has_email) cf_str_dispose(&fields->email_address);
    cf_str_dispose(&fields->password);
    memset(fields, 0, sizeof *fields);
}

/* `params.require(:user)` (CF_INVALID is ParameterMissing, a 400) plus the
 * reference's three reads.  `and_then(to_s)` is None for a missing key or an
 * upload (has_* stays false); null stringifies to "". */
static cf_err users_parse_user(cf_ctx *ctx, struct users_fields *fields) {
    memset(fields, 0, sizeof *fields);
    const cf_param *user = cf_ctx_param(ctx, users_span("user"));
    if (!users_param_present(user)) return CF_INVALID;

    const cf_param *name = users_permitted(user, "name");
    if (name != NULL) {
        cf_err rc = users_to_owned_string(name, &fields->name);
        if (rc == CF_OK) {
            fields->has_name = true;
        } else if (rc != CF_NOT_FOUND) {
            users_fields_dispose(fields);
            return CF_INTERNAL;
        }
    }

    const cf_param *email = users_permitted(user, "email_address");
    if (email != NULL) {
        cf_err rc = users_to_owned_string(email, &fields->email_address);
        if (rc == CF_OK) {
            fields->has_email = true;
        } else if (rc != CF_NOT_FOUND) {
            users_fields_dispose(fields);
            return CF_INTERNAL;
        }
    }

    const cf_param *password = users_permitted(user, "password");
    if (password != NULL) {
        cf_str text = {0};
        cf_err rc = users_to_owned_string(password, &text);
        if (rc == CF_OK) {
            fields->password = text;
        } else if (rc != CF_NOT_FOUND) {
            users_fields_dispose(fields);
            return CF_INTERNAL;
        }
    }

    fields->avatar = users_avatar_assignment(user);
    return CF_OK;
}

/* ---- create: transaction --------------------------------------------------- */

struct users_write {
    cf_str name; /* borrowed */
    bool has_email;
    cf_str email_address; /* borrowed */
    cf_str password_digest; /* borrowed; valid only when has_digest */
    bool has_digest;
    users_avatar avatar;
    const cf_active_staged *staged;
    int64_t blob_id;
    cf_user created; /* out on success */
};

static cf_err users_write_cb(cf_tx *tx, void *arg) {
    struct users_write *write = arg;
    cf_new_user attributes = {0};
    attributes.name = write->name;
    if (write->has_email) {
        attributes.email_address.present = true;
        attributes.email_address.value = write->email_address;
    }
    if (write->has_digest) {
        attributes.password_digest.present = true;
        attributes.password_digest.value = write->password_digest;
    }
    /* role/status stay the model defaults (member, active): the pinned
     * source passes no overrides (no first-user/admin promotion here). */
    cf_err rc = cf_user_create(tx, &attributes, &write->created);
    if (rc != CF_OK) return rc;

    /* `attachments::assign(tx, Record::user(id), "avatar", avatar)`: the
     * created user can hold no attachment yet, so Delete is a verified
     * no-op; Create inserts the staged upload, Invalid fails the save. */
    switch (write->avatar) {
    case USERS_AVATAR_UNCHANGED:
        return CF_OK;
    case USERS_AVATAR_DELETE: {
        bool found = false;
        cf_attachment attachment = {0};
        rc = cf_attachment_find_for(cf_tx_db(tx), users_cstr("User"),
                                    write->created.id, users_cstr("avatar"),
                                    &found, &attachment);
        cf_attachment_dispose(&attachment);
        if (rc != CF_OK) return rc;
        return found ? CF_INTERNAL : CF_OK;
    }
    case USERS_AVATAR_CREATE:
        return cf_avatar_attach(tx, &write->created, write->staged, &write->blob_id);
    case USERS_AVATAR_INVALID:
        return CF_INTERNAL;
    }
    return CF_INTERNAL;
}

/* `CGI.escape` (ruby uri.rs percent_encode, space_as_plus): unreserved
 * `A-Za-z0-9_.-~` stay literal, space is `+`, every other byte is %XX. */
static cf_err users_cgi_escape(cf_span in, cf_builder *out) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < in.len; i++) {
        unsigned char c = in.ptr[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-' ||
            c == '~') {
            cf_err rc = cf_builder_append(out, (cf_span){&in.ptr[i], 1});
            if (rc != CF_OK) return rc;
        } else if (c == ' ') {
            cf_err rc =
                cf_builder_append(out, users_span("+"));
            if (rc != CF_OK) return rc;
        } else {
            char esc[3] = {'%', hex[c >> 4], hex[c & 15]};
            cf_err rc = cf_builder_append(
                out, (cf_span){(const unsigned char *)esc, 3});
            if (rc != CF_OK) return rc;
        }
    }
    return CF_OK;
}

/* `redirect_to new_session_url(email_address:)` for the RecordNotUnique arm:
 * PUBLIC_ORIGIN + "/session/new?email_address=<CGI.escape(email)>" when an
 * address was submitted, else the bare sign-in URL. */
static cf_err users_redirect_duplicate(cf_ctx *ctx,
                                       const struct users_fields *fields) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    cf_builder location = {0};
    cf_err rc =
        cf_builder_append(&location, users_span(config->public_origin));
    if (rc == CF_OK) {
        rc = cf_builder_append(&location, users_span("/session/new"));
    }
    if (rc == CF_OK && fields->has_email) {
        rc = cf_builder_append(&location, users_span("?email_address="));
        if (rc == CF_OK) {
            rc = users_cgi_escape((cf_span){
                                      (const unsigned char *)
                                          fields->email_address.ptr,
                                      fields->email_address.len,
                                  },
                                  &location);
        }
    }
    if (rc == CF_OK) {
        rc = users_redirect_to(ctx, (cf_span){location.ptr, location.len});
    }
    cf_builder_dispose(&location);
    return rc;
}

/* The reference's RecordNotUnique rescue decision (cf. first_runs'
 * account-exists heuristic: the C writer surface cannot carry the SQLite
 * extended code across the thread): a row already holds the submitted
 * address, so the failed write was the unique-email violation.  NULL
 * addresses never conflict, and name/role/status cannot violate here. */
static cf_err users_is_duplicate_email(cf_ctx *ctx,
                                       const struct users_fields *fields,
                                       bool *out) {
    *out = false;
    if (!fields->has_email) return CF_OK;
    bool found = false;
    cf_user existing = {0};
    cf_err rc = cf_user_find_by_email_address(ctx->reader,
                                              fields->email_address, &found,
                                              &existing);
    cf_user_dispose(&existing);
    if (rc != CF_OK) return rc;
    *out = found;
    return CF_OK;
}

/* ---- show helpers ----------------------------------------------------------- */

/* `User.find(params[:id])`: integer_cast of the string param, then the row;
 * CF_NOT_FOUND when either misses (dispatch answers the public 404). */
static cf_err users_find_user(cf_ctx *ctx, cf_user *out) {
    const cf_param *param = cf_ctx_param(ctx, users_span("id"));
    if (param == NULL || cf_param_type(param) != CF_PARAM_STRING) {
        return CF_NOT_FOUND;
    }
    cf_span text = {NULL, 0};
    if (cf_param_string(param, &text) != CF_OK) return CF_NOT_FOUND;
    int64_t id = 0;
    if (!cf_views_integer_cast(text, &id)) return CF_NOT_FOUND;
    bool found = false;
    cf_err rc = cf_user_find_by_id(ctx->reader, id, &found, out);
    if (rc != CF_OK) return rc;
    if (!found) {
        cf_user_dispose(out);
        memset(out, 0, sizeof *out);
        return CF_NOT_FOUND;
    }
    return CF_OK;
}

/* `user.transfer_id`: signed id, purpose "transfer", 4-hour expiry. */
#define USERS_TRANSFER_EXPIRY_US (INT64_C(4) * INT64_C(3600) * INT64_C(1000000))

static cf_err users_transfer_id(cf_ctx *ctx, int64_t user_id, cf_str *out) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->secret_key_base == NULL) return CF_INTERNAL;
    return cf_auth_signed_id_generate(
        (cf_span){(const unsigned char *)config->secret_key_base,
                  config->secret_key_base_len},
        users_span("User"), user_id, users_span("transfer"), true, true,
        cf_now_us(ctx->app) + USERS_TRANSFER_EXPIRY_US, out);
}

/* `fresh_user_avatar_path(user)`: "/users/<avatar token>/avatar?v=<number>"
 * (campfire_routes::fresh_user_avatar; the signed avatar token plus
 * updated_at to_fs(:number) %Y%m%d%H%M%S UTC). */
static void users_to_fs_number(int64_t us, char out[15]) {
    time_t seconds = (time_t)(us / 1000000);
    if (us < 0 && us % 1000000 != 0) seconds -= 1;
    struct tm tm;
    if (gmtime_r(&seconds, &tm) == NULL) {
        memcpy(out, "00000000000000", 14);
        return;
    }
    if (strftime(out, 15, "%Y%m%d%H%M%S", &tm) == 0) {
        memcpy(out, "00000000000000", 14);
    }
}

static cf_err users_avatar_path(cf_ctx *ctx, const cf_user *user,
                                cf_str *out) {
    memset(out, 0, sizeof *out);
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->secret_key_base == NULL) return CF_INTERNAL;
    cf_str token = {0};
    cf_err rc = cf_auth_signed_id_generate(
        (cf_span){(const unsigned char *)config->secret_key_base,
                  config->secret_key_base_len},
        users_span("User"), user->id, users_span("avatar"), true, false, 0,
        &token);
    if (rc != CF_OK) return rc;
    char number[15];
    users_to_fs_number(user->updated_at, number);
    const char *prefix = "/users/";
    const char *middle = "/avatar?v=";
    size_t len =
        strlen(prefix) + token.len + strlen(middle) + sizeof number - 1;
    out->ptr = malloc(len + 1);
    if (out->ptr == NULL) {
        cf_str_dispose(&token);
        return CF_NOMEM;
    }
    size_t at = 0;
    memcpy(out->ptr + at, prefix, strlen(prefix));
    at += strlen(prefix);
    memcpy(out->ptr + at, token.ptr, token.len);
    at += token.len;
    cf_str_dispose(&token);
    memcpy(out->ptr + at, middle, strlen(middle));
    at += strlen(middle);
    memcpy(out->ptr + at, number, sizeof number - 1);
    at += sizeof number - 1;
    out->ptr[at] = '\0';
    out->len = at;
    return CF_OK;
}

/* INTEGRATOR SHIM R1 (see the header): the future cf_view_users_new call.
 * Renders through the views-packet template when it lands; until then the
 * call goes to the linked definition (the packet test stubs it). */
static cf_err users_render_new(const cf_view_ctx *view_ctx,
                               const cf_view_users_new_model *model, bool frame,
                               cf_builder *out) {
    if (frame) return cf_view_users_new_frame(view_ctx, model, out);
    return cf_view_users_new(view_ctx, model, out);
}

/* INTEGRATOR SHIM R2 (see the header): the future cf_view_users_show call. */
static cf_err users_render_show(const cf_view_ctx *view_ctx,
                                const cf_view_users_show_model *model,
                                bool frame, cf_builder *out) {
    if (frame) return cf_view_users_show_frame(view_ctx, model, out);
    return cf_view_users_show(view_ctx, model, out);
}

/* ---- actions -------------------------------------------------------------- */

cf_err cf_action_users_new(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    /* `require_unauthenticated_access`: the chain restores a session and
     * redirects a signed-in user to root after the forgery/browser steps. */
    cf_before policy = {CF_AUTH_REQUIRE_UNAUTHENTICATED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_account account = {0};
    rc = users_verify_join_code(ctx, &account);
    if (rc != CF_OK || cf_auth_halted(ctx)) {
        return rc;
    }

    const cf_format *offered[1] = {&cf_format_html};
    const cf_format *chosen = NULL;
    rc = cf_ctx_respond_to(ctx, offered, 1, &chosen);
    if (rc != CF_OK) {
        cf_account_dispose(&account);
        return rc;
    }

    cf_view_help_contact contact = {0};
    bool has_contact = false;
    rc = cf_presenter_help_contact(ctx->reader, &has_contact, &contact);
    if (rc != CF_OK) {
        cf_account_dispose(&account);
        return rc;
    }

    cf_view_layout_model layout = {0};
    rc = cf_presenter_layout_load(ctx, cf_ctx_platform(ctx), &layout);
    if (rc != CF_OK) {
        cf_view_help_contact_dispose(&contact);
        cf_account_dispose(&account);
        return rc;
    }

    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, &layout);

    cf_view_users_new_model model = {0};
    model.join_code = (cf_str){account.join_code.ptr, account.join_code.len};
    model.has_help_contact = has_contact;
    model.help_contact = has_contact ? &contact : NULL;

    bool frame = users_turbo_frame_request(ctx->request);
    cf_builder body = {0};
    rc = users_render_new(&view_ctx, &model, frame, &body);
    if (rc == CF_OK) {
        rc = users_page_response(ctx, 200, &body, frame);
    } else {
        cf_builder_dispose(&body);
    }

    cf_view_layout_model_dispose(&layout);
    cf_view_help_contact_dispose(&contact);
    cf_account_dispose(&account);
    return rc;
}

cf_err cf_action_users_create(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRE_UNAUTHENTICATED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_account account = {0};
    rc = users_verify_join_code(ctx, &account);
    cf_account_dispose(&account);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    struct users_fields fields;
    rc = users_parse_user(ctx, &fields);
    if (rc != CF_OK) return rc;
    if (!fields.has_name) {
        /* users.name is NOT NULL: the reference's raised
         * ActiveRecord::NotNullViolation (an internal error, not a 400). */
        users_fields_dispose(&fields);
        return CF_INTERNAL;
    }

    /* `password_digest`: hashed ahead of the write (bcrypt never runs on the
     * writer); an empty/missing password stores NULL. */
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL) {
        users_fields_dispose(&fields);
        return CF_INTERNAL;
    }
    cf_str digest = {0};
    bool has_digest = fields.password.len != 0;
    if (has_digest) {
        rc = cf_auth_password_digest(fields.password, config->bcrypt_cost,
                                     &digest);
        if (rc != CF_OK) {
            users_fields_dispose(&fields);
            return rc;
        }
    }

    cf_storage *storage = NULL;
    cf_active_staged staged = {0};
    if (fields.avatar == USERS_AVATAR_CREATE)
        rc = cf_avatar_stage(ctx, cf_param_field(cf_ctx_param(ctx, users_span("user")), users_span("avatar")), &storage, &staged);

    struct users_write write = {
        .name = fields.name,
        .has_email = fields.has_email,
        .email_address = fields.email_address,
        .password_digest = digest,
        .has_digest = has_digest,
        .avatar = fields.avatar,
        .staged = fields.avatar == USERS_AVATAR_CREATE ? &staged : NULL,
    };
    if (rc == CF_OK) rc = cf_write(ctx->app, users_write_cb, &write);
    if (rc == CF_OK && write.staged != NULL) {
        rc = cf_active_staged_commit(&staged);
        if (rc == CF_OK) cf_avatar_analyze_later(ctx, write.blob_id);
    }
    cf_active_staged_dispose(&staged);
    cf_storage_close(storage);
    cf_str_dispose(&digest);

    if (rc == CF_OK) {
        cf_session session = {0};
        cf_err session_rc =
            cf_auth_start_new_session_for(ctx, &write.created, &session);
        cf_session_dispose(&session);
        cf_user_dispose(&write.created);
        users_fields_dispose(&fields);
        if (session_rc != CF_OK) return session_rc;
        return users_redirect_path(ctx, users_span("/"));
    }

    cf_user_dispose(&write.created);
    bool duplicate = false;
    if (rc == CF_INVALID) {
        cf_err read_rc = users_is_duplicate_email(ctx, &fields, &duplicate);
        if (read_rc != CF_OK) {
            users_fields_dispose(&fields);
            return read_rc;
        }
    }
    if (duplicate) {
        rc = users_redirect_duplicate(ctx, &fields);
        users_fields_dispose(&fields);
        return rc;
    }
    users_fields_dispose(&fields);
    if (rc == CF_BUSY) return rc; /* D-C04 overload, mapping to 503 */
    return CF_INTERNAL;
}

cf_err cf_action_users_show(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    /* `set_user` before the format negotiation (declaration order). */
    cf_user user = {0};
    rc = users_find_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }

    const cf_format *offered[1] = {&cf_format_html};
    const cf_format *chosen = NULL;
    rc = cf_ctx_respond_to(ctx, offered, 1, &chosen);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }

    /* Presenter work: the signed transfer id, then respond_to, then
     * Layout::load (the rooms#show translation order). */
    cf_str transfer_id = {0};
    rc = users_transfer_id(ctx, user.id, &transfer_id);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }
    cf_str avatar_path = {0};
    rc = users_avatar_path(ctx, &user, &avatar_path);
    if (rc != CF_OK) {
        cf_str_dispose(&transfer_id);
        cf_user_dispose(&user);
        return rc;
    }

    cf_view_layout_model layout = {0};
    rc = cf_presenter_layout_load(ctx, cf_ctx_platform(ctx), &layout);
    if (rc != CF_OK) {
        cf_str_dispose(&avatar_path);
        cf_str_dispose(&transfer_id);
        cf_user_dispose(&user);
        return rc;
    }

    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, &layout);

    cf_view_users_show_model model = {0};
    model.id = user.id;
    model.name = (cf_str){user.name.ptr, user.name.len};
    if (user.bio.present) model.bio = user.bio;
    if (user.email_address.present) model.email_address = user.email_address;
    model.role = user.role;
    model.status = user.status;
    model.avatar_path = avatar_path;
    model.transfer_id = transfer_id;

    /* page_or_frame: the page in the application layout, or turbo-rails'
     * frame layout for a Turbo-Frame request. */
    bool frame = users_turbo_frame_request(ctx->request);
    cf_builder body = {0};
    rc = users_render_show(&view_ctx, &model, frame, &body);
    if (rc == CF_OK) {
        rc = users_page_response(ctx, 200, &body, frame);
    } else {
        cf_builder_dispose(&body);
    }

    cf_view_layout_model_dispose(&layout);
    cf_str_dispose(&avatar_path);
    cf_str_dispose(&transfer_id);
    cf_user_dispose(&user);
    return rc;
}
