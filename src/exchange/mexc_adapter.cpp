#include "src/exchange/mexc_adapter.h"

#include <cstdio>
#include <map>

#include <nlohmann/json.hpp>

#include "src/core/timestamp.h"
#include "src/util/hmac_sha256.h"
#include "src/util/logger.h"

namespace hftarb {

namespace {

// Decimal formatting keeping MEXC filters happy: up to 8 decimals, trailing
// zeros stripped, never exponent notation. micro-caps need full integer qty,
// prices of cheap coins need 8-9 decimals.
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

std::string urlEncode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

}  // namespace

MexcAdapter::MexcAdapter(std::string baseUrl, std::string apiKey, std::string apiSecret)
    : http_(std::move(baseUrl)), apiKey_(std::move(apiKey)), apiSecret_(std::move(apiSecret)) {}

HttpResponse MexcAdapter::call(const char* verb, const std::string& path,
                               const std::string& contentType, const std::string& payload,
                               const HeaderList& headers) const {
    if (verb == std::string("POST")) return http_.post(path, contentType, payload, headers);
    if (verb == std::string("DELETE")) return http_.del(path, headers);
    return http_.get(path, headers);
}

std::string MexcAdapter::sign(const std::map<std::string, std::string>& params) const {
    return hmacSha256Hex(apiSecret_, signedQuery(params));
}

// MEXC v3 requires the query parameters sorted alphabetically by key, the
// timestamp last, then the signature appended after signing.
std::string MexcAdapter::signedQuery(const std::map<std::string, std::string>& params) const {
    std::map<std::string, std::string> sorted = params;
    sorted["timestamp"] = std::to_string(Timestamps::wallMs());
    std::string q;
    for (const auto& [k, v] : sorted) {
        if (!q.empty()) q += "&";
        q += urlEncode(k) + "=" + urlEncode(v);
    }
    const std::string sig = hmacSha256Hex(apiSecret_, q);
    return q + "&signature=" + sig;
}

std::string MexcAdapter::makeUrl(const std::string& path,
                                 const std::map<std::string, std::string>& params) const {
    return path + "?" + signedQuery(params);
}

std::optional<DepthBook> MexcAdapter::fetchDepth(const std::string& symbol, int limit) {
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

std::optional<std::map<std::string, double>> MexcAdapter::fetchBalances() {
    const std::string path = makeUrl("/api/v3/account", {});
    HeaderList headers;
    if (!apiKey_.empty()) headers.emplace_back("X-MEXC-APIKEY", apiKey_);
    const HttpResponse r = call("GET", path, "", "", headers);
    if (!r.ok()) {
        Logger::instance().warn("mexc_debug", "GET /api/v3/account status=" +
                                   std::to_string(r.status) + " err=" + r.error);
        return std::nullopt;
    }

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

bool MexcAdapter::placeLimitOrder(const std::string& symbol, const std::string& side,
                                  double qty, double price, std::string& orderId,
                                  std::string& error) {
    std::map<std::string, std::string> params;
    params["symbol"] = symbol;
    params["side"] = side;
    params["type"] = "LIMIT";
    params["timeInForce"] = "GTC";
    params["quantity"] = fmtNum(qty);
    params["price"] = fmtNum(price);
    const std::string path = makeUrl("/api/v3/order", params);

    HeaderList headers;
    if (!apiKey_.empty()) headers.emplace_back("X-MEXC-APIKEY", apiKey_);
    const HttpResponse r = call("POST", path, "", "", headers);

    Logger::instance().info("mexc_debug", "POST /api/v3/order status=" + std::to_string(r.status) +
                             " body=" + r.body.substr(0, 300) + " err=" + r.error);

    try {
        const auto j = nlohmann::json::parse(r.body);
        if (r.ok()) {
            orderId = j.value("orderId", 0LL) == 0 ? "" : std::to_string(j.at("orderId").get<long long>());
            return !orderId.empty();
        }
        error = j.value("msg", j.value("message", r.body));
    } catch (...) {
        error = r.body.empty() ? r.error : r.body;
    }
    return false;
}

bool MexcAdapter::cancelOrder(const std::string& symbol, const std::string& orderId,
                              std::string& error) {
    std::map<std::string, std::string> params;
    params["symbol"] = symbol;
    params["orderId"] = orderId;
    const std::string path = makeUrl("/api/v3/order", params);
    HeaderList headers;
    if (!apiKey_.empty()) headers.emplace_back("X-MEXC-APIKEY", apiKey_);
    const HttpResponse r = call("DELETE", path, "", "", headers);
    if (r.ok()) return true;
    try {
        const auto j = nlohmann::json::parse(r.body);
        error = j.value("msg", j.value("message", r.body));
    } catch (...) {
        error = r.body.empty() ? r.error : r.body;
    }
    return false;
}

std::optional<IExchangeAdapter::OrderInfo> MexcAdapter::fetchOrderStatus(
    const std::string& symbol, const std::string& orderId) {
    std::map<std::string, std::string> params;
    params["symbol"] = symbol;
    params["orderId"] = orderId;
    const std::string path = makeUrl("/api/v3/order", params);
    HeaderList headers;
    if (!apiKey_.empty()) headers.emplace_back("X-MEXC-APIKEY", apiKey_);
    const HttpResponse r = call("GET", path, "", "", headers);
    if (!r.ok()) return std::nullopt;
    try {
        const auto j = nlohmann::json::parse(r.body);
        OrderInfo oi;
        oi.state = j.value("status", "");
        oi.filledQty = parseNum(j.value("executedQty", "0"));
        const double cummulativeQuoteQty = parseNum(j.value("cummulativeQuoteQty", "0"));
        if (oi.filledQty > 0.0 && cummulativeQuoteQty > 0.0)
            oi.avgPx = cummulativeQuoteQty / oi.filledQty;
        return oi;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<long long> MexcAdapter::fetchServerTimeOffsetMs() const {
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