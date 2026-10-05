/* src/models/account.c — D01 model family "account" (02-data-auth.md D01).
 *
 * Source of truth: tmp/rust-ref/crates/db/src/models/account.rs (pinned).
 * Translation decisions that the frozen src/models/account.h does not spell
 * out, and where the C shape differs from the Rust source:
 *
 *  - Statements are fixed per module and cached per connection through the
 *    D01 db-core cf_stmt_set contract (no SQL is assembled at runtime).  The
 *    source's update assembles only the changed columns from a fixed set; its
 *    C translation is the seven fixed subsets of {name, custom_styles,
 *    settings} in that source order, selected by their bitmask.
 *  - Account::update's `name`/`custom_styles` values are compared and stored
 *    before the UPDATE runs, and `updated_at` is set before the step, exactly
 *    as the source mutates `self`; on failure the in-memory mutation is the
 *    source's behavior while the row is untouched.
 *  - The source's `tx.now()` is the process clock (Tx::now -> Env::now); the
 *    frozen header gives no transaction clock accessor, so mutations read
 *    cf_now_us(NULL) (F01).  Tests inject it through core/testclock.h.
 *  - AccountSettings is owned canonical JSON text in C (header comment).  The
 *    JSON object is parsed with the locked yyjson amalgamation (the same
 *    library H02 uses), preserving the source's insertion order (serde_json
 *    "preserve_order"): the schema default key is appended when absent,
 *    set_... replaces a key in place, and unknown keys survive a round trip.
 *    Duplicate keys collapse to the first position with the last value,
 *    matching serde_json's Map deserialization.  Equality for update's
 *    "settings changed?" check is the source's order-insensitive Map
 *    equality, via yyjson_mut_equals.
 *  - cast_boolean/present copy the source exactly, including Rust's
 *    str::trim() Unicode White_Space set for non-blank strings.
 */

#include "models/account.h"

#include "db/db_internal.h"

#include <stdlib.h>
#include <string.h>

#include "yyjson.h"

/* The single settings key the schema understands
 * (AccountSettings::RESTRICT_ROOM_CREATION). */
#define ACCOUNT_SETTINGS_KEY "restrict_room_creation_to_administrators"

/* columns! expansion of Account's field: "column" list. */
#define ACCOUNT_COLUMNS                                                       \
    "\"accounts\".\"id\", \"accounts\".\"name\", \"accounts\".\"join_code\", " \
    "\"accounts\".\"custom_styles\", \"accounts\".\"settings\", "             \
    "\"accounts\".\"singleton_guard\", \"accounts\".\"created_at\", "         \
    "\"accounts\".\"updated_at\""

/* --- fixed statements ---------------------------------------------------- */

enum {
    ACCOUNT_STMT_FIRST = 0,
    ACCOUNT_STMT_FIND,
    ACCOUNT_STMT_COUNT,
    ACCOUNT_STMT_INSERT,
    ACCOUNT_STMT_RESET_JOIN_CODE,
    /* The seven fixed update subsets, in mask order 1..7 (name=1,
     * custom_styles=2, settings=4), each ending with the source's
     * updated_at touch. */
    ACCOUNT_STMT_UPDATE_NAME,
    ACCOUNT_STMT_UPDATE_STYLES,
    ACCOUNT_STMT_UPDATE_NAME_STYLES,
    ACCOUNT_STMT_UPDATE_SETTINGS,
    ACCOUNT_STMT_UPDATE_NAME_SETTINGS,
    ACCOUNT_STMT_UPDATE_STYLES_SETTINGS,
    ACCOUNT_STMT_UPDATE_ALL,
    ACCOUNT_STMT_TABLE_SIZE
};

#define ACCOUNT_UPDATE_1 "UPDATE \"accounts\" SET \"name\" = ?, \"updated_at\" = ? WHERE \"accounts\".\"id\" = ?"
#define ACCOUNT_UPDATE_2 "UPDATE \"accounts\" SET \"custom_styles\" = ?, \"updated_at\" = ? WHERE \"accounts\".\"id\" = ?"
#define ACCOUNT_UPDATE_3 "UPDATE \"accounts\" SET \"name\" = ?, \"custom_styles\" = ?, \"updated_at\" = ? WHERE \"accounts\".\"id\" = ?"
#define ACCOUNT_UPDATE_4 "UPDATE \"accounts\" SET \"settings\" = ?, \"updated_at\" = ? WHERE \"accounts\".\"id\" = ?"
#define ACCOUNT_UPDATE_5 "UPDATE \"accounts\" SET \"name\" = ?, \"settings\" = ?, \"updated_at\" = ? WHERE \"accounts\".\"id\" = ?"
#define ACCOUNT_UPDATE_6 "UPDATE \"accounts\" SET \"custom_styles\" = ?, \"settings\" = ?, \"updated_at\" = ? WHERE \"accounts\".\"id\" = ?"
#define ACCOUNT_UPDATE_7 "UPDATE \"accounts\" SET \"name\" = ?, \"custom_styles\" = ?, \"settings\" = ?, \"updated_at\" = ? WHERE \"accounts\".\"id\" = ?"

