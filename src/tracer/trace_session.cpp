#include "trace_session.h"
#include "../common/log.h"

#include <signal.h>

namespace bdtrace {

static const int BATCH_SIZE = 5000;
static const int WAL_CHECKPOINT_INTERVAL = 50000;
// Bound the event queue so a stalled disk cannot grow memory without limit
// (~200 bytes/event -> tens of MB worst case).
static const size_t MAX_QUEUE_EVENTS = 200000;

TraceSession::TraceSession()
    : event_count_(0), in_transaction_(false), finalized_(false)
    , write_error_count_(0), consecutive_errors_(0)
    , total_event_count_(0)
    , process_count_(0), file_access_count_(0), failed_access_count_(0)
    , dedup_dropped_(0), pid_recycles_(0)
    , async_(false), stop_requested_(false)
    , db_size_cache_(0)
{
    pthread_mutex_init(&mutex_, 0);
    pthread_cond_init(&cond_has_events_, 0);
    pthread_cond_init(&cond_has_space_, 0);
}

TraceSession::~TraceSession() {
    finalize();
    pthread_cond_destroy(&cond_has_space_);
    pthread_cond_destroy(&cond_has_events_);
    pthread_mutex_destroy(&mutex_);
}

bool TraceSession::open(const std::string& db_path) {
    if (!db_.open(db_path)) return false;
    if (!db_.init_schema()) return false;

    // Hand the sqlite connection to a dedicated writer thread so the ptrace
    // event loop never waits on commits/checkpoints. If thread creation
    // fails, fall back to synchronous writes.
    //
    // The tracer's stall watchdog and Ctrl-C handling rely on SIGALRM /
    // SIGINT / SIGTERM interrupting the event loop's blocked waitpid()
    // (EINTR). Process-directed signals are delivered to ANY thread that
    // doesn't block them, so block them here before pthread_create (the mask
    // is inherited by the new thread) and restore afterwards - otherwise the
    // writer thread can swallow SIGALRM and the watchdog, progress reports,
    // and graceful stop all go silent while the loop waits for events.
    sigset_t block_set, old_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGALRM);
    sigaddset(&block_set, SIGINT);
    sigaddset(&block_set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block_set, &old_set);

    stop_requested_ = false;
    if (pthread_create(&writer_thread_, 0, writer_thread_main, this) == 0) {
        async_ = true;
    } else {
        LOG_WARN("Could not start DB writer thread, using synchronous writes");
        async_ = false;
    }

    pthread_sigmask(SIG_SETMASK, &old_set, 0);
    return true;
}

static const int PID_GEN_STRIDE = 10000000;  // > pid_max on any Linux

int TraceSession::current_synth(int pid) {
    std::map<int, int>::iterator it = pid_gen_.find(pid);
    if (it == pid_gen_.end() || it->second == 0) return pid;
    return it->second * PID_GEN_STRIDE + pid;
}

bool TraceSession::already_seen(int pid, long key, const std::string& filename) {
    SeenSet& seen = seen_[pid];
    if (!seen.insert(std::make_pair(key, filename)).second) {
        ++dedup_dropped_;
        return true;
    }
    return false;
}

void TraceSession::enqueue(const QueuedEvent& ev) {
    if (!async_) {
        apply_event(ev);
        return;
    }
    pthread_mutex_lock(&mutex_);
    while (queue_.size() >= MAX_QUEUE_EVENTS && !stop_requested_) {
        pthread_cond_wait(&cond_has_space_, &mutex_);
    }
    queue_.push_back(ev);
    pthread_cond_signal(&cond_has_events_);
    pthread_mutex_unlock(&mutex_);
}

void* TraceSession::writer_thread_main(void* arg) {
    static_cast<TraceSession*>(arg)->writer_loop();
    return 0;
}

