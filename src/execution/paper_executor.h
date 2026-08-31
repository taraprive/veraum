#pragma once

#include <cstdint>
#include <functional>
#include <random>

#include "src/core/types.h"

namespace hftarb {

struct PaperConfig {
    double fillRateBuy = 0.97;      // base probability legs fill completely
    double fillRateSell = 0.93;
    double partialProbability = 0.05;  // chance of a partial fill
    double rejectProbability = 0.01;   // chance the sim rejects the order
    double delayMs = 20.0;             // simulated execution delay
    double priceMoveBp = 0.5;          // worst-case price drift during delay
    double adverseEdgeFrac = 0.35;     // fraction of the observed edge consumed
                                       // adversely during the delay (0 = naive)
};

// Simulates what *would* happen if the pair of orders were actually sent:
// latency delay, price drift, full / partial / rejected fills, and the
// resulting leftover exposure. This is a safe PAPER execution path — no real
// orders are ever placed by the scaffold.
class PaperExecutor {
public:
    using ResultCallback = std::function<void(const ExecutionResult&)>;

    explicit PaperExecutor(PaperConfig cfg, unsigned seed = 42);

    // Synchronous simulation; returns the result directly.
    ExecutionResult execute(const Opportunity& opp);

    void setResultCallback(ResultCallback cb) { emit_ = std::move(cb); }

private:
    double sampleFill(double baseRate, double quantity) const;
    double simulatePriceDrift(double basePrice) const;
    // Adverse move (fraction of notional) applied to a leg during the delay.
    double sampleAdverseMove(double edgeBp) const;

    // Multi-leg route (triangular): converts the entry notional through each
    // leg in order, clamping each leg to what the previous one actually
    // produced, and reports the realized cycle PnL + any stuck inventory.
    ExecutionResult executeLegs(const Opportunity& opp);

    PaperConfig cfg_;
    mutable std::mt19937 rng_;
    std::int64_t seq_ = 0;
    ResultCallback emit_;
};

}  // namespace hftarb
