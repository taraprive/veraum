#include "src/monitor/watchdog.h"

namespace hftarb {

Watchdog::Watchdog(WatchdogConfig cfg) : cfg_(cfg) {}

void Watchdog::heartbeat(const std::string& exchange, std::int64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    lastHeartbeatMs_[exchange] = nowMs;
}

bool Watchdog::isAlive(const std::string& exchange, std::int64_t nowMs) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = lastHeartbeatMs_.find(exchange);
    if (it == lastHeartbeatMs_.end()) return false;
    return (nowMs - it->second) <= cfg_.heartbeatTimeoutMs;
}

std::string Watchdog::checkAndReconnect(const std::string& exchange, std::int64_t nowMs) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = lastHeartbeatMs_.find(exchange);
        if (it != lastHeartbeatMs_.end() &&
            (nowMs - it->second) <= cfg_.heartbeatTimeoutMs) {
            return {};
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto lastAttempt = lastReconnectAttemptMs_.find(exchange);
    if (lastAttempt != lastReconnectAttemptMs_.end() &&
        (nowMs - lastAttempt->second) < cfg_.checkIntervalMs) {
        return {};  // already attempting, don't spam
    }
    lastReconnectAttemptMs_[exchange] = nowMs;

    int& count = reconnectCount_[exchange];
    if (count >= cfg_.maxReconnectAttempts) return {};  // give up after N tries

    bool ok = false;
    if (reconnect_) ok = reconnect_(exchange);
    if (ok) {
        count = 0;
    } else {
        ++count;
    }
    return exchange;
}

}  // namespace hftarb
