#ifndef BDTRACE_LOG_H
#define BDTRACE_LOG_H

#include <cstdio>
#include <cstdarg>

namespace bdtrace {

enum LogLevel {
    LOG_ERROR = 0,
    LOG_WARN  = 1,
    LOG_INFO  = 2,
    LOG_DEBUG = 3
};

extern LogLevel g_log_level;

void log_init(LogLevel level);
// Append WARN/ERROR messages to this file as well as stderr (pass 0/empty to
// disable). The file is created lazily on the first warning or error.
void log_set_file(const char* path);
// Non-empty once something was actually written to the log file.
const char* log_file_path();
// Append a raw line to the log file only (no stderr, no prefix). Used for
// multi-line diagnostic dumps whose headline went through LOG_WARN.
void log_file_raw(const char* fmt, ...);
void log_msg(LogLevel level, const char* fmt, ...);

} // namespace bdtrace

// Check the level in the macro so disabled messages (notably LOG_DEBUG in the
// per-syscall hot path) cost one integer compare, not a varargs call.
#define BDTRACE_LOG(lvl, ...) \
    do { if ((lvl) <= ::bdtrace::g_log_level) ::bdtrace::log_msg((lvl), __VA_ARGS__); } while (0)

#define LOG_ERROR(...) BDTRACE_LOG(::bdtrace::LOG_ERROR, __VA_ARGS__)
#define LOG_WARN(...)  BDTRACE_LOG(::bdtrace::LOG_WARN, __VA_ARGS__)
#define LOG_INFO(...)  BDTRACE_LOG(::bdtrace::LOG_INFO, __VA_ARGS__)
#define LOG_DEBUG(...) BDTRACE_LOG(::bdtrace::LOG_DEBUG, __VA_ARGS__)

#endif // BDTRACE_LOG_H
