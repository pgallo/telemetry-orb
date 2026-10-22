#include "core/lease.hpp"

#include <algorithm>

namespace telemetry::core {

Millis LeaseTable::grant(std::uint64_t id, TimePoint now, Millis requested) {
    const Millis granted = std::clamp(requested, policy_.min, policy_.max);
    leases_[id] = Lease{granted, now + granted};
    return granted;
}

std::optional<Millis> LeaseTable::renew(std::uint64_t id, TimePoint now) {
    auto it = leases_.find(id);
    if (it == leases_.end()) return std::nullopt;
    if (it->second.deadline <= now) {
        // Expired but not yet collected: treat as gone, so the outcome does
        // not depend on when the publisher thread last swept.
        leases_.erase(it);
        return std::nullopt;
    }
    it->second.deadline = now + it->second.duration;
    return it->second.duration;
}

bool LeaseTable::remove(std::uint64_t id) { return leases_.erase(id) != 0; }

std::vector<std::uint64_t> LeaseTable::collect_expired(TimePoint now) {
    std::vector<std::uint64_t> expired;
    for (auto it = leases_.begin(); it != leases_.end();) {
        if (it->second.deadline <= now) {
            expired.push_back(it->first);
            it = leases_.erase(it);
        } else {
            ++it;
        }
    }
    return expired;
}

std::optional<TimePoint> LeaseTable::deadline(std::uint64_t id) const {
    auto it = leases_.find(id);
    if (it == leases_.end()) return std::nullopt;
    return it->second.deadline;
}

}  // namespace telemetry::core
