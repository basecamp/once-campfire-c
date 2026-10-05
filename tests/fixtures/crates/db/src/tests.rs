//! Ports of `reference/test/models/**`, run against the reference fixtures.

mod account_test;
mod callbacks_test;
mod columns_test;
mod differential_test;
mod first_run_test;
mod fixtures_test;
mod membership_test;
mod message_test;
mod push_test;
mod room_test;
mod user_test;

use std::sync::Arc;

use rails_compat::clock::{Clock, TestClock};

use crate::fixtures::{self, identify};
use crate::testing::{BasicRichText, RecordingSink};
use crate::{Config, Connection, Database, Env, Event, Result, Timestamp, Tx};

/// A database loaded with the fixtures, a recording event sink and a controllable clock
/// (`fixtures :all` plus `ActiveSupport::Testing::TimeHelpers`).
pub struct TestDb {
    pub db: Database,
    pub sink: RecordingSink,
    pub clock: Arc<TestClock>,
    _dir: tempfile::TempDir,
}

impl TestDb {
    pub fn new() -> Self {
        let dir = tempfile::tempdir().unwrap();
        let sink = RecordingSink::new();
        let clock = Arc::new(TestClock::new());
        let env = Env { clock: clock.clone(), sink: Arc::new(sink.clone()), rich_text: Arc::new(BasicRichText), bcrypt_cost: 4 };
        let mut config = Config::new(dir.path().join("test.sqlite3"));
        config.readers = 2;
        config.environment = "test".into();
        let db = Database::open(config, env).unwrap();
        db.write_blocking(|tx| {
            let options = fixtures::Options { now: tx.now(), bcrypt_cost: 4 };
            fixtures::load(tx.conn(), &fixtures::reference_dir(), &options)
        })
        .unwrap();
        Self { db, sink, clock, _dir: dir }
    }

    pub fn write<T: Send + 'static>(&self, f: impl FnOnce(&mut Tx<'_>) -> Result<T> + Send + 'static) -> T {
        self.db.write_blocking(f).unwrap()
    }

    pub fn try_write<T: Send + 'static>(&self, f: impl FnOnce(&mut Tx<'_>) -> Result<T> + Send + 'static) -> Result<T> {
        self.db.write_blocking(f)
    }

    pub fn read<T>(&self, f: impl FnOnce(&Connection) -> Result<T>) -> T {
        self.db.read_blocking(f).unwrap()
    }

    pub fn now(&self) -> Timestamp {
        Timestamp::from_jiff(self.clock.now())
    }

    pub fn events(&self) -> Vec<Event> {
        self.sink.events()
    }

    /// `travel_to Membership::Connectable::CONNECTION_TTL.from_now + 1`
    pub fn travel(&self, seconds: i64) {
        self.clock.travel(jiff::SignedDuration::from_secs(seconds));
    }
}

/// A fixture's id, by label.
pub fn id(label: &str) -> i64 {
    identify(label)
}
