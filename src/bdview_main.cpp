#include "common/types.h"
#include "common/log.h"
#include "common/string_util.h"
#include "common/view_helpers.h"
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
        "  tree <db> [--slow MS]          Process tree with durations\n"
        "  slowest <db> [-n N]            Top N slowest processes (default 10)\n"
        "  files <db> [-p PID | -f PATH]  File access info\n"
        "  trace <db> [-p PID | -f PATH] [-w N]  Files with process ancestry\n"
        "  rebuild <db> --changed F [--changed F2 ...] [--collapse CMD ...] [--estimate]  Minimal rebuild set\n"
        "  timeline <db> [--min-duration MS]  Gantt chart of process execution\n"
        "  critical <db>                  Critical path analysis\n"
        "  hotspot <db> [--top N] [--dir] File/directory hotspots\n"
        "  failures <db> [--errno N]      Failed file accesses\n"
        "  parallel <db>                  Parallelism analysis\n"
        "  diff <db1> <db2>               Compare two trace databases\n"
        "  diagnose <db>                  Auto-detect build problems\n"
        "  pio <db> -p PID [--tree]       Process I/O summary (inputs/outputs)\n"
        "  impact <db> [--top N]          Impact ranking of source files\n"
        "  races <db>                     Detect parallel build race conditions\n"
        "  rdeps <db> -f PATH [--depth N] Reverse file dependencies\n"
    );
}

using namespace bdtrace;

// --- helpers ---
static bool cmp_start_time(const ProcessRecord& a, const ProcessRecord& b) {
    return a.start_time_us < b.start_time_us;
}

static bool cmp_duration_desc(const ProcessRecord& a, const ProcessRecord& b) {
    return (a.end_time_us - a.start_time_us) > (b.end_time_us - b.start_time_us);
}

static std::vector<ProcessRecord> get_ancestor_chain(Database& db, int pid) {
    std::vector<ProcessRecord> chain;
    std::set<int> visited;
    int cur = pid;
    while (true) {
        if (visited.find(cur) != visited.end()) break;
        visited.insert(cur);
        ProcessRecord rec;
        if (!db.get_process(cur, rec)) break;
        chain.push_back(rec);
        cur = rec.ppid;
    }
    return chain;
}

// --- summary (B4 enhanced) ---
static int cmd_summary(Database& db) {
    std::vector<ProcessRecord> procs;
    db.get_all_processes(procs);

    int proc_count = db.get_process_count();
    int file_count = db.get_file_access_count();
    int failed_count = db.get_failed_access_count();

    int64_t min_time = 0, max_time = 0;
    for (size_t i = 0; i < procs.size(); ++i) {
        if (i == 0 || procs[i].start_time_us < min_time) min_time = procs[i].start_time_us;
        if (procs[i].end_time_us > max_time) max_time = procs[i].end_time_us;
    }

    int64_t total_us = max_time - min_time;

    // Count read/write accesses
    std::vector<FileAccessRecord> all_fa;
    db.get_all_file_accesses(all_fa);
    int read_count = 0, write_count = 0;
    for (size_t i = 0; i < all_fa.size(); ++i) {
        if (is_input_mode(all_fa[i].mode)) ++read_count;
        if (is_output_mode(all_fa[i].mode)) ++write_count;
    }

    // Max parallelism: count overlapping processes at each start/end event
    int max_parallel = 0;
    {
        std::vector<std::pair<int64_t, int> > events;
        for (size_t i = 0; i < procs.size(); ++i) {
            if (procs[i].end_time_us > 0) {
                events.push_back(std::make_pair(procs[i].start_time_us, 1));
                events.push_back(std::make_pair(procs[i].end_time_us, -1));
            }
        }
        std::sort(events.begin(), events.end());
        int cur = 0;
        for (size_t i = 0; i < events.size(); ++i) {
            cur += events[i].second;
            if (cur > max_parallel) max_parallel = cur;
        }
    }

    // Top 5 slowest
    std::vector<ProcessRecord> valid;
    for (size_t i = 0; i < procs.size(); ++i) {
        if (procs[i].end_time_us > 0) valid.push_back(procs[i]);
    }
    std::sort(valid.begin(), valid.end(), cmp_duration_desc);

    // Aggregate resource usage
    int64_t total_user_us = 0, total_sys_us = 0;
    int64_t total_io_read = 0, total_io_write = 0;
    int64_t max_rss = 0;
    for (size_t i = 0; i < procs.size(); ++i) {
        total_user_us += procs[i].user_time_us;
        total_sys_us += procs[i].sys_time_us;
        total_io_read += procs[i].io_read_bytes;
        total_io_write += procs[i].io_write_bytes;
        if (procs[i].peak_rss_kb > max_rss) max_rss = procs[i].peak_rss_kb;
    }

    std::printf("=== Build Trace Summary ===\n");
    std::printf("Total time:        %s\n", format_duration_us(total_us).c_str());
    std::printf("Processes:         %d\n", proc_count);
    std::printf("Max parallelism:   %d\n", max_parallel);
    std::printf("File accesses:     %d (read: %d, write: %d)\n", file_count, read_count, write_count);
    if (failed_count > 0) {
        std::printf("Failed accesses:   %d\n", failed_count);
    }
    if (total_user_us > 0 || total_sys_us > 0) {
        std::printf("Total CPU time:    %s user, %s sys\n",
                    format_duration_us(total_user_us).c_str(),
                    format_duration_us(total_sys_us).c_str());
    }
    if (max_rss > 0) {
        if (max_rss >= 1024) {
            std::printf("Peak RSS:          %lld MB\n", (long long)(max_rss / 1024));
        } else {
            std::printf("Peak RSS:          %lld KB\n", (long long)max_rss);
        }
    }
    if (total_io_read > 0 || total_io_write > 0) {
        std::printf("I/O:               read %lld MB, write %lld MB\n",
                    (long long)(total_io_read / (1024 * 1024)),
                    (long long)(total_io_write / (1024 * 1024)));
    }

    int top = 5;
    if (top > (int)valid.size()) top = (int)valid.size();
    if (top > 0) {
        std::printf("\nSlowest processes:\n");
        for (int i = 0; i < top; ++i) {
            int64_t dur = valid[i].end_time_us - valid[i].start_time_us;
            std::printf("  %-10s [%d] %s\n",
                        format_duration_us(dur).c_str(),
                        valid[i].pid,
                        shorten_cmd(valid[i].cmdline, 60).c_str());
        }
    }

    return 0;
}

