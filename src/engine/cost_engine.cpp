#include "src/engine/cost_engine.h"

#include <cmath>

#include "src/core/order_book.h"

namespace hftarb {

CostEngine::CostEngine(CostConfig cfg) : cfg_(cfg) {}

double CostEngine::grossSpread(Price buyPrice, Price sellPrice) const {
    if (buyPrice <= 0.0) return 0.0;
    return sellPrice / buyPrice - 1.0;
}

std::optional<CostBreakdown> CostEngine::evaluate(
    const OrderBook& buyBook, const OrderBook& sellBook, Quantity quantity,
    const FeeSchedule& buyFees, const FeeSchedule& sellFees) const {
    CostBreakdown c;
    c.quantity = quantity;

    const auto buyVwap = OrderBookUtils::avgBuyPrice(buyBook, quantity);
    const auto sellVwap = OrderBookUtils::avgSellPrice(sellBook, quantity);
    if (!buyVwap || !sellVwap) return std::nullopt;  // insufficient liquidity

    c.buyPrice = *buyVwap;
    c.sellPrice = *sellVwap;
    c.notional = c.buyPrice * quantity;
    c.grossSpread = grossSpread(c.buyPrice, c.sellPrice);
    if (c.grossSpread <= 0.0) return std::nullopt;  // no edge in this direction

    c.buyFee = buyFees.takerBuyFee;
    c.sellFee = sellFees.takerSellFee;
    // Slippage is informational only: the VWAP prices used for grossSpread
    // already reflect walking the book, so subtracting it again would double
    // count the cost of hitting the levels.
    c.slippage = OrderBookUtils::buySlippage(buyBook, quantity) +
                 OrderBookUtils::sellSlippage(sellBook, quantity);
    // Network cost as fraction of notional + a fixed per-unit cost.
    c.networkCost = (buyFees.networkCost + sellFees.networkCost) / 2.0 +
                    (buyFees.networkCostPerUnit + sellFees.networkCostPerUnit) / c.notional;
    c.safetyMargin = cfg_.safetyMargin;

    c.netSpread = c.grossSpread - c.buyFee - c.sellFee -
                  c.networkCost - c.safetyMargin;
    return c;
}

}  // namespace hftarb
