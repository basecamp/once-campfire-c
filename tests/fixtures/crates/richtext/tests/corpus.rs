//! Differential test against the Rails pipeline: tests/corpus/expected.json is produced by
//! reference-tools/richtext/run.sh in the campfire-reference image. Every output is also checked
//! against security properties that don't depend on the oracle.

use std::collections::BTreeMap;

use campfire_richtext::dom::Dom;
use campfire_richtext::sanitizer::SafeList;
use campfire_richtext::{
    AttachableResolver, GidLookup, MentionUser, Presentation, RenderContext, SignedLookup, editable_value, mentioned_users,
    present_message, to_plain_text,
};
use serde_json::Value;

struct Corpus {
    json: Value,
}

impl Corpus {
    fn load() -> Corpus {
        // RICHTEXT_CORPUS points at a larger, uncommitted corpus (see reference-tools/richtext/run.sh)
        let path = std::env::var("RICHTEXT_CORPUS")
            .unwrap_or_else(|_| concat!(env!("CARGO_MANIFEST_DIR"), "/tests/corpus/expected.json").to_string());
        let text = std::fs::read_to_string(&path).expect("run reference-tools/richtext/run.sh to generate the corpus");
        Corpus { json: serde_json::from_str(&text).unwrap() }
    }

    fn users(&self) -> Vec<MentionUser> {
        self.json["users"]
            .as_array()
            .unwrap()
            .iter()
            .map(|u| MentionUser {
                id: u["id"].as_i64().unwrap(),
                name: u["name"].as_str().unwrap().into(),
                title: u["title"].as_str().unwrap().into(),
                attachable_sgid: u["attachable_sgid"].as_str().unwrap().into(),
                user_path: u["user_path"].as_str().unwrap().into(),
                avatar_path: u["avatar_path"].as_str().unwrap().into(),
            })
            .collect()
    }
}

/// Stands in for the app: SGIDs Rails minted are "verified" by exact match, and GIDs are looked
/// up by model and id.
struct TestResolver {
    users: Vec<MentionUser>,
    rooms: Vec<i64>,
    signed: Vec<(String, String, i64, bool)>,
}

impl TestResolver {
    fn from(corpus: &Corpus) -> Self {
        TestResolver {
            users: corpus.users(),
            rooms: corpus.json["rooms"].as_array().unwrap().iter().map(|r| r.as_i64().unwrap()).collect(),
            signed: corpus.json["signed"]
                .as_array()
                .unwrap()
                .iter()
                .map(|s| {
                    (
                        s["sgid"].as_str().unwrap().into(),
                        s["model"].as_str().unwrap().into(),
                        s["id"].as_i64().unwrap(),
                        s["exists"].as_bool().unwrap(),
                    )
                })
                .collect(),
        }
    }
}

impl AttachableResolver for TestResolver {
    fn locate_signed(&self, sgid: &str) -> SignedLookup {
        match self.signed.iter().find(|(s, ..)| s == sgid) {
            Some((_, model, id, true)) if model == "User" => SignedLookup::User(self.users.iter().find(|u| u.id == *id).unwrap().clone()),
            Some((_, model, _, _)) => SignedLookup::MissingRecord { model_name: model.clone() },
            None => SignedLookup::Invalid,
        }
    }

    fn find_gid(&self, gid: &str) -> GidLookup {
        // GlobalID's default locator ignores the app name
        let Some(rest) = gid.strip_prefix("gid://") else { return GidLookup::NotFound };
        if !gid.is_ascii() {
            return GidLookup::NotFound;
        }
        let Some((_app, rest)) = rest.split_once('/') else { return GidLookup::NotFound };
        let rest = rest.split('?').next().unwrap();
        let Some((model, id)) = rest.split_once('/') else { return GidLookup::NotFound };
        let Ok(id) = id.parse::<i64>() else { return GidLookup::NotFound };
        match model {
            "User" => self.users.iter().find(|u| u.id == id).cloned().map_or(GidLookup::NotFound, GidLookup::User),
            "Room" if self.rooms.contains(&id) => GidLookup::OtherModel,
            _ => GidLookup::NotFound,
        }
    }
}

/// A DOM normalization for reporting near misses: parse, drop whitespace-only text, sort attributes.
fn normalized_dom(html: &str) -> String {
    let mut dom = Dom::new();
    let Ok(root) = dom.parse_fragment(html) else { return format!("unparseable: {html}") };
    let mut out = String::new();
    normalize_into(&dom, root, &mut out);
    out
}

