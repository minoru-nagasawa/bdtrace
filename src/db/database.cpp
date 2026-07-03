#include "database.h"
#include "schema.h"
#include "../common/log.h"
#include "../../vendor/sqlite3.h"

#include <cstring>
#include <cstdio>
#include <map>

namespace bdtrace {

Database::Database()
    : db_(0)
    , stmt_insert_process_(0)
    , stmt_update_exit_(0)
    , stmt_insert_file_(0)
    , stmt_insert_meta_(0)
    , stmt_insert_failed_(0)
    , stmt_delete_process_(0)
    , stmt_intern_insert_(0)
    , stmt_intern_select_(0)
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
    // 16 MB page cache: cheap even on old boxes, speeds up bulk insert and
    // the deferred index build in finalize
    exec("PRAGMA cache_size=-16000");

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
    // Re-tracing into a pre-v6 DB: convert to interned filenames before the
    // insert statements (which reference file_id) are prepared.
    if (!migrate_to_interned()) return false;
    if (!prepare_stmts()) return false;
    // Set schema version if not set (needs stmt_insert_meta_, so after prepare)
    insert_meta("schema_version", "6");
    return true;
}

bool Database::has_column(const char* table, const char* column) {
    char sql[256];
    std::snprintf(sql, sizeof(sql), "SELECT %s FROM %s LIMIT 0", column, table);
    sqlite3_stmt* stmt = 0;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, 0) != SQLITE_OK) return false;
    sqlite3_finalize(stmt);
    return true;
}

bool Database::migrate_to_interned() {
    bool fa_old = has_table("file_accesses") && has_column("file_accesses", "filename");
    bool failed_old = has_table("failed_accesses") && has_column("failed_accesses", "filename");
    if (!fa_old && !failed_old) return true;

    LOG_INFO("Migrating database to interned filenames (schema v6)...");
    if (!exec("BEGIN TRANSACTION")) return false;
    bool ok = exec("CREATE TABLE IF NOT EXISTS files ("
                   "id INTEGER PRIMARY KEY, path TEXT UNIQUE NOT NULL)");
    if (ok && fa_old) {
        ok = exec("INSERT OR IGNORE INTO files(path) "
                  "SELECT DISTINCT filename FROM file_accesses")
          && exec("CREATE TABLE file_accesses_v6 ("
                  "id INTEGER PRIMARY KEY, pid INTEGER NOT NULL,"
                  "file_id INTEGER NOT NULL, mode INTEGER NOT NULL,"
                  "fd INTEGER, timestamp_us INTEGER NOT NULL DEFAULT 0)")
          && exec("INSERT INTO file_accesses_v6 (id, pid, file_id, mode, fd, timestamp_us) "
                  "SELECT a.id, a.pid, f.id, a.mode, a.fd, a.timestamp_us "
                  "FROM file_accesses a JOIN files f ON f.path = a.filename")
          && exec("DROP TABLE file_accesses")
          && exec("ALTER TABLE file_accesses_v6 RENAME TO file_accesses");
    }
    if (ok && failed_old) {
        ok = exec("INSERT OR IGNORE INTO files(path) "
                  "SELECT DISTINCT filename FROM failed_accesses")
          && exec("CREATE TABLE failed_accesses_v6 ("
                  "id INTEGER PRIMARY KEY, pid INTEGER NOT NULL,"
                  "file_id INTEGER NOT NULL, mode INTEGER NOT NULL,"
                  "errno_val INTEGER NOT NULL, timestamp_us INTEGER NOT NULL DEFAULT 0)")
          && exec("INSERT INTO failed_accesses_v6 (id, pid, file_id, mode, errno_val, timestamp_us) "
                  "SELECT a.id, a.pid, f.id, a.mode, a.errno_val, a.timestamp_us "
                  "FROM failed_accesses a JOIN files f ON f.path = a.filename")
          && exec("DROP TABLE failed_accesses")
          && exec("ALTER TABLE failed_accesses_v6 RENAME TO failed_accesses");
    }
    if (!ok) {
        exec("ROLLBACK");
        LOG_ERROR("Schema v6 migration failed: %s", last_error_.c_str());
        return false;
    }
    return exec("COMMIT");
}

