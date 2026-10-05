/* src/views/users_profiles.c — users/profiles/show.html + _membership + _transfer
 * (packet V-D; task A-users-profiles R1 view).
 *
 * Reference: tmp/rust-ref/crates/views/src/users.rs (ProfileShow, Transfer,
 * ProfileMembership), templates/users/profiles/{show,_membership,_transfer}.html
 * and helpers/{application,assets,filters,forms,links,rooms,tag,turbo,url,users}.rs.
 *
 * PROPOSED views.h ADDITIONS (verbatim for the integrator; local copies below
 * are deleted when these land):
 *
 *   typedef struct {
 *       int64_t id;
 *       cf_str name;                    // owned
 *       bool has_bio; cf_str bio;       // owned when has_bio
 *       bool has_email; cf_str email_address; // owned when has_email
 *       cf_role role; cf_status status;
 *       cf_str avatar_path;             // owned: fresh_user_avatar_path
 *   } cf_view_profile_user;
 *   void cf_view_profile_user_dispose(cf_view_profile_user *user);
 *
 *   typedef struct {
 *       int64_t room_id;
 *       cf_str room_param_key;          // owned: "rooms_open"/"rooms_closed"/"rooms_direct"
 *       cf_str room_display_name;       // owned
 *       cf_str involvement;             // owned: "mentions"/"everything"/"nothing"/"invisible"
 *       bool direct;
 *   } cf_view_profile_membership;
 *   typedef struct { cf_view_profile_membership *items; size_t len, cap; }
 *       cf_view_profile_membership_vector;
 *   void cf_view_profile_membership_vector_dispose(cf_view_profile_membership_vector *v);
 *
 *   typedef struct {
 *       cf_view_profile_user user;      // owned (UserSummary)
 *       bool avatar_attached;
 *       cf_str transfer_id;             // owned (signed id, purpose "transfer")
 *       cf_view_profile_membership_vector shared_memberships; // owned
 *       cf_view_profile_membership_vector direct_memberships;  // owned
 *   } cf_view_users_profile_model;
 *   void cf_view_users_profile_model_dispose(cf_view_users_profile_model *m);
 *
 *   cf_err cf_view_users_profile_show(const cf_view_ctx *,
 *       const cf_view_users_profile_model *, cf_builder *);
 *   cf_err cf_view_users_profile_show_frame(const cf_view_ctx *,
 *       const cf_view_users_profile_model *, cf_builder *);
 *   cf_err cf_view_users_profile_transfer(const cf_view_ctx *,
 *       const cf_view_profile_user *, cf_span transfer_id, cf_builder *);
 *   cf_err cf_view_users_profile_membership(const cf_view_ctx *,
 *       const cf_view_profile_membership *, cf_builder *);
 *
 * The loader (transfer_id signing, attached check, direct/shared partition)
 * stays in src/actions/users/profiles.c (cf_users_profiles_load); the
 * display-name/param-key/involvement mapping is the presenters::accounts::
 * profile_memberships port and is supplied by the caller inside the view
 * model (rendering does no SQL).  Presenters for user_summary/avatar_path
 * follow the users.c R2 shape (fresh_user_avatar_path) and are caller-side.
 *
 * OWNERSHIP TRANSFER: users/show.html.erb also includes users/profiles/_transfer
 * (for administrators).  V-A was told NOT to implement the transfer partial;
 * it is implemented HERE as cf_view_users_profile_transfer and shared by both
 * pages.  V-A's users/show should call it (proposed above) rather than
 * duplicating it.
 *
 * Rendering follows A02 (src/views/internal.h): attributes through
 * cf_view_attr*, dynamic text through cf_view_text, builders unchanged on
 * failure, 8 MiB cap.  No CSRF tokens (D-C02; the golden masks drop them).
 * Translation keys update_password/bio are rendered from a local table until
 * the integrator adds them to src/views/translations.c (R3 below).
 *
 * Integrator requests:
 *  R1. Land the views.h declarations above (verbatim).
 *  R2. Rebind routes 60-62 to call cf_view_users_profile_show(_frame) after
 *      cf_users_profiles_load + user_summary (replacing the CF_INTERNAL gate).
 *  R3. Add "update_password" and "bio" keys to src/views/translations.c
 *      (values in profile_translation_for below, from translations_table.rs);
 *      this file's local fallback can then be deleted.
 */
#include "views/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- local copies of the proposed views.h types (deleted on land) -------- */

typedef struct {
    int64_t id;
    cf_str name;
    bool has_bio;
    cf_str bio;
    bool has_email;
    cf_str email_address;
    cf_role role;
    cf_status status;
    cf_str avatar_path;
} cf_view_profile_user;

typedef struct {
    int64_t room_id;
    cf_str room_param_key;
    cf_str room_display_name;
    cf_str involvement;
    bool direct;
} cf_view_profile_membership;

typedef struct {
    cf_view_profile_membership *items;
    size_t len, cap;
} cf_view_profile_membership_vector;

typedef struct {
    cf_view_profile_user user;
    bool avatar_attached;
    cf_str transfer_id;
    cf_view_profile_membership_vector shared_memberships;
    cf_view_profile_membership_vector direct_memberships;
} cf_view_users_profile_model;

static void profile_str_clear(cf_str *value) {
    free(value->ptr);
    memset(value, 0, sizeof *value);
}

void cf_view_profile_user_dispose(cf_view_profile_user *user) {
    if (user == NULL) return;
    profile_str_clear(&user->name);
    if (user->has_bio) profile_str_clear(&user->bio);
    if (user->has_email) profile_str_clear(&user->email_address);
    profile_str_clear(&user->avatar_path);
    memset(user, 0, sizeof *user);
}

void cf_view_profile_membership_vector_dispose(
    cf_view_profile_membership_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len; i++) {
        profile_str_clear(&vector->items[i].room_param_key);
        profile_str_clear(&vector->items[i].room_display_name);
        profile_str_clear(&vector->items[i].involvement);
    }
    free(vector->items);
    memset(vector, 0, sizeof *vector);
}

void cf_view_users_profile_model_dispose(cf_view_users_profile_model *model) {
    if (model == NULL) return;
    cf_view_profile_user_dispose(&model->user);
    profile_str_clear(&model->transfer_id);
    cf_view_profile_membership_vector_dispose(&model->shared_memberships);
    cf_view_profile_membership_vector_dispose(&model->direct_memberships);
    memset(model, 0, sizeof *model);
}

/* ---- small helpers -------------------------------------------------------- */

