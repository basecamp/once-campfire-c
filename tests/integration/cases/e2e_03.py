"""E2E-03: ban/revoke while another browser is connected.

Acceptance (docs/devel/implementation/07-verification.md):
"Ban/revoke while another browser is connected stops delivery and rejects
replayed subscription."

Two real browser contexts on one campfire instance with a scratch database:

  * browser A is the account administrator (real first-run form), browser B is
    a second user signed in through the real sign-in form (the second user is
    seeded from the database because `/join/:join_code` - packet A-users - is
    a dev-501 route in this build; see docs/devel/evidence/V01.md);
  * B is connected and subscribed (`turbo-cable-stream-source` for
    RoomMessagesChannel has the `connected` attribute) and receives a message
    posted in A's browser without any reload (positive control);
  * A issues the ban through the exact route the reference admin UI's ban
    button posts to, `POST /users/:id/ban` (the button is rendered by
    users#show, which is a dev-501 route in this build, so the request is
    issued from A's own page context with a same-origin fetch);
  * B observes the disconnect behavior: the composer's fieldset becomes
    disabled (the app's `refresh-room:offline` -> `composer#offline` path),
    and no message A posts after the ban ever reaches B's DOM;
  * B's stale session cookie no longer authenticates: navigating to the room
    redirects to sign-in, and a raw WebSocket replay of the stale cookie is
    refused with the reference unauthorized disconnect frame.

The reference Ban model refuses loopback/private/link-local addresses
(`ip_address_is_public`), so a strictly local ban cannot proceed; the case
rewrites the target's recorded session address to a public documentation
address (TEST-NET-3) before the ban, exactly the fixture the independent
review used for its banned-user seed.  This is recorded in V01.md.
"""

from __future__ import annotations

import os
import sys
import time
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import _harness as h  # noqa: E402  (path set above)
from run import CaseFailure, PrerequisiteMissing  # noqa: E402

CASE_ID = "E2E-03"

ROOM = f"/rooms/{h.FIRST_ROOM_ID}"
COMPOSER_FIELDS = "#composer fieldset[data-composer-target='fields']"
CABLE_SOURCE = "turbo-cable-stream-source[channel='RoomMessagesChannel'][connected]"


def _evidence_dir() -> Path | None:
    value = os.environ.get("CF_V01_EVIDENCE_DIR")
    return Path(value) if value else None


def _say(text: str) -> None:
    print(f"[{CASE_ID}] {text}")


def _contains(browser: h.Browser, needle: str) -> bool:
    return needle in (browser.text("#message-area") or "")


