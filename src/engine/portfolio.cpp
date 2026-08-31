#include "src/engine/portfolio.h"

#include <algorithm>
#include <vector>

namespace hftarb {

Portfolio::Account& Portfolio::account(const std::string& exchange) {
    return accounts_[exchange];
}

void Portfolio::setQuote(const std::string& exchange, double usdt) {
    std::lock_guard<std::mutex> lock(mutex_);
    account(exchange).usdt = usdt;
}

void Portfolio::setBase(const std::string& exchange, const std::string& symbol, double qty) {
    std::lock_guard<std::mutex> lock(mutex_);
    account(exchange).base[symbol] = qty;
}

double Portfolio::quote(const std::string& exchange) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = accounts_.find(exchange);
    return (it != accounts_.end()) ? it->second.usdt : 0.0;
}

double Portfolio::base(const std::string& exchange, const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = accounts_.find(exchange);
    if (it == accounts_.end()) return 0.0;
    const auto bIt = it->second.base.find(symbol);
    return (bIt != it->second.base.end()) ? bIt->second : 0.0;
}

double Portfolio::maxQuote() const {
    std::lock_guard<std::mutex> lock(mutex_);
    double max = 0.0;
    for (const auto& [ex, acct] : accounts_) max = std::max(max, acct.usdt);
    return max;
}

double Portfolio::totalQuote() const {
    std::lock_guard<std::mutex> lock(mutex_);
    double total = 0.0;
    for (const auto& [ex, acct] : accounts_) total += acct.usdt;
    return total;
}

bool Portfolio::canTrade(const std::string& buyExchange, const std::string& sellExchange,
                         const std::string& symbol, double buyNotional, double sellQty) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto buy = accounts_.find(buyExchange);
    const auto sell = accounts_.find(sellExchange);
    if (buy == accounts_.end() || sell == accounts_.end()) return false;
    if (buy->second.usdt < buyNotional) return false;
    const auto bIt = sell->second.base.find(symbol);
    if (bIt == sell->second.base.end() || bIt->second < sellQty) return false;
    return true;
}

void Portfolio::applyFill(const std::string& buyExchange, const std::string& sellExchange,
                          const std::string& symbol, double buyFilled, Price avgBuyPrice,
                          double sellFilled, Price avgSellPrice) {
    std::lock_guard<std::mutex> lock(mutex_);
    Account& buy = account(buyExchange);
    Account& sell = account(sellExchange);

    // Buy leg: USDT out, base in.
    buy.usdt -= buyFilled * avgBuyPrice;
    buy.base[symbol] += buyFilled;

    // Sell leg: base out, USDT in.
    sell.base[symbol] -= sellFilled;
    sell.usdt += sellFilled * avgSellPrice;
}

std::string Portfolio::snapshot(const std::string& exchange) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = accounts_.find(exchange);
    if (it == accounts_.end()) return exchange + ":(no account)";
    std::string out = exchange + " usdt=" + std::to_string(it->second.usdt);
    for (const auto& [sym, qty] : it->second.base) {
        out += " " + sym + "=" + std::to_string(qty);
    }
    return out;
}

std::unordered_map<std::string, Portfolio::AccountView> Portfolio::snapshotAll() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::unordered_map<std::string, AccountView> out;
    out.reserve(accounts_.size());
    for (const auto& [ex, acct] : accounts_) {
        out[ex].usdt = acct.usdt;
        out[ex].base = acct.base;
    }
    return out;
}

int Portfolio::rebalance(const std::function<double(const std::string&)>& midPrice,
                         const RebalanceConfig& r) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (accounts_.size() < 2) return 0;

    int transfers = 0;
    std::vector<std::string> names;
    names.reserve(accounts_.size());
    for (const auto& [ex, acct] : accounts_) names.push_back(ex);

    // For every unordered account pair, move base from the account that holds
    // an excess of it (and is short USDT) to the account that holds the USDT
    // surplus (and is short base). The transfer settles at mid price so it
    // carries no PnL; it only re-distributes liquidity for future arbitrage.
    for (size_t i = 0; i < names.size(); ++i) {
        for (size_t j = i + 1; j < names.size(); ++j) {
            Account& a = accounts_[names[i]];
            Account& b = accounts_[names[j]];
            for (auto& [sym, qtyA] : a.base) {
                const auto bIt = b.base.find(sym);
                if (bIt == b.base.end()) continue;
                const double price = midPrice(sym);
                if (price <= 0.0) continue;

                Account* surplus = nullptr;  // excess base, short USDT
                Account* deficit = nullptr;  // excess USDT, short base
                double baseExcess = 0.0;
                double usdtExcess = 0.0;
                if (qtyA > bIt->second && a.usdt < b.usdt) {
                    surplus = &a; deficit = &b;
                    baseExcess = qtyA - bIt->second;
                    usdtExcess = b.usdt - a.usdt;
                } else if (bIt->second > qtyA && b.usdt < a.usdt) {
                    surplus = &b; deficit = &a;
                    baseExcess = bIt->second - qtyA;
                    usdtExcess = a.usdt - b.usdt;
                } else {
                    continue;
                }

                // Move half the gap (converge slowly, never overshoot), capped
                // so the payer keeps enough USDT for price + transfer fee.
                const double costBp = std::max(0.0, r.transferCostBp) / 1e4;
                const double qty = std::min({
                    baseExcess / 2.0,
                    usdtExcess / 2.0 / price,
                    r.maxSweepUsd / price,
                    deficit->usdt / (price * (1.0 + costBp))});
                if (qty <= 0.0 || qty * price < r.thresholdUsd) continue;

                // Realistic inventory management: moving base between venues
                // costs an on-chain-style fee, paid by the receiving account.
                const double transferFee = qty * price * costBp;
                surplus->base[sym] -= qty;
                surplus->usdt += qty * price;
                deficit->base[sym] += qty;
                deficit->usdt -= qty * price + transferFee;
                ++transfers;
            }
        }
    }
    return transfers;
}

}  // namespace hftarb
