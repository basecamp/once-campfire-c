# Changelog

All notable changes to the Campfire C port are documented here. Commit
subjects stay short; this file carries the detail: task IDs, what landed,
acceptance evidence, and known gaps. Live status and full evidence links live
in `docs/devel/IMPLEMENTATION-ROADMAP.md` (local working document, not committed).

## 2026-10-04 — Phase 1a: foundation modules (F03, D01 core + headers, H02, R01)

Phase status: **F03 DONE, F01 DONE** (Fil-C axis closed), **D01 IN_PROGRESS**
(db-core + frozen headers done; model bodies next), **H02 PARTIAL** (multipart
awaits S01), **R01 DONE**. Milestone **M0 (reproducible foundation) is DONE**
for the core dependency set; media probes remain open for S03.

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
- Suite: **86 cf_test cases** green under dev, bench, ASan/UBSan, TSan and
  Fil-C (core 4, config 15, app 6, db 3+7+5+3, params 20, views 27).

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

## 2026-10-04 — Phase 0: reproducible foundation (F00, F01, F02)

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
