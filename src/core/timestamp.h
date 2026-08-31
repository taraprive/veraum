#pragma once

#include <chrono>
#include <cstdint>

namespace hftarb {

// Wall-clock and monotonic time helpers used across the hot path.
// The hot path should only rely on monotonic deltas; wall time is for logging.
class Timestamps {
public:
    static std::int64_t wallMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    static std::int64_t wallUs() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    // Monotonic milliseconds since process start (arbitrary base).
    static std::int64_t monoMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    static std::int64_t monoUs() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    static std::int64_t diffMs(std::int64_t later, std::int64_t earlier) {
        return later - earlier;
    }
};

}  // namespace hftarb
