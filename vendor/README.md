# Vendored dependencies — F00 lock and probe records

`vendor/DEPS.json` is the authoritative dependency lock, assembled by the F00
dependency-lock task from the F00 probe records in `vendor/probes/`. This README
is its human-readable companion. Both files are integrator-owned shared files
(`docs/devel/implementation/08-task-cards.md`: dependency lock/recipes are
shared; workers must not edit them). Every value in `DEPS.json` is copied from
`vendor/probes/*.json`; gaps are recorded as `BLOCKED`, never filled by
inference, and discrepancies are listed instead of reconciled silently.

Repo policy: `vendor/src/` and `vendor/build/` are gitignored working trees,
recreated on demand by the pinned fetch scripts in `vendor/scripts/` through the
explicit `make deps` step — the committed records are `vendor/scripts/`,
`vendor/probes/`, `vendor/DEPS.json` and this README. The Fil-C toolchain lives
outside the repository at `~/.local/fil-c/0.685` (its `/opt/fil` bind mount,
required by optfil, is runtime state recreated by `vendor/scripts/filc.sh`). All
READY claims below were independently re-executed and confirmed per
`docs/devel/evidence/F00-verify.md`.

Status at a glance: all ten fetched dependencies built and probed under both
compilers (READY/READY) with the disclosures in section 5; the two media
subprocess pins have no probe record yet and are BLOCKED in the lock.

## 1. How `make deps` consumes these scripts

The build contract is fixed by `docs/devel/implementation/01-foundation-http.md`
(F00): "No automatic fetch during `make`; `make deps` is explicit." The Makefile
itself is F03-owned and does not exist yet; this lock does not define new make
targets. What it does fix for the consumer:

- Ordinary `make` targets must not fetch (no network access at build time;
  `01-foundation-http.md` F00). The explicit `make deps` step is the intended
  caller of the `vendor/scripts/*.sh` fetch/install scripts, one per pin:
  - `vendor/scripts/<name>.sh` (curl, gumbo, libxcrypt, nghttp2, openssl,
    picohttpparser, qrcodegen, yyjson, zlib-ng) shallow-fetches exactly the
    pinned commit into `vendor/src/<name>`, verifies `git rev-parse HEAD` equals
    the pin, and refuses to overwrite a non-matching checkout. A second run
    prints "already fetched at pinned commit" and exits 0.
  - `vendor/scripts/sqlite.sh` downloads the pinned 3.53.4 amalgamation archive,
    verifies its published SHA3-256, extracts it under `vendor/src/sqlite/`, and
    skips when already present.
  - `vendor/scripts/filc.sh` installs and verifies the pinned Fil-C 0.685
    toolchain (size+sha256 checked downloads, hello-world compile/run with both
    distributions) and re-establishes the `/opt/fil` bind mount; safe to re-run
    after a reboot.
- Dependency builds use the recorded F00 recipes: each entry's `build_command`
  per compiler writes to `vendor/build/<name>-clang/` and
  `vendor/build/<name>-filc/`. Nothing is built in place under `vendor/src/`
  (both F00 halves left those trees clean).
- Downstream targets link the recorded artifacts (for example the install
  prefixes `vendor/build/openssl-clang/install`, `vendor/build/curl-clang/install`,
  `vendor/build/openssl-filc/install`, `vendor/build/curl-filc/install`, and the
  static archives listed per dependency in `DEPS.json`) instead of re-fetching or
  silently rebuilding.
- Fil-C application builds must use the pizfix compiler from section 2 and must
  never link an ordinary-C object; `make filc` compiles the same application
  sources against the `*-filc` archives
  (`docs/devel/c-port-arch.md`, "Source and build").

## 2. Fil-C install path and exact version

- Version: **Fil-C 0.685** (`dependencies.json` `filc` entry; GitHub release tag
  `v0.685`). No other revision is authorized.
- Install root: `/home/msaraiva/.local/fil-c/0.685`
  (`vendor/probes/filc.json` `install_path`; integrator decision 4 in the
  roadmap contract log).
- Compiler used by every `-filc` dependency build and probe: the pizfix
  distribution's
  `/home/msaraiva/.local/fil-c/0.685/filc-0.685-linux-x86_64/build/bin/filcc`
  (a symlink to `build/bin/clang-20`, the same binary as `build/bin/clang`):
  `clang version 20.1.8 (Fil-C 0.685 git@github.com:pizlonator/llvm-project-deluge.git bb0d0a64eed297ab8e171002033208fb08ad9941)`.
  It needs no environment variables; the driver locates its slice relative to
  the compiler binary.
