#include "station/subscriber_impl.hpp"

#include "core/sample.hpp"
#include "orb/convert.hpp"
#include "orb/orb_util.hpp"

namespace telemetry::station {

void SubscriberImpl::push(const Telemetry::SampleBatch& batch, CORBA::ULong dropped_since_last) {
    const auto now = core::wall_clock_ns();
    const auto samples = orb::from_idl(batch);
    table_.update(samples, now, dropped_since_last);
    ++batches_;
    if (on_batch) on_batch(samples.size());
}

StationSession::StationSession(Telemetry::Hub_ptr hub, Telemetry::Subscriber_ptr self,
                               std::string name, Telemetry::Filter filter, Telemetry::DeliveryQoS qos)
    : hub_(Telemetry::Hub::_duplicate(hub)),
      self_(Telemetry::Subscriber::_duplicate(self)),
      name_(std::move(name)),
      filter_(std::move(filter)),
      qos_(qos) {}

StationSession::~StationSession() { stop(); }

void StationSession::subscribe_once() {
    const Telemetry::Subscription sub = hub_->subscribe(self_, name_.c_str(), filter_, qos_);
    id_ = sub.id;
    lease_ms_ = sub.leaseMs;
    if (log)
        log("subscribed as #" + std::to_string(sub.id) + ", lease " + std::to_string(sub.leaseMs) +
            " ms");
}

void StationSession::start() {
    subscribe_once();
    std::lock_guard lock(mutex_);
    running_ = true;
    renewer_ = std::thread([this] { renew_loop(); });
}

void StationSession::renew_loop() {
    std::unique_lock lock(mutex_);
    while (running_) {
        const auto period = std::chrono::milliseconds(std::max<std::uint32_t>(lease_ms_ / 3, 50));
        if (cv_.wait_for(lock, period, [this] { return !running_; })) break;
        lock.unlock();
        try {
            lease_ms_ = hub_->renew(id_);
        } catch (const Telemetry::UnknownSubscription&) {
            // Lease lapsed or we were evicted (e.g. after a network hiccup):
            // start over with a fresh subscription.
            try {
                subscribe_once();
                ++resubscriptions_;
            } catch (const CORBA::Exception& ex) {
                if (log) log("resubscribe failed: " + orb::describe(ex));
            }
        } catch (const CORBA::Exception& ex) {
            if (log) log("renew failed: " + orb::describe(ex));
        }
        lock.lock();
    }
}

void StationSession::stop() {
    {
        std::lock_guard lock(mutex_);
        if (!running_ && !renewer_.joinable()) return;
        running_ = false;
    }
    cv_.notify_all();
    if (renewer_.joinable()) renewer_.join();
    try {
        hub_->unsubscribe(id_);
    } catch (const CORBA::Exception&) {
        // Hub gone or subscription already expired: nothing to clean up.
    }
}

Telemetry::DeliveryStats StationSession::stats() {
    return hub_->stats(id_);
}

}  // namespace telemetry::station
