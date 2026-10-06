```
date: 2026-10-05T18:54:19-04:00
host: 7.1.0-0.rc7.260611g9716c086c8e8.50.fc45.x86_64, AMD Ryzen 9 9900X 12-Core Processor, 24 threads, 91GB
server cpus: 8-11 (nproc 4); loadgen cpus: 12-15; network: host
env: WEB_CONCURRENCY=3 JOB_CONCURRENCY=3 RAILS_MAX_THREADS=5 
seed sha256: a763014643b8e26d38f153e98647500e42ef2c305fdbaf24145fccdbcb9ae030  /home/msaraiva/dev/mark/Proj/Misc/once-campfire-c/bench/seed/.seed/default/db/production.sqlite3
c revision: 604f037c0e25+dirty10 binary sha256: 48f93d27594a4c65bdcab845993b31ef0f526d9d646aaf3831d0a6e168b01f6e cache_bytes: 0 loops: 4 image: campfire-c:bench
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
| cold start: docker run → /up 200 (ms) | 122 [119–185] | – |
| idle memory.current (MiB) | 30.0 [30.0–30.0] | – |
| idle anon (MiB) | 30.0 [30.0–30.0] | – |
| peak memory.current under load (MiB) | 39.0 [39.0–39.0] | – |
| peak anon under load (MiB) | 35.0 [35.0–36.0] | – |

### HTTP (signed in as david; keep-alive; c = concurrent connections)

| Metric | C | C (single app) |
|---|---|---|
| room_show c=1 req/s | 547 [546–556] | – |
| room_show c=1 p50 ms | 1.82 [1.79–1.83] | – |
| room_show c=1 p99 ms | 2.12 [2.06–2.53] | – |
| room_show c=1 CPU µs/success | 1,815 [1,784–1,817] | – |
| room_show c=16 req/s | 2,041 [1,870–2,048] | – |
| room_show c=16 p50 ms | 7.80 [7.79–7.80] | – |
| room_show c=16 p99 ms | 9.44 [9.03–13.47] | – |
| room_show c=16 CPU µs/success | 1,927 [1,920–2,098] | – |
| room_show c=64 req/s | 2,055 [2,033–2,076] | – |
| room_show c=64 p50 ms | 31.0 [30.7–31.4] | – |
| room_show c=64 p99 ms | 33.2 [33.0–33.2] | – |
| room_show c=64 CPU µs/success | 1,913 [1,890–1,930] | – |
| messages_page c=1 req/s | 684 [676–686] | – |
| messages_page c=1 p50 ms | 1.44 [1.44–1.44] | – |
| messages_page c=1 p99 ms | 1.85 [1.75–2.29] | – |
| messages_page c=1 CPU µs/success | 1,445 [1,439–1,458] | – |
| messages_page c=16 req/s | 2,474 [2,473–2,478] | – |
| messages_page c=16 p50 ms | 6.43 [6.42–6.45] | – |
| messages_page c=16 p99 ms | 7.58 [7.29–7.63] | – |
| messages_page c=16 CPU µs/success | 1,582 [1,581–1,586] | – |
| messages_page c=64 req/s | 2,481 [2,467–2,489] | – |
| messages_page c=64 p50 ms | 25.7 [25.6–25.9] | – |
| messages_page c=64 p99 ms | 27.4 [27.4–27.5] | – |
| messages_page c=64 CPU µs/success | 1,580 [1,573–1,588] | – |
| sidebar c=1 req/s | 6,244 [6,184–6,386] | – |
| sidebar c=1 p50 ms | 0.15 [0.15–0.16] | – |
| sidebar c=1 p99 ms | 0.25 [0.24–0.25] | – |
| sidebar c=1 CPU µs/success | 150 [146–151] | – |
| sidebar c=16 req/s | 24,915 [24,640–25,223] | – |
| sidebar c=16 p50 ms | 0.62 [0.61–0.63] | – |
| sidebar c=16 p99 ms | 1.17 [1.10–1.22] | – |
| sidebar c=16 CPU µs/success | 151 [149–154] | – |
| sidebar c=64 req/s | 24,814 [24,799–25,570] | – |
| sidebar c=64 p50 ms | 2.55 [2.47–2.55] | – |
| sidebar c=64 p99 ms | 3.45 [3.42–3.76] | – |
| sidebar c=64 CPU µs/success | 152 [148–153] | – |
| search c=1 req/s | 1,534 [1,518–1,556] | – |
| search c=1 p50 ms | 0.63 [0.62–0.64] | – |
| search c=1 p99 ms | 0.97 [0.95–1.03] | – |
| search c=1 CPU µs/success | 636 [629–645] | – |
| search c=16 req/s | 5,524 [5,488–5,667] | – |
| search c=16 p50 ms | 2.87 [2.81–2.90] | – |
| search c=16 p99 ms | 4.02 [3.72–4.07] | – |
| search c=16 CPU µs/success | 698 [681–703] | – |
| search c=64 req/s | 5,587 [5,521–5,631] | – |
| search c=64 p50 ms | 11.3 [11.3–11.5] | – |
| search c=64 p99 ms | 13.4 [12.8–13.9] | – |
| search c=64 CPU µs/success | 690 [684–698] | – |
| avatar c=1 req/s | 30,683 [30,433–31,615] | – |
| avatar c=1 p50 ms | 0.03 [0.03–0.03] | – |
| avatar c=1 p99 ms | 0.04 [0.04–0.05] | – |
| avatar c=1 CPU µs/success | 25.5 [25.1–25.5] | – |
| avatar c=16 req/s | 157,934 [149,777–159,439] | – |
| avatar c=16 p50 ms | 0.09 [0.09–0.09] | – |
| avatar c=16 p99 ms | 0.28 [0.26–0.56] | – |
| avatar c=16 CPU µs/success | 21.6 [21.2–21.8] | – |
| avatar c=64 req/s | 172,649 [168,770–173,601] | – |
| avatar c=64 p50 ms | 0.38 [0.37–0.38] | – |
| avatar c=64 p99 ms | 0.78 [0.70–0.79] | – |
| avatar c=64 CPU µs/success | 20.5 [20.2–20.6] | – |
| static_css c=1 req/s | 39,151 [38,882–39,195] | – |
| static_css c=1 p50 ms | 0.02 [0.02–0.02] | – |
| static_css c=1 p99 ms | 0.03 [0.03–0.03] | – |
| static_css c=1 CPU µs/success | 19.2 [19.1–19.2] | – |
| static_css c=16 req/s | 142,479 [134,136–143,294] | – |
| static_css c=16 p50 ms | 0.11 [0.11–0.12] | – |
| static_css c=16 p99 ms | 0.20 [0.17–0.38] | – |
| static_css c=16 CPU µs/success | 16.1 [15.4–17.7] | – |
| static_css c=64 req/s | 160,332 [157,238–165,116] | – |
| static_css c=64 p50 ms | 0.45 [0.44–0.45] | – |
| static_css c=64 p99 ms | 0.73 [0.72–0.74] | – |
| static_css c=64 CPU µs/success | 13.9 [13.2–14.8] | – |
| up c=1 req/s | 45,185 [45,002–45,394] | – |
| up c=1 p50 ms | 0.02 [0.02–0.02] | – |
| up c=1 p99 ms | 0.03 [0.03–0.03] | – |
| up c=1 CPU µs/success | 15.9 [15.9–16.1] | – |
| up c=16 req/s | 187,581 [183,062–231,023] | – |
| up c=16 p50 ms | 0.09 [0.06–0.09] | – |
| up c=16 p99 ms | 0.13 [0.12–0.20] | – |
| up c=16 CPU µs/success | 13.2 [12.1–14.5] | – |
| up c=64 req/s | 213,675 [208,383–289,578] | – |
| up c=64 p50 ms | 0.35 [0.17–0.35] | – |
| up c=64 p99 ms | 0.49 [0.48–0.74] | – |
| up c=64 CPU µs/success | 11.7 [10.8–12.3] | – |
| post_message c=1 req/s | 24,225 [24,192–24,931] | – |
| post_message c=1 p50 ms | 0.04 [0.04–0.04] | – |
| post_message c=1 p99 ms | 0.06 [0.05–0.06] | – |
| post_message c=1 CPU µs/success | 31.7 [31.0–32.0] | – |
| post_message c=16 req/s | 111,766 [110,265–114,231] | – |
| post_message c=16 p50 ms | 0.13 [0.13–0.14] | – |
| post_message c=16 p99 ms | 0.28 [0.23–0.28] | – |
| post_message c=16 CPU µs/success | 31.7 [30.4–31.9] | – |
| post_message c=64 req/s | 112,472 [109,971–113,013] | – |
| post_message c=64 p50 ms | 0.57 [0.57–0.57] | – |
| post_message c=64 p99 ms | 0.88 [0.77–0.91] | – |
| post_message c=64 CPU µs/success | 31.5 [31.5–32.2] | – |

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
