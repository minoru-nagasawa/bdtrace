#include "string_util.h"
#include <cstdio>
#include <stdint.h>

namespace bdtrace {

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> result;
    std::string current;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == delim) {
            result.push_back(current);
            current.clear();
        } else {
            current += s[i];
        }
    }
    result.push_back(current);
    return result;
}

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

std::string format_duration_us(int64_t us) {
    char buf[64];
    if (us < 1000) {
        std::snprintf(buf, sizeof(buf), "%lldus", (long long)us);
    } else if (us < 1000000) {
        std::snprintf(buf, sizeof(buf), "%.1fms", us / 1000.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%.2fs", us / 1000000.0);
    }
    return buf;
}

} // namespace bdtrace
