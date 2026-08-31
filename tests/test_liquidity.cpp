#include "test_framework.h"

#include "src/engine/liquidity_engine.h"

using namespace hftarb;

static OrderBook deepBook() {
    OrderBook b;
    b.asks = {{100.0, 2.0}, {101.0, 2.0}, {102.0, 4.0}};
    b.bids = {{99.0, 2.0}, {98.0, 2.0}, {97.0, 4.0}};
    return b;
}

TEST(liquidity_sufficient_when_depth_covers_qty) {
    LiquidityEngine e(LiquidityConfig{});
    const auto r = e.check(deepBook(), deepBook(), 0.5);
    CHECK(r.sufficient);
    CHECK(r.reason.empty());
}

TEST(liquidity_rejected_when_depth_insufficient) {
    LiquidityEngine e(LiquidityConfig{});
    const auto r = e.check(deepBook(), deepBook(), 50.0);
    CHECK(!r.sufficient);
    CHECK(r.reason == "insufficient_visible_depth");
}

TEST(liquidity_rejected_when_slippage_exceeds_cap) {
    LiquidityConfig cfg;
    cfg.maxAcceptableSlippage = 0.001;  // 10 bp
    LiquidityEngine e(cfg);
    // Qty 2.0 walks to the second level: ~1% slippage on each side.
    const auto r = e.check(deepBook(), deepBook(), 3.0);
    CHECK(!r.sufficient);
    CHECK(r.reason == "slippage_exceeds_cap");
}

TEST(liquidity_zero_qty) {
    LiquidityEngine e(LiquidityConfig{});
    const auto r = e.check(deepBook(), deepBook(), 0.0);
    CHECK(!r.sufficient);
}
