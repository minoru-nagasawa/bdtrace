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
        "  exit_code INTEGER"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_proc_ppid ON processes(ppid);"
        "CREATE TABLE IF NOT EXISTS file_accesses ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  pid INTEGER NOT NULL,"
        "  filename TEXT NOT NULL,"
        "  mode INTEGER NOT NULL,"
        "  fd INTEGER"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_fa_pid ON file_accesses(pid);"
        "CREATE INDEX IF NOT EXISTS idx_fa_filename ON file_accesses(filename);"
        ;
}

} // namespace bdtrace
