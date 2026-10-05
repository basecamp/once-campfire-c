/* src/views.h — A02 view contract: view models, renderers and presenters.
 *
 * Reference: tmp/rust-ref/crates/views (src/lib.rs ViewContext, layouts.rs,
 * sessions.rs, first_runs.rs, welcome.rs and templates/) plus the app-side
 * presenters in tmp/rust-ref/crates/campfire/src/controllers/presenters
 * ({view_context.rs, accounts.rs, page.rs}).  03-application.md "A02: views
 * and assets contract" is binding.
 *
 * Shape (the conventions frozen for this phase):
 *  - View models map the finite reference view structs: same fields, explicit
 *    presence flags for optionals, owned vectors for lists (none in the
 *    foundation families).
 *  - `cf_presenter_*` functions load rows (models inside one read
 *    transaction) and assemble the models; rendering performs no SQL,
 *    mutation, filesystem or network work.
 *  - Every renderer writes into a caller cf_builder with the 8 MiB output
 *    cap.  On any error the builder is left at its entry length (allocation
 *    failure destroys the partial page instead of emitting half of one).
 *  - html_escape is R01's cf_html_text/cf_html_attr; trusted markup built by
 *    these renderers is appended through the helpers in src/views/internal.h.
 *
 * The layout families here are the A02 foundation: application/frame layouts,
 * sessions, first runs and welcome.  Rooms and messages extend this header
 * through the integrator.
 *
 * Golden comparison: tests/views/support/golden.{h,c} ports the pinned Rust
 * runner's DOM comparison (crates/views/tests/support/dom.rs): the only masks
 * are the CSRF forgery tokens Rails renders and this app does not
 * (`is_forgery_token`), and everything else must match the token stream.
 */
#ifndef CF_VIEWS_H
#define CF_VIEWS_H

#include "auth/platform.h" /* cf_platform (the cf_view_platform definition) */
#include "cf.h"
#include "context.h"
#include "models/types.h"

/* ---------------------------------------------------------------- assets */

/* Load the digested-asset map and the build-time importmap tags from the
 * static root (the pinned fixtures' manifest:
 * <root>/public/assets/.manifest.json and <root>/importmap-tags.html).
 * Call once at startup, before serving, like cf_richtext_configure.
 * `static_root` NULL or empty selects cf_static_root() (routes.h).  A second
 * call replaces the loaded data.  Renderers read only memory afterwards: a
 * render never touches the filesystem. */
cf_err cf_views_assets_configure(const char *static_root);

/* Drop the loaded asset data (tests; returns renderers to the unconfigured
 * state where they fail with CF_INVALID instead of guessing). */
void cf_views_assets_reset(void);

/* `asset_path(source)` (crates/assets/src/helpers.rs): URLs, absolute paths,
 * `data:`/`cid:` pass through; a logical path resolves through the loaded
 * manifest to "/assets/<digested>" with any `?...`/`#...` tail kept.  A
 * logical path missing from the manifest is CF_NOT_FOUND (the reference
 * raises Propshaft::MissingAssetError). */
cf_err cf_views_asset_path(cf_span logical, cf_builder *out);

/* `stylesheet_link_tag :all, "data-turbo-track": "reload"` exactly as
 * crates/assets/src/tags.rs renders it (sorted logical CSS paths, ` />`
 * tags joined by newlines). */
cf_err cf_views_stylesheet_tags(cf_builder *out);

/* `javascript_importmap_tags`: the pinned build's tag block. */
cf_err cf_views_importmap_tags(cf_builder *out);

/* The `Link` preload header value `stylesheet_link_tag` adds
 * (append_preload_links semantics, 1,000-byte cap). */
cf_err cf_views_preload_links(cf_builder *out);

/* The loaded tag blocks, borrowed until the next configure/reset (empty
 * spans when unconfigured).  cf_view_ctx_init copies them into the context
 * the way the reference ViewContext carries importmap_tags/stylesheet_tags;
 * tests may override the spans for a fixture-faithful render. */
