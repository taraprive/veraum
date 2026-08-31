#pragma once

#include <map>
#include <optional>
#include <string>

#include "src/exchange/exchange_types.h"

namespace hftarb {

class IExchangeAdapter {
public:
    virtual ~IExchangeAdapter() = default;
    virtual std::optional<DepthBook> fetchDepth(const std::string& symbol, int limit = 20) = 0;
    virtual bool placeLimitOrder(const std::string& symbol, const std::string& side,
                                 double qty, double price, std::string& orderId,
                                 std::string& error) = 0;
    virtual bool cancelOrder(const std::string& symbol, const std::string& orderId,
                             std::string& error) = 0;
    virtual std::optional<std::map<std::string, double>> fetchBalances() = 0;

    // IOC order: fills immediately or cancels. Default falls back to limit.
    virtual bool placeIocOrder(const std::string& symbol, const std::string& side,
                               double qty, double price, std::string& orderId,
                               std::string& error) {
        return placeLimitOrder(symbol, side, qty, price, orderId, error);
    }
    // Market order: fills at best available price. Default falls back to limit.
    virtual bool placeMarketOrder(const std::string& symbol, const std::string& side,
                                  double qty, std::string& orderId, std::string& error) {
        return placeLimitOrder(symbol, side, qty, 0.0, orderId, error);
    }

    struct OrderInfo {
        std::string state;  // "filled", "live", "partially_filled", "canceled"
        double filledQty = 0.0;
        double avgPx = 0.0;
    };
    virtual std::optional<OrderInfo> fetchOrderStatus(const std::string& /*symbol*/,
                                                     const std::string& /*orderId*/) {
        return std::nullopt;
    }
};

}  // namespace hftarb
