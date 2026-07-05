#ifndef BDTRACE_PTRACE_BACKEND_H
#define BDTRACE_PTRACE_BACKEND_H

#include "tracer_backend.h"
#include "process_state.h"
#include "trace_session.h"

#include <map>
#include <string>
#include <cstdio>
#include <stdint.h>

struct user_regs_struct;

namespace bdtrace {

class PtraceBackend : public ITracerBackend {
public:
    explicit PtraceBackend(TraceSession& session);
    ~PtraceBackend();

    // Record only the process tree (fork/exec/exit), skipping all syscall
    // stops: tracees run under PTRACE_CONT at near-native speed.
    void set_procs_only(bool on) { procs_only_ = on; }

    // Allow the seccomp-BPF fast path (default on). It self-enables only on
    // kernels >= 3.5; on Linux 2.6.x behavior is the classic PTRACE_SYSCALL
    // loop either way.
    void set_seccomp_allowed(bool on) { seccomp_allowed_ = on; }

    int start(const std::vector<std::string>& argv);
    int run_event_loop();
    void stop();

private:
    TraceSession& session_;
    std::map<int, ProcessState> procs_;
    int root_pid_;
    volatile bool running_;
    bool procs_only_;
    bool seccomp_allowed_;  // user permits trying the seccomp fast path
    bool seccomp_mode_;     // fast path active: filter installed, CONT resume
    int64_t drain_deadline_us_; // 0 = not draining; else give up after this

    // Diagnostics: stall watchdog + signal/race counters (always on).
    int64_t last_event_us_;        // timestamp of the most recent waitpid() event
    int64_t last_stall_report_us_; // 0 = no stall currently being reported
    long cnt_fork_events_;
    long cnt_exec_events_;
    long cnt_sigstop_swallowed_;   // initial SIGSTOPs correctly swallowed
    long cnt_sig_reinjected_;      // signals re-injected to the tracee
    long cnt_sigstop_reinjected_;  // SIGSTOPs re-injected (red flag - see run_event_loop)
    long cnt_race_unknown_first_;  // child seen via waitpid() before its fork/clone event
    long cnt_mem_reads_;           // path strings read in bulk via /proc/<pid>/mem
    long cnt_peek_fallbacks_;      // path strings read word-by-word via PEEKDATA
    long cnt_getregs_skipped_;     // uninteresting syscall exits resumed without GETREGS
    long cnt_phase_resyncs_;       // entry/exit phase toggle disagreed with ENOSYS check
    long cnt_seccomp_stops_;       // PTRACE_EVENT_SECCOMP stops (fast path)
    long cnt_plain_sigtrap_;       // SIGTRAP stops without the TRACESYSGOOD 0x80 bit
    long cnt_stuck_kicked_;        // tracees found stuck in ptrace-stop by the watchdog
    long cnt_resume_retries_;      // lost resumes detected and retried on the spot
    long cnt_sig_ignored_;         // build-internal signals to bdtrace ignored
    long resume_count_;            // total resumes issued (for fault injection)

    void check_stall();
    void print_diag_counters(FILE* out);
    std::string read_proc_state(int pid);
    std::string read_proc_wchan(int pid);

    void setup_child(int pid);
    bool set_ptrace_options(int pid);
    void resume(int pid, long sig);
    bool kick_if_stuck(int pid, long sig);
    void handle_syscall_stop(int pid, ProcessState& ps);
    void handle_seccomp_stop(int pid, ProcessState& ps);
    void decode_syscall_entry(ProcessState& ps, const user_regs_struct& regs,
                              long syscall_nr);
    void handle_fork_event(int pid);
    void handle_exec_event(int pid);
    void handle_exit_event(int pid, int status);

    std::string read_string(int pid, unsigned long addr, size_t max_len = 4096);
    bool read_string_mem(ProcessState& ps, unsigned long addr, size_t max_len,
                         std::string& out);
    void close_mem_fd(ProcessState& ps);
    std::string read_cmdline(int pid);
    std::string read_proc_link(int pid, const char* entry);
    std::string resolve_path_cached(int pid, const std::string& path);
    bool should_filter_path(const std::string& path);
    void record_access(int pid, unsigned long addr, FileAccessMode mode, int fd = -1);
    void record_failed_access(int pid, unsigned long addr, FileAccessMode mode, int errno_val);

    void read_proc_cpu(int pid, int64_t& user_us, int64_t& sys_us);
    int64_t read_proc_peak_rss(int pid);
    void read_proc_io(int pid, int64_t& read_bytes, int64_t& write_bytes);
};

} // namespace bdtrace

#endif // BDTRACE_PTRACE_BACKEND_H
