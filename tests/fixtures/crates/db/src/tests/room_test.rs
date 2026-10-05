//! `test/models/room_test.rb`, `rooms/direct_test.rb`, `rooms/open_test.rb`

use super::*;
use crate::{Involvement, Membership, Message, Room, RoomType, User};

fn member_ids(t: &TestDb, room_id: i64) -> Vec<i64> {
    t.read(|c| Room::find(c, room_id)?.user_ids(c))
}

fn room(t: &TestDb, label: &str) -> Room {
    let room_id = id(label);
    t.read(|c| Room::find(c, room_id))
}

#[test]
fn grant_membership_to_user() {
    let t = TestDb::new();
    let watercooler = room(&t, "watercooler");
    t.write(move |tx| watercooler.grant_to(tx, &[id("kevin")]));
    assert!(member_ids(&t, id("watercooler")).contains(&id("kevin")));
}

#[test]
fn revoke_membership_from_user() {
    let t = TestDb::new();
    let watercooler = room(&t, "watercooler");
    t.write(move |tx| watercooler.revoke_from(tx, &[id("david")]));
    assert!(!member_ids(&t, id("watercooler")).contains(&id("david")));
    assert_eq!(t.events(), vec![Event::DisconnectUser { user_id: id("david"), reconnect: true }]);
}

#[test]
fn revise_memberships() {
    let t = TestDb::new();
    let watercooler = room(&t, "watercooler");
    t.write(move |tx| watercooler.revise(tx, &[id("kevin")], &[id("david")]));
    let members = member_ids(&t, id("watercooler"));
    assert!(members.contains(&id("kevin")));
    assert!(!members.contains(&id("david")));
}

#[test]
fn create_for_users_by_giving_them_immediate_membership() {
    let t = TestDb::new();
    let room = t.write(|tx| Room::create_for(tx, RoomType::Closed, Some("Hello!"), id("david"), &[id("kevin"), id("david")]));
    let members = member_ids(&t, room.id);
    assert!(members.contains(&id("kevin")) && members.contains(&id("david")));
}

#[test]
fn type_predicates() {
    let t = TestDb::new();
    assert!(room(&t, "pets").open());
    assert!(!room(&t, "pets").direct());
    assert!(room(&t, "david_and_jason").direct());
    assert!(room(&t, "designers").closed());
}

#[test]
fn default_involvement_for_new_users() {
    let t = TestDb::new();
    let room = t.write(|tx| Room::create_for(tx, RoomType::Closed, Some("Hello!"), id("david"), &[id("kevin"), id("david")]));
    let memberships = t.read(|c| room.memberships(c));
    assert_eq!(memberships.len(), 2);
    assert!(memberships.iter().all(|m| m.involved_in(Involvement::Mentions)));
}

#[test]
fn granted_memberships_are_stamped_by_sqlite() {
    // insert_all lets SQLite fill the timestamps: STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW').
    let t = TestDb::new();
    let room = t.write(|tx| Room::create_for(tx, RoomType::Closed, Some("Hello!"), id("david"), &[id("kevin")]));
    let created_at: String = t.read(|c| Ok(c.query_row("SELECT created_at FROM memberships WHERE room_id = ?", [room.id], |r| r.get(0))?));
    assert_eq!(created_at.len(), "2026-09-26 12:25:26.826".len(), "{created_at}");
}

#[test]
fn direct_rooms_keep_their_type() {
    let t = TestDb::new();
    let mut direct = room(&t, "david_and_jason");
    let result = t.try_write(move |tx| direct.update(tx, None, Some(RoomType::Open)));
    match result {
        Err(crate::Error::RecordInvalid(errors)) => {
            assert_eq!(errors.on("type"), ["can't be changed for a direct room"])
        }
        other => panic!("expected invalid, got {other:?}"),
    }
}

#[test]
fn destroying_a_room_destroys_its_messages_and_memberships() {
    let t = TestDb::new();
    let watercooler = room(&t, "watercooler");
    t.write(move |tx| watercooler.destroy(tx));
    assert!(t.read(|c| Message::for_room(c, id("watercooler"))).is_empty());
    assert!(t.read(|c| Membership::for_room(c, id("watercooler"))).is_empty());
    assert!(t.read(|c| Room::find_by_id(c, id("watercooler"))).is_none());
    assert_eq!(
        t.read(|c| crate::sql::count(c, "SELECT COUNT(*) FROM boosts WHERE message_id IN (?, ?)", [id("thirteenth"), id("fourth")])),
        0
    );
}

// Rooms::Direct

#[test]
fn create_direct_room_for_same_users() {
    let t = TestDb::new();
    let room = t.write(|tx| Room::find_or_create_direct_for(tx, &[id("jz"), id("kevin")], id("jz")));
    let members = member_ids(&t, room.id);
    assert!(members.contains(&id("jz")) && members.contains(&id("kevin")));
    assert!(!members.contains(&id("jason")));
}

#[test]
fn only_one_direct_room_will_exist_for_the_same_users() {
    let t = TestDb::new();
    let room1 = t.write(|tx| Room::find_or_create_direct_for(tx, &[id("jz"), id("kevin")], id("jz")));
    let room2 = t.write(|tx| Room::find_or_create_direct_for(tx, &[id("kevin"), id("jz")], id("kevin")));
    assert_eq!(room1.id, room2.id);

    let existing = t.write(|tx| Room::find_or_create_direct_for(tx, &[id("david"), id("kevin")], id("david")));
    assert_eq!(existing.id, id("david_and_kevin"));
}

#[test]
fn direct_default_involvement_for_new_users() {
    let t = TestDb::new();
    let room = t.write(|tx| Room::find_or_create_direct_for(tx, &[id("jz"), id("kevin")], id("jz")));
    assert!(t.read(|c| room.memberships(c)).iter().all(|m| m.involved_in(Involvement::Everything)));
}

// Rooms::Open

#[test]
fn open_room_grants_access_to_all_users_after_creation() {
    let t = TestDb::new();
    let room = t.write(|tx| Room::create(tx, RoomType::Open, Some("My open room with everyone!"), id("david")));
    assert_eq!(member_ids(&t, room.id).len() as i64, t.read(User::count));
}

#[test]
fn open_room_grants_access_to_all_users_after_becoming_open() {
    let t = TestDb::new();
    let mut watercooler = room(&t, "watercooler");
    t.write(move |tx| watercooler.update(tx, None, Some(RoomType::Open)));
    assert_eq!(member_ids(&t, id("watercooler")).len() as i64, t.read(User::count));
    assert_eq!(room(&t, "watercooler").room_type, RoomType::Open);
    let stored: String = t.read(|c| Ok(c.query_row("SELECT type FROM rooms WHERE id = ?", [id("watercooler")], |r| r.get(0))?));
    assert_eq!(stored, "Rooms::Open");
}

#[test]
fn user_room_scopes() {
    let t = TestDb::new();
    let david = id("david");
    assert_eq!(t.read(|c| Room::for_user_of_type(c, david, RoomType::Direct)).len(), 2);
    assert_eq!(t.read(|c| Room::for_user_without_directs(c, david)).len(), 4);
    assert!(t.read(|c| Room::find_for_user(c, id("kevin"), id("pets"))).is_none());
    let ordered = t.read(|c| Membership::visible_with_ordered_room(c, david));
    let names: Vec<_> = ordered.iter().map(|(_, r)| r.name.clone()).collect();
    assert_eq!(names[names.len() - 4..], [Some("All Pets".into()), Some("All Talk".into()), Some("Designers".into()), Some("HQ".into())]);
}
