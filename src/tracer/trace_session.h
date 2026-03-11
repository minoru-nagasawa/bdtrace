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

private:
    Database db_;
    int event_count_;
    bool in_transaction_;
    bool finalized_;

    void maybe_commit();
};

} // namespace bdtrace

#endif // BDTRACE_TRACE_SESSION_H
