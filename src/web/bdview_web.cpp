#include "common/types.h"
#include "common/log.h"
#include "common/string_util.h"
#include "common/view_helpers.h"
#include "db/database.h"
#include "web/json_writer.h"

extern "C" {
#include "mongoose.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>

#ifndef BDVIEW_WEB_DEV_MODE
#include "web/static_assets.h"
#endif

using namespace bdtrace;

static Database* g_db = NULL;
static std::string g_static_dir;

// --- Structs (must be at file scope for C++03 template compatibility) ---
struct FileStat {
    int access_count;
    int read_count;
    int write_count;
    std::set<int> pids;
    FileStat() : access_count(0), read_count(0), write_count(0) {}
};

struct AccumData {
    int total;
    int reads;
    int writes;
    std::set<int> pids;
    AccumData() : total(0), reads(0), writes(0) {}
};

struct HotspotEntry {
    std::string name;
    int total, reads, writes, num_procs;
};

struct GroupStats {
    int count;
    int64_t total_us;
    int64_t max_us;
    int64_t total_cpu_us;
    int64_t max_cpu_us;
    int64_t total_rss_kb;
    int64_t max_rss_kb;
    GroupStats() : count(0), total_us(0), max_us(0), total_cpu_us(0), max_cpu_us(0), total_rss_kb(0), max_rss_kb(0) {}
};

struct DfsEntry {
    int pid;
    int depth;
};

// --- Helpers ---
static bool cmp_start_time(const ProcessRecord& a, const ProcessRecord& b) {
    return a.start_time_us < b.start_time_us;
}

static bool cmp_duration_desc(const ProcessRecord& a, const ProcessRecord& b) {
    return (a.end_time_us - a.start_time_us) > (b.end_time_us - b.start_time_us);
}

static void send_json(struct mg_connection *c, const std::string& json) {
    mg_http_reply(c, 200,
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n",
        "%s", json.c_str());
}

static bool starts_with(const std::string& s, const char* prefix) {
    size_t len = std::strlen(prefix);
    return s.size() >= len && s.compare(0, len, prefix) == 0;
}

static std::string get_query_param(const std::string& query, const char* name) {
    std::string key = std::string(name) + "=";
    size_t pos = query.find(key);
    if (pos == std::string::npos) return "";
    size_t start = pos + key.size();
    size_t end = query.find('&', start);
    std::string val = (end == std::string::npos) ? query.substr(start) : query.substr(start, end - start);
    // URL decode
    std::string decoded;
    for (size_t i = 0; i < val.size(); ++i) {
        if (val[i] == '%' && i + 2 < val.size()) {
            char hex[3] = { val[i+1], val[i+2], 0 };
            decoded += (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (val[i] == '+') {
            decoded += ' ';
        } else {
            decoded += val[i];
        }
    }
    return decoded;
}

static int extract_pid_from_uri(const std::string& uri, const char* prefix, const char* suffix) {
    // e.g. /api/processes/1234/files
    size_t plen = std::strlen(prefix);
    if (!starts_with(uri, prefix)) return -1;
    size_t start = plen;
    size_t end = uri.find('/', start);
    if (end == std::string::npos) end = uri.size();
    std::string pid_str = uri.substr(start, end - start);
    if (suffix && suffix[0]) {
        std::string expected_suffix = suffix;
        if (uri.size() < end + expected_suffix.size()) return -1;
    }
    return std::atoi(pid_str.c_str());
}

// --- API handlers ---

static void handle_summary(struct mg_connection *c) {
    std::vector<ProcessRecord> procs;
    g_db->get_all_processes(procs);

    int proc_count = g_db->get_process_count();
    int file_count = g_db->get_file_access_count();
    int failed_count = g_db->get_failed_access_count();

    int64_t min_time = 0, max_time = 0;
    for (size_t i = 0; i < procs.size(); ++i) {
        if (i == 0 || procs[i].start_time_us < min_time) min_time = procs[i].start_time_us;
        if (procs[i].end_time_us > max_time) max_time = procs[i].end_time_us;
    }
    int64_t total_us = max_time - min_time;

    std::vector<FileAccessRecord> all_fa;
    g_db->get_all_file_accesses(all_fa);
    int read_count = 0, write_count = 0;
    for (size_t i = 0; i < all_fa.size(); ++i) {
        if (is_input_mode(all_fa[i].mode)) ++read_count;
        if (is_output_mode(all_fa[i].mode)) ++write_count;
    }

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

    JsonWriter jw;
    jw.beginObject();
    jw.key("total_time_us").val(total_us);
    jw.key("process_count").val(proc_count);
    jw.key("file_access_count").val(file_count);
    jw.key("failed_count").val(failed_count);
    jw.key("max_parallelism").val(max_parallel);
    jw.key("read_count").val(read_count);
    jw.key("write_count").val(write_count);

    jw.key("slowest").beginArray();
    int top = 5;
    if (top > (int)valid.size()) top = (int)valid.size();
    for (int i = 0; i < top; ++i) {
        int64_t dur = valid[i].end_time_us - valid[i].start_time_us;
        jw.beginObject();
        jw.key("pid").val(valid[i].pid);
        jw.key("cmdline").val(valid[i].cmdline);
        jw.key("duration_us").val(dur);
        jw.endObject();
    }
    jw.endArray();
    jw.endObject();

    send_json(c, jw.str());
}

static void handle_processes(struct mg_connection *c) {
    std::vector<ProcessRecord> procs;
    g_db->get_all_processes(procs);

    // Count file accesses per PID
    std::vector<FileAccessRecord> all_fa;
    g_db->get_all_file_accesses(all_fa);
    std::map<int, int> file_counts;
    for (size_t i = 0; i < all_fa.size(); ++i) {
        file_counts[all_fa[i].pid]++;
    }

    // Count failed accesses per PID
    std::vector<FailedAccessRecord> all_fails;
    g_db->get_all_failed_accesses(all_fails);
    std::map<int, int> fail_counts;
    for (size_t i = 0; i < all_fails.size(); ++i) {
        fail_counts[all_fails[i].pid]++;
    }

    std::set<int> pids;
    std::map<int, std::vector<int> > children_map;
    for (size_t i = 0; i < procs.size(); ++i) {
        pids.insert(procs[i].pid);
        children_map[procs[i].ppid].push_back(procs[i].pid);
    }

    std::vector<int> roots;
    for (size_t i = 0; i < procs.size(); ++i) {
        if (pids.find(procs[i].ppid) == pids.end()) {
            roots.push_back(procs[i].pid);
        }
    }

    JsonWriter jw;
    jw.beginObject();

    jw.key("processes").beginArray();
    for (size_t i = 0; i < procs.size(); ++i) {
        std::map<int, int>::const_iterator fc_it = file_counts.find(procs[i].pid);
        int fc = (fc_it != file_counts.end()) ? fc_it->second : 0;
        jw.beginObject();
        jw.key("pid").val(procs[i].pid);
        jw.key("ppid").val(procs[i].ppid);
        jw.key("cmdline").val(procs[i].cmdline);
        jw.key("start_time_us").val(procs[i].start_time_us);
        jw.key("end_time_us").val(procs[i].end_time_us);
        jw.key("exit_code").val(procs[i].exit_code);
        std::map<int, int>::const_iterator fl_it = fail_counts.find(procs[i].pid);
        int fl = (fl_it != fail_counts.end()) ? fl_it->second : 0;
        jw.key("file_count").val(fc);
        jw.key("fail_count").val(fl);
        jw.key("user_time_us").val(procs[i].user_time_us);
        jw.key("sys_time_us").val(procs[i].sys_time_us);
        jw.key("peak_rss_kb").val(procs[i].peak_rss_kb);
        jw.key("io_read_bytes").val(procs[i].io_read_bytes);
        jw.key("io_write_bytes").val(procs[i].io_write_bytes);
        jw.endObject();
    }
    jw.endArray();

    jw.key("roots").beginArray();
    for (size_t i = 0; i < roots.size(); ++i) jw.val(roots[i]);
    jw.endArray();

    jw.key("children_map").beginObject();
    for (std::map<int, std::vector<int> >::const_iterator it = children_map.begin();
         it != children_map.end(); ++it) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d", it->first);
        jw.key(buf).beginArray();
        for (size_t j = 0; j < it->second.size(); ++j) {
            jw.val(it->second[j]);
        }
        jw.endArray();
    }
    jw.endObject();

    jw.endObject();
    send_json(c, jw.str());
}

static void handle_process_files(struct mg_connection *c, int pid) {
    std::vector<FileAccessRecord> accesses;
    g_db->get_file_accesses_by_pid(pid, accesses);

    JsonWriter jw;
    jw.beginArray();
    for (size_t i = 0; i < accesses.size(); ++i) {
        jw.beginObject();
        jw.key("filename").val(accesses[i].filename);
        jw.key("mode").val((int)accesses[i].mode);
        jw.key("mode_str").val(mode_str(accesses[i].mode));
        jw.key("fd").val(accesses[i].fd);
        jw.key("timestamp_us").val(accesses[i].timestamp_us);
        jw.endObject();
    }
    jw.endArray();
    send_json(c, jw.str());
}

static void handle_process_children(struct mg_connection *c, int pid) {
    std::vector<ProcessRecord> children;
    g_db->get_children(pid, children);

    JsonWriter jw;
    jw.beginArray();
    for (size_t i = 0; i < children.size(); ++i) {
        jw.beginObject();
        jw.key("pid").val(children[i].pid);
        jw.key("ppid").val(children[i].ppid);
        jw.key("cmdline").val(children[i].cmdline);
        jw.key("start_time_us").val(children[i].start_time_us);
        jw.key("end_time_us").val(children[i].end_time_us);
        jw.key("exit_code").val(children[i].exit_code);
        jw.endObject();
    }
    jw.endArray();
    send_json(c, jw.str());
}

static void handle_files(struct mg_connection *c) {
    std::vector<FileAccessRecord> all_fa;
    g_db->get_all_file_accesses(all_fa);

    std::map<std::string, FileStat> file_map;
    for (size_t i = 0; i < all_fa.size(); ++i) {
        FileStat& fs = file_map[all_fa[i].filename];
        fs.access_count++;
        if (is_input_mode(all_fa[i].mode)) fs.read_count++;
        if (is_output_mode(all_fa[i].mode)) fs.write_count++;
        fs.pids.insert(all_fa[i].pid);
    }

    JsonWriter jw;
    jw.beginArray();
    for (std::map<std::string, FileStat>::const_iterator it = file_map.begin();
         it != file_map.end(); ++it) {
        jw.beginObject();
        jw.key("path").val(it->first);
        jw.key("access_count").val(it->second.access_count);
        jw.key("read_count").val(it->second.read_count);
        jw.key("write_count").val(it->second.write_count);
        jw.key("process_count").val((int)it->second.pids.size());
        jw.endObject();
    }
    jw.endArray();
    send_json(c, jw.str());
}

static void write_proc_info(JsonWriter& jw, const ProcessRecord& proc) {
    jw.key("start_time_us").val(proc.start_time_us);
    jw.key("end_time_us").val(proc.end_time_us);
    int64_t dur = proc.end_time_us - proc.start_time_us;
    jw.key("duration_us").val(dur);
    jw.key("exit_code").val(proc.exit_code);
    jw.key("user_time_us").val(proc.user_time_us);
    jw.key("sys_time_us").val(proc.sys_time_us);
    jw.key("peak_rss_kb").val(proc.peak_rss_kb);
    jw.key("io_read_bytes").val(proc.io_read_bytes);
    jw.key("io_write_bytes").val(proc.io_write_bytes);
}

struct PidAccess {
    int pid;
    std::set<int> modes;
    int64_t first_timestamp;
};

static void handle_files_by_path(struct mg_connection *c, const std::string& path) {
    std::vector<FileAccessRecord> accesses;
    g_db->get_file_accesses_by_name(path, accesses);

    // Group by PID: merge modes, keep earliest timestamp
    std::map<int, PidAccess> pid_map;
    for (size_t i = 0; i < accesses.size(); ++i) {
        int pid = accesses[i].pid;
        std::map<int, PidAccess>::iterator it = pid_map.find(pid);
        if (it == pid_map.end()) {
            PidAccess pa;
            pa.pid = pid;
            pa.modes.insert((int)accesses[i].mode);
            pa.first_timestamp = accesses[i].timestamp_us;
            pid_map[pid] = pa;
        } else {
            it->second.modes.insert((int)accesses[i].mode);
            if (accesses[i].timestamp_us < it->second.first_timestamp)
                it->second.first_timestamp = accesses[i].timestamp_us;
        }
    }

    JsonWriter jw;
    jw.beginArray();
    for (std::map<int, PidAccess>::const_iterator it = pid_map.begin();
         it != pid_map.end(); ++it) {
        ProcessRecord proc;
        bool found = g_db->get_process(it->first, proc);
        // Build combined mode string
        std::string modes;
        for (std::set<int>::const_iterator mi = it->second.modes.begin();
             mi != it->second.modes.end(); ++mi) {
            if (!modes.empty()) modes += ",";
            modes += mode_str(*mi);
        }
        jw.beginObject();
        jw.key("pid").val(it->first);
        jw.key("cmdline").val(found ? proc.cmdline : std::string());
        jw.key("mode").val(-1);
        jw.key("mode_str").val(modes);
        jw.key("timestamp_us").val(it->second.first_timestamp);
        if (found) write_proc_info(jw, proc);
        jw.endObject();
    }
    jw.endArray();
    send_json(c, jw.str());
}

static void handle_files_by_prefix(struct mg_connection *c, const std::string& prefix) {
    std::vector<FileAccessRecord> all_fa;
    g_db->get_all_file_accesses(all_fa);

    std::string pfx = prefix;
    if (!pfx.empty() && pfx[pfx.size() - 1] != '/') pfx += '/';

    // Group by (pid, filename): merge modes, keep earliest timestamp
    typedef std::pair<int, std::string> PidFile;
    std::map<PidFile, PidAccess> grouped;
    for (size_t i = 0; i < all_fa.size(); ++i) {
        if (all_fa[i].filename.compare(0, pfx.size(), pfx) != 0) continue;
        PidFile key(all_fa[i].pid, all_fa[i].filename);
        std::map<PidFile, PidAccess>::iterator it = grouped.find(key);
        if (it == grouped.end()) {
            PidAccess pa;
            pa.modes.insert((int)all_fa[i].mode);
            pa.first_timestamp = all_fa[i].timestamp_us;
            grouped[key] = pa;
        } else {
            it->second.modes.insert((int)all_fa[i].mode);
            if (all_fa[i].timestamp_us < it->second.first_timestamp)
                it->second.first_timestamp = all_fa[i].timestamp_us;
        }
    }

    JsonWriter jw;
    jw.beginArray();
    for (std::map<PidFile, PidAccess>::const_iterator it = grouped.begin();
         it != grouped.end(); ++it) {
        ProcessRecord proc;
        bool found = g_db->get_process(it->first.first, proc);
        std::string modes;
        for (std::set<int>::const_iterator mi = it->second.modes.begin();
             mi != it->second.modes.end(); ++mi) {
            if (!modes.empty()) modes += ",";
            modes += mode_str(*mi);
        }
        jw.beginObject();
        jw.key("pid").val(it->first.first);
        jw.key("cmdline").val(found ? proc.cmdline : std::string());
        jw.key("filename").val(it->first.second);
        jw.key("mode_str").val(modes);
        jw.key("timestamp_us").val(it->second.first_timestamp);
        if (found) write_proc_info(jw, proc);
        jw.endObject();
    }
    jw.endArray();
    send_json(c, jw.str());
}

static void handle_slowest(struct mg_connection *c, const std::string& query) {
    std::string group_by = get_query_param(query, "group_by");

    std::vector<ProcessRecord> procs;
    g_db->get_all_processes(procs);

    std::string cmd_filter = get_query_param(query, "cmd");

    if (!cmd_filter.empty()) {
        // Return individual processes matching cmd_name_full
        std::vector<ProcessRecord> matched;
        for (size_t i = 0; i < procs.size(); ++i) {
            if (procs[i].end_time_us <= 0) continue;
            if (cmd_name_full(procs[i].cmdline, procs[i].cwd) == cmd_filter)
                matched.push_back(procs[i]);
        }
        std::sort(matched.begin(), matched.end(), cmp_duration_desc);

        JsonWriter jw;
        jw.beginArray();
        for (size_t i = 0; i < matched.size(); ++i) {
            jw.beginObject();
            jw.key("pid").val(matched[i].pid);
            jw.key("ppid").val(matched[i].ppid);
            jw.key("cmdline").val(matched[i].cmdline);
            write_proc_info(jw, matched[i]);
            jw.endObject();
        }
        jw.endArray();
        send_json(c, jw.str());
    } else if (group_by == "cmd_name") {
        std::map<std::string, GroupStats> groups;
        for (size_t i = 0; i < procs.size(); ++i) {
            if (procs[i].end_time_us <= 0) continue;
            std::string name = cmd_name_full(procs[i].cmdline, procs[i].cwd);
            int64_t dur = procs[i].end_time_us - procs[i].start_time_us;
            int64_t cpu = procs[i].user_time_us + procs[i].sys_time_us;
            GroupStats& gs = groups[name];
            gs.count++;
            gs.total_us += dur;
            if (dur > gs.max_us) gs.max_us = dur;
            gs.total_cpu_us += cpu;
            if (cpu > gs.max_cpu_us) gs.max_cpu_us = cpu;
            gs.total_rss_kb += procs[i].peak_rss_kb;
            if (procs[i].peak_rss_kb > gs.max_rss_kb) gs.max_rss_kb = procs[i].peak_rss_kb;
        }

        JsonWriter jw;
        jw.beginArray();
        for (std::map<std::string, GroupStats>::const_iterator it = groups.begin();
             it != groups.end(); ++it) {
            jw.beginObject();
            jw.key("cmd_name").val(it->first);
            jw.key("count").val(it->second.count);
            jw.key("total_us").val(it->second.total_us);
            jw.key("max_us").val(it->second.max_us);
            jw.key("avg_us").val(it->second.count > 0 ? it->second.total_us / it->second.count : (int64_t)0);
            jw.key("total_cpu_us").val(it->second.total_cpu_us);
            jw.key("max_cpu_us").val(it->second.max_cpu_us);
            jw.key("avg_cpu_us").val(it->second.count > 0 ? it->second.total_cpu_us / it->second.count : (int64_t)0);
            jw.key("max_rss_kb").val(it->second.max_rss_kb);
            jw.endObject();
        }
        jw.endArray();
        send_json(c, jw.str());
    } else {
        std::vector<ProcessRecord> valid;
        for (size_t i = 0; i < procs.size(); ++i) {
            if (procs[i].end_time_us > 0) valid.push_back(procs[i]);
        }
        std::sort(valid.begin(), valid.end(), cmp_duration_desc);

        JsonWriter jw;
        jw.beginArray();
        int n = 20;
        if (n > (int)valid.size()) n = (int)valid.size();
        for (int i = 0; i < n; ++i) {
            int64_t dur = valid[i].end_time_us - valid[i].start_time_us;
            jw.beginObject();
            jw.key("pid").val(valid[i].pid);
            jw.key("cmdline").val(valid[i].cmdline);
            jw.key("duration_us").val(dur);
            jw.endObject();
        }
        jw.endArray();
        send_json(c, jw.str());
    }
}

static void handle_timeline(struct mg_connection *c, const std::string& query) {
    std::string min_dur_str = get_query_param(query, "min_duration_us");
    int64_t min_dur_us = min_dur_str.empty() ? 0 : (int64_t)std::atol(min_dur_str.c_str());

    std::vector<ProcessRecord> procs;
    g_db->get_all_processes(procs);

    if (procs.empty()) {
        send_json(c, "{\"processes\":[],\"min_time_us\":0,\"max_time_us\":0}");
        return;
    }

    int64_t min_time = procs[0].start_time_us;
    int64_t max_time = procs[0].end_time_us;
    for (size_t i = 1; i < procs.size(); ++i) {
        if (procs[i].start_time_us < min_time) min_time = procs[i].start_time_us;
        if (procs[i].end_time_us > max_time) max_time = procs[i].end_time_us;
    }

    // Build tree for DFS ordering
    std::set<int> all_pids;
    std::map<int, std::vector<int> > children;
    std::map<int, ProcessRecord> proc_map;
    for (size_t i = 0; i < procs.size(); ++i) {
        all_pids.insert(procs[i].pid);
        children[procs[i].ppid].push_back(procs[i].pid);
        proc_map[procs[i].pid] = procs[i];
    }

    std::vector<int> roots;
    for (size_t i = 0; i < procs.size(); ++i) {
        if (all_pids.find(procs[i].ppid) == all_pids.end()) {
            roots.push_back(procs[i].pid);
        }
    }

    // DFS to get ordered list with depth
    std::vector<DfsEntry> ordered;
    {
        // Use explicit stack for DFS
        std::vector<std::pair<int, int> > stack; // (pid, depth)
        for (int r = (int)roots.size() - 1; r >= 0; --r) {
            stack.push_back(std::make_pair(roots[r], 0));
        }
        while (!stack.empty()) {
            std::pair<int, int> top = stack.back();
            stack.pop_back();
            DfsEntry e;
            e.pid = top.first;
            e.depth = top.second;
            ordered.push_back(e);

            std::map<int, std::vector<int> >::const_iterator cit = children.find(top.first);
            if (cit != children.end()) {
                for (int c = (int)cit->second.size() - 1; c >= 0; --c) {
                    stack.push_back(std::make_pair(cit->second[c], top.second + 1));
                }
            }
        }
    }

    JsonWriter jw;
    jw.beginObject();
    jw.key("min_time_us").val(min_time);
    jw.key("max_time_us").val(max_time);

    jw.key("processes").beginArray();
    for (size_t i = 0; i < ordered.size(); ++i) {
        std::map<int, ProcessRecord>::const_iterator it = proc_map.find(ordered[i].pid);
        if (it == proc_map.end()) continue;
        const ProcessRecord& p = it->second;
        if (p.end_time_us <= 0) continue;
        int64_t dur = p.end_time_us - p.start_time_us;
        if (dur < min_dur_us) continue;

        jw.beginObject();
        jw.key("pid").val(p.pid);
        jw.key("cmdline").val(p.cmdline);
        jw.key("start_time_us").val(p.start_time_us);
        jw.key("end_time_us").val(p.end_time_us);
        jw.key("exit_code").val(p.exit_code);
        jw.key("depth").val(ordered[i].depth);
        jw.endObject();
    }
    jw.endArray();
    jw.endObject();

    send_json(c, jw.str());
}

static void handle_parallelism(struct mg_connection *c) {
    std::vector<ProcessRecord> procs;
    g_db->get_all_processes(procs);

    if (procs.empty()) {
        send_json(c, "{\"buckets\":[],\"max_parallelism\":0,\"total_time_us\":0}");
        return;
    }

    int64_t min_time = procs[0].start_time_us;
    int64_t max_time = procs[0].end_time_us;
    for (size_t i = 1; i < procs.size(); ++i) {
        if (procs[i].start_time_us < min_time) min_time = procs[i].start_time_us;
        if (procs[i].end_time_us > max_time) max_time = procs[i].end_time_us;
    }

    int64_t total_us = max_time - min_time;
    if (total_us <= 0) {
        send_json(c, "{\"buckets\":[],\"max_parallelism\":0,\"total_time_us\":0}");
        return;
    }

    int64_t bucket_us = 1000000;
    int num_buckets = (int)(total_us / bucket_us) + 1;
    if (num_buckets > 3600) {
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
        for (int b = start_b; b <= end_b; ++b) buckets[b]++;
    }

    int max_parallel = 0;
    for (int i = 0; i < num_buckets; ++i) {
        if (buckets[i] > max_parallel) max_parallel = buckets[i];
    }

    JsonWriter jw;
    jw.beginObject();
    jw.key("total_time_us").val(total_us);
    jw.key("max_parallelism").val(max_parallel);
    jw.key("bucket_us").val(bucket_us);
    jw.key("buckets").beginArray();
    for (int i = 0; i < num_buckets; ++i) jw.val(buckets[i]);
    jw.endArray();
    jw.endObject();

    send_json(c, jw.str());
}

static void handle_critical_path(struct mg_connection *c) {
    std::vector<ProcessRecord> procs;
    g_db->get_all_processes(procs);

    if (procs.empty()) {
        send_json(c, "{\"path\":[],\"total_us\":0}");
        return;
    }

    std::map<int, ProcessRecord> proc_map;
    std::map<int, std::vector<int> > children;
    std::set<int> all_pids;
    for (size_t i = 0; i < procs.size(); ++i) {
        proc_map[procs[i].pid] = procs[i];
        children[procs[i].ppid].push_back(procs[i].pid);
        all_pids.insert(procs[i].pid);
    }

    std::vector<int> roots;
    for (size_t i = 0; i < procs.size(); ++i) {
        if (all_pids.find(procs[i].ppid) == all_pids.end()) roots.push_back(procs[i].pid);
    }

    int critical_root = roots[0];
    int64_t max_span = 0;
    for (size_t i = 0; i < roots.size(); ++i) {
        int64_t span = proc_map[roots[i]].end_time_us - proc_map[roots[i]].start_time_us;
        if (span > max_span) { max_span = span; critical_root = roots[i]; }
    }

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
            if (end > best_end) { best_end = end; best_child = cit->second[i]; }
        }
        cur = best_child;
    }

    int64_t total_us = proc_map[critical_root].end_time_us - proc_map[critical_root].start_time_us;

    JsonWriter jw;
    jw.beginObject();
    jw.key("total_us").val(total_us);
    jw.key("path").beginArray();
    for (size_t i = 0; i < path.size(); ++i) {
        int64_t dur = path[i].end_time_us - path[i].start_time_us;
        jw.beginObject();
        jw.key("pid").val(path[i].pid);
        jw.key("cmdline").val(path[i].cmdline);
        jw.key("duration_us").val(dur);
        jw.endObject();
    }
    jw.endArray();
    jw.endObject();

    send_json(c, jw.str());
}

