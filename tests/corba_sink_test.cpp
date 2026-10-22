// The failure classification policy, independent of real network failures.
#include <gtest/gtest.h>

#include "core/sample.hpp"
#include "orb/convert.hpp"
#include "orb/corba_sink.hpp"

using telemetry::core::DeliveryResult;
using telemetry::orb::CorbaSink;

TEST(CorbaSinkClassify, GoneMeansDead) {
    EXPECT_EQ(CorbaSink::classify(CORBA::OBJECT_NOT_EXIST()), DeliveryResult::Dead);
    EXPECT_EQ(CorbaSink::classify(CORBA::COMM_FAILURE()), DeliveryResult::Dead);
    EXPECT_EQ(CorbaSink::classify(CORBA::INV_OBJREF()), DeliveryResult::Dead);
}

TEST(CorbaSinkClassify, MaybeLaterMeansTransient) {
    EXPECT_EQ(CorbaSink::classify(CORBA::TRANSIENT()), DeliveryResult::Transient);
    EXPECT_EQ(CorbaSink::classify(CORBA::TIMEOUT()), DeliveryResult::Transient);
    EXPECT_EQ(CorbaSink::classify(CORBA::NO_RESOURCES()), DeliveryResult::Transient);
}

TEST(Convert, SampleRoundTrip) {
    telemetry::core::Sample s;
    s.bed = "ICU-07";
    s.parameter = "SPO2";
    s.value = 93.0;
    s.units = "%";
    s.quality = telemetry::core::Quality::Questionable;
    s.source_time_ns = 1'790'000'000'123'456'789ull;
    s.seq = 4'000'000'000u;
    Telemetry::VitalSample idl;
    telemetry::orb::to_idl(s, idl);
    const auto back = telemetry::orb::from_idl(idl);
    EXPECT_EQ(back.bed, s.bed);
    EXPECT_EQ(back.parameter, s.parameter);
    EXPECT_EQ(back.value, s.value);
    EXPECT_EQ(back.units, s.units);
    EXPECT_EQ(back.quality, s.quality);
    EXPECT_EQ(back.source_time_ns, s.source_time_ns);
    EXPECT_EQ(back.seq, s.seq);
}

TEST(Convert, QosAndFilterRoundTrip) {
    telemetry::core::QosSpec q;
    q.max_queue = 77;
    q.max_batch = 7;
    q.max_latency = telemetry::core::Millis(12);
    q.min_interval = telemetry::core::Millis(34);
    q.lease = telemetry::core::Millis(5678);
    const auto back = telemetry::orb::from_idl(telemetry::orb::to_idl(q));
    EXPECT_EQ(back.max_queue, 77u);
    EXPECT_EQ(back.max_batch, 7u);
    EXPECT_EQ(back.max_latency.count(), 12);
    EXPECT_EQ(back.min_interval.count(), 34);
    EXPECT_EQ(back.lease.count(), 5678);

    telemetry::core::FilterSpec f({"ICU-0*"}, {"HR", "SPO2"});
    const auto fb = telemetry::orb::from_idl(telemetry::orb::to_idl(f));
    EXPECT_EQ(fb.beds(), f.beds());
    EXPECT_EQ(fb.parameters(), f.parameters());
}
