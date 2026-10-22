#include "core/dispatcher.hpp"

#include <stdexcept>

namespace telemetry::core {

const char* to_string(EvictionReason r) {
    switch (r) {
        case EvictionReason::LeaseExpired: return "lease expired";
        case EvictionReason::SubscriberDead: return "subscriber unreachable";
        case EvictionReason::TooManyFailures: return "too many consecutive failures";
    }
    return "?";
}

void QosSpec::validate() const {
    if (max_queue == 0 || max_queue > 1'000'000)
        throw std::invalid_argument("maxQueue must be in 1..1000000");
    if (max_batch == 0 || max_batch > max_queue)
        throw std::invalid_argument("maxBatch must be in 1..maxQueue");
    if (max_latency.count() > 60'000) throw std::invalid_argument("maxLatencyMs must be <= 60000");
    if (min_interval.count() > 60'000) throw std::invalid_argument("minIntervalMs must be <= 60000");
    if (lease.count() == 0) throw std::invalid_argument("leaseMs must be > 0");
}

Dispatcher::Subscription::Subscription(std::uint64_t id_, std::string client_,
                                       std::unique_ptr<SubscriberSink> sink_, FilterSpec filter_,
                                       QosSpec qos_, std::size_t latency_window)
    : id(id_),
      client(std::move(client_)),
      sink(std::move(sink_)),
      filter(std::move(filter_)),
      qos(qos_),
      queue(qos_.max_queue),
      latency(latency_window) {}

Dispatcher::Dispatcher(DispatcherConfig config) : config_(config), leases_(config.lease) {}

Dispatcher::Granted Dispatcher::subscribe(std::unique_ptr<SubscriberSink> sink, std::string client,
                                          FilterSpec filter, QosSpec qos, TimePoint now) {
    if (!sink) throw std::invalid_argument("null subscriber");
    filter.validate();
    qos.validate();
    std::unique_lock lock(registry_mutex_);
    const auto id = next_id_++;
    const auto granted = leases_.grant(id, now, qos.lease);
    subs_.emplace(id, std::make_shared<Subscription>(id, std::move(client), std::move(sink),
                                                     std::move(filter), qos,
                                                     config_.latency_window));
    return {id, granted};
}

std::optional<Millis> Dispatcher::renew(std::uint64_t id, TimePoint now) {
    std::unique_lock lock(registry_mutex_);
    auto granted = leases_.renew(id, now);
    if (!granted) subs_.erase(id);  // expired before the sweep got to it
    return granted;
}

bool Dispatcher::unsubscribe(std::uint64_t id) {
    std::unique_lock lock(registry_mutex_);
    leases_.remove(id);
    return subs_.erase(id) != 0;
}

std::optional<SubscriptionStats> Dispatcher::stats(std::uint64_t id) const {
    SubPtr sub;
    {
        std::shared_lock lock(registry_mutex_);
        auto it = subs_.find(id);
        if (it == subs_.end()) return std::nullopt;
        sub = it->second;
    }
    SubscriptionStats st;
    st.queued = static_cast<std::uint32_t>(sub->queue.size());
    st.dropped = sub->queue.dropped_total();
    std::lock_guard lock(sub->stats_mutex);
    st.delivered = sub->delivered;
    st.consecutive_failures = sub->consecutive_failures;
    st.queue_latency_ms = sub->latency.snapshot();
    return st;
}

std::size_t Dispatcher::publish(const std::vector<Sample>& samples, TimePoint now) {
    // Shared lock: many publishers may run in parallel; each queue has its
    // own mutex. Subscribe/unsubscribe take the exclusive lock briefly.
    std::shared_lock lock(registry_mutex_);
    std::size_t enqueued = 0;
    for (const auto& [id, sub] : subs_) {
        for (const auto& s : samples) {
            if (sub->filter.matches(s.bed, s.parameter)) {
                sub->queue.push(Queued{s, now});
                ++enqueued;
            }
        }
    }
    return enqueued;
}

bool Dispatcher::due(const Subscription& s, TimePoint now) const {
    if (s.last_push && now - *s.last_push < s.qos.min_interval) return false;
    if (s.queue.size() >= s.qos.max_batch) return true;
    bool old_enough = false;
    s.queue.peek_front([&](const Queued& q) { old_enough = now - q.enqueued >= s.qos.max_latency; });
    return old_enough;
}

std::optional<EvictionEvent> Dispatcher::flush(Subscription& s, TimePoint now,
                                               CycleReport& report) {
    std::vector<Queued> taken;
    s.queue.drain(taken, s.qos.max_batch);
    if (taken.empty()) return std::nullopt;

    std::vector<Sample> batch;
    batch.reserve(taken.size());
    for (auto& q : taken) batch.push_back(q.sample);

    const std::uint64_t dropped_now = s.queue.dropped_total();
    std::uint64_t dropped_since;
    {
        std::lock_guard lock(s.stats_mutex);
        dropped_since = dropped_now - s.dropped_reported;
    }

    std::string detail;
    const DeliveryResult result = s.sink->deliver(
        batch, static_cast<std::uint32_t>(std::min<std::uint64_t>(dropped_since, UINT32_MAX)),
        detail);

    std::lock_guard lock(s.stats_mutex);
    s.last_push = now;
    switch (result) {
        case DeliveryResult::Ok:
            s.consecutive_failures = 0;
            s.delivered += batch.size();
            s.dropped_reported = dropped_now;
            for (const auto& q : taken) s.latency.record(to_ms(now - q.enqueued));
            ++report.batches_sent;
            report.samples_sent += batch.size();
            return std::nullopt;
        case DeliveryResult::Transient:
            // The batch is lost rather than re-queued: by the next attempt
            // newer samples are waiting, and they are what a station needs.
            ++s.consecutive_failures;
            if (s.consecutive_failures >= config_.max_consecutive_failures)
                return EvictionEvent{s.id, s.client, EvictionReason::TooManyFailures, detail};
            return std::nullopt;
        case DeliveryResult::Dead:
            return EvictionEvent{s.id, s.client, EvictionReason::SubscriberDead, detail};
    }
    return std::nullopt;
}

CycleReport Dispatcher::run_cycle(TimePoint now) {
    CycleReport report;
    std::vector<SubPtr> ready;
    {
        std::unique_lock lock(registry_mutex_);
        for (auto id : leases_.collect_expired(now)) {
            auto it = subs_.find(id);
            if (it == subs_.end()) continue;
            report.evicted.push_back(
                EvictionEvent{id, it->second->client, EvictionReason::LeaseExpired, "not renewed"});
            subs_.erase(it);
        }
        for (const auto& [id, sub] : subs_)
            if (due(*sub, now)) ready.push_back(sub);
    }

    // Deliver without holding the registry lock, so publish() and
    // subscribe() are never blocked behind a slow network call.
    std::vector<std::uint64_t> to_remove;
    for (auto& sub : ready) {
        if (auto ev = flush(*sub, now, report)) {
            to_remove.push_back(ev->id);
            report.evicted.push_back(std::move(*ev));
        }
    }
    if (!to_remove.empty()) {
        std::unique_lock lock(registry_mutex_);
        for (auto id : to_remove) {
            subs_.erase(id);
            leases_.remove(id);
        }
    }
    if (on_evict)
        for (const auto& ev : report.evicted) on_evict(ev);
    return report;
}

std::size_t Dispatcher::subscription_count() const {
    std::shared_lock lock(registry_mutex_);
    return subs_.size();
}

std::vector<std::uint64_t> Dispatcher::subscription_ids() const {
    std::shared_lock lock(registry_mutex_);
    std::vector<std::uint64_t> ids;
    for (const auto& [id, sub] : subs_) ids.push_back(id);
    return ids;
}

std::string Dispatcher::client_name(std::uint64_t id) const {
    std::shared_lock lock(registry_mutex_);
    auto it = subs_.find(id);
    return it == subs_.end() ? std::string() : it->second->client;
}

}  // namespace telemetry::core
