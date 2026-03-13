#include "ptrace_backend.h"
#include "ptrace_defs.h"
#include "../common/types.h"
#include "../common/log.h"

#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/syscall.h>
#include <linux/ptrace.h>
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

// glibc declares ptrace() with enum __ptrace_request as 1st arg.
// In C++ this requires an explicit cast from int constants.
#define PT(req, pid, addr, data) \
    ptrace(static_cast<__ptrace_request>(req), (pid), (void*)(addr), (void*)(data))

namespace bdtrace {

static volatile sig_atomic_t g_child_pid = 0;
static volatile sig_atomic_t g_stop_requested = 0;
static struct sigaction g_old_sigint;
static struct sigaction g_old_sigterm;

static void signal_handler(int sig) {
    g_stop_requested = 1;
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
        PT(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);

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

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, &g_old_sigint);
    sigaction(SIGTERM, &sa, &g_old_sigterm);

    // Wait for child's initial SIGSTOP
    int status;
    waitpid(pid, &status, 0);

    setup_child(pid);

    ProcessState ps(pid, getpid());
    ps.cached_cwd = read_proc_link(pid, "cwd");
    procs_[pid] = ps;

    ProcessRecord rec;
    rec.pid = pid;
    rec.ppid = getpid();
    rec.cmdline = read_cmdline(pid);
    rec.cwd = ps.cached_cwd;
    rec.start_time_us = now_us();
    session_.on_process_start(rec);

    LOG_INFO("Tracing PID %d: %s", pid, rec.cmdline.c_str());

    PT(PTRACE_SYSCALL, pid, 0, 0);

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

    if (PT(PTRACE_SETOPTIONS, pid, 0, opts) < 0) {
        LOG_WARN("PTRACE_SETOPTIONS failed for %d: %s", pid, strerror(errno));
    }
}

int PtraceBackend::run_event_loop() {
    int64_t start_time = now_us();
    int64_t last_report_time = start_time;

    while (running_ && !procs_.empty()) {
        // P1.3: Progress reporting every 60 seconds
        int64_t now = now_us();
        if (now - last_report_time >= 60000000LL) {
            int64_t elapsed_us = now - start_time;
            int hours = (int)(elapsed_us / 3600000000LL);
            int mins = (int)((elapsed_us % 3600000000LL) / 60000000LL);
            int secs = (int)((elapsed_us % 60000000LL) / 1000000LL);
            int procs = session_.process_count();
            int files = session_.file_access_count();
            int ev_per_sec = 0;
            if (elapsed_us > 0) {
                ev_per_sec = (int)((int64_t)(procs + files) * 1000000LL / elapsed_us);
            }
            int64_t db_bytes = session_.db().get_db_size_bytes();
            int db_mb = (int)(db_bytes / (1024 * 1024));
            std::fprintf(stderr,
                "[bdtrace] %02d:%02d:%02d | %d procs | %d files | %d ev/s | DB: %d MB",
                hours, mins, secs, procs, files, ev_per_sec, db_mb);
            if (session_.write_error_count() > 0) {
                std::fprintf(stderr, " | %d errors", session_.write_error_count());
            }
            std::fprintf(stderr, "\n");
            last_report_time = now;
        }

        // P1.1: Stop on fatal DB errors
        if (session_.has_fatal_errors()) {
            LOG_ERROR("Too many consecutive DB write errors, stopping trace");
            break;
        }

        // P2.6: Graceful stop on signal - set running_ to false for natural drain
        if (g_stop_requested && running_) {
            LOG_INFO("Signal received, draining remaining events...");
            running_ = false;
        }

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
            std::map<int, ProcessState>::iterator exit_it = procs_.find(pid);
            if (exit_it != procs_.end()) {
                session_.on_process_exit(pid, now_us(), exit_code,
                                         exit_it->second.user_time_us,
                                         exit_it->second.sys_time_us,
                                         exit_it->second.peak_rss_kb,
                                         exit_it->second.io_read_bytes,
                                         exit_it->second.io_write_bytes);
            } else {
                session_.on_process_exit(pid, now_us(), exit_code);
            }
            LOG_DEBUG("Process %d exited with %d", pid, exit_code);
            procs_.erase(pid);
            continue;
        }

        if (!WIFSTOPPED(status)) continue;

        int sig = WSTOPSIG(status);
        int event = (status >> 16) & 0xff;

        if (event == PTRACE_EVENT_FORK || event == PTRACE_EVENT_VFORK || event == PTRACE_EVENT_CLONE) {
            handle_fork_event(pid);
            PT(PTRACE_SYSCALL, pid, 0, 0);
        } else if (event == PTRACE_EVENT_EXEC) {
            handle_exec_event(pid);
            PT(PTRACE_SYSCALL, pid, 0, 0);
        } else if (event == PTRACE_EVENT_EXIT) {
            handle_exit_event(pid, status);
            PT(PTRACE_SYSCALL, pid, 0, 0);
        } else if (sig == (SIGTRAP | 0x80)) {
            handle_syscall_stop(pid);
            PT(PTRACE_SYSCALL, pid, 0, 0);
        } else if (sig == SIGTRAP) {
            PT(PTRACE_SYSCALL, pid, 0, 0);
        } else if (sig == SIGSTOP && !procs_[pid].traced) {
            setup_child(pid);
            procs_[pid].traced = true;
            PT(PTRACE_SYSCALL, pid, 0, 0);
        } else {
            PT(PTRACE_SYSCALL, pid, 0, (long)sig);
        }
    }

    // Restore original signal handlers and clear stale PID
    g_child_pid = 0;
    sigaction(SIGINT, &g_old_sigint, 0);
    sigaction(SIGTERM, &g_old_sigterm, 0);

    session_.finalize();
    return 0;
}

