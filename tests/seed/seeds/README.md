# Parity seeds

A seed is a SQLite database plus an Active Storage directory, generated **by the reference Rails
app** from `reference/test/fixtures` and a scripted scenario. Both servers run on copies of the same
seed, so every screen state starts from identical data.

```sh
parity/bin/reference build              # campfire-reference image (once, and after reference/ bumps)
parity/bin/seed build                   # all seeds -> parity/.seed/<name>/
parity/bin/seed build default           # just one
parity/bin/seed list

parity/bin/reference up --seed default --port 3100 --time 2026-03-02T16:00:00Z
parity/bin/check-inventory --port 3100 --seed default
parity/bin/reference down --port 3100   # or: down --all
```

Each seed is `parity/.seed/<name>/`:

| Path | What |
|---|---|
| `db/production.sqlite3` | The database (WAL checkpointed; mount as `storage/db/production.sqlite3`) |
| `storage/` | Active Storage disk service root (`storage/files` in the app), variants and previews included |
| `labels.json` | `table.label` → id, plus the strings screens need (see below) |

Builds are reproducible: rebuilding a seed gives the same SQL dump and the same storage tree, byte
for byte (blob keys are numbered, password digests pinned, SQL-side timestamps settled).
Build in Docker (the default when the daemon is reachable): `PARITY_RUNTIME=native` seeds are for
local convenience only, because the host's libvips/ffmpeg make different variant bytes.

## How a seed is built

1. `bin/rails db:prepare` in production mode on an empty storage dir (schema.rb).
2. `bin/rails runner parity/seeds/build.rb NAME` evaluates `parity/seeds/NAME.rb` in a
   `Parity::Seed` (`lib/seed.rb`). Seeds layer with `based_on "default"`.
3. `default` loads every fixture exactly as `fixtures :all` does, with the clock stubbed at
   `NOW`, so relative fixture times become absolute. Everything after that goes through the app's own
   models (`create_with_attachment!`, `create_bot!`, `deactivate`, `boosts.create!`, …), each at an
   explicit instant (`at`), so callbacks, rich text canonicalization, attachment analysis, the
   `:thumb`/`:square` variants and video previews are all Ruby's.
4. Background jobs are discarded (push, webhooks, banned-content removal).
5. Every representation a page asks for is processed here, so no server makes one on the fly under
   a random blob key: a video's poster is `preview(format: :webp, resize_to_limit: [1200, 800])`
   (app/helpers/messages/attachment_presentation.rb), not the plain `preview(format: :webp)` that
   `Message::Attachment#process_attachment` makes.

## The clock

`NOW` = **2026-03-02 16:00:00 UTC** (`labels.clock.now`). Visual states run with the browser clock
fixed there and the server started there (`reference up --time 2026-03-02T16:00:00Z`; it ticks, or
use `--freeze`). All seeded times are at or before `NOW` and no two time-ordered rows share a
timestamp:

- fixture rows are spread out per table in file order (accounts, users, rooms… weeks before `NOW`);
  fixture messages keep their relative times (15:00–15:55 on 2026-03-02)
