#include "core/latency_stats.hpp"

#include <gtest/gtest.h>

using telemetry::core::LatencyStats;

TEST(LatencyStats, EmptySnapshot) {
    LatencyStats s;
    auto snap = s.snapshot();
    EXPECT_EQ(snap.count, 0u);
    EXPECT_EQ(snap.p99, 0.0);
}

TEST(LatencyStats, NearestRankPercentiles) {
    LatencyStats s(1000);
    for (int i = 1; i <= 100; ++i) s.record(i);
    auto snap = s.snapshot();
    EXPECT_EQ(snap.count, 100u);
    EXPECT_DOUBLE_EQ(snap.p50, 50.0);
    EXPECT_DOUBLE_EQ(snap.p99, 99.0);
    EXPECT_DOUBLE_EQ(snap.max, 100.0);
    EXPECT_DOUBLE_EQ(snap.mean, 50.5);
}

TEST(LatencyStats, WindowForgetsOldValues) {
    LatencyStats s(10);
    for (int i = 0; i < 10; ++i) s.record(1000);  // an old spike
    for (int i = 0; i < 10; ++i) s.record(1);
    auto snap = s.snapshot();
    EXPECT_EQ(snap.count, 20u);
    EXPECT_DOUBLE_EQ(snap.max, 1.0);
}

TEST(LatencyStats, SingleValue) {
    LatencyStats s;
    s.record(3.5);
    auto snap = s.snapshot();
    EXPECT_DOUBLE_EQ(snap.p50, 3.5);
    EXPECT_DOUBLE_EQ(snap.p99, 3.5);
}
