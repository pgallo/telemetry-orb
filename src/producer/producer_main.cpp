// telemetry-producer: simulates N beds and publishes their vitals to the hub.
//
//   telemetry-producer --ior-file hub.ior [--beds 6] [--rate-hz 4] [--seed 1]
//                      [--duration-ms 0] [--naming telemetry/Hub]
//
// One twoway publish() per tick carries every bed's samples. A twoway call
// gives natural back-pressure: if the hub slows down, so does the producer,
// instead of piling requests up in the ORB.
#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>
#include <vector>

#include "core/vitals_sim.hpp"
#include "orb/args.hpp"
#include "orb/convert.hpp"
#include "orb/orb_util.hpp"

using namespace telemetry;

namespace {

std::string bed_name(int i) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "ICU-%02d", i + 1);
    return buf;
}

void register_all(Telemetry::TelemetrySource_ptr src, const std::vector<core::BedSimulator>& beds) {
    for (const auto& b : beds) {
        Telemetry::BedInfo info;
        info.bed = b.bed().c_str();
        info.ward = "ICU";
        info.label = ("Synthetic bed " + b.bed()).c_str();
        src->registerBed(info);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const sigset_t signals = orb::block_termination_signals();
    CORBA::ORB_var orb = CORBA::ORB_init(argc, argv);
    int rc = 0;
    try {
        orb::Args args(argc, argv);
        const int n_beds = static_cast<int>(args.num("beds", 6));
        const double rate_hz = args.real("rate-hz", 4.0);
        const auto duration = std::chrono::milliseconds(args.num("duration-ms", 0));
        if (n_beds < 1 || n_beds > 500 || rate_hz <= 0 || rate_hz > 1000)
            throw std::invalid_argument("--beds must be 1..500 and --rate-hz 0..1000");

        CORBA::Object_var obj =
            args.has("naming")
                ? orb::resolve_name(orb, args.str("naming"))
                : orb::read_ior_file(orb, args.str("ior-file", "hub.ior"), std::chrono::seconds(10));
        Telemetry::Hub_var hub = Telemetry::Hub::_narrow(obj);
        if (CORBA::is_nil(hub)) throw std::runtime_error("reference is not a Telemetry::Hub");
        Telemetry::TelemetrySource_var src = hub->source();

        std::vector<core::BedSimulator> beds;
        const auto seed = static_cast<std::uint64_t>(args.num("seed", 1));
        for (int i = 0; i < n_beds; ++i) beds.emplace_back(bed_name(i), seed * 1000 + i);
        for (auto& b : beds) b.nibp_every = static_cast<int>(rate_hz * 15);  // cuff every ~15 s
        register_all(src, beds);
        std::printf("[producer] %d beds registered, %.1f Hz\n", n_beds, rate_hz);
        std::fflush(stdout);

        const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / rate_hz));
        const auto t0 = std::chrono::steady_clock::now();
        auto next = t0;
        std::uint64_t ticks = 0, samples_sent = 0, failures = 0;
        std::vector<core::Sample> samples;
        for (;;) {
            const int sig = orb::wait_for_termination(
                signals, std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::max(next - std::chrono::steady_clock::now(),
                                      std::chrono::steady_clock::duration(std::chrono::milliseconds(1)))));
            if (sig != 0) break;
            if (duration.count() > 0 && std::chrono::steady_clock::now() - t0 >= duration) break;
            next += period;

            samples.clear();
            const auto now_ns = core::wall_clock_ns();
            for (auto& b : beds) b.step(1.0 / rate_hz, now_ns, samples);
            Telemetry::SampleBatch_var batch = orb::to_idl(samples);
            try {
                src->publish(batch.in());
                samples_sent += samples.size();
            } catch (const Telemetry::UnknownBed&) {
                // Hub restarted and lost its registry: register again.
                register_all(src, beds);
                ++failures;
            } catch (const CORBA::TRANSIENT& ex) {
                ++failures;
                if (failures % 20 == 1)
                    std::printf("[producer] hub unreachable (%s), retrying\n",
                                orb::describe(ex).c_str());
            }
            ++ticks;
        }
        std::printf("[producer] stopped after %llu ticks, %llu samples, %llu failed publishes\n",
                    static_cast<unsigned long long>(ticks),
                    static_cast<unsigned long long>(samples_sent),
                    static_cast<unsigned long long>(failures));
        try {
            for (const auto& b : beds) src->unregisterBed(b.bed().c_str());
        } catch (const CORBA::Exception&) {
            // Hub already gone during shutdown.
        }
    } catch (const CORBA::Exception& ex) {
        std::cerr << "producer: CORBA exception: " << orb::describe(ex) << '\n';
        rc = 1;
    } catch (const std::exception& ex) {
        std::cerr << "producer: " << ex.what() << '\n';
        rc = 1;
    }
    orb->destroy();
    return rc;
}
