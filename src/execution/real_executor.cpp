#include "src/execution/real_executor.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <thread>

#include "src/core/timestamp.h"
#include "src/util/logger.h"

namespace hftarb {

// Round price and qty to precision rules per symbol (Binance/MEXC filters).
static double roundToTick(double price, const std::string& symbol) {
    // Micro-caps with 1e-8/1e-9 tick sizes (PEPE, SHIB).
    if (symbol.find("PEPE") != std::string::npos) {
        return std::round(price * 1e8) / 1e8;
    }
    if (symbol.find("SHIB") != std::string::npos) {
        return std::round(price * 1e9) / 1e9;
    }
    // Small pairs: SEI/OP tick=0.0001.
    if (symbol.find("SEI") != std::string::npos || symbol.find("OP/") != std::string::npos) {
        return std::round(price * 10000.0) / 10000.0;
    }
    return std::round(price * 100.0) / 100.0;
}

static double roundToLot(double qty, const std::string& symbol) {
    // Micro-caps trade in whole units / blocks of 100.
    if (symbol.find("PEPE") != std::string::npos) {
        return std::round(qty);
    }
    if (symbol.find("SHIB") != std::string::npos) {
        return std::round(qty / 100.0) * 100.0;
    }
    // BTC/ETH step=0.001; SOL/LINK/UNI/LTC/INJ/RENDER step=0.01;
    // XRP/AVAX/DOT/NEAR/SUI/FIL/APT/ATOM/OP/TIA/ARB step=0.1;
    // DOGE/ADA/FET/SEI step=1
    if (symbol.find("BTC") != std::string::npos || symbol.find("ETH") != std::string::npos) {
        return std::round(qty * 1000.0) / 1000.0;
    }
    if (symbol.find("SOL") != std::string::npos || symbol.find("LINK") != std::string::npos ||
        symbol.find("UNI") != std::string::npos || symbol.find("LTC") != std::string::npos ||
        symbol.find("INJ") != std::string::npos || symbol.find("RENDER") != std::string::npos) {
        return std::round(qty * 100.0) / 100.0;
    }
    if (symbol.find("DOGE") != std::string::npos || symbol.find("ADA") != std::string::npos ||
        symbol.find("FET") != std::string::npos || symbol.find("SEI") != std::string::npos) {
        return std::round(qty);
    }
    // Default: step=0.1
    return std::round(qty * 10.0) / 10.0;
}

RealExecutor::RealExecutor(AdapterMap adapters, int waitMs, int cancelTtlMs)
    : adapters_(std::move(adapters)), waitMs_(waitMs), cancelTtlMs_(cancelTtlMs) {}

std::string RealExecutor::normalizeSide(const std::string& engineSide) {
    if (engineSide == "BUY") return "BUY";
    if (engineSide == "SELL") return "SELL";
    return engineSide;
}

static std::string toExSymbol(const std::string& engineSymbol) {
    std::string s;
    s.reserve(engineSymbol.size());
    for (char c : engineSymbol) {
        if (c != '/') s.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return s;
}

void RealExecutor::updateBook(const std::string& exchange, const std::string& symbol,
                               const OrderBook& book) {
    std::lock_guard<std::mutex> lock(bookMutex_);
    books_[exchange + "|" + symbol] = book;
}

double RealExecutor::bestBid(const std::string& exchange, const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(bookMutex_);
    const auto it = books_.find(exchange + "|" + symbol);
    if (it == books_.end()) return 0.0;
    const auto b = it->second.bestBid();
    return b ? *b : 0.0;
}

double RealExecutor::bestAsk(const std::string& exchange, const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(bookMutex_);
    const auto it = books_.find(exchange + "|" + symbol);
    if (it == books_.end()) return 0.0;
    const auto a = it->second.bestAsk();
    return a ? *a : 0.0;
}

ExecutionResult RealExecutor::execute(const Opportunity& opp) {
    if (!opp.legs.empty()) return executeLegs(opp);
    std::lock_guard<std::mutex> lock(execMutex_);
    ExecutionResult r;
    r.id = std::to_string(Timestamps::wallMs());
    r.symbol = opp.symbol;
    r.ts = Timestamps::wallMs();
    r.requestedQty = opp.quantity;

    auto& log = Logger::instance();

    const auto buyIt = adapters_.find(opp.buyExchange);
    const auto sellIt = adapters_.find(opp.sellExchange);
    if (buyIt == adapters_.end() || sellIt == adapters_.end()) {
        r.status = "rejected";
        log.warn("real_executor", "adapter not found for " + opp.buyExchange + " or " + opp.sellExchange);
        return r;
    }

    const std::string exSym = toExSymbol(opp.symbol);

    // LIVE PRICING: buy at ask (Binance GTC fills at best price), sell one
    // tick below bid (guaranteed taker fill even if bid drops during round-trip).
    const double liveBuyPrice = [&]() {
        double ask = bestAsk(opp.buyExchange, opp.symbol);
        if (ask <= 0.0) return opp.cost.buyPrice;
        return roundToTick(ask, opp.symbol);
    }();
    const double liveSellPrice = [&]() {
        double bid = bestBid(opp.sellExchange, opp.symbol);
        if (bid <= 0.0) return opp.cost.sellPrice;
        double tick = 0.01;
        if (opp.symbol.find("PEPE") != std::string::npos) {
            tick = 1e-8;
        } else if (opp.symbol.find("SHIB") != std::string::npos) {
            tick = 1e-9;
        } else if (opp.symbol.find("SEI") != std::string::npos ||
                   opp.symbol.find("OP/") != std::string::npos) {
            tick = 0.0001;
        }
        return roundToTick(bid - tick, opp.symbol);
    }();

    // LIVE SPREAD GATE: re-verify the opportunity is still profitable at
    // the actual execution prices.  The cost engine evaluated the spread
    // when the arb engine fired, but prices may have moved by now.
    if (liveBuyPrice > 0.0 && liveSellPrice > 0.0) {
        const double liveGross = liveSellPrice / liveBuyPrice - 1.0;
        const double liveNet = liveGross - opp.cost.buyFee - opp.cost.sellFee;
        if (liveNet <= 0.0) {
            r.status = "rejected";
            r.rejectReason = "spread_evaporated: gross=" + std::to_string(liveGross) +
                             " net=" + std::to_string(liveNet);
            log.info("real_executor", r.rejectReason + " symbol=" + opp.symbol);
            return r;
        }
    }

    // === Leg 1: place BUY order ===
    std::string buyErr, buyOrderId;
    double buyQty = roundToLot(opp.quantity, opp.symbol);
    const bool buyOk = buyIt->second->placeLimitOrder(
        exSym, "BUY", buyQty, liveBuyPrice, buyOrderId, buyErr);

    if (!buyOk) {
        r.status = "rejected";
        r.rejectReason = "buy leg rejected: " + buyErr;
        log.warn("real_executor", r.rejectReason);
        return r;
    }

    log.info("real_executor", "buy order placed id=" + buyOrderId + " on " + opp.buyExchange);

    // Poll buy order fill (up to 2 seconds).
    IExchangeAdapter::OrderInfo buyInfo;
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto info = buyIt->second->fetchOrderStatus(exSym, buyOrderId);
        if (info && (info->state == "filled" || info->state == "FILLED")) {
            buyInfo = *info;
            break;
        }
        if (info) buyInfo = *info;
    }

    if (buyInfo.filledQty <= 0.0) {
        std::string c;
        buyIt->second->cancelOrder(exSym, buyOrderId, c);
        r.status = "rejected";
        r.rejectReason = "buy leg unfilled after timeout";
        log.warn("real_executor", r.rejectReason);
        return r;
    }

    log.info("real_executor", "buy filled qty=" + std::to_string(buyInfo.filledQty) +
             " avg_px=" + std::to_string(buyInfo.avgPx));

    // === Leg 2: place SELL order ===
    std::string sellErr, sellOrderId;
    double sellQty = roundToLot(opp.quantity, opp.symbol);
    const bool sellOk = sellIt->second->placeLimitOrder(
        exSym, "SELL", sellQty, liveSellPrice, sellOrderId, sellErr);

    if (!sellOk) {
        // Buy already filled — we have exposure even though sell was rejected.
        r.status = "partial";
        r.buyFilled = buyInfo.filledQty;
        r.sellFilled = 0.0;
        r.avgBuyPrice = buyInfo.avgPx;
        r.avgSellPrice = 0.0;
        r.exposureLeft = buyInfo.filledQty;
        r.realizedPnl = 0.0;
        r.rejectReason = "sell leg rejected: " + sellErr;
        log.warn("real_executor", "sell leg rejected after buy fill: " + sellErr +
                 " exposure=" + std::to_string(buyInfo.filledQty));
        return r;
    }

    log.info("real_executor", "sell order placed id=" + sellOrderId + " on " + opp.sellExchange);

    // Poll sell order fill (up to 5 seconds).
    IExchangeAdapter::OrderInfo sellInfo;
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto info = sellIt->second->fetchOrderStatus(exSym, sellOrderId);
        if (info && (info->state == "filled" || info->state == "FILLED")) {
            sellInfo = *info;
            break;
        }
        if (info) sellInfo = *info;
    }

