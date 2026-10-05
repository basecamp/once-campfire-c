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
| `bench/run` | `72d1b271da51c883dc4919e8fa49af4102a8d37898238b99fce4a17983aab793` |
| `bench/validate.py` | `cd9b4a262d92ab3a125e6b2d60629693ec504f23cd6355f27f45c1eda758c0d5` |
| `bench/report` | `e48adb4da2befe36f5ccacc28b81fdb0a976c19aa4bf655116ff42329b57b037` |

The patch changes no load generator, no workload, no timing constant, and no
preflight assertion.  Its parts:

1. **`bench/run`** adds the `c` app:
   * `C_IMAGE` / `C_REVISION` / `C_BINARY_SHA256` environment defaults;
   * `image_for()` maps `c` to `C_IMAGE`;
   * `SEED` becomes overridable (`SEED=${SEED:-...}`) so the imported seed
     (bench/seed/import_seed.py) is used instead of a Rust checkout's
     `parity/.seed/default`;
   * the Elixir parity-ledger gate and the `elixir source digest` line run
     only when `elixir` is in `--apps`; the `go HEAD` / `rust HEAD` env lines
     run only for their own apps (the Rust checkout path does not exist on
     this host).  For upstream app selections the behavior is identical.
   * `env_args()` forwards `-e CF_CACHE_BYTES=$CF_CACHE_BYTES` for the `c`
     app (default 0, the uncached arm; `run-c --cache-bytes 67108864` runs the
     cache-enabled arm), and env.txt gains one
     `c revision: ... binary sha256: ... cache_bytes: ... image: ...` line.
2. **`bench/validate.py`** makes the Elixir verification module
   (`bin/verify-parity`) a lazy import, needed only for the `digest` and
   `ledger` subcommands.  The preflight assertions (status 200, non-empty
   body, populated room/search, sidebar, avatar, CSS, `/up`, database count
   and integrity) are byte-identical to the pin.
3. **`bench/report`** adds the `c` column/role mappings to the table
   renderer.  With no `rust` column the advantage header reads
   `C advantage over ...`; medians and ranges are computed by pinned code.

`--apps c` plus the pinned reporting flow is the only supported use; do not
carry the patch upstream and do not add C-specific branches to workload or
timing code.

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
