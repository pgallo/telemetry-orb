// Integration tests with a real ORB: the hub servants, the publisher thread,
// a station-side Subscriber servant and the CORBA sink, all in one process.
// The test binary owns the single ORB (see main at the bottom).
#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <thread>

#include "core/bed_table.hpp"
#include "hub/hub_server.hpp"
#include "orb/convert.hpp"
#include "station/subscriber_impl.hpp"

using namespace telemetry;
using namespace std::chrono_literals;

namespace {

CORBA::ORB_var g_orb;
PortableServer::POA_var g_root;

bool eventually(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

Telemetry::DeliveryQoS make_qos(CORBA::ULong queue = 1000, CORBA::ULong batch = 32, CORBA::ULong lat = 5,
                        CORBA::ULong lease = 5000) {
    Telemetry::DeliveryQoS q;
    q.maxQueue = queue;
    q.maxBatch = batch;
    q.maxLatencyMs = lat;
    q.minIntervalMs = 0;
    q.leaseMs = lease;
    return q;
}

Telemetry::Filter make_filter(std::vector<std::string> beds, std::vector<std::string> params) {
    return orb::to_idl(core::FilterSpec(std::move(beds), std::move(params)));
}

Telemetry::SampleBatch batch_of(const std::vector<std::pair<std::string, std::string>>& items) {
    std::vector<core::Sample> v;
    std::uint32_t seq = 0;
    for (const auto& [bed, param] : items) {
        core::Sample s;
        s.bed = bed;
        s.parameter = param;
        s.value = 42.0 + seq;
        s.units = "/min";
        s.source_time_ns = core::wall_clock_ns();
        s.seq = seq++;
        v.push_back(s);
    }
    Telemetry::SampleBatch_var b = orb::to_idl(v);
    return b.in();
}

// One station: a BedTable plus an activated Subscriber servant.
struct TestStation {
    core::BedTable table;
    station::SubscriberImpl* servant;
    PortableServer::ObjectId_var oid;
    Telemetry::Subscriber_var ref;
    bool active = true;

    TestStation() : servant(new station::SubscriberImpl(table)) {
        oid = g_root->activate_object(servant);
        CORBA::Object_var obj = g_root->id_to_reference(oid);
        ref = Telemetry::Subscriber::_narrow(obj);
    }
    void deactivate() {
        if (active) g_root->deactivate_object(oid);
        active = false;
    }
    ~TestStation() {
        deactivate();
        servant->_remove_ref();
    }
};

class HubIntegration : public ::testing::Test {
protected:
    void SetUp() override {
        hub::HubConfig cfg;
        cfg.dispatcher.lease.min = 100ms;
        cfg.dispatcher.max_consecutive_failures = 3;
        cfg.sink.call_ms = 300;
        cfg.tick = 1ms;
        cfg.log = [this](const std::string& m) {
            std::lock_guard lock(log_mutex);
            log.push_back(m);
        };
        server = std::make_unique<hub::HubServer>(g_orb, g_root, cfg);
        server->start();
        hub = server->hub_ref();
        source = hub->source();
        for (const char* bed : {"ICU-01", "ICU-02"}) {
            Telemetry::BedInfo info;
            info.bed = bed;
            info.ward = "ICU";
            info.label = "test";
            source->registerBed(info);
        }
    }
    void TearDown() override { server->stop(); }

    bool logged(const std::string& needle) {
        std::lock_guard lock(log_mutex);
        for (const auto& l : log)
            if (l.find(needle) != std::string::npos) return true;
        return false;
    }

    std::unique_ptr<hub::HubServer> server;
    Telemetry::Hub_var hub;
    Telemetry::TelemetrySource_var source;
    std::mutex log_mutex;
    std::vector<std::string> log;
};

}  // namespace

TEST_F(HubIntegration, ListBedsAndSourceAccessor) {
    Telemetry::BedInfoSeq_var beds = hub->listBeds();
    ASSERT_EQ(beds->length(), 2u);
    EXPECT_STREQ(beds[0u].bed.in(), "ICU-01");
    Telemetry::TelemetrySource_var again = hub->source();
    EXPECT_TRUE(again->_is_equivalent(source));
}

TEST_F(HubIntegration, PushDeliversOnlyMatchingSamples) {
    TestStation st;
    Telemetry::Subscription sub =
        hub->subscribe(st.ref, "station-a", make_filter({"ICU-01"}, {"HR", "SPO2"}), make_qos());
    EXPECT_GT(sub.id, 0u);
    EXPECT_EQ(sub.leaseMs, 5000u);

    source->publish(batch_of({{"ICU-01", "HR"}, {"ICU-02", "HR"}, {"ICU-01", "SPO2"},
                              {"ICU-01", "TEMP"}}));
    ASSERT_TRUE(eventually([&] { return st.table.totals().samples >= 2; }));
    std::this_thread::sleep_for(30ms);  // anything else would have arrived by now
    const auto t = st.table.totals();
    EXPECT_EQ(t.samples, 2u);
    EXPECT_EQ(t.beds, 1u);
    EXPECT_TRUE(st.table.has_value("ICU-01", "SPO2"));
    EXPECT_FALSE(st.table.has_value("ICU-01", "TEMP"));
    EXPECT_GT(t.latency_ms.count, 0u);

    const Telemetry::DeliveryStats stats = hub->stats(sub.id);
    EXPECT_EQ(stats.delivered, 2u);
    EXPECT_EQ(stats.dropped, 0u);
}

TEST_F(HubIntegration, TwoStationsWithDifferentFilters) {
    TestStation a, b;
    hub->subscribe(a.ref, "a", make_filter({}, {"HR"}), make_qos());
    hub->subscribe(b.ref, "b", make_filter({"ICU-02"}, {}), make_qos());
    for (int i = 0; i < 50; ++i)
        source->publish(batch_of({{"ICU-01", "HR"}, {"ICU-02", "HR"}, {"ICU-02", "RR"}}));
    ASSERT_TRUE(eventually([&] {
        return a.table.totals().samples == 100 && b.table.totals().samples == 100;
    }));
}

TEST_F(HubIntegration, PublishForUnknownBedRaisesAndDeliversNothing) {
    TestStation st;
    hub->subscribe(st.ref, "s", make_filter({}, {}), make_qos());
    try {
        source->publish(batch_of({{"ICU-01", "HR"}, {"ICU-99", "HR"}}));
        FAIL() << "expected UnknownBed";
    } catch (const Telemetry::UnknownBed& ex) {
        EXPECT_STREQ(ex.bed.in(), "ICU-99");
    }
    std::this_thread::sleep_for(30ms);
    EXPECT_EQ(st.table.totals().samples, 0u);
}

TEST_F(HubIntegration, InvalidFilterAndQosAreUserExceptions) {
    TestStation st;
    EXPECT_THROW(hub->subscribe(st.ref, "s", make_filter({"IC*U"}, {}), make_qos()),
                 Telemetry::InvalidFilter);
    EXPECT_THROW(hub->subscribe(st.ref, "s", make_filter({}, {}), make_qos(10, 20)),
                 Telemetry::InvalidQoS);
    EXPECT_THROW(hub->subscribe(Telemetry::Subscriber::_nil(), "s", make_filter({}, {}), make_qos()),
                 Telemetry::InvalidQoS);
    EXPECT_THROW(hub->renew(12345), Telemetry::UnknownSubscription);
    EXPECT_THROW(hub->unsubscribe(12345), Telemetry::UnknownSubscription);
}

TEST_F(HubIntegration, OverflowIsReportedToSubscriber) {
    TestStation st;
    // minIntervalMs holds pushes back long enough for the queue to overflow.
    Telemetry::DeliveryQoS q = make_qos(5, 5, 0);
    q.minIntervalMs = 200;
    Telemetry::Subscription sub = hub->subscribe(st.ref, "slow", make_filter({}, {}), q);
    source->publish(batch_of({{"ICU-01", "HR"}}));  // first push goes out at once
    ASSERT_TRUE(eventually([&] { return st.table.totals().samples == 1; }));
    std::vector<std::pair<std::string, std::string>> many(20, {"ICU-01", "HR"});
    source->publish(batch_of(many));
    ASSERT_TRUE(eventually([&] { return st.table.totals().samples == 6; }));
    EXPECT_EQ(st.table.totals().dropped_at_hub, 15u);
    const Telemetry::DeliveryStats stats = hub->stats(sub.id);
    EXPECT_EQ(stats.dropped, 15u);
}

TEST_F(HubIntegration, DeactivatedSubscriberIsEvicted) {
    TestStation st;
    Telemetry::Subscription sub = hub->subscribe(st.ref, "gone", make_filter({}, {}), make_qos());
    st.deactivate();  // the next push raises OBJECT_NOT_EXIST
    source->publish(batch_of({{"ICU-01", "HR"}}));
    ASSERT_TRUE(eventually([&] { return server->counters().evictions == 1; }));
    EXPECT_TRUE(logged("subscriber unreachable"));
    EXPECT_TRUE(logged("OBJECT_NOT_EXIST"));
    EXPECT_THROW(hub->renew(sub.id), Telemetry::UnknownSubscription);
}

TEST_F(HubIntegration, UnreachableSubscriberIsEvictedAfterRepeatedFailures) {
    // A reference to a port nothing listens on: every push raises TRANSIENT
    // (connection refused). Retries are disabled, so the hub counts three
    // failures and evicts instead of stalling the publisher thread.
    CORBA::Object_var obj = g_orb->string_to_object("corbaloc::127.0.0.1:1/NoSuchStation");
    Telemetry::Subscriber_var ghost = Telemetry::Subscriber::_unchecked_narrow(obj);
    TestStation healthy;
    hub->subscribe(ghost, "ghost", make_filter({}, {}), make_qos(100, 1, 0));
    hub->subscribe(healthy.ref, "healthy", make_filter({}, {}), make_qos(100, 1, 0));
    for (int i = 0; i < 5; ++i) source->publish(batch_of({{"ICU-01", "HR"}}));
    ASSERT_TRUE(eventually([&] { return server->counters().evictions == 1; }));
    EXPECT_TRUE(logged("too many consecutive failures"));
    EXPECT_TRUE(logged("TRANSIENT"));
    ASSERT_TRUE(eventually([&] { return healthy.table.totals().samples == 5; }));
    EXPECT_EQ(server->counters().subscriptions, 1u);
}

TEST_F(HubIntegration, LeaseExpiresWithoutRenewal) {
    TestStation st;
    Telemetry::Subscription sub = hub->subscribe(st.ref, "lazy", make_filter({}, {}),
                                                 make_qos(100, 32, 5, 150));
    EXPECT_EQ(sub.leaseMs, 150u);
    ASSERT_TRUE(eventually([&] { return server->counters().subscriptions == 0; }, 2000ms));
    EXPECT_TRUE(logged("lease expired"));
    EXPECT_THROW(hub->renew(sub.id), Telemetry::UnknownSubscription);
}

TEST_F(HubIntegration, SessionRenewsLeaseAndKeepsReceiving) {
    TestStation st;
    station::StationSession session(hub, st.ref, "session", make_filter({}, {}),
                                    make_qos(100, 32, 5, 150));
    session.start();
    for (int i = 0; i < 20; ++i) {
        source->publish(batch_of({{"ICU-02", "HR"}}));
        std::this_thread::sleep_for(25ms);  // 500 ms total, more than 3 leases
    }
    ASSERT_TRUE(eventually([&] { return st.table.totals().samples == 20; }));
    EXPECT_EQ(session.resubscriptions(), 0u);
    EXPECT_FALSE(logged("lease expired"));
    session.stop();
    EXPECT_EQ(server->counters().subscriptions, 0u);
}

TEST_F(HubIntegration, SessionResubscribesAfterEviction) {
    TestStation st;
    station::StationSession session(hub, st.ref, "phoenix", make_filter({}, {}),
                                    make_qos(100, 32, 5, 300));
    session.start();
    const auto first = session.id();
    hub->unsubscribe(first);  // as if the hub had evicted it
    ASSERT_TRUE(eventually([&] { return session.resubscriptions() == 1; }));
    EXPECT_NE(session.id(), first);
    source->publish(batch_of({{"ICU-01", "HR"}}));
    ASSERT_TRUE(eventually([&] { return st.table.totals().samples == 1; }));
    session.stop();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    g_orb = CORBA::ORB_init(argc, argv);
    CORBA::Object_var obj = g_orb->resolve_initial_references("RootPOA");
    g_root = PortableServer::POA::_narrow(obj);
    PortableServer::POAManager_var mgr = g_root->the_POAManager();
    mgr->activate();
    const int rc = RUN_ALL_TESTS();
    g_root = PortableServer::POA::_nil();
    g_orb->destroy();
    return rc;
}
