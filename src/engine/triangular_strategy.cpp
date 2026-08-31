#include "src/engine/triangular_strategy.h"

#include <algorithm>
#include <stdexcept>

#include "src/core/order_book.h"
#include "src/core/timestamp.h"

namespace hftarb {

TriangularStrategy::TriangularStrategy(AppConfig::TriangleConfig cfg, CostEngine cost,
                                       RiskEngine risk)
    : cfg_(std::move(cfg)), cost_(std::move(cost)), risk_(std::move(risk)) {
    if (cfg_.legs.size() != 3) {
        throw std::runtime_error("triangle '" + cfg_.name + "' must define exactly 3 legs");
    }
    for (const auto& l : cfg_.legs) {
        if (l.symbol.empty()) throw std::runtime_error("triangle '" + cfg_.name + "' has an empty leg symbol");
    }
    if (cfg_.name.empty()) {
        std::string joined;
        for (const auto& l : cfg_.legs) {
            if (!joined.empty()) joined += "+";
            joined += l.symbol;
        }
        cfg_.name = joined;
    }
}

bool TriangularStrategy::hasSymbol(const std::string& symbol) const {
    for (const auto& l : cfg_.legs) {
        if (l.symbol == symbol) return true;
    }
    return false;
}

void TriangularStrategy::onTick(const Tick& tick) {
    if (!hasSymbol(tick.symbol)) return;
    const std::int64_t nowMs = Timestamps::monoMs();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        OrderBook& slot = books_[tick.symbol];
        // Keep whichever book is fresher when leg ticks race in.
        if (tick.book.localReceiveTs >= slot.localReceiveTs) slot = tick.book;
    }
    evaluate(nowMs);
}

std::optional<OrderBook> TriangularStrategy::book(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = books_.find(symbol);
    if (it == books_.end()) return std::nullopt;
    return it->second;
}

std::size_t TriangularStrategy::bookCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return books_.size();
}

void TriangularStrategy::addExposure(const std::string& symbol, double qty) {
    (void)symbol;
    addExposure(qty);
}

void TriangularStrategy::clearExposure(const std::string& symbol) {
    (void)symbol;
    exposure_.store(0.0);
}

void TriangularStrategy::addExposure(double qty) {
    double cur = exposure_.load();
    while (!exposure_.compare_exchange_weak(cur, cur + qty)) {
    }
}

std::optional<double> TriangularStrategy::computeCycle(
    const OrderBook& b0, const OrderBook& b1, const OrderBook& b2,
    const FeeSchedule& fees, std::vector<OrderLeg>& legs,
    CostBreakdown& cost) const {
    const double f1 = fees.takerBuyFee;   // legs 0..1 are buys
    const double f2 = fees.takerBuyFee;
    const double f3 = fees.takerSellFee;  // leg 2 is a sell

    // Size each leg from the entry notional at the last known prices, then walk
    // the books at those sizes and refine once (VWAP -> qty -> VWAP). Mock
    // books carry ~10 levels and telescoping slippage is tiny, so one pass is a
    // solid fixed point.
    Price p1 = b0.bestAsk() ? *b0.bestAsk() : 0.0;
    Price p2 = b1.bestAsk() ? *b1.bestAsk() : 0.0;
    Price p3 = b2.bestBid() ? *b2.bestBid() : 0.0;
    if (p1 <= 0.0 || p2 <= 0.0 || p3 <= 0.0) return std::nullopt;

    const double N = cfg_.notional;
    double q0 = N / p1;
    double q1 = q0 * (1.0 - f1) / p2;
    double q2 = q1 * (1.0 - f2);

    if (OrderBookUtils::availableQty(b0.asks) < q0 ||
        OrderBookUtils::availableQty(b1.asks) < q1 ||
        OrderBookUtils::availableQty(b2.bids) < q2) {
        return std::nullopt;
    }
    const auto A = OrderBookUtils::avgBuyPrice(b0, q0);
    const auto B = OrderBookUtils::avgBuyPrice(b1, q1);
    const auto C = OrderBookUtils::avgSellPrice(b2, q2);
    if (!A || !B || !C) return std::nullopt;

    p1 = *A;
    p2 = *B;
    p3 = *C;
    q0 = N / p1;
    q1 = q0 * (1.0 - f1) / p2;
    q2 = q1 * (1.0 - f2);

    // Re-check liquidity at the refined sizes.
    if (OrderBookUtils::availableQty(b0.asks) < q0 ||
        OrderBookUtils::availableQty(b1.asks) < q1 ||
        OrderBookUtils::availableQty(b2.bids) < q2) {
        return std::nullopt;
    }

    // Round-trip rate: entry notional converted X->Y->Z->X with per-leg taker
    // fee. Rate >= 1 means the cycle returns more start asset than it took.
    const double feeFreeRate = p3 / (p1 * p2);
    const double feeFactor = (1.0 - f1) * (1.0 - f2) * (1.0 - f3);
    const double rate = feeFreeRate * feeFactor;

    cost.quantity = q0;
    cost.buyPrice = p1;
    cost.sellPrice = p3;
    cost.notional = N;
    cost.grossSpread = feeFreeRate - 1.0;
    cost.buyFee = f1;
    cost.sellFee = f3;
    cost.slippage = OrderBookUtils::buySlippage(b0, q0) +
                    OrderBookUtils::buySlippage(b1, q1) +
                    OrderBookUtils::sellSlippage(b2, q2);
    cost.networkCost = cost_.config().networkCostForSpot;
    cost.safetyMargin = cost_.config().safetyMargin;
    cost.netSpread = rate - 1.0 - cost.networkCost - cost.safetyMargin;

    legs.clear();
    legs.push_back({cfg_.exchange, cfg_.legs[0].symbol, cfg_.legs[0].side, p1, q0});
    legs.push_back({cfg_.exchange, cfg_.legs[1].symbol, cfg_.legs[1].side, p2, q1});
    legs.push_back({cfg_.exchange, cfg_.legs[2].symbol, cfg_.legs[2].side, p3, q2});

    return rate;
}

