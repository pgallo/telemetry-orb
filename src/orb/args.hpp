// Minimal "--key value" / "--flag" parser for the demo executables. ORB
// options (-ORBxxx) are consumed by ORB_init before this runs.
#pragma once

#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace telemetry::orb {

class Args {
public:
    Args(int argc, char** argv, const std::vector<std::string>& flags = {}) {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a.rfind("--", 0) != 0) throw std::invalid_argument("unexpected argument: " + a);
            a = a.substr(2);
            bool is_flag = false;
            for (const auto& f : flags) is_flag |= f == a;
            if (is_flag) {
                values_[a] = "1";
            } else {
                if (i + 1 >= argc) throw std::invalid_argument("--" + a + " needs a value");
                values_[a] = argv[++i];
            }
        }
    }

    std::string str(const std::string& key, const std::string& def = "") const {
        auto it = values_.find(key);
        return it == values_.end() ? def : it->second;
    }
    long num(const std::string& key, long def) const {
        auto it = values_.find(key);
        return it == values_.end() ? def : std::stol(it->second);
    }
    double real(const std::string& key, double def) const {
        auto it = values_.find(key);
        return it == values_.end() ? def : std::stod(it->second);
    }
    bool has(const std::string& key) const { return values_.count(key) != 0; }

private:
    std::map<std::string, std::string> values_;
};

inline std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

}  // namespace telemetry::orb
