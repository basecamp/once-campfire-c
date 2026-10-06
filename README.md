# Campfire in C

A C implementation of [ONCE Campfire](https://github.com/basecamp/once-campfire).
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
qrcodegen), and the same test suite also runs under
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
`CF_CACHE_BYTES` (body cache; 0 disables), `CF_CONNECTIONS_PER_LOOP`, the
`CF_*_BYTES` budgets, the TLS certificate/key pair (`TLS_CERT_FILE`,
`TLS_KEY_FILE`), and the VAPID key pair for Web Push. TLS uses configured
certificates; automatic certificate issuance (ACME) is out of scope.

## Performance

Measured with the pinned harness behind the
[upstream comparison table](https://github.com/basecamp/once-campfire#other-implementations)
(`basecamp/once-campfire-elixir` `bench/` at `b6b82e50`), its load generator
(HTTP/1.1 `hyper` client, plain `ws` for Cable), identical seed data for every
app, one container per app pinned to four server CPUs.

**Hardware:** AMD Ryzen 9 9900X (12 cores / 24 threads, 91 GB), Fedora kernel
7.1.0-rc7, Docker 29.7.2, performance governor; each app pinned to CPUs 8–11
(4 CPUs), load generator on CPUs 12–15, loopback `--network host`, HTTP/1.1
with gzip. The host was shared during these runs (see the load note below),
so absolute numbers carry the recorded per-rep load; the interleaved ordering
keeps the comparison valid.

**Method:** 3 interleaved runs per configuration, server order reversed every
rep, 2 s warmup / 8 s measured at 1, 16 and 64 concurrent clients, medians
reported; a run is accepted only when the pinned preflight passes and every
response is a validated 200. C candidate `46ed9c5` (clean tree, binary
sha256 `2e3c17d1…`, `CF_LOOPS=4`); Rust `1ea6d6f`
(`campfire-rust:app` `sha256:e21301de…`); Rails reference `90b3300`
(`campfire-reference:app` `sha256:3a498870…`); seed database sha256
`a7630146…`. Full provenance, per-rep load accounting and raw outputs under
[`bench/results/`](bench/results/) (`46ed9c5-uncached/`, `46ed9c5-cache/`).
Every rep in this set was load-caveated: the harness's quiet gate (load < 1.5)
never opened on the shared host (observed 2.2–14.3, CPU steal 0.00%), so the
relative comparison is the meaningful part and the absolute numbers are
preliminary in that sense.

### HTTP throughput (16 concurrent clients, requests/second)

| Workload | Rails | Rust (cache on) | C (cache off) | C (cache on) | C (cache off) vs Rails | C (cache on) vs Rust |
|---|---:|---:|---:|---:|---:|---:|
| Room page | 234 | 31,741 | 1,995 | **104,523** | 8.5× | 3.3× |
| Messages page | 438 | 29,021 | 2,465 | **128,748** | 5.6× | 4.4× |
| Sidebar | 679 | 32,519 | 22,923 | **134,211** | 34× | 4.1× |
| Search | 434 | 33,010 | 5,395 | **128,249** | 12× | 3.9× |
| Post a message | 1,116 | 54,242 | 92,508 | **109,889** | 83× | 2.0× |
| `/up` | 4,363 | 131,207 | 170,347 | **180,614** | 39× | 1.4× |

**Read the Rust column with its cache on.** Rust's numbers are its
production image with the server-side response cache enabled; the pinned
harness has no knob to disable it. Compare Rust with **C (cache on)**;
**C (cache off)** is the row to compare with **Rails** (which has no app
cache). The same configuration labels apply to the latency and size tables below.

The body cache accounts for 5.9–52× on the four admitted routes at 16
clients (room 52×, messages 52×, search 24×, sidebar 5.9×); posting, the
avatar and static assets are not cache-admitted (1.1–1.3×). Cache-off C is
already ahead of Rails on every row (5.6–83×) and ahead of the Rust image
on posting (1.7×) and `/up` (1.3×); cache-on C leads Rust on every measured
row (1.4–4.4×). The whole suite also passes the pinned preflight's body
validation for every app.

### Latency (medians, milliseconds)

| Route | Clients | Rails p50 | Rails p99 | Rust (cache on) p50 | Rust (cache on) p99 | C (cache off) p50 | C (cache off) p99 | C (cache on) p50 | C (cache on) p99 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Room page | 16 | 62.4 | 175.0 | 0.5 | 1.8 | 7.9 | 10.9 | 0.1 | 0.8 |
| Room page | 64 | 310.9 | 437.4 | 1.8 | 4.5 | 31.3 | 35.4 | 0.5 | 0.8 |
| Post a message | 16 | 14.3 | 28.6 | 0.3 | 1.0 | 0.1 | 1.0 | 0.1 | 0.3 |
| Post a message | 64 | 55.1 | 111.7 | 1.0 | 2.6 | 0.6 | 1.4 | 0.6 | 0.9 |

### Startup, memory and image size (medians)

| App | Cold start until `/up` (ms) | Idle cgroup memory (MiB) | Peak cgroup memory (MiB) | Image, unpacked (MiB) |
|---|---:|---:|---:|---:|
| Rails | 2,591 | 377 | 844 | 342 |
| Rust (cache on) | 180 | 40 | 60 | 68 |
| C (cache off) | 150 | 35 | 43 | 82 |
| C (cache on) | 133 | 39 | 47 | 82 |

### Notes on the comparison

- The harness measures HTTP/1.1 plaintext and plain `ws://` for Cable on every
  app; TLS and HTTP/2 are outside the measured path. The recorded candidate
  predates the independent-review repairs for H2 request lifetime, budgets,
  trailers, file workers and TLS Cable. No
  TLS/H2-vs-HTTP/1.1 comparison is implied by these numbers. The merged
  front shows no measurable regression on the measured path: `/up` is flat
  and cache-hit CPU per success is unchanged (24–25 µs/req).
- Media rows were excluded from these recorded runs; uploads, attachments and
  downloads were exercised. The media repair now generates actual variants
  and previews with the pinned tools described above, but has not been
  benchmarked in this repair pass.
- The recorded avatar endpoint served generated initials/bot SVGs where the
  reference served a processed variant; read that row with the representation
  difference in mind. The repaired variant path needs fresh measurements.
- These numbers are not comparable to the upstream table's absolute values
  (different hardware and a differently loaded host); they are comparable
  *between the three apps measured here*, under the same conditions.

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
(`bench/run-c --apps c,rust,reference --loops 4 --cache-bytes N`), the seed
importer and the recorded results.

Repository layout: `src/` (`core`, `http`, `front`, `auth`, `richtext`,
`views`/`presenters`, `actions`, `cable`, `jobs`, `storage`, `integrations`),
`tests/` (one suite per module plus the integration cases), `bench/`,
`vendor/` (pins and build scripts). The shared module contract is
`src/cf.h`; per-area headers and evidence live beside the code.

## License

MIT. See [`MIT-LICENSE`](MIT-LICENSE). Vendored dependencies keep their own
licenses, recorded in `vendor/DEPS.json`.
