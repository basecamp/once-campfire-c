```
date: 2026-10-05T18:06:20-04:00
host: 7.1.0-0.rc7.260611g9716c086c8e8.50.fc45.x86_64, AMD Ryzen 9 9900X 12-Core Processor, 24 threads, 91GB
server cpus: 8-11 (nproc 4); loadgen cpus: 12-15; network: host
env: WEB_CONCURRENCY=3 JOB_CONCURRENCY=3 RAILS_MAX_THREADS=5 
seed sha256: a763014643b8e26d38f153e98647500e42ef2c305fdbaf24145fccdbcb9ae030  /home/msaraiva/dev/mark/Proj/Misc/once-campfire-c/bench/seed/.seed/default/db/production.sqlite3
c revision: 7a4c24e877c1+landed42 binary sha256: 65d877ad9dccbabc49efc323c1a288cdd69d035ad80e9afe12d734145faa2d5a cache_bytes: 0 loops: 4 image: campfire-c:bench
rust extra env: 
workload: suites=http HTTP_SECS=8 HTTP_CONCS=1 16 64 CABLE_CLIENTS=100 500 1000 CABLE_TPUT_SECS=15 CABLE_POSTERS=4 UPLOAD_REPS=5 REPS=3
quiet wait: LOAD_MAX=8 LOAD_WAIT_SECS=300
user agent: (none)
c image: campfire-c:bench sha256:d2e63c54ed4c02e585512bac27490c93ea2ed00b900b621baeb3915a331a30ef 2026-10-05T17:54:49.95553916-04:00 unpacked_bytes=84310015
```

Reps: c 3. Cells: median [min–max].

### Startup and memory

| Metric | C | C (single app) |
|---|---|---|
| cold start: docker run → /up 200 (ms) | 118 [113–119] | – |
| idle memory.current (MiB) | 30.0 [30.0–30.0] | – |
| idle anon (MiB) | 30.0 [30.0–30.0] | – |
| peak memory.current under load (MiB) | 38.0 [38.0–38.0] | – |
| peak anon under load (MiB) | 35.0 [35.0–35.0] | – |

### HTTP (signed in as david; keep-alive; c = concurrent connections)

