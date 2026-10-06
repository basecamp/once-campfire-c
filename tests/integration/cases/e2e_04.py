"""FINAL-06: uploaded avatar, involvement and paginated roster controls.

Uses real rendered forms and the application's Turbo lazy loader. SQLite is
used only to seed a roster large enough to exercise page two and to verify
persistent effects. No action is issued through a synthetic fetch/form.
"""
from __future__ import annotations

import struct
import sys
import time
import uuid
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import _harness as h
from run import CaseFailure

CASE_ID = "E2E-04"


def png() -> bytes:
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"\0\xff\0\0\xff\0\0" * 2)) + chunk(b"IEND", b""))


def run() -> None:
    h.require_prerequisites()
    with h.Scratch() as scratch:
        server = h.Server(scratch)
        browser = h.Browser("final06-" + uuid.uuid4().hex[:10])
        try:
            browser.reset()
            server.start()
            server.wait_ready()
            h.first_run_setup(browser, server)

            # The actual upload-preview/form controller submits the selected file.
            browser.open(server.base_url + "/users/me/profile")
            browser.wait_js("!!document.querySelector('#file')", 20, "profile avatar input")
            # Avatar URL versions use whole seconds, as in the reference.
            # Cross that boundary so Chromium requests the persisted image.
            time.sleep(1.1)
            file = scratch.path / "avatar.png"
            file.write_bytes(png())
            browser.run("upload", "#file", str(file))
            h.poll(lambda: server.query("SELECT count(*) FROM active_storage_attachments WHERE record_type='User' AND record_id=1 AND name='avatar'")[0][0] == 1,
                   30, "the uploaded profile avatar attachment")
            browser.wait_js("!!document.querySelector('.avatar__delete-btn')", 30, "the committed avatar profile to replace the upload form")
            browser.wait_js("(() => {const img=document.querySelector('[data-upload-preview-target=image]'); return img && img.complete && img.naturalWidth===2 && img.naturalHeight===2 && !img.src.startsWith('blob:')})()",
                            30, "the persisted avatar image to load")
            observed = browser.eval("(async () => {const img=document.querySelector('[data-upload-preview-target=image]'); const r=await fetch(img.src,{cache:'no-store'}); const b=new Uint8Array(await r.arrayBuffer()); return {status:r.status,type:r.headers.get('content-type'),signature:String.fromCharCode(...b.slice(0,4))+String.fromCharCode(...b.slice(8,12))}})()")
            if observed != {"status": 200, "type": "image/webp", "signature": "RIFFWEBP"}:
                raise CaseFailure(f"uploaded avatar variant: {observed!r}; blobs={server.query('SELECT id,content_type,key FROM active_storage_blobs')!r}")
            print("E2E-04: profile PNG form upload rendered a real WebP avatar")

            # Click the involvement helper's rendered next-state PUT form.
            browser.open(server.base_url + "/rooms/1/involvement")
            order = ["mentions", "everything", "nothing", "invisible"]
            for _ in range(2):
                current = server.query("SELECT involvement FROM memberships WHERE room_id=1 AND user_id=1")[0][0]
                expected = order[(order.index(current) + 1) % len(order)]
                browser.click_ui("#involvement_rooms_open_1 button[role=checkbox]")
                h.poll(lambda: server.query("SELECT involvement FROM memberships WHERE room_id=1 AND user_id=1")[0][0] == expected,
                       20, "the rendered involvement button's persistent change")
                browser.wait_js(f"document.querySelector('#involvement_rooms_open_1 button')?.classList.contains({h.jstr(expected)})",
                                20, "the refreshed involvement state")
            print("E2E-04: rendered involvement buttons changed and refreshed membership settings")

            # Seed page two. The account edit intentionally lists everyone;
            # its next-page loader adds the stream rows, so a second role
            # control proves the stream uses the production partial too.
            server.stop()
            with server._db() as db:
                for i in range(2, 502):
                    db.execute("INSERT INTO users(id,name,role,status,created_at,updated_at) VALUES(?,?,0,0,'2026-01-01 00:00:00','2026-01-01 00:00:00')",
                               (i, f"Bulk {i:04d}"))
                db.execute("INSERT INTO users(id,name,role,status,created_at,updated_at) VALUES(502,'Zed Last',0,0,'2026-01-01 00:00:00','2026-01-01 00:00:00')")
            server.start()
            server.wait_ready()
            browser.open(server.base_url + "/account/edit")
            browser.eval("document.querySelector('#next_page_container')?.scrollIntoView({block:'center'})")
            browser.wait_js("document.querySelectorAll('#role_user_502').length===2", 30, "the lazy-loaded roster row's real role control")
            browser.run("find", "last", 'label[for="role_user_502"]', "click")
            h.poll(lambda: server.query("SELECT role FROM users WHERE id=502")[0][0] == 1,
                   20, "the paginated role checkbox to promote its user")
            browser.wait_js("(() => {const controls=[...document.querySelectorAll('#role_user_502')]; return controls.length>0 && controls.every(el=>el.checked)})()", 20, "the refreshed administrator controls")
            browser.click_ui('form[action="/account/users/502"] button')
            browser.run("dialog", "accept")
            h.poll(lambda: server.query("SELECT status FROM users WHERE id=502")[0][0] == 1,
                   20, "the rendered remove-user control to deactivate its user")
            print("E2E-04: lazy-loaded account role and removal controls changed persistent user state")
        finally:
            browser.close()
            server.stop()
