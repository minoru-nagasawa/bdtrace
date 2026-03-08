#ifndef BDTRACE_TYPES_H
#define BDTRACE_TYPES_H

#include <string>
#include <vector>
#include <stdint.h>
#include <sys/time.h>

namespace bdtrace {

enum FileAccessMode {
    FA_READ        = 0,
    FA_WRITE       = 1,
    FA_RDWR        = 2,
    FA_STAT        = 3,
    FA_ACCESS      = 4,
    FA_EXEC        = 5,
    FA_UNLINK      = 6,
    FA_RENAME_SRC  = 7,
    FA_RENAME_DST  = 8,
    FA_READLINK    = 9,
    FA_MKDIR       = 10,
    FA_LINK_SRC    = 11,
    FA_LINK_DST    = 12,
    FA_SYMLINK_TARGET = 13,
    FA_SYMLINK_LINK   = 14,
    FA_CHMOD       = 15,
    FA_CHOWN       = 16,
    FA_TRUNCATE    = 17,
    FA_MKNOD       = 18,
    FA_UTIMENS     = 19,
    FA_CHDIR       = 20
};

struct ProcessRecord {
    int pid;
    int ppid;
    std::string cmdline;
    std::string cwd;
    int64_t start_time_us;
    int64_t end_time_us;
    int exit_code;
    int64_t user_time_us;
    int64_t sys_time_us;
    int64_t peak_rss_kb;
    int64_t io_read_bytes;
    int64_t io_write_bytes;

    ProcessRecord()
        : pid(0), ppid(0), start_time_us(0), end_time_us(0), exit_code(-1)
        , user_time_us(0), sys_time_us(0), peak_rss_kb(0)
        , io_read_bytes(0), io_write_bytes(0) {}
};

struct FileAccessRecord {
    int pid;
    std::string filename;
    FileAccessMode mode;
    int fd;
    int64_t timestamp_us;

    FileAccessRecord() : pid(0), mode(FA_READ), fd(-1), timestamp_us(0) {}
};

struct FailedAccessRecord {
    int pid;
    std::string filename;
    FileAccessMode mode;
    int errno_val;
    int64_t timestamp_us;

    FailedAccessRecord() : pid(0), mode(FA_READ), errno_val(0), timestamp_us(0) {}
};

inline int64_t now_us() {
    struct timeval tv;
    gettimeofday(&tv, 0);
    return static_cast<int64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
}

} // namespace bdtrace

#endif // BDTRACE_TYPES_H
