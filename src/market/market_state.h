#pragma once

#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>

#include "src/core/types.h"

namespace hftarb {

// Per-symbol market parameters: starting price and top-of-book liquidity.
struct SymbolMarketParams {
    double basePrice = 100000.0;    // starting fair mid
    double liquidityTopQty = 0.1;   // base qty resting at the best level
};

// Global cross-exchange market dynamics shared by every venue.
struct MarketDynamics {
    double volBp = 0.6;            // per-update global volatility (bp)
    double driftBp = 0.0;          // per-update drift (bp)
    double basisVolBp = 0.9;       // per-update per-exchange basis noise (bp)
    double basisReversion = 0.08;  // mean-reversion speed of the basis
    double spreadMinBp = 1.0;      // spread clamp (bp)
    double spreadMaxBp = 4.0;      // spread clamp (bp)
    double spreadWalkBp = 0.25;    // per-update spread random walk (bp)
    double burstProbability = 0.015;  // chance of an inefficiency burst per update
    double burstBp = 60.0;            // burst deviation magnitude (bp)
    int burstMinTicks = 1;            // burst lifetime range (updates)
    int burstMaxTicks = 4;
};

// Shared simulated market. Every exchange feed draws from the SAME underlying
// price process (drift + vol), so venues stay correlated as in reality. Each
// exchange adds its own small mean-reverting basis plus spread, and
// occasionally a "burst" drives one venue far off fair for a few updates — a
// realistic, fleeting cross-exchange arbitrage window.
class MarketState {
public:
    MarketState(MarketDynamics dyn,
                std::unordered_map<std::string, SymbolMarketParams> symbols,
                unsigned seed);

    // Produces the current book for (exchange, symbol), advancing the global
    // simulation to nowMs in updateMs steps. Thread-safe; the underlying
    // process is advanced once per symbol regardless of which feed asks.
    OrderBook tick(const std::string& exchange, const std::string& symbol,
                   std::int64_t nowMs, std::int64_t updateMs);

    // Per-exchange spread override (from exchanges.json).
    void setExchangeSpreadBp(const std::string& exchange, double spreadBp);

    // Current fair mid (consistent with tick()).
    double midPrice(const std::string& symbol) const;

private:
    struct ExchangeState {
        double basis = 0.0;    // mean-reverting deviation from fair (bp)
        double spreadBp = 2.0;
        double burst = 0.0;    // active inefficiency deviation (bp)
        int burstLeft = 0;     // updates remaining
    };
    struct SymbolState {
        SymbolMarketParams params;
        double mid = 0.0;
        std::int64_t lastTickMs = 0;
        std::unordered_map<std::string, ExchangeState> exch;
    };

    void advanceSymbol(SymbolState& s, std::int64_t nowMs, std::int64_t updateMs);
    OrderBook buildBook(SymbolState& s, const std::string& exchange,
                        const std::string& symbol, std::int64_t nowMs);

    MarketDynamics dyn_;
    std::unordered_map<std::string, SymbolState> symbols_;
    std::unordered_map<std::string, double> spreadOverrides_;
    mutable std::mutex mutex_;
    std::mt19937 rng_;
    std::normal_distribution<double> norm_{0.0, 1.0};
    std::uniform_real_distribution<double> unit_{0.0, 1.0};
    std::uniform_real_distribution<double> sizeNoise_{0.5, 2.0};
};

}  // namespace hftarb
