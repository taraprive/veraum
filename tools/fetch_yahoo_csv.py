#!/usr/bin/env python3
"""Fetch OHLC history from Yahoo Finance (no API key) and write CSV.

Usage: fetch_yahoo_csv.py <symbol> <interval> <range> <out.csv>
  symbol:   GC=F (gold)  SI=F (silver)  ^NDX (Nasdaq-100)  XAG= etc.
  interval: 1d 60m 5m 1m ...
  range:    3mo 6mo 1y 5d ...
"""
import json
import sys
import urllib.request

USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        return 2
    symbol, interval, span, out = sys.argv[1:5]
    url = ("https://query1.finance.yahoo.com/v8/finance/chart/"
           f"{symbol}?interval={interval}&range={span}")
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=45) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    result = data["chart"]["result"][0]
    ts = result["timestamp"]
    quote = result["indicators"]["quote"][0]
    with open(out, "w", newline="") as f:
        f.write("date,open,high,low,close\n")
        n = 0
        for i, t in enumerate(ts):
            o = quote["open"][i]
            h = quote["high"][i]
            c = quote["close"][i]
            lo = quote["low"][i]
            if c is None:
                continue
            import datetime

            day = datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime("%Y-%m-%d")
            if o is None:
                o = c
            if h is None:
                h = c
            if lo is None:
                lo = c
            f.write(f"{day},{o},{h},{lo},{c}\n")
            n += 1
    print(f"wrote {out}: {n} bars ({symbol} {interval} {span})")
    return 0


if __name__ == "__main__":
    sys.exit(main())