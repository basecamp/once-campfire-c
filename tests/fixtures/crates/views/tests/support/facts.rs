//! Loads `tests/golden/a/facts.json` (written by reference-tools/views/a/render.rb) and builds
//! the `ViewContext` a case was rendered with.

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::OnceLock;

use campfire_views::{AccountSummary, CurrentUser, Platform, ViewContext};
use serde_json::Value;

pub fn golden_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/golden/a")
}

pub fn facts() -> &'static Value {
    static FACTS: OnceLock<Value> = OnceLock::new();
    FACTS.get_or_init(|| {
        let json = std::fs::read_to_string(golden_dir().join("facts.json")).expect("facts.json; run reference-tools/views/a/render.rb");
        serde_json::from_str(&json).unwrap()
    })
}

pub fn golden(name: &str, ext: &str) -> String {
    std::fs::read_to_string(golden_dir().join(format!("{name}.{ext}"))).unwrap()
}

pub fn case(name: &str) -> &'static Value {
    &facts()["cases"][name]
}

pub fn str_of(value: &Value) -> Option<String> {
    value.as_str().map(str::to_string)
}

/// The facts for the user named `name` as of case `case_name`.
pub fn user(case_name: &str, name: &str) -> &'static Value {
    let user = &case(case_name)["users"][name];
    assert!(!user.is_null(), "no user {name} in case {case_name}");
    user
}

pub fn user_by_email(case_name: &str, email: &str) -> &'static Value {
    case(case_name)["users"]
        .as_object()
        .unwrap()
        .values()
        .find(|user| user["email_address"].as_str() == Some(email))
        .unwrap_or_else(|| panic!("no user {email}"))
}

pub fn platform(label: &str) -> Platform {
    let p = &facts()["platforms"][label];
    let flag = |name: &str| p[name].as_bool().unwrap();
    Platform {
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
        browser: p["browser"].as_str().unwrap_or("").to_string(),
        operating_system: p["operating_system"].as_str().unwrap_or("").to_string(),
    }
}

/// Per-case request facts that aren't in facts.json.
#[derive(Default)]
pub struct Request {
    /// Rendered by `ApplicationController.renderer`, outside a request.
    pub partial: bool,
    pub flash_notice: Option<String>,
    pub flash_alert: Option<String>,
}

/// Runs `f` with the `ViewContext` the reference app had for case `name`.
pub fn with_context<R>(name: &str, request: Request, f: impl FnOnce(&ViewContext) -> R) -> R {
    let facts = facts();
    let case = case(name);
    let assets: HashMap<String, String> =
        facts["assets"].as_object().unwrap().iter().map(|(k, v)| (k.clone(), v.as_str().unwrap().to_string())).collect();
    let asset_path = move |logical: &str| assets.get(logical).cloned().unwrap_or_else(|| panic!("unknown asset {logical}"));

    let current = str_of(&case["as"]).map(|email| user_by_email(name, &email));
    let current_user = current.map(|user| CurrentUser {
        id: user["id"].as_i64().unwrap(),
        name: user["name"].as_str().unwrap().to_string(),
        administrator: user["role"] == "administrator",
        bot: user["role"] == "bot",
        avatar_url: user["avatar_path"].as_str().unwrap().to_string(),
    });
    let account = &case["account"];
    let base_url = facts["base_url"].as_str().unwrap().to_string();
    let ctx = ViewContext {
        current_user,
        account: AccountSummary {
            name: str_of(&account["name"]).unwrap_or_default(),
            logo_url: str_of(&account["logo_path"]).unwrap_or_else(|| "/account/logo".into()),
            has_logo: account["has_logo"].as_bool().unwrap_or(false),
        },
        flash_notice: request.flash_notice,
        flash_alert: request.flash_alert,
        platform: platform(case["ua"].as_str().unwrap()),
        vapid_public_key: str_of(&facts["vapid_public_key"]),
        asset_path: &asset_path,
        importmap_tags: facts["importmap_tags"].as_str().unwrap(),
        stylesheet_tags: facts["stylesheet_tags"].as_str().unwrap(),
        custom_styles: str_of(&account["custom_styles"]),
        cable_url: "/cable".into(),
        request_url: format!("{base_url}{}", case["path"].as_str().unwrap()),
        base_url,
        referrer: str_of(&case["referrer"]),
        last_room_visited_id: current.and_then(|user| user["original_room_id"].as_i64()),
        app_version: facts["app_version"].as_str().unwrap().to_string(),
    };
    f(&ctx)
}
