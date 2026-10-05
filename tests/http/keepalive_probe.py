#!/usr/bin/env python3
"""Keep-alive stall probe (B01b follow-up, docs/devel/evidence/keepalive-stall.md).

Measures per-request wall time on a real loopback socket against the real
server binary, on a fresh connection vs a persistent one, and fails when a
persistent connection shows the ~41 ms Nagle/delayed-ACK step that used to
follow every split (headers + body) response.

    python3 tests/http/keepalive_probe.py                 # /up, fresh vs persistent
    python3 tests/http/keepalive_probe.py --matrix        # /up, room page, gzip, 1/8 conns
    python3 tests/http/keepalive_probe.py --url 127.0.0.1:47140   # probe a running server

By default the probe starts build/dev/campfire itself with a scratch copy of
the seed database (bench/seed/.seed/default/db/production.sqlite3) and the
repo root as cwd (the static asset root is relative: tests/fixtures/assets).
The client sets TCP_NODELAY, like the pinned load generator, so any stall is
server-side.

Exit status is 0 only when every measured persistent median is within
STALL_TOLERANCE_MS of its fresh median; otherwise it prints STALL DETECTED
and exits 1 (this is the falsification switch for the fix).
"""

import argparse
import http.client
import json
import os
import re
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_BINARY = REPO / "build" / "dev" / "campfire"
DEFAULT_SEED_DB = REPO / "bench" / "seed" / ".seed" / "default" / "db" / "production.sqlite3"
LABELS = REPO / "bench" / "seed" / ".seed" / "default" / "labels.json"
SECRET = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

# A persistent median above fresh median + this fails the probe. The stall
# under repair is ~41 ms; healthy results are well under 1 ms on loopback.
STALL_TOLERANCE_MS = 5.0
READ_CHUNK = 1 << 16


def fmt_ms(samples):
    return "[" + ", ".join(f"{ms:.1f}" for ms in samples) + "]"


# --------------------------------------------------------------- HTTP read

class Response:
    __slots__ = ("status", "headers", "body")

    def __init__(self, status, headers, body):
        self.status = status
        self.headers = headers
        self.body = body


def read_response(sock, buf=b""):
    """Read exactly one Content-Length framed response, no more."""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(READ_CHUNK)
        if not chunk:
            raise EOFError("connection closed in headers")
        buf += chunk
    head, rest = buf.split(b"\r\n\r\n", 1)
    lines = head.split(b"\r\n")
    status = int(lines[0].split(b" ", 2)[1])
    headers = {}
    for line in lines[1:]:
        name, _, value = line.partition(b":")
        headers[name.strip().lower()] = value.strip()
    length = int(headers.get(b"content-length", b"0"))
    while len(rest) < length:
        chunk = sock.recv(READ_CHUNK)
        if not chunk:
            raise EOFError("connection closed in body")
        rest += chunk
    return Response(status, headers, rest[:length]), rest[length:]


def connect(port):
    sock = socket.create_connection(("127.0.0.1", port), timeout=10)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)  # loadgen-like
    return sock


def request_bytes(target, port, cookie=None, gzip_ok=False):
    headers = [
        f"GET {target} HTTP/1.1",
        f"Host: 127.0.0.1:{port}",
        "Connection: keep-alive",
    ]
    if gzip_ok:
        headers.append("Accept-Encoding: gzip")
    else:
        headers.append("Accept-Encoding: identity")
    if cookie:
        headers.append(f"Cookie: {cookie}")
    return ("\r\n".join(headers) + "\r\n\r\n").encode()


# Whole-sequence timings on one connection: every request is sent and its
# complete response read before the next send, so a Nagle hold on any one
# response shows up as a step in the sample.
def probe_persistent(port, target, samples, cookie=None, gzip_ok=False):
    req = request_bytes(target, port, cookie, gzip_ok)
    sock = connect(port)
    out = []
    try:
        buf = b""
        for _ in range(samples):
            start = time.perf_counter()
            sock.sendall(req)
            response, buf = read_response(sock, buf)
            out.append((time.perf_counter() - start) * 1000.0)
    finally:
        sock.close()
    return out


def probe_fresh(port, target, samples, cookie=None, gzip_ok=False):
    req = request_bytes(target, port, cookie, gzip_ok)
    out = []
    for _ in range(samples):
        sock = connect(port)
        try:
            start = time.perf_counter()
            sock.sendall(req)
            read_response(sock)
            out.append((time.perf_counter() - start) * 1000.0)
        finally:
            sock.close()
    return out


