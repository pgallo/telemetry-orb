// Dispatcher tests with an in-memory sink and explicit time: QoS batching,
// overflow accounting, failure handling and lease-driven eviction.
#include "core/dispatcher.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

using namespace telemetry::core;
using std::chrono::milliseconds;

namespace {

const TimePoint T0 = TimePoint{} + std::chrono::hours(1);

struct Recorded {
    std::vector<std::vector<Sample>> batches;
    std::vector<std::uint32_t> dropped;
    std::vector<DeliveryResult> script;  // results to return, in order; then Ok
    std::size_t calls = 0;
};

class FakeSink : public SubscriberSink {
public:
    explicit FakeSink(std::shared_ptr<Recorded> rec) : rec_(std::move(rec)) {}
    DeliveryResult deliver(const std::vector<Sample>& batch, std::uint32_t dropped,
                           std::string& detail) override {
        auto i = rec_->calls++;
        DeliveryResult r = i < rec_->script.size() ? rec_->script[i] : DeliveryResult::Ok;
        if (r == DeliveryResult::Ok) {
            rec_->batches.push_back(batch);
            rec_->dropped.push_back(dropped);
        } else {
            detail = "scripted failure";
        }
        return r;
    }

private:
    std::shared_ptr<Recorded> rec_;
};

Sample make(const std::string& bed, const std::string& param, double v, std::uint32_t seq = 0) {
    Sample s;
    s.bed = bed;
    s.parameter = param;
    s.value = v;
    s.seq = seq;
    return s;
}

QosSpec qos(std::uint32_t queue, std::uint32_t batch, int latency_ms, int interval_ms = 0,
            int lease_ms = 10000) {
    QosSpec q;
    q.max_queue = queue;
    q.max_batch = batch;
    q.max_latency = milliseconds(latency_ms);
    q.min_interval = milliseconds(interval_ms);
    q.lease = milliseconds(lease_ms);
    return q;
}

struct Fixture : ::testing::Test {
    Dispatcher d{DispatcherConfig{LeasePolicy{milliseconds(100), milliseconds(60000)}, 3, 256}};

    std::pair<std::uint64_t, std::shared_ptr<Recorded>> add(FilterSpec f, QosSpec q,
                                                            TimePoint now = T0) {
        auto rec = std::make_shared<Recorded>();
        auto g = d.subscribe(std::make_unique<FakeSink>(rec), "test", std::move(f), q, now);
        return {g.id, rec};
    }
};

}  // namespace

TEST_F(Fixture, PublishRoutesByFilter) {
    auto [a, ra] = add(FilterSpec({"ICU-01"}, {}), qos(100, 10, 0));
    auto [b, rb] = add(FilterSpec({}, {"SPO2"}), qos(100, 10, 0));
    const auto n = d.publish({make("ICU-01", "HR", 80), make("ICU-02", "SPO2", 95),
                              make("ICU-01", "SPO2", 97)},
                             T0);
    EXPECT_EQ(n, 4u);  // a gets 2, b gets 2
    d.run_cycle(T0);
    ASSERT_EQ(ra->batches.size(), 1u);
    ASSERT_EQ(rb->batches.size(), 1u);
    EXPECT_EQ(ra->batches[0].size(), 2u);
    EXPECT_EQ(rb->batches[0][0].bed, "ICU-02");
    EXPECT_EQ(rb->batches[0][1].bed, "ICU-01");
}

TEST_F(Fixture, PartialBatchWaitsForMaxLatency) {
    auto [id, rec] = add({}, qos(100, 10, 50));
    d.publish({make("B", "HR", 1), make("B", "HR", 2)}, T0);
    d.run_cycle(T0 + milliseconds(49));
    EXPECT_TRUE(rec->batches.empty());
    d.run_cycle(T0 + milliseconds(50));
    ASSERT_EQ(rec->batches.size(), 1u);
    EXPECT_EQ(rec->batches[0].size(), 2u);
}

TEST_F(Fixture, FullBatchFlushesImmediatelyAndIsCapped) {
    auto [id, rec] = add({}, qos(100, 4, 1000));
    std::vector<Sample> s;
    for (int i = 0; i < 10; ++i) s.push_back(make("B", "HR", i));
    d.publish(s, T0);
    d.run_cycle(T0);
    d.run_cycle(T0);
    d.run_cycle(T0);  // 2 left, below max_batch and not yet old
    ASSERT_EQ(rec->batches.size(), 2u);
    EXPECT_EQ(rec->batches[0].size(), 4u);
    EXPECT_EQ(rec->batches[1][0].value, 4.0);
    EXPECT_EQ(d.stats(id)->queued, 2u);
}

