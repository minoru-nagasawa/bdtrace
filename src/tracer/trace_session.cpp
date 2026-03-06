#include "trace_session.h"
#include "../common/log.h"

namespace bdtrace {

static const int BATCH_SIZE = 1000;

TraceSession::TraceSession()
    : event_count_(0), in_transaction_(false)
{}

TraceSession::~TraceSession() {
    finalize();
}

bool TraceSession::open(const std::string& db_path) {
    if (!db_.open(db_path)) return false;
    if (!db_.init_schema()) return false;
    return true;
}

void TraceSession::on_process_start(const ProcessRecord& rec) {
    if (!in_transaction_) {
        db_.begin_transaction();
        in_transaction_ = true;
    }
    db_.insert_process(rec);
    ++event_count_;
    maybe_commit();
}

void TraceSession::on_process_exit(int pid, int64_t end_time_us, int exit_code) {
    if (!in_transaction_) {
        db_.begin_transaction();
        in_transaction_ = true;
    }
    db_.update_process_exit(pid, end_time_us, exit_code);
    ++event_count_;
    maybe_commit();
}

void TraceSession::on_file_access(const FileAccessRecord& rec) {
    if (!in_transaction_) {
        db_.begin_transaction();
        in_transaction_ = true;
    }
    db_.insert_file_access(rec);
    ++event_count_;
    maybe_commit();
}

void TraceSession::finalize() {
    if (in_transaction_) {
        db_.commit_transaction();
        in_transaction_ = false;
    }
}

void TraceSession::maybe_commit() {
    if (event_count_ >= BATCH_SIZE) {
        db_.commit_transaction();
        in_transaction_ = false;
        event_count_ = 0;
    }
}

} // namespace bdtrace
