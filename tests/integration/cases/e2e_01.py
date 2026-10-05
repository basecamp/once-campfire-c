"""E2E-01: two real browser contexts, setup/sign-in, live message, edit/delete.

Acceptance (docs/devel/implementation/07-verification.md E2E-01), as scoped by
the integrator's M2 ruling (roadmap "Contract changes", 2026-10-05): M2's
browser acceptance covers the reachable M2 flow —

    setup -> sign-in -> room -> text post -> live delivery -> edit/delete live

Browser A passes the real first-run form (pinned reference texts from
tests/fixtures/crates/views/tests/golden/a/first_run.html) and posts a text
message through the room composer.  Browser B is a second user signed in
through the real sign-in form and must observe the message, then the edited
text, then the removal live, without any reload.

DEFERRED to M4/V02 with their packets (do not treat as an E2E-01 failure):
`07-verification.md`'s row also names search and logout; both paths are
dev-501 in this tree by design and are exercised by their packets later:

  * search - the room's Search control opens `GET /searches`
    (`searches#index`, packet A-searches): `501 Not Implemented: route 146
    cf_action_searches_index (/searches(.:format))`;
  * logout - the reference form lives on `GET /users/me/profile`
    (`users/profiles#show`, packet A-users-profiles): `501 Not Implemented:
    route 60 cf_action_users_profiles_show (/users/:user_id/profile(.:format))`.

Nothing is skipped here: the case simply covers the ruled M2 scope.

History: the earlier revision of this case (V01, 2026-10-05) also drove search
and logout and, before the method-override repair, failed on the UI edit/delete
steps because the reference forms submit `POST` plus a hidden `_method` field
and the port never applied `cf_effective_method` before routing (404, frame
"Content missing").  That defect is fixed in `src/context.c` (see
docs/devel/evidence/method-override-wiring.md); the edit/delete steps below
now pass and the search/logout steps were removed per the ruling.
"""

from __future__ import annotations

import os
import sys
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import _harness as h  # noqa: E402  (path set above)
from run import CaseFailure, PrerequisiteMissing  # noqa: E402

CASE_ID = "E2E-01"

ROOM = f"/rooms/{h.FIRST_ROOM_ID}"


def _evidence_dir() -> Path | None:
    value = os.environ.get("CF_V01_EVIDENCE_DIR")
    return Path(value) if value else None


def _say(text: str) -> None:
    print(f"[{CASE_ID}] {text}")


def _contains(browser: h.Browser, needle: str) -> bool:
    return needle in (browser.text("#message-area") or "")


def _message_id_for(browser: h.Browser, needle: str, timeout: float = 20.0) -> str:
    return h.poll(
        lambda: browser.eval(
            "(() => {const els = Array.from(document.querySelectorAll("
            "'#message-area .message[data-message-id]'));"
            f"const m = els.find(e => e.innerText.includes({h.jstr(needle)}));"
            "return m ? m.dataset.messageId : null})()"
        ),
        timeout,
        f"a message containing {needle!r} in the DOM",
    )


def _message_text(browser: h.Browser, message_id: str):
    return browser.eval(
        "(() => {const m = document.querySelector("
        f"'.message[data-message-id=\"{message_id}\"]');"
        "return m ? m.innerText : null})()"
    )


def _open_edit_frame(browser: h.Browser, message_id: str) -> None:
    """Click the message's options menu and its Edit control (real UI)."""
    scope = f'.message[data-message-id="{message_id}"]'
    browser.click_ui(f"{scope} summary.message__options-btn")
    browser.click_ui(f"{scope} a.message__edit-btn")
    h.poll(
        lambda: browser.has(
            f'{scope} turbo-frame[id^="edit_message_"] '
            'lexxy-editor[aria-label="Edit message"]'
        ),
        15,
        "the message edit frame to load its editor",
    )


