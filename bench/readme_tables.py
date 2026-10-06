#!/usr/bin/env python3
"""Generate the publication tables (README-ready) from the two final arms.

Applies the B01b validity rule: a rep with any HTTP error/non-200 is excluded
from medians.  Rails and Rust medians pool the valid reps of both arms (their
rows do not depend on the C cache arm); C rows come from their own arm.

Usage:
    python3 bench/readme_tables.py \
        --uncached bench/results/ec73dd1-uncached \
        --cache bench/results/ec73dd1-cache
"""

from __future__ import annotations

import argparse
import glob
import json
import re
import statistics
from pathlib import Path

APPS = {"rails": "reference", "rust": "rust", "c_off": "c", "c_on": "c"}


def load(directory: Path) -> dict[str, list[dict]]:
    runs: dict[str, list[dict]] = {}
    for path in sorted(directory.glob("*-[0-9].json")):
        rep = json.loads(path.read_text())
        if "app" in rep:
            runs.setdefault(rep["app"], []).append(rep)
    return runs


def valid(rep: dict) -> bool:
    return all(e.get("errors", 0) == 0 and e.get("invalid_responses", 0) == 0
               and set(e.get("statuses", {})) == {"200"}
               for e in rep.get("http", []))


def values(runs: dict[str, list[dict]], app: str, route: str, conc: int,
           key: str) -> list[float]:
    out = []
    for rep in runs.get(app, []):
        if not valid(rep):
            continue
        for entry in rep.get("http", []):
            if entry["route"] == route and entry["conc"] == conc:
                if key == "rps":
                    out.append(entry["rps"])
                elif key == "cpu":
                    out.append(entry.get("cpu_us_per_success"))
                else:
                    out.append(entry["latency"].get(f"{key}_ms"))
    return [v for v in out if v is not None]


def med(vals):
    return statistics.median(vals) if vals else None


def fmt(v, pattern="{:,.0f}"):
    return pattern.format(v) if v is not None else "-"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--uncached", required=True)
    parser.add_argument("--cache", required=True)
    args = parser.parse_args()
    uncached = load(Path(args.uncached))
    cache = load(Path(args.cache))
    pooled = {app: uncached.get(app, []) + cache.get(app, [])
              for app in ("reference", "rust")}

    def col(app_key: str):
        if app_key == "c_off":
            return uncached
        if app_key == "c_on":
            return cache
        return pooled

    def app_of(app_key: str):
        return APPS[app_key]

    print("## Throughput, 16 clients (req/s, median of valid reps)\n")
    print("| Workload | Rails | Rust | C cache-off | C cache-on |")
    print("|---|---:|---:|---:|---:|")
    rows = [("Room page", "room_show"), ("Messages page", "messages_page"),
            ("Sidebar", "sidebar"), ("Search", "search"),
            ("Post a message", "post_message"), ("/up", "up")]
    for label, route in rows:
        cells = []
        for key in ("rails", "rust", "c_off", "c_on"):
            cells.append(fmt(med(values(col(key), app_of(key), route, 16,
                                        "rps"))))
        print(f"| {label} | " + " | ".join(cells) + " |")
    print()

    print("## Latency (ms, median of valid reps)\n")
    print("| Route | clients | Rails p50 | Rails p99 | Rust p50 | Rust p99 "
          "| C off p50 | C off p99 | C on p50 | C on p99 |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for label, route in (("Room page", "room_show"),
                         ("Post a message", "post_message")):
        for conc in (16, 64):
            cells = []
            for key in ("rails", "rust", "c_off", "c_on"):
                for metric in ("p50", "p99"):
                    cells.append(fmt(med(values(col(key), app_of(key), route,
                                                 conc, metric)), "{:.1f}"))
            print(f"| {label} | {conc} | " + " | ".join(cells) + " |")
    print()

    print("## Size and startup (medians of valid reps)\n")
    print("| App | Cold start (ms) | Idle container mem (MiB) | Peak container mem (MiB) | Image unpacked (MiB) |")
    print("|---|---:|---:|---:|---:|")
    for label, key, directory, image in (
            ("Rails", "rails", args.uncached, "campfire-reference:app"),
            ("Rust", "rust", args.uncached, "campfire-rust:app"),
            ("C cache-off", "c_off", args.uncached, "campfire-c:bench"),
            ("C cache-on", "c_on", args.cache, "campfire-c:bench")):
        runs = col(key)
        app = app_of(key)
        cold = [r.get("cold_start_ms") for r in runs.get(app, []) if valid(r)]
        idle = [r["memory"].get("idle_current_mb") for r in runs.get(app, [])
                if valid(r)]
        peak = [r["memory"].get("peak_current_mb") for r in runs.get(app, [])
                if valid(r)]
        image_mib = "-"
        for line in (Path(directory) / "env.txt").read_text().splitlines():
            if f"{image} " in line and "unpacked_bytes=" in line:
                size = int(line.rsplit("unpacked_bytes=", 1)[1].split()[0])
                image_mib = f"{size / (1024 * 1024):,.0f}"
        print(f"| {label} | {fmt(med(cold))} | {fmt(med(idle))} | "
              f"{fmt(med(peak))} | {image_mib} |")
    print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
