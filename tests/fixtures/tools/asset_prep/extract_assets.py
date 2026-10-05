#!/usr/bin/env python3
"""Extract the `crates/assets` build outputs into committed H03 asset fixtures.

The Rust reference (`tmp/rust-ref`) computes digested asset paths, compiles CSS/JS
(Propshaft port), renders the import map and embeds `reference/public` into
`$OUT_DIR/embedded.rs`. This tool reads that build artifact only -- it never
re-implements Propshaft and never invents asset names. Run it from a /tmp build copy:

    SOURCE_DATE_EPOCH=<once-campfire commit time> \
    cargo build --locked -p campfire_assets
    python3 tests/fixtures/tools/asset_prep/extract_assets.py \
        --out-dir target/debug/build/campfire_assets-*/out \
        --workspace /tmp/cf-assets-prep/rust-ref \
        --build-command 'SOURCE_DATE_EPOCH=... cargo build --locked -p campfire_assets'

Outputs (under tests/fixtures/assets/):
  public/assets/<digested>      served bodies for /assets/<digested>
  public/<path>                 served bodies for reference/public (/, /404, ...)
  public/assets/.manifest.json  Propshaft manifest exactly as served
  importmap.rb                  copy of reference/config/importmap.rb (pin source)
  importmap-tags.html           exact IMPORTMAP_TAGS rendering from the build
  importmap.json                the import map JSON inside those tags
  reference/*                   rust-ref's committed Rails-generated fixtures used
                                for cross-checking (manifest, compiled sha256, tags)
  MANIFEST.json                 provenance + per-file index (url, content type,
                                bytes, sha256, source)
  LICENSE.*, VENDOR-MANIFEST.md license/provenance copies

The script fails on any inconsistency: derived digest map vs the build's own
MANIFEST_JSON, FILES vs the digested/compiled sets, compiled set vs compiled
bodies, and sha256 of every written fixture.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path, PurePosixPath

ASSET_PREFIX = "/assets"
MANIFEST_URL = ASSET_PREFIX + "/.manifest.json"

# Rack::Mime.mime_type port from crates/assets/src/serve.rs (the extensions a
# Campfire deploy serves); lookups are case-insensitive.
MIME_TYPES = {
    ".avif": "image/avif",
    ".css": "text/css",
    ".csv": "text/csv",
    ".gif": "image/gif",
    ".gz": "application/x-gzip",
    ".htm": "text/html",
    ".html": "text/html",
    ".ico": "image/vnd.microsoft.icon",
    ".jpeg": "image/jpeg",
    ".jpg": "image/jpeg",
    ".js": "text/javascript",
    ".mjs": "text/javascript",
    ".json": "application/json",
    ".m4a": "audio/mp4a-latm",
    ".mp3": "audio/mpeg",
    ".mp4": "video/mp4",
    ".ogg": "application/ogg",
    ".otf": "font/otf",
    ".pdf": "application/pdf",
    ".png": "image/png",
    ".svg": "image/svg+xml",
    ".ttf": "font/ttf",
    ".txt": "text/plain",
    ".wav": "audio/x-wav",
    ".webm": "video/webm",
    ".webp": "image/webp",
    ".woff": "font/woff",
    ".woff2": "font/woff2",
    ".xml": "application/xml",
    ".zip": "application/zip",
}


def fail(message: str) -> None:
    print(f"ERROR: {message}", file=sys.stderr)
    sys.exit(1)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def rust_unescape(text: str) -> str:
    out = []
    i = 0
    simple = {"n": "\n", "r": "\r", "t": "\t", "\\": "\\", '"': '"', "'": "'", "0": "\0"}
    while i < len(text):
        c = text[i]
        i += 1
        if c != "\\":
            out.append(c)
            continue
        e = text[i]
        i += 1
        if e in simple:
            out.append(simple[e])
        elif e == "x":
            out.append(chr(int(text[i : i + 2], 16)))
            i += 2
        elif e == "u":
            if text[i] != "{":
                fail("unexpected \\u escape in Rust string literal")
            end = text.index("}", i)
            out.append(chr(int(text[i + 1 : end], 16)))
            i = end + 1
        else:
            fail(f"unknown Rust string escape \\{e}")
    return "".join(out)


def read_rust_string(src: str, start: int) -> tuple[str, int]:
    """Parse the Rust string literal beginning at src[start] == '"'."""
    if src[start] != '"':
        fail("expected a Rust string literal")
    i = start + 1
    chunk = []
    while i < len(src):
        c = src[i]
        i += 1
        if c == '"':
            return rust_unescape("".join(chunk)), i
        if c == "\\":
            chunk.append(c)
            chunk.append(src[i])
            i += 1
        else:
            chunk.append(c)
    fail("unterminated Rust string literal")


def read_static_str(src: str, declaration: str) -> str:
    marker = f"static {declaration}: &str = "
    i = src.index(marker)
    value, _ = read_rust_string(src, i + len(marker))
    return value


def parse_include_bytes(src: str) -> dict[str, str]:
    out: dict[str, str] = {}
    pattern = re.compile(r"static ((?:ASSET|PUBLIC)_\d+): &\[u8\] = include_bytes!\(")
    for match in pattern.finditer(src):
        quoted = src.index('"', match.end())
        path, _ = read_rust_string(src, quoted)
        out[match.group(1)] = path
    return out


def parse_quoted_str_list(src: str, static_name: str) -> list[str]:
    header = f"pub(crate) static {static_name}: "
    start = src.index(header)
    body_start = src.index("[", start) + 1
    body_end = src.index("];", body_start)
    body = src[body_start:body_end]
    values = []
    for match in re.finditer(r'"((?:[^"\\]|\\.)*)"', body):
        values.append(rust_unescape(match.group(1)))
    return values


def parse_manifest_static(src: str) -> list[tuple[str, str]]:
    list_body = "pub(crate) static MANIFEST: &[(&str, &str)] = &["
    start = src.index(list_body)
    end = src.index("];", start)
    body = src[start:end]
    pairs = []
    for logical, digested in re.findall(r'\("((?:[^"\\]|\\.)*)", "((?:[^"\\]|\\.)*)"\),', body):
        pairs.append((rust_unescape(logical), rust_unescape(digested)))
    return pairs


def parse_files_static(src: str) -> list[tuple[str, str]]:
    list_body = "pub(crate) static FILES: &[(&str, &[u8])] = &["
    start = src.index(list_body)
    end = src.index("];", start)
    body = src[start:end]
    files = []
    for url, ident in re.findall(r'\("((?:[^"\\]|\\.)*)", ([A-Za-z_][A-Za-z0-9_.]*(?:\.as_bytes\(\))?)\),', body):
        files.append((rust_unescape(url), ident))
    return files


def ruby_extname(path: str) -> str:
    base = path.rsplit("/", 1)[-1]
    trimmed = base.lstrip(".")
    dot = trimmed.rfind(".")
    return trimmed[dot:] if dot >= 0 else ""


def mime_type(url: str) -> tuple[str, bool]:
    ext = ruby_extname(url).lower()
    if ext in MIME_TYPES:
        return MIME_TYPES[ext], True
    return "text/plain", False


def httpdate(epoch: int) -> str:
    import time

    return time.strftime("%a, %d %b %Y %H:%M:%S GMT", time.gmtime(epoch))


def load_path_dirs(workspace: Path) -> list[tuple[str, Path]]:
    """(label, absolute dir) for overrides + vendor/LOAD_PATH, in scan order."""
    crate = workspace / "crates" / "assets"
    dirs: list[tuple[str, Path]] = [("overrides", crate / "overrides")]
    for line in (crate / "vendor" / "LOAD_PATH").read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        kind, _, rest = line.partition(":")
        if kind == "reference":
            dirs.append((f"reference:{rest}", workspace / "reference" / rest))
        elif kind == "vendor":
            dirs.append((f"vendor:{rest}", crate / "vendor" / rest))
        else:
            fail(f"vendor/LOAD_PATH: bad line {line!r}")
    return dirs


def category_of(source: Path, workspace: Path) -> tuple[str, str | None]:
    rel = source.relative_to(workspace)
    parts = rel.parts
    if parts[0] == "reference":
        return "reference", None
    if parts[0] == "crates" and len(parts) > 2 and parts[1] == "assets" and parts[2] == "overrides":
        return "overrides", None
    if parts[0] == "crates" and len(parts) > 3 and parts[1] == "assets" and parts[2] == "vendor":
        return "vendored-gem", parts[3]
    fail(f"body source outside the known trees: {rel}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out-dir", required=True, type=Path, help="cargo build OUT_DIR containing embedded.rs")
    parser.add_argument("--workspace", required=True, type=Path, help="the /tmp rust-ref workspace copy")
    parser.add_argument("--reference", type=Path, default=None, help="pinned once-campfire checkout (default: WORKSPACE/reference)")
    parser.add_argument("--fixtures", type=Path, default=None, help="fixture output dir (default: repo tests/fixtures/assets)")
    parser.add_argument("--build-command", required=True, help="exact cargo command that produced OUT_DIR")
    parser.add_argument("--rust-ref-revision", required=True)
    parser.add_argument("--source-revision", required=True, help="once-campfire commit of reference/")
    parser.add_argument("--source-date-epoch", type=int, default=None)
    parser.add_argument("--network-note", default="")
    args = parser.parse_args()

    out_dir: Path = args.out_dir.resolve()
    workspace: Path = args.workspace.resolve()
    reference: Path = (args.reference or workspace / "reference").resolve()
    fixtures: Path = (args.fixtures or Path(__file__).resolve().parents[4] / "tests" / "fixtures" / "assets").resolve()

    embedded_path = out_dir / "embedded.rs"
    if not embedded_path.is_file():
        fail(f"{embedded_path} not found; run cargo build -p campfire_assets first")
    src = embedded_path.read_text()

    include_paths = parse_include_bytes(src)
    files = parse_files_static(src)
    manifest_static = parse_manifest_static(src)
    stylesheet_logicals = parse_quoted_str_list(src, "STYLESHEETS")
    manifest_json = read_static_str(src, "MANIFEST_JSON")
    importmap_tags = read_static_str(src, "IMPORTMAP_TAGS")
    built_at = read_static_str(src, "BUILT_AT")
    manifest_map = json.loads(manifest_json)

    if args.source_date_epoch is not None and built_at != httpdate(args.source_date_epoch):
        fail(f"BUILT_AT {built_at!r} != SOURCE_DATE_EPOCH {args.source_date_epoch} httpdate")

    # --- cross-checks against the build's own emitted data --------------------------------
    static_pairs = sorted(manifest_static)
    json_pairs = sorted((logical, entry["digested_path"]) for logical, entry in manifest_map.items())
    if static_pairs != json_pairs:
        only_static = set(static_pairs) - set(json_pairs)
        only_json = set(json_pairs) - set(static_pairs)
        fail(f"MANIFEST static != MANIFEST_JSON (static-only {only_static}, json-only {only_json})")
    if [l for l, _ in manifest_static] != sorted(manifest_map):
        fail("MANIFEST static is not sorted by logical path")

    digested_to_logical: dict[str, str] = {}
    for logical, entry in manifest_map.items():
        digested = entry["digested_path"]
        if set(entry) != {"digested_path", "integrity"} or entry["integrity"] is not None:
            fail(f"unexpected manifest entry shape for {logical!r}: {entry!r}")
        if not digested:
            fail(f"empty digested path for {logical!r}")
        if digested in digested_to_logical:
            fail(f"digested path collision: {digested}")
        digested_to_logical[digested] = logical

    # compiled bodies: out/compiled/<digested>
    compiled_dir = out_dir / "compiled"
    compiled_files = sorted(p for p in compiled_dir.rglob("*") if p.is_file()) if compiled_dir.is_dir() else []
    compiled_digests = {str(p.relative_to(compiled_dir)) for p in compiled_files}
    expected_compiled = {e["digested_path"] for l, e in manifest_map.items() if ruby_extname(l) in (".css", ".js")}
    if compiled_digests != expected_compiled:
        fail(
            f"compiled dir != css/js digested paths "
            f"(extra {sorted(compiled_digests - expected_compiled)}, missing {sorted(expected_compiled - compiled_digests)})"
        )

    # FILES: /assets/<digested> exactly, plus the manifest, plus reference/public
    asset_urls = {f"{ASSET_PREFIX}/{d}" for d in digested_to_logical}
    file_urls = [url for url, _ in files]
    if file_urls != sorted(set(file_urls)):
        fail("FILES is not sorted/deduplicated")
    served_assets = {url for url in file_urls if url.startswith(ASSET_PREFIX + "/") and url != MANIFEST_URL}
    if served_assets != asset_urls:
        fail(f"FILES /assets URLs != manifest digested paths (extra {sorted(served_assets - asset_urls)}, missing {sorted(asset_urls - served_assets)})")
    if MANIFEST_URL not in file_urls:
        fail("FILES is missing the served manifest")
    if {url for url in file_urls if not url.startswith(ASSET_PREFIX + "/")} != {
        url for url, ident in files if ident.startswith("PUBLIC_")
    }:
        fail("FILES public URLs do not match PUBLIC_ identifiers")

    expected_stylesheets = sorted(l for l in manifest_map if ruby_extname(l) == ".css")
    if stylesheet_logicals != expected_stylesheets:
        fail(f"STYLESHEETS != css logicals ({stylesheet_logicals} vs {expected_stylesheets})")

    # --- resolve every body and its provenance -------------------------------------------
    scan_dirs = load_path_dirs(workspace)
    overrides_dir = workspace / "crates" / "assets" / "overrides"

    entries: list[dict] = []
    seen_public_paths: set[str] = set()
    for url, ident in files:
        public_path = url.lstrip("/")
        if public_path in seen_public_paths:
            fail(f"duplicate fixture path {public_path}")
        seen_public_paths.add(public_path)
        entry: dict = {
            "url": url,
            "public_path": public_path,
            "fixture": f"tests/fixtures/assets/public/{public_path}",
            "content_type": mime_type(url)[0],
            "mime_known": mime_type(url)[1],
        }

        if url == MANIFEST_URL:
            body = manifest_json.encode()
            entry.update(body="propshaft-manifest", bytes=len(body), sha256=sha256_bytes(body), logical_path=None, digested_path=None)
        elif ident == "MANIFEST_JSON.as_bytes()":
            fail(f"unexpected MANIFEST_JSON reference at {url}")
        elif ident.startswith("ASSET_"):
            digested = url[len(ASSET_PREFIX) + 1 :]
            logical = digested_to_logical.get(digested)
            if logical is None:
                fail(f"{url} is served but absent from the manifest")
            compiled = compiled_dir / digested
            if compiled.is_file():
                body_path = compiled
                body_kind = "compiled"
                source = None
                for _, directory in scan_dirs:
                    candidate = directory / logical
                    if candidate.is_file():
                        source = candidate
                        break
                if source is None:
                    fail(f"no load-path source for {logical!r}")
                if include_paths[ident] != str(compiled):
                    fail(f"{ident} include path {include_paths[ident]!r} != compiled body {compiled}")
            else:
                body_path = Path(include_paths[ident]).resolve()
                body_kind = "source"
                source = body_path
                resolved = None
                for _, directory in scan_dirs:
                    candidate = directory / logical
                    if candidate.is_file():
                        resolved = candidate.resolve()
                        break
                if resolved is None or resolved != body_path:
                    fail(f"load path resolves {logical!r} to {resolved}, but the build embedded {body_path}")
                if logical.endswith((".css", ".js")) and (overrides_dir / logical).is_file():
                    # overrides with a css/js extension are compiled; source bodies here are not
                    fail(f"{logical!r} is css/js but has no compiled body")
            if not body_path.is_file():
                fail(f"missing body {body_path} for {url}")
            body_bytes = body_path.read_bytes()
            category, gem = category_of(source, workspace)
            entry.update(
                body=body_kind,
                bytes=len(body_bytes),
                sha256=sha256_bytes(body_bytes),
                logical_path=logical,
                digested_path=digested,
                source=str(source.relative_to(workspace)),
                source_sha256=sha256_file(source),
                category=category,
                gem=gem,
            )
        elif ident.startswith("PUBLIC_"):
            body_path = Path(include_paths[ident]).resolve()
            if not body_path.is_file():
                fail(f"missing public body {body_path} for {url}")
            if str(body_path) != str((reference / "public" / url.lstrip("/")).resolve()):
                fail(f"PUBLIC_ body {body_path} != reference/public{url}")
            body_bytes = body_path.read_bytes()
            entry.update(
                body="public",
                bytes=len(body_bytes),
                sha256=sha256_bytes(body_bytes),
                logical_path=None,
                digested_path=None,
                source=str(body_path.relative_to(workspace)),
                source_sha256=sha256_bytes(body_bytes),
                category="reference",
                gem=None,
            )
        else:
            fail(f"unknown FILES identifier {ident!r} for {url}")

        entries.append(entry)

    entries.sort(key=lambda e: e["url"])

    # --- write the fixture tree -----------------------------------------------------------
    public_dir = fixtures / "public"
    if public_dir.exists():
        shutil.rmtree(public_dir)
    for entry in entries:
        dest = public_dir / entry["public_path"]
        dest.parent.mkdir(parents=True, exist_ok=True)
        if entry["url"] == MANIFEST_URL:
            data = manifest_json.encode()
        else:
            src_path = (
                compiled_dir / entry["digested_path"]
                if entry["body"] == "compiled"
                else (reference / "public" / entry["url"].lstrip("/") if entry["body"] == "public" else workspace / entry["source"])
            )
            data = src_path.read_bytes()
        dest.write_bytes(data)

    # auxiliary reference-provided files
    aux: list[dict] = []

    def copy_aux(src: Path, rel: str, note: str) -> None:
        if not src.is_file():
            fail(f"missing auxiliary file {src}")
        dest = fixtures / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dest)
        aux.append({"fixture": f"tests/fixtures/assets/{rel}", "source": str(src), "sha256": sha256_file(dest), "bytes": dest.stat().st_size, "note": note})

    copy_aux(reference / "config" / "importmap.rb", "importmap.rb", "reference import-map pin source (config/importmap.rb)")
    copy_aux(reference / "MIT-LICENSE", "LICENSE.once-campfire", "license of the asset bodies copied from the pinned once-campfire tree")
    copy_aux(workspace / "MIT-LICENSE", "LICENSE.once-campfire-rust", "license of the rust-ref overrides/build inputs")

    tags_path = fixtures / "importmap-tags.html"
    tags_path.write_bytes(importmap_tags.encode())

    tags_json = re.search(r'<script type="importmap"[^>]*>(\{.*?\})</script>', importmap_tags, re.S)
    if not tags_json:
        fail("cannot find the importmap JSON inside IMPORTMAP_TAGS")
    importmap_obj = json.loads(tags_json.group(1))
    if set(importmap_obj) != {"imports"}:
        fail(f"unexpected import map shape: {sorted(importmap_obj)}")
    for name, path in importmap_obj["imports"].items():
        digested = path[len(ASSET_PREFIX) + 1 :]
        if digested not in digested_to_logical:
            fail(f"importmap pin {name!r} -> {path!r} not in the manifest")
    json_path = fixtures / "importmap.json"
    json_path.write_bytes((json.dumps(importmap_obj, indent=2) + "\n").encode())
    preloads = re.findall(r'<link rel="modulepreload" href="([^"]*)">', importmap_tags)
    for href in preloads:
        if href[len(ASSET_PREFIX) + 1 :] not in digested_to_logical:
            fail(f"importmap preload {href!r} not in the manifest")

    # rust-ref's committed Rails-generated fixtures, copied for the record and cross-checks
    ref_src_dir = workspace / "crates" / "assets" / "tests" / "reference"
    reference_copies = [
        ("manifest.json", "Rails assets:precompile manifest (Propshaft) for the pinned reference"),
        ("compiled_sha256.json", "sha256 of the Rails-precompiled CSS/JS bodies, keyed by digested path"),
        ("javascript_importmap_tags.html", "Rails-rendered javascript_importmap_tags for the application entry point"),
    ]
    reference_entries: list[dict] = []
    for name, note in reference_copies:
        src = ref_src_dir / name
        if not src.is_file():
            fail(f"missing reference fixture {src}")
        dest = fixtures / "reference" / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dest)
        reference_entries.append(
            {
                "fixture": f"tests/fixtures/assets/reference/{name}",
                "source": f"crates/assets/tests/reference/{name} (tmp/rust-ref)",
                "sha256": sha256_file(dest),
                "bytes": dest.stat().st_size,
                "note": note,
            }
        )
    copy_aux(workspace / "crates" / "assets" / "vendor" / "MANIFEST.md", "VENDOR-MANIFEST.md", "vendored gem provenance (all MIT)")

    # --- cross-checks against the copied reference fixtures -------------------------------
    # The reference fixture records the sha256 of every Rails-precompiled asset body
    # (not only CSS/JS), keyed by digested path.
    rails_manifest = json.loads((fixtures / "reference" / "manifest.json").read_text())
    compiled_sha = json.loads((fixtures / "reference" / "compiled_sha256.json").read_text())
    fixture_hashes = {e["digested_path"]: e["sha256"] for e in entries if e["digested_path"] is not None}
    checked = 0
    skipped: list[dict] = []
    for digested, expected in compiled_sha.items():
        if digested in fixture_hashes:
            if fixture_hashes[digested] != expected:
                fail(f"asset body {digested} sha256 {fixture_hashes[digested]} != reference {expected}")
            checked += 1
        else:
            logical = digested_to_logical.get(digested) or next(
                (l for l in rails_manifest if rails_manifest[l]["digested_path"] == digested), None
            )
            skipped.append({"digested_path": digested, "logical_path": logical, "reason": "overridden (digest changed) or absent from this build"})
    extra = sorted(set(fixture_hashes) - set(compiled_sha))
    unexplained = [d for d in extra if not (workspace / "crates" / "assets" / "overrides" / digested_to_logical[d]).is_file()]
    if unexplained:
        fail(f"asset digests absent from the reference sha256 list outside overrides/: {unexplained}")

    digest_diffs: list[str] = []
    for logical, entry in rails_manifest.items():
        ours = manifest_map.get(logical)
        if ours is None:
            fail(f"reference logical {logical!r} missing from the build")
        if ours["digested_path"] != entry["digested_path"]:
            digest_diffs.append(logical)
    added = sorted(set(manifest_map) - set(rails_manifest))
    unknown = [
        logical
        for logical in digest_diffs
        if not (workspace / "crates" / "assets" / "overrides" / logical).is_file()
    ]
    if unknown:
        fail(f"digest differs from the Rails manifest outside overrides/: {unknown}")

    ref_tags = (ref_src_dir / "javascript_importmap_tags.html").read_text()
    ref_pairs = dict(re.findall(r'"([^"]+)": "(/assets/[^"]+)"', ref_tags))
    our_pairs = dict(importmap_obj["imports"])
    tags_diffs = sorted(name for name in set(ref_pairs) | set(our_pairs) if ref_pairs.get(name) != our_pairs.get(name))
    # Every differing pin must be an overridden logical path.
    for name in tags_diffs:
        logical = digested_to_logical[our_pairs[name][len(ASSET_PREFIX) + 1 :]]
        if not (overrides_dir / logical).is_file():
            fail(f"importmap pin {name!r} differs from the Rails rendering outside overrides/")

    # --- sha256 of every written file, recomputed ----------------------------------------
    for entry in entries:
        dest = public_dir / entry["public_path"]
        if sha256_file(dest) != entry["sha256"] or dest.stat().st_size != entry["bytes"]:
            fail(f"fixture verification failed for {dest}")

    # --- MANIFEST.json -------------------------------------------------------------------
    inputs = {
        "crates/assets/build.rs": workspace / "crates" / "assets" / "build.rs",
        "crates/assets/build/propshaft.rs": workspace / "crates" / "assets" / "build" / "propshaft.rs",
        "crates/assets/build/importmap.rs": workspace / "crates" / "assets" / "build" / "importmap.rs",
        "crates/assets/vendor/LOAD_PATH": workspace / "crates" / "assets" / "vendor" / "LOAD_PATH",
        "reference/config/importmap.rb": reference / "config" / "importmap.rb",
        "reference/config/initializers/assets.rb": reference / "config" / "initializers" / "assets.rb",
    }
    for label, path in inputs.items():
        if not path.is_file():
            fail(f"missing build input {label} at {path}")

    asset_entries = [e for e in entries if e["digested_path"] is not None]
    document = {
        "format": 1,
        "task": "H03-prep",
        "purpose": (
            "Pinned outputs of the Rust reference's assets build (Propshaft port, crates/assets/build.rs): "
            "digested bodies, Propshaft manifest, import map and reference/public, extracted so H03 can serve "
            "/assets/* without re-running the Rust build. Asset names and digests come only from the build's "
            "own embedded.rs; nothing here is hand-written."
        ),
        "source_repo": "https://github.com/basecamp/once-campfire.git",
        "source_revision": args.source_revision,
        "source_reference_path": "reference/ (git submodule in tmp/rust-ref/.gitmodules, pinned to source_revision)",
        "build": {
            "crate": "crates/assets",
            "rust_ref_repo": "https://github.com/basecamp/once-campfire-rust.git",
            "rust_ref_revision": args.rust_ref_revision,
            "command": args.build_command,
            "cargo": subprocess.run(["cargo", "--version"], capture_output=True, text=True, check=True).stdout.strip(),
            "rustc": subprocess.run(["rustc", "--version"], capture_output=True, text=True, check=True).stdout.strip(),
            "source_date_epoch": args.source_date_epoch,
            "built_at": built_at,
            "network": args.network_note,
            "embedded_rs_sha256": sha256_file(embedded_path),
            "inputs_sha256": {label: sha256_file(path) for label, path in inputs.items()},
            "load_path": [{"label": label, "path": str(directory.relative_to(workspace))} for label, directory in scan_dirs],
            "assets_version": "1.0",
            "prefix": ASSET_PREFIX,
        },
        "serving": {
            "static_root_fixture": "tests/fixtures/assets/public",
            "manifest_url": MANIFEST_URL,
            "manifest_served_sha256": sha256_bytes(manifest_json.encode()),
            "cache_control": "public, max-age=2592000",
            "immutable": False,
            "last_modified": built_at,
            "content_type_rule": "Rack::Mime.mime_type port in crates/assets/src/serve.rs; no extension -> text/plain",
            "compressible_content_types": ["text/*", "application/javascript*", "image/svg+xml"],
            "compressed_variant_rule": "try <url>.br then <url>.gz; only if such a stored file exists; Vary: accept-encoding",
            "extensionless_candidates": ["<path>", "<path>.html", "<path>/index.html"],
            "gzip_or_brotli_variants_generated": False,
            "vendored_precompressed_assets": sorted(l for l in manifest_map if l.endswith((".gz", ".br"))),
            "notes": [
                "The build does not gzip; the only .gz/.br bodies are the vendored lexxy files, digested as ordinary assets at their own URLs (lexxy.js-<h>.gz, lexxy.min.js-<h>.br).",
                "<url>.br/.gz sibling negotiation therefore never matches a digested path here; .br has no Rack mime type and is served as text/plain, .gz as application/x-gzip.",
                "Conditional handling: If-Modified-Since equal to Last-Modified -> 304 with no body; range/HEAD/multipart behavior lives in crates/assets/src/serve.rs.",
                "Digest = first 8 hex chars of SHA1(content + referenced asset contents in discovery order + assets version 1.0).",
            ],
        },
        "counts": {
            "assets": len(asset_entries),
            "compiled_assets": sum(1 for e in asset_entries if e["body"] == "compiled"),
            "source_assets": sum(1 for e in asset_entries if e["body"] == "source"),
            "public_files": sum(1 for e in entries if e["body"] == "public"),
            "urls_total": len(entries),
            "importmap_imports": len(importmap_obj["imports"]),
            "importmap_preloads": len(preloads),
            "stylesheet_logicals": len(stylesheet_logicals),
        },
        "stylesheet_logicals": stylesheet_logicals,
        "importmap": {
            "tags_fixture": "tests/fixtures/assets/importmap-tags.html",
            "tags_sha256": sha256_bytes(importmap_tags.encode()),
            "json_fixture": "tests/fixtures/assets/importmap.json",
            "json_sha256": sha256_file(json_path),
            "rb_fixture": "tests/fixtures/assets/importmap.rb",
            "rb_sha256": sha256_file(fixtures / "importmap.rb"),
        },
        "reference_crosschecks": {
            "compiled_sha256": {
                "source": "tests/fixtures/assets/reference/compiled_sha256.json",
                "note": "sha256 of every Rails-precompiled asset body, keyed by digested path; all common entries matched byte-for-byte",
                "checked": checked,
                "mismatches": 0,
                "skipped": skipped,
                "extra_digests": extra,
            },
            "rails_manifest": {
                "source": "tests/fixtures/assets/reference/manifest.json",
                "logicals_matching": len(rails_manifest) - len(digest_diffs),
                "digest_differences": digest_diffs,
                "digest_differences_explained_by_overrides": all(
                    (workspace / "crates" / "assets" / "overrides" / logical).is_file() for logical in digest_diffs
                ),
                "added_by_overrides": added,
            },
            "importmap_rendering": {
                "source": "tests/fixtures/assets/reference/javascript_importmap_tags.html",
                "differing_pins": tags_diffs,
                "differing_pins_explained_by_overrides": all(
                    (workspace / "crates" / "assets" / "overrides" / digested_to_logical[our_pairs[name][len(ASSET_PREFIX) + 1 :]]).is_file()
                    for name in tags_diffs
                ),
            },
        },
        "reference_files": reference_entries,
        "auxiliary_files": aux,
        "entries": entries,
    }
    manifest_path = fixtures / "MANIFEST.json"
    manifest_path.write_text(json.dumps(document, indent=2) + "\n")

    # --- final verification pass over everything written ---------------------------------
    repo_root = Path(__file__).resolve().parents[4]
    for entry in document["entries"]:
        path = repo_root / entry["fixture"]
        if sha256_file(path) != entry["sha256"] or path.stat().st_size != entry["bytes"]:
            fail(f"post-write verification failed for {path}")
    for item in document["reference_files"] + document["auxiliary_files"]:
        path = repo_root / item["fixture"]
        if sha256_file(path) != item["sha256"]:
            fail(f"post-write verification failed for {path}")

    print(
        f"wrote {len(document['entries'])} served URLs ({len(asset_entries)} assets, "
        f"{document['counts']['compiled_assets']} compiled, {document['counts']['public_files']} public) "
        f"+ {len(document['reference_files'])} reference files + MANIFEST.json"
    )
    print(f"fixture root: {fixtures}")


if __name__ == "__main__":
    main()
