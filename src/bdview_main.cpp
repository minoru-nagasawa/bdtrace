#include "common/types.h"
#include "common/log.h"
#include "common/string_util.h"
#include "db/database.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <queue>
#include <algorithm>

static void usage() {
    std::fprintf(stderr,
        "Usage: bdview <command> <database> [options]\n"
        "\n"
        "Commands:\n"
        "  summary <db>                   Overall statistics\n"
        "  tree <db>                      Process tree with durations\n"
        "  slowest <db> [-n N]            Top N slowest processes (default 10)\n"
        "  files <db> [-p PID | -f PATH]  File access info\n"
        "  rebuild <db> --changed FILE    Minimal rebuild set\n"
    );
}

using namespace bdtrace;

// --- summary ---
static int cmd_summary(Database& db) {
    std::vector<ProcessRecord> procs;
    db.get_all_processes(procs);

    int proc_count = db.get_process_count();
    int file_count = db.get_file_access_count();

    int64_t min_time = 0, max_time = 0;
    for (size_t i = 0; i < procs.size(); ++i) {
        if (i == 0 || procs[i].start_time_us < min_time) min_time = procs[i].start_time_us;
        if (procs[i].end_time_us > max_time) max_time = procs[i].end_time_us;
    }

    int64_t total_us = max_time - min_time;

    std::printf("=== Build Trace Summary ===\n");
    std::printf("Total time:      %s\n", format_duration_us(total_us).c_str());
    std::printf("Processes:       %d\n", proc_count);
    std::printf("File accesses:   %d\n", file_count);

    return 0;
}

// --- tree ---
static void print_tree(Database& db, int pid, const std::string& prefix, bool is_last) {
    ProcessRecord rec;
    if (!db.get_process(pid, rec)) return;

    int64_t duration = rec.end_time_us - rec.start_time_us;
    std::string dur_str = (rec.end_time_us > 0) ? format_duration_us(duration) : "running";

    // Extract just the command name (first word)
    std::string cmd = rec.cmdline;
    size_t space = cmd.find(' ');
    if (space != std::string::npos && cmd.size() > 60) {
        cmd = cmd.substr(0, space) + " ...";
    }

    std::printf("%s%s[%d] %s (%s)\n",
                prefix.c_str(),
                is_last ? "\\-- " : "|-- ",
                rec.pid,
                cmd.c_str(),
                dur_str.c_str());

    std::vector<ProcessRecord> children;
    db.get_children(pid, children);

    for (size_t i = 0; i < children.size(); ++i) {
        std::string new_prefix = prefix + (is_last ? "    " : "|   ");
        print_tree(db, children[i].pid, new_prefix, i == children.size() - 1);
    }
}

static int cmd_tree(Database& db) {
    std::vector<ProcessRecord> procs;
    db.get_all_processes(procs);
    if (procs.empty()) {
        std::printf("No processes recorded.\n");
        return 0;
    }

    // Find root processes (those whose ppid is not in the process set)
    std::set<int> pids;
    for (size_t i = 0; i < procs.size(); ++i) pids.insert(procs[i].pid);

    std::vector<int> roots;
    for (size_t i = 0; i < procs.size(); ++i) {
        if (pids.find(procs[i].ppid) == pids.end()) {
            roots.push_back(procs[i].pid);
        }
    }

    for (size_t i = 0; i < roots.size(); ++i) {
        print_tree(db, roots[i], "", i == roots.size() - 1);
    }

    return 0;
}

// --- slowest ---
static bool cmp_duration_desc(const ProcessRecord& a, const ProcessRecord& b) {
    return (a.end_time_us - a.start_time_us) > (b.end_time_us - b.start_time_us);
}

static int cmd_slowest(Database& db, int n) {
    std::vector<ProcessRecord> procs;
    db.get_all_processes(procs);

    // Filter out processes without end time
    std::vector<ProcessRecord> valid;
    for (size_t i = 0; i < procs.size(); ++i) {
        if (procs[i].end_time_us > 0) valid.push_back(procs[i]);
    }

    std::sort(valid.begin(), valid.end(), cmp_duration_desc);

    int count = (n < (int)valid.size()) ? n : (int)valid.size();
    std::printf("=== Top %d Slowest Processes ===\n", count);
    for (int i = 0; i < count; ++i) {
        int64_t dur = valid[i].end_time_us - valid[i].start_time_us;
        std::printf("  %s  [%d] %s\n",
                    format_duration_us(dur).c_str(),
                    valid[i].pid,
                    valid[i].cmdline.c_str());
    }
    return 0;
}

