//! `test/models/message_test.rb`, `message/searchable_test.rb`, `message/attachment_test.rb`
//! (the parts that are data, not image processing), and pagination.

use super::*;
use crate::models::active_storage::Blob;
use crate::models::message::PAGE_SIZE;
use crate::{Message, NewMessage, Timestamp};

fn create(t: &TestDb, room: &str, creator: &str, body: &str, client_message_id: &str) -> Message {
    let attributes = NewMessage {
        room_id: id(room),
        creator_id: id(creator),
        client_message_id: Some(client_message_id.into()),
        body: Some(body.into()),
        attachment_blob_id: None,
    };
    t.write(move |tx| Message::create(tx, attributes))
}

fn search(t: &TestDb, room: &str, query: &str) -> Vec<i64> {
    let room_id = id(room);
    t.read(|c| Message::search_in_room(c, room_id, query)).into_iter().map(|m| m.id).collect()
}

#[test]
fn creating_a_message_enqueues_to_push_later() {
    let t = TestDb::new();
    let message = create(&t, "designers", "jason", "Hello", "123");
    let pushes: Vec<_> = t.events().into_iter().filter(|e| matches!(e, Event::PushMessage { .. })).collect();
    assert_eq!(pushes, vec![Event::PushMessage { room_id: id("designers"), message_id: message.id }]);
}

/// Which mentioned users are mentionees; reading the mentions is `campfire_richtext`'s.
#[test]
fn mentionees() {
    let t = TestDb::new();
    let pets = id("pets");
    let mentionees = |user_ids: &[i64]| {
        t.read(|c| crate::models::message::mentionees_in_room(c, pets, user_ids)).into_iter().map(|u| u.id).collect::<Vec<_>>()
    };

    assert_eq!(mentionees(&[id("david")]), vec![id("david")]);
    assert_eq!(mentionees(&[id("david"), id("david")]), vec![id("david")]);
    // Kevin isn't in All Pets.
    assert!(mentionees(&[id("kevin")]).is_empty());
}

#[test]
fn message_body_is_indexed_and_searchable() {
    let t = TestDb::new();
    let mut message = create(&t, "designers", "david", "My hovercraft is full of eels", "earth");
    assert_eq!(search(&t, "designers", "eel"), vec![message.id]);

    let m = message.clone();
    message = t.write(move |tx| {
        let mut m = m;
        m.update_body(tx, "My hovercraft is full of sharks")?;
        Ok(m)
    });
    assert_eq!(search(&t, "designers", "sharks"), vec![message.id]);

    t.write(move |tx| message.destroy(tx));
    assert!(search(&t, "designers", "sharks").is_empty());
}

#[test]
fn search_words_are_never_query_syntax() {
    let t = TestDb::new();
    let message = create(&t, "designers", "jason", "Do NOT feed the eel OR the shark", "c1");
    for query in ["NOT", "OR", "AND", "NEAR", "eel NOT", "NOT eel", "shark OR", "\"", "*", "", "a\0b", "\0"] {
        search(&t, "designers", query);
    }
    assert_eq!(search(&t, "designers", "NOT eel"), vec![message.id]);
    assert!(search(&t, "designers", "eel dolphin").is_empty());
    assert_eq!(search(&t, "designers", "eel\0shark"), vec![message.id]);
}

#[test]
fn search_results_are_returned_in_message_order() {
    let t = TestDb::new();
    let ids: Vec<i64> = ["first cat", "second cat", "third cat", "cat cat cat"]
        .into_iter()
        .map(|body| {
            t.travel(1);
            create(&t, "designers", "david", body, body).id
        })
        .collect();
    assert_eq!(search(&t, "designers", "cat"), ids);
}

#[test]
fn rich_text_body_is_converted_to_plain_text_for_indexing() {
    let t = TestDb::new();
    let message = create(&t, "designers", "david", "<span>My hovercraft is full of eels</span>", "earth");
    assert!(search(&t, "designers", "span").is_empty());
    assert_eq!(search(&t, "designers", "eel"), vec![message.id]);
}

#[test]
fn search_reachable_only_finds_messages_in_the_users_rooms() {
    let t = TestDb::new();
    let message = create(&t, "designers", "david", "hovercraft", "earth");
    let found = t.read(|c| Message::search_reachable(c, id("kevin"), "hovercraft"));
    assert_eq!(found.iter().map(|m| m.id).collect::<Vec<_>>(), vec![message.id]);
    assert!(t.read(|c| Message::search_reachable(c, id("bender"), "hovercraft")).is_empty());
}

