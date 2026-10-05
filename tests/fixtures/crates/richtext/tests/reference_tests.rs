//! Ports of the reference app's rich text tests: test/helpers/content_filters_test.rb,
//! messages_helper_test.rb, rich_text_helper_test.rb, test/lib/rails_ext/*_test.rb,
//! test/models/action_text_attachment_test.rb and the plain-text parts of the message tests.

use base64::Engine;
use campfire_richtext::attachables::{render_mention, web_url};
use campfire_richtext::content::Content;
use campfire_richtext::dom::Dom;
use campfire_richtext::{
    AttachableResolver, GidLookup, MentionUser, RenderContext, SignedLookup, editable_value, filters, mentioned_users,
    message_presentation, to_plain_text,
};

const DAVID_SGID: &str = "eyJfcmFpbHMiOnsiZGF0YSI6ImdpZDovL2NhbXBmaXJlL1VzZXIvMT9leHBpcmVzX2luIiwicHVyIjoiYXR0YWNoYWJsZSJ9fQ==--f7d8e8773314d3310320f3cdd08e5597bb51ca1a";
/// A Room's attachable SGID, as `rooms(:pets).to_sgid(expires_in: nil, for: "attachable")` mints it.
const ROOM_SGID_MESSAGE: &str = "eyJfcmFpbHMiOnsiZGF0YSI6ImdpZDovL2NhbXBmaXJlL1Jvb20vMT9leHBpcmVzX2luIiwicHVyIjoiYXR0YWNoYWJsZSJ9fQ==";

fn david() -> MentionUser {
    MentionUser {
        id: 1,
        name: "David".into(),
        title: "David – Founder".into(),
        attachable_sgid: DAVID_SGID.into(),
        user_path: "/users/1".into(),
        avatar_path: "/users/1/avatar?v=1".into(),
    }
}

struct Fixtures;

impl AttachableResolver for Fixtures {
    fn locate_signed(&self, sgid: &str) -> SignedLookup {
        if sgid == DAVID_SGID { SignedLookup::User(david()) } else { SignedLookup::Invalid }
    }

    fn find_gid(&self, gid: &str) -> GidLookup {
        match gid.split('?').next().unwrap() {
            "gid://campfire/User/1" => GidLookup::User(david()),
            "gid://campfire/Room/1" => GidLookup::OtherModel,
            _ => GidLookup::NotFound,
        }
    }
}

fn ctx() -> RenderContext<'static> {
    RenderContext { resolver: &Fixtures, request_host: Some("once.campfire.test".into()) }
}

/// test_helper.rb's `mention_attachment_for(:david)`
fn mention_attachment_for_david() -> String {
    format!(
        "<action-text-attachment sgid=\"{DAVID_SGID}\" content-type=\"application/vnd.campfire.mention\" content=\"{}\"></action-text-attachment>",
        render_mention(&david()).replace('"', "&quot;")
    )
}

fn filtered(body: &str) -> String {
    let ctx = ctx();
    filters::apply(Content::load(body, &ctx).unwrap(), &ctx).unwrap().to_html()
}

fn loaded(body: &str) -> String {
    Content::load(body, &ctx()).unwrap().to_html()
}

fn presentation(body: &str) -> String {
    message_presentation(body, &ctx()).unwrap()
}

const BASECAMP_UNFURL: &str = "<action-text-attachment content-type=\"application/vnd.actiontext.opengraph-embed\" url=\"https://basecamp.com/assets/general/opengraph.png\" href=\"https://basecamp.com/\" filename=\"Project management software, online collaboration\" caption=\"Trusted by millions.\"></action-text-attachment>";
const TWITTER_UNFURL: &str = "<action-text-attachment content-type=\"application/vnd.actiontext.opengraph-embed\" url=\"https://pbs.twimg.com/ext_tw_video_thumb/1752476502791503873/pu/img/WEAqUgarUxWjPNHD.jpg\" href=\"https://twitter.com/dhh/status/1752476663303323939\" filename=\"DHH (@dhh)\" caption=\"We're playing.\"></action-text-attachment>";

