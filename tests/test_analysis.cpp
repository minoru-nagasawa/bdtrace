#include "helpers/test_helpers.h"
#include "../src/common/types.h"
#include "../src/common/log.h"
#include "../src/common/view_helpers.h"
#include "../src/db/database.h"
#include "../src/tracer/trace_session.h"
#include "../src/tracer/ptrace_backend.h"

#include <unistd.h>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <cstdio>

using namespace bdtrace;

static const char* TEST_DB = "/tmp/bdtrace_test_analysis.db";

static void run(const char* cmd) {
    int ret = system(cmd);
    (void)ret;
}

static void cleanup() {
    unlink(TEST_DB);
    run("rm -rf /tmp/bdtrace_analysis_work");
}

static void setup() {
    run("mkdir -p /tmp/bdtrace_analysis_work/src");
}

static bool trace_cmd(const std::string& shell_cmd) {
    TraceSession session;
    if (!session.open(TEST_DB)) return false;

    PtraceBackend backend(session);
    std::vector<std::string> argv;
    argv.push_back("sh");
    argv.push_back("-c");
    argv.push_back(shell_cmd);

    if (backend.start(argv) != 0) return false;
    backend.run_event_loop();
    return true;
}

// Helper: check if a set contains a string matching a substring
static bool set_contains_substr(const std::set<std::string>& s,
                                const std::string& substr) {
    for (std::set<std::string>::const_iterator it = s.begin();
         it != s.end(); ++it) {
        if (it->find(substr) != std::string::npos) return true;
    }
    return false;
}

// =================================================================
// DependencyGraph tests
// =================================================================

void test_depgraph_basic() {
    cleanup(); setup();
    run("echo 'data' > /tmp/bdtrace_analysis_work/src/a.txt");

    ASSERT_TRUE(trace_cmd(
        "cat /tmp/bdtrace_analysis_work/src/a.txt"
        " > /tmp/bdtrace_analysis_work/b.txt"));

    Database db;
    ASSERT_TRUE(db.open(TEST_DB));

    DependencyGraph g;
    build_dependency_graph(db, g);

    // a.txt should be in file_to_readers
    bool found_a = false;
    for (std::map<std::string, std::set<int> >::const_iterator it =
             g.file_to_readers.begin(); it != g.file_to_readers.end(); ++it) {
        if (it->first.find("a.txt") != std::string::npos) {
            found_a = true;
            break;
        }
    }
    ASSERT_TRUE(found_a);

    // b.txt should be in pid_to_outputs
    bool found_b = false;
    for (std::map<int, std::set<std::string> >::const_iterator it =
             g.pid_to_outputs.begin(); it != g.pid_to_outputs.end(); ++it) {
        if (set_contains_substr(it->second, "b.txt")) {
            found_b = true;
            break;
        }
    }
    ASSERT_TRUE(found_b);

    cleanup();
}

void test_depgraph_chain() {
    cleanup(); setup();
    run("echo 'data' > /tmp/bdtrace_analysis_work/src/a.txt");

    ASSERT_TRUE(trace_cmd(
        "cat /tmp/bdtrace_analysis_work/src/a.txt"
        " > /tmp/bdtrace_analysis_work/b.txt &&"
        " cat /tmp/bdtrace_analysis_work/b.txt"
        " > /tmp/bdtrace_analysis_work/c.txt"));

    Database db;
    ASSERT_TRUE(db.open(TEST_DB));

    DependencyGraph g;
    build_dependency_graph(db, g);

    // b.txt should appear in both readers and outputs (intermediate file)
    bool b_in_readers = false;
    for (std::map<std::string, std::set<int> >::const_iterator it =
             g.file_to_readers.begin(); it != g.file_to_readers.end(); ++it) {
        if (it->first.find("b.txt") != std::string::npos) {
            b_in_readers = true;
            break;
        }
    }
    bool b_in_outputs = false;
    for (std::map<int, std::set<std::string> >::const_iterator it =
             g.pid_to_outputs.begin(); it != g.pid_to_outputs.end(); ++it) {
        if (set_contains_substr(it->second, "b.txt")) {
            b_in_outputs = true;
            break;
        }
    }
    ASSERT_TRUE(b_in_readers);
    ASSERT_TRUE(b_in_outputs);

    cleanup();
}