    if (sellInfo.filledQty <= 0.0) {
        std::string c;
        sellIt->second->cancelOrder(exSym, sellOrderId, c);
        // Sell didn't fill — we have exposure. Log it but don't pretend we traded.
        r.status = "partial";
        r.buyFilled = buyInfo.filledQty;
        r.sellFilled = 0.0;
        r.avgBuyPrice = buyInfo.avgPx;
        r.avgSellPrice = 0.0;
        r.exposureLeft = buyInfo.filledQty;
        r.realizedPnl = 0.0;
        log.warn("real_executor", "sell leg unfilled after timeout — exposure=" +
                 std::to_string(buyInfo.filledQty));
        return r;
    }

    log.info("real_executor", "sell filled qty=" + std::to_string(sellInfo.filledQty) +
             " avg_px=" + std::to_string(sellInfo.avgPx));

    // === Compute PnL from order fills (not balance diffs) ===
    const double buyUsdtSpent = buyInfo.filledQty * buyInfo.avgPx;
    const double sellUsdtReceived = sellInfo.filledQty * sellInfo.avgPx;
    const double buyFee = buyUsdtSpent * opp.cost.buyFee;
    const double sellFee = sellUsdtReceived * opp.cost.sellFee;

    r.buyFilled = buyInfo.filledQty;
    r.sellFilled = sellInfo.filledQty;
    r.avgBuyPrice = buyInfo.avgPx;
    r.avgSellPrice = sellInfo.avgPx;
    r.feesPaid = buyFee + sellFee;
    r.realizedPnl = sellUsdtReceived - buyUsdtSpent - r.feesPaid;
    r.exposureLeft = buyInfo.filledQty - sellInfo.filledQty;
    r.fullyFilled = (r.buyFilled > 0.0 && r.sellFilled > 0.0 &&
                     std::abs(r.buyFilled - r.sellFilled) < opp.quantity * 0.01);
    r.status = r.fullyFilled ? "filled" : "partial";

