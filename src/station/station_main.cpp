// telemetry-station: central monitoring station. Subscribes to the hub and
// prints a rolling per-bed table with end-to-end latency.
//
//   telemetry-station --ior-file hub.ior [--name station-1] [--beds ICU-0*]
//                     [--params HR,SPO2] [--max-queue 500] [--max-batch 64]
//                     [--max-latency-ms 5] [--min-interval-ms 0] [--lease-ms 3000]
//                     [--refresh-ms 1000] [--duration-ms 0] [--naming telemetry/Hub]
#include <chrono>
#include <cstdio>
#include <iostream>

#include "core/bed_table.hpp"
#include "orb/args.hpp"
#include "orb/orb_util.hpp"
#include "station/subscriber_impl.hpp"

using namespace telemetry;

int main(int argc, char** argv) {
    const sigset_t signals = orb::block_termination_signals();
    CORBA::ORB_var orb = CORBA::ORB_init(argc, argv);
    int rc = 0;
    try {
        orb::Args args(argc, argv);
        const std::string name = args.str("name", "station-1");

        CORBA::Object_var obj =
            args.has("naming")
                ? orb::resolve_name(orb, args.str("naming"))
                : orb::read_ior_file(orb, args.str("ior-file", "hub.ior"), std::chrono::seconds(10));
        Telemetry::Hub_var hub = Telemetry::Hub::_narrow(obj);
        if (CORBA::is_nil(hub)) throw std::runtime_error("reference is not a Telemetry::Hub");

        CORBA::Object_var poa_obj = orb->resolve_initial_references("RootPOA");
        PortableServer::POA_var poa = PortableServer::POA::_narrow(poa_obj);
        PortableServer::POAManager_var mgr = poa->the_POAManager();
        mgr->activate();

        core::BedTable table;
        auto* servant = new station::SubscriberImpl(table);
        PortableServer::ObjectId_var oid = poa->activate_object(servant);
        servant->_remove_ref();  // the POA owns it now
        CORBA::Object_var self_obj = poa->id_to_reference(oid);
        Telemetry::Subscriber_var self = Telemetry::Subscriber::_narrow(self_obj);

        Telemetry::Filter filter;
        {
            const auto beds = orb::split_csv(args.str("beds"));
            const auto params = orb::split_csv(args.str("params"));
            filter.beds.length(static_cast<CORBA::ULong>(beds.size()));
            for (CORBA::ULong i = 0; i < beds.size(); ++i) filter.beds[i] = beds[i].c_str();
            filter.parameters.length(static_cast<CORBA::ULong>(params.size()));
            for (CORBA::ULong i = 0; i < params.size(); ++i)
                filter.parameters[i] = params[i].c_str();
        }
        Telemetry::DeliveryQoS qos;
        qos.maxQueue = static_cast<CORBA::ULong>(args.num("max-queue", 500));
        qos.maxBatch = static_cast<CORBA::ULong>(args.num("max-batch", 64));
        qos.maxLatencyMs = static_cast<CORBA::ULong>(args.num("max-latency-ms", 5));
        qos.minIntervalMs = static_cast<CORBA::ULong>(args.num("min-interval-ms", 0));
        qos.leaseMs = static_cast<CORBA::ULong>(args.num("lease-ms", 3000));

        station::StationSession session(hub, self, name, filter, qos);
        session.log = [&](const std::string& m) {
            std::printf("[%s] %s\n", name.c_str(), m.c_str());
            std::fflush(stdout);
        };
        session.start();

        const auto refresh = std::chrono::milliseconds(args.num("refresh-ms", 1000));
        const auto duration = std::chrono::milliseconds(args.num("duration-ms", 0));
        const auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            auto wait = refresh;
            if (duration.count() > 0) {
                const auto left = duration - std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now() - t0);
                if (left.count() <= 0) break;
                wait = std::min(wait, left);
            }
            if (orb::wait_for_termination(signals, wait) != 0) break;
            const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0);
            std::printf("\n[%s] t=%.1fs\n%s", name.c_str(), elapsed.count(), table.render().c_str());
            std::fflush(stdout);
        }

        const auto t = table.totals();
        try {
            const auto st = session.stats();
            std::printf("[%s] hub view: delivered=%llu dropped=%llu queue p50/p99/max=%.2f/%.2f/%.2f ms\n",
                        name.c_str(), static_cast<unsigned long long>(st.delivered),
                        static_cast<unsigned long long>(st.dropped), st.queueLatencyP50Ms,
                        st.queueLatencyP99Ms, st.queueLatencyMaxMs);
        } catch (const CORBA::Exception& ex) {
            std::printf("[%s] hub stats unavailable: %s\n", name.c_str(), orb::describe(ex).c_str());
        }
        std::printf("[%s] received %llu samples in %llu batches from %zu beds, "
                    "e2e latency p50=%.2f ms p99=%.2f ms max=%.2f ms, dropped at hub=%llu\n",
                    name.c_str(), static_cast<unsigned long long>(t.samples),
                    static_cast<unsigned long long>(t.batches), t.beds, t.latency_ms.p50,
                    t.latency_ms.p99, t.latency_ms.max,
                    static_cast<unsigned long long>(t.dropped_at_hub));
        session.stop();
        poa->deactivate_object(oid);
    } catch (const CORBA::Exception& ex) {
        std::cerr << "station: CORBA exception: " << orb::describe(ex) << '\n';
        rc = 1;
    } catch (const std::exception& ex) {
        std::cerr << "station: " << ex.what() << '\n';
        rc = 1;
    }
    orb->destroy();
    return rc;
}