cf_span cf_views_assets_importmap_tags(void);
cf_span cf_views_assets_stylesheet_tags(void);

/* The default asset resolver over the loaded manifest; `user` is ignored
 * (it lets tests install their own resolver of the same shape). */
cf_err cf_views_assets_default_path(void *user, cf_span logical,
                                    cf_builder *out);

/* ------------------------------------------------------------------ models */

/* `ApplicationPlatform` facts from the user agent (the foundation views read
 * apple_messages; the rooms bell's notification help reads browser and
 * operating_system).  A01's UA parser fills them (cf_platform_parse); a zeroed
 * value is "no platform facts" (empty strings).
 *
 * The type is cf_platform (src/auth/platform.h): the parse result carries the
 * parser's browser_version/bot/blocked fields and its two small synthesized-
 * text buffers alongside the view facts, so `cf_ctx_platform(ctx)` can be
 * passed to cf_presenter_layout_load without adaptation.  View code only reads
 * the fact fields; see platform.h for the span lifetimes.
 *
 * The action path always fills this from the request's User-Agent value
 * (cf_ctx_platform; the reference `platform` helper's parse, "" when the
 * header is absent), so no rendered page observes zeroed facts. */
typedef cf_platform cf_view_platform;

/* `Current.user` as the layout's meta tags and body classes read it
 * (crates/views/src/lib.rs CurrentUser). */
typedef struct {
    bool has_user;
    int64_t id;
    cf_str name;       /* owned */
    bool administrator;
    bool bot;
    cf_str avatar_url; /* owned; fresh_user_avatar_path (unused by the foundation templates) */
} cf_view_current_user;

/* `Current.account` (crates/views/src/lib.rs AccountSummary). */
typedef struct {
    cf_str name;     /* owned; empty before the account exists */
    cf_str logo_url; /* owned; fresh_account_logo_path */
    bool has_logo;
} cf_view_account;

/* `ViewContext` for one request.  Every span borrows the layout model, the
 * context or the loaded config; the struct owns nothing and needs no
 * disposal.  cf_view_ctx_init fills it from a presenter-loaded
 * cf_view_layout_model. */
typedef struct {
    cf_view_current_user current_user;
    cf_view_account account;
    bool has_flash_notice;
    cf_span flash_notice;
    bool has_flash_alert;
    cf_span flash_alert;
    cf_view_platform platform;
    bool has_vapid_public_key;
    cf_span vapid_public_key;
    bool has_custom_styles;
    cf_span custom_styles;
    cf_span app_version; /* "0" when no build version is wired */
    cf_span base_url;    /* request base URL, no trailing slash */
    /* Asset resolution and tags (the reference ViewContext's asset_path,
     * importmap_tags and stylesheet_tags).  cf_view_ctx_init fills these
     * from the configured asset module; a test may substitute its own. */
    cf_span importmap_tags;
    cf_span stylesheet_tags;
    cf_err (*asset_path)(void *user, cf_span logical, cf_builder *out);
    void *asset_path_user;
} cf_view_ctx;

/* `Layout::load`: the per-request data the layout needs.  Owns its strings;
 * dispose with cf_view_layout_model_dispose. */
typedef struct {
    cf_view_current_user current_user;
    cf_view_account account;
    bool has_custom_styles;
    cf_str custom_styles; /* owned when present */
    cf_view_platform platform;
    bool has_last_room_visited;
    int64_t last_room_visited_id;
    bool has_vapid_public_key;
    cf_str vapid_public_key; /* owned when present */
    cf_str app_version;      /* owned */
} cf_view_layout_model;

void cf_view_layout_model_dispose(cf_view_layout_model *layout);

