// HFT Cross-Exchange Arbitrage Bot — scaffold entry point.
//
// Wires the full offline pipeline:
//   MockFeedAdapter(s)  ->  ArbitrageEngine(s)  ->  RiskEngine  ->  PaperExecutor
// with latency monitoring, watchdog health checks and CSV logging of every
// decision. No real orders are ever placed; this is the PAPER execution path.
//
// Build & run (from project root):
//   cmake -B build && cmake --build build --config Release
//   build\Release\hft_arbitrage_bot.exe

#include <atomic>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "src/core/timestamp.h"
#include "src/engine/arbitrage_engine.h"
#include "src/engine/cost_engine.h"
#include "src/engine/directional_strategy.h"
#include "src/engine/liquidity_engine.h"
#include "src/feed/mt5_bar_feed.h"
#include "src/engine/multi_strategy_engine.h"
#include "src/engine/portfolio.h"
#include "src/engine/risk_engine.h"
#include "src/engine/triangular_strategy.h"
#include "src/exchange/binance_adapter.h"
#include "src/exchange/bybit_adapter.h"
#include "src/exchange/mexc_adapter.h"
#include "src/exchange/okx_adapter.h"
#include "src/execution/paper_executor.h"
#include "src/execution/real_executor.h"
#include "src/market/market_state.h"
#include "src/market/mock_adapter.h"
#include "src/market/rest_feed_adapter.h"
#include "src/market/ws_feed_adapter.h"
#include "src/monitor/latency_monitor.h"
#include "src/monitor/watchdog.h"
#include "src/signal/signal_service.h"
#include "src/util/config.h"
#include "src/util/logger.h"

using namespace hftarb;

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int) { g_stop.store(true); }

// --- WebSocket payload parsers (exchange stream -> normalized DepthBook). ---
// Exchange channel ids use their own symbol spelling; mapId is keyed by that
// spelling and returns the canonical "BASE/QUOTE" used everywhere in the bot.

using ChannelMap = std::unordered_map<std::string, std::string>;

// Binance combined-stream payload:
//   {"stream":"pepeusdt@depth10","data":{"lastUpdateId":N,"bids":[["p","q"],...],"asks":[...]}}
std::optional<DepthBook> parseBinanceWs(const std::string& payload, const ChannelMap& mapId) {
    try {
        const auto j = nlohmann::json::parse(payload);
        const std::string stream = j.value("stream", "");
        const auto at = stream.find('@');
        std::string chId = at == std::string::npos ? stream : stream.substr(0, at);
        const auto it = mapId.find(chId);
        if (it == mapId.end()) return std::nullopt;
        const auto& data = j.at("data");
        DepthBook b;
        b.symbol = it->second;
        for (const auto& lvl : data.value("bids", nlohmann::json::array()))
            b.bids.emplace_back(std::stod(lvl[0].get<std::string>()),
                                std::stod(lvl[1].get<std::string>()));
        for (const auto& lvl : data.value("asks", nlohmann::json::array()))
            b.asks.emplace_back(std::stod(lvl[0].get<std::string>()),
                                std::stod(lvl[1].get<std::string>()));
        return b;
    } catch (...) {
        return std::nullopt;
    }
}

// OKX books5 payload (data is a one-element array of top-5 snapshots):
//   {"arg":{"channel":"books5","instId":"PEPE-USDT"},"data":[{"asks":[["px","sz","ln","ord"],...],"bids":[...]}]}
std::optional<DepthBook> parseOkxWs(const std::string& payload, const ChannelMap& mapId) {
    try {
        const auto j = nlohmann::json::parse(payload);
        if (j.contains("event")) return std::nullopt;  // subscribe/unsubscribe ack, not a book
        const auto inst = j.value("arg", nlohmann::json::object()).value("instId", "");
        const auto it = mapId.find(inst);
        if (it == mapId.end()) return std::nullopt;
        const auto& data = j.at("data");
        if (data.empty()) return std::nullopt;
        DepthBook b;
        b.symbol = it->second;
        for (const auto& lvl : data[0].value("bids", nlohmann::json::array()))
            b.bids.emplace_back(std::stod(lvl[0].get<std::string>()),
                                std::stod(lvl[1].get<std::string>()));
        for (const auto& lvl : data[0].value("asks", nlohmann::json::array()))
            b.asks.emplace_back(std::stod(lvl[0].get<std::string>()),
                                std::stod(lvl[1].get<std::string>()));
        return b;
    } catch (...) {
        return std::nullopt;
    }
}

