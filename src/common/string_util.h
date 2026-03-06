#ifndef BDTRACE_STRING_UTIL_H
#define BDTRACE_STRING_UTIL_H

#include <string>
#include <vector>
#include <stdint.h>

namespace bdtrace {

std::vector<std::string> split(const std::string& s, char delim);
std::string trim(const std::string& s);
std::string format_duration_us(int64_t us);

} // namespace bdtrace

#endif // BDTRACE_STRING_UTIL_H