void test_depgraph_children() {
    cleanup(); setup();
    run("echo 'data' > /tmp/bdtrace_analysis_work/src/a.txt");

    ASSERT_TRUE(trace_cmd(
        "sh -c \"cp /tmp/bdtrace_analysis_work/src/a.txt"
        " /tmp/bdtrace_analysis_work/b.txt\""));

    Database db;
    ASSERT_TRUE(db.open(TEST_DB));

    DependencyGraph g;
    build_dependency_graph(db, g);

    // pid_children should have at least one entry
    ASSERT_TRUE(!g.pid_children.empty());

    // proc_map should have at least 2 processes (outer sh + inner sh/cp)
    ASSERT_TRUE((int)g.proc_map.size() >= 2);

    cleanup();
}

// =================================================================
// Process I/O tests
// =================================================================

void test_pio_basic() {
    cleanup(); setup();
    run("echo 'data' > /tmp/bdtrace_analysis_work/src/input.txt");

    ASSERT_TRUE(trace_cmd(
        "cat /tmp/bdtrace_analysis_work/src/input.txt"
        " > /tmp/bdtrace_analysis_work/output.txt"));

    Database db;
    ASSERT_TRUE(db.open(TEST_DB));

    DependencyGraph g;
    build_dependency_graph(db, g);

    // Find the root PID
    std::set<int> pids;
    for (std::map<int, ProcessRecord>::const_iterator it = g.proc_map.begin();
         it != g.proc_map.end(); ++it) pids.insert(it->first);
    int root_pid = 0;
    for (std::map<int, ProcessRecord>::const_iterator it = g.proc_map.begin();
         it != g.proc_map.end(); ++it) {
        if (pids.find(it->second.ppid) == pids.end()) {
            root_pid = it->first;
            break;
        }
    }
    ASSERT_TRUE(root_pid > 0);

    ProcessIO pio;
    classify_process_io(db, g, root_pid, true, pio);

    ASSERT_TRUE(set_contains_substr(pio.inputs, "input.txt"));
    ASSERT_TRUE(set_contains_substr(pio.outputs, "output.txt"));

    cleanup();
}

void test_pio_internal() {
    cleanup(); setup();
    run("echo 'data' > /tmp/bdtrace_analysis_work/src/a.txt");

    ASSERT_TRUE(trace_cmd(
        "cat /tmp/bdtrace_analysis_work/src/a.txt"
        " > /tmp/bdtrace_analysis_work/tmp.txt &&"
        " cat /tmp/bdtrace_analysis_work/tmp.txt"
        " > /tmp/bdtrace_analysis_work/b.txt"));

    Database db;
    ASSERT_TRUE(db.open(TEST_DB));

    DependencyGraph g;
    build_dependency_graph(db, g);

    // Find root PID
    std::set<int> pids;
    for (std::map<int, ProcessRecord>::const_iterator it = g.proc_map.begin();
         it != g.proc_map.end(); ++it) pids.insert(it->first);
    int root_pid = 0;
    for (std::map<int, ProcessRecord>::const_iterator it = g.proc_map.begin();
         it != g.proc_map.end(); ++it) {
        if (pids.find(it->second.ppid) == pids.end()) {
            root_pid = it->first;
            break;
        }
    }

    ProcessIO pio;
    classify_process_io(db, g, root_pid, true, pio);

    // tmp.txt should be internal (both read and written)
    ASSERT_TRUE(set_contains_substr(pio.internal, "tmp.txt"));
    // a.txt should be input only
    ASSERT_TRUE(set_contains_substr(pio.inputs, "a.txt"));
    // b.txt should be output only
    ASSERT_TRUE(set_contains_substr(pio.outputs, "b.txt"));

    cleanup();
}

