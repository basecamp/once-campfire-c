# Changelog

All notable changes to the Campfire C port are documented here. Commit
subjects stay short; this file carries the detail: task IDs, what landed,
acceptance evidence, and known gaps. Live status and full evidence links live
in `docs/devel/IMPLEMENTATION-ROADMAP.md` (local working document, not committed).

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