def run() -> None:
    h.require_prerequisites()
    nonce = uuid.uuid4().hex[:10]
    before = f"v01-e2e03-{nonce}-before"
    after = f"v01-e2e03-{nonce}-after"

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

            # --- administrator first-run (real form) -----------------------
            h.first_run_setup(a, server)
            _say(f"A landed on {a.url()} as the administrator")
            evidence = _evidence_dir()
            if evidence:
                a.screenshot(evidence / "e2e03-01-admin-room.png")

            # --- seed the second user (join route is dev-501) --------------
            server.stop()
            bob_id = server.seed_second_user(h.SECOND_NAME, h.SECOND_EMAIL)
            server.start()
            server.wait_ready()
            _say(f"seeded second user id={bob_id} and restarted the server")

            # --- both browsers in the room ---------------------------------
            a.open(server.base_url + ROOM)
            h.sign_in(b, server, h.SECOND_EMAIL, h.ADMIN_PASSWORD)
            _say(f"A at {a.url()}, B at {b.url()} (both room {h.FIRST_ROOM_ID})")

            # B's realtime subscription: the turbo-cable element is connected
            # only after the cable subscription is confirmed.
            b.wait_js(
                f"!!document.querySelector({h.jstr(CABLE_SOURCE)})",
                30,
                "B's RoomMessagesChannel subscription to become connected",
            )
            _say("B's turbo-cable-stream-source for RoomMessagesChannel is connected")

            # --- WebSocket probe controls (valid vs unauthorized) ----------
            valid = h.ws_probe(server.base_url, a.cookie_header())
            if not valid["frames"] or '"type":"welcome"' not in valid["frames"][0]["payload"]:
                raise CaseFailure(
                    "valid-cookie /cable handshake did not answer a welcome frame: "
                    f"{valid!r}"
                )
            bogus = h.ws_probe(server.base_url, "session_token=not-a-session")
            if not bogus["frames"] or '"unauthorized"' not in bogus["frames"][0]["payload"]:
                raise CaseFailure(
                    "bogus-cookie /cable handshake was not refused with the "
                    f"unauthorized disconnect: {bogus!r}"
                )
            _say(
                "WS probe control: valid cookie -> welcome; bogus cookie -> "
                '{"type":"disconnect","reason":"unauthorized","reconnect":false}'
            )

            # --- positive control: B receives a live message ---------------
            h.post_message(a, before)
            h.poll(
                lambda: _contains(b, before),
                20,
                f"B to receive {before!r} live before the ban",
            )
            _say(f"B received {before!r} live (delivery works before the ban)")

            # --- the ban ----------------------------------------------------
            # users#show (which renders the ban button) is dev-501; the button
            # posts POST /users/:id/ban, issued here from A's page context.
            updated = server.set_session_ip(bob_id, h.PUBLIC_TEST_IP)
            if updated < 1:
                raise CaseFailure(
                    "no session row for the second user to point at a public "
                    "address; the ban would be refused by the reference's "
                    "ip_address_is_public validation"
                )
            _say(
                f"target's {updated} recorded session address(es) set to "
                f"{h.PUBLIC_TEST_IP} (public fixture; loopback is refused by the "
                "reference Ban validation)"
            )
            ban = a.eval(
                "fetch('/users/%d/ban', {method: 'POST', redirect: 'manual'})"
                ".then(r => ({type: r.type, status: r.status}))" % bob_id
            )
            if not isinstance(ban, dict) or ban.get("type") != "opaqueredirect":
                raise CaseFailure(
                    "ban POST did not answer the reference redirect "
                    f"(302) from A's page context: {ban!r}; server log tail:\n"
                    + server.log_tail()
                )
            _say("A issued POST /users/%d/ban -> 302 (opaqueredirect)" % bob_id)

            # --- B observes the disconnect behavior ------------------------
            h.poll(
                lambda: b.eval(
                    f"(() => {{const f = document.querySelector({h.jstr(COMPOSER_FIELDS)});"
                    " return f ? f.disabled : null})()"
                ),
                25,
                "B's composer fieldset to become disabled after the disconnect",
            )
            _say("B's composer fieldset is disabled (refresh-room offline)")

            # --- B stops receiving -----------------------------------------
            h.post_message(a, after)
            h.poll(
                lambda: _contains(a, after),
                20,
                "A's post-ban message to appear in A's own DOM",
            )
            deadline = time.monotonic() + 8
            seen = False
            while time.monotonic() < deadline:
                if _contains(b, after):
                    seen = True
                    break
                time.sleep(0.5)
            if seen:
                raise CaseFailure(
                    f"B received {after!r} after being banned (delivery was not "
                    "stopped)"
                )
            _say(
                f"B did not receive {after!r} within 8s while A saw it "
                "(post-ban delivery stopped)"
            )

            if evidence:
                b.screenshot(evidence / "e2e03-02-banned-offline.png")

            # --- the stale cookie no longer authenticates ------------------
            stale_cookie = b.cookie_header()
            if "session_token" not in stale_cookie:
                raise CaseFailure(
                    f"could not read B's session cookie for the replay probe: "
                    f"{stale_cookie!r}"
                )
            b.open(server.base_url + ROOM)
            url = b.url()
            title = b.title()
            if not url.rstrip("/").endswith("/session/new") or title != h.SIGN_IN_TITLE:
                raise CaseFailure(
                    "banned browser's stale cookie still authenticated: "
                    f"navigation reached {url!r} ({title!r}), expected the sign-in page"
                )
            _say(f"B's stale cookie navigation -> {url} ({title!r})")

            # --- replayed subscription is refused --------------------------
            replay = h.ws_probe(server.base_url, stale_cookie)
            payload = replay["frames"][0]["payload"] if replay["frames"] else ""
            if '"type":"disconnect"' not in payload or '"unauthorized"' not in payload:
                raise CaseFailure(
                    "replayed stale cookie was not refused: "
                    f"{replay!r}"
                )
            if '"welcome"' in payload or "confirm_subscription" in payload:
                raise CaseFailure(
                    f"replayed stale cookie was granted a session: {replay!r}"
                )
            _say(
                "WS replay with the stale session cookie -> "
                '{"type":"disconnect","reason":"unauthorized","reconnect":false} '
                "then close; no welcome/confirm_subscription"
            )
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
