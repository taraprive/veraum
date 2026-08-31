#include "test_framework.h"

#include <algorithm>
#include <tuple>
#include <vector>

#include "src/core/timestamp.h"
#include "src/engine/cost_engine.h"
#include "src/engine/multi_strategy_engine.h"
#include "src/engine/risk_engine.h"
#include "src/engine/triangular_strategy.h"
#include "src/execution/paper_executor.h"

using namespace hftarb;

namespace {

// Canonical forward triangle: USDT -> BTC -> ETH -> USDT.
AppConfig::TriangleConfig makeTriangle(double notional = 500.0) {
    AppConfig::TriangleConfig tc;
    tc.name = "USDT_BTC_ETH";
    tc.exchange = "binance";
    tc.startAsset = "USDT";
    tc.notional = notional;
    tc.legs = {{"BTC/USDT", Side::Buy}, {"ETH/BTC", Side::Buy},
               {"ETH/USDT", Side::Sell}};
    return tc;
}

OrderBook makeBook(double ask, double bid, double depth, std::int64_t ageMs) {
    const std::int64_t now = Timestamps::monoMs();
    OrderBook b;
    b.exchange = "binance";
    b.bids = {{bid, depth}, {bid * 0.999, depth}};
    b.asks = {{ask, depth}, {ask * 1.001, depth}};
    b.localReceiveTs = now - ageMs;
    b.exchangeTs = b.localReceiveTs;
    return b;
}

struct TriHarness {
    std::vector<Opportunity> emitted;
    TriangularStrategy engine;

    TriHarness(double notional = 500.0, double cooldownMs = 1000000.0,
               double maxDataAgeMs = 500.0)
        : engine(makeTriangle(notional),
                 CostEngine(CostConfig{0.0005, 0.002, 0.0}),
                 RiskEngine(RiskConfig{maxDataAgeMs, 100.0, 0.001,
                                       1e9 /* max trade qty */, 1e9, cooldownMs,
                                       20, 0.0})) {
        engine.setConnectedFn([](const std::string&) { return true; });
        engine.setOpportunityCallback(
            [this](const Opportunity& o) { emitted.push_back(o); });
        FeeSchedule f;
        f.takerBuyFee = 0.001;
        f.takerSellFee = 0.001;
        engine.setFeeSchedule("binance", f);
    }

    void feed(const std::string& sym, double ask, double bid, double depth = 1000.0,
              std::int64_t ageMs = 0) {
        const std::int64_t now = Timestamps::monoMs();
        Tick t;
        t.exchange = "binance";
        t.symbol = sym;
        t.book = makeBook(ask, bid, depth, ageMs);
        t.receiveTs = now;
        engine.onTick(t);
    }
};

// A profitable cycle: buy BTC/USDT @100, buy ETH/BTC @0.05 (P1*P2 = 5),
// sell ETH/USDT @5.06 -> 1.2% gross, well above fees (3x0.1%) + margin.
void feedProfitable(TriHarness& h) {
    h.feed("BTC/USDT", 100.0, 99.0);
    h.feed("ETH/BTC", 0.05, 0.049);
    h.feed("ETH/USDT", 5.07, 5.06);
}

}  // namespace

TEST(triangular_detects_and_accepts_cycle) {
    TriHarness h;
    feedProfitable(h);

    CHECK(h.emitted.size() == 1);
    if (h.emitted.empty()) return;
    const Opportunity& o = h.emitted[0];
    CHECK(o.strategy == "triangular");
    CHECK(o.symbol == "USDT_BTC_ETH");
    CHECK(o.decision == Decision::Accepted);
    CHECK(o.legs.size() == 3);
    CHECK(o.cost.netSpread > 0.0);
    CHECK(o.cost.grossSpread > 0.0);
    if (o.legs.size() == 3) {
        CHECK(o.legs[0].symbol == "BTC/USDT");
        CHECK(o.legs[0].side == Side::Buy);
        CHECK(o.legs[1].symbol == "ETH/BTC");
        CHECK(o.legs[1].side == Side::Buy);
        CHECK(o.legs[2].symbol == "ETH/USDT");
        CHECK(o.legs[2].side == Side::Sell);
        // All three legs on the single triangle venue, prices positive.
        CHECK(o.legs[0].price > 0.0);
        CHECK(o.legs[1].price > 0.0);
        CHECK(o.legs[2].price > 0.0);
        // Entry base qty consistent with notional / ask.
        CHECK_NEAR(o.legs[0].quantity * o.legs[0].price, 500.0, 10.0);
    }
}

TEST(triangular_no_edge_emits_nothing) {
    TriHarness h;
    // P3 == P1*P2 exactly: no gross edge of any direction.
    h.feed("BTC/USDT", 100.0, 99.0);
    h.feed("ETH/BTC", 0.05, 0.049);
    h.feed("ETH/USDT", 5.0, 5.0);
    CHECK(h.emitted.empty());
}

TEST(triangular_below_threshold_reports_reject) {
    TriHarness h;
    // 0.5% gross, above the 3x0.1% fees but below the 20bp min net threshold.
    h.feed("BTC/USDT", 100.0, 99.0);
    h.feed("ETH/BTC", 0.05, 0.049);
    h.feed("ETH/USDT", 5.02, 5.02);
    CHECK(h.emitted.size() >= 1);
    if (h.emitted.empty()) return;
    const auto& o = h.emitted.back();
    CHECK(o.decision == Decision::Rejected);
}

