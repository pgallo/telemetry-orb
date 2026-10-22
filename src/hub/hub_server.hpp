// Hub implementation: CORBA servants for Hub and TelemetrySource on a
// persistent child POA, a bed registry, the core Dispatcher and a dedicated
// publisher thread. Used by the hub executable and, in-process, by the
// integration test.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "Telemetry.hh"
#include "core/dispatcher.hpp"
#include "orb/corba_sink.hpp"

namespace telemetry::hub {

struct HubConfig {
    core::DispatcherConfig dispatcher;
    orb::SinkTimeouts sink;
    core::Millis tick{2};  // publisher thread period
    // Called for subscribe/unsubscribe/eviction events; default prints nothing.
    std::function<void(const std::string&)> log;
};

class HubServer;

class SourceImpl : public POA_Telemetry::TelemetrySource {
public:
    explicit SourceImpl(HubServer& hub) : hub_(hub) {}
    void registerBed(const Telemetry::BedInfo& info) override;
    void unregisterBed(const char* bed) override;
    void publish(const Telemetry::SampleBatch& batch) override;

private:
    HubServer& hub_;
};

class HubImpl : public POA_Telemetry::Hub {
public:
    explicit HubImpl(HubServer& hub) : hub_(hub) {}
    Telemetry::Subscription subscribe(Telemetry::Subscriber_ptr cb, const char* client_name,
                                      const Telemetry::Filter& f,
                                      const Telemetry::DeliveryQoS& qos) override;
    CORBA::ULong renew(Telemetry::SubscriptionId id) override;
    void unsubscribe(Telemetry::SubscriptionId id) override;
    Telemetry::DeliveryStats stats(Telemetry::SubscriptionId id) override;
    Telemetry::BedInfoSeq* listBeds() override;
    Telemetry::TelemetrySource_ptr source() override;

private:
    HubServer& hub_;
};

class HubServer {
public:
    // Creates the child POA and activates both servants. The root POA
    // manager must be activated by the caller (it is shared with any other
    // servants in the process).
    HubServer(CORBA::ORB_ptr orb, PortableServer::POA_ptr root_poa, HubConfig config = {});
    ~HubServer();
    HubServer(const HubServer&) = delete;
    HubServer& operator=(const HubServer&) = delete;

    void start();  // starts the publisher thread
    void stop();   // stops it and deactivates the servants; idempotent

    Telemetry::Hub_ptr hub_ref() const { return Telemetry::Hub::_duplicate(hub_ref_); }
    Telemetry::TelemetrySource_ptr source_ref() const {
        return Telemetry::TelemetrySource::_duplicate(source_ref_);
    }

    core::Dispatcher& dispatcher() { return dispatcher_; }

    struct Counters {
        std::uint64_t published = 0;
        std::uint64_t enqueued = 0;
        std::uint64_t batches_sent = 0;
        std::uint64_t evictions = 0;
        std::size_t beds = 0;
        std::size_t subscriptions = 0;
    };
    Counters counters() const;

private:
    friend class SourceImpl;
    friend class HubImpl;

    void publisher_loop();
    void log(const std::string& msg) const {
        if (config_.log) config_.log(msg);
    }

    HubConfig config_;
    CORBA::ORB_var orb_;
    PortableServer::POA_var poa_;
    core::Dispatcher dispatcher_;
    SourceImpl source_servant_;
    HubImpl hub_servant_;
    Telemetry::Hub_var hub_ref_;
    Telemetry::TelemetrySource_var source_ref_;

    mutable std::mutex beds_mutex_;
    std::map<std::string, Telemetry::BedInfo> beds_;

    std::atomic<std::uint64_t> published_{0};
    std::atomic<std::uint64_t> enqueued_{0};
    std::atomic<std::uint64_t> batches_sent_{0};
    std::atomic<std::uint64_t> evictions_{0};

    std::mutex run_mutex_;
    std::condition_variable run_cv_;
    bool running_ = false;
    bool stopped_ = false;
    std::thread publisher_;
};

}  // namespace telemetry::hub
