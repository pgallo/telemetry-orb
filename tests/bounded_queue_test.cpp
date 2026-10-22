#include "core/bounded_queue.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

using telemetry::core::BoundedQueue;

TEST(BoundedQueue, RejectsZeroCapacity) {
    EXPECT_THROW(BoundedQueue<int>(0), std::invalid_argument);
}

TEST(BoundedQueue, FifoBelowCapacity) {
    BoundedQueue<int> q(4);
    EXPECT_FALSE(q.push(1));
    EXPECT_FALSE(q.push(2));
    EXPECT_FALSE(q.push(3));
    std::vector<int> out;
    EXPECT_EQ(q.drain(out, 10), 3u);
    EXPECT_EQ(out, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(q.size(), 0u);
    EXPECT_EQ(q.dropped_total(), 0u);
}

TEST(BoundedQueue, OverflowDropsOldest) {
    BoundedQueue<int> q(3);
    for (int i = 1; i <= 3; ++i) EXPECT_FALSE(q.push(i));
    EXPECT_TRUE(q.push(4));
    EXPECT_TRUE(q.push(5));
    EXPECT_EQ(q.size(), 3u);
    EXPECT_EQ(q.dropped_total(), 2u);
    std::vector<int> out;
    q.drain(out, 10);
    EXPECT_EQ(out, (std::vector<int>{3, 4, 5}));  // newest kept
}

TEST(BoundedQueue, DrainRespectsMaxAndAppends) {
    BoundedQueue<int> q(10);
    for (int i = 0; i < 7; ++i) q.push(i);
    std::vector<int> out{-1};
    EXPECT_EQ(q.drain(out, 3), 3u);
    EXPECT_EQ(out, (std::vector<int>{-1, 0, 1, 2}));
    EXPECT_EQ(q.size(), 4u);
}

TEST(BoundedQueue, PeekFront) {
    BoundedQueue<int> q(2);
    int seen = 0;
    EXPECT_FALSE(q.peek_front([&](int v) { seen = v; }));
    q.push(42);
    q.push(43);
    EXPECT_TRUE(q.peek_front([&](int v) { seen = v; }));
    EXPECT_EQ(seen, 42);
}

TEST(BoundedQueue, ConcurrentProducersNeverExceedCapacity) {
    BoundedQueue<int> q(100);
    constexpr int kThreads = 4, kPerThread = 20000;
    std::atomic<bool> stop{false};
    std::size_t drained = 0;
    std::size_t max_seen = 0;
    std::thread consumer([&] {
        std::vector<int> out;
        while (!stop) {
            max_seen = std::max(max_seen, q.size());
            out.clear();
            drained += q.drain(out, 16);
        }
        out.clear();
        drained += q.drain(out, 1000);
    });
    std::vector<std::thread> producers;
    for (int t = 0; t < kThreads; ++t)
        producers.emplace_back([&] {
            for (int i = 0; i < kPerThread; ++i) q.push(i);
        });
    for (auto& p : producers) p.join();
    stop = true;
    consumer.join();
    EXPECT_LE(max_seen, 100u);
    // Conservation: everything pushed was either drained or dropped.
    EXPECT_EQ(drained + q.dropped_total(), static_cast<std::size_t>(kThreads * kPerThread));
}
