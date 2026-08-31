#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "src/core/types.h"
#include "src/engine/cost_engine.h"
#include "src/engine/liquidity_engine.h"
#include "src/engine/portfolio.h"
#include "src/engine/risk_engine.h"
#include "src/engine/strategy.h"

namespace hftarb {

// Fully evaluated best candidate for one symbol (pre-decision).
struct Candidate {
    std::string symbol;
    std::string buyExchange;
    std::string sellExchange;
    Quantity quantity = 0.0;
    CostBreakdown cost;
    double roundTripLatencyMs = 0.0;
    std::int64_t bestReceiveMs = 0;
};

struct ArbitrageConfig {
    std::string symbol;
    std::vector<std::string> exchanges;
    Quantity tradeQuantity = 0.1;
};

// Scans every ordered exchange pair per symbol, applies liquidity + cost
// gating, then picks the best net edge the accounts can actually fund
// (portfolio-aware fallback). The winner routes through the risk engine and
// the outcome (accepted or rejected) is emitted for logging / paper execution.
// Implements IStrategy as the "cross_exchange" strategy.
class ArbitrageEngine final : public IStrategy {
public:
    ArbitrageEngine(ArbitrageConfig cfg, CostEngine cost, LiquidityEngine liquidity,
                    RiskEngine risk);

    const std::string& name() const override { return name_; }

    // Feed this with normalized ticks from any adapter. Updates internal state
    // and triggers a re-evaluation of the affected symbol.
    void onTick(const Tick& tick) override;

    void setOpportunityCallback(OpportunityCallback cb) override { emit_ = std::move(cb); }
    void setConnectedFn(std::function<bool(const std::string&)> fn) override {
        connected_ = std::move(fn);
    }

    // Per-exchange fee schedules used by the cost engine.
    void setFeeSchedule(const std::string& exchange, const FeeSchedule& fees) override {
        std::lock_guard<std::mutex> lock(mutex_);
        feeSchedules_[exchange] = fees;
    }

    // Optional balance model. When set, the engine walks candidates from the
    // best net edge down and picks the best one the accounts can afford on
    // both legs; unaffordable pairs are skipped rather than rejected outright.
    void setPortfolio(std::shared_ptr<Portfolio> portfolio) override {
        std::lock_guard<std::mutex> lock(mutex_);
        portfolio_ = std::move(portfolio);
    }

    double exposure(const std::string& symbol) const override {
        (void)symbol;
        return exposure_.load();
    }
    void addExposure(const std::string& symbol, double qty) override {
        (void)symbol;
        addExposure(qty);
    }
    void clearExposure(const std::string& symbol) override {
        (void)symbol;
        clearExposure();
    }

    double exposure() const { return exposure_.load(); }
    void addExposure(double qty) {
        double cur = exposure_.load();
        while (!exposure_.compare_exchange_weak(cur, cur + qty)) {
        }
    }
    void clearExposure() { exposure_.store(0.0); }

    // Latest cached book for (exchange, symbol); nullopt if not seen yet.
    std::optional<OrderBook> book(const std::string& exchange, const std::string& symbol) const;

    std::size_t bookCount() const;
    bool hasSymbol(const std::string& symbol) const override { return cfg_.symbol == symbol; }
    const std::string& riskLastReason() const { return risk_.lastReason(); }

private:
    void evaluateSymbol(const std::string& symbol, std::int64_t nowMs);

    const FeeSchedule& feeScheduleOf(const std::string& exchange) const {
        const auto it = feeSchedules_.find(exchange);
        if (it == feeSchedules_.end()) return defaultFees_;
        return it->second;
    }

    ArbitrageConfig cfg_;
    CostEngine cost_;
    LiquidityEngine liquidity_;
    RiskEngine risk_;
    FeeSchedule defaultFees_;
    const std::string name_{"cross_exchange"};

    mutable std::mutex mutex_;
    std::unordered_map<std::string, OrderBook> books_;  // key: exchange + "|" + symbol
    std::unordered_map<std::string, FeeSchedule> feeSchedules_;
    std::shared_ptr<Portfolio> portfolio_;
    std::mutex evalMutex_;  // serializes symbol evaluations across feed threads

    std::atomic<double> exposure_{0.0};
    OpportunityCallback emit_;
    std::function<bool(const std::string&)> connected_;
};

}  // namespace hftarb