static const cf_stmt_def account_stmt_defs[] = {
    [ACCOUNT_STMT_FIRST] = {
        "SELECT " ACCOUNT_COLUMNS
        " FROM \"accounts\" ORDER BY \"accounts\".\"id\" ASC LIMIT 1"},
    [ACCOUNT_STMT_FIND] = {
        "SELECT " ACCOUNT_COLUMNS
        " FROM \"accounts\" WHERE \"accounts\".\"id\" = ? LIMIT 1"},
    [ACCOUNT_STMT_COUNT] = {"SELECT COUNT(*) FROM \"accounts\""},
    [ACCOUNT_STMT_INSERT] = {
        "INSERT INTO \"accounts\" (\"created_at\", \"custom_styles\", "
        "\"join_code\", \"name\", \"settings\", \"singleton_guard\", "
        "\"updated_at\") VALUES (?, ?, ?, ?, ?, ?, ?) RETURNING \"id\""},
    [ACCOUNT_STMT_RESET_JOIN_CODE] = {
        "UPDATE \"accounts\" SET \"join_code\" = ?, \"updated_at\" = ? "
        "WHERE \"accounts\".\"id\" = ?"},
    [ACCOUNT_STMT_UPDATE_NAME] = {ACCOUNT_UPDATE_1},
    [ACCOUNT_STMT_UPDATE_STYLES] = {ACCOUNT_UPDATE_2},
    [ACCOUNT_STMT_UPDATE_NAME_STYLES] = {ACCOUNT_UPDATE_3},
    [ACCOUNT_STMT_UPDATE_SETTINGS] = {ACCOUNT_UPDATE_4},
    [ACCOUNT_STMT_UPDATE_NAME_SETTINGS] = {ACCOUNT_UPDATE_5},
    [ACCOUNT_STMT_UPDATE_STYLES_SETTINGS] = {ACCOUNT_UPDATE_6},
    [ACCOUNT_STMT_UPDATE_ALL] = {ACCOUNT_UPDATE_7},
};

static const cf_stmt_set account_stmt_set = {
    account_stmt_defs, sizeof account_stmt_defs / sizeof account_stmt_defs[0]};

_Static_assert(ACCOUNT_STMT_TABLE_SIZE ==
                   sizeof account_stmt_defs / sizeof account_stmt_defs[0],
               "statement enum and fixed table must match");
_Static_assert(ACCOUNT_STMT_UPDATE_NAME + 6 == ACCOUNT_STMT_UPDATE_ALL,
               "update subsets must be contiguous in mask order");

/* --- small text helpers -------------------------------------------------- */

static bool account_text_equal(cf_str left, cf_str right) {
    if (left.len != right.len) return false;
    if (left.len == 0) return true;
    if (left.ptr == NULL || right.ptr == NULL) return false;
    return memcmp(left.ptr, right.ptr, left.len) == 0;
}

static bool account_optional_text_equal(cf_optional_str left,
                                        cf_optional_str right) {
    if (left.present != right.present) return false;
    return !left.present || account_text_equal(left.value, right.value);
}

/* Owned copy of borrowed text; empty text stays {NULL, 0}. */
static cf_err account_str_copy(cf_str source, cf_str *out) {
    out->ptr = NULL;
    out->len = 0;
    if (source.len == 0) return CF_OK;
    if (source.ptr == NULL) {
        return cf_db_failf(CF_INVALID, "text has no bytes");
    }
    char *copy = malloc(source.len + 1);
    if (copy == NULL) {
        return cf_db_failf(CF_NOMEM, "out of memory copying model text");
    }
    memcpy(copy, source.ptr, source.len);
    copy[source.len] = '\0';
    out->ptr = copy;
    out->len = source.len;
    return CF_OK;
}

static cf_err account_optional_str_copy(cf_optional_str source,
                                        cf_optional_str *out) {
    out->present = false;
    out->value.ptr = NULL;
    out->value.len = 0;
    if (!source.present) return CF_OK;
    out->present = true;
    return account_str_copy(source.value, &out->value);
}

static cf_span account_span_of(cf_str text) {
    cf_span span;
    span.ptr = (const unsigned char *)text.ptr;
    span.len = text.len;
    return span;
}

/* --- JSON helpers (AccountSettings) -------------------------------------- */

/* A mutable JSON object with its owning document. */
typedef struct {
    yyjson_mut_doc *doc;
    yyjson_mut_val *obj;
} account_json;

static void account_json_dispose(account_json *json) {
    if (json->doc != NULL) yyjson_mut_doc_free(json->doc);
    json->doc = NULL;
    json->obj = NULL;
}

/* Parse `text` as a JSON object into a fresh mutable object.  Anything that
 * is not a JSON object (absent, empty, invalid, null, array, scalar) yields an
 * empty object, matching from_column's `unwrap_or_default`.  Members are
 * copied in order; duplicate keys collapse to the first position with the
 * last value, as serde_json's Map does. */
