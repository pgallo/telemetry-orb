// Station-side view: latest value per bed and parameter, plus end-to-end
// latency per bed (producer sample time -> station receipt). Thread-safe:
// ORB threads update it from push(), the display thread renders it.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/latency_stats.hpp"
#include "core/sample.hpp"

namespace telemetry::core {

class BedTable {
public:
    explicit BedTable(std::vector<std::string> columns = {"HR", "SPO2", "RR", "TEMP", "NIBPS",
                                                          "NIBPD"});

    // receive_time_ns: station wall clock at receipt.
    void update(const std::vector<Sample>& batch, std::uint64_t receive_time_ns,
                std::uint32_t dropped_since_last);

    std::string render() const;

    struct Totals {
        std::uint64_t batches = 0;
        std::uint64_t samples = 0;
        std::uint64_t dropped_at_hub = 0;
        std::uint64_t seq_gaps = 0;  // per-bed seq discontinuities seen here
        std::size_t beds = 0;
        LatencySnapshot latency_ms;  // over all beds
    };
    Totals totals() const;
    bool has_value(const std::string& bed, const std::string& parameter) const;
    double value(const std::string& bed, const std::string& parameter) const;

private:
    struct Cell {
        double value = 0;
        Quality quality = Quality::Valid;
    };
    struct Row {
        std::map<std::string, Cell> cells;
        LatencyStats latency{256};
        std::uint64_t samples = 0;
        bool have_seq = false;
        std::uint32_t last_seq = 0;
    };

    std::vector<std::string> columns_;
    mutable std::mutex mutex_;
    std::map<std::string, Row> rows_;
    LatencyStats all_latency_{4096};
    std::uint64_t batches_ = 0;
    std::uint64_t samples_ = 0;
    std::uint64_t dropped_ = 0;
    std::uint64_t gaps_ = 0;
};

}  // namespace telemetry::core
