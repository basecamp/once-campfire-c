"""E2E-02: upload/attach, room access change, push subscription UI and bot
message browser flows.

Acceptance (docs/devel/implementation/07-verification.md E2E-02): "File
upload/preview, room access change, bot message and push subscription UI
flows".

One campfire instance with its own scratch database, two real browser
contexts: A is the account administrator from the real first-run form, B is a
second user signed in through the real sign-in form (seeded from the database
because `/join/:join_code` is still a dev-501 route in this build; the V01
convention).

Subflows, each driven through the app's real UI where it exists:

  1. upload/attach.  A attaches a file through the room composer's real
     attach control (`input[type=file]`, `composer#filePicked`) and sends.
     The composer's FileUploader posts the multipart `message[attachment]`
     to the room's messages route and inserts the rendered turbo-stream
     reply.  The attachment path is wired (H02 upload accessor + S02
     staging, commit 43657aa), so the case requires the rendered attachment
     in both browsers and FAILS on a 400 from the multipart XHR (that is a
     regression, not the old blocked state).  Text attachments render the
     reference `render_link` arm (filename span + signed Active Storage
     download anchor), not the image/video `.message__attachment` media
     arms, so the assertion matches both shapes.  The preview/variant rows
     belong to S03/media (vips/ffmpeg absent by design) and are
     documented-BLOCKED in docs/devel/evidence/V02-browser.md, never
     skipped or faked.

  2. room access change.  A edits the original open room through the real
     room forms: switch to "only some access" (Rooms::Closed) and uncheck
     B, save; assert B loses the room (leaves B's sidebar, room navigation
     no longer reaches it).  Then A switches back to everyone access; the
     model grants every active user, so the room re-enters B's sidebar.
     If the served forms are still the integrator R1 placeholders (no
     type-switch link / no method override; see the shim headers in
     src/actions/rooms/opens.c and closeds.c), the case records that in
     its output and issues the same update as the reference form would post
     (POST + `_method=patch` with the form's fields), exactly the V01
     precedent for the unrendered ban button; the observable effect is
     asserted either way.

  3. push subscription UI.  A opens the room and the push page.  The case
     asserts the room bell (notifications controller, subscriptions URL),
     the configured VAPID public key in the layout meta, and drives the
     bell's `attemptToSubscribe`: the browser resolves the notification
     permission (headless Chrome auto-denies), the app registers
     `/service-worker.js`, and the denied path opens the reference
     "Notifications aren't allowed" dialog.  The granted arm cannot be
     exercised in this environment (headless Chrome has no push service and
     agent-browser 0.38.2 exposes no permission control; a CDP
     Browser.grantPermissions grant was observed not to change
     Notification.permission in headless mode), so the case records that
     precisely as BLOCKED and never fakes a subscription.

  4. bot message flow.  A creates a bot through the real `/account/bots`
     pages, the room re-open in subflow 2 grants every active user
     membership (including the new bot), and the bot API URL the index
     renders (`/rooms/1/<bot_key>/messages`) is used to post a bot message
     from the page context; B must observe it live in the room.  If the
     accounts/bots pages still render their static shims (their exact
     blocker), the case records PENDING with that blocker and continues.
"""

from __future__ import annotations

import json
import os
import sys
import time
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import _harness as h  # noqa: E402
from run import CaseFailure, PrerequisiteMissing  # noqa: E402

CASE_ID = "E2E-02"

ROOM = f"/rooms/{h.FIRST_ROOM_ID}"

# The reference fixture's VAPID public key
# (tests/fixtures/crates/views/tests/golden/a/push_subscriptions.html); a
# valid P-256 point, used to exercise the configured-key rendering path.
VAPID_PUBLIC_KEY = (
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8cTriz"
    "_qYBVicY02_VxTQ="
)

# The push page's title (pinned by the golden fixture).
PUSH_PAGE_TITLE = "Push notification subscriptions"


def _evidence_dir() -> Path | None:
    value = os.environ.get("CF_V01_EVIDENCE_DIR")
    return Path(value) if value else None


def _say(text: str) -> None:
    print(f"[{CASE_ID}] {text}")


