//! The broadcasts' streams, targets and markup, as delivered to subscribers.
use serde_json::{Value, json};

use super::support::*;
use crate::channels::{room_gid, user_gid};

/// The `<turbo-stream>` a delivery frame carries.
fn turbo_stream(frame: &str) -> String {
    let frame: Value = serde_json::from_str(frame).unwrap();
    frame["message"].as_str().expect("a Turbo Stream string").to_string()
}

async fn room_messages(app: &TestApp, client: &mut Client, room: &str) -> String {
    let room = app.room(room).await;
    let signed = app.signed_stream_name(&[&room_gid(&room).to_param(), "messages"]);
    let channel = identifier(json!({ "channel": "RoomMessagesChannel", "signed_stream_name": signed }));
    client.confirm(&channel).await;
    channel
}

async fn turbo(app: &TestApp, client: &mut Client, streamables: &[&str]) -> String {
    let channel = identifier(json!({ "channel": "Turbo::StreamsChannel", "signed_stream_name": app.signed_stream_name(streamables) }));
    client.confirm(&channel).await;
    channel
}

#[tokio::test]
async fn message_broadcasts() {
    let app = start().await;
    let mut kevin = app.connect("kevin").await;
    room_messages(&app, &mut kevin, "designers").await;
    let unreads = identifier(json!({ "channel": "UnreadRoomsChannel" }));
    kevin.confirm(&unreads).await;

    let designers = app.room("designers").await;
    let message = app.message("second").await;

    let (broadcasts, room, m) = (app.broadcasts.clone(), designers.clone(), message.clone());
    app.db.read(move |conn| broadcasts.message_create(conn, &room, &m, &FakePartials)).await.unwrap();
    assert_eq!(
        turbo_stream(&kevin.next_text().await),
        format!(
            r#"<turbo-stream action="append" target="messages_rooms_closed_{}"><template><div id="message_0002">message {}</div></template></turbo-stream>"#,
            designers.id, message.id
        )
    );
    assert_eq!(kevin.next_text().await, delivery(&unreads, &format!(r#"{{"roomId":{}}}"#, designers.id)));

    app.broadcasts.message_replace(&designers, &message, &FakePartials);
    assert_eq!(
        turbo_stream(&kevin.next_text().await),
        format!(
            r#"<turbo-stream maintain_scroll="true" action="replace" target="presentation_message_0002"><template><div>presentation {} & more</div></template></turbo-stream>"#,
            message.id
        )
    );

    app.broadcasts.message_remove(&designers, &message);
    assert_eq!(turbo_stream(&kevin.next_text().await), r#"<turbo-stream action="remove" target="message_0002"></turbo-stream>"#);
    kevin.assert_silent().await;
}

#[tokio::test]
async fn boost_broadcasts() {
    let app = start().await;
    let mut kevin = app.connect("kevin").await;
    room_messages(&app, &mut kevin, "designers").await;
    let designers = app.room("designers").await;
    let message = app.message("first").await;
    let boost = app.boost("first").await;

    app.broadcasts.boost_create(&designers, &message, &boost, &FakePartials);
    assert_eq!(
        turbo_stream(&kevin.next_text().await),
        format!(
            r#"<turbo-stream maintain_scroll="true" action="append" target="boosts_message_0001"><template><div>boost {}</div></template></turbo-stream>"#,
            boost.id
        )
    );

    app.broadcasts.boost_remove(&designers, &boost);
    assert_eq!(
        turbo_stream(&kevin.next_text().await),
        format!(r#"<turbo-stream action="remove" target="boost_{}"></turbo-stream>"#, boost.id)
    );
}

#[tokio::test]
async fn room_list_broadcasts() {
    let app = start().await;
    let mut jz = app.connect("jz").await;
    turbo(&app, &mut jz, &["rooms"]).await;
    let own_rooms = user_gid(id("jz")).to_param();
    turbo(&app, &mut jz, &[&own_rooms, "rooms"]).await;

    let hq = app.room("hq").await;
    app.broadcasts.open_room_create(&hq, &FakePartials);
    assert_eq!(
        turbo_stream(&jz.next_text().await),
        format!(r#"<turbo-stream action="prepend" target="shared_rooms"><template><li>shared {}</li></template></turbo-stream>"#, hq.id)
    );

    app.broadcasts.open_room_update(&hq, &FakePartials);
    assert_eq!(
        turbo_stream(&jz.next_text().await),
        format!(
            r#"<turbo-stream action="replace" target="list_rooms_open_{0}"><template><li>shared {0}</li></template></turbo-stream>"#,
            hq.id
        )
    );

    app.broadcasts.room_remove(&hq);
    assert_eq!(
        turbo_stream(&jz.next_text().await),
        format!(r#"<turbo-stream action="remove" target="list_rooms_open_{}"></turbo-stream>"#, hq.id)
    );

    // Closed rooms go to each member's own stream: jz is in designers, not the watercooler.
    let designers = app.room("designers").await;
    let watercooler = app.room("watercooler").await;
    let broadcasts = app.broadcasts.clone();
    let (d, w) = (designers.clone(), watercooler.clone());
    app.db
        .read(move |conn| {
            broadcasts.closed_room_create(conn, &w, &FakePartials)?;
            broadcasts.closed_room_create(conn, &d, &FakePartials)?;
            broadcasts.closed_room_update(conn, &d, &FakePartials)
        })
        .await
        .unwrap();
    assert_eq!(
        turbo_stream(&jz.next_text().await),
        format!(
            r#"<turbo-stream action="prepend" target="shared_rooms"><template><li>shared {}</li></template></turbo-stream>"#,
            designers.id
        )
    );
    assert_eq!(
        turbo_stream(&jz.next_text().await),
        format!(
            r#"<turbo-stream action="replace" target="list_rooms_closed_{0}"><template><li>shared {0}</li></template></turbo-stream>"#,
            designers.id
        )
    );
    jz.assert_silent().await;
}

#[tokio::test]
async fn direct_room_and_involvement_broadcasts() {
    let app = start().await;
    let mut kevin = app.connect("kevin").await;
    let own_rooms = user_gid(id("kevin")).to_param();
    turbo(&app, &mut kevin, &[&own_rooms, "rooms"]).await;

    let direct = app.room("bender_and_kevin").await;
    let broadcasts = app.broadcasts.clone();
    let room = direct.clone();
    app.db.read(move |conn| broadcasts.direct_room_create(conn, &room, &FakePartials)).await.unwrap();
    let membership = app.membership("bender_and_kevin", "kevin").await.unwrap();
    assert_eq!(
        turbo_stream(&kevin.next_text().await),
        format!(
            r#"<turbo-stream action="prepend" target="direct_rooms"><template><li>direct {}</li></template></turbo-stream>"#,
            membership.id
        )
    );
    kevin.assert_silent().await;

    use campfire_db::Involvement::*;
    let designers = app.room("designers").await;
    let mut membership = app.membership("designers", "kevin").await.unwrap();

    // Direct rooms never change the list.
    app.broadcasts.involvement_change(&direct, &membership, Some(Invisible), &FakePartials).unwrap();
    kevin.assert_silent().await;

    membership.involvement = Some(Invisible);
    app.broadcasts.involvement_change(&designers, &membership, Some(Mentions), &FakePartials).unwrap();
    assert_eq!(
        turbo_stream(&kevin.next_text().await),
        format!(r#"<turbo-stream action="remove" target="list_rooms_closed_{}"></turbo-stream>"#, designers.id)
    );

    membership.involvement = Some(Everything);
    app.broadcasts.involvement_change(&designers, &membership, Some(Invisible), &FakePartials).unwrap();
    assert_eq!(
        turbo_stream(&kevin.next_text().await),
        format!(
            r#"<turbo-stream action="prepend" target="shared_rooms"><template><li>shared {}</li></template></turbo-stream>"#,
            designers.id
        )
    );

    app.broadcasts.involvement_change(&designers, &membership, Some(Mentions), &FakePartials).unwrap();
    kevin.assert_silent().await;
    assert!(app.broadcasts.involvement_change(&designers, &membership, None, &FakePartials).is_err());
}

#[tokio::test]
async fn broadcast_frames_use_active_support_json_escaping() {
    let app = start().await;
    let mut kevin = app.connect("kevin").await;
    let channel = room_messages(&app, &mut kevin, "designers").await;
    let designers = app.room("designers").await;
    let message = app.message("first").await;
    app.broadcasts.message_replace(&designers, &message, &FakePartials);
    assert_eq!(
        kevin.next_text().await,
        delivery(
            &channel,
            &html_json(&format!(
                r#"<turbo-stream maintain_scroll="true" action="replace" target="presentation_message_0001"><template><div>presentation {} & more</div></template></turbo-stream>"#,
                message.id
            ))
        )
    );
}