/* Base64 urlsafe with padding (rooms.c precedent). */
static cf_err profile_urlsafe_base64(cf_span input, cf_builder *out) {
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

/* Local translation fallback for keys the shared module lacks (R3). */
static cf_err profile_translation_for(const char *key, cf_builder *out) {
    static const struct {
        const char *key;
        const char *langs[7];
        const char *texts[7];
    } SETS[] = {
        {"update_password",
         {"\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8", "\xF0\x9F\x87\xAA\xF0\x9F\x87\xB8",
          "\xF0\x9F\x87\xAB\xF0\x9F\x87\xB7", "\xF0\x9F\x87\xAE\xF0\x9F\x87\xB3",
          "\xF0\x9F\x87\xA9\xF0\x9F\x87\xAA", "\xF0\x9F\x87\xA7\xF0\x9F\x87\xB7",
          "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5"},
         {"Change password",
          "Cambiar contrase\xC3\xB1"
          "a",
          "Changer le mot de passe",
          "\xE0\xA4\xAA\xE0\xA4\xBE\xE0\xA4\xB8\xE0\xA4\xB5\xE0\xA4\xB0"
          "\xE0\xA5\x8D\xE0\xA4\xA1 \xE0\xA4\xAC\xE0\xA4\xA6\xE0\xA4\xB2"
          "\xE0\xA5\x87\xE0\xA4\x82",
          "Passwort \xC3\xA4ndern", "Alterar senha",
          "\xE3\x83\x91\xE3\x82\xB9\xE3\x83\xAF\xE3\x83\xBC\xE3\x83\x89"
          "\xE3\x82\x92\xE5\xA4\x89\xE6\x9B\xB4"}},
        {"bio",
         {"\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8", "\xF0\x9F\x87\xAA\xF0\x9F\x87\xB8",
          "\xF0\x9F\x87\xAB\xF0\x9F\x87\xB7", "\xF0\x9F\x87\xAE\xF0\x9F\x87\xB3",
          "\xF0\x9F\x87\xA9\xF0\x9F\x87\xAA", "\xF0\x9F\x87\xA7\xF0\x9F\x87\xB7",
          "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5"},
         {"Enter a few words about yourself.",
          "Ingresa algunas palabras sobre ti mismo.",
          "Saisissez quelques mots \xC3\xA0 propos de vous-m\xC3\xAAme.",
          "\xE0\xA4\x85\xE0\xA4\xAA\xE0\xA4\xA8\xE0\xA5\x87 \xE0\xA4\xAC\xE0\xA4\xBE\xE0\xA4\xB0\xE0\xA5\x87 \xE0\xA4\xAE\xE0\xA5\x87\xE0\xA4\x82 \xE0\xA4\x95\xE0\xA5\x81\xE0\xA4\x9B \xE0\xA4\xB6\xE0\xA4\xAC\xE0\xA5\x8D\xE0\xA4\xA6 \xE0\xA4\xB2\xE0\xA4\xBF\xE0\xA4\x96\xE0\xA5\x87\xE0\xA4\x82.",
          "Geben Sie ein paar Worte \xC3\xBC"
          "ber sich selbst ein.",
          "Insira alguma palavras sobre voc\xC3\xAA.",
          "\xE3\x81\x94\xE8\x87\xAA\xE5\x88\x86\xE3\x81\xAB\xE3\x81\xA4\xE3\x81\x84\xE3\x81\xA6\xE7\xB0\xA1\xE5\x8D\x98\xE3\x81\xAB\xE8\xA8\x98\xE5\x85\xA5\xE3\x81\x97\xE3\x81\xA6\xE3\x81\x8F\xE3\x81\xA0\xE3\x81\x95\xE3\x81\x84\xE3\x80\x82"}},
    };
    for (size_t s = 0; s < sizeof SETS / sizeof SETS[0]; s++) {
        if (strcmp(SETS[s].key, key) != 0) continue;
        cf_err rc;
        cf_view_guard guard;
        cf_view_attrs attrs;
        rc = cf_view_begin(&guard, out);
        if (rc != CF_OK) return rc;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "language-list"));
        CF_VIEW_TRY(cf_view_open_start(out, "dl", &attrs));
        for (size_t i = 0; i < 7; i++) {
            cf_view_attrs dt, dd;
            cf_view_attrs_init(&dt);
            CF_VIEW_TRY(cf_view_content_text(
                out, "dt", &dt,
                (cf_span){(const unsigned char *)SETS[s].langs[i],
                          strlen(SETS[s].langs[i])}));
            cf_view_attrs_init(&dd);
            CF_VIEW_TRY(cf_view_attr_cstr(&dd, "class", "margin-none"));
            CF_VIEW_TRY(cf_view_content_text(
                out, "dd", &dd,
                (cf_span){(const unsigned char *)SETS[s].texts[i],
                          strlen(SETS[s].texts[i])}));
        }
        CF_VIEW_TRY(cf_view_close_tag(out, "dl"));
        return cf_view_finish(&guard);
    fail:
        return cf_view_fail(&guard, rc);
    }
    return CF_NOT_FOUND;
}

static cf_err profile_translation_button(const cf_view_ctx *ctx,
                                         const char *key, cf_builder *out) {
    cf_err rc = cf_view_translation_button(ctx, key, out);
    if (rc != CF_NOT_FOUND) return rc;
    /* Local fallback (R3): same details/summary/menu shape as the shared
     * helper, with the local <dl>. */
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
    CF_VIEW_TRY(profile_translation_for(key, &list));
    {
        cf_view_attrs div;
        cf_view_attrs_init(&div);
        CF_VIEW_TRY(cf_view_attr_cstr(&div, "class",
                                      "language-list-menu shadow"));
        CF_VIEW_TRY(cf_view_attr_cstr(&div, "data-popup-target", "menu"));
        CF_VIEW_TRY(
            cf_view_content(&menu, "div", &div, cf_view_span_of(&list)));
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
        CF_VIEW_TRY(cf_view_attr_cstr(&details,
                                      "data-popup-orientation-top-class",
                                      "popup-orientation-top"));
        CF_VIEW_TRY(
            cf_view_content(out, "details", &details, cf_view_span_of(&inner)));
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

/* `link_back_to(destination)`: arrow + "Go Back", class btn. */
static cf_err profile_link_back_to(const cf_view_ctx *ctx, cf_span dest,
                                   cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder body = {0};
    cf_view_attrs link, img;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_view_attrs_init(&img);
    CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
    CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 20));
    CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("arrow-left.svg"), &img,
                                  &body));
    {
        cf_view_attrs sr;
        cf_view_attrs_init(&sr);
        CF_VIEW_TRY(cf_view_attr_cstr(&sr, "class", "for-screen-reader"));
        CF_VIEW_TRY(cf_view_content_text(&body, "span", &sr,
                                         cf_span_of_lit("Go Back")));
    }
    cf_view_attrs_init(&link);
    CF_VIEW_TRY(cf_view_attr_cstr(&link, "class", "btn"));
    CF_VIEW_TRY(cf_view_attr(&link, "href", dest));
    CF_VIEW_TRY(cf_view_content(out, "a", &link, cf_view_span_of(&body)));
    cf_builder_dispose(&body);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&body);
    return cf_view_fail(&guard, rc);
}

/* `hidden_field_tag(name, value, options)`: type/name/id/value?, then options. */
static cf_err profile_hidden_field(const char *name, const char *value,
                                   bool has_value,
                                   const cf_view_attrs *options,
                                   cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_view_attrs attrs;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    cf_view_attrs_init(&attrs);
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "type", "hidden"));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "name", name));
    {
        /* sanitize_to_id: ']' removed, others non-id become '_'. */
        char id[128];
        size_t at = 0;
        for (const char *p = name; *p != '\0' && at + 1 < sizeof id; p++) {
            if (*p == ']') continue;
            char c = *p;
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                  c == ':' || c == '.')) {
                c = '_';
            }
            id[at++] = c;
        }
        id[at] = '\0';
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "id", id));
    }
    if (has_value) CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "value", value));
    if (options != NULL) {
        for (size_t i = 0; i < options->count; i++) {
            if (!options->items[i].present) continue;
            if (options->items[i].flag) {
                CF_VIEW_TRY(cf_view_attr_flag(&attrs,
                                              options->items[i].name, true));
            } else if (options->items[i].trusted) {
                CF_VIEW_TRY(cf_view_attr_raw(&attrs, options->items[i].name,
                                             options->items[i].value));
            } else {
                CF_VIEW_TRY(cf_view_attr(&attrs, options->items[i].name,
                                         options->items[i].value));
            }
        }
    }
    CF_VIEW_TRY(cf_view_legacy_tag(out, "input", &attrs));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* `image_tag(ctx, src, attrs().aria_hidden().size(n))`. */
