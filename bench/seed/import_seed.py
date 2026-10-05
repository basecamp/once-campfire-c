#!/usr/bin/env python3
"""Import the published benchmark workload seed into a fresh C-schema database.

The pinned harness (bench/pinned/once-campfire-elixir/bench/run) reads one
seed directory:

    <seed>/db/production.sqlite3     the populated database
    <seed>/storage/                  the Active Storage root
    <seed>/labels.json               fixture addressing used by the workloads

The upstream seed is built by Ruby recipes (vendored in tests/seed/seeds/)
against the Rails reference.  Those recipes need the Rails runtime and the
prebuilt `parity/.seed/default` is not committed upstream, so this importer
synthesizes the benchmark-relevant equivalent directly into the C schema
(tests/fixtures/contracts/schema.sql), with no Ruby and no new dependencies.

What the benchmark actually needs, from the workload labels in
bench/run and bench/lib/benchlib.py:

  * users.david + emails.david + passwords.all: the harness signs in through
    the real form (the login workload).  The password digest is the pinned
    `$2a$12$...` digest for "secret123456" used by tests/auth.
  * rooms.watercooler: the busy room.  It must page (>40 messages) and have a
    message in the middle for messages_page?before= (messages.busy_060).
  * rooms.hq: the write room; POSTs and uploads go there.
  * searchable content for /searches?q=coffee (the search workload).
  * avatar_tokens.jason: a valid signed avatar token so /users/<token>/avatar
    is addressable (signed with the pinned harness SECRET_KEY_BASE and the
    same Active Record signed-id format the C port verifies).

    Deliberate divergence (integrator ruling, Phase 3 roadmap contract
    changes): jason has no avatar attachment.  The seed omits the
    active_storage_blobs / active_storage_attachments rows and the storage
    file the reference seed carries, so the fallback avatar path is
    exercised; the avatar endpoint therefore serves a generated initials SVG
    rather than the reference's processed webp variant.  The label and the
    token signature format are exactly what the preflight expects.  This
    divergence is disclosed in every result directory (DIVERGENCE.md, written
    by bench/run-c, alongside the seed hash in env.txt) and in
    bench/PINNED-MANIFEST.json.
  * sessions are not seeded: the harness's login workload creates them.
  * Rails migration metadata (schema_migrations/ar_internal_metadata) is
    written so the Rails image's `db:prepare` at boot accepts the seed
    unchanged; the C and Rust apps ignore those two tables.

Determinism: the seed clock is the pinned recipes' NOW (2026-03-02 16:00 UTC,
tests/seed/seeds/lib/seed.rb) and every row uses an explicit timestamp; no two
rows ordered by time share an instant.  The generated tree is content-stable
across runs (same bytes), so its sha256 can be recorded per measurement.

Usage:
    python3 bench/seed/import_seed.py --out bench/seed/.seed/default
    python3 bench/seed/import_seed.py --out DIR --json
"""

from __future__ import annotations

import argparse
import base64
import datetime as dt
import hashlib
import json
import re
import sqlite3
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SCHEMA = REPO_ROOT / "tests" / "fixtures" / "contracts" / "schema.sql"
DEFAULT_ENV = (REPO_ROOT / "bench" / "pinned" / "once-campfire-elixir" /
               "parity" / "reference.env")

# Disclosed seed divergence; bench/run-c writes it into every result directory.
# Keep in sync with the "seed.divergence" statement in PINNED-MANIFEST.json,
# which is the recorded authority for the ruling.
DIVERGENCE = (
    "The benchmark seed omits jason's avatar attachment so the fallback avatar "
    "path is exercised; /users/<token>/avatar therefore serves a generated "
    "initials SVG (image/svg+xml) rather than the reference's processed webp "
    "variant. This divergence is disclosed in every result file, and avatar "
    "rows must be read with the representation difference in mind."
)

