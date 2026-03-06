#include "ptrace_backend.h"
#include "ptrace_defs.h"
#include "../common/types.h"
#include "../common/log.h"

#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>

#ifndef __WALL
#define __WALL 0x40000000
#endif

#ifndef PTRACE_O_TRACESYSGOOD
#define PTRACE_O_TRACESYSGOOD 0x00000001
#endif
#ifndef PTRACE_O_TRACEFORK
#define PTRACE_O_TRACEFORK 0x00000002
#endif
#ifndef PTRACE_O_TRACEVFORK
#define PTRACE_O_TRACEVFORK 0x00000004
#endif
#ifndef PTRACE_O_TRACECLONE
#define PTRACE_O_TRACECLONE 0x00000008
#endif
#ifndef PTRACE_O_TRACEEXEC
#define PTRACE_O_TRACEEXEC 0x00000010
#endif
#ifndef PTRACE_O_TRACEEXIT
#define PTRACE_O_TRACEEXIT 0x00000040
#endif

#ifndef PTRACE_EVENT_FORK
#define PTRACE_EVENT_FORK 1
#endif
#ifndef PTRACE_EVENT_VFORK
#define PTRACE_EVENT_VFORK 2
#endif
#ifndef PTRACE_EVENT_CLONE
#define PTRACE_EVENT_CLONE 3
#endif
#ifndef PTRACE_EVENT_EXEC
#define PTRACE_EVENT_EXEC 4
#endif
#ifndef PTRACE_EVENT_EXIT
#define PTRACE_EVENT_EXIT 6
#endif

#ifndef PTRACE_SETOPTIONS
#define PTRACE_SETOPTIONS 0x4200
#endif
#ifndef PTRACE_GETEVENTMSG
#define PTRACE_GETEVENTMSG 0x4201
#endif

namespace bdtrace {

static volatile sig_atomic_t g_child_pid = 0;

static void signal_handler(int sig) {
    if (g_child_pid > 0) {
        kill(g_child_pid, sig);
    }
}

PtraceBackend::PtraceBackend(TraceSession& session)
    : session_(session), root_pid_(0), running_(false)
{}

PtraceBackend::~PtraceBackend() {}

int PtraceBackend::start(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        LOG_ERROR("No command specified");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("fork failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        // Child process
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);

        // Build argv for execvp
        std::vector<char*> c_argv;
        for (size_t i = 0; i < argv.size(); ++i) {
            c_argv.push_back(const_cast<char*>(argv[i].c_str()));
        }
        c_argv.push_back(0);

        execvp(c_argv[0], &c_argv[0]);
        std::fprintf(stderr, "bdtrace: exec failed: %s: %s\n", argv[0].c_str(), strerror(errno));
        _exit(127);
    }

    // Parent
    root_pid_ = pid;
    g_child_pid = pid;

    // Install signal handlers for forwarding
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, 0);
    sigaction(SIGTERM, &sa, 0);

    // Wait for child's initial SIGSTOP
    int status;
    waitpid(pid, &status, 0);

    setup_child(pid);

    ProcessState ps(pid, getpid());
    procs_[pid] = ps;

    ProcessRecord rec;
    rec.pid = pid;
    rec.ppid = getpid();
    rec.cmdline = read_cmdline(pid);
    rec.start_time_us = now_us();
    session_.on_process_start(rec);

    LOG_INFO("Tracing PID %d: %s", pid, rec.cmdline.c_str());

    // Resume with syscall tracing
    ptrace(PTRACE_SYSCALL, pid, 0, 0);

    running_ = true;
    return 0;
}

void PtraceBackend::setup_child(int pid) {
    long opts = PTRACE_O_TRACESYSGOOD
              | PTRACE_O_TRACEFORK
              | PTRACE_O_TRACEVFORK
              | PTRACE_O_TRACECLONE
              | PTRACE_O_TRACEEXEC
              | PTRACE_O_TRACEEXIT;

    if (ptrace(PTRACE_SETOPTIONS, pid, 0, opts) < 0) {
        LOG_WARN("PTRACE_SETOPTIONS failed for %d: %s", pid, strerror(errno));
    }
}