static cf_err account_json_parse(const char *text, size_t len,
                                 account_json *out) {
    out->doc = NULL;
    out->obj = NULL;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (doc == NULL) {
        return cf_db_failf(CF_NOMEM, "out of memory parsing account settings");
    }
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    if (obj == NULL) {
        yyjson_mut_doc_free(doc);
        return cf_db_failf(CF_NOMEM, "out of memory parsing account settings");
    }
    yyjson_mut_doc_set_root(doc, obj);

    if (text != NULL && len != 0) {
        yyjson_doc *parsed = yyjson_read(text, len, 0);
        if (parsed != NULL) {
            yyjson_val *root = yyjson_doc_get_root(parsed);
            if (root != NULL && yyjson_is_obj(root)) {
                yyjson_obj_iter iter;
                yyjson_obj_iter_init(root, &iter);
                yyjson_val *key = NULL;
                while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
                    yyjson_val *value = yyjson_obj_iter_get_val(key);
                    yyjson_mut_val *mut_key =
                        yyjson_mut_strncpy(doc, yyjson_get_str(key),
                                           yyjson_get_len(key));
                    yyjson_mut_val *mut_value = yyjson_val_mut_copy(doc, value);
                    if (mut_key == NULL || mut_value == NULL ||
                        !yyjson_mut_obj_put(obj, mut_key, mut_value)) {
                        yyjson_doc_free(parsed);
                        yyjson_mut_doc_free(doc);
                        return cf_db_failf(
                            CF_NOMEM,
                            "out of memory copying account settings");
                    }
                }
            }
            yyjson_doc_free(parsed);
        }
    }

    out->doc = doc;
    out->obj = obj;
    return CF_OK;
}

/* Compact canonical serialization (serde_json::to_string equivalent). */
static cf_err account_json_write(const account_json *json, cf_str *out) {
    out->ptr = NULL;
    out->len = 0;
    size_t len = 0;
    char *text = yyjson_mut_write(json->doc, 0, &len);
    if (text == NULL) {
        return cf_db_failf(CF_NOMEM, "out of memory encoding account settings");
    }
    out->ptr = text;
    out->len = len;
    return CF_OK;
}

static yyjson_mut_val *account_json_get(const account_json *json, cf_str key) {
    if (key.len != 0 && key.ptr == NULL) return NULL;
    const char *name = key.ptr != NULL ? key.ptr : "";
    return yyjson_mut_obj_getn(json->obj, name, key.len);
}

/* Rust's str::trim() White_Space property (char::is_whitespace). */
static bool account_whitespace(uint32_t cp) {
    switch (cp) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20:
    case 0x85: case 0xA0: case 0x1680:
    case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004:
    case 0x2005: case 0x2006: case 0x2007: case 0x2008: case 0x2009:
    case 0x200A: case 0x2028: case 0x2029: case 0x202F: case 0x205F:
    case 0x3000:
        return true;
    default:
        return false;
    }
}

/* True when the whole (valid UTF-8) string is whitespace: Rust
 * `s.trim().is_empty()`.  Invalid UTF-8 is not whitespace. */
