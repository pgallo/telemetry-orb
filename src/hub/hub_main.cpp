// telemetry-hub: hosts the Hub and TelemetrySource objects.
//
//   telemetry-hub --ior-file hub.ior [--naming telemetry/Hub] [--stats-ms 1000]
//                 [--duration-ms 0] [ORB options, e.g. -ORBendPoint giop:tcp::5050]
#include <chrono>
#include <cstdio>
#include <iostream>
#include <mutex>

#include "hub/hub_server.hpp"
#include "orb/args.hpp"
#include "orb/orb_util.hpp"

using namespace telemetry;

int main(int argc, char** argv) {
    const sigset_t signals = orb::block_termination_signals();
    CORBA::ORB_var orb = CORBA::ORB_init(argc, argv);
    int rc = 0;
    try {
        orb::Args args(argc, argv);
        std::mutex log_mutex;
        auto log = [&](const std::string& m) {
            std::lock_guard lock(log_mutex);
            std::printf("[hub] %s\n", m.c_str());
            std::fflush(stdout);
        };

        CORBA::Object_var obj = orb->resolve_initial_references("RootPOA");
        PortableServer::POA_var root = PortableServer::POA::_narrow(obj);

        hub::HubConfig cfg;
        cfg.log = log;
        cfg.dispatcher.lease.min = core::Millis(args.num("lease-min-ms", 500));
        cfg.dispatcher.lease.max = core::Millis(args.num("lease-max-ms", 60000));
        cfg.dispatcher.max_consecutive_failures =
            static_cast<std::uint32_t>(args.num("max-failures", 3));
        cfg.sink.call_ms = static_cast<unsigned>(args.num("push-timeout-ms", 500));

        hub::HubServer server(orb, root, cfg);
        PortableServer::POAManager_var mgr = root->the_POAManager();
        mgr->activate();
        server.start();

        Telemetry::Hub_var ref = server.hub_ref();
        const std::string ior_file = args.str("ior-file", "hub.ior");
        orb::write_ior_file(orb, ref, ior_file);
        log("IOR written to " + ior_file);
        if (args.has("naming")) {
            orb::bind_name(orb, args.str("naming"), ref);
            log("bound in Naming Service as " + args.str("naming"));
        }

        // Main thread: print counters periodically until signalled.
        const auto stats_every = std::chrono::milliseconds(args.num("stats-ms", 1000));
        const auto duration = std::chrono::milliseconds(args.num("duration-ms", 0));
        const auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            auto wait = stats_every;
            if (duration.count() > 0) {
                const auto left = duration - std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now() - t0);
                if (left.count() <= 0) break;
                wait = std::min(wait, left);
            }
            const int sig = orb::wait_for_termination(signals, wait);
            if (sig != 0) {
                log(std::string("signal ") + std::to_string(sig) + ", shutting down");
                break;
            }
            const auto c = server.counters();
            char buf[200];
            std::snprintf(buf, sizeof buf,
                          "beds=%zu subs=%zu published=%llu enqueued=%llu batches=%llu evictions=%llu",
                          c.beds, c.subscriptions, static_cast<unsigned long long>(c.published),
                          static_cast<unsigned long long>(c.enqueued),
                          static_cast<unsigned long long>(c.batches_sent),
                          static_cast<unsigned long long>(c.evictions));
            log(buf);
        }
        server.stop();
    } catch (const CORBA::Exception& ex) {
        std::cerr << "hub: CORBA exception: " << orb::describe(ex) << '\n';
        rc = 1;
    } catch (const std::exception& ex) {
        std::cerr << "hub: " << ex.what() << '\n';
        rc = 1;
    }
    orb->destroy();
    return rc;
}
