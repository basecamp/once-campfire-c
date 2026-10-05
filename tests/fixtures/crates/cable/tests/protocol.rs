//! The Action Cable protocol, driven over a real socket.
mod support;

use std::time::Duration;

use campfire_cable::Config;
use serde_json::{Value, json};
use support::{Frame, identifier, start, test_config};

const WELCOME: &str = r#"{"type":"welcome"}"#;

fn room(id: u64) -> String {
    identifier(json!({ "channel": "RoomChannel", "room_id": id }))
}

fn confirm(identifier: &str) -> String {
    format!(r#"{{"identifier":{},"type":"confirm_subscription"}}"#, Value::String(identifier.into()))
}

fn reject(identifier: &str) -> String {
    format!(r#"{{"identifier":{},"type":"reject_subscription"}}"#, Value::String(identifier.into()))
}

fn message(identifier: &str, message: &str) -> String {
    format!(r#"{{"identifier":{},"message":{message}}}"#, Value::String(identifier.into()))
}

#[tokio::test]
async fn non_websocket_requests_get_rails_404() {
    let app = start(test_config()).await;
    let url = app.url.replace("ws://", "http://");
    let (status, content_type, body) = http_get(&url, &[]).await;
    assert_eq!(status, 404);
    assert_eq!(content_type.as_deref(), Some("text/plain; charset=utf-8"));
    assert_eq!(body, "Page not found");
}

#[tokio::test]
async fn cross_origin_upgrades_get_rails_404() {
    let app = start(test_config()).await;
    let mut request = tokio_tungstenite::tungstenite::client::IntoClientRequest::into_client_request(app.url.as_str()).unwrap();
    request.headers_mut().insert("origin", "http://evil.example".parse().unwrap());
    request.headers_mut().insert("cookie", "session_token=1".parse().unwrap());
    let error = tokio_tungstenite::connect_async(request).await.unwrap_err();
    match error {
        tokio_tungstenite::tungstenite::Error::Http(response) => assert_eq!(response.status(), 404),
        other => panic!("expected an HTTP 404, got {other}"),
    }
}

#[tokio::test]
async fn same_origin_under_assume_ssl_means_https() {
    let app = start(Config::default()).await;
    let error = tokio_tungstenite::connect_async({
        let mut request = tokio_tungstenite::tungstenite::client::IntoClientRequest::into_client_request(app.url.as_str()).unwrap();
        request.headers_mut().insert("origin", app.origin.parse().unwrap());
        request
    })
    .await;
    assert!(error.is_err(), "http:// origin must be refused when SSL is assumed");

    let https_origin = app.origin.replace("http://", "https://");
    let mut client = app.connect_with(Some(1), &https_origin).await;
    assert_eq!(client.next_text().await, WELCOME);
}

#[tokio::test]
async fn negotiates_the_actioncable_subprotocol_and_welcomes() {
    let app = start(test_config()).await;
    let mut client = app.connect(1).await;
    assert_eq!(client.protocol.as_deref(), Some("actioncable-v1-json"));
    assert_eq!(client.next_text().await, WELCOME);
}

#[tokio::test]
async fn unauthorized_connections_are_told_not_to_reconnect_and_closed() {
    let app = start(test_config()).await;
    let mut client = app.connect_with(None, &app.origin).await;
    assert_eq!(client.next_text().await, r#"{"type":"disconnect","reason":"unauthorized","reconnect":false}"#);
    assert_eq!(client.next().await, Frame::Close(Some((1000, String::new()))));
}

#[tokio::test]
async fn subscribe_confirms_and_duplicates_are_ignored() {
    let app = start(test_config()).await;
    let mut client = app.connect(1).await;
    assert_eq!(client.next_text().await, WELCOME);

    let room = room(1);
    client.subscribe(&room).await;
    assert_eq!(client.next_text().await, confirm(&room));

    // Same identifier string: no reply at all.
    client.subscribe(&room).await;
    let heartbeat = identifier(json!({ "channel": "HeartbeatChannel" }));
    client.subscribe(&heartbeat).await;
    assert_eq!(client.next_text().await, confirm(&heartbeat));

    // A differently spelled identifier for the same room is a separate subscription.
    let respelled = r#"{"room_id":1,"channel":"RoomChannel"}"#;
    client.subscribe(respelled).await;
    assert_eq!(client.next_text().await, confirm(respelled));
}

#[tokio::test]
async fn rejection_runs_unsubscribed_and_can_be_retried() {
    let app = start(test_config()).await;
    let mut client = app.connect(1).await;
    client.next_text().await;

    let room = room(2);
    client.subscribe(&room).await;
    assert_eq!(client.next_text().await, reject(&room));
    assert_eq!(*app.log.lock().unwrap(), ["subscribed rejected=true", "unsubscribed rejected=true"]);

    // The rejected subscription was removed, so subscribing again is answered again.
    client.subscribe(&room).await;
    assert_eq!(client.next_text().await, reject(&room));
}

#[tokio::test]
async fn unknown_channels_and_malformed_commands_get_no_reply() {
    let app = start(test_config()).await;
    let mut client = app.connect(1).await;
    client.next_text().await;

    client.subscribe(&identifier(json!({ "channel": "NopeChannel" }))).await;
    client.subscribe("not json").await;
    client.send_raw("not json").await;
    client.send(json!({ "command": "dance" })).await;
    client.unsubscribe(&room(1)).await;
    client.perform(&room(1), json!({ "action": "echo" })).await;
    client.assert_silent().await;

    // The connection is still usable afterwards.
    let room = room(1);
    client.subscribe(&room).await;
    assert_eq!(client.next_text().await, confirm(&room));
}

#[tokio::test]
async fn leading_colons_resolve_like_safe_constantize() {
    let app = start(test_config()).await;
    let mut client = app.connect(1).await;
    client.next_text().await;
    let heartbeat = identifier(json!({ "channel": "::HeartbeatChannel" }));
    client.subscribe(&heartbeat).await;
    assert_eq!(client.next_text().await, confirm(&heartbeat));
}

/// A channel's broadcastings are named after its class, not after how the client spelled it.
#[tokio::test]
async fn leading_colons_stream_and_broadcast_under_the_class_name() {
    let app = start(test_config()).await;
    let mut prefixed = app.connect(1).await;
    let mut plain = app.connect(1).await;
    prefixed.next_text().await;
    plain.next_text().await;
    let prefixed_room = identifier(json!({ "channel": "::RoomChannel", "room_id": 1 }));
    prefixed.subscribe(&prefixed_room).await;
    assert_eq!(prefixed.next_text().await, confirm(&prefixed_room));
    let plain_room = room(1);
    plain.subscribe(&plain_room).await;
    assert_eq!(plain.next_text().await, confirm(&plain_room));

    assert_eq!(app.server.broadcast_to("RoomChannel", &["room-1"], &json!({ "roomId": 1 })), 2);
    assert_eq!(prefixed.next_text().await, message(&prefixed_room, r#"{"roomId":1}"#));
    assert_eq!(plain.next_text().await, message(&plain_room, r#"{"roomId":1}"#));

    prefixed.perform(&prefixed_room, json!({ "action": "start" })).await;
    let start = r#"{"action":"start","user":{"id":1}}"#;
    assert_eq!(plain.next_text().await, message(&plain_room, start));
    assert_eq!(prefixed.next_text().await, message(&prefixed_room, start));
}

#[tokio::test]
async fn broadcasts_reach_subscribers_as_escaped_json() {
    let app = start(test_config()).await;
    let mut client = app.connect(1).await;
    client.next_text().await;
    let room = room(1);
    client.subscribe(&room).await;
    client.next_text().await;

    assert_eq!(app.server.broadcast_to("RoomChannel", &["room-1"], &json!({ "roomId": 1 })), 1);
    assert_eq!(client.next_text().await, message(&room, r#"{"roomId":1}"#));

    app.server.broadcast("room:room-1", "<b>&</b>");
    assert_eq!(client.next_text().await, message(&room, r#""\u003cb\u003e\u0026\u003c/b\u003e""#));
}

#[tokio::test]
async fn perform_dispatches_actions_and_defaults_to_receive() {
    let app = start(test_config()).await;
    let mut client = app.connect(1).await;
    let mut other = app.connect(2).await;
    client.next_text().await;
    other.next_text().await;
    let room = room(1);
    client.subscribe(&room).await;
    other.subscribe(&room).await;
    client.next_text().await;
    other.next_text().await;

    client.perform(&room, json!({ "action": "echo", "text": "hi" })).await;
    assert_eq!(client.next_text().await, message(&room, r#"{"action":"echo","text":"hi"}"#));

    client.perform(&room, json!({ "text": "hi" })).await;
    assert_eq!(client.next_text().await, message(&room, r#"{"received":{"text":"hi"}}"#));

    client.perform(&room, json!({ "action": "start" })).await;
    let typing = message(&room, r#"{"action":"start","user":{"id":1}}"#);
    assert_eq!(client.next_text().await, typing);
    assert_eq!(other.next_text().await, typing);

    client.perform(&room, json!({ "action": "not_an_action" })).await;
    client.assert_silent().await;
}

#[tokio::test]
async fn unsubscribe_stops_delivery_silently() {
    let app = start(test_config()).await;
    let mut client = app.connect(1).await;
    client.next_text().await;
    let room = room(1);
    client.subscribe(&room).await;
    client.next_text().await;

    client.unsubscribe(&room).await;
    client.assert_silent().await;
    app.server.broadcast("room:room-1", &json!({}));
    client.assert_silent().await;
    assert_eq!(app.server.stream_count(), 1, "only the internal channel remains");
}

#[tokio::test]
async fn remote_disconnect_closes_every_connection_for_the_identifier() {
    let app = start(test_config()).await;
    let mut first = app.connect(1).await;
    let mut second = app.connect(1).await;
    let mut bystander = app.connect(2).await;
    for client in [&mut first, &mut second, &mut bystander] {
        client.next_text().await;
    }
    let room = room(1);
    first.subscribe(&room).await;
    first.next_text().await;

    assert_eq!(app.server.disconnect("user-1", true), 2);
    for client in [&mut first, &mut second] {
        assert_eq!(client.next_text().await, r#"{"type":"disconnect","reason":"remote","reconnect":true}"#);
        assert_eq!(client.next().await, Frame::Close(Some((1000, String::new()))));
    }
    bystander.assert_silent().await;

    // Once the client completes the close handshake, the server unsubscribes its channels.
    assert!(matches!(first.next().await, Frame::End | Frame::Error(_)));
    tokio::time::sleep(Duration::from_millis(50)).await;
    assert_eq!(app.log.lock().unwrap().last().unwrap(), "unsubscribed rejected=false");

    let mut again = app.connect(1).await;
    again.next_text().await;
    app.server.disconnect("user-1", false);
    assert_eq!(again.next_text().await, r#"{"type":"disconnect","reason":"remote","reconnect":false}"#);
}

#[tokio::test]
async fn restart_closes_with_server_restart() {
    let app = start(test_config()).await;
    let mut client = app.connect(1).await;
    client.next_text().await;
    app.server.restart();
    assert_eq!(client.next_text().await, r#"{"type":"disconnect","reason":"server_restart","reconnect":true}"#);
}

#[tokio::test]
async fn lagging_subscribers_are_disconnected_with_reconnect() {
    let app = start(Config { stream_capacity: 1, max_write_batch: 1, ..test_config() }).await;
    let mut client = app.connect(1).await;
    client.next_text().await;
    let room = room(1);
    client.subscribe(&room).await;
    client.next_text().await;

    for i in 0..10_000 {
        app.server.broadcast("room:room-1", &i);
    }
    let mut delivered = Vec::new();
    loop {
        let frame = client.next_text().await;
        if frame.contains(r#""type":"disconnect""#) {
            assert_eq!(frame, r#"{"type":"disconnect","reason":null,"reconnect":true}"#);
            break;
        }
        delivered.push(frame);
    }
    assert!(delivered.len() < 10_000);
}

#[tokio::test]
async fn pings_every_three_seconds_with_a_unix_timestamp() {
    let app = start(test_config()).await;
    let mut client = app.connect(1).await;
    assert_eq!(client.next_text().await, WELCOME);
    let started = std::time::Instant::now();
    let Frame::Text(ping) = tokio::time::timeout(Duration::from_secs(4), client.next_including_pings()).await.unwrap() else {
        panic!("expected a ping");
    };
    assert!(started.elapsed() <= Duration::from_millis(3100));
    let ping: Value = serde_json::from_str(&ping).unwrap();
    assert_eq!(ping.as_object().unwrap().keys().collect::<Vec<_>>(), ["type", "message"]);
    assert_eq!(ping["type"], "ping");
    let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_secs() as i64;
    assert!((ping["message"].as_i64().unwrap() - now).abs() <= 1);
}

#[tokio::test]
async fn turbo_streams_channel_verifies_and_guards_stream_names() {
    let app = start(test_config()).await;
    let mut client = app.connect(1).await;
    client.next_text().await;

    let rooms = identifier(json!({ "channel": "Turbo::StreamsChannel", "signed_stream_name": "signed(rooms)" }));
    client.subscribe(&rooms).await;
    assert_eq!(client.next_text().await, confirm(&rooms));

    app.server.broadcast_remove_to(&["rooms"], "list_room_1");
    assert_eq!(
        client.next_text().await,
        message(&rooms, r#""\u003cturbo-stream action=\"remove\" target=\"list_room_1\"\u003e\u003c/turbo-stream\u003e""#)
    );

    let forged = identifier(json!({ "channel": "Turbo::StreamsChannel", "signed_stream_name": "rooms" }));
    client.subscribe(&forged).await;
    assert_eq!(client.next_text().await, reject(&forged));

    let missing = identifier(json!({ "channel": "Turbo::StreamsChannel" }));
    client.subscribe(&missing).await;
    assert_eq!(client.next_text().await, reject(&missing));

    // RoomStreamsAreAuthorized: a validly signed room message stream is still turned away.
    let guarded = identifier(json!({ "channel": "Turbo::StreamsChannel", "signed_stream_name": "signed(Z2lk:messages)" }));
    client.subscribe(&guarded).await;
    assert_eq!(client.next_text().await, reject(&guarded));
    app.server.broadcast_append_to(&["Z2lk", "messages"], "messages", "<p>x</p>");
    client.assert_silent().await;

    // A non-string name raises in MessageVerifier: neither confirmed nor rejected.
    let numeric = identifier(json!({ "channel": "Turbo::StreamsChannel", "signed_stream_name": 1 }));
    client.subscribe(&numeric).await;
    client.assert_silent().await;
}

async fn http_get(url: &str, headers: &[(&str, &str)]) -> (u16, Option<String>, String) {
    use tokio::io::{AsyncReadExt, AsyncWriteExt};
    let url = url.strip_prefix("http://").unwrap();
    let (host, path) = url.split_once('/').unwrap();
    let mut stream = tokio::net::TcpStream::connect(host).await.unwrap();
    let mut request = format!("GET /{path} HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n");
    for (name, value) in headers {
        request.push_str(&format!("{name}: {value}\r\n"));
    }
    request.push_str("\r\n");
    stream.write_all(request.as_bytes()).await.unwrap();
    let mut response = String::new();
    stream.read_to_string(&mut response).await.unwrap();
    let (head, body) = response.split_once("\r\n\r\n").unwrap();
    let status = head.split(' ').nth(1).unwrap().parse().unwrap();
    let content_type = head.lines().find_map(|line| line.to_ascii_lowercase().strip_prefix("content-type: ").map(str::to_string));
    (status, content_type, body.to_string())
}

#[tokio::test]
async fn a_client_close_is_answered_with_its_code() {
    use futures_util::SinkExt;
    use tokio_tungstenite::tungstenite::protocol::{CloseFrame, frame::coding::CloseCode};
    let app = start(test_config()).await;
    let mut client = app.connect(1).await;
    assert_eq!(client.next_text().await, WELCOME);
    let close = CloseFrame { code: CloseCode::Away, reason: "".into() };
    client.socket.send(tokio_tungstenite::tungstenite::Message::Close(Some(close))).await.unwrap();
    assert_eq!(client.next().await, Frame::Close(Some((1001, String::new()))));
}

/// websocket-driver's code for the failure, not a blanket 1002.
#[tokio::test]
async fn a_protocol_error_closes_with_its_code() {
    use futures_util::SinkExt;
    use tokio_tungstenite::tungstenite::Message;
    use tokio_tungstenite::tungstenite::protocol::frame::coding::{Data, OpCode};
    let app = start(test_config()).await;
    let mut client = app.connect(1).await;
    assert_eq!(client.next_text().await, WELCOME);
    let not_utf8 = tokio_tungstenite::tungstenite::protocol::frame::Frame::message(vec![0xff, 0xfe], OpCode::Data(Data::Text), true);
    client.socket.send(Message::Frame(not_utf8)).await.unwrap();
    assert_eq!(client.next().await, Frame::Close(Some((1007, String::new()))));
}

#[tokio::test]
async fn a_connection_holds_a_bounded_number_of_subscriptions() {
    let app = start(test_config()).await;
    let mut client = app.connect(1).await;
    assert_eq!(client.next_text().await, WELCOME);
    for nonce in 0..64 {
        let heartbeat = identifier(json!({ "channel": "HeartbeatChannel", "nonce": nonce }));
        client.subscribe(&heartbeat).await;
        assert_eq!(client.next_text().await, confirm(&heartbeat));
    }
    // Past the limit, and an oversized identifier: ignored, no reply.
    client.subscribe(&identifier(json!({ "channel": "HeartbeatChannel", "nonce": 64 }))).await;
    client.assert_silent().await;
    let mut other = app.connect(2).await;
    assert_eq!(other.next_text().await, WELCOME);
    other.subscribe(&identifier(json!({ "channel": "HeartbeatChannel", "pad": "x".repeat(5000) }))).await;
    other.assert_silent().await;
}
