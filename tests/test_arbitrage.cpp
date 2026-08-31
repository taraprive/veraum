#include "test_framework.h"

#include <tuple>
#include <vector>

#include "src/core/timestamp.h"
#include "src/engine/arbitrage_engine.h"
#include "src/engine/cost_engine.h"
#include "src/engine/liquidity_engine.h"
#include "src/engine/portfolio.h"
#include "src/engine/risk_engine.h"

using namespace hftarb;

namespace {

// Builds an engine wired to collect every emitted opportunity.
struct Harness {
    ArbitrageEngine engine;
    std::vector<Opportunity> emitted;

    Harness(double tradeQty = 0.1)
        : engine(ArbitrageConfig{"BTC/USDT", {"A", "B"}, tradeQty},
                 CostEngine(CostConfig{0.0005, 0.002, 0.0}),
                 LiquidityEngine(LiquidityConfig{1.0, 0.001}),
                 RiskEngine(RiskConfig{500.0, 100.0, 0.001, 2.0, 0.5, 0.0, 1000})) {
        engine.setConnectedFn([](const std::string&) { return true; });
        engine.setOpportunityCallback([this](const Opportunity& o) { emitted.push_back(o); });
    }

    void feed(const std::string& ex, double ask, double bid) {
        const std::int64_t now = Timestamps::monoMs();
        Tick t;
        t.exchange = ex;
        t.symbol = "BTC/USDT";
        t.book.exchange = ex;
        t.book.symbol = "BTC/USDT";
        t.book.asks = {{ask, 1.0}, {ask + 1.0, 1.0}};
        t.book.bids = {{bid, 1.0}, {bid - 1.0, 1.0}};
        t.receiveTs = now;
        t.book.localReceiveTs = now;
        engine.onTick(t);
    }
};

}  // namespace

TEST(arbitrage_detects_and_accepts_edge) {
    Harness h;
    h.feed("A", 100.0, 99.0);
    h.feed("B", 101.0, 100.6);  // buy @100 on A, sell @100.6 on B

    CHECK(h.emitted.size() == 1);
    if (h.emitted.empty()) return;
    const auto& o = h.emitted[0];
    CHECK(o.buyExchange == "A");
    CHECK(o.sellExchange == "B");
    CHECK(o.decision == Decision::Accepted);
    CHECK(o.cost.netSpread > 0.0);
    CHECK(o.cost.grossSpread > o.cost.netSpread);  // costs reduced the edge
}

TEST(arbitrage_no_edge_emits_nothing) {
    Harness h;
    h.feed("A", 100.0, 99.0);
    h.feed("B", 100.0, 99.0);  // identical books, no cross spread
    CHECK(h.emitted.empty());
}

TEST(arbitrage_below_threshold_emits_nothing) {
    Harness h;
    // Edge exists but too small: buy @100, sell @100.08 (~8bp gross) < 20bp.
    h.feed("A", 100.0, 99.0);
    h.feed("B", 100.1, 100.08);
    CHECK(h.emitted.empty());
}

TEST(arbitrage_liquidity_gate_blocks_candidate) {
    Harness h(/*tradeQty=*/5.0);  // 5.0 requested but only ~1.0 visible
    h.feed("A", 100.0, 99.0);
    h.feed("B", 101.0, 100.6);
    // With quantity ladder, engine tries smaller sizes (5->2.5->1.25->0.625)
    // and 0.625 passes liquidity, so a trade IS emitted at reduced size.
    CHECK(h.emitted.size() == 1);
    if (!h.emitted.empty()) {
        CHECK(h.emitted[0].quantity < 5.0);  // scaled down
    }
}

TEST(arbitrage_reverse_direction_detected) {
    Harness h;
    // B is the cheap side now: buy on B, sell on A.
    h.feed("B", 100.0, 99.0);
    h.feed("A", 101.0, 100.6);
    CHECK(h.emitted.size() == 1);
    if (h.emitted.empty()) return;
    CHECK(h.emitted[0].buyExchange == "B");
    CHECK(h.emitted[0].sellExchange == "A");
    CHECK(h.emitted[0].decision == Decision::Accepted);
}

namespace {
// Engine over three exchanges whose books are seeded so the edge ranking is
// fixed: best = A->C, second = B->C, A->B below threshold.
struct PfaHarness {
    ArbitrageEngine engine;
    std::vector<Opportunity> emitted;
    std::shared_ptr<Portfolio> portfolio;

    PfaHarness()
        : engine(ArbitrageConfig{"BTC/USDT", {"A", "B", "C"}, 0.0004},
                 CostEngine(CostConfig{0.0005, 0.002, 0.0}),
                 LiquidityEngine(LiquidityConfig{1.0, 0.001}),
                 RiskEngine(RiskConfig{500.0, 100.0, 0.0001, 2.0, 0.5, 0.0, 1000})) {
        engine.setConnectedFn([](const std::string&) { return true; });
        engine.setOpportunityCallback([this](const Opportunity& o) { emitted.push_back(o); });
        portfolio = std::make_shared<Portfolio>();
        engine.setPortfolio(portfolio);
    }

    void seed() {
        const std::int64_t now = Timestamps::monoMs();
        Tick t;
        t.symbol = "BTC/USDT";
        t.receiveTs = now;
        t.book.symbol = "BTC/USDT";
        t.book.localReceiveTs = now;

        for (auto& [ex, ask, bid] :
             std::vector<std::tuple<std::string, double, double>>{
                 {"A", 100000.0, 99000.0},   // cheapest ask -> buy side
                 {"B", 100150.0, 100100.0},  // middle
                 {"C", 100200.0, 101000.0},  // richest bid -> sell side
             }) {
            t.exchange = ex;
            t.book.exchange = ex;
            t.book.asks = {{ask, 1.0}, {ask + 1.0, 1.0}};
            t.book.bids = {{bid, 1.0}, {bid - 1.0, 1.0}};
            engine.onTick(t);
        }
    }
};
}  // namespace

TEST(arbitrage_falls_back_to_best_affordable_pair) {
    PfaHarness h;
    h.portfolio->setQuote("A", 0.0);  // A is broke: best edge A->C unavailable
    h.portfolio->setQuote("B", 50.0);
    h.portfolio->setQuote("C", 50.0);
    h.portfolio->setBase("C", "BTC/USDT", 0.001);  // only C can deliver BTC

    h.seed();

    CHECK(h.emitted.size() == 1);
    if (h.emitted.empty()) return;
    const auto& o = h.emitted[0];
    CHECK(o.decision == Decision::Accepted);
    CHECK(o.buyExchange == "B");  // skipped unaffordable A, landed on B->C
    CHECK(o.sellExchange == "C");
}

TEST(arbitrage_nothing_affordable_emits_insufficient_funds) {
    PfaHarness h;
    h.portfolio->setQuote("A", 0.0);  // no buyer anywhere can afford a $40 leg
    h.portfolio->setQuote("B", 0.0);
    h.portfolio->setQuote("C", 0.0);
    h.portfolio->setBase("C", "BTC/USDT", 0.001);

    h.seed();

    CHECK(h.emitted.size() == 1);
    if (h.emitted.empty()) return;
    const auto& o = h.emitted[0];
    CHECK(o.decision == Decision::InsufficientFunds);
    CHECK(o.buyExchange == "A");  // diagnostic points at the best edge
    CHECK(o.sellExchange == "C");
}
