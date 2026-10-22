// ORB-independent data model. The hub's dispatch logic works on these types
// so it can be unit tested without an ORB; the CORBA layer converts at the
// edge (see src/orb/convert.hpp).
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace telemetry::core {

enum class Quality { Valid, Questionable, Invalid };

struct Sample {
    std::string bed;
    std::string parameter;
    double value = 0.0;
    std::string units;
    Quality quality = Quality::Valid;
    std::uint64_t source_time_ns = 0;  // producer wall clock, ns since epoch
    std::uint32_t seq = 0;
};

using SteadyClock = std::chrono::steady_clock;
using TimePoint = SteadyClock::time_point;
using Millis = std::chrono::milliseconds;

inline std::uint64_t wall_clock_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

inline double to_ms(SteadyClock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

}  // namespace telemetry::core
