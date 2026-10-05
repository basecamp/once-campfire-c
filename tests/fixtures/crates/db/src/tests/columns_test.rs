//! The hot models read their columns by position (`crate::sql::columns!`), which must give what
//! reading them by name gives, whatever order a table has its columns in.

use std::collections::HashMap;
use std::hint::black_box;
use std::time::{Duration, Instant};

use rails_compat::clock::SystemClock;
use rusqlite::{Row, params};

use crate::fixtures;
use crate::models::{Account, Boost, RichTextRecord, Role, Session, Status, User};
use crate::{Connection, Involvement, Membership, Message, Room, RoomType, Timestamp, query_all, schema};

/// Where a database's tables got their column order.
#[derive(Debug, Clone, Copy)]
enum Layout {
    /// `schema.sql`'s alphabetical order, as `db:prepare` loads `schema.rb` into a new database.
    Schema,
    /// The order `20231215043540_create_initial_schema.rb` created them in, which databases Rails
    /// migrated keep.
    Migrated,
}

const LAYOUTS: [Layout; 2] = [Layout::Schema, Layout::Migrated];

#[rustfmt::skip]
const MIGRATED_COLUMNS: &[(&str, &[&str])] = &[
    ("messages", &["id", "room_id", "creator_id", "created_at", "updated_at", "client_message_id"]),
    ("rooms", &["id", "name", "created_at", "updated_at", "type", "creator_id"]),
    ("memberships", &["id", "room_id", "user_id", "created_at", "updated_at", "unread_at", "involvement", "connections", "connected_at"]),
    // `bio`, `bot_token` and `status` were added after the initial schema, and `active` removed.
    ("users", &["id", "name", "created_at", "updated_at", "role", "email_address", "password_digest", "bio", "bot_token", "status"]),
    ("sessions", &["id", "user_id", "token", "ip_address", "user_agent", "last_active_at", "created_at", "updated_at"]),
    ("accounts", &["id", "name", "join_code", "created_at", "updated_at", "custom_styles", "settings", "singleton_guard"]),
    ("boosts", &["id", "message_id", "booster_id", "content", "created_at", "updated_at"]),
    ("action_text_rich_texts", &["id", "name", "body", "record_type", "record_id", "created_at", "updated_at"]),
];

fn database(layout: Layout) -> Connection {
    let mut conn = Connection::open_in_memory().unwrap();
    schema::prepare(&mut conn, "test", &SystemClock).unwrap();
    if let Layout::Migrated = layout {
        for (table, order) in MIGRATED_COLUMNS {
            reorder_columns(&conn, table, order);
        }
    }
    conn
}

fn database_with_fixtures(layout: Layout) -> Connection {
    let mut conn = database(layout);
    let tx = conn.transaction().unwrap();
    fixtures::load(&tx, &fixtures::reference_dir(), &fixtures::Options { now: at(0), bcrypt_cost: 4 }).unwrap();
    tx.commit().unwrap();
    conn
}

