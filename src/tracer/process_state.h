#ifndef BDTRACE_PROCESS_STATE_H
#define BDTRACE_PROCESS_STATE_H

#include <string>
#include <stdint.h>

namespace bdtrace {

// Which kind of syscall stop we expect next for a process. UNKNOWN means the
// first stop hasn't been classified yet (a brand-new child may first stop at
// the exit of the clone that created it, depending on kernel version), so the
// rax == -ENOSYS heuristic must be used until the phase is established.
enum SyscallPhase {
    SC_PHASE_UNKNOWN = 0,
    SC_PHASE_ENTRY_NEXT,
    SC_PHASE_EXIT_NEXT
};

struct ProcessState {
    int pid;
    int ppid;
    int sc_phase;          // SyscallPhase: which syscall stop we expect next
    bool exit_needed;      // pending syscall's exit must be inspected (GETREGS)
    long pending_syscall;  // syscall number at entry
    unsigned long pending_path_addr;  // address of 1st filename arg
    unsigned long pending_path_addr2; // address of 2nd filename arg (rename/link/symlink)
    int pending_flags;     // flags arg for open/openat
    bool traced;           // has been set up with PTRACE_SETOPTIONS
    int mem_fd;            // cached /proc/<pid>/mem fd (-1: not opened, -2: open failed)
    std::string cached_cwd; // cached cwd, updated on chdir/fchdir
    int64_t user_time_us;
    int64_t sys_time_us;
    int64_t peak_rss_kb;
    int64_t io_read_bytes;
    int64_t io_write_bytes;

    ProcessState()
        : pid(0), ppid(0), sc_phase(SC_PHASE_UNKNOWN), exit_needed(false)
        , pending_syscall(-1)
        , pending_path_addr(0), pending_path_addr2(0), pending_flags(0), traced(false)
        , mem_fd(-1)
        , user_time_us(0), sys_time_us(0), peak_rss_kb(0)
        , io_read_bytes(0), io_write_bytes(0) {}

    explicit ProcessState(int p, int pp = 0)
        : pid(p), ppid(pp), sc_phase(SC_PHASE_UNKNOWN), exit_needed(false)
        , pending_syscall(-1)
        , pending_path_addr(0), pending_path_addr2(0), pending_flags(0), traced(false)
        , mem_fd(-1)
        , user_time_us(0), sys_time_us(0), peak_rss_kb(0)
        , io_read_bytes(0), io_write_bytes(0) {}
};

} // namespace bdtrace

#endif // BDTRACE_PROCESS_STATE_H