/* One read transaction over ctx->reader: Current.user, Account.first with its
 * logo attachment, custom styles, the last-room selection
 * (TrackedRoomVisit/last_room semantics) and, from `platform`, the platform
 * facts A01's UA parser derived.  `platform` may be NULL (zeroed facts).
 * `ctx->app` supplies the config (SECRET_KEY_BASE for the avatar URL, the
 * VAPID public key and the public origin) and `ctx->identity` the
 * authenticated user. */
cf_err cf_presenter_layout_load(cf_ctx *ctx, const cf_view_platform *platform,
                                cf_view_layout_model *out);

/* Fill the render context from a loaded layout plus the request state
 * (flash, base URL).  Borrows both; no allocation. */
void cf_view_ctx_init(cf_view_ctx *out, const cf_ctx *ctx,
                      const cf_view_layout_model *layout);

/* `User.administrator.first`, for accounts/_help_contact. */
typedef struct {
    cf_str name;          /* owned */
    cf_str email_address; /* owned; empty when the column is NULL */
} cf_view_help_contact;

void cf_view_help_contact_dispose(cf_view_help_contact *contact);

/* One read transaction; found=false when no administrator exists. */
cf_err cf_presenter_help_contact(cf_db *db, bool *found,
                                 cf_view_help_contact *out);

/* `User.none?` (SessionsController#ensure_user_exists). */
cf_err cf_presenter_no_users(cf_db *db, bool *out);

/* ------------------------------------------------------------ page models */

/* sessions/new.html.erb. */
typedef struct {
    bool has_email_address;
    cf_str email_address; /* borrowed */
    bool has_help_contact;
    const cf_view_help_contact *help_contact; /* borrowed */
} cf_view_session_new_model;

/* sessions/transfers/show.html.erb; action is `session_transfer_path(id)`. */
typedef struct {
    cf_span action; /* borrowed */
} cf_view_session_transfer_model;

/* welcome/show.html.erb. */
typedef struct {
    cf_span current_user_name; /* borrowed */
} cf_view_welcome_model;

/* ------------------------------------------------ rooms and messages models */

/* `messages::UserView`: what `avatar_tag` and the author heading read. */
typedef struct {
    int64_t id;
    cf_str name;       /* owned */
    cf_str title;      /* owned: User#title, name and bio joined by " – " */
    cf_str avatar_url; /* owned: fresh_user_avatar_path */
} cf_view_user;

void cf_view_user_dispose(cf_view_user *user);

/* `rooms::RoomView`.  `display_name` is `room_display_name(room, for_user:)`;
 * `name` is the raw column (absent for direct rooms). */
typedef struct {
    int64_t id;
    cf_room_type kind;
    bool has_name;
    cf_str name;         /* owned when has_name */
    cf_str display_name; /* owned */
} cf_view_room;

void cf_view_room_dispose(cf_view_room *room);

/* Ruby's `Integer | Float` as the attachment metadata stores them
 * (`messages/support.rs RubyNumber`).  Format with the internal
 * cf_view_number_format / cf_view_number_half helpers. */
typedef struct {
    bool is_float;
    int64_t integer;
    double real;
} cf_view_number;

/* `messages::SoundImage` / `SoundView` (`/play <name>`). */
typedef struct {
    cf_str src; /* owned: image_path(image.asset_path) */
    int64_t width, height;
} cf_view_sound_image;

typedef struct {
    cf_str url; /* owned: asset_path(sound.asset_path) */
    bool has_image;
    cf_view_sound_image image; /* owned when has_image */
    bool has_text;
    cf_str text; /* owned when has_text */
} cf_view_sound;

/* `messages::AttachmentPreview` and `AttachmentView`. */
typedef enum {
    CF_VIEW_PREVIEW_VIDEO = 0,
    CF_VIEW_PREVIEW_IMAGE = 1,
    CF_VIEW_PREVIEW_FILE = 2
} cf_view_preview_kind;

