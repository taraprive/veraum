// hft_backtest: honest backtest of the directional LONG/SHORT model on a real
// OHLC series, with a walk-forward parameter scan.
//
// Usage:
//   hft_backtest <instrument> <csv> [fast_ema] [slow_ema] [atr_period]
//                [stop_at_risk] [target_at_risk] [trend_ema]
//   hft_backtest --scan <instrument> <csv>               # walk-forward sweep
//   hft_backtest --scan <instrument> <csv> <trend_ema>   # sweep with a fixed trend EMA
//
// CSV may be "date,open,high,low,close" or "date,close"; the last numeric
// column of each row is treated as the close.
//
// Walk-forward: the series is split 60/40; every candidate parameter set is
// ranked only on the OUT-OF-SAMPLE second half, so a good rank reflects how
// the model behaves on data it did not see, not curve-fit noise.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "src/engine/directional_strategy.h"

using namespace hftarb;

namespace {

struct Series {
    std::string instrument;
    std::string file;
    std::vector<Price> closes;
    std::vector<std::string> dates;
};

bool load(const char* instrument, const char* path, Series& out) {
    out.instrument = instrument;
    out.file = path;
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::vector<std::string> cols;
        std::istringstream ls(line);
        std::string tok;
        while (std::getline(ls, tok, ',')) {
            if (!tok.empty()) cols.push_back(tok);
        }
        if (cols.empty()) continue;
        char* end = nullptr;
        const double val = std::strtod(cols.back().c_str(), &end);
        if (end == cols.back().c_str()) continue;
        out.closes.push_back(val);
        out.dates.push_back(cols.front());
    }
    return out.closes.size() >= 60;
}

void printStats(const char* tag, const BacktestStats& st) {
    std::printf("%-22s trades=%3lld  wr=%5.1f%%  pnl=%9.1fbp  avg=%7.2fbp  "
                "dd=%7.1fbp  maxL=%lld\n",
                tag, (long long)st.trades, st.winRate * 100.0, st.totalPnlBp,
                st.avgPnlBp, st.maxDrawdownBp, (long long)st.maxConsecutiveLosses);
}

// Per-trade friction (spread/slippage/commission) deducted from every trade so
// the reported PnL is net, not gross. 1-2bp is typical for retail CFDs.
double g_costBp = 0.0;

BacktestStats net(const BacktestStats& st) {
    BacktestStats out = st;
    out.totalPnlBp -= out.trades * g_costBp;
    out.avgPnlBp = out.trades > 0 ? out.totalPnlBp / out.trades : 0.0;
    return out;
}

BacktestStats run(const InstrumentSpec& s, const std::vector<Price>& closes) {
    return DirectionalStrategy::backtest(s, closes);
}

int single(const char* instrument, const char* path, const InstrumentSpec& s) {
    Series cs;
    if (!load(instrument, path, cs)) {
        std::fprintf(stderr, "error: cannot open %s (or fewer than 60 rows)\n", path);
        return 2;
    }
    std::printf("== %s : %zu bars (%s .. %s)\n", cs.instrument.c_str(), cs.closes.size(),
                cs.dates[0].c_str(), cs.dates.back().c_str());
    std::printf("   params fast=%d slow=%d atr=%d stopRisk=%.1f targetRisk=%.1f "
                "trendEma=%d costBp=%.1f/trade\n",
                s.fastEma, s.slowEma, s.atrPeriod, s.stopAtRisk, s.targetAtRisk,
                s.trendEma, g_costBp);
    const BacktestStats gross = run(s, cs.closes);
    printStats("gross (no cost)", gross);
    printStats("net (-cost)", net(gross));

    DirectionalStrategy live(s);
    int emitted = 0;
    live.setSignalCallback([&](const DirectionalSignal& sig) {
        if (emitted < 3) {
            std::printf("---- sample signal ----\n%s\n",
                        DirectionalStrategy::format(sig).c_str());
        }
        ++emitted;
    });
    for (std::size_t i = 0; i < cs.closes.size(); ++i)
        live.onTick(cs.closes[i], static_cast<std::int64_t>(i) * 86400000);
    std::printf("   live-engine signals in this window: %d\n", emitted);
    return 0;
}