static cf_err profile_img_hidden(const cf_view_ctx *ctx, const char *src,
                                 int size, cf_builder *out) {
    cf_err rc;
    cf_view_attrs attrs;
    cf_view_attrs_init(&attrs);
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
    if (size >= 0) CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", size));
    CF_VIEW_TRY(
        cf_view_image_tag(ctx, cf_span_of_lit(src), &attrs, out));
    return CF_OK;
fail:
    return rc;
}

/* `image_tag(ctx, src, attrs().aria_hidden().size(n).class(c))`. */
static cf_err profile_img_hidden_class(const cf_view_ctx *ctx, const char *src,
                                       int size, const char *class_attr,
                                       cf_builder *out) {
    cf_err rc;
    cf_view_attrs attrs;
    cf_view_attrs_init(&attrs);
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "aria-hidden", "true"));
    if (size >= 0) CF_VIEW_TRY(cf_view_attr_i64(&attrs, "size", size));
    CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", class_attr));
    CF_VIEW_TRY(
        cf_view_image_tag(ctx, cf_span_of_lit(src), &attrs, out));
    return CF_OK;
fail:
    return rc;
}

/* ---- involvement helpers (helpers/rooms.rs) -------------------------------- */

static const char *profile_humanize(const char *involvement) {
    if (strcmp(involvement, "mentions") == 0) return "Notifying about @ mentions";
    if (strcmp(involvement, "everything") == 0)
        return "Notifying about all messages";
    if (strcmp(involvement, "nothing") == 0) return "Notifications are off";
    if (strcmp(involvement, "invisible") == 0)
        return "Notifications are off and room invisible in sidebar";
    return "";
}

static const char *profile_next(bool direct, const char *involvement) {
    if (direct) {
        if (strcmp(involvement, "everything") == 0) return "nothing";
        return "everything";
    }
    if (strcmp(involvement, "mentions") == 0) return "everything";
    if (strcmp(involvement, "everything") == 0) return "nothing";
    if (strcmp(involvement, "nothing") == 0) return "invisible";
    if (strcmp(involvement, "invisible") == 0) return "mentions";
    return "mentions";
}

/* users/profiles/_membership.html. */
cf_err cf_view_users_profile_membership(
    const cf_view_ctx *ctx, const cf_view_profile_membership *membership,
    cf_builder *out) {
    if (ctx == NULL || membership == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder frame_id = {0}, label_id = {0}, url = {0}, link_body = {0},
               btn_body = {0};
    /* Function scope: cf_view_attr borrows its value span, so the room href
     * must outlive the cf_view_content call below (not a block temporary). */
    char room_href[32];
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    CF_VIEW_TRY(cf_view_str(
        out,
        "<li class=\"flex align-center gap margin-none min-width "
        "membership-item\">\n  "));
    /* link_to(room): class then href. */
    {
        cf_view_attrs link;
        cf_view_attrs_init(&link);
        CF_VIEW_TRY(cf_view_attr_cstr(
            &link, "class",
            "overflow-ellipsis fill-shade txt-primary txt-undecorated"));
        {
            int n = snprintf(room_href, sizeof room_href, "/rooms/%lld",
                             (long long)membership->room_id);
            if (n < 0 || (size_t)n >= sizeof room_href) {
                CF_VIEW_TRY(CF_INTERNAL);
            }
            CF_VIEW_TRY(cf_view_attr_cstr(&link, "href", room_href));
        }
        CF_VIEW_TRY(cf_view_str(&link_body, "\n    "));
        /* <strong>display</strong> with escaping. */
        {
            cf_builder strong = {0};
            CF_VIEW_TRY(cf_view_str(&strong, "<strong>"));
            CF_VIEW_TRY(cf_view_text(
                &strong, (cf_span){(const unsigned char *)
                                       membership->room_display_name.ptr,
                                   membership->room_display_name.len}));
            CF_VIEW_TRY(cf_view_str(&strong, "</strong>\n"));
            CF_VIEW_TRY(cf_view_raw(&link_body, cf_view_span_of(&strong)));
            cf_builder_dispose(&strong);
        }
        CF_VIEW_TRY(cf_view_content(out, "a", &link,
                                    cf_view_span_of(&link_body)));
    }
    CF_VIEW_TRY(cf_view_str(
        out, "\n  <hr class=\"separator\" aria-hidden=\"true\">\n\n"
             "  <span class=\"txt-small\">\n    "));
    /* dom_id(param_key, room_id, "involvement"). */
    {
        char idbuf[32];
        int n = snprintf(idbuf, sizeof idbuf, "%lld",
                         (long long)membership->room_id);
        if (n < 0 || (size_t)n >= sizeof idbuf) CF_VIEW_TRY(CF_INTERNAL);
        CF_VIEW_TRY(cf_view_str(&frame_id, "involvement_"));
        CF_VIEW_TRY(cf_view_raw(
            &frame_id,
            (cf_span){(const unsigned char *)membership->room_param_key.ptr,
                      membership->room_param_key.len}));
        CF_VIEW_TRY(cf_view_str(&frame_id, "_"));
        CF_VIEW_TRY(cf_view_str(&frame_id, idbuf));
        CF_VIEW_TRY(cf_view_str(&label_id, "involvement_label_"));
        CF_VIEW_TRY(cf_view_raw(
            &label_id,
            (cf_span){(const unsigned char *)membership->room_param_key.ptr,
                      membership->room_param_key.len}));
        CF_VIEW_TRY(cf_view_str(&label_id, "_"));
        CF_VIEW_TRY(cf_view_str(&label_id, idbuf));
        /* with_query(room_involvement, involvement=next). */
        CF_VIEW_TRY(cf_view_str(&url, "/rooms/"));
        CF_VIEW_TRY(cf_view_str(&url, idbuf));
        CF_VIEW_TRY(cf_view_str(&url, "/involvement?involvement="));
        CF_VIEW_TRY(cf_view_str(
            &url, profile_next(membership->direct,
                               membership->involvement.ptr != NULL
                                   ? membership->involvement.ptr
                                   : "")));
    }
    /* button content: bell image + screen-reader span. */
    {
        char icon[64];
        snprintf(icon, sizeof icon, "notification-bell-%s.svg",
                 membership->involvement.ptr != NULL
                     ? membership->involvement.ptr
                     : "");
        CF_VIEW_TRY(profile_img_hidden(ctx, icon, 20, &btn_body));
        {
            cf_view_attrs sr;
            cf_view_attrs_init(&sr);
            CF_VIEW_TRY(cf_view_attr_cstr(&sr, "class", "for-screen-reader"));
            CF_VIEW_TRY(
                cf_view_attr(&sr, "id", cf_view_span_of(&label_id)));
            CF_VIEW_TRY(cf_view_content_text(
                &btn_body, "span", &sr,
                (cf_span){(const unsigned char *)profile_humanize(
                              membership->involvement.ptr != NULL
                                  ? membership->involvement.ptr
                                  : ""),
                          strlen(profile_humanize(
                              membership->involvement.ptr != NULL
                                  ? membership->involvement.ptr
                                  : ""))}));
        }
    }
    /* button_to(url) with method put: button_to consumes `method` for the
     * form's _method field, so it travels separately from the button attrs. */
    {
        cf_view_attrs button_opts;
        cf_view_attrs_init(&button_opts);
        CF_VIEW_TRY(cf_view_attr_cstr(&button_opts, "role", "checkbox"));
        CF_VIEW_TRY(
            cf_view_attr_cstr(&button_opts, "aria-checked", "true"));
        CF_VIEW_TRY(cf_view_attr(&button_opts, "aria-labelledby",
                                 cf_view_span_of(&label_id)));
        CF_VIEW_TRY(cf_view_attr_cstr(&button_opts, "tabindex", "0"));
        {
            char cls[64];
            snprintf(cls, sizeof cls, "btn %s",
                     membership->involvement.ptr != NULL
                         ? membership->involvement.ptr
                         : "");
            CF_VIEW_TRY(cf_view_attr_cstr(&button_opts, "class", cls));
        }
        cf_builder button_form = {0};
        rc = cf_view_button_to(
            &button_form, cf_view_span_of(&url), &button_opts,
            (cf_span){(const unsigned char *)"put", 3},
            (cf_span){NULL, 0}, cf_view_span_of(&btn_body));
        if (rc != CF_OK) {
            cf_builder_dispose(&button_form);
            goto fail;
        }
        cf_view_attrs frame;
        cf_view_attrs_init(&frame);
        CF_VIEW_TRY(
            cf_view_attr(&frame, "id", cf_view_span_of(&frame_id)));
        /* Block-filter whitespace: "\n      " + form + "\n". */
        {
            cf_builder framed = {0};
            CF_VIEW_TRY(cf_view_str(&framed, "\n      "));
            CF_VIEW_TRY(cf_view_raw(&framed, cf_view_span_of(&button_form)));
            CF_VIEW_TRY(cf_view_str(&framed, "\n"));
            CF_VIEW_TRY(cf_view_content(out, "turbo-frame", &frame,
                                        cf_view_span_of(&framed)));
            cf_builder_dispose(&framed);
        }
        cf_builder_dispose(&button_form);
    }
    CF_VIEW_TRY(cf_view_str(out, "  </span>\n</li>\n"));
    cf_builder_dispose(&frame_id);
    cf_builder_dispose(&label_id);
    cf_builder_dispose(&url);
    cf_builder_dispose(&link_body);
    cf_builder_dispose(&btn_body);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&frame_id);
    cf_builder_dispose(&label_id);
    cf_builder_dispose(&url);
    cf_builder_dispose(&link_body);
    cf_builder_dispose(&btn_body);
    return cf_view_fail(&guard, rc);
}

