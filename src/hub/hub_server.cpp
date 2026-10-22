#include "hub/hub_server.hpp"

#include <stdexcept>

#include "orb/convert.hpp"

namespace telemetry::hub {

using core::SteadyClock;

// ---------------------------------------------------------------- Source

void SourceImpl::registerBed(const Telemetry::BedInfo& info) {
    std::string bed = info.bed.in();
    if (bed.empty()) throw CORBA::BAD_PARAM();
    {
        std::lock_guard lock(hub_.beds_mutex_);
        hub_.beds_[bed] = info;
    }
    hub_.log("bed registered: " + bed + " (" + std::string(info.ward.in()) + ")");
}

void SourceImpl::unregisterBed(const char* bed) {
    std::lock_guard lock(hub_.beds_mutex_);
    if (hub_.beds_.erase(bed) == 0) throw Telemetry::UnknownBed(bed);
}

void SourceImpl::publish(const Telemetry::SampleBatch& batch) {
    // Validate the whole batch before enqueuing any of it, so a producer
    // that gets UnknownBed knows nothing from this call was delivered.
    {
        std::lock_guard lock(hub_.beds_mutex_);
        for (CORBA::ULong i = 0; i < batch.length(); ++i)
            if (hub_.beds_.find(batch[i].bed.in()) == hub_.beds_.end())
                throw Telemetry::UnknownBed(batch[i].bed);
    }
    const auto samples = orb::from_idl(batch);
    const auto n = hub_.dispatcher_.publish(samples, SteadyClock::now());
    hub_.published_ += samples.size();
    hub_.enqueued_ += n;
}

// ------------------------------------------------------------------- Hub

Telemetry::Subscription HubImpl::subscribe(Telemetry::Subscriber_ptr cb, const char* client_name,
                                           const Telemetry::Filter& f,
                                           const Telemetry::DeliveryQoS& qos) {
    if (CORBA::is_nil(cb)) throw Telemetry::InvalidQoS("nil subscriber reference");
    auto filter = orb::from_idl(f);
    auto spec = orb::from_idl(qos);
    try {
        filter.validate();
    } catch (const std::invalid_argument& e) {
        throw Telemetry::InvalidFilter(e.what());
    }
    try {
        spec.validate();
    } catch (const std::invalid_argument& e) {
        throw Telemetry::InvalidQoS(e.what());
    }
    const std::string desc = filter.describe();
    auto granted = hub_.dispatcher_.subscribe(
        std::make_unique<orb::CorbaSink>(cb, hub_.config_.sink), client_name, std::move(filter),
        spec, SteadyClock::now());
    hub_.log("subscribe #" + std::to_string(granted.id) + " " + client_name + " " + desc +
             " queue=" + std::to_string(spec.max_queue) + " batch=" +
             std::to_string(spec.max_batch) + " lease=" + std::to_string(granted.lease.count()) +
             "ms");
    Telemetry::Subscription out;
    out.id = granted.id;
    out.leaseMs = static_cast<CORBA::ULong>(granted.lease.count());
    return out;
}

CORBA::ULong HubImpl::renew(Telemetry::SubscriptionId id) {
    auto granted = hub_.dispatcher_.renew(id, SteadyClock::now());
    if (!granted) throw Telemetry::UnknownSubscription(id);
    return static_cast<CORBA::ULong>(granted->count());
}

void HubImpl::unsubscribe(Telemetry::SubscriptionId id) {
    const std::string name = hub_.dispatcher_.client_name(id);
    if (!hub_.dispatcher_.unsubscribe(id)) throw Telemetry::UnknownSubscription(id);
    hub_.log("unsubscribe #" + std::to_string(id) + " " + name);
}

Telemetry::DeliveryStats HubImpl::stats(Telemetry::SubscriptionId id) {
    auto st = hub_.dispatcher_.stats(id);
    if (!st) throw Telemetry::UnknownSubscription(id);
    return orb::to_idl(*st);
}

Telemetry::BedInfoSeq* HubImpl::listBeds() {
    std::lock_guard lock(hub_.beds_mutex_);
    auto* seq = new Telemetry::BedInfoSeq;
    seq->length(static_cast<CORBA::ULong>(hub_.beds_.size()));
    CORBA::ULong i = 0;
    for (const auto& [bed, info] : hub_.beds_) (*seq)[i++] = info;
    return seq;
}

Telemetry::TelemetrySource_ptr HubImpl::source() { return hub_.source_ref(); }

// ------------------------------------------------------------- HubServer

namespace {

// PERSISTENT lifespan + USER_ID: object keys are "hub" and "source", so if
// the hub restarts on the same endpoint (-ORBendPoint giop:tcp::<port>) the
// references held by producers and stations stay valid and they recover by
// retrying instead of re-resolving.
PortableServer::POA_ptr create_hub_poa(PortableServer::POA_ptr root) {
    CORBA::PolicyList policies;
    policies.length(2);
    policies[0] = root->create_lifespan_policy(PortableServer::PERSISTENT);
    policies[1] = root->create_id_assignment_policy(PortableServer::USER_ID);
    PortableServer::POAManager_var mgr = root->the_POAManager();
    PortableServer::POA_ptr poa = root->create_POA("TelemetryHub", mgr, policies);
    for (CORBA::ULong i = 0; i < policies.length(); ++i) policies[i]->destroy();
    return poa;
}

}  // namespace

HubServer::HubServer(CORBA::ORB_ptr orb, PortableServer::POA_ptr root_poa, HubConfig config)
    : config_(std::move(config)),
      orb_(CORBA::ORB::_duplicate(orb)),
      poa_(create_hub_poa(root_poa)),
      dispatcher_(config_.dispatcher),
      source_servant_(*this),
      hub_servant_(*this) {
    PortableServer::ObjectId_var hub_id = PortableServer::string_to_ObjectId("hub");
    PortableServer::ObjectId_var source_id = PortableServer::string_to_ObjectId("source");
    poa_->activate_object_with_id(hub_id, &hub_servant_);
    poa_->activate_object_with_id(source_id, &source_servant_);
    CORBA::Object_var h = poa_->id_to_reference(hub_id);
    CORBA::Object_var s = poa_->id_to_reference(source_id);
    hub_ref_ = Telemetry::Hub::_narrow(h);
    source_ref_ = Telemetry::TelemetrySource::_narrow(s);

    dispatcher_.on_evict = [this](const core::EvictionEvent& ev) {
        ++evictions_;
        log("evicted #" + std::to_string(ev.id) + " " + ev.client + ": " + to_string(ev.reason) +
            (ev.detail.empty() ? "" : " [" + ev.detail + "]"));
    };
}

HubServer::~HubServer() { stop(); }

void HubServer::start() {
    std::lock_guard lock(run_mutex_);
    if (running_ || stopped_) return;
    running_ = true;
    publisher_ = std::thread([this] { publisher_loop(); });
}

void HubServer::stop() {
    {
        std::lock_guard lock(run_mutex_);
        if (stopped_) return;
        stopped_ = true;
        running_ = false;
    }
    run_cv_.notify_all();
    if (publisher_.joinable()) publisher_.join();
    try {
        // Wait for in-flight requests on our servants before they are destroyed.
        poa_->destroy(true, true);
    } catch (const CORBA::Exception&) {
        // ORB already shut down; nothing left to deactivate.
    }
}

void HubServer::publisher_loop() {
    std::unique_lock lock(run_mutex_);
    while (running_) {
        lock.unlock();
        const auto report = dispatcher_.run_cycle(SteadyClock::now());
        batches_sent_ += report.batches_sent;
        lock.lock();
        run_cv_.wait_for(lock, config_.tick, [this] { return !running_; });
    }
}

HubServer::Counters HubServer::counters() const {
    Counters c;
    c.published = published_;
    c.enqueued = enqueued_;
    c.batches_sent = batches_sent_;
    c.evictions = evictions_;
    {
        std::lock_guard lock(beds_mutex_);
        c.beds = beds_.size();
    }
    c.subscriptions = dispatcher_.subscription_count();
    return c;
}

}  // namespace telemetry::hub
