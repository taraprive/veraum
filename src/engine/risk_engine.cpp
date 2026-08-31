#include "src/engine/risk_engine.h"

#include <algorithm>

namespace hftarb {

RiskEngine::RiskEngine(RiskConfig cfg) : cfg_(cfg) {}

Decision RiskEngine::evaluate(const Opportunity& opp, const RiskState& state) {
    lastReason_.clear();

    // 1. Feed alive on both legs.
    if (!state.isConnected(opp.buyExchange)) {
        lastReason_ = "buy_exchange_disconnected";
        return Decision::Disconnected;
    }
    if (!state.isConnected(opp.sellExchange)) {
        lastReason_ = "sell_exchange_disconnected";
        return Decision::Disconnected;
    }

    // 2. Data freshness: the freshest leg must not be older than maxDataAgeMs.
    if (state.nowMs - state.bestTickReceiveMs > cfg_.maxDataAgeMs) {
        lastReason_ = "stale_data";
        return Decision::StaleData;
    }

    // 3. End-to-end decision latency budget.
    if (opp.roundTripLatencyMs > cfg_.maxRoundTripLatencyMs) {
        lastReason_ = "latency_exceeds_budget";
        return Decision::LatencyRejected;
    }

    // 4. Trade size within limits.
    if (opp.quantity < cfg_.minTradeQty || opp.quantity > cfg_.maxTradeQty) {
        lastReason_ = "size_out_of_bounds";
        return Decision::SizeRejected;
    }

    // 5. No unbounded exposure from in-flight/partial paper trades.
    if (state.openExposure + opp.quantity > cfg_.maxOpenExposure) {
        lastReason_ = "exposure_cap_reached";
        return Decision::RiskRejected;
    }

    // 6. Per-symbol cooldown between trades.
    const auto it = lastTradeMs_.find(opp.symbol);
    if (it != lastTradeMs_.end() && (state.nowMs - it->second) < cfg_.cooldownMs) {
        lastReason_ = "cooldown_active";
        return Decision::Cooldown;
    }

    // 7. Rate limit across all symbols.
    const std::int64_t cutoff = state.nowMs - 60000;
    tradeWindow_.erase(std::remove_if(tradeWindow_.begin(), tradeWindow_.end(),
                                      [&](std::int64_t t) { return t < cutoff; }),
                       tradeWindow_.end());
    if (static_cast<int>(tradeWindow_.size()) >= cfg_.maxTradesPerMinute) {
        lastReason_ = "rate_limit";
        return Decision::RiskRejected;
    }

    lastReason_ = "ok";
    return Decision::Accepted;
}

void RiskEngine::recordTrade(const std::string& symbol, std::int64_t nowMs) {
    lastTradeMs_[symbol] = nowMs;
    tradeWindow_.push_back(nowMs);
}

}  // namespace hftarb