- Optfil (same 0.685 revision) is installed under the same root and presented
  as `/opt/fil` through a bind mount (`/opt/fil/bin/filcc`). The mount is
  runtime state that does not survive a reboot and is re-created by
  `vendor/scripts/filc.sh` (documented deviation, `docs/devel/evidence/F00-filc.md`
  section 4; the vendor-canonical alternative is `sudo ./setup.sh -u`). Optfil
  was verified (hello-world exit 0) but is not used by any dependency probe; all
  `-filc` probes use pizfix.
- Release assets and digests (re-verified by `filc.sh` on every run):
  - `filc-0.685-linux-x86_64.tar.xz` — 77045600 bytes, sha256
    `d12bd30c33f18179a9355b32ea44ba61dcc0342c7d77d1ac2548852e64994727`
  - `optfil-0.685-linux-x86_64.tar.xz` — 274092224 bytes, sha256
    `f7d2d73b17ee9bfff0859ca15b3ca9c10e8501f8d891b0dfa961e81f0390e788`
- In `DEPS.json` the `filc` entry's `clang` cell records the pizfix hello-world
  probe and its `filc` cell records the optfil one (mapping documented in the
  entry's `notes`).

## 3. Pinning and verification rules

- Git dependencies are pinned by commit in
  `docs/devel/implementation/contracts/dependencies.json`. Each fetch script
  re-verifies `git rev-parse HEAD` equals the pin; every `checked_out_commit`
  equals its `pinned_commit`, and `docs/devel/evidence/F00-verify.md`
  independently re-checked the comparison for all nine git pins.
- Git probe records carry `archive_sha256: null` by design: they are commit
  pins, not archives. Only SQLite (an archive) has a source hash: the published
  SHA3-256 of `sqlite-amalgamation-3530400.zip` (verified against
  sqlite.org/download.html) plus the locally computed SHA-256
  `1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d`. The
  Fil-C toolchain is pinned by release-asset size+sha256 (section 2).
- Probe sources are byte-identical across the two compiler halves of every
  dependency (`probe_source_sha256 == clang_probe_sha256` in each `-filc.json`).
  No assertion was weakened, no upstream source was patched, and no ordinary-C
  object is linked into a Fil-C probe. F00-verify re-executed all 20 dependency
  probes, matched all 48 artifact size+sha256 pairs, and re-checked pins and
  licenses.
- Artifact sizes/sha256 are the bytes on disk at record time. Archive bytes may
  differ on a rebuild (`ar` timestamps), so a hash mismatch after a voluntary
  rebuild is not by itself a contradiction; the recorded `build_command` is the
  reproduction recipe.
- `vendor/DEPS.json` must not be hand-edited: new probe evidence is required
  first. Missing probe data is `BLOCKED`, and every disclosed discrepancy is
  listed in the affected entry's `notes` (section 5).

## 4. Per-dependency per-compiler status

| Dependency (pin) | clang | Fil-C | Notes |
| --- | --- | --- | --- |
| picohttpparser (`465a7ff0…`) | READY | READY | F00 core; releases F01/H01/D01 |
| SQLite 3.53.4 (amalgamation) | READY | READY | FTS5 + threads; consumers must link `-lm` |
| zlib-ng 2.3.3 (`12731092…`) | READY | READY | F00 core; `*-filc/libz.a` is the curl relink input |
| yyjson 0.13.0 (`64475360…`) | READY | READY | F00 core; strict reader/writer |
| OpenSSL 4.0.3 (`af1775b6…`) | READY | READY | no-asm/no-shared in both; filc JSON metadata gap repaired (F00-verify D1) |
| libcurl 8.22.0 (`01346829…`) | READY | READY (1) | (1) Fil-C build is `--without-zlib`; disclosed, relink proposed |
| libxcrypt 4.5.2 (`db70b42b…`) | READY | READY | upstream `--disable-werror` switch, no source patch |
| gumbo (Nokogiri v1.19.4, `8cfb9daa…`) | READY | READY | sparse subtree only, no Ruby runtime |
| qrcodegen 1.8.0 (`720f62bd…`) | READY | READY | only `c/qrcodegen.c` compiled |
| nghttp2 1.70.0 (`85e300c7…`) | READY | READY | library only (no tools/server) |
| Fil-C 0.685 toolchain | READY (pizfix) | READY (optfil) | cell mapping documented in `DEPS.json` (`filc` entry) |
| libvips 8.16.1 | BLOCKED | BLOCKED | (2) no probe record; S03 prerequisite open |
| ffmpeg 7.1.5 | BLOCKED | BLOCKED | (2) no probe record; S03 prerequisite open |

(2) Per D-C05 these are subprocess dependencies (vips/ffmpeg/ffprobe), not
libraries linked into the application; they were never fetched or probed
(F00-verify discrepancy 8), so both cells are BLOCKED rather than UNTESTED —
missing probe data may not be presented as a passing state.

## 5. Recorded disclosures and open integrator questions

Details live in the `DEPS.json` entry `notes` and in the F00 evidence files.

1. libcurl Fil-C `--without-zlib` (feature difference vs the clang build): the
   Fil-C probe omits the zlib component because linking ordinary-C system libz
   into Fil-C is forbidden and no Fil-C zlib-ng archive existed at build time.
   Required checks pass. Proposed change (curl-filc.json; F00-filc-auxa.md
   section 6): now that `vendor/build/zlib-ng-filc/libz.a` exists, re-run
   configure with `--with-zlib=<zlib-ng-filc prefix>` and relink. Until then,
   Fil-C consumers must not rely on libcurl content decoding. Also disclosed:
   the first Fil-C probe attempt failed on a local-server readiness race; the
   recorded passing re-run is exit 0. (F00-verify D7)
2. (Repaired 2026-10-04) `openssl-filc.json` / `curl-filc.json` previously
   omitted `repo`/`pinned_commit`/`checked_out_commit`/`license`/`license_file`
   and no git-sourced probe record carried `source_identity`. The pin/commit
   values were copied from the clang-half JSONs after `git rev-parse HEAD`
   reproduced them, `license`/`license_file` point at the files present in
   `vendor/src/<name>/`, and `source_identity` is now recorded in the six
   git-sourced probe records edited for this repair (`openssl.json`,
   `openssl-filc.json`, `curl.json`, `curl-filc.json`, `libxcrypt.json`,
   `gumbo.json`); git-sourced records owned by other workers are unchanged and
   `source_identity` for those pins is derived from their recorded
   `pinned_commit`. `repo`/`fetch_script` remain carried from the clang-half
   records in `DEPS.json`. (F00-verify D1)
3. `openssl-filc.json` quotes a `configdata.pm` no-asm line that is not present
   verbatim in its log; F00-verify re-ran `configdata.pm` and confirmed
   `OPENSSL_NO_ASM`. (F00-verify D3)
4. `curl-filc.json` `probe_log_tail` stops before the log's final curl-tool
   sanity section; shape difference only, both runs are disclosed in the log.
   (F00-verify D4)
5. (Repaired 2026-10-04) The gumbo license note previously cited Nokogiri
   `LICENSE-DEPENDENCIES.md`, which is intentionally outside the sparse
   checkout; `license_file` now points at
   `vendor/src/gumbo/gumbo-parser/src/README.md`, which exists in the subtree
   and states the fork is used under the terms of the Apache 2.0 license, with
   Apache-2.0 notices also in the source-file headers and no standalone COPYING
   in the subtree. The SPDX statement (Apache-2.0) is unchanged. (F00-verify D5)
6. Git probe records have `archive_sha256: null` (commit pins, not archives);
   only SQLite has a source-archive hash. (F00-verify D6)
7. Media pins libvips 8.16.1 / ffmpeg 7.1.5 have no probe record anywhere, so
   their cells are BLOCKED and S03's "F00 media" prerequisite remains open.
   (F00-verify D8)
8. Disk figures disagree between the shared-rule guard and the measurements;
   both are stated in section 6 rather than reconciled.

## 6. Disk caveat

- The shared workspace rules state disk is tight (~7 GB free on `/home`) and
  require checking `df -h` before large operations. Measured while assembling
  this lock: `df -h /home` reports **651 GiB available** (1.8 TiB filesystem,
  61% used); F00 evidence recorded 512–654 GiB. The discrepancy is recorded,
  not reconciled: keep treating the conservative figure as the guard for large
  operations.
- Measured usage: `vendor/` ≈ 9.7 GiB total (`vendor/src` 300 MiB;
  `vendor/build` 9.4 GiB, dominated by `openssl-filc` 7.8 GiB, `openssl-clang`
  1.4 GiB and `curl-filc` 181 MiB). The Fil-C toolchain install is ≈ 2.2 GiB
  outside the repo; the release tarballs under `/tmp` are 77,045,600 +
  274,092,224 bytes.
- Rules: build only inside `vendor/build/`; do not duplicate source trees;
  check `df -h` before large operations; never delete files you did not create
  (other workers share this checkout).
