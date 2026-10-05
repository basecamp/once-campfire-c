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
 * Avatar states: Unchanged writes only the user row; Delete destroys the
 * (User, avatar) attachment when one exists (DELETE row, touch
 * users.updated_at, emit CF_EVENT_PURGE_BLOB, J02's kind) and is a no-op
 * otherwise; Create (a real upload) and Invalid (any non-empty string: the
 * source's Assignment has no signed-blob arm — S02's
 * cf_active_attach_classify SIGNED state serves other controllers and is
 * deliberately not consulted here) both fail loudly with CF_INTERNAL, the
 * reference's "Could not find or build blob" 500, instead of faking an
 * attachment (first_runs.c precedent).  Multipart never builds an upload
 * param node today, so Create is unreachable through dispatch.
 * analyze_later is a no-op in every reachable state (pending is None
 * unless a blob was attached) and needs no job.
 *
 * Render gate (A02): ProfileShow (users/profiles/show.html + _membership,
 * _transfer, layout, form/translation/asset helpers) has no C view yet, so
 * show answers the reference's 500 after loading (loud, never a stub
 * page).  Everything before the render — authentication, format
 * negotiation, transfer_id, the attached check and the direct/shared
 * partition — is real and covered below; the loader is non-static so the
 * action test exercises it directly.
 *
 * Integrator requests:
 *  R1. ProfileShow view model + renderer (src/views.h).  Proposed shape:
 *        typedef struct { cf_str transfer_id; bool avatar_attached; ... }
 *      carrying the loader outputs below plus the UserSummary; render
 *      users/profiles/show.html in the application layout.
 *  R2. A shared record-touch helper for the (User,avatar) destroy here and
 *      the push_subscriptions touch (exact SQL:
 *      UPDATE "<table>" SET "updated_at" = ? WHERE "<table>"."id" = ?).
 *      Until it lands, the static touch below (fixed statement, cache
 *      rules per db_internal.h) is the local implementation.
 *
 * c_symbols for the integrator's route rebind (src/routes.c rows 60-62):
 *   cf_action_users_profiles_show, cf_action_users_profiles_update.
 */
#include "cf.h"

#include "app.h"
#include "auth.h"
#include "config.h"
#include "context.h"
#include "db/db_internal.h"
#include "http/params.h"
#include "models/active_storage.h"
#include "models/membership.h"
#include "models/user.h"

#include <stdlib.h>
#include <string.h>

