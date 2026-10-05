/* src/views/rooms_forms.c — V-E: the room-subclass new/edit forms plus the
 * sidebar room partials the room broadcasts carry
 * (tmp/rust-ref/crates/views/templates/rooms/{opens,closeds,directs}/,
 * users/sidebars/rooms/{_shared,_direct}, crates/views/src/rooms.rs,
 * crates/views/src/users.rs SidebarRoom/SidebarDirect, helpers/{rooms,
 * users,forms,translations,application}.rs and campfire_routes).
 *
 * Source-as-spec (03-application.md A02): the pinned templates and helpers
 * above.  Rendering does no SQL/mutation/IO beyond the context's asset
 * closure; every renderer writes into the caller's cf_builder under the
 * 8 MiB cap and leaves it at its entry length on failure (00-contracts.md,
 * the CF_VIEW_TRY/CF_VIEW_FAIL pattern from src/views/internal.h).
 *
 * Family boundaries (03-application.md): open/closed/direct rooms keep
 * distinct permissions and participant selection.  The selection itself is
 * the actions' job (opens: creator + every active user; closeds: the
 * submitted grantees partitioned into selected/unselected; directs: no
 * gate on new, actor + member set on edit); these renderers take the
 * selection as given and render each subclass's distinct controls exactly:
 * opens rows never carry `user_ids[]` checkboxes, closeds rows do (with the
 * new-room current-user hidden input), directs/new is the autocomplete
 * select, directs/edit the member cards plus the delete-ping form.
 *
 * INTEGRATOR (views.h is integrator-owned; the declarations below are the
 * proposed addition, to be MOVED into src/views.h verbatim — struct blocks
 * plus prototypes — and removed here):
 *
 *   typedef struct {
 *       bool is_new; int64_t room_id;
 *       bool has_name; cf_str name;
 *       bool can_administer;
 *       bool has_last_room_id; int64_t last_room_id;
 *       const cf_view_user *users; size_t user_count;
 *   } cf_view_rooms_open_form_model;
 *   void cf_view_rooms_open_form_dispose(cf_view_rooms_open_form_model *model);
 *   cf_err cf_view_rooms_open_form(const cf_view_ctx *ctx,
 *       const cf_view_rooms_open_form_model *model, cf_builder *out);
 *   cf_err cf_view_rooms_open_form_frame(const cf_view_ctx *ctx,
 *       const cf_view_rooms_open_form_model *model, cf_builder *out);
 *
 *   typedef struct {
 *       bool is_new; int64_t room_id;
 *       bool has_name; cf_str name;
 *       bool can_administer; int64_t current_user_id;
 *       bool has_last_room_id; int64_t last_room_id;
 *       const cf_view_user *selected; size_t selected_count;
 *       const cf_view_user *unselected; size_t unselected_count;
 *   } cf_view_rooms_closed_form_model;
 *   void cf_view_rooms_closed_form_dispose(cf_view_rooms_closed_form_model *model);
 *   cf_err cf_view_rooms_closed_form(const cf_view_ctx *ctx,
 *       const cf_view_rooms_closed_form_model *model, cf_builder *out);
 *   cf_err cf_view_rooms_closed_form_frame(const cf_view_ctx *ctx,
 *       const cf_view_rooms_closed_form_model *model, cf_builder *out);
 *
 *   cf_err cf_view_rooms_direct_new(const cf_view_ctx *ctx, cf_builder *out);
 *   cf_err cf_view_rooms_direct_new_frame(const cf_view_ctx *ctx,
 *       cf_builder *out);
 *   typedef struct {
 *       int64_t room_id; cf_str display_name;
 *       bool has_last_room_id; int64_t last_room_id;
 *       const cf_view_user *users; size_t user_count;
 *   } cf_view_rooms_direct_edit_model;
 *   void cf_view_rooms_direct_edit_dispose(cf_view_rooms_direct_edit_model *model);
 *   cf_err cf_view_rooms_direct_edit(const cf_view_ctx *ctx,
 *       const cf_view_rooms_direct_edit_model *model, cf_builder *out);
 *   cf_err cf_view_rooms_direct_edit_frame(const cf_view_ctx *ctx,
 *       const cf_view_rooms_direct_edit_model *model, cf_builder *out);
 *
 *   cf_err cf_view_rooms_shared_room_partial(void *user,
 *       const cf_room *room, cf_builder *out);
 *   cf_err cf_view_rooms_direct_room_partial(void *user,
 *       const cf_membership *membership, cf_builder *out);
 *
 * Notes on the proposal versus the actions' R1/R2 shims (which these
 * replace; parameter/return types match the shims exactly):
 *  - The R1 proposals name a struct and its renderer identically
 *    (e.g. `cf_view_rooms_open_form` for both), which C forbids.  The
 *    structs take the codebase's `_model` suffix
 *    (cf_view_room_show_model precedent); function names are unchanged.
 *  - `has_name` carries FormRoom.name's Option (an absent name omits the
 *    input's value attribute and empties the edit title/display name).
 *  - `has_last_room_id/last_room_id` carry ViewContext.last_room_visited_id
 *    for the nav's back link (`/` when absent).  cf_view_ctx does not carry
 *    it (cf_view_layout_model does); either keep it on these models or land
 *    it on cf_view_ctx and drop the fields here.
 *  - User arrays are borrowed (the actions own the cf_view_user rows); only
 *    the owned name/display strings dispose.
 *  - `cf_view_rooms_direct_new` takes no model, like the reference
 *    DirectsNew (the template has no locals).
 *
 * R2 wiring (cable/src/actions are integrator-owned):
 *  - `cf_view_rooms_shared_room_partial` is the `Partials::shared_room`
 *    behind render_shared_room (SidebarRoom with unread=false, exactly the
 *    reference presenter): wire it into cf_broadcast_partials_views'
 *    shared_room slot (currently CF_NOT_FOUND) and swap the opens/closeds
 *    actions' static shims for it.  It unlocks the involvements
 *    update-from-invisible prepend branch (currently 404-after-commit).
 *  - `cf_view_rooms_direct_room_partial` is `Partials::direct_room`
 *    (SidebarDirect assembled like the reference presenter.sidebar_direct,
 *    rendered through cf_view_sidebar_direct_partial).  `user` must be the
 *    cable module's cf_broadcast_views (ctx drives the reads and avatar
 *    signing, view the render); NULL is CF_INVALID.  Wire it into
 *    cf_broadcast_partials_views' direct_room slot and swap the directs
 *    action's static shim for it (setting partials.user to the views pair).
 *
 * Local translation table: the room_name key is not in src/views/
 * translations.c yet, so the form's translation button is rendered here
 * from the pinned translations_table.rs entries (same markup as
 * cf_view_translation_button).  When translations.c lands room_name, drop
 * the local table and call cf_view_translation_button(ctx, "room_name").
 */
#include "views/internal.h"

#include "cable/channels.h"
#include "models/membership.h"
#include "models/room.h"
#include "models/user.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- proposed views.h additions (see the header comment) ------------------ */

typedef struct {
    bool is_new;
    int64_t room_id; /* valid when !is_new */
    bool has_name;
    cf_str name; /* owned when has_name */
    bool can_administer;
    bool has_last_room_id;
    int64_t last_room_id; /* valid when has_last_room_id */
    const cf_view_user *users; /* borrowed */
    size_t user_count;
} cf_view_rooms_open_form_model;

typedef struct {
    bool is_new;
    int64_t room_id; /* valid when !is_new */
    bool has_name;
    cf_str name; /* owned when has_name */
    bool can_administer;
    int64_t current_user_id;
    bool has_last_room_id;
    int64_t last_room_id; /* valid when has_last_room_id */
    const cf_view_user *selected; /* borrowed */
    size_t selected_count;
    const cf_view_user *unselected; /* borrowed */
    size_t unselected_count;
} cf_view_rooms_closed_form_model;

typedef struct {
    int64_t room_id;
    cf_str display_name; /* owned */
    bool has_last_room_id;
    int64_t last_room_id; /* valid when has_last_room_id */
    const cf_view_user *users; /* borrowed */
    size_t user_count;
} cf_view_rooms_direct_edit_model;

void cf_view_rooms_open_form_dispose(cf_view_rooms_open_form_model *model) {
    if (model == NULL) return;
    if (model->has_name) cf_str_dispose(&model->name);
    memset(model, 0, sizeof *model);
}

void cf_view_rooms_closed_form_dispose(cf_view_rooms_closed_form_model *model) {
    if (model == NULL) return;
    if (model->has_name) cf_str_dispose(&model->name);
    memset(model, 0, sizeof *model);
}

void cf_view_rooms_direct_edit_dispose(cf_view_rooms_direct_edit_model *model) {
    if (model == NULL) return;
    cf_str_dispose(&model->display_name);
    memset(model, 0, sizeof *model);
}

/* Forward declarations (helpers defined in template order below). */
static cf_err button_to_delete_room(const cf_view_ctx *ctx, int64_t room_id,
                                    cf_span display_name, cf_builder *out);

/* ---- small pieces --------------------------------------------------------- */

static cf_span lit(const char *text) {
    return (cf_span){(const unsigned char *)text, strlen(text)};
}

