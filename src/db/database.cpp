#include "database.h"
#include "schema.h"
#include "../common/log.h"
#include "../../vendor/sqlite3.h"

#include <cstring>

namespace bdtrace {

Database::Database()
    : db_(0)
    , stmt_insert_process_(0)
    , stmt_update_exit_(0)
    , stmt_insert_file_(0)
    , stmt_insert_meta_(0)
{}

Database::~Database() {
    close();
}

bool Database::open(const std::string& path) {
    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        LOG_ERROR("Failed to open database %s: %s", path.c_str(), last_error_.c_str());
        return false;
    }

    // Enable WAL mode
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=NORMAL");

    return true;
}

void Database::close() {
    finalize_stmts();
    if (db_) {
        sqlite3_close(db_);
        db_ = 0;
    }
}

bool Database::init_schema() {
    if (!exec(get_schema_sql())) return false;
    return prepare_stmts();
}

bool Database::exec(const char* sql) {
    char* err = 0;
    int rc = sqlite3_exec(db_, sql, 0, 0, &err);
    if (rc != SQLITE_OK) {
        last_error_ = err ? err : "unknown error";
        LOG_ERROR("SQL exec failed: %s", last_error_.c_str());
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

bool Database::exec_raw(const std::string& sql) {
    return exec(sql.c_str());
}

bool Database::prepare(const char* sql, sqlite3_stmt** stmt) {
    int rc = sqlite3_prepare_v2(db_, sql, -1, stmt, 0);
    if (rc != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        LOG_ERROR("Failed to prepare: %s", last_error_.c_str());
        return false;
    }
    return true;
}

bool Database::prepare_stmts() {
    if (!prepare("INSERT INTO processes (pid, ppid, cmdline, start_time_us, end_time_us, exit_code) VALUES (?, ?, ?, ?, ?, ?)", &stmt_insert_process_))
        return false;
    if (!prepare("UPDATE processes SET end_time_us = ?, exit_code = ? WHERE pid = ?", &stmt_update_exit_))
        return false;
    if (!prepare("INSERT INTO file_accesses (pid, filename, mode, fd) VALUES (?, ?, ?, ?)", &stmt_insert_file_))
        return false;
    if (!prepare("INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)", &stmt_insert_meta_))
        return false;
    return true;
}

void Database::finalize_stmts() {
    if (stmt_insert_process_) { sqlite3_finalize(stmt_insert_process_); stmt_insert_process_ = 0; }
    if (stmt_update_exit_)    { sqlite3_finalize(stmt_update_exit_);    stmt_update_exit_ = 0; }
    if (stmt_insert_file_)    { sqlite3_finalize(stmt_insert_file_);    stmt_insert_file_ = 0; }
    if (stmt_insert_meta_)    { sqlite3_finalize(stmt_insert_meta_);    stmt_insert_meta_ = 0; }
}

bool Database::insert_meta(const std::string& key, const std::string& value) {
    sqlite3_reset(stmt_insert_meta_);
    sqlite3_bind_text(stmt_insert_meta_, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_meta_, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt_insert_meta_);
    if (rc != SQLITE_DONE) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

bool Database::insert_process(const ProcessRecord& rec) {
    sqlite3_reset(stmt_insert_process_);
    sqlite3_bind_int(stmt_insert_process_, 1, rec.pid);
    sqlite3_bind_int(stmt_insert_process_, 2, rec.ppid);
    sqlite3_bind_text(stmt_insert_process_, 3, rec.cmdline.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_insert_process_, 4, rec.start_time_us);
    sqlite3_bind_int64(stmt_insert_process_, 5, rec.end_time_us);
    sqlite3_bind_int(stmt_insert_process_, 6, rec.exit_code);
    int rc = sqlite3_step(stmt_insert_process_);
    if (rc != SQLITE_DONE) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

bool Database::update_process_exit(int pid, int64_t end_time_us, int exit_code) {
    sqlite3_reset(stmt_update_exit_);
    sqlite3_bind_int64(stmt_update_exit_, 1, end_time_us);
    sqlite3_bind_int(stmt_update_exit_, 2, exit_code);
    sqlite3_bind_int(stmt_update_exit_, 3, pid);
    int rc = sqlite3_step(stmt_update_exit_);
    if (rc != SQLITE_DONE) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

bool Database::insert_file_access(const FileAccessRecord& rec) {
    sqlite3_reset(stmt_insert_file_);
    sqlite3_bind_int(stmt_insert_file_, 1, rec.pid);
    sqlite3_bind_text(stmt_insert_file_, 2, rec.filename.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_insert_file_, 3, rec.mode);
    sqlite3_bind_int(stmt_insert_file_, 4, rec.fd);
    int rc = sqlite3_step(stmt_insert_file_);
    if (rc != SQLITE_DONE) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

bool Database::begin_transaction() {
    return exec("BEGIN TRANSACTION");
}

bool Database::commit_transaction() {
    return exec("COMMIT");
}

// Read operations

static ProcessRecord row_to_process(sqlite3_stmt* stmt) {
    ProcessRecord r;
    r.pid = sqlite3_column_int(stmt, 0);
    r.ppid = sqlite3_column_int(stmt, 1);
    const char* cmd = (const char*)sqlite3_column_text(stmt, 2);
    if (cmd) r.cmdline = cmd;
    r.start_time_us = sqlite3_column_int64(stmt, 3);
    r.end_time_us = sqlite3_column_int64(stmt, 4);
    r.exit_code = sqlite3_column_int(stmt, 5);
    return r;
}

bool Database::get_all_processes(std::vector<ProcessRecord>& out) {
    sqlite3_stmt* stmt = 0;
    if (!prepare("SELECT pid, ppid, cmdline, start_time_us, end_time_us, exit_code FROM processes ORDER BY start_time_us", &stmt))
        return false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back(row_to_process(stmt));
    }
    sqlite3_finalize(stmt);
    return true;
}

bool Database::get_children(int ppid, std::vector<ProcessRecord>& out) {
    sqlite3_stmt* stmt = 0;
    if (!prepare("SELECT pid, ppid, cmdline, start_time_us, end_time_us, exit_code FROM processes WHERE ppid = ? ORDER BY start_time_us", &stmt))
        return false;
    sqlite3_bind_int(stmt, 1, ppid);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back(row_to_process(stmt));
    }
    sqlite3_finalize(stmt);
    return true;
}

bool Database::get_process(int pid, ProcessRecord& out) {
    sqlite3_stmt* stmt = 0;
    if (!prepare("SELECT pid, ppid, cmdline, start_time_us, end_time_us, exit_code FROM processes WHERE pid = ?", &stmt))
        return false;
    sqlite3_bind_int(stmt, 1, pid);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out = row_to_process(stmt);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool Database::get_file_accesses_by_pid(int pid, std::vector<FileAccessRecord>& out) {
    sqlite3_stmt* stmt = 0;
    if (!prepare("SELECT pid, filename, mode, fd FROM file_accesses WHERE pid = ?", &stmt))
        return false;
    sqlite3_bind_int(stmt, 1, pid);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FileAccessRecord r;
        r.pid = sqlite3_column_int(stmt, 0);
        const char* fn = (const char*)sqlite3_column_text(stmt, 1);
        if (fn) r.filename = fn;
        r.mode = static_cast<FileAccessMode>(sqlite3_column_int(stmt, 2));
        r.fd = sqlite3_column_int(stmt, 3);
        out.push_back(r);
    }
    sqlite3_finalize(stmt);
    return true;
}

bool Database::get_file_accesses_by_name(const std::string& filename, std::vector<FileAccessRecord>& out) {
    sqlite3_stmt* stmt = 0;
    if (!prepare("SELECT pid, filename, mode, fd FROM file_accesses WHERE filename = ?", &stmt))
        return false;
    sqlite3_bind_text(stmt, 1, filename.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FileAccessRecord r;
        r.pid = sqlite3_column_int(stmt, 0);
        const char* fn = (const char*)sqlite3_column_text(stmt, 1);
        if (fn) r.filename = fn;
        r.mode = static_cast<FileAccessMode>(sqlite3_column_int(stmt, 2));
        r.fd = sqlite3_column_int(stmt, 3);
        out.push_back(r);
    }
    sqlite3_finalize(stmt);
    return true;
}

bool Database::get_all_file_accesses(std::vector<FileAccessRecord>& out) {
    sqlite3_stmt* stmt = 0;
    if (!prepare("SELECT pid, filename, mode, fd FROM file_accesses", &stmt))
        return false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FileAccessRecord r;
        r.pid = sqlite3_column_int(stmt, 0);
        const char* fn = (const char*)sqlite3_column_text(stmt, 1);
        if (fn) r.filename = fn;
        r.mode = static_cast<FileAccessMode>(sqlite3_column_int(stmt, 2));
        r.fd = sqlite3_column_int(stmt, 3);
        out.push_back(r);
    }
    sqlite3_finalize(stmt);
    return true;
}

int Database::get_process_count() {
    sqlite3_stmt* stmt = 0;
    if (!prepare("SELECT COUNT(*) FROM processes", &stmt)) return -1;
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

int Database::get_file_access_count() {
    sqlite3_stmt* stmt = 0;
    if (!prepare("SELECT COUNT(*) FROM file_accesses", &stmt)) return -1;
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

} // namespace bdtrace
