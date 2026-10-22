// Lease bookkeeping for subscriptions. Pure logic with explicit time points,
// so expiry is tested without sleeping. Not thread-safe; the Dispatcher
// guards it with its registry mutex.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "core/sample.hpp"

namespace telemetry::core {

struct LeasePolicy {
    Millis min{500};
    Millis max{60000};
};

class LeaseTable {
public:
    explicit LeaseTable(LeasePolicy policy = {}) : policy_(policy) {}

    // Clamps the requested duration to the policy and returns what was granted.
    Millis grant(std::uint64_t id, TimePoint now, Millis requested);

    // Extends the lease by its granted duration from now. Returns the
    // duration, or nullopt if the id is unknown or already expired (an
    // expired lease cannot be revived; the client must subscribe again).
    std::optional<Millis> renew(std::uint64_t id, TimePoint now);

    bool remove(std::uint64_t id);
    bool contains(std::uint64_t id) const { return leases_.count(id) != 0; }

    // Removes and returns every lease whose deadline is at or before now.
    std::vector<std::uint64_t> collect_expired(TimePoint now);

    std::optional<TimePoint> deadline(std::uint64_t id) const;
    std::size_t size() const { return leases_.size(); }
    const LeasePolicy& policy() const { return policy_; }

private:
    struct Lease {
        Millis duration;
        TimePoint deadline;
    };
    LeasePolicy policy_;
    std::map<std::uint64_t, Lease> leases_;
};

}  // namespace telemetry::core