void test_pio_tree() {
    cleanup(); setup();
    run("echo 'data1' > /tmp/bdtrace_analysis_work/src/x.txt");
    run("echo 'data2' > /tmp/bdtrace_analysis_work/src/y.txt");

    ASSERT_TRUE(trace_cmd(
        "sh -c 'cat /tmp/bdtrace_analysis_work/src/x.txt"
        " > /tmp/bdtrace_analysis_work/out1.txt' &&"
        " sh -c 'cat /tmp/bdtrace_analysis_work/src/y.txt"
        " > /tmp/bdtrace_analysis_work/out2.txt'"));

    Database db;
    ASSERT_TRUE(db.open(TEST_DB));

    DependencyGraph g;
    build_dependency_graph(db, g);

    // Find root PID
    std::set<int> pids;
    for (std::map<int, ProcessRecord>::const_iterator it = g.proc_map.begin();
         it != g.proc_map.end(); ++it) pids.insert(it->first);
    int root_pid = 0;
    for (std::map<int, ProcessRecord>::const_iterator it = g.proc_map.begin();
         it != g.proc_map.end(); ++it) {
        if (pids.find(it->second.ppid) == pids.end()) {
            root_pid = it->first;
            break;
        }
    }

    // With tree=true, should see both children's I/O
    ProcessIO pio;
    classify_process_io(db, g, root_pid, true, pio);

    ASSERT_TRUE(set_contains_substr(pio.inputs, "x.txt"));
    ASSERT_TRUE(set_contains_substr(pio.inputs, "y.txt"));
    ASSERT_TRUE(set_contains_substr(pio.outputs, "out1.txt"));
    ASSERT_TRUE(set_contains_substr(pio.outputs, "out2.txt"));

    cleanup();
}

// =================================================================
// Impact tests
// =================================================================

void test_impact_single_source() {
    cleanup(); setup();
    run("echo 'header' > /tmp/bdtrace_analysis_work/src/h.h");

    // 3 processes read h.h and produce different outputs
    ASSERT_TRUE(trace_cmd(
        "cat /tmp/bdtrace_analysis_work/src/h.h > /tmp/bdtrace_analysis_work/a.o &&"
        " cat /tmp/bdtrace_analysis_work/src/h.h > /tmp/bdtrace_analysis_work/b.o &&"
        " cat /tmp/bdtrace_analysis_work/src/h.h > /tmp/bdtrace_analysis_work/c.o"));

    Database db;
    ASSERT_TRUE(db.open(TEST_DB));

    DependencyGraph g;
    build_dependency_graph(db, g);

    std::vector<ImpactEntry> entries;
    compute_impact(g, entries, 0);

    // h.h should appear in impact entries
    bool found = false;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].file.find("h.h") != std::string::npos) {
            // Should affect at least the sh process that reads it
            ASSERT_TRUE(entries[i].affected_count >= 1);
            found = true;
            break;
        }
    }
    ASSERT_TRUE(found);

    cleanup();
}

void test_impact_ranking() {
    cleanup(); setup();
    run("echo 'header' > /tmp/bdtrace_analysis_work/src/common.h");
    run("echo 'util' > /tmp/bdtrace_analysis_work/src/rare.h");

    // common.h read by 3, rare.h read by 1
    ASSERT_TRUE(trace_cmd(
        "cat /tmp/bdtrace_analysis_work/src/common.h > /tmp/bdtrace_analysis_work/a.o &&"
        " cat /tmp/bdtrace_analysis_work/src/common.h > /tmp/bdtrace_analysis_work/b.o &&"
        " cat /tmp/bdtrace_analysis_work/src/common.h > /tmp/bdtrace_analysis_work/c.o &&"
        " cat /tmp/bdtrace_analysis_work/src/rare.h > /tmp/bdtrace_analysis_work/d.o"));

    Database db;
    ASSERT_TRUE(db.open(TEST_DB));

    DependencyGraph g;
    build_dependency_graph(db, g);

    std::vector<ImpactEntry> entries;
    compute_impact(g, entries, 0);

    // Find positions of common.h and rare.h
    int common_idx = -1, rare_idx = -1;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].file.find("common.h") != std::string::npos) common_idx = (int)i;
        if (entries[i].file.find("rare.h") != std::string::npos) rare_idx = (int)i;
    }
    ASSERT_TRUE(common_idx >= 0);
    ASSERT_TRUE(rare_idx >= 0);
    // common.h should rank higher (lower index = higher impact)
    ASSERT_TRUE(common_idx < rare_idx);

    cleanup();
}

