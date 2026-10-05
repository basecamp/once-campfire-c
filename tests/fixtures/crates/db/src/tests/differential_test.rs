//! Runs the same scenario as `scenario.rb` (reproduced below) through the Rust models and
//! compares every table with the database the reference app produced from the same
//! fixtures. Ignored unless `CAMPFIRE_RUBY_SCENARIO_DB` points at that database.
//!
//! ```ruby
//! m = rooms(:designers).messages.create!(body: "Hello <b>there</b>", client_message_id: "s1", creator: david)
//! b = m.boosts.create!(content: "hi", booster: jason)
//! m.boosts.create!(content: "yo", booster: kevin)
//! b.destroy!
//! m.update!(body: "Edited hovercraft")
//! m2 = rooms(:hq).messages.create!(body: "Doomed", client_message_id: "s2", creator: jason)
//! m2.boosts.create!(content: "x", booster: david)
//! m2.destroy!
//! memberships(:kevin_designers).destroy
//! u = User.create!(name: "User", email_address: "u@example.com", password: "secret123456")
//! Rooms::Open.create!(name: "Open!", creator: david)
//! Rooms::Closed.create_for({ name: "Hello!", creator: david }, users: [kevin, david])
//! r = rooms(:watercooler).becomes!(Rooms::Open); r.save!
//! rooms(:pets).update!(name: "Pets2")
//! memberships(:jason_pets).update!(involvement: "invisible")
//! mm = memberships(:david_hq); mm.connected; mm.connected; mm.disconnected
//! Rooms::Direct.find_or_create_for([jz, kevin])        # Current.user = david
//! kevin.sessions.create!(ip_address: "8.8.8.8", user_agent: "x")
//! kevin.ban
//! kevin.unban
//! jz.remove_banned_content
//! jz.deactivate
//! 12.times { |n| david.searches.record("q#{n}") }
//! bot = User.create_bot!(name: "Bot", webhook_url: "http://x")
//! bot.update_bot!(name: "Bot2", webhook_url: "http://y")
//! a = Account.first; a.settings.restrict_room_creation_to_administrators = true; a.save!
//! rooms(:hq).messages.create!(body: "Last one", client_message_id: "s3", creator: kevin)
//! ```

use super::*;
use crate::{Account, Boost, Membership, Message, NewMessage, NewUser, Room, RoomType, Search, Session, User, UserChanges};

fn message(room: &str, creator: &str, body: &str, client_message_id: &str) -> NewMessage {
    NewMessage {
        room_id: id(room),
        creator_id: id(creator),
        client_message_id: Some(client_message_id.into()),
        body: Some(body.into()),
        attachment_blob_id: None,
    }
}

fn run_scenario(t: &TestDb) {
    // Each step is its own write, as each Rails call is its own transaction.
    let m = t.write(|tx| Message::create(tx, message("designers", "david", "Hello <b>there</b>", "s1")));
    let b = t.write(move |tx| Boost::create(tx, m.id, id("jason"), "hi"));
    t.write(move |tx| Boost::create(tx, m.id, id("kevin"), "yo").map(|_| ()));
    t.write(move |tx| b.destroy(tx));
    t.write(move |tx| Message::find(tx.conn(), m.id)?.update_body(tx, "Edited hovercraft"));
    let m2 = t.write(|tx| Message::create(tx, message("hq", "jason", "Doomed", "s2")));
    t.write(move |tx| Boost::create(tx, m2.id, id("david"), "x").map(|_| ()));
    t.write(move |tx| Message::find(tx.conn(), m2.id)?.destroy(tx));
    t.write(|tx| Membership::find(tx.conn(), id("kevin_designers"))?.destroy(tx));
    t.write(|tx| {
        User::create(
            tx,
            NewUser {
                name: "User".into(),
                email_address: Some("u@example.com".into()),
                password_digest: Some(crate::PasswordDigest::create("secret123456", 4).unwrap()),
                ..Default::default()
            },
        )
        .map(|_| ())
    });
    t.write(|tx| Room::create(tx, RoomType::Open, Some("Open!"), id("david")).map(|_| ()));
    t.write(|tx| Room::create_for(tx, RoomType::Closed, Some("Hello!"), id("david"), &[id("kevin"), id("david")]).map(|_| ()));
    t.write(|tx| Room::find(tx.conn(), id("watercooler"))?.update(tx, None, Some(RoomType::Open)));
    t.write(|tx| Room::find(tx.conn(), id("pets"))?.update(tx, Some(Some("Pets2")), None));
    t.write(|tx| Membership::find(tx.conn(), id("jason_pets"))?.update_involvement(tx, crate::Involvement::Invisible));
    t.write(|tx| {
        let mut mm = Membership::find(tx.conn(), id("david_hq"))?;
        mm.connected(tx)?;
        mm.connected(tx)?;
        mm.disconnected(tx)
    });
    t.write(|tx| Room::find_or_create_direct_for(tx, &[id("jz"), id("kevin")], id("david")).map(|_| ()));
    t.write(|tx| Session::start(tx, id("kevin"), Some("x"), Some("8.8.8.8")).map(|_| ()));
    t.write(|tx| User::find(tx.conn(), id("kevin"))?.ban(tx));
    t.write(|tx| User::find(tx.conn(), id("kevin"))?.unban(tx));
    t.write(|tx| User::find(tx.conn(), id("jz"))?.remove_banned_content(tx).map(|_| ()));
    t.write(|tx| User::find(tx.conn(), id("jz"))?.deactivate(tx));
    for n in 0..12 {
        t.write(move |tx| Search::record(tx, id("david"), &format!("q{n}")).map(|_| ()));
    }
    let bot = t.write(|tx| User::create_bot(tx, "Bot", Some("http://x")));
    t.write(move |tx| {
        let mut bot = bot;
        bot.update_bot(tx, UserChanges { name: Some("Bot2".into()), ..Default::default() }, Some("http://y"))
    });
    t.write(|tx| Account::first(tx.conn())?.unwrap().update(tx, None, None, Some(&[("restrict_room_creation_to_administrators", "true")])));
    t.write(|tx| Message::create(tx, message("hq", "kevin", "Last one", "s3")).map(|_| ()));
}