void PtraceBackend::stop() {
    running_ = false;
}

static std::string normalize_path(const std::string& path) {
    if (path.empty()) return path;

    // Fast path: if no "." or ".." components and no "//", skip normalization
    bool needs_normalize = false;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '.') {
            // Check if it's a component boundary
            bool at_start = (i == 0 || path[i - 1] == '/');
            if (at_start) {
                // "." or ".." component?
                size_t next = i + 1;
                if (next >= path.size() || path[next] == '/') {
                    needs_normalize = true;
                    break;
                }
                if (path[next] == '.' && (next + 1 >= path.size() || path[next + 1] == '/')) {
                    needs_normalize = true;
                    break;
                }
            }
        } else if (path[i] == '/' && i + 1 < path.size() && path[i + 1] == '/') {
            needs_normalize = true;
            break;
        }
    }
    if (!needs_normalize) return path;

    // Split into components
    std::vector<std::string> parts;
    bool absolute = (path[0] == '/');
    size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') ++i;
        size_t start = i;
        while (i < path.size() && path[i] != '/') ++i;
        if (i > start) {
            std::string comp = path.substr(start, i - start);
            if (comp == ".") {
                // skip
            } else if (comp == "..") {
                if (!parts.empty() && parts.back() != "..") {
                    parts.pop_back();
                } else if (!absolute) {
                    parts.push_back(comp);
                }
                // if absolute and parts is empty, just ignore (can't go above /)
            } else {
                parts.push_back(comp);
            }
        }
    }

    std::string result;
    if (absolute) result = "/";
    for (size_t j = 0; j < parts.size(); ++j) {
        if (j > 0) result += '/';
        result += parts[j];
    }
    if (result.empty()) result = ".";
    return result;
}

std::string PtraceBackend::resolve_path_cached(int pid, const std::string& path) {
    if (path.empty()) return path;
    std::string full;
    if (path[0] == '/') {
        full = path;
    } else {
        // Use cached cwd instead of reading /proc every time
        std::map<int, ProcessState>::iterator it = procs_.find(pid);
        std::string cwd;
        if (it != procs_.end() && !it->second.cached_cwd.empty()) {
            cwd = it->second.cached_cwd;
        } else {
            cwd = read_proc_link(pid, "cwd");
            if (it != procs_.end()) {
                it->second.cached_cwd = cwd;
            }
        }
        if (cwd.empty()) return path;
        full = cwd + "/" + path;
    }
    return normalize_path(full);
}

