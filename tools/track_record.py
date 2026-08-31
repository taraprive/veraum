"""Generate a public, verifiable track-record page from the signal journal.

This is the honesty ledger (the product's moat): every directional signal that
was actually emitted is shown, wins AND losses, and each entry is bound into a
SHA-256 hash chain so nothing can be retroactively edited or deleted without
breaking the chain. The page is fully static (index.html + trackrecord.json) so
it can be hosted for $0 on any static host (GitHub Pages, Cloudflare Pages...).

Verification: anyone can re-download the raw journal + bars, re-run this exact
tool, and check the published hash-chain head still matches. If a single entry
was altered or dropped, every later hash changes and the head no longer matches.

Usage:
  python tools/track_record.py --signals <journal.jsonl> --bars <bars.jsonl> \
      --out <site_dir>
"""

import argparse
import hashlib
import html
import json
import os
import re
import shutil
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
            if not ins:
                continue
            bars[ins].append((int(j["t"]), float(j["h"]), float(j["l"])))
    for ins in bars:
        seq = sorted(set(bars[ins]))
        bars[ins] = [list(r) for r in seq]
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
    return {
        "instrument": m.group(1),
        "side": m.group(2),
        "entry": float(e.group(1)),
        "stop": float(s.group(1)),
        "target": float(t.group(1)),
    }


def resolve(sig, bars, ts):
    ins = sig["instrument"]
    if ins not in bars:
        return None
    seq = bars[ins]
    start = None
    for i, (t, h, l) in enumerate(seq):
        if t > ts:
            start = i
            break
    if start is None:
        return None
    for k in range(start, len(seq)):
        _, h, l = seq[k]
        if sig["side"] == "LONG":
            stop_hit = l <= sig["stop"]
            target_hit = h >= sig["target"]
        else:
            stop_hit = h >= sig["stop"]
            target_hit = l <= sig["target"]
        if stop_hit:  # stop-first when both touched in the same bar (conservative)
            return "stop"
        if target_hit:
            return "target"
    return "open"


