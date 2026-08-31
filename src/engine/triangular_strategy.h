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
#include "src/engine/portfolio.h"
#include "src/engine/risk_engine.h"
#include "src/engine/strategy.h"
#include "src/util/config.h"

namespace hftarb {

// Same-venue triangular arbitrage: a 3-leg cycle that converts the start asset
// through two intermediate crosses and lands back in the start asset, priced
// entirely off one exchange's books. Implements IStrategy as "triangular".
//
//   buy  BTC/USDT  (pay USDT -> BTC)
//   buy  ETH/BTC   (pay BTC  -> ETH)
//   sell ETH/USDT  (pay ETH  -> USDT)
//
// The round-trip rate is the compounded product of the three leg conversions;
// fees are modeled multiplicatively per leg (spot taker fees), slippage is
// informational (VWAP walk already reflects it). An accepted opportunity
// carries its three legs so the executor replays the route verbatim.
class TriangularStrategy final : public IStrategy {
public:
    TriangularStrategy(AppConfig::TriangleConfig cfg, CostEngine cost, RiskEngine risk);

    const std::string& name() const override { return name_; }

    // True when this triangle uses the given symbol on any of its legs.
    bool hasSymbol(const std::string& symbol) const override;

    // Normalized ticks from any adapter for the triangle's own leg symbols.
    void onTick(const Tick& tick) override;

    void setOpportunityCallback(OpportunityCallback cb) override { emit_ = std::move(cb); }
    void setConnectedFn(std::function<bool(const std::string&)> fn) override {
        connected_ = std::move(fn);
    }
    void setFeeSchedule(const std::string& exchange, const FeeSchedule& fees) override {
        (void)exchange;
        std::lock_guard<std::mutex> lock(mutex_);
        fees_ = fees;
    }
    void setPortfolio(std::shared_ptr<Portfolio> portfolio) override {
        std::lock_guard<std::mutex> lock(mutex_);
        portfolio_ = std::move(portfolio);
    }

    // Residual exposure after a triangle: only nonzero when the executor
    // reported an unhedged leftover (a leg that failed to fill).
    double exposure(const std::string& symbol) const override {
        (void)symbol;
        return exposure_.load();
    }
    void addExposure(const std::string& symbol, double qty) override;
    void clearExposure(const std::string& symbol) override;

    // Latest cached book for one leg symbol; nullopt until first tick.
    std::optional<OrderBook> book(const std::string& symbol) const;
    std::size_t bookCount() const;

    // Most recently computed round-trip rate (for diagnostics/tests).
    double lastRate() const { return lastRate_.load(); }

private:
    void addExposure(double qty);

    void evaluate(std::int64_t nowMs);

    // Prices/quantities for the three legs and the round-trip rate. Returns
    // nullopt when the cycle is not executable (voids `why`/`reason` for the
    // diagnostic opportunity). `cost` and `legs` are filled on success.
    std::optional<double> computeCycle(
        const OrderBook& b0, const OrderBook& b1, const OrderBook& b2,
        const FeeSchedule& fees, std::vector<OrderLeg>& legs,
        CostBreakdown& cost) const;

    AppConfig::TriangleConfig cfg_;
    CostEngine cost_;
    RiskEngine risk_;
    FeeSchedule fees_;
    const std::string name_{"triangular"};

    mutable std::mutex mutex_;
    std::unordered_map<std::string, OrderBook> books_;  // key: leg symbol
    std::shared_ptr<Portfolio> portfolio_;
    std::mutex evalMutex_;  // serializes evaluations across leg tick races

    std::atomic<double> exposure_{0.0};
    std::atomic<double> lastRate_{0.0};
    OpportunityCallback emit_;
    std::function<bool(const std::string&)> connected_;
};

}  // namespace hftarb