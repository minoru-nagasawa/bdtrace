#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE 1  // pread64 for /proc/<pid>/mem on 32-bit builds
#endif

#include "ptrace_backend.h"
#include "ptrace_defs.h"
#include "../common/types.h"
#include "../common/log.h"

#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/prctl.h>
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

// ---------------------------------------------------------------------------
// seccomp-BPF fast path.
//
// On kernels >= 3.5 the traced child installs (pre-exec, inherited by every
// descendant) a BPF filter that returns SECCOMP_RET_TRACE for the syscalls we
// record and SECCOMP_RET_ALLOW for everything else. Tracees then run under
// PTRACE_CONT at near-native speed; only interesting syscalls stop the tracer
// (PTRACE_EVENT_SECCOMP at entry, plus one PTRACE_SYSCALL-requested exit
// stop). On Linux 2.6.x none of this activates and the classic PTRACE_SYSCALL
// loop is used unchanged.
//
// All constants and structs are defined locally: CentOS 5 headers predate
// seccomp filtering, and this code must still COMPILE there even though it
// only RUNS on newer kernels.
// ---------------------------------------------------------------------------

#ifndef PR_SET_NO_NEW_PRIVS
#define PR_SET_NO_NEW_PRIVS 38
#endif
#ifndef PR_SET_SECCOMP
#define PR_SET_SECCOMP 22
#endif
#define BD_SECCOMP_MODE_FILTER 2
#define BD_SECCOMP_RET_TRACE   0x7ff00000u
#define BD_SECCOMP_RET_ALLOW   0x7fff0000u

// Classic BPF opcodes (BPF_LD|BPF_W|BPF_ABS, BPF_JMP|BPF_JEQ|BPF_K, BPF_RET|BPF_K)
#define BD_BPF_LD_W_ABS 0x20
#define BD_BPF_JEQ_K    0x15
#define BD_BPF_RET_K    0x06

struct bd_sock_filter {
    uint16_t code;
    uint8_t jt;
    uint8_t jf;
    uint32_t k;
};

struct bd_sock_fprog {
    uint16_t len;
    struct bd_sock_filter* filter;
};

