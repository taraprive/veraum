#include "src/engine/liquidity_engine.h"

#include "src/core/order_book.h"

namespace hftarb {

LiquidityEngine::LiquidityEngine(LiquidityConfig cfg) : cfg_(cfg) {}

LiquidityResult LiquidityEngine::check(const OrderBook& buyBook,
                                       const OrderBook& sellBook,
                                       Quantity quantity) const {
    LiquidityResult r;
    r.requested = quantity;

    const double availBuy = OrderBookUtils::availableQty(buyBook.asks);
    const double availSell = OrderBookUtils::availableQty(sellBook.bids);
    r.available = std::min(availBuy, availSell);

    if (r.available < quantity * cfg_.minAvailableRatio) {
        r.reason = "insufficient_visible_depth";
        return r;
    }

    const auto buyVwap = OrderBookUtils::avgBuyPrice(buyBook, quantity);
    const auto sellVwap = OrderBookUtils::avgSellPrice(sellBook, quantity);
    if (!buyVwap || !sellVwap) {
        r.reason = "vwap_unavailable";
        return r;
    }

    r.vwapBuy = *buyVwap;
    r.vwapSell = *sellVwap;
    r.slippageBuy = OrderBookUtils::buySlippage(buyBook, quantity);
    r.slippageSell = OrderBookUtils::sellSlippage(sellBook, quantity);

    if (r.slippageBuy + r.slippageSell > cfg_.maxAcceptableSlippage) {
        r.reason = "slippage_exceeds_cap";
        return r;
    }

    r.sufficient = true;
    return r;
}

}  // namespace hftarb