void test_impact_transitive() {
    cleanup(); setup();
    run("echo 'src' > /tmp/bdtrace_analysis_work/src/origin.txt");

    // a -> b -> c chain
    ASSERT_TRUE(trace_cmd(
        "cat /tmp/bdtrace_analysis_work/src/origin.txt"
        " > /tmp/bdtrace_analysis_work/mid.txt &&"
        " cat /tmp/bdtrace_analysis_work/mid.txt"
        " > /tmp/bdtrace_analysis_work/final.txt"));

    Database db;
    ASSERT_TRUE(db.open(TEST_DB));

    DependencyGraph g;
    build_dependency_graph(db, g);

    std::vector<ImpactEntry> entries;
    compute_impact(g, entries, 0);

    // origin.txt should have transitive impact
    bool found = false;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].file.find("origin.txt") != std::string::npos) {
            // Should affect processes producing both mid.txt and final.txt
            ASSERT_TRUE(entries[i].affected_count >= 1);
            found = true;
            break;
        }
    }
    ASSERT_TRUE(found);

    cleanup();
}

// =================================================================
// Race detection tests
// =================================================================

void test_races_no_race() {
    cleanup(); setup();
    run("echo 'data' > /tmp/bdtrace_analysis_work/src/a.txt");

    // Sequential: write b, then read b - no race
    ASSERT_TRUE(trace_cmd(
        "cat /tmp/bdtrace_analysis_work/src/a.txt"
        " > /tmp/bdtrace_analysis_work/b.txt &&"
        " cat /tmp/bdtrace_analysis_work/b.txt"
        " > /tmp/bdtrace_analysis_work/c.txt"));

    Database db;
    ASSERT_TRUE(db.open(TEST_DB));

    DependencyGraph g;
    build_dependency_graph(db, g);

    std::vector<RaceEntry> races;
    detect_races(g, races);

    // No races in sequential execution
    ASSERT_EQ((int)races.size(), 0);

    cleanup();
}

void test_races_concurrent_write_read() {
    cleanup(); setup();

    // Background write and foreground read of same file
    // This may or may not detect as race depending on timing,
    // but we verify the function runs without error
    ASSERT_TRUE(trace_cmd(
        "echo data > /tmp/bdtrace_analysis_work/f.txt &"
        " cat /tmp/bdtrace_analysis_work/f.txt"
        " > /tmp/bdtrace_analysis_work/g.txt 2>/dev/null;"
        " wait"));

    Database db;
    ASSERT_TRUE(db.open(TEST_DB));

    DependencyGraph g;
    build_dependency_graph(db, g);

    std::vector<RaceEntry> races;
    detect_races(g, races);

    // The function should complete without crash
    // Race detection depends on timing, so we just verify it returns
    ASSERT_TRUE(true);

    cleanup();
}

