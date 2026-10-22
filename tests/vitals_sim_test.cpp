#include "core/vitals_sim.hpp"

#include <gtest/gtest.h>

#include <map>

using namespace telemetry::core;

TEST(BedSimulator, DeterministicForSeed) {
    BedSimulator a("ICU-01", 42), b("ICU-01", 42), c("ICU-01", 43);
    std::vector<Sample> sa, sb, sc;
    for (int i = 0; i < 100; ++i) {
        a.step(0.25, i, sa);
        b.step(0.25, i, sb);
        c.step(0.25, i, sc);
    }
    ASSERT_EQ(sa.size(), sb.size());
    bool differs = false;
    for (std::size_t i = 0; i < sa.size(); ++i) {
        EXPECT_EQ(sa[i].value, sb[i].value);
        differs |= i < sc.size() && sa[i].value != sc[i].value;
    }
    EXPECT_TRUE(differs);
}

TEST(BedSimulator, StaysWithinPhysiologicalBounds) {
    std::map<std::string, ParamModel> models;
    for (const auto& m : default_param_models()) models.emplace(m.code, m);
    BedSimulator sim("ICU-02", 7);
    std::vector<Sample> out;
    for (int i = 0; i < 20000; ++i) sim.step(0.5, i, out);
    for (const auto& s : out) {
        const auto& m = models.at(s.parameter);
        ASSERT_GE(s.value, m.lo) << s.parameter;
        ASSERT_LE(s.value, m.hi) << s.parameter;
    }
}

TEST(BedSimulator, SequenceIncrementsPerSampleAndTimestampsPropagate) {
    BedSimulator sim("ICU-03", 1);
    sim.nibp_every = 4;
    std::vector<Sample> out;
    for (int i = 0; i < 8; ++i) sim.step(1.0, 1000 + i, out);
    for (std::size_t i = 0; i < out.size(); ++i) EXPECT_EQ(out[i].seq, i);
    EXPECT_EQ(out.back().source_time_ns, 1007u);
    // 4 continuous parameters every step, 2 NIBP every 4th step.
    EXPECT_EQ(out.size(), 8u * 4 + 2u * 2);
}

TEST(BedSimulator, MeanRevertsTowardBaseline) {
    BedSimulator sim("ICU-04", 3);
    std::vector<Sample> out;
    double sum = 0;
    int n = 0;
    for (int i = 0; i < 40000; ++i) {
        out.clear();
        sim.step(0.25, i, out);
        sum += sim.value("RR");
        ++n;
    }
    EXPECT_NEAR(sum / n, 16.0, 2.0);
}

TEST(BedSimulator, DesaturationRaisesHeartRate) {
    // Over a long run, samples taken during desaturation episodes should show
    // a higher mean HR than the rest (compensatory coupling).
    BedSimulator sim("ICU-05", 11);
    std::vector<Sample> out;
    double hr_desat = 0, hr_normal = 0;
    int n_desat = 0, n_normal = 0;
    for (int i = 0; i < 200000; ++i) {
        out.clear();
        sim.step(0.25, i, out);
        if (sim.in_desaturation()) {
            hr_desat += sim.value("HR");
            ++n_desat;
        } else {
            hr_normal += sim.value("HR");
            ++n_normal;
        }
    }
    ASSERT_GT(n_desat, 100);
    EXPECT_GT(hr_desat / n_desat, hr_normal / n_normal + 3.0);
}

TEST(BedSimulator, RejectsNonPositiveStep) {
    BedSimulator sim("X", 1);
    std::vector<Sample> out;
    EXPECT_THROW(sim.step(0.0, 0, out), std::invalid_argument);
}
