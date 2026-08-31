#pragma once

#include "src/core/types.h"

namespace hftarb {

// Per-exchange fee and cost parameters (fractions of notional).
struct FeeSchedule {
    double takerBuyFee = 0.001;    // e.g. 0.1%
    double takerSellFee = 0.001;
    double networkCost = 0.0;      // blockchain transfer, only when moving assets
    double networkCostPerUnit = 0.0;  // fixed cost per unit of asset
};

struct CostConfig {
    double safetyMargin = 0.0005;    // extra buffer on top of everything
    double minNetSpread = 0.002;     // trigger threshold: 0.20%
    double networkCostForSpot = 0.0; // intra-exchange spot needs none
};

// Given two exchange books and the required quantity, produce the full cost
// breakdown. This is the heart of the strategy: the "gross" number on screen
// is rarely the number you actually get.
class CostEngine {
public:
    explicit CostEngine(CostConfig cfg);

    // Returns nullopt when the quantity cannot be filled on either side,
    // or when gross spread is non-positive (no arbitrage direction).
    std::optional<CostBreakdown> evaluate(
        const OrderBook& buyBook, const OrderBook& sellBook,
        Quantity quantity, const FeeSchedule& buyFees, const FeeSchedule& sellFees) const;

    double grossSpread(Price buyPrice, Price sellPrice) const;

    // Raw knobs the strategy-level gates read (fees network, safety margin...).
    const CostConfig& config() const { return cfg_; }

    bool meetsThreshold(const CostBreakdown& c) const {
        return c.netSpread >= cfg_.minNetSpread;
    }

private:
    CostConfig cfg_;
};

}  // namespace hftarb
