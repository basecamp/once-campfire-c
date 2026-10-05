//! Shared by the views-B golden tests (`messages_*`, `rooms_*`, `searches_*`): loads a golden
//! from `tests/golden/b` (written by `reference-tools/views/b/run.sh`), builds the
//! `ViewContext` the reference request had, and compares our HTML with the reference as the
//! canonical token stream that `reference-tools/views/b/canonical.rb` produces.
#![allow(dead_code)]

use std::collections::HashMap;
use std::path::PathBuf;

use campfire_views::{AccountSummary, CurrentUser, Platform, ViewContext};
use serde::de::DeserializeOwned;
use serde_json::Value;

pub struct Golden {
    pub name: String,
    pub kind: String,
    pub json: Value,
    assets: HashMap<String, String>,
}

pub fn golden(name: &str) -> Golden {
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/golden/b").join(format!("{name}.json"));
    let json: Value =
        serde_json::from_str(&std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("{path:?}: {e}"))).expect("golden is JSON");
    let assets = serde_json::from_value(json["context"]["assets"].clone()).unwrap_or_default();
    Golden { name: name.to_string(), kind: json["kind"].as_str().unwrap().to_string(), json, assets }
}

impl Golden {
    pub fn input<T: DeserializeOwned>(&self) -> T {
        serde_json::from_value(self.json["input"].clone()).unwrap_or_else(|e| panic!("{}: input: {e}", self.name))
    }

    pub fn input_at<T: DeserializeOwned>(&self, key: &str) -> T {
        serde_json::from_value(self.json["input"][key].clone()).unwrap_or_else(|e| panic!("{}: input.{key}: {e}", self.name))
    }

    /// Runs `render` with the reference request's context.
    pub fn render(&self, render: impl FnOnce(&ViewContext) -> String) -> String {
        let context = &self.json["context"];
        let asset_path =
            |logical: &str| self.assets.get(logical).cloned().unwrap_or_else(|| panic!("{}: unknown asset {logical}", self.name));
        let current_user = context["current_user"].as_object().map(|user| CurrentUser {
            id: user["id"].as_i64().unwrap(),
            name: user["name"].as_str().unwrap().to_string(),
            administrator: user["administrator"].as_bool().unwrap(),
            bot: user["bot"].as_bool().unwrap(),
            avatar_url: user["avatar_url"].as_str().unwrap().to_string(),
        });
        let p = &context["platform"];
        let flag = |key: &str| p[key].as_bool().unwrap_or(false);
        let platform = Platform {
            ios: flag("ios"),
            android: flag("android"),
            mac: flag("mac"),
            windows: flag("windows"),
            chrome: flag("chrome"),
            firefox: flag("firefox"),
            safari: flag("safari"),
            edge: flag("edge"),
            mobile: flag("mobile"),
            desktop: flag("desktop"),
            apple_messages: flag("apple_messages"),
            browser: p["browser"].as_str().unwrap_or_default().to_string(),
            operating_system: p["operating_system"].as_str().unwrap_or_default().to_string(),
        };
        let ctx = ViewContext {
            current_user,
            account: AccountSummary {
                name: context["account"]["name"].as_str().unwrap().to_string(),
                logo_url: context["account"]["logo_url"].as_str().unwrap().to_string(),
                has_logo: context["account"]["has_logo"].as_bool().unwrap(),
            },
            flash_notice: None,
            flash_alert: None,
            platform,
            vapid_public_key: None,
            asset_path: &asset_path,
            // Marks where the layout ends and the page's head block starts (see `head_block`).
            importmap_tags: "<script type=\"module\">import \"application\"</script>",
            stylesheet_tags: "",
            custom_styles: None,
            cable_url: "/cable".to_string(),
            base_url: context["base_url"].as_str().unwrap().to_string(),
            request_url: String::new(),
            referrer: None,
            last_room_visited_id: context["last_room_visited_id"].as_i64(),
            app_version: "0".to_string(),
        };
        render(&ctx)
    }

    /// Asserts DOM parity. Pages compare the regions the page fills (title, the head block,
    /// nav, main content, footer, sidebar); fragments compare everything.
    /// Asserts DOM parity for a template rendered without a layout against a reference page:
    /// the page's main content (or a frame layout's body) is compared with the whole render.
    pub fn assert_content(&self, actual_html: &str) {
        let expected = without_forgery_tokens(serde_json::from_value(self.json["expected"].clone()).unwrap());
        let expected = if self.kind == "page" {
            let regions = regions_named(&expected, &["main"]);
            regions.into_iter().next().map(|(_, tokens)| tokens).unwrap_or_default()
        } else {
            expected
        };
        assert_same(&self.name, &trim_whitespace(expected), &trim_whitespace(tokens(actual_html)));
    }

