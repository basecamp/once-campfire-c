#!/usr/bin/env python3
"""Summarize bench/run result directories into per-route medians and spreads.

Reads the harness's per-rep `*-<rep>.json` files and prints markdown tables
(plus a JSON digest with `--json`):

  * per app, per route, per concurrency: median [min-max] of rps and of
    p50/p90/p99 latency, and CPU microseconds per successful response;
  * the cache-on vs cache-off delta for the C app when both directories are
    given (`--cache-off DIR --cache-on DIR`).

The pinned load generator records p50/p90/p99 (it has no p95 row); this tool
does not invent one.  A rep whose HTTP entries contain errors or non-200
statuses is reported as INVALID, never averaged in.

Usage:
    python3 bench/summarize.py DIR [DIR...]
    python3 bench/summarize.py --cache-off DIR --cache-on DIR
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
from pathlib import Path

APPS = ("reference", "elixir", "go", "rust", "c")
METRICS = (
    ("rps", "req/s", "{:,.0f}"),
    ("p50", "p50 ms", "{:.1f}"),
    ("p90", "p90 ms", "{:.1f}"),
    ("p99", "p99 ms", "{:.1f}"),
    ("cpu", "CPU µs/ok", "{:,.0f}"),
)


def load_reps(directory: Path) -> dict[str, list[dict]]:
    runs: dict[str, list[dict]] = {}
    for path in sorted(directory.glob("*-*.json")):
        if path.name.endswith("-jobs.json"):
            continue
        try:
            rep = json.loads(path.read_text())
        except json.JSONDecodeError:
            continue
        if "app" not in rep:
            continue
        runs.setdefault(rep["app"], []).append(rep)
    return runs


def valid(rep: dict) -> bool:
    for entry in rep.get("http", []):
        if entry.get("errors") or entry.get("invalid_responses"):
            return False
        if set(entry.get("statuses", {})) != {"200"}:
            return False
        if entry.get("route") == "post_message":
            if entry.get("ok", 0) <= 0 or entry.get("persisted_messages") != entry["ok"]:
                return False
    return True


def fmt(values: list[float], pattern: str) -> str:
    values = [v for v in values if v is not None]
    if not values:
        return "-"
    med = statistics.median(values)
    if len(values) == 1:
        return pattern.format(med)
    return f"{pattern.format(med)} [{pattern.format(min(values))}–{pattern.format(max(values))}]"


def route_keys(runs: dict[str, list[dict]]) -> list[tuple[str, int]]:
    keys: list[tuple[str, int]] = []
    for app in APPS:
        for rep in runs.get(app, []):
            for entry in rep.get("http", []):
                key = (entry["route"], entry["conc"])
                if key not in keys:
                    keys.append(key)
    return keys


def metric_values(rep: dict, route: str, conc: int, metric: str):
    for entry in rep.get("http", []):
        if entry["route"] == route and entry["conc"] == conc:
            if metric == "rps":
                return entry.get("rps")
            if metric == "cpu":
                return entry.get("cpu_us_per_success")
            return entry["latency"].get(f"{metric}_ms")
    return None


def http_table(runs: dict[str, list[dict]], title: str) -> list[str]:
    apps = [a for a in APPS if a in runs]
    lines = [f"### {title}", "",
             "| Route | conc | " + " | ".join(apps) + " |",
             "|---|---:|" + "---|" * len(apps)]
    for route, conc in route_keys(runs):
        for metric, label, pattern in METRICS:
            cells = []
            for app in apps:
                values = []
                for rep in runs[app]:
                    if not valid(rep):
                        continue
                    values.append(metric_values(rep, route, conc, metric))
                cells.append(fmt(values, pattern))
            lines.append(f"| {route} {label} | {conc} | " + " | ".join(cells)
                         + " |")
    lines.append("")
    return lines


def med(runs: dict[str, list[dict]], route: str, conc: int, metric: str,
        app: str = "c"):
    values = []
    for rep in runs.get(app, []):
        if valid(rep):
            values.append(metric_values(rep, route, conc, metric))
    values = [v for v in values if v is not None]
    return statistics.median(values) if values else None


def cache_delta(off: dict, on: dict) -> list[str]:
    lines = ["### C: cache-off vs cache-on (median of valid reps)", "",
             "| Route | conc | off req/s | on req/s | ratio | off CPU µs/ok | on CPU µs/ok |",
             "|---|---:|---:|---:|---:|---:|---:|"]
    for route, conc in route_keys(off):
        off_rps = med(off, route, conc, "rps")
        on_rps = med(on, route, conc, "rps")
        off_cpu = med(off, route, conc, "cpu")
        on_cpu = med(on, route, conc, "cpu")
        ratio = f"{on_rps / off_rps:.2f}×" if off_rps and on_rps else "-"
        lines.append(
            f"| {route} | {conc} | "
            + (f"{off_rps:,.0f}" if off_rps else "-") + " | "
            + (f"{on_rps:,.0f}" if on_rps else "-") + " | " + ratio + " | "
            + (f"{off_cpu:,.0f}" if off_cpu else "-") + " | "
            + (f"{on_cpu:,.0f}" if on_cpu else "-") + " |")
    lines.append("")
    return lines


def scaling_table(base: dict, new: dict, app: str, base_label: str,
                  new_label: str) -> list[str]:
    lines = [f"### {app}: {base_label} vs {new_label} (median of valid reps)", "",
             f"| Route | conc | {base_label} req/s | {new_label} req/s | rps ratio | "
             f"{base_label} CPU µs/ok | {new_label} CPU µs/ok | CPU ratio |",
             "|---|---:|---:|---:|---:|---:|---:|---:|"]
    ratios = []
    for route, conc in route_keys(base):
        b = med(base, route, conc, "rps", app)
        n = med(new, route, conc, "rps", app)
        bc = med(base, route, conc, "cpu", app)
        nc = med(new, route, conc, "cpu", app)
        ratio = n / b if b and n else None
        if ratio:
            ratios.append(ratio)
        lines.append(
            f"| {route} | {conc} | "
            + (f"{b:,.0f}" if b else "-") + " | "
            + (f"{n:,.0f}" if n else "-") + " | "
            + (f"{ratio:.2f}×" if ratio else "-") + " | "
            + (f"{bc:,.0f}" if bc else "-") + " | "
            + (f"{nc:,.0f}" if nc else "-") + " | "
            + (f"{bc / nc:.2f}×" if bc and nc else "-") + " |")
    lines.append("")
    if ratios:
        lines.append(f"median rps ratio across routes/concs: "
                     f"{statistics.median(ratios):.2f}×")
        lines.append("")
    return lines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dirs", nargs="*", help="result directories")
    parser.add_argument("--cache-off")
    parser.add_argument("--cache-on")
    parser.add_argument("--scale-base", help="baseline result directory")
    parser.add_argument("--scale-new", help="changed result directory")
    parser.add_argument("--app", default="c", help="app for --scale-*")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    if args.scale_base and args.scale_new:
        base = load_reps(Path(args.scale_base))
        new = load_reps(Path(args.scale_new))
        if args.json:
            digest = {}
            for route, conc in route_keys(base):
                digest[f"{route}@{conc}"] = {
                    "base_rps": med(base, route, conc, "rps", args.app),
                    "new_rps": med(new, route, conc, "rps", args.app),
                    "base_cpu": med(base, route, conc, "cpu", args.app),
                    "new_cpu": med(new, route, conc, "cpu", args.app),
                }
            print(json.dumps(digest, indent=2, sort_keys=True))
        else:
            print("\n".join(scaling_table(
                base, new, args.app, Path(args.scale_base).name,
                Path(args.scale_new).name)))
        return 0

    if args.cache_off and args.cache_on:
        off = load_reps(Path(args.cache_off))
        on = load_reps(Path(args.cache_on))
        digest = {
            "cache_off": args.cache_off,
            "cache_on": args.cache_on,
            "valid_reps": {
                label: {app: [r["rep"] for r in reps if valid(r)]
                        for app, reps in runs.items()}
                for label, runs in (("off", off), ("on", on))
            },
        }
        if args.json:
            print(json.dumps(digest, indent=2, sort_keys=True))
        else:
            print("\n".join(cache_delta(off, on)))
        return 0

    for directory in args.dirs:
        runs = load_reps(Path(directory))
        invalid = {app: [r["rep"] for r in reps if not valid(r)]
                   for app, reps in runs.items()}
        print(f"## {directory}")
        print("reps per app: " + ", ".join(f"{a} {len(runs[a])}"
                                            for a in runs)
              + (f"; INVALID reps: {invalid}" if any(invalid.values()) else ""))
        print()
        print("\n".join(http_table(runs, "HTTP (median [min–max] across reps)")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