// --- test/helpers/content_filters_test.rb ------------------------------------------------------

#[test]
fn entire_message_contains_an_unfurled_url() {
    let body = format!("<div>https://basecamp.com/{BASECAMP_UNFURL}</div>");
    let result = filtered(&body);
    assert_ne!(loaded(&body), result);
    assert!(result.contains("<div><action-text-attachment"));
}

#[test]
fn entire_message_contains_an_unfurled_url_in_a_lexxy_body() {
    let body = format!("<p><a href=\"https://basecamp.com/\">https://basecamp.com/</a></p>{BASECAMP_UNFURL}");
    let result = filtered(&body);
    assert!(!result.contains(">https://basecamp.com/</a>"));
    assert!(result.contains("<action-text-attachment"));
}

#[test]
fn message_includes_additional_text_besides_an_unfurled_url() {
    let body = format!("<div>Hello https://basecamp.com/{BASECAMP_UNFURL}</div>");
    let result = filtered(&body);
    assert_eq!(loaded(&body), result);
    assert!(result.contains("<div>Hello https://basecamp.com/<action-text-attachment"));
}

#[test]
fn unfurled_tweet_with_an_avatar_image_gets_the_twitter_avatar_treatment() {
    let body = "<div>https://twitter.com/37signals/status/1750290547908952568<action-text-attachment content-type=\"application/vnd.actiontext.opengraph-embed\" url=\"https://pbs.twimg.com/profile_images/1671940407633010689/9P5gi6LF_200x200.jpg\" href=\"https://twitter.com/37signals/status/1750290547908952568\" filename=\"37signals (@37signals)\" caption=\"We're back up on all apps, everyone.\"></action-text-attachment></div>";
    assert!(presentation(body).contains("og-embed--twitter-avatar"));
}

#[test]
fn unfurled_tweet_with_an_avatar_image_in_a_lexxy_body_gets_the_twitter_avatar_treatment() {
    let content = "<actiontext-opengraph-embed><div class=\"og-embed gap\"><div class=\"og-embed__content\"><div class=\"og-embed__title\"><a href=\"https://twitter.com/x/status/1\">Tweet</a></div><div class=\"og-embed__description\">desc</div></div><div class=\"og-embed__image\"><img src=\"https://pbs.twimg.com/profile_images/x.jpg\" class=\"image center\" alt=\"\" /></div></div></actiontext-opengraph-embed>";
    let body = format!(
        "<p><a href=\"https://twitter.com/x/status/1\">https://twitter.com/x/status/1</a></p><action-text-attachment content-type=\"application/vnd.actiontext.opengraph-embed\" content=\"{}\"></action-text-attachment>",
        ruby_compat::erb::html_escape(content)
    );
    assert!(presentation(&body).contains("og-embed--twitter-avatar"));
}

#[test]
fn unfurled_tweet_with_a_content_image_is_not_styled_as_an_avatar() {
    let body = "<div>https://twitter.com/dhh/status/1748445489648050505<action-text-attachment content-type=\"application/vnd.actiontext.opengraph-embed\" url=\"https://pbs.twimg.com/media/GEO5l04bsAA9f6H.jpg\" href=\"https://twitter.com/dhh/status/1748445489648050505\" filename=\"DHH (@dhh)\" caption=\"MIT\"></action-text-attachment></div>";
    assert!(!presentation(body).contains("og-embed--twitter-avatar"));
}

#[test]
fn entire_message_contains_an_unfurled_url_from_x_com_but_unfurls_to_twitter_com() {
    for text in ["https://x.com/dhh/status/1752476663303323939", "https://x.com/dhh/status/1752476663303323939?s=20"] {
        let body = format!("<div>{text}{TWITTER_UNFURL}</div>");
        let result = filtered(&body);
        assert_ne!(loaded(&body), result);
        assert!(result.contains("<div><action-text-attachment"));
    }
}