namespace bdtrace {

// Syscalls the tracer records; everything else runs untraced under seccomp
// mode. Must cover every syscall decode_syscall_entry() cares about.
static const long BD_TRACED_SYSCALLS[] = {
    SYS_OPEN_NR, SYS_CREAT_NR, SYS_OPENAT_NR, SYS_OPENAT2_NR,
    SYS_STAT_NR, SYS_LSTAT_NR, SYS_NEWFSTATAT_NR, SYS_STATX_NR,
    SYS_ACCESS_NR, SYS_FACCESSAT_NR, SYS_FACCESSAT2_NR,
    SYS_CHDIR_NR, SYS_FCHDIR_NR,
    SYS_RENAME_NR, SYS_RENAMEAT_NR, SYS_RENAMEAT2_NR,
    SYS_MKDIR_NR, SYS_MKDIRAT_NR,
    SYS_LINK_NR, SYS_LINKAT_NR,
    SYS_UNLINK_NR, SYS_UNLINKAT_NR,
    SYS_SYMLINK_NR, SYS_SYMLINKAT_NR,
    SYS_READLINK_NR, SYS_READLINKAT_NR,
    SYS_CHMOD_NR, SYS_FCHMODAT_NR,
    SYS_CHOWN_NR, SYS_FCHOWNAT_NR,
    SYS_MKNOD_NR, SYS_MKNODAT_NR,
    SYS_TRUNCATE_NR, SYS_UTIMENSAT_NR,
    SYS_EXECVE_NR, SYS_EXECVEAT_NR,
};

// Runs in the forked child, pre-exec. Only async-signal-safe calls.
static int install_seccomp_filter() {
    const int n = (int)(sizeof(BD_TRACED_SYSCALLS) / sizeof(BD_TRACED_SYSCALLS[0]));
    struct bd_sock_filter prog[3 + (sizeof(BD_TRACED_SYSCALLS) / sizeof(BD_TRACED_SYSCALLS[0])) + 2];
    int idx = 0;

    // [0] load seccomp_data.arch
    prog[idx].code = BD_BPF_LD_W_ABS; prog[idx].jt = 0; prog[idx].jf = 0;
    prog[idx].k = 4; ++idx;
    // [1] foreign architecture (e.g. x32/i386 tracee under x86_64): ALLOW,
    //     i.e. untraced - matching the classic path, which cannot decode
    //     foreign-arch syscall numbers either.
    prog[idx].code = BD_BPF_JEQ_K; prog[idx].jt = 0;
    prog[idx].jf = (uint8_t)(n + 1); prog[idx].k = BD_AUDIT_ARCH; ++idx;
    // [2] load seccomp_data.nr
    prog[idx].code = BD_BPF_LD_W_ABS; prog[idx].jt = 0; prog[idx].jf = 0;
    prog[idx].k = 0; ++idx;
    // [3 .. 3+n-1] one JEQ per traced syscall, jumping to RET_TRACE at [4+n]
    for (int i = 0; i < n; ++i) {
        prog[idx].code = BD_BPF_JEQ_K;
        prog[idx].jt = (uint8_t)(n - i);
        prog[idx].jf = 0;
        prog[idx].k = (uint32_t)BD_TRACED_SYSCALLS[i];
        ++idx;
    }
    // [3+n] RET ALLOW
    prog[idx].code = BD_BPF_RET_K; prog[idx].jt = 0; prog[idx].jf = 0;
    prog[idx].k = BD_SECCOMP_RET_ALLOW; ++idx;
    // [4+n] RET TRACE
    prog[idx].code = BD_BPF_RET_K; prog[idx].jt = 0; prog[idx].jf = 0;
    prog[idx].k = BD_SECCOMP_RET_TRACE; ++idx;

    struct bd_sock_fprog fprog;
    fprog.len = (uint16_t)idx;
    fprog.filter = prog;

    // NO_NEW_PRIVS lets an unprivileged process install the filter. Side
    // effect: setuid binaries in the traced tree won't elevate - they don't
    // under plain ptrace either.
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return -1;
    if (prctl(PR_SET_SECCOMP, BD_SECCOMP_MODE_FILTER, &fprog) != 0) return -1;
    return 0;
}

// SECCOMP_RET_TRACE + PTRACE_EVENT_SECCOMP need Linux 3.5.
static bool kernel_supports_seccomp_trace() {
    struct utsname u;
    if (uname(&u) != 0) return false;
    int maj = 0, min = 0;
    if (std::sscanf(u.release, "%d.%d", &maj, &min) < 2) return false;
    if (maj > 3) return true;
    return maj == 3 && min >= 5;
}

static volatile sig_atomic_t g_child_pid = 0;
static volatile sig_atomic_t g_stop_requested = 0;
static volatile sig_atomic_t g_alarm_fired = 0;
static struct sigaction g_old_sigint;
static struct sigaction g_old_sigterm;
static struct sigaction g_old_sigalrm;

// Stall watchdog tuning.
static const int     WATCHDOG_TICK_SEC  = 2;            // SIGALRM granularity
static const int64_t STALL_THRESHOLD_US = 10000000LL;   // 10s w/o events => suspect hang
static const int64_t STALL_REPEAT_US    = 30000000LL;   // re-dump every 30s while stalled

static void signal_handler(int sig) {
    g_stop_requested = 1;
    if (g_child_pid > 0) {
        kill(g_child_pid, sig);
    }
}

// Async-signal-safe: only flips a flag. The actual stall check/dump runs back in
// the event loop, so SIGALRM merely interrupts a blocked waitpid() (EINTR).
static void alarm_handler(int) {
    g_alarm_fired = 1;
}

PtraceBackend::PtraceBackend(TraceSession& session)
    : session_(session), root_pid_(0), running_(false), procs_only_(false)
    , seccomp_allowed_(true), seccomp_mode_(false)
    , last_event_us_(0), last_stall_report_us_(0)
    , cnt_fork_events_(0), cnt_exec_events_(0)
    , cnt_sigstop_swallowed_(0), cnt_sig_reinjected_(0)
    , cnt_sigstop_reinjected_(0), cnt_race_unknown_first_(0)
    , cnt_mem_reads_(0), cnt_peek_fallbacks_(0)
    , cnt_getregs_skipped_(0), cnt_phase_resyncs_(0)
    , cnt_seccomp_stops_(0)
{}

PtraceBackend::~PtraceBackend() {
    for (std::map<int, ProcessState>::iterator it = procs_.begin();
         it != procs_.end(); ++it) {
        close_mem_fd(it->second);
    }
}

void PtraceBackend::close_mem_fd(ProcessState& ps) {
    if (ps.mem_fd >= 0) {
        close(ps.mem_fd);
    }
    ps.mem_fd = -1;
}

int PtraceBackend::start(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        LOG_ERROR("No command specified");
        return -1;
    }

