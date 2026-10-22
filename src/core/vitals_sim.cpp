#include "core/vitals_sim.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace telemetry::core {

const std::vector<ParamModel>& default_param_models() {
    static const std::vector<ParamModel> models = {
        // code    units     base   lo     hi    rev   noise  dec
        {"HR",    "/min",   78.0,  35.0, 190.0, 0.08, 1.6,  0},
        {"SPO2",  "%",      97.0,  70.0, 100.0, 0.15, 0.5,  0},
        {"RR",    "/min",   16.0,   6.0,  40.0, 0.10, 0.5,  0},
        {"TEMP",  "Cel",    37.0,  34.0,  41.0, 0.01, 0.02, 1},
        {"NIBPS", "mm[Hg]", 122.0, 70.0, 200.0, 0.05, 2.0,  0},
        {"NIBPD", "mm[Hg]", 76.0,  40.0, 120.0, 0.05, 1.5,  0},
    };
    return models;
}

namespace {
constexpr std::size_t kHr = 0, kSpo2 = 1, kNibpS = 4, kNibpD = 5;
}

double round_to(double v, int decimals) {
    const double f = std::pow(10.0, decimals);
    return std::round(v * f) / f;
}

BedSimulator::BedSimulator(std::string bed, std::uint64_t seed) : bed_(std::move(bed)), rng_(seed) {
    for (const auto& m : default_param_models()) {
        // Individual baselines differ a little per patient.
        const double spread = (m.hi - m.lo) * 0.03;
        const double b = std::clamp(m.baseline + spread * gauss_(rng_), m.lo, m.hi);
        baselines_.push_back(b);
        values_.push_back(b);
    }
}

double BedSimulator::value(const std::string& code) const {
    const auto& models = default_param_models();
    for (std::size_t i = 0; i < models.size(); ++i)
        if (code == models[i].code) return values_[i];
    throw std::out_of_range("unknown parameter " + code);
}

void BedSimulator::step(double dt_s, std::uint64_t source_time_ns, std::vector<Sample>& out) {
    if (dt_s <= 0) throw std::invalid_argument("dt must be positive");
    const auto& models = default_param_models();

    // Desaturation episodes: rare onset, lasting 10-40 s.
    if (desat_remaining_s_ <= 0 && uni_(rng_) < 0.004 * dt_s) {
        desat_remaining_s_ = 10.0 + 30.0 * uni_(rng_);
        desat_depth_ = 6.0 + 10.0 * uni_(rng_);
    }
    double spo2_target = baselines_[kSpo2];
    if (desat_remaining_s_ > 0) {
        spo2_target -= desat_depth_;
        desat_remaining_s_ -= dt_s;
    }
    const double hypoxia = std::max(0.0, baselines_[kSpo2] - values_[kSpo2]);

    const bool nibp_now = (steps_ % static_cast<std::uint64_t>(std::max(1, nibp_every))) == 0;
    ++steps_;

    for (std::size_t i = 0; i < models.size(); ++i) {
        const auto& m = models[i];
        double target = baselines_[i];
        if (i == kSpo2) target = spo2_target;
        if (i == kHr) target += 2.5 * hypoxia;
        double& x = values_[i];
        x += m.reversion * (target - x) * dt_s + m.noise * std::sqrt(dt_s) * gauss_(rng_);
        x = std::clamp(x, m.lo, m.hi);

        if ((i == kNibpS || i == kNibpD) && !nibp_now) continue;

        Sample s;
        s.bed = bed_;
        s.parameter = m.code;
        s.value = round_to(x, m.decimals);
        s.units = m.units;
        // A probe briefly reading at its bound is more likely artefact than truth.
        s.quality = (x <= m.lo || x >= m.hi) ? Quality::Questionable : Quality::Valid;
        s.source_time_ns = source_time_ns;
        s.seq = seq_++;
        out.push_back(std::move(s));
    }
}

}  // namespace telemetry::core