#[test]
fn message_keeps_strikethrough_underline_and_code_block_formatting() {
    let html = presentation("<p>Hello <s>struck</s> <u>under</u> <mark>marked</mark></p><pre data-language=\"ruby\">def x<br>end</pre>");
    assert!(html.contains("<s>struck</s>"));
    assert!(html.contains("<u>under</u>"));
    assert!(html.contains("<mark>marked</mark>"));
    assert!(html.contains("<pre data-language=\"ruby\">"));
}

#[test]
fn message_contains_a_forbidden_tag() {
    assert_eq!(filtered("Hello <img src=\"https://ssecurityrise.com/tests/billionlaughs-cache.svg\">World"), "Hello World");
}

#[test]
fn message_with_a_link_using_an_unsafe_uri_scheme() {
    let result = filtered("<div><a href=\"javascript:alert(1)\">x</a></div>");
    assert!(!result.contains("javascript:"));
    assert!(result.contains("<a>x</a>"));
}

#[test]
fn message_with_an_event_handler_attribute_on_an_allowed_tag() {
    let result = filtered("<div><a href=\"/x\" onmouseover=\"alert(1)\">x</a> <span onclick=\"alert(2)\">y</span></div>");
    assert!(!result.contains("onmouseover"));
    assert!(!result.contains("onclick"));
    assert!(result.contains("<a href=\"/x\">x</a>"));
    assert!(result.contains("<span>y</span>"));
}

#[test]
fn message_with_a_data_uri_link() {
    let result = filtered("<div><a href=\"data:text/html,pwned\">x</a></div>");
    assert!(!result.contains("data:"));
    assert!(result.contains("<a>x</a>"));
}

#[test]
fn message_with_a_safe_link_and_formatting_is_preserved() {
    let result = filtered(
        "<div><a href=\"https://example.com\">example</a> <strong>bold</strong> <code>code</code><ul><li>one</li><li>two</li></ul></div>",
    );
    assert!(result.contains("<a href=\"https://example.com\">example</a>"));
    assert!(result.contains("<strong>bold</strong>"));
    assert!(result.contains("<code>code</code>"));
    assert!(result.contains("<ul><li>one</li><li>two</li></ul>"));
}

#[test]
fn sanitize_attributes_neutralizes_unsafe_input_and_preserves_benign_content() {
    let body = format!(
        "<div><a href=\"javascript:alert(1)\" onclick=\"x()\">link</a> <a href=\"data:text/html,pwned\">data</a> <span class=\"cf-twitter-avatar\" onmouseover=\"y()\">avatar</span> <img src=\"https://evil.example/x.svg\"> Hey {}</div>",
        mention_attachment_for_david()
    );
    let content = Content::load(&body, &ctx()).unwrap();
    let result = filters::sanitize_attributes(content).unwrap().to_html();
    for forbidden in ["javascript:", "data:text/html", "onclick", "onmouseover", "evil.example"] {
        assert!(!result.contains(forbidden), "{forbidden} in {result}");
    }
    assert!(result.contains("<span class=\"cf-twitter-avatar\">avatar</span>"));
    assert!(result.contains(">link<"));
    assert!(result.contains(&format!("<action-text-attachment sgid=\"{DAVID_SGID}\"")));
}

#[test]
fn message_with_formatting_saved_under_trix_renders_unchanged() {
    let body = "<div>Hello <strong>bold</strong> <em>it</em> <del>gone</del> <a href=\"https://example.com/\">link</a><br>second line</div><h1>Heading</h1><blockquote>quoted</blockquote><pre>line 1\nline 2</pre><ul><li>one</li></ul><ol><li>first</li></ol>";
    assert_eq!(filtered(body), body);
    assert!(presentation(body).contains(body));
}

