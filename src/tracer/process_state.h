#ifndef BDTRACE_PROCESS_STATE_H
#define BDTRACE_PROCESS_STATE_H

#include <string>
#include <stdint.h>

namespace bdtrace {

struct ProcessState {
    int pid;
    int ppid;
    bool in_syscall;       // true if we're at syscall-enter, waiting for exit
    long pending_syscall;  // syscall number at entry
    unsigned long pending_path_addr;  // address of 1st filename arg
    unsigned long pending_path_addr2; // address of 2nd filename arg (rename/link/symlink)
    int pending_flags;     // flags arg for open/openat
    bool traced;           // has been set up with PTRACE_SETOPTIONS
    std::string cached_cwd; // cached cwd, updated on chdir/fchdir
    int64_t user_time_us;
    int64_t sys_time_us;
    int64_t peak_rss_kb;
    int64_t io_read_bytes;
    int64_t io_write_bytes;

    ProcessState()
        : pid(0), ppid(0), in_syscall(false), pending_syscall(-1)
        , pending_path_addr(0), pending_path_addr2(0), pending_flags(0), traced(false)
        , user_time_us(0), sys_time_us(0), peak_rss_kb(0)
        , io_read_bytes(0), io_write_bytes(0) {}

    explicit ProcessState(int p, int pp = 0)
        : pid(p), ppid(pp), in_syscall(false), pending_syscall(-1)
        , pending_path_addr(0), pending_path_addr2(0), pending_flags(0), traced(false)
        , user_time_us(0), sys_time_us(0), peak_rss_kb(0)
        , io_read_bytes(0), io_write_bytes(0) {}
};

} // namespace bdtrace

#endif // BDTRACE_PROCESS_STATE_H