// --- files ---
static const char* mode_str(int mode) {
    switch (mode) {
        case FA_READ:  return "R";
        case FA_WRITE: return "W";
        case FA_RDWR:  return "RW";
        default:       return "?";
    }
}

static int cmd_files(Database& db, int filter_pid, const std::string& filter_path) {
    std::vector<FileAccessRecord> accesses;

    if (filter_pid > 0) {
        db.get_file_accesses_by_pid(filter_pid, accesses);
        std::printf("=== File accesses by PID %d ===\n", filter_pid);
    } else if (!filter_path.empty()) {
        db.get_file_accesses_by_name(filter_path, accesses);
        std::printf("=== Processes accessing %s ===\n", filter_path.c_str());
    } else {
        db.get_all_file_accesses(accesses);
        std::printf("=== All file accesses ===\n");
    }

    for (size_t i = 0; i < accesses.size(); ++i) {
        std::printf("  [%d] %-3s %s\n",
                    accesses[i].pid,
                    mode_str(accesses[i].mode),
                    accesses[i].filename.c_str());
    }

    std::printf("Total: %d accesses\n", (int)accesses.size());
    return 0;
}

// --- rebuild ---
static int cmd_rebuild(Database& db, const std::string& changed_file) {
    std::printf("=== Minimal Rebuild Set for: %s ===\n", changed_file.c_str());

    // Step 1: Find processes that read the changed file
    std::vector<FileAccessRecord> readers;
    db.get_file_accesses_by_name(changed_file, readers);

    std::set<int> affected_pids;
    std::queue<int> worklist;

    for (size_t i = 0; i < readers.size(); ++i) {
        if (readers[i].mode == FA_READ || readers[i].mode == FA_RDWR) {
            if (affected_pids.find(readers[i].pid) == affected_pids.end()) {
                affected_pids.insert(readers[i].pid);
                worklist.push(readers[i].pid);
            }
        }
    }

    // Step 2: BFS - find files written by affected processes,
    // then find processes that read those files
    while (!worklist.empty()) {
        int pid = worklist.front();
        worklist.pop();

        std::vector<FileAccessRecord> writes;
        db.get_file_accesses_by_pid(pid, writes);

        for (size_t i = 0; i < writes.size(); ++i) {
            if (writes[i].mode != FA_WRITE && writes[i].mode != FA_RDWR) continue;

            // Find all readers of this output file
            std::vector<FileAccessRecord> downstream;
            db.get_file_accesses_by_name(writes[i].filename, downstream);

            for (size_t j = 0; j < downstream.size(); ++j) {
                if (downstream[j].mode == FA_READ || downstream[j].mode == FA_RDWR) {
                    if (affected_pids.find(downstream[j].pid) == affected_pids.end()) {
                        affected_pids.insert(downstream[j].pid);
                        worklist.push(downstream[j].pid);
                    }
                }
            }
        }
    }

    // Print results
    std::printf("Affected processes: %d\n", (int)affected_pids.size());
    for (std::set<int>::iterator it = affected_pids.begin(); it != affected_pids.end(); ++it) {
        ProcessRecord rec;
        if (db.get_process(*it, rec)) {
            std::printf("  [%d] %s\n", rec.pid, rec.cmdline.c_str());
        }
    }

    return 0;
}

// --- main ---
int main(int argc, char* argv[]) {
    bdtrace::log_init(bdtrace::LOG_WARN);

    if (argc < 3) {
        usage();
        return 1;
    }

    std::string command = argv[1];
    std::string db_path = argv[2];

    Database db;
    if (!db.open(db_path)) {
        std::fprintf(stderr, "bdview: cannot open database: %s\n", db_path.c_str());
        return 1;
    }

    if (command == "summary") {
        return cmd_summary(db);
    } else if (command == "tree") {
        return cmd_tree(db);
    } else if (command == "slowest") {
        int n = 10;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
                n = std::atoi(argv[++i]);
            }
        }
        return cmd_slowest(db, n);
    } else if (command == "files") {
        int filter_pid = 0;
        std::string filter_path;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
                filter_pid = std::atoi(argv[++i]);
            } else if (std::strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
                filter_path = argv[++i];
            }
        }
        return cmd_files(db, filter_pid, filter_path);
    } else if (command == "rebuild") {
        std::string changed;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--changed") == 0 && i + 1 < argc) {
                changed = argv[++i];
            }
        }
        if (changed.empty()) {
            std::fprintf(stderr, "bdview rebuild: --changed FILE required\n");
            return 1;
        }
        return cmd_rebuild(db, changed);
    } else {
        std::fprintf(stderr, "bdview: unknown command: %s\n", command.c_str());
        usage();
        return 1;
    }
}