bool Database::create_indexes() {
    return exec(get_index_sql());
}

bool Database::upgrade_schema() {
    // Check if failed_accesses table exists; if not, upgrade from v1
    if (!has_table("failed_accesses")) {
        LOG_INFO("Upgrading schema to v2...");
        // Check if timestamp_us column exists by trying a query
        sqlite3_stmt* stmt = 0;
        int rc = sqlite3_prepare_v2(db_, "SELECT timestamp_us FROM file_accesses LIMIT 0", -1, &stmt, 0);
        if (rc != SQLITE_OK) {
            // Column doesn't exist, add it
            exec("ALTER TABLE file_accesses ADD COLUMN timestamp_us INTEGER NOT NULL DEFAULT 0");
        } else {
            sqlite3_finalize(stmt);
        }
        // Create failed_accesses table
        exec("CREATE TABLE IF NOT EXISTS failed_accesses ("
             "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "  pid INTEGER NOT NULL,"
             "  filename TEXT NOT NULL,"
             "  mode INTEGER NOT NULL,"
             "  errno_val INTEGER NOT NULL,"
             "  timestamp_us INTEGER NOT NULL DEFAULT 0"
             ")");
        exec("CREATE INDEX IF NOT EXISTS idx_failed_pid ON failed_accesses(pid)");
    }

    // Check if resource columns exist; if not, upgrade to v3
    {
        sqlite3_stmt* stmt = 0;
        int rc = sqlite3_prepare_v2(db_, "SELECT user_time_us FROM processes LIMIT 0", -1, &stmt, 0);
        if (rc != SQLITE_OK) {
            LOG_INFO("Upgrading schema to v3 (resource tracking)...");
            exec("ALTER TABLE processes ADD COLUMN user_time_us INTEGER DEFAULT 0");
            exec("ALTER TABLE processes ADD COLUMN sys_time_us INTEGER DEFAULT 0");
            exec("ALTER TABLE processes ADD COLUMN peak_rss_kb INTEGER DEFAULT 0");
            exec("ALTER TABLE processes ADD COLUMN io_read_bytes INTEGER DEFAULT 0");
            exec("ALTER TABLE processes ADD COLUMN io_write_bytes INTEGER DEFAULT 0");
        } else {
            sqlite3_finalize(stmt);
        }
    }

    // Check if cwd column exists; if not, upgrade to v4
    {
        sqlite3_stmt* stmt = 0;
        int rc = sqlite3_prepare_v2(db_, "SELECT cwd FROM processes LIMIT 0", -1, &stmt, 0);
        if (rc != SQLITE_OK) {
            LOG_INFO("Upgrading schema to v4 (cwd tracking)...");
            exec("ALTER TABLE processes ADD COLUMN cwd TEXT DEFAULT ''");
        } else {
            sqlite3_finalize(stmt);
        }
    }

    // Check if file_count column exists; if not, upgrade to v5
    {
        sqlite3_stmt* stmt = 0;
        int rc = sqlite3_prepare_v2(db_, "SELECT file_count FROM processes LIMIT 0", -1, &stmt, 0);
        if (rc != SQLITE_OK) {
            LOG_INFO("Upgrading schema to v5 (denormalized counts)...");
            exec("ALTER TABLE processes ADD COLUMN file_count INTEGER DEFAULT 0");
            exec("ALTER TABLE processes ADD COLUMN fail_count INTEGER DEFAULT 0");
            populate_counts();
        } else {
            sqlite3_finalize(stmt);
        }
    }

    // v6: interned filenames (must precede index creation, which references
    // the file_id columns)
    if (!migrate_to_interned()) return false;

    // Ensure all indexes exist (the tracer defers index creation to finalize,
    // so a DB from an interrupted trace arrives here without them)
    create_indexes();

    // P2.6: Re-populate counts if all are zero (interrupted trace recovery)
    {
        sqlite3_stmt* stmt = 0;
        int rc = sqlite3_prepare_v2(db_,
            "SELECT COUNT(*) FROM processes WHERE file_count > 0", -1, &stmt, 0);
        if (rc == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 0) {
                int fa_count = get_file_access_count();
                if (fa_count > 0) {
                    LOG_INFO("Detected interrupted trace (counts are zero), re-populating...");
                    sqlite3_finalize(stmt);
                    populate_counts();
                    analyze();
                    return true;
                }
            }
            sqlite3_finalize(stmt);
        }
    }

    return true;
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

