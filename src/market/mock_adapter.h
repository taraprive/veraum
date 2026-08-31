#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "src/core/timestamp.h"
#include "src/market/adapter.h"
#include "src/market/market_state.h"

namespace hftarb {

// Offline feed that streams normalized ticks drawn from a shared MarketState.
// All feeds read the same underlying price process (plus per-exchange basis
// and occasional inefficiency bursts), which gives the arbitrage engine a
// realistic, self-contained market to trade — no network, no credentials.
class MockFeedAdapter final : public IMarketAdapter {
public:
    MockFeedAdapter(std::string name, std::vector<std::string> symbols,
                    std::shared_ptr<MarketState> market, std::int64_t updateMs);
    ~MockFeedAdapter() override;

    const std::string& name() const override { return name_; }
    const std::vector<std::string>& symbols() const override { return symbols_; }
    void setTickCallback(TickCallback cb) override { cb_ = std::move(cb); }
    bool start() override;
    void stop() override;
    bool isConnected() const override { return connected_.load(); }
    void heartbeat() override { lastHeartbeat_.store(Timestamps::monoMs()); }

    std::int64_t lastHeartbeatMs() const { return lastHeartbeat_.load(); }

private:
    void run();
    OrderBook makeBook(const std::string& symbol);

    std::string name_;
    std::vector<std::string> symbols_;
    std::shared_ptr<MarketState> market_;
    std::int64_t updateMs_;

    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::int64_t> lastHeartbeat_{0};
    std::thread thread_;
    TickCallback cb_;
    mutable std::mutex cbMutex_;
};

using MockFeedAdapterPtr = std::shared_ptr<MockFeedAdapter>;

}  // namespace hftarb
