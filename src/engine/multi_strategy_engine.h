#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "src/core/types.h"
#include "src/engine/portfolio.h"
#include "src/engine/strategy.h"

namespace hftarb {

// Aggregates every IStrategy behind one tick entry point and one opportunity
// callback, so the feed wiring and executor stay strategy-agnostic. Feeds fan
// ticks into onTick(); the dispatcher routes each tick only to strategies that
// own the symbol. Cross-strategy exposure is summed per symbol.
//
// The strategy list is built once before any feed thread starts and is then
// treated as immutable, so onTick can read it lock-free from feed threads.
class MultiStrategyEngine {
public:
    // Must be called before feeds start producing ticks.
    void addStrategy(std::shared_ptr<IStrategy> strategy);

    // Routes a tick to every strategy that owns the symbol.
    void onTick(const Tick& tick);

    // Shared plumbing forwarded to every strategy.
    void setOpportunityCallback(IStrategy::OpportunityCallback cb);
    void setConnectedFn(std::function<bool(const std::string&)> fn);
    void setFeeSchedule(const std::string& exchange, const FeeSchedule& fees);
    void setPortfolio(std::shared_ptr<Portfolio> portfolio);

    // True if at least one strategy owns the symbol.
    bool hasSymbol(const std::string& symbol) const;

    // Aggregate residual exposure across strategies per symbol.
    double exposure(const std::string& symbol) const;
    void addExposure(const std::string& symbol, double qty);
    void clearExposure(const std::string& symbol);

    std::size_t strategyCount() const { return strategies_.size(); }
    // Read-only access for diagnostics/tests; valid for the owning engine.
    const IStrategy& strategyAt(std::size_t i) const;

private:
    std::vector<std::shared_ptr<IStrategy>> strategies_;
};

}  // namespace hftarb