/// Every row, with values that are random or clock-dependent reduced to their shape.
fn normalized_dump(conn: &Connection, table: &str, order: &str) -> Vec<String> {
    let columns = if table == "message_search_index" { "rowid AS id, body" } else { "*" };
    let mut stmt = conn.prepare(&format!("SELECT {columns} FROM \"{table}\" ORDER BY {order}")).unwrap();
    let names: Vec<String> = stmt.column_names().into_iter().map(String::from).collect();
    let mut rows = stmt.query([]).unwrap();
    let mut out = Vec::new();
    while let Some(row) = rows.next().unwrap() {
        let mut fields: Vec<(String, String)> = Vec::new();
        for (i, name) in names.iter().enumerate() {
            let value: rusqlite::types::Value = row.get(i).unwrap();
            let rendered = match (name.as_str(), value) {
                ("password_digest", rusqlite::types::Value::Text(s)) => {
                    format!("bcrypt:{}", &s[..4])
                }
                ("bot_token" | "token" | "join_code", rusqlite::types::Value::Text(s)) => {
                    format!("random:{}", s.len())
                }
                ("email_address", rusqlite::types::Value::Text(s)) if s.contains("-deactivated-") => {
                    let (local, rest) = s.split_once("-deactivated-").unwrap();
                    format!("{local}-deactivated-<uuid:{}>{}", rest.find('@').unwrap(), &rest[rest.find('@').unwrap()..])
                }
                (n, rusqlite::types::Value::Text(s)) if n.ends_with("_at") => {
                    format!("time:{}", s.split_once('.').map(|(_, f)| f.len()).unwrap_or(0))
                }
                (_, v) => format!("{v:?}"),
            };
            fields.push((name.clone(), rendered));
        }
        fields.sort();
        out.push(fields.into_iter().map(|(n, v)| format!("{n}={v}")).collect::<Vec<_>>().join(" "));
    }
    out
}

#[test]
#[ignore = "needs CAMPFIRE_RUBY_SCENARIO_DB, the reference app's database after scenario.rb"]
fn scenario_matches_ruby() {
    let path = std::env::var("CAMPFIRE_RUBY_SCENARIO_DB").expect("CAMPFIRE_RUBY_SCENARIO_DB");
    let ruby = Connection::open(path).unwrap();
    let t = TestDb::new();
    run_scenario(&t);

    let mut mismatches = Vec::new();
    for (table, order) in [
        ("accounts", "id"),
        ("action_text_rich_texts", "id"),
        ("bans", "id"),
        ("boosts", "id"),
        ("memberships", "room_id, user_id"),
        ("messages", "id"),
        ("push_subscriptions", "id"),
        ("rooms", "id"),
        ("searches", "id"),
        ("sessions", "id"),
        ("users", "id"),
        ("webhooks", "id"),
        ("message_search_index", "rowid"),
    ] {
        let expected = normalized_dump(&ruby, table, order);
        let actual = t.read(|c| Ok(normalized_dump(c, table, order)));
        if expected != actual {
            let only_ruby: Vec<_> = expected.iter().filter(|r| !actual.contains(r)).collect();
            let only_rust: Vec<_> = actual.iter().filter(|r| !expected.contains(r)).collect();
            mismatches.push(format!("{table}:\n  ruby only: {only_ruby:#?}\n  rust only: {only_rust:#?}"));
        }
    }
    assert!(mismatches.is_empty(), "{}", mismatches.join("\n"));
}
