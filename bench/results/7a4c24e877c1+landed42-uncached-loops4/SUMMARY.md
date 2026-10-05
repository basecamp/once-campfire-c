## bench/results/7a4c24e877c1+landed42-uncached-loops4
reps per app: c 3

### HTTP (median [min–max] across reps)

| Route | conc | c |
|---|---:|---|
| room_show req/s | 1 | 41 [41–41] |
| room_show p50 ms | 1 | 24.1 [24.1–24.2] |
| room_show p90 ms | 1 | 24.6 [24.6–25.0] |
| room_show p99 ms | 1 | 26.8 [25.7–27.4] |
| room_show CPU µs/ok | 1 | 24,108 [24,076–24,285] |
| room_show req/s | 16 | 163 [156–163] |
| room_show p50 ms | 16 | 97.7 [97.2–99.8] |
| room_show p90 ms | 16 | 99.4 [99.3–113.4] |
| room_show p99 ms | 16 | 112.4 [108.0–122.8] |
| room_show CPU µs/ok | 16 | 24,446 [24,439–25,578] |
| room_show req/s | 64 | 159 [159–162] |
| room_show p50 ms | 64 | 395.0 [393.0–397.3] |
| room_show p90 ms | 64 | 413.2 [402.7–424.7] |
| room_show p99 ms | 64 | 440.8 [415.2–484.4] |
| room_show CPU µs/ok | 64 | 25,030 [24,619–25,130] |
| messages_page req/s | 1 | 43 [43–44] |
| messages_page p50 ms | 1 | 23.1 [22.8–23.1] |
| messages_page p90 ms | 1 | 23.7 [23.3–23.7] |
| messages_page p99 ms | 1 | 25.9 [24.3–27.7] |
| messages_page CPU µs/ok | 1 | 23,090 [22,822–23,116] |
| messages_page req/s | 16 | 172 [161–172] |
| messages_page p50 ms | 16 | 92.7 [92.6–96.6] |
| messages_page p90 ms | 16 | 94.8 [94.3–110.7] |
| messages_page p99 ms | 16 | 102.3 [100.3–119.4] |
| messages_page CPU µs/ok | 16 | 23,260 [23,180–24,803] |
| messages_page req/s | 64 | 170 [167–171] |
| messages_page p50 ms | 64 | 373.8 [373.8–377.6] |
| messages_page p90 ms | 64 | 382.0 [378.1–406.5] |
| messages_page p99 ms | 64 | 394.8 [383.5–424.7] |
| messages_page CPU µs/ok | 64 | 23,443 [23,312–23,914] |
| sidebar req/s | 1 | 205 [204–206] |
| sidebar p50 ms | 1 | 4.8 [4.8–4.8] |
| sidebar p90 ms | 1 | 5.0 [5.0–5.0] |
| sidebar p99 ms | 1 | 6.4 [6.0–6.6] |
| sidebar CPU µs/ok | 1 | 4,829 [4,808–4,853] |
| sidebar req/s | 16 | 811 [766–820] |
| sidebar p50 ms | 16 | 19.6 [19.3–20.5] |
| sidebar p90 ms | 16 | 20.4 [20.2–23.1] |
| sidebar p99 ms | 16 | 24.1 [24.0–25.2] |
| sidebar CPU µs/ok | 16 | 4,904 [4,858–5,205] |
| sidebar req/s | 64 | 802 [780–804] |
| sidebar p50 ms | 64 | 78.5 [78.3–81.0] |
| sidebar p90 ms | 64 | 86.0 [85.0–88.8] |
| sidebar p99 ms | 64 | 89.2 [88.1–92.0] |
| sidebar CPU µs/ok | 64 | 4,967 [4,959–5,109] |
| search req/s | 1 | 110 [110–111] |
| search p50 ms | 1 | 9.0 [8.9–9.0] |
| search p90 ms | 1 | 9.4 [9.2–9.4] |
| search p99 ms | 1 | 10.5 [10.3–10.9] |
| search CPU µs/ok | 1 | 9,017 [8,925–9,042] |
| search req/s | 16 | 420 [396–436] |
| search p50 ms | 16 | 37.0 [36.0–39.8] |
| search p90 ms | 16 | 42.2 [38.7–45.8] |
| search p99 ms | 16 | 46.9 [43.9–49.6] |
| search CPU µs/ok | 16 | 9,495 [9,133–10,076] |
| search req/s | 64 | 406 [397–436] |
| search p50 ms | 64 | 159.0 [146.3–160.8] |
| search p90 ms | 64 | 164.6 [149.1–179.6] |
| search p99 ms | 64 | 174.5 [153.2–183.8] |
| search CPU µs/ok | 64 | 9,808 [9,139–10,029] |
| avatar req/s | 1 | 905 [904–923] |
| avatar p50 ms | 1 | 1.1 [1.1–1.1] |
| avatar p90 ms | 1 | 1.1 [1.1–1.1] |
| avatar p99 ms | 1 | 1.7 [1.7–1.8] |
| avatar CPU µs/ok | 1 | 1,073 [1,063–1,078] |
| avatar req/s | 16 | 3,672 [3,658–3,731] |
| avatar p50 ms | 16 | 4.3 [4.2–4.3] |
| avatar p90 ms | 16 | 4.7 [4.7–5.0] |
| avatar p99 ms | 16 | 6.4 [6.2–6.6] |
| avatar CPU µs/ok | 16 | 1,077 [1,061–1,079] |
| avatar req/s | 64 | 3,688 [2,652–3,709] |
| avatar p50 ms | 64 | 17.2 [17.1–25.7] |
| avatar p90 ms | 64 | 18.3 [18.1–28.1] |
| avatar p99 ms | 64 | 21.0 [20.4–30.0] |
| avatar CPU µs/ok | 64 | 1,067 [1,064–1,483] |
| static_css req/s | 1 | 36,561 [34,762–37,580] |
| static_css p50 ms | 1 | 0.0 [0.0–0.0] |
| static_css p90 ms | 1 | 0.0 [0.0–0.0] |
| static_css p99 ms | 1 | 0.0 [0.0–0.0] |
| static_css CPU µs/ok | 1 | 20 [20–21] |
| static_css req/s | 16 | 127,793 [113,299–128,476] |
| static_css p50 ms | 16 | 0.1 [0.1–0.1] |
| static_css p90 ms | 16 | 0.2 [0.1–0.2] |
| static_css p99 ms | 16 | 0.2 [0.2–0.6] |
| static_css CPU µs/ok | 16 | 15 [15–16] |
| static_css req/s | 64 | 151,958 [122,689–153,314] |
| static_css p50 ms | 64 | 0.5 [0.5–0.6] |
| static_css p90 ms | 64 | 0.6 [0.5–0.6] |
| static_css p99 ms | 64 | 0.9 [0.8–1.4] |
| static_css CPU µs/ok | 64 | 15 [13–16] |
| up req/s | 1 | 39,775 [38,374–39,969] |
| up p50 ms | 1 | 0.0 [0.0–0.0] |
| up p90 ms | 1 | 0.0 [0.0–0.0] |
| up p99 ms | 1 | 0.0 [0.0–0.0] |
| up CPU µs/ok | 1 | 17 [16–17] |
| up req/s | 16 | 171,296 [159,066–173,056] |
| up p50 ms | 16 | 0.1 [0.1–0.1] |
| up p90 ms | 16 | 0.1 [0.1–0.1] |
| up p99 ms | 16 | 0.2 [0.1–0.2] |
| up CPU µs/ok | 16 | 14 [11–15] |
| up req/s | 64 | 194,052 [192,573–196,464] |
| up p50 ms | 64 | 0.4 [0.4–0.4] |
| up p90 ms | 64 | 0.4 [0.4–0.4] |
| up p99 ms | 64 | 0.5 [0.5–0.6] |
| up CPU µs/ok | 64 | 12 [12–13] |
| post_message req/s | 1 | 892 [887–911] |
| post_message p50 ms | 1 | 1.1 [1.1–1.1] |
| post_message p90 ms | 1 | 1.2 [1.1–1.2] |
| post_message p99 ms | 1 | 1.8 [1.7–1.8] |
| post_message CPU µs/ok | 1 | 1,090 [1,071–1,093] |
| post_message req/s | 16 | 3,410 [3,316–3,700] |
| post_message p50 ms | 16 | 4.4 [4.3–4.6] |
| post_message p90 ms | 16 | 5.4 [4.9–7.0] |
| post_message p99 ms | 16 | 6.9 [6.2–8.8] |
| post_message CPU µs/ok | 16 | 1,152 [1,071–1,195] |
| post_message req/s | 64 | 3,644 [3,345–3,694] |
| post_message p50 ms | 64 | 17.4 [17.0–18.3] |
| post_message p90 ms | 64 | 18.3 [18.3–22.0] |
| post_message p99 ms | 64 | 22.2 [20.3–31.1] |
| post_message CPU µs/ok | 64 | 1,085 [1,066–1,179] |