int PtraceBackend::run_event_loop() {
    while (running_ && !procs_.empty()) {
        int status;
        pid_t pid = waitpid(-1, &status, __WALL);

        if (pid < 0) {
            if (errno == ECHILD) break;
            if (errno == EINTR) continue;
            LOG_ERROR("waitpid failed: %s", strerror(errno));
            break;
        }

        // New process we haven't seen?
        if (procs_.find(pid) == procs_.end()) {
            ProcessState ps(pid, 0);
            procs_[pid] = ps;
            setup_child(pid);
            procs_[pid].traced = true;
        }

        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
            session_.on_process_exit(pid, now_us(), exit_code);
            LOG_DEBUG("Process %d exited with %d", pid, exit_code);
            procs_.erase(pid);
            continue;
        }

        if (!WIFSTOPPED(status)) continue;

        int sig = WSTOPSIG(status);
        int event = (status >> 16) & 0xff;

        if (event == PTRACE_EVENT_FORK || event == PTRACE_EVENT_VFORK || event == PTRACE_EVENT_CLONE) {
            handle_fork_event(pid);
            ptrace(PTRACE_SYSCALL, pid, 0, 0);
        } else if (event == PTRACE_EVENT_EXEC) {
            handle_exec_event(pid);
            ptrace(PTRACE_SYSCALL, pid, 0, 0);
        } else if (event == PTRACE_EVENT_EXIT) {
            handle_exit_event(pid, status);
            ptrace(PTRACE_SYSCALL, pid, 0, 0);
        } else if (sig == (SIGTRAP | 0x80)) {
            // Syscall stop
            handle_syscall_stop(pid);
            ptrace(PTRACE_SYSCALL, pid, 0, 0);
        } else if (sig == SIGTRAP) {
            // Generic trap, just continue
            ptrace(PTRACE_SYSCALL, pid, 0, 0);
        } else if (sig == SIGSTOP && !procs_[pid].traced) {
            // Initial stop of new child
            setup_child(pid);
            procs_[pid].traced = true;
            ptrace(PTRACE_SYSCALL, pid, 0, 0);
        } else {
            // Deliver the signal to the tracee
            ptrace(PTRACE_SYSCALL, pid, 0, sig);
        }
    }

    session_.finalize();

    // Return root process exit code if available
    return 0;
}

void PtraceBackend::stop() {
    running_ = false;
}

void PtraceBackend::handle_syscall_stop(int pid) {
    std::map<int, ProcessState>::iterator it = procs_.find(pid);
    if (it == procs_.end()) return;

    ProcessState& ps = it->second;

    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, pid, 0, &regs) < 0) return;

    if (!ps.in_syscall) {
        // Syscall entry
        long nr = REG_SYSCALL(regs);
        handle_syscall_entry(pid, ps, nr);
        ps.in_syscall = true;
        ps.pending_syscall = nr;
    } else {
        // Syscall exit
        handle_syscall_exit(pid, ps);
        ps.in_syscall = false;
        ps.pending_syscall = -1;
    }
}

void PtraceBackend::handle_syscall_entry(int pid, ProcessState& ps, long syscall_nr) {
    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, pid, 0, &regs) < 0) return;

    if (syscall_nr == SYS_OPEN_NR) {
        ps.pending_path_addr = REG_ARG0(regs);
        ps.pending_flags = (int)REG_ARG1(regs);
    } else if (syscall_nr == SYS_OPENAT_NR) {
        ps.pending_path_addr = REG_ARG1(regs);
        ps.pending_flags = (int)REG_ARG2(regs);
    }
}