typedef struct {
    cf_str filename;      /* owned */
    cf_str blob_path;     /* owned: rails_blob_path(attachment) */
    cf_str download_path; /* owned: rails_blob_path(attachment, disposition: "attachment") */
    cf_view_preview_kind preview;
    cf_str preview_url; /* owned when preview != FILE (poster_url / thumb_url) */
    bool has_width;
    cf_view_number width;
    bool has_height;
    cf_view_number height;
} cf_view_attachment;

/* `messages::BoostView` (`message.boosts.ordered`). */
typedef struct {
    int64_t id;
    int64_t updated_at_us; /* 0 when the caller has no version (fragment keys only) */
    int64_t message_id;
    cf_str content; /* owned */
    bool all_emoji;
    cf_view_user booster;
} cf_view_boost;

typedef struct {
    cf_view_boost *items;
    size_t len, cap;
} cf_view_boost_vector;

void cf_view_boost_vector_dispose(cf_view_boost_vector *vector);

/* `messages::MessageContent`: the tag with what each presentation needs. */
typedef enum {
    CF_VIEW_CONTENT_TEXT = 0,
    CF_VIEW_CONTENT_SOUND = 1,
    CF_VIEW_CONTENT_ATTACHMENT = 2,
    CF_VIEW_CONTENT_UNRENDERABLE = 3
} cf_view_content_kind;

/* `messages::MessageView` as `messages/_message` renders it.  Timestamps are
 * UTC microseconds; the view prints iso8601 (seconds, Z) and the epoch
 * milliseconds the client compares. */
typedef struct {
    int64_t id;
    cf_str client_message_id; /* owned: Message#to_key */
    int64_t room_id;
    cf_str room_name; /* owned: room_display_name(room, for_user: nil) */
    cf_view_user creator;
    int64_t created_at_us, updated_at_us;
    bool all_emoji;
    cf_view_content_kind content_kind;
    cf_str text_html;              /* owned when TEXT; trusted presentation HTML */
    cf_view_sound sound;           /* owned when SOUND */
    cf_view_attachment attachment; /* owned when ATTACHMENT */
    cf_view_boost_vector boosts;
} cf_view_message;

void cf_view_message_dispose(cf_view_message *message);

/* `messages::MessageItem`: the fragment itself when the cache already holds
 * this message version, else the view to render it from.  No fragment cache
 * exists in this phase, so presenters produce the view arm; the renderers
 * still accept the fragment arm (an already-cached body goes out as-is). */
typedef struct {
    bool is_fragment;
    cf_str fragment_html;             /* owned when is_fragment */
    cf_str fragment_client_message_id; /* owned when is_fragment */
    int64_t fragment_room_id;
    cf_view_message message; /* owned when !is_fragment */
} cf_view_message_item;

void cf_view_message_item_dispose(cf_view_message_item *item);

typedef struct {
    cf_view_message_item *items;
    size_t len, cap;
} cf_view_message_item_vector;

void cf_view_message_item_vector_dispose(cf_view_message_item_vector *vector);

/* `rooms::ShowView`: the room page and its message area. */
typedef struct {
    cf_view_room room;
    int64_t updated_at_us; /* room.updated_at, the refresh controller's loaded_at */
    cf_view_user user;     /* Current.user, for the client-side message template */
    cf_view_message_item_vector messages;
    /* `@room == Room.original && !@room.messages.paged?` (rooms/show/_invitation). */
    bool invitation;
    cf_str join_code;             /* owned: Current.account.join_code */
    cf_str messages_stream_name;  /* owned: Turbo signed stream name */
} cf_view_room_show_model;

void cf_view_room_show_model_dispose(cf_view_room_show_model *model);

/* `messages::EditView`. */
typedef struct {
    cf_view_message message;
    cf_str editable_body_html; /* owned: editable_body(message) */
} cf_view_message_edit_model;

void cf_view_message_edit_model_dispose(cf_view_message_edit_model *model);

/* --------------------------------------------------------- searches models */

/* An owned list of owned strings (searches::IndexView's recent searches). */
typedef struct {
    cf_str *items;
    size_t len, cap;
} cf_view_str_vector;

