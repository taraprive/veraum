#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "src/core/types.h"

namespace hftarb {

// Collects per-stage deltas and answers the operational questions:
// average / P95 / P99 / max latency per stage, so we know where time goes.
// Uses monotonic microseconds. Not thread-safe for writers by design: call
// record() from the pipeline thread only, or guard externally.
class LatencyMonitor {
public:
    LatencyMonitor();

    void record(LatencyStage stage, double deltaUs);

    struct Summary {
        std::int64_t count = 0;
        double avgUs = 0.0;
        double p95Us = 0.0;
        double p99Us = 0.0;
        double maxUs = 0.0;
    };

    Summary summary(LatencyStage stage) const;

    // Total samples recorded across all stages.
    std::int64_t totalSamples() const;

private:
    struct StageData {
        std::vector<double> samples;
        std::int64_t count = 0;
        double sum = 0.0;
        double max = 0.0;
    };

    std::array<StageData, static_cast<int>(LatencyStage::Count)> stages_;
    std::atomic<std::int64_t> total_{0};
    mutable std::mutex mutex_;  // feeds are multi-threaded
};

}  // namespace hftarb