#[test]
fn message_with_a_table_keeps_the_table() {
    let body = "<figure class=\"lexxy-content__table-wrapper\"><table><tbody><tr><th><p>Name</p></th></tr><tr><td><p>Jason</p></td></tr></tbody></table></figure>";
    assert_eq!(filtered(body), body);
    let html = presentation(body);
    let table = html.find("<table>").unwrap();
    assert!(html[table..].contains("<th><p>Name</p></th>") && html[table..].contains("<td><p>Jason</p></td>"));
}

#[test]
fn message_with_a_mention_attachment() {
    let result = filtered(&format!("<div>Hey {}</div>", mention_attachment_for_david()));
    assert!(
        result.contains(&format!(
            "<action-text-attachment sgid=\"{DAVID_SGID}\" content-type=\"application/vnd.campfire.mention\" content=\""
        ))
    );
}

// --- test/helpers/messages_helper_test.rb -------------------------------------------------------

#[test]
fn message_presentation_neutralizes_unsafe_uri_schemes_in_links() {
    let html = presentation("<div><a href=\"javascript:alert(1)\">x</a></div>");
    assert!(!html.contains("javascript:"));
    assert!(html.contains("<a>x</a>"));
}

#[test]
fn message_presentation_strips_event_handler_attributes_from_allowed_tags() {
    let html = presentation("<div><a href=\"/x\" onmouseover=\"alert(1)\">x</a></div>");
    assert!(!html.contains("onmouseover"));
    assert!(html.contains("<a href=\"/x\">x</a>"));
}

#[test]
fn message_presentation_preserves_safe_links_and_formatting() {
    let html = presentation("<div><a href=\"https://example.com\">example</a> <strong>bold</strong></div>");
    assert!(html.contains("<a href=\"https://example.com\">example</a>"));
    assert!(html.contains("<strong>bold</strong>"));
}

// --- test/helpers/rich_text_helper_test.rb ------------------------------------------------------

fn editable_attachment(body: &str) -> (String, String) {
    let value = editable_value(body, &ctx()).unwrap().unwrap();
    let mut dom = Dom::new();
    let root = dom.parse_fragment(&value).unwrap();
    let node = dom.descendants(root).into_iter().find(|&n| dom.local_name(n) == Some("action-text-attachment")).unwrap();
    (dom.attr(node, "content-type").unwrap().to_string(), dom.attr(node, "content").unwrap().to_string())
}

#[test]
fn editable_body_renders_legacy_opengraph_embeds_into_the_content_attribute() {
    let (_, content) = editable_attachment(
        "<div>https://example.com/ <action-text-attachment content-type=\"application/vnd.actiontext.opengraph-embed\" url=\"https://example.com/image.png\" href=\"https://example.com/\" filename=\"Example title\" caption=\"Example description\"></action-text-attachment></div>",
    );
    // A Trix-era embed has a url, so Lexxy leaves the content as the rendered partial
    assert!(content.contains("<a rel=\"noreferrer\" target=\"_blank\" href=\"https://example.com/\">Example title</a>"));
    assert!(content.contains("<div class=\"og-embed__description\">Example description</div>"));
    assert!(content.contains("<img src=\"https://example.com/image.png\""));
}

#[test]
fn editable_body_rebuilds_a_hand_written_embed_from_its_validated_details() {
    let content = "<actiontext-opengraph-embed data-controller=\"pwn\" data-action=\"click->pwn#run\"> <div class=\"og-embed\"><div class=\"og-embed__title\"><a href=\"/rooms/1\">Free cookies</a></div> <div class=\"og-embed__image\"><img src=\"/rooms/1/avatar\" data-action=\"load->pwn#run\"></div></div> </actiontext-opengraph-embed>";
    let body = format!(
        "<p><action-text-attachment content-type=\"application/vnd.actiontext.opengraph-embed\" url=\"https://example.com/image.png\" content=\"{}\"></action-text-attachment></p>",
        ruby_compat::erb::html_escape(content)
    );
    let (_, rebuilt) = editable_attachment(&body);
    assert!(rebuilt.contains("Free cookies"));
    assert!(!rebuilt.contains("rooms/1"));
    assert!(!rebuilt.contains("data-"));
    assert!(!rebuilt.contains("<a ") && !rebuilt.contains("<img"));
}

