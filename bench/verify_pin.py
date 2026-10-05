#!/usr/bin/env python3
"""Recompute every SHA-256 in bench/PINNED-MANIFEST.json.

Fails (exit 1) if a vendored harness file is missing, has a different size,
hash or executable bit, or if bench/pinned/ contains a file the manifest does
not list.  The pinned tree is data: nothing in the build fetches it, and a
modified harness file must fail this check rather than silently change the
measurement.

Usage:
    python3 bench/verify_pin.py
"""

from __future__ import annotations

import hashlib
import json
import os
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
MANIFEST = REPO_ROOT / "bench" / "PINNED-MANIFEST.json"
PINNED = REPO_ROOT / "bench" / "pinned"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    manifest = json.loads(MANIFEST.read_text())
    entries = manifest["entries"]
    errors: list[str] = []
    checked = 0
    bytes_total = 0

    listed: set[str] = set()
    for entry in entries:
        rel = entry["path"]
        listed.add(rel)
        path = REPO_ROOT / rel
        if not path.is_file():
            errors.append(f"missing: {rel}")
            continue
        data = path.read_bytes()
        if len(data) != entry["bytes"]:
            errors.append(
                f"{rel}: size {len(data)} != manifest {entry['bytes']}")
        digest = sha256_file(path)
        if digest != entry["sha256"]:
            errors.append(f"{rel}: sha256 {digest} != manifest {entry['sha256']}")
        want_exec = entry.get("mode", "100644") == "100755"
        have_exec = os.access(path, os.X_OK)
        if want_exec != have_exec:
            errors.append(
                f"{rel}: executable={have_exec}, manifest mode "
                f"{entry.get('mode')}")
        checked += 1
        bytes_total += len(data)

    for path in sorted(PINNED.rglob("*")):
        if path.is_file() and str(path.relative_to(REPO_ROOT)) not in listed:
            errors.append(f"unlisted file under bench/pinned/: "
                          f"{path.relative_to(REPO_ROOT)}")

    print(
        f"checked {checked} pinned harness files, {bytes_total} bytes; "
        f"source {manifest['source_repo']}@{manifest['source_revision']}"
    )
    if errors:
        for err in errors:
            print(f"FAIL {err}", file=sys.stderr)
        print(f"pin verification: {len(errors)} failure(s)", file=sys.stderr)
        return 1
    print("pin verification: all hashes match")
    return 0


if __name__ == "__main__":
    sys.exit(main())
