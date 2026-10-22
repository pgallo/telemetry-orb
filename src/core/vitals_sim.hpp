// Synthetic vital signs. Each parameter is a mean-reverting random walk
// (discretised Ornstein-Uhlenbeck) clamped to physiological bounds, with
// two couplings that make the traces look plausible on a monitor:
//   - occasional desaturation episodes pull SpO2 down for a while;
//   - heart rate rises as SpO2 falls (compensatory tachycardia).
// Deterministic for a given seed, so tests and demos are reproducible.
#pragma once

#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "core/sample.hpp"

namespace telemetry::core {

struct ParamModel {
    const char* code;
    const char* units;
    double baseline;
    double lo;
    double hi;
    double reversion;  // per second
    double noise;      // per sqrt(second)
    int decimals;
};

const std::vector<ParamModel>& default_param_models();

class BedSimulator {
public:
    BedSimulator(std::string bed, std::uint64_t seed);

    // Advances the model by dt_s seconds and appends one sample per parameter.
    // Blood pressure is sampled every nibp_every-th step, as a cuff would be.
    void step(double dt_s, std::uint64_t source_time_ns, std::vector<Sample>& out);

    const std::string& bed() const { return bed_; }
    double value(const std::string& code) const;
    bool in_desaturation() const { return desat_remaining_s_ > 0; }

    int nibp_every = 30;

private:
    std::string bed_;
    std::mt19937_64 rng_;
    std::normal_distribution<double> gauss_{0.0, 1.0};
    std::uniform_real_distribution<double> uni_{0.0, 1.0};
    std::vector<double> values_;
    std::vector<double> baselines_;
    double desat_remaining_s_ = 0;
    double desat_depth_ = 0;
    std::uint32_t seq_ = 0;
    std::uint64_t steps_ = 0;
};

double round_to(double v, int decimals);

}  // namespace telemetry::core