# The pinned seed clock (tests/seed/seeds/lib/seed.rb NOW).  UTC, naive text,
# matching the stored datetime format of the C port (cf_db_time_to_text).
NOW = dt.datetime(2026, 3, 2, 16, 0, 0)

# The pinned seeded bcrypt digest for "secret123456" (tests/auth/test_password.c,
# also the seed password in tests/seed/seeds/default.rb).
SEED_PASSWORD = "secret123456"
SEED_DIGEST = "$2a$12$kDVPfUd.VKsV9HzG8RHRFu0fyaW7gije1Krza2.A0J5S8XEcOQT4S"  # noqa: E501

WATERCOOLER_LINES = [
    "Did anyone see the game last night?", "Coffee machine is fixed!",
    "I'm heading out for lunch.", "New plants in the kitchen.",
    "Who took my stapler?", "Friday demo is at 3pm.",
    "The wifi is flaky again.", "Congrats on the launch!",
    "Anyone up for a walk?", "Back in 5.",
]

# Rails migration versions at the pinned reference (once-campfire 90b3300,
# reference/db/migrate).  The seed carries Rails' schema_migrations and
# ar_internal_metadata rows so the Rails image's `db:prepare` at boot sees a
# fully migrated database and does not try to re-run migrations against the
# seeded schema; the C and Rust apps ignore both tables.  They carry no
# benchmark content.
RAILS_MIGRATIONS = [
    "20231215043540", "20231220143106", "20240110071740", "20240115124901",
    "20240130003150", "20240130213001", "20240131105830", "20240209110503",
    "20250825100957", "20250825100958", "20250825100959", "20251126092013",
    "20251126115722", "20251126130131", "20251212154340",
]


def time_text(moment: dt.datetime) -> str:
    return moment.strftime("%Y-%m-%d %H:%M:%S.%f")


def signed_id(secret_key_base: str, model_name: str, record_id: int,
              purpose: str) -> str:
    """Active Record signed id, byte-compatible with Rails and the C port.

    cf_auth_signed_id_generate / tokens.c: key = PBKDF2-HMAC-SHA256(
    secret_key_base, "active_record/signed_id", 1000, 64); envelope =
    {"_rails":{"data":<id>,"pur":"<underscored model>/<purpose>"}}; message =
    urlsafe-base64(envelope, no padding) + "--" + hex(HMAC-SHA256(key, encoded)).
    """
    if model_name != "User":  # underscored model name, as in tokens.c
        raise ValueError("only the User model is needed by this seed")
    key = hashlib.pbkdf2_hmac(
        "sha256", secret_key_base.encode(), b"active_record/signed_id", 1000, 64)
    envelope = json.dumps(
        {"_rails": {"data": record_id, "pur": "user/" + purpose}},
        separators=(",", ":"))
    encoded = base64.urlsafe_b64encode(envelope.encode()).rstrip(b"=").decode()
    import hmac
    digest = hmac.new(key, encoded.encode(), hashlib.sha256).hexdigest()
    return f"{encoded}--{digest}"


def plain_text(html: str) -> str:
    return re.sub(r"<[^>]*>", " ", html).strip()


def apply_schema(db: sqlite3.Connection, schema_path: Path) -> None:
    db.executescript(schema_path.read_text())


