# Changelog

All notable changes to the Campfire C port are documented here. Commit
subjects stay short; this file carries the detail: task IDs, what landed,
acceptance evidence, and known gaps. Live status and full evidence links live
in `docs/devel/IMPLEMENTATION-ROADMAP.md` (local working document, not committed).

## 2026-10-05 23:15 — P01 front, V02 completion and the final measurements

- **P01 merged**: configured TLS, ALPN and the HTTP/2 front (`src/front/`),
  serving-loop and `main.c` wiring, nghttp2/OpenSSL linkage, and two response
  header accessors (`cf_response_header_count` / `cf_response_header_at`)
  added identically to `src/cf.h` and `contracts/api.h`. Documented gaps
  (outside the pinned harness's HTTP/1.1-plain path): synchronous H2 FILE
  reads, no `/cable` over TLS, H2 trailers ignored.
- **V02 completion**: profiles and push pages render through real presenters
  (21:07–22:10); accounts/bots render shims replaced; per-recipient broadcast
  partials and real room forms; browser acceptance at 3 cases / 3 passed with
  E2E-01's deferred search + profile-logout steps restored and E2E-02 driving
  room access change, the bot create→curl→live-post flow, the push UI and the
  upload path. Message attachments are wired end to end (H02 upload accessor
  + S02 staging/marcel): the composer's multipart upload now renders the
  attachment in both browsers, with media transforms still the S03
  tool-boundary block.
- **Gate on the merged tree**: dev 1608/1608 (125 binaries), sanitize
  1608/1608 with zero ASan output, Fil-C/bench/TSan green; the two
  stack-use-after-scope view-span escapes and the QR SVG length bug fixed
  along the way.
- **README + MIT license**, structured like the other Campfire ports, with
  the tables below; the throughput table also carries the two comparison
  ratios (`C (cache off) vs Rails`, `C (cache on) vs Rust`).
- **Final measurements** (pinned harness, 3 interleaved reps per arm, 16
  clients; full provenance and raw outputs under
  `bench/results/46ed9c5-{uncached,cache}/`):

| Workload | Rails | Rust (cache on) | C (cache off) | C (cache on) | C (cache off) vs Rails | C (cache on) vs Rust |
|---|---:|---:|---:|---:|---:|---:|
| Room page | 234 | 31,741 | 1,995 | 104,523 | 8.5× | 3.3× |
| Messages page | 438 | 29,021 | 2,465 | 128,748 | 5.6× | 4.4× |
| Sidebar | 679 | 32,519 | 22,923 | 134,211 | 34× | 4.1× |
| Search | 434 | 33,010 | 5,395 | 128,249 | 12× | 3.9× |
| Post a message | 1,116 | 54,242 | 92,508 | 109,889 | 83× | 2.0× |
| `/up` | 4,363 | 131,207 | 170,347 | 180,614 | 39× | 1.4× |

  Body-cache gain 5.9–52× on the four admitted routes; cache-off C ahead of
  Rails on every row (5.6–83×); cache-on C ahead of the Rust image on every
  row (1.4–4.4×). Rust's row is its production cache configuration (the
  pinned harness cannot disable it) — compare it with C (cache on), not with
  C (cache off), which pairs with Rails. Cold start 133–150 ms, idle ~35–39 MiB, image 82 MiB
  unpacked (Rails: 2,591 ms / 377 MiB / 342 MiB). Every rep was
  load-caveated (shared host, 0/18 met the harness's quiet gate; steal
  0.00%), all reps valid and preflight-passing; the merged P01 front shows
  no measurable regression (flat `/up`, unchanged cache-hit CPU/success).
- **Housekeeping**: commits carry subject-only messages — this changelog is
  the detail store; the headings above are timestamped so commits and entries
  correlate without version tags. Raw outputs live per revision under
  `bench/results/`.

## 2026-10-05 21:21 — V02 attachments: H02 upload accessor + S02 message attachment path

- H02 multipart file parts are real uploads: each non-blank-filename part is
  spooled to a unique 0600 `RackMultipart*` temp file (unlinked; the params
  own the FD) and exposed through the proposed `cf_upload` /
  `cf_param_upload` accessor, added identically to `contracts/api.h` and
  `src/cf.h`, with Rack's filename normalization (filename*, %, basename),
  declared Content-Type reads, blank-filename drops and the unchanged
  100-field/16-file bounds. Contract change proposed for integrator
  ratification (no shape was recorded when S01 landed); see
  `docs/devel/evidence/V02-attachments.md`.
- S02's missing unfurl half lands: the marcel 1.1.0 tables are generated
  from the pinned `tables.rs` (`tests/fixtures/tools/marcel_tables.py` →
  `src/storage/marcel_tables.h`, `src/storage/marcel.{c,h}`) and
  `cf_active_stage_upload` streams an upload through S01 staging
  (16 MiB cap), identifies the content type, computes base64(MD5), generates
  the key and publishes exclusively; staged files roll back unless the DB
  write commits. `cf_active_analyze_metadata` covers the null analyzer and
  stops loudly (CF_INTERNAL) for image/video/audio (S03);
  `cf_blob_update_metadata` and `cf_attachment_records_for_blob` translate
  the pinned storage/blob.rs functions.
- messages#create/#update (and the shared by_bots path) implement the S02
  states: absent unchanged, nil/"" deletes (PurgeBlob event), multipart
  upload stages + attaches in one transaction, verified signed blob ids
  attach the existing blob; create runs the reference's synchronous
  `process_attachment` (tool-free analysis + attachment-record touch +
  re-read); update runs it only for unanalyzed blobs (the reference's
  AnalyzeJob has no async path yet). The presenter builds the real
  AttachmentView (signed blob/download paths, thumb/poster variations,
  metadata dimensions) and the file/preview arms render the reference
  markup. `tests/actions/**`, `tests/http/**`, `tests/storage/**`,
  `tests/views/**` extended; full dev 1585/1585 and sanitize 1585/1585, one
  falsification red/green. Browser case untouched; its `.message__attachment`
  assertion matches preview arms only and needs retargeting for the `.txt`
  sample (evidence §"Browser upload row expectation"). Media transforms,
  async analysis and the reference's 413-vs-400 oversize status remain
  S03/documented gaps.

## 2026-10-05 18:08 — Phase 4 wiring: merge phase 3, bind all landed packets

- Merged `master` (phase 3 K01/B01.initial + pulled-forward searches,
  sidebars, avatars-show) into `phase4`: no file overlap with wave
  packets; CHANGELOG keeps both sections.
- Integrator wiring (`build:` commit): Makefile sources/test lists plus
  curl/libssl/qrcodegen linkage in all five modes; `actions.h`
  declarations; 78 route rows rebound from dev-501; `main.c` job-queue
  start, default-handler + writer-consumer registration, libcurl global
  init, and ordered shutdown; new `cf_qr_code_svg` via vendored qrcodegen
  (gem-envelope SVG at level H; module-matrix bytes may differ from the
  gem-faithful algorithm — documented gap, byte-exact `rqrcode.rs` port
  remains a defined follow-up).
- Held back deliberately: rows 50/51/74/75 (users new/create/show,
  autocomplete — controller done, page/prompt views pending in the views
  wave) and S02 routes 169-177 (helpers only, controller actions never
  dispatched). 13 dev-501s remain; route-table/builtins expectations
  updated to the landed set.
