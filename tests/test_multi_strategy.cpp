#include "test_framework.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "src/core/timestamp.h"
#include "src/engine/arbitrage_engine.h"
#include "src/engine/cost_engine.h"
#include "src/engine/liquidity_engine.h"
#include "src/engine/multi_strategy_engine.h"
#include "src/engine/portfolio.h"
#include "src/engine/risk_engine.h"
#include "src/engine/strategy.h"

using namespace hftarb;

namespace {

// Trivial strategy that counts ticks for its one owned symbol.
class CountingStrategy : public IStrategy {
public:
    CountingStrategy(std::string name, std::string symbol)
        : name_(std::move(name)), symbol_(std::move(symbol)) {}

    const std::string& name() const override { return name_; }
    bool hasSymbol(const std::string& s) const override { return s == symbol_; }
    void onTick(const Tick&) override { ++ticks; }

    void setOpportunityCallback(OpportunityCallback) override {}
    void setConnectedFn(std::function<bool(const std::string&)>) override {}
    void setFeeSchedule(const std::string&, const FeeSchedule&) override {}
    void setPortfolio(std::shared_ptr<Portfolio>) override {}
    double exposure(const std::string&) const override { return 0.0; }
    void addExposure(const std::string&, double) override {}
    void clearExposure(const std::string&) override {}

    std::string name_;
    std::string symbol_;
    int ticks = 0;
};

// Strategy that records per-symbol exposure and emits one accepted
// Opportunity tagged with its own name on every tick.
class EmittingStrategy : public IStrategy {
public:
    EmittingStrategy(std::string name, std::string symbol)
        : name_(std::move(name)), symbol_(std::move(symbol)) {}

    const std::string& name() const override { return name_; }
    bool hasSymbol(const std::string& s) const override { return s == symbol_; }

    void onTick(const Tick& tick) override {
        ++ticks;
        if (!cb_) return;
        Opportunity opp;
        opp.strategy = name_;
        opp.symbol = tick.symbol;
        opp.decision = Decision::Accepted;
        cb_(opp);
    }

    void setOpportunityCallback(OpportunityCallback cb) override { cb_ = std::move(cb); }
    void setConnectedFn(std::function<bool(const std::string&)>) override {}
    void setFeeSchedule(const std::string&, const FeeSchedule&) override {}
    void setPortfolio(std::shared_ptr<Portfolio>) override {}

    double exposure(const std::string& s) const override {
        const auto it = expo_.find(s);
        return it == expo_.end() ? 0.0 : it->second;
    }
    void addExposure(const std::string& s, double qty) override { expo_[s] += qty; }
    void clearExposure(const std::string& s) override { expo_[s] = 0.0; }

    std::string name_;
    std::string symbol_;
    int ticks = 0;
    std::unordered_map<std::string, double> expo_;
    OpportunityCallback cb_;
};

Tick makeTick(const std::string& ex, const std::string& symbol) {
    Tick t;
    t.exchange = ex;
    t.symbol = symbol;
    t.receiveTs = Timestamps::monoMs();
    t.book.exchange = ex;
    t.book.symbol = symbol;
    t.book.localReceiveTs = t.receiveTs;
    return t;
}

// Real cross_exchange strategy feeding a two-venue edge; mirrors
// tests/test_arbitrage.cpp harness so the accepted path is meaningful.
struct RealHarness {
    std::shared_ptr<MultiStrategyEngine> multi;
    std::shared_ptr<Portfolio> portfolio;
    std::vector<Opportunity> emitted;

    RealHarness() : multi(std::make_shared<MultiStrategyEngine>()) {
        auto strategy = std::make_shared<ArbitrageEngine>(
            ArbitrageConfig{"BTC/USDT", {"A", "B"}, 0.1},
            CostEngine(CostConfig{0.0005, 0.002, 0.0}),
            LiquidityEngine(LiquidityConfig{1.0, 0.001}),
            RiskEngine(RiskConfig{500.0, 100.0, 0.001, 2.0, 0.5, 0.0, 1000}));
        strategy->setConnectedFn([](const std::string&) { return true; });
        portfolio = std::make_shared<Portfolio>();
        portfolio->setQuote("A", 100000.0);
        portfolio->setQuote("B", 100000.0);
        portfolio->setBase("B", "BTC/USDT", 10.0);  // only B can deliver BTC
        multi->addStrategy(std::move(strategy));
        multi->setPortfolio(portfolio);
        multi->setOpportunityCallback([this](const Opportunity& o) { emitted.push_back(o); });
    }

