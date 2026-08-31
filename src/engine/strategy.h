#pragma once

#include <functional>
#include <memory>
#include <string>

#include "src/core/types.h"
#include "src/engine/cost_engine.h"
#include "src/engine/portfolio.h"

namespace hftarb {

// Every arbitrage strategy routes through this common contract so the rest of
// the pipeline (feeds -> MultiStrategyEngine -> executor) is strategy-agnostic.
// A strategy consumes normalized ticks for the symbols it owns, applies its own
// gating (cost/liquidity/risk), and emits occurrences via the single shared
// opportunity callback. Each emitted Opportunity must carry the strategy name.
class IStrategy {
public:
    using OpportunityCallback = std::function<void(const Opportunity&)>;

    virtual ~IStrategy() = default;

    virtual const std::string& name() const = 0;
    virtual bool hasSymbol(const std::string& symbol) const = 0;

    // Feed entry point. Strategies must be thread-safe: feed threads call
    // onTick concurrently across symbols/exchanges.
    virtual void onTick(const Tick& tick) = 0;

    virtual void setOpportunityCallback(OpportunityCallback cb) = 0;
    virtual void setConnectedFn(std::function<bool(const std::string&)> fn) = 0;
    virtual void setFeeSchedule(const std::string& exchange, const FeeSchedule& fees) = 0;
    virtual void setPortfolio(std::shared_ptr<Portfolio> portfolio) = 0;

    // Residual unhedged exposure after round trips, tracked per symbol so the
    // multi-strategy agent can aggregate risk across strategies.
    virtual double exposure(const std::string& symbol) const = 0;
    virtual void addExposure(const std::string& symbol, double qty) = 0;
    virtual void clearExposure(const std::string& symbol) = 0;
};

}  // namespace hftarb