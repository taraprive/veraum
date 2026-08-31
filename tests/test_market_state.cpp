#include "test_framework.h"

#include <cmath>

#include "src/market/market_state.h"

using namespace hftarb;

namespace {

SymbolMarketParams btcParams() {
    SymbolMarketParams p;
    p.basePrice = 100000.0;
    p.liquidityTopQty = 0.1;
    return p;
}

MarketDynamics quietDynamics() {
    MarketDynamics d;
    d.volBp = 0.0;
    d.driftBp = 0.0;
    d.basisVolBp = 0.0;
    d.basisReversion = 0.0;
    d.spreadMinBp = 2.0;
    d.spreadMaxBp = 2.0;
    d.spreadWalkBp = 0.0;
    d.burstProbability = 0.0;
    return d;
}

}  // namespace

TEST(market_state_books_are_valid_and_realistic) {
    MarketDynamics d = quietDynamics();
    std::unordered_map<std::string, SymbolMarketParams> params{{"BTC/USDT", btcParams()}};
    MarketState m(d, params, 7u);

    const std::int64_t updateMs = 10;
    for (std::int64_t t = 1; t <= 20; ++t) {
        const OrderBook b = m.tick("exchange_a", "BTC/USDT", t * updateMs, updateMs);
        CHECK(b.exchange == "exchange_a");
        CHECK(b.symbol == "BTC/USDT");
        CHECK(b.bids.size() == 10);
        CHECK(b.asks.size() == 10);
        CHECK(b.valid());
        // Bids sorted descending, asks ascending, no crossing.
        for (size_t i = 1; i < b.bids.size(); ++i) CHECK(b.bids[i].price < b.bids[i - 1].price);
        for (size_t i = 1; i < b.asks.size(); ++i) CHECK(b.asks[i].price > b.asks[i - 1].price);
        CHECK(*b.bestBid() < *b.bestAsk());
        // Positive quantities everywhere.
        for (const auto& lv : b.bids) CHECK(lv.quantity > 0.0);
        for (const auto& lv : b.asks) CHECK(lv.quantity > 0.0);
        // Mid anchored at base price (no drift/vol in quiet mode).
        const double mid = (b.asks.front().price + b.bids.front().price) / 2.0;
        CHECK_NEAR(mid, 100000.0, 10.0);
    }
}

TEST(market_state_venues_stay_correlated) {
    MarketDynamics d;
    d.basisVolBp = 0.9;
    d.basisReversion = 0.08;
    d.spreadMinBp = 1.0;
    d.spreadMaxBp = 4.0;
    d.burstProbability = 0.0;  // isolate basis behaviour
    std::unordered_map<std::string, SymbolMarketParams> params{{"BTC/USDT", btcParams()}};
    MarketState m(d, params, 11u);

    const std::int64_t updateMs = 10;
    double maxVenueSpreadRatio = 0.0;
    for (std::int64_t t = 1; t <= 500; ++t) {
        const OrderBook a = m.tick("exchange_a", "BTC/USDT", t * updateMs, updateMs);
        const OrderBook b = m.tick("exchange_b", "BTC/USDT", t * updateMs, updateMs);
        // Both venues quote within a few bp of the shared underlying even
        // though each carries independent basis noise.
        CHECK(std::fabs(*a.bestBid() / 100000.0 - 1.0) < 0.01);
        CHECK(std::fabs(*b.bestBid() / 100000.0 - 1.0) < 0.01);
        const double ratio = *a.bestBid() / *b.bestBid();
        maxVenueSpreadRatio = std::max(maxVenueSpreadRatio, std::fabs(ratio - 1.0));
    }
    // Two venues never diverge by more than ~30bp across 500 updates.
    CHECK(maxVenueSpreadRatio < 0.003);
}

TEST(market_state_reproducible_with_seed) {
    MarketDynamics d = quietDynamics();
    std::unordered_map<std::string, SymbolMarketParams> params{{"BTC/USDT", btcParams()}};
    MarketState m1(d, params, 99u);
    MarketState m2(d, params, 99u);

    const std::int64_t updateMs = 10;
    for (std::int64_t t = 1; t <= 30; ++t) {
        const OrderBook b1 = m1.tick("exchange_a", "BTC/USDT", t * updateMs, updateMs);
        const OrderBook b2 = m2.tick("exchange_a", "BTC/USDT", t * updateMs, updateMs);
        CHECK_NEAR(b1.bids.front().price, b2.bids.front().price, 1e-9);
        CHECK_NEAR(b1.asks.front().price, b2.asks.front().price, 1e-9);
        CHECK_NEAR(b1.bids.front().quantity, b2.bids.front().quantity, 1e-12);
    }
}

TEST(market_state_burst_creates_inefficiency) {
    MarketDynamics d = quietDynamics();
    d.burstProbability = 1.0;  // every update starts a burst
    d.burstBp = 300.0;         // 3% deviation
    d.burstMinTicks = 3;
    d.burstMaxTicks = 3;
    std::unordered_map<std::string, SymbolMarketParams> params{{"BTC/USDT", btcParams()}};
    MarketState m(d, params, 5u);

    const std::int64_t updateMs = 10;
    const OrderBook b = m.tick("exchange_a", "BTC/USDT", updateMs, updateMs);
    // Mid did not move (no vol/drift), so any large deviation is the burst.
    const double devBid = std::fabs(*b.bestBid() / 100000.0 - 1.0);
    const double devAsk = std::fabs(*b.bestAsk() / 100000.0 - 1.0);
    CHECK(std::max(devBid, devAsk) > 0.015);  // > 1.5% from a 3% burst
}

TEST(market_state_spread_override) {
    MarketDynamics d = quietDynamics();
    std::unordered_map<std::string, SymbolMarketParams> params{{"BTC/USDT", btcParams()}};
    MarketState m(d, params, 3u);
    m.setExchangeSpreadBp("exchange_a", 1.0);

    // Let the spread mean-revert toward the 1bp override.
    const std::int64_t updateMs = 10;
    OrderBook b;
    for (std::int64_t t = 1; t <= 20; ++t) {
        b = m.tick("exchange_a", "BTC/USDT", t * updateMs, updateMs);
    }
    const double spreadRatio = (*b.bestAsk() - *b.bestBid()) / *b.bestBid();
    // 1bp spread on a ~100k price (override is below the 2bp default clamp).
    CHECK_NEAR(spreadRatio, 1.0 / 1e4, 0.5 / 1e4);
}
