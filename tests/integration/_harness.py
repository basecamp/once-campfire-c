"""Shared harness for the V01 browser acceptance cases (E2E-01, E2E-03).

This module is not a case: the runner ignores every file whose name starts
with an underscore, and it lives outside `cases/`, so discovery never loads
it.  Cases import it by adding `tests/integration` to `sys.path`.

It provides, using the standard library only:

  * `require_prerequisites()` - fails loudly (`PrerequisiteMissing`) when the
    dev binary or the project owner's `agent-browser` CLI is absent, and
    reports the browser version the cases were validated with.
  * `Scratch` - a per-run temporary directory (server database, storage and
    log live inside it; removed on exit).
  * `Server` - one campfire process on a free port with its own scratch
    database and `PUBLIC_ORIGIN` matching that port, readiness polling, a
    captured stderr log, and SIGTERM cleanup.
  * `Browser` - one named `agent-browser` session (its own cookies/tabs), with
    DOM assertions through `eval`, polling with deadlines, cookie export and
    screenshots.
  * SQLite seeding helpers for the second user (the join route is not landed
    in this build; see V01.md) and the WebSocket replay probe used by E2E-03.

Browser flows use the installed `agent-browser` CLI per the project owner's
direction (no Chromium download); the version used is recorded in
tests/integration/README.md and docs/devel/evidence/V01.md.
"""

from __future__ import annotations

import base64
import json
import os
import signal
import socket
import sqlite3
import ssl
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

try:  # the runner registers itself as `run` before loading any case module
    from run import CaseFailure, PrerequisiteMissing, REPO_ROOT
except Exception:  # pragma: no cover - only when imported outside the runner
    REPO_ROOT = Path(__file__).resolve().parents[2]

    class CaseFailure(Exception):
        """The case ran and observed a wrong result."""

    class PrerequisiteMissing(Exception):
        """The case could not run because a prerequisite is absent."""


# ---------------------------------------------------------------- constants

BINARY = REPO_ROOT / "build" / "dev" / "campfire"
AGENT_BROWSER = Path(
    os.environ.get(
        "AGENT_BROWSER", str(Path.home() / "devtools" / "node" / "bin" / "agent-browser")
    )
)

# The browser contexts: `--session a` / `--session b`, as directed.
SESSION_A = "a"
SESSION_B = "b"

# First-run setup sends the account administrator; the second user is seeded
# from the database because /join is a dev-501 route in this build.
ADMIN_NAME = "Alice"
ADMIN_EMAIL = "alice@example.com"
ADMIN_PASSWORD = "v01-secret-pass-123"
SECOND_NAME = "Bob"
SECOND_EMAIL = "bob@example.com"

SECRET_KEY_BASE = "ab" * 32  # 64 hex characters, the configured minimum

# The reference Ban model rejects loopback/private/link-local addresses, so a
# strictly local ban cannot proceed; E2E-03 seeds the target's recorded
# session address with a public documentation address (TEST-NET-3).
PUBLIC_TEST_IP = "203.0.113.10"

# Room id and name created by the first run (FIRST_ROOM_NAME in the pinned
# reference: tmp/rails-ref/app/models/first_run.rb).
FIRST_ROOM_ID = 1
FIRST_ROOM_NAME = "All Talk"

DEFAULT_TIMEOUT = 30.0
BROWSER_TIMEOUT = 90.0


# ------------------------------------------------------------ prerequisites


def browser_version() -> str:
    out = subprocess.run(
        [str(AGENT_BROWSER), "--version"],
        capture_output=True,
        text=True,
        timeout=30,
    )
    return (out.stdout or out.stderr).strip()


def require_prerequisites() -> None:
    if not BINARY.exists():
        raise PrerequisiteMissing(
            f"{BINARY} is not built; run `make` from {REPO_ROOT}"
        )
    if not os.access(BINARY, os.X_OK):
        raise PrerequisiteMissing(f"{BINARY} is not executable")
    if not AGENT_BROWSER.exists():
        raise PrerequisiteMissing(
            f"agent-browser CLI not found at {AGENT_BROWSER}; install it or set "
            "AGENT_BROWSER to the installed path"
        )
    if not os.access(AGENT_BROWSER, os.X_OK):
        raise PrerequisiteMissing(f"{AGENT_BROWSER} is not executable")