void TraceSession::writer_loop() {
    std::deque<QueuedEvent> local;
    for (;;) {
        pthread_mutex_lock(&mutex_);
        while (queue_.empty() && !stop_requested_) {
            pthread_cond_wait(&cond_has_events_, &mutex_);
        }
        if (queue_.empty() && stop_requested_) {
            pthread_mutex_unlock(&mutex_);
            break;
        }
        local.swap(queue_);
        pthread_cond_broadcast(&cond_has_space_);
        pthread_mutex_unlock(&mutex_);

        for (std::deque<QueuedEvent>::iterator it = local.begin();
             it != local.end(); ++it) {
            apply_event(*it);
        }
        local.clear();

        // Publish the DB size for the event loop's progress reports
        if (!in_transaction_) {
            int64_t sz = db_.get_db_size_bytes();
            pthread_mutex_lock(&mutex_);
            db_size_cache_ = sz;
            pthread_mutex_unlock(&mutex_);
        }
    }
    if (in_transaction_) {
        db_.commit_transaction();
        in_transaction_ = false;
    }
}

// Runs on the writer thread in async mode, inline in sync mode. Either way a
// single thread owns db_ / transaction state / error counters at a time.
void TraceSession::apply_event(const QueuedEvent& ev) {
    if (consecutive_errors_ >= 50) return;  // fatal: stop writing
    if (!in_transaction_) {
        db_.begin_transaction();
        in_transaction_ = true;
    }
    switch (ev.type) {
    case QueuedEvent::EV_PROC_START:
        check_write_result(db_.insert_process(ev.proc), "insert_process");
        break;
    case QueuedEvent::EV_PROC_EXIT:
        check_write_result(
            db_.update_process_exit(ev.pid, ev.end_time_us, ev.exit_code,
                                    ev.user_time_us, ev.sys_time_us,
                                    ev.peak_rss_kb,
                                    ev.io_read_bytes, ev.io_write_bytes),
            "update_process_exit");
        break;
    case QueuedEvent::EV_FILE_ACCESS:
        check_write_result(db_.insert_file_access(ev.fa), "insert_file_access");
        break;
    case QueuedEvent::EV_FAILED_ACCESS:
        check_write_result(db_.insert_failed_access(ev.failed), "insert_failed_access");
        break;
    case QueuedEvent::EV_DELETE_PROC:
        check_write_result(db_.delete_process(ev.pid), "delete_process");
        break;
    }
    ++event_count_;
    maybe_commit();
}

void TraceSession::check_write_result(bool ok, const char* op) {
    if (ok) {
        pthread_mutex_lock(&mutex_);
        consecutive_errors_ = 0;
        pthread_mutex_unlock(&mutex_);
    } else {
        pthread_mutex_lock(&mutex_);
        int total = ++write_error_count_;
        int consec = ++consecutive_errors_;
        pthread_mutex_unlock(&mutex_);
        if (consec == 1 || consec == 10 || consec == 50) {
            LOG_ERROR("DB write failed (%s): %s (total errors: %d, consecutive: %d)",
                      op, db_.last_error().c_str(), total, consec);
        }
    }
}

int TraceSession::write_error_count() {
    pthread_mutex_lock(&mutex_);
    int n = write_error_count_;
    pthread_mutex_unlock(&mutex_);
    return n;
}

bool TraceSession::has_fatal_errors() {
    pthread_mutex_lock(&mutex_);
    bool fatal = consecutive_errors_ >= 50;
    pthread_mutex_unlock(&mutex_);
    return fatal;
}

int64_t TraceSession::db_size_bytes() {
    if (!async_) return db_.get_db_size_bytes();
    pthread_mutex_lock(&mutex_);
    int64_t sz = db_size_cache_;
    pthread_mutex_unlock(&mutex_);
    return sz;
}

void TraceSession::on_process_start(const ProcessRecord& rec) {
    if (has_fatal_errors()) return;
    // New incarnation of this pid (fork or exec): forget previous accesses
    seen_.erase(rec.pid);

    std::map<int, int>::iterator g = pid_gen_.find(rec.pid);
    if (g == pid_gen_.end()) {
        pid_gen_.insert(std::make_pair(rec.pid, 0));
    } else if (expect_reinsert_.erase(rec.pid) > 0 || live_pids_.count(rec.pid)) {
        // exec re-image of the live incarnation: keep the same synthetic id
    } else {
        // The kernel recycled the pid of an exited process: new incarnation,
        // or the pid row would collide with the finished one.
        ++g->second;
        ++pid_recycles_;
    }
    live_pids_.insert(rec.pid);

    QueuedEvent ev;
    ev.type = QueuedEvent::EV_PROC_START;
    ev.proc = rec;
    ev.proc.pid = current_synth(rec.pid);
    ev.proc.ppid = current_synth(rec.ppid);
    enqueue(ev);
    ++process_count_;
}

