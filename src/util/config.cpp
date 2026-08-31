#include "src/util/config.h"

#include <fstream>
#include <stdexcept>

namespace hftarb {

double ConfigLoader::getDouble(const nlohmann::json& j, const std::string& key, double def) {
    auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<double>() : def;
}

int ConfigLoader::getInt(const nlohmann::json& j, const std::string& key, int def) {
    auto it = j.find(key);
    return (it != j.end() && it->is_number_integer()) ? it->get<int>() : def;
}

std::string ConfigLoader::getString(const nlohmann::json& j, const std::string& key,
                                    const std::string& def) {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : def;
}

AppConfig ConfigLoader::load(const std::string& configPath, const std::string& exchangesPath,
                             const std::string& credentialsPath) {
    AppConfig cfg;

    std::ifstream cf(configPath);
    if (!cf.is_open()) throw std::runtime_error("cannot open config file: " + configPath);
    nlohmann::json c;
    cf >> c;

    std::ifstream ef(exchangesPath);
    if (!ef.is_open()) throw std::runtime_error("cannot open exchanges file: " + exchangesPath);
    nlohmann::json e;
    ef >> e;

    // Strategy / thresholds.
    const auto& strat = c.value("strategy", nlohmann::json::object());
    cfg.cost.safetyMargin = getDouble(strat, "safety_margin", 0.0005);
    cfg.cost.minNetSpread = getDouble(strat, "min_net_spread", 0.002);
    cfg.cost.networkCostForSpot = getDouble(strat, "network_cost_spot", 0.0);

    // Allocator: capital-aware pair planning.
    const auto& alc = c.value("allocator", nlohmann::json::object());
    cfg.allocator.checkIntervalSec = getInt(alc, "check_interval_sec", 15);
    cfg.allocator.windowSec = getInt(alc, "window_sec", 60);
    cfg.allocator.capitalBuffer = getDouble(alc, "capital_buffer", 2.5);
    cfg.allocator.minOppRate = getDouble(alc, "min_opp_rate", 0.0);

    // Portfolio rebalancing: periodically re-distribute inventory between
    // accounts so capital never strands on a single venue.
    const auto& rb = c.value("rebalance", nlohmann::json::object());
    cfg.rebalance.intervalSec = getInt(rb, "interval_sec", 30);
    cfg.rebalance.thresholdUsd = getDouble(rb, "threshold_usd", 10.0);
    cfg.rebalance.maxSweepUsd = getDouble(rb, "max_sweep_usd", 25.0);
    cfg.rebalance.transferCostBp = getDouble(rb, "transfer_cost_bp", 5.0);

    // Market simulation: shared cross-exchange dynamics.
    const auto& mkt = c.value("market", nlohmann::json::object());
    cfg.market.volBp = getDouble(mkt, "vol_bp", 0.6);
    cfg.market.driftBp = getDouble(mkt, "drift_bp", 0.0);
    cfg.market.basisVolBp = getDouble(mkt, "basis_vol_bp", 0.9);
    cfg.market.basisReversion = getDouble(mkt, "basis_reversion", 0.08);
    cfg.market.spreadMinBp = getDouble(mkt, "spread_min_bp", 1.0);
    cfg.market.spreadMaxBp = getDouble(mkt, "spread_max_bp", 4.0);
    cfg.market.spreadWalkBp = getDouble(mkt, "spread_walk_bp", 0.25);
    cfg.market.burstProbability = getDouble(mkt, "burst_probability", 0.015);
    cfg.market.burstBp = getDouble(mkt, "burst_bp", 60.0);
    cfg.market.burstMinTicks = getInt(mkt, "burst_min_ticks", 1);
    cfg.market.burstMaxTicks = getInt(mkt, "burst_max_ticks", 4);

    const auto& liq = c.value("liquidity", nlohmann::json::object());
    cfg.liquidity.minAvailableRatio = getDouble(liq, "min_available_ratio", 1.0);
    cfg.liquidity.maxAcceptableSlippage = getDouble(liq, "max_slippage", 0.001);

    const auto& risk = c.value("risk", nlohmann::json::object());
    cfg.risk.maxDataAgeMs = getDouble(risk, "max_data_age_ms", 500.0);
    cfg.risk.maxRoundTripLatencyMs = getDouble(risk, "max_round_trip_latency_ms", 100.0);
    cfg.risk.minTradeQty = getDouble(risk, "min_trade_qty", 0.001);
    cfg.risk.maxTradeQty = getDouble(risk, "max_trade_qty", 2.0);
    cfg.risk.maxOpenExposure = getDouble(risk, "max_open_exposure", 0.5);
    cfg.risk.cooldownMs = getDouble(risk, "cooldown_ms", 250.0);
    cfg.risk.maxTradesPerMinute = getInt(risk, "max_trades_per_minute", 20);
    cfg.risk.maxSessionDrawdownPct = getDouble(risk, "max_session_drawdown_pct", 0.0);

    const auto& paper = c.value("paper", nlohmann::json::object());
    cfg.paper.fillRateBuy = getDouble(paper, "fill_rate_buy", 0.97);
    cfg.paper.fillRateSell = getDouble(paper, "fill_rate_sell", 0.93);
    cfg.paper.partialProbability = getDouble(paper, "partial_probability", 0.05);
    cfg.paper.rejectProbability = getDouble(paper, "reject_probability", 0.01);
    cfg.paper.delayMs = getDouble(paper, "delay_ms", 20.0);
    cfg.paper.priceMoveBp = getDouble(paper, "price_move_bp", 0.5);
    cfg.paper.adverseEdgeFrac = getDouble(paper, "adverse_edge_frac", 0.35);

    const auto& wd = c.value("watchdog", nlohmann::json::object());
    cfg.watchdog.heartbeatTimeoutMs = getInt(wd, "heartbeat_timeout_ms", 5000);
    cfg.watchdog.checkIntervalMs = getInt(wd, "check_interval_ms", 1000);
    cfg.watchdog.maxReconnectAttempts = getInt(wd, "max_reconnect_attempts", 5);

    const auto& ops = c.value("ops", nlohmann::json::object());
    cfg.logFile = ops.value("log_file", "logs/bot.log");
    cfg.stateFile = ops.value("state_file", "data/balances.json");
    const std::string level = ops.value("log_level", "info");
    if (level == "debug") cfg.logLevel = LogLevel::Debug;
    else if (level == "warn") cfg.logLevel = LogLevel::Warn;
    else if (level == "error") cfg.logLevel = LogLevel::Error;
    cfg.runSeconds = ops.value("run_seconds", 0);
    cfg.feedSeed = ops.value("feed_seed", 0);
    cfg.mt5FeedPath = getString(ops, "mt5_feed", "");

    // Signal broadcaster: notify subscribers (Telegram) about accepted
    // opportunities, throttled per symbol. Message states clearly the edge is
    // estimated before execution — never a guaranteed profit.
    const auto& sig = c.value("signals", nlohmann::json::object());
    cfg.signals.enabled = sig.value("enabled", false);
    cfg.signals.channel = getString(sig, "channel", "telegram");
    cfg.signals.token = getString(sig, "token", "");
    cfg.signals.chatId = getString(sig, "chat_id", "");
    cfg.signals.minNetSpread = getDouble(sig, "min_net_spread", 0.001);
    cfg.signals.intervalMs = getInt(sig, "interval_ms", 15000);
    cfg.signals.jsonlPath = getString(sig, "jsonl", "");
    for (const auto& s : sig.value("symbols", nlohmann::json::array())) {
        cfg.signals.symbols.push_back(s.get<std::string>());
    }

    // Exchanges: names + fee schedules + adapter wiring.
    for (const auto& ex : e["exchanges"]) {
        const std::string name = ex.value("name", "");
        cfg.exchanges.push_back(name);

        AppConfig::ExchangeAdapterConfig ac;
        ac.adapter = getString(ex, "adapter", "mock");
        ac.baseUrl = getString(ex, "base_url", "");
        ac.updateMs = getInt(ex, "update_ms", 50);
        ac.feed = getString(ex, "feed", "rest");
        cfg.exchangeAdapters[name] = std::move(ac);

        FeeSchedule fees;
        fees.takerBuyFee = getDouble(ex, "taker_buy_fee", 0.001);
        fees.takerSellFee = getDouble(ex, "taker_sell_fee", 0.001);
        fees.networkCost = getDouble(ex, "network_cost", 0.0);
        fees.networkCostPerUnit = getDouble(ex, "network_cost_per_unit", 0.0);
        cfg.feeSchedules[name] = fees;
        cfg.exchangeSpreadBp[name] = getDouble(ex, "spread_bp", 2.0);
    }

    // Credentials: optional file, kept out of exchanges.json so API keys never
    // sit next to non-secret wiring. Absent file -> empty keys (public-only).
    {
        std::ifstream kf(credentialsPath);
        if (kf.is_open()) {
            try {
                nlohmann::json k;
                kf >> k;
                for (auto it = k.begin(); it != k.end(); ++it) {
                    cfg.apiKeys[it.key()] = getString(it.value(), "api_key", "");
                    cfg.apiSecrets[it.key()] = getString(it.value(), "api_secret", "");
                    const std::string pass = getString(it.value(), "passphrase", "");
                    if (!pass.empty()) cfg.apiKeys[it.key() + "_passphrase"] = pass;
                }
            } catch (const std::exception&) {
                throw std::runtime_error("unreadable credentials file: " + credentialsPath);
            }
        }
    }

    // Symbols: each declares which exchanges it is watched on, the trade qty,
    // and the market-sim anchor (starting price + top-of-book liquidity).
    for (const auto& s : c["symbols"]) {
        AppConfig::SymbolConfig sc;
        sc.symbol = s.value("name", "");
        sc.tradeQuantity = getDouble(s, "trade_quantity", 0.1);
        sc.basePrice = getDouble(s, "base_price", 100000.0);
        sc.liquidityTopQty = getDouble(s, "liquidity_top_qty", 0.1);
        for (const auto& x : s.value("exchanges", nlohmann::json::array())) {
            sc.exchanges.push_back(x.get<std::string>());
        }
        cfg.symbols.push_back(std::move(sc));
    }

    // Triangular arbitrage: same-venue 3-leg cycles. Each entry names its
    // exchange, the asset it starts from, the entry size, and the three legs
    // in execution order with buy/sell direction and the pair symbol.
    //   "triangles": [ { "name":"USDT_BTC_ETH", "exchange":"binance",
    //                    "start_asset":"USDT", "notional":500,
    //                    "legs":[ {"symbol":"BTC/USDT","side":"buy"},
    //                             {"symbol":"ETH/BTC","side":"buy"},
    //                             {"symbol":"ETH/USDT","side":"sell"} ] } ]
    const auto& triangles = c.value("triangles", nlohmann::json::array());
    for (const auto& t : triangles) {
        AppConfig::TriangleConfig tc;
        tc.name = getString(t, "name", "");
        tc.exchange = getString(t, "exchange", "");
        tc.startAsset = getString(t, "start_asset", "USDT");
        tc.notional = getDouble(t, "notional", 500.0);
        for (const auto& lg : t.value("legs", nlohmann::json::array())) {
            AppConfig::TriangleConfig::Leg leg;
            leg.symbol = getString(lg, "symbol", "");
            const std::string side = getString(lg, "side", "buy");
            leg.side = (side == "sell") ? Side::Sell : Side::Buy;
            tc.legs.push_back(std::move(leg));
        }
        cfg.triangles.push_back(std::move(tc));
    }

// Directional signal engines: instruments like gold / Nasdaq / silver,
    // each with its own EMA-cross parameters. Parameter choices below were
    // picked by a walk-forward scan on 5 years of real MT5 H1 history (see
    // tools/backtest_directional.cpp --scan): they are net-positive after a
    // 2bp per-trade cost but are an engineering estimate — not a promise.
    //
    // Suggested tuned sets (H1, R:R 1:1.5, cost-corrected):
    //   XAUUSD: fast 12 / slow 21 / atr 10 / stop 4 / target 6 / trend 50
    //   XAGUSD: fast 12 / slow 55 / atr 10 / stop 4 / target 6 / trend 50
    //   NDX100: fast 12 / slow 21 / atr 10 / stop 4 / target 6 / trend 50
    //
    //   "instruments": [ { "instrument":"XAUUSD", "anchor":2140.0, "vol_bp":2.0,
    //                       "fast_ema":10, "slow_ema":30, "atr_period":14,
    //                       "stop_at_risk":2.0, "target_at_risk":3.0,
    //                       "trend_ema":50, "cooldown_ms":60000 } ]
    const auto instruments = c.value("instruments", nlohmann::json::array());
    for (const auto& ins : instruments) {
        InstrumentSpec is;
        is.instrument = getString(ins, "instrument", "");
        is.anchor = getDouble(ins, "anchor", 2000.0);
        is.volBp = getDouble(ins, "vol_bp", 2.0);
        is.fastEma = getInt(ins, "fast_ema", 10);
        is.slowEma = getInt(ins, "slow_ema", 30);
        is.atrPeriod = getInt(ins, "atr_period", 14);
        is.stopAtRisk = getDouble(ins, "stop_at_risk", 2.0);
        is.targetAtRisk = getDouble(ins, "target_at_risk", 3.0);
        is.trendEma = getInt(ins, "trend_ema", 50);
        is.contractSize = getDouble(ins, "contract_size", 100.0);
        is.lotStep = getDouble(ins, "lot_step", 0.01);
        is.minLot = getDouble(ins, "min_lot", 0.01);
        is.maxLot = getDouble(ins, "max_lot", 100.0);
        is.riskUsd = getDouble(ins, "risk_usd", 0.0);
        is.reentryCooldownMs = getInt(ins, "cooldown_ms", 60000);
        if (!is.instrument.empty()) cfg.instruments.push_back(std::move(is));
    }

    // Paper portfolio: per-exchange starting balances.
    // "wallets": { "exchange_a": { "usdt": 50.0, "base": { "BTC/USDT": 0.001 } } }
    const auto& wallets = c.value("wallets", nlohmann::json::object());
    for (auto it = wallets.begin(); it != wallets.end(); ++it) {
        Wallet w;
        w.usdt = getDouble(it.value(), "usdt", 0.0);
        const auto& base = it.value().value("base", nlohmann::json::object());
        for (auto bIt = base.begin(); bIt != base.end(); ++bIt) {
            w.base[bIt.key()] = bIt.value().get<double>();
        }
        cfg.wallets[it.key()] = std::move(w);
    }

    return cfg;
}

}  // namespace hftarb