static void handle_hotspots(struct mg_connection *c, const std::string& query) {
    std::string top_str = get_query_param(query, "top");
    std::string by_dir_str = get_query_param(query, "by_dir");
    int top_n = top_str.empty() ? 20 : std::atoi(top_str.c_str());
    bool by_dir = (by_dir_str == "1");

    std::vector<FileAccessRecord> all_fa;
    g_db->get_all_file_accesses(all_fa);

    std::map<std::string, AccumData> accum;
    for (size_t i = 0; i < all_fa.size(); ++i) {
        std::string key = all_fa[i].filename;
        if (by_dir) {
            size_t slash = key.rfind('/');
            if (slash != std::string::npos) key = key.substr(0, slash + 1);
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

    // Sort by total desc
    for (size_t i = 0; i < entries.size(); ++i) {
        for (size_t j = i + 1; j < entries.size(); ++j) {
            if (entries[j].total > entries[i].total) std::swap(entries[i], entries[j]);
        }
    }

    int count = top_n;
    if (count > (int)entries.size()) count = (int)entries.size();

    JsonWriter jw;
    jw.beginArray();
    for (int i = 0; i < count; ++i) {
        jw.beginObject();
        jw.key("name").val(entries[i].name);
        jw.key("total").val(entries[i].total);
        jw.key("reads").val(entries[i].reads);
        jw.key("writes").val(entries[i].writes);
        jw.key("num_procs").val(entries[i].num_procs);
        jw.endObject();
    }
    jw.endArray();

    send_json(c, jw.str());
}

static void handle_failures(struct mg_connection *c, const std::string& query) {
    std::string errno_str = get_query_param(query, "errno");
    int filter_errno = errno_str.empty() ? 0 : std::atoi(errno_str.c_str());

    std::vector<FailedAccessRecord> failures;
    g_db->get_all_failed_accesses(failures);

    if (filter_errno > 0) {
        std::vector<FailedAccessRecord> filtered;
        for (size_t i = 0; i < failures.size(); ++i) {
            if (failures[i].errno_val == filter_errno) filtered.push_back(failures[i]);
        }
        failures = filtered;
    }

    std::map<int, int> errno_counts;
    for (size_t i = 0; i < failures.size(); ++i) {
        errno_counts[failures[i].errno_val]++;
    }

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

    JsonWriter jw;
    jw.beginObject();
    jw.key("total").val((int)failures.size());

    jw.key("by_errno").beginArray();
    for (std::map<int, int>::const_iterator it = errno_counts.begin();
         it != errno_counts.end(); ++it) {
        jw.beginObject();
        jw.key("errno_val").val(it->first);
        jw.key("name").val(errno_name(it->first));
        jw.key("count").val(it->second);
        jw.endObject();
    }
    jw.endArray();

    jw.key("by_file").beginArray();
    int show = 20;
    if (show > (int)sorted_files.size()) show = (int)sorted_files.size();
    for (int i = 0; i < show; ++i) {
        jw.beginObject();
        jw.key("filename").val(sorted_files[i].second);
        jw.key("count").val(-sorted_files[i].first);
        jw.endObject();
    }
    jw.endArray();

    jw.endObject();
    send_json(c, jw.str());
}

static void handle_diagnostics(struct mg_connection *c) {
    std::vector<ProcessRecord> procs;
    g_db->get_all_processes(procs);
    std::vector<FileAccessRecord> all_fa;
    g_db->get_all_file_accesses(all_fa);

    JsonWriter jw;
    jw.beginObject();

    if (procs.empty()) {
        jw.key("issues").beginArray().endArray();
        jw.endObject();
        send_json(c, jw.str());
        return;
    }

    int64_t min_time = procs[0].start_time_us;
    int64_t max_time = procs[0].end_time_us;
    for (size_t i = 1; i < procs.size(); ++i) {
        if (procs[i].start_time_us < min_time) min_time = procs[i].start_time_us;
        if (procs[i].end_time_us > max_time) max_time = procs[i].end_time_us;
    }
    int64_t total_us = max_time - min_time;

    jw.key("total_time_us").val(total_us);
    jw.key("issues").beginArray();

    // 1. Serial execution
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
                ProcessRecord parent;
                if (g_db->get_process(it->first, parent)) {
                    char prefix[64];
                    std::snprintf(prefix, sizeof(prefix), "%d sequential children under [%d] ",
                                  serial_count, it->first);
                    std::string msg = std::string(prefix) + parent.cmdline;
                    char detail[128];
                    std::snprintf(detail, sizeof(detail), "Potential parallelization savings: %s",
                                  format_duration_us(serial_time).c_str());
                    jw.beginObject();
                    jw.key("type").val("SERIAL");
                    jw.key("message").val(msg);
                    jw.key("detail").val(detail);
                    jw.endObject();
                }
            }
        }
    }

    // 2. Slow processes
    {
        int64_t threshold = total_us / 4;
        std::vector<ProcessRecord> sorted = procs;
        std::sort(sorted.begin(), sorted.end(), cmp_duration_desc);
        for (size_t i = 0; i < sorted.size() && i < 5; ++i) {
            int64_t dur = sorted[i].end_time_us - sorted[i].start_time_us;
            if (dur >= threshold) {
                char prefix[128];
                std::snprintf(prefix, sizeof(prefix), "%s (%d%% of total) [%d] ",
                              format_duration_us(dur).c_str(),
                              (int)(dur * 100 / total_us),
                              sorted[i].pid);
                std::string msg = std::string(prefix) + sorted[i].cmdline;
                jw.beginObject();
                jw.key("type").val("SLOW");
                jw.key("message").val(msg);
                jw.key("detail").val("Process takes >25% of build time");
                jw.endObject();
            }
        }
    }

    // 3. Hotfile
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
            char msg[256];
            std::snprintf(msg, sizeof(msg), "%d files accessed by >10 processes", dup_count);
            char detail[256];
            std::snprintf(detail, sizeof(detail), "Worst: %s (%d processes)", worst_file.c_str(), worst_count);
            jw.beginObject();
            jw.key("type").val("HOTFILE");
            jw.key("message").val(msg);
            jw.key("detail").val(detail);
            jw.endObject();
        }
    }

    // 4. Failed accesses
    {
        int failed = g_db->get_failed_access_count();
        if (failed > 100) {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "%d failed file accesses detected", failed);
            jw.beginObject();
            jw.key("type").val("FAILURES");
            jw.key("message").val(msg);
            jw.key("detail").val("Check the Failures tab for details");
            jw.endObject();
        }
    }

    jw.endArray();
    jw.endObject();
    send_json(c, jw.str());
}

