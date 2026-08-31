#include "src/market/market_state.h"

#include <algorithm>
#include <cmath>

#include "src/core/timestamp.h"

namespace hftarb {

namespace {
constexpr std::int64_t kMaxAdvanceTicks = 500;
}

MarketState::MarketState(MarketDynamics dyn,
                         std::unordered_map<std::string, SymbolMarketParams> symbols,
                         unsigned seed)
    : dyn_(dyn), rng_(seed) {
    for (auto& [sym, p] : symbols) {
        SymbolState s;
        s.params = p;
        s.mid = p.basePrice;
        s.lastTickMs = 0;
        symbols_[sym] = std::move(s);
    }
}

void MarketState::setExchangeSpreadBp(const std::string& exchange, double spreadBp) {
    std::lock_guard<std::mutex> lock(mutex_);
    spreadOverrides_[exchange] = spreadBp;
}

double MarketState::midPrice(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = symbols_.find(symbol);
    return it != symbols_.end() ? it->second.mid : 0.0;
}

void MarketState::advanceSymbol(SymbolState& s, std::int64_t nowMs, std::int64_t updateMs) {
    // Lazy start: anchor the process to the first request time so the
    // simulation does not simulate "since epoch" on the first tick.
    if (s.lastTickMs == 0) {
        s.lastTickMs = nowMs;
        return;
    }
    const std::int64_t elapsed = std::max<std::int64_t>(0, nowMs - s.lastTickMs);
    std::int64_t ticks = updateMs > 0 ? elapsed / updateMs : 1;
    if (ticks <= 0) ticks = 1;
    ticks = std::min(ticks, kMaxAdvanceTicks);

    const double drift = dyn_.driftBp / 1e4;
    const double vol = dyn_.volBp / 1e4;
    for (std::int64_t t = 0; t < ticks; ++t) {
        s.mid *= std::exp(drift + vol * norm_(rng_));
    }
    s.lastTickMs = nowMs;
}

OrderBook MarketState::buildBook(SymbolState& s, const std::string& exchange,
                                 const std::string& symbol, std::int64_t nowMs) {
    auto& es = s.exch[exchange];

    // Spread: pull toward the exchange override, plus a small random walk.
    const double target = spreadOverrides_.count(exchange) ? spreadOverrides_.at(exchange) : 2.0;
    es.spreadBp += 0.1 * (target - es.spreadBp) + dyn_.spreadWalkBp * norm_(rng_);
    const double lo = std::min(dyn_.spreadMinBp, target);
    const double hi = std::max(dyn_.spreadMaxBp, target);
    es.spreadBp = std::clamp(es.spreadBp, lo, hi);

    // Basis: Ornstein-Uhlenbeck, mean-reverting to zero.
    es.basis += dyn_.basisReversion * (0.0 - es.basis) + dyn_.basisVolBp * norm_(rng_);

    // Burst: while active it holds, then decays; otherwise maybe start one.
    if (es.burstLeft > 0) {
        --es.burstLeft;
        if (es.burstLeft == 0) es.burst = 0.0;
    } else if (unit_(rng_) < dyn_.burstProbability) {
        const int len = dyn_.burstMinTicks +
                        static_cast<int>(unit_(rng_) *
                                         (dyn_.burstMaxTicks - dyn_.burstMinTicks + 1));
        es.burstLeft = std::max(1, len);
        es.burst = unit_(rng_) < 0.5 ? -dyn_.burstBp : dyn_.burstBp;
    }

    const double deviationBp = es.basis + es.burst;
    const double fair = s.mid * std::exp(deviationBp / 1e4);
    const double halfSpread = es.spreadBp * fair / 1e4 / 2.0;

    OrderBook book;
    book.exchange = exchange;
    book.symbol = symbol;
    book.exchangeTs = nowMs;
    book.localReceiveTs = nowMs;

    // ~10 levels per side; qty and price-step grow away from the top.
    const double topQty = std::max(s.params.liquidityTopQty, 0.0);
    for (int i = 0; i < 10; ++i) {
        const double step = halfSpread * (0.8 + 1.2 * i);
        const double qty = topQty * (1.0 + i) * sizeNoise_(rng_);
        book.bids.push_back({fair - step, qty});
        book.asks.push_back({fair + step, qty});
    }
    return book;
}

OrderBook MarketState::tick(const std::string& exchange, const std::string& symbol,
                            std::int64_t nowMs, std::int64_t updateMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& s = symbols_[symbol];
    advanceSymbol(s, nowMs, updateMs);
    return buildBook(s, exchange, symbol, nowMs);
}

}  // namespace hftarb
