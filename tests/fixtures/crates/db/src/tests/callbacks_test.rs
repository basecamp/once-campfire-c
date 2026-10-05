//! Callback side effects, checked against what the reference app's SQL log shows for the
//! same operations (see the crate report for the scenarios).

use jiff::SignedDuration;

use super::*;
use crate::{Boost, Membership, Message, NewMessage, Room, Search, Session, Timestamp};

fn fts_body(t: &TestDb, message_id: i64) -> Option<String> {
    t.read(|c| crate::sql::query_one(c, "SELECT body FROM message_search_index WHERE rowid = ?", [message_id], |r| r.get(0)))
}

#[test]
fn creating_a_message_touches_the_room_marks_disconnected_members_unread_and_indexes_after_commit() {
    let t = TestDb::new();
    t.write(|tx| Membership::find(tx.conn(), id("jz_designers"))?.connected(tx));
    let room_before = t.read(|c| Room::find(c, id("designers")));
    t.travel(1);

    let attributes = NewMessage {
        room_id: id("designers"),
        creator_id: id("david"),
        client_message_id: Some("abc".into()),
        body: Some("Hello <b>there</b>".into()),
        attachment_blob_id: None,
    };
    let message = t.write(move |tx| Message::create(tx, attributes));

    // The body touches the message; the room is touched too.
    assert!(message.updated_at >= message.created_at);
    assert!(t.read(|c| Room::find(c, id("designers"))).updated_at > room_before.updated_at);

    // Unread: visible, disconnected, not the creator; unread_at is the message's created_at.
    let unread = |label: &str| t.read(|c| Membership::find(c, id(label))).unread_at;
    assert_eq!(unread("jason_designers"), Some(message.created_at));
    assert_eq!(unread("kevin_designers"), Some(message.created_at));
    assert_eq!(unread("jz_designers"), None, "connected");
    assert_eq!(unread("david_designers"), None, "creator");

    assert_eq!(fts_body(&t, message.id).as_deref(), Some("Hello there"));
    assert_eq!(t.events(), vec![Event::PushMessage { room_id: id("designers"), message_id: message.id }]);

    let body: (String, String, i64) = t.read(|c| {
        Ok(c.query_row("SELECT body, record_type, record_id FROM action_text_rich_texts WHERE record_id = ?", [message.id], |r| {
            Ok((r.get(0)?, r.get(1)?, r.get(2)?))
        })?)
    });
    assert_eq!(body, ("Hello <b>there</b>".into(), "Message".into(), message.id));
}

#[test]
fn invisible_members_are_not_marked_unread() {
    let t = TestDb::new();
    t.write(|tx| Membership::find(tx.conn(), id("kevin_designers"))?.update_involvement(tx, crate::Involvement::Invisible));
    let attributes = NewMessage { room_id: id("designers"), creator_id: id("david"), body: Some("x".into()), ..Default::default() };
    t.write(move |tx| Message::create(tx, attributes));
    assert_eq!(t.read(|c| Membership::find(c, id("kevin_designers"))).unread_at, None);
}

#[test]
fn rolled_back_writes_emit_nothing_after_commit() {
    let t = TestDb::new();
    let attributes = NewMessage { room_id: id("designers"), creator_id: id("david"), body: Some("x".into()), ..Default::default() };
    let result: crate::Result<()> = t.try_write(move |tx| {
        Message::create(tx, attributes)?;
        Err(crate::Error::other("boom"))
    });
    assert!(result.is_err());
    assert!(t.events().is_empty());
    assert_eq!(t.read(Message::count), 13);
}

#[test]
fn boosting_touches_the_message_and_room_and_reindexes() {
    let t = TestDb::new();
    let first = t.read(|c| Message::find(c, id("first")));
    let room_before = t.read(|c| Room::find(c, id("designers")));
    t.travel(1);
    let boost = t.write(|tx| Boost::create(tx, id("first"), id("jason"), "hi"));
    let touched = t.read(|c| Message::find(c, id("first")));
    assert!(touched.updated_at > first.updated_at);
    assert!(t.read(|c| Room::find(c, id("designers"))).updated_at > room_before.updated_at);
    // The fixture message was never indexed; the after_commit update finds no row, as in Rails.
    assert_eq!(fts_body(&t, first.id), None);

    t.travel(1);
    t.write(move |tx| boost.destroy(tx));
    assert!(t.read(|c| Message::find(c, id("first"))).updated_at > touched.updated_at);
    assert!(t.read(|c| Boost::for_message(c, id("first"))).len() == 1);
}

