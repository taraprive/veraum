#include "src/engine/directional_strategy.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace hftarb {

DirectionalStrategy::DirectionalStrategy(InstrumentSpec spec) : spec_(std::move(spec)) {}

void DirectionalStrategy::setSignalCallback(std::function<void(const DirectionalSignal&)> cb) {
    cb_ = std::move(cb);
}

double DirectionalStrategy::atrProxy() const {
    if (closes_.size() < 2) return 0.0;
    const auto n = std::min<std::size_t>(closes_.size() - 1,
                                         static_cast<std::size_t>(spec_.atrPeriod));
    double sum = 0.0;
    for (std::size_t i = closes_.size() - n; i < closes_.size(); ++i) {
        sum += std::abs(closes_[i] - closes_[i - 1]);
    }
    return sum / n;
}

void DirectionalStrategy::onTick(Price price, MsTimestamp ts) {
    closes_.push_back(price);
    if (closes_.size() > kMaxHistory) closes_.pop_front();

    const double kFast = 2.0 / (static_cast<double>(spec_.fastEma) + 1.0);
    const double kSlow = 2.0 / (static_cast<double>(spec_.slowEma) + 1.0);
    const double kTrend =
        spec_.trendEma > 0 ? 2.0 / (static_cast<double>(spec_.trendEma) + 1.0) : 0.0;

    if (closes_.size() == 1) {
        emaFast_ = price;
        emaSlow_ = price;
        if (spec_.trendEma > 0) emaTrend_ = price;
        return;
    }

    const double prevFast = emaFast_;
    const double prevSlow = emaSlow_;
    emaFast_ = price * kFast + emaFast_ * (1.0 - kFast);
    emaSlow_ = price * kSlow + emaSlow_ * (1.0 - kSlow);
    if (spec_.trendEma > 0) emaTrend_ = price * kTrend + emaTrend_ * (1.0 - kTrend);

    if (closes_.size() <= static_cast<std::size_t>(spec_.slowEma)) return;

    Side side;
    if (prevFast <= prevSlow && emaFast_ > emaSlow_) {
        side = Side::Buy;
    } else if (prevFast >= prevSlow && emaFast_ < emaSlow_) {
        side = Side::Sell;
    } else {
        return;
    }

    // Higher-timeframe regime gate: only trade with the larger trend once the
    // trend EMA has had enough bars to stabilise away from its start value.
    if (spec_.trendEma > 0 && closes_.size() > static_cast<std::size_t>(spec_.trendEma) &&
        ((side == Side::Buy && price < emaTrend_) ||
         (side == Side::Sell && price > emaTrend_))) {
        return;
    }

    if (ts - lastSignalTs_ < spec_.reentryCooldownMs) return;
    lastSignalTs_ = ts;

    const double vol = atrProxy();
    if (vol <= 0.0) return;

    DirectionalSignal s;
    s.instrument = spec_.instrument;
    s.side = side;
    s.entry = price;
    if (side == Side::Buy) {
        s.stop = price - spec_.stopAtRisk * vol;
        s.target = price + spec_.targetAtRisk * vol;
        s.reason = "fast EMA(" + std::to_string(spec_.fastEma) + ") crossed above slow EMA(" +
                   std::to_string(spec_.slowEma) + ") (uptrend)";
    } else {
        s.stop = price + spec_.stopAtRisk * vol;
        s.target = price - spec_.targetAtRisk * vol;
        s.reason = "fast EMA(" + std::to_string(spec_.fastEma) + ") crossed below slow EMA(" +
                   std::to_string(spec_.slowEma) + ") (downtrend)";
    }
    s.ts = ts;
    if (cb_) cb_(s);
}

std::string DirectionalStrategy::format(const DirectionalSignal& s) {
    std::ostringstream os;
    os << "SIGNAL " << s.instrument << " " << (s.side == Side::Buy ? "LONG" : "SHORT") << "\n";
    os << "entry  $" << s.entry << "\n";
    os << "stop   $" << s.stop << "\n";
    os << "target $" << s.target << "\n";
    const double risk = std::abs(s.entry - s.stop);
    const double reward = std::abs(s.target - s.entry);
    os << "R:R   1:" << (risk > 0.0 ? reward / risk : 0.0) << "\n";
    os << s.reason << "\n";
    os << "[technical estimate; trade with stop + size rules; not guaranteed profit]";
    return os.str();
}

double DirectionalStrategy::suggestLots(const InstrumentSpec& spec,
                                        const DirectionalSignal& s) {
    if (spec.riskUsd <= 0.0) return 0.0;
    if (s.stop <= 0.0 || s.entry <= 0.0) return 0.0;
    const double stopDist = std::abs(s.entry - s.stop);
    if (stopDist <= 0.0) return 0.0;
    const double lossPerLot = stopDist * spec.contractSize;
    if (lossPerLot <= 0.0) return 0.0;
    double lots = spec.riskUsd / lossPerLot;
    if (spec.lotStep > 0.0) lots = std::floor(lots / spec.lotStep) * spec.lotStep;
    lots = std::max(lots, spec.minLot);
    lots = std::min(lots, spec.maxLot);
    return lots;
}

std::string DirectionalStrategy::sizeAdvice(const InstrumentSpec& spec,
                                            const DirectionalSignal& s) {
    const double lots = suggestLots(spec, s);
    if (lots <= 0.0) return "";
    std::ostringstream os;
    os << "LOT   " << lots << "  (risk $" << spec.riskUsd << " at stop $" << s.stop
       << ")  [size estimate from contract; verify on broker]";
    return os.str();
}

