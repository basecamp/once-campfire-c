/* src/models/types.h — shared model vocabulary (D01 frozen header; no implementation).
 *
 * Sources:
 *   tmp/rust-ref/crates/db/src/models/user.rs       (Role, Status)
 *   tmp/rust-ref/crates/db/src/models/membership.rs (Involvement)
 *   tmp/rust-ref/crates/db/src/models/room.rs       (RoomType)
 *   tmp/rust-ref/crates/db/src/error.rs             (Errors)
 *   tmp/rust-ref/crates/db/src/time.rs              (Timestamp)
 * Tables: none. Each module header names its record's schema.sql tables.
 *
 * Frozen conventions (IMPLEMENTATION-ROADMAP contract change 2026-10-04, D01):
 *  - Owned text is a NUL-terminated cf_str {char *ptr; size_t len;} whose len
 *    excludes the NUL; release it with cf_str_dispose (also accepts empty
 *    state: {NULL, 0}). Optional text is cf_optional_str {bool present;
 *    cf_str value;}; present=false means the database column is NULL.
 *  - Numbers are int64_t. Nullable numbers and datetimes use cf_optional_i64
 *    (cf.h). Datetimes are UTC microseconds in memory; the reference
 *    six-fraction-digit SQL text form is only the storage representation.
 *  - JSON columns surface as cf_optional_str raw text; structured access goes
 *    through the owning model's helpers.
 *  - cf_str / cf_optional_str parameters are borrowed for the call only: the
 *    callee neither retains nor disposes them. Values written through out
 *    pointers are owned by the caller; out pointers start empty and stay empty
 *    on failure. CF_STR_LIT builds a borrowed literal argument.
 *  - Option<T> returns: pure lookups return bool (true = present) and write the
 *    value out; fallible reads return cf_err plus a bool *found out before the
 *    value out. A missing reference row returns CF_NOT_FOUND.
 *  - Slices are (const T *items, size_t len). Enum helpers use the type name.
 *  - A "Rust:" comment above a prototype names the translated reference
 *    function; these comments are the coverage anchors for
 *    contracts/model-functions.json (see docs/devel/evidence/D01-headers.md).
 */
#ifndef CF_MODELS_TYPES_H
#define CF_MODELS_TYPES_H

#include "cf.h"

/* ---- text ---------------------------------------------------------------- */

/* Owned NUL-terminated text; len excludes the NUL. */
typedef struct {
    char *ptr;
    size_t len;
} cf_str;

void cf_str_dispose(cf_str *value);

/* Optional text: present=false means absent (database NULL). */
typedef struct {
    bool present;
    cf_str value;
} cf_optional_str;

void cf_optional_str_dispose(cf_optional_str *value);

/* Borrowed cf_str for a string literal argument; never disposed. */
#define CF_STR_LIT(literal) ((cf_str){ (char *)(literal), sizeof(literal) - 1 })

/* ---- record-level validation errors -------------------------------------- */

/* ActiveModel::Errors equivalent: attribute/message pairs in the order they
 * were added. len == 0 means valid. */
typedef struct {
    cf_str field;
    cf_str message;
} cf_model_error;

typedef struct {
    cf_model_error *items;
    size_t len, cap;
} cf_model_errors;

void cf_model_errors_dispose(cf_model_errors *errors);

/* ---- shared scalar vectors ----------------------------------------------- */

/* Owned vector of copied int64 ids (e.g. Room::user_ids). */
typedef struct cf_int64_vector {
    int64_t *items;
    size_t len, cap;
} cf_int64_vector;

void cf_int64_vector_dispose(cf_int64_vector *vector);

/* ---- shared enums -------------------------------------------------------- */

/* Reference: Role (user.rs): member = 0, administrator = 1, bot = 2.
 * Role::name yields "administrator" (not "admin") in the pinned source. */
typedef enum {
    CF_ROLE_MEMBER = 0,
    CF_ROLE_ADMINISTRATOR = 1,
    CF_ROLE_BOT = 2
} cf_role;

/* Rust: Role::name */
const char *cf_role_name(cf_role role);
/* Rust: Role::from_name */
bool cf_role_from_name(cf_str name, cf_role *out);