int scan(const char* instrument, const char* path, int fixedTrend) {
    Series cs;
    if (!load(instrument, path, cs)) {
        std::fprintf(stderr, "error: cannot open %s (or fewer than 60 rows)\n", path);
        return 2;
    }
    const std::size_t split = cs.closes.size() * 60 / 100;
    const std::vector<Price> train(cs.closes.begin(), cs.closes.begin() + split);
    const std::vector<Price> test(cs.closes.begin() + split, cs.closes.end());

    struct Row {
        InstrumentSpec spec;
        BacktestStats trainSt;
        BacktestStats testSt;
    };
    std::vector<Row> rows;
    const int stopsN[] = {2, 3, 4};
    for (const int stopN : stopsN) {
        for (const int fast : {5, 8, 10, 12}) {
            for (const int slow : {21, 34, 55}) {
                if (fast >= slow) continue;
                for (const int atr : {10, 14}) {
                    InstrumentSpec s;
                    s.instrument = instrument;
                    s.fastEma = fast;
                    s.slowEma = slow;
                    s.atrPeriod = atr;
                    s.stopAtRisk = static_cast<double>(stopN);
                    s.targetAtRisk = static_cast<double>(stopN) * 1.5;  // R:R 1:1.5
                    s.trendEma = fixedTrend;
                    s.reentryCooldownMs = 0;
                    Row r;
                    r.spec = s;
                    r.trainSt = net(run(s, train));
                    r.testSt = net(run(s, test));
                    rows.push_back(r);
                }
            }
        }
    }

    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.testSt.trades < 5) return false;         // too few trades: never rank high
        if (b.testSt.trades < 5) return true;
        const double aScore = a.testSt.totalPnlBp / std::max(1.0, (double)a.testSt.trades);
        const double bScore = b.testSt.totalPnlBp / std::max(1.0, (double)b.testSt.trades);
        return aScore > bScore;
    });

    std::printf("== %s : %zu bars, walk-forward %zu train / %zu test\n",
                cs.instrument.c_str(), cs.closes.size(), train.size(), test.size());
    std::printf("   rank  fast slow atr  stop   target  trend |  TEST   wr  pnl(avgbp) |  "
                "TRAIN  wr  pnl(avgbp)      (metrics net of costBp=%.1f/trade)\n",
                g_costBp);
    const int top = std::min<int>(10, (int)rows.size());
    for (int k = 0; k < top; ++k) {
        const Row& r = rows[k];
        std::printf("   %-4d %4d %4d %3d %4.0f %7.1f %5d | %3lld %5.1f%% %7.2f | "
                    "%3lld %5.1f%% %7.2f\n",
                    k + 1, r.spec.fastEma, r.spec.slowEma, r.spec.atrPeriod,
                    r.spec.stopAtRisk, r.spec.targetAtRisk, r.spec.trendEma,
                    (long long)r.testSt.trades, r.testSt.winRate * 100.0,
                    r.testSt.totalPnlBp / std::max(1.0, (double)r.testSt.trades),
                    (long long)r.trainSt.trades, r.trainSt.winRate * 100.0,
                    r.trainSt.totalPnlBp / std::max(1.0, (double)r.trainSt.trades));
    }
    std::printf(
        "   ranking metric = avg-bp-per-trade on the TEST (out-of-sample) half; "
        "configs with <5 test trades are de-prioritised.\n");
    std::printf("   [honest reading: one walk-forward split is one sample; the model "
                "still needs live validation before any money follows.]\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // Cost basis points per trade (spread+fees); default 2bp retail CFD.
    g_costBp = 2.0;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--costbp" && i + 1 < argc) {
            g_costBp = std::atof(argv[i + 1]);
            for (int j = i; j < argc - 1; ++j) argv[j] = argv[j + 1];
            --argc;
            break;
        }
    }
    if (argc >= 3 && std::string(argv[1]) == "--scan") {
        const int fixedTrend = argc > 4 ? std::atoi(argv[4]) : 0;
        return scan(argv[2], argv[3], fixedTrend);
    }
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: hft_backtest <instrument> <csv> [fast] [slow] [atr] "
                     "[stop] [target] [trend]\n"
                     "       hft_backtest --scan <instrument> <csv> [trend_ema]\n");
        return 2;
    }
    InstrumentSpec s;
    s.instrument = argv[1];
    if (argc > 3) s.fastEma = std::atoi(argv[3]);
    if (argc > 4) s.slowEma = std::atoi(argv[4]);
    if (argc > 5) s.atrPeriod = std::atoi(argv[5]);
    if (argc > 6) s.stopAtRisk = std::atof(argv[6]);
    if (argc > 7) s.targetAtRisk = std::atof(argv[7]);
    if (argc > 8) s.trendEma = std::atoi(argv[8]);
    if (s.fastEma >= s.slowEma) {
        std::fprintf(stderr, "error: fast_ema must be < slow_ema\n");
        return 2;
    }
    return single(argv[1], argv[2], s);
}