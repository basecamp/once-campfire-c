# Benchmark results

One directory per recorded revision:

    bench/results/<revision>/

`<revision>` is the recorded candidate revision (`git rev-parse --short=12
HEAD`, or `C_REVISION=<label>` for uncommitted trees); a suffix records the
configuration when one revision contributes several rows, for example
`7a4c24e877c1-uncached` and `7a4c24e877c1-cache`.  Never overwrite a result
directory: add a new suffix or a new revision.

Each directory is written by the pinned harness (`bench/run-c`) and must keep
`env.txt` (seed sha256, image ids, CPU sets, compiler/library flags passed to
the app, workload flags, revision), the per-rep `*.json` (HTTP/statuses/
latency/CPU-per-success/memory), `.mem` and cable `.jsonl` samples,
`*-validation.json` (preflight response evidence: statuses, wire/body sizes,
body sha256), `*-jobs.json`, `uptime.log`, and `report.md` from `bench/report`.
`DIVERGENCE.md` (the disclosed seed divergence: jason's avatar is the
fallback initials SVG, not the reference webp variant, plus the seed hash) is
written by `bench/run-c` and must be kept as well.
`bench/check/check_bodies.py --out .../preflight.json` stores standalone
body-validation evidence for scaffolding work such as B01a.
`image-sizes.json` records the unpacked image size for the set's tables,
measured as root with `du -s -m -x /` inside each image (the Docker store's
own `.Size` accounting changed with the containerd switch and is no longer
used for the published column).

Every measurement row must be traceable to a code change: write the revision,
the change (commit subject or a one-line description), and cache on/off in the
directory or in the evidence note that links it.  Fil-C rows are independent
and include their full runtime footprint.  Do not delete an unfavorable row.




## Revision identifiers

Directory names use the short revision of the measured tree; files kept as
captured inside each directory (for example `env.txt`) may name the same tree
by an earlier identifier. Mapping for the archived sets:

| Identifier inside older files | Directory name |
| --- | --- |
| `ec73dd1` | `46ed9c5` |
| `7a4c24e877c1` | `71f851e` |
| `604f037c0e25` | `837aa9c` |
| `84ee2ae` | `a0028eb` |
| `25b9a3b` | `e63e4d3` |
| `ed9e118` | `85b58ea` |

## Current publication set

The README tables are generated from the four-way set (Rails, Rust, C and the
Fil-C build, one interleaved invocation per cache arm):

    python3 bench/readme_tables.py \
      --uncached bench/results/2bc574b-quad-uncached \
      --cache    bench/results/2bc574b-quad-cache

`46ed9c5-uncached/` and `46ed9c5-cache/` are the earlier three-way set
(superseded for the tables, kept as the historical record).