    log.info("real_executor",
             "symbol=" + opp.symbol + " buy=" + opp.buyExchange + " sell=" + opp.sellExchange +
             " buy_id=" + buyOrderId + " sell_id=" + sellOrderId +
             " buy_px=" + std::to_string(r.avgBuyPrice) +
             " sell_px=" + std::to_string(r.avgSellPrice) +
             " buy_filled=" + std::to_string(r.buyFilled) +
             " sell_filled=" + std::to_string(r.sellFilled) +
             " pnl=" + std::to_string(r.realizedPnl) +
             " status=" + r.status);
    return r;
}

static double tickSizeFor(const std::string& symbol) {
    if (symbol.find("PEPE") != std::string::npos) return 1e-8;
    if (symbol.find("SHIB") != std::string::npos) return 1e-9;
    if (symbol.find("SEI") != std::string::npos || symbol.find("OP/") != std::string::npos) {
        return 0.0001;
    }
    return 0.01;
}

ExecutionResult RealExecutor::executeLegs(const Opportunity& opp) {
    std::lock_guard<std::mutex> lock(execMutex_);
    ExecutionResult r;
    r.id = std::to_string(Timestamps::wallMs());
    r.symbol = opp.symbol;
    r.ts = Timestamps::wallMs();

    auto& log = Logger::instance();
    const auto& legs = opp.legs;
    if (legs.size() != 3) {
        r.status = "rejected";
        r.rejectReason = "need 3 legs";
        return r;
    }

    // Inventory walk: each leg converts what the previous leg produced so a
    // short fill never lets a later leg borrow assets we don't hold.
    double X = 0.0;                 // base of leg 0 (held after leg 0)
    double Z = 0.0;                 // base of leg 1 (held after leg 1)
    double spentQuote = 0.0;
    double receivedQuote = 0.0;
    double feesPaid = 0.0;
    double firstFillQty = 0.0, firstFillPx = 0.0;
    double sellFillQty = 0.0, sellFillPx = 0.0, sellRequested = 0.0;
    std::size_t completedLegs = 0;
    bool halted = false;

    // How much of leg 0 actually filled, carried into later legs so the route
    // stays balanced when the entry leg fills short.
    double rho = legs[0].quantity > 0.0 ? 1.0 : 0.0;

    for (std::size_t i = 0; i < legs.size() && !halted; ++i) {
        const auto& leg = legs[i];
        const auto it = adapters_.find(leg.exchange);
        if (it == adapters_.end()) {
            r.status = i > 0 ? "partial" : "rejected";
            r.rejectReason = "adapter not found for " + leg.exchange;
            halted = true;
            break;
        }

        const std::string exSym = toExSymbol(leg.symbol);
        double qty = roundToLot(i == 0 ? leg.quantity : leg.quantity * rho, leg.symbol);
        if (leg.side == Side::Sell) sellRequested = qty;

        double fillPx;
        if (leg.side == Side::Buy) {
            double ask = bestAsk(leg.exchange, leg.symbol);
            fillPx = roundToTick(ask > 0.0 ? ask : leg.price, leg.symbol);
        } else {
            double bid = bestBid(leg.exchange, leg.symbol);
            fillPx = roundToTick((bid > 0.0 ? bid : leg.price) - tickSizeFor(leg.symbol),
                                 leg.symbol);
        }

        std::string err, orderId;
        const std::string sideStr = leg.side == Side::Buy ? "BUY" : "SELL";
        if (!it->second->placeLimitOrder(exSym, sideStr, qty, fillPx, orderId, err)) {
            log.warn("real_executor", "tri leg" + std::to_string(i) + " rejected on " +
                     leg.exchange + ": " + err);
            halted = true;
            break;
        }
        log.info("real_executor", "tri leg" + std::to_string(i) + " placed id=" + orderId +
                 " " + exSym + " " + sideStr + " qty=" + std::to_string(qty) +
                 " px=" + std::to_string(fillPx));

        IExchangeAdapter::OrderInfo info;
        const int polls = leg.side == Side::Buy ? 30 : 50;
        for (int k = 0; k < polls; ++k) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto st = it->second->fetchOrderStatus(exSym, orderId);
            if (st && (st->state == "filled" || st->state == "FILLED")) {
                info = *st;
                break;
            }
            if (st) info = *st;
        }
        if (info.filledQty <= 0.0) {
            std::string c;
            it->second->cancelOrder(exSym, orderId, c);
            log.warn("real_executor", "tri leg" + std::to_string(i) + " unfilled, cancelled");
            halted = true;
            break;
        }

        const double filled = info.filledQty;
        const double avgPx = info.avgPx > 0.0 ? info.avgPx : fillPx;
        log.info("real_executor", "tri leg" + std::to_string(i) + " filled qty=" +
                 std::to_string(filled) + " avg_px=" + std::to_string(avgPx));

        if (i == 0) {
            spentQuote = filled * avgPx;
            feesPaid += filled * avgPx * opp.cost.buyFee;
            X = filled * (1.0 - opp.cost.buyFee);
            firstFillQty = filled;
            firstFillPx = avgPx;
            rho = legs[0].quantity > 0.0 ? filled / legs[0].quantity : 0.0;
        } else if (i == 1) {
            feesPaid += filled * avgPx * opp.cost.buyFee;
            Z = filled * (1.0 - opp.cost.buyFee);
        } else {
            receivedQuote = filled * avgPx * (1.0 - opp.cost.sellFee);
            feesPaid += filled * avgPx * opp.cost.sellFee;
            sellFillQty = filled;
            sellFillPx = avgPx;
        }
        ++completedLegs;
    }

    r.buyFilled = firstFillQty;
    r.avgBuyPrice = firstFillPx;
    r.sellFilled = sellFillQty;
    r.avgSellPrice = sellFillPx;
    r.feesPaid = feesPaid;
    r.realizedPnl = receivedQuote - spentQuote;

    // Stuck inventory when the cycle breaks: value it at the reference prices
    // so the residual is comparable to the quote currency.
    if (halted) {
        r.exposureLeft = completedLegs <= 1
                             ? X * legs[1].price          // stuck in leg0's base
                             : X * legs[1].price + Z * legs[2].price;
        r.status = r.buyFilled > 0.0 ? "partial" : "rejected";
        r.rejectReason = completedLegs <= 1 ? "leg1 stalled" : "leg2 stalled";
    } else {
        r.exposureLeft = 0.0;
        r.fullyFilled = completedLegs == 3 && sellFillQty >= sellRequested * 0.9;
        r.status = r.fullyFilled ? "filled" : "partial";
    }

    log.info("real_executor",
             "tri cycle symbol=" + opp.symbol + " venue=" + opp.buyExchange +
             " legs=" + std::to_string(completedLegs) + "/3" +
             " pnl=" + std::to_string(r.realizedPnl) +
             " exposure=" + std::to_string(r.exposureLeft) +
             " status=" + r.status);
    return r;
}

}  // namespace hftarb