def build(out_dir: Path, schema_path: Path, secret_key_base: str) -> dict:
    out_dir = out_dir.resolve()
    db_dir = out_dir / "db"
    storage_dir = out_dir / "storage"
    if (db_dir / "production.sqlite3").exists():
        raise SystemExit(f"refusing to overwrite existing seed in {out_dir}")
    db_dir.mkdir(parents=True, exist_ok=True)
    storage_dir.mkdir(parents=True, exist_ok=True)

    db_path = db_dir / "production.sqlite3"
    db = sqlite3.connect(db_path)
    try:
        apply_schema(db, schema_path)

        # -- account ----------------------------------------------------------
        db.execute(
            "INSERT INTO accounts (id, name, join_code, singleton_guard, "
            "created_at, updated_at) VALUES (1, ?, ?, 0, ?, ?)",
            ("37signals", "seedjoin", time_text(NOW - dt.timedelta(days=60)),
             time_text(NOW - dt.timedelta(days=60))))

        # -- users ------------------------------------------------------------
        users = [
            {"id": 1, "name": "David", "email": "david@37signals.com"},
            {"id": 2, "name": "Jason", "email": "jason@37signals.com"},
            {"id": 3, "name": "Kevin", "email": "kevin@37signals.com"},
            {"id": 4, "name": "JZ", "email": "jz@37signals.com"},
            {"id": 5, "name": "Bender", "email": "bender@37signals.com"},
        ]
        for i, user in enumerate(users):
            created = NOW - dt.timedelta(days=59) + dt.timedelta(hours=i)
            db.execute(
                "INSERT INTO users (id, name, email_address, password_digest, "
                "role, status, created_at, updated_at) VALUES (?, ?, ?, ?, 0, 0, ?, ?)",
                (user["id"], user["name"], user["email"], SEED_DIGEST,
                 time_text(created), time_text(created)))

        # -- rooms and memberships -------------------------------------------
        # 1 watercooler (busy), 2 hq (writes), 3 designers (search content).
        rooms = [
            {"id": 1, "name": "Watercooler", "creator": 1},
            {"id": 2, "name": "HQ", "creator": 1},
            {"id": 3, "name": "Designers", "creator": 1},
        ]
        memberships = [
            # david: everything in the busy room, nothing in the write room,
            # mentions in designers (mirrors tests/seed/seeds/default.rb).
            (1, 1, 1, "everything"), (1, 1, 2, "nothing"), (1, 1, 3, "mentions"),
            (2, 1, 2, "mentions"), (2, 1, 3, "everything"), (2, 1, 4, "everything"),
            (3, 1, 2, "everything"), (3, 1, 3, "mentions"), (3, 1, 1, "mentions"),
            (4, 1, 2, "mentions"), (4, 1, 3, "mentions"), (4, 1, 1, "mentions"),
            (5, 1, 1, "mentions"), (5, 1, 2, "mentions"), (5, 1, 3, "mentions"),
        ]
        for i, room in enumerate(rooms):
            created = NOW - dt.timedelta(days=58) + dt.timedelta(hours=i)
            db.execute(
                "INSERT INTO rooms (id, name, type, creator_id, created_at, "
                "updated_at) VALUES (?, ?, 'Rooms::Open', ?, ?, ?)",
                (room["id"], room["name"], room["creator"], time_text(created),
                 time_text(created)))
        for mid, (room_id, _one, user_id, involvement) in enumerate(
                memberships, start=1):
            created = NOW - dt.timedelta(days=57) + dt.timedelta(minutes=mid)
            db.execute(
                "INSERT INTO memberships (id, room_id, user_id, involvement, "
                "connections, created_at, updated_at) VALUES (?, ?, ?, ?, 0, ?, ?)",
                (mid, room_id, user_id, involvement, time_text(created),
                 time_text(created)))

        # -- messages ---------------------------------------------------------
        messages: list[dict] = []
        mid = 0

        def add_message(room_id: int, creator_id: int, created: dt.datetime,
                        body: str, label: str | None = None) -> None:
            nonlocal mid
            mid += 1
            messages.append({
                "id": mid, "room_id": room_id, "creator": creator_id,
                "created": created, "body": body, "label": label,
                "client_message_id": f"seed-message-{mid:04d}",
            })

        # Watercooler fixture messages (the 10 the busy room starts from)
        fixture_bodies = [
            "Standup notes are in the doc.", "Welcome aboard, everyone!",
            "The build is green again.", "Coffee beans are on the counter.",
            "Reminder: retro at 10.", "New laptop stickers arrived.",
            "Who is on support this week?", "The office plants survived.",
            "Sprint review moved to Thursday.", "Let's ship it.",
        ]
        for i, body in enumerate(fixture_bodies):
            add_message(1, users[i % 5]["id"],
                        NOW - dt.timedelta(days=20) + dt.timedelta(hours=i),
                        f"<p>{body}</p>")

        # 120 busy messages, 7 minutes apart (tests/seed/seeds/default.rb).
        busy_start = dt.datetime(2026, 2, 28, 8, 0, 0)
        busy_ids: dict[int, int] = {}
        for i in range(1, 121):
            body = f"{i:03d}. {WATERCOOLER_LINES[i % len(WATERCOOLER_LINES)]}"
            add_message(1, users[i % 5]["id"],
                        busy_start + dt.timedelta(minutes=7 * i),
                        f"<p>{body}</p>", f"busy_{i:03d}")
            busy_ids[i] = mid
        add_message(1, 5, NOW - dt.timedelta(minutes=20),
                    "<p>Build 1043 passed.</p>", "bot_in_watercooler")

        # HQ: the write room.  A few messages so it renders before writes.
        for i, body in enumerate([
                "Welcome to HQ.", "Launch checklist is pinned.",
                "Status update: on track."]):
            add_message(2, 1, NOW - dt.timedelta(days=10) + dt.timedelta(hours=i),
                        f"<p>{body}</p>")

        # Designers: searchable content (the /searches?q=coffee workload) and
        # one page-state message of each kind the room page renders.
        designer_bodies = [
            ("Coffee first, then the launch plan.", "mentions_coffee"),
            ("Morning! Anyone around?", None),
            ("Here's the plan for the <strong>spring launch</strong>.", None),
            ("The helper we talked about: <code>def greet(name)</code>", None),
            ("Who is reviewing the copy?", None),
        ]
        for i, (body, label) in enumerate(designer_bodies):
            add_message(3, users[(i + 1) % 5]["id"],
                        NOW - dt.timedelta(days=5) + dt.timedelta(minutes=10 * i),
                        f"<p>{body}</p>", label)

        for message in messages:
            db.execute(
                "INSERT INTO messages (id, client_message_id, creator_id, "
                "room_id, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?)",
                (message["id"], message["client_message_id"], message["creator"],
                 message["room_id"], time_text(message["created"]),
                 time_text(message["created"])))
        for message in messages:
            db.execute(
                "INSERT INTO action_text_rich_texts (id, name, body, record_type, "
                "record_id, created_at, updated_at) VALUES (?, 'body', ?, 'Message', ?, ?, ?)",
                (message["id"], message["body"], message["id"],
                 time_text(message["created"]), time_text(message["created"])))
            db.execute(
                "INSERT INTO message_search_index (rowid, body) VALUES (?, ?)",
                (message["id"], plain_text(message["body"])))

        # Room updated_at is the last message's creation.
        db.execute(
            "UPDATE rooms SET updated_at = (SELECT MAX(created_at) FROM messages "
            "WHERE messages.room_id = rooms.id) WHERE EXISTS "
            "(SELECT 1 FROM messages WHERE messages.room_id = rooms.id)")

        # -- jason's avatar ----------------------------------------------------
        # Deliberate divergence: no active_storage_blobs/attachments row and no
        # storage file, so /users/<token>/avatar exercises the fallback
        # generated-initials SVG path (see the module docstring).  The
        # avatar_tokens.jason label below is still the signed id the preflight
        # fetches.

        # -- Rails migration metadata (cross-app seed compatibility) -----------
        # The Rails image runs `db:prepare` at boot; with these rows it sees a
        # fully migrated database and leaves the seeded schema alone.  The C
        # and Rust apps ignore both tables.
        db.execute("CREATE TABLE IF NOT EXISTS schema_migrations "
                   "(version varchar NOT NULL PRIMARY KEY)")
        db.execute("CREATE TABLE IF NOT EXISTS ar_internal_metadata "
                   "(key varchar NOT NULL PRIMARY KEY, value varchar, "
                   "created_at datetime(6) NOT NULL, updated_at datetime(6) NOT NULL)")
        for version in RAILS_MIGRATIONS:
            db.execute("INSERT INTO schema_migrations (version) VALUES (?)",
                       (version,))
        stamp = time_text(NOW - dt.timedelta(days=59))
        db.execute("INSERT INTO ar_internal_metadata (key, value, created_at, "
                   "updated_at) VALUES ('environment', 'production', ?, ?)",
                   (stamp, stamp))

        db.commit()
        integrity = db.execute("PRAGMA integrity_check").fetchone()[0]
        message_count = db.execute(
            "SELECT COUNT(*) FROM messages WHERE room_id = 1").fetchone()[0]
    finally:
        db.close()

    # -- labels ---------------------------------------------------------------
    labels = {
        "clock.now": NOW.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "accounts.signal": 1,
        "users.david": 1, "users.jason": 2, "users.kevin": 3, "users.jz": 4,
        "users.bender": 5,
        "rooms.watercooler": 1, "rooms.hq": 2, "rooms.designers": 3,
        "messages.busy_060": busy_ids[60],
        "messages.busy_001": busy_ids[1],
        "messages.busy_120": busy_ids[120],
        "emails.david": "david@37signals.com",
        "emails.jason": "jason@37signals.com",
        "emails.kevin": "kevin@37signals.com",
        "passwords.all": SEED_PASSWORD,
        "avatar_tokens.david": signed_id(secret_key_base, "User", 1, "avatar"),
        "avatar_tokens.jason": signed_id(secret_key_base, "User", 2, "avatar"),
    }
    (out_dir / "labels.json").write_text(
        json.dumps(labels, indent=2, sort_keys=True) + "\n")

    db_bytes = db_path.read_bytes()
    return {
        "seed_dir": str(out_dir),
        "db": str(db_path),
        "db_sha256": hashlib.sha256(db_bytes).hexdigest(),
        "db_bytes": len(db_bytes),
        "storage_files": 0,  # no avatar attachment (deliberate divergence)
        "avatar": "token-only; fallback initials SVG (no attachment)",
        "divergence": DIVERGENCE,
        "labels": labels,
        "watercooler_messages": message_count,
        "integrity_check": integrity,
        "users": len(users),
        "rooms": [room["name"] for room in rooms],
        "messages_total": len(messages),
    }


