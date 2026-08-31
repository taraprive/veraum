#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "src/core/timestamp.h"
#include "src/exchange/exchange_types.h"
#include "src/market/adapter.h"

namespace hftarb {

// Polls depth snapshots over REST (no WebSocket library needed) and emits
// normalized Ticks into the same pipeline the mock feeds use. One thread per
// adapter, round-robining the configured symbols.
class RestFeedAdapter final : public IMarketAdapter {
public:
    using DepthFetcher = std::function<std::optional<DepthBook>(const std::string&, int)>;

    RestFeedAdapter(std::string name, std::vector<std::string> symbols,
                    DepthFetcher fetcher, std::int64_t pollMs);

    const std::string& name() const override { return name_; }
    const std::vector<std::string>& symbols() const override { return symbols_; }
    void setTickCallback(TickCallback cb) override { cb_ = std::move(cb); }
    bool start() override;
    void stop() override;
    bool isConnected() const override { return connected_.load(); }
    void heartbeat() override { lastHeartbeat_.store(Timestamps::monoMs()); }

private:
    void run();
    void pollSymbol(const std::string& symbol);

    std::string name_;
    std::vector<std::string> symbols_;
    DepthFetcher fetcher_;
    std::int64_t pollMs_;

    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::int64_t> lastHeartbeat_{0};
    std::thread thread_;
    TickCallback cb_;
    mutable std::mutex cbMutex_;
};

using RestFeedAdapterPtr = std::shared_ptr<RestFeedAdapter>;

}  // namespace hftarb
