"""Generate a synthetic dataset so the backtest / ML / replay tooling can run
standalone, without needing the C++ bot first.

Writes:
  data/synthetic_ticks.csv         - snapshots for replay
  data/synthetic_opportunities.csv - detected opportunities for backtest/ML
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pandas as pd

EXCHANGES = {
    "exchange_a": {"base": 100_000.0, "fee": 0.001},
    "exchange_b": {"base": 100_150.0, "fee": 0.001},
    "exchange_c": {"base": 101_000.0, "fee": 0.0005},
}

ROWS = 5000
MIN_NET_SPREAD = 0.002
SAFETY_MARGIN = 0.0005


def generate(out_dir: Path, seed: int = 7) -> None:
    rng = np.random.default_rng(seed)
    rows = []
    symbols = ["BTC/USDT"]

    ts = 1_700_000_000_000
    for _ in range(ROWS):
        for ex, spec in EXCHANGES.items():
            fair = spec["base"] * rng.normal(1.0, 0.0004)
            half = fair * 0.000075
            best_ask = fair + half + rng.uniform(0, fair * 0.0001)
            best_bid = fair - half - rng.uniform(0, fair * 0.0001)
            rows.append(
                {"ts": ts, "symbol": "BTC/USDT", "exchange": ex,
                 "best_ask": round(best_ask, 2), "best_bid": round(best_bid, 2)}
            )
        ts += 10

    ticks = pd.DataFrame(rows)
    ticks.to_csv(out_dir / "synthetic_ticks.csv", index=False)

    # Detect opportunities across the same timestamp (crude best-price check).
    opps = []
    for _, group in ticks.groupby("ts"):
        for b in EXCHANGES:
            for s in EXCHANGES:
                if b == s:
                    continue
                buy = group[group["exchange"] == b]["best_ask"].iloc[0]
                sell = group[group["exchange"] == s]["best_bid"].iloc[0]
                gross = sell / buy - 1.0
                net = gross - EXCHANGES[b]["fee"] - EXCHANGES[s]["fee"] - SAFETY_MARGIN
                if net >= MIN_NET_SPREAD:
                    # ~15% get rejected by the risk gate so the ML model sees
                    # both classes (mirrors the live pipeline's decision mix).
                    # Rejected candidates carry worse latency — the feature the
                    # model should learn to key on.
                    decision = "accepted" if rng.random() > 0.15 else "risk_rejected"
                    latency = rng.uniform(0.1, 2.0) if decision == "accepted" \
                        else rng.uniform(5.0, 50.0)
                    opps.append(
                        {
                            "ts": int(group["ts"].iloc[0]),
                            "symbol": "BTC/USDT",
                            "buy_exchange": b,
                            "sell_exchange": s,
                            "quantity": 0.1,
                            "buy_price": buy,
                            "sell_price": sell,
                            "gross_spread": gross,
                            "buy_fee": EXCHANGES[b]["fee"],
                            "sell_fee": EXCHANGES[s]["fee"],
                            "slippage": 0.0,
                            "network_cost": 0.0,
                            "safety_margin": SAFETY_MARGIN,
                            "net_spread": net,
                            "latency_ms": latency,
                            "decision": decision,
                        }
                    )
    opps_df = pd.DataFrame(opps)
    opps_df.to_csv(out_dir / "synthetic_opportunities.csv", index=False)
    print(f"wrote {len(ticks)} ticks, {len(opps_df)} opportunities")


if __name__ == "__main__":
    generate(Path("data"))
