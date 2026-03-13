#include "trace_session.h"
#include "../common/log.h"

namespace bdtrace {

static const int BATCH_SIZE = 5000;
static const int WAL_CHECKPOINT_INTERVAL = 50000;

TraceSession::TraceSession()
    : event_count_(0), in_transaction_(false), finalized_(false)
    , write_error_count_(0), consecutive_errors_(0)
    , total_event_count_(0)
    , process_count_(0), file_access_count_(0), failed_access_count_(0)
{}

TraceSession::~TraceSession() {
    finalize();
}

bool TraceSession::open(const std::string& db_path) {
    if (!db_.open(db_path)) return false;
    if (!db_.init_schema()) return false;
    return true;
}

void TraceSession::check_write_result(bool ok, const char* op) {
    if (ok) {
        consecutive_errors_ = 0;
    } else {
        ++write_error_count_;
        ++consecutive_errors_;
        if (consecutive_errors_ == 1 || consecutive_errors_ == 10 || consecutive_errors_ == 50) {
            LOG_ERROR("DB write failed (%s): %s (total errors: %d, consecutive: %d)",
                      op, db_.last_error().c_str(),
                      write_error_count_, consecutive_errors_);
        }
    }
}

void TraceSession::on_process_start(const ProcessRecord& rec) {
    if (has_fatal_errors()) return;
    if (!in_transaction_) {
        db_.begin_transaction();
        in_transaction_ = true;
    }
    check_write_result(db_.insert_process(rec), "insert_process");
    ++process_count_;
    ++event_count_;
    maybe_commit();
}

void TraceSession::on_process_exit(int pid, int64_t end_time_us, int exit_code,
                                    int64_t user_time_us, int64_t sys_time_us,
                                    int64_t peak_rss_kb,
                                    int64_t io_read_bytes, int64_t io_write_bytes) {
    if (has_fatal_errors()) return;
    if (!in_transaction_) {
        db_.begin_transaction();
        in_transaction_ = true;
    }
    check_write_result(
        db_.update_process_exit(pid, end_time_us, exit_code,
                                user_time_us, sys_time_us, peak_rss_kb,
                                io_read_bytes, io_write_bytes),
        "update_process_exit");
    ++event_count_;
    maybe_commit();
}

void TraceSession::on_file_access(const FileAccessRecord& rec) {
    if (has_fatal_errors()) return;
    if (!in_transaction_) {
        db_.begin_transaction();
        in_transaction_ = true;
    }
    check_write_result(db_.insert_file_access(rec), "insert_file_access");
    ++file_access_count_;
    ++event_count_;
    maybe_commit();
}

void TraceSession::on_failed_access(const FailedAccessRecord& rec) {
    if (has_fatal_errors()) return;
    if (!in_transaction_) {
        db_.begin_transaction();
        in_transaction_ = true;
    }
    check_write_result(db_.insert_failed_access(rec), "insert_failed_access");
    ++failed_access_count_;
    ++event_count_;
    maybe_commit();
}

void TraceSession::delete_process(int pid) {
    if (has_fatal_errors()) return;
    if (!in_transaction_) {
        db_.begin_transaction();
        in_transaction_ = true;
    }
    check_write_result(db_.delete_process(pid), "delete_process");
    ++event_count_;
    maybe_commit();
}

void TraceSession::finalize() {
    if (in_transaction_) {
        db_.commit_transaction();
        in_transaction_ = false;
    }
    if (!finalized_) {
        finalized_ = true;
        db_.populate_counts();
        db_.analyze();
    }
    if (write_error_count_ > 0) {
        LOG_WARN("Trace completed with %d DB write errors", write_error_count_);
    }
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
