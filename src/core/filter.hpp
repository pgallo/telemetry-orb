// Subscription filter: which beds and parameters a station wants.
#pragma once

#include <string>
#include <vector>

namespace telemetry::core {

class FilterSpec {
public:
    FilterSpec() = default;
    FilterSpec(std::vector<std::string> beds, std::vector<std::string> parameters);

    // Throws std::invalid_argument describing the first problem found.
    void validate() const;

    bool matches(const std::string& bed, const std::string& parameter) const;

    const std::vector<std::string>& beds() const { return beds_; }
    const std::vector<std::string>& parameters() const { return parameters_; }
    std::string describe() const;

    static constexpr std::size_t kMaxEntries = 256;

private:
    std::vector<std::string> beds_;
    std::vector<std::string> parameters_;
};

// Exact match, or prefix match when the pattern ends in '*'. "*" matches all.
bool pattern_matches(const std::string& pattern, const std::string& value);

}  // namespace telemetry::core