void cf_view_str_vector_dispose(cf_view_str_vector *vector);

/* `searches::IndexView` (crates/views/src/searches.rs), the data
 * searches/index.html.erb renders. */
typedef struct {
    /* `@query`: params[:q] with every non-word character turned into a space,
     * present exactly when the param was. */
    bool has_query;
    cf_str query; /* owned when has_query */
    /* `params[:q]` as submitted, the search field's value. */
    bool has_q;
    cf_str q; /* owned when has_q */
    /* `Current.user.reachable_messages.search(query).last(100)`. */
    cf_view_message_item_vector messages;
    /* `Current.user.searches.ordered.pluck(:query)`, newest first. */
    cf_view_str_vector recent_searches;
    /* `last_room_visited.id` where the exit button goes (`unwrap_or_default`:
     * 0 when the user has no room). */
    int64_t return_to_room_id;
} cf_view_searches_index_model;

void cf_view_searches_index_model_dispose(cf_view_searches_index_model *model);

/* ----------------------------------------------------- users presenter */

/* A users row as the message/room views see it: `UserView` (name, User#title,
 * fresh_user_avatar_path).  Reads no rows. */
cf_err cf_presenter_user_view(cf_ctx *ctx, const cf_user *user,
                              cf_view_user *out);

/* ----------------------------------------------------- rooms presenters */

/* `Room#display_name` sentence (`to_sentence(names, " and ")`).  Owned. */
cf_err cf_view_room_display_name(cf_span name, bool direct,
                                 const cf_span *other_member_names,
                                 size_t other_member_name_count,
                                 cf_span for_user_name, cf_str *out);

/* The `rooms#show` page model, inside one read transaction: the room view for
 * `user`, the message page (the page around `message_id` when it names a
 * message of this room, else the last page), the invitation condition, the
 * account join code and the signed room message stream name.  `room` and
 * `user` are the rows the controller resolved; joined messages load their
 * creators, boosts, attachment and rich text through R02.  An attachment on a
 * message is CF_INVALID until S02 exposes signed paths (reported). */
cf_err cf_presenter_room_show(cf_ctx *ctx, const cf_room *room,
                              const cf_user *user, bool has_message_id,
                              int64_t message_id,
                              cf_view_room_show_model *out);

/* -------------------------------------------------------- message presenters */

/* One read transaction; `row` is an already-loaded messages row.  Maps the
 * creator, room name, boosts, plain text (all_emoji) and content.  Only the
 * message's own rows are read: the caller supplies the row. */
cf_err cf_presenter_message(cf_ctx *ctx, const cf_message *row,
                            cf_view_message *out);
cf_err cf_presenter_message_item(cf_ctx *ctx, const cf_message *row,
                                 cf_view_message_item *out);
/* The whole page through one read transaction. */
cf_err cf_presenter_messages(cf_ctx *ctx, const cf_message *rows, size_t count,
                             cf_view_message_item_vector *out);

/* Mapping without opening a transaction: the caller already holds a read
 * transaction (cf_presenter_room_show maps its message page inside its own).
 * Declared here for the rooms presenter; not part of the controller API. */
cf_err cf_presenter_messages_in_transaction(
    cf_ctx *ctx, const cf_message *rows, size_t count,
    cf_view_message_item_vector *out);
/* `messages::EditView`: the message plus `editable_body(message)`. */
cf_err cf_presenter_message_edit(cf_ctx *ctx, const cf_message *row,
                                 cf_view_message_edit_model *out);
/* `messages::BoostView` for one boost row (booster loaded here). */
cf_err cf_presenter_boost(cf_ctx *ctx, const cf_boost *row,
                          cf_view_boost *out);

/* ------------------------------------------------------ sidebar presenters */