bool Database::wal_checkpoint() {
    return exec("PRAGMA wal_checkpoint(PASSIVE)");
}

bool Database::analyze() {
    return exec("ANALYZE");
}

int64_t Database::get_db_size_bytes() {
    int64_t page_count = 0, page_size = 0;
    sqlite3_stmt* stmt = 0;
    if (prepare("PRAGMA page_count", &stmt)) {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            page_count = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }
    if (prepare("PRAGMA page_size", &stmt)) {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            page_size = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return page_count * page_size;
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
    if (!prepare("INSERT INTO processes (pid, ppid, cmdline, start_time_us, end_time_us, exit_code, cwd) VALUES (?, ?, ?, ?, ?, ?, ?)", &stmt_insert_process_))
        return false;
    if (!prepare("UPDATE processes SET end_time_us = ?, exit_code = ?, user_time_us = ?, sys_time_us = ?, peak_rss_kb = ?, io_read_bytes = ?, io_write_bytes = ? WHERE pid = ?", &stmt_update_exit_))
        return false;
    if (!prepare("INSERT INTO file_accesses (pid, file_id, mode, fd, timestamp_us) VALUES (?, ?, ?, ?, ?)", &stmt_insert_file_))
        return false;
    if (!prepare("INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)", &stmt_insert_meta_))
        return false;
    if (!prepare("INSERT INTO failed_accesses (pid, file_id, mode, errno_val, timestamp_us) VALUES (?, ?, ?, ?, ?)", &stmt_insert_failed_))
        return false;
    if (!prepare("DELETE FROM processes WHERE pid = ?", &stmt_delete_process_))
        return false;
    if (!prepare("INSERT OR IGNORE INTO files (path) VALUES (?)", &stmt_intern_insert_))
        return false;
    if (!prepare("SELECT id FROM files WHERE path = ?", &stmt_intern_select_))
        return false;
    return true;
}

int64_t Database::intern_path(const std::string& path, bool& ok) {
    std::map<std::string, int64_t>::iterator it = intern_cache_.find(path);
    if (it != intern_cache_.end()) {
        ok = true;
        return it->second;
    }
    sqlite3_reset(stmt_intern_insert_);
    sqlite3_bind_text(stmt_intern_insert_, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt_intern_insert_) != SQLITE_DONE) {
        last_error_ = sqlite3_errmsg(db_);
        ok = false;
        return 0;
    }
    int64_t id;
    if (sqlite3_changes(db_) > 0) {
        id = sqlite3_last_insert_rowid(db_);
    } else {
        // Row already existed (e.g. cache lost across sessions): look it up
        sqlite3_reset(stmt_intern_select_);
        sqlite3_bind_text(stmt_intern_select_, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt_intern_select_) != SQLITE_ROW) {
            last_error_ = sqlite3_errmsg(db_);
            ok = false;
            return 0;
        }
        id = sqlite3_column_int64(stmt_intern_select_, 0);
        sqlite3_reset(stmt_intern_select_);
    }
    intern_cache_[path] = id;
    ok = true;
    return id;
}