static bool account_string_blank(const char *text, size_t len) {
    size_t i = 0;
    while (i < len) {
        unsigned char first = (unsigned char)text[i];
        uint32_t cp;
        size_t width;
        if (first < 0x80) {
            cp = first;
            width = 1;
        } else if ((first & 0xE0) == 0xC0) {
            cp = first & 0x1F;
            width = 2;
        } else if ((first & 0xF0) == 0xE0) {
            cp = first & 0x0F;
            width = 3;
        } else if ((first & 0xF8) == 0xF0) {
            cp = first & 0x07;
            width = 4;
        } else {
            return false;
        }
        if (i + width > len) return false;
        for (size_t k = 1; k < width; k++) {
            unsigned char cont = (unsigned char)text[i + k];
            if ((cont & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cont & 0x3F);
        }
        if (!account_whitespace(cp)) return false;
        i += width;
    }
    return true;
}

/* AccountSettings::present of a parsed value. */
static bool account_json_present(const yyjson_mut_val *value) {
    if (value == NULL) return false;
    switch (yyjson_mut_get_type(value)) {
    case YYJSON_TYPE_NONE:
    case YYJSON_TYPE_NULL:
        return false;
    case YYJSON_TYPE_BOOL:
        return yyjson_mut_get_bool(value);
    case YYJSON_TYPE_STR:
        return !account_string_blank(yyjson_mut_get_str(value),
                                     yyjson_mut_get_len(value));
    case YYJSON_TYPE_ARR:
        return yyjson_mut_arr_size(value) != 0;
    case YYJSON_TYPE_OBJ:
        return yyjson_mut_obj_size(value) != 0;
    default: /* numbers (and raw, which the reader never produces) */
        return true;
    }
}

/* `undefined method '<key>=' for account settings` as an owned cf_str. */
static cf_err account_undefined_setter_message(cf_str key, cf_str *out) {
    static const char prefix[] = "undefined method '";
    static const char suffix[] = "=' for account settings";
    const size_t prefix_len = sizeof prefix - 1;
    const size_t suffix_len = sizeof suffix - 1;
    out->ptr = NULL;
    out->len = 0;
    if (key.len > SIZE_MAX - prefix_len - suffix_len - 1) {
        return cf_db_failf(CF_LIMIT, "settings key too long to report");
    }
    size_t total = prefix_len + key.len + suffix_len;
    char *message = malloc(total + 1);
    if (message == NULL) {
        return cf_db_failf(CF_NOMEM, "out of memory recording settings error");
    }
    memcpy(message, prefix, prefix_len);
    if (key.len != 0) memcpy(message + prefix_len, key.ptr, key.len);
    memcpy(message + prefix_len + key.len, suffix, suffix_len);
    message[total] = '\0';
    out->ptr = message;
    out->len = total;
    return CF_OK;
}

static cf_err account_errors_push(cf_model_errors *errors,
                                  cf_model_error error) {
    if (errors->len == errors->cap) {
        size_t cap = errors->cap != 0 ? errors->cap * 2 : 4;
        cf_model_error *items = realloc(errors->items, cap * sizeof *items);
        if (items == NULL) {
            return cf_db_failf(CF_NOMEM,
                               "out of memory recording settings error");
        }
        errors->items = items;
        errors->cap = cap;
    }
    errors->items[errors->len++] = error;
    return CF_OK;
}

/* value, cast as a boolean: Some(bool) or Null (cast_boolean None). */
static cf_err account_settings_set_from_text(account_json *json, cf_str value) {
    bool cast = false;
    yyjson_mut_val *val = cf_account_cast_boolean(value, &cast)
                              ? yyjson_mut_bool(json->doc, cast)
                              : yyjson_mut_null(json->doc);
    yyjson_mut_val *key = yyjson_mut_strcpy(json->doc, ACCOUNT_SETTINGS_KEY);
    if (val == NULL || key == NULL || !yyjson_mut_obj_put(json->obj, key, val)) {
        return cf_db_failf(CF_NOMEM, "out of memory updating account settings");
    }
    return CF_OK;
}

/* One assign pair: the known key is set, anything else is the source's
 * Error::other.  On CF_INVALID *out_message carries the owned message. */
static cf_err account_settings_apply(account_json *json,
                                     cf_account_setting value,
                                     cf_str *out_message) {
    out_message->ptr = NULL;
    out_message->len = 0;
    if (account_text_equal(value.key, CF_STR_LIT(ACCOUNT_SETTINGS_KEY))) {
        return account_settings_set_from_text(json, value.value);
    }
    cf_err err = account_undefined_setter_message(value.key, out_message);
    return err == CF_OK ? CF_INVALID : err;
}

/* --- AccountSettings API ------------------------------------------------- */

bool cf_account_cast_boolean(cf_str value, bool *out) {
    static const char *const false_values[] = {
        "0", "f", "F", "false", "FALSE", "off", "OFF"};
    if (out == NULL) return false;
    if (value.len == 0) return false;
    if (value.ptr == NULL) return false;
    bool result = true;
    for (size_t i = 0; i < sizeof false_values / sizeof false_values[0]; i++) {
        const char *candidate = false_values[i];
        if (value.len == strlen(candidate) &&
            memcmp(value.ptr, candidate, value.len) == 0) {
            result = false;
            break;
        }
    }
    *out = result;
    return true;
}

bool cf_account_settings_restrict_room_creation_to_administrators(
    const cf_account_settings *settings) {
    /* The frozen prototype has no error channel: an unparseable or
     * non-object value is the empty map, so the key is absent (false), and a
     * parse allocation failure is reported as false with the message left in
     * cf_db_last_error(). */
    if (settings == NULL) return false;
    account_json json = {0};
    if (account_json_parse(settings->json.ptr, settings->json.len, &json) !=
        CF_OK) {
        return false;
    }
    bool present = account_json_present(
        yyjson_mut_obj_get(json.obj, ACCOUNT_SETTINGS_KEY));
    account_json_dispose(&json);
    return present;
}

cf_err cf_account_settings_set_restrict_room_creation_to_administrators(
    cf_account_settings *settings, cf_str value) {
    if (settings == NULL) {
        return cf_db_failf(CF_INVALID, "no account settings");
    }
    account_json json = {0};
    cf_err err = account_json_parse(settings->json.ptr, settings->json.len,
                                    &json);
    if (err != CF_OK) return err;
    err = account_settings_set_from_text(&json, value);
    if (err == CF_OK) {
        cf_str text = {0};
        err = account_json_write(&json, &text);
        if (err == CF_OK) {
            free(settings->json.ptr);
            settings->json = text;
        }
    }
    account_json_dispose(&json);
    return err;
}

cf_err cf_account_settings_assign(cf_account_settings *settings,
                                  const cf_account_setting *values,
                                  size_t values_len,
                                  cf_model_errors *out_errors) {
    if (settings == NULL) {
        return cf_db_failf(CF_INVALID, "no account settings");
    }
    if (values_len != 0 && values == NULL) {
        return cf_db_failf(CF_INVALID, "settings list has no items");
    }
    account_json json = {0};
    cf_err err = account_json_parse(settings->json.ptr, settings->json.len,
                                    &json);
    if (err != CF_OK) return err;

    for (size_t i = 0; i < values_len; i++) {
        cf_str message = {0};
        err = account_settings_apply(&json, values[i], &message);
        if (err != CF_OK) {
            /* The source's &mut assign keeps earlier assignments in the map
             * when a later key is unknown, so persist the partial state. */
            cf_str partial = {0};
            if (account_json_write(&json, &partial) == CF_OK) {
                free(settings->json.ptr);
                settings->json = partial;
            }
            if (message.ptr != NULL) {
                cf_db_failf(err, "%s", message.ptr);
                if (out_errors != NULL) {
                    cf_str field = {0};
                    if (account_str_copy(values[i].key, &field) == CF_OK) {
                        cf_model_error error = {field, message};
                        if (account_errors_push(out_errors, error) == CF_OK) {
                            message.ptr = NULL; /* owned by out_errors now */
                        } else {
                            free(field.ptr);
                        }
                    }
                }
                free(message.ptr);
            }
            account_json_dispose(&json);
            return err;
        }
    }

    cf_str text = {0};
    err = account_json_write(&json, &text);
    if (err == CF_OK) {
        free(settings->json.ptr);
        settings->json = text;
    }
    account_json_dispose(&json);
    return err;
}

bool cf_account_settings_get(const cf_account_settings *settings, cf_str key,
                             cf_str *out) {
    /* bool-only prototype: absent key, unparseable settings and an allocation
     * failure all leave out empty and return false; the failure reason (if
     * any) stays in cf_db_last_error(). */
    if (settings == NULL || out == NULL) return false;
    account_json json = {0};
    if (account_json_parse(settings->json.ptr, settings->json.len, &json) !=
        CF_OK) {
        return false;
    }
    bool found = false;
    yyjson_mut_val *value = account_json_get(&json, key);
    if (value != NULL) {
        size_t len = 0;
        char *text = yyjson_mut_val_write(value, 0, &len);
        if (text != NULL) {
            out->ptr = text;
            out->len = len;
            found = true;
        }
    }
    account_json_dispose(&json);
    return found;
}

cf_err cf_account_settings_to_json(const cf_account_settings *settings,
                                   cf_str *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "no output pointer");
    }
    out->ptr = NULL;
    out->len = 0;
    if (settings == NULL) {
        return cf_db_failf(CF_INVALID, "no account settings");
    }
    account_json json = {0};
    cf_err err = account_json_parse(settings->json.ptr, settings->json.len,
                                    &json);
    if (err != CF_OK) return err;
    err = account_json_write(&json, out);
    account_json_dispose(&json);
    return err;
}

