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

void log_init(LogLevel level);
void log_msg(LogLevel level, const char* fmt, ...);

} // namespace bdtrace

#define LOG_ERROR(...) ::bdtrace::log_msg(::bdtrace::LOG_ERROR, __VA_ARGS__)
#define LOG_WARN(...)  ::bdtrace::log_msg(::bdtrace::LOG_WARN, __VA_ARGS__)
#define LOG_INFO(...)  ::bdtrace::log_msg(::bdtrace::LOG_INFO, __VA_ARGS__)
#define LOG_DEBUG(...) ::bdtrace::log_msg(::bdtrace::LOG_DEBUG, __VA_ARGS__)

#endif // BDTRACE_LOG_H
