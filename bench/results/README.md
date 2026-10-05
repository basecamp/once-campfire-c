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

Every measurement row must be traceable to a code change: write the revision,
the change (commit subject or a one-line description), and cache on/off in the
directory or in the evidence note that links it.  Fil-C rows are independent
and include their full runtime footprint.  Do not delete an unfavorable row.
