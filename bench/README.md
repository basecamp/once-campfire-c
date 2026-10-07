# C benchmarks (B01)

Reproducible measurements of the C port on the pinned Elixir harness the
published README medians come from (`docs/devel/port-experiment.md`).  The
Rust harness under `tmp/rust-ref/bench/` stays a labeled diagnostic fallback;
it is not the primary and nothing here calls it.

## Layout

| Path | What it is |
| --- | --- |
| `PINNED-MANIFEST.json` | provenance: repo URL, commit, fetch method, license, per-file sha256/size/mode |
| `verify_pin.py` | recomputes the manifest hashes and refuses unknown files under `pinned/` |
| `pinned/once-campfire-elixir/` | the vendored harness, byte-identical to upstream (commit below) |
| `adapters/c-app.patch` | the only difference between the pinned harness and what runs; see `adapters/README.md` |
| `seed/import_seed.py` | imports the benchmark workload seed into a fresh C-schema database (stdlib only) |
| `check/check_bodies.py` | BENCH-01 body validation against a locally started server (pinned preflight checks) |
| `image/Dockerfile.c`, `image/entrypoint.sh` | production-image shape for the one-process C server |
| `image/Dockerfile.filc` | production-image shape for the Fil-C (memory-safe) build; the runtime-lib rule is implemented by `run-c --filc` |
| `run-c` | explicit invocation: builds, imports, patches a work copy, runs the harness |
| `results/` | raw outputs, one directory per recorded revision (see `results/README.md`) |

## Pinned harness

* Repo: <https://github.com/basecamp/once-campfire-elixir>
* Commit: `b6b82e50a653c4060136bb04e805eb78fd76ba10` — the commit whose README
  published the four-way Ruby/Elixir/Go/Rust medians (the current default
  branch, `f15fc9e`, changed only README.md; `bench/` is byte-identical at
  both, verified with `git diff --stat`).
* Fetch method (recorded, manual; no build step fetches anything):
  `git clone --filter=blob:none --no-checkout <url> && git archive b6b82e5 -- bench/... MIT-LICENSE parity/reference.env | tar -x`
* License: MIT (`pinned/once-campfire-elixir/MIT-LICENSE`).
* The upload suite's 505 KB JPEG is vendored from the Rails reference pinned
  by the Elixir repo's submodule: `basecamp/once-campfire`,
  `90b330024dec3e757c79b6a7e6568f93da8e3148`, `test/fixtures/files/black_hole.jpg`.
* The published results (2.5 MB, `bench/results/*` upstream) and the generated
  `bench/*.log` files are deliberately not vendored; the manifest lists them
  as external references by URL.

`python3 bench/verify_pin.py` must print `all hashes match`.  The harness is
data: no script here re-fetches or updates it.

## Prerequisites

`docker` (running), `mise` with `rust@1.98.1` (the toolchain the pinned run
script invokes), `python3`, `patch`, `sqlite3` (only for inspection), and a
C build (`make bench`; `run-c` builds it if missing).  The Fil-C arm
(`--filc`) additionally needs the pinned Fil-C 0.685 toolchain outside the
repository (`vendor/scripts/filc.sh`, idempotent) and its `make filc` build.

## Explicit invocation

```sh
# 1. import the workload seed (deterministic; refuses to overwrite)
python3 bench/seed/import_seed.py --out bench/seed/.seed/default

# 2. validate the response bodies / seed before measuring (BENCH-01)
python3 bench/check/check_bodies.py --seed bench/seed/.seed/default \
  --pinned-validate --out bench/results/<revision>/preflight.json

# 3. measure (2 reps, cache disabled then enabled, etc. -- see 06-cache-performance.md)
C_IMAGE=campfire-c:bench bench/run-c --reps 2                 # -> bench/results/<revision>/
bench/run-c --apps c --reps 2 --out bench/results/<revision>-cache --build-image
```

