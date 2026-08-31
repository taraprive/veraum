"""Independently verify a published Aurum Signals track-record hash chain.

Re-derives every signal and its outcome straight from the RAW journal + bars
(never trusting the published trackrecord.json entries), rebuilds the SHA-256
chain, and reports whether the published chain_head still matches. If any entry
was edited, deleted or reordered, the chain head will differ and this exits
non-zero — that is the tamper-evidence the whole product is built on.

Usage:
  python tools/verify_trackrecord.py --signals <journal.jsonl> --bars <bars.jsonl> \
      --published <site_dir>/trackrecord.json
"""

import argparse
import hashlib
import json
import os
import re
import sys
from collections import defaultdict

SIGNAL_RE = re.compile(r"SIGNAL\s+(\S+)\s+(LONG|SHORT)")
ENTRY_RE = re.compile(r"entry\s+\$([0-9.]+)")
STOP_RE = re.compile(r"stop\s+\$([0-9.]+)")
TARGET_RE = re.compile(r"target\s+\$([0-9.]+)")


def load_bars(path):
    bars = defaultdict(list)
    if not os.path.exists(path):
        return bars
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
            if ins:
                bars[ins].append((int(j["t"]), float(j["h"]), float(j["l"])))
    for ins in bars:
        bars[ins] = sorted(set(bars[ins]))
    return bars


def parse_signal(text):
    m = SIGNAL_RE.search(text)
    if not m:
        return None
    e, s, t = ENTRY_RE.search(text), STOP_RE.search(text), TARGET_RE.search(text)
    if not (e and s and t):
        return None
    return {
        "instrument": m.group(1),
        "side": m.group(2),
        "entry": float(e.group(1)),
        "stop": float(s.group(1)),
        "target": float(t.group(1)),
    }


def resolve(sig, bars, ts):
    seq = bars.get(sig["instrument"])
    if not seq:
        return "open"
    start = next((i for i, (t, _, _) in enumerate(seq) if t > ts), None)
    if start is None:
        return "open"
    for k in range(start, len(seq)):
        _, h, l = seq[k]
        if sig["side"] == "LONG":
            stop_hit, target_hit = l <= sig["stop"], h >= sig["target"]
        else:
            stop_hit, target_hit = h >= sig["stop"], l <= sig["target"]
        if stop_hit:
            return "stop"
        if target_hit:
            return "target"
    return "open"


def canonical(obj):
    return json.dumps(obj, sort_keys=True, separators=(",", ":"))


def rebuild(signals_path, bars_path):
    bars = load_bars(bars_path)
    entries = []
    if not os.path.exists(signals_path):
        return entries, "0" * 64
    with open(signals_path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                env = json.loads(line)
            except json.JSONDecodeError:
                continue
            text = env.get("text", "")
            sig = parse_signal(text) if text else None
            if not sig:
                continue
            ts = int(env.get("ts", 0))
            outcome = resolve(sig, bars, ts)
            rec = {
                "ts": ts, "instrument": sig["instrument"], "side": sig["side"],
                "entry": sig["entry"], "stop": sig["stop"], "target": sig["target"],
                "outcome": outcome,
            }
            if outcome in ("target", "stop"):
                stop_dist = abs(sig["entry"] - sig["stop"])
                rec["r"] = (round(abs(sig["target"] - sig["entry"]) / stop_dist, 3)
                            if stop_dist else 0) if outcome == "target" else -1.0
            entries.append(rec)
    entries.sort(key=lambda r: r["ts"])
    prev = "0" * 64
    for rec in entries:
        body = canonical({k: rec[k] for k in
                          ("ts", "instrument", "side", "entry", "stop", "target",
                           "outcome", "r")
                          if k in rec})
        prev = hashlib.sha256((prev + body).encode("utf-8")).hexdigest()
        rec["computed_hash_from_self"] = prev
    return entries, prev


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--signals", required=True)
    ap.add_argument("--bars", required=True)
    ap.add_argument("--published", required=True)
    a = ap.parse_args()

    with open(a.published, encoding="utf-8") as f:
        published = json.load(f)

    entries, rebuilt_head = rebuild(a.signals, a.bars)

    # The chain includes prev_hash as it chains, so verify the chain is sound
    # by requiring the published head == rebuilt head.
    ok = (published.get("chain_head") == rebuilt_head)
    pub_count = len(published.get("entries", []))

    print("== track-record verification ==")
    print(f"journal-derived entries : {len(entries)}")
    print(f"published entry count   : {pub_count}")
    print(f"published chain_head    : {published.get('chain_head')}")
    print(f"rebuilt chain_head      : {rebuilt_head}")
    if len(entries) != pub_count:
        print("FAIL: entry count mismatch (entries added/removed?)")
        ok = False
    if ok:
        print("OK: chain_head matches the raw journal — track record is intact.")
    else:
        print("FAIL: chain_head does NOT match the raw journal — tampering detected.")
        sys.exit(1)


if __name__ == "__main__":
    main()