/// Recreates the empty `table` with its columns in `order`, each keeping its type, NOT NULL and
/// default.
fn reorder_columns(conn: &Connection, table: &str, order: &[&str]) {
    let mut definitions: HashMap<String, String> = query_all(conn, &format!(r#"PRAGMA table_info("{table}")"#), [], |row| {
        let name: String = row.get("name")?;
        let mut definition = format!(r#""{name}" {}"#, row.get::<_, String>("type")?);
        if row.get("pk")? {
            definition.push_str(" PRIMARY KEY AUTOINCREMENT");
        }
        if row.get("notnull")? {
            definition.push_str(" NOT NULL");
        }
        if let Some(default) = row.get::<_, Option<String>>("dflt_value")? {
            definition.push_str(&format!(" DEFAULT {default}"));
        }
        Ok((name, definition))
    })
    .unwrap()
    .into_iter()
    .collect();
    assert_eq!(definitions.len(), order.len(), "{table}: {order:?} names every column once");
    let columns: Vec<String> = order.iter().map(|column| definitions.remove(*column).unwrap()).collect();
    conn.execute_batch(&format!(r#"DROP TABLE "{table}"; CREATE TABLE "{table}" ({})"#, columns.join(", "))).unwrap();
    let names = query_all(conn, &format!(r#"SELECT "name" FROM pragma_table_info('{table}')"#), [], |row| row.get::<_, String>(0)).unwrap();
    assert_eq!(names, order);
}

/// A distinct time for each `n`, with microseconds.
fn at(n: i64) -> Timestamp {
    Timestamp::from_microsecond(1_790_000_000_000_000 + n * 1_000_123)
}

fn insert_user(conn: &Connection, id: i64) {
    conn.execute(r#"INSERT INTO "users" ("id", "name", "created_at", "updated_at") VALUES (?, 'User', ?, ?)"#, params![id, at(0), at(0)])
        .unwrap();
}

fn insert_room(conn: &Connection, room: &Room) {
    conn.execute(
        r#"INSERT INTO "rooms" ("id", "name", "type", "creator_id", "created_at", "updated_at") VALUES (?, ?, ?, ?, ?, ?)"#,
        params![room.id, room.name, room.room_type, room.creator_id, room.created_at, room.updated_at],
    )
    .unwrap();
}

fn insert_message(conn: &Connection, message: &Message) {
    conn.execute(
        r#"INSERT INTO "messages" ("id", "room_id", "creator_id", "client_message_id", "created_at", "updated_at") VALUES (?, ?, ?, ?, ?, ?)"#,
        params![message.id, message.room_id, message.creator_id, message.client_message_id, message.created_at, message.updated_at],
    )
    .unwrap();
}

fn insert_membership(conn: &Connection, membership: &Membership) {
    conn.execute(
        r#"INSERT INTO "memberships" ("id", "room_id", "user_id", "involvement", "unread_at", "connected_at", "connections", "created_at", "updated_at") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"#,
        params![
            membership.id,
            membership.room_id,
            membership.user_id,
            membership.involvement,
            membership.unread_at,
            membership.connected_at,
            membership.connections,
            membership.created_at,
            membership.updated_at
        ],
    )
    .unwrap();
}

/// A room with a distinct value in every column.
fn distinct_room() -> Room {
    Room {
        id: 2001,
        name: Some("Room 2002".into()),
        room_type: RoomType::Closed,
        creator_id: 2003,
        created_at: at(2004),
        updated_at: at(2005),
    }
}

/// A room with every nullable column null.
fn unnamed_room() -> Room {
    Room { id: 2011, name: None, room_type: RoomType::Direct, creator_id: 2013, created_at: at(2014), updated_at: at(2015) }
}

fn user_ids(conn: &Connection) -> Vec<i64> {
    query_all(conn, r#"SELECT "id" FROM "users" ORDER BY "id""#, [], |row| row.get(0)).unwrap()
}

// Messages

/// How `Message::from_row` read a row before it read by position.
fn message_by_name(row: &Row<'_>) -> rusqlite::Result<Message> {
    Ok(Message {
        id: row.get("id")?,
        room_id: row.get("room_id")?,
        creator_id: row.get("creator_id")?,
        client_message_id: row.get("client_message_id")?,
        created_at: row.get("created_at")?,
        updated_at: row.get("updated_at")?,
    })
}

#[test]
fn a_message_reads_each_column_into_its_own_field() {
    for layout in LAYOUTS {
        let conn = database(layout);
        let room = distinct_room();
        insert_user(&conn, room.creator_id);
        insert_room(&conn, &room);
        let message = Message {
            id: 1001,
            room_id: room.id,
            creator_id: room.creator_id,
            client_message_id: "client 1004".into(),
            created_at: at(1005),
            updated_at: at(1006),
        };
        insert_message(&conn, &message);

        assert_eq!(Message::find(&conn, message.id).unwrap(), message, "{layout:?}");
        assert_eq!(Message::last_page(&conn, room.id).unwrap(), std::slice::from_ref(&message), "{layout:?}");
    }
}

#[test]
fn messages_read_by_position_as_by_name() {
    for layout in LAYOUTS {
        let conn = database_with_fixtures(layout);
        let messages = query_all(&conn, r#"SELECT * FROM "messages" ORDER BY "id""#, [], message_by_name).unwrap();
        assert!(messages.len() > 5);
        for message in &messages {
            assert_eq!(&Message::find(&conn, message.id).unwrap(), message, "{layout:?}");
            let mut in_room = Message::for_room(&conn, message.room_id).unwrap();
            in_room.sort_by_key(|m| m.id);
            assert_eq!(in_room, messages.iter().filter(|m| m.room_id == message.room_id).cloned().collect::<Vec<_>>(), "{layout:?}");
        }
    }
}

// Rooms

/// How `Room::from_row` read a row before it read by position.
fn room_by_name(row: &Row<'_>) -> rusqlite::Result<Room> {
    Ok(Room {
        id: row.get("id")?,
        name: row.get("name")?,
        room_type: row.get("type")?,
        creator_id: row.get("creator_id")?,
        created_at: row.get("created_at")?,
        updated_at: row.get("updated_at")?,
    })
}

#[test]
fn a_room_reads_each_column_into_its_own_field() {
    for layout in LAYOUTS {
        let conn = database(layout);
        let room = distinct_room();
        let unnamed = unnamed_room();
        insert_room(&conn, &room);
        insert_room(&conn, &unnamed);

        assert_eq!(Room::find(&conn, room.id).unwrap(), room, "{layout:?}");
        assert_eq!(Room::find(&conn, unnamed.id).unwrap(), unnamed, "{layout:?}");
        assert_eq!(Room::all(&conn).unwrap(), [room, unnamed], "{layout:?}");
    }
}

#[test]
fn rooms_read_by_position_as_by_name() {
    for layout in LAYOUTS {
        let conn = database_with_fixtures(layout);
        let rooms = query_all(&conn, r#"SELECT * FROM "rooms" ORDER BY "id""#, [], room_by_name).unwrap();
        assert!(rooms.len() > 3);
        for room in &rooms {
            assert_eq!(&Room::find(&conn, room.id).unwrap(), room, "{layout:?}");
        }
        for user_id in user_ids(&conn) {
            let mut for_user = Room::for_user(&conn, user_id).unwrap();
            for_user.sort_by_key(|room| room.id);
            let by_name = query_all(
                &conn,
                r#"SELECT "rooms".* FROM "rooms" INNER JOIN "memberships" ON "rooms"."id" = "memberships"."room_id" WHERE "memberships"."user_id" = ? ORDER BY "rooms"."id""#,
                [user_id],
                room_by_name,
            )
            .unwrap();
            assert_eq!(for_user, by_name, "{layout:?}");
        }
    }
}

// Memberships, alone and with their rooms

/// How `Membership::from_row` read a row before it read by position.
fn membership_by_name(row: &Row<'_>) -> rusqlite::Result<Membership> {
    Ok(Membership {
        id: row.get("id")?,
        room_id: row.get("room_id")?,
        user_id: row.get("user_id")?,
        involvement: row.get("involvement")?,
        unread_at: row.get("unread_at")?,
        connected_at: row.get("connected_at")?,
        connections: row.get("connections")?,
        created_at: row.get("created_at")?,
        updated_at: row.get("updated_at")?,
    })
}

/// How `with_ordered_room` selected a membership with its room before, the room's columns aliased
/// so that they could be read by name.
const WITH_ROOM_BY_NAME: &str = r#"SELECT "memberships".*, "rooms"."id" AS r_id, "rooms"."created_at" AS r_created_at, "rooms"."creator_id" AS r_creator_id, "rooms"."name" AS r_name, "rooms"."type" AS r_type, "rooms"."updated_at" AS r_updated_at FROM "memberships" INNER JOIN "rooms" ON "rooms"."id" = "memberships"."room_id" WHERE "memberships"."user_id" = ?"#;

fn membership_with_room_by_name(row: &Row<'_>) -> rusqlite::Result<(Membership, Room)> {
    let room = Room {
        id: row.get("r_id")?,
        name: row.get("r_name")?,
        room_type: row.get("r_type")?,
        creator_id: row.get("r_creator_id")?,
        created_at: row.get("r_created_at")?,
        updated_at: row.get("r_updated_at")?,
    };
    Ok((membership_by_name(row)?, room))
}

fn by_membership_id(mut pairs: Vec<(Membership, Room)>) -> Vec<(Membership, Room)> {
    pairs.sort_by_key(|(membership, _)| membership.id);
    pairs
}

#[test]
fn a_membership_and_its_room_read_each_column_into_their_own_fields() {
    for layout in LAYOUTS {
        let conn = database(layout);
        let (room, unnamed) = (distinct_room(), unnamed_room());
        let membership = Membership {
            id: 3001,
            room_id: room.id,
            user_id: 3003,
            involvement: Some(Involvement::Everything),
            unread_at: Some(at(3004)),
            connected_at: Some(at(3005)),
            connections: 3006,
            created_at: at(3007),
            updated_at: at(3008),
        };
        let unset = Membership {
            id: 3011,
            room_id: unnamed.id,
            user_id: membership.user_id,
            involvement: None,
            unread_at: None,
            connected_at: None,
            connections: 3016,
            created_at: at(3017),
            updated_at: at(3018),
        };
        for (membership, room) in [(&membership, &room), (&unset, &unnamed)] {
            insert_room(&conn, room);
            insert_membership(&conn, membership);
        }

        assert_eq!(Membership::find(&conn, membership.id).unwrap(), membership, "{layout:?}");
        assert_eq!(Membership::find(&conn, unset.id).unwrap(), unset, "{layout:?}");
        // Ordered by room name, nulls first.
        assert_eq!(
            Membership::with_ordered_room(&conn, membership.user_id).unwrap(),
            [(unset.clone(), unnamed.clone()), (membership.clone(), room.clone())],
            "{layout:?}"
        );
        // A null involvement isn't `!= 'invisible'`.
        assert_eq!(Membership::visible_with_ordered_room(&conn, membership.user_id).unwrap(), [(membership, room)], "{layout:?}");
    }
}

#[test]
fn memberships_read_by_position_as_by_name() {
    for layout in LAYOUTS {
        let conn = database_with_fixtures(layout);
        let memberships = query_all(&conn, r#"SELECT * FROM "memberships" ORDER BY "id""#, [], membership_by_name).unwrap();
        assert!(memberships.len() > 5);
        for membership in &memberships {
            assert_eq!(&Membership::find(&conn, membership.id).unwrap(), membership, "{layout:?}");
        }
        for user_id in user_ids(&conn) {
            let with_room = query_all(&conn, WITH_ROOM_BY_NAME, [user_id], membership_with_room_by_name).unwrap();
            let visible = format!(r#"{WITH_ROOM_BY_NAME} AND "memberships"."involvement" != 'invisible'"#);
            let visible = query_all(&conn, &visible, [user_id], membership_with_room_by_name).unwrap();
            assert_eq!(by_membership_id(Membership::with_ordered_room(&conn, user_id).unwrap()), by_membership_id(with_room), "{layout:?}");
            assert_eq!(
                by_membership_id(Membership::visible_with_ordered_room(&conn, user_id).unwrap()),
                by_membership_id(visible),
                "{layout:?}"
            );
        }
    }
}

// Users, sessions, accounts, boosts and rich texts

#[test]
fn users_sessions_accounts_boosts_and_rich_texts_read_each_column_into_its_own_field() {
    for layout in LAYOUTS {
        let conn = database(layout);
        let user = User {
            id: 4001,
            name: "User 4002".into(),
            email_address: Some("4003@example.com".into()),
            password_digest: Some("digest 4004".into()),
            role: Role::Bot,
            status: Status::Banned,
            bio: Some("bio 4005".into()),
            bot_token: Some("token 4006".into()),
            created_at: at(4007),
            updated_at: at(4008),
        };
        conn.execute(
            r#"INSERT INTO "users" ("id", "name", "email_address", "password_digest", "role", "status", "bio", "bot_token", "created_at", "updated_at") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"#,
            params![user.id, user.name, user.email_address, user.password_digest, user.role, user.status, user.bio, user.bot_token, user.created_at, user.updated_at],
        )
        .unwrap();
        let session = Session {
            id: 5001,
            user_id: user.id,
            token: "token 5003".into(),
            ip_address: Some("10.0.50.4".into()),
            user_agent: Some("agent 5005".into()),
            last_active_at: at(5006),
            created_at: at(5007),
            updated_at: at(5008),
        };
        conn.execute(
            r#"INSERT INTO "sessions" ("id", "user_id", "token", "ip_address", "user_agent", "last_active_at", "created_at", "updated_at") VALUES (?, ?, ?, ?, ?, ?, ?, ?)"#,
            params![session.id, session.user_id, session.token, session.ip_address, session.user_agent, session.last_active_at, session.created_at, session.updated_at],
        )
        .unwrap();
        let account = Account {
            id: 6001,
            name: "Account 6002".into(),
            join_code: "code-6003".into(),
            custom_styles: Some("/* 6004 */".into()),
            settings_json: Some(r#"{"n":6005}"#.into()),
            singleton_guard: 6006,
            created_at: at(6007),
            updated_at: at(6008),
        };
        conn.execute(
            r#"INSERT INTO "accounts" ("id", "name", "join_code", "custom_styles", "settings", "singleton_guard", "created_at", "updated_at") VALUES (?, ?, ?, ?, ?, ?, ?, ?)"#,
            params![account.id, account.name, account.join_code, account.custom_styles, account.settings_json, account.singleton_guard, account.created_at, account.updated_at],
        )
        .unwrap();
        let room = Room { creator_id: user.id, ..distinct_room() };
        insert_room(&conn, &room);
        let message = Message {
            id: 7002,
            room_id: room.id,
            creator_id: user.id,
            client_message_id: "7002".into(),
            created_at: at(0),
            updated_at: at(0),
        };
        insert_message(&conn, &message);
        let boost = Boost {
            id: 7001,
            message_id: message.id,
            booster_id: user.id,
            content: "👍 7004".into(),
            created_at: at(7005),
            updated_at: at(7006),
        };
        conn.execute(
            r#"INSERT INTO "boosts" ("id", "message_id", "booster_id", "content", "created_at", "updated_at") VALUES (?, ?, ?, ?, ?, ?)"#,
            params![boost.id, boost.message_id, boost.booster_id, boost.content, boost.created_at, boost.updated_at],
        )
        .unwrap();
        let rich_text = RichTextRecord {
            id: 8001,
            name: "body".into(),
            body: Some("<p>8003</p>".into()),
            record_type: "Message".into(),
            record_id: 8005,
            created_at: at(8006),
            updated_at: at(8007),
        };
        conn.execute(
            r#"INSERT INTO "action_text_rich_texts" ("id", "name", "body", "record_type", "record_id", "created_at", "updated_at") VALUES (?, ?, ?, ?, ?, ?, ?)"#,
            params![rich_text.id, rich_text.name, rich_text.body, rich_text.record_type, rich_text.record_id, rich_text.created_at, rich_text.updated_at],
        )
        .unwrap();

        assert_eq!(User::find(&conn, user.id).unwrap(), user, "{layout:?}");
        assert_eq!(Session::find_by_token(&conn, &session.token).unwrap(), Some(session), "{layout:?}");
        assert_eq!(Account::first(&conn).unwrap(), Some(account), "{layout:?}");
        assert_eq!(Boost::for_message_ordered(&conn, boost.message_id).unwrap(), [boost], "{layout:?}");
        assert_eq!(RichTextRecord::find_for(&conn, "Message", rich_text.record_id, "body").unwrap(), Some(rich_text), "{layout:?}");
    }
}

// Timing

/// A room page's worth of rows and more: 200 messages in room 1, and user 1 in 12 rooms.
fn timing_database() -> Connection {
    let conn = database(Layout::Schema);
    insert_user(&conn, 1);
    for id in 1..=12 {
        let (created_at, updated_at) = (at(id), at(id + 1));
        insert_room(
            &conn,
            &Room { id, name: Some(format!("Room {id}")), room_type: RoomType::Open, creator_id: 1, created_at, updated_at },
        );
        let unread_at = (id % 2 == 0).then_some(created_at);
        let involvement = Some(Involvement::Mentions);
        let membership =
            Membership { id, room_id: id, user_id: 1, involvement, unread_at, connected_at: None, connections: 0, created_at, updated_at };
        insert_membership(&conn, &membership);
    }
    for id in 1..=200 {
        let client_message_id = format!("5f0c2b4e-1d7a-4c39-9a57-{id:012}");
        insert_message(&conn, &Message { id, room_id: 1, creator_id: 1, client_message_id, created_at: at(id), updated_at: at(id + 1) });
    }
    conn
}

/// The median over 7 runs of the mean time per call.
fn time_per_call(iterations: u32, mut call: impl FnMut()) -> Duration {
    let mut runs: Vec<Duration> = (0..7)
        .map(|_| {
            let start = Instant::now();
            for _ in 0..iterations {
                call();
            }
            start.elapsed() / iterations
        })
        .collect();
    runs.sort();
    runs[runs.len() / 2]
}

/// `cargo test --release -p campfire_db --lib -- --ignored --nocapture row_reading_timing`
#[test]
#[ignore = "a timing, run by hand in a release build"]
fn row_reading_timing() {
    let conn = timing_database();
    let before = Message::find(&conn, 161).unwrap();
    let timings = [
        ("Message::last_page (40 rows)", time_per_call(20_000, || drop(black_box(Message::last_page(&conn, 1).unwrap())))),
        ("Message::page_before (40 rows)", time_per_call(20_000, || drop(black_box(Message::page_before(&conn, 1, &before).unwrap())))),
        (
            "Membership::visible_with_ordered_room (12 rows)",
            time_per_call(20_000, || drop(black_box(Membership::visible_with_ordered_room(&conn, 1).unwrap()))),
        ),
        ("Message::find (1 row)", time_per_call(200_000, || drop(black_box(Message::find(&conn, 100).unwrap())))),
        ("Room::find (1 row)", time_per_call(200_000, || drop(black_box(Room::find(&conn, 5).unwrap())))),
    ];
    for (name, per_call) in timings {
        println!("{name}: {per_call:?}");
    }
}
