#include "schema.h"

namespace bdtrace {

const char* get_schema_sql() {
    return
        "CREATE TABLE IF NOT EXISTS meta ("
        "  key TEXT PRIMARY KEY,"
        "  value TEXT"
        ");"
        "CREATE TABLE IF NOT EXISTS processes ("
        "  pid INTEGER PRIMARY KEY,"
        "  ppid INTEGER,"
        "  cmdline TEXT,"
        "  start_time_us INTEGER,"
        "  end_time_us INTEGER,"
        "  exit_code INTEGER,"
        "  user_time_us INTEGER DEFAULT 0,"
        "  sys_time_us INTEGER DEFAULT 0,"
        "  peak_rss_kb INTEGER DEFAULT 0,"
        "  io_read_bytes INTEGER DEFAULT 0,"
        "  io_write_bytes INTEGER DEFAULT 0,"
        "  cwd TEXT DEFAULT '',"
        "  file_count INTEGER DEFAULT 0,"
        "  fail_count INTEGER DEFAULT 0"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_proc_ppid ON processes(ppid);"
        "CREATE INDEX IF NOT EXISTS idx_proc_start ON processes(start_time_us);"
        "CREATE TABLE IF NOT EXISTS file_accesses ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  pid INTEGER NOT NULL,"
        "  filename TEXT NOT NULL,"
        "  mode INTEGER NOT NULL,"
        "  fd INTEGER,"
        "  timestamp_us INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_fa_pid ON file_accesses(pid);"
        "CREATE INDEX IF NOT EXISTS idx_fa_filename ON file_accesses(filename);"
        "CREATE TABLE IF NOT EXISTS failed_accesses ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  pid INTEGER NOT NULL,"
        "  filename TEXT NOT NULL,"
        "  mode INTEGER NOT NULL,"
        "  errno_val INTEGER NOT NULL,"
        "  timestamp_us INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_failed_pid ON failed_accesses(pid);"
        "CREATE INDEX IF NOT EXISTS idx_failed_filename ON failed_accesses(filename);"
        ;
}

const char* get_schema_v2_upgrade_sql() {
    return
        "ALTER TABLE file_accesses ADD COLUMN timestamp_us INTEGER NOT NULL DEFAULT 0;"
        "CREATE TABLE IF NOT EXISTS failed_accesses ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  pid INTEGER NOT NULL,"
        "  filename TEXT NOT NULL,"
        "  mode INTEGER NOT NULL,"
        "  errno_val INTEGER NOT NULL,"
        "  timestamp_us INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_failed_pid ON failed_accesses(pid);"
        ;
}

const char* get_schema_v3_upgrade_sql() {
    return
        "ALTER TABLE processes ADD COLUMN user_time_us INTEGER DEFAULT 0;"
        "ALTER TABLE processes ADD COLUMN sys_time_us INTEGER DEFAULT 0;"
        "ALTER TABLE processes ADD COLUMN peak_rss_kb INTEGER DEFAULT 0;"
        "ALTER TABLE processes ADD COLUMN io_read_bytes INTEGER DEFAULT 0;"
        "ALTER TABLE processes ADD COLUMN io_write_bytes INTEGER DEFAULT 0;"
        ;
}

const char* get_schema_v4_upgrade_sql() {
    return
        "ALTER TABLE processes ADD COLUMN cwd TEXT DEFAULT '';"
        ;
}

const char* get_schema_v5_upgrade_sql() {
    return
        "ALTER TABLE processes ADD COLUMN file_count INTEGER DEFAULT 0;"
        "ALTER TABLE processes ADD COLUMN fail_count INTEGER DEFAULT 0;"
        ;
}

} // namespace bdtrace
