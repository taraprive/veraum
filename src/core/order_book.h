#pragma once

#include <optional>

#include "src/core/types.h"

namespace hftarb {

// Pure helpers over normalized order book data. No I/O, no clocks:
// everything is deterministic on the input levels so it is easily unit-tested.
class OrderBookUtils {
public:
    // Expected average price to *buy* `qty` walking the ask levels from the best.
    // Returns nullopt if total available ask quantity < qty.
    static std::optional<Price> avgBuyPrice(const OrderBook& book, Quantity qty);

    // Expected average price to *sell* `qty` walking the bid levels from the best.
    static std::optional<Price> avgSellPrice(const OrderBook& book, Quantity qty);

    // Total quantity available within `levels` (best-first).
    static Quantity availableQty(const std::vector<Level>& levels);

    // Slippage as a fraction for a buy leg: (vwap - bestAsk) / bestAsk.
    static double buySlippage(const OrderBook& book, Quantity qty);

    // Slippage as a fraction for a sell leg: (bestBid - vwap) / bestBid.
    static double sellSlippage(const OrderBook& book, Quantity qty);

    // Whether the book is fresh enough (age since localReceiveTs < maxAgeMs).
    static bool isFresh(const OrderBook& book, std::int64_t nowMs, std::int64_t maxAgeMs);
};

}  // namespace hftarb