/* `users::UserSummary` as the sidebar templates read it: the id, the name and
 * the token-based `fresh_user_avatar_path`.  The full summary (bio, role,
 * status, transfer ids, ...) belongs to A-users; the sidebar never reads the
 * extra fields, so the presenter does not load them. */
typedef struct {
    int64_t id;
    cf_str name;        /* owned */
    cf_str avatar_path; /* owned: fresh_user_avatar_path (S02 does not change it) */
} cf_view_sidebar_user;

void cf_view_sidebar_user_dispose(cf_view_sidebar_user *user);

typedef struct {
    cf_view_sidebar_user *items;
    size_t len, cap;
} cf_view_sidebar_user_vector;

void cf_view_sidebar_user_vector_dispose(cf_view_sidebar_user_vector *vector);

/* `users::SidebarDirect`: one `users/sidebars/rooms/_direct` membership. */
typedef struct {
    int64_t room_id;
    bool unread;
    cf_str updated_at_epoch; /* owned: room.updated_at.to_fs(:epoch) text */
    /* `room.users.without(membership.user).presence || [ membership.user ]` */
    cf_view_sidebar_user_vector members;
} cf_view_sidebar_direct;

void cf_view_sidebar_direct_dispose(cf_view_sidebar_direct *membership);

typedef struct {
    cf_view_sidebar_direct *items;
    size_t len, cap;
} cf_view_sidebar_direct_vector;

void cf_view_sidebar_direct_vector_dispose(
    cf_view_sidebar_direct_vector *vector);

/* `users::SidebarRoom`: one `users/sidebars/rooms/_shared` room. */
typedef struct {
    int64_t id;
    cf_str param_key; /* owned: "rooms_open" / "rooms_closed" / "rooms_direct" */
    cf_str name;      /* owned; empty for a nameless room */
    bool unread;
} cf_view_sidebar_room;

void cf_view_sidebar_room_dispose(cf_view_sidebar_room *room);

typedef struct {
    cf_view_sidebar_room *items;
    size_t len, cap;
} cf_view_sidebar_room_vector;

void cf_view_sidebar_room_vector_dispose(cf_view_sidebar_room_vector *vector);

/* `users::SidebarShow` (Users::SidebarsController#show). */
typedef struct {
    cf_view_sidebar_user current_user; /* owned */
    cf_str rooms_stream;               /* owned: signed_stream_name(:rooms) */
    cf_str user_rooms_stream;          /* owned: signed_stream_name([user, :rooms]) */
    cf_view_sidebar_direct_vector direct_memberships;       /* owned */
    cf_view_sidebar_user_vector direct_placeholder_users;   /* owned */
    cf_view_sidebar_room_vector other_memberships;          /* owned */
    /* `Current.user.administrator? || !account.settings.restrict_room_creation_to_administrators?` */
    bool can_create_rooms;
} cf_view_sidebar_model;

void cf_view_sidebar_model_dispose(cf_view_sidebar_model *model);

/* `Users::SidebarsController::DIRECT_PLACEHOLDERS` (the placeholder limit). */
#define CF_VIEW_SIDEBAR_DIRECT_PLACEHOLDERS 20

/* The sidebar read, inside one read transaction on ctx->reader: the visible
 * memberships (direct rooms sorted by room.updated_at descending, stable, and
 * the rest in `Membership::visible_with_ordered_room` order), the direct
 * placeholder users, the account's room-creation setting and the two signed
 * stream names.  No fragment cache exists yet, so direct memberships always
 * carry their view (the A02 message-item precedent); `user` is the
 * authenticated row the controller already resolved. */
cf_err cf_presenter_sidebar(cf_ctx *ctx, const cf_user *user,
                            cf_view_sidebar_model *out);

/* ------------------------------------------------------------- renderers */

/* Each family renders two ways, matching the reference's
 * page_or_frame: the application layout (full page) or turbo-rails' frame
 * layout (a `Turbo-Frame` request), which carries only the head and content
 * blocks.  All return cf_err; on failure the builder is unchanged. */