def canonical(obj):
    return json.dumps(obj, sort_keys=True, separators=(",", ":"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--signals", required=True)
    ap.add_argument("--bars", required=True)
    ap.add_argument("--out", required=True, help="output directory for index.html + trackrecord.json")
    a = ap.parse_args()

    bars = load_bars(a.bars)
    entries = []

    if os.path.exists(a.signals):
        with open(a.signals, encoding="utf-8") as f:
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
                    "ts": ts,
                    "instrument": sig["instrument"],
                    "side": sig["side"],
                    "entry": sig["entry"],
                    "stop": sig["stop"],
                    "target": sig["target"],
                    "outcome": outcome if outcome else "open",
                }
                if outcome in ("target", "stop"):
                    stop_dist = abs(sig["entry"] - sig["stop"])
                    if outcome == "target":
                        rec["r"] = round(abs(sig["target"] - sig["entry"]) / stop_dist, 3) if stop_dist else 0
                    else:
                        rec["r"] = -1.0
                entries.append(rec)

    # Build the hash chain over entries sorted by timestamp.
    entries.sort(key=lambda r: r["ts"])
    prev = "0" * 64
    for rec in entries:
        rec["prev_hash"] = prev
        body = canonical({k: rec[k] for k in
                          ("ts", "instrument", "side", "entry", "stop", "target",
                           "outcome", "r")
                          if k in rec})
        h = hashlib.sha256((prev + body).encode("utf-8")).hexdigest()
        rec["hash"] = h
        prev = h
    chain_head = prev if entries else "0" * 64

    # Summary (resolved only).
    per = defaultdict(lambda: {"emitted": 0, "wins": 0, "losses": 0, "r": 0.0, "open": 0})
    for rec in entries:
        it = per[rec["instrument"]]
        it["emitted"] += 1
        if rec["outcome"] == "target":
            it["wins"] += 1
            it["r"] += rec.get("r", 0)
        elif rec["outcome"] == "stop":
            it["losses"] += 1
            it["r"] += rec.get("r", 0)
        elif rec["outcome"] == "open":
            it["open"] += 1
    summary = {}
    for ins, it in sorted(per.items()):
        resolved = it["wins"] + it["losses"]
        summary[ins] = {
            "emitted": it["emitted"],
            "wins": it["wins"],
            "losses": it["losses"],
            "open": it["open"],
            "win_rate_resolved": round(it["wins"] / resolved, 4) if resolved else 0.0,
            "avg_r_resolved": round(it["r"] / resolved, 3) if resolved else 0.0,
        }

    total_resolved = sum(v["wins"] + v["losses"] for v in summary.values())
    total_wins = sum(v["wins"] for v in summary.values())

    payload = {
        "generated_at": __import__("datetime").datetime.utcnow().isoformat() + "Z",
        "chain_head": chain_head,
        "entry_count": len(entries),
        "resolved_count": total_resolved,
        "win_count": total_wins,
        "summary": summary,
        "entries": entries,
        "method": (
            "Each entry is bound into a SHA-256 chain: hash(i) = sha256("
            "hash(i-1) + canonical(entry)). Re-running tools/track_record.py on "
            "the raw journal must reproduce chain_head. Outcomes resolve stop-first "
            "on same-bar conflict; entry at signal close. Not financial advice."
        ),
    }

    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "trackrecord.json"), "w", encoding="utf-8") as f:
        f.write(json.dumps(payload, indent=2, sort_keys=True))

    # Ship the raw evidence alongside the page so anyone can verify the chain
    # from the hosted files alone (no re-download of build internals).
    raw_signals = os.path.basename(a.signals)
    raw_bars = os.path.basename(a.bars)
    if os.path.exists(a.signals):
        shutil.copy(a.signals, os.path.join(a.out, raw_signals))
    if os.path.exists(a.bars):
        shutil.copy(a.bars, os.path.join(a.out, raw_bars))

    def esc(x):
        return html.escape(str(x))

    rows = ""
    for i, rec in enumerate(reversed(entries)):
        cls = "win" if rec["outcome"] == "target" else ("loss" if rec["outcome"] == "stop" else "open")
        label = {"target": "TARGET", "stop": "STOP", "open": "OPEN"}[rec["outcome"]]
        rows += (
            f'<tr class="{cls}"><td>{i + 1}</td>'
            f"<td>{esc(rec['instrument'])}</td><td>{rec['side']}</td>"
            f"<td>${rec['entry']:,.1f}</td><td>${rec['stop']:,.1f}</td>"
            f"<td>${rec['target']:,.1f}</td><td>{label}</td>"
            f"<td>{rec.get('r', '')}</td>"
            f'<td class="mono" title="{rec["hash"]}">{rec["hash"][:12]}&hellip;</td></tr>'
        )

    summary_rows = ""
    for ins, v in sorted(summary.items()):
        summary_rows += (
            f"<tr><td><b>{esc(ins)}</b></td><td>{v['emitted']}</td>"
            f"<td>{v['wins']}</td><td>{v['losses']}</td><td>{v['open']}</td>"
            f"<td>{v['win_rate_resolved'] * 100:.1f}%</td>"
            f"<td>{v['avg_r_resolved']:+.2f}R</td></tr>"
        )

    html_doc = f"""<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Aurum Signals — Verifiable Track Record</title>
<style>
  body{{font-family:-apple-system,Segoe UI,Roboto,sans-serif;max-width:900px;margin:2rem auto;padding:0 1rem;color:#1c1c1e;background:#fff}}
  h1{{font-size:1.5rem}} .ledger{{background:#f4f4f6;border:1px solid #ddd;padding:1rem;border-radius:8px;font-size:.9rem}}
  .mono{{font-family:monospace;font-size:.75rem;color:#666}}
  table{{border-collapse:collapse;width:100%;margin-top:1rem}}
  th,td{{border:1px solid #e0e0e0;padding:.4rem .5rem;text-align:left;font-size:.85rem}}
  th{{background:#fafafa}} tr.win td{{background:#e9f7ef}} tr.loss td{{background:#fdeeee}}
  tr.open td{{background:#fbf6ec}} .head{{font-size:.8rem;color:#555}}
</style></head><body>
<h1>Aurum Signals&nbsp;&mdash; Verifiable Track Record</h1>
<p>Every signal this engine emitted, wins and losses, bound into a SHA-256 hash
chain so nothing can be edited or deleted retroactively. Download
<a href="trackrecord.json">trackrecord.json</a> and re-run
<code>tools/track_record.py</code> on the raw journal to verify
<code>chain_head</code> yourself.</p>
<div class="ledger">
  <b>Chain head:</b> <span class="mono">{esc(chain_head)}</span><br>
  <b>{len(entries)}</b> signals emitted &middot; <b>{total_resolved}</b> resolved
  &middot; <b>{total_wins}</b> wins &middot;
  <b>{total_resolved - total_wins}</b> losses
</div>
<h2>Summary by instrument</h2>
<table><thead><tr><th>Instrument</th><th>Emitted</th><th>Wins</th><th>Losses</th>
<th>Open</th><th>Win rate (resolved)</th><th>Avg R (resolved)</th></tr></thead>
<tbody>{summary_rows}</tbody></table>
<h2>Signals (newest first)</h2>
<table><thead><tr><th>#</th><th>Instrument</th><th>Side</th><th>Entry</th><th>Stop</th>
<th>Target</th><th>Outcome</th><th>R</th><th>Hash</th></tr></thead>
<tbody>{rows}</tbody></table>
<p class="head">Honesty policy: we publish losses as openly as wins. Outcomes
resolve stop-first on same-bar conflicts; entry at signal close. This is
decision-support data, not financial advice. We never guarantee profit.</p>
</body></html>"""

    with open(os.path.join(a.out, "index.html"), "w", encoding="utf-8") as f:
        f.write(html_doc)

    print("== track record ==")
    print(f"entries={len(entries)} resolved={total_resolved} wins={total_wins} "
          f"chain_head={chain_head}")
    print(f"wrote {a.out}\\trackrecord.json and {a.out}\\index.html")


if __name__ == "__main__":
    try:
        main()
    except BrokenPipeError:
        sys.exit(0)