/* `User#name.to_lowercase` for the row's data-value (ASCII fold; other bytes
 * pass through — the golden fixtures use ASCII names). */
static cf_err lower_name(cf_span name, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    for (size_t i = 0; i < name.len; i++) {
        unsigned char c = name.ptr[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        rc = cf_view_raw(out, (cf_span){&c, 1});
        if (rc != CF_OK) return cf_view_fail(&guard, rc);
    }
    return cf_view_finish(&guard);
}

/* `RoomsHelper#link_back_to_last_room_visited`: the back link to the last
 * room, else root. */
static cf_err back_link(const cf_view_ctx *ctx, bool has_last_room_id,
                        int64_t last_room_id, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder path = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    {
        char buf[32];
        int n;
        if (has_last_room_id) {
            n = snprintf(buf, sizeof buf, "/rooms/%lld",
                         (long long)last_room_id);
        } else {
            n = snprintf(buf, sizeof buf, "/");
        }
        if (n < 0 || (size_t)n >= sizeof buf) return cf_view_fail(&guard, rc);
        rc = cf_view_raw(&path, (cf_span){(const unsigned char *)buf,
                                          (size_t)n});
        if (rc != CF_OK) {
            cf_builder_dispose(&path);
            return cf_view_fail(&guard, rc);
        }
    }
    {
        cf_builder content = {0};
        rc = CF_OK;
        {
            cf_view_attrs img;
            cf_view_attrs_init(&img);
            if (rc == CF_OK) {
                rc = cf_view_attr_cstr(&img, "aria-hidden", "true");
            }
            if (rc == CF_OK) rc = cf_view_attr_i64(&img, "size", 20);
            if (rc == CF_OK) {
                rc = cf_view_image_tag(ctx, lit("arrow-left.svg"), &img,
                                       &content);
            }
        }
        if (rc == CF_OK) {
            rc = cf_view_str(&content,
                             "<span class=\"for-screen-reader\">Go Back"
                             "</span>");
        }
        if (rc == CF_OK) {
            cf_view_attrs link;
            cf_view_attrs_init(&link);
            rc = cf_view_attr_cstr(&link, "class", "btn");
            if (rc == CF_OK) {
                rc = cf_view_attr(&link, "href", cf_view_span_of(&path));
            }
            if (rc == CF_OK) {
                rc = cf_view_content(out, "a", &link,
                                     cf_view_span_of(&content));
            }
        }
        cf_builder_dispose(&content);
    }
    cf_builder_dispose(&path);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

/* The `_new`/`_edit` layouts' nav block. */
static cf_err rooms_nav(const cf_view_ctx *ctx, bool has_last_room_id,
                        int64_t last_room_id, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(out, "<div class=\"flex-item-justify-start\">\n"));
    CF_VIEW_TRY(back_link(ctx, has_last_room_id, last_room_id, out));
    CF_VIEW_TRY(cf_view_str(out, "\n</div>"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* ---- the room_name translation button (local until translations.c lands
 * the key; same markup as cf_view_translation_button) ----------------------- */

static const char *const room_name_languages[] = {
    "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8",
    "\xF0\x9F\x87\xAA\xF0\x9F\x87\xB8",
    "\xF0\x9F\x87\xAB\xF0\x9F\x87\xB7",
    "\xF0\x9F\x87\xAE\xF0\x9F\x87\xB3",
    "\xF0\x9F\x87\xA9\xF0\x9F\x87\xAA",
    "\xF0\x9F\x87\xA7\xF0\x9F\x87\xB7",
    "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5",
};

static const char *const room_name_texts[] = {
    "Name the room",
    "Nombrar la sala",
    "Nommez la salle",
    "\xE0\xA4\x95\xE0\xA4\xAE\xE0\xA4\xB0\xE0\xA5\x87 \xE0\xA4\x95\xE0\xA4\xBE "
    "\xE0\xA4\xA8\xE0\xA4\xBE\xE0\xA4\xAE \xE0\xA4\xA6\xE0\xA5\x87\xE0\xA4\x82",
    "Geben Sie dem Raum einen Namen",
    "D\xC3\xAA um nome a essa sala",
    "\xE3\x83\xAB\xE3\x83\xBC\xE3\x83\xA0\xE3\x81\xAB\xE5\x90\x8D\xE5\x89\x8D"
    "\xE3\x82\x92\xE4\xBB\x98\xE3\x81\x91\xE3\x82\x8B",
};

/* `translations_for("room_name")`. */
static cf_err room_name_list(cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    {
        cf_view_attrs attrs;
        cf_view_attrs_init(&attrs);
        CF_VIEW_TRY(cf_view_attr_cstr(&attrs, "class", "language-list"));
        CF_VIEW_TRY(cf_view_open_start(out, "dl", &attrs));
    }
    for (size_t i = 0;
         i < sizeof room_name_languages / sizeof room_name_languages[0];
         i++) {
        cf_view_attrs dt;
        cf_view_attrs_init(&dt);
        CF_VIEW_TRY(cf_view_content_text(
            out, "dt", &dt,
            (cf_span){(const unsigned char *)room_name_languages[i],
                      strlen(room_name_languages[i])}));
        cf_view_attrs dd;
        cf_view_attrs_init(&dd);
        CF_VIEW_TRY(cf_view_attr_cstr(&dd, "class", "margin-none"));
        CF_VIEW_TRY(cf_view_content_text(
            out, "dd", &dd,
            (cf_span){(const unsigned char *)room_name_texts[i],
                      strlen(room_name_texts[i])}));
    }
    CF_VIEW_TRY(cf_view_close_tag(out, "dl"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* `translation_button(ctx, "room_name")`. */
static cf_err room_name_button(const cf_view_ctx *ctx, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder summary = {0}, menu = {0}, list = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    CF_VIEW_TRY(cf_view_str(&summary, "<summary class=\"btn\" tabindex=\"-1\">"));
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "size", "20"));
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "class", "color-icon"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, lit("globe.svg"), &img, &summary));
        cf_view_attrs sr;
        cf_view_attrs_init(&sr);
        CF_VIEW_TRY(cf_view_attr_cstr(&sr, "class", "for-screen-reader"));
        CF_VIEW_TRY(cf_view_content_text(&summary, "span", &sr,
                                         lit("Translate")));
    }
    CF_VIEW_TRY(cf_view_str(&summary, "</summary>"));

    CF_VIEW_TRY(room_name_list(&list));
    {
        cf_view_attrs div;
        cf_view_attrs_init(&div);
        CF_VIEW_TRY(cf_view_attr_cstr(&div, "class",
                                      "language-list-menu shadow"));
        CF_VIEW_TRY(cf_view_attr_cstr(&div, "data-popup-target", "menu"));
        CF_VIEW_TRY(cf_view_content(&menu, "div", &div,
                                    cf_view_span_of(&list)));
    }
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
        CF_VIEW_TRY(cf_view_open_start(out, "details", &details));
    }
    CF_VIEW_TRY(cf_view_raw(out, cf_view_span_of(&summary)));
    CF_VIEW_TRY(cf_view_raw(out, cf_view_span_of(&menu)));
    CF_VIEW_TRY(cf_view_close_tag(out, "details"));
    cf_builder_dispose(&summary);
    cf_builder_dispose(&menu);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&summary);
    cf_builder_dispose(&menu);
    cf_builder_dispose(&list);
    return cf_view_fail(&guard, rc);
}

/* rooms/layouts/_form: the form around the open/closed fields
 * (`room_form` filter).  `action` is FormRoom.action (relative); `kind_open`
 * selects the opens versus closeds type-change wording downstream. */