TEST_F(Fixture, MinIntervalRateLimitsPushes) {
    auto [id, rec] = add({}, qos(100, 1, 0, 100));
    d.publish({make("B", "HR", 1), make("B", "HR", 2), make("B", "HR", 3)}, T0);
    d.run_cycle(T0);
    d.run_cycle(T0 + milliseconds(50));
    EXPECT_EQ(rec->batches.size(), 1u);
    d.run_cycle(T0 + milliseconds(100));
    EXPECT_EQ(rec->batches.size(), 2u);
}

TEST_F(Fixture, OverflowDropsOldestAndReportsCountOnNextPush) {
    auto [id, rec] = add({}, qos(3, 3, 0));
    std::vector<Sample> s;
    for (int i = 0; i < 8; ++i) s.push_back(make("B", "HR", i));
    d.publish(s, T0);
    d.run_cycle(T0);
    ASSERT_EQ(rec->batches.size(), 1u);
    EXPECT_EQ(rec->dropped[0], 5u);
    EXPECT_EQ(rec->batches[0].front().value, 5.0);  // newest three survive
    EXPECT_EQ(rec->batches[0].back().value, 7.0);

    d.publish({make("B", "HR", 8)}, T0);
    d.run_cycle(T0);
    EXPECT_EQ(rec->dropped[1], 0u);  // reported once, not cumulatively

    auto st = d.stats(id);
    EXPECT_EQ(st->dropped, 5u);
    EXPECT_EQ(st->delivered, 4u);
}

TEST_F(Fixture, SlowSubscriberDoesNotAffectOthers) {
    auto [fast, rf] = add({}, qos(1000, 100, 0));
    auto [slow, rs] = add({}, qos(10, 10, 0, 1000));  // one push per second
    for (int t = 0; t < 20; ++t) {
        std::vector<Sample> s;
        for (int i = 0; i < 10; ++i) s.push_back(make("B", "HR", t * 10 + i));
        d.publish(s, T0 + milliseconds(t * 10));
        d.run_cycle(T0 + milliseconds(t * 10));
    }
    EXPECT_EQ(d.stats(fast)->delivered, 200u);
    EXPECT_EQ(d.stats(fast)->dropped, 0u);
    EXPECT_EQ(d.stats(slow)->delivered, 10u);
    EXPECT_EQ(d.stats(slow)->dropped, 180u);
    EXPECT_EQ(d.stats(slow)->queued, 10u);
}

TEST_F(Fixture, QueueLatencyIsMeasured) {
    auto [id, rec] = add({}, qos(100, 100, 30));
    d.publish({make("B", "HR", 1)}, T0);
    d.publish({make("B", "HR", 2)}, T0 + milliseconds(10));
    d.run_cycle(T0 + milliseconds(30));
    auto st = d.stats(id);
    EXPECT_DOUBLE_EQ(st->queue_latency_ms.max, 30.0);
    EXPECT_DOUBLE_EQ(st->queue_latency_ms.p50, 20.0);
}

TEST_F(Fixture, DeadSubscriberIsEvictedAtOnce) {
    auto [id, rec] = add({}, qos(100, 1, 0));
    rec->script = {DeliveryResult::Dead};
    std::vector<EvictionEvent> seen;
    d.on_evict = [&](const EvictionEvent& e) { seen.push_back(e); };
    d.publish({make("B", "HR", 1)}, T0);
    auto report = d.run_cycle(T0);
    ASSERT_EQ(report.evicted.size(), 1u);
    EXPECT_EQ(report.evicted[0].reason, EvictionReason::SubscriberDead);
    EXPECT_EQ(seen.size(), 1u);
    EXPECT_EQ(d.subscription_count(), 0u);
    EXPECT_FALSE(d.renew(id, T0));
}