void PtraceBackend::record_access(int pid, unsigned long addr, FileAccessMode mode, int fd) {
    std::string path = read_string(pid, addr);
    if (!path.empty() && !should_filter_path(path)) {
        path = resolve_path_cached(pid, path);
        if (should_filter_path(path)) return;
        FileAccessRecord fa;
        fa.pid = pid;
        fa.filename = path;
        fa.mode = mode;
        fa.fd = fd;
        fa.timestamp_us = now_us();
        LOG_DEBUG("File access: pid=%d %s mode=%d fd=%d",
                  pid, path.c_str(), mode, fd);
        session_.on_file_access(fa);
    }
}

void PtraceBackend::record_failed_access(int pid, unsigned long addr, FileAccessMode mode, int errno_val) {
    std::string path = read_string(pid, addr);
    if (!path.empty() && !should_filter_path(path)) {
        path = resolve_path_cached(pid, path);
        if (should_filter_path(path)) return;
        FailedAccessRecord fa;
        fa.pid = pid;
        fa.filename = path;
        fa.mode = mode;
        fa.errno_val = errno_val;
        fa.timestamp_us = now_us();
        LOG_DEBUG("Failed access: pid=%d %s mode=%d errno=%d",
                  pid, path.c_str(), mode, errno_val);
        session_.on_failed_access(fa);
    }
}

