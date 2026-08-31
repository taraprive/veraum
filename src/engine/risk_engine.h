#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "src/core/types.h"

namespace hftarb {

struct RiskConfig {
    double maxDataAgeMs = 500.0;       // oldest book age we will act on
    double maxRoundTripLatencyMs = 100.0;  // tick->decision budget
    double minTradeQty = 0.001;
    double maxTradeQty = 2.0;
    double maxOpenExposure = 0.5;      // max unfilled net exposure (in qty)
    double cooldownMs = 250.0;         // min gap between two trades per symbol
    int maxTradesPerMinute = 20;
    double maxSessionDrawdownPct = 0.0;  // hard stop: 0 = disabled
};

// Snapshot of conditions the risk engine needs at decision time.
struct RiskState {
    // returns true when the named exchange feed is alive
    const std::function<bool(const std::string&)>& isConnected;
    double openExposure = 0.0;      // net qty currently unhedged
    std::int64_t nowMs = 0;         // decision time (monotonic ms)
    std::int64_t bestTickReceiveMs = 0;  // receive time of the freshest leg
};

// Gate that every candidate opportunity must pass before paper execution.
// One rejection reason wins; the reason is recorded for analytics.
class RiskEngine {
public:
    explicit RiskEngine(RiskConfig cfg);

    // Evaluates the opportunity against all risk rules. Updates internal
    // cooldown/rate-limit bookkeeping ONLY on acceptance.
    Decision evaluate(const Opportunity& opp, const RiskState& state);

    void recordTrade(const std::string& symbol, std::int64_t nowMs);

    const std::string& lastReason() const { return lastReason_; }

private:
    RiskConfig cfg_;
    std::string lastReason_;
    std::unordered_map<std::string, std::int64_t> lastTradeMs_;
    std::vector<std::int64_t> tradeWindow_;
};

}  // namespace hftarb
