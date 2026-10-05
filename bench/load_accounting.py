#!/usr/bin/env python3
"""Per-rep load accounting for one bench/run log + result directory.

Emits markdown: harness quiet-gate wait and load, per-rep 1/5/15-minute load at
start/end (from the harness JSON), cold start.  Writes LOAD-ACCOUNTING.md into
the result directory when --out is given.

Usage:
    python3 bench/load_accounting.py --log bench/.work/b01b-loops4.log \
        --dir "bench/results/<rev>" [--out]
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

START = re.compile(r"^\[(\d\d:\d\d:\d\d)\] (\S+) rep (\d+): starting")
GATE = re.compile(
    r"^\[(\d\d:\d\d:\d\d)\] load before run: ([\d.]+) ([\d.]+) ([\d.]+) "
    r"\(waited (\d+)s\)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", required=True)
    parser.add_argument("--dir", required=True)
    parser.add_argument("--out", action="store_true",
                        help="write LOAD-ACCOUNTING.md into --dir")
    parser.add_argument("--title", default=None)
    args = parser.parse_args()

    gate, order = [], []
    for line in Path(args.log).read_text().splitlines():
        match = GATE.match(line)
        if match:
            gate.append((float(match.group(2)), int(match.group(5))))
        match = START.match(line)
        if match:
            order.append((match.group(2), int(match.group(3))))

    lines = [f"### {args.title or Path(args.dir).name}", "",
             "| app | rep | gate wait (s) | load1 at gate | met 1.5 gate | "
             "load 1/5/15 start | load 1/5/15 end | cold ms |",
             "|---|---:|---:|---:|---|---|---|---:|"]
    met = 0
    for (app, rep), (load1, waited) in zip(order, gate):
        path = Path(args.dir) / f"{app}-{rep}.json"
        if not path.is_file():
            continue
        data = json.loads(path.read_text())
        quiet = load1 < 1.5
        met += quiet
        lines.append(
            f"| {app} | {rep} | {waited} | {load1:.2f} | "
            f"{'yes' if quiet else 'no'} | "
            f"{' '.join(data.get('loadavg_start', '').split()[:3])} | "
            f"{' '.join(data.get('loadavg_end', '').split()[:3])} | "
            f"{data.get('cold_start_ms')} |")
    lines.append("")
    lines.append(f"reps meeting the pinned quiet gate (load1 < 1.5): "
                 f"{met}/{len(order)}")
    lines.append("")
    text = "\n".join(lines)
    print(text)
    if args.out:
        (Path(args.dir) / "LOAD-ACCOUNTING.md").write_text(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