void PtraceBackend::handle_syscall_stop(int pid) {
    std::map<int, ProcessState>::iterator it = procs_.find(pid);
    if (it == procs_.end()) return;

    ProcessState& ps = it->second;

    struct user_regs_struct regs;
    if (PT(PTRACE_GETREGS, pid, 0, &regs) < 0) return;

    long syscall_nr = REG_SYSCALL(regs);
    long rax = REG_RETVAL(regs);

    // On syscall entry, rax == -ENOSYS (kernel sets this before dispatch).
    // On syscall exit, rax == actual return value.
    bool is_entry = (rax == -ENOSYS);

    if (is_entry) {
        ps.pending_syscall = syscall_nr;
        ps.pending_path_addr = 0;
        ps.pending_path_addr2 = 0;
        ps.pending_flags = 0;

        // --- open family: save path + flags ---
        if (syscall_nr == SYS_OPEN_NR) {
            ps.pending_path_addr = REG_ARG0(regs);
            ps.pending_flags = (int)REG_ARG1(regs);
        } else if (syscall_nr == SYS_OPENAT_NR || syscall_nr == SYS_OPENAT2_NR) {
            ps.pending_path_addr = REG_ARG1(regs);
            ps.pending_flags = (int)REG_ARG2(regs);
        } else if (syscall_nr == SYS_CREAT_NR) {
            ps.pending_path_addr = REG_ARG0(regs);
            ps.pending_flags = O_WRONLY | O_CREAT | O_TRUNC;
        }
        // --- single-path at arg0 ---
        else if (syscall_nr == SYS_STAT_NR || syscall_nr == SYS_LSTAT_NR
              || syscall_nr == SYS_ACCESS_NR
              || syscall_nr == SYS_CHDIR_NR
              || syscall_nr == SYS_UNLINK_NR
              || syscall_nr == SYS_MKDIR_NR
              || syscall_nr == SYS_CHMOD_NR
              || syscall_nr == SYS_CHOWN_NR
              || syscall_nr == SYS_READLINK_NR
              || syscall_nr == SYS_TRUNCATE_NR
              || syscall_nr == SYS_MKNOD_NR
              || syscall_nr == SYS_EXECVE_NR) {
            ps.pending_path_addr = REG_ARG0(regs);
        }
        // --- single-path "at" variants: path at arg1 ---
        else if (syscall_nr == SYS_NEWFSTATAT_NR
              || syscall_nr == SYS_FACCESSAT_NR || syscall_nr == SYS_FACCESSAT2_NR
              || syscall_nr == SYS_UNLINKAT_NR
              || syscall_nr == SYS_MKDIRAT_NR
              || syscall_nr == SYS_FCHMODAT_NR
              || syscall_nr == SYS_FCHOWNAT_NR
              || syscall_nr == SYS_READLINKAT_NR
              || syscall_nr == SYS_UTIMENSAT_NR
              || syscall_nr == SYS_MKNODAT_NR
              || syscall_nr == SYS_EXECVEAT_NR
              || syscall_nr == SYS_STATX_NR) {
            ps.pending_path_addr = REG_ARG1(regs);
        }
        // --- two-path: rename(old, new), link(old, new), symlink(target, linkpath) ---
        else if (syscall_nr == SYS_RENAME_NR || syscall_nr == SYS_LINK_NR
              || syscall_nr == SYS_SYMLINK_NR) {
            ps.pending_path_addr = REG_ARG0(regs);
            ps.pending_path_addr2 = REG_ARG1(regs);
        }
        // --- two-path "at": renameat(dfd1, old, dfd2, new) ---
        else if (syscall_nr == SYS_RENAMEAT_NR || syscall_nr == SYS_RENAMEAT2_NR) {
            ps.pending_path_addr = REG_ARG1(regs);
            ps.pending_path_addr2 = REG_ARG3(regs);
        }
        // --- linkat(olddfd, old, newdfd, new, flags) ---
        else if (syscall_nr == SYS_LINKAT_NR) {
            ps.pending_path_addr = REG_ARG1(regs);
            ps.pending_path_addr2 = REG_ARG3(regs);
        }
        // --- symlinkat(target, newdfd, linkpath) ---
        else if (syscall_nr == SYS_SYMLINKAT_NR) {
            ps.pending_path_addr = REG_ARG0(regs);
            ps.pending_path_addr2 = REG_ARG2(regs);
        }
        // --- fchdir(fd) ---
        else if (syscall_nr == SYS_FCHDIR_NR) {
            // No path to save; handled at exit via /proc/pid/cwd
        }
    } else {
        // Syscall exit - record based on pending_syscall
        long sc = ps.pending_syscall;

        // --- open/openat/openat2/creat ---
        if (sc == SYS_OPEN_NR || sc == SYS_OPENAT_NR || sc == SYS_OPENAT2_NR
            || sc == SYS_CREAT_NR) {
            if (ps.pending_path_addr) {
                int accmode = ps.pending_flags & O_ACCMODE;
                FileAccessMode mode = FA_RDWR;
                if (accmode == O_RDONLY) mode = FA_READ;
                else if (accmode == O_WRONLY) mode = FA_WRITE;
                if (rax >= 0) {
                    record_access(pid, ps.pending_path_addr, mode, (int)rax);
                } else {
                    record_failed_access(pid, ps.pending_path_addr, mode, (int)(-rax));
                }
            }
        }
        // --- stat/lstat/newfstatat/statx ---
        else if (sc == SYS_STAT_NR || sc == SYS_LSTAT_NR
              || sc == SYS_NEWFSTATAT_NR || sc == SYS_STATX_NR) {
            if (ps.pending_path_addr) {
                if (rax >= 0) {
                    record_access(pid, ps.pending_path_addr, FA_STAT);
                } else {
                    record_failed_access(pid, ps.pending_path_addr, FA_STAT, (int)(-rax));
                }
            }
        }
        // --- access/faccessat/faccessat2 ---
        else if (sc == SYS_ACCESS_NR || sc == SYS_FACCESSAT_NR
              || sc == SYS_FACCESSAT2_NR) {
            if (ps.pending_path_addr) {
                if (rax >= 0) {
                    record_access(pid, ps.pending_path_addr, FA_ACCESS);
                } else {
                    record_failed_access(pid, ps.pending_path_addr, FA_ACCESS, (int)(-rax));
                }
            }
        }
        // --- execve/execveat ---
        else if (sc == SYS_EXECVE_NR || sc == SYS_EXECVEAT_NR) {
            // execve only returns on failure; successful exec is handled
            // by handle_exec_event. Record at entry for failed exec.
            if (rax < 0 && ps.pending_path_addr) {
                record_access(pid, ps.pending_path_addr, FA_EXEC);
            }
        }
        // --- unlink/unlinkat ---
        else if (sc == SYS_UNLINK_NR || sc == SYS_UNLINKAT_NR) {
            if (rax >= 0 && ps.pending_path_addr) {
                record_access(pid, ps.pending_path_addr, FA_UNLINK);
            }
        }
        // --- rename/renameat/renameat2 ---
        else if (sc == SYS_RENAME_NR || sc == SYS_RENAMEAT_NR
              || sc == SYS_RENAMEAT2_NR) {
            if (rax >= 0) {
                if (ps.pending_path_addr)
                    record_access(pid, ps.pending_path_addr, FA_RENAME_SRC);
                if (ps.pending_path_addr2)
                    record_access(pid, ps.pending_path_addr2, FA_RENAME_DST);
            }
        }
        // --- link/linkat ---
        else if (sc == SYS_LINK_NR || sc == SYS_LINKAT_NR) {
            if (rax >= 0) {
                if (ps.pending_path_addr)
                    record_access(pid, ps.pending_path_addr, FA_LINK_SRC);
                if (ps.pending_path_addr2)
                    record_access(pid, ps.pending_path_addr2, FA_LINK_DST);
            }
        }
        // --- symlink/symlinkat ---
        else if (sc == SYS_SYMLINK_NR || sc == SYS_SYMLINKAT_NR) {
            if (rax >= 0) {
                if (ps.pending_path_addr)
                    record_access(pid, ps.pending_path_addr, FA_SYMLINK_TARGET);
                if (ps.pending_path_addr2)
                    record_access(pid, ps.pending_path_addr2, FA_SYMLINK_LINK);
            }
        }
        // --- readlink/readlinkat ---
        else if (sc == SYS_READLINK_NR || sc == SYS_READLINKAT_NR) {
            if (rax >= 0 && ps.pending_path_addr) {
                record_access(pid, ps.pending_path_addr, FA_READLINK);
            }
        }
        // --- mkdir/mkdirat ---
        else if (sc == SYS_MKDIR_NR || sc == SYS_MKDIRAT_NR) {
            if (rax >= 0 && ps.pending_path_addr) {
                record_access(pid, ps.pending_path_addr, FA_MKDIR);
            }
        }
        // --- chmod/fchmodat ---
        else if (sc == SYS_CHMOD_NR || sc == SYS_FCHMODAT_NR) {
            if (rax >= 0 && ps.pending_path_addr) {
                record_access(pid, ps.pending_path_addr, FA_CHMOD);
            }
        }
        // --- chown/fchownat ---
        else if (sc == SYS_CHOWN_NR || sc == SYS_FCHOWNAT_NR) {
            if (rax >= 0 && ps.pending_path_addr) {
                record_access(pid, ps.pending_path_addr, FA_CHOWN);
            }
        }
        // --- truncate ---
        else if (sc == SYS_TRUNCATE_NR) {
            if (rax >= 0 && ps.pending_path_addr) {
                record_access(pid, ps.pending_path_addr, FA_TRUNCATE);
            }
        }
        // --- mknod/mknodat ---
        else if (sc == SYS_MKNOD_NR || sc == SYS_MKNODAT_NR) {
            if (rax >= 0 && ps.pending_path_addr) {
                record_access(pid, ps.pending_path_addr, FA_MKNOD);
            }
        }
        // --- utimensat ---
        else if (sc == SYS_UTIMENSAT_NR) {
            if (rax >= 0 && ps.pending_path_addr) {
                record_access(pid, ps.pending_path_addr, FA_UTIMENS);
            }
        }
        // --- chdir ---
        else if (sc == SYS_CHDIR_NR) {
            if (rax >= 0 && ps.pending_path_addr) {
                record_access(pid, ps.pending_path_addr, FA_CHDIR);
                // Update cached cwd
                ps.cached_cwd = read_proc_link(pid, "cwd");
            }
        }
        // --- fchdir(fd) - read cwd from /proc ---
        else if (sc == SYS_FCHDIR_NR) {
            if (rax >= 0) {
                std::string cwd = read_proc_link(pid, "cwd");
                // Update cached cwd
                ps.cached_cwd = cwd;
                if (!cwd.empty() && !should_filter_path(cwd)) {
                    FileAccessRecord fa;
                    fa.pid = pid;
                    fa.filename = cwd;
                    fa.mode = FA_CHDIR;
                    fa.fd = -1;
                    fa.timestamp_us = now_us();
                    session_.on_file_access(fa);
                }
            }
        }

        ps.pending_syscall = -1;
        ps.pending_path_addr = 0;
        ps.pending_path_addr2 = 0;
    }
}

