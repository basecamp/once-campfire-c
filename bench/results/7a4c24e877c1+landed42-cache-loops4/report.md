```
date: 2026-10-05T17:55:12-04:00
host: 7.1.0-0.rc7.260611g9716c086c8e8.50.fc45.x86_64, AMD Ryzen 9 9900X 12-Core Processor, 24 threads, 91GB
server cpus: 8-11 (nproc 4); loadgen cpus: 12-15; network: host
env: WEB_CONCURRENCY=3 JOB_CONCURRENCY=3 RAILS_MAX_THREADS=5 
seed sha256: a763014643b8e26d38f153e98647500e42ef2c305fdbaf24145fccdbcb9ae030  /home/msaraiva/dev/mark/Proj/Misc/once-campfire-c/bench/seed/.seed/default/db/production.sqlite3
c revision: 7a4c24e877c1+landed42 binary sha256: 65d877ad9dccbabc49efc323c1a288cdd69d035ad80e9afe12d734145faa2d5a cache_bytes: 67108864 loops: 4 image: campfire-c:bench
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
| cold start: docker run → /up 200 (ms) | 118 [111–123] | – |
| idle memory.current (MiB) | 31.0 [30.0–31.0] | – |
| idle anon (MiB) | 30.0 [30.0–30.0] | – |
| peak memory.current under load (MiB) | 37.0 [37.0–37.0] | – |
| peak anon under load (MiB) | 34.0 [34.0–34.0] | – |

### HTTP (signed in as david; keep-alive; c = concurrent connections)

| Metric | C | C (single app) |
|---|---|---|
| room_show c=1 req/s | 1,802 [1,777–1,849] | – |
| room_show c=1 p50 ms | 0.54 [0.53–0.55] | – |
| room_show c=1 p99 ms | 0.81 [0.66–0.84] | – |
| room_show c=1 CPU µs/success | 537 [527–544] | – |
| room_show c=16 req/s | 7,294 [7,137–7,344] | – |
| room_show c=16 p50 ms | 2.20 [2.15–2.21] | – |
| room_show c=16 p99 ms | 4.02 [4.01–4.47] | – |
| room_show c=16 CPU µs/success | 535 [534–545] | – |
| room_show c=64 req/s | 7,253 [7,220–7,340] | – |
| room_show c=64 p50 ms | 8.64 [8.64–8.74] | – |
| room_show c=64 p99 ms | 11.6 [11.2–11.9] | – |
| room_show c=64 CPU µs/success | 534 [534–543] | – |
| messages_page c=1 req/s | 1,840 [1,816–1,842] | – |
| messages_page c=1 p50 ms | 0.53 [0.53–0.54] | – |
| messages_page c=1 p99 ms | 0.70 [0.67–0.71] | – |
| messages_page c=1 CPU µs/success | 531 [530–538] | – |
| messages_page c=16 req/s | 7,300 [7,212–7,327] | – |
| messages_page c=16 p50 ms | 2.19 [2.15–2.22] | – |
| messages_page c=16 p99 ms | 4.28 [3.84–4.44] | – |
| messages_page c=16 CPU µs/success | 535 [531–538] | – |
| messages_page c=64 req/s | 7,309 [7,290–7,407] | – |
| messages_page c=64 p50 ms | 8.67 [8.52–8.68] | – |
| messages_page c=64 p99 ms | 11.3 [11.2–11.4] | – |
| messages_page c=64 CPU µs/success | 536 [531–538] | – |
| sidebar c=1 req/s | 1,841 [1,833–1,860] | – |
| sidebar c=1 p50 ms | 0.53 [0.53–0.54] | – |
| sidebar c=1 p99 ms | 0.66 [0.64–0.69] | – |
| sidebar c=1 CPU µs/success | 530 [525–532] | – |
| sidebar c=16 req/s | 7,286 [7,142–7,326] | – |
| sidebar c=16 p50 ms | 2.21 [2.14–2.26] | – |
| sidebar c=16 p99 ms | 4.15 [4.09–4.23] | – |
| sidebar c=16 CPU µs/success | 533 [528–534] | – |
| sidebar c=64 req/s | 7,306 [7,252–7,312] | – |
| sidebar c=64 p50 ms | 8.62 [8.61–8.63] | – |
| sidebar c=64 p99 ms | 11.7 [11.6–11.8] | – |
| sidebar c=64 CPU µs/success | 535 [531–537] | – |
| search c=1 req/s | 1,821 [1,790–1,862] | – |
| search c=1 p50 ms | 0.54 [0.53–0.54] | – |
| search c=1 p99 ms | 0.68 [0.65–0.90] | – |
| search c=1 CPU µs/success | 536 [526–542] | – |
| search c=16 req/s | 7,327 [7,211–7,336] | – |
| search c=16 p50 ms | 2.16 [2.12–2.23] | – |
| search c=16 p99 ms | 4.23 [4.03–4.40] | – |
| search c=16 CPU µs/success | 533 [533–540] | – |
| search c=64 req/s | 7,336 [7,251–7,355] | – |
| search c=64 p50 ms | 8.64 [8.56–8.66] | – |
| search c=64 p99 ms | 11.6 [11.2–11.8] | – |
| search c=64 CPU µs/success | 534 [529–535] | – |
| avatar c=1 req/s | 952 [938–956] | – |
| avatar c=1 p50 ms | 1.03 [1.03–1.05] | – |
| avatar c=1 p99 ms | 1.27 [1.26–1.33] | – |
| avatar c=1 CPU µs/success | 1,037 [1,033–1,051] | – |
| avatar c=16 req/s | 3,748 [3,637–3,787] | – |
| avatar c=16 p50 ms | 4.21 [4.17–4.21] | – |
| avatar c=16 p99 ms | 6.13 [6.06–7.63] | – |
| avatar c=16 CPU µs/success | 1,055 [1,039–1,074] | – |
| avatar c=64 req/s | 3,721 [3,665–3,790] | – |
| avatar c=64 p50 ms | 16.9 [16.8–17.1] | – |
| avatar c=64 p99 ms | 24.5 [19.7–26.1] | – |
| avatar c=64 CPU µs/success | 1,065 [1,041–1,074] | – |
| static_css c=1 req/s | 40,223 [38,237–40,733] | – |
| static_css c=1 p50 ms | 0.02 [0.02–0.03] | – |
| static_css c=1 p99 ms | 0.03 [0.03–0.03] | – |
| static_css c=1 CPU µs/success | 19.5 [19.2–19.5] | – |
| static_css c=16 req/s | 139,683 [137,659–141,742] | – |
| static_css c=16 p50 ms | 0.12 [0.12–0.12] | – |
| static_css c=16 p99 ms | 0.19 [0.18–0.21] | – |
| static_css c=16 CPU µs/success | 17.1 [16.9–17.1] | – |
| static_css c=64 req/s | 153,727 [142,607–158,384] | – |
| static_css c=64 p50 ms | 0.46 [0.46–0.48] | – |
| static_css c=64 p99 ms | 0.75 [0.73–0.77] | – |
| static_css c=64 CPU µs/success | 13.4 [12.9–14.3] | – |
| up c=1 req/s | 44,950 [43,818–45,488] | – |
| up c=1 p50 ms | 0.02 [0.02–0.02] | – |
| up c=1 p99 ms | 0.03 [0.03–0.03] | – |
| up c=1 CPU µs/success | 16.0 [15.9–16.2] | – |
| up c=16 req/s | 181,959 [173,147–199,372] | – |
| up c=16 p50 ms | 0.09 [0.09–0.10] | – |
| up c=16 p99 ms | 0.14 [0.12–0.17] | – |
| up c=16 CPU µs/success | 14.3 [11.9–15.1] | – |
| up c=64 req/s | 203,623 [198,964–204,981] | – |
| up c=64 p50 ms | 0.37 [0.36–0.37] | – |
| up c=64 p99 ms | 0.52 [0.52–0.52] | – |
| up c=64 CPU µs/success | 12.2 [11.8–12.9] | – |
| post_message c=1 req/s | 936 [921–941] | – |
| post_message c=1 p50 ms | 1.05 [1.05–1.06] | – |
| post_message c=1 p99 ms | 1.40 [1.26–1.60] | – |
| post_message c=1 CPU µs/success | 1,051 [1,044–1,065] | – |
| post_message c=16 req/s | 3,717 [3,678–3,752] | – |
| post_message c=16 p50 ms | 4.28 [4.21–4.29] | – |
| post_message c=16 p99 ms | 5.98 [5.92–6.79] | – |
| post_message c=16 CPU µs/success | 1,065 [1,055–1,071] | – |
| post_message c=64 req/s | 3,714 [3,662–3,730] | – |
| post_message c=64 p50 ms | 17.1 [17.0–17.2] | – |
| post_message c=64 p99 ms | 20.2 [19.5–21.7] | – |
| post_message c=64 CPU µs/success | 1,068 [1,062–1,077] | – |

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
