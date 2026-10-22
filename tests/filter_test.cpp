#include "core/filter.hpp"

#include <gtest/gtest.h>

using telemetry::core::FilterSpec;
using telemetry::core::pattern_matches;

TEST(PatternMatches, ExactAndPrefix) {
    EXPECT_TRUE(pattern_matches("ICU-01", "ICU-01"));
    EXPECT_FALSE(pattern_matches("ICU-01", "ICU-010"));
    EXPECT_TRUE(pattern_matches("ICU-0*", "ICU-07"));
    EXPECT_FALSE(pattern_matches("ICU-0*", "ICU-17"));
    EXPECT_TRUE(pattern_matches("*", "anything"));
    EXPECT_TRUE(pattern_matches("ICU*", "ICU"));  // empty remainder still matches
    EXPECT_FALSE(pattern_matches("NIBP*", "HR"));
}

TEST(FilterSpec, EmptyMatchesEverything) {
    FilterSpec f;
    EXPECT_NO_THROW(f.validate());
    EXPECT_TRUE(f.matches("ICU-01", "HR"));
    EXPECT_EQ(f.describe(), "beds=* params=*");
}

TEST(FilterSpec, BedsAndParametersAreAnded) {
    FilterSpec f({"ICU-01", "ICU-02"}, {"HR", "NIBP*"});
    EXPECT_TRUE(f.matches("ICU-01", "HR"));
    EXPECT_TRUE(f.matches("ICU-02", "NIBPS"));
    EXPECT_FALSE(f.matches("ICU-03", "HR"));
    EXPECT_FALSE(f.matches("ICU-01", "SPO2"));
}

TEST(FilterSpec, OnlyOneDimensionRestricted) {
    FilterSpec by_param({}, {"SPO2"});
    EXPECT_TRUE(by_param.matches("ANY", "SPO2"));
    EXPECT_FALSE(by_param.matches("ANY", "HR"));
    FilterSpec by_bed({"ICU-1*"}, {});
    EXPECT_TRUE(by_bed.matches("ICU-12", "TEMP"));
    EXPECT_FALSE(by_bed.matches("ICU-02", "TEMP"));
}

TEST(FilterSpec, ValidationRejectsBadPatterns) {
    EXPECT_THROW(FilterSpec({""}, {}).validate(), std::invalid_argument);
    EXPECT_THROW(FilterSpec({"IC*U"}, {}).validate(), std::invalid_argument);
    EXPECT_THROW(FilterSpec({}, {"*HR"}).validate(), std::invalid_argument);
    std::vector<std::string> many(FilterSpec::kMaxEntries + 1, "X");
    EXPECT_THROW(FilterSpec(many, {}).validate(), std::invalid_argument);
}
