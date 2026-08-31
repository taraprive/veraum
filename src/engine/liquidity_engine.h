#pragma once

#include "src/core/types.h"

namespace hftarb {

struct LiquidityConfig {
    double minAvailableRatio = 1.0;    // need >= 100% of requested qty visible
    double maxAcceptableSlippage = 0.001;  // 10 bp combined slippage cap
};

struct LiquidityResult {
    bool sufficient = false;
    double available = 0.0;
    double requested = 0.0;
    double vwapBuy = 0.0;
    double vwapSell = 0.0;
    double slippageBuy = 0.0;
    double slippageSell = 0.0;
    std::string reason;
};

// Decides whether the visible depth is enough to actually trade the requested
// size, and quantifies the resulting slippage. Never trusts the top-of-book
// price for the whole order.
class LiquidityEngine {
public:
    explicit LiquidityEngine(LiquidityConfig cfg);

    LiquidityResult check(const OrderBook& buyBook, const OrderBook& sellBook,
                          Quantity quantity) const;

private:
    LiquidityConfig cfg_;
};

}  // namespace hftarb
