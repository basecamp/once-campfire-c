//! DOM normalization for parity tests: two documents are the same page when their token
//! streams match after sorting attributes, merging adjacent text, collapsing whitespace runs to
//! one space, and dropping the forgery tokens Rails renders and this app doesn't. Whitespace-only
//! text is kept (as a single space) because it can affect inline layout.

use std::cell::RefCell;

use html5ever::tendril::StrTendril;
use html5ever::tokenizer::{
    BufferQueue, CharacterTokens, CommentToken, DoctypeToken, EndTag, NullCharacterToken, StartTag, TagToken, Token, TokenSink,
    TokenSinkResult, Tokenizer, TokenizerOpts,
};

#[derive(Default)]
struct Sink {
    lines: RefCell<Vec<String>>,
    text: RefCell<String>,
}

impl Sink {
    fn flush_text(&self) {
        let text = std::mem::take(&mut *self.text.borrow_mut());
        if text.is_empty() {
            return;
        }
        let collapsed = collapse_whitespace(&text);
        self.lines.borrow_mut().push(format!("\"{collapsed}\""));
    }
}

fn collapse_whitespace(text: &str) -> String {
    let mut out = String::new();
    let mut in_space = false;
    for c in text.chars() {
        if c.is_ascii_whitespace() {
            if !in_space {
                out.push(' ');
            }
            in_space = true;
        } else {
            out.push(c);
            in_space = false;
        }
    }
    out
}

impl TokenSink for Sink {
    type Handle = ();

    fn process_token(&self, token: Token, _line: u64) -> TokenSinkResult<()> {
        match token {
            CharacterTokens(text) => self.text.borrow_mut().push_str(&text),
            NullCharacterToken => self.text.borrow_mut().push('\0'),
            TagToken(tag) => {
                let name = tag.name.to_string();
                let mut attrs: Vec<(String, String)> =
                    tag.attrs.iter().map(|attr| (attr.name.local.to_string(), attr.value.to_string())).collect();
                // Dropped before flushing, so the text on either side merges as if it weren't there.
                if tag.kind == StartTag && is_forgery_token(&name, &attrs) {
                    return TokenSinkResult::Continue;
                }
                self.flush_text();
                match tag.kind {
                    StartTag => {
                        attrs.sort();
                        let attrs: String = attrs.iter().map(|(k, v)| format!(" {k}={v:?}")).collect();
                        self.lines.borrow_mut().push(format!("<{name}{attrs}>"));
                    }
                    EndTag => self.lines.borrow_mut().push(format!("</{name}>")),
                }
            }
            DoctypeToken(doctype) => {
                self.flush_text();
                self.lines.borrow_mut().push(format!("<!DOCTYPE {:?}>", doctype.name.map(|n| n.to_string())));
            }
            CommentToken(_) => self.flush_text(),
            _ => {}
        }
        TokenSinkResult::Continue
    }
}

/// The CSRF meta tags and hidden token fields Rails renders. This app protects against forgery by
/// `Sec-Fetch-Site` instead, so its pages have none (all are void elements: no end tag to drop).
fn is_forgery_token(tag: &str, attrs: &[(String, String)]) -> bool {
    let named = |name: &str| attrs.iter().any(|(k, v)| k == "name" && v == name);
    (tag == "input" && named("authenticity_token")) || (tag == "meta" && (named("csrf-token") || named("csrf-param")))
}

/// The normalized token lines of an HTML document or fragment.
pub fn normalize_html(html: &str) -> Vec<String> {
    let sink = Sink::default();
    let tokenizer = Tokenizer::new(sink, TokenizerOpts::default());
    let input = BufferQueue::default();
    input.push_back(StrTendril::from_slice(html));
    let _ = tokenizer.feed(&input);
    tokenizer.end();
    tokenizer.sink.flush_text();
    tokenizer.sink.lines.take()
}

/// A readable report of the first differences between two normalized documents, or `None`.
pub fn diff(expected: &[String], actual: &[String]) -> Option<String> {
    if expected == actual {
        return None;
    }
    let first = expected.iter().zip(actual).position(|(e, a)| e != a).unwrap_or(expected.len().min(actual.len()));
    let from = first.saturating_sub(4);
    let mut report = format!("first difference at token {first} (expected {} tokens, got {})\n", expected.len(), actual.len());
    for index in from..(first + 6) {
        let e = expected.get(index).map(String::as_str).unwrap_or("<end>");
        let a = actual.get(index).map(String::as_str).unwrap_or("<end>");
        let marker = if e == a { " " } else { "!" };
        report.push_str(&format!("{marker} rails: {e}\n{marker} rust:  {a}\n"));
    }
    Some(report)
}