// --- tree (B6 enhanced with --slow) ---
static int g_slow_threshold_us = 0;

static void print_tree(Database& db, int pid, const std::string& prefix, bool is_last) {
    ProcessRecord rec;
    if (!db.get_process(pid, rec)) return;

    int64_t duration = rec.end_time_us - rec.start_time_us;
    std::string dur_str = (rec.end_time_us > 0) ? format_duration_us(duration) : "running";

    std::string cmd = rec.cmdline;
    size_t space = cmd.find(' ');
    if (space != std::string::npos && cmd.size() > 60) {
        cmd = cmd.substr(0, space) + " ...";
    }

    bool is_slow = (g_slow_threshold_us > 0 && rec.end_time_us > 0 && duration >= g_slow_threshold_us);

    std::printf("%s%s[%d] %s (%s)%s\n",
                prefix.c_str(),
                is_last ? "\\-- " : "|-- ",
                rec.pid,
                cmd.c_str(),
                dur_str.c_str(),
                is_slow ? " ***" : "");

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
static int cmd_slowest(Database& db, int n) {
    std::vector<ProcessRecord> procs;
    db.get_all_processes(procs);

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

// --- trace ---
static int cmd_trace(Database& db, int filter_pid, const std::string& filter_path, int col_width) {
    size_t cmd_max = (col_width > 0) ? (size_t)col_width : 0;

    if (filter_pid > 0) {
        std::vector<ProcessRecord> chain = get_ancestor_chain(db, filter_pid);
        if (chain.empty()) {
            std::printf("Process %d not found.\n", filter_pid);
            return 1;
        }

        std::printf("=== Trace for PID %d ===\n", filter_pid);
        std::printf("Process chain:\n");
        for (size_t i = 0; i < chain.size(); ++i) {
            std::string indent(i * 2, ' ');
            std::string cmd = cmd_max ? shorten_cmd(chain[i].cmdline, cmd_max) : chain[i].cmdline;
            std::printf("  %s%s%s [%d]\n", indent.c_str(),
                        i == 0 ? "" : "\\_ ",
                        cmd.c_str(), chain[i].pid);
        }

        std::vector<FileAccessRecord> accesses;
        db.get_file_accesses_by_pid(filter_pid, accesses);
        std::printf("\nFiles (%d):\n", (int)accesses.size());
        for (size_t i = 0; i < accesses.size(); ++i) {
            std::printf("  %-3s %s\n", mode_str(accesses[i].mode),
                        accesses[i].filename.c_str());
        }
        return 0;
    }

    if (!filter_path.empty()) {
        std::vector<FileAccessRecord> accesses;
        db.get_file_accesses_by_name(filter_path, accesses);

        if (accesses.empty()) {
            std::printf("No accesses found for: %s\n", filter_path.c_str());
            return 0;
        }

        std::printf("=== Trace for: %s ===\n\n", filter_path.c_str());
        std::set<int> seen_pids;
        for (size_t i = 0; i < accesses.size(); ++i) {
            int pid = accesses[i].pid;
            if (seen_pids.find(pid) != seen_pids.end()) continue;
            seen_pids.insert(pid);

            std::vector<ProcessRecord> chain = get_ancestor_chain(db, pid);
            std::printf("  %-3s [%d]", mode_str(accesses[i].mode), pid);
            for (size_t j = 0; j < chain.size(); ++j) {
                std::string cmd = cmd_max ? shorten_cmd(chain[j].cmdline, cmd_max) : chain[j].cmdline;
                if (j == 0) {
                    std::printf(" %s", cmd.c_str());
                } else {
                    std::printf("\n      %s\\_ %s [%d]",
                                std::string(j * 3, ' ').c_str(),
                                cmd.c_str(), chain[j].pid);
                }
            }
            std::printf("\n\n");
        }
        return 0;
    }

    // No filter: group by file
    std::vector<FileAccessRecord> all_accesses;
    db.get_all_file_accesses(all_accesses);

    if (all_accesses.empty()) {
        std::printf("No file accesses recorded.\n");
        return 0;
    }

    std::map<std::string, std::vector<FileAccessRecord> > by_file;
    std::vector<std::string> file_order;
    for (size_t i = 0; i < all_accesses.size(); ++i) {
        const std::string& fn = all_accesses[i].filename;
        if (by_file.find(fn) == by_file.end()) {
            file_order.push_back(fn);
        }
        by_file[fn].push_back(all_accesses[i]);
    }

    std::printf("=== File Access Trace (%d files) ===\n\n", (int)file_order.size());

    for (size_t fi = 0; fi < file_order.size(); ++fi) {
        const std::string& filename = file_order[fi];
        const std::vector<FileAccessRecord>& accs = by_file[filename];

        std::printf("%s\n", filename.c_str());

        std::set<int> seen_pids;
        for (size_t i = 0; i < accs.size(); ++i) {
            int pid = accs[i].pid;
            if (seen_pids.find(pid) != seen_pids.end()) continue;
            seen_pids.insert(pid);

            std::vector<ProcessRecord> chain = get_ancestor_chain(db, pid);
            if (chain.empty()) continue;

            std::string cmd0 = cmd_max ? shorten_cmd(chain[0].cmdline, cmd_max) : chain[0].cmdline;
            std::printf("  %-3s %s [%d]\n", mode_str(accs[i].mode),
                        cmd0.c_str(), chain[0].pid);
            for (size_t j = 1; j < chain.size(); ++j) {
                std::string cmd = cmd_max ? shorten_cmd(chain[j].cmdline, cmd_max) : chain[j].cmdline;
                std::printf("      %s\\_ %s [%d]\n",
                            std::string((j - 1) * 3, ' ').c_str(),
                            cmd.c_str(), chain[j].pid);
            }
        }
        std::printf("\n");
    }

    return 0;
}

// --- rebuild ---
static int cmd_rebuild(Database& db, const std::vector<std::string>& changed_args,
                       const std::set<std::string>& collapse_names,
                       bool estimate) {
    DependencyGraph g;
    build_dependency_graph(db, g);

    std::set<std::string> changed_files;
    for (size_t a = 0; a < changed_args.size(); ++a) {
        const std::string& arg = changed_args[a];
        if (g.file_to_readers.find(arg) != g.file_to_readers.end()) {
            changed_files.insert(arg);
        }
        std::string prefix = arg;
        if (!prefix.empty() && prefix[prefix.size() - 1] != '/') {
            prefix += '/';
        }
        std::map<std::string, std::set<int> >::const_iterator it =
            g.file_to_readers.lower_bound(prefix);
        while (it != g.file_to_readers.end()) {
            if (it->first.compare(0, prefix.size(), prefix) != 0) break;
            changed_files.insert(it->first);
            ++it;
        }
    }

    std::set<int> affected = rebuild_bfs(g, changed_files);
    RebuildResult rr = filter_rebuild_set(g, affected, collapse_names);

    std::printf("=== Minimal Rebuild Set for:");
    for (size_t i = 0; i < changed_args.size(); ++i) {
        std::printf(" %s", changed_args[i].c_str());
    }
    std::printf(" ===\n");

    std::printf("Affected processes: %d\n", (int)rr.processes.size());
    std::string last_cwd;
    for (size_t i = 0; i < rr.processes.size(); ++i) {
        if (!rr.processes[i].cwd.empty() && rr.processes[i].cwd != last_cwd) {
            std::printf("  cd %s\n", rr.processes[i].cwd.c_str());
            last_cwd = rr.processes[i].cwd;
        }
        std::printf("  [%d] %s\n", rr.processes[i].pid, rr.processes[i].cmdline.c_str());
    }

    if (estimate) {
        RebuildEstimate est;
        compute_rebuild_estimate(g, affected, est);
        std::printf("\n--- Rebuild Estimate ---\n");
        std::printf("  Affected processes: %d\n", est.affected_count);
        std::printf("  Serial estimate:    %.1fs (sum of all durations)\n",
                    est.serial_estimate_us / 1000000.0);
        std::printf("  Longest single:     %.1fs (minimum possible time)\n",
                    est.longest_single_us / 1000000.0);
    }

    return 0;
}

// --- pio ---
static int cmd_pio(Database& db, int pid, bool tree) {
    DependencyGraph g;
    build_dependency_graph(db, g);

    if (g.proc_map.find(pid) == g.proc_map.end()) {
        std::fprintf(stderr, "bdview pio: PID %d not found\n", pid);
        return 1;
    }

    ProcessIO pio;
    classify_process_io(db, g, pid, tree, pio);

    std::printf("=== Process I/O for PID %d%s ===\n", pid,
                tree ? " (with descendants)" : "");

    std::printf("\nInputs (%d):\n", (int)pio.inputs.size());
    for (std::set<std::string>::const_iterator it = pio.inputs.begin();
         it != pio.inputs.end(); ++it) {
        std::printf("  R  %s\n", it->c_str());
    }
    std::printf("\nOutputs (%d):\n", (int)pio.outputs.size());
    for (std::set<std::string>::const_iterator it = pio.outputs.begin();
         it != pio.outputs.end(); ++it) {
        std::printf("  W  %s\n", it->c_str());
    }
    if (!pio.internal.empty()) {
        std::printf("\nInternal (%d):\n", (int)pio.internal.size());
        for (std::set<std::string>::const_iterator it = pio.internal.begin();
             it != pio.internal.end(); ++it) {
            std::printf("  RW %s\n", it->c_str());
        }
    }
    return 0;
}

// --- impact ---
static int cmd_impact(Database& db, int top_n) {
    DependencyGraph g;
    build_dependency_graph(db, g);

    std::vector<ImpactEntry> entries;
    compute_impact(g, entries, top_n);

    std::printf("=== Impact Ranking (top %d) ===\n", top_n);
    std::printf("  %5s  %10s  %s\n", "PROCS", "DURATION", "FILE");
    for (size_t i = 0; i < entries.size(); ++i) {
        std::printf("  %5d  %9.1fs  %s\n",
                    entries[i].affected_count,
                    entries[i].affected_duration_us / 1000000.0,
                    entries[i].file.c_str());
    }
    return 0;
}

// --- races ---
static int cmd_races(Database& db) {
    DependencyGraph g;
    build_dependency_graph(db, g);

    std::vector<RaceEntry> races;
    detect_races(g, races);

    if (races.empty()) {
        std::printf("=== No Race Conditions Detected ===\n");
        return 0;
    }

    std::printf("=== Race Conditions Detected: %d ===\n", (int)races.size());
    for (size_t i = 0; i < races.size(); ++i) {
        std::printf("\n  FILE: %s\n", races[i].file.c_str());
        std::map<int, ProcessRecord>::const_iterator wp =
            g.proc_map.find(races[i].writer_pid);
        std::map<int, ProcessRecord>::const_iterator rp =
            g.proc_map.find(races[i].reader_pid);
        if (wp != g.proc_map.end()) {
            std::printf("    Writer: [%d] %s\n",
                        races[i].writer_pid, wp->second.cmdline.c_str());
        }
        if (rp != g.proc_map.end()) {
            std::printf("    Reader: [%d] %s\n",
                        races[i].reader_pid, rp->second.cmdline.c_str());
        }
        std::printf("    Overlap: %.3fs\n", races[i].overlap_us / 1000000.0);
    }
    return 0;
}

// --- rdeps ---
static void print_rdeps_tree(const DependencyGraph& g,
                             const RdepsNode& node, int indent) {
    for (int i = 0; i < indent; ++i) std::printf("  ");
    if (indent > 0) std::printf("-> ");
    std::printf("%s", node.file.c_str());
    if (node.via_pid != 0) {
        std::map<int, ProcessRecord>::const_iterator pit =
            g.proc_map.find(node.via_pid);
        if (pit != g.proc_map.end()) {
            std::printf("  (via [%d] %s)", node.via_pid,
                        cmd_name(pit->second.cmdline).c_str());
        }
    }
    std::printf("\n");
    for (size_t i = 0; i < node.children.size(); ++i) {
        print_rdeps_tree(g, node.children[i], indent + 1);
    }
}

static int cmd_rdeps(Database& db, const std::string& path, int depth) {
    DependencyGraph g;
    build_dependency_graph(db, g);

    if (g.file_to_readers.find(path) == g.file_to_readers.end()) {
        std::fprintf(stderr, "bdview rdeps: file '%s' not found in trace\n",
                     path.c_str());
        return 1;
    }

    RdepsNode root;
    compute_rdeps(g, path, depth, root);

    std::printf("=== Reverse Dependencies for %s (depth %d) ===\n",
                path.c_str(), depth);
    print_rdeps_tree(g, root, 0);
    return 0;
}

// --- timeline (B1) ---
static int cmd_timeline(Database& db, int min_duration_ms) {
    std::vector<ProcessRecord> procs;
    db.get_all_processes(procs);
    if (procs.empty()) {
        std::printf("No processes recorded.\n");
        return 0;
    }

    int64_t min_time = procs[0].start_time_us;
    int64_t max_time = procs[0].end_time_us;
    for (size_t i = 1; i < procs.size(); ++i) {
        if (procs[i].start_time_us < min_time) min_time = procs[i].start_time_us;
        if (procs[i].end_time_us > max_time) max_time = procs[i].end_time_us;
    }

    int64_t total_us = max_time - min_time;
    if (total_us <= 0) {
        std::printf("No measurable time span.\n");
        return 0;
    }

    int64_t min_dur_us = (int64_t)min_duration_ms * 1000;

    // Filter and sort by start time
    std::vector<ProcessRecord> filtered;
    for (size_t i = 0; i < procs.size(); ++i) {
        if (procs[i].end_time_us <= 0) continue;
        int64_t dur = procs[i].end_time_us - procs[i].start_time_us;
        if (dur >= min_dur_us) {
            filtered.push_back(procs[i]);
        }
    }
    std::sort(filtered.begin(), filtered.end(), cmp_start_time);

    if (filtered.empty()) {
        std::printf("No processes match the duration filter.\n");
        return 0;
    }

    // Build process tree depth for indentation
    std::set<int> all_pids;
    std::map<int, int> pid_ppid;
    for (size_t i = 0; i < procs.size(); ++i) {
        all_pids.insert(procs[i].pid);
        pid_ppid[procs[i].pid] = procs[i].ppid;
    }

    // Calculate depth for each process
    std::map<int, int> depth_cache;
    for (size_t i = 0; i < filtered.size(); ++i) {
        int pid = filtered[i].pid;
        if (depth_cache.find(pid) != depth_cache.end()) continue;
        std::vector<int> chain;
        int cur = pid;
        while (all_pids.find(cur) != all_pids.end() && depth_cache.find(cur) == depth_cache.end()) {
            chain.push_back(cur);
            cur = pid_ppid[cur];
        }
        int base = (depth_cache.find(cur) != depth_cache.end()) ? depth_cache[cur] + 1 : 0;
        for (int j = (int)chain.size() - 1; j >= 0; --j) {
            depth_cache[chain[j]] = base + ((int)chain.size() - 1 - j);
        }
    }

    // Chart parameters
    int bar_width = 50;
    int label_width = 25;

    // Print time header
    std::printf("%-*s ", label_width, "PROCESS");
    double total_sec = total_us / 1000000.0;
    // Print scale markers
    for (int i = 0; i <= bar_width; i += bar_width / 5) {
        double t = total_sec * i / bar_width;
        if (i == 0) {
            std::printf("%-10.1fs", t);
        } else if (i >= bar_width - 2) {
            // skip, no room
        } else {
            char tbuf[16];
            std::snprintf(tbuf, sizeof(tbuf), "%.1fs", t);
            std::printf("%-10s", tbuf);
        }
    }
    std::printf("\n");

    // Print bars
    for (size_t i = 0; i < filtered.size(); ++i) {
        int depth = 0;
        if (depth_cache.find(filtered[i].pid) != depth_cache.end()) {
            depth = depth_cache[filtered[i].pid];
        }

        std::string name = cmd_name(filtered[i].cmdline);
        std::string indent(depth * 2, ' ');
        std::string label = indent + name;
        if ((int)label.size() > label_width - 1) {
            label = label.substr(0, label_width - 1);
        }

        int start_col = (int)((filtered[i].start_time_us - min_time) * bar_width / total_us);
        int end_col = (int)((filtered[i].end_time_us - min_time) * bar_width / total_us);
        if (end_col <= start_col) end_col = start_col + 1;
        if (end_col > bar_width) end_col = bar_width;

        std::printf("%-*s |", label_width, label.c_str());
        for (int c = 0; c < bar_width; ++c) {
            if (c >= start_col && c < end_col) {
                std::printf("=");
            } else {
                std::printf(" ");
            }
        }

        int64_t dur = filtered[i].end_time_us - filtered[i].start_time_us;
        std::printf("| %s\n", format_duration_us(dur).c_str());
    }

    std::printf("\nTotal: %s, %d processes shown\n",
                format_duration_us(total_us).c_str(), (int)filtered.size());

    return 0;
}

// --- critical (B2) ---
static int cmd_critical(Database& db) {
    std::vector<ProcessRecord> procs;
    db.get_all_processes(procs);
    if (procs.empty()) {
        std::printf("No processes recorded.\n");
        return 0;
    }

    // Build tree
    std::map<int, ProcessRecord> proc_map;
    std::map<int, std::vector<int> > children;
    std::set<int> all_pids;
    for (size_t i = 0; i < procs.size(); ++i) {
        proc_map[procs[i].pid] = procs[i];
        children[procs[i].ppid].push_back(procs[i].pid);
        all_pids.insert(procs[i].pid);
    }

    // Find roots
    std::vector<int> roots;
    for (size_t i = 0; i < procs.size(); ++i) {
        if (all_pids.find(procs[i].ppid) == all_pids.end()) {
            roots.push_back(procs[i].pid);
        }
    }

    // For each process, compute the "critical path duration" through it:
    // its own end_time - start_time, but for non-leaf nodes the critical
    // child is the one whose end_time_us is latest (since children run
    // within the parent's timespan).
    //
    // We trace: from root, always pick the child that ends latest.
    // The critical path = chain of processes that determined the total build time.

    // Find the root with the longest span
    int critical_root = roots[0];
    int64_t max_span = 0;
    for (size_t i = 0; i < roots.size(); ++i) {
        int64_t span = proc_map[roots[i]].end_time_us - proc_map[roots[i]].start_time_us;
        if (span > max_span) {
            max_span = span;
            critical_root = roots[i];
        }
    }

    // Trace critical path: from root, at each level pick the child
    // that ends the latest (it's the bottleneck)
    std::vector<ProcessRecord> path;
    int cur = critical_root;
    while (true) {
        path.push_back(proc_map[cur]);
        std::map<int, std::vector<int> >::iterator cit = children.find(cur);
        if (cit == children.end() || cit->second.empty()) break;

        int best_child = cit->second[0];
        int64_t best_end = proc_map[best_child].end_time_us;
        for (size_t i = 1; i < cit->second.size(); ++i) {
            int64_t end = proc_map[cit->second[i]].end_time_us;
            if (end > best_end) {
                best_end = end;
                best_child = cit->second[i];
            }
        }
        cur = best_child;
    }

    int64_t total_us = proc_map[critical_root].end_time_us - proc_map[critical_root].start_time_us;

    std::printf("=== Critical Path Analysis ===\n");
    std::printf("Total build time: %s\n\n", format_duration_us(total_us).c_str());

    for (size_t i = 0; i < path.size(); ++i) {
        int64_t dur = path[i].end_time_us - path[i].start_time_us;
        int64_t wait = 0;
        if (i > 0) {
            wait = path[i].start_time_us - path[i - 1].start_time_us;
        }

        std::string indent(i * 2, ' ');
        std::printf("%s[%d] %s\n", indent.c_str(), path[i].pid,
                    shorten_cmd(path[i].cmdline, 60).c_str());
        std::printf("%s     duration: %s", indent.c_str(),
                    format_duration_us(dur).c_str());
        if (i > 0 && wait > 0) {
            std::printf("  (started %s after parent)", format_duration_us(wait).c_str());
        }
        std::printf("\n");
    }

    return 0;
}

// --- hotspot (B3) ---
struct HotspotEntry {
    std::string name;
    int total;
    int reads;
    int writes;
    int num_procs;
};

static bool cmp_hotspot_desc(const HotspotEntry& a, const HotspotEntry& b) {
    return a.total > b.total;
}

struct AccumData {
    int total;
    int reads;
    int writes;
    std::set<int> pids;
    AccumData() : total(0), reads(0), writes(0) {}
};

static int cmd_hotspot(Database& db, int top_n, bool by_dir) {
    std::vector<FileAccessRecord> all_fa;
    db.get_all_file_accesses(all_fa);

    if (all_fa.empty()) {
        std::printf("No file accesses recorded.\n");
        return 0;
    }

    std::map<std::string, AccumData> accum;
    for (size_t i = 0; i < all_fa.size(); ++i) {
        std::string key = all_fa[i].filename;
        if (by_dir) {
            size_t slash = key.rfind('/');
            if (slash != std::string::npos) {
                key = key.substr(0, slash + 1);
            }
        }
        AccumData& d = accum[key];
        d.total++;
        if (is_input_mode(all_fa[i].mode)) d.reads++;
        if (is_output_mode(all_fa[i].mode)) d.writes++;
        d.pids.insert(all_fa[i].pid);
    }

    std::vector<HotspotEntry> entries;
    for (std::map<std::string, AccumData>::const_iterator it = accum.begin();
         it != accum.end(); ++it) {
        HotspotEntry e;
        e.name = it->first;
        e.total = it->second.total;
        e.reads = it->second.reads;
        e.writes = it->second.writes;
        e.num_procs = (int)it->second.pids.size();
        entries.push_back(e);
    }

    std::sort(entries.begin(), entries.end(), cmp_hotspot_desc);

    int count = top_n;
    if (count > (int)entries.size()) count = (int)entries.size();

    std::printf("=== File %s Hotspots (top %d) ===\n",
                by_dir ? "Directory" : "Access", count);
    std::printf("%-6s %-5s %-5s %-5s %s\n", "COUNT", "READ", "WRITE", "PROCS", "PATH");

    for (int i = 0; i < count; ++i) {
        std::printf("%-6d %-5d %-5d %-5d %s\n",
                    entries[i].total, entries[i].reads, entries[i].writes,
                    entries[i].num_procs, entries[i].name.c_str());
    }

    return 0;
}

// --- failures (B5) ---
static int cmd_failures(Database& db, int filter_errno) {
    std::vector<FailedAccessRecord> failures;
    db.get_all_failed_accesses(failures);

    if (failures.empty()) {
        std::printf("No failed accesses recorded.\n");
        return 0;
    }

    // Filter by errno if specified
    if (filter_errno > 0) {
        std::vector<FailedAccessRecord> filtered;
        for (size_t i = 0; i < failures.size(); ++i) {
            if (failures[i].errno_val == filter_errno) {
                filtered.push_back(failures[i]);
            }
        }
        failures = filtered;
    }

    // Group by errno
    std::map<int, int> errno_counts;
    for (size_t i = 0; i < failures.size(); ++i) {
        errno_counts[failures[i].errno_val]++;
    }

    std::printf("=== Failed File Accesses (%d total) ===\n\n", (int)failures.size());

    // Errno summary
    std::printf("By error code:\n");
    for (std::map<int, int>::const_iterator it = errno_counts.begin();
         it != errno_counts.end(); ++it) {
        const char* name = errno_name(it->first);
        if (name[0]) {
            std::printf("  %s (errno %d): %d\n", name, it->first, it->second);
        } else {
            std::printf("  errno %d: %d\n", it->first, it->second);
        }
    }

    // Group by filename, sorted by frequency
    std::map<std::string, int> file_counts;
    for (size_t i = 0; i < failures.size(); ++i) {
        file_counts[failures[i].filename]++;
    }

    std::vector<std::pair<int, std::string> > sorted_files;
    for (std::map<std::string, int>::const_iterator it = file_counts.begin();
         it != file_counts.end(); ++it) {
        sorted_files.push_back(std::make_pair(-it->second, it->first));
    }
    std::sort(sorted_files.begin(), sorted_files.end());

    int show = 20;
    if (show > (int)sorted_files.size()) show = (int)sorted_files.size();
    std::printf("\nTop %d files with failures:\n", show);
    for (int i = 0; i < show; ++i) {
        std::printf("  %5d  %s\n", -sorted_files[i].first, sorted_files[i].second.c_str());
    }

    return 0;
}

// --- parallel (B10) ---
static int cmd_parallel(Database& db) {
    std::vector<ProcessRecord> procs;
    db.get_all_processes(procs);
    if (procs.empty()) {
        std::printf("No processes recorded.\n");
        return 0;
    }

    int64_t min_time = procs[0].start_time_us;
    int64_t max_time = procs[0].end_time_us;
    for (size_t i = 1; i < procs.size(); ++i) {
        if (procs[i].start_time_us < min_time) min_time = procs[i].start_time_us;
        if (procs[i].end_time_us > max_time) max_time = procs[i].end_time_us;
    }

    int64_t total_us = max_time - min_time;
    if (total_us <= 0) {
        std::printf("No measurable time span.\n");
        return 0;
    }

    // 1-second buckets
    int64_t bucket_us = 1000000;
    int num_buckets = (int)(total_us / bucket_us) + 1;
    if (num_buckets > 3600) {
        // Cap at 1 hour, use larger buckets
        num_buckets = 3600;
        bucket_us = total_us / num_buckets + 1;
    }

    std::vector<int> buckets(num_buckets, 0);

    for (size_t i = 0; i < procs.size(); ++i) {
        if (procs[i].end_time_us <= 0) continue;
        int start_b = (int)((procs[i].start_time_us - min_time) / bucket_us);
        int end_b = (int)((procs[i].end_time_us - min_time) / bucket_us);
        if (start_b < 0) start_b = 0;
        if (end_b >= num_buckets) end_b = num_buckets - 1;
        for (int b = start_b; b <= end_b; ++b) {
            buckets[b]++;
        }
    }

    int max_parallel = 0;
    int64_t sum_parallel = 0;
    int active_buckets = 0;
    for (int i = 0; i < num_buckets; ++i) {
        if (buckets[i] > max_parallel) max_parallel = buckets[i];
        if (buckets[i] > 0) {
            sum_parallel += buckets[i];
            active_buckets++;
        }
    }

    double avg = active_buckets > 0 ? (double)sum_parallel / active_buckets : 0;

    std::printf("=== Parallelism Analysis ===\n");
    std::printf("Total time:        %s\n", format_duration_us(total_us).c_str());
    std::printf("Max parallelism:   %d\n", max_parallel);
    std::printf("Avg parallelism:   %.1f\n\n", avg);

    // Histogram
    int hist_width = 50;
    int max_val = max_parallel > 0 ? max_parallel : 1;

    std::printf("TIME(s)  PROCS  HISTOGRAM\n");
    for (int i = 0; i < num_buckets; ++i) {
        if (buckets[i] == 0 && i > 0 && i < num_buckets - 1) continue;
        double t = (double)i * bucket_us / 1000000.0;
        int bar_len = buckets[i] * hist_width / max_val;

        std::printf("%7.1f  %5d  ", t, buckets[i]);
        for (int j = 0; j < bar_len; ++j) std::printf("#");
        std::printf("\n");
    }

    return 0;
}

// --- diff (B8) ---
static int cmd_diff(const std::string& db_path1, const std::string& db_path2) {
    Database db1, db2;
    if (!db1.open(db_path1)) {
        std::fprintf(stderr, "Cannot open: %s\n", db_path1.c_str());
        return 1;
    }
    if (!db2.open(db_path2)) {
        std::fprintf(stderr, "Cannot open: %s\n", db_path2.c_str());
        return 1;
    }
    db1.upgrade_schema();
    db2.upgrade_schema();

    std::vector<ProcessRecord> procs1, procs2;
    db1.get_all_processes(procs1);
    db2.get_all_processes(procs2);

    int fa_count1 = db1.get_file_access_count();
    int fa_count2 = db2.get_file_access_count();

    // Total time for each
    int64_t min1 = 0, max1 = 0, min2 = 0, max2 = 0;
    for (size_t i = 0; i < procs1.size(); ++i) {
        if (i == 0 || procs1[i].start_time_us < min1) min1 = procs1[i].start_time_us;
        if (procs1[i].end_time_us > max1) max1 = procs1[i].end_time_us;
    }
    for (size_t i = 0; i < procs2.size(); ++i) {
        if (i == 0 || procs2[i].start_time_us < min2) min2 = procs2[i].start_time_us;
        if (procs2[i].end_time_us > max2) max2 = procs2[i].end_time_us;
    }
    int64_t total1 = max1 - min1;
    int64_t total2 = max2 - min2;
    int64_t diff_time = total2 - total1;

    std::printf("=== Build Comparison ===\n");
    std::printf("                     %-20s %-20s %s\n", "DB1", "DB2", "DIFF");
    std::printf("Total time:          %-20s %-20s %s%s\n",
                format_duration_us(total1).c_str(),
                format_duration_us(total2).c_str(),
                diff_time >= 0 ? "+" : "",
                format_duration_us(diff_time).c_str());
    std::printf("Processes:           %-20d %-20d %+d\n",
                (int)procs1.size(), (int)procs2.size(),
                (int)procs2.size() - (int)procs1.size());
    std::printf("File accesses:       %-20d %-20d %+d\n",
                fa_count1, fa_count2, fa_count2 - fa_count1);

    // Match commands by cmdline and compare durations
    std::map<std::string, int64_t> dur1, dur2;
    for (size_t i = 0; i < procs1.size(); ++i) {
        if (procs1[i].end_time_us > 0) {
            std::string key = procs1[i].cmdline;
            dur1[key] = procs1[i].end_time_us - procs1[i].start_time_us;
        }
    }
    for (size_t i = 0; i < procs2.size(); ++i) {
        if (procs2[i].end_time_us > 0) {
            std::string key = procs2[i].cmdline;
            dur2[key] = procs2[i].end_time_us - procs2[i].start_time_us;
        }
    }

    // Find biggest time differences
    std::vector<std::pair<int64_t, std::string> > diffs;
    for (std::map<std::string, int64_t>::const_iterator it = dur2.begin();
         it != dur2.end(); ++it) {
        std::map<std::string, int64_t>::const_iterator it1 = dur1.find(it->first);
        if (it1 != dur1.end()) {
            int64_t d = it->second - it1->second;
            if (d != 0) {
                diffs.push_back(std::make_pair(d < 0 ? d : -d, it->first));
                // Store actual diff for display
            }
        }
    }
    std::sort(diffs.begin(), diffs.end());

    if (!diffs.empty()) {
        int show = 10;
        if (show > (int)diffs.size()) show = (int)diffs.size();
        std::printf("\nBiggest time changes:\n");
        for (int i = 0; i < show; ++i) {
            const std::string& cmd = diffs[i].second;
            int64_t d = dur2[cmd] - dur1[cmd];
            std::printf("  %s%s  %s\n",
                        d >= 0 ? "+" : "",
                        format_duration_us(d).c_str(),
                        shorten_cmd(cmd, 60).c_str());
        }
    }

    // New/removed processes
    std::set<std::string> cmds1, cmds2;
    for (size_t i = 0; i < procs1.size(); ++i) cmds1.insert(procs1[i].cmdline);
    for (size_t i = 0; i < procs2.size(); ++i) cmds2.insert(procs2[i].cmdline);

    int new_count = 0, removed_count = 0;
    for (std::set<std::string>::const_iterator it = cmds2.begin(); it != cmds2.end(); ++it) {
        if (cmds1.find(*it) == cmds1.end()) new_count++;
    }
    for (std::set<std::string>::const_iterator it = cmds1.begin(); it != cmds1.end(); ++it) {
        if (cmds2.find(*it) == cmds2.end()) removed_count++;
    }

    if (new_count > 0 || removed_count > 0) {
        std::printf("\nNew processes: %d, Removed processes: %d\n", new_count, removed_count);
    }

    return 0;
}

// --- diagnose (B9) ---
static int cmd_diagnose(Database& db) {
    std::vector<ProcessRecord> procs;
    db.get_all_processes(procs);
    std::vector<FileAccessRecord> all_fa;
    db.get_all_file_accesses(all_fa);

    if (procs.empty()) {
        std::printf("No processes recorded.\n");
        return 0;
    }

    int64_t min_time = procs[0].start_time_us;
    int64_t max_time = procs[0].end_time_us;
    for (size_t i = 1; i < procs.size(); ++i) {
        if (procs[i].start_time_us < min_time) min_time = procs[i].start_time_us;
        if (procs[i].end_time_us > max_time) max_time = procs[i].end_time_us;
    }
    int64_t total_us = max_time - min_time;

    std::printf("=== Build Diagnostics ===\n");
    std::printf("Total build time: %s\n\n", format_duration_us(total_us).c_str());

    int issue_count = 0;

    // 1. Serial execution detection
    // Find sequences of processes at the same tree level that run sequentially
    // (non-overlapping) when they could potentially run in parallel
    {
        std::map<int, std::vector<ProcessRecord> > by_parent;
        for (size_t i = 0; i < procs.size(); ++i) {
            by_parent[procs[i].ppid].push_back(procs[i]);
        }

        for (std::map<int, std::vector<ProcessRecord> >::iterator it = by_parent.begin();
             it != by_parent.end(); ++it) {
            std::vector<ProcessRecord>& siblings = it->second;
            if (siblings.size() < 2) continue;
            std::sort(siblings.begin(), siblings.end(), cmp_start_time);

            int serial_count = 0;
            int64_t serial_time = 0;
            for (size_t i = 1; i < siblings.size(); ++i) {
                if (siblings[i].start_time_us >= siblings[i - 1].end_time_us) {
                    serial_count++;
                    serial_time += siblings[i].end_time_us - siblings[i].start_time_us;
                }
            }
            if (serial_count >= 3 && serial_time > total_us / 10) {
                ++issue_count;
                ProcessRecord parent;
                if (db.get_process(it->first, parent)) {
                    std::printf("[SERIAL] %d sequential children under [%d] %s\n",
                                serial_count, it->first,
                                shorten_cmd(parent.cmdline, 40).c_str());
                    std::printf("         Potential parallelization savings: %s\n\n",
                                format_duration_us(serial_time).c_str());
                }
            }
        }
    }

    // 2. Long processes
    {
        int64_t threshold = total_us / 4;
        std::vector<ProcessRecord> sorted = procs;
        std::sort(sorted.begin(), sorted.end(), cmp_duration_desc);

        bool header_printed = false;
        for (size_t i = 0; i < sorted.size() && i < 5; ++i) {
            int64_t dur = sorted[i].end_time_us - sorted[i].start_time_us;
            if (dur >= threshold) {
                if (!header_printed) {
                    std::printf("[SLOW] Processes taking >25%% of build time:\n");
                    header_printed = true;
                    ++issue_count;
                }
                std::printf("  %s (%d%% of total) [%d] %s\n",
                            format_duration_us(dur).c_str(),
                            (int)(dur * 100 / total_us),
                            sorted[i].pid,
                            shorten_cmd(sorted[i].cmdline, 50).c_str());
            }
        }
        if (header_printed) std::printf("\n");
    }

    // 3. Duplicate file accesses
    {
        std::map<std::string, std::set<int> > file_pids;
        for (size_t i = 0; i < all_fa.size(); ++i) {
            if (is_input_mode(all_fa[i].mode)) {
                file_pids[all_fa[i].filename].insert(all_fa[i].pid);
            }
        }

        int dup_count = 0;
        std::string worst_file;
        int worst_count = 0;
        for (std::map<std::string, std::set<int> >::const_iterator it = file_pids.begin();
             it != file_pids.end(); ++it) {
            if ((int)it->second.size() > 10) {
                dup_count++;
                if ((int)it->second.size() > worst_count) {
                    worst_count = (int)it->second.size();
                    worst_file = it->first;
                }
            }
        }
        if (dup_count > 0) {
            ++issue_count;
            std::printf("[HOTFILE] %d files accessed by >10 processes\n", dup_count);
            std::printf("          Worst: %s (%d processes)\n\n",
                        worst_file.c_str(), worst_count);
        }
    }

    // 4. Failed accesses
    {
        int failed = db.get_failed_access_count();
        if (failed > 100) {
            ++issue_count;
            std::printf("[FAILURES] %d failed file accesses detected\n", failed);
            std::printf("           Run 'bdview failures <db>' for details\n\n");
        }
    }

    if (issue_count == 0) {
        std::printf("No significant issues detected.\n");
    } else {
        std::printf("Found %d potential issue(s).\n", issue_count);
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

    // Special case: diff takes two db paths
    if (command == "diff") {
        if (argc < 4) {
            std::fprintf(stderr, "bdview diff: requires two database paths\n");
            return 1;
        }
        return cmd_diff(argv[2], argv[3]);
    }

    std::string db_path = argv[2];

    Database db;
    if (!db.open(db_path)) {
        std::fprintf(stderr, "bdview: cannot open database: %s\n", db_path.c_str());
        return 1;
    }
    db.upgrade_schema();

    if (command == "summary") {
        return cmd_summary(db);
    } else if (command == "tree") {
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--slow") == 0 && i + 1 < argc) {
                g_slow_threshold_us = std::atoi(argv[++i]) * 1000;
            }
        }
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
    } else if (command == "trace") {
        int filter_pid = 0;
        std::string filter_path;
        int col_width = 0;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
                filter_pid = std::atoi(argv[++i]);
            } else if (std::strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
                filter_path = argv[++i];
            } else if (std::strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
                col_width = std::atoi(argv[++i]);
            }
        }
        return cmd_trace(db, filter_pid, filter_path, col_width);
    } else if (command == "rebuild") {
        std::vector<std::string> changed;
        std::set<std::string> collapse;
        bool estimate = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--changed") == 0 && i + 1 < argc) {
                changed.push_back(argv[++i]);
            } else if (std::strcmp(argv[i], "--collapse") == 0 && i + 1 < argc) {
                collapse.insert(argv[++i]);
            } else if (std::strcmp(argv[i], "--estimate") == 0) {
                estimate = true;
            }
        }
        if (changed.empty()) {
            std::fprintf(stderr, "bdview rebuild: --changed FILE required\n");
            return 1;
        }
        return cmd_rebuild(db, changed, collapse, estimate);
    } else if (command == "timeline") {
        int min_dur = 0;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--min-duration") == 0 && i + 1 < argc) {
                min_dur = std::atoi(argv[++i]);
            }
        }
        return cmd_timeline(db, min_dur);
    } else if (command == "critical") {
        return cmd_critical(db);
    } else if (command == "hotspot") {
        int top_n = 20;
        bool by_dir = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
                top_n = std::atoi(argv[++i]);
            } else if (std::strcmp(argv[i], "--dir") == 0) {
                by_dir = true;
            }
        }
        return cmd_hotspot(db, top_n, by_dir);
    } else if (command == "failures") {
        int filter_errno = 0;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--errno") == 0 && i + 1 < argc) {
                filter_errno = std::atoi(argv[++i]);
            }
        }
        return cmd_failures(db, filter_errno);
    } else if (command == "parallel") {
        return cmd_parallel(db);
    } else if (command == "diagnose") {
        return cmd_diagnose(db);
    } else if (command == "pio") {
        int pid = 0;
        bool tree = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
                pid = std::atoi(argv[++i]);
            } else if (std::strcmp(argv[i], "--tree") == 0) {
                tree = true;
            }
        }
        if (pid == 0) {
            std::fprintf(stderr, "bdview pio: -p PID required\n");
            return 1;
        }
        return cmd_pio(db, pid, tree);
    } else if (command == "impact") {
        int top_n = 20;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
                top_n = std::atoi(argv[++i]);
            }
        }
        return cmd_impact(db, top_n);
    } else if (command == "races") {
        return cmd_races(db);
    } else if (command == "rdeps") {
        std::string path;
        int depth = 3;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
                path = argv[++i];
            } else if (std::strcmp(argv[i], "--depth") == 0 && i + 1 < argc) {
                depth = std::atoi(argv[++i]);
            }
        }
        if (path.empty()) {
            std::fprintf(stderr, "bdview rdeps: -f PATH required\n");
            return 1;
        }
        return cmd_rdeps(db, path, depth);
    } else {
        std::fprintf(stderr, "bdview: unknown command: %s\n", command.c_str());
        usage();
        return 1;
    }
}
