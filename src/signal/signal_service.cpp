#include "src/signal/signal_service.h"

#include <chrono>
#include <fstream>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "src/core/timestamp.h"
#include "src/net/http_client.h"
#include "src/util/logger.h"

namespace hftarb {

SignalService::SignalService(SignalConfig cfg) : cfg_(std::move(cfg)) {
    if (!cfg_.enabled) return;
    worker_ = std::thread([this] { run(); });
}

SignalService::~SignalService() {
    stop();
    if (worker_.joinable()) worker_.join();
}

void SignalService::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    cv_.notify_all();
}

void SignalService::emit(const Opportunity& opp) {
    if (!cfg_.enabled || opp.decision != Decision::Accepted) return;
    if (!shouldSend(opp)) return;
    const std::string text = format(opp);
    appendJsonl(opp, text);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(text);
    }
    cv_.notify_all();
}

void SignalService::emitRaw(const std::string& text, const std::string& key) {
    if (!cfg_.enabled) return;
    const auto now = Timestamps::monoMs();
    const auto it = lastSentMs_.find(key);
    if (it != lastSentMs_.end() && now - it->second < cfg_.intervalMs) return;
    lastSentMs_[key] = now;
    appendRawJsonl(text);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(text);
    }
    cv_.notify_all();
}

bool SignalService::shouldSend(const Opportunity& opp) {
    if (opp.cost.netSpread < cfg_.minNetSpread) return false;
    if (!cfg_.symbols.empty()) {
        bool found = false;
        for (const auto& s : cfg_.symbols) {
            if (s == opp.symbol) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    const auto now = Timestamps::monoMs();
    const auto it = lastSentMs_.find(opp.symbol);
    if (it != lastSentMs_.end() && now - it->second < cfg_.intervalMs) return false;
    lastSentMs_[opp.symbol] = now;
    return true;
}

std::string SignalService::format(const Opportunity& opp) {
    std::ostringstream os;
    os << "ARB SIGNAL " << opp.symbol << "\n";
    if (!opp.legs.empty()) {
        os << "route: " << opp.strategy << " @ " << opp.buyExchange << "\n";
        for (const auto& lg : opp.legs) {
            os << "  " << (lg.side == Side::Buy ? "buy  " : "sell ") << lg.exchange << " "
               << lg.symbol << " @ $" << lg.price << " x " << lg.quantity << "\n";
        }
    } else {
        os << "buy  @ " << opp.buyExchange << " $" << opp.cost.buyPrice << "\n";
        os << "sell @ " << opp.sellExchange << " $" << opp.cost.sellPrice << "\n";
        os << "qty " << opp.quantity << " (notional $" << opp.cost.notional << ")\n";
    }
    os << "net edge +" << (opp.cost.netSpread * 10000.0) << " bp (" << (opp.cost.netSpread * 100.0)
       << "%)\n";
    os << "latency " << opp.roundTripLatencyMs << " ms | estimated, before execution, not "
          "guaranteed profit";
    return os.str();
}

void SignalService::deliverTelegram(const std::string& text) {
    if (cfg_.token.empty() || cfg_.chatId.empty()) return;
    try {
        HttpClient http("https://api.telegram.org");
        const nlohmann::json body = {{"chat_id", cfg_.chatId},
                                     {"text", text},
                                     {"disable_web_page_preview", true}};
        const auto res = http.post("/bot" + cfg_.token + "/sendMessage", "application/json",
                                   body.dump());
        if (!res.ok()) {
            Logger::instance().warn("signals", "telegram send failed status=" +
                                                   std::to_string(res.status) + " " + res.error);
        }
    } catch (const std::exception& e) {
        Logger::instance().warn("signals", "telegram send error: " + std::string(e.what()));
    }
}

void SignalService::appendJsonl(const Opportunity& opp, const std::string& text) {
    if (cfg_.jsonlPath.empty()) return;
    try {
        const nlohmann::json j = {{"ts", opp.decisionTs},
                                  {"strategy", opp.strategy},
                                  {"symbol", opp.symbol},
                                  {"net_spread", opp.cost.netSpread},
                                  {"text", text}};
        std::ofstream ofs(cfg_.jsonlPath, std::ios::app);
        if (ofs) ofs << j.dump() << "\n";
    } catch (const std::exception&) {
    }
}

void SignalService::appendRawJsonl(const std::string& text) {
    if (cfg_.jsonlPath.empty()) return;
    try {
        const nlohmann::json j = {{"ts", Timestamps::wallMs()},
                                  {"kind", "raw"},
                                  {"text", text}};
        std::ofstream ofs(cfg_.jsonlPath, std::ios::app);
        if (ofs) ofs << j.dump() << "\n";
    } catch (const std::exception&) {
    }
}

void SignalService::run() {
    for (;;) {
        std::string msg;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return !queue_.empty() || !running_; });
            if (queue_.empty() && !running_) break;
            msg = std::move(queue_.front());
            queue_.pop_front();
        }
        if (cfg_.channel == "telegram") deliverTelegram(msg);
        Logger::instance().info("signals", msg);
    }
}

}  // namespace hftarb