TEST(triangular_liquidity_gate_silent) {
    TriHarness h;
    // Profitable cross but leg 0 book only shows 0.5 vs ~5 required units.
    h.feed("BTC/USDT", 100.0, 99.0, /*depth=*/0.5);
    h.feed("ETH/BTC", 0.05, 0.049);
    h.feed("ETH/USDT", 5.07, 5.06);
    CHECK(h.emitted.empty());
}

TEST(triangular_stale_books_rejected) {
    TriHarness h;
    // Books stamped 10s before now on a 500ms freshness budget: the only
    // books the engine has seen are stale -> StaleData.
    h.feed("BTC/USDT", 100.0, 99.0, 1000.0, /*ageMs=*/10000);
    h.feed("ETH/BTC", 0.05, 0.049, 1000.0, /*ageMs=*/10000);
    h.feed("ETH/USDT", 5.07, 5.06, 1000.0, /*ageMs=*/10000);
    const auto it = std::find_if(h.emitted.begin(), h.emitted.end(),
                                 [](const Opportunity& o) {
                                     return o.decision == Decision::StaleData;
                                 });
    CHECK(it != h.emitted.end());
}

TEST(triangular_cooldown_blocks_reentry) {
    TriHarness h(/*notional=*/500.0, /*cooldownMs=*/1000000.0);
    feedProfitable(h);
    CHECK(!h.emitted.empty());
    const auto accepted = std::find_if(
        h.emitted.begin(), h.emitted.end(),
        [](const Opportunity& o) { return o.decision == Decision::Accepted; });
    CHECK(accepted != h.emitted.end());

    // Re-tick the same profitable books immediately: cooldown must win.
    h.feed("BTC/USDT", 100.0, 99.0);
    h.feed("ETH/BTC", 0.05, 0.049);
    h.feed("ETH/USDT", 5.07, 5.06);
    const auto cooled = std::find_if(
        h.emitted.begin(), h.emitted.end(),
        [](const Opportunity& o) { return o.decision == Decision::Cooldown; });
    CHECK(cooled != h.emitted.end());
}

TEST(triangular_has_symbol_covers_cross_pairs) {
    TriHarness h;
    CHECK(h.engine.hasSymbol("BTC/USDT"));
    CHECK(h.engine.hasSymbol("ETH/BTC"));
    CHECK(h.engine.hasSymbol("ETH/USDT"));
    CHECK(!h.engine.hasSymbol("DOGE/USDT"));
    CHECK(h.engine.name() == "triangular");
}

TEST(triangular_exposure_tracks_residual) {
    TriHarness h;
    CHECK(h.engine.exposure("USDT_BTC_ETH") == 0.0);
    h.engine.addExposure("USDT_BTC_ETH", 2.5);
    CHECK_NEAR(h.engine.exposure("USDT_BTC_ETH"), 2.5, 1e-9);
    h.engine.clearExposure("USDT_BTC_ETH");
    CHECK(h.engine.exposure("USDT_BTC_ETH") == 0.0);
}

TEST(triangular_routes_through_multi_strategy) {
    MultiStrategyEngine ms;
    std::vector<Opportunity> emitted;
    auto engine = std::make_shared<TriangularStrategy>(
        makeTriangle(), CostEngine(CostConfig{0.0005, 0.002, 0.0}),
        RiskEngine(RiskConfig{500.0, 100.0, 0.001, 1e9, 1e9, 1000000.0, 20, 0.0}));
    FeeSchedule f;
    f.takerBuyFee = 0.001;
    f.takerSellFee = 0.001;
    engine->setFeeSchedule("binance", f);
    engine->setConnectedFn([](const std::string&) { return true; });
    engine->setOpportunityCallback(
        [&emitted](const Opportunity& o) { emitted.push_back(o); });
    ms.addStrategy(engine);

    // The dispatcher owns the triangle's cross pairs even though no
    // cross-exchange strategy was registered.
    CHECK(ms.hasSymbol("ETH/BTC"));
    CHECK(ms.hasSymbol("BTC/USDT"));

    const std::int64_t now = Timestamps::monoMs();
    for (const auto& [sym, ask, bid] :
         std::vector<std::tuple<std::string, double, double>>{
             {"BTC/USDT", 100.0, 99.0},
             {"ETH/BTC", 0.05, 0.049},
             {"ETH/USDT", 5.07, 5.06}}) {
        Tick t;
        t.exchange = "binance";
        t.symbol = sym;
        t.book = makeBook(ask, bid, 1000.0, 0);
        t.receiveTs = now;
        ms.onTick(t);
    }

    const auto accepted = std::find_if(
        emitted.begin(), emitted.end(),
        [](const Opportunity& o) { return o.decision == Decision::Accepted; });
    CHECK(accepted != emitted.end());
    if (accepted != emitted.end()) {
        CHECK(accepted->strategy == "triangular");
        CHECK(accepted->legs.size() == 3);
    }
}

TEST(triangular_accepted_cycle_runs_through_paper) {
    TriHarness h;
    feedProfitable(h);
    const auto accepted = std::find_if(
        h.emitted.begin(), h.emitted.end(),
        [](const Opportunity& o) { return o.decision == Decision::Accepted; });
    CHECK(accepted != h.emitted.end());
    if (accepted == h.emitted.end()) return;

    PaperExecutor pe(
        PaperConfig{0.97, 0.93, 0.05, 0.01, 20.0, 0.5, 0.35}, 123u);
    const ExecutionResult r = pe.execute(*accepted);
    CHECK(r.status == "filled" || r.status == "partial");
    CHECK(r.buyFilled > 0.0);
    CHECK(r.sellFilled > 0.0 || r.status == "filled");
    CHECK(r.exposureLeft >= 0.0);
    CHECK(r.realizedPnl > -0.05);  // adverse sim noise stays tiny
}