void PtraceBackend::handle_fork_event(int pid) {
    unsigned long child_pid = 0;
    PT(PTRACE_GETEVENTMSG, pid, 0, &child_pid);

    if (child_pid == 0) return;

    LOG_DEBUG("Fork: %d -> %d", pid, (int)child_pid);

    ProcessState ps((int)child_pid, pid);
    // Inherit parent's cached cwd (fork preserves cwd)
    std::map<int, ProcessState>::iterator parent_it = procs_.find(pid);
    if (parent_it != procs_.end() && !parent_it->second.cached_cwd.empty()) {
        ps.cached_cwd = parent_it->second.cached_cwd;
    } else {
        ps.cached_cwd = read_proc_link((int)child_pid, "cwd");
    }
    procs_[(int)child_pid] = ps;

    ProcessRecord rec;
    rec.pid = (int)child_pid;
    rec.ppid = pid;
    rec.start_time_us = now_us();
    rec.cmdline = read_cmdline((int)child_pid);
    rec.cwd = ps.cached_cwd;
    session_.on_process_start(rec);
}

void PtraceBackend::handle_exec_event(int pid) {
    std::string cmdline = read_cmdline(pid);
    LOG_DEBUG("Exec: %d -> %s", pid, cmdline.c_str());

    // Record the exec'd binary
    std::string exe = read_proc_link(pid, "exe");
    if (!exe.empty() && !should_filter_path(exe)) {
        FileAccessRecord fa;
        fa.pid = pid;
        fa.filename = exe;
        fa.mode = FA_EXEC;
        fa.fd = -1;
        fa.timestamp_us = now_us();
        session_.on_file_access(fa);
    }

    std::map<int, ProcessState>::iterator it = procs_.find(pid);
    if (it != procs_.end()) {
        it->second.pending_syscall = -1;
        it->second.pending_path_addr = 0;
        it->second.pending_path_addr2 = 0;
        // Refresh cached cwd (exec may change via interpreter path)
        it->second.cached_cwd = read_proc_link(pid, "cwd");
    }

    ProcessRecord rec;
    rec.pid = pid;
    if (it != procs_.end()) {
        rec.ppid = it->second.ppid;
        rec.cwd = it->second.cached_cwd;
    } else {
        rec.cwd = read_proc_link(pid, "cwd");
    }
    rec.cmdline = cmdline;
    rec.start_time_us = now_us();

    session_.delete_process(pid);
    session_.on_process_start(rec);
}