void Database::finalize_stmts() {
    if (stmt_insert_process_) { sqlite3_finalize(stmt_insert_process_); stmt_insert_process_ = 0; }
    if (stmt_update_exit_)    { sqlite3_finalize(stmt_update_exit_);    stmt_update_exit_ = 0; }
    if (stmt_insert_file_)    { sqlite3_finalize(stmt_insert_file_);    stmt_insert_file_ = 0; }
    if (stmt_insert_meta_)    { sqlite3_finalize(stmt_insert_meta_);    stmt_insert_meta_ = 0; }
    if (stmt_insert_failed_)  { sqlite3_finalize(stmt_insert_failed_);  stmt_insert_failed_ = 0; }
    if (stmt_delete_process_) { sqlite3_finalize(stmt_delete_process_); stmt_delete_process_ = 0; }
    if (stmt_intern_insert_)  { sqlite3_finalize(stmt_intern_insert_);  stmt_intern_insert_ = 0; }
    if (stmt_intern_select_)  { sqlite3_finalize(stmt_intern_select_);  stmt_intern_select_ = 0; }
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
    sqlite3_bind_text(stmt_insert_process_, 7, rec.cwd.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt_insert_process_);
    if (rc != SQLITE_DONE) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

bool Database::update_process_exit(int pid, int64_t end_time_us, int exit_code,
                                    int64_t user_time_us, int64_t sys_time_us,
                                    int64_t peak_rss_kb,
                                    int64_t io_read_bytes, int64_t io_write_bytes) {
    sqlite3_reset(stmt_update_exit_);
    sqlite3_bind_int64(stmt_update_exit_, 1, end_time_us);
    sqlite3_bind_int(stmt_update_exit_, 2, exit_code);
    sqlite3_bind_int64(stmt_update_exit_, 3, user_time_us);
    sqlite3_bind_int64(stmt_update_exit_, 4, sys_time_us);
    sqlite3_bind_int64(stmt_update_exit_, 5, peak_rss_kb);
    sqlite3_bind_int64(stmt_update_exit_, 6, io_read_bytes);
    sqlite3_bind_int64(stmt_update_exit_, 7, io_write_bytes);
    sqlite3_bind_int(stmt_update_exit_, 8, pid);
    int rc = sqlite3_step(stmt_update_exit_);
    if (rc != SQLITE_DONE) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

bool Database::insert_file_access(const FileAccessRecord& rec) {
    bool ok = false;
    int64_t file_id = intern_path(rec.filename, ok);
    if (!ok) return false;
    sqlite3_reset(stmt_insert_file_);
    sqlite3_bind_int(stmt_insert_file_, 1, rec.pid);
    sqlite3_bind_int64(stmt_insert_file_, 2, file_id);
    sqlite3_bind_int(stmt_insert_file_, 3, rec.mode);
    sqlite3_bind_int(stmt_insert_file_, 4, rec.fd);
    sqlite3_bind_int64(stmt_insert_file_, 5, rec.timestamp_us);
    int rc = sqlite3_step(stmt_insert_file_);
    if (rc != SQLITE_DONE) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

bool Database::delete_process(int pid) {
    sqlite3_reset(stmt_delete_process_);
    sqlite3_bind_int(stmt_delete_process_, 1, pid);
    int rc = sqlite3_step(stmt_delete_process_);
    if (rc != SQLITE_DONE) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

bool Database::insert_failed_access(const FailedAccessRecord& rec) {
    bool ok = false;
    int64_t file_id = intern_path(rec.filename, ok);
    if (!ok) return false;
    sqlite3_reset(stmt_insert_failed_);
    sqlite3_bind_int(stmt_insert_failed_, 1, rec.pid);
    sqlite3_bind_int64(stmt_insert_failed_, 2, file_id);
    sqlite3_bind_int(stmt_insert_failed_, 3, rec.mode);
    sqlite3_bind_int(stmt_insert_failed_, 4, rec.errno_val);
    sqlite3_bind_int64(stmt_insert_failed_, 5, rec.timestamp_us);
    int rc = sqlite3_step(stmt_insert_failed_);
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
    int col_count = sqlite3_column_count(stmt);
    if (col_count > 6) {
        r.user_time_us = sqlite3_column_int64(stmt, 6);
        r.sys_time_us = sqlite3_column_int64(stmt, 7);
        r.peak_rss_kb = sqlite3_column_int64(stmt, 8);
        r.io_read_bytes = sqlite3_column_int64(stmt, 9);
        r.io_write_bytes = sqlite3_column_int64(stmt, 10);
    }
    if (col_count > 11) {
        const char* cwd = (const char*)sqlite3_column_text(stmt, 11);
        if (cwd) r.cwd = cwd;
    }
    if (col_count > 13) {
        r.file_count = sqlite3_column_int(stmt, 12);
        r.fail_count = sqlite3_column_int(stmt, 13);
    }
    return r;
}

bool Database::get_all_processes(std::vector<ProcessRecord>& out) {
    sqlite3_stmt* stmt = 0;
    if (!prepare("SELECT pid, ppid, cmdline, start_time_us, end_time_us, exit_code, user_time_us, sys_time_us, peak_rss_kb, io_read_bytes, io_write_bytes, cwd, file_count, fail_count FROM processes ORDER BY start_time_us", &stmt))
        return false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back(row_to_process(stmt));
    }
    sqlite3_finalize(stmt);
    return true;
}

bool Database::get_children(int ppid, std::vector<ProcessRecord>& out) {
    sqlite3_stmt* stmt = 0;
    if (!prepare("SELECT pid, ppid, cmdline, start_time_us, end_time_us, exit_code, user_time_us, sys_time_us, peak_rss_kb, io_read_bytes, io_write_bytes, cwd, file_count, fail_count FROM processes WHERE ppid = ? ORDER BY start_time_us", &stmt))
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
    if (!prepare("SELECT pid, ppid, cmdline, start_time_us, end_time_us, exit_code, user_time_us, sys_time_us, peak_rss_kb, io_read_bytes, io_write_bytes, cwd, file_count, fail_count FROM processes WHERE pid = ?", &stmt))
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

bool Database::get_root_processes(std::vector<ProcessRecord>& out) {
    sqlite3_stmt* stmt = 0;
    if (!prepare("SELECT pid, ppid, cmdline, start_time_us, end_time_us, exit_code, "
                 "user_time_us, sys_time_us, peak_rss_kb, io_read_bytes, io_write_bytes, "
                 "cwd, file_count, fail_count FROM processes "
                 "WHERE ppid NOT IN (SELECT pid FROM processes) "
                 "ORDER BY start_time_us", &stmt))
        return false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back(row_to_process(stmt));
    }
    sqlite3_finalize(stmt);
    return true;
}

bool Database::get_pids_with_children(std::set<int>& out) {
    sqlite3_stmt* stmt = 0;
    if (!prepare("SELECT DISTINCT ppid FROM processes", &stmt))
        return false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.insert(sqlite3_column_int(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return true;
}

bool Database::get_descendant_pids(int root_pid, std::set<int>& out) {
    sqlite3_stmt* stmt = 0;
    if (!prepare(
            "WITH RECURSIVE descendants(pid) AS ("
            "  SELECT pid FROM processes WHERE ppid = ?"
            "  UNION ALL"
            "  SELECT p.pid FROM processes p"
            "    JOIN descendants d ON p.ppid = d.pid"
            ") SELECT pid FROM descendants", &stmt))
        return false;
    sqlite3_bind_int(stmt, 1, root_pid);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.insert(sqlite3_column_int(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return true;
}

bool Database::populate_counts() {
    if (!db_) return false;
    // P3.6: Use GROUP BY + batch UPDATE instead of correlated subqueries
    // Reset all to 0 first
    if (!exec("UPDATE processes SET file_count = 0, fail_count = 0"))
        return false;
    // Batch update file_count using a JOIN pattern
    if (!exec("UPDATE processes SET file_count = COALESCE("
              "(SELECT cnt FROM (SELECT pid, COUNT(*) as cnt FROM file_accesses GROUP BY pid) fa "
              "WHERE fa.pid = processes.pid), 0)"))
        return false;
    // Batch update fail_count
    if (has_table("failed_accesses")) {
        if (!exec("UPDATE processes SET fail_count = COALESCE("
                  "(SELECT cnt FROM (SELECT pid, COUNT(*) as cnt FROM failed_accesses GROUP BY pid) fa "
                  "WHERE fa.pid = processes.pid), 0)"))
            return false;
    }
    return true;
}

static FileAccessRecord row_to_file_access(sqlite3_stmt* stmt) {
    FileAccessRecord r;
    r.pid = sqlite3_column_int(stmt, 0);
    const char* fn = (const char*)sqlite3_column_text(stmt, 1);
    if (fn) r.filename = fn;
    r.mode = static_cast<FileAccessMode>(sqlite3_column_int(stmt, 2));
    r.fd = sqlite3_column_int(stmt, 3);
    r.timestamp_us = sqlite3_column_int64(stmt, 4);
    return r;
}

bool Database::get_file_accesses_by_pid(int pid, std::vector<FileAccessRecord>& out) {
    sqlite3_stmt* stmt = 0;
    if (!prepare("SELECT a.pid, f.path, a.mode, a.fd, a.timestamp_us "
                 "FROM file_accesses a JOIN files f ON f.id = a.file_id "
                 "WHERE a.pid = ?", &stmt))
        return false;
    sqlite3_bind_int(stmt, 1, pid);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back(row_to_file_access(stmt));
    }
    sqlite3_finalize(stmt);
    return true;
}

bool Database::get_file_accesses_by_name(const std::string& filename, std::vector<FileAccessRecord>& out) {
    sqlite3_stmt* stmt = 0;
    if (!prepare("SELECT a.pid, f.path, a.mode, a.fd, a.timestamp_us "
                 "FROM file_accesses a JOIN files f ON f.id = a.file_id "
                 "WHERE a.file_id = (SELECT id FROM files WHERE path = ?)", &stmt))
        return false;
    sqlite3_bind_text(stmt, 1, filename.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back(row_to_file_access(stmt));
    }
    sqlite3_finalize(stmt);
    return true;
}

bool Database::get_all_file_accesses(std::vector<FileAccessRecord>& out) {
    sqlite3_stmt* stmt = 0;
    if (!prepare("SELECT a.pid, f.path, a.mode, a.fd, a.timestamp_us "
                 "FROM file_accesses a JOIN files f ON f.id = a.file_id", &stmt))
        return false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back(row_to_file_access(stmt));
    }
    sqlite3_finalize(stmt);
    return true;
}

bool Database::get_all_failed_accesses(std::vector<FailedAccessRecord>& out) {
    if (!has_table("failed_accesses")) return true;
    sqlite3_stmt* stmt = 0;
    if (!prepare("SELECT a.pid, f.path, a.mode, a.errno_val, a.timestamp_us "
                 "FROM failed_accesses a JOIN files f ON f.id = a.file_id", &stmt))
        return false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FailedAccessRecord r;
        r.pid = sqlite3_column_int(stmt, 0);
        const char* fn = (const char*)sqlite3_column_text(stmt, 1);
        if (fn) r.filename = fn;
        r.mode = static_cast<FileAccessMode>(sqlite3_column_int(stmt, 2));
        r.errno_val = sqlite3_column_int(stmt, 3);
        r.timestamp_us = sqlite3_column_int64(stmt, 4);
        out.push_back(r);
    }
    sqlite3_finalize(stmt);
    return true;
}

bool Database::get_file_count_by_pid(std::map<int, int>& out) {
    sqlite3_stmt* stmt = 0;
    if (!prepare("SELECT pid, COUNT(*) FROM file_accesses GROUP BY pid", &stmt))
        return false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out[sqlite3_column_int(stmt, 0)] = sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);
    return true;
}

bool Database::get_failed_count_by_pid(std::map<int, int>& out) {
    sqlite3_stmt* stmt = 0;
    if (!prepare("SELECT pid, COUNT(*) FROM failed_accesses GROUP BY pid", &stmt))
        return false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out[sqlite3_column_int(stmt, 0)] = sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);
    return true;
}

bool Database::get_file_access_summary(std::map<int, int>& mode_counts) {
    sqlite3_stmt* stmt = 0;
    if (!prepare("SELECT mode, COUNT(*) FROM file_accesses GROUP BY mode", &stmt))
        return false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        mode_counts[sqlite3_column_int(stmt, 0)] = sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);
    return true;
}

bool Database::get_file_accesses_by_prefix(const std::string& prefix, std::vector<FileAccessRecord>& out) {
    std::string pfx = prefix;
    if (!pfx.empty() && pfx[pfx.size() - 1] != '/') pfx += '/';
    // Calculate upper bound for range query: increment last char
    std::string upper = pfx;
    if (!upper.empty()) {
        upper[upper.size() - 1] = upper[upper.size() - 1] + 1;
    }
    sqlite3_stmt* stmt = 0;
    if (!prepare("SELECT a.pid, f.path, a.mode, a.fd, a.timestamp_us "
                 "FROM file_accesses a JOIN files f ON f.id = a.file_id "
                 "WHERE f.path >= ? AND f.path < ?", &stmt))
        return false;
    sqlite3_bind_text(stmt, 1, pfx.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, upper.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FileAccessRecord r;
        r.pid = sqlite3_column_int(stmt, 0);
        r.filename = (const char*)sqlite3_column_text(stmt, 1);
        r.mode = static_cast<FileAccessMode>(sqlite3_column_int(stmt, 2));
        r.fd = sqlite3_column_int(stmt, 3);
        r.timestamp_us = sqlite3_column_int64(stmt, 4);
        out.push_back(r);
    }
    sqlite3_finalize(stmt);
    return true;
}

int Database::scan_file_accesses_by_prefix(const std::string& prefix,
                                           FileScanCallback cb, void* user_data) {
    std::string pfx = prefix;
    if (!pfx.empty() && pfx[pfx.size() - 1] != '/') pfx += '/';
    std::string upper = pfx;
    if (!upper.empty()) {
        upper[upper.size() - 1] = upper[upper.size() - 1] + 1;
    }
    sqlite3_stmt* stmt = 0;
    if (!prepare("SELECT a.pid, f.path, a.mode "
                 "FROM file_accesses a JOIN files f ON f.id = a.file_id "
                 "WHERE f.path >= ? AND f.path < ?", &stmt))
        return -1;
    sqlite3_bind_text(stmt, 1, pfx.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, upper.c_str(), -1, SQLITE_TRANSIENT);
    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int pid = sqlite3_column_int(stmt, 0);
        const char* fn = (const char*)sqlite3_column_text(stmt, 1);
        int mode = sqlite3_column_int(stmt, 2);
        cb(pid, fn, mode, user_data);
        ++count;
    }
    sqlite3_finalize(stmt);
    return count;
}