static cf_err room_form_open(bool is_new, int64_t room_id, bool kind_open,
                             cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    {
        char buf[64];
        int n;
        if (is_new) {
            n = snprintf(buf, sizeof buf, "/rooms/%s",
                         kind_open ? "opens" : "closeds");
        } else if (kind_open) {
            n = snprintf(buf, sizeof buf, "/rooms/opens/%lld",
                         (long long)room_id);
        } else {
            n = snprintf(buf, sizeof buf, "/rooms/closeds/%lld",
                         (long long)room_id);
        }
        if (n < 0 || (size_t)n >= sizeof buf) {
            rc = CF_INTERNAL;
            goto fail;
        }
        CF_VIEW_TRY(cf_view_str(out, "<form action=\""));
        CF_VIEW_TRY(cf_view_raw(
            out, (cf_span){(const unsigned char *)buf, (size_t)n}));
        CF_VIEW_TRY(cf_view_str(out,
                                "\" accept-charset=\"UTF-8\" method=\"post\">"));
    }
    if (!is_new) CF_VIEW_TRY(cf_view_method_tag(out, "patch"));
    CF_VIEW_TRY(cf_view_str(out, "\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* The name row: the editable input for administrators, else the display
 * heading. */
static cf_err room_name_row(const cf_view_ctx *ctx, bool can_administer,
                            bool has_name, cf_span name, cf_span display_name,
                            cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(out, "<div class=\"flex align-center gap\">\n"));
    if (can_administer) {
        CF_VIEW_TRY(room_name_button(ctx, out));
        CF_VIEW_TRY(cf_view_str(out, "\n<label class=\"flex-item-grow "
                                     "txt-large\">\n"));
        {
            cf_view_attrs input;
            cf_view_attrs_init(&input);
            CF_VIEW_TRY(cf_view_attr_cstr(&input, "name", "room[name]"));
            CF_VIEW_TRY(cf_view_attr_cstr(&input, "id", "room_name"));
            CF_VIEW_TRY(cf_view_attr_cstr(&input, "class", "input full-width"));
            CF_VIEW_TRY(cf_view_attr_cstr(&input, "required", "required"));
            CF_VIEW_TRY(cf_view_attr_cstr(&input, "autofocus", "autofocus"));
            CF_VIEW_TRY(cf_view_attr_cstr(&input, "placeholder",
                                          "Name the room"));
            CF_VIEW_TRY(cf_view_attr_cstr(&input, "data-turbo-permanent",
                                          "true"));
            CF_VIEW_TRY(cf_view_attr_cstr(
                &input, "data-action",
                "keydown.enter->form#submit:prevent"));
            CF_VIEW_TRY(cf_view_attr_cstr(&input, "type", "text"));
            CF_VIEW_TRY(cf_view_attr_opt(&input, "value", has_name, name));
            CF_VIEW_TRY(cf_view_legacy_tag(out, "input", &input));
        }
        CF_VIEW_TRY(cf_view_str(out, "\n<span class=\"for-screen-reader\">Name "
                                     "this room</span>\n"));
        CF_VIEW_TRY(cf_view_str(out, "</label>\n"));
    } else {
        CF_VIEW_TRY(cf_view_str(out, "\n<h1 class=\"flex-item-grow "
                                     "txt-x-large\"> "));
        CF_VIEW_TRY(cf_view_text(out, display_name));
        CF_VIEW_TRY(cf_view_str(out, " </h1>\n"));
    }
    CF_VIEW_TRY(cf_view_str(out, "</div>\n"));
    CF_VIEW_TRY(cf_view_str(out, "<hr class=\"margin-block borderless\">\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* The avatar link the room-form user rows share (title/class/turbo-frame/
 * href around the lazy avatar image). */
static cf_err user_avatar_link(const cf_view_ctx *ctx,
                               const cf_view_user *user, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder src = {0};
    /* Stack number buffers borrowed by attribute spans must outlive the tag
     * render (attrs borrow; cf_view_*_tag copies nothing). */
    char id_buf[32];
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = cf_view_ctx_asset(
        ctx, (cf_span){(const unsigned char *)user->avatar_url.ptr,
                       user->avatar_url.len},
        &src);
    if (rc != CF_OK) {
        cf_builder_dispose(&src);
        return cf_view_fail(&guard, rc);
    }
    {
        cf_view_attrs link;
        cf_view_attrs_init(&link);
        CF_VIEW_TRY(cf_view_attr(
            &link, "title",
            (cf_span){(const unsigned char *)user->title.ptr,
                      user->title.len}));
        CF_VIEW_TRY(cf_view_attr_cstr(&link, "class", "btn avatar"));
        CF_VIEW_TRY(cf_view_attr_cstr(&link, "data-turbo-frame", "_top"));
        {
            int n = snprintf(id_buf, sizeof id_buf, "/users/%lld",
                             (long long)user->id);
            if (n < 0 || (size_t)n >= sizeof id_buf) {
                rc = CF_INTERNAL;
                goto fail;
            }
            CF_VIEW_TRY(cf_view_attr(
                &link, "href",
                (cf_span){(const unsigned char *)id_buf, (size_t)n}));
        }
        CF_VIEW_TRY(cf_view_open_start(out, "a", &link));
    }
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "loading", "lazy"));
        CF_VIEW_TRY(cf_view_attr(&img, "src", cf_view_span_of(&src)));
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "width", "48"));
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "height", "48"));
        CF_VIEW_TRY(cf_view_legacy_tag(out, "img", &img));
    }
    CF_VIEW_TRY(cf_view_close_tag(out, "a"));
    cf_builder_dispose(&src);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&src);
    return cf_view_fail(&guard, rc);
}

/* The user row shell the open/closed forms share (avatar, name, separator)
 * with the subclass control appended by the caller. */
static cf_err user_row_open(const cf_view_ctx *ctx, const cf_view_user *user,
                            cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder value = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = lower_name((cf_span){(const unsigned char *)user->name.ptr,
                              user->name.len},
                    &value);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    {
        cf_view_attrs li;
        cf_view_attrs_init(&li);
        CF_VIEW_TRY(cf_view_attr_cstr(
            &li, "class", "flex align-center gap margin-none"));
        CF_VIEW_TRY(cf_view_attr(&li, "data-value", cf_view_span_of(&value)));
        CF_VIEW_TRY(cf_view_open_start(out, "li", &li));
    }
    cf_builder_dispose(&value);
    CF_VIEW_TRY(cf_view_str(out, "\n<figure class=\"avatar flex-item-no-shrink\" "
                                 "style=\"--avatar-size: 4ch;\">\n"));
    CF_VIEW_TRY(user_avatar_link(ctx, user, out));
    CF_VIEW_TRY(cf_view_str(out, "\n</figure>\n"));
    CF_VIEW_TRY(cf_view_str(out, "<div class=\"min-width\">\n"
                                 "<div class=\"overflow-ellipsis fill-shade\">"
                                 "<strong>"));
    CF_VIEW_TRY(cf_view_text(
        out, (cf_span){(const unsigned char *)user->name.ptr,
                       user->name.len}));
    CF_VIEW_TRY(cf_view_str(out, "</strong></div>\n</div>\n"));
    CF_VIEW_TRY(cf_view_str(out, "<hr class=\"separator\" aria-hidden=\"true\">"
                                 "\n"));
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&value);
    return cf_view_fail(&guard, rc);
}

static cf_err user_row_close(cf_builder *out) {
    return cf_view_str(out, "</li>\n");
}

/* `user_filter_menu_tag { content }`. */
static cf_err filter_menu_open(cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    {
        cf_view_attrs menu;
        cf_view_attrs_init(&menu);
        CF_VIEW_TRY(cf_view_attr_cstr(
            &menu, "class",
            "flex flex-column gap margin-none pad overflow-y "
            "constrain-height"));
        CF_VIEW_TRY(cf_view_attr_cstr(&menu, "data-controller", "filter"));
        CF_VIEW_TRY(cf_view_attr_cstr(&menu, "data-filter-active-class",
                                      "filter--active"));
        CF_VIEW_TRY(cf_view_attr_cstr(&menu, "data-filter-selected-class",
                                      "selected"));
        CF_VIEW_TRY(cf_view_open_start(out, "menu", &menu));
    }
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* `user_filter_search_tag`. */
static cf_err filter_search_tag(cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    {
        cf_view_attrs input;
        cf_view_attrs_init(&input);
        CF_VIEW_TRY(cf_view_attr_cstr(&input, "type", "search"));
        CF_VIEW_TRY(cf_view_attr_cstr(&input, "id", "search"));
        CF_VIEW_TRY(cf_view_attr_cstr(&input, "autocorrect", "off"));
        CF_VIEW_TRY(cf_view_attr_cstr(&input, "autocomplete", "off"));
        CF_VIEW_TRY(cf_view_attr_cstr(&input, "data-1p-ignore", "true"));
        CF_VIEW_TRY(cf_view_attr_cstr(
            &input, "class", "input input--transparent full-width"));
        CF_VIEW_TRY(cf_view_attr_cstr(&input, "placeholder",
                                      "Filter\xE2\x80\xA6"));
        CF_VIEW_TRY(cf_view_attr_cstr(&input, "data-action",
                                      "input->filter#filter"));
        CF_VIEW_TRY(cf_view_builder_tag(out, "input", &input));
    }
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* The Everyone row's type-switch link (opens: checked + "only some access";
 * closeds: unchecked + "everyone access"). */
static cf_err everyone_switch(bool kind_open, bool is_new, int64_t room_id,
                              cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    {
        char buf[64];
        int n;
        if (is_new) {
            n = snprintf(buf, sizeof buf, "/rooms/%s/new",
                         kind_open ? "closeds" : "opens");
        } else if (kind_open) {
            n = snprintf(buf, sizeof buf, "/rooms/closeds/%lld/edit",
                         (long long)room_id);
        } else {
            n = snprintf(buf, sizeof buf, "/rooms/opens/%lld/edit",
                         (long long)room_id);
        }
        if (n < 0 || (size_t)n >= sizeof buf) {
            rc = CF_INTERNAL;
            goto fail;
        }
        cf_view_attrs link;
        cf_view_attrs_init(&link);
        CF_VIEW_TRY(cf_view_attr_cstr(&link, "class", "btn--faux flex-inline"));
        CF_VIEW_TRY(cf_view_attr_cstr(&link, "tabindex", "-1"));
        CF_VIEW_TRY(cf_view_attr_cstr(&link, "data-turbo-action", "replace"));
        CF_VIEW_TRY(cf_view_attr(
            &link, "href", (cf_span){(const unsigned char *)buf, (size_t)n}));
        CF_VIEW_TRY(cf_view_open_start(out, "a", &link));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n<label for=\"room_type\" class=\"switch\">"
                                 "\n<input type=\"checkbox\" id=\"room_type\" "
                                 "class=\"switch__input\""));
    if (kind_open) CF_VIEW_TRY(cf_view_str(out, " checked=\"checked\""));
    CF_VIEW_TRY(cf_view_str(out, " />\n"
                                 "<span class=\"switch__btn round\"></span>\n"
                                 "<span class=\"for-screen-reader\">"));
    CF_VIEW_TRY(cf_view_str(out, kind_open
                                       ? "Give only some access to this room"
                                       : "Give everyone access to this room"));
    CF_VIEW_TRY(cf_view_str(out, "</span>\n</label>\n</a>"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* The Everyone row (avatar, name, separator, the administer-only switch). */
static cf_err everyone_row(const cf_view_ctx *ctx, bool can_administer,
                           bool kind_open, bool is_new, int64_t room_id,
                           cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder src = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = cf_view_ctx_asset(ctx, lit("everyone.svg"), &src);
    if (rc != CF_OK) {
        cf_builder_dispose(&src);
        return cf_view_fail(&guard, rc);
    }
    CF_VIEW_TRY(cf_view_str(out, "<li class=\"flex align-center gap "
                                 "margin-none\">\n"
                                 "<figure class=\"avatar flex-item-no-shrink\" "
                                 "style=\"--avatar-border-radius: 0; "
                                 "--avatar-size: 4ch;\">\n"));
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "class", "colorize--black"));
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "style",
                                      "background-color: transparent"));
        CF_VIEW_TRY(cf_view_attr(&img, "src", cf_view_span_of(&src)));
        CF_VIEW_TRY(cf_view_legacy_tag(out, "img", &img));
    }
    cf_builder_dispose(&src);
    CF_VIEW_TRY(cf_view_str(out, "\n<span class=\"for-screen-reader\">Everyone"
                                 "</span>\n</figure>\n"
                                 "<div class=\"min-width\">\n"
                                 "<div class=\"overflow-ellipsis fill-shade\">"
                                 "<strong>Everyone</strong></div>\n</div>\n"
                                 "<hr class=\"separator\" "
                                 "aria-hidden=\"true\">\n"));
    if (can_administer) {
        CF_VIEW_TRY(everyone_switch(kind_open, is_new, room_id, out));
        CF_VIEW_TRY(cf_view_str(out, "\n"));
    }
    CF_VIEW_TRY(cf_view_str(out, "</li>\n"));
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&src);
    return cf_view_fail(&guard, rc);
}

