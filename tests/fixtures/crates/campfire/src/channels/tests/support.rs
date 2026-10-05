//! A cable server with Campfire's channels over a fixtures database, and a WebSocket client.
#![allow(dead_code)]

use std::sync::{Arc, OnceLock};
use std::time::Duration;

use campfire_cable::Config;
use campfire_db::fixtures::{self, identify};
use campfire_db::{Boost, Database, Event, EventSink, Membership, Message, Room, Session, Timestamp};
use campfire_kit::{Clock, TestClock};
use futures_util::{SinkExt, StreamExt};
use rails_compat::Secrets;
use serde_json::{Value, json};
use tokio::net::TcpStream;
use tokio_tungstenite::tungstenite::Message as WsMessage;
use tokio_tungstenite::tungstenite::client::IntoClientRequest;
use tokio_tungstenite::{MaybeTlsStream, WebSocketStream};

use crate::channels::{self, Broadcasts, Cable, Deps, Partials, revocation};
use crate::rich_text::AppRichText;

pub const SECRET_KEY_BASE: &str = "channels-test-secret-key-base";

/// Routes `Event::DisconnectUser` to the cable server, as app core's sink does.
#[derive(Default)]
struct CableSink {
    server: OnceLock<Cable>,
}

impl EventSink for CableSink {
    fn emit(&self, event: Event) {
        if let Some(server) = self.server.get() {
            revocation::handle_event(server, &event);
        }
    }
}

pub struct TestApp {
    pub db: Database,
    pub server: Cable,
    pub broadcasts: Broadcasts,
    pub secrets: Arc<Secrets>,
    pub clock: Arc<TestClock>,
    pub url: String,
    pub origin: String,
    _dir: tempfile::TempDir,
}

pub async fn start() -> TestApp {
    let dir = tempfile::tempdir().unwrap();
    let sink = Arc::new(CableSink::default());
    let clock = Arc::new(TestClock::new());
    let secrets = Arc::new(Secrets::new(SECRET_KEY_BASE));
    let rich_text = Arc::new(AppRichText::new(secrets.clone(), clock.clone()));
    let env = campfire_db::Env { clock: clock.clone(), sink: sink.clone(), rich_text, bcrypt_cost: 4 };
    let mut config = campfire_db::Config::new(dir.path().join("test.sqlite3"));
    config.readers = 2;
    config.environment = "test".into();
    let db = Database::open(config, env).unwrap();
    db.write(|tx| {
        let options = fixtures::Options { now: tx.now(), bcrypt_cost: 4 };
        fixtures::load(tx.conn(), &fixtures::reference_dir(), &options).map(|_| ())
    })
    .await
    .unwrap();

    let deps = Deps { db: db.clone(), secrets: secrets.clone(), clock: clock.clone() };
    let server = channels::server(deps, Config { assume_ssl: false, ..Config::default() });
    let _ = sink.server.set(server.clone());

    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = listener.local_addr().unwrap();
    let app = server.router::<()>("/cable");
    tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });

    TestApp {
        broadcasts: Broadcasts::new(server.clone()),
        db,
        server,
        secrets,
        clock,
        url: format!("ws://{addr}/cable"),
        origin: format!("http://{addr}"),
        _dir: dir,
    }
}

pub fn id(label: &str) -> i64 {
    identify(label)
}

impl TestApp {
    /// `Time.current` on the clock the database and the cable server share.
    pub fn now(&self) -> Timestamp {
        Timestamp::from_jiff(self.clock.now())
    }

    /// A session cookie for the fixture user (`cookies.signed[:session_token]`).
    pub async fn cookie_for(&self, user: &str) -> String {
        let user_id = id(user);
        let session = self.db.write(move |tx| Session::start(tx, user_id, Some("test"), Some("8.8.8.8"))).await.unwrap();
        self.cookie_with_token(&session.token)
    }

    pub fn cookie_with_token(&self, token: &str) -> String {
        self.cookie_with_token_expiring(token, None)
    }

