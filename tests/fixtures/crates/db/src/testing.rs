//! Stand-ins for tests, behind the `test-support` feature like [`crate::fixtures`], so that
//! production can't be wired with a sink that drops every event or with rich text that only
//! approximates Action Text's.

use std::sync::{Arc, Mutex};

use base64::Engine;
use rails_compat::clock::SystemClock;
use rusqlite::Connection;

use crate::{Env, Event, EventSink, RichText, User};

/// The system clock, no side effects and [`BasicRichText`].
impl Default for Env {
    fn default() -> Self {
        Self {
            clock: Arc::new(SystemClock),
            sink: Arc::new(NullSink),
            rich_text: Arc::new(BasicRichText),
            bcrypt_cost: rails_compat::password::COST,
        }
    }
}

/// Drops every event.
#[derive(Debug, Default, Clone, Copy)]
pub struct NullSink;

impl EventSink for NullSink {
    fn emit(&self, _event: Event) {}
}

/// Records events (`assert_enqueued_jobs` and friends).
#[derive(Debug, Default, Clone)]
pub struct RecordingSink {
    events: Arc<Mutex<Vec<Event>>>,
}

impl RecordingSink {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn events(&self) -> Vec<Event> {
        self.events.lock().unwrap().clone()
    }

    pub fn take(&self) -> Vec<Event> {
        std::mem::take(&mut *self.events.lock().unwrap())
    }
}

impl EventSink for RecordingSink {
    fn emit(&self, event: Event) {
        self.events.lock().unwrap().push(event);
    }
}

/// Tag stripping, with mentions read from unverified SGIDs, as Campfire's plain text reads them
/// (`reference/lib/rails_ext/action_text_attachables.rb`); the exact rules belong to
/// `campfire_richtext`. It reports no mentions: those need verified SGIDs (`Content#attachables`),
/// so tests that need mentions state them.
#[derive(Debug, Default, Clone, Copy)]
pub struct BasicRichText;

impl RichText for BasicRichText {
    fn to_plain_text(&self, conn: &Connection, html: &str) -> String {
        let mut out = String::new();
        let mut rest = html;
        while let Some(start) = rest.find('<') {
            out.push_str(&rest[..start]);
            let Some(end) = rest[start..].find('>') else {
                rest = "";
                break;
            };
            let tag = &rest[start + 1..start + end];
            let closing = tag.starts_with('/');
            let name: String = tag.trim_start_matches('/').chars().take_while(|c| c.is_ascii_alphanumeric() || *c == '-').collect();
            if name == "action-text-attachment" && !closing {
                let mention =
                    attribute_values(tag, "sgid").first().and_then(|sgid| user_id_from_sgid(sgid)).and_then(|id| user_name(conn, id));
                if let Some(name) = mention {
                    out.push('@');
                    out.push_str(&name);
                }
                // Skip the attachment's inner content.
                if let Some(close) = rest[start..].find("</action-text-attachment>") {
                    rest = &rest[start + close + "</action-text-attachment>".len()..];
                    continue;
                }
            }
            if matches!(name.as_str(), "br" | "p" | "div" | "li" | "h1" | "blockquote" | "pre") && !out.is_empty() && !closing {
                out.push('\n');
            }
            rest = &rest[start + end + 1..];
        }
        out.push_str(rest);
        decode_entities(out.trim())
    }

    fn mentioned_user_ids(&self, _conn: &Connection, _html: &str) -> Vec<i64> {
        Vec::new()
    }
}

fn user_name(conn: &Connection, id: i64) -> Option<String> {
    User::find_by_id(conn, id).ok().flatten().map(|user| user.name)
}

fn attribute_values(html: &str, name: &str) -> Vec<String> {
    let needle = format!("{name}=\"");
    let mut values = Vec::new();
    let mut rest = html;
    while let Some(start) = rest.find(&needle) {
        let after = &rest[start + needle.len()..];
        let Some(end) = after.find('"') else { break };
        values.push(after[..end].to_string());
        rest = &after[end..];
    }
    values
}

/// Reads `gid://campfire/User/<id>` out of an SGID's message without verifying it.
fn user_id_from_sgid(sgid: &str) -> Option<i64> {
    let message = sgid.split("--").next()?;
    let message = message.replace("%3D", "=").replace("%2B", "+").replace("%2F", "/");
    let decoded = base64::engine::general_purpose::STANDARD
        .decode(message.as_bytes())
        .or_else(|_| base64::engine::general_purpose::URL_SAFE.decode(message.as_bytes()))
        .ok()?;
    let text = String::from_utf8_lossy(&decoded);
    let marker = "gid://campfire/User/";
    let start = text.find(marker)? + marker.len();
    let digits: String = text[start..].chars().take_while(|c| c.is_ascii_digit()).collect();
    digits.parse().ok()
}

fn decode_entities(s: &str) -> String {
    s.replace("&nbsp;", "\u{a0}")
        .replace("&lt;", "<")
        .replace("&gt;", ">")
        .replace("&quot;", "\"")
        .replace("&#39;", "'")
        .replace("&amp;", "&")
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::tests::{TestDb, id};

    fn memory() -> Connection {
        Connection::open_in_memory().unwrap()
    }

    /// `MentionTestHelper#mention_attachment_for`, with an unsigned SGID.
    fn unsigned_mention(user_id: i64) -> String {
        let payload = base64::engine::general_purpose::STANDARD
            .encode(format!(r#"{{"_rails":{{"data":"gid://campfire/User/{user_id}","pur":"attachable"}}}}"#));
        format!(
            r#"<action-text-attachment sgid="{payload}--unsigned" content-type="application/vnd.campfire.mention"></action-text-attachment>"#
        )
    }

    #[test]
    fn plain_text_strips_tags() {
        assert_eq!(BasicRichText.to_plain_text(&memory(), "<span>My hovercraft is full of eels</span>"), "My hovercraft is full of eels");
        assert_eq!(BasicRichText.to_plain_text(&memory(), "Hello <b>there</b>"), "Hello there");
    }

    #[test]
    fn plain_text_renders_mentions_by_name() {
        let t = TestDb::new();
        let html = format!("Hey {}", unsigned_mention(id("kevin")));
        assert_eq!(t.read(|c| Ok(BasicRichText.to_plain_text(c, &html))), "Hey @Kevin");
    }

    #[test]
    fn reports_no_mentions() {
        let html = format!("<div>Hey {}</div>", unsigned_mention(id("kevin")));
        assert_eq!(BasicRichText.mentioned_user_ids(&memory(), &html), Vec::<i64>::new());
    }
}
