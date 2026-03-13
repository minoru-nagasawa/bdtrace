#ifndef BDTRACE_TRACE_SESSION_H
#define BDTRACE_TRACE_SESSION_H

#include <string>
#include "../db/database.h"
#include "../common/types.h"

namespace bdtrace {

class TraceSession {
public:
    TraceSession();
    ~TraceSession();

    bool open(const std::string& db_path);
    void on_process_start(const ProcessRecord& rec);
    void on_process_exit(int pid, int64_t end_time_us, int exit_code,
                         int64_t user_time_us = 0, int64_t sys_time_us = 0,
                         int64_t peak_rss_kb = 0,
                         int64_t io_read_bytes = 0, int64_t io_write_bytes = 0);
    void on_file_access(const FileAccessRecord& rec);
    void on_failed_access(const FailedAccessRecord& rec);
    void delete_process(int pid);
    void finalize();

    Database& db() { return db_; }

    // Progress counters (P1.3)
    int process_count() const { return process_count_; }
    int file_access_count() const { return file_access_count_; }
    int failed_access_count() const { return failed_access_count_; }

    // Error tracking (P1.1)
    int write_error_count() const { return write_error_count_; }
    bool has_fatal_errors() const { return consecutive_errors_ >= 50; }

private:
    Database db_;
    int event_count_;
    bool in_transaction_;
    bool finalized_;

    // P1.1: Error tracking
    int write_error_count_;
    int consecutive_errors_;

    // P1.2: WAL checkpoint tracking
    int total_event_count_;

    // P1.3: Progress counters
    int process_count_;
    int file_access_count_;
    int failed_access_count_;

    void maybe_commit();
    void check_write_result(bool ok, const char* op);
};

} // namespace bdtrace

#endif // BDTRACE_TRACE_SESSION_H
