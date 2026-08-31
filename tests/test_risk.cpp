#include "test_framework.h"

#include "src/engine/risk_engine.h"

using namespace hftarb;

static Opportunity makeOpp(const std::string& sym = "BTC/USDT") {
    Opportunity o;
    o.symbol = sym;
    o.buyExchange = "A";
    o.sellExchange = "B";
    o.quantity = 0.1;
    o.roundTripLatencyMs = 1.0;
    return o;
}

TEST(risk_accepts_healthy_opportunity) {
    RiskEngine r(RiskConfig{});
    const auto connected = [](const std::string&) { return true; };
    RiskState st{connected, 0.0, 5000, 4950};
    CHECK(r.evaluate(makeOpp(), st) == Decision::Accepted);
}

TEST(risk_rejects_stale_data) {
    RiskEngine r(RiskConfig{});
    const auto connected = [](const std::string&) { return true; };
    // Book received 1000ms ago but max age is 500ms.
    RiskState st{connected, 0.0, 5000, 4000};
    CHECK(r.evaluate(makeOpp(), st) == Decision::StaleData);
}

TEST(risk_rejects_disconnected_exchange) {
    RiskEngine r(RiskConfig{});
    const auto connected = [](const std::string& ex) { return ex != "B"; };
    RiskState st{connected, 0.0, 5000, 4950};
    CHECK(r.evaluate(makeOpp(), st) == Decision::Disconnected);
}

TEST(risk_rejects_latency_over_budget) {
    RiskEngine r(RiskConfig{});
    const auto connected = [](const std::string&) { return true; };
    auto o = makeOpp();
    o.roundTripLatencyMs = 500.0;
    RiskState st{connected, 0.0, 5000, 4950};
    CHECK(r.evaluate(o, st) == Decision::LatencyRejected);
}

TEST(risk_rejects_size_out_of_bounds) {
    RiskEngine r(RiskConfig{});
    const auto connected = [](const std::string&) { return true; };
    auto o = makeOpp();
    o.quantity = 100.0;
    RiskState st{connected, 0.0, 5000, 4950};
    CHECK(r.evaluate(o, st) == Decision::SizeRejected);
}

TEST(risk_rejects_exposure_cap) {
    RiskEngine r(RiskConfig{});
    const auto connected = [](const std::string&) { return true; };
    // Exposure 0.4 + qty 0.2 > max 0.5
    RiskState st{connected, 0.4, 5000, 4950};
    auto o = makeOpp();
    o.quantity = 0.2;
    CHECK(r.evaluate(o, st) == Decision::RiskRejected);
}

TEST(risk_enforces_cooldown_per_symbol) {
    RiskEngine r(RiskConfig{});
    const auto connected = [](const std::string&) { return true; };
    RiskState st{connected, 0.0, 5000, 4950};
    CHECK(r.evaluate(makeOpp(), st) == Decision::Accepted);
    r.recordTrade("BTC/USDT", 5000);
    // Same symbol immediately again -> cooldown. Different symbol -> OK.
    CHECK(r.evaluate(makeOpp(), st) == Decision::Cooldown);
    CHECK(r.evaluate(makeOpp("ETH/USDT"), st) == Decision::Accepted);
}

TEST(risk_enforces_trade_rate_limit) {
    RiskConfig cfg;
    cfg.maxTradesPerMinute = 3;
    cfg.cooldownMs = 0;  // disable cooldown so only the rate limit is exercised
    RiskEngine r(cfg);
    const auto connected = [](const std::string&) { return true; };
    RiskState st{connected, 0.0, 60000, 59900};
    for (int i = 0; i < 3; ++i) {
        CHECK(r.evaluate(makeOpp(), st) == Decision::Accepted);
        r.recordTrade("BTC/USDT", 60000);
    }
    CHECK(r.evaluate(makeOpp(), st) == Decision::RiskRejected);
}
