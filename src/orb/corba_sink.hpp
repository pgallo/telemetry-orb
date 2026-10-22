// Delivers batches to a remote Subscriber and classifies CORBA failures.
#pragma once

#include <string>

#include "Telemetry.hh"
#include "core/dispatcher.hpp"

namespace telemetry::orb {

struct SinkTimeouts {
    unsigned call_ms = 500;  // bounds a push into a full TCP buffer (slow station)
};

class CorbaSink final : public core::SubscriberSink {
public:
    CorbaSink(Telemetry::Subscriber_ptr subscriber, SinkTimeouts timeouts = {});

    core::DeliveryResult deliver(const std::vector<core::Sample>& batch,
                                 std::uint32_t dropped_since_last, std::string& detail) override;

    // How each system exception maps to a delivery outcome. Public so the
    // policy is unit tested without provoking real network failures.
    static core::DeliveryResult classify(const CORBA::SystemException& ex);

private:
    Telemetry::Subscriber_var subscriber_;
};

}  // namespace telemetry::orb
