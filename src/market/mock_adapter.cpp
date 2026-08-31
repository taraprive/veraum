#include "src/market/mock_adapter.h"

#include <chrono>

#include "src/core/timestamp.h"

namespace hftarb {

MockFeedAdapter::MockFeedAdapter(std::string name, std::vector<std::string> symbols,
                                 std::shared_ptr<MarketState> market, std::int64_t updateMs)
    : name_(std::move(name)),
      symbols_(std::move(symbols)),
      market_(std::move(market)),
      updateMs_(updateMs) {}

MockFeedAdapter::~MockFeedAdapter() { stop(); }

bool MockFeedAdapter::start() {
    if (!running_.exchange(true)) {
        thread_ = std::thread(&MockFeedAdapter::run, this);
        connected_.store(true);
        return true;
    }
    return false;
}

void MockFeedAdapter::stop() {
    if (running_.exchange(false)) {
        connected_.store(false);
        if (thread_.joinable()) thread_.join();
    }
}

OrderBook MockFeedAdapter::makeBook(const std::string& symbol) {
    // The shared market simulates time between two ticks; feeding the local
    // clock as "now" keeps the whole cross-exchange market in lock-step.
    const std::int64_t now = Timestamps::monoMs();
    return market_->tick(name_, symbol, now, updateMs_);
}

void MockFeedAdapter::run() {
    while (running_.load()) {
        for (const auto& sym : symbols_) {
            if (!running_.load()) break;
            Tick tick;
            tick.exchange = name_;
            tick.symbol = sym;
            tick.book = makeBook(sym);
            tick.exchangeTs = tick.book.exchangeTs;
            tick.receiveTs = Timestamps::monoMs();
            tick.parseTs = Timestamps::monoMs();
            {
                std::lock_guard<std::mutex> lock(cbMutex_);
                if (cb_) cb_(tick);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(updateMs_));
        }
    }
}

}  // namespace hftarb
