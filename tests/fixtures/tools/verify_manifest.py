#!/usr/bin/env python3
"""Recompute every SHA-256 in tests/fixtures/MANIFEST.json and audit the tree.

Fails (exit 1) if a copied fixture or contract input is missing, has a
different size or hash, if tests/fixtures/ contains files the manifest does not
list, or if a repo-local external reference is missing or changed.

All required test inputs are committed: the route/schema development contracts
are copied byte for byte into tests/fixtures/contracts/ and listed as
role="contract-input" entries, so the default run succeeds in a clean checkout
without docs/devel.  tmp/ external references are verified when the read-only
oracle tree is present and reported as absent otherwise (tmp/ is not required
at runtime); --require-tmp makes their absence fatal.

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

    # The fixture tree must contain exactly the manifest-listed bytes (plus the
    # manifest and this tools/ directory).
    listed = {e["fixture"] for e in manifest["entries"]}
    for path in sorted(FIXTURES.rglob("*")):
        if path.is_dir():
            continue
        rel = path.relative_to(REPO_ROOT).as_posix()
        if rel == "tests/fixtures/MANIFEST.json" or rel.startswith("tests/fixtures/tools/"):
            continue
        if rel not in listed:
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
