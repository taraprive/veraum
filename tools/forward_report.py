"""Forward-tracking report for directional signals.

Reads the live feed bars (JSONL, one per closed bar) plus the signal journal
(signals.jsonl) and resolves every directional signal that has since been
acted on: LONG wins when price first reaches the target, loses when it first
reaches the stop; SHORT is mirrored. Signals with no later bars stay "open".

This is the honest out-of-sample evidence trail: the model was tuned on the
past; this report only counts what happens AFTER the signal was emitted.

Caveats (built-in, not hidden):
  * Resolution uses bar high/low, so an intra-bar touch is credited even if
    the exact price was not fillable (optimistic for the stop side too).
    In the same bar, stop is assumed hit first (conservative).
  * Entry is assumed at the signal's stated entry (the closing price of the
    signal bar); a real order would use the next open, which may differ.

Usage:
  python tools/forward_report.py --signals build/forward/forward_signals.jsonl \
      --bars build/mt5_bars.jsonl
"""

import argparse
import json
import re
from collections import defaultdict

SIGNAL_RE = re.compile(r"SIGNAL\s+(\S+)\s+(LONG|SHORT)")
ENTRY_RE = re.compile(r"entry\s+\$([0-9.]+)")
STOP_RE = re.compile(r"stop\s+\$([0-9.]+)")
TARGET_RE = re.compile(r"target\s+\$([0-9.]+)")
LOT_RE = re.compile(r"LOT\s+([0-9.]+)")


def load_bars(path):
    bars = defaultdict(list)
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                j = json.loads(line)
            except json.JSONDecodeError:
                continue
            ins = j.get("i", "")
            if not ins:
                continue
            bars[ins].append((int(j["t"]), float(j["h"]), float(j["l"])))
    for ins in bars:
        bars[ins].sort(key=lambda r: r[0])
        seen = set()
        uniq = []
        for r in bars[ins]:
            if r[0] in seen:
                continue
            seen.add(r[0])
            uniq.append(r)
        bars[ins] = uniq
    return bars


def parse_signal(text):
    m = SIGNAL_RE.search(text)
    if not m:
        return None
    e = ENTRY_RE.search(text)
    s = STOP_RE.search(text)
    t = TARGET_RE.search(text)
    if not (e and s and t):
        return None
    ent = float(e.group(1))
    stop = float(s.group(1))
    tgt = float(t.group(1))
    lot = LOT_RE.search(text)
    return {
        "instrument": m.group(1),
        "side": m.group(2),
        "entry": ent,
        "stop": stop,
        "target": tgt,
        "lot": float(lot.group(1)) if lot else 0.0,
    }


def resolve(sig, bars):
    ins = sig["instrument"]
    if ins not in bars:
        return None
    seq = bars[ins]
    # First bar after the signal bar.
    start = 0
    for i, (t, h, l) in enumerate(seq):
        if t > sig["ts"]:
            start = i
            break
    else:
        return None  # signal too recent / no later bars at all
    for k in range(start, len(seq)):
        _, h, l = seq[k]
        if sig["side"] == "LONG":
            stop_hit = l <= sig["stop"]
            target_hit = h >= sig["target"]
        else:
            stop_hit = h >= sig["stop"]
            target_hit = l <= sig["target"]
        if stop_hit and target_hit:
            return {"outcome": "stop", "bar": k}
        if stop_hit:
            return {"outcome": "stop", "bar": k}
        if target_hit:
            return {"outcome": "target", "bar": k}
    return {"outcome": "open", "bar": len(seq)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--signals", required=True)
    ap.add_argument("--bars", required=True)
    a = ap.parse_args()

    bars = load_bars(a.bars)
    counts = defaultdict(int)
    wins = defaultdict(int)
    losses = defaultdict(int)
    total_r = defaultdict(float)
    open_sigs = defaultdict(list)
    detail = []

    with open(a.signals, encoding="utf-8") as f:
        for line in f:
            if not line.strip():
                continue
            try:
                env = json.loads(line)
            except json.JSONDecodeError:
                continue
            text = env.get("text", "")
            if text and SIGNAL_RE.search(text):
                sig = parse_signal(text)
            else:
                sig = None
            if not sig:
                continue
            sig["ts"] = int(env.get("ts", 0))
            counts[sig["instrument"]] += 1
            if sig["instrument"] not in bars:
                open_sigs[sig["instrument"]].append(sig)
                continue
            res = resolve(sig, bars)
            if res is None or res["outcome"] == "open":
                open_sigs[sig["instrument"]].append(sig)
                continue
            stop_dist = abs(sig["entry"] - sig["stop"])
            if stop_dist <= 0:
                continue
            if res["outcome"] == "target":
                wins[sig["instrument"]] += 1
                r = abs(sig["target"] - sig["entry"]) / stop_dist
            else:
                losses[sig["instrument"]] += 1
                r = -1.0
            total_r[sig["instrument"]] += r
            detail.append((sig, res["outcome"], r))

    print("==", "forward resolution", "==")
    for ins in sorted(set(list(counts) + list(wins) + list(losses))):
        total = counts[ins]
        resolved = wins[ins] + losses[ins]
        wr = (wins[ins] / resolved) if resolved else 0.0
        fr = (wins[ins] / total) if total else 0.0
        avg_r = (total_r[ins] / resolved) if resolved else 0.0
        print(f"{ins:8s} emitted={total:3d} resolved={resolved:3d} "
              f"wins={wins[ins]:2d} losses={losses[ins]:2d} "
              f"winRate={wr:.2f} avgR={avg_r:+.2f} open={len(open_sigs[ins])}")
    print("-- open signals (not yet resolved):",
          sum(len(v) for v in open_sigs.values()))
    for ins, sigs in sorted(open_sigs.items()):
        for s in sigs:
            print(f"   {ins:8s} {s['side']:5s} entry={s['entry']} "
                  f"stop={s['stop']} target={s['target']} ts={s['ts']}")
    all_resolved = sum(wins.values()) + sum(losses.values())
    all_emitted = sum(counts.values())
    if all_resolved:
        print("-- aggregate:", f"resolved={all_resolved}/{all_emitted}",
              f"avgR={sum(total_r.values())/all_resolved:+.2f}")
    print("-- caveats: bar high/low resolution, stop-first on same bar,",
          "entry at signal close. Not financial advice; demo account.")


if __name__ == "__main__":
    main()