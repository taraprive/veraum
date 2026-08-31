#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "src/core/types.h"

namespace hftarb {

// A transparent directional signal model for instruments like XAUUSD, NDX100,
// XAGUSD. Every emitted signal carries an entry, a volatility-based stop and a
// target so it can be traded with a risk plan — an estimate, never a guarantee.
struct InstrumentSpec {
    std::string instrument;
    double anchor = 2000.0;   // starting price for the sim / display
    double volBp = 2.0;       // per-tick volatility (bp) used when no history yet
    int fastEma = 10;
    int slowEma = 30;
    int atrPeriod = 14;       // volatility window (proxy for ATR)
    double stopAtRisk = 2.0;  // stop distance = N x avg price move
    double targetAtRisk = 3.0;
    // Higher-timeframe regime gate: entries are allowed only when price trades
    // on the same side of this trend EMA as the signal (LONG above it, SHORT
    // below it). 0 disables the filter. This is the model's main anti-whipsaw
    // control; default 50 works on 1h+ frames but should be sized to the bar
    // count per day (e.g. ~200 on 5m gold = one day of trend memory).
    int trendEma = 50;
    // Position sizing guidance (contract definitions come from the broker's
    // symbol info; riskUsd=0 switches the LOT advice off).
    double contractSize = 100.0;  // units per 1.00 lot (XAUUSD.m=100, XAGUSD.m=5000)
    double lotStep = 0.01;
    double minLot = 0.01;
    double maxLot = 100.0;
    double riskUsd = 0.0;         // 0 = no sizing advice
    std::int64_t reentryCooldownMs = 60000;
};

struct DirectionalSignal {
    std::string instrument;
    Side side = Side::Buy;  // Buy = LONG, Sell = SHORT
    double entry = 0.0;
    double stop = 0.0;
    double target = 0.0;
    std::int64_t ts = 0;
    std::string reason;
};

struct BacktestStats {
    std::int64_t trades = 0;
    std::int64_t wins = 0;
    std::int64_t losses = 0;
    double winRate = 0.0;        // 0..1
    double totalPnlBp = 0.0;     // cumulative net edge in basis points
    double maxDrawdownBp = 0.0;  // peak-to-trough of the equity curve
    std::int64_t maxConsecutiveLosses = 0;
    double avgPnlBp = 0.0;
};

// Feeds one price per tick; fires a callback on every confirmed EMA cross
// (LONG when the fast EMA crosses above the slow one, SHORT on the inverse),
// throttled per instrument by reentryCooldownMs.
class DirectionalStrategy {
public:
    explicit DirectionalStrategy(InstrumentSpec spec);

    void onTick(Price price, MsTimestamp ts);
    void setSignalCallback(std::function<void(const DirectionalSignal&)> cb);

    // Human-readable delivery message. States clearly this is an estimate.
    static std::string format(const DirectionalSignal& s);

    // Position sizing: lots sized so the stop distance risks `spec.riskUsd`
    // dollars (units-per-lot from the broker's contract file). Uses the exact
    // broker step/min/max; rounds DOWN to stay under the risk budget. Returns
    // 0.0 when sizing is off (riskUsd<=0) or the signal has no valid stop.
    static double suggestLots(const InstrumentSpec& spec,
                              const DirectionalSignal& s);

    // "LOT ..." delivery line for human channels (empty when sizing is off).
    static std::string sizeAdvice(const InstrumentSpec& spec,
                                  const DirectionalSignal& s);

    // Honest offline evaluation of the model on a historical price series.
    static BacktestStats backtest(const InstrumentSpec& spec,
                                  const std::vector<Price>& series);

private:
    double atrProxy() const;

    InstrumentSpec spec_;
    std::function<void(const DirectionalSignal&)> cb_;
    std::deque<Price> closes_;
    double emaFast_ = 0.0;
    double emaSlow_ = 0.0;
    double emaTrend_ = 0.0;
    MsTimestamp lastSignalTs_ = 0;
    static constexpr std::size_t kMaxHistory = 512;
};

}  // namespace hftarb