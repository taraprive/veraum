#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "src/core/timestamp.h"
#include "src/exchange/exchange_types.h"
#include "src/market/adapter.h"
#include "src/net/ws_client.h"

namespace hftarb {

// Streaming feed over a single WebSocket. The caller provides:
//  - wsUrl + subMessage (built from the exchange's stream protocol)
//  - a parser that turns each raw payload into a normalized DepthBook
// Reconnection and re-subscription are handled internally (WsClient), so the
// feed stays healthy through transient network blips.
class WsFeedAdapter final : public IMarketAdapter {
public:
    using Parser = std::function<std::optional<DepthBook>(const std::string& payload)>;

    WsFeedAdapter(std::string name, std::string wsUrl, std::string subMessage, Parser parser);

    const std::string& name() const override { return name_; }
    const std::vector<std::string>& symbols() const override { return symbols_; }
    void setTickCallback(TickCallback cb) override { cb_ = std::move(cb); }
    bool start() override;
    void stop() override;
    bool isConnected() const override { return connected_.load(); }
    void heartbeat() override { lastHeartbeat_.store(Timestamps::monoMs()); }

private:
    void onMessage(const std::string& payload);
    void onState(bool up);

    std::string name_;
    std::vector<std::string> symbols_;
    Parser parser_;
    std::unique_ptr<WsClient> client_;

    std::atomic<bool> connected_{false};
    std::atomic<std::int64_t> lastHeartbeat_{0};
    TickCallback cb_;
    mutable std::mutex cbMutex_;
    mutable std::mutex stateMutex_;
};

using WsFeedAdapterPtr = std::shared_ptr<WsFeedAdapter>;

}  // namespace hftarb