fn normalize_into(dom: &Dom, node: usize, out: &mut String) {
    for &child in dom.children(node) {
        if let Some(text) = dom.text(child) {
            let collapsed = text.split_whitespace().collect::<Vec<_>>().join(" ");
            if !collapsed.is_empty() {
                out.push_str(&collapsed);
            }
        } else if let Some(name) = dom.local_name(child) {
            let mut attrs = dom.attrs(child);
            attrs.sort();
            out.push_str(&format!("<{name} {attrs:?}>"));
            normalize_into(dom, child, out);
            out.push_str(&format!("</{name}>"));
        }
    }
}

// --- Deliberate differences ----------------------------------------------------------------------

/// Rails' presentation as the port renders it on purpose (see "Known differences" in README.md):
/// `<` and `>` are escaped in attribute values, and the links rails_autolink inserted inside an
/// attribute value (its stored XSS) are left as the text they replaced; `name` attributes are dropped.
fn with_port_divergences(rails: &str) -> String {
    const INSERTED_LINK: &str = "<a target=\"_blank\" href=\"";
    enum State {
        Text,
        Tag,
        Value,
    }
    let mut out = String::with_capacity(rails.len());
    let mut state = State::Text;
    let mut rest = rails;
    while let Some(c) = rest.chars().next() {
        if matches!(state, State::Value) && rest.starts_with(INSERTED_LINK) {
            let text_start = rest.find("\">").unwrap() + 2;
            let text_end = text_start + rest[text_start..].find("</a>").unwrap();
            out.push_str(&rest[text_start..text_end].replace('>', "&gt;"));
            rest = &rest[text_end + 4..];
            continue;
        }
        if matches!(state, State::Tag) && rest.starts_with(" name=\"") {
            let value_end = " name=\"".len() + rest[" name=\"".len()..].find('"').unwrap();
            rest = &rest[value_end + 1..];
            continue;
        }
        match (&state, c) {
            (State::Text, '<') => state = State::Tag,
            (State::Tag, '>') => state = State::Text,
            (State::Tag, '"') => state = State::Value,
            (State::Value, '"') => state = State::Tag,
            _ => {}
        }
        match (&state, c) {
            (State::Value, '<') => out.push_str("&lt;"),
            (State::Value, '>') => out.push_str("&gt;"),
            _ => out.push(c),
        }
        rest = &rest[c.len_utf8()..];
    }
    out
}

#[test]
fn port_divergences_apply_to_attribute_values_only() {
    assert_eq!(
        with_port_divergences(
            "<p title=\"a>b <a target=\"_blank\" href=\"http://x.test/\">http://x.test/</a>\">c > <a target=\"_blank\" href=\"http://y.test/\">y</a></p>"
        ),
        "<p title=\"a&gt;b http://x.test/\">c > <a target=\"_blank\" href=\"http://y.test/\">y</a></p>"
    );
    assert_eq!(with_port_divergences("<a name=\"x y\" title=\"name=\">n</a>"), "<a title=\"name=\">n</a>");
}

// --- Security assertions -------------------------------------------------------------------------

const DANGEROUS_ELEMENTS: &[&str] = &[
    "script", "style", "iframe", "frame", "frameset", "object", "embed", "applet", "base", "meta", "link", "form", "input", "button",
    "textarea", "select", "svg", "math", "template", "noscript", "xmp", "plaintext", "noembed",
];

const URL_ATTRIBUTES: &[&str] = &["href", "src", "action", "formaction", "poster", "cite", "background", "xlink:href", "srcset", "data"];

fn dangerous_url(value: &str) -> bool {
    // What a browser would see: control characters and whitespace ignored, case folded
    let cleaned: String = value.chars().filter(|c| !c.is_control() && !c.is_whitespace()).collect::<String>().to_lowercase();
    if cleaned.starts_with("javascript:") || cleaned.starts_with("vbscript:") || cleaned.starts_with("livescript:") {
        return true;
    }
    // A data: URL is only dangerous as markup or script; an unparseable media type is text/plain
    if let Some(rest) = cleaned.strip_prefix("data:") {
        let mediatype = rest.split(',').next().unwrap_or("").split(';').next().unwrap_or("");
        let token = |s: &str| !s.is_empty() && s.chars().all(|c| c.is_ascii_alphanumeric() || "!#$%&'*+-.^_`|~".contains(c));
        let valid = mediatype.split_once('/').is_some_and(|(t, sub)| token(t) && token(sub));
        return valid && ["html", "xml", "svg", "script", "ecmascript"].iter().any(|kind| mediatype.contains(kind));
    }
    false
}