cf_err cf_view_layout_page(const cf_view_ctx *ctx, cf_span page_title,
                           bool has_page_title, cf_span body_class,
                           bool has_body_class, cf_span head, cf_span content,
                           cf_span nav, cf_span footer, cf_span sidebar,
                           cf_builder *out);
cf_err cf_view_layout_frame(const cf_view_ctx *ctx, cf_span head,
                            cf_span content, cf_builder *out);

cf_err cf_view_session_new(const cf_view_ctx *ctx,
                           const cf_view_session_new_model *model, cf_builder *out);
cf_err cf_view_session_new_frame(const cf_view_ctx *ctx,
                                 const cf_view_session_new_model *model,
                                 cf_builder *out);

cf_err cf_view_session_incompatible(const cf_view_ctx *ctx, cf_builder *out);
cf_err cf_view_session_incompatible_frame(const cf_view_ctx *ctx,
                                          cf_builder *out);

cf_err cf_view_session_transfer(const cf_view_ctx *ctx,
                                const cf_view_session_transfer_model *model,
                                cf_builder *out);
cf_err cf_view_session_transfer_frame(const cf_view_ctx *ctx,
                                      const cf_view_session_transfer_model *model,
                                      cf_builder *out);

cf_err cf_view_first_run(const cf_view_ctx *ctx, cf_builder *out);
cf_err cf_view_first_run_frame(const cf_view_ctx *ctx, cf_builder *out);

cf_err cf_view_welcome(const cf_view_ctx *ctx, const cf_view_welcome_model *model,
                       cf_builder *out);
cf_err cf_view_welcome_frame(const cf_view_ctx *ctx,
                             const cf_view_welcome_model *model, cf_builder *out);

/* rooms/involvements/_bell.html (the membership/involvement controls the
 * room header carries, including the pwa notification-help partials). */
cf_err cf_view_room_bell(const cf_view_ctx *ctx, const cf_view_room *room,
                         cf_builder *out);

/* rooms/show.html.erb (page and turbo-rails frame). */
cf_err cf_view_room_show(const cf_view_ctx *ctx,
                         const cf_view_room_show_model *model, cf_builder *out);
cf_err cf_view_room_show_frame(const cf_view_ctx *ctx,
                               const cf_view_room_show_model *model,
                               cf_builder *out);

/* messages/index.html.erb (`layout false`): the message partials only. */
cf_err cf_view_message_index(const cf_view_ctx *ctx,
                             const cf_view_message_item *items, size_t count,
                             cf_builder *out);
/* messages/show.html.erb inside the application layout. */
cf_err cf_view_message_show(const cf_view_ctx *ctx,
                            const cf_view_message *message, cf_builder *out);
/* messages/edit.html.erb inside the application layout. */
cf_err cf_view_message_edit(const cf_view_ctx *ctx,
                            const cf_view_message_edit_model *model,
                            cf_builder *out);
/* messages/_message and the partials it includes. */
cf_err cf_view_message_partial(const cf_view_ctx *ctx,
                               const cf_view_message *message, cf_builder *out);
cf_err cf_view_message_item_partial(const cf_view_ctx *ctx,
                                    const cf_view_message_item *item,
                                    cf_builder *out);
/* messages/_presentation (what messages#update broadcasts). */
cf_err cf_view_message_presentation(const cf_view_ctx *ctx,
                                    const cf_view_message *message,
                                    cf_builder *out);
/* messages/boosts/_boost (what a boost create broadcasts). */
cf_err cf_view_boost_partial(const cf_view_ctx *ctx, const cf_view_boost *boost,
                             cf_builder *out);
/* messages/boosts/_boosts. */
cf_err cf_view_boosts_partial(const cf_view_ctx *ctx,
                              const cf_view_message *message, cf_builder *out);

/* messages/create.turbo_stream.html.erb (page and nothing else). */
cf_err cf_view_message_create_stream(const cf_view_ctx *ctx,
                                     const cf_view_message_item *item,
                                     cf_room_type room_kind, cf_builder *out);
