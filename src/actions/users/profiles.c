/* src/actions/users/profiles.c — Users::ProfilesController (task
 * A-users-profiles; route ID 60 `show`, route IDs 61/62 `update`;
 * contracts/routes.json).
 *
 * Source: tmp/rust-ref/crates/campfire/src/controllers/users/profiles.rs,
 * the pinned port of reference/app/controllers/users/profiles_controller.rb:
 *
 *   show:   before_actions(Before::default()) -> respond_to([HTML]) ->
 *           require_current_user -> transfer_id (signed id purpose
 *           "transfer", 4h expiry) -> read(avatar attached?,
 *           profile_memberships) -> user_summary ->
 *           framed_page(OK, users::ProfileShow)
 *   update: before_actions(Before::default()) -> require_current_user ->
 *           params.require(:user).permit(:name, :avatar, :email_address,
 *           :password, :bio) (.compact) -> UserChanges{name, email_address,
 *           password_digest (blank passwords ignored, hashed off-writer),
 *           bio} -> avatar Assignment::from_params (nil compacted to
 *           Unchanged, "" deletes, upload creates, anything else Invalid) ->
 *           notice ("It may take up to 30 minutes..." when user[avatar] is
 *           non-nil, else "✓") -> avatar.stage -> write(user.update,
 *           attachments::assign) -> analyze_later ->
 *           redirect_to_with(user_profile, notice)
 *
 * The C translation preserves the callback order, the permit list, the
 * require/compact/present semantics (kit params.rs: require accepts a
 * present value or `false`; permit keeps every non-array/object scalar,
 * uploads included; string_attribute flattens to_s, so JSON null reads as
 * ""), the notice rule (raw user[avatar] non-null), the update-then-assign
 * write order, and the redirect (302, absolute user_profile URL,
 * text/html; charset=utf-8, flash notice).
 *
 * No model validation runs on update: the reference User::update writes
 * what changed (nothing at all, not even updated_at, when nothing did) and
 * only raises for database failures, so a blank name is written, not a
 * 422.  Database errors propagate to the generic 500 mapping
 * (app.rs db_error: other -> Internal).
 *
 * Avatar states: Unchanged updates only the user; Delete destroys its
 * attachment and emits a purge event. Create stages bytes off the writer,
 * then replaces the attachment and creates its blob in the same transaction
 * as the user update. Commit keeps the file; rollback removes it. Analysis
 * is admitted to the bounded media queue after commit. Invalid values fail
 * before the write.
 *
 * Render (A02/V02): the show gate is gone; show renders
 * cf_view_users_profile_show(_frame) from the V02 presenter
 * cf_presenter_users_profile (transfer_id signing, attached check,
 * profile_memberships mapping and user_summary inside one read transaction)
 * through Layout::load, page_or_frame.
 *
 * Integrator requests:
 *  R1. A shared record-touch helper for the (User,avatar) destroy here and
 *      the push_subscriptions touch (exact SQL:
 *      UPDATE "<table>" SET "updated_at" = ? WHERE "<table>"."id" = ?).
 *      Until it lands, the static touch below (fixed statement, cache
 *      rules per db_internal.h) is the local implementation.
 *
 * c_symbols for the integrator's route rebind (src/routes.c rows 60-62):
 *   cf_action_users_profiles_show, cf_action_users_profiles_update.
 */
#include "cf.h"
#include "actions/avatar_upload.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "db/db_internal.h"
#include "http/params.h"
#include "models/active_storage.h"
#include "models/touch.h"
#include "models/user.h"
#include "presenters/users_profiles.h"
#include "views.h"

#include <stdlib.h>
#include <string.h>

static cf_span profiles_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `require_current_user`: the chain guarantees one; a missing row is the
 * reference's internal error (messages.c convention). */
static cf_err profiles_current_user(cf_ctx *ctx, cf_user *out) {
    bool found = false;
    cf_err rc = cf_auth_current_user(ctx, &found, out);
    if (rc != CF_OK) return rc;
    if (!found) return CF_INTERNAL;
    return CF_OK;
}

/* `redirect_to user_profile` (Rails default 302, the absolute location, the
 * reference redirect's content type).  With `notice`, the flash carries it
 * (redirect_to_with). */
static cf_err profiles_redirect_to(cf_ctx *ctx, cf_span location) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;
    ctx->response->status = 302;
    cf_err rc = cf_response_header(ctx->response, profiles_span("Location"),
                                   location);
    if (rc == CF_OK) {
        rc = cf_response_header(ctx->response,
                                profiles_span("Content-Type"),
                                profiles_span("text/html; charset=utf-8"));
    }
    return rc;
}

