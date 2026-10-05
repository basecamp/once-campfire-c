#!/usr/bin/env python3
"""Recompute every SHA-256 in tests/fixtures/MANIFEST.json and audit the tree.

Fails (exit 1) if a copied fixture or contract input is missing, has a
different size or hash, if tests/fixtures/ contains files the manifest does not
list, or if a repo-local external reference is missing or changed.

All required test inputs are committed: the route/schema development contracts
are copied byte for byte into tests/fixtures/contracts/ and listed as
role="contract-input" entries, the UA corpus is a normal listed entry, and the
asset tree is listed by delegation.  A role="delegated-subtree" entry pins one
committed inventory file (fixture == delegated_manifest) that records every
other file under the named `subtree` with its sha256 (and bytes where
recorded).  The verifier hashes that inventory like any other entry and then
checks every file under the subtree against it, so an added, removed or
altered asset file fails until the delegated inventory and its root pin are
updated -- without duplicating hundreds of asset entries here.  The default
run therefore succeeds in a clean checkout without docs/devel.  tmp/ external
references are verified when the read-only oracle tree is present and reported
as absent otherwise (tmp/ is not required at runtime); --require-tmp makes
their absence fatal.

Usage:
    python3 tests/fixtures/tools/verify_manifest.py [--require-tmp]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
FIXTURES = REPO_ROOT / "tests" / "fixtures"
MANIFEST = FIXTURES / "MANIFEST.json"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--require-tmp",
        action="store_true",
        help="fail if tmp/ external references are not present",
    )
    args = parser.parse_args()

    manifest = json.loads(MANIFEST.read_text())
    errors: list[str] = []
    checked = 0
    bytes_total = 0

    def check(entry: dict, section: str) -> None:
        nonlocal checked, bytes_total
        rel = entry["fixture"]
        path = REPO_ROOT / rel
        if not path.is_file():
            errors.append(f"{section}: missing fixture {rel}")
            return
        data_len = path.stat().st_size
        digest = sha256_file(path)
        if data_len != entry["bytes"]:
            errors.append(f"{section}: {rel} size {data_len} != manifest {entry['bytes']}")
        if digest != entry["sha256"]:
            errors.append(f"{section}: {rel} sha256 {digest} != manifest {entry['sha256']}")
        checked += 1
        bytes_total += data_len

    for entry in manifest["entries"]:
        check(entry, "entry")
    for entry in manifest["seed"]:
        check(entry, "seed")

    # Required contract inputs must carry their provenance; a missing field
    # would make the committed copy unverifiable from the manifest alone.
    contract_inputs = 0
    for entry in manifest["entries"]:
        if entry.get("role") != "contract-input":
            continue
        contract_inputs += 1
        for key in ("source_path", "purpose", "pinned_revision", "captured_from_revision"):
            if not entry.get(key):
                errors.append(f"entry: contract input {entry['fixture']} lacks {key}")

    # Delegated subtrees: a role="delegated-subtree" entry in this manifest
    # pins one committed inventory file (entry["fixture"], equal to
    # entry["delegated_manifest"]), and the inventory lists every other file
    # under entry["subtree"] with its sha256 (bytes where recorded).  This
    # keeps large generated trees out of this manifest without skipping them:
    # an added, removed or altered file under the subtree fails here.
    delegated_prefixes: list[str] = []
    delegated_checked = 0
    delegated_bytes = 0
    fixtures_root = FIXTURES.resolve()

    for entry in manifest["entries"]:
        if entry.get("role") != "delegated-subtree":
            continue
        subtree_rel = entry.get("subtree")
        if not subtree_rel or not entry.get("delegated_manifest"):
            errors.append(
                f"delegated: {entry.get('fixture')} lacks subtree/delegated_manifest"
            )
            continue
        subtree_dir = (REPO_ROOT / subtree_rel).resolve()
        manifest_path = (REPO_ROOT / entry["delegated_manifest"]).resolve()
        if not (subtree_dir.is_relative_to(fixtures_root) and subtree_dir != fixtures_root):
            errors.append(
                f"delegated: subtree {subtree_rel} must be a strict subdirectory "
                "of tests/fixtures/"
            )
            continue
        if not (manifest_path.is_relative_to(subtree_dir) and manifest_path.is_file()):
            errors.append(
                f"delegated: {entry['delegated_manifest']} is not a file inside "
                f"{subtree_rel}"
            )
            continue
        if entry["fixture"] != entry["delegated_manifest"]:
            errors.append(
                f"delegated: {entry['fixture']} must name its delegated_manifest "
                f"{entry['delegated_manifest']}"
            )
        manifest_rel = manifest_path.relative_to(REPO_ROOT).as_posix()
        delegated_prefixes.append(subtree_dir.relative_to(REPO_ROOT).as_posix() + "/")

        try:
            inventory = json.loads(manifest_path.read_text())
        except (OSError, json.JSONDecodeError) as exc:
            errors.append(f"delegated: cannot read {manifest_rel}: {exc}")
            continue

        records: dict[str, tuple[str, int | None]] = {}

        def record(rel: object, digest: object, size: object, where: str) -> None:
            if not isinstance(rel, str) or not rel:
                errors.append(f"delegated: {manifest_rel} {where} record lacks a fixture path")
                return
            full = (REPO_ROOT / rel).resolve()
            if not full.is_relative_to(subtree_dir):
                errors.append(
                    f"delegated: {manifest_rel} {where} record {rel} is outside "
                    f"{subtree_rel}"
                )
                return
            if not isinstance(digest, str) or len(digest) != 64:
                errors.append(f"delegated: {manifest_rel} {where} record {rel} lacks a sha256")
                return
            if size is not None and not isinstance(size, int):
                errors.append(
                    f"delegated: {manifest_rel} {where} record {rel} has non-integer bytes"
                )
                return
            previous = records.get(rel)
            if previous is not None:
                prev_digest, prev_size = previous
                if prev_digest != digest:
                    errors.append(f"delegated: {manifest_rel} records {rel} inconsistently")
                    return
                if prev_size is not None and size is not None and prev_size != size:
                    errors.append(f"delegated: {manifest_rel} records {rel} inconsistently")
                    return
                if prev_size is not None:
                    size = prev_size
            records[rel] = (digest, size)

        for item in inventory.get("entries", []):
            record(item.get("fixture"), item.get("sha256"), item.get("bytes"), "entries")
        for section in ("reference_files", "auxiliary_files"):
            for item in inventory.get(section, []):
                record(item.get("fixture"), item.get("sha256"), item.get("bytes"), section)
        importmap = inventory.get("importmap", {})
        for rel_key, digest_key in (
            ("tags_fixture", "tags_sha256"),
            ("json_fixture", "json_sha256"),
            ("rb_fixture", "rb_sha256"),
        ):
            if importmap.get(rel_key):
                record(importmap[rel_key], importmap.get(digest_key), None, "importmap")

        actual: dict[str, Path] = {}
        for path in sorted(subtree_dir.rglob("*")):
            if path.is_file():
                actual[path.relative_to(REPO_ROOT).as_posix()] = path
        for rel in sorted(records):
            if rel not in actual:
                errors.append(f"delegated: missing fixture {rel} (recorded in {manifest_rel})")
        for rel, path in sorted(actual.items()):
            if rel == manifest_rel:
                # The inventory file itself is pinned by its root entry.
                continue
            if rel not in records:
                errors.append(f"delegated: unlisted file under {subtree_rel}: {rel}")
                continue
            digest, size = records[rel]
            data_len = path.stat().st_size
            actual_digest = sha256_file(path)
            if actual_digest != digest:
                errors.append(
                    f"delegated: {rel} sha256 {actual_digest} != {manifest_rel} {digest}"
                )
            if size is not None and data_len != size:
                errors.append(
                    f"delegated: {rel} size {data_len} != {manifest_rel} {size}"
                )
            delegated_checked += 1
            delegated_bytes += data_len

    # The fixture tree must contain exactly the manifest-listed bytes (plus the
    # manifest, this tools/ directory, and the delegated subtrees, which are
    # checked file by file above).
    listed = {e["fixture"] for e in manifest["entries"]}
    for path in sorted(FIXTURES.rglob("*")):
        if path.is_dir():
            continue
        rel = path.relative_to(REPO_ROOT).as_posix()
        if rel == "tests/fixtures/MANIFEST.json" or rel.startswith("tests/fixtures/tools/"):
            continue
        if rel in listed:
            continue
        if any(rel.startswith(prefix) for prefix in delegated_prefixes):
            continue
        errors.append(f"unlisted file under tests/fixtures/: {rel}")

    tmp_present = (REPO_ROOT / "tmp/rust-ref").is_dir()
    external_checked = 0
    for entry in manifest["external_references"]:
        rel = entry["path"]
        path = REPO_ROOT / rel
        if rel.startswith("tests/fixtures/"):
            errors.append(
                f"external: {rel} is committed fixture content; list it as an entry instead"
            )
            continue
        if not path.exists():
            if rel.startswith("tmp/"):
                if args.require_tmp:
                    errors.append(f"external: required tmp reference missing: {rel}")
                else:
                    print(f"external: absent (tmp oracle not present): {rel}")
            else:
                errors.append(f"external: missing repo-local reference: {rel}")
            continue
        if path.is_file() and "sha256" in entry:
            digest = sha256_file(path)
            if digest != entry["sha256"]:
                errors.append(
                    f"external: {rel} sha256 {digest} != manifest {entry['sha256']}"
                )
            external_checked += 1
        elif path.is_file():
            errors.append(f"external: {rel} has no sha256 recorded in the manifest")
        else:
            print(f"external: directory reference present: {rel}")

    totals = manifest["totals"]
    expected_files = totals["fixture_files"] + totals["seed_files"]
    if checked != expected_files:
        errors.append(f"checked {checked} files, manifest totals say {expected_files}")
    print(
        f"checked {checked} files, {bytes_total} bytes "
        f"(fixtures {totals['fixture_files']}, seed {totals['seed_files']}); "
        f"contract inputs: {contract_inputs}; "
        f"delegated subtrees: {len(delegated_prefixes)} "
        f"({delegated_checked} files, {delegated_bytes} bytes); "
        f"external file references hashed: {external_checked}; "
        f"tmp/rust-ref present: {tmp_present}"
    )
    if errors:
        for err in errors:
            print(f"FAIL {err}", file=sys.stderr)
        print(f"manifest verification: {len(errors)} failure(s)", file=sys.stderr)
        return 1
    print("manifest verification: all hashes match")
    return 0


if __name__ == "__main__":
    sys.exit(main())