/* The save button closing an administer form. */
static cf_err save_button(const cf_view_ctx *ctx, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    {
        cf_view_attrs button;
        cf_view_attrs_init(&button);
        CF_VIEW_TRY(cf_view_attr_cstr(&button, "name", "button"));
        CF_VIEW_TRY(cf_view_attr_cstr(&button, "type", "submit"));
        CF_VIEW_TRY(cf_view_attr_cstr(&button, "class",
                                      "btn btn--reversed txt-large center"));
        CF_VIEW_TRY(cf_view_open_start(out, "button", &button));
    }
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_i64(&img, "size", 20));
        CF_VIEW_TRY(cf_view_image_tag(ctx, lit("check.svg"), &img, out));
    }
    CF_VIEW_TRY(cf_view_str(out, "<span class=\"for-screen-reader\">Save"
                                 "</span>"));
    CF_VIEW_TRY(cf_view_close_tag(out, "button"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* The full `room_form` filter body around already-rendered fields. */
static cf_err room_form_wrap(const cf_view_ctx *ctx, bool is_new,
                             int64_t room_id, bool kind_open,
                             bool can_administer, bool has_name, cf_span name,
                             cf_span display_name, cf_span fields,
                             cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(room_form_open(is_new, room_id, kind_open, out));
    CF_VIEW_TRY(room_name_row(ctx, can_administer, has_name, name,
                              display_name, out));
    CF_VIEW_TRY(cf_view_str(out, "\n<section class=\"room-access margin-block "
                                 "pad-inline fill-shade border-radius\">\n"));
    CF_VIEW_TRY(cf_view_raw(out, fields));
    CF_VIEW_TRY(cf_view_str(out, "\n</section>\n"));
    if (can_administer) CF_VIEW_TRY(save_button(ctx, out));
    CF_VIEW_TRY(cf_view_str(out, "\n</form>"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* The `_new`/`_edit` layout section around the form, plus the delete-room
 * section for an administer edit. */
static cf_err room_page_section(const cf_view_ctx *ctx, bool is_new,
                                int64_t room_id, bool can_administer,
                                cf_span display_name, cf_span form,
                                cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    if (is_new) {
        CF_VIEW_TRY(cf_view_str(out, "<section class=\"panel "
                                     "txt-align-center\" "
                                     "style=\"view-transition-name: new-room\">"
                                     "\n"));
    } else {
        char buf[128];
        int n = snprintf(buf, sizeof buf,
                         "<section class=\"panel txt-align-center\" "
                         "style=\"view-transition-name: edit-room-%lld\">\n",
                         (long long)room_id);
        if (n < 0 || (size_t)n >= sizeof buf) {
            rc = CF_INTERNAL;
            goto fail;
        }
        CF_VIEW_TRY(cf_view_raw(
            out, (cf_span){(const unsigned char *)buf, (size_t)n}));
    }
    CF_VIEW_TRY(cf_view_raw(out, form));
    CF_VIEW_TRY(cf_view_str(out, "\n</section>\n"));
    if (!is_new && can_administer) {
        CF_VIEW_TRY(cf_view_str(out, "<section class=\"panel "
                                     "txt-align-center\">\n"));
        CF_VIEW_TRY(button_to_delete_room(ctx, room_id, display_name, out));
        CF_VIEW_TRY(cf_view_str(out, "\n</section>"));
    }
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

static cf_err room_page_title(bool is_new, bool has_name, cf_span name,
                              cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    if (is_new) {
        CF_VIEW_TRY(cf_view_str(out, "New chat room"));
    } else {
        CF_VIEW_TRY(cf_view_str(out, "Edit settings for "));
        if (has_name) CF_VIEW_TRY(cf_view_raw(out, name));
    }
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* The application-layout page around nav + section content. */
static cf_err room_form_page(const cf_view_ctx *ctx, cf_span title,
                             bool has_last_room_id, int64_t last_room_id,
                             cf_span content, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder nav = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = rooms_nav(ctx, has_last_room_id, last_room_id, &nav);
    if (rc != CF_OK) {
        cf_builder_dispose(&nav);
        return cf_view_fail(&guard, rc);
    }
    rc = cf_view_layout_page(ctx, title, true,
                             (cf_span){NULL, 0}, false, (cf_span){NULL, 0},
                             content, cf_view_span_of(&nav), (cf_span){NULL, 0},
                             (cf_span){NULL, 0}, out);
    cf_builder_dispose(&nav);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

/* ---- rooms/opens/new + edit ----------------------------------------------- */

/* rooms/opens/_user: the row shell plus the administer check.  Open rooms
 * never carry `user_ids[]` controls — membership is Everyone. */
static cf_err opens_user_row(const cf_view_ctx *ctx, const cf_view_user *user,
                             bool can_administer, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(user_row_open(ctx, user, out));
    if (can_administer) {
        cf_builder src = {0};
        rc = cf_view_ctx_asset(ctx, lit("check.svg"), &src);
        if (rc != CF_OK) {
            cf_builder_dispose(&src);
            goto fail;
        }
        {
            cf_view_attrs img;
            cf_view_attrs_init(&img);
            CF_VIEW_TRY(cf_view_attr_cstr(&img, "class",
                                          "colorize--black "
                                          "flex-item-no-shrink"));
            CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
            CF_VIEW_TRY(cf_view_attr(&img, "src", cf_view_span_of(&src)));
            CF_VIEW_TRY(cf_view_attr_cstr(&img, "width", "20"));
            CF_VIEW_TRY(cf_view_attr_cstr(&img, "height", "20"));
            CF_VIEW_TRY(cf_view_legacy_tag(out, "img", &img));
        }
        cf_builder_dispose(&src);
        CF_VIEW_TRY(cf_view_str(out, "\n"));
    }
    CF_VIEW_TRY(user_row_close(out));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* rooms/opens/_form inside the room_form filter. */
static cf_err opens_fields(const cf_view_ctx *ctx,
                           const cf_view_rooms_open_form_model *model,
                           cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(filter_menu_open(out));
    CF_VIEW_TRY(cf_view_str(out, "\n"));
    CF_VIEW_TRY(everyone_row(ctx, model->can_administer, true, model->is_new,
                             model->room_id, out));
    CF_VIEW_TRY(cf_view_str(out, "<hr class=\"separator full-width\" "
                                 "style=\"--border-style: solid\">\n"));
    if (model->user_count > 20) CF_VIEW_TRY(filter_search_tag(out));
    CF_VIEW_TRY(cf_view_str(out, "\n<div data-filter-target=\"list\" contents>"
                                 "\n"));
    for (size_t i = 0; i < model->user_count; i++) {
        CF_VIEW_TRY(opens_user_row(ctx, &model->users[i],
                                   model->can_administer, out));
    }
    CF_VIEW_TRY(cf_view_str(out, "</div>\n"));
    CF_VIEW_TRY(cf_view_close_tag(out, "menu"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

static cf_err opens_split(const cf_view_ctx *ctx,
                          const cf_view_rooms_open_form_model *model,
                          cf_builder *content_out, cf_builder *title_out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder fields = {0}, form = {0};
    cf_span name = {NULL, 0}, display = {NULL, 0};
    rc = cf_view_begin(&guard, content_out);
    if (rc != CF_OK) return rc;
    if (model->users == NULL && model->user_count != 0) {
        return cf_view_fail(&guard, CF_INVALID);
    }
    if (model->has_name) {
        name = (cf_span){(const unsigned char *)model->name.ptr,
                         model->name.len};
        display = name;
    }
    rc = opens_fields(ctx, model, &fields);
    if (rc != CF_OK) goto fail;
    rc = room_form_wrap(ctx, model->is_new, model->room_id, true,
                        model->can_administer, model->has_name, name, display,
                        cf_view_span_of(&fields), &form);
    cf_builder_dispose(&fields);
    if (rc != CF_OK) goto fail;
    rc = room_page_section(ctx, model->is_new, model->room_id,
                           model->can_administer, display,
                           cf_view_span_of(&form), content_out);
    cf_builder_dispose(&form);
    if (rc != CF_OK) goto fail;
    rc = room_page_title(model->is_new, model->has_name, name, title_out);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&fields);
    cf_builder_dispose(&form);
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_rooms_open_form(const cf_view_ctx *ctx,
                               const cf_view_rooms_open_form_model *model,
                               cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder content = {0}, title = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = opens_split(ctx, model, &content, &title);
    if (rc != CF_OK) {
        cf_builder_dispose(&content);
        cf_builder_dispose(&title);
        return cf_view_fail(&guard, rc);
    }
    rc = room_form_page(ctx, cf_view_span_of(&title),
                        model->has_last_room_id, model->last_room_id,
                        cf_view_span_of(&content), out);
    cf_builder_dispose(&content);
    cf_builder_dispose(&title);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_rooms_open_form_frame(const cf_view_ctx *ctx,
                                     const cf_view_rooms_open_form_model *model,
                                     cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder content = {0}, title = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = opens_split(ctx, model, &content, &title);
    cf_builder_dispose(&title);
    if (rc != CF_OK) {
        cf_builder_dispose(&content);
        return cf_view_fail(&guard, rc);
    }
    rc = cf_view_layout_frame(ctx, (cf_span){NULL, 0},
                              cf_view_span_of(&content), out);
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

/* ---- rooms/closeds/new + edit --------------------------------------------- */

/* rooms/closeds/_user: the row shell plus the administer grant control.  A
 * new room pins its creator with a hidden input; every other row (and every
 * row of a persisted room) is a `user_ids[]` checkbox. */
static cf_err closeds_user_row(const cf_view_ctx *ctx, const cf_view_user *user,
                               bool can_administer, bool selected, bool pinned,
                               cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(user_row_open(ctx, user, out));
    if (can_administer) {
        if (pinned) {
            cf_view_attrs hidden;
            cf_view_attrs_init(&hidden);
            CF_VIEW_TRY(cf_view_attr_cstr(&hidden, "type", "hidden"));
            CF_VIEW_TRY(cf_view_attr_cstr(&hidden, "name", "user_ids[]"));
            {
                char buf[24];
                int n = snprintf(buf, sizeof buf, "%lld",
                                 (long long)user->id);
                if (n < 0 || (size_t)n >= sizeof buf) {
                    rc = CF_INTERNAL;
                    goto fail;
                }
                CF_VIEW_TRY(cf_view_attr(
                    &hidden, "value",
                    (cf_span){(const unsigned char *)buf, (size_t)n}));
                CF_VIEW_TRY(cf_view_legacy_tag(out, "input", &hidden));
            }
            CF_VIEW_TRY(cf_view_str(out, "\n"));
            {
                cf_builder src = {0};
                rc = cf_view_ctx_asset(ctx, lit("check.svg"), &src);
                if (rc != CF_OK) {
                    cf_builder_dispose(&src);
                    goto fail;
                }
                cf_view_attrs img;
                cf_view_attrs_init(&img);
                CF_VIEW_TRY(cf_view_attr_cstr(&img, "class",
                                              "colorize--black "
                                              "flex-item-no-shrink"));
                CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
                CF_VIEW_TRY(cf_view_attr(&img, "src", cf_view_span_of(&src)));
                CF_VIEW_TRY(cf_view_attr_cstr(&img, "width", "20"));
                CF_VIEW_TRY(cf_view_attr_cstr(&img, "height", "20"));
                CF_VIEW_TRY(cf_view_legacy_tag(out, "img", &img));
                cf_builder_dispose(&src);
                CF_VIEW_TRY(cf_view_str(out, "\n"));
            }
        } else {
            CF_VIEW_TRY(cf_view_str(out, "\n<label class=\"switch "
                                         "flex-item-no-shrink\">\n"
                                         "<input type=\"checkbox\" "
                                         "name=\"user_ids[]\" value=\""));
            {
                char buf[24];
                int n = snprintf(buf, sizeof buf, "%lld",
                                 (long long)user->id);
                if (n < 0 || (size_t)n >= sizeof buf) {
                    rc = CF_INTERNAL;
                    goto fail;
                }
                CF_VIEW_TRY(cf_view_raw(
                    out, (cf_span){(const unsigned char *)buf, (size_t)n}));
            }
            CF_VIEW_TRY(cf_view_str(out, "\" class=\"switch__input\""));
            if (selected) CF_VIEW_TRY(cf_view_str(out, " checked=\"checked\""));
            CF_VIEW_TRY(cf_view_str(out, " />\n"
                                         "<span class=\"switch__btn round\">"
                                         "</span>\n"
                                         "<span class=\"for-screen-reader\">"
                                         "Give "));
            CF_VIEW_TRY(cf_view_text(
                out, (cf_span){(const unsigned char *)user->name.ptr,
                               user->name.len}));
            CF_VIEW_TRY(cf_view_str(out, " access to this room</span>\n"
                                         "</label>\n"));
        }
    }
    CF_VIEW_TRY(user_row_close(out));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* rooms/closeds/_form inside the room_form filter. */
static cf_err closeds_fields(const cf_view_ctx *ctx,
                             const cf_view_rooms_closed_form_model *model,
                             cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(filter_menu_open(out));
    CF_VIEW_TRY(cf_view_str(out, "\n"));
    if (model->can_administer) {
        CF_VIEW_TRY(everyone_row(ctx, true, false, model->is_new,
                                 model->room_id, out));
        CF_VIEW_TRY(cf_view_str(out, "<hr class=\"separator full-width\" "
                                     "style=\"--border-style: solid\">\n"));
    }
    if (model->selected_count + model->unselected_count > 20) {
        CF_VIEW_TRY(filter_search_tag(out));
        CF_VIEW_TRY(cf_view_str(out, "\n"));
    }
    CF_VIEW_TRY(cf_view_str(out, "<div data-filter-target=\"list\" contents>"
                                 "\n"));
    for (size_t i = 0; i < model->selected_count; i++) {
        CF_VIEW_TRY(closeds_user_row(ctx, &model->selected[i],
                                     model->can_administer, true, false, out));
    }
    if (model->selected_count != 0 && model->unselected_count != 0) {
        CF_VIEW_TRY(cf_view_str(out, "<hr class=\"separator full-width\" "
                                     "style=\"--border-style: solid\">\n"));
    }
    for (size_t i = 0; i < model->unselected_count; i++) {
        bool pinned = model->is_new &&
                      model->unselected[i].id == model->current_user_id;
        CF_VIEW_TRY(closeds_user_row(ctx, &model->unselected[i],
                                     model->can_administer, false, pinned,
                                     out));
    }
    CF_VIEW_TRY(cf_view_str(out, "</div>\n"));
    CF_VIEW_TRY(cf_view_close_tag(out, "menu"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* ---- rooms/directs/new ---------------------------------------------------- */

static cf_err closeds_split(const cf_view_ctx *ctx,
                            const cf_view_rooms_closed_form_model *model,
                            cf_builder *content_out, cf_builder *title_out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder fields = {0}, form = {0}, section = {0}, title = {0};
    cf_span name = {NULL, 0}, display = {NULL, 0};
    rc = cf_view_begin(&guard, content_out);
    if (rc != CF_OK) return rc;
    if ((model->selected == NULL && model->selected_count != 0) ||
        (model->unselected == NULL && model->unselected_count != 0)) {
        return cf_view_fail(&guard, CF_INVALID);
    }
    if (model->has_name) {
        name = (cf_span){(const unsigned char *)model->name.ptr,
                         model->name.len};
        display = name;
    }
    rc = closeds_fields(ctx, model, &fields);
    if (rc != CF_OK) goto fail;
    rc = room_form_wrap(ctx, model->is_new, model->room_id, false,
                        model->can_administer, model->has_name, name, display,
                        cf_view_span_of(&fields), &form);
    cf_builder_dispose(&fields);
    if (rc != CF_OK) goto fail;
    rc = room_page_section(ctx, model->is_new, model->room_id,
                           model->can_administer, display,
                           cf_view_span_of(&form), &section);
    cf_builder_dispose(&form);
    if (rc != CF_OK) goto fail;
    rc = room_page_title(model->is_new, model->has_name, name, &title);
    if (rc != CF_OK) goto fail;
    rc = cf_view_raw(content_out, cf_view_span_of(&section));
    if (rc == CF_OK) rc = cf_view_raw(title_out, cf_view_span_of(&title));
    cf_builder_dispose(&section);
    cf_builder_dispose(&title);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&fields);
    cf_builder_dispose(&form);
    cf_builder_dispose(&section);
    cf_builder_dispose(&title);
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_rooms_closed_form(const cf_view_ctx *ctx,
                                 const cf_view_rooms_closed_form_model *model,
                                 cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder content = {0}, title = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = closeds_split(ctx, model, &content, &title);
    if (rc != CF_OK) {
        cf_builder_dispose(&content);
        cf_builder_dispose(&title);
        return cf_view_fail(&guard, rc);
    }
    rc = room_form_page(ctx, cf_view_span_of(&title),
                        model->has_last_room_id, model->last_room_id,
                        cf_view_span_of(&content), out);
    cf_builder_dispose(&content);
    cf_builder_dispose(&title);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_rooms_closed_form_frame(const cf_view_ctx *ctx,
                                       const cf_view_rooms_closed_form_model *model,
                                       cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder content = {0}, title = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = closeds_split(ctx, model, &content, &title);
    cf_builder_dispose(&title);
    if (rc != CF_OK) {
        cf_builder_dispose(&content);
        return cf_view_fail(&guard, rc);
    }
    rc = cf_view_layout_frame(ctx, (cf_span){NULL, 0},
                              cf_view_span_of(&content), out);
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

/* ---- rooms/directs/new ---------------------------------------------------- */

/* users/autocompletables/_template.html. */
static cf_err autocompletable_template(const cf_view_ctx *ctx,
                                      cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(out, "<template id=\"autocompletable-user\">\n"
                                 "<div class=\"autocomplete__pill max-width\" "
                                 "data-value=\"\" tabindex=\"0\">\n"
                                 "<img class=\"avatar flex-item-no-shrink\" "
                                 "data-content=\"avatar\" src=\"\" />\n"
                                 "<span class=\"autocomplete-field__selected-"
                                 "value-text overflow-ellipsis "
                                 "flex-item-grow\" data-content=\"label\">"
                                 "</span>\n"
                                 "<button type=\"button\" "
                                 "data-action=\"autocomplete#remove:prevent\" "
                                 "data-value=\"\" tabindex=\"-1\" "
                                 "class=\"btn btn--plain txt-small "
                                 "translucent flex-item-no-shrink\">\n"));
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "class", "colorize--black"));
        CF_VIEW_TRY(cf_view_image_tag(ctx, lit("remove-circle.svg"), &img,
                                      out));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n<span class=\"for-screen-reader\">Remove "
                                 "<span data-content=\"screenReaderLabel\">"
                                 "</span></span>\n"
                                 "</button>\n</div>\n</template>"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* rooms/directs/new.html. */
static cf_err directs_new_content(const cf_view_ctx *ctx, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder check_src = {0}, arrow_src = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = cf_view_ctx_asset(ctx, lit("arrow-left.svg"), &arrow_src);
    if (rc == CF_OK) rc = cf_view_ctx_asset(ctx, lit("check.svg"), &check_src);
    if (rc != CF_OK) {
        cf_builder_dispose(&check_src);
        cf_builder_dispose(&arrow_src);
        return cf_view_fail(&guard, rc);
    }
    {
        cf_view_attrs frame;
        cf_view_attrs_init(&frame);
        CF_VIEW_TRY(cf_view_attr_cstr(&frame, "id", "direct_rooms_control"));
        CF_VIEW_TRY(cf_view_attr_cstr(&frame, "target", "_top"));
        CF_VIEW_TRY(cf_view_open_start(out, "turbo-frame", &frame));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n<div class=\"directs directs--new flex "
                                 "flex-column gap\">\n"));
    {
        cf_view_attrs form;
        cf_view_attrs_init(&form);
        CF_VIEW_TRY(cf_view_attr_cstr(&form, "class", "flex gap "
                                                    "flex-item-grow"));
        CF_VIEW_TRY(cf_view_attr_cstr(&form, "data-controller", "form"));
        CF_VIEW_TRY(cf_view_attr_cstr(&form, "data-action",
                                      "keydown.esc->form#cancel"));
        CF_VIEW_TRY(cf_view_attr_cstr(&form, "action", "/rooms/directs"));
        CF_VIEW_TRY(cf_view_attr_cstr(&form, "accept-charset", "UTF-8"));
        CF_VIEW_TRY(cf_view_attr_cstr(&form, "method", "post"));
        CF_VIEW_TRY(cf_view_open_start(out, "form", &form));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n"));
    {
        cf_view_attrs cancel;
        cf_view_attrs_init(&cancel);
        CF_VIEW_TRY(cf_view_attr_cstr(&cancel, "class",
                                      "btn flex-item-no-shrink"));
        CF_VIEW_TRY(cf_view_attr_cstr(&cancel, "data-turbo-frame",
                                      "user_sidebar"));
        CF_VIEW_TRY(cf_view_attr_cstr(&cancel, "data-form-target", "cancel"));
        CF_VIEW_TRY(cf_view_attr_cstr(&cancel, "href", "/users/me/sidebar"));
        CF_VIEW_TRY(cf_view_open_start(out, "a", &cancel));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n"));
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr(&img, "src", cf_view_span_of(&arrow_src)));
        CF_VIEW_TRY(cf_view_legacy_tag(out, "img", &img));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n<span class=\"for-screen-reader\">Cancel "
                                 "changes</span>\n</a>\n"
                                 "<section class=\"autocomplete__container "
                                 "unpad input input--actor\">\n"
                                 "<div class=\"autocomplete__input input "
                                 "flex flex-wrap position-relative "
                                 "flex-item-grow\" "
                                 "data-controller=\"autocomplete\" "
                                 "data-autocomplete-url-value=\""
                                 "/autocompletable/users\">\n"
                                 "<select name=\"user_ids[]\" "
                                 "data-autocomplete-target=\"select\" "
                                 "data-template-id=\"autocompletable-user\" "
                                 "multiple=\"true\" hidden=\"\" "
                                 "required=\"\"></select>\n"));
    CF_VIEW_TRY(autocompletable_template(ctx, out));
    CF_VIEW_TRY(cf_view_str(out, "\n"));
    {
        cf_view_attrs input;
        cf_view_attrs_init(&input);
        CF_VIEW_TRY(cf_view_attr_cstr(&input, "autocomplete", "off"));
        CF_VIEW_TRY(cf_view_attr_cstr(&input, "autocorrect", "off"));
        CF_VIEW_TRY(cf_view_attr_cstr(&input, "data-1p-ignore", "true"));
        CF_VIEW_TRY(cf_view_attr_cstr(&input, "class",
                                      "autocomplete__input input flex "
                                      "flex-wrap position-relative"));
        CF_VIEW_TRY(cf_view_attr_cstr(&input, "data-autocomplete-target",
                                      "input"));
        CF_VIEW_TRY(cf_view_attr_cstr(
            &input, "data-action",
            "input->autocomplete#search keydown->autocomplete#didPressKey"));
        CF_VIEW_TRY(cf_view_attr_cstr(&input, "type", "text"));
        CF_VIEW_TRY(cf_view_attr_cstr(&input, "name",
                                      "rooms_direct[user_ids_input]"));
        CF_VIEW_TRY(cf_view_attr_cstr(&input, "id",
                                      "rooms_direct_user_ids_input"));
        CF_VIEW_TRY(cf_view_legacy_tag(out, "input", &input));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n</div>\n</section>\n"));
    {
        cf_view_attrs submit;
        cf_view_attrs_init(&submit);
        CF_VIEW_TRY(cf_view_attr_cstr(&submit, "name", "button"));
        CF_VIEW_TRY(cf_view_attr_cstr(&submit, "type", "submit"));
        CF_VIEW_TRY(cf_view_attr_cstr(&submit, "class",
                                      "btn btn--reversed flex-item-no-shrink"));
        CF_VIEW_TRY(cf_view_open_start(out, "button", &submit));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n"));
    {
        cf_view_attrs img;
        cf_view_attrs_init(&img);
        CF_VIEW_TRY(cf_view_attr_cstr(&img, "aria-hidden", "true"));
        CF_VIEW_TRY(cf_view_attr(&img, "src", cf_view_span_of(&check_src)));
        CF_VIEW_TRY(cf_view_legacy_tag(out, "img", &img));
    }
    CF_VIEW_TRY(cf_view_str(out, "\n<span class=\"for-screen-reader\">Start "
                                 "Ping</span>\n</button></form>\n"
                                 "<span class=\"txt-small translucent "
                                 "pad-inline-half center\">"
                                 "Type names to ping someone\xE2\x80\xA6"
                                 "</span>\n</div>\n"));
    CF_VIEW_TRY(cf_view_close_tag(out, "turbo-frame"));
    cf_builder_dispose(&check_src);
    cf_builder_dispose(&arrow_src);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&check_src);
    cf_builder_dispose(&arrow_src);
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_rooms_direct_new(const cf_view_ctx *ctx, cf_builder *out) {
    if (ctx == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder content = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = directs_new_content(ctx, &content);
    if (rc != CF_OK) {
        cf_builder_dispose(&content);
        return cf_view_fail(&guard, rc);
    }
    /* DirectsNew carries no page title, nav, footer or sidebar blocks. */
    rc = cf_view_layout_page(ctx, (cf_span){NULL, 0}, false,
                             (cf_span){NULL, 0}, false, (cf_span){NULL, 0},
                             cf_view_span_of(&content), (cf_span){NULL, 0},
                             (cf_span){NULL, 0}, (cf_span){NULL, 0}, out);
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_rooms_direct_new_frame(const cf_view_ctx *ctx,
                                      cf_builder *out) {
    if (ctx == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder content = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = directs_new_content(ctx, &content);
    if (rc != CF_OK) {
        cf_builder_dispose(&content);
        return cf_view_fail(&guard, rc);
    }
    rc = cf_view_layout_frame(ctx, (cf_span){NULL, 0},
                              cf_view_span_of(&content), out);
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

/* ---- rooms/directs/edit --------------------------------------------------- */

/* The member card (avatar link plus name). */
static cf_err direct_member_card(const cf_view_ctx *ctx,
                                const cf_view_user *user, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    CF_VIEW_TRY(cf_view_str(out, "<div class=\"member flex flex-column gap "
                                 "fill-shade pad border-radius\">\n"
                                 "<figure class=\"avatar center\" "
                                 "style=\"--avatar-border-radius: 10ch; "
                                 "--avatar-size: 10ch;\">\n"));
    CF_VIEW_TRY(user_avatar_link(ctx, user, out));
    CF_VIEW_TRY(cf_view_str(out, "\n</figure>\n<strong>"));
    CF_VIEW_TRY(cf_view_text(
        out, (cf_span){(const unsigned char *)user->name.ptr,
                       user->name.len}));
    CF_VIEW_TRY(cf_view_str(out, "</strong>\n</div>\n"));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

/* The delete-ping form (raw template form, not the button_to helper). */
static cf_err direct_delete_form(const cf_view_ctx *ctx, int64_t room_id,
                                 cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder action = {0}, src = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    {
        char buf[32];
        int n = snprintf(buf, sizeof buf, "/rooms/directs/%lld",
                         (long long)room_id);
        if (n < 0 || (size_t)n >= sizeof buf) {
            rc = CF_INTERNAL;
            goto fail;
        }
        rc = cf_view_ctx_url(
            ctx, (cf_span){(const unsigned char *)buf, (size_t)n}, &action);
        if (rc != CF_OK) goto fail;
    }
    rc = cf_view_ctx_asset(ctx, lit("trash.svg"), &src);
    if (rc != CF_OK) goto fail;
    CF_VIEW_TRY(cf_view_str(out, "<form class=\"button_to\" method=\"post\" "
                                 "action=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&action)));
    CF_VIEW_TRY(cf_view_str(out, "\">"));
    CF_VIEW_TRY(cf_view_method_tag(out, "delete"));
    CF_VIEW_TRY(cf_view_str(
        out,
        "<button class=\"btn btn--negative center\" aria-label=\"Delete Ping\" "
        "data-turbo-confirm=\"Are you sure you want to delete this ping and "
        "all messages in it? This can\xE2\x80\x99t be undone.\" "
        "type=\"submit\">\n"));
    CF_VIEW_TRY(cf_view_str(out, "<img aria-hidden=\"true\" src=\""));
    CF_VIEW_TRY(cf_view_html_attr(out, cf_view_span_of(&src)));
    CF_VIEW_TRY(cf_view_str(out, "\" />\n Ping \n</button></form>"));
    cf_builder_dispose(&action);
    cf_builder_dispose(&src);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&action);
    cf_builder_dispose(&src);
    return cf_view_fail(&guard, rc);
}

static cf_err directs_edit_split(const cf_view_ctx *ctx,
                                 const cf_view_rooms_direct_edit_model *model,
                                 cf_builder *content_out,
                                 cf_builder *title_out) {
    cf_err rc;
    cf_view_guard guard;
    rc = cf_view_begin(&guard, content_out);
    if (rc != CF_OK) return rc;
    if ((model->users == NULL && model->user_count != 0) ||
        model->display_name.ptr == NULL) {
        return cf_view_fail(&guard, CF_INVALID);
    }
    CF_VIEW_TRY(cf_view_str(content_out, "<div class=\"panel "
                                         "txt-align-center\">\n"
                                         "<section class=\"directs--edit "
                                         "margin-block-end\">\n"));
    for (size_t i = 0; i < model->user_count; i++) {
        CF_VIEW_TRY(direct_member_card(ctx, &model->users[i], content_out));
    }
    CF_VIEW_TRY(cf_view_str(content_out, "</section>\n"));
    CF_VIEW_TRY(direct_delete_form(ctx, model->room_id, content_out));
    CF_VIEW_TRY(cf_view_str(content_out, "</div>\n"));
    CF_VIEW_TRY(cf_view_str(title_out, "Edit settings for "));
    CF_VIEW_TRY(cf_view_raw(
        title_out, (cf_span){(const unsigned char *)model->display_name.ptr,
                             model->display_name.len}));
    return cf_view_finish(&guard);
fail:
    return cf_view_fail(&guard, rc);
}

cf_err cf_view_rooms_direct_edit(const cf_view_ctx *ctx,
                                 const cf_view_rooms_direct_edit_model *model,
                                 cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder content = {0}, title = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = directs_edit_split(ctx, model, &content, &title);
    if (rc != CF_OK) {
        cf_builder_dispose(&content);
        cf_builder_dispose(&title);
        return cf_view_fail(&guard, rc);
    }
    rc = room_form_page(ctx, cf_view_span_of(&title),
                        model->has_last_room_id, model->last_room_id,
                        cf_view_span_of(&content), out);
    cf_builder_dispose(&content);
    cf_builder_dispose(&title);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}

cf_err cf_view_rooms_direct_edit_frame(const cf_view_ctx *ctx,
                                       const cf_view_rooms_direct_edit_model *model,
                                       cf_builder *out) {
    if (ctx == NULL || model == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_builder content = {0}, title = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    rc = directs_edit_split(ctx, model, &content, &title);
    cf_builder_dispose(&title);
    if (rc != CF_OK) {
        cf_builder_dispose(&content);
        return cf_view_fail(&guard, rc);
    }
    rc = cf_view_layout_frame(ctx, (cf_span){NULL, 0},
                              cf_view_span_of(&content), out);
    cf_builder_dispose(&content);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
}
static cf_err button_to_delete_room(const cf_view_ctx *ctx, int64_t room_id,
                                    cf_span display_name, cf_builder *out) {
    cf_err rc;
    cf_view_guard guard;
    cf_builder url = {0}, content = {0};
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    {
        char buf[32];
        int n = snprintf(buf, sizeof buf, "/rooms/%lld", (long long)room_id);
        if (n < 0 || (size_t)n >= sizeof buf) {
            rc = CF_INTERNAL;
            goto fail;
        }
        rc = cf_view_ctx_url(
            ctx, (cf_span){(const unsigned char *)buf, (size_t)n}, &url);
        if (rc != CF_OK) goto fail;
    }
    {
        cf_builder src = {0};
        rc = cf_view_ctx_asset(ctx, lit("trash.svg"), &src);
        if (rc != CF_OK) {
            cf_builder_dispose(&src);
            goto fail;
        }
        rc = cf_view_str(
            &content,
            "<img aria-hidden=\"true\" src=\"");
        if (rc == CF_OK) rc = cf_view_html_attr(&content, cf_view_span_of(&src));
        cf_builder_dispose(&src);
        if (rc == CF_OK) {
            rc = cf_view_str(&content,
                             "\" width=\"20\" height=\"20\" />"
                             "<span class=\"overflow-ellipsis\">");
        }
        if (rc == CF_OK) rc = cf_view_text(&content, display_name);
        if (rc == CF_OK) rc = cf_view_str(&content, "</span>");
        if (rc != CF_OK) goto fail;
    }
    {
        cf_view_attrs button;
        cf_builder label = {0};
        cf_view_attrs_init(&button);
        CF_VIEW_TRY(cf_view_attr_cstr(&button, "class",
                                      "btn btn--negative max-width"));
        CF_VIEW_TRY(cf_view_str(&label, "Delete "));
        CF_VIEW_TRY(cf_view_raw(&label, display_name));
        /* aria-label carries the raw name (Rails does not escape the
         * option hash here beyond the attribute writer).  The attr borrows
         * the label bytes, so label must outlive the button render. */
        CF_VIEW_TRY(cf_view_attr(&button, "aria-label",
                                 cf_view_span_of(&label)));
        CF_VIEW_TRY(cf_view_attr_cstr(
            &button, "data-turbo-confirm",
            "Are you sure you want to delete this room and all messages in "
            "it? This can\xE2\x80\x99t be undone."));
        CF_VIEW_TRY(cf_view_button_to(out, cf_view_span_of(&url), &button,
                                      lit("delete"), (cf_span){NULL, 0},
                                      cf_view_span_of(&content)));
        cf_builder_dispose(&label);
    }
    cf_builder_dispose(&url);
    cf_builder_dispose(&content);
    return cf_view_finish(&guard);
fail:
    cf_builder_dispose(&url);
    cf_builder_dispose(&content);
    return cf_view_fail(&guard, rc);
}

/* ---- R2: the sidebar room partials behind the room broadcasts ------------- */

/* `room_param_key(room.room_type)` for the shared partial's dom id. */
static const char *shared_param_key(cf_room_type room_type) {
    switch (room_type) {
    case CF_ROOM_CLOSED:
        return "rooms_closed";
    case CF_ROOM_DIRECT:
        return "rooms_direct";
    case CF_ROOM_OPEN:
    default:
        return "rooms_open";
    }
}

static cf_err str_copy(cf_span span, cf_str *out) {
    char *copy;
    memset(out, 0, sizeof *out);
    if (span.len != 0 && span.ptr == NULL) return CF_INVALID;
    copy = malloc(span.len + 1);
    if (copy == NULL) return CF_NOMEM;
    if (span.len != 0) memcpy(copy, span.ptr, span.len);
    copy[span.len] = '\0';
    out->ptr = copy;
    out->len = span.len;
    return CF_OK;
}

/* `Partials::shared_room` behind render_shared_room
 * (users/sidebars/rooms/_shared with `room:`): the reference presenter's
 * SidebarRoom — id, the STI param key, the name ("" when absent) and
 * unread=false.  Pure mapping over the room row; no SQL.  `user` is unused
 * (the partial is detached, like the reference's render_detached_at). */
cf_err cf_view_rooms_shared_room_partial(void *user, const cf_room *room,
                                         cf_builder *out) {
    (void)user;
    if (room == NULL || out == NULL) return CF_INVALID;
    cf_err rc;
    cf_view_guard guard;
    cf_view_sidebar_room view;
    memset(&view, 0, sizeof view);
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;
    view.id = room->id;
    rc = str_copy(lit(shared_param_key(room->room_type)), &view.param_key);
    if (rc != CF_OK) goto fail;
    if (room->name.present) {
        rc = str_copy((cf_span){(const unsigned char *)room->name.value.ptr,
                                room->name.value.len},
                      &view.name);
    } else {
        rc = str_copy(lit(""), &view.name);
    }
    if (rc != CF_OK) goto fail;
    view.unread = false;
    rc = cf_view_sidebar_shared_partial(&view, out);
    cf_view_sidebar_room_dispose(&view);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
fail:
    cf_view_sidebar_room_dispose(&view);
    return cf_view_fail(&guard, rc);
}

/* `room.users.without(membership.user).presence || [ membership.user ]` as
 * sidebar users, through the broadcast views' reader. */
static cf_err direct_members(cf_ctx *ctx, const cf_membership *membership,
                             const cf_room *room,
                             cf_view_sidebar_user_vector *out) {
    cf_err rc;
    cf_user_vector users = {0};
    memset(out, 0, sizeof *out);
    rc = cf_room_users(ctx->reader, room, &users);
    if (rc != CF_OK) return rc;
    for (size_t i = 0; i < users.len && rc == CF_OK; i++) {
        const cf_user *candidate = &users.items[i];
        cf_view_user mapped = {0};
        cf_view_sidebar_user item;
        if (candidate->id == membership->user_id) continue;
        rc = cf_presenter_user_view(ctx, candidate, &mapped);
        if (rc != CF_OK) break;
        memset(&item, 0, sizeof item);
        item.id = mapped.id;
        rc = str_copy((cf_span){(const unsigned char *)mapped.name.ptr,
                                mapped.name.len},
                      &item.name);
        if (rc == CF_OK) {
            rc = str_copy(
                (cf_span){(const unsigned char *)mapped.avatar_url.ptr,
                          mapped.avatar_url.len},
                &item.avatar_path);
        }
        cf_view_user_dispose(&mapped);
        if (rc != CF_OK) {
            cf_view_sidebar_user_dispose(&item);
            break;
        }
        if (out->len == out->cap) {
            size_t cap = out->cap != 0 ? out->cap * 2 : 4;
            cf_view_sidebar_user *grown;
            if (cap < out->cap ||
                cap > SIZE_MAX / sizeof *out->items) {
                cf_view_sidebar_user_dispose(&item);
                rc = CF_LIMIT;
                break;
            }
            grown = realloc(out->items, cap * sizeof *out->items);
            if (grown == NULL) {
                cf_view_sidebar_user_dispose(&item);
                rc = CF_NOMEM;
                break;
            }
            out->items = grown;
            out->cap = cap;
        }
        out->items[out->len++] = item;
        memset(&item, 0, sizeof item);
    }
    cf_user_vector_dispose(&users);
    if (rc != CF_OK) {
        cf_view_sidebar_user_vector_dispose(out);
        return rc;
    }
    if (out->len != 0) return CF_OK;

    /* The membership's own user, when the room has no other member. */
    {
        cf_user own = {0};
        bool found = false;
        cf_view_user mapped = {0};
        cf_view_sidebar_user item;
        rc = cf_user_find_by_id(ctx->reader, membership->user_id, &found,
                                &own);
        if (rc != CF_OK) return rc;
        if (!found) {
            cf_user_dispose(&own);
            return CF_NOT_FOUND;
        }
        rc = cf_presenter_user_view(ctx, &own, &mapped);
        cf_user_dispose(&own);
        if (rc != CF_OK) return rc;
        memset(&item, 0, sizeof item);
        item.id = mapped.id;
        rc = str_copy((cf_span){(const unsigned char *)mapped.name.ptr,
                                mapped.name.len},
                      &item.name);
        if (rc == CF_OK) {
            rc = str_copy(
                (cf_span){(const unsigned char *)mapped.avatar_url.ptr,
                          mapped.avatar_url.len},
                &item.avatar_path);
        }
        cf_view_user_dispose(&mapped);
        if (rc != CF_OK) {
            cf_view_sidebar_user_dispose(&item);
            return rc;
        }
        if (out->len == out->cap) {
            cf_view_sidebar_user *grown =
                realloc(out->items, sizeof *out->items);
            if (grown == NULL) {
                cf_view_sidebar_user_dispose(&item);
                return CF_NOMEM;
            }
            out->items = grown;
            out->cap = 1;
        }
        out->items[out->len++] = item;
        memset(&item, 0, sizeof item);
    }
    return CF_OK;
}

/* `Partials::direct_room` (users/sidebars/rooms/_direct with `membership:`):
 * the reference presenter.sidebar_direct assembled through the broadcast
 * views' reader, rendered through cf_view_sidebar_direct_partial.  `user`
 * is the cable module's cf_broadcast_views (ctx drives the reads and the
 * avatar signing, view the render). */
cf_err cf_view_rooms_direct_room_partial(void *user,
                                         const cf_membership *membership,
                                         cf_builder *out) {
    if (user == NULL || membership == NULL || out == NULL) return CF_INVALID;
    const cf_broadcast_views *views = user;
    if (views->ctx == NULL || views->ctx->reader == NULL ||
        views->view == NULL) {
        return CF_INVALID;
    }
    cf_err rc;
    cf_view_guard guard;
    cf_room room = {0};
    cf_view_sidebar_direct direct;
    bool have_room = false;
    memset(&direct, 0, sizeof direct);
    rc = cf_view_begin(&guard, out);
    if (rc != CF_OK) return rc;

    {
        bool found = false;
        rc = cf_room_find_by_id(views->ctx->reader, membership->room_id,
                                &found, &room);
        if (rc != CF_OK) goto fail;
        if (!found) {
            rc = CF_NOT_FOUND;
            goto fail;
        }
        have_room = true;
    }
    direct.room_id = room.id;
    direct.unread = cf_membership_unread(membership);
    {
        char number[32];
        int n = snprintf(number, sizeof number, "%lld",
                         (long long)cf_view_epoch_ms(room.updated_at));
        if (n < 0 || (size_t)n >= sizeof number) {
            rc = CF_INTERNAL;
            goto fail;
        }
        rc = str_copy((cf_span){(const unsigned char *)number, (size_t)n},
                      &direct.updated_at_epoch);
        if (rc != CF_OK) goto fail;
    }
    rc = direct_members(views->ctx, membership, &room, &direct.members);
    if (rc != CF_OK) goto fail;
    if (direct.members.len == 0) {
        rc = CF_NOT_FOUND;
        goto fail;
    }
    rc = cf_view_sidebar_direct_partial(views->view, &direct, out);
    cf_room_dispose(&room);
    cf_view_sidebar_direct_dispose(&direct);
    if (rc != CF_OK) return cf_view_fail(&guard, rc);
    return cf_view_finish(&guard);
fail:
    if (have_room) cf_room_dispose(&room);
    cf_view_sidebar_direct_dispose(&direct);
    return cf_view_fail(&guard, rc);
}