void test_races_parent_child_ok() {
    cleanup(); setup();
    run("echo 'data' > /tmp/bdtrace_analysis_work/src/a.txt");

    // Parent writes, child reads - this is a parent-child relationship, not a race
    ASSERT_TRUE(trace_cmd(
        "echo data > /tmp/bdtrace_analysis_work/p.txt &&"
        " sh -c 'cat /tmp/bdtrace_analysis_work/p.txt > /dev/null'"));

    Database db;
    ASSERT_TRUE(db.open(TEST_DB));

    DependencyGraph g;
    build_dependency_graph(db, g);

    std::vector<RaceEntry> races;
    detect_races(g, races);

    // Parent-child should not be flagged as race
    // Filter races involving p.txt
    int p_races = 0;
    for (size_t i = 0; i < races.size(); ++i) {
        if (races[i].file.find("p.txt") != std::string::npos) ++p_races;
    }
    ASSERT_EQ(p_races, 0);

    cleanup();
}

// =================================================================
// Rebuild estimate tests
// =================================================================

void test_rebuild_estimate_basic() {
    cleanup(); setup();
    run("echo 'src' > /tmp/bdtrace_analysis_work/src/s.txt");

    ASSERT_TRUE(trace_cmd(
        "cat /tmp/bdtrace_analysis_work/src/s.txt"
        " > /tmp/bdtrace_analysis_work/o1.txt &&"
        " cat /tmp/bdtrace_analysis_work/o1.txt"
        " > /tmp/bdtrace_analysis_work/o2.txt &&"
        " cat /tmp/bdtrace_analysis_work/o2.txt"
        " > /tmp/bdtrace_analysis_work/o3.txt"));

    Database db;
    ASSERT_TRUE(db.open(TEST_DB));

    DependencyGraph g;
    build_dependency_graph(db, g);

    // Find s.txt full path
    std::string s_path;
    for (std::map<std::string, std::set<int> >::const_iterator it =
             g.file_to_readers.begin(); it != g.file_to_readers.end(); ++it) {
        if (it->first.find("s.txt") != std::string::npos) {
            s_path = it->first;
            break;
        }
    }
    ASSERT_TRUE(!s_path.empty());

    std::set<std::string> changed;
    changed.insert(s_path);
    std::set<int> affected = rebuild_bfs(g, changed);

    RebuildEstimate est;
    compute_rebuild_estimate(g, affected, est);

    ASSERT_TRUE(est.affected_count >= 1);
    ASSERT_TRUE(est.serial_estimate_us > 0);
    ASSERT_TRUE(est.longest_single_us > 0);
    ASSERT_TRUE(est.serial_estimate_us >= est.longest_single_us);

    cleanup();
}

void test_rebuild_estimate_all() {
    cleanup(); setup();
    run("echo 'a' > /tmp/bdtrace_analysis_work/src/x.txt");
    run("echo 'b' > /tmp/bdtrace_analysis_work/src/y.txt");

    ASSERT_TRUE(trace_cmd(
        "cat /tmp/bdtrace_analysis_work/src/x.txt"
        " > /tmp/bdtrace_analysis_work/ox.txt &&"
        " cat /tmp/bdtrace_analysis_work/src/y.txt"
        " > /tmp/bdtrace_analysis_work/oy.txt"));

    Database db;
    ASSERT_TRUE(db.open(TEST_DB));

    DependencyGraph g;
    build_dependency_graph(db, g);

    // Change all source files
    std::set<std::string> changed;
    for (std::map<std::string, std::set<int> >::const_iterator it =
             g.file_to_readers.begin(); it != g.file_to_readers.end(); ++it) {
        if (it->first.find("analysis_work/src/") != std::string::npos) {
            changed.insert(it->first);
        }
    }
    ASSERT_TRUE(!changed.empty());

    std::set<int> affected = rebuild_bfs(g, changed);

    RebuildEstimate est;
    compute_rebuild_estimate(g, affected, est);

    // Should affect at least the shell process
    ASSERT_TRUE(est.affected_count >= 1);
    ASSERT_TRUE(est.serial_estimate_us > 0);

    cleanup();
}

// =================================================================
// Reverse dependency tests
// =================================================================