/* ---- users/profiles/_transfer.html (SHARED with users/show) -----------------
 * Transfer{ctx, user, transfer_id}: url = ctx.url(session_transfer(id)).
 * is_current_user selects the label arm.  V-A's users/show calls this too.
 */
cf_err cf_view_users_profile_transfer(const cf_view_ctx *ctx,
                                      const cf_view_profile_user *user,
                                      cf_span transfer_id, cf_builder *out) {
    if (ctx == NULL || user == NULL || out == NULL) return CF_INVALID;
    if (transfer_id.len != 0 && transfer_id.ptr == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder url = {0}, qr = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    CF_VIEW_TRY(cf_view_raw(&url, ctx->base_url));
    {
        char tmp[64];
        int n = snprintf(tmp, sizeof tmp, "/session/transfers/%.*s",
                         (int)transfer_id.len,
                         transfer_id.ptr != NULL ? (const char *)transfer_id.ptr
                                                 : "");
        if (n < 0 || (size_t)n >= sizeof tmp) {
            /* transfer ids exceed 64 bytes: append piecewise. */
            CF_VIEW_TRY(cf_view_str(&url, "/session/transfers/"));
            CF_VIEW_TRY(cf_view_raw(&url, transfer_id));
        } else {
            CF_VIEW_TRY(cf_view_raw(
                &url, (cf_span){(const unsigned char *)tmp, (size_t)n}));
        }
    }
    /* QR path: /qr_code/<urlsafe_b64(url)>. */
    CF_VIEW_TRY(cf_view_str(&qr, "/qr_code/"));
    CF_VIEW_TRY(profile_urlsafe_base64(cf_view_span_of(&url), &qr));

    CF_VIEW_TRY(cf_view_str(out, "<fieldset>\n  <legend class=\"gap\">\n    "));
    CF_VIEW_TRY(profile_img_hidden_class(ctx, "laptop.svg", 36,
                                         "colorize--black", out));
    CF_VIEW_TRY(cf_view_str(out, "\n    "));
    CF_VIEW_TRY(profile_img_hidden_class(ctx, "transfer.svg", 36,
                                         "colorize--black", out));
    CF_VIEW_TRY(cf_view_str(out, "\n    "));
    CF_VIEW_TRY(profile_img_hidden_class(ctx, "mobile-phone.svg", 36,
                                         "colorize--black", out));
    CF_VIEW_TRY(cf_view_str(
        out, "\n  </legend>\n\n\n  <div class=\"flex flex-column gap\">\n"));
    {
        bool is_current = ctx->current_user.has_user &&
                          ctx->current_user.id == user->id;
        if (!is_current) {
            CF_VIEW_TRY(cf_view_str(
                out, "      <div class=\"flex align-center gap "
                     "justify-center\">\n        "));
            {
                cf_view_attrs crown;
                cf_view_attrs_init(&crown);
                CF_VIEW_TRY(cf_view_attr_i64(&crown, "size", 16));
                CF_VIEW_TRY(cf_view_attr_cstr(&crown, "aria-hidden", "true"));
                CF_VIEW_TRY(cf_view_attr_cstr(
                    &crown, "class", "flex-item-no-shrink colorize--black"));
                CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("crown.svg"),
                                              &crown, out));
            }
            CF_VIEW_TRY(cf_view_str(
                out, "\n        <label for=\"session_transfer_url\">Share to "
                     "get them back into their account</label>\n"
                     "      </div>\n"));
        } else {
            CF_VIEW_TRY(cf_view_str(
                out,
                "      <label for=\"session_transfer_url\" "
                "class=\"for-screen-reader\">Use this link to login "
                "automatically on another device</label>\n"));
        }
    }
    CF_VIEW_TRY(cf_view_str(out, "    <input type=\"text\" class=\"input\" "
                                 "value=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&url)));
    CF_VIEW_TRY(cf_view_str(
        out, "\" id=\"session_transfer_url\" readonly>\n\n"
             "    <div class=\"flex align-center center gap\">\n      "));
    /* link_to_zoom_qr_code(url). */
    {
        cf_view_attrs link;
        cf_view_attrs_init(&link);
        CF_VIEW_TRY(cf_view_attr_cstr(&link, "class", "btn"));
        CF_VIEW_TRY(cf_view_attr_cstr(&link, "data-lightbox-target", "image"));
        CF_VIEW_TRY(cf_view_attr_cstr(&link, "data-action", "lightbox#open"));
        CF_VIEW_TRY(cf_view_attr(&link, "data-lightbox-url-value",
                                 cf_view_span_of(&qr)));
        CF_VIEW_TRY(cf_view_attr(&link, "href", cf_view_span_of(&qr)));
        cf_builder body = {0};
        CF_VIEW_TRY(cf_view_str(
            &body, "\n        <span class=\"for-screen-reader\">Show "
                   "auto-login QR code</span>\n        "));
        CF_VIEW_TRY(profile_img_hidden_class(ctx, "qr-code.svg", 20,
                                              "colorize--black", &body));
        CF_VIEW_TRY(cf_view_str(&body, "\n"));
        CF_VIEW_TRY(cf_view_content(out, "a", &link, cf_view_span_of(&body)));
        cf_builder_dispose(&body);
    }
    CF_VIEW_TRY(cf_view_str(out, "\n      "));
    /* button_to_copy_to_clipboard(url). */
    {
        cf_view_attrs btn;
        cf_view_attrs_init(&btn);
        CF_VIEW_TRY(cf_view_attr_cstr(&btn, "class", "btn"));
        CF_VIEW_TRY(
            cf_view_attr_cstr(&btn, "data-controller", "copy-to-clipboard"));
        CF_VIEW_TRY(cf_view_attr_cstr(&btn, "data-action",
                                      "copy-to-clipboard#copy"));
        CF_VIEW_TRY(cf_view_attr_cstr(&btn,
                                      "data-copy-to-clipboard-success-class",
                                      "btn--success"));
        CF_VIEW_TRY(cf_view_attr(&btn, "data-copy-to-clipboard-content-value",
                                 cf_view_span_of(&url)));
        cf_builder body = {0};
        CF_VIEW_TRY(cf_view_str(
            &body, "\n        <span class=\"for-screen-reader\">Copy "
                   "auto-login link</span>\n        "));
        {
            cf_view_attrs img;
            cf_view_attrs_init(&img);
            CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 20));
            CF_VIEW_TRY(cf_view_attr_cstr(
                &img, "class", "flex-item-no-shrink colorize--black"));
            CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("copy-paste.svg"),
                                          &img, &body));
        }
        CF_VIEW_TRY(cf_view_str(&body, "\n"));
        CF_VIEW_TRY(cf_view_content(out, "button", &btn,
                                    cf_view_span_of(&body)));
        cf_builder_dispose(&body);
    }
    CF_VIEW_TRY(cf_view_str(out, "\n      "));
    /* web_share_session_button(url, title, text). */
    {
        cf_view_attrs btn;
        cf_view_attrs_init(&btn);
        CF_VIEW_TRY(cf_view_attr_cstr(&btn, "class", "btn"));
        CF_VIEW_TRY(cf_view_attr_flag(&btn, "hidden", true));
        CF_VIEW_TRY(cf_view_attr_cstr(&btn, "data-controller", "web-share"));
        CF_VIEW_TRY(
            cf_view_attr_cstr(&btn, "data-action", "web-share#share"));
        CF_VIEW_TRY(cf_view_attr(&btn, "data-web-share-url-value",
                                 cf_view_span_of(&url)));
        CF_VIEW_TRY(cf_view_attr_cstr(
            &btn, "data-web-share-text-value",
            "This is your own private sign-in URL, DO NOT SHARE IT. Use it "
            "to sign-in on another device or if you get locked out."));
        CF_VIEW_TRY(cf_view_attr_cstr(&btn, "data-web-share-title-value",
                                      "Your sign-in link"));
        cf_builder body = {0};
        CF_VIEW_TRY(cf_view_str(
            &body, "\n        <span class=\"for-screen-reader\">Share "
                   "auto-login link</span>\n        "));
        {
            cf_view_attrs img;
            cf_view_attrs_init(&img);
            CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 20));
            CF_VIEW_TRY(cf_view_attr_cstr(
                &img, "class", "flex-item-no-shrink colorize--black"));
            CF_VIEW_TRY(cf_view_image_tag(ctx, cf_span_of_lit("share.svg"),
                                          &img, &body));
        }
        CF_VIEW_TRY(cf_view_str(&body, "\n"));
        CF_VIEW_TRY(cf_view_content(out, "button", &btn,
                                    cf_view_span_of(&body)));
        cf_builder_dispose(&body);
    }
    CF_VIEW_TRY(cf_view_str(out, "    </div>\n  </div>\n</fieldset>\n"));
    cf_builder_dispose(&url);
    cf_builder_dispose(&qr);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&url);
    cf_builder_dispose(&qr);
    return cf_view_fail(&guard, rc);
}