    pub fn cookie_with_token_expiring(&self, token: &str, expires_at: Option<jiff::Timestamp>) -> String {
        let signed = rails_compat::cookies::sign(&self.secrets, "session_token", token, expires_at);
        format!("session_token={}", signed.replace('+', "%2B").replace('/', "%2F").replace('=', "%3D"))
    }

    /// Connects as the fixture user and reads the welcome.
    pub async fn connect(&self, user: &str) -> Client {
        let cookie = self.cookie_for(user).await;
        let mut client = self.connect_with_cookie(Some(&cookie)).await;
        assert_eq!(client.next_text().await, r#"{"type":"welcome"}"#);
        client
    }

    pub async fn connect_with_cookie(&self, cookie: Option<&str>) -> Client {
        let mut request = self.url.as_str().into_client_request().unwrap();
        let headers = request.headers_mut();
        headers.insert("origin", self.origin.parse().unwrap());
        headers.insert("sec-websocket-protocol", "actioncable-v1-json, actioncable-unsupported".parse().unwrap());
        if let Some(cookie) = cookie {
            headers.insert("cookie", cookie.parse().unwrap());
        }
        let (socket, _) = tokio_tungstenite::connect_async(request).await.expect("upgrade");
        Client { socket }
    }

    pub fn signed_stream_name(&self, streamables: &[&str]) -> String {
        rails_compat::turbo::signed_stream_name(&self.secrets, streamables)
    }

    pub async fn room(&self, label: &str) -> Room {
        let room_id = id(label);
        self.db.read(move |conn| Room::find(conn, room_id)).await.unwrap()
    }

    pub async fn membership(&self, room: &str, user: &str) -> Option<Membership> {
        let (room_id, user_id) = (id(room), id(user));
        self.db.read(move |conn| Membership::find_by_room_and_user(conn, room_id, user_id)).await.unwrap()
    }

    pub async fn message(&self, label: &str) -> Message {
        let message_id = id(label);
        self.db.read(move |conn| Message::find(conn, message_id)).await.unwrap()
    }

    pub async fn boost(&self, label: &str) -> Boost {
        let boost_id = id(label);
        self.db.read(move |conn| Boost::find(conn, boost_id)).await.unwrap()
    }
}

/// Stand-in partials that name what they render, so frames show which partial and record.
pub struct FakePartials;

impl Partials for FakePartials {
    fn message(&self, message: &Message) -> String {
        format!(r#"<div id="message_{}">message {}</div>"#, message.client_message_id, message.id)
    }
    fn message_presentation(&self, message: &Message) -> String {
        format!("<div>presentation {} & more</div>", message.id)
    }
    fn boost(&self, boost: &Boost) -> String {
        format!("<div>boost {}</div>", boost.id)
    }
    fn shared_room(&self, room: &Room) -> String {
        format!("<li>shared {}</li>", room.id)
    }
    fn direct_room(&self, membership: &Membership) -> String {
        format!("<li>direct {}</li>", membership.id)
    }
}

pub fn identifier(value: Value) -> String {
    value.to_string()
}

pub fn room_identifier(channel: &str, room_id: i64) -> String {
    identifier(json!({ "channel": channel, "room_id": room_id }))
}

pub fn confirmation(identifier: &str) -> String {
    json!({ "identifier": identifier, "type": "confirm_subscription" }).to_string()
}

pub fn rejection(identifier: &str) -> String {
    json!({ "identifier": identifier, "type": "reject_subscription" }).to_string()
}

/// The frame a broadcast of `message` (already ActiveSupport-JSON-encoded) arrives in.
pub fn delivery(identifier: &str, encoded_message: &str) -> String {
    format!(r#"{{"identifier":{},"message":{}}}"#, rails_compat::json::encode(identifier), encoded_message)
}

pub struct Client {
    pub socket: WebSocketStream<MaybeTlsStream<TcpStream>>,
}

#[derive(Debug, PartialEq)]
pub enum Frame {
    Text(String),
    Close,
    End,
}

impl Client {
    pub async fn send(&mut self, command: Value) {
        self.socket.send(WsMessage::Text(command.to_string().into())).await.unwrap();
    }

    pub async fn subscribe(&mut self, identifier: &str) {
        self.send(json!({ "command": "subscribe", "identifier": identifier })).await;
    }

    /// Subscribes and returns the confirm or reject frame.
    pub async fn subscribe_reply(&mut self, identifier: &str) -> String {
        self.subscribe(identifier).await;
        self.next_text().await
    }

    pub async fn confirm(&mut self, identifier: &str) {
        assert_eq!(self.subscribe_reply(identifier).await, confirmation(identifier), "subscribing to {identifier}");
    }

    pub async fn reject(&mut self, identifier: &str) {
        assert_eq!(self.subscribe_reply(identifier).await, rejection(identifier), "subscribing to {identifier}");
    }

    pub async fn unsubscribe(&mut self, identifier: &str) {
        self.send(json!({ "command": "unsubscribe", "identifier": identifier })).await;
    }

    pub async fn perform(&mut self, identifier: &str, data: Value) {
        self.send(json!({ "command": "message", "identifier": identifier, "data": data.to_string() })).await;
    }

    /// The next frame, skipping pings (which don't extend the 5s it has to arrive in, so a
    /// missing frame fails the test rather than waiting through heartbeats forever).
    pub async fn next(&mut self) -> Frame {
        let deadline = tokio::time::Instant::now() + Duration::from_secs(5);
        loop {
            let message = tokio::time::timeout_at(deadline, self.socket.next()).await.expect("a frame within 5s");
            return match message {
                Some(Ok(WsMessage::Text(text))) if text.starts_with(r#"{"type":"ping""#) => continue,
                Some(Ok(WsMessage::Text(text))) => Frame::Text(text.to_string()),
                Some(Ok(WsMessage::Close(_))) => Frame::Close,
                Some(Ok(_)) => continue,
                Some(Err(_)) | None => Frame::End,
            };
        }
    }

    pub async fn next_text(&mut self) -> String {
        match self.next().await {
            Frame::Text(text) => text,
            other => panic!("expected a text frame, got {other:?}"),
        }
    }

    /// Asserts nothing arrives (pings aside) for a moment.
    pub async fn assert_silent(&mut self) {
        let result = tokio::time::timeout(Duration::from_millis(250), self.next()).await;
        assert!(result.is_err(), "expected no frame, got {result:?}");
    }

    /// Reads until the server closes the socket, returning the text frames before the close.
    pub async fn until_closed(&mut self) -> Vec<String> {
        let mut frames = Vec::new();
        loop {
            match self.next().await {
                Frame::Text(text) => frames.push(text),
                // Keep reading so the client's close reply goes out and the server finishes
                // closing (unsubscribing everything) without waiting out its close timeout.
                Frame::Close => {}
                Frame::End => return frames,
            }
        }
    }
}

/// Waits until `check` holds, for state the server changes after a frame is sent (unsubscribe
/// has no reply to wait for).
pub async fn eventually<F, Fut>(mut check: F)
where
    F: FnMut() -> Fut,
    Fut: std::future::Future<Output = bool>,
{
    for _ in 0..100 {
        if check().await {
            return;
        }
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
    panic!("condition never held");
}

/// A JSON string literal the way ActiveSupport encodes HTML in it: quotes and backslashes
/// escaped, and `<`, `>`, `&` as `<`, `>`, `&`.
pub fn html_json(html: &str) -> String {
    let backslash = '\\';
    let escaped = html
        .replace(backslash, &format!("{backslash}{backslash}"))
        .replace('"', &format!("{backslash}\""))
        .replace('<', &format!("{backslash}u003c"))
        .replace('>', &format!("{backslash}u003e"))
        .replace('&', &format!("{backslash}u0026"));
    format!("\"{escaped}\"")
}