/// Parses rendered output the way a browser would and returns the security violations in it.
fn security_violations(html: &str, allow_style: bool) -> Vec<String> {
    let mut dom = Dom::new();
    let Ok(root) = dom.parse_fragment(html) else { return vec!["unparseable output".into()] };
    let mut violations = Vec::new();
    for node in dom.descendants(root) {
        let Some(name) = dom.local_name(node) else { continue };
        if DANGEROUS_ELEMENTS.contains(&name) {
            violations.push(format!("<{name}> element"));
        }
        // Nothing auto_link inserts may escape its own sanitizer's allowlist
        let allowed = SafeList::auto_link();
        if !allowed.tags.contains(&name) {
            violations.push(format!("<{name}> not in the allowlist"));
        }
        for (attr, value) in dom.attrs(node) {
            if !allowed.attributes.contains(&attr.as_str()) && attr != "target" && !(attr == "style" && allow_style) {
                violations.push(format!("{attr} on <{name}> not in the allowlist"));
            }
            let lower = attr.to_lowercase();
            if lower.starts_with("on") {
                violations.push(format!("{attr} attribute on <{name}>"));
            }
            if URL_ATTRIBUTES.contains(&lower.as_str()) && dangerous_url(&value) {
                violations.push(format!("{attr}={value:?} on <{name}>"));
            }
            if lower == "style" && !allow_style {
                violations.push(format!("style on <{name}>"));
            }
            if lower.starts_with("data-") && lower != "data-language" {
                violations.push(format!("{attr} on <{name}>"));
            }
        }
    }
    violations
}

// --- The comparison ------------------------------------------------------------------------------

#[derive(Default)]
struct Tally {
    exact: usize,
    dom_equal: usize,
    mismatched: Vec<String>,
}

impl Tally {
    fn record(&mut self, label: String, expected: &str, actual: &str) {
        if expected == actual {
            self.exact += 1;
        } else if normalized_dom(expected) == normalized_dom(actual) {
            self.dom_equal += 1;
            self.mismatched.push(format!("{label} (DOM-equal)\n  expected: {expected:?}\n  actual:   {actual:?}"));
        } else {
            self.mismatched.push(format!("{label}\n  expected: {expected:?}\n  actual:   {actual:?}"));
        }
    }
}

fn outcome_str(v: &Value) -> Result<Option<String>, String> {
    if let Some(e) = v.get("error") {
        return Err(e.as_str().unwrap().to_string());
    }
    Ok(v["ok"].as_str().map(str::to_string))
}

fn render<T: std::fmt::Debug>(r: &Result<T, campfire_richtext::Error>) -> String {
    format!("{r:?}")
}