int Database::scan_all_file_accesses(FileScanCallback cb, void* user_data) {
    sqlite3_stmt* stmt = 0;
    if (!prepare("SELECT a.pid, f.path, a.mode "
                 "FROM file_accesses a JOIN files f ON f.id = a.file_id", &stmt))
        return -1;
    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int pid = sqlite3_column_int(stmt, 0);
        const char* fn = (const char*)sqlite3_column_text(stmt, 1);
        int mode = sqlite3_column_int(stmt, 2);
        cb(pid, fn, mode, user_data);
        ++count;
    }
    sqlite3_finalize(stmt);
    return count;
}

bool Database::get_file_stats_grouped(std::vector<FileStatRow>& out) {
    sqlite3_stmt* stmt = 0;
    if (!prepare(
            "SELECT f.path, COUNT(*), "
            "SUM(CASE WHEN a.mode IN (0,2,3,4,5,9) THEN 1 ELSE 0 END), "
            "SUM(CASE WHEN a.mode IN (1,2,8,10,12,14,17,18) THEN 1 ELSE 0 END), "
            "COUNT(DISTINCT a.pid) "
            "FROM file_accesses a JOIN files f ON f.id = a.file_id "
            "GROUP BY a.file_id", &stmt))
        return false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FileStatRow r;
        const char* fn = (const char*)sqlite3_column_text(stmt, 0);
        if (fn) r.filename = fn;
        r.access_count = sqlite3_column_int(stmt, 1);
        r.read_count = sqlite3_column_int(stmt, 2);
        r.write_count = sqlite3_column_int(stmt, 3);
        r.process_count = sqlite3_column_int(stmt, 4);
        out.push_back(r);
    }
    sqlite3_finalize(stmt);
    return true;
}

