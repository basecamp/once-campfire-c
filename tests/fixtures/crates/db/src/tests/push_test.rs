//! `test/models/room/push_test.rb` (who gets pushed; delivery is the app's) and
//! `test/models/push/subscription_test.rb` (validation and endpoint pinning).

use super::*;
use crate::models::push_subscription::{MAX_PAYLOAD_BODY_BYTES, PushSubscription};
use crate::{Membership, Message, NewMessage, RichText};

const WEB_PUSH_PUBLIC_TEST_IP: &str = "142.250.185.206";

/// A body's rich text, mentioning these users.
struct Mentions(Vec<i64>);

impl RichText for Mentions {
    fn to_plain_text(&self, conn: &Connection, html: &str) -> String {
        BasicRichText.to_plain_text(conn, html)
    }

    fn mentioned_user_ids(&self, _conn: &Connection, _html: &str) -> Vec<i64> {
        self.0.clone()
    }
}

/// How many deliveries `Room::MessagePusher#push` would queue for a new message.
fn deliveries(t: &TestDb, room: &str, body: &str, mentioned: &[i64]) -> usize {
    let attributes = NewMessage {
        room_id: id(room),
        creator_id: id("david"),
        client_message_id: Some("earth".into()),
        body: Some(body.into()),
        attachment_blob_id: None,
    };
    let message = t.write(move |tx| Message::create(tx, attributes));
    let now = t.now();
    let rich_text = Mentions(mentioned.to_vec());
    let (_, everything, mentions) = t.read(|c| PushSubscription::pushes_for(c, &rich_text, &message, now));
    everything.len() + mentions.len()
}

#[test]
fn deliver_new_message_to_other_room_users_with_push_subscriptions() {
    let t = TestDb::new();
    let all = t.read(PushSubscription::count);
    let davids = t.read(|c| PushSubscription::for_user(c, id("david"))).len() as i64;
    assert_eq!(deliveries(&t, "hq", "This is from earth", &[]) as i64, all - davids);
}

#[test]
fn notifies_subscribed_users() {
    let t = TestDb::new();
    assert_eq!(deliveries(&t, "designers", "This is from earth", &[]), 2);
    assert_eq!(deliveries(&t, "designers", "Hey @Kevin", &[id("kevin")]), 3);
}

#[test]
fn does_not_notify_for_connected_rooms() {
    let t = TestDb::new();
    t.write(|tx| Membership::find(tx.conn(), id("kevin_designers"))?.connected(tx));
    assert_eq!(deliveries(&t, "designers", "Hey @Kevin", &[id("kevin")]), 2);
}

#[test]
fn does_not_notify_for_invisible_rooms() {
    let t = TestDb::new();
    t.write(|tx| Membership::find(tx.conn(), id("kevin_designers"))?.update_involvement(tx, crate::Involvement::Invisible));
    assert_eq!(deliveries(&t, "designers", "Hey @Kevin", &[id("kevin")]), 2);
}

#[test]
fn payloads() {
    let t = TestDb::new();
    let attributes = NewMessage { room_id: id("designers"), creator_id: id("david"), body: Some("Hi".into()), ..Default::default() };
    let message = t.write(move |tx| Message::create(tx, attributes));
    let room = t.read(|c| message.room(c));
    let payload = t.read(|c| PushSubscription::payload_for(c, &BasicRichText, &room, &message));
    assert_eq!((payload.title.as_str(), payload.body.as_str()), ("Designers", "David: Hi"));
    assert_eq!(payload.path, format!("/rooms/{}", id("designers")));

    let attributes = NewMessage { room_id: id("david_and_jason"), creator_id: id("david"), body: Some("Hi".into()), ..Default::default() };
    let message = t.write(move |tx| Message::create(tx, attributes));
    let room = t.read(|c| message.room(c));
    let payload = t.read(|c| PushSubscription::payload_for(c, &BasicRichText, &room, &message));
    assert_eq!((payload.title.as_str(), payload.body.as_str()), ("David", "Hi"));
}

