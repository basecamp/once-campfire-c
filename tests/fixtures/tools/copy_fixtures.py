#!/usr/bin/env python3
"""Copy the F02 reference fixtures out of the read-only rust-ref oracle tree.

This is a maintenance tool, not a test: it is the only thing under tests/ that
reads tmp/. The copied fixtures are self-contained; the test runners never read
tmp/ at runtime.

What it does
------------
1. Verifies tmp/rust-ref is at the pinned revision
   64f86353021145b63849fb1cd93adeb08f3b8dbb.
2. Copies the fixture set named by docs/devel/implementation/07-verification.md
   ("Fixture provenance" table) and the F02 task card, preserving the path
   relative to the rust-ref root (tests/fixtures/<relative path>), plus the
   upstream license as tests/fixtures/LICENSE.rust-ref.
3. Copies the pinned seed recipes (parity/bin/seed, parity/seeds/**) into
   tests/seed/ (tests/seed/bin/seed, tests/seed/seeds/**).
4. Writes tests/fixtures/MANIFEST.json with per-file provenance: source repo,
   pinned revision, source path, sha256 of the copied bytes, license, selection
   of case IDs, and explicit OUT_OF_SCOPE: D-C01 marks for the old-key/Marshal
   compatibility vectors excluded by 02-data-auth.md A01.

Re-running is idempotent for unchanged sources: existing destination files must
be byte-identical or the copy fails (it never silently overwrites a difference).

Usage
-----
    python3 tests/fixtures/tools/copy_fixtures.py [--rust-ref DIR]

The default rust-ref directory is <repo>/tmp/rust-ref.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
FIXTURES = REPO_ROOT / "tests" / "fixtures"
SEED = REPO_ROOT / "tests" / "seed"
PINNED_REVISION = "64f86353021145b63849fb1cd93adeb08f3b8dbb"
SOURCE_REPO = "https://github.com/basecamp/once-campfire-rust.git"
LICENSE = "MIT"
DCO1 = (
    "OUT_OF_SCOPE: D-C01 - the C port keeps only the configured secret and the "
    "current JSON envelope; it has no rotation list, Marshal reader, legacy "
    "serializer, or fallback verifier (02-data-auth.md A01)."
)

# ---------------------------------------------------------------------------
# The fixture set, by surface.  Paths are relative to the rust-ref root.
# ---------------------------------------------------------------------------

DIRECT_FILES: dict[str, list[str]] = {
    "parameters": [
        "crates/kit/tests/params_vectors.json",
        "crates/kit/tests/params_vectors.rs",
    ],
    "cookies_ids": [
        "vectors/rails_compat.json",
        "vectors/campfire_sessions.json",
    ],
    "models": [
        "crates/db/src/fixtures.rs",
        "crates/db/src/testing.rs",
        "crates/db/src/tests.rs",
        "crates/db/src/tests/account_test.rs",
        "crates/db/src/tests/callbacks_test.rs",
        "crates/db/src/tests/columns_test.rs",
        "crates/db/src/tests/differential_test.rs",
        "crates/db/src/tests/first_run_test.rs",
        "crates/db/src/tests/fixtures_test.rs",
        "crates/db/src/tests/membership_test.rs",
        "crates/db/src/tests/message_test.rs",
        "crates/db/src/tests/push_test.rs",
        "crates/db/src/tests/room_test.rs",
        "crates/db/src/tests/user_test.rs",
    ],
    "views": [
        "crates/views/tests/messages_support/mod.rs",
        "crates/views/tests/messages_views.rs",
        "crates/views/tests/parity_a.rs",
        "crates/views/tests/rooms_views.rs",
        "crates/views/tests/searches_views.rs",
        "crates/views/tests/support/dom.rs",
        "crates/views/tests/support/facts.rs",
        "crates/views/tests/support/mod.rs",
    ],
    "cable": [
        "crates/cable/tests/golden.rs",
        "crates/cable/tests/protocol.rs",
        "crates/cable/tests/support/mod.rs",
        "crates/cable/tests/golden/fixtures.rb",
        "crates/cable/tests/golden/reference.json",
        "crates/campfire/src/channels/tests.rs",
        "crates/campfire/src/channels/tests/broadcasts_test.rs",
        "crates/campfire/src/channels/tests/channels_test.rs",
        "crates/campfire/src/channels/tests/golden.rs",
        "crates/campfire/src/channels/tests/revocation_test.rs",
        "crates/campfire/src/channels/tests/support.rs",
        "crates/campfire/src/channels/tests/golden/fixtures.rb",
        "crates/campfire/src/channels/tests/golden/reference.json",
        "crates/campfire/src/channels/tests/golden/trigger.rb",
    ],
    "richtext": [
        "crates/richtext/tests/corpus.rs",
        "crates/richtext/tests/hardening.rs",
        "crates/richtext/tests/reference_tests.rs",
        "crates/richtext/tests/corpus/expected.json",
        "crates/richtext/tests/corpus/inputs.yml",
    ],
    "storage": [
        "vectors/storage.json",
        "crates/storage/tests/vectors.rs",
    ],
    "browser": [
        "parity/screens.yml",
    ],
    "license": [
        "MIT-LICENSE",
    ],
}

# Whole directories, recursively (dotfiles are skipped).
DIRECTORIES: dict[str, list[str]] = {
    "views_golden": [
        "crates/views/tests/golden/a",
        "crates/views/tests/golden/b",
    ],
    "storage_vectors": [
        "vectors/storage",
    ],
    "integrations": [
        "crates/campfire/src/integrations/testdata",
    ],
}

# Seed recipes copied to tests/seed/ instead of tests/fixtures/.
SEED_TREES: list[tuple[str, str]] = [
    ("parity/bin/seed", "tests/seed/bin/seed"),
    ("parity/seeds", "tests/seed/seeds"),
]

# Special destinations: source path -> path relative to tests/fixtures/.
SPECIAL: dict[str, str] = {"MIT-LICENSE": "LICENSE.rust-ref"}

# ---------------------------------------------------------------------------
# rails_compat.json selection (07-verification "current-format selections only").
# Every case-bearing array is enumerated; the entries below are the old-key /
# legacy-format read paths that D-C01 excludes.
# ---------------------------------------------------------------------------

RAILS_COMPAT_OUT_OF_SCOPE: dict[str, list[str]] = {
    "signed_cookies.verify": [
        "no metadata (pre-5.2), any name",
        "no metadata under another name",
        "marshal serialized value",
        "marshal serialized value, no metadata",
    ],
    "encrypted_cookies.verify": [
        "no metadata (any name)",
        "marshal value",
    ],
    "signed_ids.verify": [
        "legacy fallback verifier (SHA1, standard base64)",
        "legacy fallback verifier, wrong purpose",
    ],
    "sgids.verify": [
        "marshal era (Rails 7.0)",
        "marshal era with expires_in param",
        "marshal era, wrong purpose",
        "marshal era, expired",
        "json legacy envelope",
        "self-validated metadata (globalid < 1.0)",
        "self-validated metadata, wrong purpose",
        "self-validated metadata, expired",
        "self-validated metadata, not yet expired",
    ],
    "unverified_sgids": [
        "marshal era user sgid, rotated secret",
        "marshal era user sgid with params, rotated secret",
        "marshal era user sgid, marshal with a long string",
        "marshal era room sgid, rotated secret",
        "marshal era gid of another app",
    ],
}

# Every top-level section that contains case IDs, in manifest order.
RAILS_COMPAT_CASE_SECTIONS = [
    "signed_cookies.verify",
    "encrypted_cookies.verify",
    "csrf.validity",
    "signed_ids.verify",
    "sgids.verify",
    "unverified_sgids",
    "turbo_stream_names.verify",
    "app_verifiers.verify",
]


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def load_reference_files() -> dict[str, str]:
    """path -> sha256 from the frozen contract, keyed by rust-ref-relative path."""
    contract = REPO_ROOT / "docs/devel/implementation/contracts/reference-files.json"
    data = json.loads(contract.read_text())
    out = {}
    for entry in data["files"]:
        p = entry["path"]
        if p.startswith("tmp/rust-ref/"):
            out[p[len("tmp/rust-ref/"):]] = entry["sha256"]
    return out


def _has_cases(node) -> bool:
    """True if any nested list holds dicts with a "case" key."""
    if isinstance(node, dict):
        return any(_has_cases(v) for v in node.values())
    if isinstance(node, list):
        return any(isinstance(x, dict) and "case" in x or _has_cases(x) for x in node)
    return False


def rails_compat_selection(src: Path) -> dict:
    """Explicit selected / OUT_OF_SCOPE case IDs for vectors/rails_compat.json."""
    data = json.loads(src.read_text())
    selected = []
    out_of_scope = []
    for dotted in RAILS_COMPAT_CASE_SECTIONS:
        parts = dotted.split(".")
        node = data
        for part in parts:
            node = node[part]
        excluded = RAILS_COMPAT_OUT_OF_SCOPE.get(dotted, [])
        for index, case in enumerate(node):
            name = case["case"] if isinstance(case, dict) else None
            if name in excluded:
                out_of_scope.append(
                    {"section": dotted, "index": index, "case": name, "mark": DCO1}
                )
            else:
                selected.append({"section": dotted, "index": index, "case": name})
    listed = {e["section"] for e in selected} | {e["section"] for e in out_of_scope}
    missing = [s for s in RAILS_COMPAT_CASE_SECTIONS if s not in listed]
    if missing:
        raise RuntimeError(f"rails_compat selection missed sections: {missing}")
    # Top-level sections with no per-case IDs are selected wholesale.
    wholesale = [k for k, v in data.items() if not _has_cases(v)]
    return {
        "rule": (
            "current JSON envelope vectors only; wrong-secret/expiry/tamper/malformed "
            "negatives remain selected"
        ),
        "selected": selected,
        "out_of_scope": out_of_scope,
        "selected_wholesale_sections": wholesale,
    }


def campfire_sessions_selection(src: Path) -> dict:
    data = json.loads(src.read_text())
    selected = []
    for i in range(len(data["sessions"])):
        selected.append({"section": "sessions", "index": i, "case": "sessions[%d]" % i})
    for i in range(len(data["blobs"])):
        selected.append({"section": "blobs", "index": i, "case": "blobs[%d]" % i})
    selected.append({"section": "forged", "index": 0, "case": "forged"})
    return {
        "rule": "all entries are the current signed-JSON-envelope format",
        "selected": selected,
        "out_of_scope": [],
    }


def copy_one(src: Path, dest: Path) -> None:
    if src.is_symlink():
        raise RuntimeError(f"refusing to copy symlink: {src}")
    if not src.is_file():
        raise RuntimeError(f"source fixture missing: {src}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    src_bytes = src.read_bytes()
    mode = src.stat().st_mode & 0o777
    if dest.exists():
        if dest.read_bytes() != src_bytes:
            raise RuntimeError(
                f"destination exists with different bytes; refusing to overwrite: {dest}"
            )
        os.chmod(dest, mode)
        return
    dest.write_bytes(src_bytes)
    os.chmod(dest, mode)


def iter_dir_files(root: Path):
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise RuntimeError(f"refusing to copy symlink: {path}")
        if path.is_dir():
            continue
        if path.name.startswith("."):
            raise RuntimeError(f"unexpected dotfile in copied tree: {path}")
        yield path


def case_ids_for(src_rel: str, src: Path):
    """Selected case IDs / counts recorded for a fixture that names cases."""
    if src_rel.endswith("params_vectors.json"):
        return {"selection": "all", "count": len(json.loads(src.read_text()))}
    if src_rel.endswith("vectors/storage.json"):
        data = json.loads(src.read_text())
        counts = {k: len(v) for k, v in data.items() if isinstance(v, list)}
        return {"selection": "all", "counts": counts}
    if src_rel.endswith("richtext/tests/corpus/expected.json"):
        return {"selection": "all", "count": len(json.loads(src.read_text())["cases"])}
    if src_rel.endswith("richtext/tests/corpus/inputs.yml"):
        return {
            "selection": "all",
            "count": len(re.findall(r"^\s*-\s*name:", src.read_text(), re.M)),
        }
    if src_rel.endswith("cable/tests/golden/reference.json") or src_rel.endswith(
        "channels/tests/golden/reference.json"
    ):
        data = json.loads(src.read_text())
        counts = {k: len(v) for k, v in data.items() if isinstance(v, (dict, list))}
        return {"selection": "all", "counts": counts}
    if src_rel.endswith("integrations/testdata/opengraph_cases.json"):
        data = json.loads(src.read_text())
        ids = [c.get("name") for c in data["cases"]]
        return {"selection": "all", "count": len(ids), "ids": ids}
    if src_rel.endswith("integrations/testdata/webhook_cases.json"):
        data = json.loads(src.read_text())
        ids = [c.get("name") for c in data]
        return {"selection": "all", "count": len(ids), "ids": ids}
    if src_rel.endswith("integrations/testdata/web_push_expected.json"):
        return {"selection": "all", "count": 1}
    if src_rel.endswith("parity/screens.yml"):
        ids = re.findall(r"^-\s+id:\s*(\S+)", src.read_text(), re.M)
        return {"selection": "all", "count": len(ids), "ids": ids}
    if src_rel.startswith("crates/views/tests/golden/"):
        return {"selection": "file", "ids": [Path(src_rel).stem]}
    return None


def build_manifest(fixtures: Path, rust_ref: Path, copied: list[dict]) -> dict:
    frozen = load_reference_files()
    entries = []
    for record in copied:
        src_rel = record["src_rel"]
        dest = record["dest"]
        fixture_rel = dest.relative_to(REPO_ROOT).as_posix()
        src = rust_ref / src_rel
        sha = sha256_file(dest)
        entry = {
            "fixture": fixture_rel,
            "source_repo": SOURCE_REPO,
            "source_revision": PINNED_REVISION,
            "source_path": f"tmp/rust-ref/{src_rel}",
            "surface": record["surface"],
            "role": record["role"],
            "bytes": dest.stat().st_size,
            "sha256": sha,
            "source_sha256": sha256_file(src),
            "license": LICENSE,
            "frozen_in_reference_files": src_rel in frozen,
        }
        if src_rel in frozen and frozen[src_rel] != entry["source_sha256"]:
            raise RuntimeError(
                f"source sha256 differs from contracts/reference-files.json: {src_rel}"
            )
        if record.get("selection"):
            entry["selection"] = record["selection"]
        case_ids = case_ids_for(src_rel, src)
        if case_ids is not None:
            entry["case_ids"] = case_ids
        entries.append(entry)

    # External references: pinned tables that are contract artifacts or
    # source-as-spec and are deliberately not duplicated under tests/fixtures/.
    external = []
    ext_paths = [
        {
            "surface": "routes",
            "path": "docs/devel/implementation/contracts/routes.json",
            "kind": "contract-artifact",
            "note": "ordered 177 routes; contract already in docs/, not duplicated",
        },
        {
            "surface": "routes",
            "path": "docs/devel/implementation/contracts/route-recognition.json",
            "kind": "contract-artifact",
            "note": "111 recognition vectors; contract already in docs/, not duplicated",
        },
        {
            "surface": "routes",
            "path": "docs/devel/implementation/contracts/schema.sql",
            "kind": "contract-artifact",
            "note": "fresh schema; contract already in docs/, not duplicated",
        },
        {
            "surface": "controllers",
            "path": "tmp/rust-ref/crates/campfire/src/controllers",
            "kind": "source-as-spec",
            "note": "controller tests and presenter test_support; owned by A02 packets, not fixture data",
        },
        {
            "surface": "models",
            "path": "tmp/rust-ref/crates/db/src/models",
            "kind": "source-as-spec",
            "note": "model-local tests and SQL; D01 translates these files, not fixture data",
        },
        {
            "surface": "cable",
            "path": "tmp/rust-ref/crates/cable/src",
            "kind": "source-as-spec",
            "note": "cable inline tests (channel/socket/protocol); C01 translates, not fixture data",
        },
        {
            "surface": "browser",
            "path": "tmp/rust-ref/parity/bin",
            "kind": "runner-source",
            "note": "documented browser runner (parity/bin/capture, parity/bin/compare); V01/V02 own wiring it",
        },
        {
            "surface": "browser",
            "path": "tmp/rust-ref/parity/package-lock.json",
            "kind": "pin",
            "note": "Playwright/parse5/pixelmatch pins for the browser runner",
        },
        {
            "surface": "browser",
            "path": "tmp/rust-ref/parity/SCREENS.md",
            "kind": "doc",
            "note": "screens.yml format and capture/compare semantics",
        },
        {
            "surface": "bench",
            "path": "tmp/rust-ref/bench/lib/benchlib.py",
            "kind": "source-as-spec",
            "note": "bench seed importer reference: SEED=parity/.seed/default, labels.json, per-run snapshot",
        },
        {
            "surface": "bench",
            "path": "tmp/rust-ref/bench/run",
            "kind": "source-as-spec",
            "note": "bench harness start from a fresh seed copy",
        },
    ]
    for item in ext_paths:
        p = REPO_ROOT / item["path"]
        exists = p.exists()
        record = dict(item)
        record["exists_at_authoring"] = exists
        if exists and p.is_file():
            record["sha256"] = sha256_file(p)
        external.append(record)

    # Seed entries were copied through the same list; split them out by path.
    seed_paths = [e for e in entries if e["fixture"].startswith("tests/seed/")]
    fixture_entries = [e for e in entries if not e["fixture"].startswith("tests/seed/")]
    # tests/seed/README.md is authored by F02 (no upstream source).
    readme = SEED / "README.md"
    if readme.exists():
        seed_paths.append(
            {
                "fixture": "tests/seed/README.md",
                "source_repo": SOURCE_REPO,
                "source_revision": PINNED_REVISION,
                "source_path": None,
                "surface": "seed",
                "role": "authored-doc",
                "bytes": readme.stat().st_size,
                "sha256": sha256_file(readme),
                "source_sha256": None,
                "license": LICENSE,
                "frozen_in_reference_files": False,
            }
        )

    return {
        "format": 1,
        "task": "F02",
        "source_repo": SOURCE_REPO,
        "source_revision": PINNED_REVISION,
        "license": {"spdx": "MIT", "file": "tests/fixtures/LICENSE.rust-ref"},
        "note": (
            "Reference fixtures copied by tests/fixtures/tools/copy_fixtures.py; "
            "tests never read tmp/ at runtime. sha256 is over the copied bytes."
        ),
        "entries": fixture_entries,
        "seed": seed_paths,
        "external_references": external,
        "totals": {
            "fixture_files": len(fixture_entries),
            "fixture_bytes": sum(e["bytes"] for e in fixture_entries),
            "seed_files": len(seed_paths),
            "seed_bytes": sum(e["bytes"] for e in seed_paths),
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--rust-ref",
        default=str(REPO_ROOT / "tmp" / "rust-ref"),
        help="read-only rust-ref checkout (default: <repo>/tmp/rust-ref)",
    )
    args = parser.parse_args()
    rust_ref = Path(args.rust_ref).resolve()

    head = subprocess.run(
        ["git", "-C", str(rust_ref), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    if head != PINNED_REVISION:
        print(
            f"rust-ref is at {head}, expected pinned revision {PINNED_REVISION}",
            file=sys.stderr,
        )
        return 1

    copied: list[dict] = []
    for surface, paths in DIRECT_FILES.items():
        for src_rel in paths:
            dest = FIXTURES / SPECIAL.get(src_rel, src_rel)
            copy_one(rust_ref / src_rel, dest)
            role = "license" if surface == "license" else "fixture-data"
            copied.append({"src_rel": src_rel, "dest": dest, "surface": surface, "role": role})

    for surface, dirs in DIRECTORIES.items():
        for dir_rel in dirs:
            root = rust_ref / dir_rel
            for path in iter_dir_files(root):
                rel = path.relative_to(rust_ref).as_posix()
                dest = FIXTURES / rel
                copy_one(path, dest)
                copied.append(
                    {"src_rel": rel, "dest": dest, "surface": surface, "role": "fixture-data"}
                )

    for src_rel, dest_rel in SEED_TREES:
        src = rust_ref / src_rel
        dest = REPO_ROOT / dest_rel
        if src.is_dir():
            for path in iter_dir_files(src):
                rel = path.relative_to(src).as_posix()
                copy_one(path, dest / rel)
                copied.append(
                    {
                        "src_rel": path.relative_to(rust_ref).as_posix(),
                        "dest": dest / rel,
                        "surface": "seed",
                        "role": "seed-recipe",
                    }
                )
        else:
            copy_one(src, dest)
            copied.append(
                {"src_rel": src_rel, "dest": dest, "surface": "seed", "role": "seed-recipe"}
            )

    # Selection records, attached to the two cookie/ID fixtures.
    for record in copied:
        if record["src_rel"] == "vectors/rails_compat.json":
            record["selection"] = rails_compat_selection(rust_ref / record["src_rel"])
        elif record["src_rel"] == "vectors/campfire_sessions.json":
            record["selection"] = campfire_sessions_selection(rust_ref / record["src_rel"])

    manifest = build_manifest(FIXTURES, rust_ref, copied)
    manifest_path = FIXTURES / "MANIFEST.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")

    total_files = manifest["totals"]["fixture_files"] + manifest["totals"]["seed_files"]
    total_bytes = manifest["totals"]["fixture_bytes"] + manifest["totals"]["seed_bytes"]
    print(f"copied/verified {total_files} files, {total_bytes} bytes")
    print(f"manifest: {manifest_path.relative_to(REPO_ROOT)}")
    oos = sum(
        len(e.get("selection", {}).get("out_of_scope", [])) for e in manifest["entries"]
    )
    print(f"rails_compat OUT_OF_SCOPE cases: {oos}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