// --- Static file serving ---
#ifndef BDVIEW_WEB_DEV_MODE
static void serve_embedded(struct mg_connection *c, const char* data, unsigned int len, const char* ct) {
    char headers[256];
    std::snprintf(headers, sizeof(headers), "Content-Type: %s\r\n", ct);
    mg_http_reply(c, 200, headers, "%.*s", (int)len, data);
}
#endif

static void serve_static(struct mg_connection *c, struct mg_http_message *hm) {
    std::string uri(hm->uri.buf, hm->uri.len);
    if (uri == "/") uri = "/index.html";

    // Dev mode: serve from filesystem
    if (!g_static_dir.empty()) {
        std::string path = g_static_dir + uri;
        // Use mongoose's built-in file serving
        struct mg_http_serve_opts opts;
        std::memset(&opts, 0, sizeof(opts));
        opts.root_dir = g_static_dir.c_str();
        mg_http_serve_dir(c, hm, &opts);
        return;
    }

#ifndef BDVIEW_WEB_DEV_MODE
    // Embedded mode
    if (uri == "/index.html") {
        serve_embedded(c, asset_index_html, asset_index_html_len, "text/html");
    } else if (uri == "/app.css") {
        serve_embedded(c, asset_app_css, asset_app_css_len, "text/css");
    } else if (uri == "/app.js") {
        serve_embedded(c, asset_app_js, asset_app_js_len, "application/javascript");
    } else if (uri == "/timeline.js") {
        serve_embedded(c, asset_timeline_js, asset_timeline_js_len, "application/javascript");
    } else {
        mg_http_reply(c, 404, "", "Not found");
    }
#else
    mg_http_reply(c, 404, "", "Not found (dev mode requires --static-dir)");
#endif
}