// Build a streaming feed for exchanges whose market stream is JSON over
// WebSocket (binance/okx). Returns nullptr for exchanges that must stay on
// REST polling (e.g. MEXC's protobuf-only streams).
std::shared_ptr<WsFeedAdapter> makeWsFeed(const std::string& ex,
                                          const std::vector<std::string>& syms) {
    if (ex == "binance") {
        auto wsHost = std::string("wss://stream.testnet.binance.vision/stream?streams=");
        ChannelMap mapId;
        std::string streams;
        for (const auto& s : syms) {
            std::string ch = s;
            ch.erase(std::remove(ch.begin(), ch.end(), '/'), ch.end());
            std::transform(ch.begin(), ch.end(), ch.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            mapId[ch] = s;
            if (!streams.empty()) streams += "/";
            streams += ch + "@depth10";
        }
        if (mapId.empty()) return nullptr;
        return std::make_shared<WsFeedAdapter>(
            ex, wsHost + streams, "",
            [mapId](const std::string& p) { return parseBinanceWs(p, mapId); });
    }
    if (ex == "okx") {
        ChannelMap mapId;
        nlohmann::json args = nlohmann::json::array();
        for (const auto& s : syms) {
            std::string inst = s;
            std::replace(inst.begin(), inst.end(), '/', '-');
            mapId[inst] = s;
            args.push_back({{"channel", "books5"}, {"instId", inst}});
        }
        if (mapId.empty()) return nullptr;
        // REST baseUrl is HTTPS, never the WS endpoint — hardcode the correct
        // public market stream URL.
        nlohmann::json sub = {{"op", "subscribe"}, {"args", args}};
        return std::make_shared<WsFeedAdapter>(
            ex, "wss://ws.okx.com:8443/ws/v5/public", sub.dump(),
            [mapId](const std::string& p) { return parseOkxWs(p, mapId); });
    }
    return nullptr;
}

const char* decisionName(Decision d) {
    switch (d) {
        case Decision::Accepted: return "accepted";
        case Decision::Rejected: return "rejected";
        case Decision::StaleData: return "stale_data";
        case Decision::LowLiquidity: return "low_liquidity";
        case Decision::RiskRejected: return "risk_rejected";
        case Decision::LatencyRejected: return "latency_rejected";
        case Decision::SizeRejected: return "size_rejected";
        case Decision::Disconnected: return "disconnected";
        case Decision::Cooldown: return "cooldown";
        case Decision::InsufficientFunds: return "insufficient_funds";
        default: return "none";
    }
}

// CSV recorder: every opportunity (accepted or rejected) is written with its
// full cost breakdown, latency and decision — the raw material for backtesting
// and the ML fill-probability model.
class CsvRecorder {
public:
    explicit CsvRecorder(const std::string& path) : out_(path) {
        std::lock_guard<std::mutex> lock(mutex_);
        out_ << "ts,symbol,buy_exchange,sell_exchange,quantity,buy_price,sell_price,"
                "gross_spread,buy_fee,sell_fee,slippage,network_cost,safety_margin,"
                "net_spread,latency_ms,decision\n";
        out_.flush();
    }

    void record(const Opportunity& opp, double latencyMs) {
        std::lock_guard<std::mutex> lock(mutex_);
        out_ << opp.decisionTs << "," << opp.symbol << "," << opp.buyExchange << ","
             << opp.sellExchange << "," << opp.quantity << "," << opp.cost.buyPrice << ","
             << opp.cost.sellPrice << "," << opp.cost.grossSpread << "," << opp.cost.buyFee
             << "," << opp.cost.sellFee << "," << opp.cost.slippage << ","
             << opp.cost.networkCost << "," << opp.cost.safetyMargin << ","
             << opp.cost.netSpread << "," << latencyMs << "," << decisionName(opp.decision)
             << "\n";
        out_.flush();
    }

private:
    std::ofstream out_;
    std::mutex mutex_;
};

// Capital-aware pair allocation. Every window it measures how many arbitrage
// opportunities each symbol actually produced (opportunities/min) and how much
// USDT the best-funded wallet currently has (a buy leg is financed by a single
// wallet). A pair stays "active" (feeds reach its engine) only while some
// wallet can finance it: required = tradeQty x basePrice x capitalBuffer.
// Ranking by opportunity rate lets cheap, active pairs earn first; as a wallet
// grows, bigger pairs become affordable and are switched on automatically — the
// escalation loop the operator asked for.
class PairAllocator {
public:
    PairAllocator(const AppConfig& cfg, std::shared_ptr<Portfolio> portfolio)
        : cfg_(cfg), portfolio_(std::move(portfolio)) {
        // Start with everything active so opportunities are measured before
        // the first planning pass.
        for (const auto& s : cfg_.symbols) {
            active_.insert(s.symbol);
            eligible_.insert(s.symbol);
        }
    }

    void record(const std::string& symbol) {
        std::lock_guard<std::mutex> lock(mutex_);
        // Only cross-exchange symbols compete for capital; triangle names
        // (same-venue cycles) never enter the ranking map.
        if (!eligible_.count(symbol)) return;
        auto& q = events_[symbol];
        const std::int64_t now = Timestamps::monoMs();
        q.push_back(now);
        prune(q, now - static_cast<std::int64_t>(cfg_.allocator.windowSec) * 1000);
    }

    bool isActive(const std::string& symbol) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_.count(symbol) > 0;
    }

    // Re-plan the active set from current capital + measured rates. Logs the
    // ranking and any activation changes so the escalation is visible.
    void run() {
        struct Ranked {
            double rate;      // opportunities/min (rolling window)
            double need;      // required USDT to finance one round trip
            std::string symbol;
        };
        std::vector<Ranked> ranked;

                auto& log = Logger::instance();
        double availableUsdt = 0.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const std::int64_t now = Timestamps::monoMs();
            const std::int64_t cutoff = now - static_cast<std::int64_t>(cfg_.allocator.windowSec) * 1000;

            for (const auto& s : cfg_.symbols) {
                auto it = events_.find(s.symbol);
                std::int64_t count = 0;
                if (it != events_.end()) {
                    prune(it->second, cutoff);
                    count = static_cast<std::int64_t>(it->second.size());
                }
                ranked.push_back(
                    {count * (60.0 / cfg_.allocator.windowSec),
                     s.tradeQuantity * s.basePrice * cfg_.allocator.capitalBuffer, s.symbol});
            }

            std::sort(ranked.begin(), ranked.end(),
                      [](const Ranked& a, const Ranked& b) {
                          if (a.rate != b.rate) return a.rate > b.rate;
                          return a.need < b.need;
                      });

            availableUsdt = portfolio_->maxQuote();

            // Hysteresis: pairs that are already active may keep trading until
            // capital falls 20% below their need; new pairs only activate once
            // fully affordable. This stops symbols flapping on/off as a single
            // wallet drains and refills across ticks.
            constexpr double deactivateFactor = 0.8;
            std::unordered_set<std::string> next;
            for (const auto& r : ranked) {
                if (active_.count(r.symbol)) {
                    if (r.need * deactivateFactor <= availableUsdt) next.insert(r.symbol);
                }
            }
            for (const auto& r : ranked) {
                if (next.count(r.symbol)) continue;
                if (r.need <= availableUsdt) next.insert(r.symbol);
            }
            if (next.empty() && !ranked.empty()) {
                // Cannot finance any pair yet: keep the cheapest one active so
                // it can trade the moment a window opens.
                const auto cheapest = std::min_element(
                    ranked.begin(), ranked.end(),
                    [](const Ranked& a, const Ranked& b) { return a.need < b.need; });
                next.insert(cheapest->symbol);
            }

            std::string changes;
            for (const auto& s : active_) {
                if (!next.count(s)) changes += " -" + s;
            }
            for (const auto& s : next) {
                if (!active_.count(s)) changes += " +" + s;
            }

            std::string rankLine;
            for (const auto& r : ranked) {
                if (!rankLine.empty()) rankLine += ", ";
                rankLine += r.symbol + ":" + std::to_string(r.rate) + "/min(need$" +
                            std::to_string(r.need) + ")";
            }

            active_ = std::move(next);

            // In a purely directional session (no crypto symbols configured)
            // the allocator has nothing to plan; keep the log silent so it does
            // not bury the directional signal evidence.
            if (!ranked.empty()) {
                log.info("allocator", "capital=$" + std::to_string(availableUsdt) +
                                          " active=[" + join(active_) + "]" +
                                          (changes.empty() ? "" : " changes:" + changes) +
                                          " ranking[" + rankLine + "]");
            }
        }
    }

