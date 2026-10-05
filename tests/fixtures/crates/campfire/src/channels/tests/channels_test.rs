//! Ports of reference/test/channels/**, plus the channels the reference doesn't test directly.
use campfire_db::Session;
use serde_json::json;

use super::support::*;
use crate::channels::{room_gid, user_gid};

// ApplicationCable::Connection

#[tokio::test]
async fn connects_with_a_valid_session_cookie() {
    let app = start().await;
    let mut client = app.connect("david").await;
    client.confirm(&identifier(json!({ "channel": "HeartbeatChannel" }))).await;
}

#[tokio::test]
async fn rejects_a_connection_without_or_with_a_bad_session_cookie() {
    let app = start().await;
    let unauthorized = r#"{"type":"disconnect","reason":"unauthorized","reconnect":false}"#;

    for cookie in [None, Some(app.cookie_with_token("-1")), Some("session_token=forged--0000".to_string())] {
        let mut client = app.connect_with_cookie(cookie.as_deref()).await;
        assert_eq!(client.until_closed().await, vec![unauthorized.to_string()]);
    }
}

#[tokio::test]
async fn rejects_a_session_cookie_once_it_expires() {
    let app = start().await;
    let user_id = id("david");
    let session = app.db.write(move |tx| Session::start(tx, user_id, Some("test"), Some("8.8.8.8"))).await.unwrap();
    let expires_at = app.now().jiff() + jiff::SignedDuration::from_secs(60);
    let cookie = app.cookie_with_token_expiring(&session.token, Some(expires_at));

    let mut client = app.connect_with_cookie(Some(&cookie)).await;
    assert_eq!(client.next_text().await, r#"{"type":"welcome"}"#);

    app.clock.travel(jiff::SignedDuration::from_secs(61));
    let mut client = app.connect_with_cookie(Some(&cookie)).await;
    assert_eq!(client.until_closed().await, vec![r#"{"type":"disconnect","reason":"unauthorized","reconnect":false}"#.to_string()]);
}

// HeartbeatChannel and ApplicationCable::Channel

#[tokio::test]
async fn heartbeat_and_base_channels_confirm() {
    let app = start().await;
    let mut client = app.connect("jz").await;
    client.confirm(&identifier(json!({ "channel": "HeartbeatChannel" }))).await;
    client.confirm(&identifier(json!({ "channel": "ApplicationCable::Channel" }))).await;
    client.assert_silent().await;
}

// RoomChannel

#[tokio::test]
async fn room_channel_streams_for_member_rooms_only() {
    let app = start().await;
    let mut client = app.connect("kevin").await;
    let designers = app.room("designers").await;

    let member = room_identifier("RoomChannel", designers.id);
    client.confirm(&member).await;
    client.reject(&room_identifier("RoomChannel", id("watercooler"))).await;
    client.reject(&room_identifier("RoomChannel", -1)).await;
    client.reject(&identifier(json!({ "channel": "RoomChannel" }))).await;
    // Params are cast like Active Record casts an id.
    let by_string = identifier(json!({ "channel": "RoomChannel", "room_id": designers.id.to_string() }));
    client.confirm(&by_string).await;

    // Both subscriptions stream the room; a connection polls its subscriptions in no set order.
    let stream = format!("room:{}", room_gid(&designers).to_param());
    app.server.broadcast(&stream, &json!({ "hello": 1 }));
    let mut received = vec![client.next_text().await, client.next_text().await];
    received.sort();
    let mut expected = vec![delivery(&member, r#"{"hello":1}"#), delivery(&by_string, r#"{"hello":1}"#)];
    expected.sort();
    assert_eq!(received, expected);
    client.assert_silent().await;
}

// PresenceChannel (reference/test/channels/presence_channel_test.rb)

#[tokio::test]
async fn presence_subscribes_and_marks_the_membership_connected() {
    let app = start().await;
    let mut client = app.connect("david").await;
    let reads = identifier(json!({ "channel": "ReadRoomsChannel" }));
    client.confirm(&reads).await;

    let membership = app.membership("designers", "david").await.unwrap();
    assert!(!membership.is_connected(app.now()));
    // A member's unread room gets cleared by `present`.
    app.db
        .write(move |tx| {
            tx.conn().execute("UPDATE memberships SET unread_at = '2024-01-01 00:00:00' WHERE id = ?", [membership.id]).map_err(Into::into)
        })
        .await
        .unwrap();

    let presence = room_identifier("PresenceChannel", id("designers"));
    client.confirm(&presence).await;
    assert_eq!(client.next_text().await, delivery(&reads, &format!(r#"{{"room_id":{}}}"#, id("designers"))));

    let membership = app.membership("designers", "david").await.unwrap();
    assert!(membership.is_connected(app.now()));
    assert_eq!(membership.connections, 1);
    assert_eq!(membership.unread_at, None);

    client.unsubscribe(&presence).await;
    eventually(|| async { !app.membership("designers", "david").await.unwrap().is_connected(app.now()) }).await;
    let membership = app.membership("designers", "david").await.unwrap();
    assert_eq!((membership.connections, membership.connected_at), (0, None));
}

#[tokio::test]
async fn presence_counts_connections_and_refreshes() {
    let app = start().await;
    let presence = room_identifier("PresenceChannel", id("designers"));
    let mut first = app.connect("david").await;
    let mut second = app.connect("david").await;
    first.confirm(&presence).await;
    second.confirm(&presence).await;
    assert_eq!(app.membership("designers", "david").await.unwrap().connections, 2);

    // `refresh` reconnects a membership that timed out.
    app.clock.travel(jiff::SignedDuration::from_secs(61));
    assert!(!app.membership("designers", "david").await.unwrap().is_connected(app.now()));
    first.perform(&presence, json!({ "action": "refresh" })).await;
    eventually(|| async { app.membership("designers", "david").await.unwrap().is_connected(app.now()) }).await;
    assert_eq!(app.membership("designers", "david").await.unwrap().connections, 1);

    second.unsubscribe(&presence).await;
    eventually(|| async { app.membership("designers", "david").await.unwrap().connected_at.is_none() }).await;
}

#[tokio::test]
async fn presence_rejects_rooms_the_user_is_not_in_without_touching_memberships() {
    let app = start().await;
    let mut client = app.connect("david").await;
    let before = app.membership("bender_and_kevin", "kevin").await.unwrap();

    client.reject(&room_identifier("PresenceChannel", id("bender_and_kevin"))).await;
    client.reject(&room_identifier("PresenceChannel", -1)).await;
    client.assert_silent().await;
    assert_eq!(app.membership("bender_and_kevin", "kevin").await.unwrap(), before);
}

// ReadRoomsChannel and UnreadRoomsChannel (reference/test/channels/unread_rooms_channel_test.rb)

#[tokio::test]
async fn unread_rooms_streams_only_the_subscribers_own_stream() {
    let app = start().await;
    let direct = app.room("bender_and_kevin").await;
    let unreads = identifier(json!({ "channel": "UnreadRoomsChannel" }));

    let mut outsider = app.connect("jz").await;
    let mut member = app.connect("kevin").await;
    outsider.confirm(&unreads).await;
    member.confirm(&unreads).await;

    let broadcasts = app.broadcasts.clone();
    let message = app.message("first").await;
    let room = direct.clone();
    app.db.read(move |conn| broadcasts.message_create(conn, &room, &message, &FakePartials)).await.unwrap();

    assert_eq!(member.next_text().await, delivery(&unreads, &format!(r#"{{"roomId":{}}}"#, direct.id)));
    member.assert_silent().await;
    outsider.assert_silent().await;
}

#[tokio::test]
async fn read_rooms_streams_the_users_reads() {
    let app = start().await;
    let reads = identifier(json!({ "channel": "ReadRoomsChannel" }));
    let mut client = app.connect("jason").await;
    client.confirm(&reads).await;
    crate::channels::broadcasts::read_room(&app.server, id("jason"), 7);
    crate::channels::broadcasts::read_room(&app.server, id("david"), 8);
    assert_eq!(client.next_text().await, delivery(&reads, r#"{"room_id":7}"#));
    client.assert_silent().await;
}

// TypingNotificationsChannel

#[tokio::test]
async fn typing_notifications_broadcast_start_and_stop_to_the_room() {
    let app = start().await;
    let typing = room_identifier("TypingNotificationsChannel", id("designers"));
    let mut typist = app.connect("jz").await;
    let mut reader = app.connect("kevin").await;
    typist.confirm(&typing).await;
    reader.confirm(&typing).await;

    typist.perform(&typing, json!({ "action": "start" })).await;
    let start = format!(r#"{{"action":"start","user":{{"id":{},"name":"JZ"}}}}"#, id("jz"));
    assert_eq!(reader.next_text().await, delivery(&typing, &start));
    assert_eq!(typist.next_text().await, delivery(&typing, &start));

    typist.perform(&typing, json!({ "action": "stop" })).await;
    let stop = format!(r#"{{"action":"stop","user":{{"id":{},"name":"JZ"}}}}"#, id("jz"));
    assert_eq!(reader.next_text().await, delivery(&typing, &stop));

    // Not an action: nothing is sent.
    typist.perform(&typing, json!({ "action": "dance" })).await;
    reader.assert_silent().await;

    let designers = app.room("designers").await;
    let stream = format!("typing_notifications:{}", room_gid(&designers).to_param());
    assert_eq!(typing_stream_name(&designers), stream);
}

fn typing_stream_name(room: &campfire_db::Room) -> String {
    campfire_cable::naming::broadcasting_for("TypingNotificationsChannel", &[&room_gid(room).to_param()])
}

/// `safe_constantize` resolves "::TypingNotificationsChannel" to the class, whose broadcastings
/// are named after the class alone, so both spellings share the room's typing stream.
#[tokio::test]
async fn typing_notifications_reach_the_room_however_the_channel_is_spelled() {
    let app = start().await;
    let prefixed = room_identifier("::TypingNotificationsChannel", id("designers"));
    let plain = room_identifier("TypingNotificationsChannel", id("designers"));
    let mut typist = app.connect("jz").await;
    let mut reader = app.connect("kevin").await;
    typist.confirm(&prefixed).await;
    reader.confirm(&plain).await;

    typist.perform(&prefixed, json!({ "action": "start" })).await;
    let start = format!(r#"{{"action":"start","user":{{"id":{},"name":"JZ"}}}}"#, id("jz"));
    assert_eq!(reader.next_text().await, delivery(&plain, &start));
    assert_eq!(typist.next_text().await, delivery(&prefixed, &start));

    reader.perform(&plain, json!({ "action": "stop" })).await;
    let stop = format!(r#"{{"action":"stop","user":{{"id":{},"name":"Kevin"}}}}"#, id("kevin"));
    assert_eq!(typist.next_text().await, delivery(&prefixed, &stop));
    assert_eq!(reader.next_text().await, delivery(&plain, &stop));
}

/// A subscription whose `subscribed` failed has no room: typing there is an error, not a panic
/// that takes the whole connection down.
#[tokio::test]
async fn typing_on_a_failed_subscription_leaves_the_connection_up() {
    let app = start().await;
    let rename = |from: &'static str, to: &'static str| {
        app.db.write(move |tx| tx.conn().execute_batch(&format!("ALTER TABLE {from} RENAME TO {to}")).map_err(Into::into))
    };
    let mut client = app.connect("jz").await;
    let typing = room_identifier("TypingNotificationsChannel", id("designers"));

    rename("rooms", "rooms_away").await.unwrap();
    client.subscribe(&typing).await;
    client.confirm(&identifier(json!({ "channel": "HeartbeatChannel" }))).await;
    rename("rooms_away", "rooms").await.unwrap();

    client.perform(&typing, json!({ "action": "start" })).await;
    client.confirm(&identifier(json!({ "channel": "ApplicationCable::Channel" }))).await;
}

#[tokio::test]
async fn typing_notifications_reject_non_members() {
    let app = start().await;
    let mut client = app.connect("jz").await;
    client.reject(&room_identifier("TypingNotificationsChannel", id("watercooler"))).await;
}

// RoomMessagesChannel (reference/test/channels/room_messages_channel_test.rb)

#[tokio::test]
async fn a_member_may_subscribe_to_a_rooms_message_stream() {
    let app = start().await;
    let designers = app.room("designers").await;
    let signed = app.signed_stream_name(&[&room_gid(&designers).to_param(), "messages"]);
    let channel = identifier(json!({ "channel": "RoomMessagesChannel", "signed_stream_name": signed }));

    let mut kevin = app.connect("kevin").await;
    kevin.confirm(&channel).await;

    let message = app.message("first").await;
    app.broadcasts.message_remove(&designers, &message);
    let frame = kevin.next_text().await;
    assert_eq!(frame, delivery(&channel, &html_json(r#"<turbo-stream action="remove" target="message_0001"></turbo-stream>"#)));
}

#[tokio::test]
async fn room_message_streams_are_rejected_for_everyone_else() {
    let app = start().await;
    let designers = app.room("designers").await;
    let hq = app.room("hq").await;
    let signed = app.signed_stream_name(&[&room_gid(&designers).to_param(), "messages"]);
    let subscribe = |signed: serde_json::Value| identifier(json!({ "channel": "RoomMessagesChannel", "signed_stream_name": signed }));

    // A user who was never a member.
    let mut bender = app.connect("bender").await;
    bender.reject(&subscribe(json!(signed))).await;
    // Another room the user isn't in.
    bender.reject(&subscribe(json!(app.signed_stream_name(&[&room_gid(&hq).to_param(), "messages"])))).await;

    let mut kevin = app.connect("kevin").await;
    // An unsigned stream name.
    kevin.reject(&subscribe(json!(format!("{}:messages", room_gid(&designers).to_param())))).await;
    // A missing stream name.
    kevin.reject(&identifier(json!({ "channel": "RoomMessagesChannel" }))).await;
    // A signed name that isn't a room's message stream.
    kevin.reject(&subscribe(json!(app.signed_stream_name(&["rooms"])))).await;
    // A room whose type changed since the name was signed (`Rooms::Open.find` of a closed room).
    let stale = campfire_db::Room { room_type: campfire_db::RoomType::Open, ..designers.clone() };
    kevin.reject(&subscribe(json!(app.signed_stream_name(&[&room_gid(&stale).to_param(), "messages"])))).await;
    // A user GID in place of a room.
    kevin.reject(&subscribe(json!(app.signed_stream_name(&[&user_gid(id("kevin")).to_param(), "messages"])))).await;
}

#[tokio::test]
async fn a_revoked_member_may_not_resubscribe_with_a_harvested_stream_name() {
    let app = start().await;
    let designers = app.room("designers").await;
    let signed = app.signed_stream_name(&[&room_gid(&designers).to_param(), "messages"]);
    let channel = identifier(json!({ "channel": "RoomMessagesChannel", "signed_stream_name": signed }));

    let mut kevin = app.connect("kevin").await;
    kevin.confirm(&channel).await;

    let room = designers.clone();
    app.db.write(move |tx| room.revoke_from(tx, &[id("kevin")])).await.unwrap();
    kevin.until_closed().await;

    let mut kevin = app.connect("kevin").await;
    kevin.reject(&channel).await;
}

// Turbo::StreamsChannel with RoomStreamsAreAuthorized

#[tokio::test]
async fn the_stock_turbo_channel_refuses_room_message_streams_but_serves_the_room_list() {
    let app = start().await;
    let designers = app.room("designers").await;
    let turbo = |signed: String| identifier(json!({ "channel": "Turbo::StreamsChannel", "signed_stream_name": signed }));
    let mut kevin = app.connect("kevin").await;

    kevin.reject(&turbo(app.signed_stream_name(&[&room_gid(&designers).to_param(), "messages"]))).await;
    kevin.reject(&turbo("forged--0000".into())).await;
    kevin.reject(&identifier(json!({ "channel": "Turbo::StreamsChannel" }))).await;

    let rooms = turbo(app.signed_stream_name(&["rooms"]));
    kevin.confirm(&rooms).await;
    app.broadcasts.room_remove(&designers);
    assert_eq!(
        kevin.next_text().await,
        delivery(
            &rooms,
            &html_json(&format!(r#"<turbo-stream action="remove" target="list_rooms_closed_{}"></turbo-stream>"#, designers.id))
        )
    );
}

// Public `subscribed` is an action in Ruby: performing it streams again.

#[tokio::test]
async fn performing_subscribed_streams_twice_like_ruby() {
    let app = start().await;
    let unreads = identifier(json!({ "channel": "UnreadRoomsChannel" }));
    let mut client = app.connect("kevin").await;
    client.confirm(&unreads).await;
    client.perform(&unreads, json!({ "action": "subscribed" })).await;
    client.assert_silent().await;

    let broadcasts = app.broadcasts.clone();
    let room = app.room("bender_and_kevin").await;
    app.db.read(move |conn| broadcasts.unread_room(conn, &room)).await.unwrap();
    let expected = delivery(&unreads, &format!(r#"{{"roomId":{}}}"#, id("bender_and_kevin")));
    assert_eq!(client.next_text().await, expected);
    assert_eq!(client.next_text().await, expected);
}