// --- New analysis handlers ---

static void handle_process_io(struct mg_connection *c, int pid,
                               const std::string& query) {
    bool tree = get_query_param(query, "tree") == "1";
    DependencyGraph g;
    build_dependency_graph(*g_db, g);
    ProcessIO pio;
    classify_process_io(*g_db, g, pid, tree, pio);

    JsonWriter w;
    w.beginObject();
    w.key("inputs").beginArray();
    for (std::set<std::string>::const_iterator it = pio.inputs.begin();
         it != pio.inputs.end(); ++it) w.val(*it);
    w.endArray();
    w.key("outputs").beginArray();
    for (std::set<std::string>::const_iterator it = pio.outputs.begin();
         it != pio.outputs.end(); ++it) w.val(*it);
    w.endArray();
    w.key("internal").beginArray();
    for (std::set<std::string>::const_iterator it = pio.internal.begin();
         it != pio.internal.end(); ++it) w.val(*it);
    w.endArray();
    w.endObject();
    send_json(c, w.str());
}

static void handle_impact(struct mg_connection *c, const std::string& query) {
    int top_n = 20;
    std::string top_s = get_query_param(query, "top");
    if (!top_s.empty()) top_n = std::atoi(top_s.c_str());

    DependencyGraph g;
    build_dependency_graph(*g_db, g);
    std::vector<ImpactEntry> entries;
    compute_impact(g, entries, top_n);

    JsonWriter w;
    w.beginArray();
    for (size_t i = 0; i < entries.size(); ++i) {
        w.beginObject();
        w.key("file").val(entries[i].file);
        w.key("affected_procs").val(entries[i].affected_count);
        w.key("affected_duration_us").val(entries[i].affected_duration_us);
        w.endObject();
    }
    w.endArray();
    send_json(c, w.str());
}

