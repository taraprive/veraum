#include "src/exchange/bybit_adapter.h"

#include <cstdio>

#include <nlohmann/json.hpp>

#include "src/core/timestamp.h"
#include "src/util/hmac_sha256.h"
#include "src/util/logger.h"

namespace hftarb {

namespace {

std::string fmtNum(double v) {
    if (v == 0.0) return "0";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.8f", v);
    std::string s(buf);
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
    return s;
}

double parseNum(const std::string& s) {
    try { return std::stod(s); } catch (...) { return 0.0; }
}

}  // namespace

BybitAdapter::BybitAdapter(std::string baseUrl, std::string apiKey, std::string apiSecret)
    : http_(std::move(baseUrl)), apiKey_(std::move(apiKey)), apiSecret_(std::move(apiSecret)) {}

std::string BybitAdapter::sign(long long ts, const std::string& payload) const {
    std::string data = std::to_string(ts) + apiKey_ + "5000" + payload;
    return hmacSha256Hex(apiSecret_, data);
}

std::optional<DepthBook> BybitAdapter::fetchDepth(const std::string& symbol, int limit) {
    const std::string path = "/v5/market/orderbook?category=spot&symbol=" + symbol +
                             "&limit=" + std::to_string(limit);
    const HttpResponse r = http_.get(path);
    if (!r.ok()) return std::nullopt;

    try {
        const auto j = nlohmann::json::parse(r.body);
        if (j.value("retCode", -1) != 0) return std::nullopt;
        const auto& res = j.at("result");
        DepthBook b;
        b.symbol = symbol;
        b.lastUpdateId = res.value("ts", 0LL);
        for (const auto& lvl : res.value("b", nlohmann::json::array())) {
            b.bids.emplace_back(parseNum(lvl[0].get<std::string>()),
                                parseNum(lvl[1].get<std::string>()));
        }
        for (const auto& lvl : res.value("a", nlohmann::json::array())) {
            b.asks.emplace_back(parseNum(lvl[0].get<std::string>()),
                                parseNum(lvl[1].get<std::string>()));
        }
        return b;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::map<std::string, double>> BybitAdapter::fetchBalances() {
    const long long ts = Timestamps::wallMs();
    std::string query = "accountType=UNIFIED&timestamp=" + std::to_string(ts) + "&recvWindow=5000";
    const std::string sig = sign(ts, query);
    const std::string path = "/v5/account/wallet-balance?" + query + "&sign=" + sig;
    HeaderList headers;
    headers.emplace_back("X-BAPI-API-KEY", apiKey_);
    headers.emplace_back("X-BAPI-SIGN", sig);
    headers.emplace_back("X-BAPI-TIMESTAMP", std::to_string(ts));
    headers.emplace_back("X-BAPI-RECV-WINDOW", "5000");
    const HttpResponse r = http_.get(path, headers);
    Logger::instance().info("bybit_debug", "GET /v5/account/wallet-balance status=" +
                             std::to_string(r.status) + " body=" + r.body.substr(0, 500));
    if (!r.ok()) return std::nullopt;

    try {
        const auto j = nlohmann::json::parse(r.body);
        if (j.value("retCode", -1) != 0) return std::nullopt;
        const auto& list = j.at("result").at("list");
        if (list.empty()) return std::nullopt;
        std::map<std::string, double> out;
        for (const auto& coin : list[0].value("coin", nlohmann::json::array())) {
            const double free = parseNum(coin.value("availableToWithdraw",
                                           coin.value("equity", "0")));
            if (free != 0.0) out[coin.value("coin", "")] = free;
        }
        return out;
    } catch (...) {
        return std::nullopt;
    }
}

bool BybitAdapter::placeLimitOrder(const std::string& symbol, const std::string& side,
                                   double qty, double price, std::string& orderId,
                                   std::string& error) {
    const long long ts = Timestamps::wallMs();
    std::string body = "category=spot&symbol=" + symbol + "&side=" + side +
                       "&orderType=Limit&qty=" + fmtNum(qty) + "&price=" + fmtNum(price) +
                       "&timeInForce=GTC&timestamp=" + std::to_string(ts) + "&recvWindow=5000";
    const std::string sig = sign(ts, body);
    HeaderList headers;
    headers.emplace_back("X-BAPI-API-KEY", apiKey_);
    headers.emplace_back("X-BAPI-SIGN", sig);
    headers.emplace_back("X-BAPI-TIMESTAMP", std::to_string(ts));
    headers.emplace_back("X-BAPI-RECV-WINDOW", "5000");

    Logger::instance().info("bybit_debug", "ORDER body=" + body);

    const HttpResponse r = http_.post("/v5/order/create", "application/x-www-form-urlencoded",
                                      body, headers);

    Logger::instance().info("bybit_debug", "POST /v5/order/create status=" + std::to_string(r.status) +
                             " body=" + r.body.substr(0, 300) + " err=" + r.error);

    try {
        const auto j = nlohmann::json::parse(r.body);
        if (j.value("retCode", -1) == 0) {
            orderId = j.at("result").value("orderId", "");
            return !orderId.empty();
        }
        error = j.value("retMsg", r.body);
    } catch (...) {
        error = r.body.empty() ? r.error : r.body;
    }
    return false;
}

bool BybitAdapter::cancelOrder(const std::string& symbol, const std::string& orderId,
                               std::string& error) {
    const long long ts = Timestamps::wallMs();
    std::string body = "category=spot&symbol=" + symbol + "&orderId=" + orderId +
                       "&timestamp=" + std::to_string(ts) + "&recvWindow=5000";
    const std::string sig = sign(ts, body);
    HeaderList headers;
    headers.emplace_back("X-BAPI-API-KEY", apiKey_);
    headers.emplace_back("X-BAPI-SIGN", sig);
    headers.emplace_back("X-BAPI-TIMESTAMP", std::to_string(ts));
    headers.emplace_back("X-BAPI-RECV-WINDOW", "5000");
    const HttpResponse r = http_.post("/v5/order/cancel", "application/x-www-form-urlencoded",
                                      body, headers);
    if (!r.ok()) { error = r.error; return false; }
    try {
        const auto j = nlohmann::json::parse(r.body);
        if (j.value("retCode", -1) == 0) return true;
        error = j.value("retMsg", r.body);
    } catch (...) {
        error = r.body.empty() ? r.error : r.body;
    }
    return false;
}

}  // namespace hftarb