/* ---- avatar forms ---------------------------------------------------------- */

static cf_err profile_file_input(const cf_view_form *form) {
    /* FormBuilder::input_field("file", ...) with fetch_or_set name/id: the
     * template's explicit id="file" wins over the auto user_avatar id.
     * (cf_view_form_field replaces id; the shared helper serves callers
     * without an explicit id.) */
    cf_view_attrs opts;
    cf_view_attrs_init(&opts);
    cf_err rc = cf_view_attr_cstr(&opts, "id", "file");
    if (rc != CF_OK) return rc;
    rc = cf_view_attr_cstr(&opts, "class", "input");
    if (rc != CF_OK) return rc;
    rc = cf_view_attr_cstr(&opts, "accept", "image/*");
    if (rc != CF_OK) return rc;
    rc = cf_view_attr_cstr(&opts, "data-upload-preview-target", "input");
    if (rc != CF_OK) return rc;
    rc = cf_view_attr_cstr(&opts, "data-action",
                           "upload-preview#previewImage change->form#submit");
    if (rc != CF_OK) return rc;
    if (cf_view_attr_get(&opts, "type") == NULL) {
        rc = cf_view_attr_cstr(&opts, "type", "file");
        if (rc != CF_OK) return rc;
    }
    if (cf_view_attr_get(&opts, "name") == NULL) {
        cf_builder name = {0};
        if (form->param_key != NULL) {
            rc = cf_view_str(&name, form->param_key);
            if (rc == CF_OK) rc = cf_view_str(&name, "[avatar]");
        } else {
            rc = cf_view_str(&name, "avatar");
        }
        if (rc == CF_OK) {
            rc = cf_view_attr(&opts, "name", cf_view_span_of(&name));
        }
        if (rc == CF_OK) rc = cf_view_legacy_tag(form->out, "input", &opts);
        cf_builder_dispose(&name);
        return rc;
    }
    return cf_view_legacy_tag(form->out, "input", &opts);
}