private:
    static void prune(std::deque<std::int64_t>& q, std::int64_t cutoff) {
        while (!q.empty() && q.front() < cutoff) q.pop_front();
    }

    static std::string join(const std::unordered_set<std::string>& s) {
        std::string out;
        for (const auto& x : s) {
            if (!out.empty()) out += " ";
            out += x;
        }
        return out;
    }

    AppConfig cfg_;
    std::shared_ptr<Portfolio> portfolio_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::deque<std::int64_t>> events_;
    std::unordered_set<std::string> active_;
    std::unordered_set<std::string> eligible_;  // cfg.symbols only
};

}  // namespace

int main(int argc, char** argv) {
    const std::string cfgPath = argc > 1 ? argv[1] : "config/config.json";
    const std::string exPath = argc > 2 ? argv[2] : "config/exchanges.json";
    const std::string credsPath = argc > 3 ? argv[3] : "config/credentials.json";

    try {
        const AppConfig cfg = ConfigLoader::load(cfgPath, exPath, credsPath);

        auto& log = Logger::instance();
        log.setLevel(cfg.logLevel);
        if (!cfg.logFile.empty()) log.setFile(cfg.logFile);

        LatencyMonitor latency;
        Watchdog watchdog(cfg.watchdog);

        bool anyReal = false;
        for (const auto& ex : cfg.exchanges) {
            const auto it = cfg.exchangeAdapters.find(ex);
            if (it != cfg.exchangeAdapters.end() && it->second.adapter != "mock") anyReal = true;
        }
        log.info("main", "starting HFT arbitrage bot" +
                             std::string(anyReal ? " (LIVE testnet mode)" : " (paper mode)"));

        std::function<ExecutionResult(const Opportunity&)> executor;
        std::shared_ptr<RealExecutor> realExecPtr;

        auto portfolio = std::make_shared<Portfolio>();

        // First seed from config (fallback).
        for (const auto& [ex, w] : cfg.wallets) {
            portfolio->setQuote(ex, w.usdt);
            for (const auto& [sym, qty] : w.base) portfolio->setBase(ex, sym, qty);
            log.info("portfolio", "seeded " + portfolio->snapshot(ex));
        }

        // Persistence: if a previous run saved its balances, resume from them
        // instead of the config seed, so PnL carries across sessions.
        if (!cfg.stateFile.empty()) {
            std::ifstream ifs(cfg.stateFile);
            if (ifs.is_open()) {
                try {
                    nlohmann::json st;
                    ifs >> st;
                    int loaded = 0;
                    for (auto it = st.begin(); it != st.end(); ++it) {
                        if (!it.value().is_object() || !it.value().contains("usdt")) continue;
                        portfolio->setQuote(it.key(), it.value()["usdt"].get<double>());
                        const auto& base = it.value().value("base", nlohmann::json::object());
                        for (auto bIt = base.begin(); bIt != base.end(); ++bIt) {
                            portfolio->setBase(it.key(), bIt.key(), bIt.value().get<double>());
                        }
                        ++loaded;
                        log.info("portfolio", "resumed " + portfolio->snapshot(it.key()));
                    }
                    if (loaded > 0) {
                        log.info("main", "resumed portfolio from " + cfg.stateFile);
                    } else {
                        log.warn("main", "state file " + cfg.stateFile +
                                             " has no accounts; using config seed");
                    }
                } catch (const std::exception&) {
                    log.warn("main", "unreadable state file " + cfg.stateFile +
                                         " (ignored, using config seed)");
                }
            }
        }

        // Last known mid price per symbol, captured from the tick path so the
        // rebalancer can settle internal transfers at a fair (no-PnL) price.
        std::mutex midMutex;
        std::unordered_map<std::string, double> lastMid;
        const auto midOf = [&](const std::string& sym) -> double {
            std::lock_guard<std::mutex> lock(midMutex);
            const auto it = lastMid.find(sym);
            return it != lastMid.end() ? it->second : 0.0;
        };

        // Mark-to-market equity: USDT + base inventory priced at the latest mid
        // (config base price before feeds warm up). This is the true measure of
        // profitability — realized PnL alone ignores open inventory.
        const auto priceOf = [&](const std::string& sym) -> double {
            const double m = midOf(sym);
            if (m > 0.0) return m;
            for (const auto& s : cfg.symbols) {
                if (s.symbol == sym) return s.basePrice;
            }
            return 0.0;
        };
        const auto equityNow = [&]() -> double {
            double eq = 0.0;
            for (const auto& [ex, view] : portfolio->snapshotAll()) {
                eq += view.usdt;
                for (const auto& [sym, qty] : view.base) eq += qty * priceOf(sym);
            }
            return eq;
        };
        const double initialEquity = equityNow();

        CsvRecorder recorder("data/opportunities.csv");
        SignalService signals(cfg.signals);
        if (cfg.signals.enabled) {
            log.info("signals", "signal broadcaster enabled channel=" + cfg.signals.channel +
                                    " min_net_spread=" + std::to_string(cfg.signals.minNetSpread));
        } else {
            log.info("signals", "signal broadcaster disabled");
        }
        struct SymbolPnl { std::int64_t trades = 0; double pnl = 0.0; };
        std::unordered_map<std::string, SymbolPnl> pnlBySymbol;
        std::mutex statsMutex;

        // Capital-aware pair allocator: keeps financing the most
        // opportunity-dense pairs and switches on bigger pairs as capital grows.
        auto allocator = std::make_shared<PairAllocator>(cfg, portfolio);
        allocator->run();

        // One cross-exchange strategy per symbol; they share cost/liquidity/
        // risk configs and all route through a single MultiStrategyEngine so
        // future strategies (triangular, funding rate) plug in unchanged.
        auto multistrategy = std::make_shared<MultiStrategyEngine>();
        std::unordered_map<std::string, std::int64_t> lastSkipLog;  // reject-log throttle

        // Shared opportunity callback: every strategy (cross-exchange today,
        // triangular next) emits through this single path so recording,
        // allocation, execution and exposure accounting stay strategy-agnostic.
        const auto oppCb = [&](const Opportunity& opp) {
            const double latencyMs = opp.roundTripLatencyMs;
            allocator->record(opp.symbol);
            recorder.record(opp, latencyMs);
            latency.record(LatencyStage::OrderBookToArbitrage, latencyMs * 1000.0);

            if (opp.decision == Decision::Accepted) {
                // Signal subscribers BEFORE execution: the edge described was
                // detected at decision time, it is not a guaranteed fill.
                signals.emit(opp);
                const ExecutionResult r = executor(opp);
                {
                    std::lock_guard<std::mutex> lock(statsMutex);
                    SymbolPnl& sp = pnlBySymbol[r.symbol];
                    ++sp.trades;
                    sp.pnl += r.realizedPnl;
                }
                log.info("executor", "id=" + r.id + " symbol=" + r.symbol + " status=" + r.status +
                                          " buy=" + std::to_string(r.buyFilled) +
                                          " sell=" + std::to_string(r.sellFilled) +
                                          " pnl=" + std::to_string(r.realizedPnl));
                // True unhedged exposure is the buy/sell fill residual of
                // the round trip, not an optimistic full-quantity credit
                // before we know what actually filled.
                multistrategy->clearExposure(opp.symbol);
                multistrategy->addExposure(opp.symbol, std::max(0.0, r.exposureLeft));
                if (opp.legs.empty()) {
                    // Legacy two-leg round trip: move money across venues.
                    portfolio->applyFill(opp.buyExchange, opp.sellExchange, opp.symbol,
                                         r.buyFilled, r.avgBuyPrice, r.sellFilled,
                                         r.avgSellPrice);
                } else {
                    // Multi-leg cycle stays on one venue; the recorded fills
                    // moved that venue's own inventory.
                    log.info("portfolio",
                             "after tri cycle " + portfolio->snapshot(opp.buyExchange));
                }
                log.info("engine", "ARBITRAGE " + opp.symbol + " buy@" + opp.buyExchange +
                                       " sell@" + opp.sellExchange +
                                       " net=" + std::to_string(opp.cost.netSpread) +
                                       " latency_ms=" + std::to_string(latencyMs));
            } else {
                // Rejections can repeat hundreds of times per second; log
                // each (symbol, decision) at most once per second.
                const std::string key =
                    opp.symbol + "|" + std::string(decisionName(opp.decision));
                const std::int64_t now = Timestamps::monoMs();
                const auto it = lastSkipLog.find(key);
                if (it == lastSkipLog.end() || now - it->second > 1000) {
                    lastSkipLog[key] = now;
                    log.info("engine", "skip " + opp.symbol + " " +
                                           std::string(decisionName(opp.decision)) + " " +
                                           opp.rejectReason);
                }
            }
        };

        for (const auto& s : cfg.symbols) {
            ArbitrageConfig acfg;
            acfg.symbol = s.symbol;
            acfg.exchanges = s.exchanges;
            acfg.tradeQuantity = s.tradeQuantity;

            auto strategy = std::make_shared<ArbitrageEngine>(
                acfg, CostEngine(cfg.cost), LiquidityEngine(cfg.liquidity),
                RiskEngine(cfg.risk));
            for (const auto& ex : cfg.exchanges) {
                const auto it = cfg.feeSchedules.find(ex);
                if (it != cfg.feeSchedules.end()) strategy->setFeeSchedule(ex, it->second);
            }
            strategy->setConnectedFn([&](const std::string& ex) {
                return watchdog.isAlive(ex, Timestamps::monoMs());
            });
            strategy->setPortfolio(portfolio);
            strategy->setOpportunityCallback(oppCb);
            multistrategy->addStrategy(std::move(strategy));
        }

        // Same-venue triangular cycles: one strategy per configured triangle.
        // buy/sell attribution point at the single venue so the recorder and
        // executor treat it as a local 3-leg route (opp.legs drives execution).
        std::unordered_set<std::string> triangleSyms;  // leg pairs bypass the allocator gate
        for (const auto& t : cfg.triangles) {
            if (t.legs.size() != 3) {
                log.warn("main",
                         "skipping triangle '" + t.name + "': must define exactly 3 legs");
                continue;
            }
            auto strategy = std::make_shared<TriangularStrategy>(
                t, CostEngine(cfg.cost), RiskEngine(cfg.risk));
            const auto it = cfg.feeSchedules.find(t.exchange);
            if (it != cfg.feeSchedules.end()) strategy->setFeeSchedule(t.exchange, it->second);
            strategy->setConnectedFn([&](const std::string& ex) {
                return watchdog.isAlive(ex, Timestamps::monoMs());
            });
            strategy->setPortfolio(portfolio);
            strategy->setOpportunityCallback(oppCb);
            multistrategy->addStrategy(std::move(strategy));
            for (const auto& lg : t.legs) triangleSyms.insert(lg.symbol);
            log.info("main", "triangle registered: " + t.name + " @" + t.exchange +
                                 " notional=" + std::to_string(t.notional));
        }

        // Directional engines (gold / Nasdaq / silver etc): each listens for
        // price ticks and emits honest LONG/SHORT signals with a stop and a
        // target. Registered now; the feed wiring (MT5/broker data) is the
        // next integration step.
        std::vector<std::shared_ptr<DirectionalStrategy>> directionalEngines;
        std::unordered_map<std::string, std::shared_ptr<DirectionalStrategy>>
            directionalByName;
        for (const auto& instr : cfg.instruments) {
            auto eng = std::make_shared<DirectionalStrategy>(instr);
            eng->setSignalCallback(
                [&signals, instr](const DirectionalSignal& sig) {
                    std::string text = DirectionalStrategy::format(sig);
                    const std::string size =
                        DirectionalStrategy::sizeAdvice(instr, sig);
                    if (!size.empty()) text += "\n" + size;
                    signals.emitRaw(text, sig.instrument);
                });
            directionalEngines.push_back(eng);
            directionalByName.emplace(instr.instrument, eng);
            log.info("main", "directional engine ready: " + instr.instrument);
        }

        // MT5 live bridge: when ops.mt5_feed points at the JSONL file written
        // by tools/mt5_feed.py, an off-loop thread polls it and feeds every
        // freshly closed bar to the matching directional engine. Symmetry with
        // the crypto feeds: same downstream engine, real broker price source.
        if (!cfg.mt5FeedPath.empty() && !directionalByName.empty()) {
            std::thread mt5Bridge([&logs = log, &byName = directionalByName,
                                   &feedPath = cfg.mt5FeedPath] {
                Mt5BarFeed feed(feedPath);
                feed.setCallback([&logs, &byName](const Mt5Bar& b) {
                    const auto it = byName.find(b.instrument);
                    if (it == byName.end()) return;
                    it->second->onTick(b.close, b.tsMs);
                    logs.info("mt5", b.instrument + " bar " + std::to_string(b.close));
                });
                while (!g_stop.load()) {
                    feed.poll();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                }
                logs.info("mt5", "bridge stopped");
            });
            mt5Bridge.detach();
            log.info("main", "MT5 bridge watching " + cfg.mt5FeedPath);
        }

        // Feeds: one per exchange. Each exchange declares its adapter in
        // exchanges.json — "mock" streams the shared simulated MarketState;
        // "binance" polls real depth over REST and emits the same normalized
        // Ticks. Both plug into an identical downstream pipeline.
        std::vector<std::shared_ptr<IMarketAdapter>> feeds;
        // Random per-run seed by default; config `ops.feed_seed` fixes it for
        // reproducible experiments.
        const unsigned feedSeed = cfg.feedSeed > 0
                                      ? static_cast<unsigned>(cfg.feedSeed)
                                      : static_cast<unsigned>(
                                            std::chrono::steady_clock::now()
                                                .time_since_epoch()
                                                .count());
        std::unordered_map<std::string, SymbolMarketParams> symbolParams;
        for (const auto& s : cfg.symbols) {
            symbolParams[s.symbol] = {s.basePrice, s.liquidityTopQty};
        }
        // Triangle legs may reference cross pairs (ETH/BTC) not in cfg.symbols.
        // Anchor their market-sim mid from the two /USDT base prices when known
        // so mock feeds can synthesize those books too.
        for (const auto& t : cfg.triangles) {
            for (const auto& lg : t.legs) {
                if (symbolParams.count(lg.symbol)) continue;
                const auto slash = lg.symbol.find('/');
                if (slash == std::string::npos) continue;
                const std::string base = lg.symbol.substr(0, slash);
                const std::string quote = lg.symbol.substr(slash + 1);
                double baseP = 0.0, quoteP = 0.0;
                for (const auto& s : cfg.symbols) {
                    if (s.symbol == base + "/USDT") baseP = s.basePrice;
                    if (s.symbol == quote + "/USDT") quoteP = s.basePrice;
                }
                if (baseP > 0.0 && quoteP > 0.0) {
                    symbolParams[lg.symbol] = {baseP / quoteP, 0.1};
                    log.info("main", "triangle cross-pair " + lg.symbol +
                                         " anchored at " + std::to_string(baseP / quoteP));
                }
            }
        }
        bool anyMock = false;
        for (const auto& ex : cfg.exchanges) {
            const auto it = cfg.exchangeAdapters.find(ex);
            if (it == cfg.exchangeAdapters.end() || it->second.adapter == "mock") {
                anyMock = true;
            }
        }

        // Build exchange adapters (shared between feeds and executor).
        std::unordered_map<std::string, std::shared_ptr<IExchangeAdapter>> adapterMap;
        for (const auto& ex : cfg.exchanges) {
            const auto it = cfg.exchangeAdapters.find(ex);
            if (it == cfg.exchangeAdapters.end()) continue;
            const auto& ac = it->second;
            const std::string key = cfg.apiKeys.count(ex) ? cfg.apiKeys.at(ex) : "";
            const std::string secret = cfg.apiSecrets.count(ex) ? cfg.apiSecrets.at(ex) : "";
            if (ac.adapter == "binance") {
                adapterMap[ex] = std::make_shared<BinanceAdapter>(ac.baseUrl, key, secret);
            } else if (ac.adapter == "bybit") {
                adapterMap[ex] = std::make_shared<BybitAdapter>(ac.baseUrl, key, secret);
            } else if (ac.adapter == "mexc") {
                adapterMap[ex] = std::make_shared<MexcAdapter>(ac.baseUrl, key, secret);
            } else if (ac.adapter == "okx") {
                const std::string pass = cfg.apiKeys.count(ex + "_passphrase")
                                             ? cfg.apiKeys.at(ex + "_passphrase") : "";
                adapterMap[ex] = std::make_shared<OkxAdapter>(ac.baseUrl, key, secret, pass, true);
            }
        }

        // Choose executor based on adapter mix.
        if (anyReal) {
            auto realExec = std::make_shared<RealExecutor>(adapterMap, 100);
            realExecPtr = realExec;
            executor = [realExec](const Opportunity& o) { return realExec->execute(o); };
            log.info("main", "real executor active (live testnet orders)");

            // Override portfolio with actual exchange balances.
            // Build set of known base assets from config so we don't import
            // thousands of OKX test tokens.
            std::unordered_set<std::string> knownBases;
            for (const auto& s : cfg.symbols) {
                // s.symbol = "BTC/USDT" -> base = "BTC"
                auto pos = s.symbol.find('/');
                if (pos != std::string::npos) {
                    knownBases.insert(s.symbol.substr(0, pos));
                }
            }
            // Triangle legs can introduce base assets (ETH via ETH/BTC) that
            // no cfg.symbol references; import them too so live balances cover
            // the intermediate legs.
            for (const auto& t : cfg.triangles) {
                for (const auto& lg : t.legs) {
                    auto pos = lg.symbol.find('/');
                    if (pos != std::string::npos) {
                        knownBases.insert(lg.symbol.substr(0, pos));
                    }
                }
            }
            for (const auto& [ex, adapter] : adapterMap) {
                auto bals = adapter->fetchBalances();
                if (!bals) {
                    log.warn("main", "could not fetch balances from " + ex + " (using config seed)");
                    continue;
                }
                double usdt = 0.0;
                std::unordered_map<std::string, double> baseMap;
                for (const auto& [asset, qty] : *bals) {
                    if (asset == "USDT") {
                        usdt = qty;
                    } else if (knownBases.count(asset)) {
                        baseMap[asset] = qty;
                    }
                }
                if (usdt > 0.0 || !baseMap.empty()) {
                    portfolio->setQuote(ex, usdt);
                    for (const auto& [sym, qty] : baseMap) {
                        portfolio->setBase(ex, sym + "/USDT", qty);
                    }
                    log.info("portfolio", "actual " + portfolio->snapshot(ex));
                }
            }
        } else {
            auto paperExec = std::make_shared<PaperExecutor>(cfg.paper, 20260811u);
            executor = [paperExec](const Opportunity& o) { return paperExec->execute(o); };
        }
        auto market = std::make_shared<MarketState>(cfg.market, symbolParams, feedSeed);
        if (anyMock) {
            for (const auto& ex : cfg.exchanges) {
                if (cfg.exchangeSpreadBp.count(ex)) market->setExchangeSpreadBp(ex, cfg.exchangeSpreadBp.at(ex));
            }
        }

        for (const auto& ex : cfg.exchanges) {
            // Subscribe to every symbol this exchange participates in.
            std::vector<std::string> syms;
            for (const auto& s : cfg.symbols) {
                for (const auto& sx : s.exchanges) {
                    if (sx == ex) syms.push_back(s.symbol);
                }
            }
            // Plus the leg pairs of any triangle running on this venue.
            for (const auto& t : cfg.triangles) {
                if (t.exchange != ex) continue;
                for (const auto& lg : t.legs) syms.push_back(lg.symbol);
            }
            // Dedupe: a pair can be both a cross-exchange symbol and a triangle
            // leg on the same venue (dup WS stream ids / REST polls are waste).
            std::sort(syms.begin(), syms.end());
            syms.erase(std::unique(syms.begin(), syms.end()), syms.end());

            const auto onTick = [&](const Tick& tick) {
                // Heartbeat/latency on every received tick, even if the symbol
                // is not currently financed, so the watchdog never sees a
                // healthy feed as dead.
                watchdog.heartbeat(ex, tick.receiveTs);
                latency.record(LatencyStage::ExchangeToLocal, 0.0);
                // Track the latest mid price for the rebalancer.
                if (const auto bid = tick.book.bestBid(); bid && tick.book.bestAsk()) {
                    const double mid = (bid.value() + tick.book.bestAsk().value()) * 0.5;
                    {
                        std::lock_guard<std::mutex> lock(midMutex);
                        lastMid[tick.symbol] = mid;
                    }
                }
                // Only route ticks for pairs the allocator currently finances;
                // triangle leg pairs always flow (they bypass the allocator).
                if (!allocator->isActive(tick.symbol) && !triangleSyms.count(tick.symbol)) return;
                // Feed live books to executor for marketable pricing.
                if (realExecPtr) realExecPtr->updateBook(ex, tick.symbol, tick.book);
                // Multi-strategy dispatcher routes to every strategy owning
                // the symbol (cross-exchange today; triangular/funding later).
                multistrategy->onTick(tick);
            };

            // Real adapters get a feed: exchanges with "ws" in exchanges.json
            // stream depth over WebSocket (JSON); everything else polls REST.
            // Mock exchanges get the simulated MarketState feed.
            const auto it = cfg.exchangeAdapters.find(ex);
            if (it != cfg.exchangeAdapters.end() && it->second.adapter != "mock") {
                std::shared_ptr<IMarketAdapter> feed;
                if (it->second.feed == "ws") {
                    feed = makeWsFeed(ex, syms);
                }
                if (!feed) {
                    auto api = adapterMap.at(ex);
                    feed = std::make_shared<RestFeedAdapter>(
                        ex, syms,
                        [api](const std::string& s, int l) { return api->fetchDepth(s, l); },
                        it->second.updateMs);
                }
                feed->setTickCallback(onTick);
                feeds.push_back(feed);
            } else {
                auto feed = std::make_shared<MockFeedAdapter>(ex, syms, market, 10);
                feed->setTickCallback(onTick);
                feeds.push_back(feed);
            }
        }

        // Start feeds.
        for (auto& f : feeds) {
            if (f->start()) log.info("main", "feed started: " + f->name());
        }

        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);

        // Allocator loop: re-plan the active pairs on the config interval, and
        // sweep the portfolio rebalancer on its own (independent) interval.
        // Also watches drawdown and trips a hard stop if the session loses too
        // much mark-to-market equity (kill switch).
        std::atomic<bool> allocStop{false};
        std::atomic<bool> guardTriggered{false};
        std::thread allocThread([&] {
            std::int64_t lastRebalanceMs = 0;
            std::int64_t lastBalanceSyncMs = 0;
            double peakEquity = initialEquity;
            while (!allocStop.load()) {
                for (int i = 0; i < cfg.allocator.checkIntervalSec * 5 && !allocStop.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
                if (allocStop.load()) break;
                allocator->run();

                // Drawdown guard: stop trading if equity drops by more than the
                // configured percentage from its session high.
                const double eq = equityNow();
                peakEquity = std::max(peakEquity, eq);
                if (cfg.risk.maxSessionDrawdownPct > 0.0 && peakEquity > 0.0) {
                    const double ddPct = (peakEquity - eq) / peakEquity * 100.0;
                    if (ddPct >= cfg.risk.maxSessionDrawdownPct) {
                        if (!guardTriggered.exchange(true)) {
                            log.warn("risk",
                                     "drawdown guard triggered: peak=$" +
                                         std::to_string(peakEquity) + " current=$" +
                                         std::to_string(eq) + " drawdown=" +
                                         std::to_string(ddPct) + "% — stopping trading");
                        }
                        g_stop.store(true);
                    }
                }

                if (cfg.rebalance.intervalSec > 0 && !anyReal) {
                    const std::int64_t nowMs = Timestamps::monoMs();
                    if (nowMs - lastRebalanceMs >= cfg.rebalance.intervalSec * 1000) {
                        lastRebalanceMs = nowMs;
                        const int transfers = portfolio->rebalance(midOf, cfg.rebalance);
                        if (transfers > 0) {
                            std::string wallets;
                            for (const auto& ex : cfg.exchanges) {
                                if (!wallets.empty()) wallets += " | ";
                                wallets += portfolio->snapshot(ex);
                            }
                            log.info("rebalance", "transfers=" + std::to_string(transfers) +
                                                       " " + wallets);
                        }
                    }
                }

                // Periodic balance re-sync + capital recovery:
                // Fetch actual exchange balances every 10s so portfolio stays
                // aligned with reality. When USDT runs low on an exchange,
                // sell a small amount of base to replenish buying power.
                if (anyReal) {
                    const std::int64_t nowMs2 = Timestamps::monoMs();
                    if (nowMs2 - lastBalanceSyncMs >= 10000) {
                        lastBalanceSyncMs = nowMs2;
                        std::unordered_set<std::string> knownBases2;
                        for (const auto& s : cfg.symbols) {
                            auto pos = s.symbol.find('/');
                            if (pos != std::string::npos) {
                                knownBases2.insert(s.symbol.substr(0, pos));
                            }
                        }
                        for (const auto& [ex, adapter] : adapterMap) {
                            auto bals = adapter->fetchBalances();
                            if (!bals) continue;
                            double usdt = 0.0;
                            std::unordered_map<std::string, double> baseMap;
                            for (const auto& [asset, qty] : *bals) {
                                if (asset == "USDT") {
                                    usdt = qty;
                                } else if (knownBases2.count(asset)) {
                                    baseMap[asset] = qty;
                                }
                            }
                            if (usdt > 0.0 || !baseMap.empty()) {
                                portfolio->setQuote(ex, usdt);
                                for (const auto& [sym, qty] : baseMap) {
                                    portfolio->setBase(ex, sym + "/USDT", qty);
                                }
                            }
                        }

                        // Capital recovery: if USDT on any exchange < target,
                        // sell 5% of the largest base position to replenish.
                        constexpr double recoveryTargetUsdt = 80.0;
                        for (const auto& ex : cfg.exchanges) {
                            double usdtNow = portfolio->quote(ex);
                            if (usdtNow >= recoveryTargetUsdt) continue;

                            auto adapterIt = adapterMap.find(ex);
                            if (adapterIt == adapterMap.end()) continue;
                            const auto& adapter = adapterIt->second;

                            // Find largest base position by value.
                            std::string bestSym;
                            double bestVal = 0.0;
                            double bestQty = 0.0;
                            double bestMid = 0.0;
                            for (const auto& s : cfg.symbols) {
                                double qty = portfolio->base(ex, s.symbol);
                                if (qty <= 0.0) continue;
                                double mid = 0.0;
                                {
                                    std::lock_guard<std::mutex> lock(midMutex);
                                    auto it = lastMid.find(s.symbol);
                                    if (it != lastMid.end()) mid = it->second;
                                }
                                if (mid <= 0.0) continue;
                                double val = qty * mid;
                                if (val > bestVal) {
                                    bestVal = val;
                                    bestQty = qty;
                                    bestSym = s.symbol;
                                    bestMid = mid;
                                }
                            }
                            if (bestSym.empty() || bestVal < 5.0) continue;

                            // Sell just enough to reach target, round to step.
                            double needUsdt = recoveryTargetUsdt - usdtNow;
                            double sellQty = std::min(bestQty * 0.10,
                                                      needUsdt / bestMid);
                            if (sellQty * bestMid < 1.0) continue;
                            // Round to Binance LOT_SIZE step per symbol.
                            double lotStep = 0.1;
                            if (bestSym.find("BTC") != std::string::npos || bestSym.find("ETH") != std::string::npos)
                                lotStep = 0.001;
                            else if (bestSym.find("SOL") != std::string::npos || bestSym.find("LINK") != std::string::npos ||
                                     bestSym.find("UNI") != std::string::npos || bestSym.find("LTC") != std::string::npos ||
                                     bestSym.find("INJ") != std::string::npos || bestSym.find("RENDER") != std::string::npos)
                                lotStep = 0.01;
                            else if (bestSym.find("DOGE") != std::string::npos || bestSym.find("ADA") != std::string::npos ||
                                     bestSym.find("FET") != std::string::npos || bestSym.find("SEI") != std::string::npos)
                                lotStep = 1.0;
                            sellQty = std::round(sellQty / lotStep) * lotStep;
                            if (sellQty <= 0.0) continue;

                            // Convert symbol to exchange format (BTCUSDT).
                            std::string exSym;
                            for (char c : bestSym) {
                                if (c != '/')
                                    exSym.push_back(
                                        static_cast<char>(std::toupper(
                                            static_cast<unsigned char>(c))));
                            }

                            double bid = realExecPtr
                                             ? realExecPtr->bestBid(ex, bestSym)
                                             : 0.0;
                            if (bid <= 0.0) {
                                // Fallback: use mid price slightly below.
                                bid = bestMid * 0.9995;
                            }
                            // Round to exchange tick size for Binance PRICE_FILTER.
                            if (ex == "binance") {
                                double tick = 0.01;
                                if (bestMid < 0.1) tick = 0.0001;
                                bid = std::round(bid / tick) * tick;
                            }

                            std::string orderId, err;
                            bool ok = adapter->placeLimitOrder(
                                exSym, "SELL", sellQty, bid, orderId, err);
                            if (ok) {
                                log.info("recovery",
                                         "sell " + std::to_string(sellQty) + " " +
                                             bestSym + " on " + ex + " @$" +
                                             std::to_string(bid) + " to recover $" +
                                             std::to_string(sellQty * bid) +
                                             " USDT (had $" +
                                             std::to_string(usdtNow) + ")");
                            } else {
                                log.warn("recovery",
                                         "sell failed on " + ex + ": " + err);
                            }
                        }
                    }
                }
            }
        });

        const std::int64_t startMs = Timestamps::monoMs();
        const std::int64_t durationMs = cfg.runSeconds > 0 ? cfg.runSeconds * 1000 : 0;
        while (!g_stop.load()) {
            if (durationMs > 0 && Timestamps::monoMs() - startMs > durationMs) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            // Watchdog sweep: log dead feeds (mock feeds never die, but the
            // hook exists for real adapters).
            for (const auto& ex : cfg.exchanges) {
                const std::string dead = watchdog.checkAndReconnect(ex, Timestamps::monoMs());
                if (!dead.empty()) log.warn("watchdog", "reconnect attempted for " + dead);
            }
        }

        // Shutdown + summary.
        allocStop.store(true);
        allocator->run();  // final plan so the shutdown log shows the last state
        if (allocThread.joinable()) allocThread.join();
        for (auto& f : feeds) f->stop();
        signals.stop();

        log.info("main", "shutdown. latency samples=" +
                             std::to_string(latency.totalSamples()));
        double totalPnl = 0.0;
        std::int64_t totalTrades = 0;
        {
            std::lock_guard<std::mutex> lock(statsMutex);
            for (const auto& [sym, sp] : pnlBySymbol) {
                totalPnl += sp.pnl;
                totalTrades += sp.trades;
                log.info("pnl", "symbol=" + sym + " trades=" + std::to_string(sp.trades) +
                                    " realized_pnl_usd=" + std::to_string(sp.pnl));
            }
        }
        log.info("pnl", "TOTAL trades=" + std::to_string(totalTrades) +
                            " realized_pnl_usd=" + std::to_string(totalPnl));
        for (const auto& ex : cfg.exchanges) {
            log.info("portfolio", "final " + portfolio->snapshot(ex));
        }
        const double finalEquity = equityNow();
        log.info("equity",
                 "start=$" + std::to_string(initialEquity) + " final=$" +
                     std::to_string(finalEquity) + " delta=$" +
                     std::to_string(finalEquity - initialEquity));
        if (!cfg.stateFile.empty()) {
            try {
                const std::filesystem::path dir =
                    std::filesystem::path(cfg.stateFile).parent_path();
                if (!dir.empty()) std::filesystem::create_directories(dir);
                nlohmann::json st = nlohmann::json::object();
                for (const auto& [ex, view] : portfolio->snapshotAll()) {
                    st[ex]["usdt"] = view.usdt;
                    for (const auto& [sym, qty] : view.base) st[ex]["base"][sym] = qty;
                }
                std::ofstream ofs(cfg.stateFile);
                if (ofs) {
                    ofs << st.dump(2) << "\n";
                    log.info("main", "portfolio state saved to " + cfg.stateFile);
                } else {
                    log.warn("main", "cannot save portfolio state to " + cfg.stateFile);
                }
            } catch (const std::exception& e) {
                log.warn("main", "cannot save portfolio state: " + std::string(e.what()));
            }
        }
        for (int i = 0; i < static_cast<int>(LatencyStage::Count); ++i) {
            const auto s = latency.summary(static_cast<LatencyStage>(i));
            log.info("latency", std::string(stageName(static_cast<LatencyStage>(i))) +
                                    " avg_us=" + std::to_string(s.avgUs) +
                                    " p95_us=" + std::to_string(s.p95Us) +
                                    " p99_us=" + std::to_string(s.p99Us) +
                                    " max_us=" + std::to_string(s.maxUs) +
                                    " n=" + std::to_string(s.count));
        }

        return 0;
    } catch (const std::exception& ex) {
        Logger::instance().error("main", std::string("fatal: ") + ex.what());
        std::cerr << "fatal: " << ex.what() << "\n";
        return 1;
    }
}