- `test_media` live case: reports BLOCKED and passes inside `make test`
  (pinned tools absent by design); stays strict-red under
  CF_MEDIA_LIVE=1, with env save/restore so full-suite strict runs work.
- Full dev suite green on the wired tree.

## 2026-10-05 19:32 — Phase 4 wiring 2: views wave, storage controllers

- Makefile/test wiring for all eight views-wave packets; `actions.h`
  S02 declarations; routes 50/51/74/75 + 169-177 rebound — **zero
  dev-501 rows remain** (both route expectation tests pin the completed
  set; profiles/push routes stay bound to loud-500 gates until their
  presenter mapping lands).
- V-A closed at the action level: test stubs deleted, assertions
  retargeted to real renders, `User#title` fills the mention model
  (G1 golden gap closed, empty-title fallback keeps Anna byte-exact).
- V-F: bot/bot-boost JSON shims replaced by the shared serializers
  (PUBLIC_ORIGIN confirmed). Touch helper at three call sites.
- Wiring repairs: profiles membership href use-after-scope (borrowed
  block temporary), uninitialized sweep title field.
- Deferred with owner: bots/accounts/profiles/push shim→real swaps
  (need presenter constructors), room form/partial swaps + cable slots
  + broadcast-expectation updates, translation/host-resolve call-site
  switches, transfer-partial + model-header dedups, byte-exact
  `rqrcode.rs` port. Full dev suite green.

## 2026-10-05 16:14 — Phase 4 wave 1: jobs, storage, integrations, first controllers

Six packets implemented and unit-verified at dispatch level in the `phase4`
worktree. New sources and tests are committed; shared wiring (`Makefile`
`SRCS`/`UNIT_TEST_SRCS` entries, `src/actions/actions.h` declarations,
`src/routes.c` rebinding, `main.c` startup/consumer registration) is held for
a serial integrator pass, so the affected routes still answer dev-501 and the
new objects are not yet linked into the app build.

### Wave 2: remaining controller packets (dispatch level, unwired)
- **W2-A transfers/users/autocomplete/qr** (9,10,11 / 50,51,74 / 75 / 52):
  one-use signed transfer tokens, join-code signup, membership-scoped
  autocomplete JSON, pinned QR format. Tests 10+18+9+6=43 pass.
- **W2-B accounts family** (44,46,47 / 19,24,25,26 / 37 / 38,39 / 40,41,42):
  admin-before-lookup gating, permit/require/respond_to matrix, join-code
  rotation, conditional-GET 304. Tests 62/62 incl. ASan/UBSan. Logo uploads
  fail loudly pending S02/S03.
- **W2-C bots/keys** (29-32,34-36 / 27,28): key rotation invalidation,
  destroy-to-deactivate with DISCONNECT, P12-01 revalidation. Tests 27+5
  incl. ASan/UBSan.
- **W2-D profiles/push/avatars-destroy** (60-62 / 66,67,73 / 65 / 54):
  ownership gates, unconditional redirects, loud-500 render gates where
  A02 views are missing. Tests 15+19+7+5=46 incl. ASan/UBSan.
- **W2-E room subclasses** (105-112 / 113-120 / 121-125,128): per-subclass
  grants/participants, closed two-write update+revise with mandatory
  DISCONNECT(reconnect=true). Tests 29+24+23; rooms 27/27 unbroken.
- **W2-F boosts/by_bots/unfurl** (129-131,136 / 84,85 / 86-90 / 148):
  room-scoped auth then privilege checks, bot-key auth, unfurl
  400/204/200/500 arms. Tests 8+6+10+3; messages 35/35 unbroken.
- Known integrator queue: route rebinding for all rows above,
  `actions.h` declarations, Makefile `SRCS`/test lists, `main.c`/J02
  consumer wiring, I01 link objects, missing A02 view/presenter symbols
  (bots, accounts edit/users-stream/custom-styles, ProfileShow,
  PushSubscriptionsIndex, room new/edit forms, sidebar shared/direct
  partials, message/boost JSON), record-touch + private-host helpers,
  qrcodegen-backed `cf_qr_code_svg`, closed-revise control handler in
  production.

### J02 model-event job handlers
- New: `src/jobs/handlers.{c,h}`, `tests/jobs/test_handlers.c` (14/14 x3;
  existing `test_jobs` 9/9 and `test_jobs_writer` 4/4 unaffected).
- One handler per queueable kind with own-reader discipline (reader opened
  per invocation, closed before integration calls/`cf_write`);
  deleted/revoked targets are recorded no-ops; failures return non-`CF_OK`
  with no retry. RemoveBannedContent scans in fixed SQL bounded by
  `LIMIT 101`, destroys at most 100 messages per `cf_write`, and requeues
  the remainder (205-message convergence: 100+100+5, 2 requeues).
  I01/I02/S02 calls are weak-hook seams (NULL today, TODO-marked).
- Acceptance: JOB-02/03, DB-02 re-read/revalidation half. Gaps: sanitizer
  runs not done; non-NULL cable broadcast path untested (NULL by design, no
  sockets in tests).

### S02 Active Storage signed helpers
- New: `src/storage/active_storage.{c,h}`, `tests/storage/test_active_storage.c`
  (24/24 dev and ASan/UBSan; race case 8x stable on pthread barriers).
- Signed blob IDs, variation keys/digests (all 12 pinned digests), disk
  download/upload tokens with exact purpose separation; 5-state attachment
  machine; proxy 200/206/416 + multipart framing; disk OPTIONS/HEAD/304 and
  direct-upload 422/413 matrix; purge referenced-refuse/unreferenced-delete/
  missing-success. Token/variation/range/disposition vectors byte-exact
  against the pinned fixtures. Representation processing returns
  fail-loudly `CF_INTERNAL` (S03); variant-record SQL stays with actions.
- Acceptance: STORE-02/03 + DB-02 file halves. Live media-bytes parity
  BLOCKED (S03/pinned tools absent here) — not claimed.

### S03 media argv builders
- New: `src/storage/media.{c,h}`, `tests/storage/test_media.c` (21/21
  deterministic vectors dev and ASan/UBSan; live gate reports BLOCKED, never
  skip/pass).
- Fixed argv builders for vips/ffmpeg/ffprobe per the five pinned Rust
  modules; scalar-only user arguments; 4-slot bound; task-owned temp
  intermediates with kill/reap/cleanup; 16-byte stdout-cap and checksum
  vectors. Installed tools observed: vips absent, ffmpeg/ffprobe 9.0.2 vs
  pinned 7.1.5 — live byte parity BLOCKED by design.
- Acceptance: STORE-04 vectors pass; live bytes BLOCKED. Open adapter items:
  sharpen `conv` mask spelling vs pinned vips 8.16.1; Openslide has no CLI
  equivalent.

### I01 outbound HTTP, unfurl, webhooks
- New: `src/integrations/{http,unfurl,webhook}.{c,h}`,
  `tests/integrations/test_{unfurl,webhook}.c` (17/17 + 9/9, ASan/UBSan/LSan
  clean; 3 leak/over-read defects found and fixed via sanitizers).
- libcurl exchange with TLS verification never disabled; test-CA + RESOLVE
  origin for loopback servers; http/https-only enforcement. Unfurl honors
  the 16-slot/5s/10s/256-attr/5MiB/10-redirect contract; webhook honors
  7s/60s/100MB-decoded with exact JSON/signatures and no POST retry.
  Bodies byte-compared against `opengraph_expected.json`. Decoding is
  implemented over zlib (magic-based multi-member gzip) because the Fil-C
  libcurl is `--without-zlib`; integrator links vendored zlib-ng.