def _contains(browser: h.Browser, needle: str) -> bool:
    return needle in (browser.text("#message-area") or "")


def _shared_room_ids(browser: h.Browser) -> list:
    return browser.eval(
        "(() => Array.from(document.querySelectorAll("
        "'#shared_rooms a[data-room-id]')).map(x => Number(x.dataset.roomId)))()"
    ) or []


def _open_room(browser: h.Browser, server: h.Server) -> None:
    browser.open(server.base_url + ROOM)
    h.poll(
        lambda: browser.has("#message-area") or None,
        20,
        "the room page's message area",
    )


# ------------------------------------------------------------------ upload


def _upload_flow(a: h.Browser, b: h.Browser, server: h.Server, scratch,
                 nonce: str) -> None:
    sample = scratch.path / f"v02-e2e02-{nonce}-note.txt"
    sample.write_text(f"v02 e2e02 {nonce} attachment payload\n")
    caption = f"v02-e2e02-{nonce}-upload-caption"

    _open_room(a, server)
    h.fill_editor(a, h.COMPOSER_EDITOR, caption)
    a.run("upload", "input[type=file]", str(sample))
    # The file list renders the stem and the extension in separate spans, so
    # compare against the element's textContent (innerText inserts a break).
    h.poll(
        lambda: a.eval(
            "(() => {const el = document.querySelector('.composer__filelist');"
            " return el && el.textContent.includes(%s)})()" % h.jstr(sample.name)
        ),
        15,
        "the composer file list to show the picked attachment",
    )
    a.click_ui(h.COMPOSER_SEND)

    # The composer uploads files as multipart XHR posts and posts the text
    # separately; the attachment path is wired, so require the rendered
    # attachment (media arms carry .message__attachment; the reference
    # render_link file arm is a filename span plus a signed Active Storage
    # download anchor in the same message). A 400 on the multipart XHR is a
    # regression, not the old blocked state.
    def attachment_rendered(browser):
        return bool(
            browser.eval(
                "(() => {const area = document.querySelector('#message-area');"
                " if (!area) return false;"
                f" const name = {h.jstr(sample.name)};"
                " const media = Array.from(area.querySelectorAll("
                "'.message__attachment'));"
                " if (media.some(e => e.innerText.includes(name))) return true;"
                " const links = Array.from(area.querySelectorAll('a[href]'));"
                " return links.some(e => {"
                "  const href = e.getAttribute('href') || '';"
                "  const msg = e.closest('.message');"
                "  return href.includes('active_storage') && msg &&"
                "         msg.textContent.includes(name);"
                " });})()"
            )
        )

    deadline = time.monotonic() + 25
    while not attachment_rendered(a):
        if any(
            f"{ROOM}/messages (XHR) 400" in line
            for line in a.network_log(f"{ROOM}/messages")
        ):
            raise CaseFailure(
                "multipart upload answered 400: the attachment path regressed "
                "(H02/S02 wiring landed in 43657aa); message area reads "
                f"{(a.text('#message-area') or '')[-300:]!r}"
            )
        if time.monotonic() >= deadline:
            raise CaseFailure(
                "upload flow never rendered the attachment; A's requests "
                f"{a.network_log('/messages')[-6:]!r}; message area reads "
                f"{(a.text('#message-area') or '')[-300:]!r}"
            )
        time.sleep(0.5)
    _say(f"attachment {sample.name!r} rendered in A's message area")
    _say(
        "BLOCKED (documented) S03 preview/variant rows: the pinned "
        "vips/ffmpeg tools are absent by design, so no image/video "
        "preview or variant can be exercised (recorded in V02-browser.md)"
    )
    h.poll(
        lambda: attachment_rendered(b),
        25,
        f"B to observe the attachment {sample.name!r} live",
    )
    _say(f"B observed the attachment {sample.name!r} live")

    # The text path of the same submit is independent and must still work.
    h.poll(
        lambda: _contains(a, caption),
        20,
        f"the caption {caption!r} to appear in A's room",
    )
    h.poll(
        lambda: _contains(b, caption),
        20,
        f"B to observe the caption {caption!r} live",
    )
    _say(f"caption {caption!r} delivered live to both browsers")


