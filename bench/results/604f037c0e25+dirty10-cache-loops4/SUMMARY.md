## bench/results/604f037c0e25+dirty10-cache-loops4
reps per app: c 3

### HTTP (median [min–max] across reps)

| Route | conc | c |
|---|---:|---|
| room_show req/s | 1 | 28,485 [25,660–28,510] |
| room_show p50 ms | 1 | 0.0 [0.0–0.0] |
| room_show p90 ms | 1 | 0.0 [0.0–0.0] |
| room_show p99 ms | 1 | 0.0 [0.0–0.1] |
| room_show CPU µs/ok | 1 | 27 [27–27] |
| room_show req/s | 16 | 130,635 [129,758–132,863] |
| room_show p50 ms | 16 | 0.1 [0.1–0.1] |
| room_show p90 ms | 16 | 0.2 [0.1–0.2] |
| room_show p99 ms | 16 | 0.2 [0.2–0.3] |
| room_show CPU µs/ok | 16 | 24 [23–25] |
| room_show req/s | 64 | 141,037 [136,213–142,947] |
| room_show p50 ms | 64 | 0.5 [0.5–0.5] |
| room_show p90 ms | 64 | 0.6 [0.6–0.6] |
| room_show p99 ms | 64 | 0.8 [0.7–1.0] |
| room_show CPU µs/ok | 64 | 23 [23–23] |
| messages_page req/s | 1 | 29,887 [29,343–30,156] |
| messages_page p50 ms | 1 | 0.0 [0.0–0.0] |
| messages_page p90 ms | 1 | 0.0 [0.0–0.0] |
| messages_page p99 ms | 1 | 0.0 [0.0–0.0] |
| messages_page CPU µs/ok | 1 | 26 [26–26] |
| messages_page req/s | 16 | 142,763 [142,174–148,602] |
| messages_page p50 ms | 16 | 0.1 [0.1–0.1] |
| messages_page p90 ms | 16 | 0.2 [0.1–0.2] |
| messages_page p99 ms | 16 | 0.2 [0.2–0.3] |
| messages_page CPU µs/ok | 16 | 23 [23–23] |
| messages_page req/s | 64 | 153,039 [151,501–163,572] |
| messages_page p50 ms | 64 | 0.4 [0.4–0.4] |
| messages_page p90 ms | 64 | 0.5 [0.5–0.5] |
| messages_page p99 ms | 64 | 0.8 [0.7–0.8] |
| messages_page CPU µs/ok | 64 | 21 [20–22] |
| sidebar req/s | 1 | 32,108 [32,018–32,884] |
| sidebar p50 ms | 1 | 0.0 [0.0–0.0] |
| sidebar p90 ms | 1 | 0.0 [0.0–0.0] |
| sidebar p99 ms | 1 | 0.0 [0.0–0.0] |
| sidebar CPU µs/ok | 1 | 24 [23–24] |
| sidebar req/s | 16 | 148,846 [144,734–160,786] |
| sidebar p50 ms | 16 | 0.1 [0.1–0.1] |
| sidebar p90 ms | 16 | 0.1 [0.1–0.1] |
| sidebar p99 ms | 16 | 0.2 [0.2–0.2] |
| sidebar CPU µs/ok | 16 | 22 [20–22] |
| sidebar req/s | 64 | 167,074 [159,318–169,390] |
| sidebar p50 ms | 64 | 0.4 [0.4–0.4] |
| sidebar p90 ms | 64 | 0.5 [0.5–0.5] |
| sidebar p99 ms | 64 | 0.7 [0.7–0.7] |
| sidebar CPU µs/ok | 64 | 20 [19–20] |
| search req/s | 1 | 30,530 [29,317–31,003] |
| search p50 ms | 1 | 0.0 [0.0–0.0] |
| search p90 ms | 1 | 0.0 [0.0–0.0] |
| search p99 ms | 1 | 0.1 [0.0–0.1] |
| search CPU µs/ok | 1 | 25 [25–25] |
| search req/s | 16 | 149,171 [128,919–151,328] |
| search p50 ms | 16 | 0.1 [0.1–0.1] |
| search p90 ms | 16 | 0.1 [0.1–0.2] |
| search p99 ms | 16 | 0.2 [0.2–0.5] |
| search CPU µs/ok | 16 | 22 [22–23] |
| search req/s | 64 | 152,961 [103,106–163,706] |
| search p50 ms | 64 | 0.4 [0.4–0.6] |
| search p90 ms | 64 | 0.5 [0.5–0.9] |
| search p99 ms | 64 | 0.7 [0.7–1.9] |
| search CPU µs/ok | 64 | 20 [20–28] |
| avatar req/s | 1 | 30,992 [27,840–31,846] |
| avatar p50 ms | 1 | 0.0 [0.0–0.0] |
| avatar p90 ms | 1 | 0.0 [0.0–0.0] |
| avatar p99 ms | 1 | 0.0 [0.0–0.1] |
| avatar CPU µs/ok | 1 | 26 [25–27] |
| avatar req/s | 16 | 157,895 [143,841–158,315] |
| avatar p50 ms | 16 | 0.1 [0.1–0.1] |
| avatar p90 ms | 16 | 0.1 [0.1–0.1] |
| avatar p99 ms | 16 | 0.3 [0.3–0.4] |
| avatar CPU µs/ok | 16 | 22 [22–23] |
| avatar req/s | 64 | 171,638 [167,213–174,915] |
| avatar p50 ms | 64 | 0.4 [0.4–0.4] |
| avatar p90 ms | 64 | 0.5 [0.5–0.5] |
| avatar p99 ms | 64 | 0.8 [0.8–0.8] |
| avatar CPU µs/ok | 64 | 20 [20–21] |
| static_css req/s | 1 | 39,491 [38,172–40,224] |
| static_css p50 ms | 1 | 0.0 [0.0–0.0] |
| static_css p90 ms | 1 | 0.0 [0.0–0.0] |
| static_css p99 ms | 1 | 0.0 [0.0–0.0] |
| static_css CPU µs/ok | 1 | 19 [19–20] |
| static_css req/s | 16 | 144,636 [97,693–152,779] |
| static_css p50 ms | 16 | 0.1 [0.1–0.2] |
| static_css p90 ms | 16 | 0.1 [0.1–0.2] |
| static_css p99 ms | 16 | 0.2 [0.2–0.3] |
| static_css CPU µs/ok | 16 | 16 [12–17] |
| static_css req/s | 64 | 162,456 [152,046–192,709] |
| static_css p50 ms | 64 | 0.5 [0.4–0.5] |
| static_css p90 ms | 64 | 0.5 [0.5–0.6] |
| static_css p99 ms | 64 | 0.8 [0.7–0.8] |
| static_css CPU µs/ok | 64 | 14 [14–15] |
| up req/s | 1 | 44,423 [43,950–44,431] |
| up p50 ms | 1 | 0.0 [0.0–0.0] |
| up p90 ms | 1 | 0.0 [0.0–0.0] |
| up p99 ms | 1 | 0.0 [0.0–0.0] |
| up CPU µs/ok | 1 | 16 [16–16] |
| up req/s | 16 | 180,372 [171,428–181,729] |
| up p50 ms | 16 | 0.1 [0.1–0.1] |
| up p90 ms | 16 | 0.1 [0.1–0.1] |
| up p99 ms | 16 | 0.1 [0.1–0.2] |
| up CPU µs/ok | 16 | 15 [15–15] |
| up req/s | 64 | 202,784 [199,432–211,907] |
| up p50 ms | 64 | 0.4 [0.4–0.4] |
| up p90 ms | 64 | 0.4 [0.4–0.4] |
| up p99 ms | 64 | 0.5 [0.5–0.5] |
| up CPU µs/ok | 64 | 12 [12–13] |
| post_message req/s | 1 | 24,280 [22,769–24,310] |
| post_message p50 ms | 1 | 0.0 [0.0–0.0] |
| post_message p90 ms | 1 | 0.0 [0.0–0.0] |
| post_message p99 ms | 1 | 0.1 [0.1–0.1] |
| post_message CPU µs/ok | 1 | 32 [32–33] |
| post_message req/s | 16 | 104,435 [92,038–106,413] |
| post_message p50 ms | 16 | 0.1 [0.1–0.1] |
| post_message p90 ms | 16 | 0.2 [0.2–0.2] |
| post_message p99 ms | 16 | 0.3 [0.3–0.9] |
| post_message CPU µs/ok | 16 | 33 [33–34] |
| post_message req/s | 64 | 112,650 [109,613–113,039] |
| post_message p50 ms | 64 | 0.6 [0.6–0.6] |
| post_message p90 ms | 64 | 0.7 [0.7–0.7] |
| post_message p99 ms | 64 | 0.9 [0.9–1.0] |
| post_message CPU µs/ok | 64 | 32 [31–32] |

