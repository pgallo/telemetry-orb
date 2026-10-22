#include "core/bed_table.hpp"

#include <gtest/gtest.h>

using namespace telemetry::core;

namespace {
Sample make(const std::string& bed, const std::string& p, double v, std::uint32_t seq,
            std::uint64_t t_ns, Quality q = Quality::Valid) {
    Sample s;
    s.bed = bed;
    s.parameter = p;
    s.value = v;
    s.seq = seq;
    s.source_time_ns = t_ns;
    s.quality = q;
    return s;
}
}  // namespace

TEST(BedTable, KeepsLatestValueAndLatency) {
    BedTable t;
    t.update({make("ICU-01", "HR", 80, 0, 1'000'000), make("ICU-01", "HR", 82, 1, 2'000'000)},
             5'000'000, 0);
    EXPECT_DOUBLE_EQ(t.value("ICU-01", "HR"), 82.0);
    auto tot = t.totals();
    EXPECT_EQ(tot.samples, 2u);
    EXPECT_EQ(tot.batches, 1u);
    EXPECT_DOUBLE_EQ(tot.latency_ms.max, 4.0);
    EXPECT_DOUBLE_EQ(tot.latency_ms.p50, 3.0);
}

TEST(BedTable, CountsDropsAndSequenceGaps) {
    BedTable t;
    t.update({make("A", "HR", 1, 0, 0), make("A", "HR", 1, 1, 0)}, 0, 0);
    t.update({make("A", "HR", 1, 5, 0)}, 0, 3);  // 3 dropped at hub -> gap
    t.update({make("B", "HR", 1, 9, 0)}, 0, 0);  // first sample of B: no gap
    auto tot = t.totals();
    EXPECT_EQ(tot.dropped_at_hub, 3u);
    EXPECT_EQ(tot.seq_gaps, 1u);
    EXPECT_EQ(tot.beds, 2u);
}

TEST(BedTable, ClockSkewDoesNotProduceNegativeLatency) {
    BedTable t;
    t.update({make("A", "HR", 1, 0, 10'000'000)}, 5'000'000, 0);
    EXPECT_EQ(t.totals().latency_ms.count, 0u);
}

TEST(BedTable, RenderShowsColumnsAndQualityMarks) {
    BedTable t({"HR", "SPO2", "TEMP"});
    t.update({make("ICU-01", "HR", 101, 0, 0), make("ICU-01", "SPO2", 70, 1, 0, Quality::Questionable),
              make("ICU-01", "TEMP", 37.25, 2, 0)},
             0, 0);
    const auto s = t.render();
    EXPECT_NE(s.find("BED"), std::string::npos);
    EXPECT_NE(s.find("ICU-01"), std::string::npos);
    EXPECT_NE(s.find("101"), std::string::npos);
    EXPECT_NE(s.find("70?"), std::string::npos);
    EXPECT_NE(s.find("37.2"), std::string::npos);
    EXPECT_FALSE(t.has_value("ICU-01", "RR"));
}