# ------------------------------------------------------------------- timing


def poll(fn, timeout: float, desc: str, interval: float = 0.25):
    """Run `fn` until truthy or the deadline; raise CaseFailure with `desc`."""
    deadline = time.monotonic() + timeout
    last = None
    while True:
        try:
            last = fn()
        except (CaseFailure, PrerequisiteMissing):
            raise
        except Exception as exc:  # transient browser/server hiccups
            last = exc
        if last:
            return last
        if time.monotonic() >= deadline:
            raise CaseFailure(
                f"timed out after {timeout:g}s waiting for {desc} (last observation: {last!r})"
            )
        time.sleep(interval)


def poll_js(browser: "Browser", expression: str, timeout: float, desc: str):
    return poll(
        lambda: browser.eval(expression) or None,
        timeout,
        desc,
    )


def jstr(value: str) -> str:
    """A JSON string literal safe to embed in JavaScript."""
    return json.dumps(value)


# ------------------------------------------------------------ browser flows

# The room composer and its send control (pinned room page).
COMPOSER_EDITOR = "lexxy-editor[aria-label='Write a message']"
COMPOSER_SEND = "button[name=send]"


def editor_ready(browser: "Browser", selector: str) -> bool:
    return bool(
        browser.eval(
            "(() => {const el = document.querySelector(%s);"
            " return !!(el && el.querySelector('[contenteditable]'))})()" % jstr(selector)
        )
    )


def fill_editor(
    browser: "Browser",
    selector: str,
    text: str,
    timeout: float = 20.0,
    replace: bool = False,
) -> None:
    """Type into a lexxy editor the way a user does: focus, real keystrokes.

    The custom element upgrades asynchronously, and a value set without real
    key events leaves its `isBlank` state untouched (the Send control then
    no-ops).  This waits for the contenteditable child, focuses it, types with
    real key events, and verifies both the visible text and the editor's own
    `isBlank === false` state.  With `replace=True` the existing content is
    selected first (Ctrl+A), so the typed text replaces it, as a user editing
    a message would.
    """
    poll(
        lambda: editor_ready(browser, selector),
        timeout,
        f"the editor {selector!r} to become ready",
    )
    # Focus the inner contenteditable directly; focusing the custom element
    # only delegates to it asynchronously (and sometimes not at all).
    browser.run("focus", f"{selector} [contenteditable]")
    poll(
        lambda: browser.eval(
            "!!(document.activeElement && document.activeElement.closest(%s))"
            % jstr(selector)
        ),
        timeout,
        f"focus to land inside the editor {selector!r}",
    )
    if replace:
        # A real user edit replaces the existing body; without this the typed
        # text would be appended to the loaded message.
        browser.press("Control+a")
    browser.run("keyboard", "type", text)
    observed = browser.eval(
        "(() => {const el = document.querySelector(%s);"
        " return el ? el.innerText : null})()" % jstr(selector)
    )
    ok = (observed or "").strip() == text if replace else text in (observed or "")
    if not ok:
        raise CaseFailure(
            f"editor {selector!r} did not accept {text!r}; it reads {observed!r}"
        )
    if browser.eval(
        "(() => {const el = document.querySelector(%s);"
        " return el && el.isBlank})()" % jstr(selector)
    ):
        raise CaseFailure(
            f"editor {selector!r} still reports isBlank after typing {text!r}"
        )


def post_message(browser: "Browser", text: str, timeout: float = 20.0) -> None:
    """Post a message through the room composer's real UI.

    The send control sits in the sticky footer, where a naive click can be
    swallowed by the surrounding composer chrome; `click_ui` scrolls it into
    the viewport centre and uses the page's own click path when the point is
    still covered.
    """
    fill_editor(browser, COMPOSER_EDITOR, text, timeout)
    browser.click_ui(COMPOSER_SEND)

