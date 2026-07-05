#ifndef BDTRACE_TRACE_SESSION_H
#define BDTRACE_TRACE_SESSION_H

#include <deque>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <pthread.h>
#include "../db/database.h"
#include "../common/types.h"

namespace bdtrace {

// One queued trace event. SQLite is built with SQLITE_THREADSAFE=0, so during
// a trace only the writer thread may touch the Database; the ptrace event
// loop just enqueues copies and never blocks on fsync/checkpoint stalls.
struct QueuedEvent {
    enum Type {
        EV_PROC_START,
        EV_PROC_EXIT,
        EV_FILE_ACCESS,
        EV_FAILED_ACCESS,
        EV_DELETE_PROC
    };
    int type;
    ProcessRecord proc;        // EV_PROC_START
    FileAccessRecord fa;       // EV_FILE_ACCESS
    FailedAccessRecord failed; // EV_FAILED_ACCESS
    int pid;                   // EV_PROC_EXIT / EV_DELETE_PROC
    int64_t end_time_us;       // EV_PROC_EXIT fields...
    int exit_code;
    int64_t user_time_us;
    int64_t sys_time_us;
    int64_t peak_rss_kb;
    int64_t io_read_bytes;
    int64_t io_write_bytes;

    QueuedEvent()
        : type(0), pid(0), end_time_us(0), exit_code(0)
        , user_time_us(0), sys_time_us(0), peak_rss_kb(0)
        , io_read_bytes(0), io_write_bytes(0) {}
};

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

    // DB size for progress reports; cached by the writer thread so the event
    // loop never touches the sqlite connection while the writer owns it.
    int64_t db_size_bytes();

    // Progress counters (P1.3)
    int process_count() const { return process_count_; }
    int file_access_count() const { return file_access_count_; }
    int failed_access_count() const { return failed_access_count_; }

    // Error tracking (P1.1). Counters are updated by the writer thread and
    // read by the event loop, so reads take the queue mutex.
    int write_error_count();
    bool has_fatal_errors();

private:
    Database db_;
    int event_count_;
    bool in_transaction_;
    bool finalized_;

    // P1.1: Error tracking (owned by the writer thread in async mode)
    int write_error_count_;
    int consecutive_errors_;

    // P1.2: WAL checkpoint tracking
    int total_event_count_;

    // P1.3: Progress counters (owned by the enqueuing/main thread)
    int process_count_;
    int file_access_count_;
    int failed_access_count_;

    // Dedup: per process (per exec incarnation), remember which accesses were
    // already written and drop repeats. Key encodes mode (and errno for failed
    // accesses) alongside the path. Cleared when the pid starts a new
    // incarnation (fork/exec) and freed when it exits. Main-thread only.
    typedef std::set<std::pair<long, std::string> > SeenSet;
    std::map<int, SeenSet> seen_;
    long dedup_dropped_;

    // PID recycling: kernel pids wrap (pid_max defaults to 32768 on Linux
    // 2.6), so a long build reuses pids of exited processes while
    // processes.pid is the primary key. Track a generation per real pid and
    // remap every event to a synthetic id (generation * 10,000,000 + pid;
    // generation 0 keeps the real pid, so short traces are unaffected). An
    // exec re-image (delete_process + on_process_start) keeps its generation.
    std::map<int, int> pid_gen_;
    std::set<int> live_pids_;       // pids whose current incarnation is alive
    std::set<int> expect_reinsert_; // delete_process'd, awaiting exec re-insert
    long pid_recycles_;
    int current_synth(int pid);

    // Writer thread state
    bool async_;                    // writer thread running; enqueue instead of write
    bool stop_requested_;
    pthread_t writer_thread_;
    pthread_mutex_t mutex_;         // guards queue_, error counters, db_size_cache_
    pthread_cond_t cond_has_events_;
    pthread_cond_t cond_has_space_; // backpressure: bounded queue
    std::deque<QueuedEvent> queue_;
    int64_t db_size_cache_;

    static void* writer_thread_main(void* arg);
    void writer_loop();
    void apply_event(const QueuedEvent& ev);
    void enqueue(const QueuedEvent& ev);

    bool already_seen(int pid, long key, const std::string& filename);
    void maybe_commit();
    void check_write_result(bool ok, const char* op);
};

} // namespace bdtrace

#endif // BDTRACE_TRACE_SESSION_H
