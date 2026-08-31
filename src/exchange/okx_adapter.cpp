#include "src/exchange/okx_adapter.h"

#include <cstdio>
#include <cctype>
#include <ctime>

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

std::string toIso8601Ms(long long epochMs) {
    std::time_t secs = epochMs / 1000;
    int millis = static_cast<int>(epochMs % 1000);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &secs);
#else
    gmtime_r(&secs, &utc);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min, utc.tm_sec, millis);
    return std::string(buf);
}

}  // namespace

OkxAdapter::OkxAdapter(std::string baseUrl, std::string apiKey, std::string apiSecret,
                         std::string passphrase, bool demo)
    : http_(std::move(baseUrl)), apiKey_(std::move(apiKey)), apiSecret_(std::move(apiSecret)),
      passphrase_(std::move(passphrase)), demo_(demo) {}

std::string OkxAdapter::sign(const std::string& timestamp, const std::string& method,
                              const std::string& requestPath, const std::string& body) const {
    std::string prehash = timestamp + method + requestPath + body;
    return hmacSha256Base64(apiSecret_, prehash);
}

std::string OkxAdapter::toOkxSymbol(const std::string& symbol) const {
    std::string s = symbol;
    for (auto& c : s) { if (c == '/') c = '-'; }
    if (s.find('-') == std::string::npos && s.size() >= 6) {
        for (size_t i = 1; i < s.size(); ++i) {
            if (s[i] == 'U' && i + 4 <= s.size() && s.substr(i, 4) == "USDT") {
                s.insert(s.begin() + i, '-');
                break;
            }
        }
    }
    return s;
}