# Reference texts pinned by tests/fixtures/crates/views/tests/golden/a/
# first_run.html and sessions_new.html (never re-derived from the candidate).
FIRST_RUN_TITLE = "Set up Campfire"
FIRST_RUN_NAME_PLACEHOLDER = "Name"
FIRST_RUN_EMAIL_PLACEHOLDER = "Email address"
FIRST_RUN_PASSWORD_PLACEHOLDER = "Password"
SIGN_IN_TITLE = "Sign in"
SIGN_IN_EMAIL_PLACEHOLDER = "Enter your email address"
SIGN_IN_PASSWORD_PLACEHOLDER = "Enter your password"


def _placeholder(browser: "Browser", selector: str):
    return browser.eval(
        f"(() => {{const el = document.querySelector({jstr(selector)});"
        " return el ? el.getAttribute('placeholder') : null})()"
    )


def first_run_setup(
    browser: "Browser",
    server: "Server",
    name: str = ADMIN_NAME,
    email: str = ADMIN_EMAIL,
    password: str = ADMIN_PASSWORD,
) -> None:
    """Drive the real first-run form; leaves the browser in the original room."""
    browser.open(server.base_url + "/")
    title = poll(
        lambda: browser.title() or None,
        20,
        "the first-run page title to become readable",
    )
    if title != FIRST_RUN_TITLE:
        raise CaseFailure(
            f"first-run page title: expected {FIRST_RUN_TITLE!r}, observed {title!r}"
        )
    observed = {
        "title": title,
        "name_placeholder": _placeholder(browser, "#user_name"),
        "email_placeholder": _placeholder(browser, "#user_email_address"),
        "password_placeholder": _placeholder(browser, "#user_password"),
        "password_maxlength": browser.eval(
            "document.querySelector('#user_password').getAttribute('maxlength')"
        ),
        "avatar_field": browser.has("#user_avatar"),
        "legend": browser.text("legend"),
    }
    expected = {
        "name_placeholder": FIRST_RUN_NAME_PLACEHOLDER,
        "email_placeholder": FIRST_RUN_EMAIL_PLACEHOLDER,
        "password_placeholder": FIRST_RUN_PASSWORD_PLACEHOLDER,
        "password_maxlength": "72",
    }
    for key, want in expected.items():
        if observed[key] != want:
            raise CaseFailure(
                f"first-run form field {key}: expected {want!r}, observed {observed[key]!r}"
            )
    if not observed["avatar_field"]:
        raise CaseFailure("first-run form is missing the avatar file field")
    if "Set up Campfire" not in (observed["legend"] or ""):
        raise CaseFailure(
            f"first-run legend: expected the pinned heading, observed {observed['legend']!r}"
        )
    print(f"[first-run] observed {observed}")

    browser.fill("#user_name", name)
    browser.fill("#user_email_address", email)
    browser.fill("#user_password", password)
    browser.click("button[name=button]")
    browser.wait_url(f"/rooms/{FIRST_ROOM_ID}", timeout=30)


def sign_in(browser: "Browser", server: "Server", email: str, password: str) -> None:
    """Drive the real sign-in form and land in the original room."""
    browser.open(server.base_url + "/session/new")
    title = poll(
        lambda: browser.title() or None,
        20,
        "the sign-in page title to become readable",
    )
    if title != SIGN_IN_TITLE:
        raise CaseFailure(
            f"sign-in page title: expected {SIGN_IN_TITLE!r}, observed {title!r}"
        )
    observed = {
        "email_placeholder": _placeholder(browser, "#email_address"),
        "password_placeholder": _placeholder(browser, "#password"),
    }
    expected = {
        "email_placeholder": SIGN_IN_EMAIL_PLACEHOLDER,
        "password_placeholder": SIGN_IN_PASSWORD_PLACEHOLDER,
    }
    for key, want in expected.items():
        if observed[key] != want:
            raise CaseFailure(
                f"sign-in form field {key}: expected {want!r}, observed {observed[key]!r}"
            )
    browser.fill("#email_address", email)
    browser.fill("#password", password)
    browser.click("button[name=log_in]")
    browser.wait_url(f"/rooms/{FIRST_ROOM_ID}", timeout=30)