cf_err cf_account_settings_of(const cf_account *account,
                              cf_account_settings *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "no output pointer");
    }
    out->json.ptr = NULL;
    out->json.len = 0;
    if (account == NULL) {
        return cf_db_failf(CF_INVALID, "no account");
    }

    const char *raw = NULL;
    size_t raw_len = 0;
    if (account->settings_json.present) {
        raw = account->settings_json.value.ptr;
        raw_len = account->settings_json.value.len;
    }

    account_json json = {0};
    cf_err err = account_json_parse(raw, raw_len, &json);
    if (err != CF_OK) return err;

    /* data.entry(KEY).or_insert(false): appended only when absent. */
    if (yyjson_mut_obj_get(json.obj, ACCOUNT_SETTINGS_KEY) == NULL) {
        yyjson_mut_val *key = yyjson_mut_strcpy(json.doc, ACCOUNT_SETTINGS_KEY);
        yyjson_mut_val *val = yyjson_mut_bool(json.doc, false);
        if (key == NULL || val == NULL ||
            !yyjson_mut_obj_put(json.obj, key, val)) {
            account_json_dispose(&json);
            return cf_db_failf(
                CF_NOMEM, "out of memory merging account settings defaults");
        }
    }

    err = account_json_write(&json, &out->json);
    account_json_dispose(&json);
    return err;
}

/* --- row reading --------------------------------------------------------- */

static cf_err account_column_text(sqlite3_stmt *stmt, int column,
                                  cf_optional_str *out) {
    out->present = false;
    out->value.ptr = NULL;
    out->value.len = 0;
    cf_buf *buf = NULL;
    cf_err err = cf_stmt_column_copy_text(stmt, column, &buf);
    if (err != CF_OK) return err;
    if (buf == NULL) return CF_OK; /* NULL column */
    out->present = true;
    cf_span bytes = cf_buf_span(buf);
    if (bytes.len == 0) {
        err = CF_OK;
    } else {
        char *copy = malloc(bytes.len + 1);
        if (copy == NULL) {
            err = cf_db_failf(CF_NOMEM, "out of memory copying accounts text");
        } else {
            memcpy(copy, bytes.ptr, bytes.len);
            copy[bytes.len] = '\0';
            out->value.ptr = copy;
            out->value.len = bytes.len;
            err = CF_OK;
        }
    }
    cf_buf_release(buf);
    return err;
}

static cf_err account_column_time(sqlite3_stmt *stmt, int column,
                                  int64_t *out) {
    cf_span text = cf_stmt_column_text(stmt, column);
    return cf_db_time_from_text(text, out);
}

