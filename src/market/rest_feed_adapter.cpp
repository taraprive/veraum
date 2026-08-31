#include "src/market/rest_feed_adapter.h"

#include <chrono>
#include <cctype>

#include "src/core/timestamp.h"

namespace hftarb {

RestFeedAdapter::RestFeedAdapter(std::string name, std::vector<std::string> symbols,
                                 DepthFetcher fetcher, std::int64_t pollMs)
    : name_(std::move(name)), symbols_(std::move(symbols)), fetcher_(std::move(fetcher)),
      pollMs_(pollMs > 0 ? pollMs : 50) {}

bool RestFeedAdapter::start() {
    if (running_.load()) return connected_.load();
    running_.store(true);
    if (!symbols_.empty()) pollSymbol(symbols_[0]);
    thread_ = std::thread(&RestFeedAdapter::run, this);
    return true;
}

void RestFeedAdapter::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

void RestFeedAdapter::run() {
    size_t idx = 0;
    while (running_.load()) {
        if (symbols_.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(pollMs_));
            continue;
        }
        pollSymbol(symbols_[idx % symbols_.size()]);
        idx = (idx + 1) % symbols_.size();
        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs_));
    }
}

void RestFeedAdapter::pollSymbol(const std::string& symbol) {
    std::string exSymbol;
    exSymbol.reserve(symbol.size());
    for (char c : symbol) {
        if (c != '/') exSymbol.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }

    const auto depth = fetcher_(exSymbol, 10);
    if (!depth) {
        connected_.store(false);
        return;
    }
    connected_.store(true);
    lastHeartbeat_.store(Timestamps::monoMs());

    const std::int64_t nowMono = Timestamps::monoMs();
    Tick tick;
    tick.exchange = name_;
    tick.symbol = symbol;
    tick.exchangeTs = Timestamps::wallMs();
    tick.receiveTs = nowMono;
    tick.parseTs = nowMono;
    tick.book.exchange = name_;
    tick.book.symbol = symbol;
    tick.book.exchangeTs = tick.exchangeTs;
    tick.book.localReceiveTs = nowMono;
    for (const auto& lvl : depth->bids) tick.book.bids.push_back({lvl.first, lvl.second});
    for (const auto& lvl : depth->asks) tick.book.asks.push_back({lvl.first, lvl.second});

    std::lock_guard<std::mutex> lock(cbMutex_);
    if (cb_) cb_(tick);
}

}  // namespace hftarb