# ------------------------------------------------------------------ scratch


class Scratch:
    """A per-run temporary directory removed on exit."""

    def __init__(self, prefix: str = "cf-v01-"):
        self.path = Path(tempfile.mkdtemp(prefix=prefix))
        (self.path / "data").mkdir()

    def cleanup(self) -> None:
        import shutil

        shutil.rmtree(self.path, ignore_errors=True)

    def __enter__(self) -> "Scratch":
        return self

    def __exit__(self, *exc) -> None:
        self.cleanup()


# ------------------------------------------------------------------- server


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _tls_test_context():
    # The committed certificate names www.example.com; tests listen on
    # loopback. Verify its pinned CA while disabling only hostname matching.
    certs = REPO_ROOT / "tests/fixtures/crates/campfire/src/integrations/testdata/tls"
    ctx = ssl.create_default_context(cafile=str(certs / "ca.pem"))
    ctx.check_hostname = False
    ctx.set_alpn_protocols(["http/1.1"])
    return ctx


class Server:
    """One campfire instance on a free port with its own scratch database."""

    def __init__(self, scratch: Scratch, extra_env: dict | None = None):
        self.scratch = scratch
        self.data_dir = scratch.path / "data"
        self.db_path = self.data_dir / "campfire.sqlite3"
        self.storage_path = self.data_dir / "files"
        self.log_path = scratch.path / "server.log"
        self.port = _free_port()
        self.tls = os.environ.get("CF_E2E_TLS") == "1"
        scheme = "https" if self.tls else "http"
        self.base_url = f"{scheme}://127.0.0.1:{self.port}"
        self.proc: subprocess.Popen | None = None
        # Case-specific configuration (e.g. VAPID keys for the push flow);
        # applied on top of the shared environment below.
        self.extra_env = dict(extra_env or {})

    # -- lifecycle ---------------------------------------------------------

    def _env(self) -> dict:
        env = dict(os.environ)
        env.update(
            {
                "HOST": "127.0.0.1",
                "PORT": str(self.port),
                "PUBLIC_ORIGIN": self.base_url,
                "DATABASE_PATH": str(self.db_path),
                "STORAGE_PATH": str(self.storage_path),
                "SECRET_KEY_BASE": SECRET_KEY_BASE,
                "DISABLE_SSL": "1",
            }
        )
        if self.tls:
            certs = REPO_ROOT / "tests/fixtures/crates/campfire/src/integrations/testdata/tls"
            env.update(DISABLE_SSL="0", TLS_CERT_FILE=str(certs / "server.pem"),
                       TLS_KEY_FILE=str(certs / "server.key"))
        env.update(self.extra_env)
        return env

    def start(self) -> None:
        log = open(self.log_path, "ab")
        # cwd is the repository root: the static front mount resolves the
        # compile-time default "tests/fixtures/assets" relative to it.
        self.proc = subprocess.Popen(
            [str(BINARY)],
            cwd=str(REPO_ROOT),
            env=self._env(),
            stdout=log,
            stderr=subprocess.STDOUT,
        )
        log.close()  # the child owns the descriptor

    def wait_ready(self, timeout: float = 15.0) -> None:
        import urllib.error
        import urllib.request

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc is not None and self.proc.poll() is not None:
                raise CaseFailure(
                    f"campfire exited early (code {self.proc.returncode}); log tail:\n"
                    + self.log_tail()
                )
            try:
                with urllib.request.urlopen(
                    self.base_url + "/up", timeout=1.0,
                    context=_tls_test_context() if self.tls else None,
                ) as resp:
                    if resp.status == 200:
                        return
            except Exception:
                pass
            time.sleep(0.1)
        raise CaseFailure(
            f"campfire did not become ready on {self.base_url} within {timeout:g}s; "
            "log tail:\n" + self.log_tail()
        )

    def stop(self, timeout: float = 10.0) -> None:
        if self.proc is None:
            return
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        self.proc = None

    def log_tail(self, lines: int = 30) -> str:
        try:
            text = self.log_path.read_text(errors="replace").splitlines()
        except OSError:
            return "(no server log)"
        return "\n".join(text[-lines:])

    # -- database helpers --------------------------------------------------

    def _db(self) -> sqlite3.Connection:
        return sqlite3.connect(str(self.db_path), timeout=10.0)

    def query(self, sql: str, params=()) -> list:
        with self._db() as db:
            return list(db.execute(sql, params))

    def admin_digest(self) -> str:
        rows = self.query(
            "SELECT password_digest FROM users ORDER BY id LIMIT 1"
        )
        if not rows or not rows[0][0]:
            raise CaseFailure("first-run setup did not create an administrator row")
        return rows[0][0]

    def seed_second_user(self, name: str, email: str) -> int:
        """Insert a second active user with the administrator's password.

        `/join/:join_code` (users#new/create, packet A-users) is a dev-501
        route in this build, so the only way to have two real users is to seed
        one; the case then signs in through the real form.  The seeded row
        copies the administrator's bcrypt digest, so both accounts accept the
        same password.
        """
        digest = self.admin_digest()
        now = time.strftime("%Y-%m-%d %H:%M:%S")
        db = self._db()
        try:
            cur = db.execute(
                "INSERT INTO users (name, email_address, password_digest, role, "
                "status, created_at, updated_at) VALUES (?, ?, ?, 0, 0, ?, ?)",
                (name, email, digest, now, now),
            )
            user_id = cur.lastrowid
            db.execute(
                "INSERT INTO memberships (user_id, room_id, involvement, connections, "
                "created_at, updated_at) VALUES (?, ?, 'mentions', 0, ?, ?)",
                (user_id, FIRST_ROOM_ID, now, now),
            )
            db.commit()
        finally:
            db.close()
        return user_id

    def set_session_ip(self, user_id: int, ip_address: str) -> int:
        """Point every recorded session of `user_id` at a public address.

        The reference Ban model refuses loopback/private/link-local addresses
        (`ip_address_is_public`), so banning a user whose sessions were all
        recorded from 127.0.0.1 fails in the reference too.  E2E-03 therefore
        rewrites the *target's* recorded session address (a test fixture, not
        a production path) before issuing the ban.
        """
        db = self._db()
        try:
            cur = db.execute(
                "UPDATE sessions SET ip_address = ? WHERE user_id = ?",
                (ip_address, user_id),
            )
            db.commit()
            return cur.rowcount
        finally:
            db.close()


