# Pinned seed recipes (F02)

These are the reference seed recipes used by the browser parity suite and the
benchmarks, copied verbatim from the pinned Rust reference checkout
(`basecamp/once-campfire-rust`, revision
`64f86353021145b63849fb1cd93adeb08f3b8dbb`). They are data, not C code: a seed
is a fully populated reference database plus its storage tree, so importing it
is the way to give the C server the same rows every other surface is tested
against.

| Path here | Upstream source | Notes |
| --- | --- | --- |
| `tests/seed/bin/seed` | `tmp/rust-ref/parity/bin/seed` | builder: `build [NAME...]`, `list` |
| `tests/seed/seeds/build.rb` | `tmp/rust-ref/parity/seeds/build.rb` | runner entry used by `bin/seed` |
| `tests/seed/seeds/default.rb` | `tmp/rust-ref/parity/seeds/default.rb` | the default seed (`load_fixtures` plus the screen-state tour) |
| `tests/seed/seeds/crowd.rb` | `tmp/rust-ref/parity/seeds/crowd.rb` | many-user seed |
| `tests/seed/seeds/first_run.rb` | `tmp/rust-ref/parity/seeds/first_run.rb` | setup/first-run seed |
| `tests/seed/seeds/restricted.rb` | `tmp/rust-ref/parity/seeds/restricted.rb` | restricted-access seed |
| `tests/seed/seeds/custom_styles.rb` | `tmp/rust-ref/parity/seeds/custom_styles.rb` | custom styles seed |
| `tests/seed/seeds/lib/seed.rb` | `tmp/rust-ref/parity/seeds/lib/seed.rb` | shared helpers (`load_fixtures`, labels, ...) |
| `tests/seed/seeds/user_agents.yml` | `tmp/rust-ref/parity/seeds/user_agents.yml` | user-agent matrix inputs |
| `tests/seed/seeds/README.md` | `tmp/rust-ref/parity/seeds/README.md` | upstream tour/labels description |

Provenance (source path, pinned revision, SHA-256 of every copied byte) is
recorded in `tests/fixtures/MANIFEST.json`, section `seed`.

## How the reference builds them (upstream contract)

`parity/bin/seed build [NAME...]` prepares each seed with the reference Rails
app (`db:prepare`, i.e. `schema.rb`) and then runs `parity/seeds/NAME.rb` under
`bin/rails runner`. Each seed lands in `parity/.seed/NAME/` as:

    db/production.sqlite3     reference database
    storage/                  Active Storage root (blobs, variants)
    labels.json               fixture labels used by parity and bench addressing

The builder needs the Ruby reference runtime (`parity/bin/reference`,
`PARITY_RUNTIME=docker|native`); it is not run by the C build.

## How a future `make seed` imports into a fresh C database

`make seed` (F03 wires the target; D01/B01 own the importer) will:

1. require a prebuilt pinned seed directory (`parity/.seed/NAME` or
   `tests/seed/.seed/NAME`) — the Ruby reference is only used beforehand to
   regenerate pinned seed data, exactly like the fixtures; the C build does not
   run Ruby;
2. create a **fresh** C database with `contracts/schema.sql` and
   `user_version=1` (no migration path, D-C01), never an existing file;
3. copy the seed `storage/` tree into the C storage root and import the rows
   into the fresh schema, preserving IDs, timestamps, rich text, attachments
   and labels;
4. be a test/benchmark command only: **production startup never imports a seed**
   and never upgrades a Rails database (02-data-auth.md, D01).

Benchmarks consume the same layout: `bench/lib/benchlib.py` uses
`SEED = parity/.seed/default`, snapshots it once per run and neuters outbound
endpoints, and `bench/run` starts each configuration from a fresh copy. Those
files stay upstream-owned (B01); they are recorded as external references in
`tests/fixtures/MANIFEST.json`.
