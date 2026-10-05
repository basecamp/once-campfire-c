//! `test/models/account_test.rb`, `test/models/account/joinable_test.rb`

use super::*;
use crate::Account;

fn signal(t: &TestDb) -> Account {
    t.read(|c| Ok(Account::first(c)?.unwrap()))
}

#[test]
fn settings() {
    let t = TestDb::new();
    let mut account = signal(&t);
    assert_eq!(account.settings_json, None, "fixture leaves settings NULL");

    let mut settings = account.settings();
    settings.set_restrict_room_creation_to_administrators("true");
    assert!(settings.restrict_room_creation_to_administrators());
    assert_eq!(settings.to_json(), r#"{"restrict_room_creation_to_administrators":true}"#);

    let mut a = account.clone();
    t.write(move |tx| a.update(tx, None, None, Some(&[("restrict_room_creation_to_administrators", "true")])));
    account.reload_from(&t);
    assert!(account.settings().restrict_room_creation_to_administrators());
    assert_eq!(account.settings_json.as_deref(), Some(r#"{"restrict_room_creation_to_administrators":true}"#));

    let mut settings = account.settings();
    settings.set_restrict_room_creation_to_administrators("false");
    assert!(!settings.restrict_room_creation_to_administrators());
    assert_eq!(settings.to_json(), r#"{"restrict_room_creation_to_administrators":false}"#);

    let mut a = account.clone();
    t.write(move |tx| a.update(tx, None, None, Some(&[("restrict_room_creation_to_administrators", "false")])));
    account.reload_from(&t);
    assert!(!account.settings().restrict_room_creation_to_administrators());
}

#[test]
fn updating_other_attributes_leaves_null_settings_alone() {
    // What Rails does: `update!(name:)` on the fixture account doesn't write settings.
    let t = TestDb::new();
    let mut account = signal(&t);
    t.write(move |tx| account.update(tx, Some("X"), None, None));
    let account = signal(&t);
    assert_eq!(account.name, "X");
    assert_eq!(account.settings_json, None);
}

#[test]
fn unknown_settings_are_rejected() {
    let t = TestDb::new();
    let mut account = signal(&t);
    assert!(t.try_write(move |tx| account.update(tx, None, None, Some(&[("nope", "1")]))).is_err());
}

#[test]
fn new_accounts_get_a_joinable_code() {
    let t = TestDb::new();
    let account = t.write(|tx| {
        tx.conn().execute("DELETE FROM accounts", [])?;
        Account::create(tx, "Chat")
    });
    let parts: Vec<&str> = account.join_code.split('-').collect();
    assert_eq!(parts.len(), 3);
    assert!(parts.iter().all(|p| p.len() == 4 && p.chars().all(|c| c.is_ascii_alphanumeric())));
    assert_eq!(account.settings_json.as_deref(), Some(r#"{"restrict_room_creation_to_administrators":false}"#));
    assert_eq!(account.singleton_guard, 0);
}

#[test]
fn only_one_account_can_exist() {
    let t = TestDb::new();
    assert!(t.try_write(|tx| Account::create(tx, "Second")).is_err());
}

#[test]
fn accounts_can_reset_join_code() {
    let t = TestDb::new();
    let before = signal(&t).join_code;
    let mut account = signal(&t);
    t.write(move |tx| account.reset_join_code(tx));
    assert_ne!(signal(&t).join_code, before);
}

impl Account {
    fn reload_from(&mut self, t: &TestDb) {
        let id = self.id;
        *self = t.read(|c| Account::find(c, id));
    }
}
