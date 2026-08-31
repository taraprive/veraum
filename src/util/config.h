#pragma once

#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "src/engine/arbitrage_engine.h"
#include "src/engine/cost_engine.h"
#include "src/engine/directional_strategy.h"
#include "src/engine/liquidity_engine.h"
#include "src/engine/risk_engine.h"
#include "src/execution/paper_executor.h"
#include "src/market/market_state.h"
#include "src/monitor/watchdog.h"
#include "src/signal/signal_service.h"
#include "src/util/logger.h"

namespace hftarb {

// Initial per-exchange account balances (paper portfolio).
struct Wallet {
    double usdt = 0.0;
    std::unordered_map<std::string, double> base;  // symbol -> qty
};

// Loads config/config.json, config/exchanges.json and the optional
// config/credentials.json into typed settings. Failures throw std::runtime_error
// with a descriptive message so the process refuses to start with a broken
// configuration instead of running blind.
struct AppConfig {
    struct SymbolConfig {
        std::string symbol;
        Quantity tradeQuantity = 0.1;
        double basePrice = 100000.0;        // market sim anchor
        double liquidityTopQty = 0.1;       // market sim top-of-book qty
        std::vector<std::string> exchanges;
    };

    // A same-venue triangular cycle: start in `startAsset`, pass through each
    // leg in order, and land back in `startAsset`. Legs must be configured in
    // execution order; sides follow the direction of travel (buy = pay quote,
    // sell = pay base). `notional` is the entry size in `startAsset`; per-leg
    // quantities are derived from the live books at evaluation time.
    struct TriangleConfig {
        struct Leg {
            std::string symbol;
            Side side = Side::Buy;
        };
        std::string name;           // e.g. "USDT_BTC_ETH"
        std::string exchange;       // single venue for all three legs
        std::string startAsset = "USDT";
        double notional = 500.0;    // entry size in startAsset
        std::vector<Leg> legs;      // exactly three
    };

    // Connectivity description of one venue (exchanges.json "adapter" field).
    struct ExchangeAdapterConfig {
        std::string adapter = "mock";       // "mock" | "binance" | "bybit" | "okx" | "mexc"
        std::string baseUrl;                // REST base URL for real adapters
        std::int64_t updateMs = 50;         // REST poll interval
        std::string feed = "rest";          // "rest" (poll) | "ws" (streaming)
    };

    // Capital-aware pair allocation: trades the most opportunity-dense pairs
    // the portfolio can finance, and enables larger pairs as capital grows.
    struct AllocatorConfig {
        int checkIntervalSec = 15;   // how often to re-plan
        int windowSec = 60;          // rolling window for opportunity rate
        double capitalBuffer = 2.5;  // required = notional * buffer (USDT)
        double minOppRate = 0.0;     // informational floor (opportunities/min)
    };

    std::vector<std::string> exchanges;
    std::unordered_map<std::string, ExchangeAdapterConfig> exchangeAdapters;
    std::unordered_map<std::string, std::string> apiKeys;     // exchange -> api key
    std::unordered_map<std::string, std::string> apiSecrets;  // exchange -> api secret
    std::vector<SymbolConfig> symbols;
    std::vector<TriangleConfig> triangles;
    std::vector<InstrumentSpec> instruments;  // directional LONG/SHORT engines
    std::unordered_map<std::string, FeeSchedule> feeSchedules;
    std::unordered_map<std::string, double> exchangeSpreadBp;  // market sim spread override
    std::unordered_map<std::string, Wallet> wallets;           // paper portfolio seed
    AllocatorConfig allocator;
    Portfolio::RebalanceConfig rebalance;
    MarketDynamics market;
    CostConfig cost;
    LiquidityConfig liquidity;
    RiskConfig risk;
    PaperConfig paper;
    WatchdogConfig watchdog;
    SignalConfig signals;
    std::string logFile;
    LogLevel logLevel = LogLevel::Info;
    std::int64_t runSeconds = 0;   // 0 = run until interrupted
    std::int64_t feedSeed = 0;     // 0 = random per run; >0 = reproducible feed
    std::string mt5FeedPath;       // JSONL written by tools/mt5_feed.py; "" = off
    std::string stateFile = "data/balances.json";  // "" = persistence disabled
};

class ConfigLoader {
public:
    static AppConfig load(const std::string& configPath, const std::string& exchangesPath,
                          const std::string& credentialsPath = "config/credentials.json");

private:
    static double getDouble(const nlohmann::json& j, const std::string& key, double def);
    static int getInt(const nlohmann::json& j, const std::string& key, int def);
    static std::string getString(const nlohmann::json& j, const std::string& key,
                                 const std::string& def);
};

}  // namespace hftarb