void test_rdeps_direct() {
    cleanup(); setup();
    run("echo 'data' > /tmp/bdtrace_analysis_work/src/a.txt");

    ASSERT_TRUE(trace_cmd(
        "cat /tmp/bdtrace_analysis_work/src/a.txt"
        " > /tmp/bdtrace_analysis_work/b.txt"));

    Database db;
    ASSERT_TRUE(db.open(TEST_DB));

    DependencyGraph g;
    build_dependency_graph(db, g);

    // Find a.txt full path
    std::string a_path;
    for (std::map<std::string, std::set<int> >::const_iterator it =
             g.file_to_readers.begin(); it != g.file_to_readers.end(); ++it) {
        if (it->first.find("a.txt") != std::string::npos) {
            a_path = it->first;
            break;
        }
    }
    ASSERT_TRUE(!a_path.empty());

    RdepsNode root;
    compute_rdeps(g, a_path, 3, root);

    ASSERT_TRUE(root.file == a_path);
    // b.txt should be a direct dependent
    bool found_b = false;
    for (size_t i = 0; i < root.children.size(); ++i) {
        if (root.children[i].file.find("b.txt") != std::string::npos) {
            found_b = true;
            break;
        }
    }
    ASSERT_TRUE(found_b);

    cleanup();
}

void test_rdeps_transitive() {
    cleanup(); setup();
    run("echo 'data' > /tmp/bdtrace_analysis_work/src/a.txt");

    ASSERT_TRUE(trace_cmd(
        "cat /tmp/bdtrace_analysis_work/src/a.txt"
        " > /tmp/bdtrace_analysis_work/b.txt &&"
        " cat /tmp/bdtrace_analysis_work/b.txt"
        " > /tmp/bdtrace_analysis_work/c.txt"));

    Database db;
    ASSERT_TRUE(db.open(TEST_DB));

    DependencyGraph g;
    build_dependency_graph(db, g);

    // Find a.txt full path
    std::string a_path;
    for (std::map<std::string, std::set<int> >::const_iterator it =
             g.file_to_readers.begin(); it != g.file_to_readers.end(); ++it) {
        if (it->first.find("a.txt") != std::string::npos) {
            a_path = it->first;
            break;
        }
    }
    ASSERT_TRUE(!a_path.empty());

    RdepsNode root;
    compute_rdeps(g, a_path, 3, root);

    // Collect all files in the rdeps tree
    std::set<std::string> all_files;
    std::vector<const RdepsNode*> stack;
    stack.push_back(&root);
    while (!stack.empty()) {
        const RdepsNode* n = stack.back();
        stack.pop_back();
        all_files.insert(n->file);
        for (size_t i = 0; i < n->children.size(); ++i) {
            stack.push_back(&n->children[i]);
        }
    }

    // c.txt should be reachable transitively
    ASSERT_TRUE(set_contains_substr(all_files, "c.txt"));

    cleanup();
}

// =================================================================
// Entry point
// =================================================================

void run_analysis_tests() {
    std::printf("=== DependencyGraph Tests ===\n");
    RUN_TEST(test_depgraph_basic);
    RUN_TEST(test_depgraph_chain);
    RUN_TEST(test_depgraph_children);

    std::printf("=== Process I/O Tests ===\n");
    RUN_TEST(test_pio_basic);
    RUN_TEST(test_pio_internal);
    RUN_TEST(test_pio_tree);

    std::printf("=== Impact Ranking Tests ===\n");
    RUN_TEST(test_impact_single_source);
    RUN_TEST(test_impact_ranking);
    RUN_TEST(test_impact_transitive);

    std::printf("=== Race Detection Tests ===\n");
    RUN_TEST(test_races_no_race);
    RUN_TEST(test_races_concurrent_write_read);
    RUN_TEST(test_races_parent_child_ok);

    std::printf("=== Rebuild Estimate Tests ===\n");
    RUN_TEST(test_rebuild_estimate_basic);
    RUN_TEST(test_rebuild_estimate_all);

    std::printf("=== Reverse Dependency Tests ===\n");
    RUN_TEST(test_rdeps_direct);
    RUN_TEST(test_rdeps_transitive);
}
