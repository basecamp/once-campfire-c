## bench/results/604f037c0e25+dirty10-uncached-loops4
reps per app: c 3

### HTTP (median [min–max] across reps)

| Route | conc | c |
|---|---:|---|
| room_show req/s | 1 | 547 [546–556] |
| room_show p50 ms | 1 | 1.8 [1.8–1.8] |
| room_show p90 ms | 1 | 1.9 [1.9–1.9] |
| room_show p99 ms | 1 | 2.1 [2.1–2.5] |
| room_show CPU µs/ok | 1 | 1,815 [1,784–1,817] |
| room_show req/s | 16 | 2,041 [1,870–2,048] |
| room_show p50 ms | 16 | 7.8 [7.8–7.8] |
| room_show p90 ms | 16 | 8.3 [8.2–11.8] |
| room_show p99 ms | 16 | 9.4 [9.0–13.5] |
| room_show CPU µs/ok | 16 | 1,927 [1,920–2,098] |
| room_show req/s | 64 | 2,055 [2,033–2,076] |
| room_show p50 ms | 64 | 31.0 [30.7–31.4] |
| room_show p90 ms | 64 | 32.1 [31.7–32.5] |
| room_show p99 ms | 64 | 33.2 [33.0–33.2] |
| room_show CPU µs/ok | 64 | 1,913 [1,890–1,930] |
| messages_page req/s | 1 | 684 [676–686] |
| messages_page p50 ms | 1 | 1.4 [1.4–1.4] |
| messages_page p90 ms | 1 | 1.5 [1.5–1.6] |
| messages_page p99 ms | 1 | 1.9 [1.8–2.3] |
| messages_page CPU µs/ok | 1 | 1,445 [1,439–1,458] |
| messages_page req/s | 16 | 2,474 [2,473–2,478] |
| messages_page p50 ms | 16 | 6.4 [6.4–6.4] |
| messages_page p90 ms | 16 | 6.8 [6.8–6.8] |
| messages_page p99 ms | 16 | 7.6 [7.3–7.6] |
| messages_page CPU µs/ok | 16 | 1,582 [1,581–1,586] |
| messages_page req/s | 64 | 2,481 [2,467–2,489] |
| messages_page p50 ms | 64 | 25.7 [25.6–25.9] |
| messages_page p90 ms | 64 | 26.6 [26.5–26.7] |
| messages_page p99 ms | 64 | 27.4 [27.4–27.5] |
| messages_page CPU µs/ok | 64 | 1,580 [1,573–1,588] |
| sidebar req/s | 1 | 6,244 [6,184–6,386] |
| sidebar p50 ms | 1 | 0.2 [0.2–0.2] |
| sidebar p90 ms | 1 | 0.2 [0.2–0.2] |
| sidebar p99 ms | 1 | 0.2 [0.2–0.3] |
| sidebar CPU µs/ok | 1 | 150 [146–151] |
| sidebar req/s | 16 | 24,915 [24,640–25,223] |
| sidebar p50 ms | 16 | 0.6 [0.6–0.6] |
| sidebar p90 ms | 16 | 0.7 [0.7–0.8] |
| sidebar p99 ms | 16 | 1.2 [1.1–1.2] |
| sidebar CPU µs/ok | 16 | 151 [149–154] |
| sidebar req/s | 64 | 24,814 [24,799–25,570] |
| sidebar p50 ms | 64 | 2.6 [2.5–2.6] |
| sidebar p90 ms | 64 | 2.8 [2.7–2.8] |
| sidebar p99 ms | 64 | 3.4 [3.4–3.8] |
| sidebar CPU µs/ok | 64 | 152 [148–153] |
| search req/s | 1 | 1,534 [1,518–1,556] |
| search p50 ms | 1 | 0.6 [0.6–0.6] |
| search p90 ms | 1 | 0.7 [0.7–0.7] |
| search p99 ms | 1 | 1.0 [0.9–1.0] |
| search CPU µs/ok | 1 | 636 [629–645] |
| search req/s | 16 | 5,524 [5,488–5,667] |
| search p50 ms | 16 | 2.9 [2.8–2.9] |
| search p90 ms | 16 | 3.2 [3.1–3.2] |
| search p99 ms | 16 | 4.0 [3.7–4.1] |
| search CPU µs/ok | 16 | 698 [681–703] |
| search req/s | 64 | 5,587 [5,521–5,631] |
| search p50 ms | 64 | 11.3 [11.3–11.5] |
| search p90 ms | 64 | 12.1 [12.0–12.3] |
| search p99 ms | 64 | 13.4 [12.8–13.9] |
| search CPU µs/ok | 64 | 690 [684–698] |
| avatar req/s | 1 | 30,683 [30,433–31,615] |
| avatar p50 ms | 1 | 0.0 [0.0–0.0] |
| avatar p90 ms | 1 | 0.0 [0.0–0.0] |
| avatar p99 ms | 1 | 0.0 [0.0–0.0] |
| avatar CPU µs/ok | 1 | 25 [25–25] |
| avatar req/s | 16 | 157,934 [149,777–159,439] |
| avatar p50 ms | 16 | 0.1 [0.1–0.1] |
| avatar p90 ms | 16 | 0.1 [0.1–0.1] |
| avatar p99 ms | 16 | 0.3 [0.3–0.6] |
| avatar CPU µs/ok | 16 | 22 [21–22] |
| avatar req/s | 64 | 172,649 [168,770–173,601] |
| avatar p50 ms | 64 | 0.4 [0.4–0.4] |
| avatar p90 ms | 64 | 0.5 [0.4–0.5] |
| avatar p99 ms | 64 | 0.8 [0.7–0.8] |
| avatar CPU µs/ok | 64 | 20 [20–21] |
| static_css req/s | 1 | 39,151 [38,882–39,195] |
| static_css p50 ms | 1 | 0.0 [0.0–0.0] |
| static_css p90 ms | 1 | 0.0 [0.0–0.0] |
| static_css p99 ms | 1 | 0.0 [0.0–0.0] |
| static_css CPU µs/ok | 1 | 19 [19–19] |
| static_css req/s | 16 | 142,479 [134,136–143,294] |
| static_css p50 ms | 16 | 0.1 [0.1–0.1] |
| static_css p90 ms | 16 | 0.1 [0.1–0.1] |
| static_css p99 ms | 16 | 0.2 [0.2–0.4] |
| static_css CPU µs/ok | 16 | 16 [15–18] |
| static_css req/s | 64 | 160,332 [157,238–165,116] |
| static_css p50 ms | 64 | 0.4 [0.4–0.5] |
| static_css p90 ms | 64 | 0.5 [0.5–0.5] |
| static_css p99 ms | 64 | 0.7 [0.7–0.7] |
| static_css CPU µs/ok | 64 | 14 [13–15] |
| up req/s | 1 | 45,185 [45,002–45,394] |
| up p50 ms | 1 | 0.0 [0.0–0.0] |
| up p90 ms | 1 | 0.0 [0.0–0.0] |
| up p99 ms | 1 | 0.0 [0.0–0.0] |
| up CPU µs/ok | 1 | 16 [16–16] |
| up req/s | 16 | 187,581 [183,062–231,023] |
| up p50 ms | 16 | 0.1 [0.1–0.1] |
| up p90 ms | 16 | 0.1 [0.1–0.1] |
| up p99 ms | 16 | 0.1 [0.1–0.2] |
| up CPU µs/ok | 16 | 13 [12–15] |
| up req/s | 64 | 213,675 [208,383–289,578] |
| up p50 ms | 64 | 0.3 [0.2–0.3] |
| up p90 ms | 64 | 0.4 [0.4–0.4] |
| up p99 ms | 64 | 0.5 [0.5–0.7] |
| up CPU µs/ok | 64 | 12 [11–12] |
| post_message req/s | 1 | 24,225 [24,192–24,931] |
| post_message p50 ms | 1 | 0.0 [0.0–0.0] |
| post_message p90 ms | 1 | 0.0 [0.0–0.0] |
| post_message p99 ms | 1 | 0.1 [0.1–0.1] |
| post_message CPU µs/ok | 1 | 32 [31–32] |
| post_message req/s | 16 | 111,766 [110,265–114,231] |
| post_message p50 ms | 16 | 0.1 [0.1–0.1] |
| post_message p90 ms | 16 | 0.2 [0.2–0.2] |
| post_message p99 ms | 16 | 0.3 [0.2–0.3] |
| post_message CPU µs/ok | 16 | 32 [30–32] |
| post_message req/s | 64 | 112,472 [109,971–113,013] |
| post_message p50 ms | 64 | 0.6 [0.6–0.6] |
| post_message p90 ms | 64 | 0.7 [0.6–0.7] |
| post_message p99 ms | 64 | 0.9 [0.8–0.9] |
| post_message CPU µs/ok | 64 | 32 [31–32] |