    // Try the seccomp fast path only when the kernel can support it; on
    // Linux 2.6.x this is always false and nothing below changes behavior.
    bool attempt_seccomp = seccomp_allowed_ && !procs_only_
        && !std::getenv("BDTRACE_NO_SECCOMP")
        && kernel_supports_seccomp_trace();
    int sfd[2] = { -1, -1 };
    if (attempt_seccomp && pipe(sfd) != 0) {
        attempt_seccomp = false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("fork failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        // Child process
        PT(PTRACE_TRACEME, 0, 0, 0);
        if (attempt_seccomp) {
            // Install the (inheritable) filter pre-exec and report the result
            // to the parent. Written before SIGSTOP, so the parent's read
            // after waitpid() cannot block.
            char ack = (install_seccomp_filter() == 0) ? 'S' : 'F';
            ssize_t w = write(sfd[1], &ack, 1);
            (void)w;
            close(sfd[0]);
            close(sfd[1]);
        }
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

    if (attempt_seccomp) {
        close(sfd[1]);
        char ack = 'F';
        ssize_t n = read(sfd[0], &ack, 1);
        close(sfd[0]);
        if (n == 1 && ack == 'S') {
            seccomp_mode_ = true;
            if (!set_ptrace_options(pid)) {
                // Filter is installed but we cannot receive its stops: the
                // tracee's traced syscalls would all fail with ENOSYS.
                LOG_ERROR("kernel accepted the seccomp filter but refused "
                          "PTRACE_O_TRACESECCOMP; aborting (use --no-seccomp)");
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                return -1;
            }
            LOG_INFO("seccomp-BPF fast path enabled");
        } else {
            LOG_WARN("seccomp filter install failed, using full syscall tracing");
            setup_child(pid);
        }
    } else {
        setup_child(pid);
    }

    ProcessState ps(pid, getpid());
    ps.cached_cwd = read_proc_link(pid, "cwd");
    ps.cached_cmdline = read_cmdline(pid);
    procs_[pid] = ps;

    ProcessRecord rec;
    rec.pid = pid;
    rec.ppid = getpid();
    rec.cmdline = ps.cached_cmdline;
    rec.cwd = ps.cached_cwd;
    rec.start_time_us = now_us();
    session_.on_process_start(rec);

    LOG_INFO("Tracing PID %d: %s", pid, rec.cmdline.c_str());

    resume(pid, 0);

    running_ = true;
    return 0;
}

// Resume a stopped tracee. Classic mode stops at every syscall entry/exit
// (PTRACE_SYSCALL); procs-only and seccomp modes use PTRACE_CONT, so tracees
// run at near-native speed and only the requested events (fork/exec/exit,
// plus seccomp stops in seccomp mode) reach the tracer.
void PtraceBackend::resume(int pid, long sig) {
    PT((procs_only_ || seccomp_mode_) ? PTRACE_CONT : PTRACE_SYSCALL,
       pid, 0, sig);
}

bool PtraceBackend::set_ptrace_options(int pid) {
    long opts = PTRACE_O_TRACESYSGOOD
              | PTRACE_O_TRACEFORK
              | PTRACE_O_TRACEVFORK
              | PTRACE_O_TRACECLONE
              | PTRACE_O_TRACEEXEC
              | PTRACE_O_TRACEEXIT;

    if (seccomp_mode_) {
        // EXITKILL (3.8+) is best-effort: if the tracer dies, tracees are
        // killed instead of being left with a filter whose RET_TRACE turns
        // every traced syscall into ENOSYS.
        if (PT(PTRACE_SETOPTIONS, pid, 0,
               opts | PTRACE_O_TRACESECCOMP | PTRACE_O_EXITKILL) == 0)
            return true;
        return PT(PTRACE_SETOPTIONS, pid, 0, opts | PTRACE_O_TRACESECCOMP) == 0;
    }
    return PT(PTRACE_SETOPTIONS, pid, 0, opts) == 0;
}

void PtraceBackend::setup_child(int pid) {
    if (!set_ptrace_options(pid)) {
        LOG_WARN("PTRACE_SETOPTIONS failed for %d: %s", pid, strerror(errno));
    }
}

int PtraceBackend::run_event_loop() {
    int64_t start_time = now_us();
    int64_t last_report_time = start_time;
    last_event_us_ = start_time;

    // Install the stall-watchdog timer signal. SIGALRM interrupts a blocked
    // waitpid() so a hang (no events while processes remain) becomes visible.
    struct sigaction sa_alrm;
    std::memset(&sa_alrm, 0, sizeof(sa_alrm));
    sa_alrm.sa_handler = alarm_handler;
    sigaction(SIGALRM, &sa_alrm, &g_old_sigalrm);

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
            int64_t db_bytes = session_.db_size_bytes();
            int db_mb = (int)(db_bytes / (1024 * 1024));
            std::fprintf(stderr,
                "[bdtrace] %02d:%02d:%02d | %d procs | %d files | %d ev/s | DB: %d MB",
                hours, mins, secs, procs, files, ev_per_sec, db_mb);
            if (session_.write_error_count() > 0) {
                std::fprintf(stderr, " | %d errors", session_.write_error_count());
            }
            std::fprintf(stderr, "\n");
            print_diag_counters(stderr);
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

        // Arm the watchdog only around the blocking wait, so SIGALRM never
        // interrupts event processing / DB writes below.
        alarm(WATCHDOG_TICK_SEC);
        int status;
        pid_t pid = waitpid(-1, &status, __WALL);
        alarm(0);

        if (pid < 0) {
            if (errno == ECHILD) break;
            if (errno == EINTR) {
                if (g_alarm_fired) { g_alarm_fired = 0; check_stall(); }
                continue;
            }
            LOG_ERROR("waitpid failed: %s", strerror(errno));
            break;
        }

        // An event arrived: the tracer is making progress.
        g_alarm_fired = 0;
        last_event_us_ = now_us();
        if (last_stall_report_us_ != 0) {
            std::fprintf(stderr, "[bdtrace] STALL CLEARED: events resumed (pid %d)\n", pid);
            last_stall_report_us_ = 0;
        }

        // A process we haven't registered yet. This happens when a newly
        // cloned/forked child's initial SIGSTOP is reported by waitpid() before
        // the parent's PTRACE_EVENT_{FORK,VFORK,CLONE} stop. The order of those
        // two events is not guaranteed, notably on Linux 2.6.x (CentOS 5).
        //
        // We must NOT mark it traced here: the very first stop of a brand-new
        // child is its initial SIGSTOP, which has to be SWALLOWED by the SIGSTOP
        // branch below (which only fires when !traced). If we set traced=true
        // here, that SIGSTOP falls through to the default branch and gets
        // re-injected, leaving the child permanently stopped - which deadlocks
        // multi-threaded tracees (e.g. the JVM waits at a safepoint barrier for
        // the stopped thread forever, and the tracer then blocks in waitpid()).
        std::map<int, ProcessState>::iterator pit = procs_.find(pid);
        if (pit == procs_.end()) {
            pit = procs_.insert(std::make_pair(pid, ProcessState(pid, 0))).first;
            setup_child(pid);
            ++cnt_race_unknown_first_;
        }
        ProcessState& ps = pit->second;

        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
            session_.on_process_exit(pid, now_us(), exit_code,
                                     ps.user_time_us, ps.sys_time_us,
                                     ps.peak_rss_kb,
                                     ps.io_read_bytes, ps.io_write_bytes);
            LOG_DEBUG("Process %d exited with %d", pid, exit_code);
            close_mem_fd(ps);
            procs_.erase(pit);
            continue;
        }

        if (!WIFSTOPPED(status)) continue;

        int sig = WSTOPSIG(status);
        int event = (status >> 16) & 0xff;

        if (event == PTRACE_EVENT_FORK || event == PTRACE_EVENT_VFORK || event == PTRACE_EVENT_CLONE) {
            ++cnt_fork_events_;
            handle_fork_event(pid);
            resume(pid, 0);
        } else if (event == PTRACE_EVENT_EXEC) {
            ++cnt_exec_events_;
            handle_exec_event(pid);
            resume(pid, 0);
        } else if (event == PTRACE_EVENT_EXIT) {
            handle_exit_event(pid, status);
            resume(pid, 0);
        } else if (event == PTRACE_EVENT_SECCOMP) {
            ++cnt_seccomp_stops_;
            handle_seccomp_stop(pid, ps);  // resumes the tracee itself
        } else if (sig == (SIGTRAP | 0x80)) {
            handle_syscall_stop(pid, ps);
            resume(pid, 0);
        } else if (sig == SIGTRAP) {
            resume(pid, 0);
        } else if (sig == SIGSTOP && !ps.traced) {
            setup_child(pid);
            ps.traced = true;
            ++cnt_sigstop_swallowed_;
            resume(pid, 0);
        } else {
            // Re-inject any other signal to the tracee. A SIGSTOP reaching here
            // (i.e. for an already-traced process) is a red flag: it leaves the
            // tracee stopped and is the classic cause of a multi-threaded hang.
            if (sig == SIGSTOP) ++cnt_sigstop_reinjected_;
            ++cnt_sig_reinjected_;
            resume(pid, (long)sig);
        }
    }