/* messages/destroy.turbo_stream.html.erb. */
cf_err cf_view_message_destroy_stream(const cf_view_message *message,
                                      cf_builder *out);
/* messages/room_not_found.html.erb inside the application layout. */
cf_err cf_view_message_room_not_found(const cf_view_ctx *ctx, cf_builder *out);

/* users/sidebars/show.html.erb (page and turbo-rails frame): the room list
 * for the `user_sidebar` turbo frame.  The content block wraps itself in
 * `sidebar_turbo_frame_tag` (no src), so the page's sidebar region is empty
 * and the frame response carries the same content in turbo-rails' frame
 * layout. */
cf_err cf_view_users_sidebar_show(const cf_view_ctx *ctx,
                                  const cf_view_sidebar_model *model,
                                  cf_builder *out);
cf_err cf_view_users_sidebar_show_frame(const cf_view_ctx *ctx,
                                        const cf_view_sidebar_model *model,
                                        cf_builder *out);

/* users/sidebars/rooms/_direct.html.erb and _shared.html.erb on their own
 * (the sidebar includes them; the room broadcasts render a single room). */
cf_err cf_view_sidebar_direct_partial(const cf_view_ctx *ctx,
                                      const cf_view_sidebar_direct *membership,
                                      cf_builder *out);
cf_err cf_view_sidebar_shared_partial(const cf_view_sidebar_room *room,
                                      cf_builder *out);

/* `SearchesHelper#search_path`: "/searches?q=" + CGI.escape(query) into the
 * caller's builder (the renderer's links and the create action's redirect).
 * The builder is left at its entry length on failure. */
cf_err cf_view_searches_search_path(cf_span query, cf_builder *out);

/* searches/index.html.erb (page and turbo-rails frame). */
cf_err cf_view_searches_index(const cf_view_ctx *ctx,
                              const cf_view_searches_index_model *model,
                              cf_builder *out);
cf_err cf_view_searches_index_frame(const cf_view_ctx *ctx,
                                    const cf_view_searches_index_model *model,
                                    cf_builder *out);

/* messages/boosts/index and messages/boosts/new (pages and frames). */
cf_err cf_view_boosts_index(const cf_view_ctx *ctx,
                            const cf_view_message *message, cf_builder *out);
cf_err cf_view_boosts_index_frame(const cf_view_ctx *ctx,
                                  const cf_view_message *message,
                                  cf_builder *out);
cf_err cf_view_new_boost(const cf_view_ctx *ctx, const cf_view_message *message,
                         const cf_view_user *user, cf_builder *out);
cf_err cf_view_new_boost_frame(const cf_view_ctx *ctx,
                               const cf_view_message *message,
                               const cf_view_user *user, cf_builder *out);

/* ------------------------------------------------------- users avatars */

/* `users/avatars/show.svg.erb` (crates/views/src/users.rs `AvatarSvg`): the
 * initials avatar A-users-avatars serves for a non-bot user without an
 * uploaded avatar.  `initials` is `User#initials` (cf_user_initials); the
 * fill color is `avatar_background_color(user_id)`.  Pure render: no rows,
 * no filesystem.  On failure the builder is unchanged. */
cf_err cf_view_users_avatar_svg(int64_t user_id, cf_span initials,
                                cf_builder *out);

/* The 8 MiB output cap (03-application.md A02). */
#define CF_VIEWS_MAX_OUTPUT (8u * 1024u * 1024u)

/* `Rails.application.config.app_version` (APP_VERSION / GIT_REVISION, else
 * "0").  The integrator bakes the build-time define; tests may compile an
 * override or set cf_view_layout_model.app_version directly. */
#ifndef CF_VIEWS_APP_VERSION
#define CF_VIEWS_APP_VERSION "0"
#endif

#endif /* CF_VIEWS_H */
