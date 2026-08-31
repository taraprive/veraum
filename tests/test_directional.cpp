#include <cmath>
#include <string>
#include <vector>

#include "test_framework.h"

#include "src/engine/directional_strategy.h"

using namespace hftarb;

namespace {
InstrumentSpec spec() {
    InstrumentSpec s;
    s.instrument = "XAUUSD";
    s.anchor = 2000.0;
    s.fastEma = 5;
    s.slowEma = 15;
    s.atrPeriod = 5;
    s.stopAtRisk = 2.0;
    s.targetAtRisk = 3.0;
    s.trendEma = 0;  // off in the basic tests; the filter gets its own test
    s.reentryCooldownMs = 0;
    return s;
}

// i in [0,25): gradual decline; i in [25,60): strong, sustained rise -> the
// fast EMA must cross above the slow EMA shortly after the reversal -> LONG.
std::vector<Price> uptrendSeries() {
    std::vector<Price> pts;
    for (int i = 0; i < 25; ++i) pts.push_back(100.0 - i * 0.5);
    for (int i = 0; i < 35; ++i) pts.push_back(90.0 + i * 1.2);
    return pts;
}

// Mirror image: rise, then sustained fall -> SHORT.
std::vector<Price> downtrendSeries() {
    std::vector<Price> pts;
    for (int i = 0; i < 25; ++i) pts.push_back(100.0 + i * 0.5);
    for (int i = 0; i < 35; ++i) pts.push_back(112.5 - i * 1.2);
    return pts;
}
}  // namespace

TEST(directional_uptrend_emits_long) {
    DirectionalStrategy strat(spec());
    const DirectionalSignal* found = nullptr;
    DirectionalSignal kept;
    strat.setSignalCallback([&](const DirectionalSignal& s) {
        if (!found) {
            found = &kept;
            kept = s;
        }
    });
    std::int64_t ts = 0;
    for (const auto p : uptrendSeries()) strat.onTick(p, ++ts);
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(kept.side == Side::Buy);
    CHECK(kept.entry > 0.0);
    CHECK(kept.stop < kept.entry);
    CHECK(kept.target > kept.entry);
    CHECK(kept.reason.find("uptrend") != std::string::npos);
}

TEST(directional_downtrend_emits_short) {
    DirectionalStrategy strat(spec());
    const DirectionalSignal* found = nullptr;
    DirectionalSignal kept;
    strat.setSignalCallback([&](const DirectionalSignal& s) {
        if (!found) {
            found = &kept;
            kept = s;
        }
    });
    std::int64_t ts = 0;
    for (const auto p : downtrendSeries()) strat.onTick(p, ++ts);
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(kept.side == Side::Sell);
    CHECK(kept.stop > kept.entry);
    CHECK(kept.target < kept.entry);
    CHECK(kept.reason.find("downtrend") != std::string::npos);
}

TEST(directional_format_is_honest) {
    DirectionalSignal s;
    s.instrument = "XAUUSD";
    s.side = Side::Buy;
    s.entry = 2145.0;
    s.stop = 2130.0;
    s.target = 2175.0;
    s.reason = "uptrend test";
    const std::string text = DirectionalStrategy::format(s);
    CHECK(text.find("SIGNAL XAUUSD LONG") != std::string::npos);
    CHECK(text.find("R:R") != std::string::npos);
    CHECK(text.find("not guaranteed profit") != std::string::npos);
}

TEST(directional_backtest_reports_stats) {
    auto s = spec();
    // Alternating strong up/down cycles gives the EMA model many crossings
    // that should be resolved to a finite, well-formed win/loss tally.
    std::vector<Price> pts;
    for (int wave = 0; wave < 6; ++wave) {
        const bool up = (wave % 2) == 0;
        for (int i = 0; i < 40; ++i) {
            const double edge = up ? i * 0.8 : -i * 0.8;
            pts.push_back(100.0 + edge + 10.0 * (i % 3));
        }
    }
    const BacktestStats st = DirectionalStrategy::backtest(s, pts);
    CHECK(st.trades >= 2);
    CHECK(st.winRate >= 0.0 && st.winRate <= 1.0);
    CHECK(st.maxDrawdownBp >= 0.0);
    CHECK(st.totalPnlBp == st.totalPnlBp);  // not NaN
    CHECK(st.trades == st.wins + st.losses);
}

TEST(directional_backtest_empty_series_safe) {
    const BacktestStats st = DirectionalStrategy::backtest(spec(), {});
    CHECK(st.trades == 0);
    CHECK(st.winRate == 0.0);
}