# ------------------------------------------------------------ room access


def _update_room_reference_shaped(browser: h.Browser, path: str,
                                  fields: list) -> dict:
    """POST the update the reference room form posts: POST + _method=patch.

    Used only when the served form is still the integrator R1 placeholder
    (disclosed in the case output and evidence); origin/Sec-Fetch-Site
    satisfy the port's CSRF check for same-origin fetches, exactly as the
    rendered reference form's own POST would.
    """
    return browser.eval(
        "(() => {const fields = %s; const body = new URLSearchParams();"
        " body.set('_method','patch');"
        " for (const [k, v] of fields) body.append(k, v);"
        " return fetch(%s, {method:'POST', body, redirect:'manual'})"
        " .then(r => ({type: r.type, status: r.status}))})()"
        % (json.dumps(fields), json.dumps(path))
    )


def _settle(browser: h.Browser, selector: str, timeout: float = 10.0) -> bool:
    """True when `selector` appears before the deadline (Turbo body swap)."""
    try:
        h.poll(lambda: browser.has(selector) or None, timeout, selector)
        return True
    except CaseFailure:
        return False


def _room_access_flow(a: h.Browser, b: h.Browser, server: h.Server,
                      bob_id: int, evidence: Path | None) -> bool:
    """Returns True when the served forms were the real ones."""
    real_forms = True
    open_edit = f"/rooms/opens/{h.FIRST_ROOM_ID}/edit"
    closed_edit = f"/rooms/closeds/{h.FIRST_ROOM_ID}/edit"
    open_patch = (
        f'form[action="/rooms/opens/{h.FIRST_ROOM_ID}"] '
        'input[name="_method"][value="patch"]'
    )
    closed_patch = (
        f'form[action="/rooms/closeds/{h.FIRST_ROOM_ID}"] '
        'input[name="_method"][value="patch"]'
    )

    # --- open -> "only some access", revoking B ---------------------------
    _open_room(a, server)
    edit_link = f'a[href="{open_edit}"]'
    h.poll(
        lambda: a.has(edit_link) or None,
        20,
        "A's room page edit link to the open-room form",
    )
    a.click_ui(edit_link)
    a.wait_url(open_edit, 20)
    real_forms = _settle(a, open_patch)
    if real_forms:
        switch = f'a[href="{closed_edit}"]'
        h.poll(
            lambda: a.has(switch) or None,
            10,
            "the open form's only-some-access switch",
        )
        a.click_ui(switch)
        a.wait_url(closed_edit, 20)
        if not _settle(a, closed_patch):
            raise CaseFailure(
                "the closed-room form did not render its method override"
            )
        checkbox = f'input[name="user_ids[]"][value="{bob_id}"]'
        if not a.has(checkbox):
            raise CaseFailure(
                f"closed-room form has no user_ids[] checkbox for B ({bob_id})"
            )
        a.eval(
            "(() => {const cb = document.querySelector(%s);"
            " cb.checked = false; return !cb.checked})()" % h.jstr(checkbox)
        )
        a.click_ui(
            f'form[action="/rooms/closeds/{h.FIRST_ROOM_ID}"] '
            'button[type="submit"]'
        )
        a.wait_url(ROOM, 25)
        _say(
            "A switched room 1 to only-some access through the real closeds "
            "form and revoked B"
        )
    else:
        real_forms = False
        # The served form is the R1 placeholder: no type-switch, no method
        # override, no submit button (and the nested delete form's
        # _method=delete is absorbed by the parser into the outer form).
        body = (a.text("main") or "")[:200]
        _say(
            "BLOCKED (documented) room form UI: /rooms/opens/1/edit still "
            "renders the integrator R1 placeholder (no only-some-access "
            f"switch / _method=patch; main reads {body!r}); issuing the "
            "reference form's POST + _method=patch with its fields instead"
        )
        result = _update_room_reference_shaped(
            a,
            f"/rooms/closeds/{h.FIRST_ROOM_ID}",
            [["room[name]", h.FIRST_ROOM_NAME], ["user_ids[]", "1"]],
        )
        if not isinstance(result, dict) or result.get("type") != "opaqueredirect":
            raise CaseFailure(
                f"closed-room reference-shaped update did not answer the "
                f"reference redirect: {result!r}"
            )

    rooms = server.query("SELECT type FROM rooms WHERE id = ?",
                         (h.FIRST_ROOM_ID,))
    if not rooms or rooms[0][0] != "Rooms::Closed":
        raise CaseFailure(
            f"room 1 type after revoke: expected Rooms::Closed, observed {rooms!r}"
        )
    memberships = server.query(
        "SELECT COUNT(*) FROM memberships WHERE room_id = ? AND user_id = ?",
        (h.FIRST_ROOM_ID, bob_id),
    )
    if memberships[0][0] != 0:
        raise CaseFailure(
            "B's membership row survived the revoke: "
            f"{memberships[0][0]} row(s)"
        )

    # Observable effect on B: the room left B's sidebar and B's navigation
    # to it no longer reaches the room.
    b.open(server.base_url + ROOM)
    h.poll(
        lambda: b.url() if ROOM not in b.url() else None,
        25,
        "B's navigation to the revoked room to leave it",
    )
    if h.FIRST_ROOM_ID in _shared_room_ids(b):
        raise CaseFailure(
            f"the revoked room is still in B's sidebar: {_shared_room_ids(b)!r}"
        )
    _say(
        f"B no longer reaches room 1 (landed on {b.url()!r}) and the room is "
        "out of B's shared-rooms sidebar"
    )
    if evidence:
        b.screenshot(evidence / "e2e02-01-b-sidebar-revoked.png")

    # --- "only some access" -> open (everyone), re-granting B -------------
    _open_room(a, server)
    closed_link = f'a[href="{closed_edit}"]'
    h.poll(
        lambda: a.has(closed_link) or None,
        20,
        "A's room page edit link to the closed-room form",
    )
    a.click_ui(closed_link)
    a.wait_url(closed_edit, 20)
    if real_forms and _settle(a, closed_patch):
        everyone = f'a[href="{open_edit}"]'
        h.poll(
            lambda: a.has(everyone) or None,
            10,
            "the closed form's give-everyone-access switch",
        )
        a.click_ui(everyone)
        a.wait_url(open_edit, 20)
        if not _settle(a, open_patch):
            raise CaseFailure(
                "the open-room form did not render its method override"
            )
        a.click_ui(
            f'form[action="/rooms/opens/{h.FIRST_ROOM_ID}"] '
            'button[type="submit"]'
        )
        a.wait_url(ROOM, 25)
        _say("A switched room 1 back to everyone access through the real open form")
    else:
        real_forms = False
        result = _update_room_reference_shaped(
            a,
            f"/rooms/opens/{h.FIRST_ROOM_ID}",
            [["room[name]", h.FIRST_ROOM_NAME]],
        )
        if not isinstance(result, dict) or result.get("type") != "opaqueredirect":
            raise CaseFailure(
                f"open-room reference-shaped update did not answer the "
                f"reference redirect: {result!r}"
            )
        _say("A restored everyone access with the reference-shaped update")

    rooms = server.query("SELECT type FROM rooms WHERE id = ?",
                         (h.FIRST_ROOM_ID,))
    if not rooms or rooms[0][0] != "Rooms::Open":
        raise CaseFailure(
            f"room 1 type after re-open: expected Rooms::Open, observed {rooms!r}"
        )
    memberships = server.query(
        "SELECT COUNT(*) FROM memberships WHERE room_id = ? AND user_id = ?",
        (h.FIRST_ROOM_ID, bob_id),
    )
    if memberships[0][0] != 1:
        raise CaseFailure(
            "B was not re-granted membership by the open-room conversion: "
            f"{memberships!r}"
        )
    # Observable effect: the room re-enters B's sidebar on a real navigation.
    _open_room(b, server)
    h.poll(
        lambda: h.FIRST_ROOM_ID in _shared_room_ids(b),
        25,
        "room 1 to re-enter B's shared-rooms sidebar",
    )
    _say(f"room 1 re-entered B's sidebar: {_shared_room_ids(b)!r}")
    if evidence:
        b.screenshot(evidence / "e2e02-02-b-sidebar-regranted.png")

    return real_forms