void PtraceBackend::handle_exit_event(int pid, int status) {
    unsigned long exit_status = 0;
    PT(PTRACE_GETEVENTMSG, pid, 0, &exit_status);
    LOG_DEBUG("Exit event: %d status=%lu", pid, exit_status);

    // Read resource usage while process still exists in /proc
    int64_t user_us = 0, sys_us = 0;
    read_proc_cpu(pid, user_us, sys_us);
    int64_t peak_rss = read_proc_peak_rss(pid);
    int64_t io_r = 0, io_w = 0;
    read_proc_io(pid, io_r, io_w);

    std::map<int, ProcessState>::iterator it = procs_.find(pid);
    if (it != procs_.end()) {
        it->second.user_time_us = user_us;
        it->second.sys_time_us = sys_us;
        it->second.peak_rss_kb = peak_rss;
        it->second.io_read_bytes = io_r;
        it->second.io_write_bytes = io_w;
    }
}

std::string PtraceBackend::read_string(int pid, unsigned long addr, size_t max_len) {
    if (addr == 0) return std::string();

    std::string result;
    for (size_t i = 0; i < max_len; i += sizeof(long)) {
        errno = 0;
        long word = PT(PTRACE_PEEKDATA, pid, addr + i, 0);
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

    for (size_t i = 0; i < n; ++i) {
        if (buf[i] == '\0') {
            if (i + 1 < n) result += ' ';
        } else {
            result += buf[i];
        }
    }
    return result;
}

std::string PtraceBackend::read_proc_link(int pid, const char* entry) {
    char path[128];
    std::snprintf(path, sizeof(path), "/proc/%d/%s", pid, entry);

    char buf[4096];
    ssize_t len = readlink(path, buf, sizeof(buf) - 1);
    if (len <= 0) return "";
    buf[len] = '\0';
    return std::string(buf, len);
}

bool PtraceBackend::should_filter_path(const std::string& path) {
    if (path.size() < 2) return true;
    if (path.compare(0, 5, "/dev/") == 0) return true;
    if (path.compare(0, 6, "/proc/") == 0) return true;
    if (path.compare(0, 4, "/sys/") == 0) return true;
    return false;
}

void PtraceBackend::read_proc_cpu(int pid, int64_t& user_us, int64_t& sys_us) {
    user_us = 0;
    sys_us = 0;
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE* f = std::fopen(path, "r");
    if (!f) return;

    char buf[1024];
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    if (n == 0) return;
    buf[n] = '\0';

    // Fields are space-separated; comm (field 2) may contain spaces/parens.
    // Find closing ')' to skip past comm field.
    char* p = std::strrchr(buf, ')');
    if (!p) return;
    p++; // skip ')'

    // After ')': " S ppid pgrp sid ... utime stime ..."
    // state=field 3, utime=field 14, stime=field 15
    // Skip fields 3-13 (11 space-separated tokens) to reach field 14
    long utime = 0, stime = 0;
    int skip = 11; // fields 3 through 13
    while (*p && skip > 0) {
        while (*p == ' ') ++p;
        while (*p && *p != ' ') ++p;
        --skip;
    }
    while (*p == ' ') ++p;
    utime = std::strtol(p, &p, 10);
    while (*p == ' ') ++p;
    stime = std::strtol(p, &p, 10);

    // Convert clock ticks to microseconds
    long ticks_per_sec = sysconf(_SC_CLK_TCK);
    if (ticks_per_sec <= 0) ticks_per_sec = 100;
    user_us = (int64_t)utime * 1000000 / ticks_per_sec;
    sys_us = (int64_t)stime * 1000000 / ticks_per_sec;
}

int64_t PtraceBackend::read_proc_peak_rss(int pid) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE* f = std::fopen(path, "r");
    if (!f) return 0;

    char line[256];
    int64_t peak_kb = 0;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "VmHWM:", 6) == 0) {
            peak_kb = std::strtol(line + 6, NULL, 10);
            break;
        }
    }
    std::fclose(f);
    return peak_kb;
}

void PtraceBackend::read_proc_io(int pid, int64_t& read_bytes, int64_t& write_bytes) {
    read_bytes = 0;
    write_bytes = 0;
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/io", pid);
    FILE* f = std::fopen(path, "r");
    if (!f) return; // /proc/pid/io may not exist on older kernels

    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "read_bytes:", 11) == 0) {
            read_bytes = std::strtol(line + 11, NULL, 10);
        } else if (std::strncmp(line, "write_bytes:", 12) == 0) {
            write_bytes = std::strtol(line + 12, NULL, 10);
        }
    }
    std::fclose(f);
}

} // namespace bdtrace