static cf_err profiles_profile_url(cf_ctx *ctx, cf_span *location,
                                   cf_builder *holder) {
    const cf_config *config = cf_app_config(ctx->app);
    if (config == NULL || config->public_origin == NULL) return CF_INTERNAL;
    cf_err rc = cf_builder_append(holder,
                                  profiles_span(config->public_origin));
    if (rc == CF_OK) {
        rc = cf_builder_append(holder,
                               profiles_span("/users/me/profile"));
    }
    if (rc != CF_OK) return rc;
    *location = (cf_span){holder->ptr, holder->len};
    return CF_OK;
}

static cf_err profiles_redirect_profile(cf_ctx *ctx, const char *notice) {
    cf_builder holder = {0};
    cf_span location = {NULL, 0};
    cf_err rc = profiles_profile_url(ctx, &location, &holder);
    if (rc == CF_OK && notice != NULL) {
        rc = cf_ctx_flash_set(ctx, profiles_span("notice"),
                              profiles_span(notice));
    }
    if (rc == CF_OK) rc = profiles_redirect_to(ctx, location);
    cf_builder_dispose(&holder);
    return rc;
}

/* ---- show ----------------------------------------------------------------- */

/* `c.is_turbo_frame_request()` (users.c/sidebars.c convention): a non-blank
 * `Turbo-Frame` header selects turbo-rails' frame layout. */
static unsigned char profiles_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c - 'A' + 'a') : c;
}