`run-c` accepts every `bench/run` option and passes it through (`--apps`,
`--reps`, `--out`, `--user-agent`); it adds `--apps c` and
`--out bench/results/<candidate-revision>/` unless given.  It then applies
`adapters/c-app.patch` to a fresh copy under `bench/.work/pinned/` and execs
the pinned `bench/run`.  The Rust/Go/Elixir apps stay runnable unchanged if
their images and checkouts exist.

### Cache ablation arms (06: uncached -> whole-body cache)

`--cache-bytes N` sets the C complete-body cache budget for the run (the
adapter forwards `-e CF_CACHE_BYTES=N` to the `c` container; the entrypoint
defaults it to the C config default, 0).  The value is written into
`env.txt` (`c revision: ... cache_bytes: N ...`), so every result row states
which arm it is.  A flag is used rather than a second image tag because the
budget is runtime configuration, not a build input: one image digest keeps
both arms attributable to the same binary.

```sh
# uncached arm (default; identical to CF_CACHE_BYTES=0)
bench/run-c --loops 4 --reps 3 --out bench/results/<revision>-uncached

# cache-enabled arm (64 MiB, 06's benchmark budget)
bench/run-c --cache-bytes 67108864 --loops 4 --reps 3 \
  --out bench/results/<revision>-cache
```

`--loops 4` matches the four hardware threads the harness allocates per app
(the references use them; the C config default is 1).  The B01b diagnostic
(`docs/devel/evidence/B01b.md` §9) measured only 1.05–1.09× median change
from loops=1 to loops=4 — dynamic routes are CPU-bound in the 4-CPU
allocation — but 4 is the methodology-parity setting for published rows.

Both arms use the same seed, image, CPUs and workload flags; the only
difference is the budget.  Interleave them in separate invocations as 06
requires (at least three interleaved pairs, alternating order).

B01b-prep validation (2026-10-05) found two issues the integrator must
settle before headline rows: a ~41 ms per-response stall on keep-alive
connections (affected routes in both arms, capping per-connection throughput
near 390 rps), and gzip negotiation being active only in the cache-enabled
arm.  Details, raw numbers and reproduction commands:
`docs/devel/evidence/B01b-prep.md`.

### Fil-C arm (memory-safe build of the same sources)

`--filc` adds a second C app, `c-filc`, built with the pinned Fil-C 0.685
toolchain (`make filc`), so the two build flavors of identical sources
interleave in one invocation; with no `--apps` it runs `--apps c,c-filc`:

```sh
# four-way published set: both C flavors, Rust and Rails, one interleaved run
bench/run-c --filc --loops 4 --apps c,c-filc,rust,reference --reps 3 \
  --cache-bytes 67108864 --out bench/results/<revision>-quad-cache
```

The Fil-C binary's ELF interpreter and RUNPATH are absolute paths into the
pinned pizfix distribution, so `run-c --filc` recreates that directory tree
under `bench/.work/filc-image/rootfs/` and `image/Dockerfile.filc` copies it
to `/`; the unmodified binary (the sha256 `env.txt` records) runs with its
own loader, libc and runtime in the container.  `env.txt` gains a
`c-filc revision: ... binary sha256: ...` line identical in shape to the
`c` line, and `bench/report` labels the column `C (Fil-C)`.

### Results tooling

`bench/summarize.py` turns result directories into per-route median [min–max]
tables (markdown, `--json` digest) and, with `--cache-off`/`--cache-on`, the C
cache delta.  Invalid reps (errors, non-200) are excluded and reported, never
averaged.  The pinned load generator records p50/p90/p99 (no p95).

B01b's first dataset (HTTP suite only, 3 interleaved reps, cache off vs on,
c/rust/reference) is in `bench/results/7a4c24e877c1+landed42-{uncached,cache}/`
with the analysis in `docs/devel/evidence/B01b.md`; every rep is
load-caveated (host never met the quiet gate) and a quiet rerun is just the
two invocations in that document.

### Comparison arms (Rails, Rust)