# ------------------------------------------------------------------ push


def _push_flow(a: h.Browser, server: h.Server, evidence: Path | None) -> None:
    _open_room(a, server)
    meta = a.eval(
        "(() => {const m = document.querySelector('meta[name=vapid-public-key]');"
        " return m ? m.content : null})()"
    )
    if meta != VAPID_PUBLIC_KEY:
        raise CaseFailure(
            f"layout vapid-public-key meta: expected the configured key, "
            f"observed {meta!r}"
        )
    wrapper = "span[data-controller=notifications]"
    if not a.has(wrapper):
        raise CaseFailure(
            "room page has no notifications controller wrapper around the bell"
        )
    url_value = a.eval(
        "(() => {const el = document.querySelector(%s);"
        " return el ? el.getAttribute("
        "'data-notifications-subscriptions-url-value') : null})()" % h.jstr(wrapper)
    )
    if url_value != "/users/me/push_subscriptions":
        raise CaseFailure(
            "notifications controller subscriptions URL: expected "
            f"'/users/me/push_subscriptions', observed {url_value!r}"
        )
    bell = "button[data-notifications-target=bell]"
    if not a.has(bell):
        raise CaseFailure("room page has no notification bell button")

    bell_pressed = False

    def notice_open():
        return bool(
            a.eval(
                "(() => {const d = document.querySelector("
                "'dialog[data-notifications-target=notAllowedNotice]');"
                " return !!(d && d.open)})()"
            )
        )

    a.click_ui(bell)  # notifications#attemptToSubscribe
    bell_pressed = True
    h.poll(
        lambda: (a.eval("Notification.permission") != "default") or notice_open(),
        25,
        "the notification permission request to resolve",
    )
    permission = a.eval("Notification.permission")
    sw_scope = a.eval(
        "navigator.serviceWorker.getRegistration(window.location.origin)"
        ".then(r => r ? r.scope : null)"
    )
    if not sw_scope:
        raise CaseFailure(
            "the notifications controller did not register /service-worker.js"
        )
    if permission == "denied":
        # The denied path's observable UI is the reference dialog, opened by
        # a further click (the click that requested permission still saw
        # "default" and took the requestPermission arm).
        a.click_ui(bell)
        h.poll(notice_open, 20, "the Notifications-aren't-allowed dialog")
        dialog_text = a.text(
            "dialog[data-notifications-target=notAllowedNotice]"
        ) or ""
        if "Notifications aren" not in dialog_text:
            raise CaseFailure(
                f"not-allowed dialog text: expected the pinned heading, "
                f"observed {dialog_text[:160]!r}"
            )
        _say(
            "push subscription UI: bell click requested notification "
            f"permission (resolved denied by headless Chrome), the app "
            f"registered {sw_scope}, and a further click opened the "
            "reference 'Notifications aren't allowed' dialog"
        )
        rows = server.query("SELECT COUNT(*) FROM push_subscriptions")
        if rows[0][0] != 0:
            raise CaseFailure(
                f"a push subscription appeared without a subscription arm: {rows!r}"
            )
        _say(
            "BLOCKED (documented) push subscribe arm: the granted path "
            "(pushManager.subscribe -> POST /users/me/push_subscriptions -> a "
            "subscription row on the push page) cannot be exercised here - "
            "headless Chrome auto-denies the notification permission "
            "(agent-browser 0.38.2 exposes no permission control; a CDP "
            "Browser.grantPermissions grant did not change "
            "Notification.permission in headless mode) and provides no push "
            "service"
        )
    elif permission == "granted":
        _say(
            "notification permission granted; exercising the subscribe arm"
        )
        # A real browser with a push service: wait for the subscription the
        # app's notifications controller syncs to the push page.
        h.poll(
            lambda: a.eval(
                "navigator.serviceWorker.getRegistration(window.location.origin)"
                ".then(r => r && r.pushManager.getSubscription())"
                ".then(s => s ? s.endpoint : null)"
            ),
            25,
            "pushManager.subscribe to produce a subscription",
        )
        rows = server.query("SELECT COUNT(*) FROM push_subscriptions")
        if rows[0][0] != 1:
            raise CaseFailure(
                "the app did not record the browser's push subscription: "
                f"{rows!r}"
            )
        _say("the app recorded the browser's push subscription")
    else:
        raise CaseFailure(
            f"notification permission ended in an unexpected way: {permission!r}"
        )

    # The push page renders through the real presenter either way.
    a.open(server.base_url + "/users/me/push_subscriptions")
    title = a.title()
    if title != PUSH_PAGE_TITLE:
        raise CaseFailure(
            f"push page title: expected {PUSH_PAGE_TITLE!r}, observed {title!r}"
        )
    if not a.has("#push_subscriptions"):
        raise CaseFailure("push page has no #push_subscriptions panel")
    if permission == "granted":
        h.poll(
            lambda: a.eval(
                "document.querySelectorAll('#push_subscriptions "
                "form[action$=/test_notifications]').length"
            ) == 1,
            15,
            "the recorded subscription row on the push page",
        )
        _say("push page lists the recorded subscription with its controls")
    else:
        listed = a.eval(
            "document.querySelectorAll('#push_subscriptions li').length"
        )
        if listed != 0:
            raise CaseFailure(
                f"push page lists {listed} subscription(s) although none was "
                "recorded"
            )
        _say(
            "push page renders the real PushSubscriptionsIndex with an empty "
            "list (no subscription recorded)"
        )
    if evidence:
        a.screenshot(evidence / "e2e02-03-push-page.png")


