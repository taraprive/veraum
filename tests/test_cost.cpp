#include "test_framework.h"

#include "src/engine/cost_engine.h"

using namespace hftarb;

static OrderBook makeBook(double ask, double bid) {
    OrderBook b;
    b.asks = {{ask, 1.0}, {ask + 1.0, 1.0}};
    b.bids = {{bid, 1.0}, {bid - 1.0, 1.0}};
    return b;
}

TEST(cost_gross_spread_is_relative) {
    CostEngine e(CostConfig{});
    CHECK_NEAR(e.grossSpread(100.0, 101.0), 0.01, 1e-9);
    CHECK_NEAR(e.grossSpread(200.0, 200.0), 0.0, 1e-9);
    CHECK_NEAR(e.grossSpread(100.0, 99.0), -0.01, 1e-9);
}

TEST(cost_net_spread_subtracts_everything) {
    CostEngine e(CostConfig{0.0005, 0.002, 0.0});  // margin, threshold, net
    FeeSchedule f;
    f.takerBuyFee = 0.001;
    f.takerSellFee = 0.001;
    f.networkCost = 0.0;

    OrderBook buy = makeBook(100.0, 99.0);
    OrderBook sell = makeBook(101.0, 100.6);  // ~0.6% gross on the bid side
    const auto c = e.evaluate(buy, sell, 0.5, f, f);

    CHECK(c.has_value());
    if (!c) return;
    // gross = 100.6/100 - 1 = 0.006
    // fees = 0.002, slippage ~0, network 0, margin 0.0005
    CHECK_NEAR(c->grossSpread, 0.006, 1e-9);
    CHECK_NEAR(c->netSpread, 0.006 - 0.002 - 0.0005, 1e-9);
}

TEST(cost_rejects_insufficient_liquidity) {
    CostEngine e(CostConfig{});
    FeeSchedule f;
    OrderBook buy = makeBook(100.0, 99.0);
    OrderBook sell = makeBook(101.0, 100.6);
    // Ask only 0.5 available on the buy side but we want 5.0.
    const auto c = e.evaluate(buy, sell, 5.0, f, f);
    CHECK(!c.has_value());
}

TEST(cost_rejects_negative_gross) {
    CostEngine e(CostConfig{});
    FeeSchedule f;
    // Buying at 101 to sell at 100: no edge.
    const auto c = e.evaluate(makeBook(101.0, 100.0), makeBook(101.0, 100.0), 0.5, f, f);
    CHECK(!c.has_value());
}

TEST(cost_threshold_gate) {
    CostConfig cfg;
    cfg.minNetSpread = 0.01;  // 1%
    CostEngine e(cfg);
    FeeSchedule f;
    f.takerBuyFee = 0.0;
    f.takerSellFee = 0.0;
    const auto c = e.evaluate(makeBook(100.0, 99.0), makeBook(101.0, 100.5), 0.5, f, f);
    CHECK(c.has_value());
    if (!c) return;
    CHECK(!e.meetsThreshold(*c));  // ~0.5% < 1%
}
