/* src/models/account.h — frozen model header (D01; declarations only).
 *
 * Source: tmp/rust-ref/crates/db/src/models/account.rs
 * Tables: accounts (schema.sql) and, through AccountSettings, its settings
 *         JSON column.
 * Prototypes in this header have no implementation yet (D01 owns headers;
 * a later model .c family implements them).
 */
#ifndef CF_MODELS_ACCOUNT_H
#define CF_MODELS_ACCOUNT_H

#include "types.h"

/* accounts row. */
typedef struct cf_account {
    int64_t id;
    cf_str name;
    cf_str join_code;
    cf_optional_str custom_styles; /* JSON column raw text */
    cf_optional_str settings_json; /* settings JSON column raw text; see
                                    * cf_account_settings_of */
    int64_t singleton_guard;
    int64_t created_at; /* UTC microseconds */
    int64_t updated_at; /* UTC microseconds */
} cf_account;

void cf_account_dispose(cf_account *account);

typedef struct cf_account_vector {
    cf_account *items;
    size_t len, cap;
} cf_account_vector;

void cf_account_vector_dispose(cf_account_vector *vector);

/* Parsed accounts.settings JSON with the schema defaults merged in
 * (AccountSettings). json is the owned canonical serialization; reading the
 * record's raw column instead keeps NULL distinct. */
typedef struct cf_account_settings {
    cf_str json;
} cf_account_settings;

void cf_account_settings_dispose(cf_account_settings *settings);

/* One (&str, &str) settings assignment pair; borrowed for the call. */
typedef struct {
    cf_str key;
    cf_str value;
} cf_account_setting;

/* Rust: AccountSettings::restrict_room_creation_to_administrators */
bool cf_account_settings_restrict_room_creation_to_administrators(const cf_account_settings *settings);
/* Rust: AccountSettings::set_restrict_room_creation_to_administrators */
cf_err cf_account_settings_set_restrict_room_creation_to_administrators(cf_account_settings *settings, cf_str value);
/* Rust: AccountSettings::assign — every key must be in the schema; on
 * CF_INVALID out_errors carries the reference message. */
cf_err cf_account_settings_assign(cf_account_settings *settings, const cf_account_setting *values, size_t values_len,
                                  cf_model_errors *out_errors);
/* Rust: AccountSettings::get — true and out = owned JSON text of the value
 * ("true", "\"x\"", ...) when the key exists; false leaves out empty. */
bool cf_account_settings_get(const cf_account_settings *settings, cf_str key, cf_str *out);
/* Rust: AccountSettings::to_json */
cf_err cf_account_settings_to_json(const cf_account_settings *settings, cf_str *out);
/* Rust: cast_boolean — false means the value is blank (Option::None). */
bool cf_account_cast_boolean(cf_str value, bool *out);
/* Rust: Account::settings — parse settings_json with defaults merged. */
cf_err cf_account_settings_of(const cf_account *account, cf_account_settings *out);

/* Rust: Account::first */
cf_err cf_account_first(cf_db *db, bool *found, cf_account *out);
/* Rust: Account::find — CF_NOT_FOUND when absent. */
cf_err cf_account_find(cf_db *db, int64_t id, cf_account *out);
/* Rust: Account::count */
cf_err cf_account_count(cf_db *db, int64_t *out);
/* Rust: Account::create */
cf_err cf_account_create(cf_tx *tx, cf_str name, cf_account *out);
/* Rust: Account::reset_join_code */
cf_err cf_account_reset_join_code(cf_tx *tx, cf_account *account);
/* Rust: Account::update — name: present=false leaves it unchanged;
 * custom_styles: NULL leaves it unchanged, present=false writes NULL;
 * settings: NULL leaves them unchanged, otherwise the whole borrowed slice is
 * assigned (cf_account_settings_assign semantics). */
cf_err cf_account_update(cf_tx *tx, cf_account *account, cf_optional_str name, const cf_optional_str *custom_styles,
                         const cf_account_setting *settings, size_t settings_len);
/* Rust: Account::reload */
cf_err cf_account_reload(cf_db *db, cf_account *account);
/* Rust: generate_join_code — SecureRandom.alphanumeric(12) as 4-4-4 groups. */
cf_err cf_account_generate_join_code(cf_str *out);

/* Every model-functions.json symbol for this module is prototyped above. */

#endif /* CF_MODELS_ACCOUNT_H */