bool Database::get_file_accesses_by_pids(const std::set<int>& pids,
                                          std::vector<FileAccessRecord>& out) {
    if (pids.empty()) return true;
    std::string sql = "SELECT a.pid, f.path, a.mode, a.fd, a.timestamp_us "
                      "FROM file_accesses a JOIN files f ON f.id = a.file_id "
                      "WHERE a.pid IN (";
    bool first = true;
    for (std::set<int>::const_iterator it = pids.begin(); it != pids.end(); ++it) {
        if (!first) sql += ",";
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", *it);
        sql += buf;
        first = false;
    }
    sql += ")";
    sqlite3_stmt* stmt = 0;
    if (!prepare(sql.c_str(), &stmt)) return false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back(row_to_file_access(stmt));
    }
    sqlite3_finalize(stmt);
    return true;
}

bool Database::get_processes_by_pids(const std::set<int>& pids,
                                      std::map<int, ProcessRecord>& out) {
    if (pids.empty()) return true;
    std::string sql = "SELECT pid, ppid, cmdline, start_time_us, end_time_us, exit_code, "
                      "user_time_us, sys_time_us, peak_rss_kb, io_read_bytes, io_write_bytes, "
                      "cwd, file_count, fail_count FROM processes WHERE pid IN (";
    bool first = true;
    for (std::set<int>::const_iterator it = pids.begin(); it != pids.end(); ++it) {
        if (!first) sql += ",";
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", *it);
        sql += buf;
        first = false;
    }
    sql += ")";
    sqlite3_stmt* stmt = 0;
    if (!prepare(sql.c_str(), &stmt)) return false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ProcessRecord r = row_to_process(stmt);
        out[r.pid] = r;
    }
    sqlite3_finalize(stmt);
    return true;
}