TEST_F(Fixture, TransientFailuresEvictAfterThreshold) {
    auto [id, rec] = add({}, qos(100, 1, 0));
    rec->script = {DeliveryResult::Transient, DeliveryResult::Transient, DeliveryResult::Ok,
                   DeliveryResult::Transient, DeliveryResult::Transient, DeliveryResult::Transient};
    for (int i = 0; i < 6; ++i) d.publish({make("B", "HR", i)}, T0);
    for (int i = 0; i < 5; ++i) {
        auto r = d.run_cycle(T0);
        EXPECT_TRUE(r.evicted.empty()) << "cycle " << i;
    }
    EXPECT_EQ(d.stats(id)->consecutive_failures, 2u);  // reset by the Ok in between
    auto r = d.run_cycle(T0);
    ASSERT_EQ(r.evicted.size(), 1u);
    EXPECT_EQ(r.evicted[0].reason, EvictionReason::TooManyFailures);
}

TEST_F(Fixture, LeaseExpiryEvictsAndRenewKeepsAlive) {
    auto [keep, rk] = add({}, qos(10, 1, 0, 0, 1000));
    auto [lapse, rl] = add({}, qos(10, 1, 0, 0, 1000));
    EXPECT_TRUE(d.renew(keep, T0 + milliseconds(800)));
    auto r = d.run_cycle(T0 + milliseconds(1000));
    ASSERT_EQ(r.evicted.size(), 1u);
    EXPECT_EQ(r.evicted[0].id, lapse);
    EXPECT_EQ(r.evicted[0].reason, EvictionReason::LeaseExpired);
    EXPECT_EQ(d.subscription_ids(), (std::vector<std::uint64_t>{keep}));
    EXPECT_FALSE(d.renew(lapse, T0 + milliseconds(1000)));
}

TEST_F(Fixture, RenewAfterDeadlineFailsEvenBeforeSweep) {
    auto [id, rec] = add({}, qos(10, 1, 0, 0, 1000));
    EXPECT_FALSE(d.renew(id, T0 + milliseconds(1500)));
    EXPECT_EQ(d.subscription_count(), 0u);
}

TEST_F(Fixture, UnsubscribeStopsDelivery) {
    auto [id, rec] = add({}, qos(10, 1, 0));
    EXPECT_TRUE(d.unsubscribe(id));
    EXPECT_FALSE(d.unsubscribe(id));
    EXPECT_EQ(d.publish({make("B", "HR", 1)}, T0), 0u);
    EXPECT_FALSE(d.stats(id));
}

TEST_F(Fixture, InvalidRequestsRejected) {
    auto rec = std::make_shared<Recorded>();
    EXPECT_THROW(d.subscribe(nullptr, "x", {}, qos(10, 1, 0), T0), std::invalid_argument);
    EXPECT_THROW(d.subscribe(std::make_unique<FakeSink>(rec), "x", {}, qos(10, 11, 0), T0),
                 std::invalid_argument);
    EXPECT_THROW(d.subscribe(std::make_unique<FakeSink>(rec), "x", {}, qos(0, 1, 0), T0),
                 std::invalid_argument);
    EXPECT_THROW(d.subscribe(std::make_unique<FakeSink>(rec), "x", FilterSpec({"a*b"}, {}),
                             qos(10, 1, 0), T0),
                 std::invalid_argument);
    EXPECT_EQ(d.subscription_count(), 0u);
}

TEST_F(Fixture, ConcurrentPublishSubscribeAndCycle) {
    // Smoke test for the locking scheme: publishers, a cycling thread and
    // subscription churn running together must not deadlock or lose counts.
    auto [id, rec] = add({}, qos(100000, 500, 0));
    std::atomic<bool> stop{false};
    std::thread cycler([&] {
        while (!stop) d.run_cycle(SteadyClock::now());
    });
    std::thread churn([&] {
        for (int i = 0; i < 200; ++i) {
            auto [cid, crec] = add({}, qos(10, 1, 0), SteadyClock::now());
            d.unsubscribe(cid);
        }
    });
    std::vector<std::thread> pubs;
    for (int t = 0; t < 3; ++t)
        pubs.emplace_back([&] {
            for (int i = 0; i < 2000; ++i) d.publish({make("B", "HR", i)}, SteadyClock::now());
        });
    for (auto& p : pubs) p.join();
    churn.join();
    while (d.stats(id)->queued > 0) std::this_thread::yield();
    stop = true;
    cycler.join();
    auto st = d.stats(id);
    EXPECT_EQ(st->delivered + st->dropped, 6000u);
    EXPECT_EQ(st->dropped, 0u);
}