The pinned production images are built and validated (digests, build logs and
preflight evidence in `docs/devel/evidence/B01b-images.md`):

```sh
export SEED=$PWD/bench/seed/.seed/default
export RUST_IMAGE=campfire-rust:app REFERENCE_IMAGE=campfire-reference:app
export RUST_ROOT=$PWD/bench/.work/rust-ref
bench/run-c --apps rust,reference --reps 3 --out bench/results/<revision>-compare
```

Both images boot from the same seed through the harness's per-rep volume
mounts and pass the pinned preflight.  `bench/check/check_served.py` runs
that preflight against any already-running server.

Target and configuration, as the pinned harness sets them (`--network host`):

| Knob | Default | Meaning |
| --- | --- | --- |
| `PORT` | 47130 | public listener; `bench/run` passes `HTTP_PORT=$PORT` and `TARGET_PORT=$PORT+1` |
| `SEED` | `bench/seed/.seed/default` | `{db/production.sqlite3, storage/, labels.json}` |
| `C_IMAGE` | `campfire-c:bench` | image built from `bench/image/Dockerfile.c` |
| `SERVER_CPUS` / `LOADGEN_CPUS` | `8-11` / `12-15` | `taskset` sets; keep generator CPUs separate |
| `HTTP_SECS` / `HTTP_CONCS` | `8` / `1 16 64` | per-route closed-loop load; a 2 s warmup at conc 4 runs first |
| `CABLE_CLIENTS` / `CABLE_TPUT_SECS` / `CABLE_POSTERS` | `100 500 1000` / `15` / `4` | Action Cable fan-out |
| `UPLOAD_REPS` | `5` | 505 KB JPEG through to its thumbnail |
| `SUITES` | `http cable upload` | select a subset |
| `CF_CACHE_BYTES` | build default | set through the image environment for cache on/off pairs |

The container entrypoint maps `HTTP_PORT` to the C `PORT`, sets
`PUBLIC_ORIGIN=http://127.0.0.1:$HTTP_PORT`, mounts the per-rep seed copy at
`/rails/storage/{db,files}` and forces `DISABLE_SSL=1` (the pinned
reference.env carries the Rails `true`, which the C parser rejects; TLS is out
of scope, D-C06).

Workload definitions are the pinned ones: eight signed-in HTTP workloads
(`room_show`, `messages_page?before=<messages.busy_060>`, `sidebar`, `search?
q=coffee`, `avatar`, `static_css`, `/up`, `post_message`), Cable fan-out, and
the upload.  The load generator is `bench/loadgen` from the pinned commit.

## Outputs

Raw outputs land in `bench/results/<revision>/`: `c-<rep>.json` (per-rep
throughput/latency/statuses/CPU per success/memory), `.c-<rep>-cable*.jsonl`
and `.mem` samples, `*-validation.json` (preflight response evidence:
statuses, sizes, body hashes), `*-jobs.json`, `env.txt` (seed sha256, image
ids, CPU sets, flags, revision), `uptime.log`, and `report.md` from the pinned
`bench/report`.  `results/README.md` records the naming convention; keep
`env.txt` with every directory.

## Preflight (BENCH-01)

The pinned `bench/validate.py` preflight is the harness's own validation and
runs for every rep, before any timing.  It rejects:

* redirects and errors — every route must return exactly 200;
* empty bodies — after gunzip, the body must be non-empty;
* unpopulated room/search — `room_show`, `messages_page` and `search` must
  contain `data-message-id="<n>"`, and the busy-room database must hold more
  than 50 messages with `PRAGMA integrity_check = ok`;
* additionally: the sidebar contains `shared_rooms` and the room id; the
  avatar is an `image/*` body over 100 bytes; the CSS body contains `{`; `/up`
  contains `background-color: green`.

`bench/check/check_bodies.py` performs the same checks against a locally
started server (same paths, same assertions) and records per-route evidence;
`--pinned-validate` also executes the pinned preflight itself.  The C routes
that have not landed yet are reported as `blocked` (dev-only 501), not as
passes.