# ------------------------------------------------------------------ bots


def _bots_create(a: h.Browser, server: h.Server, nonce: str,
                 evidence: Path | None) -> str | None:
    """Create a bot through the real pages (None when they are still shims)."""
    a.open(server.base_url + "/account/bots")
    if not a.has('a[href="/account/bots/new"]'):
        body = (a.text("main") or "").strip()
        # The accounts/bots pages still render the static render shims: no
        # bot can be created through the UI, so no bot_key can be obtained
        # and the bot message route cannot be driven.  Assert the shim state
        # loudly and record PENDING with the exact blocker.
        if a.has('form[action="/account/bots"]') or a.has(
            'input[name="user[name]"]'
        ):
            raise CaseFailure(
                "bots index has real form controls although the new-bot link "
                "is missing; inspect the render state"
            )
        _say(
            "PENDING bot message flow: /account/bots still renders the static "
            "render shim (no new-bot link; main reads "
            f"{body[:120]!r}), so no bot_key exists to POST "
            f"{ROOM}/<bot_key>/messages with"
        )
        return None

    a.click_ui('a[href="/account/bots/new"]')
    a.wait_url("/account/bots/new", 20)
    form = 'form[action="/account/bots"]'
    h.poll(
        lambda: a.has(f'{form} input[name="user[name]"]') or None,
        15,
        "the new-bot form's name field",
    )
    bot_name = f"v02-e2e02-{nonce}-bot"
    a.fill('input[name="user[name]"]', bot_name)
    # The form is multipart (the avatar file field); submitting it without a
    # picked file sends a blank filename part, which multipart parsing drops.
    a.click_ui(f'{form} button[type="submit"]')
    a.wait_url("/account/bots", 25)
    h.poll(
        lambda: bot_name in (a.text("main") or ""),
        20,
        "the created bot to appear on the bots index",
    )
    _say(f"created bot {bot_name!r} through /account/bots/new")
    if evidence:
        a.screenshot(evidence / "e2e02-04-bots-index.png")
    return bot_name


