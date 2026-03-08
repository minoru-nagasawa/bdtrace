#ifndef BDTRACE_PTRACE_BACKEND_H
#define BDTRACE_PTRACE_BACKEND_H

#include "tracer_backend.h"
#include "process_state.h"
#include "trace_session.h"

#include <map>
#include <string>

namespace bdtrace {

class PtraceBackend : public ITracerBackend {
public:
    explicit PtraceBackend(TraceSession& session);
    ~PtraceBackend();

    int start(const std::vector<std::string>& argv);
    int run_event_loop();
    void stop();

private:
    TraceSession& session_;
    std::map<int, ProcessState> procs_;
    int root_pid_;
    volatile bool running_;

    void setup_child(int pid);
    void handle_syscall_stop(int pid);
    void handle_fork_event(int pid);
    void handle_exec_event(int pid);
    void handle_exit_event(int pid, int status);

    std::string read_string(int pid, unsigned long addr, size_t max_len = 4096);
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
