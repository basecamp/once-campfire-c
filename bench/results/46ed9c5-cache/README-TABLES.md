## Throughput, 16 clients (req/s, median of valid reps)

| Workload | Rails | Rust | C cache-off | C cache-on |
|---|---:|---:|---:|---:|
| Room page | 234 | 31,741 | 1,995 | 104,523 |
| Messages page | 438 | 29,021 | 2,465 | 128,748 |
| Sidebar | 679 | 32,519 | 22,923 | 134,211 |
| Search | 434 | 33,010 | 5,395 | 128,249 |
| Post a message | 1,116 | 54,242 | 92,508 | 109,889 |
| /up | 4,363 | 131,207 | 170,347 | 180,614 |

## Latency (ms, median of valid reps)

| Route | clients | Rails p50 | Rails p99 | Rust p50 | Rust p99 | C off p50 | C off p99 | C on p50 | C on p99 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Room page | 16 | 62.4 | 175.0 | 0.5 | 1.8 | 7.9 | 10.9 | 0.1 | 0.8 |
| Room page | 64 | 310.9 | 437.4 | 1.8 | 4.5 | 31.3 | 35.4 | 0.5 | 0.8 |
| Post a message | 16 | 14.3 | 28.6 | 0.3 | 1.0 | 0.1 | 1.0 | 0.1 | 0.3 |
| Post a message | 64 | 55.1 | 111.7 | 1.0 | 2.6 | 0.6 | 1.4 | 0.6 | 0.9 |

## Size and startup (medians of valid reps)

| App | Cold start (ms) | Idle container mem (MiB) | Peak container mem (MiB) | Image unpacked (MiB) |
|---|---:|---:|---:|---:|
| Rails | 2,591 | 377 | 844 | 342 |
| Rust | 180 | 40 | 60 | 68 |
| C cache-off | 150 | 35 | 43 | 82 |
| C cache-on | 133 | 39 | 47 | 82 |

