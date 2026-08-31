#include "src/market/ws_feed_adapter.h"

namespace hftarb {

WsFeedAdapter::WsFeedAdapter(std::string name, std::string wsUrl, std::string subMessage,
                             Parser parser)
    : name_(std::move(name)),
      symbols_(),
      parser_(std::move(parser)),
      client_(std::make_unique<WsClient>(std::move(wsUrl), std::move(subMessage))) {}

bool WsFeedAdapter::start() {
    client_->setMessageCallback(
        [this](const std::string& payload) { onMessage(payload); });
    client_->setStateCallback([this](bool up) { onState(up); });
    return client_->connect();
}

void WsFeedAdapter::stop() {
    client_->disconnect();
    connected_.store(false);
}

void WsFeedAdapter::onState(bool up) {
    if (up) {
        lastHeartbeat_.store(Timestamps::monoMs());
    }
    connected_.store(up);
}

void WsFeedAdapter::onMessage(const std::string& payload) {
    lastHeartbeat_.store(Timestamps::monoMs());

    std::optional<DepthBook> book = parser_(payload);
    if (!book) return;

    connected_.store(true);
    const std::int64_t nowMono = Timestamps::monoMs();
    Tick tick;
    tick.exchange = name_;
    tick.symbol = book->symbol;
    tick.exchangeTs = Timestamps::wallMs();
    tick.receiveTs = nowMono;
    tick.parseTs = nowMono;
    tick.book.exchange = name_;
    tick.book.symbol = book->symbol;
    tick.book.exchangeTs = tick.exchangeTs;
    tick.book.localReceiveTs = nowMono;
    for (const auto& lvl : book->bids) tick.book.bids.push_back({lvl.first, lvl.second});
    for (const auto& lvl : book->asks) tick.book.asks.push_back({lvl.first, lvl.second});

    std::lock_guard<std::mutex> lock(cbMutex_);
    if (cb_) cb_(tick);
}

}  // namespace hftarb