#[test]
fn editable_body_restores_the_content_type_of_a_mention_edited_under_trix() {
    let (content_type, content) = editable_attachment(&format!(
        "<div>Hey <action-text-attachment sgid=\"{DAVID_SGID}\" content-type=\"application/octet-stream\"></action-text-attachment></div>"
    ));
    assert_eq!(content_type, "application/vnd.campfire.mention");
    assert!(content.contains("David"));
}

#[test]
fn editable_body_leaves_bodies_without_attachments_unchanged() {
    assert_eq!(editable_value("<p>Plain text</p>", &ctx()).unwrap().as_deref(), Some("<p>Plain text</p>"));
}

// --- test/lib/rails_ext/action_text_attachables_test.rb, test/models/action_text_attachment_test.rb

fn attachment_plain_text(sgid: Option<&str>) -> String {
    let attribute = sgid.map(|s| format!(" sgid=\"{s}\"")).unwrap_or_default();
    to_plain_text(&format!("<action-text-attachment{attribute}></action-text-attachment>"), &ctx()).unwrap()
}

#[test]
fn from_node_with_a_valid_sgid() {
    assert_eq!(attachment_plain_text(Some(DAVID_SGID)), "@David");
}

#[test]
fn from_node_with_a_rails_7_sgid() {
    // Base64.urlsafe_encode64(Marshal.dump("gid://campfire/User/1"))
    let mut marshaled = vec![0x04, 0x08, b'I', b'"', 0x1a];
    marshaled.extend_from_slice(b"gid://campfire/User/1");
    marshaled.extend_from_slice(&[0x06, b':', 0x06, b'E', b'T']);
    let message = base64::engine::general_purpose::URL_SAFE.encode(&marshaled);
    let payload = format!("{{\"_rails\":{{\"message\":\"{message}\",\"exp\":null,\"pur\":\"attachable\"}}}}");
    let sgid = format!("{}--invalidsignature", base64::engine::general_purpose::STANDARD.encode(payload));
    assert_eq!(attachment_plain_text(Some(&sgid)), "@David");
}

#[test]
fn lookup_user_attachable_with_invalid_signature() {
    let message = DAVID_SGID.split("--").next().unwrap();
    assert_eq!(attachment_plain_text(Some(&format!("{message}--invalid"))), "@David");
}

#[test]
fn lookup_invalid_sgid_for_an_attachable_requiring_a_valid_sgid() {
    // A tampered SGID for any model but User is a missing attachable, which renders as ☒
    for sgid in [format!("{ROOM_SGID_MESSAGE}--invalid"), format!("{ROOM_SGID_MESSAGE}--f7d8e8773314d3310320f3cdd08e5597bb51ca1ainvalid")] {
        assert_eq!(attachment_plain_text(Some(&sgid)), "");
        let html = presentation(&format!("<p><action-text-attachment sgid=\"{sgid}\"></action-text-attachment></p>"));
        assert!(html.contains("☒"), "{html}");
        assert!(!html.contains("mention"));
    }
}

#[test]
fn lookup_attachable_with_nil_sgid() {
    let html = presentation("<p><action-text-attachment content-type=\"application/pdf\"></action-text-attachment></p>");
    assert!(html.contains("☒"));
}

// --- test/lib/rails_ext/actiontext_opengraph_embeds_test.rb -------------------------------------

#[test]
fn keeps_absolute_http_and_https_links_and_images() {
    assert_eq!(web_url(Some("http://example.com/page"), "").unwrap().as_deref(), Some("http://example.com/page"));
    assert_eq!(web_url(Some("https://example.com/image.png"), "").unwrap().as_deref(), Some("https://example.com/image.png"));
}

