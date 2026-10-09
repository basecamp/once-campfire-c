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
Cache lookups recheck authorization and observe external SQLite commits. A commit during
authentication bypasses cache lookup and admission for that request. Flash-bearing
requests also bypass lookup so cached pages cannot hide one-time notices.

## Benchmarks

Measured with 16 concurrent clients on an AMD Ryzen AI MAX+ 395 with 32 GB RAM,
with four hardware cores allocated to each app.

| HTTP workload (requests/sec) | Rails | [Django](https://github.com/basecamp/once-campfire-django) | [Laravel](https://github.com/basecamp/once-campfire-laravel) | [Express](https://github.com/basecamp/once-campfire-express) | [Elixir](https://github.com/basecamp/once-campfire-elixir) | [Go](https://github.com/basecamp/once-campfire-go) | [Rust](https://github.com/basecamp/once-campfire-rust) | [C](https://github.com/basecamp/once-campfire-c) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Room page | 4,101 | 1,507 | 3,872 | 42,636 | 5,350 | 53,060 | 106,494 | 137,524 |
| Messages page | 4,115 | 1,596 | 3,995 | 74,362 | 5,712 | 54,800 | 102,697 | 144,642 |
| Sidebar | 4,333 | 1,873 | 4,493 | 94,329 | 5,949 | 59,144 | 120,294 | 152,002 |
| Search | 4,282 | 1,862 | 4,172 | 84,665 | 5,848 | 60,509 | 121,378 | 149,487 |
| Post a message | 330 | 262 | 794 | 2,155 | 1,278 | 9,021 | 8,037 | 7,530 |
| Backend KLOC | 5.5 | 5.3 | 4.1 | 6.8 | 11.6 | 23.2 | 30.7 | 94.8 |

KLOC counts backend code plus executable code in templates; excludes plain HTML, frontend, tests, dependencies and generated files.

[Shared verification](https://github.com/basecamp/once-campfire-verification) · [Detailed results](https://github.com/basecamp/once-campfire-verification/blob/main/docs/performance-review.md).

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
`agent-browser` (no Chromium download). Current comparison commands use the
shared verification harness; see [`bench/`](bench/README.md).

Repository layout: `src/` (`core`, `http`, `front`, `auth`, `richtext`,
`views`/`presenters`, `actions`, `cable`, `jobs`, `storage`, `integrations`),
`tests/` (one suite per module plus the integration cases), `bench/`,
`vendor/` (pins and build scripts). The shared module contract is
`src/cf.h`; per-area headers and evidence live beside the code.

## Known differences

The session transfer page explicitly closes its auto-submit form; the pinned reference omits the closing tag. Its view regression checks the fixed markup while comparing the rest of the page against the original fixture.

- Sidebar connection refresh waits for the current Turbo frame to finish loading,
  preventing an aborted response on startup or reconnect. Obsolete connections and removed frames do not reload.

- Search returns the newest 100 accessible matches by insertion ID and displays them in ID
  order. Backdated imports can therefore appear in a different order from Rails.
- Incremental refresh uses a port-owned `(room_id, updated_at)` index; the frozen reference
  schema remains unchanged.

## License

MIT. See [`MIT-LICENSE`](MIT-LICENSE). Vendored dependencies keep their own
licenses, recorded in `vendor/DEPS.json`.
