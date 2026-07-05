#include "log.h"
#include <time.h>
#include <string>

namespace bdtrace {

LogLevel g_log_level = LOG_INFO;

static std::string g_log_file_path;
static FILE* g_log_file = 0;

void log_init(LogLevel level) {
    g_log_level = level;
}

void log_set_file(const char* path) {
    if (g_log_file) {
        std::fclose(g_log_file);
        g_log_file = 0;
    }
    g_log_file_path = (path && *path) ? path : "";
}

const char* log_file_path() {
    // Only meaningful once the lazy open actually happened (= something
    // was written); otherwise no file exists.
    return g_log_file ? g_log_file_path.c_str() : "";
}

// Lazy open shared by log_msg and log_file_raw
static bool ensure_log_file_open() {
    if (g_log_file_path.empty()) return false;
    if (!g_log_file) {
        g_log_file = std::fopen(g_log_file_path.c_str(), "a");
        if (!g_log_file) {
            std::fprintf(stderr, "[bdtrace] could not open log file %s\n",
                         g_log_file_path.c_str());
            g_log_file_path.clear();  // don't retry forever
            return false;
        }
    }
    return true;
}

void log_file_raw(const char* fmt, ...) {
    if (!ensure_log_file_open()) return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_log_file, fmt, args);
    va_end(args);
    std::fputc('\n', g_log_file);
    std::fflush(g_log_file);
}

void log_msg(LogLevel level, const char* fmt, ...) {
    bool to_console = (level <= g_log_level);
    bool to_file = (level <= LOG_WARN && !g_log_file_path.empty());
    if (!to_console && !to_file) return;

    const char* prefix = "";
    switch (level) {
        case LOG_ERROR: prefix = "[ERROR] "; break;
        case LOG_WARN:  prefix = "[WARN]  "; break;
        case LOG_INFO:  prefix = "[INFO]  "; break;
        case LOG_DEBUG: prefix = "[DEBUG] "; break;
    }

    // P2.5: Add timestamp
    char ts[64];
    ts[0] = '\0';
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm* tm_ptr = localtime_r(&now, &tm_buf);
    if (tm_ptr) {
        std::snprintf(ts, sizeof(ts), "[%04d-%02d-%02d %02d:%02d:%02d] ",
                      tm_ptr->tm_year + 1900, tm_ptr->tm_mon + 1, tm_ptr->tm_mday,
                      tm_ptr->tm_hour, tm_ptr->tm_min, tm_ptr->tm_sec);
    }

    char msg[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    if (to_console) {
        std::fprintf(stderr, "%s%s%s\n", ts, prefix, msg);
    }
    if (to_file && ensure_log_file_open()) {
        std::fprintf(g_log_file, "%s%s%s\n", ts, prefix, msg);
        std::fflush(g_log_file);
    }
}

} // namespace bdtrace
