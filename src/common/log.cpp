#include "log.h"
#include <time.h>

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

    // P2.5: Add timestamp
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm* tm_ptr = localtime_r(&now, &tm_buf);
    if (tm_ptr) {
        char ts[64];
        std::snprintf(ts, sizeof(ts), "[%04d-%02d-%02d %02d:%02d:%02d] ",
                      tm_ptr->tm_year + 1900, tm_ptr->tm_mon + 1, tm_ptr->tm_mday,
                      tm_ptr->tm_hour, tm_ptr->tm_min, tm_ptr->tm_sec);
        std::fprintf(stderr, "%s", ts);
    }

    std::fprintf(stderr, "%s", prefix);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fprintf(stderr, "\n");
}

} // namespace bdtrace
