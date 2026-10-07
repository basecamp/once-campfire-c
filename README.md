# Campfire in C

A C implementation of [ONCE Campfire](https://github.com/basecamp/once-campfire),
originally created by [mrsaraiva](https://github.com/mrsaraiva/once-campfire-c).
This is a greenfield implementation using SQLite, a filesystem storage layout
and signed/encrypted cookies. Existing installations and schema upgrades are
not supported.

One `campfire` executable replaces Ruby, Puma, Redis, Resque and Thruster. The
front speaks TLS (configured certificates) with ALPN and HTTP/2, the app
implements the reference's controllers, views and Action Cable-compatible
WebSockets, uploads are Active Storage-compatible, and Web Push, bot webhooks,
search and the rich-text pipeline are included.

Everything is pinned and self-contained: the Makefile builds against vendored
dependencies recorded in [`vendor/DEPS.json`](vendor/DEPS.json) (SQLite,
OpenSSL, curl, nghttp2, zlib-ng, gumbo, libxcrypt, yyjson, picohttpparser,
qrcodegen), and the same test suite can also run under
[Fil-C](https://fil-c.org) for memory-safety validation.

## Running it

Build the pinned dependencies once, then the app:

```sh
make deps                 # fetch pinned sources and install Fil-C
make deps-build           # build and probe the clang dependencies
make                      # dev build -> build/dev/campfire
make MODE=bench build-app # optimized build (same dependency archives)
```

Dependency recipes require clang, make, CMake, Perl, autoconf, automake,
libtool, pkg-config and zlib development headers; fetch scripts also use Git,
curl, unzip and Python 3. The recorded clang curl recipe links system zlib.
For Fil-C, run `make deps-build MODE=filc` before `make MODE=filc build-app`.
`deps-build` runs the committed build/probe recipes without downloading;
ordinary application builds also never fetch.

Image variants, video previews and full media acceptance also require the
reference-pinned media tools and the fixed libvips adapter. See the
[media build instructions](vendor/media/README.md) for the isolated tool
build and the fixed executable paths to select when building the app/tests.

Run it:

```sh
SECRET_KEY_BASE="$(openssl rand -hex 64)" \
PUBLIC_ORIGIN=http://localhost:3000 \
DISABLE_SSL=1 \
./build/dev/campfire
```

Configuration is read once from the environment (no reload). The settings
include `HOST`/`PORT`, `DATABASE_PATH`, `STORAGE_PATH`, `PUBLIC_ORIGIN`,
`SECRET_KEY_BASE`, `CF_LOOPS`/`CF_READERS`/`CF_REQUEST_SLOTS`,
`CF_CACHE_BYTES` (64 MiB body cache by default; 0 disables), `CF_CONNECTIONS_PER_LOOP`, the
`CF_*_BYTES` budgets, the TLS certificate/key pair (`TLS_CERT_FILE`,
`TLS_KEY_FILE`), and the VAPID key pair for Web Push. TLS uses configured
certificates; automatic certificate issuance (ACME) is out of scope. On Linux, the default
event-loop count follows CPU affinity, capped at four; `CF_LOOPS` overrides it.
Cache lookups recheck authorization and observe external SQLite commits.

## Benchmarks

The current [`benchmark harness`](bench/README.md) validates every response and audits
each acknowledged message ID, stored body and FTS entry.
Rebuild benchmark seeds after updating: the earlier importer confused user and room IDs,
so the historical POST throughput figures were invalid.

Current C and Fil-C builds need fresh measurements with the corrected seed. The other
implementations' current comparison is in the [Rust README](https://github.com/basecamp/once-campfire-rust#performance).

## Development

Build the pinned media tools and export their four `CF_PROC_*_PATH` variables
as described in [media setup](vendor/media/README.md) before the full suites.
Use a fresh output tree when changing these compile-time paths. The strict
media and HTTPS browser arms are enabled below:

```sh
CF_MEDIA_LIVE=1 make test          # dev suite
CF_MEDIA_LIVE=1 make sanitize      # ASan/UBSan/LSan suite
CF_MEDIA_LIVE=1 make MODE=filc test # the suite under Fil-C
make tsan          # the threaded subset under TSan
CF_E2E_TLS=1 CF_E2E_MEDIA=1 python3 tests/integration/run.py --all
```

The integration cases drive a real server and real browsers through
`agent-browser` (no Chromium download). The benchmark lives in
[`bench/`](bench/README.md): the pinned harness, the C adapter
(`bench/run-c --filc --apps c,c-filc,rust,reference --loops 4 --cache-bytes N`),
the seed importer and persistent-write validation.

Repository layout: `src/` (`core`, `http`, `front`, `auth`, `richtext`,
`views`/`presenters`, `actions`, `cable`, `jobs`, `storage`, `integrations`),
`tests/` (one suite per module plus the integration cases), `bench/`,
`vendor/` (pins and build scripts). The shared module contract is
`src/cf.h`; per-area headers and evidence live beside the code.

## Known differences

- Sidebar connection refresh waits for the current Turbo frame to finish loading,
  preventing an aborted response on startup or reconnect. Obsolete connections and removed frames do not reload.

- Search returns the newest 100 accessible matches by insertion ID and displays them in ID
  order. Backdated imports can therefore appear in a different order from Rails.
- Incremental refresh uses a port-owned `(room_id, updated_at)` index; the frozen reference
  schema remains unchanged.

## License

MIT. See [`MIT-LICENSE`](MIT-LICENSE). Vendored dependencies keep their own
licenses, recorded in `vendor/DEPS.json`.
