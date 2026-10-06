# C adapter patch for the pinned harness

`c-app.patch` is the complete, reviewable difference between the vendored
pinned harness (`bench/pinned/once-campfire-elixir/`) and the copy
`bench/run-c` actually executes.  It is applied with `patch -p1` to a fresh
copy under `bench/.work/pinned/`; the vendored tree is never modified, so
`bench/verify_pin.py` keeps checking it against
`bench/PINNED-MANIFEST.json`.

Post-patch SHA-256 (verify after regenerating the patch):

| File | sha256 |
| --- | --- |
| `bench/run` | `7009f7b6b3e1d6ed4e769c0bf23e311b48add163ee09df31aaef512ae2e4ceba` |
| `bench/validate.py` | `cd9b4a262d92ab3a125e6b2d60629693ec504f23cd6355f27f45c1eda758c0d5` |
| `bench/report` | `294ea697b6a962b9fe454cfad55f90842dfcfe9f2dfaff41fa6ae7825db88d44` |

The patch changes no load generator, no workload, no timing constant, and no
preflight assertion.  Its parts:

1. **`bench/run`** adds the `c` app (and the `c-filc` Fil-C build flavor as a
   second app of the same source — same image/env plumbing, own
   `C_FILC_IMAGE` / `C_FILC_REVISION` / `C_FILC_BINARY_SHA256` and its own
   env.txt line, so both build flavors can interleave in one invocation):
   * `C_IMAGE` / `C_REVISION` / `C_BINARY_SHA256` environment defaults;
   * `image_for()` maps `c` to `C_IMAGE` and `c-filc` to `C_FILC_IMAGE`;
   * `SEED` becomes overridable (`SEED=${SEED:-...}`) so the imported seed
     (bench/seed/import_seed.py) is used instead of a Rust checkout's
     `parity/.seed/default`;
   * the Elixir parity-ledger gate and the `elixir source digest` line run
     only when `elixir` is in `--apps`; the `go HEAD` / `rust HEAD` env lines
     run only for their own apps (the Rust checkout path does not exist on
     this host).  For upstream app selections the behavior is identical.
   * `env_args()` forwards `-e CF_CACHE_BYTES=$CF_CACHE_BYTES` and
     `-e CF_LOOPS=$CF_LOOPS` for the `c` and `c-filc` apps (defaults 0 and 1;
     the flags `--cache-bytes 67108864` and `--loops 4` select the benchmark
     arms), and env.txt gains one
     `c revision: ... binary sha256: ... cache_bytes: ... loops: ... image: ...`
     line per C flavor present.
2. **`bench/validate.py`** makes the Elixir verification module
   (`bin/verify-parity`) a lazy import, needed only for the `digest` and
   `ledger` subcommands.  The preflight assertions (status 200, non-empty
   body, populated room/search, sidebar, avatar, CSS, `/up`, database count
   and integrity) are byte-identical to the pin.
3. **`bench/report`** adds the `c` and `c-filc` column/role mappings to the
   table renderer (`C (Fil-C)` label, `campfire` process role for both).
   With no `rust` column the advantage header reads
   `C advantage over ...`; medians and ranges are computed by pinned code.

`--apps c` / `--apps c,c-filc` plus the pinned reporting flow is the only
supported use; do not carry the patch upstream and do not add C-specific
branches to workload or timing code.

## Applying manually

```sh
rm -rf bench/.work/pinned
cp -a bench/pinned/once-campfire-elixir bench/.work/pinned
patch -p1 -d bench/.work/pinned < bench/adapters/c-app.patch
```

## Regenerating

Edit a copy of the pinned tree, then:

```sh
diff -ruN c-orig c-work > bench/adapters/c-app.patch
```

Update the hashes above and re-run `bench/run-c`'s setup (which applies the
patch and fails loudly if it no longer applies).
