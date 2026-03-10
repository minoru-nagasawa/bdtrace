#ifndef BDTRACE_VIEW_HELPERS_H
#define BDTRACE_VIEW_HELPERS_H

#include <string>
#include <map>
#include <set>
#include <vector>
#include "types.h"

namespace bdtrace {

class Database;

const char* mode_str(int mode);
bool is_input_mode(int mode);
bool is_output_mode(int mode);
std::string cmd_name(const std::string& cmdline);
// Returns the script's absolute path for interpreter commands, using cwd
// to resolve relative paths. For non-interpreter commands, returns basename.
std::string cmd_name_full(const std::string& cmdline, const std::string& cwd);
std::string shorten_cmd(const std::string& cmdline, size_t max_len);
const char* errno_name(int e);

// --- Run compaction ---
// Detect separate bdtrace runs by time gaps and shift later runs
// so they appear immediately after the previous run.
void compact_runs(std::vector<ProcessRecord>& procs);

// --- Dependency graph ---

struct DependencyGraph {
    std::map<std::string, std::set<int> > file_to_readers;
    std::map<int, std::set<std::string> > pid_to_outputs;
    std::map<int, ProcessRecord> proc_map;
    std::map<int, std::vector<int> > pid_children;
};

void build_dependency_graph(Database& db, DependencyGraph& g);

// BFS from changed files, returns all transitively affected PIDs
std::set<int> rebuild_bfs(const DependencyGraph& g,
                          const std::set<std::string>& changed_files);

// Reason why a PID is in the rebuild set
struct RebuildReason {
    std::string file;   // trigger file
    int source_pid;     // 0=user-specified changed file, non-0=PID that outputs this file

    RebuildReason() : source_pid(0) {}
    RebuildReason(const std::string& f, int src) : file(f), source_pid(src) {}
};

// BFS with reasons: returns map from affected PID to its rebuild reasons
std::map<int, std::vector<RebuildReason> > rebuild_bfs_reasons(
    const DependencyGraph& g,
    const std::set<std::string>& changed_files);

// --- Process I/O classification ---

struct ProcessIO {
    std::set<std::string> inputs;
    std::set<std::string> outputs;
    std::set<std::string> internal;
};

void classify_process_io(Database& db, const DependencyGraph& g,
                         int pid, bool tree, ProcessIO& result);

// --- Impact ranking ---

struct ImpactEntry {
    std::string file;
    int affected_count;
    int64_t affected_duration_us;
};

void compute_impact(const DependencyGraph& g,
                    std::vector<ImpactEntry>& result, int top_n);

// --- Race detection ---

struct RaceEntry {
    std::string file;
    int writer_pid;
    int reader_pid;
    int64_t overlap_us;
};

void detect_races(const DependencyGraph& g, std::vector<RaceEntry>& result);

// --- Rebuild estimate ---

struct RebuildResult {
    std::vector<ProcessRecord> processes;  // filtered leaf processes
    int total_affected;                    // total before filtering
    // For collapsed processes: PID -> list of hidden descendant PIDs it represents
    std::map<int, std::vector<int> > collapsed_children;
};

// Filter affected PIDs to minimal rebuild set (leaf processes with outputs).
// collapse_names: command names whose subtrees should be collapsed.
RebuildResult filter_rebuild_set(const DependencyGraph& g,
                                 const std::set<int>& affected,
                                 const std::set<std::string>& collapse_names);

struct RebuildEstimate {
    int affected_count;
    int64_t serial_estimate_us;
    int64_t longest_single_us;
};

void compute_rebuild_estimate(const DependencyGraph& g,
                              const std::set<int>& affected,
                              RebuildEstimate& est);

// --- Reverse dependencies ---

struct RdepsNode {
    std::string file;
    int via_pid;
    std::vector<RdepsNode> children;

    RdepsNode() : via_pid(0) {}
};

void compute_rdeps(const DependencyGraph& g, const std::string& file,
                   int depth, RdepsNode& root);

} // namespace bdtrace

#endif // BDTRACE_VIEW_HELPERS_H
