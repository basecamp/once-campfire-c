//! DOM parity of the message views with the reference app (goldens in tests/golden/b).

mod messages_support;

use askama::Template;
use campfire_views::messages::{self, EditView, MessageView, RoomKind, UserView};
use messages_support::golden;

#[test]
fn show_text_message() {
    let g = golden("messages_show_text");
    let message: MessageView = g.input();
    g.assert_content(&g.render(|ctx| messages::Show { ctx, message: &message }.render().unwrap()));
}

/// The partial is cached once for every request, so nothing in it may come from the request's
/// Host header (README, Known differences).
#[test]
fn message_partial_is_the_same_on_every_host() {
    let mut g = golden("messages_show_text");
    let message: MessageView = g.input();
    let render = |g: &messages_support::Golden| g.render(|ctx| messages::MessagePartial { ctx, message: &message }.render().unwrap());
    let on_the_reference_host = render(&g);
    g.json["context"]["base_url"] = "https://evil.example".into();
    assert_eq!(render(&g), on_the_reference_host);
    assert!(on_the_reference_host.contains(&format!("data-copy-to-clipboard-url-value=\"/rooms/{}/@{}\"", message.room_id, message.id)));
}

#[test]
fn show_image_message() {
    let g = golden("messages_show_image");
    let message: MessageView = g.input();
    g.assert_content(&g.render(|ctx| messages::Show { ctx, message: &message }.render().unwrap()));
}

#[test]
fn index() {
    let g = golden("messages_index");
    let messages: Vec<messages::MessageItem> = g.input_at("messages");
    g.assert_content(&g.render(|ctx| messages::Index { ctx, messages: &messages }.render().unwrap()));
}

#[test]
fn edit_text_message() {
    let g = golden("messages_edit_text");
    let edit: EditView = g.input();
    g.assert_content(&g.render(|ctx| messages::Edit { ctx, edit: &edit }.render().unwrap()));
}

#[test]
fn edit_attachment_message() {
    let g = golden("messages_edit_attachment");
    let edit: EditView = g.input();
    g.assert_content(&g.render(|ctx| messages::Edit { ctx, edit: &edit }.render().unwrap()));
}

#[test]
fn create_stream() {
    let g = golden("messages_create");
    let message: messages::MessageItem = g.input_at("message");
    let room_kind: RoomKind = g.input_at("room_kind");
    g.assert_dom(&g.render(|ctx| messages::CreateStream { ctx, message: &message, room_kind }.render().unwrap()));
}

#[test]
fn destroy_stream() {
    let g = golden("messages_destroy");
    let message: MessageView = g.input();
    g.assert_dom(&g.render(|_| messages::DestroyStream { message: &message }.render().unwrap()));
}

#[test]
fn room_not_found() {
    let g = golden("messages_room_not_found");
    g.assert_content(&messages::RoomNotFound.render().unwrap());
}

#[test]
fn boosts_index() {
    let g = golden("messages_boosts_index");
    let message: MessageView = g.input();
    g.assert_content(&g.render(|ctx| messages::BoostsIndex { ctx, message: &message }.render().unwrap()));
}

#[test]
fn new_boost() {
    let g = golden("messages_boosts_new");
    let message: MessageView = g.input_at("message");
    let user: UserView = g.input_at("user");
    g.assert_content(&g.render(|ctx| messages::NewBoost { ctx, message: &message, user: &user }.render().unwrap()));
}

#[test]
fn by_bots_index_json() {
    let g = golden("messages_by_bots_index");
    let input: Vec<messages::json::MessageJson> = g.input();
    assert_eq!(messages::json::by_bots_index(&input), g.json["raw"].as_str().unwrap());
}

#[test]
fn by_bots_show_json() {
    let g = golden("messages_by_bots_show");
    let input: messages::json::MessageJson = g.input();
    assert_eq!(messages::json::by_bots_show(&input), g.json["raw"].as_str().unwrap());
}

#[test]
fn boosts_by_bots_show_json() {
    let g = golden("messages_boosts_by_bots_show");
    let input: messages::json::BoostJson = g.input();
    assert_eq!(messages::json::boosts_by_bots_show(&input), g.json["raw"].as_str().unwrap());
}