static cf_span profiles_span(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `TRANSFER_LINK_EXPIRY_DURATION`: 4 hours, in microseconds. */
#define PROFILES_TRANSFER_EXPIRY_US (INT64_C(4) * INT64_C(3600) * INT64_C(1000000))

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

/* ---- show data (everything before the gated render) ---------------------- */

/* Move one pair into a destination vector (shallow copy + zero the source
 * slot so the source dispose cannot free it twice). */
static cf_err profiles_pairs_push(cf_membership_room_pair_vector *dest,
                                  cf_membership_room_pair *slot) {
    if (dest->len == dest->cap) {
        size_t cap = dest->cap == 0 ? 4 : dest->cap * 2;
        if (cap < dest->cap ||
            cap > SIZE_MAX / sizeof *dest->items) {
            return CF_NOMEM;
        }
        cf_membership_room_pair *items =
            realloc(dest->items, cap * sizeof *items);
        if (items == NULL) return CF_NOMEM;
        dest->items = items;
        dest->cap = cap;
    }
    dest->items[dest->len] = slot[0];
    memset(slot, 0, sizeof *slot);
    dest->len++;
    return CF_OK;
}

/* Non-static so the action test exercises the pre-render logic directly:
 * transfer_id signing, the attached check and the direct/shared partition.
 * `now_us` is Time.current (the action passes the process clock).  Only
 * public types cross the boundary (messages.c msg_create_write_cb
 * precedent).  Outputs start empty and stay empty on failure. */
cf_err cf_users_profiles_load(cf_db *db, const cf_config *config,
                              int64_t user_id, int64_t now_us,
                              cf_str *transfer_id_out, bool *avatar_attached_out,
                              cf_membership_room_pair_vector *direct_out,
                              cf_membership_room_pair_vector *shared_out) {
    if (transfer_id_out != NULL) memset(transfer_id_out, 0, sizeof *transfer_id_out);
    if (avatar_attached_out != NULL) *avatar_attached_out = false;
    if (direct_out != NULL) *direct_out = (cf_membership_room_pair_vector){0};
    if (shared_out != NULL) *shared_out = (cf_membership_room_pair_vector){0};
    if (transfer_id_out == NULL || avatar_attached_out == NULL ||
        direct_out == NULL || shared_out == NULL) {
        return CF_INVALID;
    }
    if (db == NULL || config == NULL) return CF_INTERNAL;
    if (config->secret_key_base == NULL) return CF_INTERNAL;

    /* `transfer_id`: signed id purpose "transfer", 4h expiry. */
    cf_err rc = cf_auth_signed_id_generate(
        (cf_span){(const unsigned char *)config->secret_key_base,
                  config->secret_key_base_len},
        profiles_span("User"), user_id, profiles_span("transfer"), true,
        true, now_us + PROFILES_TRANSFER_EXPIRY_US, transfer_id_out);
    if (rc != CF_OK) return rc;

    /* `attached_blob(...).is_some()`: the attachment row implies its blob
     * (schema FK); the blob itself is only read by the gated render. */
    bool attached = false;
    {
        cf_attachment attachment = {0};
        rc = cf_attachment_find_for(
            db, (cf_str){(char *)"User", 4}, user_id,
            (cf_str){(char *)"avatar", 6}, &attached, &attachment);
        cf_attachment_dispose(&attachment);
        if (rc != CF_OK) {
            cf_str_dispose(transfer_id_out);
            return rc;
        }
    }
    *avatar_attached_out = attached;

    /* `profile_memberships`: with_ordered_room, partitioned by direct?
     * (relative order within each side preserved, like slice partition). */
    cf_membership_room_pair_vector pairs = {0};
    rc = cf_membership_with_ordered_room(db, user_id, &pairs);
    if (rc != CF_OK) {
        cf_str_dispose(transfer_id_out);
        return rc;
    }
    for (size_t i = 0; i < pairs.len && rc == CF_OK; i++) {
        bool direct = pairs.items[i].room.room_type == CF_ROOM_DIRECT;
        rc = profiles_pairs_push(direct ? direct_out : shared_out,
                                 &pairs.items[i]);
    }
    /* Every slot was moved (zeroed) on success; dispose frees the array
     * and any unmoved tails on failure. */
    cf_membership_room_pair_vector_dispose(&pairs);
    if (rc != CF_OK) {
        cf_str_dispose(transfer_id_out);
        cf_membership_room_pair_vector_dispose(direct_out);
        cf_membership_room_pair_vector_dispose(shared_out);
        *direct_out = (cf_membership_room_pair_vector){0};
        *shared_out = (cf_membership_room_pair_vector){0};
        return rc;
    }
    return CF_OK;
}

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
    int64_t user_id = user.id;
    if (rc == CF_OK) {
        cf_str transfer_id = {0};
        bool avatar_attached = false;
        cf_membership_room_pair_vector direct = {0};
        cf_membership_room_pair_vector shared = {0};
        rc = cf_users_profiles_load(ctx->reader, cf_app_config(ctx->app),
                                    user_id, cf_now_us(NULL), &transfer_id,
                                    &avatar_attached, &direct, &shared);
        /* user_summary + framed_page(ProfileShow) need A02's R1 view. */
        cf_str_dispose(&transfer_id);
        cf_membership_room_pair_vector_dispose(&direct);
        cf_membership_room_pair_vector_dispose(&shared);
        if (rc == CF_OK) rc = CF_INTERNAL; /* R1 render gate: loud, no stub */
    }
    cf_user_dispose(&user);
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

enum { PROFILES_STMT_TOUCH_USER };

static const cf_stmt_def profiles_stmt_defs[] = {
    [PROFILES_STMT_TOUCH_USER] = {
        "UPDATE \"users\" SET \"updated_at\" = ? WHERE \"users\".\"id\" = ?"},
};

static const cf_stmt_set profiles_stmt_set = {
    profiles_stmt_defs,
    sizeof profiles_stmt_defs / sizeof profiles_stmt_defs[0]};

/* `belongs_to :record, touch: true` for the avatar's user (R2 until a
 * shared helper lands). */
static cf_err profiles_touch_user(cf_db *db, int64_t user_id) {
    if (db == NULL) return CF_INVALID;
    char timebuf[CF_DB_TIME_TEXT_CAP];
    cf_err rc = cf_db_time_to_text(cf_now_us(NULL), timebuf);
    sqlite3_stmt *stmt = NULL;
    if (rc == CF_OK) {
        rc = cf_db_stmt(db, &profiles_stmt_set, PROFILES_STMT_TOUCH_USER,
                        &stmt);
    }
    if (rc == CF_OK) {
        rc = cf_stmt_bind_text(
            stmt, 1,
            (cf_span){(const unsigned char *)timebuf, strlen(timebuf)});
    }
    if (rc == CF_OK) rc = cf_stmt_bind_i64(stmt, 2, user_id);
    if (rc == CF_OK) {
        int step = sqlite3_step(stmt);
        if (step != SQLITE_DONE) rc = cf_db_err(step);
    }
    cf_db_stmt_done(stmt);
    return rc;
}

typedef struct {
    cf_user user; /* in/out: the reference updates the loaded record */
    cf_user_changes changes;
    profiles_avatar avatar;
} profiles_write_arg;

/* `user.update(tx, changes)` then `attachments::assign` (Delete destroys
 * the attachment, touches the user and purges the blob after commit).
 * The source performs no in-transaction re-read here. */
static cf_err profiles_write_cb(cf_tx *tx, void *arg) {
    profiles_write_arg *write = arg;
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) return CF_INTERNAL;
    cf_err rc = cf_user_update(tx, &write->user, &write->changes);
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
        if (rc == CF_OK && found) rc = profiles_touch_user(db, write->user.id);
        if (rc == CF_OK && found) {
            rc = cf_tx_event(tx, (cf_event){
                                      .kind = CF_EVENT_PURGE_BLOB,
                                      .blob_id = blob_id,
                                  });
        }
        return rc;
    }
    case PROFILES_AVATAR_CREATE:
    case PROFILES_AVATAR_INVALID:
        /* Staged-blob attach (S02) / "Could not find or build blob". */
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

    if (rc == CF_OK &&
        (fields.avatar == PROFILES_AVATAR_CREATE ||
         fields.avatar == PROFILES_AVATAR_INVALID)) {
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
        };
        /* The write mutates the caller's row (reference semantics); the
         * action's `user` is that row. */
        rc = cf_write(ctx->app, profiles_write_cb, &arg);
        user = arg.user;
        memset(&arg.user, 0, sizeof arg.user);
    }
    const char *notice = fields.avatar_long_notice
                             ? "It may take up to 30 minutes to change "
                               "everywhere."
                             : "\342\234\223"; /* "✓" */
    if (rc == CF_OK) rc = profiles_redirect_profile(ctx, notice);
    profiles_fields_dispose(&fields);
    cf_user_dispose(&user);
    return rc;
}
