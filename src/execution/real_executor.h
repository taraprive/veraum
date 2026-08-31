#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "src/core/types.h"
#include "src/core/order_book.h"
#include "src/exchange/exchange_adapter_if.h"

namespace hftarb {

// Executes arbitrage by placing real orders on both exchanges sequentially.
// Uses market orders on OKX (for guaranteed fills) and GTC limits on Binance.
// Polls order status API for fill detection. Designed for testnet first.
class RealExecutor {
public:
    using AdapterMap = std::unordered_map<std::string, std::shared_ptr<IExchangeAdapter>>;

    explicit RealExecutor(AdapterMap adapters, int waitMs = 500, int cancelTtlMs = 2000);

    ExecutionResult execute(const Opportunity& opp);

    // Multi-leg route (triangular): sequentially places each leg as a limit
    // order on its venue (buy = ask price, sell = bid less one tick), waits
    // for each fill, and cancels any leg that stalls. Handles the partial
    // cycle when a later leg fails: the stuck inventory is reported as
    // exposure instead of a fake fill.
    ExecutionResult executeLegs(const Opportunity& opp);

    void updateBook(const std::string& exchange, const std::string& symbol,
                    const OrderBook& book);

    double bestBid(const std::string& exchange, const std::string& symbol) const;
    double bestAsk(const std::string& exchange, const std::string& symbol) const;

private:
    static std::string normalizeSide(const std::string& engineSide);

    AdapterMap adapters_;
    int waitMs_;
    int cancelTtlMs_;
    std::unordered_map<std::string, OrderBook> books_;
    mutable std::mutex bookMutex_;
    std::mutex execMutex_;
};

}  // namespace hftarb
