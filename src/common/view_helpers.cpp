#include "view_helpers.h"
#include "../db/database.h"
#include <algorithm>
#include <queue>

namespace bdtrace {

const char* mode_str(int mode) {
    switch (mode) {
        case FA_READ:           return "R";
        case FA_WRITE:          return "W";
        case FA_RDWR:           return "RW";
        case FA_STAT:           return "ST";
        case FA_ACCESS:         return "AC";
        case FA_EXEC:           return "EX";
        case FA_UNLINK:         return "UL";
        case FA_RENAME_SRC:     return "RS";
        case FA_RENAME_DST:     return "RD";
        case FA_READLINK:       return "RL";
        case FA_MKDIR:          return "MD";
        case FA_LINK_SRC:       return "LS";
        case FA_LINK_DST:       return "LD";
        case FA_SYMLINK_TARGET: return "SY";
        case FA_SYMLINK_LINK:   return "SL";
        case FA_CHMOD:          return "CM";
        case FA_CHOWN:          return "CO";
        case FA_TRUNCATE:       return "TR";
        case FA_MKNOD:          return "MN";
        case FA_UTIMENS:        return "UT";
        case FA_CHDIR:          return "CD";
        default:                return "?";
    }
}

bool is_input_mode(int mode) {
    return mode == FA_READ || mode == FA_RDWR || mode == FA_EXEC
        || mode == FA_STAT || mode == FA_ACCESS || mode == FA_READLINK;
}

bool is_output_mode(int mode) {
    return mode == FA_WRITE || mode == FA_RDWR || mode == FA_RENAME_DST
        || mode == FA_LINK_DST || mode == FA_SYMLINK_LINK
        || mode == FA_TRUNCATE || mode == FA_MKNOD || mode == FA_MKDIR;
}

static std::string basename_of(const std::string& path) {
    size_t sl = path.rfind('/');
    if (sl != std::string::npos) return path.substr(sl + 1);
    return path;
}

static bool is_interpreter(const std::string& name) {
    return name == "perl" || name == "python" || name == "python2"
        || name == "python3" || name == "ruby" || name == "wish"
        || name == "tclsh" || name == "lua" || name == "node"
        || name == "sh" || name == "bash" || name == "dash"
        || name == "zsh" || name == "ksh" || name == "csh"
        || name == "tcsh" || name == "env"
        || name == "awk" || name == "gawk" || name == "mawk" || name == "nawk"
        || name == "sed" || name == "gsed"
        || name == "m4" || name == "gm4"
        || name == "java" || name == "groovy" || name == "gradle"
        || name == "cmake" || name == "ctest"
        || name == "php";
}

// Interpreters where the first non-flag argument is code, not a script file.
// Script file is specified via -f flag only.
static bool is_code_first_interpreter(const std::string& name) {
    return name == "awk" || name == "gawk" || name == "mawk" || name == "nawk"
        || name == "sed" || name == "gsed";
}

std::string cmd_name(const std::string& cmdline) {
    std::string first = cmdline;
    size_t sp = first.find(' ');
    if (sp != std::string::npos) first = first.substr(0, sp);
    std::string exe = basename_of(first);

    if (!is_interpreter(exe) || sp == std::string::npos)
        return exe;

    // For interpreters, find the script argument (skip flags starting with -)
    std::string rest = cmdline.substr(sp + 1);

    // Handle "env [-flags] [VAR=val...] cmd [args...]" by recursing
    if (exe == "env") {
        // Skip env options and VAR=val assignments
        size_t pos = 0;
        while (pos < rest.size()) {
            // skip leading spaces
            while (pos < rest.size() && rest[pos] == ' ') ++pos;
            if (pos >= rest.size()) break;
            if (rest[pos] == '-') {
                // skip flag token
                while (pos < rest.size() && rest[pos] != ' ') ++pos;
            } else if (rest.find('=', pos) != std::string::npos
                       && (rest.find(' ', pos) == std::string::npos
                           || rest.find('=', pos) < rest.find(' ', pos))) {
                // skip VAR=val token
                while (pos < rest.size() && rest[pos] != ' ') ++pos;
            } else {
                break;
            }
        }
        if (pos < rest.size()) {
            return cmd_name(rest.substr(pos));
        }
        return exe;
    }

    // For awk/sed: first non-flag arg is code, not a script.
    // Only -f <file> specifies a script file.
    if (is_code_first_interpreter(exe)) {
        size_t pos = 0;
        while (pos < rest.size()) {
            while (pos < rest.size() && rest[pos] == ' ') ++pos;
            if (pos >= rest.size() || rest[pos] != '-') break;
            size_t fstart = pos;
            while (pos < rest.size() && rest[pos] != ' ') ++pos;
            std::string flag = rest.substr(fstart, pos - fstart);
            if (flag == "-f") {
                // Next token is the script file
                while (pos < rest.size() && rest[pos] == ' ') ++pos;
                if (pos >= rest.size()) return exe;
                size_t end = rest.find(' ', pos);
                std::string script = (end != std::string::npos)
                    ? rest.substr(pos, end - pos) : rest.substr(pos);
                std::string script_base = basename_of(script);
                return script_base.empty() ? exe : script_base;
            }
        }
        // No -f found: first non-flag arg is code expression, return exe
        return exe;
    }

    // Determine which flags mean "inline code" (no script file follows)
    // and which flags consume the next token as their argument.
    bool shell = (exe == "sh" || exe == "bash" || exe == "dash"
               || exe == "zsh" || exe == "ksh" || exe == "csh" || exe == "tcsh");

    size_t pos = 0;
    while (pos < rest.size()) {
        while (pos < rest.size() && rest[pos] == ' ') ++pos;
        if (pos >= rest.size() || rest[pos] != '-') break;
        size_t fstart = pos;
        while (pos < rest.size() && rest[pos] != ' ') ++pos;
        std::string flag = rest.substr(fstart, pos - fstart);
        // For shells: -c means inline code, return exe
        if (shell && flag == "-c") return exe;
        // For non-shells (perl/ruby/python/node): -e/-c means inline code
        if (!shell && (flag == "-e" || flag == "-c")) return exe;
        // Flags that consume the next token as their argument (skip it)
        // These are interpreter-specific; shells have very few.
        bool has_arg = false;
        if (!shell) {
            // perl/python/ruby/node flags that take an argument
            has_arg = (flag == "-I" || flag == "-m" || flag == "-M"
                    || flag == "-x" || flag == "-W" || flag == "-X"
                    || flag == "-o" || flag == "-O");
        }
        if (has_arg) {
            while (pos < rest.size() && rest[pos] == ' ') ++pos;
            while (pos < rest.size() && rest[pos] != ' ') ++pos;
        }
    }
    if (pos >= rest.size()) return exe;

    // Extract the script name token
    size_t end = rest.find(' ', pos);
    std::string script = (end != std::string::npos)
        ? rest.substr(pos, end - pos)
        : rest.substr(pos);
    std::string script_base = basename_of(script);

    if (script_base.empty()) return exe;
    return script_base;
}

static std::string resolve_path(const std::string& path, const std::string& cwd) {
    if (path.empty()) return path;
    if (path[0] == '/') return path;  // already absolute
    if (cwd.empty()) return path;
    std::string base = cwd;
    if (!base.empty() && base[base.size() - 1] != '/') base += '/';
    return base + path;
}

// Extract the script argument token from cmdline (same logic as cmd_name
// but returns the raw token before basename extraction).
static std::string script_token(const std::string& cmdline) {
    std::string first = cmdline;
    size_t sp = first.find(' ');
    if (sp == std::string::npos) return "";
    first = first.substr(0, sp);
    std::string exe = basename_of(first);

    if (!is_interpreter(exe)) return "";

    std::string rest = cmdline.substr(sp + 1);

    // Handle env
    if (exe == "env") {
        size_t pos = 0;
        while (pos < rest.size()) {
            while (pos < rest.size() && rest[pos] == ' ') ++pos;
            if (pos >= rest.size()) break;
            if (rest[pos] == '-') {
                while (pos < rest.size() && rest[pos] != ' ') ++pos;
            } else if (rest.find('=', pos) != std::string::npos
                       && (rest.find(' ', pos) == std::string::npos
                           || rest.find('=', pos) < rest.find(' ', pos))) {
                while (pos < rest.size() && rest[pos] != ' ') ++pos;
            } else {
                break;
            }
        }
        if (pos < rest.size()) {
            return script_token(rest.substr(pos));
        }
        return "";
    }

    // For awk/sed: only -f <file> specifies a script
    if (is_code_first_interpreter(exe)) {
        size_t pos = 0;
        while (pos < rest.size()) {
            while (pos < rest.size() && rest[pos] == ' ') ++pos;
            if (pos >= rest.size() || rest[pos] != '-') break;
            size_t fstart = pos;
            while (pos < rest.size() && rest[pos] != ' ') ++pos;
            std::string flag = rest.substr(fstart, pos - fstart);
            if (flag == "-f") {
                while (pos < rest.size() && rest[pos] == ' ') ++pos;
                if (pos >= rest.size()) return "";
                size_t end = rest.find(' ', pos);
                return (end != std::string::npos) ? rest.substr(pos, end - pos) : rest.substr(pos);
            }
        }
        return "";
    }

    // Skip interpreter flags (same logic as cmd_name)
    bool shell = (exe == "sh" || exe == "bash" || exe == "dash"
               || exe == "zsh" || exe == "ksh" || exe == "csh" || exe == "tcsh");
    size_t pos = 0;
    while (pos < rest.size()) {
        while (pos < rest.size() && rest[pos] == ' ') ++pos;
        if (pos >= rest.size() || rest[pos] != '-') break;
        size_t fstart = pos;
        while (pos < rest.size() && rest[pos] != ' ') ++pos;
        std::string flag = rest.substr(fstart, pos - fstart);
        if (shell && flag == "-c") return "";
        if (!shell && (flag == "-e" || flag == "-c")) return "";
        bool has_arg = false;
        if (!shell) {
            has_arg = (flag == "-I" || flag == "-m" || flag == "-M"
                    || flag == "-x" || flag == "-W" || flag == "-X"
                    || flag == "-o" || flag == "-O");
        }
        if (has_arg) {
            while (pos < rest.size() && rest[pos] == ' ') ++pos;
            while (pos < rest.size() && rest[pos] != ' ') ++pos;
        }
    }
    if (pos >= rest.size()) return "";

    size_t end = rest.find(' ', pos);
    return (end != std::string::npos) ? rest.substr(pos, end - pos) : rest.substr(pos);
}

std::string cmd_name_full(const std::string& cmdline, const std::string& cwd) {
    std::string token = script_token(cmdline);
    if (token.empty()) {
        // Non-interpreter: return basename of executable
        return cmd_name(cmdline);
    }
    return resolve_path(token, cwd);
}

std::string shorten_cmd(const std::string& cmdline, size_t max_len) {
    if (cmdline.size() <= max_len) return cmdline;
    if (max_len <= 3) return cmdline.substr(0, max_len);
    return cmdline.substr(0, max_len - 3) + "...";
}

const char* errno_name(int e) {
    switch (e) {
        case 1:  return "EPERM";
        case 2:  return "ENOENT";
        case 13: return "EACCES";
        case 17: return "EEXIST";
        case 20: return "ENOTDIR";
        case 21: return "EISDIR";
        case 22: return "EINVAL";
        case 36: return "ENAMETOOLONG";
        case 40: return "ELOOP";
        default: return "";
    }
}

// --- Run compaction ---

static bool proc_start_less(const ProcessRecord& a, const ProcessRecord& b) {
    return a.start_time_us < b.start_time_us;
}

struct CompactRun {
    size_t first;
    size_t last;
    int64_t min_start;
    int64_t max_end;
};

void compact_runs(std::vector<ProcessRecord>& procs) {
    if (procs.size() <= 1) return;

    // Sort by start_time to detect run boundaries
    std::vector<ProcessRecord> sorted = procs;
    std::sort(sorted.begin(), sorted.end(), proc_start_less);

    // Detect runs: a new run starts when start_time > running max_end + threshold
    static const int64_t GAP_THRESHOLD_US = 1000000; // 1 second

    std::vector<CompactRun> runs;
    CompactRun cur;
    cur.first = 0;
    cur.last = 0;
    cur.min_start = sorted[0].start_time_us;
    cur.max_end = sorted[0].end_time_us;

    for (size_t i = 1; i < sorted.size(); ++i) {
        if (sorted[i].start_time_us > cur.max_end + GAP_THRESHOLD_US) {
            cur.last = i - 1;
            runs.push_back(cur);
            cur.first = i;
            cur.min_start = sorted[i].start_time_us;
            cur.max_end = sorted[i].end_time_us;
        } else {
            if (sorted[i].end_time_us > cur.max_end) {
                cur.max_end = sorted[i].end_time_us;
            }
        }
    }
    cur.last = sorted.size() - 1;
    runs.push_back(cur);

    if (runs.size() <= 1) return;

    // Build PID -> shift map from sorted order
    std::map<int, int64_t> pid_shift;
    int64_t cumulative_gap = 0;
    for (size_t r = 1; r < runs.size(); ++r) {
        cumulative_gap += runs[r].min_start - runs[r - 1].max_end;
        for (size_t i = runs[r].first; i <= runs[r].last; ++i) {
            pid_shift[sorted[i].pid] = cumulative_gap;
        }
    }

    // Apply shifts to original vector
    for (size_t i = 0; i < procs.size(); ++i) {
        std::map<int, int64_t>::const_iterator it = pid_shift.find(procs[i].pid);
        if (it != pid_shift.end()) {
            procs[i].start_time_us -= it->second;
            procs[i].end_time_us -= it->second;
        }
    }
}

// --- Dependency graph ---

void build_dependency_graph(Database& db, DependencyGraph& g) {
    std::vector<FileAccessRecord> all_acc;
    std::vector<ProcessRecord> all_procs;
    db.get_all_file_accesses(all_acc);
    db.get_all_processes(all_procs);

    for (size_t i = 0; i < all_acc.size(); ++i) {
        const FileAccessRecord& fa = all_acc[i];
        if (is_input_mode(fa.mode)) {
            g.file_to_readers[fa.filename].insert(fa.pid);
        }
        if (is_output_mode(fa.mode)) {
            g.pid_to_outputs[fa.pid].insert(fa.filename);
            g.file_to_writers[fa.filename].insert(fa.pid);
        }
    }
    for (size_t i = 0; i < all_procs.size(); ++i) {
        g.proc_map[all_procs[i].pid] = all_procs[i];
        g.pid_children[all_procs[i].ppid].push_back(all_procs[i].pid);
    }

    // Pre-compute which PIDs read any file (for efficient shell redirect fixup)
    std::set<int> pid_reads_any;
    for (std::map<std::string, std::set<int> >::const_iterator fit =
             g.file_to_readers.begin(); fit != g.file_to_readers.end(); ++fit) {
        for (std::set<int>::const_iterator sit = fit->second.begin();
             sit != fit->second.end(); ++sit) {
            pid_reads_any.insert(*sit);
        }
    }

    // Shell redirect fixup: when a child process has no outputs but reads
    // files, it is likely using a shell redirect (parent opened the output).
    // Propagate parent's outputs to such children so the dependency chain
    // is not broken.
    for (std::map<int, std::vector<int> >::const_iterator pit =
             g.pid_children.begin(); pit != g.pid_children.end(); ++pit) {
        int parent = pit->first;
        std::map<int, std::set<std::string> >::const_iterator parent_out =
            g.pid_to_outputs.find(parent);
        if (parent_out == g.pid_to_outputs.end()) continue;

        const std::vector<int>& children = pit->second;
        for (size_t i = 0; i < children.size(); ++i) {
            int child = children[i];
            // Only propagate if child has no outputs of its own
            if (g.pid_to_outputs.find(child) != g.pid_to_outputs.end())
                continue;
            // Only propagate if child reads files (is a real worker process)
            bool child_reads = (pid_reads_any.find(child) != pid_reads_any.end());
            if (!child_reads) continue;

            // Propagate parent's outputs to this child
            const std::set<std::string>& pouts = parent_out->second;
            for (std::set<std::string>::const_iterator fi = pouts.begin();
                 fi != pouts.end(); ++fi) {
                g.pid_to_outputs[child].insert(*fi);
                g.file_to_writers[*fi].insert(child);
            }
        }
    }
}

std::set<int> rebuild_bfs(const DependencyGraph& g,
                          const std::set<std::string>& changed_files) {
    std::set<int> affected;
    std::queue<int> worklist;

    for (std::set<std::string>::const_iterator fi = changed_files.begin();
         fi != changed_files.end(); ++fi) {
        std::map<std::string, std::set<int> >::const_iterator rit =
            g.file_to_readers.find(*fi);
        if (rit == g.file_to_readers.end()) continue;
        const std::set<int>& readers = rit->second;
        for (std::set<int>::const_iterator ri = readers.begin();
             ri != readers.end(); ++ri) {
            if (affected.insert(*ri).second) {
                worklist.push(*ri);
            }
        }
    }
    while (!worklist.empty()) {
        int pid = worklist.front();
        worklist.pop();
        std::map<int, std::set<std::string> >::const_iterator oit =
            g.pid_to_outputs.find(pid);
        if (oit == g.pid_to_outputs.end()) continue;
        const std::set<std::string>& outputs = oit->second;
        for (std::set<std::string>::const_iterator fi = outputs.begin();
             fi != outputs.end(); ++fi) {
            std::map<std::string, std::set<int> >::const_iterator rit =
                g.file_to_readers.find(*fi);
            if (rit == g.file_to_readers.end()) continue;
            const std::set<int>& readers = rit->second;
            for (std::set<int>::const_iterator ri = readers.begin();
                 ri != readers.end(); ++ri) {
                if (affected.insert(*ri).second) {
                    worklist.push(*ri);
                }
            }
        }
    }
    return affected;
}

std::map<int, std::vector<RebuildReason> > rebuild_bfs_reasons(
    const DependencyGraph& g,
    const std::set<std::string>& changed_files) {
    std::map<int, std::vector<RebuildReason> > reason_map;
    std::set<int> affected;
    std::queue<int> worklist;

    // Seed phase: PIDs that read a user-specified changed file
    for (std::set<std::string>::const_iterator fi = changed_files.begin();
         fi != changed_files.end(); ++fi) {
        std::map<std::string, std::set<int> >::const_iterator rit =
            g.file_to_readers.find(*fi);
        if (rit == g.file_to_readers.end()) continue;
        const std::set<int>& readers = rit->second;
        for (std::set<int>::const_iterator ri = readers.begin();
             ri != readers.end(); ++ri) {
            if (affected.insert(*ri).second) {
                worklist.push(*ri);
            }
            reason_map[*ri].push_back(RebuildReason(*fi, 0));
        }
    }
    // Propagation phase: PIDs that read outputs of affected PIDs
    while (!worklist.empty()) {
        int pid = worklist.front();
        worklist.pop();
        std::map<int, std::set<std::string> >::const_iterator oit =
            g.pid_to_outputs.find(pid);
        if (oit == g.pid_to_outputs.end()) continue;
        const std::set<std::string>& outputs = oit->second;
        for (std::set<std::string>::const_iterator fi = outputs.begin();
             fi != outputs.end(); ++fi) {
            std::map<std::string, std::set<int> >::const_iterator rit =
                g.file_to_readers.find(*fi);
            if (rit == g.file_to_readers.end()) continue;
            const std::set<int>& readers = rit->second;
            for (std::set<int>::const_iterator ri = readers.begin();
                 ri != readers.end(); ++ri) {
                if (affected.insert(*ri).second) {
                    worklist.push(*ri);
                }
                reason_map[*ri].push_back(RebuildReason(*fi, pid));
            }
        }
    }
    return reason_map;
}

// --- Process I/O classification ---

static void collect_descendant_pids(const DependencyGraph& g, int pid,
                                    std::set<int>& pids) {
    pids.insert(pid);
    std::map<int, std::vector<int> >::const_iterator it =
        g.pid_children.find(pid);
    if (it == g.pid_children.end()) return;
    for (size_t i = 0; i < it->second.size(); ++i) {
        if (pids.find(it->second[i]) == pids.end()) {
            collect_descendant_pids(g, it->second[i], pids);
        }
    }
}

void classify_process_io(Database& db, const DependencyGraph& g,
                         int pid, bool tree, ProcessIO& result) {
    std::set<int> pids;
    if (tree) {
        collect_descendant_pids(g, pid, pids);
    } else {
        pids.insert(pid);
    }

    std::set<std::string> reads;
    std::set<std::string> writes;

    // P1.7: Batch query instead of N+1 per-PID queries
    std::vector<FileAccessRecord> acc;
    db.get_file_accesses_by_pids(pids, acc);
    for (size_t i = 0; i < acc.size(); ++i) {
        if (is_input_mode(acc[i].mode)) {
            reads.insert(acc[i].filename);
        }
        if (is_output_mode(acc[i].mode)) {
            writes.insert(acc[i].filename);
        }
    }

    for (std::set<std::string>::const_iterator it = reads.begin();
         it != reads.end(); ++it) {
        if (writes.find(*it) != writes.end()) {
            result.internal.insert(*it);
        } else {
            result.inputs.insert(*it);
        }
    }
    for (std::set<std::string>::const_iterator it = writes.begin();
         it != writes.end(); ++it) {
        if (reads.find(*it) == reads.end()) {
            result.outputs.insert(*it);
        }
    }
}

// --- Impact ranking ---

static bool cmp_impact_desc(const ImpactEntry& a, const ImpactEntry& b) {
    if (a.affected_duration_us != b.affected_duration_us)
        return a.affected_duration_us > b.affected_duration_us;
    return a.affected_count > b.affected_count;
}

void compute_impact(const DependencyGraph& g,
                    std::vector<ImpactEntry>& result, int top_n,
                    int64_t deadline_us) {
    // P3.8: Optimized impact using reverse propagation
    // Instead of BFS per source file, propagate impact scores backwards

    // Find all outputs set
    std::set<std::string> all_outputs;
    for (std::map<int, std::set<std::string> >::const_iterator it =
             g.pid_to_outputs.begin(); it != g.pid_to_outputs.end(); ++it) {
        for (std::set<std::string>::const_iterator fi = it->second.begin();
             fi != it->second.end(); ++fi) {
            all_outputs.insert(*fi);
        }
    }

    // For each source file, compute impact via BFS (still per-file for
    // correct per-source attribution), but with deadline
    std::set<std::string> source_files;
    for (std::map<std::string, std::set<int> >::const_iterator it =
             g.file_to_readers.begin(); it != g.file_to_readers.end(); ++it) {
        if (all_outputs.find(it->first) == all_outputs.end()) {
            source_files.insert(it->first);
        }
    }

    for (std::set<std::string>::const_iterator si = source_files.begin();
         si != source_files.end(); ++si) {
        if (deadline_us > 0 && now_us() > deadline_us) break;

        std::set<std::string> changed;
        changed.insert(*si);
        std::set<int> affected = rebuild_bfs(g, changed);

        int64_t total_dur = 0;
        for (std::set<int>::const_iterator ai = affected.begin();
             ai != affected.end(); ++ai) {
            std::map<int, ProcessRecord>::const_iterator pit =
                g.proc_map.find(*ai);
            if (pit != g.proc_map.end()) {
                int64_t dur = pit->second.end_time_us - pit->second.start_time_us;
                if (dur > 0) total_dur += dur;
            }
        }

        ImpactEntry entry;
        entry.file = *si;
        entry.affected_count = (int)affected.size();
        entry.affected_duration_us = total_dur;
        result.push_back(entry);
    }

    std::sort(result.begin(), result.end(), cmp_impact_desc);
    if (top_n > 0 && (int)result.size() > top_n) {
        result.resize(top_n);
    }
}

// --- Race detection ---

bool detect_races(const DependencyGraph& g, std::vector<RaceEntry>& result,
                  int64_t deadline_us) {
    // P2.7: Pre-compute DFS in/out timestamps for O(1) ancestor checks
    std::map<int, int> dfs_in, dfs_out;
    {
        int timer = 0;
        // Find roots
        std::set<int> all_pids;
        for (std::map<int, ProcessRecord>::const_iterator it = g.proc_map.begin();
             it != g.proc_map.end(); ++it) {
            all_pids.insert(it->first);
        }
        std::vector<int> roots;
        for (std::map<int, ProcessRecord>::const_iterator it = g.proc_map.begin();
             it != g.proc_map.end(); ++it) {
            if (all_pids.find(it->second.ppid) == all_pids.end()) {
                roots.push_back(it->first);
            }
        }
        // DFS using explicit stack
        std::vector<std::pair<int, bool> > stack; // (pid, entering)
        for (size_t i = 0; i < roots.size(); ++i) {
            stack.push_back(std::make_pair(roots[i], true));
        }
        while (!stack.empty()) {
            std::pair<int, bool> top = stack.back();
            stack.pop_back();
            if (top.second) {
                dfs_in[top.first] = timer++;
                stack.push_back(std::make_pair(top.first, false));
                std::map<int, std::vector<int> >::const_iterator cit =
                    g.pid_children.find(top.first);
                if (cit != g.pid_children.end()) {
                    for (int c = (int)cit->second.size() - 1; c >= 0; --c) {
                        stack.push_back(std::make_pair(cit->second[c], true));
                    }
                }
            } else {
                dfs_out[top.first] = timer++;
            }
        }
    }

    for (std::map<std::string, std::set<int> >::const_iterator fit =
             g.file_to_readers.begin(); fit != g.file_to_readers.end(); ++fit) {
        // P2.4: Deadline check
        if (deadline_us > 0 && now_us() > deadline_us) return false;

        const std::string& file = fit->first;
        const std::set<int>& readers = fit->second;

        std::map<std::string, std::set<int> >::const_iterator wit =
            g.file_to_writers.find(file);
        if (wit == g.file_to_writers.end()) continue;
        const std::set<int>& writers = wit->second;

        for (std::set<int>::const_iterator wi = writers.begin();
             wi != writers.end(); ++wi) {
            std::map<int, ProcessRecord>::const_iterator wp =
                g.proc_map.find(*wi);
            if (wp == g.proc_map.end()) continue;

            for (std::set<int>::const_iterator ri = readers.begin();
                 ri != readers.end(); ++ri) {
                if (*ri == *wi) continue;
                std::map<int, ProcessRecord>::const_iterator rp =
                    g.proc_map.find(*ri);
                if (rp == g.proc_map.end()) continue;

                // Check time overlap
                int64_t w_start = wp->second.start_time_us;
                int64_t w_end = wp->second.end_time_us;
                int64_t r_start = rp->second.start_time_us;
                int64_t r_end = rp->second.end_time_us;

                int64_t overlap_start = w_start > r_start ? w_start : r_start;
                int64_t overlap_end = w_end < r_end ? w_end : r_end;
                int64_t overlap = overlap_end - overlap_start;
                if (overlap <= 0) continue;

                // P2.7: O(1) ancestor check using DFS timestamps
                bool ancestor = false;
                std::map<int, int>::const_iterator wi_in = dfs_in.find(*wi);
                std::map<int, int>::const_iterator wi_out = dfs_out.find(*wi);
                std::map<int, int>::const_iterator ri_in = dfs_in.find(*ri);
                std::map<int, int>::const_iterator ri_out = dfs_out.find(*ri);
                if (wi_in != dfs_in.end() && wi_out != dfs_out.end()
                    && ri_in != dfs_in.end() && ri_out != dfs_out.end()) {
                    // A is ancestor of B iff dfs_in[A] <= dfs_in[B] && dfs_out[A] >= dfs_out[B]
                    if (wi_in->second <= ri_in->second && wi_out->second >= ri_out->second)
                        ancestor = true;
                    if (ri_in->second <= wi_in->second && ri_out->second >= wi_out->second)
                        ancestor = true;
                }
                if (ancestor) continue;

                RaceEntry entry;
                entry.file = file;
                entry.writer_pid = *wi;
                entry.reader_pid = *ri;
                entry.overlap_us = overlap;
                result.push_back(entry);
            }
        }
    }
    return true;
}

// --- Rebuild filtering ---

static bool cmp_start_time(const ProcessRecord& a, const ProcessRecord& b) {
    return a.start_time_us < b.start_time_us;
}

RebuildResult filter_rebuild_set(const DependencyGraph& g,
                                 const std::set<int>& affected,
                                 const std::set<std::string>& collapse_names) {
    RebuildResult res;
    res.total_affected = (int)affected.size();

    // Collapse: find ALL processes matching collapse_names (not just affected),
    // then hide their affected descendants.  The collapse process itself is
    // promoted into the result only when it has affected descendants.
    std::set<int> collapsed_pids;
    std::set<int> hidden_pids;
    for (std::map<int, ProcessRecord>::const_iterator pit = g.proc_map.begin();
         pit != g.proc_map.end(); ++pit) {
        if (collapse_names.find(cmd_name(pit->second.cmdline)) != collapse_names.end()) {
            collapsed_pids.insert(pit->first);
        }
    }
    std::map<int, std::vector<int> > collapsed_children_map;
    for (std::set<int>::const_iterator ci = collapsed_pids.begin();
         ci != collapsed_pids.end(); ++ci) {
        std::queue<int> dq;
        dq.push(*ci);
        while (!dq.empty()) {
            int p = dq.front();
            dq.pop();
            std::map<int, std::vector<int> >::const_iterator ch = g.pid_children.find(p);
            if (ch == g.pid_children.end()) continue;
            for (size_t i = 0; i < ch->second.size(); ++i) {
                int child = ch->second[i];
                if (affected.find(child) != affected.end()) {
                    hidden_pids.insert(child);
                    collapsed_children_map[*ci].push_back(child);
                }
                dq.push(child);
            }
        }
    }

    // Leaf filter: only show processes with outputs that have no children with outputs
    std::set<int> has_output_child;
    for (std::set<int>::const_iterator it = affected.begin();
         it != affected.end(); ++it) {
        if (g.pid_to_outputs.find(*it) == g.pid_to_outputs.end()) continue;
        std::map<int, ProcessRecord>::const_iterator pit = g.proc_map.find(*it);
        if (pit == g.proc_map.end()) continue;
        int ppid = pit->second.ppid;
        if (affected.find(ppid) != affected.end()
            && g.pid_to_outputs.find(ppid) != g.pid_to_outputs.end()) {
            has_output_child.insert(ppid);
        }
    }

    // Determine which collapsed PIDs have affected descendants (to include them)
    std::set<int> collapsed_with_descendants;
    for (std::set<int>::const_iterator ci = collapsed_pids.begin();
         ci != collapsed_pids.end(); ++ci) {
        // Check if any hidden_pid is a descendant of this collapsed pid
        std::queue<int> dq;
        dq.push(*ci);
        bool has_affected_desc = false;
        while (!dq.empty() && !has_affected_desc) {
            int p = dq.front();
            dq.pop();
            std::map<int, std::vector<int> >::const_iterator ch = g.pid_children.find(p);
            if (ch == g.pid_children.end()) continue;
            for (size_t i = 0; i < ch->second.size(); ++i) {
                if (hidden_pids.find(ch->second[i]) != hidden_pids.end()) {
                    has_affected_desc = true;
                    break;
                }
                dq.push(ch->second[i]);
            }
        }
        if (has_affected_desc || affected.find(*ci) != affected.end()) {
            collapsed_with_descendants.insert(*ci);
        }
    }

    for (std::set<int>::const_iterator it = affected.begin();
         it != affected.end(); ++it) {
        if (hidden_pids.find(*it) != hidden_pids.end()) continue;
        if (collapsed_pids.find(*it) != collapsed_pids.end()) {
            // Will be added from collapsed_with_descendants
            continue;
        }
        if (g.pid_to_outputs.find(*it) != g.pid_to_outputs.end()
            && has_output_child.find(*it) == has_output_child.end()) {
            res.processes.push_back(g.proc_map.find(*it)->second);
        }
    }
    // Add collapsed processes (both affected and non-affected with affected descendants)
    for (std::set<int>::const_iterator ci = collapsed_with_descendants.begin();
         ci != collapsed_with_descendants.end(); ++ci) {
        if (hidden_pids.find(*ci) != hidden_pids.end()) continue;
        res.processes.push_back(g.proc_map.find(*ci)->second);
    }
    res.collapsed_children = collapsed_children_map;
    std::sort(res.processes.begin(), res.processes.end(), cmp_start_time);
    return res;
}

// --- Rebuild estimate ---

void compute_rebuild_estimate(const DependencyGraph& g,
                              const std::set<int>& affected,
                              RebuildEstimate& est) {
    est.affected_count = (int)affected.size();
    est.serial_estimate_us = 0;
    est.longest_single_us = 0;

    for (std::set<int>::const_iterator it = affected.begin();
         it != affected.end(); ++it) {
        std::map<int, ProcessRecord>::const_iterator pit =
            g.proc_map.find(*it);
        if (pit == g.proc_map.end()) continue;
        int64_t dur = pit->second.end_time_us - pit->second.start_time_us;
        if (dur > 0) {
            est.serial_estimate_us += dur;
            if (dur > est.longest_single_us) {
                est.longest_single_us = dur;
            }
        }
    }
}

// --- Reverse dependencies ---

static void compute_rdeps_recursive(const DependencyGraph& g,
                                    const std::string& file, int depth,
                                    std::set<std::string>& visited,
                                    RdepsNode& node) {
    node.file = file;
    node.via_pid = 0;
    if (depth <= 0) return;

    std::map<std::string, std::set<int> >::const_iterator rit =
        g.file_to_readers.find(file);
    if (rit == g.file_to_readers.end()) return;

    const std::set<int>& readers = rit->second;
    for (std::set<int>::const_iterator ri = readers.begin();
         ri != readers.end(); ++ri) {
        std::map<int, std::set<std::string> >::const_iterator oit =
            g.pid_to_outputs.find(*ri);
        if (oit == g.pid_to_outputs.end()) continue;

        const std::set<std::string>& outputs = oit->second;
        for (std::set<std::string>::const_iterator fi = outputs.begin();
             fi != outputs.end(); ++fi) {
            if (visited.find(*fi) != visited.end()) continue;
            visited.insert(*fi);

            RdepsNode child;
            child.via_pid = *ri;
            compute_rdeps_recursive(g, *fi, depth - 1, visited, child);
            node.children.push_back(child);
        }
    }
}

void compute_rdeps(const DependencyGraph& g, const std::string& file,
                   int depth, RdepsNode& root) {
    std::set<std::string> visited;
    visited.insert(file);
    compute_rdeps_recursive(g, file, depth, visited, root);
}

} // namespace bdtrace