static cf_err account_from_row(sqlite3_stmt *stmt, cf_account *out) {
    cf_account row = {0};
    cf_optional_str text = {0};
    cf_err err = CF_OK;

    row.id = cf_stmt_column_i64(stmt, 0);

    err = account_column_text(stmt, 1, &text);
    if (err != CF_OK) goto fail;
    if (!text.present) {
        err = cf_db_failf(CF_DB, "accounts.name is NULL");
        goto fail;
    }
    row.name = text.value;

    err = account_column_text(stmt, 2, &text);
    if (err != CF_OK) goto fail;
    if (!text.present) {
        err = cf_db_failf(CF_DB, "accounts.join_code is NULL");
        goto fail;
    }
    row.join_code = text.value;

    err = account_column_text(stmt, 3, &row.custom_styles);
    if (err != CF_OK) goto fail;
    err = account_column_text(stmt, 4, &row.settings_json);
    if (err != CF_OK) goto fail;

    row.singleton_guard = cf_stmt_column_i64(stmt, 5);
    err = account_column_time(stmt, 6, &row.created_at);
    if (err != CF_OK) goto fail;
    err = account_column_time(stmt, 7, &row.updated_at);
    if (err != CF_OK) goto fail;

    *out = row;
    return CF_OK;

fail:
    cf_account_dispose(&row);
    return err;
}

/* One row-returning statement; `id` binds only when bind_id is true. */
static cf_err account_select(cf_db *db, size_t stmt_id, bool bind_id, int64_t id,
                             bool *found, cf_account *out) {
    if (found == NULL || out == NULL) {
        return cf_db_failf(CF_INVALID, "no output pointer");
    }
    *found = false;
    sqlite3_stmt *stmt = NULL;
    cf_err err = cf_db_stmt(db, &account_stmt_set, stmt_id, &stmt);
    if (err != CF_OK) return err;
    if (bind_id) err = cf_stmt_bind_i64(stmt, 1, id);
    if (err == CF_OK) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            cf_account row = {0};
            err = account_from_row(stmt, &row);
            if (err == CF_OK) {
                *out = row;
                *found = true;
            }
        } else if (rc != SQLITE_DONE) {
            err = cf_db_failf(cf_db_err(rc), "accounts select: %s",
                              sqlite3_errmsg(cf_db_handle(db)));
        }
    }
    cf_db_stmt_done(stmt);
    return err;
}

/* --- Account reads ------------------------------------------------------- */

cf_err cf_account_first(cf_db *db, bool *found, cf_account *out) {
    return account_select(db, ACCOUNT_STMT_FIRST, false, 0, found, out);
}

cf_err cf_account_find(cf_db *db, int64_t id, cf_account *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "no output pointer");
    }
    bool found = false;
    cf_err err = account_select(db, ACCOUNT_STMT_FIND, true, id, &found, out);
    if (err != CF_OK) return err;
    if (!found) return cf_db_failf(CF_NOT_FOUND, "Couldn't find Account");
    return CF_OK;
}

cf_err cf_account_count(cf_db *db, int64_t *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "no output pointer");
    }
    *out = 0;
    sqlite3_stmt *stmt = NULL;
    cf_err err = cf_db_stmt(db, &account_stmt_set, ACCOUNT_STMT_COUNT, &stmt);
    if (err != CF_OK) return err;
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out = cf_stmt_column_i64(stmt, 0);
    } else if (rc != SQLITE_DONE) {
        err = cf_db_failf(cf_db_err(rc), "accounts count: %s",
                          sqlite3_errmsg(cf_db_handle(db)));
    } else {
        err = cf_db_failf(CF_DB, "accounts count returned no row");
    }
    cf_db_stmt_done(stmt);
    return err;
}

/* --- Account mutations --------------------------------------------------- */

static cf_err account_tx_db(cf_tx *tx, cf_db **out) {
    *out = NULL;
    if (tx == NULL) {
        return cf_db_failf(CF_INTERNAL, "no transaction");
    }
    cf_db *db = cf_tx_db(tx);
    if (db == NULL) {
        return cf_db_failf(CF_INTERNAL, "transaction has no database");
    }
    *out = db;
    return CF_OK;
}

cf_err cf_account_create(cf_tx *tx, cf_str name, cf_account *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "no output pointer");
    }
    if (name.len != 0 && name.ptr == NULL) {
        return cf_db_failf(CF_INVALID, "account name has no bytes");
    }
    cf_db *db = NULL;
    cf_err err = account_tx_db(tx, &db);
    if (err != CF_OK) return err;

    /* tx.now() once for both created_at and updated_at. */
    int64_t now = cf_now_us(NULL);
    char now_text[CF_DB_TIME_TEXT_CAP];
    err = cf_db_time_to_text(now, now_text);
    if (err != CF_OK) return err;
    cf_span now_span = {(const unsigned char *)now_text, strlen(now_text)};

    cf_str join_code = {0};
    err = cf_account_generate_join_code(&join_code);
    if (err != CF_OK) return err;

    /* AccountSettings::from_column(None).to_json(). */
    cf_account_settings settings = {0};
    cf_account blank = {0};
    err = cf_account_settings_of(&blank, &settings);
    if (err != CF_OK) {
        free(join_code.ptr);
        return err;
    }

    sqlite3_stmt *stmt = NULL;
    err = cf_db_stmt(db, &account_stmt_set, ACCOUNT_STMT_INSERT, &stmt);
    if (err == CF_OK) {
        err = cf_stmt_bind_text(stmt, 1, now_span);          /* created_at */
        if (err == CF_OK) err = cf_stmt_bind_null(stmt, 2);  /* custom_styles */
        if (err == CF_OK) err = cf_stmt_bind_text(stmt, 3, account_span_of(join_code));
        if (err == CF_OK) err = cf_stmt_bind_text(stmt, 4, account_span_of(name));
        if (err == CF_OK) err = cf_stmt_bind_text(stmt, 5, account_span_of(settings.json));
        if (err == CF_OK) err = cf_stmt_bind_i64(stmt, 6, 0); /* singleton_guard */
        if (err == CF_OK) err = cf_stmt_bind_text(stmt, 7, now_span); /* updated_at */
        if (err == CF_OK) {
            int rc = sqlite3_step(stmt);
            if (rc == SQLITE_ROW) {
                int64_t id = cf_stmt_column_i64(stmt, 0);
                err = cf_account_find(db, id, out);
            } else {
                err = cf_db_failf(cf_db_err(rc), "accounts insert: %s",
                                  sqlite3_errmsg(cf_db_handle(db)));
            }
        }
    }
    cf_db_stmt_done(stmt);
    free(join_code.ptr);
    free(settings.json.ptr);
    return err;
}

