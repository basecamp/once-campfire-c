```
date: 2026-10-05T15:09:50-04:00
host: 7.1.0-0.rc7.260611g9716c086c8e8.50.fc45.x86_64, AMD Ryzen 9 9900X 12-Core Processor, 24 threads, 91GB
server cpus: 8-11 (nproc 4); loadgen cpus: 12-15; network: host
env: WEB_CONCURRENCY=3 JOB_CONCURRENCY=3 RAILS_MAX_THREADS=5 
seed sha256: f0ed1d0f060e70f1540061f056b983807ab8c53245026414e40e847fb2b9e5b9  /home/msaraiva/dev/mark/Proj/Misc/once-campfire-c/bench/seed/.seed/default/db/production.sqlite3
c revision: 7a4c24e877c1 binary sha256: d947f3674c80e563acb69a1be4cb0d38575f5d53881d4310808643da791bf645 cache_bytes: 67108864 image: campfire-c:bench
rust extra env: 
workload: suites=http HTTP_SECS=2 HTTP_CONCS=1 16 CABLE_CLIENTS=100 500 1000 CABLE_TPUT_SECS=15 CABLE_POSTERS=4 UPLOAD_REPS=5 REPS=1
quiet wait: LOAD_MAX=99 LOAD_WAIT_SECS=1
user agent: (none)
c image: campfire-c:bench sha256:8b25e5dd39952daa178d09d99c3184bc6b21882f5127e651958832ae0a9dd4b9 2026-10-05T15:06:31.571660183-04:00 unpacked_bytes=84307512
```

Reps: c 1. Cells: median [min–max].

### Startup and memory

| Metric | C | C (single app) |
|---|---|---|
| cold start: docker run → /up 200 (ms) | 294 | – |
| idle memory.current (MiB) | 9.00 | – |
| idle anon (MiB) | 8.00 | – |
| peak memory.current under load (MiB) | 16.0 | – |
| peak anon under load (MiB) | 13.0 | – |

### HTTP (signed in as david; keep-alive; c = concurrent connections)

| Metric | C | C (single app) |
|---|---|---|
| room_show c=1 req/s | 24.4 | – |
| room_show c=1 p50 ms | 42.1 | – |
| room_show c=1 p99 ms | 45.8 | – |
| room_show c=1 CPU µs/success | 1,080 | – |
| room_show c=16 req/s | 379 | – |
| room_show c=16 p50 ms | 43.0 | – |
| room_show c=16 p99 ms | 54.1 | – |
| room_show c=16 CPU µs/success | 959 | – |
| messages_page c=1 req/s | 24.3 | – |
| messages_page c=1 p50 ms | 42.0 | – |
| messages_page c=1 p99 ms | 44.0 | – |
| messages_page c=1 CPU µs/success | 692 | – |
| messages_page c=16 req/s | 386 | – |
| messages_page c=16 p50 ms | 42.0 | – |
| messages_page c=16 p99 ms | 44.1 | – |
| messages_page c=16 CPU µs/success | 596 | – |
| sidebar c=1 req/s | 22.4 | – |
| sidebar c=1 p50 ms | 46.2 | – |
| sidebar c=1 p99 ms | 49.0 | – |
| sidebar c=1 CPU µs/success | 5,093 | – |
| sidebar c=16 req/s | 351 | – |
| sidebar c=16 p50 ms | 46.0 | – |
| sidebar c=16 p99 ms | 60.7 | – |
| sidebar c=16 CPU µs/success | 4,888 | – |
| search c=1 req/s | 110 | – |
| search c=1 p50 ms | 8.88 | – |
| search c=1 p99 ms | 12.1 | – |
| search c=1 CPU µs/success | 8,950 | – |
| search c=16 req/s | 450 | – |
| search c=16 p50 ms | 35.1 | – |
| search c=16 p99 ms | 41.7 | – |
| search c=16 CPU µs/success | 8,821 | – |
| avatar c=1 req/s | 23.9 | – |
| avatar c=1 p50 ms | 43.0 | – |
| avatar c=1 p99 ms | 43.6 | – |
| avatar c=1 CPU µs/success | 1,202 | – |
| avatar c=16 req/s | 377 | – |
| avatar c=16 p50 ms | 43.0 | – |
| avatar c=16 p99 ms | 45.1 | – |
| avatar c=16 CPU µs/success | 1,079 | – |
| static_css c=1 req/s | 24.3 | – |
| static_css c=1 p50 ms | 42.0 | – |
| static_css c=1 p99 ms | 43.0 | – |
| static_css c=1 CPU µs/success | 104 | – |
| static_css c=16 req/s | 389 | – |
| static_css c=16 p50 ms | 42.0 | – |
| static_css c=16 p99 ms | 43.1 | – |
| static_css c=16 CPU µs/success | 22.0 | – |
| up c=1 req/s | 24.5 | – |
| up c=1 p50 ms | 42.0 | – |
| up c=1 p99 ms | 42.2 | – |
| up c=1 CPU µs/success | 88.1 | – |
| up c=16 req/s | 390 | – |
| up c=16 p50 ms | 42.0 | – |
| up c=16 p99 ms | 43.0 | – |
| up c=16 CPU µs/success | 26.8 | – |
| post_message c=1 req/s | 24.4 | – |
| post_message c=1 p50 ms | 43.0 | – |
| post_message c=1 p99 ms | 44.0 | – |
| post_message c=1 CPU µs/success | 1,233 | – |
| post_message c=16 req/s | 386 | – |
| post_message c=16 p50 ms | 43.0 | – |
| post_message c=16 p99 ms | 45.9 | – |
| post_message c=16 CPU µs/success | 1,147 | – |

### HTTP errors / non-2xx-3xx (first rep, per app)

| Metric | C | C (single app) |
|---|---|---|
- c: none

### Action Cable fan-out (one room; chatter.js subscriptions per client)

| Metric | C | C (single app) |
|---|---|---|

### Upload + thumbnail (black_hole.jpg, 505 KB)

| Metric | C | C (single app) |
|---|---|---|
| POST with attachment (ms) | – | – |
| then GET thumb → 200 (ms) | – | – |
| POST → thumbnail served (ms) | – | – |

### Memory during cable fan-out, by process (MiB, peak within the phase)

App process: Elixir BEAM; Rails Puma master/workers when included;
Go/Rust/C's integrated campfire processes. Serving totals include native helpers, Redis and Thruster.
PSS apportions shared pages; RssAnon counts them in each process.

| Metric | C | C (single app) |
|---|---|---|