#[test]
fn long_payloads_are_cut_short_to_fit_a_push_message() {
    let t = TestDb::new();
    let body = format!("{}\"{}", "é".repeat(1000), "x".repeat(2000));
    let attributes = NewMessage { room_id: id("designers"), creator_id: id("david"), body: Some(body), ..Default::default() };
    let message = t.write(move |tx| Message::create(tx, attributes));
    let room = t.read(|c| message.room(c));
    let payload = t.read(|c| PushSubscription::payload_for(c, &BasicRichText, &room, &message));

    let json_len = |text: &str| serde_json::to_string(text).unwrap().len() - 2;
    assert!(payload.body.starts_with("David: éé"));
    assert!(payload.body.ends_with("xx…"));
    assert!(json_len(&payload.body) <= MAX_PAYLOAD_BODY_BYTES);
    assert!(json_len(&payload.body) > MAX_PAYLOAD_BODY_BYTES - 4);
}

fn build(endpoint: &str) -> PushSubscription {
    PushSubscription::new(id("david"), Some(endpoint), Some("test_key"), Some("test_auth"), None)
}

fn public(_: &str) -> Option<String> {
    Some(WEB_PUSH_PUBLIC_TEST_IP.into())
}

fn private(_: &str) -> Option<String> {
    None
}

fn errors(subscription: &PushSubscription, resolve: fn(&str) -> Option<String>) -> Vec<String> {
    subscription.validate(&resolve).on("endpoint").into_iter().map(String::from).collect()
}

#[test]
fn valid_subscription_with_permitted_endpoint() {
    assert!(errors(&build("https://fcm.googleapis.com/fcm/send/abc123"), public).is_empty());
}

#[test]
fn rejects_endpoint_with_non_https_scheme() {
    assert!(errors(&build("http://fcm.googleapis.com/fcm/send/abc123"), public).contains(&"must use HTTPS".into()));
}

#[test]
fn rejects_endpoint_with_non_permitted_host() {
    assert!(errors(&build("https://attacker.example.com/webhook"), public).contains(&"is not a permitted push service".into()));
}

#[test]
fn rejects_endpoint_whose_host_only_suffix_matches_a_permitted_host() {
    assert!(
        errors(&build("https://evilfcm.googleapis.com.attacker.example/webhook"), public)
            .contains(&"is not a permitted push service".into())
    );
}

#[test]
fn rejects_blank_endpoint() {
    assert!(errors(&build(""), public).contains(&"can't be blank".into()));
}

#[test]
fn rejects_endpoint_on_a_non_default_port() {
    assert!(errors(&build("https://fcm.googleapis.com:8443/fcm/send/abc123"), public).contains(&"must use the default HTTPS port".into()));
}

#[test]
fn rejects_endpoint_that_resolves_to_private_ip() {
    let subscription = build("https://fcm.googleapis.com/fcm/send/abc123");
    assert!(errors(&subscription, private).contains(&"resolves to a private or invalid IP address".into()));
    assert_eq!(subscription.resolved_endpoint_ip(&private), None);
}

#[test]
fn resolved_endpoint_ip_returns_the_pinned_public_ip() {
    assert_eq!(build("https://fcm.googleapis.com/fcm/send/abc123").resolved_endpoint_ip(&public).as_deref(), Some(WEB_PUSH_PUBLIC_TEST_IP));
}

#[test]
fn delivery_is_skipped_for_a_non_permitted_host_or_port() {
    assert_eq!(build("https://attacker.example.com/collect").resolved_endpoint_ip(&public), None);
    assert_eq!(build("https://fcm.googleapis.com:22/fcm/send/abc123").resolved_endpoint_ip(&public), None);
}

#[test]
fn accepts_all_permitted_push_service_domains() {
    for endpoint in [
        "https://fcm.googleapis.com/fcm/send/token123",
        "https://jmt17.google.com/fcm/send/token123",
        "https://updates.push.services.mozilla.com/wpush/v2/token123",
        "https://web.push.apple.com/QaBC123",
        "https://wns2-db5p.notify.windows.com/w/?token=abc123",
    ] {
        assert!(errors(&build(endpoint), public).is_empty(), "{endpoint}");
    }
}

#[test]
fn create_validates() {
    let t = TestDb::new();
    assert!(t.try_write(|tx| PushSubscription::create(tx, &build("http://fcm.googleapis.com/x"), &public)).is_err());
    let created = t.write(|tx| PushSubscription::create(tx, &build("https://fcm.googleapis.com/x"), &public));
    assert_eq!(t.read(|c| PushSubscription::find(c, created.id)).endpoint.as_deref(), Some("https://fcm.googleapis.com/x"));
}
