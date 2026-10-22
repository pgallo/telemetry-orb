// Hub fan-out logic, independent of the ORB.
//
//   publish()    called concurrently by ORB worker threads; copies each
//                sample into the queue of every subscription whose filter
//                matches. Never blocks on a subscriber.
//   run_cycle()  called by the single publisher thread; sweeps expired
//                leases, flushes subscriptions whose batching rules say they
//                are due, and evicts subscribers whose delivery failed.
//
// Delivery goes through SubscriberSink, so tests can use an in-memory sink
// and the hub uses a CORBA sink (src/orb/corba_sink.hpp).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "core/bounded_queue.hpp"
#include "core/filter.hpp"
#include "core/latency_stats.hpp"
#include "core/lease.hpp"
#include "core/sample.hpp"

namespace telemetry::core {

enum class DeliveryResult {
    Ok,
    Transient,  // try again next cycle (timeout, connection refused)
    Dead,       // the subscriber is gone (object does not exist, connection broke)
};

class SubscriberSink {
public:
    virtual ~SubscriberSink() = default;
    // Called only from the publisher thread. Must not throw; failures are
    // reported through the return value with a short description in detail.
    virtual DeliveryResult deliver(const std::vector<Sample>& batch,
                                   std::uint32_t dropped_since_last,
                                   std::string& detail) = 0;
};

struct QosSpec {
    std::uint32_t max_queue = 1000;
    std::uint32_t max_batch = 64;
    Millis max_latency{50};
    Millis min_interval{0};
    Millis lease{10000};

    // Throws std::invalid_argument.
    void validate() const;
};

struct SubscriptionStats {
    std::uint64_t delivered = 0;
    std::uint64_t dropped = 0;
    std::uint32_t queued = 0;
    std::uint32_t consecutive_failures = 0;
    LatencySnapshot queue_latency_ms;  // enqueue at hub -> handed to the ORB
};

enum class EvictionReason { LeaseExpired, SubscriberDead, TooManyFailures };
const char* to_string(EvictionReason r);

struct EvictionEvent {
    std::uint64_t id;
    std::string client;
    EvictionReason reason;
    std::string detail;
};

struct CycleReport {
    std::size_t batches_sent = 0;
    std::size_t samples_sent = 0;
    std::vector<EvictionEvent> evicted;
};

struct DispatcherConfig {
    LeasePolicy lease;
    std::uint32_t max_consecutive_failures = 3;
    std::size_t latency_window = 2048;
};

class Dispatcher {
public:
    explicit Dispatcher(DispatcherConfig config = {});

    struct Granted {
        std::uint64_t id;
        Millis lease;
    };

    // Throws std::invalid_argument for a bad filter or QoS.
    Granted subscribe(std::unique_ptr<SubscriberSink> sink, std::string client,
                      FilterSpec filter, QosSpec qos, TimePoint now);
    std::optional<Millis> renew(std::uint64_t id, TimePoint now);
    bool unsubscribe(std::uint64_t id);
    std::optional<SubscriptionStats> stats(std::uint64_t id) const;

    // Returns the number of (sample, subscription) enqueues performed.
    std::size_t publish(const std::vector<Sample>& samples, TimePoint now);

    CycleReport run_cycle(TimePoint now);

    std::size_t subscription_count() const;
    std::vector<std::uint64_t> subscription_ids() const;
    std::string client_name(std::uint64_t id) const;

    // Invoked from run_cycle for each eviction (after the report is built).
    std::function<void(const EvictionEvent&)> on_evict;

private:
    struct Queued {
        Sample sample;
        TimePoint enqueued;
    };

    struct Subscription {
        Subscription(std::uint64_t id, std::string client, std::unique_ptr<SubscriberSink> sink,
                     FilterSpec filter, QosSpec qos, std::size_t latency_window);
        const std::uint64_t id;
        const std::string client;
        const std::unique_ptr<SubscriberSink> sink;
        const FilterSpec filter;
        const QosSpec qos;
        BoundedQueue<Queued> queue;

        // Touched only by the publisher thread, read by stats() under stats_mutex.
        mutable std::mutex stats_mutex;
        LatencyStats latency;
        std::uint64_t delivered = 0;
        std::uint64_t dropped_reported = 0;
        std::uint32_t consecutive_failures = 0;
        std::optional<TimePoint> last_push;
    };
    using SubPtr = std::shared_ptr<Subscription>;

    bool due(const Subscription& s, TimePoint now) const;
    // Returns nullopt on success, otherwise the reason to evict (if any).
    std::optional<EvictionEvent> flush(Subscription& s, TimePoint now, CycleReport& report);

    DispatcherConfig config_;
    mutable std::shared_mutex registry_mutex_;
    std::map<std::uint64_t, SubPtr> subs_;
    LeaseTable leases_;
    std::uint64_t next_id_ = 1;
};

}  // namespace telemetry::core