def load_secret_key_base(explicit: str | None) -> str:
    if explicit:
        return explicit
    for line in DEFAULT_ENV.read_text().splitlines():
        if line.startswith("SECRET_KEY_BASE="):
            return line.split("=", 1)[1].strip()
    raise SystemExit(f"no SECRET_KEY_BASE in {DEFAULT_ENV}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True,
                        help="seed directory to create (must not exist)")
    parser.add_argument("--schema", default=str(DEFAULT_SCHEMA))
    parser.add_argument("--secret-key-base", default=None,
                        help="default: the pinned bench/reference.env value")
    parser.add_argument("--json", action="store_true",
                        help="print the summary as one JSON object")
    args = parser.parse_args()

    summary = build(
        Path(args.out), Path(args.schema),
        load_secret_key_base(args.secret_key_base))
    if args.json:
        print(json.dumps(summary, indent=2, sort_keys=True))
    else:
        print(f"seed: {summary['seed_dir']}")
        print(f"  db sha256: {summary['db_sha256']} ({summary['db_bytes']} bytes)")
        print(f"  watercooler messages: {summary['watercooler_messages']} "
              f"(total {summary['messages_total']}), "
              f"integrity {summary['integrity_check']}")
        print(f"  avatar: {summary['avatar']}")
        print("  labels: " + ", ".join(sorted(summary["labels"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