    // Restore original signal handlers and clear stale PID
    alarm(0);
    g_child_pid = 0;
    sigaction(SIGINT, &g_old_sigint, 0);
    sigaction(SIGTERM, &g_old_sigterm, 0);
    sigaction(SIGALRM, &g_old_sigalrm, 0);

    std::fprintf(stderr, "[bdtrace] final ");
    print_diag_counters(stderr);

    session_.finalize();
    return 0;
}

void PtraceBackend::print_diag_counters(FILE* out) {
    std::fprintf(out,
        "events: fork/clone=%ld exec=%ld | sigstop swallowed=%ld | "
        "reinjected=%ld (sigstop=%ld) | unknown-pid-first races=%ld | "
        "mem reads=%ld peek fallbacks=%ld | "
        "getregs skipped=%ld phase resyncs=%ld | seccomp stops=%ld\n",
        cnt_fork_events_, cnt_exec_events_, cnt_sigstop_swallowed_,
        cnt_sig_reinjected_, cnt_sigstop_reinjected_, cnt_race_unknown_first_,
        cnt_mem_reads_, cnt_peek_fallbacks_,
        cnt_getregs_skipped_, cnt_phase_resyncs_, cnt_seccomp_stops_);
}

std::string PtraceBackend::read_proc_state(int pid) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE* f = std::fopen(path, "r");
    if (!f) return "";

    char line[256];
    std::string result;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "State:", 6) == 0) {
            // e.g. "State:\tT (stopped)\n" -> "T(stopped)"
            const char* p = line + 6;
            while (*p == ' ' || *p == '\t') ++p;
            for (; *p && *p != '\n'; ++p) {
                if (*p == ' ') continue;
                result += *p;
            }
            break;
        }
    }
    std::fclose(f);
    return result;
}