def probe_fresh_connections(port, target, samples, conns, cookie=None,
                            gzip_ok=False):
    """Fresh connection per request, `conns` clients in parallel; the same
    concurrency profile as probe_connections so medians are comparable."""
    results = [None] * conns

    def worker(index):
        results[index] = probe_fresh(port, target, samples, cookie, gzip_ok)

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(conns)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    return results


def probe_once(port, target, cookie=None, gzip_ok=False):
    """One request used to verify the row answers 200 before timing it."""
    sock = connect(port)
    try:
        sock.sendall(request_bytes(target, port, cookie, gzip_ok))
        response, _ = read_response(sock)
        return response.status, len(response.body)
    finally:
        sock.close()


def probe_connections(port, target, samples, conns, cookie=None, gzip_ok=False):
    """`conns` persistent connections in parallel; returns per-connection lists."""
    results = [None] * conns

    def worker(index):
        results[index] = probe_persistent(port, target, samples, cookie, gzip_ok)

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(conns)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    return results


# ------------------------------------------------------------------ server

class Server:
    def __init__(self, binary, port, db_path, cache_bytes):
        self.binary = binary
        self.port = port
        self.cache_bytes = cache_bytes
        self.tmp = Path(tempfile.mkdtemp(prefix="cf-keepalive-"))
        self.db = self.tmp / "production.sqlite3"
        shutil.copyfile(db_path, self.db)
        (self.tmp / "files").mkdir()
        self.proc = None

    def env(self):
        env = dict(os.environ)
        env.update(
            HOST="127.0.0.1",
            PORT=str(self.port),
            PUBLIC_ORIGIN=f"http://127.0.0.1:{self.port}",
            DATABASE_PATH=str(self.db),
            STORAGE_PATH=str(self.tmp / "files"),
            DISABLE_SSL="1",
            SECRET_KEY_BASE=SECRET,
            CF_CACHE_BYTES=str(self.cache_bytes),
        )
        return env

    def start(self):
        self.proc = subprocess.Popen(
            [str(self.binary)],
            cwd=str(REPO),
            env=self.env(),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                err = self.proc.stderr.read().decode(errors="replace")
                raise SystemExit(f"server exited early:\n{err}")
            try:
                sock = connect(self.port)
                sock.sendall(request_bytes("/up", self.port))
                response, _ = read_response(sock)
                sock.close()
                if response.status == 200:
                    return
            except OSError:
                time.sleep(0.05)
        raise SystemExit("server did not become ready")

    def stop(self):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        shutil.rmtree(self.tmp, ignore_errors=True)


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


# ------------------------------------------------------------------- login

def login(port, email, password):
    """Real form login; returns the Cookie header for later requests."""
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    conn.request("GET", "/session/new")
    response = conn.getresponse()
    page = response.read()
    cookies = [value.split(";", 1)[0] for name, value in response.getheaders()
               if name.lower() == "set-cookie"]
    match = re.search(rb'<meta name="csrf-token" content="([^"]*)"', page)
    token = match.group(1).decode() if match else ""
    form = urllib.parse.urlencode({
        "email_address": email, "password": password,
        "authenticity_token": token})
    conn.request("POST", "/session", body=form, headers={
        "Content-Type": "application/x-www-form-urlencoded",
        "Cookie": "; ".join(cookies),
        "Sec-Fetch-Site": "same-origin",
    })
    response = conn.getresponse()
    response.read()
    cookies += [value.split(";", 1)[0] for name, value in response.getheaders()
                if name.lower() == "set-cookie"]
    conn.close()
    if response.status != 302 or not any(c.startswith("session_token=")
                                         for c in cookies):
        raise SystemExit(f"login failed: POST /session -> {response.status}")
    return "; ".join(cookies)


# -------------------------------------------------------------------- rows

class Row:
    def __init__(self, name, target, conns=1, cookie=None, gzip_ok=False):
        self.name = name
        self.target = target
        self.conns = conns
        self.cookie = cookie
        self.gzip_ok = gzip_ok


def run_row(port, row, samples):
    status, wire = probe_once(port, row.target, row.cookie, row.gzip_ok)
    if status != 200 or wire == 0:
        raise SystemExit(f"{row.name}: {row.target} -> {status}, {wire} B")
    if row.conns == 1:
        fresh = [probe_fresh(port, row.target, samples, row.cookie,
                             row.gzip_ok)]
        persistent = [probe_persistent(port, row.target, samples,
                                       row.cookie, row.gzip_ok)]
    else:
        fresh = probe_fresh_connections(port, row.target, samples, row.conns,
                                        row.cookie, row.gzip_ok)
        persistent = probe_connections(port, row.target, samples, row.conns,
                                       row.cookie, row.gzip_ok)
    return fresh, persistent, (status, wire)


def report_row(row, fresh, persistent, check):
    fresh_med = statistics.median(ms for series in fresh for ms in series)
    persistent_med = statistics.median(ms for series in persistent
                                       for ms in series)
    step = persistent_med - fresh_med
    verdict = "STALL" if step > STALL_TOLERANCE_MS else "ok"
    print(f"{row.name}: {row.target} -> {check[0]}, {check[1]} B")
    for label, series in (("fresh", fresh), ("persistent", persistent)):
        for index, samples in enumerate(series):
            name = label if len(series) == 1 else f"{label}[{index}]"
            print(f"{'':>{len(row.name)}}  {name} {fmt_ms(samples)} "
                  f"med {statistics.median(samples):.2f} ms")
    print(f"{'':>{len(row.name)}}  -> persistent-fresh {step:+.2f} ms  "
          f"[{verdict}]")
    return step


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY,
                        help="server binary (default build/dev/campfire)")
    parser.add_argument("--seed-db", type=Path, default=DEFAULT_SEED_DB)
    parser.add_argument("--port", type=int, default=0,
                        help="port (0 picks a free one)")
    parser.add_argument("--samples", type=int, default=8,
                        help="requests per series (default 8)")
    parser.add_argument("--matrix", action="store_true",
                        help="small/large body, gzip/identity, 1/8 connections")
    parser.add_argument("--url", metavar="HOST:PORT",
                        help="probe an already running server instead of starting one")
    parser.add_argument("--json", type=Path, help="also write raw samples here")
    args = parser.parse_args()

    port = args.port or free_port()
    server = None
    results = {}
    try:
        if args.url:
            host, _, port_text = args.url.rpartition(":")
            port = int(port_text)
            if not host:
                host = "127.0.0.1"
            if host != "127.0.0.1":
                raise SystemExit("probe only supports loopback")
        else:
            if not args.binary.exists():
                raise SystemExit(f"missing binary {args.binary}; run make first")
            if not args.seed_db.exists():
                raise SystemExit(f"missing seed db {args.seed_db}; run the "
                                 "seed importer first (bench/seed/import_seed.py)")
            # Cache enabled: the room-page rows need gzip negotiation, which
            # the port ties to the cache (B01b-prep finding b).
            server = Server(args.binary, port, args.seed_db, 64 << 20)
            server.start()
            print(f"server: {args.binary} on 127.0.0.1:{port} "
                  f"(seed copy {server.tmp})")

        rows = [Row("/up small identity", "/up")]
        if args.matrix:
            cookie = None
            if LABELS.exists():
                labels = json.loads(LABELS.read_text())
                cookie = login(port, labels["emails.david"],
                               labels["passwords.all"])
                print("room page: logged in as David")
            else:
                print("room page: labels.json missing; using /users/me/sidebar "
                      "instead", file=sys.stderr)
            large = "/rooms/1" if cookie else "/users/me/sidebar"
            rows += [
                Row("room large identity", large, cookie=cookie),
                Row("room large gzip", large, cookie=cookie, gzip_ok=True),
                Row("/up small 8 conns", "/up", conns=8),
                Row("room gzip 8 conns", large, conns=8, cookie=cookie,
                    gzip_ok=True),
            ]

        fail = False
        for row in rows:
            fresh, persistent, check = run_row(port, row, args.samples)
            step = report_row(row, fresh, persistent, check)
            fail = fail or step > STALL_TOLERANCE_MS
            results[row.name] = {"fresh": fresh, "persistent": persistent,
                                 "status": check[0], "wire_bytes": check[1]}

        if args.json:
            args.json.write_text(json.dumps(results, indent=2))
            print(f"raw samples: {args.json}")

        if fail:
            print("STALL DETECTED: persistent connections step above "
                  f"{STALL_TOLERANCE_MS} ms over fresh", file=sys.stderr)
            return 1
        print("probe ok: persistent == fresh (no stall)")
        return 0
    finally:
        if server is not None:
            server.stop()


if __name__ == "__main__":
    sys.exit(main())