void TraceSession::on_process_exit(int pid, int64_t end_time_us, int exit_code,
                                    int64_t user_time_us, int64_t sys_time_us,
                                    int64_t peak_rss_kb,
                                    int64_t io_read_bytes, int64_t io_write_bytes) {
    if (has_fatal_errors()) return;
    seen_.erase(pid);
    QueuedEvent ev;
    ev.type = QueuedEvent::EV_PROC_EXIT;
    ev.pid = current_synth(pid);
    live_pids_.erase(pid);
    expect_reinsert_.erase(pid);
    ev.end_time_us = end_time_us;
    ev.exit_code = exit_code;
    ev.user_time_us = user_time_us;
    ev.sys_time_us = sys_time_us;
    ev.peak_rss_kb = peak_rss_kb;
    ev.io_read_bytes = io_read_bytes;
    ev.io_write_bytes = io_write_bytes;
    enqueue(ev);
}

void TraceSession::on_file_access(const FileAccessRecord& rec) {
    if (has_fatal_errors()) return;
    if (already_seen(rec.pid, rec.mode, rec.filename)) return;
    QueuedEvent ev;
    ev.type = QueuedEvent::EV_FILE_ACCESS;
    ev.fa = rec;
    ev.fa.pid = current_synth(rec.pid);
    enqueue(ev);
    ++file_access_count_;
}

void TraceSession::on_failed_access(const FailedAccessRecord& rec) {
    if (has_fatal_errors()) return;
    // Distinguish failed from successful accesses (and by errno) in the key
    long key = 0x40000000L | ((long)rec.errno_val << 8) | (long)rec.mode;
    if (already_seen(rec.pid, key, rec.filename)) return;
    QueuedEvent ev;
    ev.type = QueuedEvent::EV_FAILED_ACCESS;
    ev.failed = rec;
    ev.failed.pid = current_synth(rec.pid);
    enqueue(ev);
    ++failed_access_count_;
}

void TraceSession::delete_process(int pid) {
    if (has_fatal_errors()) return;
    QueuedEvent ev;
    ev.type = QueuedEvent::EV_DELETE_PROC;
    ev.pid = current_synth(pid);
    // The exec flow re-inserts this pid right after; that re-insert belongs
    // to the SAME incarnation, not a kernel pid reuse.
    expect_reinsert_.insert(pid);
    enqueue(ev);
}

void TraceSession::finalize() {
    // Stop the writer thread first; it drains the queue and commits any open
    // transaction before exiting. After the join, this thread owns db_ again.
    if (async_) {
        pthread_mutex_lock(&mutex_);
        stop_requested_ = true;
        pthread_cond_broadcast(&cond_has_events_);
        pthread_cond_broadcast(&cond_has_space_);
        pthread_mutex_unlock(&mutex_);
        pthread_join(writer_thread_, 0);
        async_ = false;
    }
    if (in_transaction_) {
        db_.commit_transaction();
        in_transaction_ = false;
    }
    if (!finalized_) {
        finalized_ = true;
        // Indexes are deferred during the trace (bulk load into bare tables);
        // build them now, before populate_counts which groups by pid.
        db_.create_indexes();
        db_.populate_counts();
        db_.analyze();
        if (write_error_count_ > 0) {
            LOG_WARN("Trace completed with %d DB write errors", write_error_count_);
        }
        if (dedup_dropped_ > 0) {
            LOG_INFO("Dedup: %ld repeated accesses not written", dedup_dropped_);
        }
        if (pid_recycles_ > 0) {
            LOG_INFO("PID reuse: %ld recycled pids remapped to synthetic ids",
                     pid_recycles_);
        }
    }
    seen_.clear();
}

void TraceSession::maybe_commit() {
    if (event_count_ >= BATCH_SIZE) {
        db_.commit_transaction();
        in_transaction_ = false;
        total_event_count_ += event_count_;
        event_count_ = 0;

        // P1.2: Periodic WAL checkpoint
        if (total_event_count_ % WAL_CHECKPOINT_INTERVAL == 0) {
            db_.wal_checkpoint();
        }
    }
}

} // namespace bdtrace
