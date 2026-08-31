#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hftarb {

using Price = double;
using Quantity = double;
using MsTimestamp = std::int64_t;  // milliseconds since epoch (wall) or arbitrary base

enum class Side : std::uint8_t {
    Buy = 0,
    Sell = 1,
};

enum class Decision : std::uint8_t {
    None = 0,
    Accepted,
    Rejected,
    StaleData,
    LowLiquidity,
    RiskRejected,
    LatencyRejected,
    SizeRejected,
    Disconnected,
    Cooldown,
    InsufficientFunds,
};

struct Level {
    Price price = 0.0;
    Quantity quantity = 0.0;
};

// A normalized, exchange-agnostic view of one side of the order book.
struct OrderBook {
    std::string exchange;
    std::string symbol;
    std::vector<Level> bids;  // best first (descending price)
    std::vector<Level> asks;  // best first (ascending price)
    MsTimestamp exchangeTs = 0;  // as reported by the exchange
    MsTimestamp localReceiveTs = 0;  // when we received the message

    std::optional<Price> bestBid() const {
        if (bids.empty() || bids.front().quantity <= 0.0) return std::nullopt;
        return bids.front().price;
    }
    std::optional<Price> bestAsk() const {
        if (asks.empty() || asks.front().quantity <= 0.0) return std::nullopt;
        return asks.front().price;
    }
    bool valid() const { return bestBid().has_value() && bestAsk().has_value(); }
};

// Single normalized market data message delivered to the pipeline.
struct Tick {
    std::string exchange;
    std::string symbol;
    OrderBook book;
    MsTimestamp exchangeTs = 0;   // exchange timestamp
    MsTimestamp receiveTs = 0;    // local receive timestamp (stage 1)
    MsTimestamp parseTs = 0;      // local parsing finished (stage 2)
};

// Cost breakdown for one candidate arbitrage leg pair.
// One step of a multi-leg route (e.g. a same-venue triangular cycle). Filled
// in by the strategy that computed the legs so the executor can replay them
// verbatim: which venue, which pair, which direction, and the expected fill
// price + quantity the strategy priced them at.
struct OrderLeg {
    std::string exchange;
    std::string symbol;
    Side side = Side::Buy;
    Price price = 0.0;         // expected average fill price (VWAP over qty)
    Quantity quantity = 0.0;   // qty of the pair's base asset to trade
};

struct CostBreakdown {
    Price buyPrice = 0.0;        // expected avg buy price (VWAP over required qty)
    Price sellPrice = 0.0;       // expected avg sell price (VWAP over required qty)
    Quantity quantity = 0.0;
    double grossSpread = 0.0;    // sell/buy - 1
    double buyFee = 0.0;         // fraction
    double sellFee = 0.0;        // fraction
    double slippage = 0.0;       // fraction (estimated from book depth)
    double networkCost = 0.0;    // fraction of notional, 0 for intra-exchange spot
    double safetyMargin = 0.0;   // fraction
    double netSpread = 0.0;      // gross - fees - slippage - network - margin
    double notional = 0.0;       // buyPrice * quantity
};

// A fully evaluated arbitrage opportunity (pre risk gate). Filled in by the
// strategy that produced it so the recorder/executor keep attribution even in
// a multi-strategy pipeline.
struct Opportunity {
    std::string strategy;      // e.g. "cross_exchange" | "triangular" | "funding_rate"
    std::string symbol;
    std::string buyExchange;
    std::string sellExchange;
    Quantity quantity = 0.0;
    CostBreakdown cost;
    MsTimestamp decisionTs = 0;
    // Non-empty = a multi-leg route (triangular/funding). When set, the legs
    // fully describe the trade (venue, pair, side, price, qty) and replace the
    // legacy two-leg interpretation of buyExchange/sellExchange/quantity, which
    // still carry the first-leg provider and the last-leg receiver for the
    // recorder/attribution.
    std::vector<OrderLeg> legs;
    double roundTripLatencyMs = 0.0;  // time from best tick receive to decision
    Decision decision = Decision::None;
    std::string rejectReason;
};

// Result of a simulated (paper) execution.
struct ExecutionResult {
    std::string id;
    std::string symbol;
    MsTimestamp ts = 0;
    Quantity requestedQty = 0.0;
    Quantity buyFilled = 0.0;
    Quantity sellFilled = 0.0;
    Price avgBuyPrice = 0.0;
    Price avgSellPrice = 0.0;
    double realizedPnl = 0.0;
    double feesPaid = 0.0;
    double exposureLeft = 0.0;   // buy_filled - sell_filled
    bool fullyFilled = false;
    std::string status;          // "filled", "partial", "rejected", "delayed"
    std::string rejectReason;
};

// Stage names for latency instrumentation. Every number is in the same units.
enum class LatencyStage : std::uint8_t {
    ExchangeToLocal = 0,
    LocalToParse,
    ParseToOrderBook,
    OrderBookToArbitrage,
    ArbitrageToDecision,
    DecisionToPaper,
    Count,
};

inline const char* stageName(LatencyStage s) {
    static const char* names[] = {
        "exchange_to_local", "local_to_parse", "parse_to_order_book",
        "order_book_to_arbitrage", "arbitrage_to_decision", "decision_to_paper"};
    return names[static_cast<int>(s)];
}

}  // namespace hftarb