static void handle_races(struct mg_connection *c) {
    DependencyGraph g;
    build_dependency_graph(*g_db, g);
    std::vector<RaceEntry> races;
    detect_races(g, races);

    JsonWriter w;
    w.beginObject();
    w.key("count").val((int)races.size());
    w.key("races").beginArray();
    for (size_t i = 0; i < races.size(); ++i) {
        w.beginObject();
        w.key("file").val(races[i].file);
        w.key("writer_pid").val(races[i].writer_pid);
        w.key("reader_pid").val(races[i].reader_pid);
        w.key("overlap_us").val(races[i].overlap_us);
        std::map<int, ProcessRecord>::const_iterator wp =
            g.proc_map.find(races[i].writer_pid);
        std::map<int, ProcessRecord>::const_iterator rp =
            g.proc_map.find(races[i].reader_pid);
        w.key("writer_cmd").val(wp != g.proc_map.end() ? wp->second.cmdline : "");
        w.key("reader_cmd").val(rp != g.proc_map.end() ? rp->second.cmdline : "");
        w.endObject();
    }
    w.endArray();
    w.endObject();
    send_json(c, w.str());
}

static void handle_rebuild_api(struct mg_connection *c, const std::string& query) {
    std::string changed_str = get_query_param(query, "changed");
    if (changed_str.empty()) {
        mg_http_reply(c, 400, "", "changed parameter required");
        return;
    }

    DependencyGraph g;
    build_dependency_graph(*g_db, g);

    // Parse comma-separated changed files
    std::set<std::string> changed_files;
    size_t start = 0;
    while (start < changed_str.size()) {
        size_t comma = changed_str.find(',', start);
        if (comma == std::string::npos) comma = changed_str.size();
        std::string f = changed_str.substr(start, comma - start);
        if (!f.empty()) {
            if (g.file_to_readers.find(f) != g.file_to_readers.end()) {
                changed_files.insert(f);
            }
            std::string prefix = f;
            if (!prefix.empty() && prefix[prefix.size() - 1] != '/') prefix += '/';
            std::map<std::string, std::set<int> >::const_iterator it =
                g.file_to_readers.lower_bound(prefix);
            while (it != g.file_to_readers.end()) {
                if (it->first.compare(0, prefix.size(), prefix) != 0) break;
                changed_files.insert(it->first);
                ++it;
            }
        }
        start = comma + 1;
    }

    std::set<int> affected = rebuild_bfs(g, changed_files);

    // Parse optional collapse parameter (comma-separated command names)
    std::set<std::string> collapse_names;
    std::string collapse_str = get_query_param(query, "collapse");
    if (!collapse_str.empty()) {
        size_t cs = 0;
        while (cs < collapse_str.size()) {
            size_t cc = collapse_str.find(',', cs);
            if (cc == std::string::npos) cc = collapse_str.size();
            std::string name = collapse_str.substr(cs, cc - cs);
            if (!name.empty()) collapse_names.insert(name);
            cs = cc + 1;
        }
    }

    // Apply same filtering as CLI (leaf processes with outputs)
    RebuildResult rr = filter_rebuild_set(g, affected, collapse_names);

    RebuildEstimate est;
    compute_rebuild_estimate(g, affected, est);

    // Find trace-wide min start time (same baseline as Processes tab)
    int64_t trace_min_start = 0;
    for (std::map<int, ProcessRecord>::const_iterator pit = g.proc_map.begin();
         pit != g.proc_map.end(); ++pit) {
        if (trace_min_start == 0 || pit->second.start_time_us < trace_min_start) {
            trace_min_start = pit->second.start_time_us;
        }
    }

    JsonWriter w;
    w.beginObject();
    w.key("affected_count").val((int)rr.processes.size());
    w.key("total_affected").val(rr.total_affected);
    w.key("trace_min_start_us").val(trace_min_start);
    w.key("serial_estimate_us").val(est.serial_estimate_us);
    w.key("longest_single_us").val(est.longest_single_us);
    w.key("affected").beginArray();
    for (size_t i = 0; i < rr.processes.size(); ++i) {
        w.beginObject();
        int64_t dur = rr.processes[i].end_time_us - rr.processes[i].start_time_us;
        if (dur < 0) dur = 0;
        w.key("pid").val(rr.processes[i].pid);
        w.key("cmdline").val(rr.processes[i].cmdline);
        w.key("cwd").val(rr.processes[i].cwd);
        w.key("start_time_us").val(rr.processes[i].start_time_us);
        w.key("duration_us").val(dur);
        w.endObject();
    }
    w.endArray();
    w.endObject();
    send_json(c, w.str());
}