std::optional<DepthBook> OkxAdapter::fetchDepth(const std::string& symbol, int limit) {
    const std::string instId = toOkxSymbol(symbol);
    const std::string path = "/api/v5/market/books?instId=" + instId +
                             "&sz=" + std::to_string(limit);
    const HttpResponse r = http_.get(path);
    if (!r.ok()) return std::nullopt;

    try {
        const auto j = nlohmann::json::parse(r.body);
        if (j.value("code", "0") != "0") return std::nullopt;
        const auto& data = j.at("data");
        if (data.empty()) return std::nullopt;
        const auto& book = data[0];

        DepthBook b;
        b.symbol = symbol;
        b.lastUpdateId = 0;
        try {
            const std::string tsStr = book.value("ts", "0");
            b.lastUpdateId = std::stoll(tsStr);
        } catch (...) {}

        for (const auto& lvl : book.value("bids", nlohmann::json::array())) {
            if (lvl.size() >= 2)
                b.bids.emplace_back(parseNum(lvl[0].get<std::string>()),
                                    parseNum(lvl[1].get<std::string>()));
        }
        for (const auto& lvl : book.value("asks", nlohmann::json::array())) {
            if (lvl.size() >= 2)
                b.asks.emplace_back(parseNum(lvl[0].get<std::string>()),
                                    parseNum(lvl[1].get<std::string>()));
        }
        return b;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::map<std::string, double>> OkxAdapter::fetchBalances() {
    const std::string method = "GET";
    const std::string requestPath = "/api/v5/account/balance";
    const std::string ts = toIso8601Ms(Timestamps::wallMs());
    const std::string sig = sign(ts, method, requestPath, "");

    HeaderList headers;
    headers.emplace_back("OK-ACCESS-KEY", apiKey_);
    headers.emplace_back("OK-ACCESS-SIGN", sig);
    headers.emplace_back("OK-ACCESS-TIMESTAMP", ts);
    headers.emplace_back("OK-ACCESS-PASSPHRASE", passphrase_);
    if (demo_) headers.emplace_back("x-simulated-trading", "1");

    const HttpResponse r = http_.get(requestPath, headers);
    Logger::instance().info("okx_debug", "GET /api/v5/account/balance status=" +
                             std::to_string(r.status) + " body=" + r.body.substr(0, 2000));
    if (!r.ok()) return std::nullopt;

    try {
        const auto j = nlohmann::json::parse(r.body);
        if (j.value("code", "-1") != "0") return std::nullopt;
        const auto& data = j.at("data");
        if (data.empty()) return std::nullopt;
        std::map<std::string, double> out;
        for (const auto& detail : data[0].value("details", nlohmann::json::array())) {
            const double avail = parseNum(detail.value("availBal", "0"));
            if (avail != 0.0) out[detail.value("ccy", "")] = avail;
        }
        return out;
    } catch (...) {
        return std::nullopt;
    }
}

bool OkxAdapter::placeLimitOrder(const std::string& symbol, const std::string& side,
                                  double qty, double price, std::string& orderId,
                                  std::string& error) {
    return placeOrderInternal(symbol, side, qty, price, "limit", orderId, error);
}

bool OkxAdapter::placeIocOrder(const std::string& symbol, const std::string& side,
                                double qty, double price, std::string& orderId,
                                std::string& error) {
    return placeOrderInternal(symbol, side, qty, price, "ioc", orderId, error);
}

bool OkxAdapter::placeMarketOrder(const std::string& symbol, const std::string& side,
                                   double qty, std::string& orderId, std::string& error) {
    return placeOrderInternal(symbol, side, qty, 0.0, "market", orderId, error);
}

bool OkxAdapter::placeOrderInternal(const std::string& symbol, const std::string& side,
                                     double qty, double price, const std::string& ordType,
                                     std::string& orderId, std::string& error) {
    const std::string instId = toOkxSymbol(symbol);
    const std::string method = "POST";
    const std::string requestPath = "/api/v5/trade/order";
    const std::string ts = toIso8601Ms(Timestamps::wallMs());

    std::string okxSide = side;
    for (auto& c : okxSide) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    nlohmann::json bodyJson;
    bodyJson["instId"] = instId;
    bodyJson["tdMode"] = "cross";
    bodyJson["side"] = okxSide;
    bodyJson["ordType"] = ordType;
    bodyJson["sz"] = fmtNum(qty);
    if (ordType != "market" && price > 0.0) {
        bodyJson["px"] = fmtNum(price);
    }
    if (ordType == "market" && okxSide == "buy") {
        bodyJson["tgtCcy"] = "base_ccy";
    }
    const std::string body = bodyJson.dump();

    const std::string sig = sign(ts, method, requestPath, body);

    HeaderList headers;
    headers.emplace_back("Content-Type", "application/json");
    headers.emplace_back("OK-ACCESS-KEY", apiKey_);
    headers.emplace_back("OK-ACCESS-SIGN", sig);
    headers.emplace_back("OK-ACCESS-TIMESTAMP", ts);
    headers.emplace_back("OK-ACCESS-PASSPHRASE", passphrase_);
    if (demo_) headers.emplace_back("x-simulated-trading", "1");

    Logger::instance().info("okx_debug", "ORDER body=" + body);

    const HttpResponse r = http_.post(requestPath, "application/json", body, headers);

    Logger::instance().info("okx_debug", "POST /api/v5/trade/order status=" +
                             std::to_string(r.status) + " body=" + r.body.substr(0, 300) +
                             " err=" + r.error);

    try {
        const auto j = nlohmann::json::parse(r.body);
        if (j.value("code", "-1") == "0") {
            const auto& data = j.at("data");
            if (!data.empty()) {
                orderId = data[0].value("ordId", "");
                return !orderId.empty();
            }
        }
        const auto& data = j.value("data", nlohmann::json::array());
        if (!data.empty()) error = data[0].value("sMsg", j.value("msg", r.body));
        else error = j.value("msg", r.body);
    } catch (...) {
        error = r.body.empty() ? r.error : r.body;
    }
    return false;
}

bool OkxAdapter::cancelOrder(const std::string& symbol, const std::string& orderId,
                              std::string& error) {
    const std::string instId = toOkxSymbol(symbol);
    const std::string method = "POST";
    const std::string requestPath = "/api/v5/trade/cancel-order";
    const std::string ts = toIso8601Ms(Timestamps::wallMs());

    nlohmann::json bodyJson;
    bodyJson["instId"] = instId;
    bodyJson["ordId"] = orderId;
    const std::string body = bodyJson.dump();

    const std::string sig = sign(ts, method, requestPath, body);

    HeaderList headers;
    headers.emplace_back("Content-Type", "application/json");
    headers.emplace_back("OK-ACCESS-KEY", apiKey_);
    headers.emplace_back("OK-ACCESS-SIGN", sig);
    headers.emplace_back("OK-ACCESS-TIMESTAMP", ts);
    headers.emplace_back("OK-ACCESS-PASSPHRASE", passphrase_);
    if (demo_) headers.emplace_back("x-simulated-trading", "1");

    const HttpResponse r = http_.post(requestPath, "application/json", body, headers);
    if (!r.ok()) { error = r.error; return false; }
    try {
        const auto j = nlohmann::json::parse(r.body);
        if (j.value("code", "-1") == "0") return true;
        error = j.value("msg", r.body);
    } catch (...) {
        error = r.body.empty() ? r.error : r.body;
    }
    return false;
}

std::optional<IExchangeAdapter::OrderInfo> OkxAdapter::fetchOrderStatus(
    const std::string& symbol, const std::string& orderId) {
    const std::string instId = toOkxSymbol(symbol);
    const std::string requestPath = "/api/v5/trade/order?instId=" + instId +
                                    "&ordId=" + orderId;
    const std::string method = "GET";
    const std::string ts = toIso8601Ms(Timestamps::wallMs());
    const std::string sig = sign(ts, method, requestPath, "");

    HeaderList headers;
    headers.emplace_back("OK-ACCESS-KEY", apiKey_);
    headers.emplace_back("OK-ACCESS-SIGN", sig);
    headers.emplace_back("OK-ACCESS-TIMESTAMP", ts);
    headers.emplace_back("OK-ACCESS-PASSPHRASE", passphrase_);
    if (demo_) headers.emplace_back("x-simulated-trading", "1");

    const HttpResponse r = http_.get(requestPath, headers);
    if (!r.ok()) return std::nullopt;

    try {
        const auto j = nlohmann::json::parse(r.body);
        if (j.value("code", "-1") != "0") return std::nullopt;
        const auto& data = j.at("data");
        if (data.empty()) return std::nullopt;
        const auto& o = data[0];
        OrderInfo oi;
        oi.state = o.value("state", "");
        oi.filledQty = parseNum(o.value("accFillSz", "0"));
        oi.avgPx = parseNum(o.value("avgPx", "0"));
        return oi;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace hftarb