/* The avatar__form div with its two file forms and the optional delete. */
static cf_err profile_avatar_block(const cf_view_ctx *ctx,
                                   const cf_view_users_profile_model *model,
                                   cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(
        out, "  <div class=\"align-center center avatar__form gap\" "
             "data-controller=\"upload-preview\">\n    "));
    /* Camera form (class txt-medium). */
    {
        cf_view_form form;
        cf_view_form_init(&form, out);
        cf_builder action = {0};
        CF_VIEW_TRY(cf_view_str(&action, "/users/me/profile"));
        form.action = cf_view_span_of(&action);
        form.method = "patch";
        form.param_key = "user";
        form.class_attr = "txt-medium";
        cf_view_attrs_init(&form.data);
        form.has_data = true;
        rc = cf_view_attr_cstr(&form.data, "data-controller", "form");
        if (rc != CF_OK) {
            cf_builder_dispose(&action);
            goto fail;
        }
        form.multipart = true;
        /* form_with block: open, label, close. */
        {
            cf_builder inner = {0};
            CF_VIEW_TRY(cf_view_str(&inner, "\n      <label class=\"btn "
                                             "input--file\">\n        "));
            CF_VIEW_TRY(profile_img_hidden(ctx, "camera.svg", 20, &inner));
            CF_VIEW_TRY(cf_view_str(&inner, "\n        "));
            {
                cf_view_form inner_form;
                cf_view_form_init(&inner_form, &inner);
                inner_form.param_key = "user";
                CF_VIEW_TRY(profile_file_input(&inner_form));
            }
            CF_VIEW_TRY(cf_view_str(
                &inner, "\n        <span class=\"for-screen-reader\">Upload "
                        "avatar</span>\n      </label>\n"));
            rc = cf_view_form_open(&form);
            if (rc != CF_OK) {
                cf_builder_dispose(&inner);
                cf_builder_dispose(&action);
                goto fail;
            }
            rc = cf_view_raw(out, cf_view_span_of(&inner));
            cf_builder_dispose(&inner);
            if (rc != CF_OK) {
                cf_builder_dispose(&action);
                goto fail;
            }
            CF_VIEW_TRY(cf_view_str(out, "</form>"));
        }
        cf_builder_dispose(&action);
    }
    CF_VIEW_TRY(cf_view_str(out, "\n    "));
    /* Avatar image form. */
    {
        cf_view_form form;
        cf_view_form_init(&form, out);
        cf_builder action = {0};
        CF_VIEW_TRY(cf_view_str(&action, "/users/me/profile"));
        form.action = cf_view_span_of(&action);
        form.method = "patch";
        form.param_key = "user";
        form.class_attr = NULL;
        cf_view_attrs_init(&form.data);
        form.has_data = true;
        rc = cf_view_attr_cstr(&form.data, "data-controller", "form");
        if (rc != CF_OK) {
            cf_builder_dispose(&action);
            goto fail;
        }
        form.multipart = true;
        {
            cf_builder inner = {0};
            CF_VIEW_TRY(cf_view_str(
                &inner, "\n      <label class=\"btn avatar input--file "
                        "txt-xx-large\">\n        "));
            {
                cf_view_attrs img;
                cf_view_attrs_init(&img);
                CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
                CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 300));
                CF_VIEW_TRY(cf_view_attr_cstr(
                    &img, "data-upload-preview-target", "image"));
                CF_VIEW_TRY(cf_view_image_tag(
                    ctx,
                    (cf_span){(const unsigned char *)
                                  model->user.avatar_path.ptr,
                              model->user.avatar_path.len},
                    &img, &inner));
            }
            CF_VIEW_TRY(cf_view_str(&inner, "\n        "));
            {
                cf_view_form inner_form;
                cf_view_form_init(&inner_form, &inner);
                inner_form.param_key = "user";
                CF_VIEW_TRY(profile_file_input(&inner_form));
            }
            CF_VIEW_TRY(cf_view_str(
                &inner, "\n        <span class=\"for-screen-reader\">Avatar"
                        "</span>\n      </label>\n"));
            rc = cf_view_form_open(&form);
            if (rc != CF_OK) {
                cf_builder_dispose(&inner);
                cf_builder_dispose(&action);
                goto fail;
            }
            rc = cf_view_raw(out, cf_view_span_of(&inner));
            cf_builder_dispose(&inner);
            if (rc != CF_OK) {
                cf_builder_dispose(&action);
                goto fail;
            }
            CF_VIEW_TRY(cf_view_str(out, "</form>"));
        }
        cf_builder_dispose(&action);
    }
    if (model->avatar_attached) {
        CF_VIEW_TRY(cf_view_str(out, "  "));
        cf_view_attrs opts;
        cf_view_attrs_init(&opts);
        CF_VIEW_TRY(cf_view_attr_cstr(
            &opts, "class", "btn btn--negative txt-small avatar__delete-btn"));
        cf_builder body = {0};
        CF_VIEW_TRY(cf_view_str(&body, "\n        "));
        CF_VIEW_TRY(profile_img_hidden(ctx, "minus.svg", 20, &body));
        CF_VIEW_TRY(cf_view_str(
            &body, "\n        <span class=\"for-screen-reader\">Delete "
                   "avatar</span>\n"));
        {
            char path[48];
            int n = snprintf(path, sizeof path, "/users/%lld/avatar",
                             (long long)model->user.id);
            if (n < 0 || (size_t)n >= sizeof path) {
                cf_builder_dispose(&body);
                CF_VIEW_TRY(CF_INTERNAL);
            }
            rc = cf_view_button_to(
                out, (cf_span){(const unsigned char *)path, (size_t)n},
                &opts, (cf_span){(const unsigned char *)"delete", 6},
                (cf_span){NULL, 0}, cf_view_span_of(&body));
            cf_builder_dispose(&body);
            if (rc != CF_OK) goto fail;
        }
    }
    CF_VIEW_TRY(cf_view_str(out, "  </div>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* ---- main profile form ------------------------------------------------------ */

static cf_err profile_textarea(const cf_view_form *form, const char *method,
                               const cf_str *value, bool has_value,
                               const cf_view_attrs *options,
                               cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_view_attrs attrs;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    if (options != NULL) {
        memcpy(&attrs, options, sizeof attrs);
    } else {
        cf_view_attrs_init(&attrs);
    }
    /* name/id last (fetch_or_set semantics: only when absent). */
    {
        cf_builder name = {0}, id = {0};
        if (form->param_key == NULL) {
            CF_VIEW_TRY(cf_view_str(&name, method));
            CF_VIEW_TRY(cf_view_str(&id, method));
        } else {
            CF_VIEW_TRY(cf_view_str(&name, form->param_key));
            CF_VIEW_TRY(cf_view_str(&name, "["));
            CF_VIEW_TRY(cf_view_str(&name, method));
            CF_VIEW_TRY(cf_view_str(&name, "]"));
            for (const char *p = form->param_key; *p != '\0'; p++) {
                char c = *p;
                bool keep = (c >= 'a' && c <= 'z') ||
                            (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == ':' ||
                            c == '.';
                if (keep) {
                    CF_VIEW_TRY(cf_view_raw(
                        &id, (cf_span){(const unsigned char *)p, 1}));
                } else {
                    CF_VIEW_TRY(cf_view_str(&id, "_"));
                }
            }
            CF_VIEW_TRY(cf_view_str(&id, "_"));
            CF_VIEW_TRY(cf_view_str(&id, method));
        }
        if (cf_view_attr_get(&attrs, "name") == NULL) {
            CF_VIEW_TRY(
                cf_view_attr(&attrs, "name", cf_view_span_of(&name)));
        }
        if (cf_view_attr_get(&attrs, "id") == NULL) {
            CF_VIEW_TRY(cf_view_attr(&attrs, "id", cf_view_span_of(&id)));
        }
        CF_VIEW_TRY(cf_view_open_start(out, "textarea", &attrs));
        CF_VIEW_TRY(cf_view_str(out, "\n"));
        if (has_value && value != NULL) {
            CF_VIEW_TRY(cf_view_text(
                out, (cf_span){(const unsigned char *)value->ptr,
                               value->len}));
        }
        CF_VIEW_TRY(cf_view_close_tag(out, "textarea"));
        cf_builder_dispose(&name);
        cf_builder_dispose(&id);
    }
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

static cf_err profile_submit_button(const cf_view_ctx *ctx, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder body = {0};
    cf_view_attrs btn;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(profile_img_hidden(ctx, "check.svg", 20, &body));
    {
        cf_view_attrs sr;
        cf_view_attrs_init(&sr);
        CF_VIEW_TRY(cf_view_attr_cstr(&sr, "class", "for-screen-reader"));
        CF_VIEW_TRY(
            cf_view_content_text(&body, "span", &sr,
                                 cf_span_of_lit("Save changes")));
    }
    cf_view_attrs_init(&btn);
    CF_VIEW_TRY(
        cf_view_attr_cstr(&btn, "class", "btn btn--reversed center txt-large"));
    CF_VIEW_TRY(cf_view_attr_cstr(&btn, "type", "submit"));
    CF_VIEW_TRY(cf_view_content(out, "button", &btn, cf_view_span_of(&body)));
    cf_builder_dispose(&body);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&body);
    return cf_view_fail(&guard, rc);
}

static cf_err profile_main_form(const cf_view_ctx *ctx,
                                const cf_view_users_profile_model *model,
                                cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder action = {0}, inner = {0};
    cf_view_form form;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(&action, "/users/me/profile"));
    cf_view_form_init(&form, &inner);
    form.action = cf_view_span_of(&action);
    form.method = "patch";
    form.param_key = "user";
    form.class_attr = NULL;
    cf_view_attrs_init(&form.data);
    form.has_data = true;
    CF_VIEW_TRY(cf_view_attr_cstr(&form.data, "data-controller", "form"));
    form.multipart = false;

    CF_VIEW_TRY(cf_view_str(&inner, "\n    <div class=\"flex flex-column "
                                    "gap\">\n      <div class=\"flex "
                                    "align-center gap\">\n        "));
    CF_VIEW_TRY(profile_translation_button(ctx, "user_name", &inner));
    CF_VIEW_TRY(cf_view_str(
        &inner, "\n\n        <label class=\"flex align-center gap "
                "flex-item-grow input input--actor\">\n          "));
    {
        cf_view_attrs opts;
        cf_view_attrs_init(&opts);
        CF_VIEW_TRY(cf_view_attr_cstr(&opts, "class", "input txt-large "));
        CF_VIEW_TRY(cf_view_attr_cstr(&opts, "autocomplete", "name"));
        CF_VIEW_TRY(
            cf_view_attr_cstr(&opts, "placeholder", "Enter your name"));
        CF_VIEW_TRY(cf_view_attr_flag(&opts, "autofocus", true));
        CF_VIEW_TRY(cf_view_attr_flag(&opts, "required", true));
        CF_VIEW_TRY(cf_view_attr_cstr(&opts, "data-1p-ignore", "true"));
        CF_VIEW_TRY(cf_view_form_field(
            &form, "text", "name",
            (cf_span){(const unsigned char *)model->user.name.ptr,
                      model->user.name.len},
            true, &opts));
    }
    CF_VIEW_TRY(cf_view_str(&inner, "\n          "));
    CF_VIEW_TRY(profile_img_hidden_class(ctx, "person.svg", 24,
                                         "colorize--black", &inner));
    CF_VIEW_TRY(cf_view_str(
        &inner, "\n        </label>\n      </div>\n\n      <div class=\"flex "
                "align-center gap\">\n        "));
    CF_VIEW_TRY(profile_translation_button(ctx, "email_address", &inner));
    CF_VIEW_TRY(cf_view_str(
        &inner, "\n\n        <label class=\"flex align-center gap "
                "flex-item-grow input input--actor\">\n          "));
    {
        cf_view_attrs opts;
        cf_view_attrs_init(&opts);
        CF_VIEW_TRY(cf_view_attr_cstr(&opts, "class", "input txt-large"));
        if (model->user.has_email) {
            CF_VIEW_TRY(cf_view_attr(
                &opts, "value",
                (cf_span){(const unsigned char *)
                              model->user.email_address.ptr,
                          model->user.email_address.len}));
        }
        CF_VIEW_TRY(cf_view_attr_cstr(&opts, "autocomplete", "username"));
        CF_VIEW_TRY(cf_view_attr_cstr(&opts, "placeholder",
                                      "Enter your email address"));
        CF_VIEW_TRY(cf_view_attr_flag(&opts, "required", false));
        /* The option's value keeps its position (hash assignment replaces in
         * place); has_value re-asserts it so form_field cannot drop it. */
        CF_VIEW_TRY(cf_view_form_field(
            &form, "email", "email_address",
            model->user.has_email
                ? (cf_span){(const unsigned char *)
                                model->user.email_address.ptr,
                            model->user.email_address.len}
                : (cf_span){NULL, 0},
            model->user.has_email, &opts));
    }
    CF_VIEW_TRY(cf_view_str(&inner, "\n          "));
    CF_VIEW_TRY(profile_img_hidden_class(ctx, "email.svg", 24,
                                         "colorize--black", &inner));
    CF_VIEW_TRY(cf_view_str(
        &inner, "\n        </label>\n      </div>\n\n      <div class=\"flex "
                "align-center gap\">\n        "));
    CF_VIEW_TRY(profile_translation_button(ctx, "update_password", &inner));
    CF_VIEW_TRY(cf_view_str(
        &inner, "\n\n        <label class=\"flex align-center gap "
                "flex-item-grow input input--actor\">\n          "));
    {
        cf_view_attrs opts;
        cf_view_attrs_init(&opts);
        CF_VIEW_TRY(cf_view_attr_cstr(&opts, "class", "input txt-large"));
        CF_VIEW_TRY(cf_view_attr_cstr(&opts, "autocomplete", "new-password"));
        CF_VIEW_TRY(
            cf_view_attr_cstr(&opts, "placeholder", "Change password"));
        CF_VIEW_TRY(cf_view_attr_flag(&opts, "required", false));
        CF_VIEW_TRY(cf_view_attr_i64(&opts, "maxlength", 72));
        CF_VIEW_TRY(cf_view_form_password_field(&form, "password", &opts));
    }
    CF_VIEW_TRY(cf_view_str(&inner, "\n          "));
    CF_VIEW_TRY(profile_img_hidden_class(ctx, "password.svg", 24,
                                         "colorize--black", &inner));
    CF_VIEW_TRY(cf_view_str(
        &inner, "\n        </label>\n      </div>\n\n      <div class=\"flex "
                "align-start gap\">\n        "));
    CF_VIEW_TRY(profile_translation_button(ctx, "bio", &inner));
    CF_VIEW_TRY(cf_view_str(
        &inner, "\n\n        <label class=\"flex align--center gap "
                "flex-item--grow input input--actor\">\n          "));
    {
        cf_view_attrs opts;
        cf_view_attrs_init(&opts);
        CF_VIEW_TRY(cf_view_attr_cstr(&opts, "class", "input txt-large"));
        CF_VIEW_TRY(cf_view_attr_cstr(&opts, "placeholder",
                                      "A few words about yourself\xE2\x80\xA6"));
        CF_VIEW_TRY(cf_view_attr_i64(&opts, "maxlength", 200));
        CF_VIEW_TRY(cf_view_attr_i64(&opts, "rows", 3));
        CF_VIEW_TRY(cf_view_attr_flag(&opts, "required", false));
        CF_VIEW_TRY(profile_textarea(
            &form, "bio",
            model->user.has_bio ? &model->user.bio : NULL,
            model->user.has_bio, &opts, &inner));
    }
    CF_VIEW_TRY(cf_view_str(&inner, "\n          "));
    CF_VIEW_TRY(profile_img_hidden_class(ctx, "bio.svg", 24,
                                         "colorize--black", &inner));
    CF_VIEW_TRY(cf_view_str(
        &inner, "\n        </label>\n      </div>\n\n      "));
    CF_VIEW_TRY(profile_submit_button(ctx, &inner));
    CF_VIEW_TRY(cf_view_str(&inner, "\n    </div>\n"));

    /* The open tag leads: fields rendered into `inner` above, the form
     * element itself goes straight to the page. */
    form.out = out;
    rc = cf_view_form_open(&form);
    if (rc != CF_OK) {
        cf_builder_dispose(&action);
        cf_builder_dispose(&inner);
        return cf_view_fail(&guard, rc);
    }
    CF_VIEW_TRY(cf_view_raw(out, cf_view_span_of(&inner)));
    CF_VIEW_TRY(cf_view_str(out, "</form>"));
    cf_builder_dispose(&action);
    cf_builder_dispose(&inner);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&action);
    cf_builder_dispose(&inner);
    return cf_view_fail(&guard, rc);
}

/* ---- memberships menu ------------------------------------------------------- */

static cf_err profile_memberships_block(
    const cf_view_ctx *ctx, const cf_view_users_profile_model *model,
    cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(out, "  <div class=\"margin-block pad-inline "
                                 "pad-block fill-shade border-radius\">\n"
                                 "    <menu class=\"flex flex-column gap "
                                 "margin-none pad\">\n"));
    for (size_t i = 0; i < model->shared_memberships.len; i++) {
        CF_VIEW_TRY(cf_view_str(out, "      "));
        CF_VIEW_TRY(cf_view_users_profile_membership(
            ctx, &model->shared_memberships.items[i], out));
    }
    if (model->direct_memberships.len != 0 &&
        model->shared_memberships.len != 0) {
        CF_VIEW_TRY(cf_view_str(out, "        <hr class=\"separator "
                                     "full-width\" style=\"--border-style: "
                                     "solid\">\n"));
    }
    for (size_t i = 0; i < model->direct_memberships.len; i++) {
        CF_VIEW_TRY(cf_view_str(out, "      "));
        CF_VIEW_TRY(cf_view_users_profile_membership(
            ctx, &model->direct_memberships.items[i], out));
    }
    CF_VIEW_TRY(
        cf_view_str(out, "    </menu>\n  </div>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* ---- page blocks -------------------------------------------------------------- */

static cf_err profile_nav(const cf_view_ctx *ctx, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder dest = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    /* link_back: the referrer unless missing (else root).  Paths are
     * relative (link_to takes the route path); cf_view_ctx carries no
     * referrer (V-A G2 gap), so fixtures with referrer None link root. */
    CF_VIEW_TRY(cf_view_str(out, "  <div class=\"flex-item-justify-start\">\n"
                                 "    "));
    CF_VIEW_TRY(cf_view_str(&dest, "/"));
    CF_VIEW_TRY(profile_link_back_to(
        ctx, (cf_span){dest.ptr, dest.len}, out));
    CF_VIEW_TRY(cf_view_str(out, "\n  </div>\n\n  <div "
                                 "class=\"flex-item-justify-end\">\n    "));
    {
        cf_view_form form;
        cf_view_form_init(&form, out);
        cf_builder action = {0};
        CF_VIEW_TRY(cf_view_str(&action, "/session"));
        form.action = cf_view_span_of(&action);
        form.method = "delete";
        form.param_key = NULL;
        form.class_attr = NULL;
        cf_view_attrs_init(&form.data);
        form.has_data = true;
        rc = cf_view_attr_cstr(&form.data, "data-controller", "sessions");
        if (rc != CF_OK) {
            cf_builder_dispose(&action);
            cf_builder_dispose(&dest);
            return cf_view_fail(&guard, rc);
        }
        form.multipart = false;
        {
            cf_builder inner = {0};
            CF_VIEW_TRY(cf_view_str(&inner, "\n      "));
            {
                cf_view_attrs opts;
                cf_view_attrs_init(&opts);
                CF_VIEW_TRY(cf_view_attr_cstr(
                    &opts, "data-sessions-target",
                    "pushSubscriptionEndpoint"));
                CF_VIEW_TRY(profile_hidden_field(
                    "push_subscription_endpoint", NULL, false, &opts,
                    &inner));
            }
            CF_VIEW_TRY(cf_view_str(&inner, "\n\n      <button class=\"btn\" "
                                            "data-action=\"sessions#logout:"
                                            "prevent\">\n        "));
            CF_VIEW_TRY(profile_img_hidden(ctx, "logout.svg", -1, &inner));
            CF_VIEW_TRY(cf_view_str(
                &inner, "\n        <span class=\"for-screen-reader\">Log "
                        "out</span>\n      </button>\n"));
            rc = cf_view_form_open(&form);
            if (rc != CF_OK) {
                cf_builder_dispose(&inner);
                cf_builder_dispose(&action);
                cf_builder_dispose(&dest);
                return cf_view_fail(&guard, rc);
            }
            rc = cf_view_raw(out, cf_view_span_of(&inner));
            cf_builder_dispose(&inner);
            if (rc != CF_OK) {
                cf_builder_dispose(&action);
                cf_builder_dispose(&dest);
                return cf_view_fail(&guard, rc);
            }
            CF_VIEW_TRY(cf_view_str(out, "</form>"));
        }
        cf_builder_dispose(&action);
    }
    CF_VIEW_TRY(cf_view_str(out, "  </div>\n"));
    cf_builder_dispose(&dest);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&dest);
    return cf_view_fail(&guard, rc);
}

static cf_err profile_content(const cf_view_ctx *ctx,
                              const cf_view_users_profile_model *model,
                              cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    {
        char style[64];
        int n = snprintf(style, sizeof style,
                         "view-transition-name: avatar-%lld",
                         (long long)model->user.id);
        if (n < 0 || (size_t)n >= sizeof style) CF_VIEW_TRY(CF_INTERNAL);
        CF_VIEW_TRY(cf_view_str(
            out, "<section class=\"panel flex flex-column gap\" style=\""));
        CF_VIEW_TRY(cf_view_str(out, style));
        CF_VIEW_TRY(cf_view_str(out, "\">\n  "));
    }
    CF_VIEW_TRY(cf_view_pwa_install_instructions(ctx, out));
    CF_VIEW_TRY(cf_view_str(out, "\n\n"));
    CF_VIEW_TRY(profile_avatar_block(ctx, model, out));
    CF_VIEW_TRY(cf_view_str(out, "\n"));
    CF_VIEW_TRY(profile_main_form(ctx, model, out));
    CF_VIEW_TRY(cf_view_str(out, "\n"));
    CF_VIEW_TRY(profile_memberships_block(ctx, model, out));
    CF_VIEW_TRY(cf_view_str(out, "\n  "));
    CF_VIEW_TRY(cf_view_users_profile_transfer(
        ctx, &model->user,
        (cf_span){(const unsigned char *)model->transfer_id.ptr,
                  model->transfer_id.len},
        out));
    CF_VIEW_TRY(cf_view_str(out, "</section>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_users_profile_show(const cf_view_ctx *ctx,
                                  const cf_view_users_profile_model *model,
                                  cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    if (model->user.name.ptr == NULL || model->transfer_id.ptr == NULL) {
        return CF_INVALID;
    }
    cf_err rc;
    cf_view_guard guard;
    cf_builder head = {0}, content = {0}, nav = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = profile_content(ctx, model, &content);
    if (rc == CF_OK) rc = profile_nav(ctx, &nav);
    if (rc == CF_OK) {
        rc = cf_view_layout_page(
            ctx,
            (cf_span){(const unsigned char *)model->user.name.ptr,
                      model->user.name.len},
            true, (cf_span){NULL, 0}, false, cf_view_span_of(&head),
            cf_view_span_of(&content), cf_view_span_of(&nav),
            (cf_span){NULL, 0}, (cf_span){NULL, 0}, out);
    }
    cf_builder_dispose(&head);
    cf_builder_dispose(&content);
    cf_builder_dispose(&nav);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_users_profile_show_frame(
    const cf_view_ctx *ctx, const cf_view_users_profile_model *model,
    cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    if (model->user.name.ptr == NULL || model->transfer_id.ptr == NULL) {
        return CF_INVALID;
    }
    cf_err rc;
    cf_view_guard guard;
    cf_builder head = {0}, content = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = profile_content(ctx, model, &content);
    if (rc == CF_OK) {
        rc = cf_view_layout_frame(ctx, cf_view_span_of(&head),
                                  cf_view_span_of(&content), out);
    }
    cf_builder_dispose(&head);
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}
