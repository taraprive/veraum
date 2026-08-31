#include "src/engine/arbitrage_engine.h"

#include <algorithm>

#include "src/core/timestamp.h"
#include "src/util/logger.h"

namespace hftarb {

ArbitrageEngine::ArbitrageEngine(ArbitrageConfig cfg, CostEngine cost,
                                 LiquidityEngine liquidity, RiskEngine risk)
    : cfg_(std::move(cfg)),
      cost_(std::move(cost)),
      liquidity_(std::move(liquidity)),
      risk_(std::move(risk)) {}

std::optional<OrderBook> ArbitrageEngine::book(const std::string& exchange,
                                               const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = books_.find(exchange + "|" + symbol);
    if (it == books_.end()) return std::nullopt;
    return it->second;
}

std::size_t ArbitrageEngine::bookCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return books_.size();
}

void ArbitrageEngine::onTick(const Tick& tick) {
    const std::int64_t nowMs = Timestamps::monoMs();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        OrderBook& slot = books_[tick.exchange + "|" + tick.symbol];
        // Keep whichever book is fresher when duplicates race in.
        if (tick.book.localReceiveTs >= slot.localReceiveTs) slot = tick.book;
    }
    evaluateSymbol(tick.symbol, nowMs);
}

void ArbitrageEngine::evaluateSymbol(const std::string& symbol, std::int64_t nowMs) {
    std::lock_guard<std::mutex> evalLock(evalMutex_);

    // 1) Collect every viable candidate (liquidity + cost threshold) for this
    //    symbol across all exchange pairs. Try multiple quantities from the
    //    configured size down to 10% so smaller capital still trades.
    std::vector<Candidate> candidates;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Build decreasing quantity ladder: 100%, 50%, 25%, 10%.
        std::vector<double> quantities;
        {
            double q = cfg_.tradeQuantity;
            for (int i = 0; i < 4 && q >= 0.00001; ++i) {
                quantities.push_back(q);
                q *= 0.5;
            }
        }

        for (const auto& buyEx : cfg_.exchanges) {
            for (const auto& sellEx : cfg_.exchanges) {
                if (buyEx == sellEx) continue;

                const auto bIt = books_.find(buyEx + "|" + symbol);
                const auto sIt = books_.find(sellEx + "|" + symbol);
                if (bIt == books_.end() || sIt == books_.end()) continue;

                for (double qty : quantities) {
                    // Liquidity first: is the size even tradable?
                    const LiquidityResult lq =
                        liquidity_.check(bIt->second, sIt->second, qty);
                    if (!lq.sufficient) continue;

                    // Full cost model.
                    const auto cost = cost_.evaluate(bIt->second, sIt->second, qty,
                                                     feeScheduleOf(buyEx), feeScheduleOf(sellEx));
                    if (!cost || !cost_.meetsThreshold(*cost)) continue;

                    const std::int64_t bestReceive = std::max(bIt->second.localReceiveTs,
                                                              sIt->second.localReceiveTs);
                    candidates.push_back(Candidate{symbol, buyEx, sellEx, qty,
                                                   *cost,
                                                   static_cast<double>(nowMs - bestReceive),
                                                   bestReceive});
                    // Don't break — try smaller sizes too so canTrade can
                    // pick one that fits within wallet limits.
                }
            }
        }
    }

    if (candidates.empty()) return;

    // 2) Best net edge first, so portfolio-aware fallback walks quality down.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.cost.netSpread > b.cost.netSpread;
              });

    const std::function<bool(const std::string&)>& isConn =
        connected_ ? connected_ : [](const std::string&) { return true; };

    std::shared_ptr<Portfolio> portfolio;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        portfolio = portfolio_;
    }

    // 3) Try candidates from best edge downward: skip pairs the accounts
    //    cannot fund, and let the risk gate decide on the first affordable one.
    //    If risk blocks it (cooldown/latency/...), fall through to the next
    //    affordable pair instead of giving up on the symbol.
    std::optional<Opportunity> diag;
    for (const Candidate& cand : candidates) {
        if (portfolio &&
            !portfolio->canTrade(cand.buyExchange, cand.sellExchange, cand.symbol,
                                 cand.cost.notional, cand.quantity)) {
            continue;
        }

        Opportunity opp;
        opp.strategy = name_;
        opp.symbol = cand.symbol;
        opp.buyExchange = cand.buyExchange;
        opp.sellExchange = cand.sellExchange;
        opp.quantity = cand.quantity;
        opp.cost = cand.cost;
        opp.roundTripLatencyMs = cand.roundTripLatencyMs;
        opp.decisionTs = nowMs;

        RiskState state{isConn, exposure(), nowMs, cand.bestReceiveMs};
        opp.decision = risk_.evaluate(opp, state);
        opp.rejectReason = risk_.lastReason();

        // Keep the best affordable outcome for diagnostics if nothing trades.
        if (!diag) diag = opp;

        if (opp.decision != Decision::Accepted) continue;

        risk_.recordTrade(opp.symbol, nowMs);
        addExposure(opp.quantity);
        if (emit_) emit_(opp);
        return;
    }

    // 4) Nothing traded this tick. Emit one diagnostic:
    //    - nothing affordable  -> best edge with insufficient funds
    //    - risk-blocked        -> best affordable candidate with its reason
    if (!diag) {
        Opportunity opp;
        opp.strategy = name_;
        opp.symbol = candidates.front().symbol;
        opp.buyExchange = candidates.front().buyExchange;
        opp.sellExchange = candidates.front().sellExchange;
        opp.quantity = candidates.front().quantity;
        opp.cost = candidates.front().cost;
        opp.roundTripLatencyMs = candidates.front().roundTripLatencyMs;
        opp.decisionTs = nowMs;
        opp.decision = Decision::InsufficientFunds;
        opp.rejectReason = "insufficient_funds";
        if (emit_) emit_(opp);
        return;
    }
    if (emit_) emit_(*diag);
}

}  // namespace hftarb
