#include "src/engine/multi_strategy_engine.h"

#include <utility>

namespace hftarb {

void MultiStrategyEngine::addStrategy(std::shared_ptr<IStrategy> strategy) {
    strategies_.push_back(std::move(strategy));
}

void MultiStrategyEngine::onTick(const Tick& tick) {
    for (const auto& s : strategies_) {
        if (s->hasSymbol(tick.symbol)) s->onTick(tick);
    }
}

void MultiStrategyEngine::setOpportunityCallback(IStrategy::OpportunityCallback cb) {
    for (const auto& s : strategies_) s->setOpportunityCallback(cb);
}

void MultiStrategyEngine::setConnectedFn(std::function<bool(const std::string&)> fn) {
    for (const auto& s : strategies_) s->setConnectedFn(fn);
}

void MultiStrategyEngine::setFeeSchedule(const std::string& exchange, const FeeSchedule& fees) {
    for (const auto& s : strategies_) s->setFeeSchedule(exchange, fees);
}

void MultiStrategyEngine::setPortfolio(std::shared_ptr<Portfolio> portfolio) {
    for (const auto& s : strategies_) s->setPortfolio(portfolio);
}

bool MultiStrategyEngine::hasSymbol(const std::string& symbol) const {
    for (const auto& s : strategies_) {
        if (s->hasSymbol(symbol)) return true;
    }
    return false;
}

double MultiStrategyEngine::exposure(const std::string& symbol) const {
    double total = 0.0;
    for (const auto& s : strategies_) total += s->exposure(symbol);
    return total;
}

void MultiStrategyEngine::addExposure(const std::string& symbol, double qty) {
    for (const auto& s : strategies_) {
        if (s->hasSymbol(symbol)) s->addExposure(symbol, qty);
    }
}

void MultiStrategyEngine::clearExposure(const std::string& symbol) {
    for (const auto& s : strategies_) {
        if (s->hasSymbol(symbol)) s->clearExposure(symbol);
    }
}

const IStrategy& MultiStrategyEngine::strategyAt(std::size_t i) const {
    return *strategies_.at(i);
}

}  // namespace hftarb