// Latency summary over a sliding window of the most recent samples.
// Percentiles come from a sorted copy of the window: O(n log n) per snapshot
// with n <= window, which is fine for a stats call every few seconds and
// avoids the bucket-resolution questions a histogram would raise.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace telemetry::core {

struct LatencySnapshot {
    std::uint64_t count = 0;  // all samples ever recorded
    double p50 = 0;
    double p99 = 0;
    double max = 0;           // over the window
    double mean = 0;          // over the window
};

class LatencyStats {
public:
    explicit LatencyStats(std::size_t window = 1024) : window_(window ? window : 1) {
        ring_.reserve(window_);
    }

    void record(double value) {
        if (ring_.size() < window_) {
            ring_.push_back(value);
        } else {
            ring_[next_] = value;
        }
        next_ = (next_ + 1) % window_;
        ++count_;
    }

    LatencySnapshot snapshot() const {
        LatencySnapshot s;
        s.count = count_;
        if (ring_.empty()) return s;
        std::vector<double> sorted(ring_);
        std::sort(sorted.begin(), sorted.end());
        s.p50 = percentile(sorted, 0.50);
        s.p99 = percentile(sorted, 0.99);
        s.max = sorted.back();
        double sum = 0;
        for (double v : sorted) sum += v;
        s.mean = sum / static_cast<double>(sorted.size());
        return s;
    }

    void reset() {
        ring_.clear();
        next_ = 0;
        count_ = 0;
    }

    // Nearest-rank percentile on a sorted, non-empty vector.
    static double percentile(const std::vector<double>& sorted, double q) {
        const auto n = sorted.size();
        auto rank = static_cast<std::size_t>(q * static_cast<double>(n) + 0.999999);
        rank = std::clamp<std::size_t>(rank, 1, n);
        return sorted[rank - 1];
    }

private:
    std::size_t window_;
    std::vector<double> ring_;
    std::size_t next_ = 0;
    std::uint64_t count_ = 0;
};

}  // namespace telemetry::core
