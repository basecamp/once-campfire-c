```
date: 2026-10-05T18:42:30-04:00
host: 7.1.0-0.rc7.260611g9716c086c8e8.50.fc45.x86_64, AMD Ryzen 9 9900X 12-Core Processor, 24 threads, 91GB
server cpus: 8-11 (nproc 4); loadgen cpus: 12-15; network: host
env: WEB_CONCURRENCY=3 JOB_CONCURRENCY=3 RAILS_MAX_THREADS=5 
seed sha256: a763014643b8e26d38f153e98647500e42ef2c305fdbaf24145fccdbcb9ae030  /home/msaraiva/dev/mark/Proj/Misc/once-campfire-c/bench/seed/.seed/default/db/production.sqlite3
c revision: 604f037c0e25+dirty10 binary sha256: 48f93d27594a4c65bdcab845993b31ef0f526d9d646aaf3831d0a6e168b01f6e cache_bytes: 67108864 loops: 4 image: campfire-c:bench
rust extra env: 
workload: suites=http HTTP_SECS=8 HTTP_CONCS=1 16 64 CABLE_CLIENTS=100 500 1000 CABLE_TPUT_SECS=15 CABLE_POSTERS=4 UPLOAD_REPS=5 REPS=3
quiet wait: LOAD_MAX=8 LOAD_WAIT_SECS=300
user agent: (none)
c image: campfire-c:bench sha256:ed22cdb399ab9f1c5b9a56b9584f9b20d27938f46ce24f0d1da3ccb325a5cf49 2026-10-05T18:42:11.177489681-04:00 unpacked_bytes=84307459
```

Reps: c 3. Cells: median [min–max].

### Startup and memory

| Metric | C | C (single app) |
|---|---|---|
| cold start: docker run → /up 200 (ms) | 112 [112–138] | – |
| idle memory.current (MiB) | 31.0 [30.0–31.0] | – |
| idle anon (MiB) | 30.0 [30.0–30.0] | – |
| peak memory.current under load (MiB) | 39.0 [38.0–39.0] | – |
| peak anon under load (MiB) | 35.0 [34.0–35.0] | – |

### HTTP (signed in as david; keep-alive; c = concurrent connections)

