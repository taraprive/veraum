#include "src/execution/paper_executor.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "src/core/timestamp.h"

namespace hftarb {

PaperExecutor::PaperExecutor(PaperConfig cfg, unsigned seed)
    : cfg_(cfg), rng_(seed) {}

double PaperExecutor::sampleFill(double baseRate, double quantity) const {
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::uniform_real_distribution<double> partial(0.4, 0.9);

    if (u(rng_) < cfg_.rejectProbability) return 0.0;
    if (u(rng_) < cfg_.partialProbability) {
        // Partial: between 40% and 90% of the requested size.
        return quantity * partial(rng_);
    }
    return (u(rng_) < baseRate) ? quantity : quantity * 0.9;
}

double PaperExecutor::simulatePriceDrift(double basePrice) const {
    std::uniform_real_distribution<double> drift(-cfg_.priceMoveBp, cfg_.priceMoveBp);
    return basePrice * (1.0 + drift(rng_) / 1e4);
}

double PaperExecutor::sampleAdverseMove(double edgeBp) const {
    if (edgeBp <= 0.0) return 0.0;
    // The edge decays while we execute: others race to take it. Sample a
    // fraction of the edge (0.5x..1.5x of the configured share) that moves
    // against our legs during the delay.
    std::uniform_real_distribution<double> u(0.5, 1.5);
    return (edgeBp * cfg_.adverseEdgeFrac * u(rng_)) / 1e4;
}

ExecutionResult PaperExecutor::execute(const Opportunity& opp) {
    if (!opp.legs.empty()) return executeLegs(opp);
    ExecutionResult r;
    r.id = "paper_" + std::to_string(++seq_);
    r.symbol = opp.symbol;
    r.ts = Timestamps::wallMs();
    r.requestedQty = opp.quantity;

    // Edge observed on the book at decision time, in bp.
    const double edgeBp = opp.cost.buyPrice > 0.0
                              ? (opp.cost.sellPrice - opp.cost.buyPrice) / opp.cost.buyPrice * 1e4
                              : 0.0;

    // Adverse selection: while the legs execute, the book moves against us.
    // The buy leg gets a rising ask, the sell leg a falling bid. Realized
    // prices therefore land worse than the snapshot we traded on, so PnL per
    // trade shrinks exactly the way it does against real competition.
    const double advBuy = sampleAdverseMove(edgeBp);
    const double advSell = sampleAdverseMove(edgeBp);

    // The sell leg is the slower side of the race: its fill probability also
    // drops as the sell price sags (we are less likely to get a good fill).
    std::uniform_real_distribution<double> penaltyNoise(0.5, 1.0);
    const double sellFillPenalty =
        edgeBp > 0.0 ? std::min(1.0, (advSell * 1e4) / edgeBp) * 0.3 * penaltyNoise(rng_)
                     : 0.0;

    const double buyFilled = sampleFill(cfg_.fillRateBuy, opp.quantity);
    const double sellFilled =
        sampleFill(cfg_.fillRateSell * (1.0 - sellFillPenalty), opp.quantity);

    if (buyFilled <= 0.0 || sellFilled <= 0.0) {
        r.status = "rejected";
        r.realizedPnl = 0.0;
        r.feesPaid = 0.0;
        r.exposureLeft = 0.0;
        r.fullyFilled = false;
        if (emit_) emit_(r);
        return r;
    }

    // Price moves against us during the simulated execution delay.
    const Price buyPx = simulatePriceDrift(opp.cost.buyPrice) * (1.0 + advBuy);
    const Price sellPx = simulatePriceDrift(opp.cost.sellPrice) * (1.0 - advSell);

    r.avgBuyPrice = buyPx;
    r.avgSellPrice = sellPx;
    r.buyFilled = buyFilled;
    r.sellFilled = sellFilled;
    r.exposureLeft = buyFilled - sellFilled;

    // Fees are charged on the actual notional of each leg.
    const double buyNotional = buyPx * buyFilled;
    const double sellNotional = sellPx * sellFilled;
    r.feesPaid = buyNotional * opp.cost.buyFee + sellNotional * opp.cost.sellFee;

    // Realized PnL only on the quantity that round-tripped (min of both legs).
    // The leftover inventory is an open position carried at its buy cost, NOT
    // a realized loss: selling 0.0003 of the 0.0004 bought is not a $10 loss,
    // the remaining 0.0001 is simply still on the books.
    const double matched = std::min(buyFilled, sellFilled);
    r.realizedPnl = matched * (sellPx - buyPx) -
                    matched * (buyPx * opp.cost.buyFee + sellPx * opp.cost.sellFee);
    r.fullyFilled = (buyFilled == opp.quantity) && (sellFilled == opp.quantity);
    r.status = r.fullyFilled ? "filled" : (buyFilled > 0.0 ? "partial" : "rejected");

    if (emit_) emit_(r);
    return r;
}

ExecutionResult PaperExecutor::executeLegs(const Opportunity& opp) {
    ExecutionResult r;
    r.id = "paper_" + std::to_string(++seq_);
    r.symbol = opp.symbol;
    r.ts = Timestamps::wallMs();

    const auto& legs = opp.legs;
    if (legs.size() != 3) {
        r.status = "rejected";
        r.rejectReason = "need 3 legs";
        if (emit_) emit_(r);
        return r;
    }

    const double buyFee = opp.cost.buyFee;
    const double sellFee = opp.cost.sellFee;
    const double edgeBp = opp.cost.grossSpread > 0.0 ? opp.cost.grossSpread * 1e4 : 0.0;

    // Each leg converts *everything currently held* into the next asset; a
    // reject halts the cycle and leaves the held asset as open inventory.
    r.requestedQty = legs[0].quantity;

    // Leg 0 (buy): quote -> base0.
    const double f0 = sampleFill(cfg_.fillRateBuy, 1.0);
    if (f0 <= 0.0) {
        r.status = "rejected";
        r.rejectReason = "leg0 rejected";
        if (emit_) emit_(r);
        return r;
    }
    const Price p0 = simulatePriceDrift(legs[0].price) *
                     (1.0 + sampleAdverseMove(edgeBp) / 3.0);
    const double q0 = legs[0].quantity * f0;
    double heldX = q0 * (1.0 - buyFee);
    double spentQuote = q0 * p0;

    // Leg 1 (buy): base0 -> base1, requires heldX.
    const double f1 = sampleFill(cfg_.fillRateBuy, 1.0);
    if (f1 <= 0.0) {
        r.avgBuyPrice = p0;
        r.buyFilled = q0;
        r.exposureLeft = heldX * p0;  // stuck in base0, marked at p0
        r.realizedPnl = 0.0;
        r.status = "partial";
        r.rejectReason = "leg1 rejected after leg0";
        if (emit_) emit_(r);
        return r;
    }
    const Price p1 = simulatePriceDrift(legs[1].price) *
                     (1.0 + sampleAdverseMove(edgeBp) / 3.0);
    double q1 = legs[1].quantity * f1;
    if (q1 * p1 > heldX) q1 = heldX / p1;      // cannot convert base we don't hold
    double heldZ = q1 * (1.0 - buyFee);
    double leftoverX = heldX - q1 * p1;

    // Leg 2 (sell): base1 -> quote.
    std::uniform_real_distribution<double> penaltyNoise(0.5, 1.0);
    const double sellPenalty =
        edgeBp > 0.0 ? std::min(1.0, (sampleAdverseMove(edgeBp) * 1e4) / edgeBp) *
                          0.3 * penaltyNoise(rng_)
                     : 0.0;
    const double f2 = sampleFill(cfg_.fillRateSell * (1.0 - sellPenalty), 1.0);
    if (f2 <= 0.0) {
        r.avgBuyPrice = p0;
        r.avgSellPrice = p1;
        r.buyFilled = q0;
        r.sellFilled = 0.0;
        r.exposureLeft = leftoverX * p0 + heldZ;  // inventory, base1 marked 1:1
        r.realizedPnl = 0.0;
        r.status = "partial";
        r.rejectReason = "leg2 rejected after leg1";
        if (emit_) emit_(r);
        return r;
    }
    const Price p2 = simulatePriceDrift(legs[2].price) *
                     (1.0 - sampleAdverseMove(edgeBp) / 3.0);
    double q2 = legs[2].quantity * f2;
    if (q2 > heldZ) q2 = heldZ;
    const double leftoverZ = heldZ - q2;
    const double receivedQuote = q2 * p2 * (1.0 - sellFee);

    r.avgBuyPrice = p0;
    r.avgSellPrice = p2;
    r.buyFilled = q0;
    r.sellFilled = q2;
    r.exposureLeft = leftoverX * p0 + leftoverZ * p2;
    r.realizedPnl = receivedQuote - spentQuote;
    r.feesPaid = q0 * p0 * buyFee + q1 * p1 * buyFee + q2 * p2 * sellFee;
    r.fullyFilled = (q0 >= legs[0].quantity && q1 >= legs[1].quantity &&
                     q2 >= legs[2].quantity);
    r.status = r.fullyFilled ? "filled"
                             : (receivedQuote > 0.0 && spentQuote > 0.0 ? "partial" : "rejected");

    if (emit_) emit_(r);
    return r;
}

}  // namespace hftarb
