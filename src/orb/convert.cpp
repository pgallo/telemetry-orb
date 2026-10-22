#include "orb/convert.hpp"

namespace telemetry::orb {

namespace {

core::Quality from_idl(Telemetry::SampleQuality q) {
    switch (q) {
        case Telemetry::QUESTIONABLE: return core::Quality::Questionable;
        case Telemetry::INVALID: return core::Quality::Invalid;
        default: return core::Quality::Valid;
    }
}

Telemetry::SampleQuality to_idl(core::Quality q) {
    switch (q) {
        case core::Quality::Questionable: return Telemetry::QUESTIONABLE;
        case core::Quality::Invalid: return Telemetry::INVALID;
        default: return Telemetry::VALID;
    }
}

std::vector<std::string> from_idl(const Telemetry::StringSeq& seq) {
    std::vector<std::string> out;
    out.reserve(seq.length());
    for (CORBA::ULong i = 0; i < seq.length(); ++i) out.emplace_back(seq[i].in());
    return out;
}

Telemetry::StringSeq to_idl(const std::vector<std::string>& v) {
    Telemetry::StringSeq seq;
    seq.length(static_cast<CORBA::ULong>(v.size()));
    for (CORBA::ULong i = 0; i < seq.length(); ++i) seq[i] = v[i].c_str();
    return seq;
}

}  // namespace

core::Sample from_idl(const Telemetry::VitalSample& s) {
    core::Sample out;
    out.bed = s.bed.in();
    out.parameter = s.parameter.in();
    out.value = s.value;
    out.units = s.units.in();
    out.quality = from_idl(s.quality);
    out.source_time_ns = s.sourceTime;
    out.seq = s.seq;
    return out;
}

void to_idl(const core::Sample& in, Telemetry::VitalSample& out) {
    out.bed = in.bed.c_str();
    out.parameter = in.parameter.c_str();
    out.value = in.value;
    out.units = in.units.c_str();
    out.quality = to_idl(in.quality);
    out.sourceTime = in.source_time_ns;
    out.seq = in.seq;
}

std::vector<core::Sample> from_idl(const Telemetry::SampleBatch& batch) {
    std::vector<core::Sample> out;
    out.reserve(batch.length());
    for (CORBA::ULong i = 0; i < batch.length(); ++i) out.push_back(from_idl(batch[i]));
    return out;
}

Telemetry::SampleBatch* to_idl(const std::vector<core::Sample>& batch) {
    auto* seq = new Telemetry::SampleBatch;
    seq->length(static_cast<CORBA::ULong>(batch.size()));
    for (CORBA::ULong i = 0; i < seq->length(); ++i) to_idl(batch[i], (*seq)[i]);
    return seq;
}

core::FilterSpec from_idl(const Telemetry::Filter& f) {
    return core::FilterSpec(from_idl(f.beds), from_idl(f.parameters));
}

Telemetry::Filter to_idl(const core::FilterSpec& f) {
    Telemetry::Filter out;
    out.beds = to_idl(f.beds());
    out.parameters = to_idl(f.parameters());
    return out;
}

core::QosSpec from_idl(const Telemetry::DeliveryQoS& q) {
    core::QosSpec out;
    out.max_queue = q.maxQueue;
    out.max_batch = q.maxBatch;
    out.max_latency = core::Millis(q.maxLatencyMs);
    out.min_interval = core::Millis(q.minIntervalMs);
    out.lease = core::Millis(q.leaseMs);
    return out;
}

Telemetry::DeliveryQoS to_idl(const core::QosSpec& q) {
    Telemetry::DeliveryQoS out;
    out.maxQueue = q.max_queue;
    out.maxBatch = q.max_batch;
    out.maxLatencyMs = static_cast<CORBA::ULong>(q.max_latency.count());
    out.minIntervalMs = static_cast<CORBA::ULong>(q.min_interval.count());
    out.leaseMs = static_cast<CORBA::ULong>(q.lease.count());
    return out;
}

Telemetry::DeliveryStats to_idl(const core::SubscriptionStats& s) {
    Telemetry::DeliveryStats out;
    out.delivered = s.delivered;
    out.dropped = s.dropped;
    out.queued = s.queued;
    out.consecutiveFailures = s.consecutive_failures;
    out.queueLatencyP50Ms = s.queue_latency_ms.p50;
    out.queueLatencyP99Ms = s.queue_latency_ms.p99;
    out.queueLatencyMaxMs = s.queue_latency_ms.max;
    return out;
}

}  // namespace telemetry::orb