- the designers showcase is on 2026-03-01 from 09:00 (so the fixtures' messages start a new day)
- the busy room's 120 generated messages run from 2026-02-28 08:07, 7 minutes apart

## Seeds

| Seed | Contents |
|---|---|
| `default` | Fixtures + the scenario below. Almost every state uses it. |
| `first_run` | Empty schema: no account, no users. `/session/new` redirects to `/first_run`. |
| `custom_styles` | `default` + an account logo (`black_hole.jpg`, `:large`/`:small` variants processed) + custom CSS redefining every custom property in `colors.css`, light and dark. |
| `restricted` | `default` + "Must be admin to create new rooms". |
| `crowd` | `default` + 520 members (all in the open rooms): past the room user filter (> 20), the 20 direct-ping placeholders, and the account user list's 500-per-page. |

## The default scenario

**People** (password for everyone: `secret123456`, `labels.passwords.all`; sign-in email:
`labels.emails.<user>`)

| Label | Who |
|---|---|
| `david` | Administrator. Initials avatar. Has 2 push subscriptions, 3 recent searches. |
| `jason` | Administrator with an image avatar (`moon.jpg`). |
| `jz` | Member, bio "Designer". |
| `kevin` | Member, bio "Programmer". A member in rooms David created ("someone else's room"); creator of `quiet`. |
| `bender` | Bot with a webhook (`bot_keys.bender`). |
| `deploy_bot` | Bot with an avatar, no webhook (`bot_keys.deploy_bot`). |
| `old_bot` | Deactivated bot. |
| `rita` | Deactivated member; her message stays in designers. |
| `mallory` | Banned member; ban on IP `203.0.113.9` (`ips.banned`). |
| `loner` | Member of no rooms at all (the welcome page). |

**Rooms**

| Label | Type | Members | Notes |
|---|---|---|---|
| `pets` | Open | david, jason (+ everyone created later) | `Room.original`, no messages: shows the invitation |
| `hq` | Open | david, jason, jz, kevin (+ later) | David's involvement: nothing |
| `archive` | Open | all active users | David's involvement: invisible (not in his sidebar) |
| `designers` | Closed | david, jason, jz, kevin, deploy_bot, mallory | The showcase: one message per presentation type |
| `watercooler` ("All Talk") | Closed | david, jason, bender | The busy room: 131 messages; unread for David |
| `quiet` | Closed | kevin (creator), david | Empty |
| `broken` | Closed | david | One unrenderable message (its author row is gone) |
| `david_and_jason` | Direct | | 3 messages |
| `david_and_kevin` | Direct | | Unread for David; David's involvement: nothing |
| `bender_and_kevin` | Direct | | Bot ping |
| `group_direct` | Direct | david, jason, jz, kevin | Avatar group in the sidebar; unread for David |

David's involvements cover every bell: designers mentions, pets/watercooler everything, hq nothing,
archive invisible, david_and_kevin nothing. Unread: David has watercooler, david_and_kevin and
group_direct; Kevin has designers.

**Messages** (`messages.<label>`, all in designers unless noted)

| Label | Presentation |
|---|---|
| `plain`, `threaded` | Plain text; the second is Jason again 2 minutes later (threaded) |
| `long` | Paragraphs, bold/italic, ordered and unordered lists, blockquote, inline code, h2, a link |
| `emoji` | Emoji only (`message--emoji`) |
| `code`, `code_plain` | `<pre data-language="ruby">` with `<br>` lines; a Trix-era `<pre>` with newlines and no language |
| `table` | Lexxy table in `figure.lexxy-content__table-wrapper` |
| `formatting` | `s`, `u`, `mark`, `strong`, `em`, `del` |
| `mention` | Lexxy mention of Kevin (valid SGID) |
| `mention_marshal` | Trix-era body mentioning David with a Rails 7 Marshal-era SGID whose signature doesn't verify |
| `sound_image`, `sound_text` | `/play 56k` (image sound), `/play bell` (text sound) |
| `opengraph` | Text + link + unfurled embed (Lexxy) |
| `solo_unfurl` | A link that is only unfurled (the link paragraph is removed) |
| `opengraph_trix_tweet` | Trix-era embed with attributes and a Twitter avatar image (`og-embed--twitter-avatar`) |
| `autolink` | Bare URLs, auto-linked at render |
| `image`, `image_large` | `moon.jpg` 640×640 (as is), `black_hole.jpg` 3840×2160 (scaled to 1200 wide) |
| `video` | `alpha-centuri.mov` with its webp poster preview |
| `file`, `file_bmp` | `launch-notes.txt`, and `pixel.bmp` (an image that isn't variable) |
| `bot` | From Deploy Bot |
| `by_deactivated_user` | From Rita |
| `edited` | Edited 3 minutes after posting (`updated_at` > `created_at`) |
| `unboosted`, `boosted_one`, `boosted_many`, `boosted_by_david` | Boosts: none; one (jz 🍕); six from five people incl. text and David's; only David's ("yours") |
| `first` … `thirteenth` | The fixtures' messages (first/second/third in designers, the rest in watercooler) |
| `busy_001` … `busy_120` | The busy room's older messages; `busy_060` is a full 81-message `page_around` |
| `bot_in_watercooler` | Bender in watercooler |
| `direct_first`, `direct_unread`, `group_direct_first` | In the direct rooms |
| `unrenderable` | In `broken` |

**Other labels**: `boosts.*` (incl. `david_on_boosted_by_david`), `memberships.*`, `push_subscriptions.*`
(`david_chrome`, `david_firefox`, …), `searches.david_pizza`, `sessions.david_safari`, `bans.mallory`,
`webhooks.bender`, `accounts.signal`, and strings: `transfers.david` (valid until `NOW` + 4h),
`transfers.david_expired`, `transfers.kevin`, `bot_keys.*`, `join_codes.signal`, `avatar_tokens.*`,
`passwords.all`, `ips.banned`, `emails.*`. The `crowd` seed adds `users.crowd_first`/`crowd_last`.

## Notes for the capture engine

- **Sign-in is rate limited**: 10 per IP per 3 minutes (`SessionsController`). Sign each user in once
  per instance and reuse the cookies.
- **Absolute URLs contain the request host**: the join link, transfer links, their QR codes and the
  bot curl commands. Serve both apps under the same origin (or map it) or they differ.
- **Mutating states** (`mutates: true` in `screens.yml`) change the database; give each a fresh
  instance (`reference up` takes a few seconds and instances run side by side).
- **Visiting a room marks it read** for that user (`PresenceChannel#present` clears
  `memberships.unread_at`), and every sidebar shows unread rooms. States that open a room where
  their user has unread messages (David: watercooler, david_and_kevin, group_direct; Kevin:
  designers) are therefore `mutates: true`, so the other states see the seed's unread badges
  whatever order they run in.

## Action Cable in the reference image

The parity image (`parity/docker/Dockerfile`) adds one initializer to the app,
`config/initializers/parity_action_cable.rb`, which sets `config.action_cable.worker_pool_size = 1`
(`PARITY_CABLE_WORKER_POOL_SIZE` overrides it, for experiments). Action Cable otherwise hands each
incoming command to a pool of 4 threads, so a connection's commands can run out of order. Every room
page sends an `unsubscribe` immediately followed by a `subscribe` for the same identifier (the
sidebar turbo-frame replaces its `turbo-cable-stream-source` elements); when the subscribe runs
first the server drops it as a duplicate and never confirms it, and the page renders differently
from run to run. The Rust cable server runs a connection's commands sequentially, in the order they
arrive, so one worker makes the oracle behave the way the port does, deterministically. It changes
no rendering: it only removes the reordering.
- **External images** in seeded embeds (`https://example.com/og/**`,
  `https://pbs.twimg.com/profile_images/**`) must be answered by the harness, e.g. with
  `reference/test/fixtures/files/moon.jpg`.
- **User agents** for platform branches are in `user_agents.yml`. How `ApplicationPlatform`
  classifies them: chrome_* are Chrome (android/ios/windows/mac by OS), safari_* Safari, firefox_*
  Firefox, `edge_windows` Edge (the useragent gem only recognizes the legacy `Edge/` token, and
  reports every Edge as Windows, so the Edge-on-macOS branches are unreachable), `opera_linux` none
  of them (the `else` branches), `chrome_outdated` is blocked by `allow_browser`, `apple_messages`
  gets the "Campfire" title on the unsupported-browser page.
- **Reference bug the inventory no longer captures**: with an EdgeHTML user agent (`Edge/`),
  `pwa/_install_instructions` references a missing `install-edge.svg`, so the profile and every
  room page answer 500 in Rails. The Rust app ships the image (`crates/assets/overrides/`).
- **Server errors**: `/searches?q=NOT` is a 500 in Rails (a bare FTS5 operator). The Rust app
  searches for the word instead, so the inventory no longer captures it.
