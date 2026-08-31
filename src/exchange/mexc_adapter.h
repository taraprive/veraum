#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>

#include "src/exchange/exchange_adapter_if.h"
#include "src/exchange/exchange_types.h"
#include "src/net/http_client.h"

namespace hftarb {

// REST adapter for the MEXC v3 spot API (Binance-compatible style). MEXC is
// deliberately included for zero/0.05% maker-taker fees and deep micro-cap
// liquidity (PEPE, SHIB, ...) which makes small-capital arbitrage viable.
// Signed endpoints are HMAC-SHA256 hex with the provided API secret; the
// query parameters must be sorted alphabetically before signing.
class MexcAdapter : public IExchangeAdapter {
public:
    MexcAdapter(std::string baseUrl, std::string apiKey, std::string apiSecret);

    std::optional<DepthBook> fetchDepth(const std::string& symbol, int limit = 20) override;

    std::optional<std::map<std::string, double>> fetchBalances() override;

    bool placeLimitOrder(const std::string& symbol, const std::string& side, double qty,
                         double price, std::string& orderId, std::string& error) override;

    bool cancelOrder(const std::string& symbol, const std::string& orderId,
                     std::string& error) override;

    std::optional<OrderInfo> fetchOrderStatus(const std::string& symbol,
                                              const std::string& orderId) override;

    std::optional<long long> fetchServerTimeOffsetMs() const;

private:
    std::string sign(const std::map<std::string, std::string>& params) const;
    // Builds "timestamp=...&..." (sorted) plus trailing "&signature=...".
    std::string signedQuery(const std::map<std::string, std::string>& params) const;
    std::string makeUrl(const std::string& path,
                        const std::map<std::string, std::string>& params) const;
    HttpResponse call(const char* verb, const std::string& path, const std::string& contentType,
                      const std::string& payload, const HeaderList& headers) const;

    HttpClient http_;
    std::string apiKey_;
    std::string apiSecret_;
};

}  // namespace hftarb