//! `test/models/first_run_test.rb`

use super::*;
use crate::{FirstRun, PasswordDigest, Room, User};

fn fresh() -> TestDb {
    let t = TestDb::new();
    // Account.destroy_all, Room.destroy_all, User.destroy_all
    t.write(|tx| {
        tx.conn().execute_batch(
            "DELETE FROM accounts; DELETE FROM boosts; DELETE FROM action_text_rich_texts; DELETE FROM messages; DELETE FROM memberships;
             DELETE FROM rooms; DELETE FROM sessions; DELETE FROM searches; DELETE FROM push_subscriptions; DELETE FROM webhooks;
             DELETE FROM bans; DELETE FROM users;",
        )?;
        Ok(())
    });
    t
}

fn create_first_run_user(t: &TestDb) -> User {
    let digest = PasswordDigest::create("secret123456", 4).unwrap();
    t.write(move |tx| FirstRun::create(tx, "User", "user@example.com", digest))
}

#[test]
fn creating_makes_first_user_an_administrator() {
    let t = fresh();
    assert!(create_first_run_user(&t).is_administrator());
}

#[test]
fn first_user_has_access_to_first_room() {
    let t = fresh();
    let user = create_first_run_user(&t);
    let rooms = t.read(|c| Room::for_user(c, user.id));
    assert_eq!(rooms.len(), 1);
    assert_eq!(rooms[0].name.as_deref(), Some("All Talk"));
}

#[test]
fn first_room_is_an_open_room() {
    let t = fresh();
    create_first_run_user(&t);
    assert!(t.read(|c| Ok(Room::original(c)?.unwrap())).open());
}

#[test]
fn first_user_can_sign_in() {
    let t = fresh();
    create_first_run_user(&t);
    let found = t.read(|c| User::find_active_by_email_address(c, "user@example.com"));
    assert!(User::authenticated(found.clone(), "secret123456").is_some());
    assert!(User::authenticated(found.clone(), "wrong").is_none());
    assert!(User::authenticated(found, "").is_none());
    let missing = t.read(|c| User::find_active_by_email_address(c, "nobody@example.com"));
    assert!(User::authenticated(missing, "secret123456").is_none());
}