- Acceptance: INT-01/02 on the executed matrix; JOB-01/02 webhook side
  (no-retry verified, consumer registration left to integrator). Gaps: full
  90-case opengraph replay not executed; IPv6 pinning falls back to system
  resolution after guard approval.

### I02 Web Push
- New: `src/integrations/push.{c,h}`, `tests/integrations/test_push.c`
  (23/23 dev, ASan/UBSan/LSan, and Fil-C 0.685).
- OpenSSL EVP port of encryption/VAPID/pool: RFC 8291 vectors, JWT segments
  and `authorization_k` byte-exact vs `web_push_expected.json`; loopback
  wire case; deletion matrix (404/410/invalid-key destroy; 4xx/5xx/TLS
  preserve); 3KiB/256B valid-UTF-8 truncation; sanitized logs proven free
  of endpoint/key text; missing VAPID is an explicit error.
- Acceptance: INT-03 unit evidence. Real TLS delivery/timeout enforcement
  lives in I01's exchange helper (signature proposed); push-side queue
  wiring stays with D02/J02.

### First controller slice: A-pwa, A-rooms-refreshes, A-rooms-involvements
- New: `src/actions/pwa.c`, `src/actions/rooms/{refreshes,involvements}.c`,
  `tests/actions/{pwa,rooms_refreshes,rooms_involvements}_test.c`
  (8+13+18 = 39/39; neighbors `rooms_test` 27/27, `messages_test` 35/35
  unbroken).
- Manifest field order/escaping and verbatim service-worker bytes;
  refreshes `since` to_i/saturating-clamp with bare TURBO_STREAM bytes
  verified against Askama 0.14; involvements blank-to-nil incl. `[]`,
  writer update, change broadcast with previous, room-URL redirect.
- Acceptance: VIEW-04, AUTH-06. Gaps: update-from-`invisible` prepend path
  needs the sidebar `_shared` partial (404 after commit until A02 provides
  it); view/presenter symbols requested from the integrator as listed in
  the packet handoff; routes 91/93/94/95/149/150 binding pending.
## 2026-10-05 18:41 — B01b repair: memoized PBKDF2 derived keys (perf-profile finding 1)

- `auth_pbkdf2_sha256` now memoizes derived keys per full secret + salt +
  length in a bounded (32-entry), mutex-guarded table (the pin's
  ActiveSupport::CachingKeyGenerator semantics); cache miss, full cache and
  oversized keying material derive directly, so returned bytes never depend
  on cache state. No caller/API change, no Makefile change.
- Callgrind paired deltas (fresh baseline byte-identical to the profile's
  frozen build, 65d877ad…): PBKDF2 calls/req room hit 1.00 -> 0.00, sidebar
  1.00 -> 0.00, avatar 2.00 -> 0.00, room render 44.00 -> 0.00; Ir/req
  16.46 M -> 0.23 M (hit) and 744.9 M -> 30.7 M (render). Whole process:
  login + 13 renders 573 -> 3 derivations. Native rusage CPU/req: hit
  567 -> 31 us, render 23.7 -> 1.9 ms, avatar 985 -> 94 us.