    pub fn assert_dom(&self, actual_html: &str) {
        let expected = without_forgery_tokens(serde_json::from_value(self.json["expected"].clone()).unwrap());
        let actual = tokens(actual_html);
        if self.kind == "page" {
            let regions = regions(&expected);
            assert!(!regions.is_empty(), "{}: no regions in reference", self.name);
            let actual_regions = regions_named(&actual, &regions.iter().map(|(n, _)| *n).collect::<Vec<_>>());
            for ((name, expected), (_, actual)) in regions.iter().zip(actual_regions.iter()) {
                assert_same(&format!("{} [{name}]", self.name), expected, actual);
            }
        } else {
            assert_same(&self.name, &trim_whitespace(expected), &trim_whitespace(actual));
        }
    }
}

/// The reference's token stream without the CSRF tags and fields Rails renders (this app has none),
/// with the text around a dropped tag merged as the tokenizer would have merged it.
fn without_forgery_tokens(expected: Vec<String>) -> Vec<String> {
    let is_token = |t: &str| {
        (t.starts_with("<input ") && t.contains(r#"name="authenticity_token""#))
            || (t.starts_with("<meta ") && (t.contains(r#"name="csrf-token""#) || t.contains(r#"name="csrf-param""#)))
    };
    let mut out = Vec::with_capacity(expected.len());
    for token in expected.into_iter().filter(|t| !is_token(t)).map(with_relative_copy_link) {
        match token.strip_prefix('#') {
            Some(text) => push_text(&mut out, text),
            None => out.push(token),
        }
    }
    out
}

/// A message's "Copy link" button carries the message's path, which the browser makes absolute,
/// rather than an absolute URL built from the request's host (README, Known differences).
fn with_relative_copy_link(token: String) -> String {
    const ABSOLUTE: &str = "data-copy-to-clipboard-content-value=\"http";
    let Some(start) = token.find(ABSOLUTE).filter(|_| token.contains("title=\"Copy link\"")) else { return token };
    let value_start = start + "data-copy-to-clipboard-content-value=\"".len();
    let value_end = value_start + token[value_start..].find('"').unwrap();
    let url = &token[value_start..value_end];
    let path = &url[url.find("://").unwrap() + 3..];
    let path = &path[path.find('/').unwrap()..];
    format!("{}data-copy-to-clipboard-url-value=\"{path}\"{}", &token[..start], &token[value_end + 1..])
}

fn assert_same(label: &str, expected: &[String], actual: &[String]) {
    if expected == actual {
        return;
    }
    let index = expected.iter().zip(actual.iter()).position(|(e, a)| e != a).unwrap_or(expected.len().min(actual.len()));
    let from = index.saturating_sub(4);
    let show = |tokens: &[String]| tokens[from.min(tokens.len())..(index + 6).min(tokens.len())].join("\n    ");
    let dir = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../target/views-b-diff");
    let _ = std::fs::create_dir_all(&dir);
    let file = label.replace([' ', '[', ']'], "_");
    let _ = std::fs::write(dir.join(format!("{file}.expected")), expected.join("\n"));
    let _ = std::fs::write(dir.join(format!("{file}.actual")), actual.join("\n"));
    panic!(
        "{label}: DOM differs at token {index} (expected {} tokens, got {})\n  expected:\n    {}\n  actual:\n    {}\n  full streams in target/views-b-diff/{file}.*",
        expected.len(),
        actual.len(),
        show(expected),
        show(actual)
    );
}

type Region<'a> = (&'static str, Vec<String>);

const REGION_NAMES: [&str; 6] = ["title", "head", "nav", "main", "footer", "sidebar"];

fn regions(tokens: &[String]) -> Vec<Region<'_>> {
    regions_named(tokens, &REGION_NAMES)
}

fn regions_named<'a>(tokens: &'a [String], names: &[&'static str]) -> Vec<Region<'a>> {
    let has_main = tokens.iter().any(|t| t.starts_with("<main id=\"main-content\""));
    if !has_main {
        // A turbo-frame request's minimal layout: compare the body.
        return vec![("body", trim_whitespace(element_children(tokens, |t| t.starts_with("<body"))))];
    }
    names
        .iter()
        .map(|&name| {
            let region = match name {
                "title" => element_children(tokens, |t| t == "<title>"),
                "head" => head_block(tokens),
                "nav" => element_children(tokens, |t| t.starts_with("<nav id=\"nav\"")),
                "main" => {
                    let mut main = element_children(tokens, |t| t.starts_with("<main id=\"main-content\""));
                    if let Some(footer) = main.iter().position(|t| t.starts_with("<footer id=\"footer\"")) {
                        main.truncate(footer);
                    }
                    main
                }
                "footer" => element_children(tokens, |t| t.starts_with("<footer id=\"footer\"")),
                "sidebar" => element_children(tokens, |t| t.starts_with("<aside id=\"sidebar\"")),
                _ => unreachable!(),
            };
            (name, trim_whitespace(region))
        })
        .collect()
}

/// The page's `yield :head`: everything in head after the importmap's module script.
fn head_block(tokens: &[String]) -> Vec<String> {
    let head = element_children(tokens, |t| t == "<head>");
    let start = head.iter().rposition(|t| t.starts_with("<script type=\"module\"")).map(|i| i + 3).unwrap_or(head.len());
    head[start.min(head.len())..].to_vec()
}

fn trim_whitespace(mut tokens: Vec<String>) -> Vec<String> {
    while tokens.first().is_some_and(|t| t == "# ") {
        tokens.remove(0);
    }
    while tokens.last().is_some_and(|t| t == "# ") {
        tokens.pop();
    }
    tokens
}

fn element_children(tokens: &[String], is_start: impl Fn(&str) -> bool) -> Vec<String> {
    let Some(start) = tokens.iter().position(|t| is_start(t)) else { return Vec::new() };
    let name = tag_name(&tokens[start]);
    let mut depth = 0usize;
    for (offset, token) in tokens[start + 1..].iter().enumerate() {
        if token.starts_with('<') && !token.starts_with("</") && tag_name(token) == name && !VOID.contains(&name.as_str()) {
            depth += 1;
        } else if token == &format!("</{name}>") {
            if depth == 0 {
                return tokens[start + 1..start + 1 + offset].to_vec();
            }
            depth -= 1;
        }
    }
    tokens[start + 1..].to_vec()
}

fn tag_name(token: &str) -> String {
    token.trim_start_matches("</").trim_start_matches('<').split([' ', '>']).next().unwrap_or_default().to_string()
}

const VOID: [&str; 14] = ["area", "base", "br", "col", "embed", "hr", "img", "input", "keygen", "link", "meta", "source", "track", "wbr"];
const RAW_TEXT: [&str; 4] = ["script", "style", "textarea", "title"];

/// Tokenizes well-formed HTML (our own output) into the canonical stream.
pub fn tokens(html: &str) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    let bytes = html.as_bytes();
    let mut i = 0;
    let mut text = String::new();
    // Open elements, so that an end tag closes any elements left open inside it, as the HTML
    // parser does (Rails' blockless `form_with` leaves its form open).
    let mut open: Vec<String> = Vec::new();
    let flush = |text: &mut String, out: &mut Vec<String>| {
        if !text.is_empty() {
            push_text(out, &decode(text));
            text.clear();
        }
    };
    while i < bytes.len() {
        if bytes[i] == b'<' {
            if html[i..].starts_with("<!--") {
                flush(&mut text, &mut out);
                i = html[i..].find("-->").map(|e| i + e + 3).unwrap_or(bytes.len());
                continue;
            }
            if html[i..].starts_with("<!") {
                flush(&mut text, &mut out);
                i = html[i..].find('>').map(|e| i + e + 1).unwrap_or(bytes.len());
                continue;
            }
            if html[i..].starts_with("</") {
                flush(&mut text, &mut out);
                let end = html[i..].find('>').map(|e| i + e).unwrap();
                let name = html[i + 2..end].split_whitespace().next().unwrap_or_default().to_ascii_lowercase();
                if let Some(depth) = open.iter().rposition(|n| *n == name) {
                    for closed in open.drain(depth..).rev() {
                        out.push(format!("</{closed}>"));
                    }
                }
                i = end + 1;
                continue;
            }
            if bytes.get(i + 1).is_some_and(|b| b.is_ascii_alphabetic()) {
                flush(&mut text, &mut out);
                let (token, name, end) = start_tag(html, i);
                if !token.is_empty() {
                    out.push(token);
                }
                i = end;
                if !VOID.contains(&name.as_str()) {
                    open.push(name.clone());
                }
                if RAW_TEXT.contains(&name.as_str()) {
                    let close = format!("</{name}");
                    let stop = html[i..].to_ascii_lowercase().find(&close).map(|e| i + e).unwrap_or(bytes.len());
                    let raw = &html[i..stop];
                    if !raw.is_empty() {
                        push_text(&mut out, &if name == "script" || name == "style" { raw.to_string() } else { decode(raw) });
                    }
                    i = stop;
                } else if VOID.contains(&name.as_str()) {
                    // No end token.
                }
                continue;
            }
        }
        let next = html[i..].find('<').map(|e| i + e.max(1)).unwrap_or(bytes.len());
        text.push_str(&html[i..next]);
        i = next;
    }
    flush(&mut text, &mut out);
    for closed in open.drain(..).rev() {
        out.push(format!("</{closed}>"));
    }
    out.retain(|t| t != "#");
    out
}

fn push_text(out: &mut Vec<String>, text: &str) {
    let collapsed = collapse(text);
    if let Some(last) = out.last_mut().filter(|t| t.starts_with('#')) {
        let merged = collapse(&format!("{}{}", &last[1..], collapsed));
        *last = format!("#{merged}");
    } else {
        out.push(format!("#{collapsed}"));
    }
}

fn collapse(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    let mut in_space = false;
    for c in text.chars() {
        if matches!(c, ' ' | '\t' | '\n' | '\r' | '\x0c') {
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

fn start_tag(html: &str, start: usize) -> (String, String, usize) {
    let bytes = html.as_bytes();
    let mut i = start + 1;
    let name_end = html[i..].find(|c: char| c.is_whitespace() || c == '>' || c == '/').map(|e| i + e).unwrap();
    let name = html[i..name_end].to_ascii_lowercase();
    i = name_end;
    let mut attrs: Vec<(String, String)> = Vec::new();
    loop {
        while i < bytes.len() && (bytes[i].is_ascii_whitespace() || bytes[i] == b'/') {
            i += 1;
        }
        if i >= bytes.len() || bytes[i] == b'>' {
            i += 1;
            break;
        }
        let key_end = html[i..].find(|c: char| c.is_whitespace() || c == '=' || c == '>').map(|e| i + e).unwrap();
        let key = html[i..key_end].to_ascii_lowercase();
        i = key_end;
        while bytes[i].is_ascii_whitespace() {
            i += 1;
        }
        let value = if bytes[i] == b'=' {
            i += 1;
            while bytes[i].is_ascii_whitespace() {
                i += 1;
            }
            if bytes[i] == b'"' || bytes[i] == b'\'' {
                let quote = bytes[i] as char;
                let end = html[i + 1..].find(quote).map(|e| i + 1 + e).unwrap();
                let value = decode(&html[i + 1..end]);
                i = end + 1;
                value
            } else {
                let end = html[i..].find(|c: char| c.is_whitespace() || c == '>').map(|e| i + e).unwrap();
                let value = decode(&html[i..end]);
                i = end;
                value
            }
        } else {
            String::new()
        };
        if !attrs.iter().any(|(k, _)| *k == key) {
            attrs.push((key, value));
        }
    }
    // Rails renders forgery tokens; this app doesn't (forgery protection is by `Sec-Fetch-Site`).
    let named = |value: &str| attrs.iter().any(|(k, v)| k == "name" && v == value);
    if (name == "input" && named("authenticity_token")) || (name == "meta" && (named("csrf-token") || named("csrf-param"))) {
        return (String::new(), name, i);
    }
    let rendered: String =
        attrs.iter().map(|(k, v)| format!(" {k}=\"{}\"", v.replace('&', "&amp;").replace('"', "&quot;").replace('<', "&lt;"))).collect();
    (format!("<{name}{rendered}>"), name, i)
}

fn decode(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    let mut rest = text;
    while let Some(amp) = rest.find('&') {
        out.push_str(&rest[..amp]);
        rest = &rest[amp..];
        let Some(semi) = rest[..rest.len().min(12)].find(';') else {
            out.push('&');
            rest = &rest[1..];
            continue;
        };
        let entity = &rest[1..semi];
        let decoded = match entity {
            "amp" => Some('&'),
            "lt" => Some('<'),
            "gt" => Some('>'),
            "quot" => Some('"'),
            "apos" => Some('\''),
            "nbsp" => Some('\u{a0}'),
            _ if entity.starts_with("#x") || entity.starts_with("#X") => {
                u32::from_str_radix(&entity[2..], 16).ok().and_then(char::from_u32)
            }
            _ if entity.starts_with('#') => entity[1..].parse().ok().and_then(char::from_u32),
            _ => None,
        };
        match decoded {
            Some(c) => {
                out.push(c);
                rest = &rest[semi + 1..];
            }
            None => {
                out.push('&');
                rest = &rest[1..];
            }
        }
    }
    out.push_str(rest);
    out
}