static bool profiles_turbo_frame_request(const cf_request *request) {
    if (request == NULL) return false;
    cf_span value = {NULL, 0};
    bool found = false;
    for (size_t i = 0; i < request->header_count; i++) {
        const cf_header *header = &request->headers[i];
        if (header->name.len != 11) continue;
        static const char name[] = "turbo-frame";
        bool match = true;
        for (size_t k = 0; k < 11; k++) {
            if (profiles_lower(header->name.ptr[k]) !=
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

/* `Layout#page` appends the stylesheet preload links (`Link` header); the
 * frame layout carries none. */
static cf_err profiles_link_header(cf_ctx *ctx) {
    cf_builder links = {0};
    cf_err rc = cf_views_preload_links(&links);
    if (rc == CF_INVALID) return CF_OK;
    if (rc == CF_OK && links.len != 0) {
        rc = cf_response_header(ctx->response, profiles_span("Link"),
                                (cf_span){links.ptr, links.len});
    }
    cf_builder_dispose(&links);
    return rc;
}

/* Freeze a rendered page into the response: status, HTML content type, and
 * (page layout only) the preload header.  Consumes the builder. */
static cf_err profiles_page_response(cf_ctx *ctx, unsigned status,
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
    rc = cf_response_header(ctx->response, profiles_span("Content-Type"),
                            profiles_span("text/html; charset=utf-8"));
    if (rc != CF_OK) return rc;
    if (!frame) rc = profiles_link_header(ctx);
    return rc;
}

/* `users/profiles#show` (route 60): page_or_frame(OK, ProfileShow). */
cf_err cf_action_users_profiles_show(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    /* `before_actions(Before::default())`. */
    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    /* `respond_to([HTML])` before the user lookup (source order). */
    const cf_format *offered[1] = {&cf_format_html};
    const cf_format *chosen = NULL;
    rc = cf_ctx_respond_to(ctx, offered, 1, &chosen);
    if (rc != CF_OK) return rc;

    cf_user user = {0};
    rc = profiles_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }

    /* One read: transfer_id, attached check, memberships + user summary. */
    cf_view_users_profile_model model = {0};
    rc = cf_presenter_users_profile(ctx, &user, &model);
    cf_user_dispose(&user);
    if (rc != CF_OK) return rc;

    cf_view_layout_model layout = {0};
    rc = cf_presenter_layout_load(ctx, cf_ctx_platform(ctx), &layout);
    if (rc != CF_OK) {
        cf_view_users_profile_model_dispose(&model);
        return rc;
    }
    cf_view_ctx view_ctx;
    cf_view_ctx_init(&view_ctx, ctx, &layout);

    bool frame = profiles_turbo_frame_request(ctx->request);
    cf_builder body = {0};
    rc = frame ? cf_view_users_profile_show_frame(&view_ctx, &model, &body)
               : cf_view_users_profile_show(&view_ctx, &model, &body);
    if (rc == CF_OK) {
        rc = profiles_page_response(ctx, 200, &body, frame);
    } else {
        cf_builder_dispose(&body);
    }

    cf_view_layout_model_dispose(&layout);
    cf_view_users_profile_model_dispose(&model);
    return rc;
}

/* ---- update --------------------------------------------------------------- */

/* `Param::is_present` (messages.c convention): blank is nil, false,
 * whitespace-only strings and empty collections. */
static bool profiles_param_blank(cf_span text) {
    for (size_t i = 0; i < text.len; i++) {
        unsigned char c = text.ptr[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\v' && c != '\f' &&
            c != '\r') {
            return false;
        }
    }
    return true;
}

static bool profiles_param_present(const cf_param *param) {
    if (param == NULL) return false;
    switch (cf_param_type(param)) {
    case CF_PARAM_NULL:
        return false;
    case CF_PARAM_BOOL: {
        bool present = false, value = false;
        if (cf_param_bool(param, &present, &value) != CF_OK) return false;
        return value;
    }
    case CF_PARAM_NUMBER:
    case CF_PARAM_UPLOAD:
        return true;
    case CF_PARAM_STRING: {
        cf_span text = {NULL, 0};
        if (cf_param_string(param, &text) != CF_OK) return false;
        return !profiles_param_blank(text);
    }
    case CF_PARAM_ARRAY:
    case CF_PARAM_OBJECT:
        return cf_param_count(param) > 0;
    }
    return false;
}

/* `params.require(:user)`: present, or the false literal. */
static bool profiles_param_required(const cf_param *param) {
    if (param == NULL) return false;
    if (cf_param_type(param) == CF_PARAM_BOOL) {
        bool present = false, value = false;
        if (cf_param_bool(param, &present, &value) != CF_OK) return false;
        return true; /* false is what require accepts */
    }
    return profiles_param_present(param);
}

/* The permitted scalar for `key` (strong parameters: arrays/objects are
 * dropped); NULL when the key is absent or unpermitted. */
static const cf_param *profiles_permitted(const cf_param *object,
                                          const char *key) {
    if (object == NULL || cf_param_type(object) != CF_PARAM_OBJECT) {
        return NULL;
    }
    const cf_param *value =
        cf_param_field(object, profiles_span(key));
    if (value == NULL) return NULL;
    cf_param_kind kind = cf_param_type(value);
    if (kind == CF_PARAM_ARRAY || kind == CF_PARAM_OBJECT) return NULL;
    return value;
}

/* `string_attribute(...).flatten()`: None when the key was not given;
 * otherwise the to_s text when the value has one (uploads do not). */
static void profiles_present_text(const cf_param *object, const char *key,
                                  bool *given, bool *has_text,
                                  cf_span *text) {
    *given = false;
    *has_text = false;
    if (text != NULL) *text = (cf_span){NULL, 0};
    const cf_param *value = profiles_permitted(object, key);
    if (value == NULL) return;
    *given = true;
    cf_span out = {NULL, 0};
    if (cf_param_to_s(value, &out) != CF_OK) return; /* e.g. an upload */
    *has_text = true;
    if (text != NULL) *text = out;
}

/* Owned copy of a borrowed span. */
static cf_err profiles_str_dup(cf_span span, cf_str *out) {
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

typedef enum {
    PROFILES_AVATAR_UNCHANGED = 0,
    PROFILES_AVATAR_DELETE,
    PROFILES_AVATAR_CREATE,
    PROFILES_AVATAR_INVALID
} profiles_avatar;

/* `Assignment::from_params` for the permitted `avatar`, plus the update's
 * `.compact` arm: Delete becomes Unchanged when the permitted value is
 * absent-or-null (nil is compacted away); "" deletes. */
static profiles_avatar profiles_avatar_assignment(const cf_param *permitted) {
    const cf_param *value = profiles_permitted(permitted, "avatar");
    if (value == NULL) return PROFILES_AVATAR_UNCHANGED;
    switch (cf_param_type(value)) {
    case CF_PARAM_NULL:
        return PROFILES_AVATAR_UNCHANGED; /* compacted */
    case CF_PARAM_STRING: {
        cf_span text = {NULL, 0};
        if (cf_param_string(value, &text) != CF_OK) {
            return PROFILES_AVATAR_INVALID;
        }
        return text.len == 0 ? PROFILES_AVATAR_DELETE
                             : PROFILES_AVATAR_INVALID;
    }
    case CF_PARAM_UPLOAD:
        return PROFILES_AVATAR_CREATE;
    default:
        return PROFILES_AVATAR_INVALID;
    }
}

/* `belongs_to :record, touch: true` for the avatar's user (shared
 * cf_touch_user in src/models/touch.h). */
typedef struct {
    cf_user user; /* in/out: the reference updates the loaded record */
    cf_user_changes changes;
    profiles_avatar avatar;
    const cf_active_staged *staged;
    int64_t blob_id;
} profiles_write_arg;

/* `user.update(tx, changes)` then `attachments::assign` (Delete destroys
 * the attachment, touches the user and purges the blob after commit).
 * Re-read the user in the writer before applying any changes. */
static cf_err profiles_write_cb(cf_tx *tx, void *arg) {
    profiles_write_arg *write = arg;
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;
    /* Authentication may have changed while this callback waited. */
    bool active_found = false;
    cf_user fresh = {0};
    cf_err rc = cf_user_find_by_id(db, write->user.id, &active_found, &fresh);
    if (rc != CF_OK || !active_found || !cf_user_is_active(&fresh)) {
        cf_user_dispose(&fresh);
        return rc != CF_OK ? rc : CF_FORBIDDEN;
    }
    cf_user_dispose(&write->user);
    write->user = fresh;
    rc = cf_user_update(tx, &write->user, &write->changes);
    if (rc != CF_OK) return rc;

    switch (write->avatar) {
    case PROFILES_AVATAR_UNCHANGED:
        return CF_OK;
    case PROFILES_AVATAR_DELETE: {
        bool found = false;
        cf_attachment attachment = {0};
        rc = cf_attachment_find_for(db, (cf_str){(char *)"User", 4},
                                    write->user.id,
                                    (cf_str){(char *)"avatar", 6}, &found,
                                    &attachment);
        int64_t blob_id = 0;
        if (rc == CF_OK && found) {
            blob_id = attachment.blob_id;
            rc = cf_attachment_delete(tx, &attachment);
        }
        cf_attachment_dispose(&attachment);
        if (rc == CF_OK && found) rc = cf_touch_user(tx, &write->user);
        if (rc == CF_OK && found) {
            rc = cf_tx_event(tx, (cf_event){
                                      .kind = CF_EVENT_PURGE_BLOB,
                                      .blob_id = blob_id,
                                  });
        }
        return rc;
    }
    case PROFILES_AVATAR_CREATE:
        return cf_avatar_attach(tx, &write->user, write->staged, &write->blob_id);
    case PROFILES_AVATAR_INVALID:
        /* "Could not find or build blob". */
        return CF_INTERNAL;
    }
    return CF_INTERNAL;
}

typedef struct {
    bool has_name;
    cf_str name; /* owned when has_name */
    bool has_email;
    cf_optional_str email; /* owned when has_email (always present) */
    bool has_digest;
    cf_str digest; /* owned when has_digest */
    bool has_bio;
    cf_optional_str bio; /* owned when has_bio (always present) */
    profiles_avatar avatar;
    bool avatar_long_notice; /* raw user[avatar] non-null */
} profiles_fields;

static void profiles_fields_dispose(profiles_fields *fields) {
    if (fields == NULL) return;
    if (fields->has_name) cf_str_dispose(&fields->name);
    if (fields->has_email) cf_optional_str_dispose(&fields->email);
    if (fields->has_digest) cf_str_dispose(&fields->digest);
    if (fields->has_bio) cf_optional_str_dispose(&fields->bio);
    memset(fields, 0, sizeof *fields);
}

cf_err cf_action_users_profiles_update(cf_ctx *ctx) {
    if (ctx == NULL || ctx->response == NULL) return CF_INVALID;

    /* `before_actions(Before::default())`. */
    cf_before policy = {CF_AUTH_REQUIRED, true, true};
    cf_err rc = cf_before_actions(ctx, policy);
    if (rc != CF_OK || cf_auth_halted(ctx)) return rc;

    cf_user user = {0};
    rc = profiles_current_user(ctx, &user);
    if (rc != CF_OK) {
        cf_user_dispose(&user);
        return rc;
    }

    /* `params.require(:user).permit(:name, :avatar, :email_address,
     * :password, :bio)`: require fails missing/blank (false accepted);
     * permit of a non-hash is empty. */
    const cf_param *raw = cf_ctx_param(ctx, profiles_span("user"));
    if (!profiles_param_required(raw)) {
        cf_user_dispose(&user);
        return CF_INVALID;
    }
    const cf_param *permitted =
        (raw != NULL && cf_param_type(raw) == CF_PARAM_OBJECT) ? raw : NULL;

    profiles_fields fields;
    memset(&fields, 0, sizeof fields);
    bool given = false, has_text = false;
    cf_span text = {NULL, 0};

    profiles_present_text(permitted, "name", &given, &has_text, &text);
    if (given && has_text) {
        rc = profiles_str_dup(text, &fields.name);
        if (rc == CF_OK) fields.has_name = true;
    }
    if (rc == CF_OK) {
        profiles_present_text(permitted, "email_address", &given, &has_text,
                              &text);
        if (given && has_text) {
            fields.email.present = true;
            rc = profiles_str_dup(text, &fields.email.value);
            if (rc == CF_OK) fields.has_email = true;
        }
    }
    if (rc == CF_OK) {
        /* `password=` ignores a blank password: only the empty string is
         * filtered (whitespace-only still hashes, like the source's
         * `!password.is_empty()`). */
        profiles_present_text(permitted, "password", &given, &has_text,
                              &text);
        if (given && has_text && text.len != 0) {
            const cf_config *config = cf_app_config(ctx->app);
            if (config == NULL) {
                rc = CF_INTERNAL;
            } else {
                /* `password_digest(c, password)`: hashed off-writer; any
                 * failure is the reference's Error::internal (500), so a
                 * queue refusal collapses to CF_INTERNAL too. */
                rc = cf_auth_password_digest(
                    (cf_str){(char *)text.ptr, text.len},
                    config->bcrypt_cost, &fields.digest);
                if (rc == CF_OK) {
                    fields.has_digest = true;
                } else {
                    rc = CF_INTERNAL;
                }
            }
        }
    }
    if (rc == CF_OK) {
        profiles_present_text(permitted, "bio", &given, &has_text, &text);
        if (given && has_text) {
            fields.bio.present = true;
            rc = profiles_str_dup(text, &fields.bio.value);
            if (rc == CF_OK) fields.has_bio = true;
        }
    }
    if (rc == CF_OK) fields.avatar = profiles_avatar_assignment(permitted);

    /* `params[:user][:avatar]` non-nil (raw, unpermitted) selects the
     * 30-minute notice. */
    if (rc == CF_OK) {
        const cf_param *raw_avatar = NULL;
        if (raw != NULL && cf_param_type(raw) == CF_PARAM_OBJECT) {
            raw_avatar = cf_param_field(raw, profiles_span("avatar"));
        }
        fields.avatar_long_notice =
            raw_avatar != NULL &&
            cf_param_type(raw_avatar) != CF_PARAM_NULL;
    }

    cf_storage *storage = NULL;
    cf_active_staged staged = {0};
    if (rc == CF_OK && fields.avatar == PROFILES_AVATAR_CREATE)
        rc = cf_avatar_stage(ctx, profiles_permitted(permitted, "avatar"), &storage, &staged);
    if (rc == CF_OK && fields.avatar == PROFILES_AVATAR_INVALID) {
        /* Fail before the write, like the staged/invalid arms inside it. */
        rc = CF_INTERNAL;
    }

    if (rc == CF_OK) {
        cf_user_changes changes;
        memset(&changes, 0, sizeof changes);
        if (fields.has_name) changes.name = &fields.name;
        if (fields.has_email) changes.email_address = &fields.email;
        if (fields.has_digest) changes.password_digest = &fields.digest;
        if (fields.has_bio) changes.bio = &fields.bio;
        profiles_write_arg arg = {
            .user = user,
            .changes = changes,
            .avatar = fields.avatar,
            .staged = fields.avatar == PROFILES_AVATAR_CREATE ? &staged : NULL,
        };
        /* The write mutates the caller's row (reference semantics); the
         * action's `user` is that row. */
        rc = cf_write(ctx->app, profiles_write_cb, &arg);
        if (rc == CF_OK && arg.staged != NULL) {
            rc = cf_active_staged_commit(&staged);
            if (rc == CF_OK) cf_avatar_analyze_later(ctx, arg.blob_id);
        }
        user = arg.user;
        memset(&arg.user, 0, sizeof arg.user);
    }
    const char *notice = fields.avatar_long_notice
                             ? "It may take up to 30 minutes to change "
                               "everywhere."
                             : "\342\234\223"; /* "✓" */
    if (rc == CF_OK) rc = profiles_redirect_profile(ctx, notice);
    cf_active_staged_dispose(&staged);
    cf_storage_close(storage);
    profiles_fields_dispose(&fields);
    cf_user_dispose(&user);
    return rc;
}
