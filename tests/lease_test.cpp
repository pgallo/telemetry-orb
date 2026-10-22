#include "core/lease.hpp"

#include <gtest/gtest.h>

using namespace telemetry::core;
using std::chrono::milliseconds;

namespace {
const TimePoint T0 = TimePoint{} + std::chrono::hours(1);
}

TEST(LeaseTable, GrantClampsToPolicy) {
    LeaseTable t(LeasePolicy{milliseconds(500), milliseconds(5000)});
    EXPECT_EQ(t.grant(1, T0, milliseconds(100)), milliseconds(500));
    EXPECT_EQ(t.grant(2, T0, milliseconds(2000)), milliseconds(2000));
    EXPECT_EQ(t.grant(3, T0, milliseconds(99999)), milliseconds(5000));
    EXPECT_EQ(*t.deadline(2), T0 + milliseconds(2000));
}

TEST(LeaseTable, RenewExtendsFromNow) {
    LeaseTable t;
    t.grant(7, T0, milliseconds(1000));
    auto r = t.renew(7, T0 + milliseconds(900));
    ASSERT_TRUE(r);
    EXPECT_EQ(*r, milliseconds(1000));
    EXPECT_EQ(*t.deadline(7), T0 + milliseconds(1900));
}

TEST(LeaseTable, ExpiredLeaseCannotBeRenewed) {
    LeaseTable t;
    t.grant(7, T0, milliseconds(1000));
    // Exactly at the deadline counts as expired.
    EXPECT_FALSE(t.renew(7, T0 + milliseconds(1000)));
    EXPECT_FALSE(t.contains(7));
    EXPECT_FALSE(t.renew(7, T0));
}

TEST(LeaseTable, CollectExpiredRemovesOnlyDueLeases) {
    LeaseTable t;
    t.grant(1, T0, milliseconds(500));
    t.grant(2, T0, milliseconds(1500));
    t.grant(3, T0, milliseconds(1000));
    auto expired = t.collect_expired(T0 + milliseconds(1000));
    EXPECT_EQ(expired, (std::vector<std::uint64_t>{1, 3}));
    EXPECT_EQ(t.size(), 1u);
    EXPECT_TRUE(t.contains(2));
    EXPECT_TRUE(t.collect_expired(T0 + milliseconds(1001)).empty());
}

TEST(LeaseTable, RenewalKeepsLeaseAliveIndefinitely) {
    LeaseTable t;
    t.grant(1, T0, milliseconds(1000));
    TimePoint now = T0;
    for (int i = 0; i < 100; ++i) {
        now += milliseconds(700);
        ASSERT_TRUE(t.renew(1, now)) << "iteration " << i;
        ASSERT_TRUE(t.collect_expired(now).empty());
    }
}

TEST(LeaseTable, RemoveIsIdempotent) {
    LeaseTable t;
    t.grant(1, T0, milliseconds(1000));
    EXPECT_TRUE(t.remove(1));
    EXPECT_FALSE(t.remove(1));
    EXPECT_FALSE(t.deadline(1));
}