static void write_rdeps_json(JsonWriter& w, const DependencyGraph& g,
                              const RdepsNode& node) {
    w.beginObject();
    w.key("file").val(node.file);
    w.key("via_pid").val(node.via_pid);
    if (node.via_pid != 0) {
        std::map<int, ProcessRecord>::const_iterator pit =
            g.proc_map.find(node.via_pid);
        w.key("via_cmd").val(pit != g.proc_map.end() ? pit->second.cmdline : "");
    }
    w.key("children").beginArray();
    for (size_t i = 0; i < node.children.size(); ++i) {
        write_rdeps_json(w, g, node.children[i]);
    }
    w.endArray();
    w.endObject();
}

static void handle_rdeps(struct mg_connection *c, const std::string& query) {
    std::string file = get_query_param(query, "file");
    if (file.empty()) {
        mg_http_reply(c, 400, "", "file parameter required");
        return;
    }
    int depth = 3;
    std::string depth_s = get_query_param(query, "depth");
    if (!depth_s.empty()) depth = std::atoi(depth_s.c_str());

    DependencyGraph g;
    build_dependency_graph(*g_db, g);
    RdepsNode root;
    compute_rdeps(g, file, depth, root);

    JsonWriter w;
    write_rdeps_json(w, g, root);
    send_json(c, w.str());
}