- Guard tests: multi-secret isolation (63/64-byte shared prefix and
  prefix-length variants), 40-secret capacity overflow, oversized secret,
  token-level cross-secret verify, and a 4-thread churn case under TSan;
  falsified by a cache-off mutation (regresses to the profile's 1.00/44.00)
  and a forced collision mutation (guard tests fail). Evidence:
  docs/devel/evidence/pbkdf2-cache.md.
- Finding 2 (per-render avatar URL memo) not taken: call sites are outside
  src/auth and the marginal win after this fix is ~1-2 us/URL (render
  remainder 30.7 M Ir/req).

## 2026-10-05 17:31 — Phase 3: body cache and first pinned comparison (K01, B01.initial)

### K01 — complete-body cache with explicit keys/snapshot/version rules
- **K01a**: `cf_gzip` (deterministic level 6, mtime 0, exact byte vectors) and
  Accept-Encoding selection (q-values, wildcard, explicit q=0, 406 when both
  codings are forbidden) derived from the pinned app-layer deflater; 8+11
  focused cases, 89 select assertions, 12 falsification mutations
  (K01a.md).
- **K01b**: `src/cache.{c,h}` — fixed 4096-bucket chained table, one mutex +
  FIFO, keyed HMAC-SHA256 hash truncated to 64 bits, exact budget accounting
  (bucket array + key capacity + entry metadata + body allocation), 1 MiB
  entry / 2048-byte key caps, oversized bypass, lazy stale eviction,
  version-mutex admission, hit/miss/bypass/stale/eviction/version-reject/
  duplicate counters; 16+2 cases across clang/gcc/LTO/Fil-C/ASan/TSan with 12
  falsification mutations (K01b.md).
- **K01c**: key encoding for the spec's seven fields (versioned length+bytes,
  absence != empty), the context round API (lookup/304/406/gzip/Vary/
  admission), app lifecycle wiring, and call-site admission for all four
  admitted handlers (rooms#show, messages#index, users/sidebars#show,
  searches#index) after a per-handler body-input audit found no uncovered
  input; 26 falsification mutations; CACHE-01..06 correctness mapped
  (K01c.md). Documented: body-hash ETag/304/storage stay cache-scoped while
  encoding negotiation is always on (pin-derived); uncached admitted routes
  still lack the pin's weak body ETag and no Cache-Control is emitted.
- **Pulled-forward prerequisites** (ruled to make cache admission and the
  pinned benchmark preflight complete): A-users-sidebars (route 57, page +
  frame renders byte-identical to the Rust goldens with no masks),
  A-searches (145-147, incl. the pinned 771-range query sanitizer and scoped
  search), A-users-avatars show (53; token/404, bot, initials-SVG fallback,
  freshness/304/Cache-Control; the attachment-variant arm is a documented
  fail-loudly blocked arm pending S02/S03; route 54 stays deferred).
- **Two defects found by the pinned harness and fixed**: a ~41 ms keep-alive
  stall (Nagle holding the body send; fixed with TCP_NODELAY on accepted
  sockets — exactly the pin's own front server — plus a hyper-matching
  vectored flush) and gzip coupled to the cache flag (now always-on
  representation; storage/304 remain cache-scoped).

### B01.initial — pinned comparison harness and first measurements
- Harness pinned from `basecamp/once-campfire-elixir` @ `b6b82e50` (the
  README-medians commit) with a sha256 manifest + verifier; `bench/run-c`
  adapter (C app + Dockerfile) validated live; deterministic seed importer
  (schema + fixtures, hash recorded; avatar attachment omitted per the ruled
  fallback — disclosed). Reference images built and pinned:
  `campfire-rust:app` @ `1ea6d6f` (digest e21301de…), `campfire-reference:app`
  (Rails) @ `90b3300` (digest 3a498870…); both pass the pinned preflight.
- Two arms (CF_CACHE_BYTES 0 / 64 MiB), apps `c,rust,reference`, 3
  interleaved reps (order reversed per rep), HTTP suite, 1/16/64 clients,
  2 s warmup / 8 s measured; raw outputs + DIGEST/env/load accounting under
  `bench/results/7a4c24e877c1+landed42-{uncached,cache}/` (B01b.md).

| c=16 req/s (median of 3) | C off | C on | Rust | Rails |
|---|---:|---:|---:|---:|
| room_show | 144 | 6,744 | 28,225 | 223 |
| messages_page | 158 | 6,427 | 30,315 | 345 |
| sidebar | 774 | 7,045 | 32,619 | 606 |
| search | 431 | 7,054 | 30,992 | 389 |
| post_message | 3,223 | 3,763 | 43,735 | 950 |
| /up | 71,907 | 174,128 | 105,760 | 3,896 |

- **Post-profile fix (commit `837aa9c`)**: the PBKDF2 finding above was fixed
  and re-measured — see "B01b repair" for the fix details; the C-only A/B at
  loops=4 gives cached dynamic routes 7.3k -> 130-149k rps (room 17.9x,
  messages 19.6x, sidebar 20.4x, search 20.4x; avatar 42x, post 28x),
  uncached 163-3.4k -> 2.0k-24.9k, static/`/up` unchanged as controls;
  CPU/success room hit 535 -> 24 us, render 24.4 -> 1.9 ms. Cross-app
  (load-caveated, different runs): C's cached rows now lead the dynamic
  routes (room 130.6k vs Rust 28.3k, Rails 232), and uncached C is
  Rust-class on sidebar/post; the full room render remains the honest gap.
  A new ~130-160k rps ceiling (shared-listener accept path and/or generator
  capacity) is the next investigation; the quiet-window rerun will produce
  the publication-grade table.
- Cache delta on C: 44-54x (room), 40-44x (messages), 9-10x (sidebar), 16-17x
  (search); CPU/success ~25-31 ms -> ~0.55 ms. **All 18 reps are
  load-caveated**: host load1 at the gate was 6.0-11.1 (gate 1.5, 0 reps
  qualified) because the project owner's parallel Phase 4 build kept the host
  busy; relative comparisons are interleaved under the same load, but absolute
  numbers await the quiet-window rerun (two invocations, setup complete).
  Rust's row is its production image with its own server-side cache enabled
  (not togglable by the harness), so C-uncached vs Rails-uncached is the fair
  like-for-like and C-cache vs Rust is the each-at-its-best pair. C has no
  media/S02 features, the avatar row is the disclosed fallback SVG, the seed
  is a synthesis, and cable/upload/cold/slow/Fil-C rows are B01.final. No
  "fastest" claim: preliminary, provenance-complete dataset only.

## 2026-10-05 14:20 — Phase 2c: remaining controllers, review repairs, A01/A02 completion

### Controllers (routes rebound from the dev-501 placeholder)
- **A-rooms** (96/97/101/104): show/index/destroy with reference bodies; the
  destroy broadcast is byte-equal to the pinned `action_tag` + protocol +
  `json::encode` rule (HTML-entity escaping included) and ordered
  write → broadcast → redirect; Turbo-Frame page/frame variants.
- **A-messages** (76-83, 137, 138, 140-144): index/create/edit/show/update/
  destroy; canonical bodies through `cf_richtext_canonical_body`; ETag digests
  recomputed from the pinned preimages (insertion order matters, verified
  independently).
- **A-users-bans** (55/56): ban/unban with the DISCONNECT effects; the
  first_run leak and welcome integer-cast divergences from the Phase 2b
  verification are repaired.

### Phases 1-2 independent review repairs (INDEPENDENT-REVIEW.md)
- **P12-01** stale authorization: `rooms#destroy` and ban/unban revalidate the
  actor (status/role), room membership/ownership and the target inside the
  write transaction before the first mutation. Controlled-race tests reproduce
  the review's two scenarios pre-fix; independent verification 28/28 probes and
  falsification flips 18 red. (p12-01-stale-authz.md, p12-01-verify.md)
- **P12-02** WebSocket admission: upgrades now hold a lifetime connection lease
  (released exactly once via the existing abandon CAS; unreserved upgrades are
  refused) and each loop runs exactly one bounded reactor thread over epoll with
  one DB reader — no per-connection threads/readers. The review's one-slot probe
  goes from 4 sockets/4 extra threads to 14/14 with 1 thread per socket rework.
  **P12-02b conformance** then moved upgrade-auth/subscribe/model onto the
  bounded request-worker pool through the frozen `cf_app_submit_worker` seam
  (install via the C03 gate; full queue refuses/rejects), reduced revocations to
  one preallocated control slot per loop, added live output-budget coverage and
  documented the enforced stop order. Independent verification: strace
  attribution (all DB syscalls on worker/writer threads), injected 400 ms DB
  delays leaving other sockets ≤42 ms, exactly-once closures, 24-socket
  per-loop slot accounting, all falsifications red. (p12-02-*.md)
- **P12-03**: route tests read committed `tests/fixtures/contracts` copies
  (clean-checkout failure fixed).
- **P12-04** crypto queue: every `crypt_r` now runs on a refcounted bounded
  queue (`CF_CRYPTO_WORKERS` workers, 32 pending, counted blocking backpressure,
  drain/join shutdown with counted discards) at the bcrypt boundary, so
  sign-in, the unknown-account dummy and setup hashing all queue without a read
  transaction held. Independent verification measured the worker bound, the
  saturation semantics, shutdown and two-app refcounting, with falsifications.
  (p12-04-crypto-queue.md, p12-04-verify.md)
- **Turbo-Frame predicate**: the first Phase 2c repair misread
  `HeaderValue::to_str` (Unicode whitespace vs the pin's visible-ASCII+HTAB byte
  gate). All five action helpers now implement the exact predicate; adversarial
  re-verification over 9,119 vectors — including a Rust oracle executing the
  cached pinned http 1.5.0 crate — shows 0 mismatches, and the fuzz/dispatch
  probes are clean. (p2c-turbo-frame-ascii*.md)
- **Header-gate sweep** (defect-class ruling recorded in the roadmap): every
  request-header read the pin performs through `to_str` now treats any byte
  outside HTAB/visible-ASCII as absent — UA readability, CSRF Origin and
  Sec-Fetch-Site, Cookie drop-whole, Accept/Content-Type/X-Requested-With,
  If-None-Match/If-Modified-Since, assets Accept-Encoding, richtext
  `request_host` with the pin's localhost fallback, Content-Type HTAB, the
  HTTP/1.0 absent-Host base_url, and the Cable handshake headers. Version
  zero-segment ordering fixed (`1 == 1.0`; 352/352 corpus comparisons).
  Independent verification: 0 mismatches vs a Rust `to_str` oracle over 4,814
  vectors, wire probes per site, all falsifications red. Mapped error responses
  were confirmed to carry no X-Version/X-Rev per the pin. (a01-a02-completion-r2.md,
  header-gate-sweep*.md, cable-handshake-gate.md)

### A01/A02 completion (review completion gaps)
- useragent/ApplicationPlatform ported to `src/auth/{user_agent,platform}` with
  the pinned corpus copied to `tests/fixtures/ua` (385 cases × 16 fields, 0
  mismatches; hostile-input fuzz clean). `X-Version` is set on every dispatched
  response from the Makefile-baked `CF_APP_VERSION`; `X-Rev` only when a
  nonempty `CF_GIT_REVISION` define exists. `allow_browser` renders
  `sessions/incompatible_browser` (page/frame/own-layout exactly per the pin,
  200 text/html for any format, never 406); absent/blank/unreadable UAs are
  never blocked and get the pin's empty-parse facts. `cf_ctx_platform` feeds
  every layout call site; the declared-but-undefined `cf_auth_key_derive` is
  gone.

### Phase-exit acceptance and late findings
- **V01 two-browser acceptance** landed as runnable cases under
  `tests/integration/cases` (E2E-01 setup/sign-in/room/post/live delivery/
  edit-delete live; E2E-03 ban/revoke with a connected peer and a refused
  replay), driving the installed `agent-browser` CLI via the stdlib runner —
  no Chromium download. Both pass, stable across reruns. Search and
  profile-logout browser steps are deferred to M4/V02 with their packets
  (A-searches, A-users-profiles; both dev-501 by design), recorded as a ruled
  acceptance scope.
- **`_method` override was never applied before routing** (found by V01;
  UI edit/delete/logout forms mis-routed). `cf_ctx_create` now computes
  `cf_effective_method` first and carries the effective verb through routing,
  CSRF and actions on a shallow request copy; POST+`_method=HEAD` serializes
  byte-identical to wire HEAD via a narrow write-back at the H01 seam.
- **`src/cf.h` contract restoration** (pre-commit check): the completion pass
  had added an include and two `cf_ctx` fields to the frozen shared header;
  the platform now lives in A00's `private_state` with a context.h
  setter/getter, and `src/cf.h` is declaration-identical to `contracts/api.h`
  again (preprocessed-diff proof).

### Test harness
- TSan target now covers the actions and auth buckets plus the Cable live
  reactor tests (21 binaries / 258 cases when extended; suites grow with the
  repair cases).
- Cable live waits are deadline-polling with a falsifiable 20 s cap (was fixed
  3-5 s deadlines that expired under oversubscription); the remaining
  load-only races (24-socket two-loop accept spread, churn/hammer helper
  synchronization in test_cable_wiring.c) were made deterministic with
  bounded handshakes — falsified to still fail, never pass vacuously.

### Verification snapshot
- End-of-phase gate: dev 868/868, sanitize 868/868 (ASan/UBSan/LSan clean),
  Fil-C 868/868, TSan 287/287 (0 warnings), bench 868/868, V01 integration
  2/2. Evidence: docs/devel/IMPLEMENTATION-ROADMAP.md and docs/devel/evidence/.

## 2026-10-05 07:17 — Phase 2b: views, Cable channels/revocation, first controllers (A02, C02, C03, A-welcome, A-first_runs, A-sessions)

### Verification (independent verifiers; every finding repaired and re-verified)
- **A02**: rooms/messages verified CONFIRMED; the foundation verifier found cap
  arithmetic and error-path disposal defects (escaped-expansion bypass, over-cap
  underflow, a first_run leak) — fixed with falsifiable regressions and
  independently re-verified (1,081-check adversarial cap probe, 0 failures;
  pre-fix falsification produced up to 75 MB overshoot; 24/24 golden dumps
  byte-identical; leak sweeps clean on 74/66 injected failure ordinals).
- **C02/C03**: verification found a socket data race (30 TSan reports), a
  socket-lifetime use-after-free window, action-name trimming and channel-name
  mapping divergences, and C03's production control wiring unapplied. All
  repaired: TSan 0 reports, the UAF has a proven regression (ASan UAF with the
  fix disabled, ~1.15M barrier calls clean with it), naming follows `naming.rs`,
  and the revocation barrier is wired through the loop/socket/channel paths
  with the writer registration (68/70 cases in four modes).
- **Controllers**: A-sessions verified CONFIRMED (16/16 through the action path
  incl. goldens, logout disconnect barrier, rate-limit boundary). A-welcome's
  last-room integer cast was not the pinned `ruby_compat::integer_cast` — replaced
  with a faithful shared port (174-record differential, 0 mismatches) and the
  layout presenter's duplicate deduped. A-first_runs' JSON numeric fidelity gap
  was closed in the params layer: exact lexemes retained, `cf_param_to_s`
  independently verified against serde_json 1.0.151 + zmij 1.0.23 (14,871
  lexemes and 101,841 exact-bit doubles, 0 mismatches) and the controller arm
  switched.
- **R02 outcomes**: the unrenderable-vs-page-fail distinction was re-derived from
  the pinned source, ratified, and applied (`cf_richtext_to_plain_text_outcome`);
  the three-outcome classifier matches Rust on all 658 corpus bodies and the
  oracle diff stays 3948/3948.
- **Live smoke**: `/` → `/session/new` → `/first_run` matches the reference's
  auth-first chain; the 26 KB setup page renders through A02 views with R02
  content; assets serve; SIGTERM drains cleanly.

### Added
- **A02**: view-model conventions, presenters (rows loaded in one read
  transaction; renders do no SQL), and the layout/session/first-run/welcome/
  rooms/messages families with golden verification under the Rust runner's named
  masks only; 8 MiB output cap with exact escaped-expansion accounting.
- **C02**: the eight reference channels, per-loop subscription maps, bounded
  cross-loop fan-out with drop counters, broadcast payloads rendered through the
  A02 presenters (now 8 MiB-capped), production `/cable` mount wiring.
- **C03**: the revocation barrier wired end-to-end — loop control slots,
  owner-thread service, install gates with bounded resubmission, fresh-auth
  reconnect, and the writer's mandatory-consumer registration.
- **Controllers**: A-welcome (route 1), A-first_runs (routes 4, 8), A-sessions
  (routes 12, 17, 18) with `src/actions/actions.h` and rebinding from the
  development 501 (now exactly the 112 unlanded rows).
- **Build**: views/presenters/actions/Cable sources wired; a parity-version
  `layout.o` for golden-bucket binaries; `APP_VERSION` baked at build time;
  `cf_views_assets_configure` at boot; new actions/views/cable test buckets.

### Integration state
- Suite: **717 cf_test cases across 70 binaries** green in dev, bench, Fil-C,
  ASan/UBSan and TSan, plus the standalone core programs and CLI checks.

### Known gaps
- S02-dependent arms (avatar/attachment assignment; signed blob paths in
  presenters) stay disclosed-unimplemented until S02 lands; view platform facts
  await A01's UA parser; `cf_richtext_editable` keeps a ruled 400-vs-500
  difference on edit-page render failure; asset byte-ranges deferred to V02;
  the channel subscription-id cast is queued for the channels fidelity pass.

## 2026-10-05 04:26 — Phase 2a: rich text, Cable transport, jobs, storage (R02, C01, J01, S01)

### Verification (independent verifiers; every finding repaired and re-verified)
- **R02**: corpus replay 658 × 4 fields + 54 web_url with zero mismatches and zero
  security violations. The verifier's C-vs-Rust oracle diff (3948 lines) exposed a
  confirmed foreign-namespace defect class (rendered attachments dropped inside
  SVG/MathML) — fixed; two further adversarial parity gaps (MathML
  `annotation-xml` integration point; `<svg><title>` raw-text scanning) closed in
  follow-ups. Oracle now byte-identical in plain/ASan/Fil-C; 46 XSS bodies and 37
  extra injections byte-identical; 12/12 focused tests with a pre-fix negative
  control. Documented data-loss-only fail-safes remain (neutral-name literal
  collision; three conservative-miss scanner classes; script double-escape).
- **C01**: verified CONFIRMED (handshake/subprotocol matrix, framing adversarial
  suite, deflate no-context-takeover, 1 MiB assembled+inflated cap, 4 MiB/30 s
  pending limits, counters). Orphan-continuation close-code ordering fixed to
  `socket.rs` (1003/1009/1002; independent 148-check driver). The test harness's
  blocking-send flake is fixed (nonblocking + poll deadline; 36/36 Fil-C runs
  clean, pre-fix hang reproduced on a scratch copy).
- **J01**: verified CONFIRMED (128-slot per-kind FIFOs, CF_BUSY boundary before
  execution, per-kind isolation, exact counters at 127/128/129 and shutdown
  discards, no hidden durability, TSan clean).
- **S01**: verified CONFIRMED (path safety incl. symlink probes, 0600 unique
  staging, move-before-commit, checksum, argv-only subprocess with process-group
  kill/reap and output caps). The trailing-slash `O_NOFOLLOW` bypass is fixed
  (symlinked root refused with any trailing slashes; 127/127 independent probe).
- **Doubles retired**: model tests now exercise the production R02/A01 pipeline
  (228/228 in dev/ASan/Fil-C).
- **Integration**: the full stack links into the application; `cf_richtext_configure`
  runs at startup with the configured secret; an auxiliary-object depfile gap was
  fixed (it had produced stale-object failures across bench/Fil-C/TSan trees).

### Added
- **R02** (11 units): Gumbo DOM adapter at the reference's parser limits;
  sanitizer/filters/autolink/plain-text/editor pipeline in reference order;
  signed-mention and attachment resolution through A01 and the DB; immutable
  process-wide pipeline returned by `cf_tx_rich_text`.
- **C01**: WebSocket handshake (version/key/Origin policy/auth/subprotocol),
  framing (masking, fragmentation, control-frame interleave, UTF-8 validation),
  permessage-deflate without context takeover, 1 MiB assembled+inflated cap,
  4 MiB pending / 30 s stalled-write closes with separate counters, and the H01
  upgrade seam with clean connection detach.
- **J01**: per-kind bounded job queues, worker threads (media fixed at four),
  handler-registration API, exact per-kind accounting, discard-and-count shutdown.
- **S01**: storage key layout under STORAGE_PATH, 0600 staging with incremental
  checksum and move-before-commit, deletion rules, argv-only subprocess helper
  with timeouts, output caps and process-group kill.

### Integration state
- Suite: **543 cf_test cases across 58 binaries** green in dev, bench, Fil-C,
  ASan/UBSan and TSan, plus the standalone core programs and CLI checks.

### Rulings and known gaps
- Cross-module surfaces ratified (`richtext.h`, `cable.h`, `jobs.h`,
  `storage.h`); asset byte-ranges deferred to V02 completeness; C01's production
  mount wiring lands with C02, J01's start with J02, storage open with S02/H02;
  libvips remains F00-BLOCKED and the installed ffmpeg is 9.0.2 rather than the
  pinned 7.1.5 (S03 prerequisite).

## 2026-10-05 02:16 — Phase 1c: live server, auth, routes and assets (A00, A01, H03)

**Phase 1 exit met**: HTTP-01..08, DB-01..03/06 core paths and the measured
`/up` baseline are all satisfied; the server boots, serves, and drains cleanly.

### Verification (independent verifiers; all found defects fixed and re-verified)
- **A00**: the verifiers (three independent runs) reproduced a blocking defect —
  requests without an `Accept:` header segfaulted on an uninitialized span.
  Fixed; independently CONFIRMED with valgrind crash-before, raw-socket matrices
  in plain/ASan/Fil-C, a re-derived 28-case format probe, and a shadow-root test
  proving the asset front mount runs before dispatch. 47/47 app + 25/25
  routes/assets cases across four modes; 103k differential route cases clean.
- **H03**: 177 rows re-derived with zero drift, all 111 recognition vectors,
  built-ins and 41 reference-error responses verified; the four POST conductor
  rows now enforce CSRF semantics live (403/422 matrix); unmatched-route bodies
  are byte-equal to the reference `public/404.html`; dev-501 is exactly the 118
  unlanded implement rows.
- **A01**: two confirmed cookie divergences fixed with reference-exact bytes —
  logout/reset now delete `_campfire_session` (byte-compared deletion line,
  including the null-filter reset path) and identical session writes no longer
  re-encrypt/re-set cookies. Independent verification falsified by reverting the
  fix; `CF_NOMEM` propagation proven mechanically with `--wrap=malloc`; 45/45
  in plain/ASan/Fil-C. One claimed minor was refuted (signed-id underscore
  behavior), one confirmed unreachable.

### Added
- **A00**: `cf_ctx` context (params merge body→query→path, cookie queue +
  `cf_finish_cookies`, private format/flash state), dispatcher with the generic
  error mapping, CF_READERS worker pool with per-worker reader connections,
  process-wide CF_REQUEST_SLOTS admission accounting, per-loop budget division,
  `cf_writer_start` wiring, and the live serve path in `main.c`.
- **A01**: current-format cookies/signed IDs/PBKDF2 message verification via
  OpenSSL, session lifecycle (fresh session on login, hourly activity refresh,
  logout/ban/deactivation effects), the before-action chain in reference order,
  the CSRF safe/unsafe × HTTP/HTTPS × Origin × Fetch-Site matrix, and the fixed
  IP rate window (10 attempts / 180 s, bounded map).
- **H03**: the 177-row ordered route table with the finite matcher (literals,
  `:name`, `*name`, `(.:format)`, non-greedy globs, post-recognition percent
  decoding), built-in health/turbo_native/mailbox/conductor responses, the 41
  reference-error rows, and static asset serving from the pinned fixtures
  (digested paths, immutable caching, recorded Last-Modified, identity/gzip
  negotiation groundwork, traversal rejection).
- **Build**: Makefile wiring for context/routes/assets/auth (model-free subset),
  app/routes/assets/auth test buckets with their link recipes, TSan coverage for
  the threaded app cases, OpenSSL linked per mode, and section folding for the
  partial live link until R02 lands.
- **Measured `/up` baseline** (diagnostic, not the competitive benchmark):
  cold 0.114 ms; 56.6k req/s on one keep-alive connection (p50 16.5 µs,
  p95 23.6 µs, ~12 µs server CPU/response); 79.2k req/s on 8 connections;
  213 wire bytes; 80k+ requests all exact 200s; clean 0.06 s SIGTERM drain.

### Integration state
- Full suite: **469 cf_test cases across 45 binaries** plus the standalone core
  programs and CLI checks — green under dev, bench, Fil-C, ASan/UBSan and TSan.

### Known gaps / ruled deferrals
- A01 remaining: bounded crypto queue (required by 00-contracts), build-time
  X-Version/X-Rev defines, `allow_browser` with A02, 429/422 bodies with their
  packets, `cf_auth_key_derive` cleanup, AUTH-06 through controller packets.
- Asset byte-ranges deferred to V02 completeness; generic error bodies await
  A02 views; compression selection is K01; the rich-text bridge lands with R02.

## 2026-10-05 00:23 — Phase 1b: HTTP transport, writer, model families (H01, D02, D01)

Phase status: **H01 DONE**, **D02 PARTIAL** (A00 start wiring + C03/J01 consumers land
in Phase 1c), **D01 model bodies implemented** (PARTIAL only on the A01/R02
production boundaries), reference asset fixtures pinned and verified ahead of H03.

### Verification (independent verifiers; every defect found was repaired and re-verified)
- **H01**: 37 cases covering HTTP-01..05/08/09 + CORE-03/04 under
  plain/ASan/TSan/Fil-C. The verifier found bracketed IPv6 Hosts with an
  explicit port were always rejected 400; the fix was independently confirmed
  (93/93 adversarial matrix over 11 origin configurations, 43/43 suite in three
  modes, pre-fix bytes reproduce the failure).
- **D02**: writer independently CONFIRMED — rollback discards rows and queued
  events, version advance is post-commit and serialized, queue-full returns
  CF_BUSY before execution, best-effort drops are counted, and a mandatory
  DISCONNECT_USER without a registered consumer returns non-CF_OK and
  non-retryable after the commit (never a pretend rollback).
- **Models**: grouped verification found `cf_user_create`'s open-room grants
  binding the process clock instead of the reference's `STRFTIME(...,'NOW')`,
  plus push_subscription guard/output-clearing gaps. Repaired (reference SQL,
  canonical `src/models/types.c` replacing weak fallbacks, exact
  `Couldn't find <Model>` texts) and independently CONFIRMED — 226/226 under
  clang, ASan/UBSan and Fil-C, with a reverted-SQL run proving the test has
  teeth. A follow-up NULL-guard completion verified at 71/71 own probe cases
  (227/227 suite); the crash it uncovered for a malformed by-value endpoint
  shape ({ptr=NULL, len>0}) was reproduced (SIGSEGV / ASan / Fil-C) and fixed
  with the sibling-convention guard (228/228).
- **Assets**: a from-scratch reimplementation of the reference's Propshaft
  digesting reproduced all 315 digested names and all 134 compiled bodies
  byte-for-byte from a fresh clone of the pinned Rails commit.

### Added
- **HTTP/1.1 transport (H01)**: connection state machine with slot generations;
  framing validation (TE+CL, repeated/overflowed Content-Length, obs-fold, NUL,
  chunked with bounded trailers, Expect/417); limits (32 KiB headers, 100
  headers, 8 KiB target, 16 MiB body, 8 MiB pending output, 30 s deadlines);
  pipelining with ordered responses; short-write/EAGAIN/EINTR resume with
  immutable queued headers; 64 KiB worker-chunked file streaming; completion
  queue with generation+sequence stale checks; 5 s drain shutdown; counters.
- **Single writer (D02)**: `cf_write` with BEGIN IMMEDIATE, callback, commit or
  rollback; tx events (five kinds) with post-commit handoff; bounded queue;
  data-version advance through the app; registration points for C03/J01/I02.
- **15 model families (D01)**: account, active_storage, ban, boost, first_run,
  membership, message (page-of-40 algorithms, FTS rowid = message id),
  push_subscription, rich_text_record, room (Open/Closed/Direct), search,
  session (base58 token, hourly refresh), sound (static table), user (roles,
  statuses, email normalization, bot keys, bans), webhook — all against the
  frozen headers and the pinned source.
- **Asset fixtures for H03**: 331 files / 16.6 MB under `tests/fixtures/assets/`
  with a provenance manifest, digest mapping, importmaps and the embedded
  public files.
- **Build**: Makefile wiring for H01, D02, the model families and their test
  doubles; pinned per-mode libxcrypt headers (fixed a real `struct crypt_data`
  layout mismatch that Fil-C caught as a memory-safety violation); bundled
  picohttpparser compiled with upstream flags (never `-Werror` on upstream).
- **Test doubles**: BasicRichText-shaped pipeline and bcrypt verification under
  `tests/models/support/` (test-only, mirroring the Rust reference test
  support) so model tests link from committed files alone.

### Integration state
- Full suite: **358 cf_test cases across 30 binaries** plus four standalone core
  programs and the app config/lifecycle checks — green under dev, bench,
  Fil-C, ASan/UBSan and TSan.

### Known gaps
- D02 needs A00 to call `cf_writer_start` and C03/J01 to register consumers
  before it is DONE; the model rich-text bridge (`cf_tx_rich_text` + R02
  pipeline) lands with R02; the `/cable` upgrade seam is C01's prerequisite;
  F00 media probes remain BLOCKED for S03.

## 2026-10-04 23:26 — Independent review repairs (Phase 0 reopenings + Phase 1a findings)

All findings in `docs/devel/INDEPENDENT-REVIEW.md` are resolved and
independently re-verified; on that evidence **M0 (reproducible foundation) is
re-claimed DONE**.

- **P0-2 (High, reopened)** — probe recipes required gitignored sources.
  Committed probe sources (`vendor/probes/src/`), 20 build+probe scripts
  (`vendor/probes/scripts/`) and the curl loopback fixture. Verified from a
  fresh `git archive` checkout: all 20 probes (10 deps × clang/Fil-C) pass
  from committed inputs; pins, claims and the lock are untouched; normal
  builds never fetch; removing or mutating a probe source fails loudly.
- **P0-3 (Medium, reopened)** — fixture verification depended on ignored docs.
  Contract inputs committed under `tests/fixtures/contracts/`; the default
  verifier and schema regeneration now use committed copies only; verified
  from clean-checkout simulations with mutation teeth checks.
- **P1A-01 (High)** — multipart separator underflow (heap OOB read on
  adjacent boundary lines). Bounds fixed; the original 12-byte repro now
  rejects `CF_INVALID` with empty output under clang/ASan/Fil-C; 13,160
  truncation/substitution parses, a 55-case edge matrix and 180k random cases
  run clean; the pre-fix parser reproduces the abort.
- **P1A-02 (Medium)** — `make deps` ran Bash scripts via `sh`. The target now
  honors each script's interpreter (bash fallback); verified with the host
  `sh` as both bash and dash, including a failing script aborting the target.
- **Disclosure** — rebuilt dependency archives are not byte-identical to the
  original probe artifacts (archive timestamps, embedded paths, configured
  prefixes); the lock and probe records were deliberately left unchanged.
- **Follow-up recorded** — relink Fil-C libcurl against the Fil-C zlib-ng
  archive and check content decoding before accepting I01.

## 2026-10-04 22:36 — Phase 1a: foundation modules (F03, D01 core + headers, H02, R01)

Phase status: **F03 DONE, F01 DONE** (Fil-C axis closed), **D01 IN_PROGRESS**
(db-core + frozen headers done; model bodies next), **H02 PARTIAL** (multipart
awaits S01), **R01 DONE**. Milestone M0 was held open by the independent
review's clean-checkout reproducibility findings and was re-claimed after the
review repairs above; media probes remain open for S03.

### Verification (independent verifiers on every task; defects found and re-verified)
- Fil-C 0.685 drops custom section data, so `tests/cf_test.h` case registration
  failed under Fil-C → dual registration (section table on clang, constructor
  list under Fil-C, exactly-once execution) now enumerates identically under
  both compilers; orphan-case link failure preserved.
- A shutdown signal race in `src/main.c` (loss window between predicate check
  and `sigsuspend`) → replaced with `pthread_sigmask` + `sigwait` (no
  handler). The verifier injected signals into the exact old window and the
  repaired binary exited 0 in 40/40 dev and 32/32 TSan runs, while a
  reconstructed old-pattern binary hung — the test has teeth.
- H02 deep-merged body/query where the reference replaces whole top-level
  values (plus a depth-1 copy bug) → repaired; the verifier re-derived the
  rule with its own Rust reference driver: 39/39 merge shapes byte-identical,
  corpus re-derived exactly (1961 successes, 794 matched errors, exactly one
  spec-mandated depth divergence — the 99-bracket vector under the depth-32 C
  limit), 241/241 adversarial checks pass, reverts reproduce the defects.
- Suite: **86 registered cases** green under dev, bench, ASan/UBSan and Fil-C
  (core 4, config 15, app 6, db 3+7+5+3, params 20, views 27); the focused
  TSan target executes 21 registered cases plus the standalone buffer program,
  not all 86.

### Added — database core (D01)
- `src/db/schema.c` + embedded `schema_sql.h` (provenance SHA-256): fresh
  database executed transactionally with `user_version=1` from the exact
  contract DDL; open rules require version 1 and required tables.
- `src/db/reader.c` / `statements.c` / `db_internal.h`: WAL, NORMAL sync,
  FK ON, mmap 0, 1 s busy timeout, autocheckpoint 1000; per-connection
  statement sets with reset/clear discipline; FK/FTS5 verified; datetime
  text↔µs helpers at six fractional digits (reference-exact conditional
  fraction).

### Added — model headers (D01)
- Frozen headers for all 15 model families plus shared types (`cf_str`,
  `cf_optional_str`, UTC-µs datetimes, per-model record/vector/dispose,
  `cf_model_error`) covering all 232 inventory symbols (227 prototyped,
  5 deferred with reasons); naming and argument rules per the shared contract.

### Added — params and escaping (H02, R01)
- `src/http/params.*`: bounded bracket-notation tree (depth 32, 4096 nodes),
  form/JSON/multipart parsing, the reference shallow merge rule, the approved
  accessor subset, and method override with the fixed allowed list. Multipart
  uploads are rejected explicitly until S01 (partial, disclosed — no
  completion claimed).
- `src/views/escape.c`: reference-exact HTML text/attribute, JSON string and
  URL-component escaping plus trusted-HTML handling (VIEW-01).

### Added — build, config, lifecycle (F03)
- `Makefile`: dev/bench/filc/sanitize/tsan modes, dependency paths from
  `vendor/DEPS.json` (SQLite + yyjson, `-lm`), explicit source/test lists,
  `deps`/`clean`, per-binary failure aggregation, loud failures for missing
  modes/compilers.
- `src/config.*`: the full fixed environment table with checked parsing and
  failures naming the setting; secrets never logged and zeroed on destroy.
- `src/app.*` + `src/app_internal.h`: minimal app seed (config,
  `data_version=1` with the cache/version mutex always initialized, worker
  registry with join-before-free per CORE-05); `src/main.c` boot/shutdown
  lifecycle with race-free signal handling.

### Known gaps
- H02 multipart uploads await S01; D01 model bodies land in Phase 1b; F00
  media probes (libvips/ffmpeg) still BLOCKED for S03.

## 2026-10-04 21:51 — Phase 0: reproducible foundation (F00, F01, F02)

Phase status: **F00 PARTIAL, F01 PARTIAL, F02 DONE**. No milestone (M0) claimed.
Executed as a 13-agent swarm; every task independently verified by a separate
adversarial verifier (verdicts: F00 CONFIRMED, F01 CONFIRMED, F02 CONFIRMED).

### Verification
- F00: all 10 code dependencies built and probed under ordinary clang **and**
  Fil-C 0.685; every READY claim re-executed by an independent verifier;
  source pins, artifact hashes and licenses re-checked
  (`docs/devel/evidence/F00-verify.md`).
- F01: preprocessed declaration diff of `src/cf.h` vs `contracts/api.h` is
  byte-identical; CORE-01/CORE-02 pass plain and under ASan/UBSan; an
  independent adversarial refcount/freeze/overflow program passes
  (`docs/devel/evidence/F01-verify.md`).
- F02: every MANIFEST SHA-256 recomputed; runner fail-on-missing-prerequisite
  semantics exercised; three fixtures byte-compared against `tmp/rust-ref`
  (`docs/devel/evidence/F02-verify.md`, zero discrepancies).

### Added — dependencies (F00)
- `vendor/DEPS.json`: lock for picohttpparser (465a7ff0), SQLite 3.53.4
  amalgamation, zlib-ng 2.3.3, yyjson 0.13.0, OpenSSL 4.0.3 (no-asm),
  libcurl 8.22.0, libxcrypt 4.5.2, Nokogiri-gumbo subtree, qrcodegen 1.8.0,
  nghttp2 1.70.0 — per-compiler status, artifacts, probe commands, evidence.
- `vendor/scripts/*.sh`: idempotent pinned fetch/build recipes (`make deps`
  will consume them; ordinary builds never fetch).
- Fil-C 0.685 installed from its pinned prebuilt release to
  `~/.local/fil-c/0.685`; `optfil` needs an `/opt/fil` bind mount, recreated
  idempotently by `vendor/scripts/filc.sh`.
- `vendor/README.md`: pinning/verification rules, per-dependency status table,
  disclosed limitations.

### Added — core (F01)
- `src/cf.h` installed from `contracts/api.h` (declarations byte-identical,
  guard `CF_H`); `cf_err_name` added identically to both copies by the
  integrator (recorded in the roadmap contract-changes table).
- `src/core`: single-allocation atomically-refcounted `cf_buf`, checked-growth
  `cf_builder`, real clocks with test-only injection seams, getrandom with
  /dev/urandom fallback, stable error names.
- `tests/core`: CORE-01/CORE-02 plus clock/RNG/error tests; standalone clang
  runner for plain and ASan/UBSan builds.

### Added — fixtures and test infrastructure (F02)
- `tests/fixtures`: 196 pinned files (11,679,323 bytes) with per-file SHA-256
  MANIFEST (source repo+revision, source path, MIT license, case IDs); 22
  explicit OUT_OF_SCOPE D-C01 marks for legacy key/Marshal vectors; fixtures
  are self-contained (tests never read `tmp/` at runtime).
- `tests/cf_test.h`: minimal C test framework (registration, CF_CHECK/CF_REQUIRE,
  name filter, nonzero exit, no silent skips).
- `tests/seed`: 11 pinned seed recipe files (49,607 bytes) plus the
  fresh-database import contract for a future `make seed`.
- `tests/integration/run.py`: case-runner scaffold; missing prerequisites fail
  with their reason instead of skipping.

### Known gaps
- F00 media probes (libvips 8.16.1, ffmpeg 7.1.5) are BLOCKED — required
  before S03; no READY claim is made.
- Core code has no Fil-C test run yet; `make filc` arrives with F03.
- 27 copied fixtures are not in `contracts/reference-files.json` (not frozen
  when the spec was authored); their hashes are recorded in the local manifest.
- libcurl's Fil-C build is `--without-zlib` (disclosed); revisit only if a
  Fil-C artifact needs curl with zlib.

### Repo policy
- `docs/devel` stays local (existing gitignore policy). `build/`, `vendor/src/`,
  `vendor/build/`, `storage/` are gitignored working trees recreated by the
  pinned recipes; `vendor/scripts`, `vendor/probes`, `vendor/DEPS.json` and
  `vendor/README.md` are the committed dependency records.
