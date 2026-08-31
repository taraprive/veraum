"""Replay engine.

Feeds a recorded stream of normalized order-book snapshots through a
pure-Python re-implementation of the arbitrage decision path and re-emits the
same opportunity CSV the C++ bot would have produced. Useful for regression
tests and for verifying pipeline behaviour on historical data offline.
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
from typing import Dict, List


class Replayer:
    def __init__(self, min_net_spread: float = 0.002) -> None:
        self.min_net_spread = min_net_spread
        self.books: Dict[str, Dict[str, dict]] = {}  # (symbol, exchange) -> book

    def on_tick(self, exchange: str, symbol: str, asks, bids, ts: float) -> List[Dict]:
        """Insert one snapshot; returns opportunities found for this symbol."""
        self.books.setdefault(symbol, {})[exchange] = {"asks": asks, "bids": bids, "ts": ts}
        opps = []
        books = self.books[symbol]
        exchanges = [ex for ex in books.keys()]

        for buy_ex in exchanges:
            for sell_ex in exchanges:
                if buy_ex == sell_ex:
                    continue
                best_ask = books[buy_ex]["asks"][0]
                best_bid = books[sell_ex]["bids"][0]
                if best_ask[0] <= 0 or best_bid[0] <= 0:
                    continue
                gross = best_bid[0] / best_ask[0] - 1.0
                if gross <= self.min_net_spread:
                    continue
                opps.append(
                    {
                        "symbol": symbol,
                        "buy_exchange": buy_ex,
                        "sell_exchange": sell_ex,
                        "gross_spread": gross,
                        "net_spread": gross - 0.002,  # rough fees/margin proxy
                    }
                )
        return opps


def main() -> None:
    ap = argparse.ArgumentParser(description="Replay recorded snapshots through the decision path")
    ap.add_argument("--input", type=Path, required=True, help="CSV: ts,symbol,exchange,best_ask,best_bid")
    ap.add_argument("--output", type=Path, default=Path("data/replayed_opportunities.csv"))
    args = ap.parse_args()

    replayer = Replayer()
    with open(args.input, "r", encoding="utf-8", newline="") as fin, open(
        args.output, "w", encoding="utf-8", newline=""
    ) as fout:
        writer = csv.writer(fout)
        writer.writerow(["symbol", "buy_exchange", "sell_exchange", "gross_spread", "net_spread"])
        count = 0
        for row in csv.DictReader(fin):
            ts = float(row["ts"])
            for opp in replayer.on_tick(
                row["exchange"], row["symbol"],
                [(float(row["best_ask"]), 1.0)],
                [(float(row["best_bid"]), 1.0)],
                ts,
            ):
                writer.writerow([opp[k] for k in opp])
                count += 1
    print(f"replayed {count} opportunities -> {args.output}")


if __name__ == "__main__":
    main()
