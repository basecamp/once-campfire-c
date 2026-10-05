#!/usr/bin/env python3
"""Run the pinned preflight against an already-running server.

`check_bodies.py` starts the local C binary itself; this variant points the
same pinned `validate.py` preflight at any base URL, which is how the
reference images (Rails, Rust) are validated when they run as containers:

    bench/check/check_served.py --base http://127.0.0.1:55081 \
        --seed bench/seed/.seed/default --db /tmp/scratch/db/production.sqlite3 \
        --out /tmp/rails-preflight.json

It signs in through the real form with the seed's david credentials, reads the
CSS href from the room page, and executes the pinned (adapter-patched)
preflight.  The patch only makes the Elixir verification module lazily
imported; the assertions are untouched (bench/adapters/c-app.patch).
"""

from __future__ import annotations

import argparse
import gzip
import http.client
import json
import re
import shutil
import subprocess
import sys
import urllib.parse
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
WORK = REPO_ROOT / "bench" / ".work" / "pinned"


def request(host: str, port: int, method: str, path: str,
            cookie: str | None = None, headers: dict | None = None,
            body: bytes | None = None):
    conn = http.client.HTTPConnection(host, port, timeout=30)
    hdrs = dict(headers or {})
    if cookie:
        hdrs["Cookie"] = cookie
    conn.request(method, path, body=body, headers=hdrs)
    response = conn.getresponse()
    data = response.read()
    info = {"status": response.status,
            "location": response.getheader("Location"),
            "content_encoding": response.getheader("Content-Encoding"),
            "set_cookie": [v for k, v in response.getheaders()
                           if k.lower() == "set-cookie"]}
    conn.close()
    return info, data


def login(host: str, port: int, email: str, password: str) -> str:
    info, data = request(host, port, "GET", "/session/new")
    if info["status"] != 200:
        raise SystemExit(f"login: /session/new returned {info['status']}")
    cookies = [value.split(";", 1)[0] for value in info["set_cookie"]]
    match = re.search(rb'<meta name="csrf-token" content="([^"]*)"', data)
    token = match.group(1).decode() if match else ""
    form = urllib.parse.urlencode({"email_address": email, "password": password,
                                   "authenticity_token": token})
    info, _ = request(host, port, "POST", "/session",
                      cookie="; ".join(cookies), headers={
                          "Content-Type": "application/x-www-form-urlencoded",
                          "Sec-Fetch-Site": "same-origin"}, body=form.encode())
    cookies += [value.split(";", 1)[0] for value in info["set_cookie"]]
    if info["status"] != 302 or not any(c.startswith("session_token=")
                                         for c in cookies):
        raise SystemExit(f"login: POST /session returned {info['status']}")
    return "; ".join(cookies)


def prepare_work_copy() -> Path:
    if WORK.exists():
        shutil.rmtree(WORK)
    shutil.copytree(REPO_ROOT / "bench" / "pinned" / "once-campfire-elixir",
                    WORK)
    with (REPO_ROOT / "bench" / "adapters" / "c-app.patch").open() as patch:
        subprocess.run(["patch", "-p1", "-s"], cwd=WORK, stdin=patch,
                       check=True)
    return WORK


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, help="http://host:port")
    parser.add_argument("--seed", default=str(REPO_ROOT / "bench" / "seed" /
                                               ".seed" / "default"))
    parser.add_argument("--db", required=True,
                        help="the database file the server is using")
    parser.add_argument("--out", required=True,
                        help="where validate.py writes its evidence JSON")
    args = parser.parse_args()

    labels = json.loads((Path(args.seed) / "labels.json").read_text())
    split = urllib.parse.urlsplit(args.base)
    host, port = split.hostname, split.port or 80
    room = labels["rooms.watercooler"]
    before = labels["messages.busy_060"]
    avatar = labels["avatar_tokens.jason"]

    cookie = login(host, port, labels["emails.david"], labels["passwords.all"])
    info, data = request(host, port, "GET", f"/rooms/{room}", cookie=cookie,
                         headers={"Accept-Encoding": "gzip"})
    if info["content_encoding"] == "gzip":
        data = gzip.decompress(data)
    match = re.search(rb'href="(/assets/[^"]+\.css)"', data)
    css = match.group(1).decode() if match else "/assets/missing.css"

    work = prepare_work_copy()
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    proc = subprocess.run(
        [sys.executable, str(work / "bench" / "validate.py"), "preflight",
         args.base, cookie, str(room), str(before), avatar, css, args.db,
         str(out)], capture_output=True, text=True)
    result = {"base": args.base, "returncode": proc.returncode,
              "stderr": proc.stderr, "validation": None}
    if out.is_file():
        result["validation"] = json.loads(out.read_text())
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if proc.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