namespace {
struct BacktestPosition {
    bool open = false;
    Side side = Side::Buy;
    double entry = 0.0;
    double stop = 0.0;
    double target = 0.0;
};

double meanAbsMove(const std::vector<Price>& closes, std::size_t end, std::size_t window) {
    if (end < 2) return 0.0;
    const std::size_t start = end >= window ? end - window : 1;
    double sum = 0.0;
    for (std::size_t i = start; i < end; ++i) {
        sum += std::abs(closes[i] - closes[i - 1]);
    }
    const auto n = end - start;
    return n > 0 ? sum / n : 0.0;
}
}  // namespace

BacktestStats DirectionalStrategy::backtest(const InstrumentSpec& spec,
                                            const std::vector<Price>& series) {
    BacktestStats st;
    if (series.size() <= static_cast<std::size_t>(spec.slowEma)) return st;

    const double kFast = 2.0 / (static_cast<double>(spec.fastEma) + 1.0);
    const double kSlow = 2.0 / (static_cast<double>(spec.slowEma) + 1.0);
    const double kTrend =
        spec.trendEma > 0 ? 2.0 / (static_cast<double>(spec.trendEma) + 1.0) : 0.0;
    double emaFast = series[0];
    double emaSlow = series[0];
    double emaTrend = spec.trendEma > 0 ? series[0] : 0.0;

    BacktestPosition pos;
    double equity = 0.0;
    double peak = 0.0;
    std::int64_t streak = 0;

    for (std::size_t i = 1; i < series.size(); ++i) {
        const double price = series[i];
        const double prevFast = emaFast;
        const double prevSlow = emaSlow;
        emaFast = price * kFast + emaFast * (1.0 - kFast);
        emaSlow = price * kSlow + emaSlow * (1.0 - kSlow);
        if (spec.trendEma > 0) emaTrend = price * kTrend + emaTrend * (1.0 - kTrend);

        // Exit check comes first so a signal on the same bar uses no stale state.
        if (pos.open) {
            bool exited = false;
            double exitPrice = 0.0;
            if (pos.side == Side::Buy) {
                if (price <= pos.stop) {
                    exitPrice = pos.stop;
                    exited = true;
                    ++st.losses;
                    ++streak;
                } else if (price >= pos.target) {
                    exitPrice = pos.target;
                    exited = true;
                    ++st.wins;
                    streak = 0;
                }
            } else {
                if (price >= pos.stop) {
                    exitPrice = pos.stop;
                    exited = true;
                    ++st.losses;
                    ++streak;
                } else if (price <= pos.target) {
                    exitPrice = pos.target;
                    exited = true;
                    ++st.wins;
                    streak = 0;
                }
            }
            if (exited) {
                const double signedMove =
                    pos.side == Side::Buy ? exitPrice - pos.entry : pos.entry - exitPrice;
                const double pnlBp = signedMove / pos.entry * 1e4;
                equity += pnlBp;
                peak = std::max(peak, equity);
                st.maxDrawdownBp = std::max(st.maxDrawdownBp, peak - equity);
                st.maxConsecutiveLosses = std::max(st.maxConsecutiveLosses, streak);
                pos.open = false;
            }
        }

        if (i <= static_cast<std::size_t>(spec.slowEma)) continue;

        const bool crossLong = prevFast <= prevSlow && emaFast > emaSlow;
        const bool crossShort = prevFast >= prevSlow && emaFast < emaSlow;
        if (pos.open || (!crossLong && !crossShort)) continue;

        // Higher-timeframe regime gate (live engine semantics replicated
        // exactly): the trend EMA needs its warm-up before it can veto.
        bool trendOk = true;
        if (spec.trendEma > 0 && i > static_cast<std::size_t>(spec.trendEma)) {
            if ((crossLong && price < emaTrend) || (crossShort && price > emaTrend))
                trendOk = false;
        }
        if (!trendOk) continue;

        const double vol = meanAbsMove(series, i + 1, spec.atrPeriod);
        if (vol <= 0.0) continue;

        pos.open = true;
        pos.side = crossLong ? Side::Buy : Side::Sell;
        pos.entry = price;
        if (pos.side == Side::Buy) {
            pos.stop = price - spec.stopAtRisk * vol;
            pos.target = price + spec.targetAtRisk * vol;
        } else {
            pos.stop = price + spec.stopAtRisk * vol;
            pos.target = price - spec.targetAtRisk * vol;
        }
    }

    // Close any leftover position at the final print so the stats are complete.
    if (pos.open) {
        double lastPnlBp = (series.back() - pos.entry) / pos.entry * 1e4;
        if (pos.side == Side::Sell) lastPnlBp = -lastPnlBp;
        if (lastPnlBp >= 0.0) {
            ++st.wins;
        } else {
            ++st.losses;
            ++streak;
        }
        equity += lastPnlBp;
        peak = std::max(peak, equity);
        st.maxDrawdownBp = std::max(st.maxDrawdownBp, peak - equity);
        st.maxConsecutiveLosses = std::max(st.maxConsecutiveLosses, streak);
        pos.open = false;
    }

    st.trades += st.wins + st.losses;
    st.winRate = st.trades > 0 ? static_cast<double>(st.wins) / static_cast<double>(st.trades)
                               : 0.0;
    st.totalPnlBp = equity;
    st.avgPnlBp = st.trades > 0 ? equity / static_cast<double>(st.trades) : 0.0;
    return st;
}

}  // namespace hftarb