/* Reference: Status (user.rs): active = 0, deactivated = 1, banned = 2. */
typedef enum {
    CF_STATUS_ACTIVE = 0,
    CF_STATUS_DEACTIVATED = 1,
    CF_STATUS_BANNED = 2
} cf_status;

/* Rust: Status::name */
const char *cf_status_name(cf_status status);
/* Rust: Status::from_name */
bool cf_status_from_name(cf_str name, cf_status *out);

/* Reference: Involvement (membership.rs): stored text names in declaration
 * order; the column default is "mentions". */
typedef enum {
    CF_INVOLVEMENT_INVISIBLE = 0,
    CF_INVOLVEMENT_NOTHING = 1,
    CF_INVOLVEMENT_MENTIONS = 2,
    CF_INVOLVEMENT_EVERYTHING = 3
} cf_involvement;

/* Optional involvement: present=false mirrors a NULL membership row. */
typedef struct {
    bool present;
    cf_involvement value;
} cf_optional_involvement;

/* Rust: Involvement::name */
const char *cf_involvement_name(cf_involvement involvement);
/* Rust: Involvement::from_name */
bool cf_involvement_from_name(cf_str name, cf_involvement *out);

/* Reference: RoomType (room.rs), stored as the STI "type" column text
 * "Rooms::Open" / "Rooms::Closed" / "Rooms::Direct". */
typedef enum {
    CF_ROOM_OPEN = 0,
    CF_ROOM_CLOSED = 1,
    CF_ROOM_DIRECT = 2
} cf_room_type;

/* Rust: RoomType::class_name */
const char *cf_room_type_class_name(cf_room_type room_type);
/* Rust: RoomType::from_class_name */
bool cf_room_type_from_class_name(cf_str name, cf_room_type *out);
/* Rust: RoomType::default_involvement ("everything" in direct rooms,
 * "mentions" elsewhere), typed rather than returned as stored text. */
cf_involvement cf_room_type_default_involvement(cf_room_type room_type);

/* ---- cross-module record and vector types -------------------------------- */

/* Defined by the module headers; the tags match so these forward typedefs are
 * compatible re-declarations used by prototypes that only need the name. */
typedef struct cf_account cf_account;
typedef struct cf_account_vector cf_account_vector;
typedef struct cf_account_settings cf_account_settings;
typedef struct cf_blob cf_blob;
typedef struct cf_blob_vector cf_blob_vector;
typedef struct cf_attachment cf_attachment;
typedef struct cf_attachment_vector cf_attachment_vector;
typedef struct cf_ban cf_ban;
typedef struct cf_ban_vector cf_ban_vector;
typedef struct cf_boost cf_boost;
typedef struct cf_boost_vector cf_boost_vector;
typedef struct cf_membership cf_membership;
typedef struct cf_membership_vector cf_membership_vector;
typedef struct cf_membership_room_pair cf_membership_room_pair;
typedef struct cf_membership_room_pair_vector cf_membership_room_pair_vector;
typedef struct cf_message cf_message;
typedef struct cf_message_vector cf_message_vector;
typedef struct cf_push_subscription cf_push_subscription;
typedef struct cf_push_subscription_vector cf_push_subscription_vector;
typedef struct cf_push_payload cf_push_payload;
typedef struct cf_rich_text_record cf_rich_text_record;
typedef struct cf_rich_text_record_vector cf_rich_text_record_vector;
typedef struct cf_room cf_room;
typedef struct cf_room_vector cf_room_vector;
typedef struct cf_search cf_search;
typedef struct cf_search_vector cf_search_vector;
typedef struct cf_session cf_session;
typedef struct cf_session_vector cf_session_vector;
typedef struct cf_sound cf_sound;
typedef struct cf_sound_image cf_sound_image;
typedef struct cf_sound_name_vector cf_sound_name_vector;
typedef struct cf_user cf_user;
typedef struct cf_user_vector cf_user_vector;
typedef struct cf_webhook cf_webhook;
typedef struct cf_webhook_vector cf_webhook_vector;

/* The Action Text pipeline the message model calls for plain text and
 * mentions. D01 freezes only this pointer type; R02 supplies the concrete
 * definition and construction in src/richtext.h (see the D01 evidence for the
 * proposed boundary). */
typedef struct cf_richtext cf_richtext;

#endif /* CF_MODELS_TYPES_H */