cf_err cf_account_reset_join_code(cf_tx *tx, cf_account *account) {
    if (account == NULL) {
        return cf_db_failf(CF_INVALID, "no account");
    }
    cf_db *db = NULL;
    cf_err err = account_tx_db(tx, &db);
    if (err != CF_OK) return err;

    int64_t now = cf_now_us(NULL);
    char now_text[CF_DB_TIME_TEXT_CAP];
    err = cf_db_time_to_text(now, now_text);
    if (err != CF_OK) return err;
    cf_span now_span = {(const unsigned char *)now_text, strlen(now_text)};

    cf_str join_code = {0};
    err = cf_account_generate_join_code(&join_code);
    if (err != CF_OK) return err;

    sqlite3_stmt *stmt = NULL;
    err = cf_db_stmt(db, &account_stmt_set, ACCOUNT_STMT_RESET_JOIN_CODE, &stmt);
    if (err == CF_OK) {
        err = cf_stmt_bind_text(stmt, 1, account_span_of(join_code));
        if (err == CF_OK) err = cf_stmt_bind_text(stmt, 2, now_span);
        if (err == CF_OK) err = cf_stmt_bind_i64(stmt, 3, account->id);
        if (err == CF_OK) {
            int rc = sqlite3_step(stmt);
            if (rc != SQLITE_DONE) {
                err = cf_db_failf(cf_db_err(rc), "accounts reset_join_code: %s",
                                  sqlite3_errmsg(cf_db_handle(db)));
            }
        }
    }
    cf_db_stmt_done(stmt);
    if (err != CF_OK) {
        free(join_code.ptr);
        return err;
    }

    free(account->join_code.ptr);
    account->join_code = join_code;
    account->updated_at = now;
    return CF_OK;
}

