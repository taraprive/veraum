#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace hftarb {

// Bar pushed by the MT5 bridge (tools/mt5_feed.py) — one freshly closed bar.
// "t" is the bar open time in ms UTC and is used to deduplicate bars so a
// line is delivered exactly once even if the bridge rewrites the tail.
struct Mt5Bar {
    std::string instrument;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    std::int64_t tsMs = 0;
};

// Tails a JSONL file written by mt5_feed.py and delivers each new bar to the
// registered callback. Polling rescans from the last consumed offset; bars
// already seen (by instrument + bar-open timestamp) are deduplicated, and a
// truncated/replaced file resumes from its start without replaying history.
class Mt5BarFeed {
public:
    using Callback = std::function<void(const Mt5Bar&)>;

    explicit Mt5BarFeed(std::string path);

    // Polls the file and delivers all new bars. Call from one thread only.
    void poll();
    void setCallback(Callback cb);

    // Parses one JSONL line. Empty on malformed input.
    static Mt5Bar parseLine(const std::string& line);

private:
    std::string path_;
    Callback cb_;
    std::uint64_t offset_ = 0;
    bool firstSeen_ = false;
    std::unordered_map<std::string, std::int64_t> lastTs_;
};

}  // namespace hftarb