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
    unsigned long pending_path_addr; // address of filename arg for open/openat
    int pending_flags;     // flags arg for open/openat
    bool traced;           // has been set up with PTRACE_SETOPTIONS

    ProcessState()
        : pid(0), ppid(0), in_syscall(false), pending_syscall(-1)
        , pending_path_addr(0), pending_flags(0), traced(false) {}

    explicit ProcessState(int p, int pp = 0)
        : pid(p), ppid(pp), in_syscall(false), pending_syscall(-1)
        , pending_path_addr(0), pending_flags(0), traced(false) {}
};

} // namespace bdtrace

#endif // BDTRACE_PROCESS_STATE_H
