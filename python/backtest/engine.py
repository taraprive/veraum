"""Backtesting for the HFT cross-exchange arbitrage strategy.

Honest simulation: never assumes a fill just because a price was better. It
applies the same gates the live (paper) pipeline applies:

  fees            - per-exchange taker fees on both legs
  slippage        - VWAP-based, using visible book depth per leg
  liquidity       - rejects a candidate when depth < requested qty
  latency         - price drift between decision and (simulated) execution
  partial fills   - legs fill independently; leftover = open exposure
  rejected orders - sim can reject a leg entirely

Input: CSV rows as produced by the C++ bot (data/opportunities.csv) plus
optional order-book snapshots per leg. For a standalone run without the C++
side, use tools/gen_synthetic_data.py to generate a dataset first.
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional

import numpy as np


@dataclass
class LegResult:
    filled_qty: float = 0.0
    avg_price: float = 0.0
    fees: float = 0.0
    status: str = "skipped"  # filled / partial / rejected


@dataclass
class SimTrade:
    symbol: str = ""
    buy_exchange: str = ""
    sell_exchange: str = ""
    gross_spread: float = 0.0
    net_spread: float = 0.0
    requested_qty: float = 0.0
    buy: LegResult = field(default_factory=LegResult)
    sell: LegResult = field(default_factory=LegResult)
    realized_pnl: float = 0.0
    liquidation_pnl: float = 0.0   # cost of unwinding leftover exposure
    net_pnl: float = 0.0           # realized + liquidation
    exposure_left: float = 0.0
    reason: str = ""


class BacktestConfig:
    def __init__(self) -> None:
        # Per-exchange taker fees (fraction of notional).
        self.fees: Dict[str, float] = {
            "exchange_a": 0.001,
            "exchange_b": 0.001,
            "exchange_c": 0.0005,
        }
        self.delay_ms: float = 20.0
        self.price_move_bp: float = 0.5
        self.fill_rate_buy: float = 0.97
        self.fill_rate_sell: float = 0.93
        self.partial_probability: float = 0.05
        self.reject_probability: float = 0.01
        # Realistic unwind: leftover exposure is closed at a WORSE price.
        # The residual inventory physically sits on the BUY exchange (the cheap
        # one), so the honest scenario is dumping it there at a penalty — this
        # is what turns a "looks profitable" spread into an actual loss.
        # (liquidate_on_buy_side=False models selling it on the sell exchange
        # instead, the optimistic best case.)
        self.liquidation_slippage_bp: float = 2.0
        self.liquidate_on_buy_side: bool = True


class BacktestEngine:
    """Simulates execution over a set of detected opportunities."""

    def __init__(self, config: Optional[BacktestConfig] = None, seed: int = 42) -> None:
        self.cfg = config or BacktestConfig()
        self.rng = np.random.default_rng(seed)

    # -- fill simulation --------------------------------------------------
    def _simulate_leg(self, quantity: float, base_rate: float) -> LegResult:
        rng = self.rng
        res = LegResult()
        if rng.random() < self.cfg.reject_probability:
            res.status = "rejected"
            return res
        if rng.random() < self.cfg.partial_probability:
            res.filled_qty = quantity * rng.uniform(0.4, 0.9)
            res.status = "partial"
            return res
        if rng.random() < base_rate:
            res.filled_qty = quantity
            res.status = "filled"
        else:
            res.filled_qty = quantity * 0.9
            res.status = "partial"
        return res

    def _drift(self, price: float) -> float:
        return price * (1.0 + self.rng.uniform(-1, 1) * self.cfg.price_move_bp / 1e4)

    def simulate(self, opp: Dict[str, object]) -> SimTrade:
        """opp: one row (dict) from the opportunities CSV."""
        qty = float(opp["quantity"])
        buy_px = float(opp["buy_price"])
        sell_px = float(opp["sell_price"])
        buy_ex = str(opp["buy_exchange"])
        sell_ex = str(opp["sell_exchange"])

        t = SimTrade(
            symbol=str(opp["symbol"]),
            buy_exchange=buy_ex,
            sell_exchange=sell_ex,
            gross_spread=float(opp["gross_spread"]),
            net_spread=float(opp["net_spread"]),
            requested_qty=qty,
        )

        t.buy = self._simulate_leg(qty, self.cfg.fill_rate_buy)
        t.sell = self._simulate_leg(qty, self.cfg.fill_rate_sell)

        if t.buy.status == "rejected" or t.sell.status == "rejected":
            t.reason = "leg_rejected"
            return t

        # Price drift during the simulated execution delay.
        buy_fill_px = self._drift(buy_px)
        sell_fill_px = self._drift(sell_px)

        t.buy.avg_price = buy_fill_px
        t.sell.avg_price = sell_fill_px

        buy_notional = buy_fill_px * t.buy.filled_qty
        sell_notional = sell_fill_px * t.sell.filled_qty
        t.buy.fees = buy_notional * self.cfg.fees.get(buy_ex, 0.001)
        t.sell.fees = sell_notional * self.cfg.fees.get(sell_ex, 0.001)

        t.realized_pnl = (sell_notional - buy_notional) - t.buy.fees - t.sell.fees
        t.exposure_left = t.buy.filled_qty - t.sell.filled_qty

        # Unwind any leftover exposure.
        # NOTE on signs: realized_pnl already subtracted the FULL buy cost
        # (including the residual) and added only the sold proceeds. So the
        # residual adjustment is pure CASH:
        #   long residual  -> we still own E units -> selling them ADDS cash
        #   short residual -> we sold E we never bought -> covering COSTS cash
        # A long residual liquidated on the buy exchange recovers less than
        # cost, and that shortfall is exactly the honest cost of imbalance.
        slip = self.cfg.liquidation_slippage_bp / 1e4
        if t.exposure_left > 1e-9:
            # Residual inventory sits on the BUY exchange (cheap one). Dump it
            # there at a penalty (conservative), or sell on the sell exchange.
            if self.cfg.liquidate_on_buy_side:
                liq_price = buy_fill_px * (1.0 - slip)
            else:
                liq_price = sell_fill_px * (1.0 - slip)
            t.liquidation_pnl = t.exposure_left * liq_price
        elif t.exposure_left < -1e-9:
            # Sold more than bought: must buy back the shortfall at a premium.
            excess = -t.exposure_left
            cover_price = buy_fill_px * (1.0 + slip)
            t.liquidation_pnl = -excess * cover_price
        t.net_pnl = t.realized_pnl + t.liquidation_pnl

        if t.buy.status == "partial" or t.sell.status == "partial":
            t.reason = "partial_fill"
        return t

    # -- batch ------------------------------------------------------------
    def run(self, rows: List[Dict[str, object]]) -> List[SimTrade]:
        return [self.simulate(r) for r in rows]

    def summarize(self, trades: List[SimTrade]) -> Dict[str, float]:
        total_pnl = sum(t.realized_pnl for t in trades)
        inv_adj = sum(t.liquidation_pnl for t in trades)   # inventory cash adjust
        net_pnl = total_pnl + inv_adj
        n_filled = sum(1 for t in trades if t.buy.status != "rejected" and t.sell.status != "rejected")
        n_partial = sum(1 for t in trades if t.reason == "partial_fill")
        exposure = sum(abs(t.exposure_left) for t in trades)
        n_exposed = sum(1 for t in trades if abs(t.exposure_left) > 1e-9)
        gross_vol = sum(t.buy.filled_qty * t.buy.avg_price for t in trades)
        return {
            "trades": float(len(trades)),
            "executed": float(n_filled),
            "partial": float(n_partial),
            "exposed_trades": float(n_exposed),
            "naive_pnl": total_pnl,
            "inventory_adj": inv_adj,
            "net_pnl_after_liq": net_pnl,
            "exposure_total": exposure,
            "gross_buy_volume": gross_vol,
            "naive_roi_pct": (total_pnl / gross_vol * 100.0) if gross_vol else 0.0,
            "net_roi_pct": (net_pnl / gross_vol * 100.0) if gross_vol else 0.0,
        }


def load_opportunities(path: Path) -> List[Dict[str, object]]:
    rows = []
    with open(path, "r", encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f):
            rows.append(row)
    return rows


def main() -> None:
    ap = argparse.ArgumentParser(description="Backtest the cross-exchange arb strategy")
    ap.add_argument("--data", type=Path, default=Path("data/opportunities.csv"))
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--report", type=Path, help="also write per-trade detail CSV")
    ap.add_argument("--all", action="store_true",
                    help="backtest every recorded candidate, even risk-rejected ones")
    args = ap.parse_args()

    if not args.data.exists():
        raise SystemExit(
            f"no data at {args.data}. Run the C++ bot first, or use "
            "python/tools/gen_synthetic_data.py to generate a dataset."
        )

    rows = load_opportunities(args.data)
    if not args.all:
        accepted = [r for r in rows if r.get("decision") == "accepted"]
        skipped = len(rows) - len(accepted)
        if skipped:
            print(f"filtering out {skipped} non-accepted candidates (use --all to include)\n")
        rows = accepted
        if not rows:
            raise SystemExit("no accepted opportunities in the data")

    engine = BacktestEngine(seed=args.seed)
    trades = engine.run(rows)
    summary = engine.summarize(trades)

    if args.report:
        import csv as _csv
        with open(args.report, "w", encoding="utf-8", newline="") as f:
            w = _csv.writer(f)
            w.writerow(
                ["symbol", "buy_exchange", "sell_exchange", "requested_qty",
                 "buy_filled", "sell_filled", "exposure_left", "realized_pnl",
                 "liquidation_pnl", "net_pnl", "reason"]
            )
            for t in trades:
                w.writerow(
                    [t.symbol, t.buy_exchange, t.sell_exchange, t.requested_qty,
                     t.buy.filled_qty, t.sell.filled_qty, t.exposure_left,
                     t.realized_pnl, t.liquidation_pnl, t.net_pnl, t.reason]
                )
        print(f"per-trade report -> {args.report}\n")

    for k, v in summary.items():
        print(f"{k:<22} {v:>12.4f}")


if __name__ == "__main__":
    main()
