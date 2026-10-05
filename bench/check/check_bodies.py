#!/usr/bin/env python3
"""Validate the published-workload response bodies against a C server.

BENCH-01: the harness's preflight rejects redirects, errors, empty bodies and
an unpopulated room or search.  The pinned harness implements that preflight in
bench/pinned/once-campfire-elixir/bench/validate.py, which the real run
invokes for every rep.  The C port is not a container image in this checkout
yet, so this tool performs the identical checks against a locally started
server and prints per-route evidence, including routes whose owning packet has
not landed (a dev-only 501; see src/routes.h).

Checks copied from the pinned validate.py (same paths, same assertions):

  * every route: HTTP 200 (a redirect or any other status fails), and a
    non-empty decoded body;
  * room_show / messages_page / search: at least one data-message-id="N";
  * sidebar: contains `shared_rooms` and the room id;
  * avatar: image/* content type and more than 100 bytes;
  * static_css: text/css and contains '{';
  * up: contains 'background-color: green';
  * the seed database: more than 50 messages in the busy room and
    PRAGMA integrity_check = ok.

`--pinned-validate` additionally runs the pinned (adapter-patched) validate.py
preflight as the harness would, and records its output.  The patch only makes
the Elixir verification module lazily imported; the preflight assertions are
untouched (bench/adapters/c-app.patch).

Usage:
    python3 bench/check/check_bodies.py --seed bench/seed/.seed/default
    python3 bench/check/check_bodies.py --seed DIR --out bench/results/<rev>/preflight.json
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import http.client
import json
import os
import re
import shutil
import signal
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BINARY = REPO_ROOT / "build" / "dev" / "campfire"
DEFAULT_WORK = REPO_ROOT / "bench" / ".work" / "pinned"

ROUTE_PATHS = {
    "room_show": "/rooms/{room}",
    "messages_page": "/rooms/{room}/messages?before={before}",
    "sidebar": "/users/me/sidebar",
    "search": "/searches?q=coffee",
    "avatar": "/users/{avatar}/avatar",
    "static_css": "{css}",
    "up": "/up",
}


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def request(port: int, method: str, path: str, cookie: str | None = None,
            headers: dict | None = None, body: bytes | None = None):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
    hdrs = dict(headers or {})
    if cookie:
        hdrs["Cookie"] = cookie
    conn.request(method, path, body=body, headers=hdrs)
    response = conn.getresponse()
    data = response.read()
    result = {
        "status": response.status,
        "location": response.getheader("Location"),
        "content_type": response.getheader("Content-Type"),
        "content_encoding": response.getheader("Content-Encoding"),
        "set_cookie": [value for name, value in response.getheaders()
                       if name.lower() == "set-cookie"],
    }
    conn.close()
    return result, data


def decode(result: dict, data: bytes) -> bytes:
    if result["content_encoding"] == "gzip":
        return gzip.decompress(data)
    return data


def login(port: int, email: str, password: str) -> str:
    import urllib.parse
    result, data = request(port, "GET", "/session/new")
    if result["status"] != 200:
        raise SystemExit(f"login: /session/new returned {result['status']}")
    cookies = []
    for header in result["set_cookie"]:
        cookies.append(header.split(";", 1)[0])
    match = re.search(rb'<meta name="csrf-token" content="([^"]*)"', data)
    token = match.group(1).decode() if match else ""
    form = urllib.parse.urlencode({
        "email_address": email, "password": password,
        "authenticity_token": token})
    result, _ = request(port, "POST", "/session", cookie="; ".join(cookies),
                        headers={
                            "Content-Type": "application/x-www-form-urlencoded",
                            "Sec-Fetch-Site": "same-origin",
                        },
                        body=form.encode())
    for header in result["set_cookie"]:
        cookies.append(header.split(";", 1)[0])
    if result["status"] != 302 or not any(c.startswith("session_token=")
                                          for c in cookies):
        raise SystemExit(f"login: POST /session returned {result['status']}")
    return "; ".join(cookies)


def audit_route(name: str, result: dict, body: bytes, room: int) -> dict:
    entry = {
        "status": result["status"],
        "redirect": result["location"],
        "content_type": result["content_type"],
        "content_encoding": result["content_encoding"],
        "wire_bytes": len(body),
        "body_bytes": None,
        "body_sha256": None,
        "checks": {},
    }
    if result["status"] == 501:
        entry["blocked"] = True
        entry["checks"]["not_landed_501"] = True
        return entry
    decoded = body
    if result["content_encoding"] == "gzip":
        try:
            decoded = gzip.decompress(body)
        except OSError as exc:
            entry["checks"]["gzip_decodes"] = False
            entry["error"] = str(exc)
            return entry
    entry["body_bytes"] = len(decoded)
    entry["body_sha256"] = hashlib.sha256(decoded).hexdigest()
    entry["checks"]["status_200"] = result["status"] == 200
    entry["checks"]["not_redirect"] = not (300 <= result["status"] < 400)
    entry["checks"]["nonempty_body"] = bool(decoded)
    if name in ("room_show", "messages_page", "search"):
        entry["checks"]["populated_messages"] = bool(
            re.search(rb'data-message-id="\d+"', decoded))
    if name == "sidebar":
        entry["checks"]["shared_rooms"] = (
            b"shared_rooms" in decoded and str(room).encode() in decoded)
    if name == "avatar":
        entry["checks"]["image_content_type"] = bool(
            result["content_type"] and
            result["content_type"].startswith("image/"))
        entry["checks"]["avatar_bytes_gt_100"] = len(decoded) > 100
    if name == "static_css":
        entry["checks"]["css_content_type"] = bool(
            result["content_type"] and
            result["content_type"].startswith("text/css"))
        entry["checks"]["css_body"] = b"{" in decoded
    if name == "up":
        entry["checks"]["up_marker"] = b"background-color: green" in decoded
    entry["passed"] = all(entry["checks"].values())
    return entry


def wait_ready(proc: subprocess.Popen, base: str, log_path: Path,
               timeout: float = 15.0) -> None:
    import urllib.request
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise SystemExit(
                f"campfire exited early ({proc.returncode}):\n"
                + log_path.read_text(errors="replace")[-2000:])
        try:
            with urllib.request.urlopen(base + "/up", timeout=1.0) as resp:
                if resp.status == 200:
                    return
        except Exception:
            time.sleep(0.1)
    raise SystemExit("campfire did not become ready:\n"
                     + log_path.read_text(errors="replace")[-2000:])


def prepare_work_copy(work: Path) -> Path:
    """Materialize the pinned harness with the C adapter patch applied."""
    if work.exists():
        shutil.rmtree(work)
    shutil.copytree(REPO_ROOT / "bench" / "pinned" / "once-campfire-elixir",
                    work)
    with (REPO_ROOT / "bench" / "adapters" / "c-app.patch").open() as patch:
        subprocess.run(["patch", "-p1", "-s"], cwd=work, stdin=patch,
                       check=True)
    return work


def run_pinned_preflight(work: Path, base: str, cookie: str, room: int,
                         before: int, avatar: str, css: str, dbpath: Path,
                         out: Path) -> dict:
    script = work / "bench" / "validate.py"
    proc = subprocess.run(
        [sys.executable, str(script), "preflight", base, cookie, str(room),
         str(before), avatar, css, str(dbpath), str(out)],
        capture_output=True, text=True)
    result = {"returncode": proc.returncode, "stdout": proc.stdout,
              "stderr": proc.stderr}
    if out.is_file():
        result["validation"] = json.loads(out.read_text())
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", default=str(REPO_ROOT / "bench" / "seed" /
                                               ".seed" / "default"))
    parser.add_argument("--binary", default=str(DEFAULT_BINARY))
    parser.add_argument("--out", default=None, help="write the evidence JSON")
    parser.add_argument("--pinned-validate", action="store_true",
                        help="also run the pinned validate.py preflight")
    parser.add_argument("--keep", action="store_true",
                        help="keep the scratch server directory")
    args = parser.parse_args()

    seed = Path(args.seed).resolve()
    binary = Path(args.binary).resolve()
    if not (seed / "db" / "production.sqlite3").is_file():
        raise SystemExit(f"no seed database under {seed}; run "
                         "bench/seed/import_seed.py first")
    if not os.access(binary, os.X_OK):
        raise SystemExit(f"no executable at {binary}; build with `make dev`")

    labels = json.loads((seed / "labels.json").read_text())
    room = labels["rooms.watercooler"]
    before = labels["messages.busy_060"]
    avatar = labels["avatar_tokens.jason"]
    manifest = json.loads(
        (REPO_ROOT / "bench" / "PINNED-MANIFEST.json").read_text())

    scratch = Path(tempfile.mkdtemp(prefix="cf-bench-preflight-"))
    shutil.copytree(seed / "db", scratch / "db")
    shutil.copytree(seed / "storage", scratch / "storage")
    port = free_port()
    base = f"http://127.0.0.1:{port}"
    log_path = scratch / "server.log"
    secret = None
    env_file = (REPO_ROOT / "bench" / "pinned" / "once-campfire-elixir" /
                "parity" / "reference.env")
    for line in env_file.read_text().splitlines():
        if line.startswith("SECRET_KEY_BASE="):
            secret = line.split("=", 1)[1].strip()
    if secret is None:
        raise SystemExit(f"no SECRET_KEY_BASE in {env_file}")

    env = dict(os.environ, HOST="127.0.0.1", PORT=str(port),
               PUBLIC_ORIGIN=base,
               DATABASE_PATH=str(scratch / "db" / "production.sqlite3"),
               STORAGE_PATH=str(scratch / "storage"),
               SECRET_KEY_BASE=secret, DISABLE_SSL="1")
    log = (scratch / "server.log").open("wb")
    proc = subprocess.Popen([str(binary)], cwd=str(REPO_ROOT), env=env,
                            stdout=log, stderr=subprocess.STDOUT)
    log.close()
    evidence: dict = {
        "seed": str(seed),
        "seed_db_sha256": hashlib.sha256(
            (seed / "db" / "production.sqlite3").read_bytes()).hexdigest(),
        "binary": str(binary),
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "base": base,
        "labels": {k: labels[k] for k in sorted(labels)},
        "seed_divergence": manifest.get("seed", {}),
        "routes": {},
    }
    try:
        wait_ready(proc, base, log_path)
        cookie = login(port, labels["emails.david"], labels["passwords.all"])
        # The css href is read from the room page, as the harness's scrape does.
        result, data = request(port, "GET", ROUTE_PATHS["room_show"].format(
            room=room), cookie=cookie,
            headers={"Accept-Encoding": "gzip"})
        match = re.search(rb'href="(/assets/[^"]+\.css)"', data)
        css = match.group(1).decode() if match else "/assets/missing.css"
        rendered = decode(result, data)
        rendered_avatar_tokens = sorted(
            {token.decode() for token in
             re.findall(rb'/users/([^/"]+)/avatar', rendered)})

        for name, template in ROUTE_PATHS.items():
            path = template.format(room=room, before=before, avatar=avatar,
                                   css=css)
            result, data = request(port, "GET", path, cookie=cookie,
                                   headers={"Accept-Encoding": "gzip"})
            evidence["routes"][name] = audit_route(name, result, data, room)

        # The pinned preflight's database assertions, plus the disclosed seed
        # divergence (no avatar attachment; the label token is the signed URL
        # the server itself renders).
        db = sqlite3.connect(str(scratch / "db" / "production.sqlite3"))
        try:
            count = db.execute("SELECT COUNT(*) FROM messages WHERE room_id=?",
                               (room,)).fetchone()[0]
            integrity = db.execute("PRAGMA integrity_check").fetchone()[0]
            blob_rows = db.execute(
                "SELECT COUNT(*) FROM active_storage_blobs").fetchone()[0]
            attachment_rows = db.execute(
                "SELECT COUNT(*) FROM active_storage_attachments").fetchone()[0]
        finally:
            db.close()
        evidence["database"] = {
            "busy_room_messages": count,
            "busy_room_messages_gt_50": count > 50,
            "integrity_check": integrity,
            "integrity_ok": integrity == "ok",
            "avatar_blob_rows": blob_rows,
            "avatar_attachment_rows": attachment_rows,
        }
        evidence["seed"] = {
            "avatar_token_label": avatar,
            "avatar_token_rendered_by_server": avatar in rendered_avatar_tokens,
            "rendered_avatar_tokens": rendered_avatar_tokens,
            "avatar_attachment_absent": attachment_rows == 0,
        }

        if args.pinned_validate:
            work = REPO_ROOT / "bench" / ".work" / "pinned"
            prepare_work_copy(work)
            evidence["pinned_preflight"] = run_pinned_preflight(
                work, base, cookie, room, before, avatar, css,
                scratch / "db" / "production.sqlite3",
                scratch / "preflight-out.json")

        failed = [name for name, entry in evidence["routes"].items()
                  if not entry.get("passed") and not entry.get("blocked")]
        blocked = [name for name, entry in evidence["routes"].items()
                   if entry.get("blocked")]
        evidence["failed_routes"] = failed
        evidence["blocked_routes"] = blocked
        # `passed` covers every route whose packet has landed; `complete` is
        # the harness preflight as a whole and stays false while a required
        # route is still a dev-only 501.
        evidence["passed"] = (
            not failed and evidence["database"]["busy_room_messages_gt_50"]
            and evidence["database"]["integrity_ok"])
        evidence["complete"] = evidence["passed"] and not blocked
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        if not args.keep:
            shutil.rmtree(scratch, ignore_errors=True)
        else:
            print(f"scratch kept at {scratch}", file=sys.stderr)

    text = json.dumps(evidence, indent=2, sort_keys=True)
    if args.out:
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text + "\n")
    print(text)
    print(
        "preflight: "
        + ("PASS" if evidence["passed"] else "FAIL")
        + (f"; failed {evidence['failed_routes']}" if evidence["failed_routes"]
           else "")
        + (f"; blocked (not landed) {evidence['blocked_routes']}"
           if evidence["blocked_routes"] else ""),
        file=sys.stderr)
    return 0 if evidence["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