TEST(directional_backtest_short_pnl_sign) {
    // Regression: a SHORT that rides down to its target must contribute a
    // *positive* PnL. The old code subtracted sell-side PnL with the wrong
    // sign, so a winning short looked like a losing trade in totalPnlBp.
    auto s = spec();
    std::vector<Price> pts;
    for (int i = 0; i < 40; ++i) pts.push_back(100.0);  // warm-up plateau
    for (const double p : {95.0, 90.0, 85.0, 80.0, 75.0, 70.0, 65.0, 60.0,
                           58.0, 55.0, 52.0, 50.0, 48.0, 46.0, 44.0, 42.0,
                           40.0, 39.0, 38.0, 37.0, 36.0, 35.0, 34.0, 33.0})
        pts.push_back(p);
    const BacktestStats st = DirectionalStrategy::backtest(s, pts);
    CHECK(st.trades >= 1);
    CHECK(st.wins >= 1);
    CHECK(st.totalPnlBp > 0.0);
}

TEST(directional_trend_overlay_blocks_counter_trend) {
    // A bullish EMA cross that happens while price still trades below the
    // higher-timeframe trend EMA must be vetoed (only with-the-trend entries).
    auto s = spec();
    s.trendEma = 60;
    s.reentryCooldownMs = 0;
    std::vector<Price> pts;
    // Warm-up (filter inactive): a pure decline so no LONG can form there.
    for (int i = 0; i < 60; ++i) pts.push_back(100.0 - i * 0.5);
    // Then a declining wave: the fast/slow EMAs cross repeatedly (several
    // cycles), but every crossing occurs below the slow-decaying trend EMA,
    // so every candidate entry is a SHORT. No LONG may slip through.
    for (int i = 0; i < 100; ++i) {
        const double base = 70.0 - i * 0.3;
        const double wave = 6.0 * std::sin(2.0 * 3.14159265 * i / 12.0);
        pts.push_back(base + wave);
    }
    const BacktestStats st = DirectionalStrategy::backtest(s, pts);
    CHECK(st.trades >= 1);
    // Verify via the live engine: no Buy signal is delivered while price < trend EMA.
    DirectionalStrategy strat(s);
    std::vector<std::string> sides;
    strat.setSignalCallback([&](const DirectionalSignal& sig) {
        sides.push_back(sig.side == Side::Buy ? "LONG" : "SHORT");
    });
    std::int64_t ts = 0;
    for (const auto p : pts) strat.onTick(p, ++ts * 1000);
    CHECK(!sides.empty());
    for (const auto& side : sides) CHECK(side == "SHORT");
}

TEST(directional_trend_overlay_allows_trend_entry) {
    // Sustained rise: price sits above the trend EMA, so the LONG is allowed.
    auto s = spec();
    s.trendEma = 10;
    s.reentryCooldownMs = 0;
    const BacktestStats st = DirectionalStrategy::backtest(s, uptrendSeries());
    CHECK(st.trades >= 1);
    CHECK(st.wins + st.losses == st.trades);
    DirectionalStrategy strat(s);
    const DirectionalSignal* found = nullptr;
    DirectionalSignal kept;
    strat.setSignalCallback([&](const DirectionalSignal& sig) {
        if (!found) {
            found = &kept;
            kept = sig;
        }
    });
    std::int64_t ts = 0;
    for (const auto p : uptrendSeries()) strat.onTick(p, ++ts * 1000);
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(kept.side == Side::Buy);
}

TEST(directional_sizing_gold_contract) {
    // XAUUSD.m: 100 oz per lot. Risk $50 with a $20 stop -> loss per lot $2000
    // -> 0.025 lots, rounded DOWN to the 0.01 step = 0.02 (risk capped at $40).
    InstrumentSpec spec;
    spec.contractSize = 100.0;
    spec.lotStep = 0.01;
    spec.minLot = 0.01;
    spec.maxLot = 100.0;
    spec.riskUsd = 50.0;
    DirectionalSignal s;
    s.entry = 4420.0;
    s.stop = 4400.0;
    s.target = 4435.0;
    const double lots = DirectionalStrategy::suggestLots(spec, s);
    CHECK(lots == 0.02);
    CHECK(DirectionalStrategy::sizeAdvice(spec, s).find("LOT") != std::string::npos);
}

TEST(directional_sizing_disabled_and_clamped) {
    InstrumentSpec spec;
    spec.contractSize = 100.0;
    spec.lotStep = 0.01;
    spec.minLot = 0.05;
    spec.maxLot = 0.5;
    DirectionalSignal s;
    s.entry = 3000.0;
    s.stop = 2900.0;
    // riskUsd unset: no sizing advice at all.
    CHECK(DirectionalStrategy::suggestLots(spec, s) == 0.0);
    CHECK(DirectionalStrategy::sizeAdvice(spec, s).empty());
    // Huge risk budget on a wide stop: min lot floor applies.
    spec.riskUsd = 500000.0;
    CHECK(DirectionalStrategy::suggestLots(spec, s) == 0.5);
    // No valid stop -> nothing.
    s.stop = 0.0;
    CHECK(DirectionalStrategy::suggestLots(spec, s) == 0.0);
}