cf_err cf_account_update(cf_tx *tx, cf_account *account, cf_optional_str name,
                         const cf_optional_str *custom_styles,
                         const cf_account_setting *settings,
                         size_t settings_len) {
    if (account == NULL) {
        return cf_db_failf(CF_INVALID, "no account");
    }
    if (settings_len != 0 && settings == NULL) {
        return cf_db_failf(CF_INVALID, "settings list has no items");
    }
    cf_db *db = NULL;
    cf_err err = account_tx_db(tx, &db);
    if (err != CF_OK) return err;

    enum { CHANGE_NAME = 1, CHANGE_STYLES = 2, CHANGE_SETTINGS = 4 };
    unsigned mask = 0;

    if (name.present && !account_text_equal(name.value, account->name)) {
        cf_str copy = {0};
        err = account_str_copy(name.value, &copy);
        if (err != CF_OK) return err;
        free(account->name.ptr);
        account->name = copy;
        mask |= CHANGE_NAME;
    }

    if (custom_styles != NULL &&
        !account_optional_text_equal(*custom_styles, account->custom_styles)) {
        cf_optional_str copy = {0};
        err = account_optional_str_copy(*custom_styles, &copy);
        if (err != CF_OK) return err;
        free(account->custom_styles.value.ptr);
        account->custom_styles = copy;
        mask |= CHANGE_STYLES;
    }

    if (settings != NULL) {
        /* original = self.settings(); updated = original.clone();
         * updated.assign(values)?  Both sides carry the merged defaults. */
        cf_account_settings original = {0};
        err = cf_account_settings_of(account, &original);
        if (err != CF_OK) return err;

        account_json before = {0};
        err = account_json_parse(original.json.ptr, original.json.len, &before);
        if (err != CF_OK) {
            free(original.json.ptr);
            return err;
        }
        account_json updated_doc = {0};
        err = account_json_parse(original.json.ptr, original.json.len,
                                 &updated_doc);
        free(original.json.ptr);
        if (err != CF_OK) {
            account_json_dispose(&before);
            return err;
        }

        for (size_t i = 0; i < settings_len; i++) {
            cf_str message = {0};
            err = account_settings_apply(&updated_doc, settings[i], &message);
            if (err != CF_OK) {
                if (message.ptr != NULL) {
                    cf_db_failf(err, "%s", message.ptr);
                    free(message.ptr);
                }
                account_json_dispose(&updated_doc);
                account_json_dispose(&before);
                return err;
            }
        }

        if (!account->settings_json.present ||
            !yyjson_mut_equals(before.obj, updated_doc.obj)) {
            cf_str json_text = {0};
            err = account_json_write(&updated_doc, &json_text);
            if (err == CF_OK) {
                free(account->settings_json.value.ptr);
                account->settings_json.present = true;
                account->settings_json.value = json_text;
                mask |= CHANGE_SETTINGS;
            }
        }
        account_json_dispose(&updated_doc);
        account_json_dispose(&before);
        if (err != CF_OK) return err;
    }

    if (mask == 0) return CF_OK;

    int64_t now = cf_now_us(NULL);
    char now_text[CF_DB_TIME_TEXT_CAP];
    err = cf_db_time_to_text(now, now_text);
    if (err != CF_OK) return err;
    cf_span now_span = {(const unsigned char *)now_text, strlen(now_text)};
    account->updated_at = now;

    size_t stmt_id = (size_t)ACCOUNT_STMT_UPDATE_NAME + (mask - 1);
    sqlite3_stmt *stmt = NULL;
    err = cf_db_stmt(db, &account_stmt_set, stmt_id, &stmt);
    if (err == CF_OK) {
        int index = 1;
        if (mask & CHANGE_NAME) {
            err = cf_stmt_bind_text(stmt, index++, account_span_of(account->name));
        }
        if (err == CF_OK && (mask & CHANGE_STYLES)) {
            err = cf_stmt_bind_opt_text(stmt, index++,
                                        account->custom_styles.present,
                                        account_span_of(account->custom_styles.value));
        }
        if (err == CF_OK && (mask & CHANGE_SETTINGS)) {
            err = cf_stmt_bind_text(stmt, index++,
                                    account_span_of(account->settings_json.value));
        }
        if (err == CF_OK) err = cf_stmt_bind_text(stmt, index++, now_span);
        if (err == CF_OK) err = cf_stmt_bind_i64(stmt, index, account->id);
        if (err == CF_OK) {
            int rc = sqlite3_step(stmt);
            if (rc != SQLITE_DONE) {
                err = cf_db_failf(cf_db_err(rc), "accounts update: %s",
                                  sqlite3_errmsg(cf_db_handle(db)));
            }
        }
    }
    cf_db_stmt_done(stmt);
    return err;
}

cf_err cf_account_reload(cf_db *db, cf_account *account) {
    if (account == NULL) {
        return cf_db_failf(CF_INVALID, "no account");
    }
    cf_account fresh = {0};
    cf_err err = cf_account_find(db, account->id, &fresh);
    if (err != CF_OK) return err;
    cf_account_dispose(account);
    *account = fresh;
    return CF_OK;
}

/* --- join codes ---------------------------------------------------------- */

cf_err cf_account_generate_join_code(cf_str *out) {
    if (out == NULL) {
        return cf_db_failf(CF_INVALID, "no output pointer");
    }
    out->ptr = NULL;
    out->len = 0;

    /* SecureRandom.alphanumeric(12), 62-character alphabet. */
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    enum { CODE_LEN = 12 };
    char code[CODE_LEN];
    for (size_t i = 0; i < CODE_LEN; i++) {
        unsigned char byte = 0;
        for (int attempt = 0; attempt < 64; attempt++) {
            cf_err err = cf_random_bytes(&byte, 1);
            if (err != CF_OK) return err;
            if (byte < 248) break; /* 248 = 62 * 4: uniform over 0..247 */
        }
        code[i] = alphabet[byte % 62];
    }

    /* "{a}-{b}-{c}" from the 12 characters. */
    char joined[CODE_LEN + 2 + 1];
    memcpy(joined, code, 4);
    joined[4] = '-';
    memcpy(joined + 5, code + 4, 4);
    joined[9] = '-';
    memcpy(joined + 10, code + 8, 4);
    joined[14] = '\0';

    char *owned = malloc(sizeof joined);
    if (owned == NULL) {
        return cf_db_failf(CF_NOMEM, "out of memory generating join code");
    }
    memcpy(owned, joined, sizeof joined);
    out->ptr = owned;
    out->len = CODE_LEN + 2;
    return CF_OK;
}

/* --- disposal ------------------------------------------------------------ */

void cf_account_dispose(cf_account *account) {
    if (account == NULL) return;
    free(account->name.ptr);
    free(account->join_code.ptr);
    free(account->custom_styles.value.ptr);
    free(account->settings_json.value.ptr);
    memset(account, 0, sizeof *account);
}

void cf_account_vector_dispose(cf_account_vector *vector) {
    if (vector == NULL) return;
    for (size_t i = 0; i < vector->len; i++) {
        cf_account_dispose(&vector->items[i]);
    }
    free(vector->items);
    vector->items = NULL;
    vector->len = 0;
    vector->cap = 0;
}

void cf_account_settings_dispose(cf_account_settings *settings) {
    if (settings == NULL) return;
    free(settings->json.ptr);
    settings->json.ptr = NULL;
    settings->json.len = 0;
}