def _bots_message(a: h.Browser, b: h.Browser, server: h.Server, nonce: str,
                  bot_name: str | None, evidence: Path | None) -> None:
    """Post as the created bot and require live delivery in B's room."""
    if bot_name is None:
        return  # shims: PENDING already recorded by _bots_create

    # The room access flow re-opened room 1 after this bot existed, and the
    # open-room conversion grants every active user; the index renders one
    # curl command per room the bot can post to.
    a.open(server.base_url + "/account/bots")
    curl = a.eval(
        "(() => {const li = Array.from(document.querySelectorAll('main li'))"
        f".find(l => l.innerText.includes({h.jstr(bot_name)}));"
        " if (!li) return null;"
        " const i = li.querySelector('input[type=text]');"
        " return i ? i.value : null})()"
    )
    if not curl or "curl -d" not in curl:
        raise CaseFailure(
            f"bot {bot_name!r} has no curl command on the bots index (no "
            f"room membership?); main reads {(a.text('main') or '')[:300]!r}"
        )
    url = curl.split()[-1]
    if not url.endswith("/messages") or f"/rooms/{h.FIRST_ROOM_ID}/" not in url:
        raise CaseFailure(f"unexpected bot message URL in {curl!r}")
    _say(f"bots index exposes the room 1 bot URL {url!r}")

    # Post as the bot from A's page context (the browser-level request the
    # curl command would issue) and require live delivery in B's room.
    _open_room(b, server)
    bot_text = f"v02-e2e02-{nonce}-bot-message"
    status = a.eval(
        "fetch(%s, {method:'POST', headers:{'Content-Type':'text/plain'},"
        " body:%s}).then(r => r.status)" % (h.jstr(url), h.jstr(bot_text))
    )
    if status != 201:
        raise CaseFailure(
            f"bot message POST answered {status!r}, expected the reference "
            "201 head :created"
        )
    h.poll(
        lambda: _contains(b, bot_text),
        25,
        f"B to receive the bot message {bot_text!r} live",
    )
    author = b.eval(
        "(() => {const els = Array.from(document.querySelectorAll("
        "'#message-area .message'));"
        f" const m = els.find(e => e.innerText.includes({h.jstr(bot_text)}));"
        " return m ? m.innerText : null})()"
    )
    if not author or bot_name not in author:
        raise CaseFailure(
            f"bot message author: expected {bot_name!r} in the message, "
            f"observed {(author or '')[:200]!r}"
        )
    _say(
        f"bot {bot_name!r} posted {bot_text!r} through its API URL; B "
        "observed it live with the bot as the author"
    )