#[test]
fn creating_a_blank_message_with_attachment_uses_filename_as_plain_text_body() {
    let t = TestDb::new();
    let message = t.write(|tx| {
        let blob = Blob::create(
            tx,
            &Blob {
                id: 0,
                key: "abc123".into(),
                filename: "moon.jpg".into(),
                content_type: Some("image/jpeg".into()),
                metadata: Some(r#"{"identified":true}"#.into()),
                service_name: "local".into(),
                byte_size: 1,
                checksum: None,
                created_at: tx.now(),
            },
        )?;
        Message::create(
            tx,
            NewMessage {
                room_id: id("hq"),
                creator_id: id("david"),
                client_message_id: Some("message".into()),
                body: None,
                attachment_blob_id: Some(blob.id),
            },
        )
    });
    assert_eq!(t.read(|c| message.plain_text_body(c, &BasicRichText)), "moon.jpg");
    assert_eq!(t.read(|c| message.content_type(c, &BasicRichText)), crate::ContentType::Attachment);
    assert_eq!(search(&t, "hq", "moon"), vec![message.id]);

    t.write(move |tx| message.destroy(tx));
    assert!(t.events().iter().any(|e| matches!(e, Event::PurgeBlob { .. })));
    assert_eq!(t.read(|c| crate::sql::count(c, "SELECT COUNT(*) FROM active_storage_attachments", [])), 0);
}

#[test]
fn sound_messages() {
    let t = TestDb::new();
    let message = create(&t, "designers", "david", "/play trombone", "x");
    assert_eq!(t.read(|c| message.content_type(c, &BasicRichText)), crate::ContentType::Sound);
    assert_eq!(t.read(|c| message.sound(c, &BasicRichText)).unwrap().name, "trombone");
    let message = create(&t, "designers", "david", "/play nosuchsound", "y");
    assert_eq!(t.read(|c| message.content_type(c, &BasicRichText)), crate::ContentType::Text);
}

/// `create_message` answers with what `Message::create` returns, without reading it back, so that
/// has to be the row as stored: after the commit's hooks, at the microseconds the columns keep.
#[test]
fn create_returns_the_row_as_stored() {
    let t = TestDb::new();
    let blob_id = |key: &str| {
        let blob = Blob {
            id: 0,
            key: key.into(),
            filename: "moon.jpg".into(),
            content_type: Some("image/jpeg".into()),
            metadata: None,
            service_name: "local".into(),
            byte_size: 1,
            checksum: None,
            created_at: t.now(),
        };
        t.write(move |tx| Ok(Blob::create(tx, &blob)?.id))
    };
    let new = |body: Option<&str>, attachment_blob_id: Option<i64>, client_message_id: Option<&str>| NewMessage {
        room_id: id("designers"),
        creator_id: id("david"),
        client_message_id: client_message_id.map(Into::into),
        body: body.map(Into::into),
        attachment_blob_id,
    };
    let cases = [
        new(Some("<p>Hello <strong>there</strong></p>"), None, Some("text")),
        new(Some(""), None, Some("empty body")),
        new(None, None, Some("nothing")),
        new(None, Some(blob_id("attachment")), Some("attachment")),
        new(Some("With a picture"), Some(blob_id("both")), Some("both")),
        new(Some("no client id"), None, None),
    ];
    for (n, attributes) in cases.into_iter().enumerate() {
        if n == 1 {
            // A frozen clock with nanoseconds, which the columns don't keep.
            t.clock.travel_to(jiff::Timestamp::new(1_900_000_000, 123_456_789).unwrap());
        }
        let created = t.write(move |tx| Message::create(tx, attributes));
        let id = created.id;
        assert_eq!(created, t.read(|c| Message::find(c, id)), "case {n}");
    }
}

#[test]
fn client_message_id_defaults_to_a_uuid() {
    let t = TestDb::new();
    let message = t.write(|tx| {
        Message::create(tx, NewMessage { room_id: id("hq"), creator_id: id("bender"), body: Some("beep".into()), ..Default::default() })
    });
    assert_eq!(message.client_message_id.len(), 36);
}

#[test]
fn pagination() {
    let t = TestDb::new();
    let watercooler = id("watercooler");
    let all = t.read(|c| Message::last_page(c, watercooler));
    assert_eq!(all.len(), 10, "watercooler fixtures");
    assert!(all.windows(2).all(|w| w[0].created_at <= w[1].created_at));
    assert!(!t.read(|c| Message::paged(c, watercooler)));

    let middle = all[5].clone();
    let before = t.read(|c| Message::page_before(c, watercooler, &middle));
    let after = t.read(|c| Message::page_after(c, watercooler, &middle));
    assert_eq!(before, all[..5]);
    assert_eq!(after, all[6..]);
    let around = t.read(|c| Message::page_around(c, watercooler, &middle));
    assert_eq!(around, all);
    assert!(t.read(|c| Message::exists_before(c, watercooler, &middle)));
    assert!(!t.read(|c| Message::exists_after(c, watercooler, &all[9])));

    for n in 0..PAGE_SIZE {
        t.travel(1);
        create(&t, "watercooler", "david", &format!("message {n}"), &format!("c{n}"));
    }
    assert!(t.read(|c| Message::paged(c, watercooler)));
    let last = t.read(|c| Message::last_page(c, watercooler));
    assert_eq!(last.len() as i64, PAGE_SIZE);
    assert_eq!(t.read(|c| Message::first_page(c, watercooler))[0], all[0]);

    let since: Timestamp = all[9].created_at;
    let created = t.read(|c| Message::page_created_since(c, watercooler, since));
    assert_eq!(created.len() as i64, PAGE_SIZE);
    let created_ids: Vec<i64> = created.iter().map(|m| m.id).collect();
    let updated = t.read(|c| Message::page_updated_since(c, watercooler, since, &created_ids));
    assert!(updated.iter().all(|m| !created_ids.contains(&m.id)));
}
