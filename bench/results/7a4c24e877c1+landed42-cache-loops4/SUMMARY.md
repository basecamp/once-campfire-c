## bench/results/7a4c24e877c1+landed42-cache-loops4
reps per app: c 3

### HTTP (median [min–max] across reps)

| Route | conc | c |
|---|---:|---|
| room_show req/s | 1 | 1,802 [1,777–1,849] |
| room_show p50 ms | 1 | 0.5 [0.5–0.5] |
| room_show p90 ms | 1 | 0.6 [0.6–0.6] |
| room_show p99 ms | 1 | 0.8 [0.7–0.8] |
| room_show CPU µs/ok | 1 | 537 [527–544] |
| room_show req/s | 16 | 7,294 [7,137–7,344] |
| room_show p50 ms | 16 | 2.2 [2.1–2.2] |
| room_show p90 ms | 16 | 2.6 [2.4–2.7] |
| room_show p99 ms | 16 | 4.0 [4.0–4.5] |
| room_show CPU µs/ok | 16 | 535 [534–545] |
| room_show req/s | 64 | 7,253 [7,220–7,340] |
| room_show p50 ms | 64 | 8.6 [8.6–8.7] |
| room_show p90 ms | 64 | 9.5 [9.3–9.9] |
| room_show p99 ms | 64 | 11.6 [11.2–11.9] |
| room_show CPU µs/ok | 64 | 534 [534–543] |
| messages_page req/s | 1 | 1,840 [1,816–1,842] |
| messages_page p50 ms | 1 | 0.5 [0.5–0.5] |
| messages_page p90 ms | 1 | 0.6 [0.6–0.6] |
| messages_page p99 ms | 1 | 0.7 [0.7–0.7] |
| messages_page CPU µs/ok | 1 | 531 [530–538] |
| messages_page req/s | 16 | 7,300 [7,212–7,327] |
| messages_page p50 ms | 16 | 2.2 [2.2–2.2] |
| messages_page p90 ms | 16 | 2.5 [2.5–2.6] |
| messages_page p99 ms | 16 | 4.3 [3.8–4.4] |
| messages_page CPU µs/ok | 16 | 535 [531–538] |
| messages_page req/s | 64 | 7,309 [7,290–7,407] |
| messages_page p50 ms | 64 | 8.7 [8.5–8.7] |
| messages_page p90 ms | 64 | 9.3 [9.2–9.4] |
| messages_page p99 ms | 64 | 11.3 [11.2–11.4] |
| messages_page CPU µs/ok | 64 | 536 [531–538] |
| sidebar req/s | 1 | 1,841 [1,833–1,860] |
| sidebar p50 ms | 1 | 0.5 [0.5–0.5] |
| sidebar p90 ms | 1 | 0.6 [0.6–0.6] |
| sidebar p99 ms | 1 | 0.7 [0.6–0.7] |
| sidebar CPU µs/ok | 1 | 530 [525–532] |
| sidebar req/s | 16 | 7,286 [7,142–7,326] |
| sidebar p50 ms | 16 | 2.2 [2.1–2.3] |
| sidebar p90 ms | 16 | 2.6 [2.5–2.6] |
| sidebar p99 ms | 16 | 4.2 [4.1–4.2] |
| sidebar CPU µs/ok | 16 | 533 [528–534] |
| sidebar req/s | 64 | 7,306 [7,252–7,312] |
| sidebar p50 ms | 64 | 8.6 [8.6–8.6] |
| sidebar p90 ms | 64 | 9.7 [9.5–9.9] |
| sidebar p99 ms | 64 | 11.7 [11.6–11.8] |
| sidebar CPU µs/ok | 64 | 535 [531–537] |
| search req/s | 1 | 1,821 [1,790–1,862] |
| search p50 ms | 1 | 0.5 [0.5–0.5] |
| search p90 ms | 1 | 0.6 [0.6–0.6] |
| search p99 ms | 1 | 0.7 [0.6–0.9] |
| search CPU µs/ok | 1 | 536 [526–542] |
| search req/s | 16 | 7,327 [7,211–7,336] |
| search p50 ms | 16 | 2.2 [2.1–2.2] |
| search p90 ms | 16 | 2.6 [2.5–2.6] |
| search p99 ms | 16 | 4.2 [4.0–4.4] |
| search CPU µs/ok | 16 | 533 [533–540] |
| search req/s | 64 | 7,336 [7,251–7,355] |
| search p50 ms | 64 | 8.6 [8.6–8.7] |
| search p90 ms | 64 | 9.5 [9.4–9.8] |
| search p99 ms | 64 | 11.6 [11.2–11.8] |
| search CPU µs/ok | 64 | 534 [529–535] |
| avatar req/s | 1 | 952 [938–956] |
| avatar p50 ms | 1 | 1.0 [1.0–1.1] |
| avatar p90 ms | 1 | 1.1 [1.1–1.1] |
| avatar p99 ms | 1 | 1.3 [1.3–1.3] |
| avatar CPU µs/ok | 1 | 1,037 [1,033–1,051] |
| avatar req/s | 16 | 3,748 [3,637–3,787] |
| avatar p50 ms | 16 | 4.2 [4.2–4.2] |
| avatar p90 ms | 16 | 4.6 [4.5–5.0] |
| avatar p99 ms | 16 | 6.1 [6.1–7.6] |
| avatar CPU µs/ok | 16 | 1,055 [1,039–1,074] |
| avatar req/s | 64 | 3,721 [3,665–3,790] |
| avatar p50 ms | 64 | 16.9 [16.8–17.1] |
| avatar p90 ms | 64 | 17.6 [17.5–18.7] |
| avatar p99 ms | 64 | 24.5 [19.7–26.1] |
| avatar CPU µs/ok | 64 | 1,065 [1,041–1,074] |
| static_css req/s | 1 | 40,223 [38,237–40,733] |
| static_css p50 ms | 1 | 0.0 [0.0–0.0] |
| static_css p90 ms | 1 | 0.0 [0.0–0.0] |
| static_css p99 ms | 1 | 0.0 [0.0–0.0] |
| static_css CPU µs/ok | 1 | 19 [19–20] |
| static_css req/s | 16 | 139,683 [137,659–141,742] |
| static_css p50 ms | 16 | 0.1 [0.1–0.1] |
| static_css p90 ms | 16 | 0.1 [0.1–0.1] |
| static_css p99 ms | 16 | 0.2 [0.2–0.2] |
| static_css CPU µs/ok | 16 | 17 [17–17] |
| static_css req/s | 64 | 153,727 [142,607–158,384] |
| static_css p50 ms | 64 | 0.5 [0.5–0.5] |
| static_css p90 ms | 64 | 0.5 [0.5–0.6] |
| static_css p99 ms | 64 | 0.7 [0.7–0.8] |
| static_css CPU µs/ok | 64 | 13 [13–14] |
| up req/s | 1 | 44,950 [43,818–45,488] |
| up p50 ms | 1 | 0.0 [0.0–0.0] |
| up p90 ms | 1 | 0.0 [0.0–0.0] |
| up p99 ms | 1 | 0.0 [0.0–0.0] |
| up CPU µs/ok | 1 | 16 [16–16] |
| up req/s | 16 | 181,959 [173,147–199,372] |
| up p50 ms | 16 | 0.1 [0.1–0.1] |
| up p90 ms | 16 | 0.1 [0.1–0.1] |
| up p99 ms | 16 | 0.1 [0.1–0.2] |
| up CPU µs/ok | 16 | 14 [12–15] |
| up req/s | 64 | 203,623 [198,964–204,981] |
| up p50 ms | 64 | 0.4 [0.4–0.4] |
| up p90 ms | 64 | 0.4 [0.4–0.4] |
| up p99 ms | 64 | 0.5 [0.5–0.5] |
| up CPU µs/ok | 64 | 12 [12–13] |
| post_message req/s | 1 | 936 [921–941] |
| post_message p50 ms | 1 | 1.0 [1.0–1.1] |
| post_message p90 ms | 1 | 1.1 [1.1–1.1] |
| post_message p99 ms | 1 | 1.4 [1.3–1.6] |
| post_message CPU µs/ok | 1 | 1,051 [1,044–1,065] |
| post_message req/s | 16 | 3,717 [3,678–3,752] |
| post_message p50 ms | 16 | 4.3 [4.2–4.3] |
| post_message p90 ms | 16 | 4.8 [4.7–4.9] |
| post_message p99 ms | 16 | 6.0 [5.9–6.8] |
| post_message CPU µs/ok | 16 | 1,065 [1,055–1,071] |
| post_message req/s | 64 | 3,714 [3,662–3,730] |
| post_message p50 ms | 64 | 17.1 [17.0–17.2] |
| post_message p90 ms | 64 | 18.2 [17.9–18.9] |
| post_message p99 ms | 64 | 20.2 [19.5–21.7] |
| post_message CPU µs/ok | 64 | 1,068 [1,062–1,077] |

