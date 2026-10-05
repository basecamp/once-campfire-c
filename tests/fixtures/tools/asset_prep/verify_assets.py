#!/usr/bin/env python3
"""Offline verification of the committed H03 asset fixtures.

Recomputes every sha256, checks the fixture tree against tests/fixtures/assets/MANIFEST.json
(no orphan or missing files, served .manifest.json consistent with the index, import map pins
resolving, reference-file hashes), and never reads tmp/.

With --probe probe.json (from dump_reference.rs) it additionally asserts the fixtures against
the compiled Rust crate's own mapping: manifest_json bytes, every (logical, digested) pair,
the rendered import map and the stylesheet list.

    python3 tests/fixtures/tools/asset_prep/verify_assets.py [--probe /tmp/cf-assets-prep/probe.json]
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from extract_assets import MIME_TYPES, ruby_extname  # noqa: E402  (same-directory tool)

ASSET_PREFIX = "/assets"
MANIFEST_URL = ASSET_PREFIX + "/.manifest.json"


def fail(message: str) -> None:
    print(f"ERROR: {message}", file=sys.stderr)
    sys.exit(1)


def sha256_file(path: Path) -> str:
    import hashlib

    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def repo_root() -> Path:
    return Path(__file__).resolve().parents[4]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fixtures", type=Path, default=None, help="default: repo tests/fixtures/assets")
    parser.add_argument("--probe", type=Path, default=None, help="JSON written by dump_reference.rs")
    parser.add_argument("--spot-checks", default="application.js,add.svg", help="logical paths to print explicitly")
    args = parser.parse_args()

    fixtures: Path = (args.fixtures or repo_root() / "tests" / "fixtures" / "assets").resolve()
    manifest_path = fixtures / "MANIFEST.json"
    if not manifest_path.is_file():
        fail(f"{manifest_path} not found")
    doc = json.loads(manifest_path.read_text())
    entries = doc["entries"]
    if doc["format"] != 1 or doc["task"] != "H03-prep":
        fail("unexpected manifest format/task")

    # --- per-file integrity and index consistency ---
    for entry in entries:
        path = repo_root() / entry["fixture"]
        if not path.is_file():
            fail(f"missing fixture {path}")
        if path.stat().st_size != entry["bytes"]:
            fail(f"size mismatch for {path}: {path.stat().st_size} != {entry['bytes']}")
        actual = sha256_file(path)
        if actual != entry["sha256"]:
            fail(f"sha256 mismatch for {path}: {actual} != {entry['sha256']}")
        if entry["public_path"] != entry["url"].lstrip("/") or entry["fixture"] != f"tests/fixtures/assets/public/{entry['public_path']}":
            fail(f"fixture path/url mismatch: {entry['url']} -> {entry['fixture']}")
        ext = ruby_extname(entry["url"]).lower()
        expected_content_type = MIME_TYPES.get(ext, "text/plain")
        if entry["content_type"] != expected_content_type or entry["mime_known"] != (ext in MIME_TYPES):
            fail(f"content type mismatch for {entry['url']}: {entry['content_type']} vs {expected_content_type}")

    public = fixtures / "public"
    on_disk = {str(p.relative_to(public)) for p in public.rglob("*") if p.is_file()}
    indexed = {entry["public_path"] for entry in entries}
    if on_disk != indexed:
        fail(f"public/ tree != manifest entries (orphans {sorted(on_disk - indexed)}, missing {sorted(indexed - on_disk)})")

    # --- counts ---
    counts = doc["counts"]
    asset_entries = [e for e in entries if e["digested_path"] is not None]
    if counts["assets"] != len(asset_entries) or counts["urls_total"] != len(entries):
        fail(f"count mismatch: {counts} vs assets={len(asset_entries)} urls={len(entries)}")
    if counts["compiled_assets"] != sum(1 for e in asset_entries if e["body"] == "compiled"):
        fail("compiled count mismatch")
    if counts["public_files"] != sum(1 for e in entries if e["body"] == "public"):
        fail("public count mismatch")

    # --- served Propshaft manifest ---
    served_manifest = json.loads((public / "assets" / ".manifest.json").read_text())
    indexed_manifest = {e["logical_path"]: e["digested_path"] for e in asset_entries}
    if served_manifest != {logical: {"digested_path": digested, "integrity": None} for logical, digested in indexed_manifest.items()}:
        fail("served public/assets/.manifest.json != indexed logical->digested map")
    if not all(e["url"] == f"{ASSET_PREFIX}/{e['digested_path']}" for e in asset_entries):
        fail("asset URL != /assets/<digested path>")

    # --- import map ---
    tags = (fixtures / "importmap-tags.html").read_bytes()
    if sha256_file(fixtures / "importmap-tags.html") != doc["importmap"]["tags_sha256"]:
        fail("importmap tags sha256 mismatch")
    match = re.search(r'<script type="importmap"[^>]*>(\{.*?\})</script>', tags.decode(), re.S)
    if not match:
        fail("no importmap JSON in importmap-tags.html")
    tags_json = json.loads(match.group(1))
    importmap = json.loads((fixtures / "importmap.json").read_text())
    if tags_json != importmap:
        fail("importmap.json != JSON embedded in importmap-tags.html")
    digits = {e["digested_path"] for e in asset_entries}
    for name, url in importmap["imports"].items():
        if not url.startswith(ASSET_PREFIX + "/") or url[len(ASSET_PREFIX) + 1 :] not in digits:
            fail(f"importmap pin {name!r} -> {url!r} has no fixture")
    for href in re.findall(r'<link rel="modulepreload" href="([^"]*)">', tags.decode()):
        if href[len(ASSET_PREFIX) + 1 :] not in digits:
            fail(f"importmap preload {href!r} has no fixture")

    # --- auxiliary and reference files ---
    for item in doc["reference_files"] + doc["auxiliary_files"]:
        path = repo_root() / item["fixture"]
        if not path.is_file() or sha256_file(path) != item["sha256"]:
            fail(f"reference/auxiliary verification failed for {path}")

    # --- embedded cross-checks recorded at extraction time ---
    cross = doc["reference_crosschecks"]
    if cross["compiled_sha256"]["mismatches"] or cross["rails_manifest"]["digest_differences_explained_by_overrides"] is not True:
        fail("extraction cross-checks are not clean")
    if cross["importmap_rendering"]["differing_pins_explained_by_overrides"] is not True:
        fail("importmap rendering differences are not explained by overrides")

    # --- optional probe against the compiled Rust crate ---
    if args.probe:
        probe = json.loads(args.probe.read_text())
        if probe["manifest_json"] != (public / "assets" / ".manifest.json").read_text():
            fail("probe manifest_json != served .manifest.json")
        probe_pairs = [(logical, digested) for logical, digested in probe["manifest"]]
        if probe_pairs != sorted(indexed_manifest.items()):
            fail("probe manifest pairs != fixture MANIFEST.json mapping")
        if probe["importmap_tags"] != tags.decode():
            fail("probe importmap_tags != importmap-tags.html")
        if list(probe["stylesheet_logicals"]) != doc["stylesheet_logicals"]:
            fail("probe stylesheet list != MANIFEST.json stylesheet_logicals")
        print(f"probe: {len(probe_pairs)} manifest pairs, manifest_json and importmap tags identical to fixtures")

    # --- explicit spot-checks against the Rust mapping ---
    spot_logicals = [s for s in args.spot_checks.split(",") if s]
    by_logical = dict(indexed_manifest)
    for logical in spot_logicals:
        if logical not in by_logical:
            fail(f"spot-check logical {logical!r} not in the fixtures")
        print(f"spot-check: {logical} -> {by_logical[logical]}")

    print(
        f"OK: {len(entries)} served URLs verified ({len(asset_entries)} assets, "
        f"{counts['compiled_assets']} compiled, {counts['public_files']} public), "
        f"{len(importmap['imports'])} importmap pins, {len(doc['reference_files'])} reference files"
    )


if __name__ == "__main__":
    main()
