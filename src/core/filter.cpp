#include "core/filter.hpp"

#include <algorithm>
#include <stdexcept>

namespace telemetry::core {

bool pattern_matches(const std::string& pattern, const std::string& value) {
    if (!pattern.empty() && pattern.back() == '*') {
        const auto prefix_len = pattern.size() - 1;
        return value.compare(0, prefix_len, pattern, 0, prefix_len) == 0;
    }
    return pattern == value;
}

FilterSpec::FilterSpec(std::vector<std::string> beds, std::vector<std::string> parameters)
    : beds_(std::move(beds)), parameters_(std::move(parameters)) {}

namespace {

void validate_list(const std::vector<std::string>& list, const char* what) {
    if (list.size() > FilterSpec::kMaxEntries)
        throw std::invalid_argument(std::string("too many ") + what + " entries");
    for (const auto& p : list) {
        if (p.empty()) throw std::invalid_argument(std::string("empty ") + what + " pattern");
        const auto star = p.find('*');
        if (star != std::string::npos && star != p.size() - 1)
            throw std::invalid_argument(std::string(what) + " pattern '" + p +
                                        "': '*' is only allowed at the end");
    }
}

bool any_match(const std::vector<std::string>& patterns, const std::string& value) {
    if (patterns.empty()) return true;
    return std::any_of(patterns.begin(), patterns.end(),
                       [&](const std::string& p) { return pattern_matches(p, value); });
}

std::string join(const std::vector<std::string>& v) {
    if (v.empty()) return "*";
    std::string out;
    for (const auto& s : v) {
        if (!out.empty()) out += ',';
        out += s;
    }
    return out;
}

}  // namespace

void FilterSpec::validate() const {
    validate_list(beds_, "bed");
    validate_list(parameters_, "parameter");
}

bool FilterSpec::matches(const std::string& bed, const std::string& parameter) const {
    return any_match(beds_, bed) && any_match(parameters_, parameter);
}

std::string FilterSpec::describe() const {
    return "beds=" + join(beds_) + " params=" + join(parameters_);
}

}  // namespace telemetry::core
