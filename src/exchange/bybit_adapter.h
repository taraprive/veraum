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

class BybitAdapter : public IExchangeAdapter {
public:
    BybitAdapter(std::string baseUrl, std::string apiKey, std::string apiSecret);

    std::optional<DepthBook> fetchDepth(const std::string& symbol, int limit = 20) override;
    bool placeLimitOrder(const std::string& symbol, const std::string& side, double qty,
                         double price, std::string& orderId, std::string& error) override;
    bool cancelOrder(const std::string& symbol, const std::string& orderId,
                     std::string& error) override;
    std::optional<std::map<std::string, double>> fetchBalances() override;

private:
    std::string sign(long long ts, const std::string& payload) const;

    HttpClient http_;
    std::string apiKey_;
    std::string apiSecret_;
};

}  // namespace hftarb
