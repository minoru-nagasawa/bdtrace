#ifndef BDTRACE_VIEW_HELPERS_H
#define BDTRACE_VIEW_HELPERS_H

#include <string>
#include "types.h"

namespace bdtrace {

const char* mode_str(int mode);
bool is_input_mode(int mode);
bool is_output_mode(int mode);
std::string cmd_name(const std::string& cmdline);
std::string shorten_cmd(const std::string& cmdline, size_t max_len);
const char* errno_name(int e);

} // namespace bdtrace

#endif // BDTRACE_VIEW_HELPERS_H