// Called when the watchdog fires (a blocked waitpid was interrupted by SIGALRM).
// If no ptrace event has arrived for STALL_THRESHOLD_US while processes are still
// tracked, the tracer is almost certainly hung - dump enough state to find which
// tracee is stuck and why (notably any process in 'T (stopped)' that we believe
// we already continued, the signature of a mishandled SIGSTOP).
void PtraceBackend::check_stall() {
    int64_t now = now_us();
    int64_t gap = now - last_event_us_;
    if (gap < STALL_THRESHOLD_US) return;
    if (!running_ || procs_.empty()) return;
    if (last_stall_report_us_ != 0 && (now - last_stall_report_us_) < STALL_REPEAT_US) return;

    // In seccomp/procs-only modes, long gaps without events are normal (a
    // compiler can crunch for minutes without touching a traced syscall).
    // Only report if some tracee is actually sitting in 'T (stopped)'.
    if (seccomp_mode_ || procs_only_) {
        bool any_stopped = false;
        int checked = 0;
        for (std::map<int, ProcessState>::iterator it = procs_.begin();
             it != procs_.end() && checked < 256; ++it, ++checked) {
            std::string st = read_proc_state(it->first);
            if (!st.empty() && st[0] == 'T') { any_stopped = true; break; }
        }
        if (!any_stopped) {
            last_stall_report_us_ = now;  // throttle the rescan
            return;
        }
    }
    last_stall_report_us_ = now;

    std::fprintf(stderr,
        "[bdtrace] *** STALL: no ptrace events for %ds, %d process(es) tracked, "
        "tracer blocked in waitpid() ***\n",
        (int)(gap / 1000000LL), (int)procs_.size());
    std::fprintf(stderr, "[bdtrace] ");
    print_diag_counters(stderr);

    const int MAX_DUMP = 50;
    int shown = 0;
    for (std::map<int, ProcessState>::iterator it = procs_.begin();
         it != procs_.end(); ++it) {
        if (shown >= MAX_DUMP) {
            std::fprintf(stderr, "[bdtrace]   ... and %d more process(es)\n",
                         (int)procs_.size() - shown);
            break;
        }
        ProcessState& p = it->second;
        std::string st = read_proc_state(it->first);
        const char* flag = "";
        if (!st.empty() && st[0] == 'T') flag = "   <-- STOPPED (suspect)";
        std::fprintf(stderr,
            "[bdtrace]   pid=%d ppid=%d traced=%d last_syscall=%ld state=%s%s\n",
            it->first, p.ppid, (int)p.traced, p.pending_syscall,
            st.empty() ? "?" : st.c_str(), flag);
        ++shown;
    }
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

// Record what we need from a syscall entry: pending syscall number, path
// argument addresses, open flags, and whether the exit stop must be
// inspected. Shared by the classic PTRACE_SYSCALL entry stop and the
// seccomp-mode PTRACE_EVENT_SECCOMP stop (both occur at syscall entry with
// the argument registers intact).
void PtraceBackend::decode_syscall_entry(ProcessState& ps,
                                         const user_regs_struct& regs,
                                         long syscall_nr) {
    {
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

        // Only syscalls that saved a path (plus fchdir) need their exit
        // inspected; everything else lets the exit stop skip GETREGS.
        ps.exit_needed = (ps.pending_path_addr != 0
                          || syscall_nr == SYS_FCHDIR_NR);
    }
}

// seccomp mode: the filter trapped an interesting syscall at entry. Decode
// it, then request this one syscall's exit stop with PTRACE_SYSCALL; all
// other syscalls run untraced under PTRACE_CONT.
void PtraceBackend::handle_seccomp_stop(int pid, ProcessState& ps) {
    struct user_regs_struct regs;
    if (PT(PTRACE_GETREGS, pid, 0, &regs) < 0) {
        PT(PTRACE_CONT, pid, 0, 0);
        return;
    }

    decode_syscall_entry(ps, regs, REG_SYSCALL(regs));

    if (ps.exit_needed) {
        ps.sc_phase = SC_PHASE_EXIT_NEXT;
        PT(PTRACE_SYSCALL, pid, 0, 0);
    } else {
        ps.sc_phase = SC_PHASE_ENTRY_NEXT;
        ps.pending_syscall = -1;
        PT(PTRACE_CONT, pid, 0, 0);
    }
}

void PtraceBackend::handle_syscall_stop(int pid, ProcessState& ps) {
    // Fast path: the exit stop of a syscall we don't record. Nothing in the
    // registers is needed, so skip PTRACE_GETREGS entirely (one syscall saved
    // per uninteresting entry/exit pair - the majority during a build).
    if (ps.sc_phase == SC_PHASE_EXIT_NEXT && !ps.exit_needed) {
        ps.sc_phase = SC_PHASE_ENTRY_NEXT;
        ps.pending_syscall = -1;
        ++cnt_getregs_skipped_;
        return;
    }

    struct user_regs_struct regs;
    if (PT(PTRACE_GETREGS, pid, 0, &regs) < 0) return;

    long syscall_nr = REG_SYSCALL(regs);
    long rax = REG_RETVAL(regs);

    // On syscall entry, rax == -ENOSYS (kernel sets this before dispatch).
    // On syscall exit, rax == actual return value.
    bool is_entry = (rax == -ENOSYS);

    // The phase toggle normally agrees with the ENOSYS heuristic. If they
    // disagree (e.g. a kernel that doesn't report a syscall-exit stop where we
    // expected one), trust the registers and resync.
    if (ps.sc_phase == SC_PHASE_ENTRY_NEXT && !is_entry) ++cnt_phase_resyncs_;
    else if (ps.sc_phase == SC_PHASE_EXIT_NEXT && is_entry) ++cnt_phase_resyncs_;
    ps.sc_phase = is_entry ? SC_PHASE_EXIT_NEXT : SC_PHASE_ENTRY_NEXT;

    if (is_entry) {
        decode_syscall_entry(ps, regs, syscall_nr);
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
        ps.exit_needed = false;
    }
}

void PtraceBackend::handle_fork_event(int pid) {
    unsigned long child_pid = 0;
    PT(PTRACE_GETEVENTMSG, pid, 0, &child_pid);

    if (child_pid == 0) return;

    LOG_DEBUG("Fork: %d -> %d", pid, (int)child_pid);

    // Inherit parent's cached cwd and cmdline (fork/clone preserves both;
    // they only change on exec, which refreshes the caches).
    std::string cwd;
    std::string cmdline;
    std::map<int, ProcessState>::iterator parent_it = procs_.find(pid);
    if (parent_it != procs_.end() && !parent_it->second.cached_cwd.empty()) {
        cwd = parent_it->second.cached_cwd;
    } else {
        cwd = read_proc_link((int)child_pid, "cwd");
    }
    if (parent_it != procs_.end() && !parent_it->second.cached_cmdline.empty()) {
        cmdline = parent_it->second.cached_cmdline;
    } else {
        cmdline = read_cmdline((int)child_pid);
    }

    // The child's initial SIGSTOP and this parent-side fork/clone event may be
    // reported by waitpid() in either order (see note in run_event_loop). If the
    // child was already registered via its initial SIGSTOP, update that entry in
    // place - do NOT overwrite it, or we would reset its 'traced' flag back to
    // false and mishandle a later stop. Only create a fresh entry if unseen.
    std::map<int, ProcessState>::iterator child_it = procs_.find((int)child_pid);
    if (child_it != procs_.end()) {
        child_it->second.ppid = pid;
        child_it->second.cached_cwd = cwd;
        child_it->second.cached_cmdline = cmdline;
    } else {
        ProcessState ps((int)child_pid, pid);
        ps.cached_cwd = cwd;
        ps.cached_cmdline = cmdline;
        procs_[(int)child_pid] = ps;
    }

    ProcessRecord rec;
    rec.pid = (int)child_pid;
    rec.ppid = pid;
    rec.start_time_us = now_us();
    rec.cmdline = cmdline;
    rec.cwd = cwd;
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
        // The execve that triggered this event still reports a syscall-exit
        // stop afterwards; nothing to inspect there.
        it->second.exit_needed = false;
        // The /proc/<pid>/mem fd may be bound to the pre-exec address space on
        // some kernels; drop it and reopen lazily against the new image.
        close_mem_fd(it->second);
        // Refresh cached cwd (exec may change via interpreter path)
        it->second.cached_cwd = read_proc_link(pid, "cwd");
        it->second.cached_cmdline = cmdline;
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

// Bulk path read via /proc/<pid>/mem: one pread per page instead of one
// PTRACE_PEEKDATA per word. Works on Linux 2.6.x because the target is always
// ptrace-stopped by us when this is called. Reads never cross a page boundary
// so a string ending just before an unmapped page still succeeds.
// Returns false if /proc/<pid>/mem is unusable (caller falls back to PEEKDATA).
bool PtraceBackend::read_string_mem(ProcessState& ps, unsigned long addr,
                                    size_t max_len, std::string& out) {
    if (ps.mem_fd == -2) return false;
    if (ps.mem_fd < 0) {
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/mem", ps.pid);
        ps.mem_fd = open(path, O_RDONLY);
        if (ps.mem_fd < 0) {
            ps.mem_fd = -2;
            return false;
        }
    }

    static size_t page_size = 0;
    if (page_size == 0) {
        long p = sysconf(_SC_PAGESIZE);
        page_size = (p > 0) ? (size_t)p : 4096;
    }

    char buf[4096];
    size_t total = 0;
    bool first_chunk = true;
    while (total < max_len) {
        size_t to_page_end = page_size - ((addr + total) % page_size);
        size_t chunk = max_len - total;
        if (chunk > to_page_end) chunk = to_page_end;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);

        ssize_t n = pread64(ps.mem_fd, buf, chunk, (off64_t)(addr + total));
        if (n <= 0) {
            if (first_chunk) return false;  // fd unusable or addr bad: fall back
            break;  // string runs into unmapped memory: return what we have
        }
        first_chunk = false;

        const char* nul = (const char*)std::memchr(buf, '\0', (size_t)n);
        if (nul) {
            out.append(buf, nul - buf);
            return true;
        }
        out.append(buf, (size_t)n);
        total += (size_t)n;
    }
    return true;
}

std::string PtraceBackend::read_string(int pid, unsigned long addr, size_t max_len) {
    if (addr == 0) return std::string();

    std::map<int, ProcessState>::iterator it = procs_.find(pid);
    if (it != procs_.end()) {
        std::string result;
        if (read_string_mem(it->second, addr, max_len, result)) {
            ++cnt_mem_reads_;
            return result;
        }
    }

    // Fallback: word-by-word PTRACE_PEEKDATA
    ++cnt_peek_fallbacks_;
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
