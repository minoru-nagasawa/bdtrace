#include "log.h"

namespace bdtrace {

static LogLevel g_log_level = LOG_INFO;

void log_init(LogLevel level) {
    g_log_level = level;
}

void log_msg(LogLevel level, const char* fmt, ...) {
    if (level > g_log_level) return;

    const char* prefix = "";
    switch (level) {
        case LOG_ERROR: prefix = "[ERROR] "; break;
        case LOG_WARN:  prefix = "[WARN]  "; break;
        case LOG_INFO:  prefix = "[INFO]  "; break;
        case LOG_DEBUG: prefix = "[DEBUG] "; break;
    }

    std::fprintf(stderr, "%s", prefix);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fprintf(stderr, "\n");
}

} // namespace bdtrace