bool Database::get_hotfile_stats(int min_procs, int& file_count,
                                  std::string& worst_file, int& worst_procs) {
    file_count = 0;
    worst_procs = 0;
    worst_file.clear();
    sqlite3_stmt* stmt = 0;
    if (!prepare(
            "SELECT f.path, COUNT(DISTINCT a.pid) as num_procs "
            "FROM file_accesses a JOIN files f ON f.id = a.file_id "
            "WHERE a.mode IN (0,2,3,4,5,9) "
            "GROUP BY a.file_id "
            "HAVING COUNT(DISTINCT a.pid) > ? "
            "ORDER BY num_procs DESC", &stmt))
        return false;
    sqlite3_bind_int(stmt, 1, min_procs);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (file_count == 0) {
            const char* fn = (const char*)sqlite3_column_text(stmt, 0);
            if (fn) worst_file = fn;
            worst_procs = sqlite3_column_int(stmt, 1);
        }
        ++file_count;
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

int Database::get_failed_access_count() {
    if (!has_table("failed_accesses")) return 0;
    sqlite3_stmt* stmt = 0;
    if (!prepare("SELECT COUNT(*) FROM failed_accesses", &stmt)) return -1;
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

bool Database::has_table(const std::string& table_name) {
    sqlite3_stmt* stmt = 0;
    if (!prepare("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=?", &stmt))
        return false;
    sqlite3_bind_text(stmt, 1, table_name.c_str(), -1, SQLITE_TRANSIENT);
    bool exists = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        exists = sqlite3_column_int(stmt, 0) > 0;
    }
    sqlite3_finalize(stmt);
    return exists;
}

} // namespace bdtrace