// --- Main event handler ---
static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev != MG_EV_HTTP_MSG) return;

    struct mg_http_message *hm = (struct mg_http_message *)ev_data;
    std::string uri(hm->uri.buf, hm->uri.len);
    std::string query(hm->query.buf, hm->query.len);

    if (uri == "/api/summary") {
        handle_summary(c);
    } else if (uri == "/api/processes") {
        handle_processes(c);
    } else if (starts_with(uri, "/api/processes/") && uri.find("/files") != std::string::npos) {
        int pid = extract_pid_from_uri(uri, "/api/processes/", "/files");
        if (pid > 0) handle_process_files(c, pid);
        else mg_http_reply(c, 400, "", "Bad PID");
    } else if (starts_with(uri, "/api/processes/") && uri.find("/children") != std::string::npos) {
        int pid = extract_pid_from_uri(uri, "/api/processes/", "/children");
        if (pid > 0) handle_process_children(c, pid);
        else mg_http_reply(c, 400, "", "Bad PID");
    } else if (uri == "/api/files") {
        handle_files(c);
    } else if (uri == "/api/files/by-path") {
        std::string path = get_query_param(query, "path");
        handle_files_by_path(c, path);
    } else if (uri == "/api/files/by-prefix") {
        std::string prefix = get_query_param(query, "prefix");
        handle_files_by_prefix(c, prefix);
    } else if (uri == "/api/slowest") {
        handle_slowest(c, query);
    } else if (uri == "/api/timeline") {
        handle_timeline(c, query);
    } else if (uri == "/api/parallelism") {
        handle_parallelism(c);
    } else if (uri == "/api/critical-path") {
        handle_critical_path(c);
    } else if (uri == "/api/hotspots") {
        handle_hotspots(c, query);
    } else if (uri == "/api/failures") {
        handle_failures(c, query);
    } else if (uri == "/api/diagnostics") {
        handle_diagnostics(c);
    } else if (starts_with(uri, "/api/processes/") && uri.find("/io") != std::string::npos) {
        int pid = extract_pid_from_uri(uri, "/api/processes/", "/io");
        if (pid > 0) handle_process_io(c, pid, query);
        else mg_http_reply(c, 400, "", "Bad PID");
    } else if (uri == "/api/impact") {
        handle_impact(c, query);
    } else if (uri == "/api/races") {
        handle_races(c);
    } else if (uri == "/api/rebuild") {
        handle_rebuild_api(c, query);
    } else if (uri == "/api/rdeps") {
        handle_rdeps(c, query);
    } else {
        serve_static(c, hm);
    }
}