#[test]
fn corpus_matches_rails() {
    let corpus = Corpus::load();
    let resolver = TestResolver::from(&corpus);
    let mut tallies: BTreeMap<&str, Tally> = BTreeMap::new();
    let mut security = Vec::new();
    let cases = corpus.json["cases"].as_array().unwrap();

    for case in cases {
        let name = case["name"].as_str().unwrap();
        let body = case["body"].as_str().unwrap();
        let ctx = RenderContext { resolver: &resolver, request_host: case["host"].as_str().map(str::to_string) };

        // presentation
        let expected = match outcome_str(&case["presentation"]) {
            Ok(html) => Presentation::Html(with_port_divergences(&html.unwrap_or_default())),
            Err(_) => Presentation::Unrenderable,
        };
        let actual = present_message(body, &ctx);
        let label = format!("[presentation] {name}");
        // Deliberate: a missing attachable Rails can't find a partial for (a deleted user's
        // mention) renders ☒ instead of raising and blanking the message.
        let missing_partial =
            case["presentation_raised_message"].as_str().is_some_and(|m| m.contains("to_missing_attachable_partial_path"));
        match (&expected, &actual) {
            (Presentation::Html(_), Presentation::Html(a)) if missing_partial => {
                assert!(a.contains('☒'), "{label}: {a}");
                tallies.entry("presentation").or_default().exact += 1;
            }
            (Presentation::Html(e), Presentation::Html(a)) => tallies.entry("presentation").or_default().record(label, e, a),
            _ => tallies.entry("presentation").or_default().record(label, &format!("{expected:?}"), &format!("{actual:?}")),
        }
        if let Presentation::Html(html) = &actual {
            for v in security_violations(html, false) {
                security.push(format!("{name}: {v}"));
            }
        }

        // plain text
        let actual = to_plain_text(body, &ctx);
        let label = format!("[plain_text] {name}");
        match (outcome_str(&case["plain_text"]), &actual) {
            (Ok(e), Ok(a)) => tallies.entry("plain_text").or_default().record(label, &e.unwrap_or_default(), a),
            (Err(_), Err(_)) => tallies.entry("plain_text").or_default().exact += 1,
            (e, _) => tallies.entry("plain_text").or_default().record(label, &format!("{e:?}"), &render(&actual)),
        }

        // editable value
        let actual = editable_value(body, &ctx);
        let label = format!("[editable] {name}");
        // Deliberate: missing attachables leave the editor, where Rails raises.
        let missing_in_editor = case["editable"]["message"].as_str().is_some_and(|m| m.contains("MissingAttachable"));
        match (outcome_str(&case["editable"]), &actual) {
            (Err(_), Ok(_)) if missing_in_editor => tallies.entry("editable").or_default().exact += 1,
            (Ok(e), Ok(a)) => tallies.entry("editable").or_default().record(label, &format!("{e:?}"), &format!("{a:?}")),
            (Err(_), Err(_)) => tallies.entry("editable").or_default().exact += 1,
            (e, _) => tallies.entry("editable").or_default().record(label, &format!("{e:?}"), &render(&actual)),
        }

        // mentioned users
        let actual = mentioned_users(body, &ctx).map(|users| users.iter().map(|u| u.id).collect::<Vec<_>>());
        let expected = match case["mentioned"].get("ok") {
            Some(ids) => Ok(ids.as_array().unwrap().iter().map(|i| i.as_i64().unwrap()).collect::<Vec<_>>()),
            None => Err(()),
        };
        let label = format!("[mentioned] {name}");
        match (&expected, &actual) {
            (Ok(e), Ok(a)) => tallies.entry("mentioned").or_default().record(label, &format!("{e:?}"), &format!("{a:?}")),
            (Err(_), Err(_)) => tallies.entry("mentioned").or_default().exact += 1,
            _ => tallies.entry("mentioned").or_default().record(label, &format!("{expected:?}"), &format!("{actual:?}")),
        }
    }

    // opengraph URL checks
    for url in corpus.json["web_urls"].as_array().unwrap() {
        let value = url["value"].as_str().unwrap();
        let host = url["host"].as_str().unwrap();
        let actual = campfire_richtext::attachables::web_url(Some(value), host);
        let label = format!("[web_url] {value:?}");
        match (outcome_str(&url["result"]), &actual) {
            (Ok(e), Ok(a)) => tallies.entry("web_url").or_default().record(label, &format!("{e:?}"), &format!("{a:?}")),
            (Err(_), Err(_)) => tallies.entry("web_url").or_default().exact += 1,
            (e, _) => tallies.entry("web_url").or_default().record(label, &format!("{e:?}"), &render(&actual)),
        }
    }

    let mut failed = false;
    for (kind, tally) in &tallies {
        let total = tally.exact + tally.mismatched.len();
        println!(
            "{kind}: {total} cases, {} byte-identical, {} DOM-equal only, {} different",
            tally.exact,
            tally.dom_equal,
            tally.mismatched.len() - tally.dom_equal
        );
        for m in &tally.mismatched {
            println!("  MISMATCH {m}");
            failed = true;
        }
    }
    println!("security: {} outputs checked, {} violations", cases.len(), security.len());
    for v in &security {
        println!("  VIOLATION {v}");
    }
    assert!(security.is_empty(), "security assertions failed");
    assert!(!failed, "differences from the Rails pipeline");
}

/// The oracle's own outputs must pass the security assertions too: two implementations can agree
/// on something unsafe. (They do: rails_autolink breaks out of attribute values, which is why the
/// port diverges there, so the assertions run on the output as the port means to render it.)
#[test]
fn rails_outputs_pass_security_assertions() {
    let corpus = Corpus::load();
    let mut violations = Vec::new();
    for case in corpus.json["cases"].as_array().unwrap() {
        if let Ok(Some(html)) = outcome_str(&case["presentation"]) {
            for v in security_violations(&with_port_divergences(&html), false) {
                violations.push(format!("{}: {v}", case["name"].as_str().unwrap()));
            }
        }
    }
    for v in &violations {
        println!("  VIOLATION {v}");
    }
    assert!(violations.is_empty());
}

/// The security gate has to catch what it's for, or passing it means nothing.
#[test]
fn security_assertions_catch_planted_defects() {
    for (html, what) in [
        ("<script>alert(1)</script>", "script element"),
        ("<p onclick=\"x()\">p</p>", "event handler"),
        ("<a href=\"java\tscript:alert(1)\">x</a>", "javascript URL"),
        ("<a href=\" JAVASCRIPT:alert(1)\">x</a>", "javascript URL"),
        ("<img src=\"data:text/html,<script>\">", "data URL"),
        ("<span style=\"color: red\">x</span>", "style attribute"),
        ("<svg><a xlink:href=\"javascript:1\">x</a></svg>", "svg"),
        ("<span data-controller=\"x\">x</span>", "data attribute"),
        ("<p title=\"a\" _blank\"=\"\">x</p>", "attribute outside the allowlist"),
        ("<details>x</details>", "element outside the allowlist"),
    ] {
        assert!(!security_violations(html, false).is_empty(), "missed {what} in {html}");
    }
    assert!(security_violations("<p><a href=\"https://example.com\">x</a><img src=\"/a.png\"></p>", false).is_empty());
}
