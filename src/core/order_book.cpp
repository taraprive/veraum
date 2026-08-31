#include "src/core/order_book.h"

namespace hftarb {

std::optional<Price> OrderBookUtils::avgBuyPrice(const OrderBook& book, Quantity qty) {
    if (qty <= 0.0) return std::nullopt;
    double remaining = qty;
    double spent = 0.0;
    for (const auto& lvl : book.asks) {
        if (lvl.quantity <= 0.0) continue;
        const double take = (remaining < lvl.quantity) ? remaining : lvl.quantity;
        spent += take * lvl.price;
        remaining -= take;
        if (remaining <= 0.0) return spent / qty;
    }
    return std::nullopt;  // not enough liquidity
}

std::optional<Price> OrderBookUtils::avgSellPrice(const OrderBook& book, Quantity qty) {
    if (qty <= 0.0) return std::nullopt;
    double remaining = qty;
    double received = 0.0;
    for (const auto& lvl : book.bids) {
        if (lvl.quantity <= 0.0) continue;
        const double take = (remaining < lvl.quantity) ? remaining : lvl.quantity;
        received += take * lvl.price;
        remaining -= take;
        if (remaining <= 0.0) return received / qty;
    }
    return std::nullopt;
}

Quantity OrderBookUtils::availableQty(const std::vector<Level>& levels) {
    double total = 0.0;
    for (const auto& lvl : levels) {
        if (lvl.quantity > 0.0) total += lvl.quantity;
    }
    return total;
}

double OrderBookUtils::buySlippage(const OrderBook& book, Quantity qty) {
    const auto best = book.bestAsk();
    const auto vwap = avgBuyPrice(book, qty);
    if (!best || !vwap || *best <= 0.0) return 0.0;
    return (*vwap - *best) / *best;
}

double OrderBookUtils::sellSlippage(const OrderBook& book, Quantity qty) {
    const auto best = book.bestBid();
    const auto vwap = avgSellPrice(book, qty);
    if (!best || !vwap || *best <= 0.0) return 0.0;
    return (*best - *vwap) / *best;
}

bool OrderBookUtils::isFresh(const OrderBook& book, std::int64_t nowMs, std::int64_t maxAgeMs) {
    return (nowMs - book.localReceiveTs) <= maxAgeMs;
}

}  // namespace hftarb