    void feed(const std::string& ex, double ask, double bid) {
        Tick t = makeTick(ex, "BTC/USDT");
        t.book.asks = {{ask, 1.0}, {ask + 1.0, 1.0}};
        t.book.bids = {{bid, 1.0}, {bid - 1.0, 1.0}};
        multi->onTick(t);
    }
};

}  // namespace

TEST(multi_strategy_fans_out_to_interested_only) {
    auto a = std::make_shared<CountingStrategy>("S_A", "BTC/USDT");
    auto b = std::make_shared<CountingStrategy>("S_B", "ETH/USDT");
    MultiStrategyEngine multi;
    multi.addStrategy(a);
    multi.addStrategy(b);

    multi.onTick(makeTick("X", "BTC/USDT"));
    CHECK(a->ticks == 1);
    CHECK(b->ticks == 0);  // not interested in BTC/USDT

    multi.onTick(makeTick("X", "ETH/USDT"));
    CHECK(a->ticks == 1);
    CHECK(b->ticks == 1);
    CHECK(multi.strategyCount() == 2);
}

TEST(multi_strategy_shares_one_opportunity_callback) {
    auto e1 = std::make_shared<EmittingStrategy>("cross_exchange", "BTC/USDT");
    auto e2 = std::make_shared<EmittingStrategy>("funding_rate", "ETH/USDT");
    MultiStrategyEngine multi;
    multi.addStrategy(e1);
    multi.addStrategy(e2);

    std::vector<Opportunity> emitted;
    multi.setOpportunityCallback([&](const Opportunity& o) { emitted.push_back(o); });

    multi.onTick(makeTick("A", "BTC/USDT"));
    multi.onTick(makeTick("A", "ETH/USDT"));

    CHECK(emitted.size() == 2);
    if (emitted.size() != 2) return;
    CHECK(emitted[0].strategy == "cross_exchange");
    CHECK(emitted[1].strategy == "funding_rate");
}

TEST(multi_strategy_aggregates_exposure_per_symbol) {
    auto e1 = std::make_shared<EmittingStrategy>("S1", "BTC/USDT");
    auto e2 = std::make_shared<EmittingStrategy>("S2", "BTC/USDT");
    MultiStrategyEngine multi;
    multi.addStrategy(e1);
    multi.addStrategy(e2);

    multi.addExposure("BTC/USDT", 0.25);
    CHECK_NEAR(multi.exposure("BTC/USDT"), 0.50, 1e-12);  // summed across strategies
    CHECK_NEAR(multi.exposure("ETH/USDT"), 0.0, 1e-12);  // untouched symbol

    multi.clearExposure("BTC/USDT");
    CHECK_NEAR(multi.exposure("BTC/USDT"), 0.0, 1e-12);
}

TEST(multi_strategy_has_symbol_any_owner) {
    MultiStrategyEngine multi;
    multi.addStrategy(std::make_shared<CountingStrategy>("S1", "BTC/USDT"));
    multi.addStrategy(std::make_shared<CountingStrategy>("S2", "DOGE/USDT"));

    CHECK(multi.hasSymbol("BTC/USDT"));
    CHECK(multi.hasSymbol("DOGE/USDT"));
    CHECK(!multi.hasSymbol("ETH/USDT"));
}

TEST(multi_strategy_cross_exchange_attribution_and_exposure) {
    RealHarness h;
    h.feed("A", 100.0, 99.0);
    h.feed("B", 101.0, 100.6);  // buy @100 on A, sell @100.6 on B

    CHECK(h.emitted.size() == 1);
    if (h.emitted.empty()) return;
    const auto& o = h.emitted[0];
    CHECK(o.strategy == "cross_exchange");
    CHECK(o.decision == Decision::Accepted);
    // The real engine credited its own exposure after the accepted emission.
    CHECK_NEAR(h.multi->exposure("BTC/USDT"), o.quantity, 1e-12);
}