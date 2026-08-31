#include "src/feed/mt5_bar_feed.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "src/third_party/nlohmann/json.hpp"

namespace hftarb {

Mt5BarFeed::Mt5BarFeed(std::string path) : path_(std::move(path)) {}

void Mt5BarFeed::setCallback(Callback cb) { cb_ = std::move(cb); }

Mt5Bar Mt5BarFeed::parseLine(const std::string& line) {
    Mt5Bar bar;
    try {
        const auto j = nlohmann::json::parse(line);
        const auto instr = j.value("i", "");
        bar.instrument = instr;
        bar.close = j.value("p", 0.0);
        bar.open = j.value("o", bar.close);
        bar.high = j.value("h", bar.close);
        bar.low = j.value("l", bar.close);
        bar.tsMs = j.value("t", std::int64_t{0});
        if (instr.empty() || bar.close <= 0.0) return Mt5Bar{};
    } catch (...) {
        return Mt5Bar{};
    }
    return bar;
}

void Mt5BarFeed::poll() {
    if (cb_ == nullptr) return;

    std::ifstream in(path_, std::ios::in);
    if (!in.is_open()) return;

    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path_, ec);
    if (ec) return;

    // First contact with the file: treat everything already present as past
    // history so weeks of old bars are not replayed into the engines.
    if (!firstSeen_) {
        firstSeen_ = true;
        offset_ = size;
    }
    if (size == 0) {
        offset_ = 0;
        return;
    }
    // A smaller file than we consumed means it was replaced/truncated: resume
    // from its start and let timestamp dedupe suppress anything already seen.
    in.seekg(static_cast<std::streamoff>(
        std::min<std::uintmax_t>(offset_, size - 1)));
    if (!in) return;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        Mt5Bar bar = parseLine(line);
        if (!bar.instrument.empty()) {
            const auto it = lastTs_.find(bar.instrument);
            bool newBar = (it == lastTs_.end() || bar.tsMs > it->second);
            if (newBar) {
                lastTs_[bar.instrument] = bar.tsMs;
                cb_(bar);
            }
        }
    }
    offset_ = size;
}

}  // namespace hftarb