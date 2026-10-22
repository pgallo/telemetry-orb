#include "orb/corba_sink.hpp"

#include <omniORB4/omniORB.h>

#include "orb/convert.hpp"

namespace telemetry::orb {

namespace {

// The ORB's default behaviour on TRANSIENT is to retry the call, with
// back-off, before raising. For fan-out that would let one unreachable
// station stall the publisher thread for everyone, so failures surface
// immediately and the dispatcher's own failure counter decides.
CORBA::Boolean no_retry_transient(void*, CORBA::ULong, const CORBA::TRANSIENT&) { return false; }
CORBA::Boolean no_retry_comm(void*, CORBA::ULong, const CORBA::COMM_FAILURE&) { return false; }

}  // namespace

CorbaSink::CorbaSink(Telemetry::Subscriber_ptr subscriber, SinkTimeouts timeouts)
    : subscriber_(Telemetry::Subscriber::_duplicate(subscriber)) {
    omniORB::installTransientExceptionHandler(subscriber_.in(), nullptr, no_retry_transient);
    omniORB::installCommFailureExceptionHandler(subscriber_.in(), nullptr, no_retry_comm);
    omniORB::setClientCallTimeout(subscriber_.in(), timeouts.call_ms);
}

core::DeliveryResult CorbaSink::classify(const CORBA::SystemException& ex) {
    // Dead: the object or its process is definitely gone or the connection
    // broke mid-call. Transient: might succeed later (refused connect while a
    // station restarts, timeout because it is momentarily slow).
    if (dynamic_cast<const CORBA::OBJECT_NOT_EXIST*>(&ex)) return core::DeliveryResult::Dead;
    if (dynamic_cast<const CORBA::COMM_FAILURE*>(&ex)) return core::DeliveryResult::Dead;
    if (dynamic_cast<const CORBA::INV_OBJREF*>(&ex)) return core::DeliveryResult::Dead;
    if (dynamic_cast<const CORBA::TRANSIENT*>(&ex)) return core::DeliveryResult::Transient;
    if (dynamic_cast<const CORBA::TIMEOUT*>(&ex)) return core::DeliveryResult::Transient;
    return core::DeliveryResult::Transient;
}

core::DeliveryResult CorbaSink::deliver(const std::vector<core::Sample>& batch,
                                        std::uint32_t dropped_since_last, std::string& detail) {
    try {
        Telemetry::SampleBatch_var seq = to_idl(batch);
        subscriber_->push(seq.in(), dropped_since_last);
        return core::DeliveryResult::Ok;
    } catch (const CORBA::SystemException& ex) {
        detail = std::string(ex._name()) + " (minor " + std::to_string(ex.minor()) + ")";
        return classify(ex);
    } catch (const CORBA::Exception& ex) {
        detail = ex._name();
        return core::DeliveryResult::Dead;
    }
}

}  // namespace telemetry::orb
