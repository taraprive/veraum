#!/usr/bin/env python3
"""MT5 live bar bridge for the hft_arbitrage_bot directional engines.

Connects to a RUNNING, logged-in MetaTrader 5 terminal and emits newly
*closed* bars of the configured instruments as JSON Lines, one per line:

    {"i":"XAUUSD","p":2405.1,"o":2402.0,"h":2408.2,"l":2401.4,"t":1735689600000}

  i : resolved MT5 symbol name
  p : close of the just-closed bar
  o/h/l : open/high/low of that bar
  t : bar open time in ms UTC

Usage:
  mt5_feed.py --probe                          # account + last bars, exit
  mt5_feed.py [--tf H1|M15|M5] [--out file]    # stream JSONL to file (or stdout)
  mt5_feed.py --dump H1 20000 outdir           # full OHLC history to CSVs, exit

Symbols are matched by alias (gold/silver/Nasdaq) against whatever the broker
calls them. Bars that annoyingly refuse to resolve are printed with the full
available symbol list so the mapping can be fixed.
"""
import argparse
import datetime as dt
import json
import os
import sys
import time

try:
    import MetaTrader5 as mt5
except ImportError:
    sys.exit("install the official library first: pip install MetaTrader5")

INSTRUMENTS = {
    "XAUUSD": ["XAUUSD", "GOLD", "XAUUSD.a", "XAUUSDcash", "GOLDSUSD"],
    "XAGUSD": ["XAGUSD", "SILVER", "XAGUSD.a", "AGUSD", "SILVERUSD"],
    "NDX100": ["NDX100", "USTEC", "NAS100", "US100", "US100.a", "USTEC.a"],
}

# Many brokers rename symbols with a suffix (.m, .std, .a, ...). Match by
# base name with the common suffixes appended to every alias.
SUFFIXES = ("", ".m", ".std", ".a")

TF_SECONDS = {"M1": 60, "M5": 300, "M15": 900, "M30": 1800, "H1": 3600, "H4": 14400, "D1": 86400}


def resolve_symbols():
    avail = [s.name for s in (mt5.symbols_get() or [])]
    avail_lower = {n.lower(): n for n in avail}
    resolved = {}
    missing = []
    for want, aliases in INSTRUMENTS.items():
        for a in aliases:
            for suf in SUFFIXES:
                cand = (a + suf).lower()
                if cand in avail_lower:
                    resolved[want] = avail_lower[cand]
                    break
            if want in resolved:
                break
        if want not in resolved:
            missing.append(want)
    return resolved, missing, avail


def last_bars(symbol, tf, count):
    return mt5.copy_rates_from_pos(symbol, mt5.TIMEFRAME_M1 if tf == "M1" else
                                   getattr(mt5, "TIMEFRAME_" + tf), 0, count)


def dump(tf, count, outdir):
    os.makedirs(outdir, exist_ok=True)
    resolved, missing, _ = resolve_symbols()
    if missing:
        print("WARNING unresolved:", ", ".join(missing))
    for want, sym in resolved.items():
        rates = last_bars(sym, tf, count)
        if rates is None or len(rates) == 0:
            print(f"  {want}: no history returned")
            continue
        path = os.path.join(outdir, f"{want}_{tf}.csv")
        with open(path, "w", encoding="utf-8") as f:
            f.write("date,open,high,low,close\n")
            for row in rates:
                t = dt.datetime.utcfromtimestamp(row[0]).strftime("%Y-%m-%d %H:%M")
                f.write(f"{t},{row[1]:.3f},{row[2]:.3f},{row[3]:.3f},{row[4]:.3f}\n")
        first = dt.datetime.utcfromtimestamp(rates[0][0]).strftime("%Y-%m-%d")
        last = dt.datetime.utcfromtimestamp(rates[-1][0]).strftime("%Y-%m-%d")
        print(f"  {want:8s} -> {sym:12s} {len(rates):6d} bars ({first} .. {last}) -> {path}")
    return 0


def probe():
    info = mt5.account_info()
    if info is None:
        return "NOT logged in (open the terminal, login to your demo account, retry)"
    resolved, missing, avail = resolve_symbols()
    print(f"account {info.login} | {info.name} | {info.currency} | equity {info.equity:.2f} | "
          f"leverage 1:{info.leverage}")
    for want, sym in resolved.items():
        rates = last_bars(sym, "H1", 1)
        if rates is None or len(rates) == 0:
            print(f"  {want:8s} -> {sym:12s} (no bars)")
            continue
        r = rates[-1]
        t = _time(r[0])
        print(f"  {want:8s} -> {sym:12s} close {r[4]:.2f}  @ {t} UTC")
    if missing:
        print("UNRESOLVED:", ", ".join(missing))
        print("sample of available symbols:", ", ".join(avail[:60]) + "...")
    return None


def _time(ts_second):
    return dt.datetime.utcfromtimestamp(ts_second).strftime("%Y-%m-%d %H:%M")


def stream(tf, out):
    resolved, missing, avail = resolve_symbols()
    if missing:
        print("WARNING unresolved symbols:", ", ".join(missing))
        print("available:", ", ".join(avail[:80]), file=sys.stderr)
        for m in missing:
            INSTRUMENTS.pop(m, None)
    if not resolved:
        sys.exit("no instruments resolved; fix aliases above")
    print("streaming bars:", {k: v for k, v in resolved.items()},
          f"tf={tf} -> {out or 'stdout'}", flush=True)

    opened = {want: 0 for want in resolved}  # track last bar open time (seconds)
    while True:
        for want, sym in resolved.items():
            tf_sec = TF_SECONDS[tf]
            # keep a small window so a missed bar is not lost on reconnect
            rates = last_bars(sym, tf, 8)
            if rates is None:
                continue
            for r in rates:
                if r[4] == 0.0:
                    continue
                if r[0] > opened[want]:
                    line = {
                        "i": want,
                        "p": round(r[4], 3),
                        "o": round(r[1], 3),
                        "h": round(r[2], 3),
                        "l": round(r[3], 3),
                        "t": int(r[0]) * 1000,
                    }
                    opened[want] = r[0]
                    s = json.dumps(line, separators=(",", ":"))
                    if out:
                        with open(out, "a", encoding="utf-8") as f:
                            f.write(s + "\n")
                    else:
                        print(s, flush=True)
        time.sleep(TF_SECONDS[tf] / 2.0 if tf != "M1" else 10)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe", action="store_true")
    ap.add_argument("--dump", nargs=3, metavar=("TF", "COUNT", "OUTDIR"),
                    help="write OHLC history CSVs and exit")
    ap.add_argument("--tf", default="H1", choices=sorted(TF_SECONDS))
    ap.add_argument("--out")
    a = ap.parse_args()
    if not mt5.initialize():
        sys.exit(f"can't connect to running terminal: {mt5.last_error()}")
    try:
        if a.dump:
            return dump(a.dump[0], int(a.dump[1]), a.dump[2])
        if a.probe:
            err = probe()
            sys.exit(0 if err is None else 2)
        stream(a.tf, a.out)
    finally:
        mt5.shutdown()
    return 0


if __name__ == "__main__":
    main()