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

std::string cmd_name(const std::string& cmdline) {
    std::string first = cmdline;
    size_t sp = first.find(' ');
    if (sp != std::string::npos) first = first.substr(0, sp);
    size_t sl = first.rfind('/');
    if (sl != std::string::npos) first = first.substr(sl + 1);
    return first;
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
        }
    }
    for (size_t i = 0; i < all_procs.size(); ++i) {
        g.proc_map[all_procs[i].pid] = all_procs[i];
        g.pid_children[all_procs[i].ppid].push_back(all_procs[i].pid);
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
            if (g.file_to_readers.empty()) continue;
            bool child_reads = false;
            for (std::map<std::string, std::set<int> >::const_iterator fit =
                     g.file_to_readers.begin(); fit != g.file_to_readers.end(); ++fit) {
                if (fit->second.find(child) != fit->second.end()) {
                    child_reads = true;
                    break;
                }
            }
            if (!child_reads) continue;

            // Propagate parent's outputs to this child
            const std::set<std::string>& pouts = parent_out->second;
            for (std::set<std::string>::const_iterator fi = pouts.begin();
                 fi != pouts.end(); ++fi) {
                g.pid_to_outputs[child].insert(*fi);
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

    for (std::set<int>::const_iterator pi = pids.begin();
         pi != pids.end(); ++pi) {
        std::vector<FileAccessRecord> acc;
        db.get_file_accesses_by_pid(*pi, acc);
        for (size_t i = 0; i < acc.size(); ++i) {
            if (is_input_mode(acc[i].mode)) {
                reads.insert(acc[i].filename);
            }
            if (is_output_mode(acc[i].mode)) {
                writes.insert(acc[i].filename);
            }
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
                    std::vector<ImpactEntry>& result, int top_n) {
    // Find source files: files that are read but never written
    std::set<std::string> all_outputs;
    for (std::map<int, std::set<std::string> >::const_iterator it =
             g.pid_to_outputs.begin(); it != g.pid_to_outputs.end(); ++it) {
        for (std::set<std::string>::const_iterator fi = it->second.begin();
             fi != it->second.end(); ++fi) {
            all_outputs.insert(*fi);
        }
    }

    std::set<std::string> source_files;
    for (std::map<std::string, std::set<int> >::const_iterator it =
             g.file_to_readers.begin(); it != g.file_to_readers.end(); ++it) {
        if (all_outputs.find(it->first) == all_outputs.end()) {
            source_files.insert(it->first);
        }
    }

    for (std::set<std::string>::const_iterator si = source_files.begin();
         si != source_files.end(); ++si) {
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

static bool is_ancestor(const DependencyGraph& g, int ancestor, int descendant) {
    std::set<int> visited;
    int cur = descendant;
    while (true) {
        if (cur == ancestor) return true;
        if (visited.find(cur) != visited.end()) return false;
        visited.insert(cur);
        std::map<int, ProcessRecord>::const_iterator pit = g.proc_map.find(cur);
        if (pit == g.proc_map.end()) return false;
        cur = pit->second.ppid;
    }
}

void detect_races(const DependencyGraph& g, std::vector<RaceEntry>& result) {
    // For each file, collect writers and readers with their time ranges
    // writer = process that outputs the file
    // reader = process that reads the file (from file_to_readers)

    for (std::map<std::string, std::set<int> >::const_iterator fit =
             g.file_to_readers.begin(); fit != g.file_to_readers.end(); ++fit) {
        const std::string& file = fit->first;
        const std::set<int>& readers = fit->second;

        // Find writers for this file
        std::set<int> writers;
        for (std::map<int, std::set<std::string> >::const_iterator pit =
                 g.pid_to_outputs.begin(); pit != g.pid_to_outputs.end(); ++pit) {
            if (pit->second.find(file) != pit->second.end()) {
                writers.insert(pit->first);
            }
        }
        if (writers.empty()) continue;

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

                // Skip parent-child relationships
                if (is_ancestor(g, *wi, *ri) || is_ancestor(g, *ri, *wi))
                    continue;

                RaceEntry entry;
                entry.file = file;
                entry.writer_pid = *wi;
                entry.reader_pid = *ri;
                entry.overlap_us = overlap;
                result.push_back(entry);
            }
        }
    }
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

    // Collapse: find processes matching collapse_names, hide their descendants
    std::set<int> collapsed_pids;
    std::set<int> hidden_pids;
    for (std::set<int>::const_iterator it = affected.begin();
         it != affected.end(); ++it) {
        std::map<int, ProcessRecord>::const_iterator pit = g.proc_map.find(*it);
        if (pit == g.proc_map.end()) continue;
        if (collapse_names.find(cmd_name(pit->second.cmdline)) != collapse_names.end()) {
            collapsed_pids.insert(*it);
        }
    }
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

    for (std::set<int>::const_iterator it = affected.begin();
         it != affected.end(); ++it) {
        if (hidden_pids.find(*it) != hidden_pids.end()) continue;
        if (collapsed_pids.find(*it) != collapsed_pids.end()) {
            res.processes.push_back(g.proc_map.find(*it)->second);
            continue;
        }
        if (g.pid_to_outputs.find(*it) != g.pid_to_outputs.end()
            && has_output_child.find(*it) == has_output_child.end()) {
            res.processes.push_back(g.proc_map.find(*it)->second);
        }
    }
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