# ------------------------------------------------------------------ browser


class Browser:
    """One named agent-browser session (its own cookies, tabs and refs)."""

    def __init__(self, session: str):
        self.session = session

    # -- raw command -------------------------------------------------------

    def run(
        self,
        *args: str,
        timeout: float = BROWSER_TIMEOUT,
        check: bool = True,
        stdin: str | None = None,
    ):
        cmd = [str(AGENT_BROWSER), "--session", self.session, *args]
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
            input=stdin,
            stdin=None if stdin is not None else subprocess.DEVNULL,
        )
        if check and proc.returncode != 0:
            raise CaseFailure(
                f"agent-browser {' '.join(args)} failed (exit {proc.returncode}): "
                f"{(proc.stderr or proc.stdout).strip()[:400]}"
            )
        return proc

    # -- lifecycle ---------------------------------------------------------

    def reset(self) -> None:
        """Close a leftover browser for this session and prime a live one.

        After `close`, the session's tab binding is stale until the next
        navigation; opening about:blank here leaves the session attached to a
        live browser so the case's first real `open` behaves.
        """
        self.run("close", check=False, timeout=30)
        self.run("open", "about:blank", check=False)

    def close(self) -> None:
        self.run("close", check=False, timeout=30)

    def open(self, url: str) -> str:
        # A freshly (re)launched browser can answer before the session's tab
        # binding is live, leaving reads on about:blank; retry the navigation
        # until the session reports a real page, within one deadline.
        deadline = time.monotonic() + 40
        last = ""
        while True:
            out = self.run("open", url, check=False).stdout.strip()
            try:
                poll(
                    lambda: self.url() or None,
                    6,
                    "the session to report a page URL",
                    interval=0.2,
                )
            except CaseFailure as exc:
                last = str(exc)
            current = self.url()
            if current and current != "about:blank":
                return out
            if time.monotonic() >= deadline:
                raise CaseFailure(
                    f"browser session {self.session!r} never attached to {url!r} "
                    f"(last URL {current!r}; {last})"
                )
            time.sleep(0.3)

    def url(self) -> str:
        proc = self.run("get", "url", check=False)
        text = (proc.stdout or "").strip()
        return text.splitlines()[0].strip() if text else ""

    def title(self) -> str:
        proc = self.run("get", "title", check=False)
        text = (proc.stdout or "").strip()
        return text.splitlines()[0].strip() if text else ""

    # -- DOM ---------------------------------------------------------------

    def eval(self, js: str):
        """Evaluate JS in the page; returns the decoded JSON value.

        `agent-browser eval --stdin` prints the serialized result, so strings
        arrive quoted and objects/arrays as JSON.  A result that does not
        decode (for example `undefined` or a thrown error) comes back as the
        raw trimmed text.
        """
        proc = self.run("eval", "--stdin", timeout=BROWSER_TIMEOUT, stdin=js)
        text = proc.stdout.strip()
        try:
            return json.loads(text)
        except json.JSONDecodeError:
            return text

    def fill(self, selector: str, text: str) -> None:
        self.run("fill", selector, text)

    def click(self, selector: str) -> None:
        self.run("click", selector)

    def hittable(self, selector: str) -> bool:
        """True when the element's centre point hit-tests to itself.

        A running view transition (the layouts use `view-transition-name`)
        paints an overlay that swallows pointer input; the element is not
        hittable until the transition finishes.
        """
        return bool(
            self.eval(
                "(() => {const el = document.querySelector(%s); if (!el) return false;"
                " const r = el.getBoundingClientRect();"
                " if (!r.width || !r.height) return false;"
                " const t = document.elementFromPoint(r.x + r.width / 2, r.y + r.height / 2);"
                " return !!(t && (t === el || el.contains(t)))})()" % jstr(selector)
            )
        )

    def click_ui(self, selector: str) -> None:
        """Click a page control, scrolling it into view first.

        The room's sticky composer covers the lower part of the page, and an
        in-flight view transition covers all of it; the click waits for the
        element to be hit-testable (bounded) and then clicks for real.  When
        the click point is still covered, the page's own click path is used on
        the same real element (`el.click()`), which is what a user's click
        would reach.
        """
        self.eval(
            "(() => {const el = document.querySelector(%s);"
            " if (el) el.scrollIntoView({block: 'center'}); return !!el})()"
            % jstr(selector)
        )
        try:
            poll(
                lambda: self.hittable(selector),
                15,
                f"the control {selector!r} to become clickable",
                interval=0.2,
            )
        except CaseFailure:
            pass  # fall through to the click (and its covered fallback)
        proc = self.run("click", selector, check=False)
        out = f"{proc.stdout}\n{proc.stderr}"
        if proc.returncode == 0 and "covered by" not in out:
            return
        self.run(
            "eval",
            "--stdin",
            stdin=(
                f"(() => {{const el = document.querySelector({jstr(selector)});"
                " if (!el) throw new Error('element not found'); el.click(); return true})()"
            ),
        )

    def press(self, key: str) -> None:
        self.run("press", key)

    def text(self, selector: str):
        return self.eval(
            f"(() => {{const el = document.querySelector({jstr(selector)});"
            " return el ? el.innerText : null})()"
        )

    def has(self, selector: str) -> bool:
        return bool(self.eval(f"!!document.querySelector({jstr(selector)})"))

    def wait_js(self, expression: str, timeout: float, desc: str):
        return poll_js(self, expression, timeout, desc)

    def wait_url(self, substring: str, timeout: float = DEFAULT_TIMEOUT) -> str:
        return poll(
            lambda: self.url() if substring in self.url() else None,
            timeout,
            f"URL containing {substring!r}",
        )

    def screenshot(self, path) -> None:
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        self.run("screenshot", str(path), check=False)

    def dialog_accept(self) -> None:
        self.run("dialog", "accept", check=False, timeout=30)

    def network_log(self, pattern: str = "") -> list:
        out = self.run("network", "requests", check=False).stdout
        lines = [line.strip() for line in out.splitlines() if line.strip()]
        if pattern:
            lines = [line for line in lines if pattern in line]
        return lines

    # -- cookies -----------------------------------------------------------

    def cookies(self) -> list:
        proc = self.run("cookies", "get", "--json", check=False)
        try:
            return json.loads(proc.stdout)["data"]["cookies"]
        except Exception:
            return []

    def cookie_header(self) -> str:
        return "; ".join(f"{c['name']}={c['value']}" for c in self.cookies())