void PtraceBackend::handle_syscall_exit(int pid, ProcessState& ps) {
    long nr = ps.pending_syscall;

    if (nr != SYS_OPEN_NR && nr != SYS_OPENAT_NR) return;

    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, pid, 0, &regs) < 0) return;

    long retval = REG_RETVAL(regs);
    // Check if syscall failed (fd < 0)
    if ((long long)retval < 0) return;

    std::string path = read_string(pid, ps.pending_path_addr);
    if (path.empty() || should_filter_path(path)) return;

    FileAccessRecord fa;
    fa.pid = pid;
    fa.filename = path;
    fa.fd = (int)retval;

    int flags = ps.pending_flags;
    int accmode = flags & O_ACCMODE;
    if (accmode == O_RDONLY) {
        fa.mode = FA_READ;
    } else if (accmode == O_WRONLY) {
        fa.mode = FA_WRITE;
    } else {
        fa.mode = FA_RDWR;
    }

    LOG_DEBUG("File access: pid=%d %s mode=%d fd=%d",
              pid, path.c_str(), fa.mode, fa.fd);

    session_.on_file_access(fa);
}

void PtraceBackend::handle_fork_event(int pid) {
    unsigned long child_pid = 0;
    ptrace(PTRACE_GETEVENTMSG, pid, 0, &child_pid);

    if (child_pid == 0) return;

    LOG_DEBUG("Fork: %d -> %d", pid, (int)child_pid);

    ProcessState ps((int)child_pid, pid);
    procs_[(int)child_pid] = ps;

    ProcessRecord rec;
    rec.pid = (int)child_pid;
    rec.ppid = pid;
    rec.start_time_us = now_us();
    rec.cmdline = read_cmdline((int)child_pid);
    session_.on_process_start(rec);
}

void PtraceBackend::handle_exec_event(int pid) {
    std::string cmdline = read_cmdline(pid);
    LOG_DEBUG("Exec: %d -> %s", pid, cmdline.c_str());

    // Update the process record with the new command
    // We re-insert; the DB will update via INSERT OR REPLACE
    // Actually we just log it; the original insert has the pid already
    // For simplicity, we update the cmdline by inserting a new record
    // but since pid is PRIMARY KEY, we need to use a different approach.
    // Let's update via direct SQL or just accept the original cmdline.
    // For now, the cmdline from fork may be the parent's; exec is where
    // we get the real command. We'll update it.

    // Use a direct update approach
    ProcessRecord rec;
    rec.pid = pid;
    if (procs_.find(pid) != procs_.end()) {
        rec.ppid = procs_[pid].ppid;
    }
    rec.cmdline = cmdline;
    rec.start_time_us = now_us();

    // Delete old and re-insert (simple approach for C++03)
    char sql_buf[128];
    std::snprintf(sql_buf, sizeof(sql_buf), "DELETE FROM processes WHERE pid = %d", pid);
    session_.db().exec_raw(sql_buf);
    session_.on_process_start(rec);
}

void PtraceBackend::handle_exit_event(int pid, int status) {
    unsigned long exit_status = 0;
    ptrace(PTRACE_GETEVENTMSG, pid, 0, &exit_status);
    LOG_DEBUG("Exit event: %d status=%lu", pid, exit_status);
}

std::string PtraceBackend::read_string(int pid, unsigned long addr, size_t max_len) {
    std::string result;
    if (addr == 0) return result;

    for (size_t i = 0; i < max_len; i += sizeof(long)) {
        errno = 0;
        long word = ptrace(PTRACE_PEEKDATA, pid, addr + i, 0);
        if (errno != 0) break;

        const char* p = reinterpret_cast<const char*>(&word);
        for (size_t j = 0; j < sizeof(long); ++j) {
            if (p[j] == '\0') return result;
            result += p[j];
        }
    }
    return result;
}

std::string PtraceBackend::read_cmdline(int pid) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);

    FILE* f = std::fopen(path, "r");
    if (!f) return "";

    std::string result;
    char buf[4096];
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);

    if (n == 0) return "";

    // cmdline has NUL-separated args; replace with spaces
    for (size_t i = 0; i < n; ++i) {
        if (buf[i] == '\0') {
            if (i + 1 < n) result += ' ';
        } else {
            result += buf[i];
        }
    }
    return result;
}

bool PtraceBackend::should_filter_path(const std::string& path) {
    if (path.size() < 2) return true;
    if (path.compare(0, 5, "/dev/") == 0) return true;
    if (path.compare(0, 6, "/proc/") == 0) return true;
    if (path.compare(0, 4, "/sys/") == 0) return true;
    return false;
}

} // namespace bdtrace
