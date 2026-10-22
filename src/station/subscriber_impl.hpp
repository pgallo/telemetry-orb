// Station side: the Subscriber servant that receives pushed batches, and a
// session object that subscribes, renews the lease from its own thread and
// resubscribes when the hub reports the subscription as unknown.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "Telemetry.hh"
#include "core/bed_table.hpp"

namespace telemetry::station {

class SubscriberImpl : public POA_Telemetry::Subscriber {
public:
    explicit SubscriberImpl(core::BedTable& table) : table_(table) {}

    void push(const Telemetry::SampleBatch& batch, CORBA::ULong dropped_since_last) override;

    std::uint64_t batches() const { return batches_; }
    // Optional hook for tests: called after each batch is applied.
    std::function<void(std::size_t samples)> on_batch;

private:
    core::BedTable& table_;
    std::atomic<std::uint64_t> batches_{0};
};

class StationSession {
public:
    StationSession(Telemetry::Hub_ptr hub, Telemetry::Subscriber_ptr self, std::string name,
                   Telemetry::Filter filter, Telemetry::DeliveryQoS qos);
    ~StationSession();

    void start();  // subscribes, then renews at a third of the granted lease
    void stop();   // stops renewing and unsubscribes (best effort)

    Telemetry::SubscriptionId id() const { return id_; }
    std::uint32_t resubscriptions() const { return resubscriptions_; }
    // Fetches the hub's view of this subscription; throws on CORBA errors.
    Telemetry::DeliveryStats stats();

    std::function<void(const std::string&)> log;

private:
    void subscribe_once();
    void renew_loop();

    Telemetry::Hub_var hub_;
    Telemetry::Subscriber_var self_;
    std::string name_;
    Telemetry::Filter filter_;
    Telemetry::DeliveryQoS qos_;

    std::atomic<Telemetry::SubscriptionId> id_{0};
    std::atomic<std::uint32_t> lease_ms_{0};
    std::atomic<std::uint32_t> resubscriptions_{0};

    std::mutex mutex_;
    std::condition_variable cv_;
    bool running_ = false;
    std::thread renewer_;
};

}  // namespace telemetry::station