// --- Main ---
static void usage() {
    std::fprintf(stderr,
        "Usage: bdview-web <database.db> [options]\n"
        "\n"
        "Options:\n"
        "  --port PORT          HTTP port (default: 8080)\n"
        "  --static-dir DIR     Serve static files from DIR (dev mode)\n"
    );
}

int main(int argc, char* argv[]) {
    bdtrace::log_init(bdtrace::LOG_WARN);

    if (argc < 2) {
        usage();
        return 1;
    }

    std::string db_path = argv[1];
    int port = 8080;

    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--static-dir") == 0 && i + 1 < argc) {
            g_static_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        }
    }

    Database db;
    if (!db.open(db_path)) {
        std::fprintf(stderr, "Cannot open database: %s\n", db_path.c_str());
        return 1;
    }
    db.upgrade_schema();
    g_db = &db;

    char listen_url[64];
    std::snprintf(listen_url, sizeof(listen_url), "http://0.0.0.0:%d", port);

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    struct mg_connection *conn = mg_http_listen(&mgr, listen_url, ev_handler, NULL);
    if (!conn) {
        std::fprintf(stderr, "Failed to start listener on %s\n", listen_url);
        return 1;
    }

    std::printf("bdview-web: serving %s on http://localhost:%d\n", db_path.c_str(), port);
    if (!g_static_dir.empty()) {
        std::printf("  static files from: %s\n", g_static_dir.c_str());
    }

    for (;;) {
        mg_mgr_poll(&mgr, 100);
    }

    mg_mgr_free(&mgr);
    return 0;
}