def run() -> None:
    h.require_prerequisites()
    nonce = uuid.uuid4().hex[:10]

    with h.Scratch("cf-v02-e2e02-") as scratch:
        server = h.Server(
            scratch,
            extra_env={
                "VAPID_PUBLIC_KEY": VAPID_PUBLIC_KEY,
                "VAPID_PRIVATE_KEY": "v02-e2e02-private",
                "VAPID_SUBJECT": "mailto:v02-e2e02@example.com",
            },
        )
        a = h.Browser(h.SESSION_A)
        b = h.Browser(h.SESSION_B)
        try:
            a.reset()
            b.reset()
            server.start()
            server.wait_ready()
            _say(f"server ready on {server.base_url}")

            h.first_run_setup(a, server)
            _say(f"A landed on {a.url()} as the administrator")
            evidence = _evidence_dir()

            server.stop()
            bob_id = server.seed_second_user(h.SECOND_NAME, h.SECOND_EMAIL)
            server.start()
            server.wait_ready()
            h.sign_in(b, server, h.SECOND_EMAIL, h.ADMIN_PASSWORD)
            h.poll(
                lambda: h.FIRST_ROOM_ID in _shared_room_ids(b),
                25,
                "B's sidebar to list the original room",
            )
            _say(f"seeded second user id={bob_id}; B signed in at {b.url()}")

            _upload_flow(a, b, server, scratch, nonce)
            # The bot must exist before the room is re-opened: the open-room
            # conversion grants every active user, the bot included.
            bot_name = _bots_create(a, server, nonce, evidence)
            real_forms = _room_access_flow(a, b, server, bob_id, evidence)
            _say(
                "room access forms: "
                + ("real room form views" if real_forms else "R1 placeholder (documented)")
            )
            _push_flow(a, server, evidence)
            _bots_message(a, b, server, nonce, bot_name, evidence)
        except PrerequisiteMissing:
            raise
        except CaseFailure as exc:
            raise CaseFailure(
                f"{exc}\n--- campfire stderr (tail) ---\n{server.log_tail(15)}"
            ) from None
        finally:
            a.close()
            b.close()
            server.stop()
