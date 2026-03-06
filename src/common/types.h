#ifndef BDTRACE_TYPES_H
#define BDTRACE_TYPES_H

#include <string>
#include <vector>
#include <stdint.h>
#include <sys/time.h>

namespace bdtrace {

enum FileAccessMode {
    FA_READ  = 0,
    FA_WRITE = 1,
    FA_RDWR  = 2
};

struct ProcessRecord {
    int pid;
    int ppid;
    std::string cmdline;
    int64_t start_time_us;
    int64_t end_time_us;
    int exit_code;

    ProcessRecord()
        : pid(0), ppid(0), start_time_us(0), end_time_us(0), exit_code(-1) {}
};

struct FileAccessRecord {
    int pid;
    std::string filename;
    FileAccessMode mode;
    int fd;

    FileAccessRecord() : pid(0), mode(FA_READ), fd(-1) {}
};

inline int64_t now_us() {
    struct timeval tv;
    gettimeofday(&tv, 0);
    return static_cast<int64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
}

} // namespace bdtrace

#endif // BDTRACE_TYPES_H
