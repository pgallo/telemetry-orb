#include "core/bed_table.hpp"

#include <cstdio>
#include <stdexcept>

namespace telemetry::core {

BedTable::BedTable(std::vector<std::string> columns) : columns_(std::move(columns)) {}

void BedTable::update(const std::vector<Sample>& batch, std::uint64_t receive_time_ns,
                      std::uint32_t dropped_since_last) {
    std::lock_guard lock(mutex_);
    ++batches_;
    dropped_ += dropped_since_last;
    for (const auto& s : batch) {
        Row& row = rows_[s.bed];
        row.cells[s.parameter] = Cell{s.value, s.quality};
        ++row.samples;
        ++samples_;
        // seq is per bed and increments per sample; a filter on parameters
        // also produces jumps, so gaps are only meaningful for full feeds.
        if (row.have_seq && s.seq != row.last_seq + 1 && s.seq > row.last_seq) ++gaps_;
        row.have_seq = true;
        row.last_seq = s.seq;
        if (receive_time_ns >= s.source_time_ns) {
            const double ms = static_cast<double>(receive_time_ns - s.source_time_ns) / 1e6;
            row.latency.record(ms);
            all_latency_.record(ms);
        }
    }
}

std::string BedTable::render() const {
    std::lock_guard lock(mutex_);
    std::string out;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%-8s", "BED");
    out += buf;
    for (const auto& c : columns_) {
        std::snprintf(buf, sizeof buf, "%7s", c.c_str());
        out += buf;
    }
    out += "   e2e p50/p99 ms   samples\n";
    for (const auto& [bed, row] : rows_) {
        std::snprintf(buf, sizeof buf, "%-8s", bed.c_str());
        out += buf;
        for (const auto& c : columns_) {
            auto it = row.cells.find(c);
            if (it == row.cells.end()) {
                std::snprintf(buf, sizeof buf, "%7s", "-");
            } else {
                const char mark = it->second.quality == Quality::Questionable ? '?'
                                  : it->second.quality == Quality::Invalid    ? '!'
                                                                              : ' ';
                if (c == "TEMP")
                    std::snprintf(buf, sizeof buf, "%6.1f%c", it->second.value, mark);
                else
                    std::snprintf(buf, sizeof buf, "%6.0f%c", it->second.value, mark);
            }
            out += buf;
        }
        const auto lat = row.latency.snapshot();
        std::snprintf(buf, sizeof buf, "   %6.2f /%7.2f   %7llu\n", lat.p50, lat.p99,
                      static_cast<unsigned long long>(row.samples));
        out += buf;
    }
    return out;
}

BedTable::Totals BedTable::totals() const {
    std::lock_guard lock(mutex_);
    Totals t;
    t.batches = batches_;
    t.samples = samples_;
    t.dropped_at_hub = dropped_;
    t.seq_gaps = gaps_;
    t.beds = rows_.size();
    t.latency_ms = all_latency_.snapshot();
    return t;
}

bool BedTable::has_value(const std::string& bed, const std::string& parameter) const {
    std::lock_guard lock(mutex_);
    auto r = rows_.find(bed);
    return r != rows_.end() && r->second.cells.count(parameter) != 0;
}

double BedTable::value(const std::string& bed, const std::string& parameter) const {
    std::lock_guard lock(mutex_);
    return rows_.at(bed).cells.at(parameter).value;
}

}  // namespace telemetry::core
