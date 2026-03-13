#ifndef BDTRACE_DATABASE_H
#define BDTRACE_DATABASE_H

#include <string>
#include <vector>
#include <map>
#include <set>
#include <stdint.h>
#include "../common/types.h"

struct sqlite3;
struct sqlite3_stmt;

namespace bdtrace {

// Aggregated file statistics (result of GROUP BY filename)
struct FileStatRow {
    std::string filename;
    int access_count;
    int read_count;
    int write_count;
    int process_count;
    FileStatRow() : access_count(0), read_count(0), write_count(0), process_count(0) {}
};

class Database {
public:
    Database();
    ~Database();

    bool open(const std::string& path);
    void close();
    bool init_schema();

    // Schema upgrade
    bool upgrade_schema();

    // Write operations
    bool insert_meta(const std::string& key, const std::string& value);
    bool insert_process(const ProcessRecord& rec);
    bool update_process_exit(int pid, int64_t end_time_us, int exit_code,
                             int64_t user_time_us = 0, int64_t sys_time_us = 0,
                             int64_t peak_rss_kb = 0,
                             int64_t io_read_bytes = 0, int64_t io_write_bytes = 0);
    bool insert_file_access(const FileAccessRecord& rec);
    bool insert_failed_access(const FailedAccessRecord& rec);
    bool delete_process(int pid);

    // Transaction control
    bool begin_transaction();
    bool commit_transaction();

    // Read operations (for bdview)
    bool get_all_processes(std::vector<ProcessRecord>& out);
    bool get_children(int ppid, std::vector<ProcessRecord>& out);
    bool get_process(int pid, ProcessRecord& out);
    bool get_file_accesses_by_pid(int pid, std::vector<FileAccessRecord>& out);
    bool get_file_accesses_by_name(const std::string& filename, std::vector<FileAccessRecord>& out);
    bool get_all_file_accesses(std::vector<FileAccessRecord>& out);
    bool get_all_failed_accesses(std::vector<FailedAccessRecord>& out);
    bool get_root_processes(std::vector<ProcessRecord>& out);
    bool get_pids_with_children(std::set<int>& out);
    bool get_descendant_pids(int root_pid, std::set<int>& out);
    bool populate_counts();
    bool get_file_count_by_pid(std::map<int, int>& out);
    bool get_failed_count_by_pid(std::map<int, int>& out);
    bool get_file_access_summary(std::map<int, int>& mode_counts);
    bool get_file_accesses_by_prefix(const std::string& prefix, std::vector<FileAccessRecord>& out);
    bool get_file_accesses_by_pids(const std::set<int>& pids, std::vector<FileAccessRecord>& out);
    bool get_processes_by_pids(const std::set<int>& pids, std::map<int, ProcessRecord>& out);
    bool get_file_stats_grouped(std::vector<FileStatRow>& out);
    bool get_hotfile_stats(int min_procs, int& file_count,
                           std::string& worst_file, int& worst_procs);

    // Lightweight: callback receives (pid, filename, mode) for each row in range
    // Returns number of rows processed, or -1 on error
    typedef void (*FileScanCallback)(int pid, const char* filename, int mode, void* user_data);
    int scan_file_accesses_by_prefix(const std::string& prefix, FileScanCallback cb, void* user_data);
    int scan_all_file_accesses(FileScanCallback cb, void* user_data);
    int get_process_count();
    int get_file_access_count();
    int get_failed_access_count();
    bool has_table(const std::string& table_name);

    const std::string& last_error() const { return last_error_; }

    // WAL checkpoint (P1.2)
    bool wal_checkpoint();

    // Run ANALYZE for optimizer statistics (P1.5)
    bool analyze();

    // Get database file size in bytes
    int64_t get_db_size_bytes();

    // Raw SQL execution (public for special cases)
    bool exec_raw(const std::string& sql);

private:
    bool exec(const char* sql);
    bool prepare(const char* sql, sqlite3_stmt** stmt);

    sqlite3* db_;
    std::string last_error_;

    // Prepared statements for inserts
    sqlite3_stmt* stmt_insert_process_;
    sqlite3_stmt* stmt_update_exit_;
    sqlite3_stmt* stmt_insert_file_;
    sqlite3_stmt* stmt_insert_meta_;
    sqlite3_stmt* stmt_insert_failed_;
    sqlite3_stmt* stmt_delete_process_;

    void finalize_stmts();
    bool prepare_stmts();
};

} // namespace bdtrace

#endif // BDTRACE_DATABASE_H
