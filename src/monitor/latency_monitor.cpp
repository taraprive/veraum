#include "src/monitor/latency_monitor.h"

#include <algorithm>

namespace hftarb {

LatencyMonitor::LatencyMonitor() = default;

void LatencyMonitor::record(LatencyStage stage, double deltaUs) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& s = stages_[static_cast<int>(stage)];
    s.samples.push_back(deltaUs);
    s.count += 1;
    s.sum += deltaUs;
    if (deltaUs > s.max) s.max = deltaUs;
    total_.fetch_add(1);
}

LatencyMonitor::Summary LatencyMonitor::summary(LatencyStage stage) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto& s = stages_[static_cast<int>(stage)];
    Summary out;
    out.count = s.count;
    if (s.count == 0) return out;

    out.avgUs = s.sum / static_cast<double>(s.count);
    out.maxUs = s.max;

    auto sorted = s.samples;
    std::sort(sorted.begin(), sorted.end());
    const auto at = [&](double q) {
        const std::size_t idx = static_cast<std::size_t>(q * static_cast<double>(sorted.size()));
        return sorted[std::min(idx, sorted.size() - 1)];
    };
    out.p95Us = at(0.95);
    out.p99Us = at(0.99);
    return out;
}

std::int64_t LatencyMonitor::totalSamples() const { return total_.load(); }

}  // namespace hftarb
