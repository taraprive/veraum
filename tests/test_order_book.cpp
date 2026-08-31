#include "test_framework.h"

#include "src/core/order_book.h"

using namespace hftarb;

static OrderBook bookWithLevels() {
    OrderBook b;
    b.exchange = "test";
    b.symbol = "BTC/USDT";
    b.asks = {{100.0, 0.5}, {101.0, 0.5}, {102.0, 2.0}};
    b.bids = {{99.0, 0.5}, {98.0, 0.5}, {97.0, 2.0}};
    return b;
}

TEST(order_book_best_ask_and_bid) {
    const auto b = bookWithLevels();
    CHECK(b.bestAsk().has_value());
    CHECK(b.bestBid().has_value());
    if (b.bestAsk()) CHECK_NEAR(*b.bestAsk(), 100.0, 1e-9);
    if (b.bestBid()) CHECK_NEAR(*b.bestBid(), 99.0, 1e-9);
    CHECK(b.valid());
}

TEST(order_book_avg_buy_price_crosses_levels) {
    const auto b = bookWithLevels();
    // 0.5 qty fills entirely at the best ask: avg = 100.0
    const auto vwap = OrderBookUtils::avgBuyPrice(b, 0.5);
    CHECK(vwap.has_value());
    if (vwap) CHECK_NEAR(*vwap, 100.0, 1e-9);
    // 0.75 @ 100 + 0.25 @ 101 => avg = (75 + 25.25)/0.75 = 100.3333
    const auto vwap2 = OrderBookUtils::avgBuyPrice(b, 0.75);
    CHECK(vwap2.has_value());
    if (vwap2) CHECK_NEAR(*vwap2, 100.33333333, 1e-6);
}

TEST(order_book_avg_buy_price_exceeds_depth) {
    const auto b = bookWithLevels();
    const auto vwap = OrderBookUtils::avgBuyPrice(b, 10.0);  // only 3.0 available
    CHECK(!vwap.has_value());
}

TEST(order_book_avg_sell_price) {
    const auto b = bookWithLevels();
    // 0.5 qty fills entirely at the best bid: avg = 99.0
    const auto vwap = OrderBookUtils::avgSellPrice(b, 0.5);
    CHECK(vwap.has_value());
    if (vwap) CHECK_NEAR(*vwap, 99.0, 1e-9);
    // 0.25 @ 99 + 0.25 @ 98 => avg = (24.75 + 24.5)/0.5 = 98.5
    const auto vwap2 = OrderBookUtils::avgSellPrice(b, 1.0);
    (void)vwap2;
    const auto vwap3 = OrderBookUtils::avgSellPrice(b, 0.75);
    CHECK(vwap3.has_value());
    if (vwap3) CHECK_NEAR(*vwap3, (0.5 * 99.0 + 0.25 * 98.0) / 0.75, 1e-9);
}

TEST(order_book_slippage_fractions) {
    const auto b = bookWithLevels();
    const double buySlip = OrderBookUtils::buySlippage(b, 1.0);
    // vwap(1.0) = (0.5*100 + 0.5*101)/1 = 100.5; slip = 0.5/100 = 0.005
    CHECK_NEAR(buySlip, 0.005, 1e-9);
    const double sellSlip = OrderBookUtils::sellSlippage(b, 1.0);
    // vwap = 98.5; slip = (99 - 98.5)/99
    CHECK_NEAR(sellSlip, 0.5 / 99.0, 1e-9);
}

TEST(order_book_available_qty) {
    const auto b = bookWithLevels();
    CHECK_NEAR(OrderBookUtils::availableQty(b.asks), 3.0, 1e-9);
    CHECK_NEAR(OrderBookUtils::availableQty(b.bids), 3.0, 1e-9);
}

TEST(order_book_freshness) {
    auto b = bookWithLevels();
    b.localReceiveTs = 1000;
    CHECK(OrderBookUtils::isFresh(b, 1200, 500));
    CHECK(!OrderBookUtils::isFresh(b, 1600, 500));
}
