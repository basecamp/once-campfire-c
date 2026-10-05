//! Prints the compiled `campfire_assets` crate's own asset data as JSON, for verifying the
//! committed H03 fixtures against the Rust build's mapping (not against a re-parse of
//! embedded.rs).
//!
//! Usage from the /tmp workspace copy of tmp/rust-ref:
//!   cp tests/fixtures/tools/asset_prep/dump_reference.rs crates/assets/examples/
//!   SOURCE_DATE_EPOCH=<epoch> cargo run --locked -p campfire_assets \
//!       --example dump_reference -- /tmp/cf-assets-prep/probe.json
//!   python3 tests/fixtures/tools/asset_prep/verify_assets.py --probe /tmp/cf-assets-prep/probe.json

use std::fs;

fn main() {
    let path = std::env::args().nth(1).expect("usage: dump_reference <out.json>");
    let manifest: Vec<(String, String)> =
        campfire_assets::manifest().iter().map(|(logical, digested)| (logical.to_string(), digested.to_string())).collect();
    let value = serde_json::json!({
        "manifest": manifest,
        "manifest_json": campfire_assets::manifest_json(),
        "importmap_tags": campfire_assets::javascript_importmap_tags(),
        "stylesheet_logicals": campfire_assets::all_stylesheet_paths(),
    });
    fs::write(&path, serde_json::to_string_pretty(&value).unwrap()).expect("writing probe output");
    eprintln!("wrote {path}");
}
