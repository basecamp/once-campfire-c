```
date: 2026-10-05T15:23:30-04:00
host: 7.1.0-0.rc7.260611g9716c086c8e8.50.fc45.x86_64, AMD Ryzen 9 9900X 12-Core Processor, 24 threads, 91GB
server cpus: 8-11 (nproc 4); loadgen cpus: 12-15; network: host
env: WEB_CONCURRENCY=3 JOB_CONCURRENCY=3 RAILS_MAX_THREADS=5 
seed sha256: a763014643b8e26d38f153e98647500e42ef2c305fdbaf24145fccdbcb9ae030  /home/msaraiva/dev/mark/Proj/Misc/once-campfire-c/bench/seed/.seed/default/db/production.sqlite3
c revision: 7a4c24e877c1+landed42 binary sha256: 65d877ad9dccbabc49efc323c1a288cdd69d035ad80e9afe12d734145faa2d5a cache_bytes: 0 image: campfire-c:bench
rust extra env: 
workload: suites=http HTTP_SECS=8 HTTP_CONCS=1 16 64 CABLE_CLIENTS=100 500 1000 CABLE_TPUT_SECS=15 CABLE_POSTERS=4 UPLOAD_REPS=5 REPS=3
quiet wait: LOAD_MAX=8 LOAD_WAIT_SECS=300
user agent: (none)
c image: campfire-c:bench sha256:2f8a2d4de959e76ddb756bcc3eff6e7f9c86a4e3793104c782309b1786bffc7b 2026-10-05T15:23:06.756450239-04:00 unpacked_bytes=84309896
```

Reps: c 3. Cells: median [min–max].

### Startup and memory

| Metric | C | C (single app) |
|---|---|---|
| cold start: docker run → /up 200 (ms) | 144 [115–155] | – |
| idle memory.current (MiB) | 10.0 [9.0–11.0] | – |
| idle anon (MiB) | 8.00 [8.00–8.00] | – |
| peak memory.current under load (MiB) | 17.0 [17.0–18.0] | – |
| peak anon under load (MiB) | 14.0 [14.0–14.0] | – |

### HTTP (signed in as david; keep-alive; c = concurrent connections)