void TriangularStrategy::evaluate(std::int64_t nowMs) {
    std::lock_guard<std::mutex> evalLock(evalMutex_);

    OrderBook b0, b1, b2;
    FeeSchedule fees;
    std::shared_ptr<Portfolio> portfolio;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto i0 = books_.find(cfg_.legs[0].symbol);
        const auto i1 = books_.find(cfg_.legs[1].symbol);
        const auto i2 = books_.find(cfg_.legs[2].symbol);
        if (i0 == books_.end() || i1 == books_.end() || i2 == books_.end()) return;
        b0 = i0->second;
        b1 = i1->second;
        b2 = i2->second;
        fees = fees_;
        portfolio = portfolio_;
    }

    std::vector<OrderLeg> legs;
    CostBreakdown cost;
    const auto rate = computeCycle(b0, b1, b2, fees, legs, cost);
    lastRate_.store(rate ? *rate : 0.0);
    if (!rate || *rate <= 1.0) return;  // not executable or no edge: silence

    Opportunity opp;
    opp.strategy = name_;
    opp.symbol = cfg_.name;
    opp.buyExchange = cfg_.exchange;
    opp.sellExchange = cfg_.exchange;
    opp.quantity = cost.quantity;
    opp.cost = cost;
    opp.legs = std::move(legs);
    opp.decisionTs = nowMs;
    opp.roundTripLatencyMs =
        static_cast<double>(nowMs - std::max({b0.localReceiveTs, b1.localReceiveTs,
                                              b2.localReceiveTs}));

    if (!cost_.meetsThreshold(cost)) {
        opp.decision = Decision::Rejected;
        opp.rejectReason = "below_threshold";
        if (emit_) emit_(opp);
        return;
    }
    if (portfolio && cfg_.startAsset == "USDT" &&
        portfolio->quote(cfg_.exchange) < cfg_.notional) {
        opp.decision = Decision::InsufficientFunds;
        opp.rejectReason = "insufficient_funds";
        if (emit_) emit_(opp);
        return;
    }

    // Shared risk gate (staleness, latency, size, exposure, cooldown, rate
    // limit) — identical to the cross-exchange path.
    const std::function<bool(const std::string&)>& isConn =
        connected_ ? connected_ : [](const std::string&) { return true; };
    const std::int64_t bestReceive =
        std::max({b0.localReceiveTs, b1.localReceiveTs, b2.localReceiveTs});
    RiskState state{isConn, exposure(""), nowMs, bestReceive};
    opp.decision = risk_.evaluate(opp, state);
    opp.rejectReason = risk_.lastReason();

    if (opp.decision == Decision::Accepted) {
        risk_.recordTrade(opp.symbol, nowMs);
        addExposure(cost.quantity);
    }
    if (emit_) emit_(opp);
}

}  // namespace hftarb