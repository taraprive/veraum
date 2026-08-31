#include <iostream>

#include "test_framework.h"

#include "src/exchange/binance_adapter.h"
#include "src/exchange/bybit_adapter.h"

using namespace hftarb;

TEST(binance_live_depth_public) {
    BinanceAdapter a("https://testnet.binance.vision", "", "");
    const auto book = a.fetchDepth("BTCUSDT", 5);
    if (!book) {
        std::cerr << "  SKIP binance_live_depth_public (testnet unreachable)\n";
        return;
    }
    CHECK(!book->bids.empty());
    CHECK(!book->asks.empty());
    CHECK(book->bids.front().first > 0.0);
    CHECK(book->asks.front().first > 0.0);
    CHECK(book->bids.front().first <= book->asks.front().first * 1.001);
    CHECK(book->bids.front().second > 0.0);
}

TEST(binance_live_time_offset) {
    BinanceAdapter a("https://testnet.binance.vision", "", "");
    const auto off = a.fetchServerTimeOffsetMs();
    if (!off) {
        std::cerr << "  SKIP binance_live_time_offset (testnet unreachable)\n";
        return;
    }
    CHECK(*off > -5000 && *off < 5000);
}

TEST(binance_signed_requires_credentials) {
    BinanceAdapter a("https://testnet.binance.vision", "", "");
    const auto balances = a.fetchBalances();
    if (!balances.has_value()) {
        std::cerr << "  NOTE binance_signed_requires_credentials: rejected as expected\n";
        return;
    }
    CHECK(false);
}

TEST(bybit_live_depth_public) {
    BybitAdapter a("https://api-testnet.bybit.com", "", "");
    const auto book = a.fetchDepth("BTCUSDT", 5);
    if (!book) {
        std::cerr << "  SKIP bybit_live_depth_public (testnet unreachable)\n";
        return;
    }
    CHECK(!book->bids.empty());
    CHECK(!book->asks.empty());
    CHECK(book->bids.front().first > 0.0);
    CHECK(book->asks.front().first > 0.0);
    CHECK(book->bids.front().first <= book->asks.front().first * 1.001);
    CHECK(book->bids.front().second > 0.0);
}

TEST(bybit_live_depth_cross_check) {
    BinanceAdapter b("https://testnet.binance.vision", "", "");
    BybitAdapter y("https://api-testnet.bybit.com", "", "");
    const auto bb = b.fetchDepth("BTCUSDT", 5);
    const auto yb = y.fetchDepth("BTCUSDT", 5);
    if (!bb || !yb) {
        std::cerr << "  SKIP bybit_live_depth_cross_check (one or both testnets unreachable)\n";
        return;
    }
    const double binanceMid = (bb->bids.front().first + bb->asks.front().first) * 0.5;
    const double bybitMid = (yb->bids.front().first + yb->asks.front().first) * 0.5;
    const double spread = std::abs(binanceMid - bybitMid) / binanceMid;
    std::cerr << "  INFO binance_mid=" << binanceMid << " bybit_mid=" << bybitMid
              << " spread=" << (spread * 10000.0) << "bp\n";
    CHECK(spread < 0.02);
}
