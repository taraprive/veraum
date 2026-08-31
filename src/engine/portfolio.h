#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include "src/core/types.h"

namespace hftarb {

// Per-exchange account balances: quote (USDT) + base asset per symbol (BTC...).
// The arbitrage decision now answers the question the mock never asked:
// "can this exchange actually afford the order?" If not -> insufficient funds.
class Portfolio {
public:
    // Tuning knobs for the periodic inventory rebalancer.
    struct RebalanceConfig {
        int intervalSec = 30;    // how often to sweep accounts (0 = disabled)
        double thresholdUsd = 10.0;  // act only when a pair's imbalance is this large
        double maxSweepUsd = 25.0;   // cap on any single internal transfer (notional)
        double transferCostBp = 5.0; // on-chain-style fee applied to internal transfers
    };

    void setQuote(const std::string& exchange, double usdt);
    void setBase(const std::string& exchange, const std::string& symbol, double qty);

    double quote(const std::string& exchange) const;
    double base(const std::string& exchange, const std::string& symbol) const;
    // Largest single-account USDT balance.
    double maxQuote() const;
    // Sum of USDT across all exchanges (arb capital is pooled).
    double totalQuote() const;

    // True when the buy exchange can pay buyNotional AND the sell exchange can
    // deliver sellQty of base asset. No state change.
    bool canTrade(const std::string& buyExchange, const std::string& sellExchange,
                  const std::string& symbol, double buyNotional, double sellQty) const;

    // Applies the actual (possibly partial) fills of a paper execution.
    void applyFill(const std::string& buyExchange, const std::string& sellExchange,
                   const std::string& symbol, double buyFilled, Price avgBuyPrice,
                   double sellFilled, Price avgSellPrice);

    // Internal transfers of base asset between accounts at `midPrice` (no PnL),
    // converging each account toward an equal USDT/base distribution so arb
    // capital never strands on one venue while another is short. Returns the
    // number of transfers executed this sweep.
    int rebalance(const std::function<double(const std::string&)>& midPrice,
                  const RebalanceConfig& r);

    std::string snapshot(const std::string& exchange) const;

    // Structured view of every account, for persistence across runs.
    struct AccountView {
        double usdt = 0.0;
        std::unordered_map<std::string, double> base;  // symbol -> qty
    };
    std::unordered_map<std::string, AccountView> snapshotAll() const;

private:
    struct Account {
        double usdt = 0.0;
        std::unordered_map<std::string, double> base;  // symbol -> qty
    };

    Account& account(const std::string& exchange);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Account> accounts_;
};

}  // namespace hftarb