| Metric | C | C (single app) |
|---|---|---|
| room_show c=1 req/s | 39.5 [39.4–40.8] | – |
| room_show c=1 p50 ms | 24.8 [24.3–25.3] | – |
| room_show c=1 p99 ms | 27.2 [26.8–34.0] | – |
| room_show c=1 CPU µs/success | 25,064 [24,351–25,209] | – |
| room_show c=16 req/s | 145 [140–148] | – |
| room_show c=16 p50 ms | 105 [99–105] | – |
| room_show c=16 p99 ms | 139 [136–187] | – |
| room_show c=16 CPU µs/success | 27,453 [26,929–28,462] | – |
| room_show c=64 req/s | 158 [141–162] | – |
| room_show c=64 p50 ms | 401 [393–452] | – |
| room_show c=64 p99 ms | 430 [415–515] | – |
| room_show c=64 CPU µs/success | 25,161 [24,637–28,278] | – |
| messages_page c=1 req/s | 42.2 [41.9–42.7] | – |
| messages_page c=1 p50 ms | 23.3 [23.3–23.7] | – |
| messages_page c=1 p99 ms | 29.2 [25.6–29.4] | – |
| messages_page c=1 CPU µs/success | 23,512 [23,241–23,735] | – |
| messages_page c=16 req/s | 159 [139–168] | – |
| messages_page c=16 p50 ms | 98.5 [94.0–105.7] | – |
| messages_page c=16 p99 ms | 119 [112–175] | – |
| messages_page c=16 CPU µs/success | 25,098 [23,740–28,653] | – |
| messages_page c=64 req/s | 147 [110–147] | – |
| messages_page c=64 p50 ms | 427 [424–613] | – |
| messages_page c=64 p99 ms | 519 [487–652] | – |
| messages_page c=64 CPU µs/success | 27,128 [27,004–36,202] | – |
| sidebar c=1 req/s | 196 [129–198] | – |
| sidebar c=1 p50 ms | 5.03 [4.92–8.26] | – |
| sidebar c=1 p99 ms | 7.29 [6.97–11.34] | – |
| sidebar c=1 CPU µs/success | 5,062 [4,986–7,361] | – |
| sidebar c=16 req/s | 780 [653–781] | – |
| sidebar c=16 p50 ms | 20.1 [20.0–23.9] | – |
| sidebar c=16 p99 ms | 28.9 [24.9–33.0] | – |
| sidebar c=16 CPU µs/success | 5,109 [5,102–6,081] | – |
| sidebar c=64 req/s | 787 [756–801] | – |
| sidebar c=64 p50 ms | 80.0 [79.5–83.4] | – |
| sidebar c=64 p99 ms | 96.0 [85.1–103.9] | – |
| sidebar c=64 CPU µs/success | 5,057 [4,966–5,262] | – |
| search c=1 req/s | 106 [105–109] | – |
| search c=1 p50 ms | 9.13 [9.10–9.20] | – |
| search c=1 p99 ms | 13.3 [11.0–15.5] | – |
| search c=1 CPU µs/success | 9,318 [9,139–9,422] | – |
| search c=16 req/s | 389 [355–402] | – |
| search c=16 p50 ms | 37.8 [37.1–45.0] | – |
| search c=16 p99 ms | 55.5 [51.7–65.1] | – |
| search c=16 CPU µs/success | 10,216 [9,900–11,217] | – |
| search c=64 req/s | 392 [329–408] | – |
| search c=64 p50 ms | 161 [152–194] | – |
| search c=64 p99 ms | 195 [188–223] | – |
| search c=64 CPU µs/success | 10,158 [9,745–12,086] | – |
| avatar c=1 req/s | 869 [843–883] | – |
| avatar c=1 p50 ms | 1.10 [1.09–1.11] | – |
| avatar c=1 p99 ms | 1.88 [1.79–1.89] | – |
| avatar c=1 CPU µs/success | 1,121 [1,103–1,155] | – |
| avatar c=16 req/s | 3,322 [3,088–3,504] | – |
| avatar c=16 p50 ms | 4.63 [4.50–5.04] | – |
| avatar c=16 p99 ms | 7.25 [6.58–7.60] | – |
| avatar c=16 CPU µs/success | 1,188 [1,130–1,269] | – |
| avatar c=64 req/s | 3,267 [3,226–3,513] | – |
| avatar c=64 p50 ms | 18.0 [17.9–19.8] | – |
| avatar c=64 p99 ms | 25.1 [22.5–31.4] | – |
| avatar c=64 CPU µs/success | 1,196 [1,119–1,212] | – |
| static_css c=1 req/s | 30,924 [21,933–34,630] | – |
| static_css c=1 p50 ms | 0.03 [0.03–0.03] | – |
| static_css c=1 p99 ms | 0.05 [0.04–0.36] | – |
| static_css c=1 CPU µs/success | 24.1 [20.2–26.1] | – |
| static_css c=16 req/s | 79,180 [76,428–86,039] | – |
| static_css c=16 p50 ms | 0.16 [0.15–0.20] | – |
| static_css c=16 p99 ms | 1.10 [0.45–1.93] | – |
| static_css c=16 CPU µs/success | 13.7 [12.7–16.2] | – |
| static_css c=64 req/s | 96,025 [78,007–99,216] | – |
| static_css c=64 p50 ms | 0.60 [0.59–0.81] | – |
| static_css c=64 p99 ms | 2.57 [1.94–2.62] | – |
| static_css c=64 CPU µs/success | 12.2 [11.8–15.3] | – |
| up c=1 req/s | 36,141 [28,475–37,146] | – |
| up c=1 p50 ms | 0.02 [0.02–0.03] | – |
| up c=1 p99 ms | 0.04 [0.04–0.07] | – |
| up c=1 CPU µs/success | 17.9 [17.1–21.5] | – |
| up c=16 req/s | 155,738 [150,680–165,355] | – |
| up c=16 p50 ms | 0.09 [0.09–0.10] | – |
| up c=16 p99 ms | 0.17 [0.14–0.18] | – |
| up c=16 CPU µs/success | 11.8 [11.0–13.0] | – |
| up c=64 req/s | 171,222 [164,496–181,975] | – |
| up c=64 p50 ms | 0.36 [0.36–0.37] | – |
| up c=64 p99 ms | 0.57 [0.50–0.68] | – |
| up c=64 CPU µs/success | 11.0 [10.0–12.1] | – |
| post_message c=1 req/s | 855 [853–880] | – |
| post_message c=1 p50 ms | 1.11 [1.10–1.12] | – |
| post_message c=1 p99 ms | 1.89 [1.78–1.91] | – |
| post_message c=1 CPU µs/success | 1,138 [1,107–1,140] | – |
| post_message c=16 req/s | 3,541 [3,360–3,546] | – |
| post_message c=16 p50 ms | 4.49 [4.49–4.62] | – |
| post_message c=16 p99 ms | 6.74 [6.47–7.71] | – |
| post_message c=16 CPU µs/success | 1,117 [1,113–1,179] | – |
| post_message c=64 req/s | 3,546 [3,243–3,557] | – |
| post_message c=64 p50 ms | 17.8 [17.7–19.6] | – |
| post_message c=64 p99 ms | 22.4 [22.4–22.6] | – |
| post_message c=64 CPU µs/success | 1,112 [1,110–1,223] | – |

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
