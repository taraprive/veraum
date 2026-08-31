#include "test_framework.h"

#include "src/engine/portfolio.h"

using namespace hftarb;

TEST(portfolio_seed_and_balance_query) {
    Portfolio p;
    p.setQuote("A", 50.0);
    p.setBase("A", "BTC/USDT", 0.001);
    p.setQuote("C", 50.0);
    p.setBase("C", "BTC/USDT", 0.001);

    CHECK_NEAR(p.quote("A"), 50.0, 1e-9);
    CHECK_NEAR(p.base("A", "BTC/USDT"), 0.001, 1e-12);
    // Unknown account / symbol -> zero, never throws.
    CHECK_NEAR(p.quote("NOPE"), 0.0, 1e-9);
    CHECK_NEAR(p.base("A", "ETH/USDT"), 0.0, 1e-12);
}

TEST(portfolio_can_trade_checks_both_legs) {
    Portfolio p;
    p.setQuote("A", 50.0);          // buyer has $50
    p.setBase("A", "BTC/USDT", 0.0);
    p.setQuote("C", 50.0);
    p.setBase("C", "BTC/USDT", 0.001);  // seller holds 0.001 BTC

    // Affordable buy + available inventory -> ok.
    CHECK(p.canTrade("A", "C", "BTC/USDT", 40.0, 0.0004));
    // Buyer can't cover the notional.
    CHECK(!p.canTrade("A", "C", "BTC/USDT", 60.0, 0.0004));
    // Seller lacks the base asset.
    CHECK(!p.canTrade("A", "C", "BTC/USDT", 40.0, 0.002));
}

TEST(portfolio_apply_fill_moves_money) {
    Portfolio p;
    p.setQuote("A", 50.0);
    p.setBase("A", "BTC/USDT", 0.0);
    p.setQuote("C", 50.0);
    p.setBase("C", "BTC/USDT", 0.001);

    p.applyFill("A", "C", "BTC/USDT", 0.0004, 100000.0, 0.0004, 100500.0);

    // A spent 0.0004 * 100000 = $40, now holds 0.0004 BTC.
    CHECK_NEAR(p.quote("A"), 10.0, 1e-6);
    CHECK_NEAR(p.base("A", "BTC/USDT"), 0.0004, 1e-12);
    // C sold 0.0004 BTC, received 0.0004 * 100500 = $40.2.
    CHECK_NEAR(p.base("C", "BTC/USDT"), 0.0006, 1e-12);
    CHECK_NEAR(p.quote("C"), 90.2, 1e-6);
}

TEST(portfolio_rebalance_moves_excess_inventory) {
    Portfolio p;
    // A has surplus BTC but is short USDT; C has surplus USDT but is short BTC.
    p.setQuote("A", 5.0);
    p.setBase("A", "BTC/USDT", 0.01);
    p.setQuote("C", 90.0);
    p.setBase("C", "BTC/USDT", 0.001);

    Portfolio::RebalanceConfig r;
    r.thresholdUsd = 10.0;
    r.maxSweepUsd = 25.0;
    r.transferCostBp = 5.0;
    auto mid = [](const std::string&) { return 100000.0; };

    const int transfers = p.rebalance(mid, r);
    CHECK(transfers == 1);
    // qty capped by maxSweepUsd: 25 / 100000 = 0.00025 BTC moved from A to C
    // at mid, so A gains $25; C pays $25 plus a 5bp on-chain-style transfer fee
    // (0.00025 * 100000 * 5e-4 = $0.0125), so C nets $64.9875.
    CHECK_NEAR(p.base("A", "BTC/USDT"), 0.00975, 1e-12);
    CHECK_NEAR(p.quote("A"), 30.0, 1e-6);
    CHECK_NEAR(p.base("C", "BTC/USDT"), 0.00125, 1e-12);
    CHECK_NEAR(p.quote("C"), 64.9875, 1e-6);
}

TEST(portfolio_rebalance_skips_small_imbalance) {
    Portfolio p;
    p.setQuote("A", 49.0);
    p.setBase("A", "BTC/USDT", 0.0011);
    p.setQuote("C", 51.0);
    p.setBase("C", "BTC/USDT", 0.001);

    Portfolio::RebalanceConfig r;
    r.thresholdUsd = 10.0;
    auto mid = [](const std::string&) { return 100000.0; };

    // Biggest candidate move is 0.00005 BTC = $5, below the $10 threshold.
    CHECK(p.rebalance(mid, r) == 0);
    CHECK_NEAR(p.base("A", "BTC/USDT"), 0.0011, 1e-12);
    CHECK_NEAR(p.quote("A"), 49.0, 1e-9);
}

TEST(portfolio_rebalance_respects_missing_symbol) {
    Portfolio p;
    // ETH only exists on A: nothing can be rebalanced for it.
    p.setQuote("A", 5.0);
    p.setBase("A", "ETH/USDT", 0.2);
    p.setQuote("C", 90.0);
    p.setBase("C", "BTC/USDT", 0.001);

    Portfolio::RebalanceConfig r;
    r.thresholdUsd = 10.0;
    auto mid = [](const std::string&) { return 100000.0; };

    CHECK(p.rebalance(mid, r) == 0);
}

TEST(portfolio_snapshot_all_returns_every_account) {
    Portfolio p;
    p.setQuote("A", 50.0);
    p.setBase("A", "BTC/USDT", 0.001);
    p.setBase("A", "ETH/USDT", 0.02);
    p.setQuote("C", 90.0);
    p.setBase("C", "BTC/USDT", 0.002);

    const auto all = p.snapshotAll();
    CHECK(all.size() == 2);
    CHECK_NEAR(all.at("A").usdt, 50.0, 1e-9);
    CHECK_NEAR(all.at("A").base.at("BTC/USDT"), 0.001, 1e-12);
    CHECK_NEAR(all.at("A").base.at("ETH/USDT"), 0.02, 1e-12);
    CHECK_NEAR(all.at("C").usdt, 90.0, 1e-9);
    CHECK_NEAR(all.at("C").base.at("BTC/USDT"), 0.002, 1e-12);
}