| Metric | C | C (single app) |
|---|---|---|
| room_show c=1 req/s | 28,485 [25,660–28,510] | – |
| room_show c=1 p50 ms | 0.03 [0.03–0.03] | – |
| room_show c=1 p99 ms | 0.05 [0.05–0.06] | – |
| room_show c=1 CPU µs/success | 27.1 [26.9–27.5] | – |
| room_show c=16 req/s | 130,635 [129,758–132,863] | – |
| room_show c=16 p50 ms | 0.12 [0.12–0.12] | – |
| room_show c=16 p99 ms | 0.24 [0.22–0.26] | – |
| room_show c=16 CPU µs/success | 23.7 [23.1–25.1] | – |
| room_show c=64 req/s | 141,037 [136,213–142,947] | – |
| room_show c=64 p50 ms | 0.48 [0.48–0.48] | – |
| room_show c=64 p99 ms | 0.82 [0.74–1.02] | – |
| room_show c=64 CPU µs/success | 23.1 [22.6–23.2] | – |
| messages_page c=1 req/s | 29,887 [29,343–30,156] | – |
| messages_page c=1 p50 ms | 0.03 [0.03–0.03] | – |
| messages_page c=1 p99 ms | 0.05 [0.04–0.05] | – |
| messages_page c=1 CPU µs/success | 25.9 [25.7–26.2] | – |
| messages_page c=16 req/s | 142,763 [142,174–148,602] | – |
| messages_page c=16 p50 ms | 0.10 [0.10–0.11] | – |
| messages_page c=16 p99 ms | 0.25 [0.24–0.26] | – |
| messages_page c=16 CPU µs/success | 22.9 [22.6–23.4] | – |
| messages_page c=64 req/s | 153,039 [151,501–163,572] | – |
| messages_page c=64 p50 ms | 0.44 [0.41–0.44] | – |
| messages_page c=64 p99 ms | 0.76 [0.66–0.77] | – |
| messages_page c=64 CPU µs/success | 21.5 [20.3–21.7] | – |
| sidebar c=1 req/s | 32,108 [32,018–32,884] | – |
| sidebar c=1 p50 ms | 0.03 [0.03–0.03] | – |
| sidebar c=1 p99 ms | 0.04 [0.04–0.04] | – |
| sidebar c=1 CPU µs/success | 23.8 [23.5–23.9] | – |
| sidebar c=16 req/s | 148,846 [144,734–160,786] | – |
| sidebar c=16 p50 ms | 0.10 [0.09–0.11] | – |
| sidebar c=16 p99 ms | 0.22 [0.21–0.24] | – |
| sidebar c=16 CPU µs/success | 21.5 [20.2–22.1] | – |
| sidebar c=64 req/s | 167,074 [159,318–169,390] | – |
| sidebar c=64 p50 ms | 0.41 [0.41–0.43] | – |
| sidebar c=64 p99 ms | 0.69 [0.67–0.71] | – |
| sidebar c=64 CPU µs/success | 19.5 [19.1–20.1] | – |
| search c=1 req/s | 30,530 [29,317–31,003] | – |
| search c=1 p50 ms | 0.03 [0.03–0.03] | – |
| search c=1 p99 ms | 0.05 [0.04–0.06] | – |
| search c=1 CPU µs/success | 24.7 [24.7–24.9] | – |
| search c=16 req/s | 149,171 [128,919–151,328] | – |
| search c=16 p50 ms | 0.10 [0.10–0.11] | – |
| search c=16 p99 ms | 0.24 [0.21–0.46] | – |
| search c=16 CPU µs/success | 22.2 [21.6–23.0] | – |
| search c=64 req/s | 152,961 [103,106–163,706] | – |
| search c=64 p50 ms | 0.45 [0.42–0.58] | – |
| search c=64 p99 ms | 0.71 [0.68–1.93] | – |
| search c=64 CPU µs/success | 20.5 [20.0–28.2] | – |
| avatar c=1 req/s | 30,992 [27,840–31,846] | – |
| avatar c=1 p50 ms | 0.03 [0.03–0.03] | – |
| avatar c=1 p99 ms | 0.04 [0.04–0.07] | – |
| avatar c=1 CPU µs/success | 25.6 [24.9–26.7] | – |
| avatar c=16 req/s | 157,895 [143,841–158,315] | – |
| avatar c=16 p50 ms | 0.09 [0.09–0.09] | – |
| avatar c=16 p99 ms | 0.31 [0.28–0.40] | – |
| avatar c=16 CPU µs/success | 22.1 [22.1–22.8] | – |
| avatar c=64 req/s | 171,638 [167,213–174,915] | – |
| avatar c=64 p50 ms | 0.37 [0.37–0.38] | – |
| avatar c=64 p99 ms | 0.76 [0.76–0.78] | – |
| avatar c=64 CPU µs/success | 20.5 [20.2–20.6] | – |
| static_css c=1 req/s | 39,491 [38,172–40,224] | – |
| static_css c=1 p50 ms | 0.02 [0.02–0.03] | – |
| static_css c=1 p99 ms | 0.03 [0.03–0.04] | – |
| static_css c=1 CPU µs/success | 19.1 [18.8–19.5] | – |
| static_css c=16 req/s | 144,636 [97,693–152,779] | – |
| static_css c=16 p50 ms | 0.12 [0.11–0.15] | – |
| static_css c=16 p99 ms | 0.24 [0.19–0.26] | – |
| static_css c=16 CPU µs/success | 16.5 [12.1–17.2] | – |
| static_css c=64 req/s | 162,456 [152,046–192,709] | – |
| static_css c=64 p50 ms | 0.45 [0.41–0.46] | – |
| static_css c=64 p99 ms | 0.78 [0.70–0.82] | – |
| static_css c=64 CPU µs/success | 13.8 [13.5–14.9] | – |
| up c=1 req/s | 44,423 [43,950–44,431] | – |
| up c=1 p50 ms | 0.02 [0.02–0.02] | – |
| up c=1 p99 ms | 0.03 [0.03–0.03] | – |
| up c=1 CPU µs/success | 16.1 [16.1–16.2] | – |
| up c=16 req/s | 180,372 [171,428–181,729] | – |
| up c=16 p50 ms | 0.09 [0.09–0.10] | – |
| up c=16 p99 ms | 0.14 [0.13–0.15] | – |
| up c=16 CPU µs/success | 14.6 [14.5–15.4] | – |
| up c=64 req/s | 202,784 [199,432–211,907] | – |
| up c=64 p50 ms | 0.36 [0.36–0.36] | – |
| up c=64 p99 ms | 0.50 [0.49–0.52] | – |
| up c=64 CPU µs/success | 12.1 [11.5–12.7] | – |
| post_message c=1 req/s | 24,280 [22,769–24,310] | – |
| post_message c=1 p50 ms | 0.04 [0.04–0.04] | – |
| post_message c=1 p99 ms | 0.06 [0.06–0.08] | – |
| post_message c=1 CPU µs/success | 31.7 [31.6–32.6] | – |
| post_message c=16 req/s | 104,435 [92,038–106,413] | – |
| post_message c=16 p50 ms | 0.14 [0.14–0.14] | – |
| post_message c=16 p99 ms | 0.29 [0.27–0.86] | – |
| post_message c=16 CPU µs/success | 33.1 [33.0–34.0] | – |
| post_message c=64 req/s | 112,650 [109,613–113,039] | – |
| post_message c=64 p50 ms | 0.57 [0.57–0.58] | – |
| post_message c=64 p99 ms | 0.92 [0.89–1.04] | – |
| post_message c=64 CPU µs/success | 31.6 [31.4–32.0] | – |

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