def run() -> None:
    h.require_prerequisites()
    nonce = uuid.uuid4().hex[:10]
    first = f"v01-e2e01-{nonce}-first"
    live = f"v01-e2e01-{nonce}-live"
    edited = f"v01-e2e01-{nonce}-edited"

    with h.Scratch() as scratch:
        server = h.Server(scratch)
        a = h.Browser(h.SESSION_A)
        b = h.Browser(h.SESSION_B)
        try:
            a.reset()
            b.reset()
            server.start()
            server.wait_ready()
            _say(f"server ready on {server.base_url}")

            # --- browser A: real first-run setup ---------------------------
            h.first_run_setup(a, server)
            if a.url() != server.base_url + ROOM:
                raise CaseFailure(
                    f"first-run did not land in the original room: {a.url()!r}"
                )
            title = a.title()
            if title != h.FIRST_ROOM_NAME:
                raise CaseFailure(
                    f"room page title: expected {h.FIRST_ROOM_NAME!r}, observed {title!r}"
                )
            composer = a.eval(
                "(() => {const f = document.querySelector('#composer');"
                "return f ? {action: f.getAttribute('action'), method: f.method} : null})()"
            )
            if not composer or composer.get("action") != f"{ROOM}/messages":
                raise CaseFailure(
                    f"room composer form is not the reference form: {composer!r}"
                )
            _say(
                f"A: first-run -> {a.url()} ({title!r}); composer action {composer['action']}"
            )

            # A posts the first message through the composer.
            h.post_message(a, first)
            first_id = _message_id_for(a, first)
            _say(f"A posted {first!r} (message id {first_id})")

            evidence = _evidence_dir()
            if evidence:
                a.screenshot(evidence / "e2e01-01-admin-room.png")

            # --- second user (join route is dev-501) -----------------------
            server.stop()
            server.seed_second_user(h.SECOND_NAME, h.SECOND_EMAIL)
            server.start()
            server.wait_ready()
            a.open(server.base_url + ROOM)

            # --- browser B: real sign-in form ------------------------------
            h.sign_in(b, server, h.SECOND_EMAIL, h.ADMIN_PASSWORD)
            _say(f"B signed in and landed on {b.url()}")
            # Sentinel: a page reload would clear these; the live-delivery
            # assertion below refuses a reloaded page.
            b.eval("window.__v01Sentinel = 1; window.__v01T0 = performance.timeOrigin")
            t0 = b.eval("window.__v01T0")

            # --- live delivery (no manual reload) --------------------------
            h.post_message(a, live)
            h.poll(
                lambda: _contains(b, live),
                20,
                f"B to observe {live!r} live without a reload",
            )
            if b.eval("window.__v01Sentinel") != 1 or b.eval("window.__v01T0") != t0:
                raise CaseFailure(
                    "B's page was reloaded between opening the room and the live "
                    "message arriving"
                )
            live_id = _message_id_for(b, live)
            _say(
                f"B observed {live!r} live (message id {live_id}); page-load "
                "sentinel unchanged"
            )
            if evidence:
                b.screenshot(evidence / "e2e01-02-b-live-message.png")

            # --- edit through the UI (the frame posts _method=patch) --------
            a.open(server.base_url + ROOM)
            _open_edit_frame(a, live_id)
            h.fill_editor(
                a,
                f'.message[data-message-id="{live_id}"] '
                'lexxy-editor[aria-label="Edit message"]',
                edited,
                replace=True,
            )
            a.click_ui(
                f'.message[data-message-id="{live_id}"] '
                'turbo-frame[id^="edit_message_"] button[type="submit"].btn--reversed'
            )
            try:
                # The edited text replaces the original: the message must gain
                # the new body and lose the old one, live.
                h.poll(
                    lambda: _contains(b, edited) and not _contains(b, live),
                    15,
                    f"B to observe the edited text {edited!r} replace {live!r} live",
                )
            except CaseFailure:
                _say(
                    f"edit failed; A's requests {a.network_log('/messages/')[-3:]!r}; "
                    f"A's message reads {_message_text(a, live_id)!r}"
                )
                raise
            _say(f"B observed the edited text {edited!r} live")
            if evidence:
                b.screenshot(evidence / "e2e01-03-b-edited.png")

            # --- delete through the UI (the form posts _method=delete) ------
            a.open(server.base_url + ROOM)
            _open_edit_frame(a, live_id)
            a.click_ui(
                f'.message[data-message-id="{live_id}"] '
                'turbo-frame[id^="edit_message_"] button.btn--negative'
            )
            a.dialog_accept()  # turbo_confirm "Are you sure you want to delete..."
            try:
                h.poll(
                    lambda: not _contains(b, edited) and not _contains(b, live),
                    15,
                    "B to observe the message removed live",
                )
            except CaseFailure:
                _say(
                    f"delete failed; A's requests {a.network_log('/messages/')[-3:]!r}; "
                    f"A's message reads {_message_text(a, live_id)!r}"
                )
                raise
            _say("B observed the message removed live")
            if evidence:
                b.screenshot(evidence / "e2e01-04-b-removed.png")
        except PrerequisiteMissing:
            raise
        except CaseFailure as exc:
            # Every failure carries the server's captured stderr tail.
            raise CaseFailure(
                f"{exc}\n--- campfire stderr (tail) ---\n{server.log_tail(15)}"
            ) from None
        finally:
            a.close()
            b.close()
            server.stop()
