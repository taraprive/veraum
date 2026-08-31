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

class OkxAdapter : public IExchangeAdapter {
public:
    OkxAdapter(std::string baseUrl, std::string apiKey, std::string apiSecret,
               std::string passphrase, bool demo = true);

    std::optional<DepthBook> fetchDepth(const std::string& symbol, int limit = 20) override;
    bool placeLimitOrder(const std::string& symbol, const std::string& side, double qty,
                         double price, std::string& orderId, std::string& error) override;
    bool placeIocOrder(const std::string& symbol, const std::string& side, double qty,
                       double price, std::string& orderId, std::string& error) override;
    bool placeMarketOrder(const std::string& symbol, const std::string& side, double qty,
                          std::string& orderId, std::string& error);
    bool cancelOrder(const std::string& symbol, const std::string& orderId,
                     std::string& error) override;
    std::optional<std::map<std::string, double>> fetchBalances() override;

    std::optional<OrderInfo> fetchOrderStatus(const std::string& symbol,
                                              const std::string& orderId) override;

private:
    std::string sign(const std::string& timestamp, const std::string& method,
                     const std::string& requestPath, const std::string& body) const;
    std::string toOkxSymbol(const std::string& symbol) const;
    bool placeOrderInternal(const std::string& symbol, const std::string& side,
                            double qty, double price, const std::string& ordType,
                            std::string& orderId, std::string& error);

    HttpClient http_;
    std::string apiKey_;
    std::string apiSecret_;
    std::string passphrase_;
    bool demo_;
};

}  // namespace hftarb