#[test]
fn boosts_are_ordered_by_creation() {
    let t = TestDb::new();
    t.travel(1);
    t.write(|tx| Boost::create(tx, id("first"), id("jason"), "2"));
    let contents: Vec<String> = t.read(|c| Message::find(c, id("first"))?.boosts(c)).into_iter().map(|b| b.content).collect();
    assert_eq!(contents, ["Hello", "2"]);
}

#[test]
fn session_start_and_resume() {
    let t = TestDb::new();
    let david = id("david");
    let session = t.write(move |tx| Session::start(tx, david, Some("ua"), Some("1.2.3.4")));
    assert_eq!(session.token.len(), 24);
    assert!(session.token.chars().all(|c| c.is_ascii_alphanumeric() && !"0OIl".contains(c)));
    assert_eq!(t.read(|c| Session::find_by_token(c, &session.token)).unwrap().id, session.id);

    // Within the hour: nothing is written.
    let s = session.clone();
    let resumed = t.write(move |tx| {
        let mut s = s;
        s.resume(tx, Some("ua2"), Some("9.9.9.9"))?;
        Ok(s)
    });
    assert_eq!(t.read(|c| Session::find(c, session.id)).user_agent.as_deref(), Some("ua"));

    t.clock.travel(SignedDuration::from_hours(1) + SignedDuration::from_secs(1));
    t.write(move |tx| {
        let mut s = resumed;
        s.resume(tx, Some("ua2"), Some("9.9.9.9"))
    });
    let reloaded = t.read(|c| Session::find(c, session.id));
    assert_eq!((reloaded.user_agent.as_deref(), reloaded.ip_address.as_deref()), (Some("ua2"), Some("9.9.9.9")));
    assert!(reloaded.last_active_at > session.last_active_at);
}

#[test]
fn fixture_session_resumes_because_it_is_two_hours_old() {
    let t = TestDb::new();
    let session = t.read(|c| Session::find(c, id("david_safari")));
    t.write(move |tx| {
        let mut s = session;
        s.resume(tx, Some("ua"), Some("9.9.9.9"))
    });
    assert_eq!(t.read(|c| Session::find(c, id("david_safari"))).ip_address.as_deref(), Some("9.9.9.9"));
}

#[test]
fn recording_searches_keeps_the_ten_most_recent() {
    let t = TestDb::new();
    let david = id("david");
    for n in 0..12 {
        t.travel(1);
        t.write(move |tx| Search::record(tx, david, &format!("query {n}")));
    }
    let searches = t.read(|c| Search::ordered_for_user(c, david));
    // The 12th creation trims to ten before its touch.
    assert_eq!(searches.len(), 10);
    assert_eq!(searches[0].query, "query 11");
    assert!(!searches.iter().any(|s| s.query == "pizza"));

    t.travel(1);
    t.write(move |tx| Search::record(tx, david, "query 5"));
    assert_eq!(t.read(|c| Search::ordered_for_user(c, david))[0].query, "query 5");
}

#[test]
fn timestamps_are_written_like_active_record() {
    let t = TestDb::new();
    t.clock.travel_to(Timestamp::parse_db("2026-09-26 12:34:56.123456").unwrap().jiff());
    let attributes = NewMessage { room_id: id("hq"), creator_id: id("david"), body: Some("x".into()), ..Default::default() };
    let message = t.write(move |tx| Message::create(tx, attributes));
    let (created_at, typeof_created): (String, String) = t.read(|c| {
        Ok(c.query_row("SELECT created_at, typeof(created_at) FROM messages WHERE id = ?", [message.id], |r| Ok((r.get(0)?, r.get(1)?)))?)
    });
    assert_eq!((created_at.as_str(), typeof_created.as_str()), ("2026-09-26 12:34:56.123456", "text"));

    t.clock.travel_to(Timestamp::parse_db("2026-09-26 12:00:00").unwrap().jiff());
    let attributes = NewMessage { room_id: id("hq"), creator_id: id("david"), body: Some("y".into()), ..Default::default() };
    let message = t.write(move |tx| Message::create(tx, attributes));
    let created_at: String = t.read(|c| Ok(c.query_row("SELECT created_at FROM messages WHERE id = ?", [message.id], |r| r.get(0))?));
    assert_eq!(created_at, "2026-09-26 12:00:00", "no fraction when microseconds are zero");
}

#[tokio::test(flavor = "multi_thread")]
async fn async_writes_and_reads() {
    let t = tokio::task::block_in_place(TestDb::new);
    let writes = (0..20).map(|n| {
        let db = t.db.clone();
        tokio::spawn(async move { db.write(move |tx| Search::record(tx, id("jason"), &format!("q{n}"))).await })
    });
    for write in writes {
        write.await.unwrap().unwrap();
    }
    let count = t.db.read(|c| Search::count_for_user(c, id("jason"))).await.unwrap();
    assert_eq!(count, 10);
}
