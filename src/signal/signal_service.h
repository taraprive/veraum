#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "src/core/types.h"

namespace hftarb {

// Configuration for the signal broadcaster.
struct SignalConfig {
    bool enabled = false;
    std::string channel = "telegram";  // "telegram" | "none"
    std::string token;                 // telegram bot token
    std::string chatId;                // telegram chat/group to notify
    double minNetSpread = 0.001;       // only signal opportunities above this (fraction)
    std::int64_t intervalMs = 15000;   // per-symbol throttle to avoid spam
    std::vector<std::string> symbols;  // empty = all symbols
    std::string jsonlPath;             // optional append-only log of emitted signals
};

// Emits human-readable opportunity signals in a background thread so delivery
// never blocks the trading path. Signal text describes an edge detected at
// decision time, before execution — it is an estimate, not a guaranteed profit.
class SignalService {
public:
    explicit SignalService(SignalConfig cfg);
    ~SignalService();

    SignalService(const SignalService&) = delete;
    SignalService& operator=(const SignalService&) = delete;

    // Called from the strategy callback; returns immediately.
    void emit(const Opportunity& opp);

    // Generic delivery for non-arbitrage signals (e.g. directional LONG/SHORT).
    // Throttled by `key` the same way symbols are.
    void emitRaw(const std::string& text, const std::string& key);

    void stop();

private:
    void run();
    static std::string format(const Opportunity& opp);
    bool shouldSend(const Opportunity& opp);
    void deliverTelegram(const std::string& text);
    void appendJsonl(const Opportunity& opp, const std::string& text);
    void appendRawJsonl(const std::string& text);

    SignalConfig cfg_;
    bool running_ = true;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::string> queue_;
    std::unordered_map<std::string, std::int64_t> lastSentMs_;
    std::thread worker_;
};

}  // namespace hftarb