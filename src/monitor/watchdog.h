#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace hftarb {

struct WatchdogConfig {
    std::int64_t heartbeatTimeoutMs = 5000;  // considered dead after silence
    std::int64_t checkIntervalMs = 1000;
    int maxReconnectAttempts = 5;
};

// Connection supervision: tracks last heartbeat per exchange, decides whether
// a feed is alive, and owns the reconnect decision + resubscribe + re-sync
// sequence contract. The scaffold's mock feeds keep themselves alive, so the
// watchdog mostly reports health for the risk engine.
class Watchdog {
public:
    using ReconnectHandler = std::function<bool(const std::string& exchange)>;

    explicit Watchdog(WatchdogConfig cfg);

    void setReconnectHandler(ReconnectHandler h) { reconnect_ = std::move(h); }

    void heartbeat(const std::string& exchange, std::int64_t nowMs);
    bool isAlive(const std::string& exchange, std::int64_t nowMs) const;

    // Returns the exchange name when a reconnect was triggered, else empty.
    std::string checkAndReconnect(const std::string& exchange, std::int64_t nowMs);

    int reconnectCount(const std::string& exchange) const { return reconnectCount_.count(exchange) ? reconnectCount_.at(exchange) : 0; }

private:
    WatchdogConfig cfg_;
    std::function<bool(const std::string&)> reconnect_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::int64_t> lastHeartbeatMs_;
    std::unordered_map<std::string, int> reconnectCount_;
    std::unordered_map<std::string, std::int64_t> lastReconnectAttemptMs_;
};

}  // namespace hftarb
