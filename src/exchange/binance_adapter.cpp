#include "src/exchange/binance_adapter.h"

#include <cstdio>

#include <nlohmann/json.hpp>

#include "src/core/timestamp.h"
#include "src/util/hmac_sha256.h"
#include "src/util/logger.h"

namespace hftarb {

namespace {

// Decimal formatting that matches Binance filters on testnet: up to 8 places,
// trailing zeros stripped, never exponent notation.
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
    try {
        return std::stod(s);
    } catch (...) {
        return 0.0;
    }
}

}  // namespace

BinanceAdapter::BinanceAdapter(std::string baseUrl, std::string apiKey, std::string apiSecret)
    : http_(std::move(baseUrl)), apiKey_(std::move(apiKey)), apiSecret_(std::move(apiSecret)) {}

HttpResponse BinanceAdapter::call(const char* verb, const std::string& path,
                                  const std::string& contentType, const std::string& payload,
                                  const HeaderList& headers) const {
    if (verb == std::string("POST")) return http_.post(path, contentType, payload, headers);
    if (verb == std::string("DELETE")) return http_.del(path, headers);
    return http_.get(path, headers);
}

std::string BinanceAdapter::sign(const std::string& query) const {
    return hmacSha256Hex(apiSecret_, query);
}

std::string BinanceAdapter::signedQuery(const std::string& params) const {
    const long long ts = Timestamps::wallMs();
    std::string q = params + "timestamp=" + std::to_string(ts) + "&recvWindow=5000";
    return q + "&signature=" + sign(q);
}

std::optional<DepthBook> BinanceAdapter::fetchDepth(const std::string& symbol, int limit) {
    const std::string path = "/api/v3/depth?symbol=" + symbol + "&limit=" + std::to_string(limit);
    const HttpResponse r = http_.get(path);
    if (!r.ok()) return std::nullopt;

    try {
        const auto j = nlohmann::json::parse(r.body);
        DepthBook b;
        b.symbol = symbol;
        b.lastUpdateId = j.value("lastUpdateId", 0LL);
        for (const auto& level : j.value("bids", nlohmann::json::array())) {
            b.bids.emplace_back(parseNum(level[0].get<std::string>()),
                                parseNum(level[1].get<std::string>()));
        }
        for (const auto& level : j.value("asks", nlohmann::json::array())) {
            b.asks.emplace_back(parseNum(level[0].get<std::string>()),
                                parseNum(level[1].get<std::string>()));
        }
        return b;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::map<std::string, double>> BinanceAdapter::fetchBalances() {
    const std::string path = "/api/v3/account?" + signedQuery("");
    HeaderList headers;
    if (!apiKey_.empty()) headers.emplace_back("X-MBX-APIKEY", apiKey_);
    const HttpResponse r = call("GET", path, "", "", headers);
    if (!r.ok()) return std::nullopt;

    try {
        const auto j = nlohmann::json::parse(r.body);
        std::map<std::string, double> out;
        for (const auto& bal : j.value("balances", nlohmann::json::array())) {
            const double free = parseNum(bal.value("free", "0"));
            if (free != 0.0) out[bal.value("asset", "")] = free;
        }
        return out;
    } catch (...) {
        return std::nullopt;
    }
}

bool BinanceAdapter::placeLimitOrder(const std::string& symbol, const std::string& side,
                                     double qty, double price, std::string& orderId,
                                     std::string& error) {
    std::string params = "symbol=" + symbol + "&side=" + side + "&type=LIMIT&timeInForce=GTC" +
                         "&quantity=" + fmtNum(qty) + "&price=" + fmtNum(price) + "&";
    const std::string payload = signedQuery(params);
    const std::string path = "/api/v3/order?" + payload;

    HeaderList headers;
    if (!apiKey_.empty()) headers.emplace_back("X-MBX-APIKEY", apiKey_);
    const HttpResponse r = call("POST", path, "", "", headers);

    Logger::instance().info("binance_debug", "POST /api/v3/order status=" + std::to_string(r.status) +
                             " body=" + r.body.substr(0, 300) + " err=" + r.error);

    try {
        const auto j = nlohmann::json::parse(r.body);
        if (r.ok()) {
            orderId = j.value("orderId", 0LL) == 0 ? "" : std::to_string(j.at("orderId").get<long long>());
            return !orderId.empty();
        }
        error = j.value("msg", r.body);
    } catch (...) {
        error = r.body.empty() ? r.error : r.body;
    }
    return false;
}

bool BinanceAdapter::cancelOrder(const std::string& symbol, const std::string& orderId,
                                 std::string& error) {
    std::string params = "symbol=" + symbol + "&orderId=" + orderId + "&";
    const std::string payload = signedQuery(params);
    const std::string path = "/api/v3/order?" + payload;
    HeaderList headers;
    if (!apiKey_.empty()) headers.emplace_back("X-MBX-APIKEY", apiKey_);
    const HttpResponse r = call("DELETE", path, "", "", headers);
    if (r.ok()) return true;
    try {
        const auto j = nlohmann::json::parse(r.body);
        error = j.value("msg", r.body);
    } catch (...) {
        error = r.body.empty() ? r.error : r.body;
    }
    return false;
}

std::optional<IExchangeAdapter::OrderInfo> BinanceAdapter::fetchOrderStatus(
    const std::string& symbol, const std::string& orderId) {
    std::string params = "symbol=" + symbol + "&orderId=" + orderId + "&";
    const std::string payload = signedQuery(params);
    const std::string path = "/api/v3/order?" + payload;
    HeaderList headers;
    if (!apiKey_.empty()) headers.emplace_back("X-MBX-APIKEY", apiKey_);
    const HttpResponse r = call("GET", path, "", "", headers);
    if (!r.ok()) return std::nullopt;
    try {
        const auto j = nlohmann::json::parse(r.body);
        OrderInfo oi;
        oi.state = j.value("status", "");
        oi.filledQty = std::stod(j.value("executedQty", "0"));
        double cummulativeQuoteQty = std::stod(j.value("cummulativeQuoteQty", "0"));
        if (oi.filledQty > 0.0 && cummulativeQuoteQty > 0.0)
            oi.avgPx = cummulativeQuoteQty / oi.filledQty;
        return oi;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<long long> BinanceAdapter::fetchServerTimeOffsetMs() const {
    const HttpResponse r = http_.get("/api/v3/time");
    if (!r.ok()) return std::nullopt;
    try {
        const auto j = nlohmann::json::parse(r.body);
        const long long server = j.value("serverTime", 0LL);
        return server - Timestamps::wallMs();
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace hftarb