| Metric | C | C (single app) |
|---|---|---|
| room_show c=1 req/s | 41.3 [41.0–41.4] | – |
| room_show c=1 p50 ms | 24.1 [24.1–24.2] | – |
| room_show c=1 p99 ms | 26.8 [25.7–27.4] | – |
| room_show c=1 CPU µs/success | 24,108 [24,076–24,285] | – |
| room_show c=16 req/s | 163 [156–163] | – |
| room_show c=16 p50 ms | 97.7 [97.2–99.8] | – |
| room_show c=16 p99 ms | 112 [108–123] | – |
| room_show c=16 CPU µs/success | 24,446 [24,439–25,578] | – |
| room_show c=64 req/s | 159 [159–162] | – |
| room_show c=64 p50 ms | 395 [393–397] | – |
| room_show c=64 p99 ms | 441 [415–484] | – |
| room_show c=64 CPU µs/success | 25,030 [24,619–25,130] | – |
| messages_page c=1 req/s | 43.1 [43.1–43.7] | – |
| messages_page c=1 p50 ms | 23.1 [22.8–23.1] | – |
| messages_page c=1 p99 ms | 25.9 [24.3–27.7] | – |
| messages_page c=1 CPU µs/success | 23,090 [22,822–23,116] | – |
| messages_page c=16 req/s | 172 [161–172] | – |
| messages_page c=16 p50 ms | 92.7 [92.6–96.6] | – |
| messages_page c=16 p99 ms | 102 [100–119] | – |
| messages_page c=16 CPU µs/success | 23,260 [23,180–24,803] | – |
| messages_page c=64 req/s | 170 [167–171] | – |
| messages_page c=64 p50 ms | 374 [374–378] | – |
| messages_page c=64 p99 ms | 395 [383–425] | – |
| messages_page c=64 CPU µs/success | 23,443 [23,312–23,914] | – |
| sidebar c=1 req/s | 205 [204–206] | – |
| sidebar c=1 p50 ms | 4.81 [4.79–4.83] | – |
| sidebar c=1 p99 ms | 6.43 [6.01–6.55] | – |
| sidebar c=1 CPU µs/success | 4,829 [4,808–4,853] | – |
| sidebar c=16 req/s | 811 [766–820] | – |
| sidebar c=16 p50 ms | 19.6 [19.3–20.5] | – |
| sidebar c=16 p99 ms | 24.1 [24.0–25.2] | – |
| sidebar c=16 CPU µs/success | 4,904 [4,858–5,205] | – |
| sidebar c=64 req/s | 802 [780–804] | – |
| sidebar c=64 p50 ms | 78.5 [78.3–81.0] | – |
| sidebar c=64 p99 ms | 89.2 [88.1–92.0] | – |
| sidebar c=64 CPU µs/success | 4,967 [4,959–5,109] | – |
| search c=1 req/s | 110 [110–111] | – |
| search c=1 p50 ms | 9.02 [8.86–9.03] | – |
| search c=1 p99 ms | 10.5 [10.3–10.9] | – |
| search c=1 CPU µs/success | 9,017 [8,925–9,042] | – |
| search c=16 req/s | 420 [396–436] | – |
| search c=16 p50 ms | 37.0 [36.0–39.8] | – |
| search c=16 p99 ms | 46.9 [43.9–49.6] | – |
| search c=16 CPU µs/success | 9,495 [9,133–10,076] | – |
| search c=64 req/s | 406 [397–436] | – |
| search c=64 p50 ms | 159 [146–161] | – |
| search c=64 p99 ms | 174 [153–184] | – |
| search c=64 CPU µs/success | 9,808 [9,139–10,029] | – |
| avatar c=1 req/s | 905 [904–923] | – |
| avatar c=1 p50 ms | 1.07 [1.05–1.08] | – |
| avatar c=1 p99 ms | 1.72 [1.72–1.77] | – |
| avatar c=1 CPU µs/success | 1,073 [1,063–1,078] | – |
| avatar c=16 req/s | 3,672 [3,658–3,731] | – |
| avatar c=16 p50 ms | 4.31 [4.23–4.35] | – |
| avatar c=16 p99 ms | 6.40 [6.15–6.64] | – |
| avatar c=16 CPU µs/success | 1,077 [1,061–1,079] | – |
| avatar c=64 req/s | 3,688 [2,652–3,709] | – |
| avatar c=64 p50 ms | 17.2 [17.1–25.7] | – |
| avatar c=64 p99 ms | 21.0 [20.4–30.0] | – |
| avatar c=64 CPU µs/success | 1,067 [1,064–1,483] | – |
| static_css c=1 req/s | 36,561 [34,762–37,580] | – |
| static_css c=1 p50 ms | 0.03 [0.02–0.03] | – |
| static_css c=1 p99 ms | 0.04 [0.04–0.04] | – |
| static_css c=1 CPU µs/success | 20.0 [19.7–20.5] | – |
| static_css c=16 req/s | 127,793 [113,299–128,476] | – |
| static_css c=16 p50 ms | 0.12 [0.12–0.14] | – |
| static_css c=16 p99 ms | 0.23 [0.21–0.60] | – |
| static_css c=16 CPU µs/success | 14.9 [14.7–15.6] | – |
| static_css c=64 req/s | 151,958 [122,689–153,314] | – |
| static_css c=64 p50 ms | 0.46 [0.46–0.55] | – |
| static_css c=64 p99 ms | 0.87 [0.84–1.43] | – |
| static_css c=64 CPU µs/success | 14.7 [12.6–16.2] | – |
| up c=1 req/s | 39,775 [38,374–39,969] | – |
| up c=1 p50 ms | 0.02 [0.02–0.02] | – |
| up c=1 p99 ms | 0.04 [0.04–0.04] | – |
| up c=1 CPU µs/success | 16.5 [16.5–16.6] | – |
| up c=16 req/s | 171,296 [159,066–173,056] | – |
| up c=16 p50 ms | 0.09 [0.09–0.09] | – |
| up c=16 p99 ms | 0.15 [0.14–0.16] | – |
| up c=16 CPU µs/success | 13.7 [11.3–14.6] | – |
| up c=64 req/s | 194,052 [192,573–196,464] | – |
| up c=64 p50 ms | 0.36 [0.36–0.37] | – |
| up c=64 p99 ms | 0.53 [0.53–0.55] | – |
| up c=64 CPU µs/success | 12.3 [12.2–12.6] | – |
| post_message c=1 req/s | 892 [887–911] | – |
| post_message c=1 p50 ms | 1.08 [1.07–1.09] | – |
| post_message c=1 p99 ms | 1.75 [1.69–1.83] | – |
| post_message c=1 CPU µs/success | 1,090 [1,071–1,093] | – |
| post_message c=16 req/s | 3,410 [3,316–3,700] | – |
| post_message c=16 p50 ms | 4.42 [4.34–4.57] | – |
| post_message c=16 p99 ms | 6.92 [6.16–8.82] | – |
| post_message c=16 CPU µs/success | 1,152 [1,071–1,195] | – |
| post_message c=64 req/s | 3,644 [3,345–3,694] | – |
| post_message c=64 p50 ms | 17.4 [17.0–18.3] | – |
| post_message c=64 p99 ms | 22.2 [20.3–31.1] | – |
| post_message c=64 CPU µs/success | 1,085 [1,066–1,179] | – |

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