#[test]
fn drops_a_link_and_an_image_that_arent_web_urls() {
    for value in [
        "javascript:alert(1)", "data:text/html,pwned", "vbscript:msgbox(1)", "//example.com/image.png", "/rooms/1", "rooms/1", "",
        "http://exa mple.com/ ", "https:/rooms/1", "https:rooms/1", "http:/rooms/1", "https://", "http://:80/rooms/1",
    ] {
        assert_eq!(web_url(Some(value), "").unwrap(), None, "{value:?}");
    }
}

#[test]
fn drops_a_link_and_an_image_on_this_campfires_own_host_however_it_is_spelled() {
    for value in [
        "https://once.campfire.test/rooms/1",
        "http://once.campfire.test/rooms/1",
        "https://ONCE.Campfire.Test/rooms/1",
        "https://once.campfire.test./rooms/1",
        "https://%6fnce.campfire.test/rooms/1",
        "https://%77ww.example.com/x.png",
    ] {
        assert_eq!(web_url(Some(value), "once.campfire.test").unwrap(), None, "{value:?}");
    }
    assert!(web_url(Some("https://example.com/page"), "once.campfire.test").unwrap().is_some());
}

#[test]
fn drops_a_link_and_an_image_on_a_bare_address_rather_than_a_domain_name() {
    for value in [
        "http://127.0.0.1/rooms/1",
        "http://2130706433/rooms/1",
        "http://0177.0.0.1/rooms/1",
        "http://0x7f.0.0.1/rooms/1",
        "http://1.2.3.0xff/rooms/1",
        "http://[::1]/rooms/1",
        "http://localhost/rooms/1",
        "https://203.0.113.10/image.png",
    ] {
        assert_eq!(web_url(Some(value), "").unwrap(), None, "{value:?}");
    }
}

#[test]
fn keeps_an_internationalized_domain_written_in_punycode() {
    assert!(web_url(Some("https://xn--80aswg.xn--p1ai/page"), "").unwrap().is_some());
}

#[test]
fn renders_the_title_and_the_description_as_text() {
    let html = presentation(
        "<action-text-attachment content-type=\"application/vnd.actiontext.opengraph-embed\" href=\"https://example.com/page\" url=\"https://example.com/image.png\" filename=\"&lt;b&gt;Title&lt;/b&gt;\" caption=\"&lt;img src=x onerror=alert(1)&gt;\"></action-text-attachment>",
    );
    assert!(!html.contains("<b>"));
    assert!(!html.contains("<img src=x"));
    assert!(html.contains("&lt;b&gt;Title&lt;/b&gt;"));
}

// --- Plain text: test/models/message_test.rb, message/searchable_test.rb, webhook_test.rb ---------

#[test]
fn rich_text_body_is_converted_to_plain_text_for_indexing() {
    assert_eq!(to_plain_text("<span>My hovercraft is full of eels</span>", &ctx()).unwrap(), "My hovercraft is full of eels");
    assert_eq!(to_plain_text("First post!", &ctx()).unwrap(), "First post!");
}

#[test]
fn mentionees_are_the_mentioned_users_once_each() {
    let body = format!("<div>Hey {} {}</div>", mention_attachment_for_david(), mention_attachment_for_david());
    let users = mentioned_users(&body, &ctx()).unwrap();
    assert_eq!(users.iter().map(|u| u.id).collect::<Vec<_>>(), vec![1]);
}

#[test]
fn webhook_plain_body_drops_the_recipients_mentions() {
    let plain = to_plain_text(&format!("<p>{} hello</p>", mention_attachment_for_david()), &ctx()).unwrap();
    assert_eq!(plain, "@David hello");
    assert_eq!(campfire_richtext::without_recipient_mentions(&plain, "David"), "hello");
}
