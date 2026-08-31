#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "src/exchange/exchange_adapter_if.h"
#include "src/exchange/exchange_types.h"
#include "src/net/http_client.h"

namespace hftarb {

// REST adapter for a Binance-style spot API. The base URL selects the
// environment (mainnet vs testnet); signed endpoints are HMAC-SHA256 with the
// provided credentials. All methods are blocking and thread-safe to call one
// at a time.
class BinanceAdapter : public IExchangeAdapter {
public:
    BinanceAdapter(std::string baseUrl, std::string apiKey, std::string apiSecret);

    // Public market data; works with empty credentials.
    std::optional<DepthBook> fetchDepth(const std::string& symbol, int limit = 20) override;

    // Signed account data: asset -> free balance. Requires credentials.
    std::optional<std::map<std::string, double>> fetchBalances() override;

    // Places a LIMIT GTC order. side is "BUY" or "SELL".
    // Returns true and fills *orderId on success; false + *error otherwise.
    bool placeLimitOrder(const std::string& symbol, const std::string& side, double qty,
                         double price, std::string& orderId, std::string& error) override;

    bool cancelOrder(const std::string& symbol, const std::string& orderId,
                     std::string& error) override;

    std::optional<OrderInfo> fetchOrderStatus(const std::string& symbol,
                                              const std::string& orderId) override;

    // Fetches the server clock skew (serverTime - localTime, ms).
    std::optional<long long> fetchServerTimeOffsetMs() const;

private:
    std::string sign(const std::string& query) const;
    std::string signedQuery(const std::string& params) const;
    HttpResponse call(const char* verb, const std::string& path, const std::string& contentType,
                      const std::string& payload, const HeaderList& headers) const;

    HttpClient http_;
    std::string apiKey_;
    std::string apiSecret_;
};

}  // namespace hftarb
