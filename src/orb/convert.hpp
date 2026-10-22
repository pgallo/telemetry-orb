// Conversions between IDL-generated types and the ORB-free core model.
#pragma once

#include <vector>

#include "Telemetry.hh"
#include "core/dispatcher.hpp"
#include "core/filter.hpp"
#include "core/sample.hpp"

namespace telemetry::orb {

core::Sample from_idl(const Telemetry::VitalSample& s);
void to_idl(const core::Sample& in, Telemetry::VitalSample& out);

std::vector<core::Sample> from_idl(const Telemetry::SampleBatch& batch);
Telemetry::SampleBatch* to_idl(const std::vector<core::Sample>& batch);

core::FilterSpec from_idl(const Telemetry::Filter& f);
Telemetry::Filter to_idl(const core::FilterSpec& f);

core::QosSpec from_idl(const Telemetry::DeliveryQoS& q);
Telemetry::DeliveryQoS to_idl(const core::QosSpec& q);

Telemetry::DeliveryStats to_idl(const core::SubscriptionStats& s);

}  // namespace telemetry::orb
