//! The fixture loader against facts from a Ruby-loaded fixtures database, and (ignored
//! unless `CAMPFIRE_RUBY_FIXTURES_DB` is set) a row-for-row comparison with one.

use super::*;
use crate::{Membership, Message, Role, Room, RoomType, Session, User};

#[test]
fn ids_are_label_crcs() {
    let t = TestDb::new();
    assert_eq!(t.read(|c| User::find(c, 127326141)).name, "David");
    assert_eq!(t.read(|c| Room::find(c, 654632876)).name.as_deref(), Some("Designers"));
}

#[test]
fn associations_enums_and_defaults() {
    let t = TestDb::new();
    let pets = t.read(|c| Room::find(c, id("pets")));
    assert_eq!((pets.room_type, pets.creator_id), (RoomType::Open, id("david")));

    assert_eq!(t.read(|c| User::find(c, id("david"))).role, Role::Administrator);
    assert_eq!(t.read(|c| User::find(c, id("jz"))).role, Role::Member);
    let bender = t.read(|c| User::find(c, id("bender")));
    assert_eq!((bender.role, bender.bot_token.as_ref().map(String::len)), (Role::Bot, Some(12)));

    let kevin_designers = t.read(|c| Membership::find(c, id("kevin_designers")));
    assert_eq!(kevin_designers.involvement, Some(crate::Involvement::Mentions), "column default");
    assert_eq!(kevin_designers.connections, 0);

    let rich_text: (i64, String) = t.read(|c| {
        Ok(c.query_row("SELECT record_id, record_type FROM action_text_rich_texts WHERE id = ?", [id("first")], |r| {
            Ok((r.get(0)?, r.get(1)?))
        })?)
    });
    assert_eq!(rich_text, (id("first"), "Message".into()));
}

#[test]
fn erb_times_are_whole_seconds_and_default_timestamps_are_now() {
    let t = TestDb::new();
    let first = t.read(|c| Message::find(c, id("first")));
    assert_eq!(first.created_at.subsec_microsecond(), 0);
    assert!(first.updated_at > first.created_at);
    let session = t.read(|c| Session::find(c, id("david_safari")));
    assert_eq!(session.last_active_at.subsec_microsecond(), 0);
    assert!((session.created_at.as_second() - session.last_active_at.as_second() - 7200).abs() <= 1);
}

#[test]
fn passwords_are_bcrypt_of_secret123456() {
    let t = TestDb::new();
    let david = t.read(|c| User::find(c, id("david")));
    assert!(david.authenticate("secret123456"));
    assert!(david.password_digest.unwrap().starts_with("$2a$"));
}

/// Dump a table with values normalized where Ruby and Rust can't agree (bcrypt salts, bot
/// tokens, and timestamps, which are compared by shape: whole seconds vs. fractional).
pub(super) fn dump(conn: &Connection, table: &str) -> Vec<String> {
    let mut stmt = conn.prepare(&format!("SELECT * FROM \"{table}\" ORDER BY id")).unwrap();
    let names: Vec<String> = stmt.column_names().into_iter().map(String::from).collect();
    let mut rows = stmt.query([]).unwrap();
    let mut out = Vec::new();
    while let Some(row) = rows.next().unwrap() {
        let mut fields: Vec<String> = Vec::new();
        let mut sorted: Vec<(String, String)> = names
            .iter()
            .enumerate()
            .map(|(i, name)| {
                let value: rusqlite::types::Value = row.get(i).unwrap();
                let rendered = match (name.as_str(), value) {
                    ("password_digest", rusqlite::types::Value::Text(s)) => {
                        format!("bcrypt:{}", &s[..4])
                    }
                    ("bot_token", rusqlite::types::Value::Text(s)) => format!("token:{}", s.len()),
                    (n, rusqlite::types::Value::Text(s)) if n.ends_with("_at") => {
                        if s.contains('.') {
                            "time:fraction".into()
                        } else {
                            format!("time:whole:{}", &s[..10])
                        }
                    }
                    (_, v) => format!("{v:?}"),
                };
                (name.clone(), rendered)
            })
            .collect();
        sorted.sort();
        for (name, value) in sorted {
            fields.push(format!("{name}={value}"));
        }
        out.push(fields.join(" "));
    }
    out
}

#[test]
#[ignore = "needs CAMPFIRE_RUBY_FIXTURES_DB, a database the reference app filled with `db:fixtures:load`"]
fn fixtures_match_ruby_row_for_row() {
    let path = std::env::var("CAMPFIRE_RUBY_FIXTURES_DB").expect("CAMPFIRE_RUBY_FIXTURES_DB");
    let ruby = Connection::open(path).unwrap();
    let t = TestDb::new();
    let tables = [
        "accounts", "action_text_rich_texts", "boosts", "memberships", "messages", "push_subscriptions", "rooms", "searches", "sessions",
        "users", "webhooks",
    ];
    for table in tables {
        let expected = dump(&ruby, table);
        let actual = t.read(|c| Ok(dump(c, table)));
        assert_eq!(actual, expected, "{table}");
    }
}

#[test]
#[ignore = "writes a database to CAMPFIRE_EXPORT_DB for the Rails rollback check"]
fn export_database_for_rails() {
    let path = std::env::var("CAMPFIRE_EXPORT_DB").expect("CAMPFIRE_EXPORT_DB");
    let _ = std::fs::remove_file(&path);
    let env = crate::Env { bcrypt_cost: 4, ..Default::default() };
    let mut config = crate::Config::new(&path);
    config.environment = "test".into();
    let db = crate::Database::open(config, env).unwrap();
    db.write_blocking(|tx| {
        let options = crate::fixtures::Options { now: tx.now(), bcrypt_cost: 4 };
        crate::fixtures::load(tx.conn(), &crate::fixtures::reference_dir(), &options)?;
        Ok(())
    })
    .unwrap();
    db.write_blocking(|tx| {
        let message = Message::create(
            tx,
            crate::NewMessage {
                room_id: id("designers"),
                creator_id: id("david"),
                client_message_id: Some("rust-1".into()),
                body: Some("Written by <b>Rust</b> hovercraft".into()),
                attachment_blob_id: None,
            },
        )?;
        crate::Boost::create(tx, message.id, id("jason"), "🦀")?;
        let user = User::create(
            tx,
            crate::NewUser {
                name: "Rusty".into(),
                email_address: Some("rusty@example.com".into()),
                password_digest: Some(crate::PasswordDigest::create("secret123456", 4).unwrap()),
                ..Default::default()
            },
        )?;
        crate::Session::start(tx, user.id, Some("ua"), Some("8.8.8.8"))?;
        crate::Search::record(tx, user.id, "hovercraft")?;
        Room::create_for(tx, RoomType::Closed, Some("Rust Room"), user.id, &[user.id, id("david")])?;
        let mut account = crate::Account::first(tx.conn())?.unwrap();
        account.update(tx, None, None, Some(&[("restrict_room_creation_to_administrators", "true")]))?;
        Ok(())
    })
    .unwrap();
}
