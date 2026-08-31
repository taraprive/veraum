#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "test_framework.h"

#include "src/core/types.h"
#include "src/signal/signal_service.h"
#include "src/util/config.h"

using namespace hftarb;

namespace {
std::string readAll(const std::string& path) {
    std::ifstream ifs(path);
    std::ostringstream os;
    os << ifs.rdbuf();
    return os.str();
}

Opportunity acceptedOpp(const std::string& symbol, double netSpread) {
    Opportunity o;
    o.strategy = "cross_exchange";
    o.symbol = symbol;
    o.buyExchange = "binance";
    o.sellExchange = "okx";
    o.quantity = 0.01;
    o.cost.buyPrice = 100.0;
    o.cost.sellPrice = 100.5;
    o.cost.netSpread = netSpread;
    o.cost.notional = 1.0;
    o.decisionTs = 123456789;
    o.decision = Decision::Accepted;
    return o;
}
}  // namespace

TEST(signal_ignores_non_accepted) {
    SignalConfig cfg;
    cfg.enabled = true;
    cfg.channel = "none";
    cfg.minNetSpread = 0.0;
    cfg.jsonlPath = "test_signal_jsonl1.ndjson";
    std::remove(cfg.jsonlPath.c_str());

    Opportunity o = acceptedOpp("BTC/USDT", 0.01);
    o.decision = Decision::RiskRejected;
    {
        SignalService svc(cfg);
        svc.emit(o);
        svc.stop();
    }
    CHECK(readAll(cfg.jsonlPath).empty());
    std::remove(cfg.jsonlPath.c_str());
}

TEST(signal_respects_min_net_spread) {
    SignalConfig cfg;
    cfg.enabled = true;
    cfg.channel = "none";
    cfg.minNetSpread = 0.005;
    cfg.jsonlPath = "test_signal_jsonl2.ndjson";
    std::remove(cfg.jsonlPath.c_str());

    {
        SignalService svc(cfg);
        svc.emit(acceptedOpp("BTC/USDT", 0.003));  // below threshold -> silent
        svc.emit(acceptedOpp("ETH/USDT", 0.007));  // above threshold -> emitted
        svc.stop();
    }
    const std::string content = readAll(cfg.jsonlPath);
    CHECK(content.find("BTC/USDT") == std::string::npos);
    CHECK(content.find("ETH/USDT") != std::string::npos);
    CHECK(content.find("not guaranteed profit") != std::string::npos);
    std::remove(cfg.jsonlPath.c_str());
}

TEST(signal_respects_symbol_filter) {
    SignalConfig cfg;
    cfg.enabled = true;
    cfg.channel = "none";
    cfg.minNetSpread = 0.0;
    cfg.symbols = {"BTC/USDT"};
    cfg.jsonlPath = "test_signal_jsonl3.ndjson";
    std::remove(cfg.jsonlPath.c_str());

    {
        SignalService svc(cfg);
        svc.emit(acceptedOpp("BTC/USDT", 0.01));
        svc.emit(acceptedOpp("SOL/USDT", 0.02));
        svc.stop();
    }
    const std::string content = readAll(cfg.jsonlPath);
    CHECK(content.find("BTC/USDT") != std::string::npos);
    CHECK(content.find("SOL/USDT") == std::string::npos);
    std::remove(cfg.jsonlPath.c_str());
}

TEST(signal_throttles_per_symbol) {
    SignalConfig cfg;
    cfg.enabled = true;
    cfg.channel = "none";
    cfg.minNetSpread = 0.0;
    cfg.intervalMs = 60000;
    cfg.jsonlPath = "test_signal_jsonl4.ndjson";
    std::remove(cfg.jsonlPath.c_str());

    {
        SignalService svc(cfg);
        svc.emit(acceptedOpp("BTC/USDT", 0.01));
        svc.emit(acceptedOpp("BTC/USDT", 0.02));
        svc.emit(acceptedOpp("BTC/USDT", 0.03));
        svc.emit(acceptedOpp("ETH/USDT", 0.01));
        svc.stop();
    }
    const std::string content = readAll(cfg.jsonlPath);
    std::istringstream iss(content);
    std::string line;
    int btc = 0, eth = 0;
    while (std::getline(iss, line)) {
        if (line.find("BTC/USDT") != std::string::npos) ++btc;
        if (line.find("ETH/USDT") != std::string::npos) ++eth;
    }
    CHECK(btc == 1);  // throttle suppressed the 2nd/3rd attempts for the same symbol
    CHECK(eth == 1);  // different symbol is not throttled
    std::remove(cfg.jsonlPath.c_str());
}

TEST(signal_triangular_format_has_legs) {
    SignalConfig cfg;
    cfg.enabled = true;
    cfg.channel = "none";
    cfg.minNetSpread = 0.0;
    cfg.jsonlPath = "test_signal_jsonl5.ndjson";
    std::remove(cfg.jsonlPath.c_str());

    Opportunity o = acceptedOpp("USDT_BTC_ETH", 0.008);
    o.strategy = "triangular";
    o.legs = {{"binance", "BTC/USDT", Side::Buy, 79000.0, 0.0025},
              {"binance", "ETH/BTC", Side::Buy, 0.0319, 6.18},
              {"binance", "ETH/USDT", Side::Sell, 2522.0, 6.17}};
    {
        SignalService svc(cfg);
        svc.emit(o);
        svc.stop();
    }
    const std::string content = readAll(cfg.jsonlPath);
    CHECK(content.find("buy") != std::string::npos);
    CHECK(content.find("sell") != std::string::npos);
    CHECK(content.find("ETH/BTC") != std::string::npos);
    CHECK(content.find("80 bp") != std::string::npos);  // 0.008 * 10000
    std::remove(cfg.jsonlPath.c_str());
}