## Seed importer

`bench/seed/import_seed.py` synthesizes the published workload seed's
benchmark-relevant content into the C schema (`tests/fixtures/contracts/schema.sql`)
and writes the harness's `labels.json`.  The upstream seed is built by the
Ruby recipes vendored in `tests/seed/` and its prebuilt
`parity/.seed/default` is not committed, so the importer derives from those
recipes instead of running them (no Ruby in this repository).  What the
benchmark needs, and where it comes from:

| Need | Imported |
| --- | --- |
| sign-in user | `users.david` (`david@37signals.com`) with the pinned bcrypt digest for `passwords.all` ("secret123456", tests/auth/test_password.c) |
| busy room | `rooms.watercooler`, 131 messages incl. `messages.busy_060` in the middle for paging |
| write room | `rooms.hq` (POST and upload target) |
| searchable content | messages containing "coffee" in the busy room and designers, indexed into `message_search_index` (plain text, as the models do) |
| memberships | David in every room (involvement `everything`/`nothing`/`mentions` as `default.rb` sets), other users in the shared rooms |
| avatar | `avatar_tokens.jason` — a real Active Record signed id (PBKDF2/HMAC, `user/avatar`), with **no attachment** (deliberate divergence below) |
| sessions | not seeded; the harness's login workload creates them |
| stamp | `clock.now` = `2026-03-02T16:00:00Z`, the pinned seed clock; all timestamps explicit |

The seed is content-stable across runs; `import_seed.py --json` prints the
database sha256 that `env.txt` also records.  It is a benchmark-equivalent
synthesis, not a byte-copy of `parity/.seed/default`; the two are not claimed
to be identical, and the seed hash is part of every result directory.

### Disclosed divergence: jason's avatar

Per the integrator ruling recorded in `PINNED-MANIFEST.json` (`seed`), the
seed leaves jason without an avatar attachment so the fallback avatar path is
exercised.  `/users/<token>/avatar` therefore serves a generated initials SVG
(`image/svg+xml`, >100 B) instead of the reference's processed webp variant.
The `avatar_tokens.jason` label and its signature format are unchanged, so the
pinned preflight still addresses the route.  `bench/run-c` writes
`DIVERGENCE.md` into every result directory with the statement and the seed
hash; avatar rows must be read with that representation difference in mind.

## Status (B01a)

Validated on the current checkout: pin hashes, importer determinism, the
container app start (cold start, `/up`, login, room page), the pinned load
generator (login/scrape/http/post/cable), and — since 2026-10-05 — the full
pinned preflight (`check_bodies --pinned-validate`: all seven routes 200, DB
checks pass, pinned `validate.py` exit 0).  The avatar route now serves the
seeded fallback (630-byte `image/svg+xml` initials), confirming the disclosed
divergence's expected body.  The upload suite still requires S02/S03 media;
select `SUITES="http cable"` until then.  Details and raw evidence:
`docs/devel/evidence/B01a.md` and `docs/devel/evidence/B01b-prep.md`.

## Persistent write checks

The seed importer now uses explicit `(user_id, room_id, involvement)` tuples
for existing rooms and users. Run `python3 bench/seed/test_import_seed.py`
for membership and determinism checks. Run
`python3 bench/check/test_validate_writes.py` to check rejection of false POST
successes, missing bodies and missing search entries. Rebuild existing seeds after this fix.

The C adapter runs `bench/check/validate_writes.py` for every app: a preflight
POST must return a Turbo Stream containing the submitted text and persist
both the message and its rich-text body. The warmup and every measured POST
run must create exactly one new database message per successful response.
These checks run outside the timed load-generator interval. A nonempty
HTTP 200 error page can no longer pass the write benchmark.

Historical POST results made with the old importer are invalid: the posting
user was not a member of HQ, and HTTP 200 error pages were counted as writes.
Rebuild the seed and rerun rather than reusing those figures. The reporting
tools reject POST rows without a verified persistent-message count.