# ------------------------------------------------------------ WebSocket probe


def _read_frame(sock: socket.socket, buf: bytes):
    while len(buf) < 2:
        chunk = sock.recv(4096)
        if not chunk:
            return None, buf
        buf += chunk
    b0, b1 = buf[0], buf[1]
    length = b1 & 0x7F
    index = 2
    if length == 126:
        while len(buf) < 4:
            buf += sock.recv(4096)
        length = struct.unpack(">H", buf[2:4])[0]
        index = 4
    elif length == 127:
        while len(buf) < 10:
            buf += sock.recv(4096)
        length = struct.unpack(">Q", buf[2:10])[0]
        index = 10
    while len(buf) < index + length:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
    payload = buf[index : index + length]
    buf = buf[index + length :]
    return {"opcode": b0 & 0x0F, "payload": payload.decode("utf-8", "replace")}, buf


def ws_probe(
    base_url: str,
    cookie_header: str,
    path: str = "/cable",
    frames: int = 2,
    timeout: float = 5.0,
) -> dict:
    """Open a raw WebSocket handshake against `/cable` and read `frames`.

    Returns {"status": <status line>, "frames": [...]}.  Server-to-client
    frames are unmasked and no extensions are offered, so the frames decode
    with the RFC 6455 header only.  Used by E2E-03 to replay a stale session
    cookie and to prove the valid-cookie control still receives a welcome.
    """
    tls = base_url.startswith("https://")
    scheme = "https" if tls else "http"
    host, port = base_url.removeprefix(scheme + "://").removesuffix("/").split(":")
    sock = socket.create_connection((host, int(port)), timeout=timeout)
    if tls:
        sock = _tls_test_context().wrap_socket(sock, server_hostname=host)
    try:
        key = base64.b64encode(os.urandom(16)).decode()
        request = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            f"Origin: {scheme}://{host}:{port}\r\n"
        )
        if cookie_header:
            request += f"Cookie: {cookie_header}\r\n"
        request += "\r\n"
        sock.sendall(request.encode())

        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = sock.recv(4096)
            if not chunk:
                break
            buf += chunk
        head, _, rest = buf.partition(b"\r\n\r\n")
        status = head.split(b"\r\n")[0].decode("utf-8", "replace")
        result = {"status": status, "frames": []}
        sock.settimeout(timeout)
        try:
            for _ in range(frames):
                frame, rest = _read_frame(sock, rest)
                if frame is None:
                    break
                result["frames"].append(frame)
        except socket.timeout:
            pass
        return result
    finally:
        sock.close()
