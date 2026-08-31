"""Latency analysis for the C++ pipeline.

Reads the latency summary from logs/bot.log (or a standalone CSV of per-stage
samples) and reports average / P95 / P99 / max per stage so you can see exactly
where time is spent. Kept dependency-free on purpose.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path
from typing import Dict, List


def parse_log(path: Path) -> Dict[str, List[float]]:
    """Extract per-stage latency samples from the bot's log output."""
    stages: Dict[str, List[float]] = {}
    pattern = re.compile(
        r"\[latency\] (\w+) avg_us=([0-9.eE+-]+) p95_us=([0-9.eE+-]+) "
        r"p99_us=([0-9.eE+-]+) max_us=([0-9.eE+-]+) n=(\d+)"
    )
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = pattern.search(line)
        if not m:
            continue
        name = m.group(1)
        stages[name] = [
            float(m.group(i)) for i in range(2, 6)
        ] + [float(m.group(6))]
    return stages


def summarize(stages: Dict[str, List[float]]) -> None:
    print(f"{'stage':<24} {'avg_us':>12} {'p95_us':>12} {'p99_us':>12} {'max_us':>12} {'n':>8}")
    for name, vals in stages.items():
        avg, p95, p99, mx, n = vals
        print(f"{name:<24} {avg:>12.2f} {p95:>12.2f} {p99:>12.2f} {mx:>12.2f} {int(n):>8}")


def main() -> None:
    ap = argparse.ArgumentParser(description="Summarize pipeline latency per stage")
    ap.add_argument("--log", type=Path, default=Path("logs/bot.log"))
    args = ap.parse_args()
    if not args.log.exists():
        raise SystemExit(f"no log at {args.log}")
    summarize(parse_log(args.log))


if __name__ == "__main__":
    main()
