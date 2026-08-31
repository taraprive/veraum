"""Fill-probability model.

The AI is an *assistant*, never the strategy itself: it consumes the same
features the pipeline already computes and outputs P(both legs fill fully),
which the risk engine can use as an additional gate.

Usage: train on historical decisions, then export the per-candidate score.
"""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np

from backtest.engine import load_opportunities


def _features(row) -> np.ndarray:
    """Feature vector per opportunity (mirrors the C++ CostBreakdown)."""
    return np.array(
        [
            float(row.get("gross_spread", 0.0)),
            float(row.get("slippage", 0.0)),
            float(row.get("quantity", 0.0)),
            float(row.get("latency_ms", 0.0)),
        ],
        dtype=float,
    )


class LogisticFillModel:
    """Tiny pure-numpy logistic regression; swaps in cleanly for sklearn."""

    def __init__(self, lr: float = 0.1, epochs: int = 2000) -> None:
        self.w: np.ndarray | None = None
        self.b: float = 0.0
        self.lr = lr
        self.epochs = epochs

    def _predict_proba(self, X: np.ndarray) -> np.ndarray:
        z = X @ self.w + self.b
        return 1.0 / (1.0 + np.exp(-np.clip(z, -30, 30)))

    def fit(self, X: np.ndarray, y: np.ndarray) -> None:
        X = np.asarray(X, dtype=float)
        y = np.asarray(y, dtype=float)
        n, d = X.shape
        self.w = np.zeros(d)
        for _ in range(self.epochs):
            p = self._predict_proba(X)
            grad = (X.T @ (p - y)) / n
            self.w -= self.lr * grad
            self.b -= self.lr * float(np.mean(p - y))

    def predict_proba(self, X: np.ndarray) -> np.ndarray:
        return self._predict_proba(np.asarray(X, dtype=float))


def build_training_set(rows) -> tuple[np.ndarray, np.ndarray]:
    """Label = 1 when the decision was accepted (proxy for a good fill)."""
    X, y = [], []
    for r in rows:
        decision = str(r.get("decision", ""))
        if decision == "accepted":
            X.append(_features(r))
            y.append(1.0)
        elif decision in ("risk_rejected", "cooldown", "size_rejected", "disconnected"):
            X.append(_features(r))
            y.append(0.0)
    if not X:
        raise SystemExit("no labelled rows to train on")
    return np.array(X), np.array(y)


def main() -> None:
    ap = argparse.ArgumentParser(description="Train the fill-probability model")
    ap.add_argument("--data", type=Path, default=Path("data/opportunities.csv"))
    args = ap.parse_args()

    rows = load_opportunities(args.data)
    X, y = build_training_set(rows)
    model = LogisticFillModel()
    model.fit(X, y)
    print(f"trained on {len(y)} labelled opportunities, features={X.shape[1]}")
    print(f"weights={np.round(model.w, 6)} bias={model.b:.6f}")

    # Report mean predicted fill probability per observed class.
    print("mean P(full fill) accepted:", round(float(model.predict_proba(X[y == 1]).mean()), 4))
    if (y == 0).any():
        print("mean P(full fill) rejected:", round(float(model.predict_proba(X[y == 0]).mean()), 4))


if __name__ == "__main__":
    main()
